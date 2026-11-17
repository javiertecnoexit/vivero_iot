#include "config_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stdlib.h>
#include <string.h>

#include "calibration.h"
#include "climate.h"
#include "config.h"
#include "alarm.h"
#include "factory_reset.h"
#include "irrigation.h"
#include "local_cycle.h"
#include "local_profile.h"
#include "relay.h"
#include "scheduler.h"
#include "sensors.h"
#include "settings.h"
#include "state_machine.h"
#include "window.h"
#include "ws_client.h"

// ============================================================================
// config_portal.cpp — Portal local de configuración (modo Configuración)
// Fuente: especificacion_firmware.md sección 4.
// - Levanta AP (SSID "Vivero-IoT-XXXX") + servidor web.
// - Formulario único: WiFi, backend, canales, calibración, perfil local de
//   riego/horarios/umbrales, cama caliente y alarmas.
// - "Guardar y volver a Normal" aplica todo junto → NVS, sin reiniciar.
// - Cualquier request HTTP reinicia el timeout de inactividad (4.3).
// - Aviso de reinicio próximo ~2 min antes (4.3) mostrado en el portal.
// - Rutas:
//     GET  /                    → página de configuración
//     POST /save                → guarda todo y reinicia
//     GET  /api/soil-raw?zone=N → ADC crudo en vivo (JSON, calibración §8.2)
//     POST /api/calibrate       → fija dry/wet de una zona (§8.3-8.4)
// ============================================================================

static WebServer s_server(80);
static DNSServer s_dnsServer;  // portal cautivo: todo dominio → IP del AP
static uint32_t s_lastActivityMs = 0UL;
static bool     s_timeout = false;
static bool     s_warned = false;
static bool     s_running = false;
static bool     s_exitRequested = false;
static bool     s_restartPending = false;
static uint32_t s_restartAtMs = 0UL;
static char     s_apIp[16] = {0};
static char     s_apSsid[33] = {0};
static char     s_apPassword[64] = {0};

// Prueba de canal desde el portal (una por vez, auto-off).
static int8_t   s_testChannel = -1;
static uint32_t s_testEndMs = 0UL;

static String hhmmString(uint8_t hour, uint8_t minute) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)hour, (unsigned)minute);
    return String(buf);
}

// Dias de la semana de un horario (bit 0 = lunes).
// El campo oculto daym_<id> marca que el formulario llego completo: sin el no
// se puede distinguir "el cliente no envio los dias" de "el usuario desmarco
// los 7". Lo primero debe conservar la mascara; lo segundo significa de verdad
// "sin dias". Antes ambos casos se convertian en "todos los dias".
static void appendDayCheckboxes(String& html, const String& id, uint8_t mask) {
    static const char* const labels[7] = { "Lu", "Ma", "Mi", "Ju", "Vi", "Sa", "Do" };
    html += " dias:";
    html += "<input type='hidden' name='daym_" + id + "' value='1'>";
    for (uint8_t d = 0; d < 7; d++) {
        html += "<label style='display:inline;font-weight:normal;margin-right:4px'>"
                "<input type='checkbox' name='day_" + id + "_" + String(d) +
                "' value='1'";
        if ((mask & (uint8_t)(1u << d)) != 0) {
            html += " checked";
        }
        html += ">" + String(labels[d]) + "</label>";
    }
}

// Formulario de una zona de riego: duración, umbral y 4 horarios.
// Se renderiza desde el perfil local guardado para no mezclarlo con la config
// del backend cuando esa es la fuente activa.
static void appendIrrigationZoneForm(String& html, const char* prefix, const char* title,
                                     const LocalZoneProfile& p, bool showHumidity) {
    const String base(prefix);
    html += "<div class='zone'><b>" + String(title) + "</b>";
    html += "<label>Duracion base (s)</label>";
    html += "<input type='number' min='1' max='3600' name='dur_" + base + "' value='" +
            String(p.duration_s) + "'>";
    if (showHumidity) {
        html += "<label>Humedad minima para regar (%)</label>";
        html += "<input type='number' min='0' max='100' name='min_" + base + "' value='" +
                String(p.minHumidityPct) + "'>";
        html += "<label style='font-weight:normal'><input type='checkbox' name='thr_" + base +
                "' value='1'";
        if (p.thresholdEnabled) {
            html += " checked";
        }
        html += "> Regar automaticamente por umbral de humedad</label>";
    }
    html += "<label style='font-weight:normal'><input type='checkbox' name='pause_" + base +
            "' value='1'";
    if (p.paused) {
        html += " checked";
    }
    html += "> Pausar automatizacion (el manual sigue)</label>";

    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        const LocalSchedule& s = p.schedules[i];
        const String id = base + "_" + String(i);
        html += "<div style='border-top:1px dashed #ccc;margin-top:6px;padding-top:6px'>";
        html += "Horario " + String(i + 1) + ": ";
        html += "<input type='text' maxlength='5' placeholder='HH:MM' style='width:72px' "
                "name='sh_" + id + "' value='" +
                (s.enabled ? hhmmString(s.hour, s.minute) : String("")) + "'> ";
        html += "dur <input type='number' min='1' max='3600' style='width:88px' "
                "name='sd_" + id + "' value='" +
                String(s.enabled && s.duration_s > 0 ? s.duration_s : p.duration_s) + "'> s ";
        appendDayCheckboxes(html, id, s.daysMask);
        html += " <label style='display:inline;font-weight:normal'><input type='checkbox' "
                "name='se_" + id + "' value='1'";
        if (s.enabled) {
            html += " checked";
        }
        html += "> activo</label></div>";
    }
    html += "</div>";
}

// Formulario de iluminación/ventilación: 4 horarios desde el perfil local.
static void appendChannelScheduleForm(String& html, const char* prefix, const char* title,
                                      const LocalSchedule* schedules,
                                      uint16_t defaultDuration) {
    const String base(prefix);
    html += "<div class='zone'><b>" + String(title) + "</b>";
    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        const LocalSchedule& s = schedules[i];
        const String id = base + "_" + String(i);
        html += "<div style='border-top:1px dashed #ccc;margin-top:6px;padding-top:6px'>";
        html += "Horario " + String(i + 1) + ": ";
        html += "<input type='text' maxlength='5' placeholder='HH:MM' style='width:72px' "
                "name='sh_" + id + "' value='" +
                (s.enabled ? hhmmString(s.hour, s.minute) : String("")) + "'> ";
        html += "dur <input type='number' min='1' max='57600' style='width:96px' "
                "name='sd_" + id + "' value='" +
                String(s.enabled && s.duration_s > 0 ? s.duration_s : defaultDuration) + "'> s ";
        appendDayCheckboxes(html, id, s.daysMask);
        html += " <label style='display:inline;font-weight:normal'><input type='checkbox' "
                "name='se_" + id + "' value='1'";
        if (s.enabled) {
            html += " checked";
        }
        html += "> activo</label></div>";
    }
    html += "</div>";
}

// ----------------------------------------------------------------------------
// Página de configuración (HTML + JS mínimo)
// ----------------------------------------------------------------------------
// El JS hace polling a /api/soil-raw para mostrar el ADC en vivo y copia el
// valor mostrado a un hidden input al pulsar Fijar. El form va a /save.
// Se construye con String por partes (valores actuales + selects).

static String buildIndexHtml() {
    const bool isWarned = s_warned;
    const bool maintenance = (stateMachineGetMode() == OperationMode::MANTENIMIENTO);
    const uint8_t subZones = settingsGetSubstrateZones();

    const char* modeName = "NORMAL";
    if (stateMachineGetMode() == OperationMode::PORTAL) {
        modeName = "PORTAL";
    } else if (stateMachineGetMode() == OperationMode::MANTENIMIENTO) {
        modeName = "MANTENIMIENTO";
    }
    const ConfigSource configSource = settingsGetConfigSource();

    // El editor siempre muestra el perfil local guardado (respaldo/override),
    // no la config activa del backend.
    LocalProfile localProfile = {};
    if (!localProfileGetStored(localProfile)) {
        localProfile = localProfileGetRuntime();
    }

    String html;
    html.reserve(8192);
    html += "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>Vivero IoT - Configuracion</title>";
    html += "<style>"
            "body{font-family:sans-serif;max-width:640px;margin:20px auto;padding:0 12px}"
            "h1{font-size:1.4em}h2{margin-top:28px;border-top:1px solid #ccc;padding-top:12px}"
            "label{display:block;margin:10px 0 4px;font-weight:bold}"
            "input[type=text],input[type=password],select{width:100%;padding:8px;box-sizing:border-box}"
            "button{padding:10px 16px;margin:4px 8px 4px 0;border:1px solid #888;background:#eee;"
            "border-radius:4px;cursor:pointer}"
            "button.save{background:#2e7d32;color:#fff;border:none;font-size:1.05em;padding:12px 24px;"
            "margin-top:24px}"
            ".zone{border:1px solid #ccc;padding:8px 12px;margin:8px 0;border-radius:6px}"
            ".raw{font-size:1.3em;font-weight:bold;color:#1565c0}"
            ".danger{background:#c62828;color:#fff;border:none}"
            "#warn{display:none;background:#fff3c4;border:1px solid #e6a700;padding:10px;"
            "border-radius:6px;margin:12px 0}</style></head><body>";
    html += "<h1>Vivero IoT</h1>";
    if (maintenance) {
        html += "<div style='background:#ffe0b2;border:1px solid #e65100;color:#7f3f00;"
                "padding:10px;border-radius:6px;margin:12px 0'><b>MODO MANTENIMIENTO</b>"
                "<br>Automatizacion suspendida y reles OFF. Los botones fisicos estan "
                "ignorados. Las pruebas de canal duran 30 s.</div>";
        html += "<button type='button' onclick=\"maintAction('exit')\">Salir de "
                "mantenimiento y volver a Normal</button>";
    } else {
        html += "<div style='background:#e3f2fd;border:1px solid #1565c0;padding:10px;"
                "border-radius:6px;margin:12px 0'><b>Modo Portal:</b> la automatizacion "
                "sigue activa con la configuracion guardada.</div>";
        html += "<button type='button' onclick=\"maintAction('enter')\">Entrar en "
                "mantenimiento</button>";
    }
    if (alarmChannelIsDisabled()) {
        html += "<div style='background:#ffd6d6;border:1px solid #c62828;color:#7f0000;"
                "padding:10px;border-radius:6px;margin:12px 0'><b>ADVERTENCIA:</b> el canal "
                "de alarma esta deshabilitado. Las reglas se evaluan y los eventos se "
                "registran, pero el rele no suena.</div>";
    }
    if (isWarned) {
        html += "<div id='warn' style='display:block'>El portal se cerrara "
                "pronto por inactividad (~2 minutos). Guarda los cambios si los "
                "necesitas.</div>";
    } else {
        html += "<div id='warn'>Aviso de cierre del portal por inactividad (~2 min).</div>";
    }
    html += "<form method='POST' action='/save'>";
    html += "<h2>Red WiFi</h2>";
    html += "<label>SSID</label>";
    html += "<input name='wifi_ssid' maxlength='32' value='" + String(settingsGetWifiSsid()) + "'>";
    html += "<label>Contrasena</label>";
    html += "<input name='wifi_pass' type='password' maxlength='63'>";
    html += "<div style='font-size:.85em;color:#666'>Dejar vacio para "
            "conservar la contrasena guardada.</div>";

    html += "<h2>Punto de acceso (AP)</h2>";
    html += "<div class='zone'><b>SSID actual:</b> " + String(configPortalGetSsid()) +
            "<br><b>Clave actual:</b> " + String(configPortalGetPassword()) + "</div>";
    html += "<label>Nueva clave del AP</label>";
    html += "<input type='password' name='ap_pass' maxlength='63'>";
    html += "<div style='font-size:.85em;color:#666'>Dejar vacio para conservar. "
            "Minimo 8 caracteres. Se aplica la proxima vez que se abra el portal. "
            "La OLED muestra 18 caracteres: si necesitas leerla ahi, usa una "
            "clave corta.</div>";

    html += "<h2>Backend (nube)</h2>";
    html += "<label>URL del backend (ej: http://192.168.1.50:8000)</label>";
    html += "<input name='backend_url' maxlength='200' value='" +
            String(settingsGetBackendUrl()) + "'>";
    html += "<div style='font-size:.85em;color:#666'>Dejar vacio para desactivar "
            "la nube (riego local por horario/umbral).</div>";
    html += "<label>URL WebSocket (ej: ws://192.168.1.50:8000/ws/device)</label>";
    html += "<input name='ws_url' maxlength='200' value='" +
            String(settingsGetWsUrl()) + "'>";
    html += "<div style='font-size:.85em;color:#666'>Vacio = derivarla del backend. "
            "El claim puede sobrescribirla. wss:// no esta soportado en v0.1.</div>";

    html += "<h2>Fuente de configuracion</h2>";
    html += "<select name='cfg_source'>";
    html += "<option value='backend'";
    if (configSource == ConfigSource::BACKEND) html += " selected";
    html += ">Backend (recomendado)</option>";
    html += "<option value='local'";
    if (configSource == ConfigSource::LOCAL) html += " selected";
    html += ">Perfil local (override)</option>";
    html += "</select>";
    html += "<div style='font-size:.85em;color:#666'>Con backend manda la ultima "
            "config del servidor; las ediciones del perfil local se guardan como "
            "respaldo pero no cambian el comportamiento activo. Con local, la config "
            "del backend se guarda pero no se aplica.</div>";

    html += "<h2>Estado y provisioning</h2>";
    html += "<div class='zone'>";
    html += "<b>Firmware:</b> " + String(FIRMWARE_VERSION) + "<br>";
    html += "<b>Modo:</b> " + String(modeName) + "<br>";
    html += "<b>Version de config del backend:</b> " +
            String(settingsGetConfigVersion()) + "<br>";
    html += "<b>Version del perfil local:</b> " +
            String(settingsGetLocalVersion()) + "<br>";
    html += "<b>Perfil del backend:</b> " +
            String(backendProfileExists() ? "guardado" : "sin datos") + "<br>";
    html += "<b>Device ID:</b> " +
            String(settingsGetDeviceId()[0] ? settingsGetDeviceId() : "(sin reclamar)") +
            "<br>";
    html += "<b>Token API:</b> " +
            String(settingsHasDeviceIdentity() ? "configurado" : "sin configurar") + "<br>";
    html += "<b>Canales habilitados:</b> " +
            String((unsigned)__builtin_popcount(
                (unsigned)settingsGetEnabledChannelsMask())) + " de 8<br>";
    html += "<b>WebSocket:</b> " + String(wsStateName()) +
            (wsIsConnected() ? " (conectado)" : " (sin sesion)") + "<br>";
    html += "<b>Colas WS:</b> " + String((unsigned)wsPendingSamples()) +
            " muestras, " + String((unsigned)wsPendingEvents()) + " eventos<br>";
    if (wsLastError()[0] != '\0') {
        html += "<b>Ultimo error WS:</b> " + String(wsLastError()) + "<br>";
    }
    html += "</div>";
    html += "<label>Claim code (alta en el backend)</label>";
    html += "<input name='claim_code' maxlength='32' value='" +
            String(settingsGetClaimCode()) + "'>";
    html += "<div style='font-size:.85em;color:#666'>Se guarda en NVS. El alta "
            "contra el backend se completa en el hito de WebSocket.</div>";

    html += "<h2>Canales de rele (fijos)</h2>";
    html += "<div style='font-size:.85em;color:#666'>La funcion y el GPIO no se "
            "modifican. Solo se habilitan o deshabilitan. Una prueba dura como "
            "maximo 10 s (la ventana, 5 s).</div>";
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        const RelayChannel ch = (RelayChannel)i;
        html += "<div class='zone'><label style='display:inline-block;font-weight:normal'>";
        html += "<input type='checkbox' name='ch" + String(i) + "' value='1'";
        if (settingsIsChannelEnabled(ch)) html += " checked";
        html += "> " + String(relayChannelLabel(ch));
        html += " <span style='color:#666;font-size:.85em'>(" +
                String(relayChannelName(ch)) + ")</span></label>";
        html += "<button type='button' style='float:right' onclick=\"testCh(" +
                String(i) + ")\">Probar</button></div>";
    }

    html += "<h2>Perfil local — Riego</h2>";
    html += "<div style='font-size:.85em;color:#666'>Editor del perfil local "
            "guardado en NVS (respaldo/override). Con fuente backend no cambia el "
            "comportamiento activo. Los horarios se evaluan en hora local (UTC-3). "
            "El control manual siempre tiene prioridad.</div>";
    for (uint8_t z = 0; z < subZones && z < MAX_SUBSTRATE_ZONES; z++) {
        const String base = "z" + String(z + 1);
        const String title = "Sustrato Z" + String(z + 1);
        appendIrrigationZoneForm(html, base.c_str(), title.c_str(),
                                 localProfile.substrate[z], true);
    }
    appendIrrigationZoneForm(html, "asp", "Aspersion",
                             localProfile.sprinkler[0], false);

    html += "<h2>Perfil local — Iluminacion y ventilacion</h2>";
    html += "<div style='font-size:.85em;color:#666'>Horarios diarios. La "
            "iluminacion se corta a las 16 h continuas; la ventilacion respeta "
            "sus minimos de marcha y descanso.</div>";
    appendChannelScheduleForm(html, "ilum", "Iluminacion", localProfile.light,
                              (uint16_t)(12UL * 3600UL));
    appendChannelScheduleForm(html, "vent", "Ventilacion", localProfile.vent, 300);

    html += "<h2>Perfil local — Cama caliente y limites</h2>";
    html += "<label>Setpoint de la cama (C)</label>";
    html += "<input type='number' min='5' max='34.9' step='0.1' name='bed_target' "
            "value='" + String(settingsGetBedTargetC(), 1) + "'>";
    html += "<div style='font-size:.85em;color:#666'>Corte absoluto de seguridad "
            "a 35 C (no configurable). Histeresis de 1 C.</div>";
    html += "<label style='display:inline-block;font-weight:normal'>"
            "<input type='checkbox' name='bed_auto' value='1'";
    if (localProfile.bedAutoEnabled) html += " checked";
    html += "> Termostato automatico de la cama</label>";
    html += "<label style='display:inline-block;font-weight:normal'>"
            "<input type='checkbox' name='interlock' value='1'";
    if (localProfile.interlock) html += " checked";
    html += "> Un solo riego a la vez (Z1, Z2 y aspersion)</label>";
    html += "<label>Duracion maxima de riego — sustrato (s)</label>";
    html += "<input type='number' min='1' max='3600' name='max_run_sub' value='" +
            String(localProfile.maxRunSubstrateS) + "'>";
    html += "<label>Duracion maxima de riego — aspersion (s)</label>";
    html += "<input type='number' min='1' max='3600' name='max_run_spr' value='" +
            String(localProfile.maxRunSprinklerS) + "'>";
    html += "<label>Tiempo minimo apagado entre ciclos por umbral (s)</label>";
    html += "<input type='number' min='0' max='" +
            String((unsigned)IRRIGATION_MIN_OFF_MAX_S) + "' name='min_off' value='" +
            String(localProfile.minOffS) + "'>";
    html += "<label>Espera entre zonas distintas tras un riego (s)</label>";
    html += "<input type='number' min='0' max='600' name='stagger' value='" +
            String(localProfile.staggerS) + "'>";

    html += "<h2>Perfil local — Hora local</h2>";
    html += "<label>Offset respecto de UTC (minutos)</label>";
    html += "<input type='number' min='-720' max='840' name='tz_offset' value='" +
            String(localProfile.timezoneOffsetMin) + "'>";
    html += "<div style='font-size:.85em;color:#666'>Ejemplo: -180 = UTC-3. "
            "Los horarios se evaluan en hora local; la telemetria viaja en UTC.</div>";

    html += "<h2>Perfil local — Clima de ventilacion</h2>";
    html += "<label style='display:inline-block;font-weight:normal'>"
            "<input type='checkbox' name='vent_auto' value='1'";
    if (localProfile.ventAutoEnabled) html += " checked";
    html += "> Automatizacion de ventilacion (horario y clima)</label>";
    html += "<label style='display:inline-block;font-weight:normal'>"
            "<input type='checkbox' name='vent_climate' value='1'";
    if (localProfile.ventClimateEnabled) html += " checked";
    html += "> Control por temperatura/humedad</label>";
    html += "<label>Encender ventilacion desde temp (C)</label>";
    html += "<input type='number' min='-20' max='80' step='0.1' name='vent_on_temp' value='" +
            String(localProfile.ventOnTempC, 1) + "'>";
    html += "<label>Apagar ventilacion por debajo de temp (C)</label>";
    html += "<input type='number' min='-20' max='80' step='0.1' name='vent_off_temp' value='" +
            String(localProfile.ventOffTempC, 1) + "'>";
    html += "<label>Encender ventilacion desde humedad (%)</label>";
    html += "<input type='number' min='0' max='100' step='0.1' name='vent_on_rh' value='" +
            String(localProfile.ventOnRhPct, 1) + "'>";
    html += "<label>Apagar ventilacion por debajo de humedad (%)</label>";
    html += "<input type='number' min='0' max='100' step='0.1' name='vent_off_rh' value='" +
            String(localProfile.ventOffRhPct, 1) + "'>";
    html += "<div style='font-size:.85em;color:#666'>La ventilacion arranca si "
            "temp >= encendido o humedad >= encendido, sostenido 2 ciclos. "
            "Se apaga si ambas quedan por debajo del umbral de apagado.</div>";

    html += "<h2>Umbrales de alarma de aire</h2>";
    html += "<label>Temperatura maxima (C)</label>";
    html += "<input type='number' min='-20' max='80' step='0.1' name='alarm_max_temp' "
            "value='" + String(localProfile.alarmMaxTemp, 1) + "'>";
    html += "<label>Temperatura minima (C)</label>";
    html += "<input type='number' min='-50' max='40' step='0.1' name='alarm_min_temp' "
            "value='" + String(localProfile.alarmMinTemp, 1) + "'>";
    html += "<div style='font-size:.85em;color:#666'>La maxima debe ser mayor que "
            "la minima; si no, se conservan los valores anteriores.</div>";

    html += "<h2>Alarmas</h2>";
    html += "<div id='alarmBox' style='background:#e8f5e9;border:1px solid #9e9e9e;"
            "padding:10px;border-radius:6px;white-space:pre-wrap'>Consultando...</div>";
    html += "<button type='button' onclick=\"alarmAction('silence')\">Silenciar 10 min</button>";
    html += "<button type='button' onclick=\"alarmAction('reset')\">Limpiar latches</button>";

    html += "<h2>Calibracion de humedad por zona</h2>";
    html += "<div style='font-size:.85em;color:#666'>El modulo de humedad se "
            "energiza solo durante la lectura (25 ms + 12 muestras). El valor "
            "ADC se actualiza en vivo cada 1,5 s.</div>";
    for (uint8_t z = 0; z < subZones && z < MAX_SUBSTRATE_ZONES; z++) {
        const ZoneCalibration c = calibrationGet(z);
        html += "<div class='zone'><b>Zona sustrato " + String(z + 1) + "</b>";
        html += " - ADC: <span class='raw' id='raw" + String(z) + "'>...</span>";
        if (c.calibrated) {
            html += " <i>(seco=" + String(c.dryRaw) + ", humedo=" + String(c.wetRaw) + ")</i>";
        } else {
            html += " <i>(sin calibrar)</i>";
        }
        html += "<br>";
        html += "<button type='button' onclick=\"setCal(" + String(z) +
                ",'dry')\">Fijar como SECO</button>";
        html += "<button type='button' onclick=\"setCal(" + String(z) +
                ",'wet')\">Fijar como HUMEDO</button>";
        html += "<input type='hidden' name='dry" + String(z) + "' id='dry" + String(z) +
                "' value='" + String(c.dryRaw) + "'>";
        html += "<input type='hidden' name='wet" + String(z) + "' id='wet" + String(z) +
                "' value='" + String(c.wetRaw) + "'>";
        html += "</div>";
    }

    html += "<h2>Sensores (diagnostico)</h2>";
    html += "<div style='font-size:.85em;color:#666'>La lectura de las 3 sondas "
            "DS18B20 tarda 2-3 s. La ventana se lee del sensor de cerrada.</div>";
    html += "<button type='button' onclick=\"readSensors()\">Leer sensores</button>";
    html += "<pre id='sens' style='background:#f4f4f4;padding:8px;border-radius:6px;"
            "white-space:pre-wrap'>Sin lectura</pre>";

    if (maintenance) {
        html += "<button class='save' type='submit'>Guardar (sigue en mantenimiento)</button>";
    } else {
        html += "<button class='save' type='submit'>Guardar y volver a Normal</button>";
    }
    html += "</form>";

    html += "<h2>Mantenimiento del dispositivo</h2>";
    html += "<div class='zone'><b>Borrar solo WiFi</b><br>Borra las credenciales de "
            "la red y la clave del AP. Conserva perfiles, canales, calibracion, "
            "identidad y alarmas. Al reiniciar se abre el portal.";
    html += "<form method='POST' action='/wifi-reset' onsubmit=\"return "
            "confirm('Borrar WiFi y reiniciar el equipo?')\">";
    html += "<label>Escribe WIFI para confirmar</label>";
    html += "<input name='confirm' maxlength='8' placeholder='WIFI'>";
    html += "<button type='submit'>Borrar WiFi y reiniciar</button>";
    html += "</form></div>";

    html += "<div class='zone' style='border-color:#c62828'>";
    html += "<b>Reset de fabrica</b><br>Borra WiFi, clave del AP, backend, "
            "identidad/claim, perfiles local y del backend, canales habilitados, "
            "calibracion y latches de alarma. El equipo reinicia y abre el portal.";
    html += "<form method='POST' action='/factory-reset' onsubmit=\"return "
            "confirm('RESET DE FABRICA: se borra toda la configuracion. Continuar?')\">";
    html += "<label>Escribe RESET para confirmar</label>";
    html += "<input name='confirm' maxlength='8' placeholder='RESET'>";
    html += "<button class='danger' type='submit'>Reset de fabrica</button>";
    html += "</form></div>";

    html += "<script>"
            "function poll(zone){fetch('/api/soil-raw?zone='+zone).then(r=>r.json())"
            ".then(d=>{if(d.raw===undefined)return;"
            "document.getElementById('raw'+zone).textContent=d.raw;})"
            ".catch(()=>{});setTimeout(function(){poll(zone)},1500);}"
            "function setCal(zone,kind){var raw=parseInt("
            "document.getElementById('raw'+zone).textContent);"
            "if(isNaN(raw)){alert('Espera a que aparezca el valor ADC');return;}"
            "document.getElementById(kind+zone).value=raw;}"
            "function testCh(ch){fetch('/api/test?ch='+ch).then(function(r){"
            "return r.text();}).then(function(t){alert(t);}).catch(function(){"
            "alert('No se pudo iniciar la prueba');});}"
            "function readSensors(){var el=document.getElementById('sens');"
            "el.textContent='Leyendo...';fetch('/api/sensors').then(function(r){"
            "return r.json();}).then(function(d){var s='';"
            "s+='Aire: '+(d.air.valid?(d.air.t.toFixed(1)+' C, '+d.air.h.toFixed(0)+' %'):'N/D')+'\\n';"
            "s+='Humedad Z1/Z2 ADC: '+d.soil[0].raw+' / '+d.soil[1].raw+'\\n';"
            "for(var i=0;i<d.ds18b20.length;i++){var x=d.ds18b20[i];"
            "s+=x.name+': '+(x.valid?(x.t.toFixed(1)+' C'):'FALLA')+'\\n';}"
            "s+='Ventana: '+d.window;el.textContent=s;}).catch(function(){"
            "el.textContent='Error de lectura';});}"
            "function alarmAction(a){fetch('/api/alarm?action='+a).then(function(r){"
            "return r.text();}).then(function(t){alert(t);}).catch(function(){"
            "alert('No se pudo ejecutar la accion');});}"
            "function maintAction(a){var m=(a==='enter')?'Entrar en mantenimiento? "
            "Se apagaran todos los reles y se suspendera la automatizacion.':"
            "'Salir de mantenimiento y volver a Normal?';if(!confirm(m)){return;}"
            "fetch('/api/maintenance?action='+a).then(function(r){return r.text();})"
            ".then(function(t){alert(t);if(a!=='exit'){location.reload();}})"
            ".catch(function(){alert('No se pudo ejecutar la accion');});}"
            "function pollAlarm(){fetch('/api/alarm').then(function(r){return r.json();})"
            ".then(function(d){var s='';if(!d.channel_enabled){s+='ADVERTENCIA: canal de "
            "alarma deshabilitado.\\n';}if(d.conditions.length===0){s+='Sin alarmas activas.';}"
            "else{s+='Activas: '+d.conditions.join(', ');}if(d.latched.length>0){"
            "s+='\\nLatches: '+d.latched.join(', ');}if(d.silenced){s+='\\nSilenciada: '+"
            "d.silence_remaining_s+' s restantes.';}var b=document.getElementById('alarmBox');"
            "b.textContent=s;b.style.background=d.conditions.length?'#ffd6d6':'#e8f5e9';})"
            ".catch(function(){});setTimeout(pollAlarm,3000);}"
            "for(var z=0;z<" + String(subZones) + ";z++){poll(z);}pollAlarm();"
            "</script></body></html>";

    return html;
}

// ----------------------------------------------------------------------------
// Handlers
// ----------------------------------------------------------------------------

static void handleRoot() {
    s_lastActivityMs = millis();
    s_server.send(200, "text/html; charset=utf-8", buildIndexHtml());
}

// --- Parseo de campos del perfil local -------------------------------------
// Los valores invalidos no se aplican: se conserva el runtime anterior y se
// deja constancia en el log. Los formularios siempre envian todos los campos.

static bool parseHhMm(const String& text, uint8_t& hour, uint8_t& minute) {
    String value = text;
    value.trim();
    const int colon = value.indexOf(':');
    if (colon < 1 || colon > 2) {
        return false;
    }
    const String hh = value.substring(0, colon);
    const String mm = value.substring(colon + 1);
    if (mm.length() != 2) {
        return false;
    }
    for (size_t i = 0; i < hh.length(); i++) {
        if (hh[i] < '0' || hh[i] > '9') return false;
    }
    for (size_t i = 0; i < mm.length(); i++) {
        if (mm[i] < '0' || mm[i] > '9') return false;
    }
    const int h = hh.toInt();
    const int m = mm.toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    hour = (uint8_t)h;
    minute = (uint8_t)m;
    return true;
}

static bool parseUnsigned(const String& text, uint32_t minValue, uint32_t maxValue,
                          uint32_t& out) {
    String value = text;
    value.trim();
    if (value.length() == 0 || value.length() > 9) {
        return false;
    }
    for (size_t i = 0; i < value.length(); i++) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    const uint32_t parsed = (uint32_t)value.toInt();
    if (parsed < minValue || parsed > maxValue) {
        return false;
    }
    out = parsed;
    return true;
}

static bool parseIntRange(const String& text, int minValue, int maxValue, int& out) {
    String value = text;
    value.trim();
    if (value.length() == 0 || value.length() > 7) {
        return false;
    }
    uint8_t start = 0;
    if (value[0] == '-') {
        start = 1;
        if (value.length() == 1) {
            return false;
        }
    }
    for (uint8_t i = start; i < value.length(); i++) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    const long parsed = value.toInt();
    if (parsed < minValue || parsed > maxValue) {
        return false;
    }
    out = (int)parsed;
    return true;
}

static bool parseFloatRange(const String& text, float minValue, float maxValue,
                            float& out) {    String value = text;
    value.trim();
    if (value.length() == 0) {
        return false;
    }
    char* end = nullptr;
    const double parsed = strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    if (parsed < minValue || parsed > maxValue) {
        return false;
    }
    out = (float)parsed;
    return true;
}

// Dias de la semana enviados por el formulario (bit 0 = lunes).
// - Con daym_<id>: el formulario llego completo -> se respeta lo marcado y
//   desmarcar los 7 significa "sin dias" (el caller desactiva el horario).
// - Sin daym_<id>: formulario incompleto o cliente viejo -> se asume "todos
//   los dias" para no desactivar un horario por accidente.
static uint8_t readDaysMask(const String& id) {
    if (!s_server.hasArg("daym_" + id)) {
        return (uint8_t)SCHEDULE_DAYS_ALL;
    }
    uint8_t mask = 0;
    for (uint8_t d = 0; d < 7; d++) {
        if (s_server.hasArg("day_" + id + "_" + String(d))) {
            mask |= (uint8_t)(1u << d);
        }
    }
    return mask;  // 0 = sin dias
}

// Horarios de una zona de riego: sh_<base>_<i>, sd_<base>_<i>, se_<base>_<i>.
static void applyZoneScheduleFields(ZoneType type, uint8_t zone, const String& base) {
    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        const String id = base + "_" + String(i);
        ZoneSchedule sched = { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
        sched.daysMask = readDaysMask(id);

        if (s_server.hasArg("se_" + id)) {
            if (!parseHhMm(s_server.arg("sh_" + id), sched.hour, sched.minute)) {
                Serial.printf("[PORTAL] Horario %s[%u] invalido: se desactiva\n",
                              base.c_str(), (unsigned)(i + 1));
            } else if (sched.daysMask == 0) {
                Serial.printf("[PORTAL] Horario %s[%u] sin dias: se desactiva\n",
                              base.c_str(), (unsigned)(i + 1));
            } else {
                uint32_t duration = 0;
                if (!parseUnsigned(s_server.arg("sd_" + id), 1,
                                   IRRIGATION_MAX_RUN_ABS_S, duration)) {
                    duration = irrigationGetDuration(type, zone);
                    Serial.printf("[PORTAL] Duracion %s[%u] invalida: uso %u s\n",
                                  base.c_str(), (unsigned)(i + 1),
                                  (unsigned)duration);
                }
                sched.duration_s = (uint16_t)duration;
                sched.enabled = true;
            }
        }

        irrigationSetSchedule(type, zone, i, sched);
    }
}

static void applyIrrigationZoneForm(ZoneType type, uint8_t zone, const String& base) {
    uint32_t value = 0;

    if (parseUnsigned(s_server.arg("dur_" + base), 1, IRRIGATION_MAX_RUN_ABS_S, value)) {
        irrigationSetDuration(type, zone, (uint16_t)value);
    } else {
        Serial.printf("[PORTAL] Duracion base %s invalida: se conserva %u s\n",
                      base.c_str(), (unsigned)irrigationGetDuration(type, zone));
    }

    if (type == ZoneType::SUBSTRATE) {
        if (parseUnsigned(s_server.arg("min_" + base), 0, 100, value)) {
            irrigationSetMinHumidity(zone, (uint8_t)value);
        } else {
            Serial.printf("[PORTAL] Humedad minima %s invalida: se conserva %u %%\n",
                          base.c_str(), (unsigned)irrigationGetMinHumidity(zone));
        }
        irrigationSetThresholdEnabled(zone, s_server.hasArg("thr_" + base));
    }

    irrigationSetPaused(type, zone, s_server.hasArg("pause_" + base));
    applyZoneScheduleFields(type, zone, base);
}

// Horarios de iluminacion/ventilacion: mismo esquema de nombres.
static void applyChannelScheduleForm(RelayChannel channel, const String& base,
                                     uint16_t fallbackDuration) {
    const uint32_t maxDuration = 16UL * 3600UL;
    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        const String id = base + "_" + String(i);
        ChannelSchedule sched = { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
        sched.daysMask = readDaysMask(id);

        if (s_server.hasArg("se_" + id)) {
            if (!parseHhMm(s_server.arg("sh_" + id), sched.hour, sched.minute)) {
                Serial.printf("[PORTAL] Horario %s[%u] invalido: se desactiva\n",
                              base.c_str(), (unsigned)(i + 1));
            } else if (sched.daysMask == 0) {
                Serial.printf("[PORTAL] Horario %s[%u] sin dias: se desactiva\n",
                              base.c_str(), (unsigned)(i + 1));
            } else {
                uint32_t duration = 0;
                if (!parseUnsigned(s_server.arg("sd_" + id), 1, maxDuration, duration)) {
                    duration = fallbackDuration;
                    Serial.printf("[PORTAL] Duracion %s[%u] invalida: uso %u s\n",
                                  base.c_str(), (unsigned)(i + 1),
                                  (unsigned)duration);
                }
                sched.duration_s = (uint16_t)duration;
                sched.enabled = true;
            }
        }

        schedulerSetSchedule(channel, i, sched);
    }
}

static void handleSave() {
    s_lastActivityMs = millis();

    const String ssid = s_server.arg("wifi_ssid");
    const String pass = s_server.arg("wifi_pass");

    // Guardar la contraseña SOLO si se escribió algo; si el campo viene vacío
    // se conserva la guardada (evita borrarla al guardar otros cambios).
    if (pass.length() > 0) {
        settingsSetWifi(ssid.c_str(), pass.c_str());
    } else {
        settingsSetWifi(ssid.c_str(), settingsGetWifiPassword());
    }

    const String apPass = s_server.arg("ap_pass");
    if (apPass.length() > 0) {
        if (apPass.length() >= AP_PASSWORD_MIN_LEN &&
            apPass.length() <= SETTINGS_AP_PASS_MAX) {
            settingsSetApPassword(apPass.c_str());
        } else {
            Serial.println("[PORTAL] Clave AP invalida (8-63): se conserva");
        }
    }

    const String backendUrl = s_server.arg("backend_url");
    settingsSetBackendUrl(backendUrl.c_str());

    const String wsUrl = s_server.arg("ws_url");
    String wsUrlTrimmed = wsUrl;
    wsUrlTrimmed.trim();
    if (wsUrlTrimmed.length() == 0 ||
        wsUrlTrimmed.startsWith("ws://") || wsUrlTrimmed.startsWith("wss://")) {
        settingsSetWsUrl(wsUrlTrimmed.c_str());
        if (wsUrlTrimmed.startsWith("wss://")) {
            Serial.println("[PORTAL] Aviso: wss:// no esta soportado en v0.1");
        }
    } else {
        Serial.println("[PORTAL] ws_url invalida (debe empezar con ws:// o wss://): se conserva");
    }

    if (s_server.hasArg("claim_code")) {
        settingsSetClaimCode(s_server.arg("claim_code").c_str());
    }

    // Fuente de configuración: se aplica después de guardar el perfil local,
    // para que el cambio de fuente use los valores recién editados.
    const String sourceArg = s_server.arg("cfg_source");
    const ConfigSource currentSource = settingsGetConfigSource();
    ConfigSource requestedSource = currentSource;
    if (sourceArg == "local") {
        requestedSource = ConfigSource::LOCAL;
    } else if (sourceArg == "backend") {
        requestedSource = ConfigSource::BACKEND;
    }
    const bool sourceChange = (requestedSource != currentSource);

    // Canales habilitados: checkbox presente = habilitado.
    uint16_t mask = 0;
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        if (s_server.hasArg("ch" + String(i))) {
            mask |= (uint16_t)(1u << i);
        }
    }
    settingsSetEnabledChannelsMask(mask);
    if (mask == 0) {
        Serial.println("[PORTAL] Aviso: todos los canales quedaron deshabilitados");
    }

    // Foto previa para detectar si el formulario cambió el perfil. Evita que
    // guardar solo WiFi/canales/calibración pise el respaldo local cuando la
    // fuente activa es el backend.
    const LocalProfile profileBefore = localProfileGetRuntime();

    // Perfil local: riego, horarios, cama caliente, interlocks y umbrales.
    for (uint8_t z = 0; z < settingsGetSubstrateZones() && z < MAX_SUBSTRATE_ZONES; z++) {
        const String base = "z" + String(z + 1);
        applyIrrigationZoneForm(ZoneType::SUBSTRATE, z, base);
    }
    applyIrrigationZoneForm(ZoneType::SPRINKLER, 0, String("asp"));

    applyChannelScheduleForm(RelayChannel::ILUMINACION, String("ilum"),
                             (uint16_t)(12UL * 3600UL));
    applyChannelScheduleForm(RelayChannel::VENTILACION, String("vent"), 300);

    irrigationSetInterlock(s_server.hasArg("interlock"));

    uint32_t limit = 0;
    if (parseUnsigned(s_server.arg("max_run_sub"), 1, IRRIGATION_MAX_RUN_ABS_S, limit)) {
        irrigationSetMaxRunS(ZoneType::SUBSTRATE, (uint16_t)limit);
    } else {
        Serial.println("[PORTAL] max_run sustrato invalido: se conserva");
    }
    if (parseUnsigned(s_server.arg("max_run_spr"), 1, IRRIGATION_MAX_RUN_ABS_S, limit)) {
        irrigationSetMaxRunS(ZoneType::SPRINKLER, (uint16_t)limit);
    } else {
        Serial.println("[PORTAL] max_run aspersion invalido: se conserva");
    }
    if (parseUnsigned(s_server.arg("min_off"), 0, IRRIGATION_MIN_OFF_MAX_S, limit)) {
        irrigationSetMinOffS((uint16_t)limit);
    } else {
        Serial.println("[PORTAL] min_off invalido: se conserva");
    }
    if (parseUnsigned(s_server.arg("stagger"), 0, 600, limit)) {
        irrigationSetStaggerS((uint16_t)limit);
    } else {
        Serial.println("[PORTAL] Espera entre zonas invalida: se conserva");
    }

    int tzOffset = 0;
    if (parseIntRange(s_server.arg("tz_offset"), -720, 840, tzOffset)) {
        settingsSetTimezoneOffsetMin((int16_t)tzOffset);
    } else {
        Serial.println("[PORTAL] Offset horario invalido: se conserva");
    }

    float target = 0.0f;
    if (parseFloatRange(s_server.arg("bed_target"), 5.0f, BED_MAX_C - 0.1f, target)) {
        settingsSetBedTargetC(target);
    } else {
        Serial.println("[PORTAL] Setpoint de cama invalido: se conserva");
    }

    climateSetBedAutoEnabled(s_server.hasArg("bed_auto"));
    climateSetVentAutoEnabled(s_server.hasArg("vent_auto"));
    climateSetVentClimateEnabled(s_server.hasArg("vent_climate"));

    float ventOnTemp = 0.0f;
    float ventOffTemp = 0.0f;
    float ventOnRh = 0.0f;
    float ventOffRh = 0.0f;
    const bool ventTempOk =
        parseFloatRange(s_server.arg("vent_on_temp"), -20.0f, 80.0f, ventOnTemp) &&
        parseFloatRange(s_server.arg("vent_off_temp"), -20.0f, 80.0f, ventOffTemp) &&
        ventOnTemp > ventOffTemp;
    const bool ventRhOk =
        parseFloatRange(s_server.arg("vent_on_rh"), 0.0f, 100.0f, ventOnRh) &&
        parseFloatRange(s_server.arg("vent_off_rh"), 0.0f, 100.0f, ventOffRh) &&
        ventOnRh > ventOffRh;
    if (ventTempOk && ventRhOk) {
        climateSetVentThresholds(ventOnTemp, ventOffTemp, ventOnRh, ventOffRh);
    } else {
        Serial.println("[PORTAL] Umbrales de ventilacion invalidos: se conservan");
    }

    float alarmMax = 0.0f;
    float alarmMin = 0.0f;
    const bool maxOk = parseFloatRange(s_server.arg("alarm_max_temp"),
                                       -20.0f, 80.0f, alarmMax);
    const bool minOk = parseFloatRange(s_server.arg("alarm_min_temp"),
                                       -50.0f, 40.0f, alarmMin);
    if (maxOk && minOk && alarmMax > alarmMin) {
        alarmSetMaxTemp(alarmMax);
        alarmSetMinTemp(alarmMin);
    } else {
        Serial.println("[PORTAL] Umbrales de alarma invalidos: se conservan");
    }

    // Calibración: para cada zona configurada leer dry/wet del form (0 = no
    // tocado → conserva el valor previo). Se valida dry != wet (si fueran
    // iguales el porcentaje sería siempre 50 %, ver rawToPercent).
    for (uint8_t z = 0; z < settingsGetSubstrateZones() && z < MAX_SUBSTRATE_ZONES; z++) {
        const String dry = s_server.arg("dry" + String(z));
        const String wet = s_server.arg("wet" + String(z));
        if (dry.length() > 0 && wet.length() > 0) {
            const uint16_t d = (uint16_t)dry.toInt();
            const uint16_t w = (uint16_t)wet.toInt();
            if (d != w) {
                ZoneCalibration c = calibrationGet(z);
                c.dryRaw = d;
                c.wetRaw = w;
                calibrationSet(z, c);  // marca calibrado=true (§8.4)
            } else {
                Serial.printf("[PORTAL] Zona %u: calibración ignorada (dry==wet=%u)\n",
                              z, (unsigned int)d);
            }
        }
    }

    // Persistir el perfil local solo si el formulario lo modificó.
    const LocalProfile profileAfter = localProfileGetRuntime();
    const bool profileChanged =
        (memcmp(&profileBefore, &profileAfter, sizeof(LocalProfile)) != 0);
    if (profileChanged) {
        localProfileSave();
    } else {
        Serial.println("[PORTAL] Perfil local sin cambios: no se reescribe NVS");
    }

    if (sourceChange) {
        settingsSetConfigSource(requestedSource);
        bool applied = false;
        if (requestedSource == ConfigSource::LOCAL) {
            applied = localProfileApplyStored();
        } else {
            applied = backendProfileApply();
        }
        Serial.printf("[PORTAL] Fuente de config → %s (%s)\n",
                      (requestedSource == ConfigSource::LOCAL) ? "local" : "backend",
                      applied ? "perfil aplicado" : "sin perfil guardado");

        // Si se pidió local pero el blob no se pudo aplicar, no dejar la fuente
        // apuntando a un perfil inexistente.
        if (requestedSource == ConfigSource::LOCAL && !applied) {
            settingsSetConfigSource(currentSource);
            Serial.println("[PORTAL] Sin perfil local valido: se conserva la fuente");
        }
    } else if (currentSource == ConfigSource::BACKEND && profileChanged) {
        // Con backend activo, las ediciones del portal quedan como respaldo
        // local pero no desplazan la config vigente del servidor.
        if (backendProfileApply()) {
            Serial.println("[PORTAL] Fuente backend: ediciones guardadas como "
                           "respaldo local");
        }
    }

    if (stateMachineGetMode() == OperationMode::MANTENIMIENTO) {
        s_server.send(200, "text/html; charset=utf-8",
                      "<html><body><h2>Cambios guardados</h2>"
                      "<p>El equipo sigue en MANTENIMIENTO: automatizacion suspendida "
                      "y reles OFF. Usa el boton de salida para volver a Normal.</p>"
                      "<p><a href='/'>Volver al portal</a></p></body></html>");
        Serial.println("[PORTAL] Cambios guardados (sigue en MANTENIMIENTO)");
        return;
    }

    s_server.send(200, "text/html; charset=utf-8",
                  "<html><body><h2>Cambios guardados</h2>"
                  "<p>El dispositivo vuelve al modo Normal sin reiniciar. "
                  "La automatizacion sigue activa.</p></body></html>");
    Serial.println("[PORTAL] Cambios guardados → saliendo del portal sin reiniciar");
    s_exitRequested = true;
}

static void handleSoilRaw() {
    s_lastActivityMs = millis();
    const uint8_t zone = (uint8_t)s_server.arg("zone").toInt();
    // Energiza, estabiliza, lee y apaga (no deja el módulo alimentado).
    const uint16_t raw = sensorsReadSoilRawAdc(zone);
    char json[32];
    snprintf(json, sizeof(json), "{\"raw\":%u}", (unsigned int)raw);
    s_server.send(200, "application/json", json);
}

static void stopChannelTest() {
    if (s_testChannel >= 0) {
        relaySetChannel((RelayChannel)s_testChannel, false);
        s_testChannel = -1;
        s_testEndMs = 0;
    }
}

// Página simple de confirmación tras un borrado. El reinicio se programa en
// configPortalUpdate() para que la respuesta llegue completa al navegador.
static void sendResetPage(const char* title, const char* message) {
    String html;
    html.reserve(420);
    html += "<!DOCTYPE html><html lang='es'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>Vivero IoT</title></head><body style='font-family:sans-serif;"
            "max-width:640px;margin:20px auto;padding:0 12px'>";
    html += "<h2>" + String(title) + "</h2><p>" + String(message) + "</p>";
    html += "<p>El equipo se reiniciara en unos segundos.</p></body></html>";
    s_server.send(200, "text/html; charset=utf-8", html);
}

static String normalizedConfirm() {
    String value = s_server.arg("confirm");
    value.trim();
    value.toUpperCase();
    return value;
}

static void handleWifiReset() {
    s_lastActivityMs = millis();
    if (normalizedConfirm() != "WIFI") {
        s_server.send(400, "text/plain; charset=utf-8",
                      "Confirmacion invalida: escribe WIFI");
        return;
    }

    stopChannelTest();
    relayAllOff();
    factoryResetWifiOnly();
    sendResetPage("WiFi borrado",
                  "Se borraron las credenciales WiFi y la clave del AP. El resto "
                  "de la configuracion se conserva.");
    s_restartPending = true;
    s_restartAtMs = millis() + 1500UL;
    Serial.println("[PORTAL] Borrado de solo WiFi solicitado");
}

static void handleFactoryReset() {
    s_lastActivityMs = millis();
    if (normalizedConfirm() != "RESET") {
        s_server.send(400, "text/plain; charset=utf-8",
                      "Confirmacion invalida: escribe RESET");
        return;
    }

    stopChannelTest();
    relayAllOff();
    factoryResetAll();
    sendResetPage("Reset de fabrica",
                  "Se borro toda la configuracion persistida: WiFi, AP, backend, "
                  "identidad, perfiles, canales, calibracion y alarmas.");
    s_restartPending = true;
    s_restartAtMs = millis() + 1500UL;
    Serial.println("[PORTAL] Reset de fabrica solicitado");
}

static void handleTest() {
    s_lastActivityMs = millis();

    const int value = s_server.arg("ch").toInt();
    if (value < 0 || value >= RELAY_CHANNEL_COUNT) {
        s_server.send(400, "text/plain; charset=utf-8", "Canal invalido");
        return;
    }

    const RelayChannel channel = (RelayChannel)value;
    if (!settingsIsChannelEnabled(channel)) {
        s_server.send(409, "text/plain; charset=utf-8",
                      "Canal deshabilitado: habilitalo y guarda primero");
        return;
    }
    if (s_testChannel >= 0) {
        s_server.send(409, "text/plain; charset=utf-8",
                      "Ya hay una prueba en curso");
        return;
    }

    if (!relaySetChannel(channel, true)) {
        s_server.send(500, "text/plain; charset=utf-8", "No se pudo activar");
        return;
    }

    // En mantenimiento la prueba puede durar 30 s (docs/03 §2.2); en el resto
    // del portal se mantiene el auto-off corto de 10 s.
    const uint32_t testMs = (stateMachineGetMode() == OperationMode::MANTENIMIENTO)
                                ? PORTAL_TEST_MAINT_MS
                                : PORTAL_TEST_MS;
    s_testChannel = (int8_t)value;
    s_testEndMs = millis() + testMs;

    char msg[96];
    snprintf(msg, sizeof(msg), "Prueba de %s iniciada (auto-off en %lu s)",
             relayChannelLabel(channel), (unsigned long)(testMs / 1000UL));
    s_server.send(200, "text/plain; charset=utf-8", msg);
    Serial.printf("[PORTAL] Prueba canal %s durante %lu s\n",
                  relayChannelName(channel), (unsigned long)(testMs / 1000UL));
}

static const char* windowProtocolName() {
    switch (windowGetState()) {
        case WindowState::CLOSED: return "closed";
        case WindowState::OPEN:   return "open";
        case WindowState::MOVING: return "moving";
        case WindowState::FAULT:  return "fault";
    }
    return "unknown";
}

static void appendAlarmConditionList(String& out, AlarmMask mask) {
    bool first = true;
    for (uint8_t i = 1; i < ALARM_CONDITION_COUNT; i++) {
        const AlarmCondition condition = (AlarmCondition)i;
        if ((mask & ALARM_BIT(condition)) == 0) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        out += "\"";
        out += alarmConditionKey(condition);
        out += "\"";
        first = false;
    }
}

static void handleAlarm() {
    s_lastActivityMs = millis();

    const String action = s_server.arg("action");
    if (action == "silence") {
        alarmSilence(0);
        s_server.send(200, "text/plain; charset=utf-8",
                      "Alarma silenciada 10 min (sin borrar condiciones)");
        return;
    }
    if (action == "reset") {
        alarmResetLatches();
        s_server.send(200, "text/plain; charset=utf-8", "Latches de alarma limpiados");
        return;
    }
    if (action.length() > 0) {
        s_server.send(400, "text/plain; charset=utf-8", "Accion invalida");
        return;
    }

    String json;
    json.reserve(420);
    json += "{";
    json += "\"active\":";
    json += alarmIsActive() ? "true" : "false";
    json += ",\"silenced\":";
    json += alarmIsSilenced() ? "true" : "false";
    json += ",\"silence_remaining_s\":";
    json += String(alarmSilenceRemainingMs() / 1000UL);
    json += ",\"channel_enabled\":";
    json += settingsIsChannelEnabled(RelayChannel::ALARMA) ? "true" : "false";
    json += ",\"conditions\":[";
    appendAlarmConditionList(json, alarmGetActiveMask());
    json += "],\"latched\":[";
    appendAlarmConditionList(json, alarmGetLatchedMask());
    json += "]}";

    s_server.send(200, "application/json", json);
}

static void handleMaintenance() {
    s_lastActivityMs = millis();
    const String action = s_server.arg("action");

    if (action == "enter") {
        if (stateMachineGetMode() != OperationMode::PORTAL) {
            s_server.send(409, "text/plain; charset=utf-8",
                          "Solo se puede entrar en mantenimiento desde el modo Portal");
            return;
        }
        stopChannelTest();
        // Cortar cualquier viaje de ventana ANTES de forzar los relés a OFF:
        // si no, windowUpdate() esperaría la transición del sensor y levantaría
        // WINDOW_FAULT (latch por defecto) 20 s después por un corte nuestro.
        windowAbortMotion();
        // Lo mismo que la cama caliente en §2.2: apagar el relé dejando el
        // estado activo haría que el próximo ciclo reportara un vencimiento.
        irrigationStopAllActive();
        relayAllOff();
        stateMachineEnterMaintenance();
        s_server.send(200, "text/plain; charset=utf-8",
                      "MANTENIMIENTO activo: reles OFF, automatizacion y botones "
                      "suspendidos. Las pruebas duran 30 s.");
        Serial.println("[PORTAL] Entrada a MANTENIMIENTO: 8 reles OFF");
        return;
    }

    if (action == "exit") {
        if (stateMachineGetMode() != OperationMode::MANTENIMIENTO) {
            s_server.send(409, "text/plain; charset=utf-8",
                          "El equipo no esta en mantenimiento");
            return;
        }
        stopChannelTest();
        windowAbortMotion();   // estado de ventana consistente al volver a NORMAL
        relayAllOff();
        stateMachineExitMaintenance();
        s_exitRequested = true;  // configPortalUpdate cierra el AP y reanuda STA
        s_server.send(200, "text/plain; charset=utf-8",
                      "Saliendo de mantenimiento: vuelve a NORMAL y se reanuda la "
                      "automatizacion.");
        Serial.println("[PORTAL] Salida de MANTENIMIENTO → NORMAL sin reiniciar");
        return;
    }

    char json[64];
    snprintf(json, sizeof(json), "{\"mode\":\"%s\"}",
             (stateMachineGetMode() == OperationMode::MANTENIMIENTO)
                 ? "mantenimiento" : "portal");
    s_server.send(200, "application/json", json);
}

static void handleSensors() {
    s_lastActivityMs = millis();

    // 1) Dispara las 3 conversiones DS18B20: buses independientes, en paralelo.
    sensorsRequestAllTemps();

    // 2) La ventana de conversión se aprovecha para leer el ADC de humedad.
    const uint16_t raw1 = sensorsReadSoilRawAdc(0);
    const uint16_t raw2 = sensorsReadSoilRawAdc(1);

    // 3) Única espera del bloque: conversión de 12 bits (750 ms máx.).
    //    Total ~800 ms; antes eran 3 × 800 ms en serie (2,45 s) y eso
    //    congelaba botones, corte de relés y alarmas durante el diagnóstico.
    delay(750);

    float temps[MAX_DS18B20] = {0};
    bool ok[MAX_DS18B20] = {false};
    for (uint8_t i = 0; i < MAX_DS18B20; i++) {
        ok[i] = sensorsReadDs18b20Ready(i, temps[i]);
    }

    const SensorReadings& last = localCycleLastReadings();

    // Peor caso medido (temp/humedad con signo, 3 sondas y window): ~240 B.
    // 512 deja holgura para sumar campos sin pisar el fin de cadena.
    char json[512];
    snprintf(json, sizeof(json),
             "{\"air\":{\"valid\":%d,\"t\":%.2f,\"h\":%.2f},"
             "\"soil\":[{\"raw\":%u},{\"raw\":%u}],"
             "\"ds18b20\":["
             "{\"name\":\"sustrato Z1\",\"valid\":%d,\"t\":%.2f},"
             "{\"name\":\"sustrato Z2\",\"valid\":%d,\"t\":%.2f},"
             "{\"name\":\"cama caliente\",\"valid\":%d,\"t\":%.2f}],"
             "\"window\":\"%s\"}",
             last.air.valid ? 1 : 0, last.air.tempC, last.air.humidityPct,
             (unsigned)raw1, (unsigned)raw2,
             ok[0] ? 1 : 0, temps[0],
             ok[1] ? 1 : 0, temps[1],
             ok[2] ? 1 : 0, temps[2],
             windowProtocolName());

    s_server.send(200, "application/json", json);
}

static void handleNotFound() {
    s_lastActivityMs = millis();
    // En el AP, cualquier ruta desconocida vuelve al portal (captive portal).
    String url = String("http://") + String(s_apIp) + "/";
    s_server.sendHeader("Location", url);
    s_server.send(302, "text/plain", "");
}

// ----------------------------------------------------------------------------
// Ciclo de vida del portal
// ----------------------------------------------------------------------------

void configPortalStart() {
    if (s_running) {
        return;
    }

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String ssid = String(AP_SSID_PREFIX) + mac.substring(mac.length() - 4);

    // Clave WPA2: la guardada desde el portal o, si no hay, la inicial
    // derivada de la MAC (>= 8 caracteres). Siempre visible en la OLED.
    String apPassword;
    if (settingsGetApPassword()[0] != '\0') {
        apPassword = settingsGetApPassword();
    } else {
        apPassword = String("vivero") + mac.substring(mac.length() - 6);
    }

    WiFi.mode(WIFI_AP);  // Un solo modo a la vez (regla 2 de CLINE.md)
    WiFi.softAP(ssid.c_str(), apPassword.c_str());
    strncpy(s_apIp, WiFi.softAPIP().toString().c_str(), sizeof(s_apIp) - 1);
    s_apIp[sizeof(s_apIp) - 1] = '\0';
    strncpy(s_apSsid, ssid.c_str(), sizeof(s_apSsid) - 1);
    s_apSsid[sizeof(s_apSsid) - 1] = '\0';
    strncpy(s_apPassword, apPassword.c_str(), sizeof(s_apPassword) - 1);
    s_apPassword[sizeof(s_apPassword) - 1] = '\0';

    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/save", HTTP_POST, handleSave);
    s_server.on("/wifi-reset", HTTP_POST, handleWifiReset);
    s_server.on("/factory-reset", HTTP_POST, handleFactoryReset);
    s_server.on("/api/soil-raw", HTTP_GET, handleSoilRaw);
    s_server.on("/api/test", HTTP_GET, handleTest);
    s_server.on("/api/sensors", HTTP_GET, handleSensors);
    s_server.on("/api/alarm", HTTP_GET, handleAlarm);
    s_server.on("/api/maintenance", HTTP_GET, handleMaintenance);
    s_server.onNotFound(handleNotFound);
    s_server.begin();

    // DNS cautivo: cualquier nombre resuelve a la IP del AP.
    s_dnsServer.start(53, "*", WiFi.softAPIP());

    s_lastActivityMs = millis();
    s_timeout = false;
    s_warned = false;
    s_exitRequested = false;
    s_restartPending = false;
    s_running = true;

    Serial.printf("[PORTAL] AP iniciado: %s (IP: %s)\n",
                  ssid.c_str(), WiFi.softAPIP().toString().c_str());
}

void configPortalStop() {
    if (!s_running) {
        return;
    }
    stopChannelTest();
    s_dnsServer.stop();
    s_server.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    s_running = false;
    s_exitRequested = false;
    s_apIp[0] = '\0';
    s_apSsid[0] = '\0';
    s_apPassword[0] = '\0';
    Serial.println("[PORTAL] AP y servidor detenidos");
}

void configPortalUpdate() {
    if (!s_running) {
        return;
    }

    s_dnsServer.processNextRequest();
    s_server.handleClient();

    // Reset de fábrica / WiFi: la respuesta ya se envió; reiniciar unos ms
    // después para que el navegador reciba la página completa.
    if (s_restartPending && (int32_t)(millis() - s_restartAtMs) >= 0) {
        s_restartPending = false;
        Serial.println("[PORTAL] Reiniciando por reset de configuracion");
        configPortalStop();
        Serial.flush();
        delay(100);
        stateMachineRestart();
    }

    // Guardar cambios: el handler marcó la salida. Se cierra el portal acá,
    // fuera del handler HTTP, para que la respuesta llegue al navegador.
    if (s_exitRequested) {
        configPortalStop();
        stateMachineExitConfig();
        return;
    }

    // Auto-off de la prueba de canal.
    if (s_testChannel >= 0 && (int32_t)(millis() - s_testEndMs) >= 0) {
        Serial.printf("[PORTAL] Fin de prueba del canal %d\n", (int)s_testChannel);
        stopChannelTest();
    }

    const uint32_t now = millis();
    if ((now - s_lastActivityMs) >= CONFIG_TIMEOUT_MS) {
        s_timeout = true;
        Serial.println("[PORTAL] Timeout de inactividad (15 min) alcanzado");
    } else if ((now - s_lastActivityMs) >= CONFIG_WARN_MS && !s_warned) {
        s_warned = true;
        Serial.println("[PORTAL] Aviso: reinicio por inactividad en ~2 min");
        // El aviso se muestra en el portal en el próximo render de / (warn div).
    }
}

bool configPortalIsTimeout() {
    return s_timeout;
}

const char* configPortalGetIp() {
    return s_apIp;
}

const char* configPortalGetSsid() {
    return s_apSsid;
}

const char* configPortalGetPassword() {
    return s_apPassword;
}

bool configPortalTestActive() {
    return s_testChannel >= 0;
}