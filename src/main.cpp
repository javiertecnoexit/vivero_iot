#include <Arduino.h>

#include "alarm.h"
#include "buttons.h"
#include "channel_control.h"
#include "climate.h"
#include "config.h"
#include "config_portal.h"
#include "display.h"
#include "events.h"
#include "factory_reset.h"
#include "irrigation.h"
#include "local_cycle.h"
#include "local_profile.h"
#include "manual_ui.h"
#include "ntp_sync.h"
#include "relay.h"
#include "rtc.h"
#include "scheduler.h"
#include "sensors.h"
#include "settings.h"
#include "state_machine.h"
#include "wifi_client.h"
#include "window.h"
#include "ws_client.h"

// ============================================================================
// main.cpp — Orquestación del firmware (Milestone: Portal completo)
// Máquina de estados Normal/Configuración con sensores, riego, alarmas y red.
// Regla 1 de CLINE.md: sin delay() bloqueante en el loop principal.
// ============================================================================

// True cuando el portal debe abrirse sin haber presionado el botón:
// primer arranque sin credenciales WiFi o falla de conexión > 60 s (§4.4).
static bool s_autoOpenPortal = false;

// Gesto de reset de fábrica al arrancar (docs/03 §11.3):
//   - mantener Configuración 10 s desde el encendido;
//   - soltar y pulsar Confirmar dentro de los 5 s siguientes.
// Se ejecuta en setup(), antes de inicializar relés y automatización. Los
// delay() son intencionales: es una rutina de arranque, no el loop principal.
//
// GPIO36 y GPIO39 son solo entrada y SIN pull-up interno: sin el pull-up
// externo de 10 kΩ el pin flota. Estos ajustes cubren dos fallos reales:
//   1) un pin permanentemente en bajo dejaba setup() colgado PARA SIEMPRE
//      (el equipo no arrancaba y no había forma de recuperarlo);
//   2) ruido momentáneo disparaba o cancelaba la secuencia.
#define BOOT_BTN_STABLE_MS          100UL   // estabilización de una lectura
#define BOOT_BTN_RELEASE_TIMEOUT_MS 5000UL  // tope al soltar Configuración

// true si el pin lee LOW de forma ininterrumpida durante `ms`.
static bool bootBtnStableLow(uint8_t pin, uint32_t ms) {
    const uint32_t start = millis();
    do {
        if (digitalRead(pin) != LOW) {
            return false;
        }
        delay(5);
    } while ((millis() - start) < ms);
    return true;
}

static bool factoryResetBootGesture() {
    // 1) Entrada: debe haber LOW estable. Un pico de ruido no dispara el gesto
    //    ni mete al equipo en 10 s de cuenta regresiva sin motivo.
    if (!bootBtnStableLow(PIN_BTN_CONFIG, BOOT_BTN_STABLE_MS)) {
        return false;  // no estaba presionado al arrancar
    }

    const uint32_t startMs = millis();
    int lastShown = -1;
    bool releaseSeen = false;
    uint32_t releaseSinceMs = 0;

    for (;;) {
        const uint32_t heldMs = millis() - startMs;
        const int remaining = 10 - (int)(heldMs / 1000UL);
        if (remaining != lastShown) {
            char line[24];
            snprintf(line, sizeof(line), "Mantener %d s", remaining > 0 ? remaining : 0);
            displayShowFactoryReset("RESET DE FABRICA", line, -1);
            lastShown = remaining;
        }
        if (heldMs >= 10000UL) {
            break;
        }
        if (digitalRead(PIN_BTN_CONFIG) != LOW) {
            // Cancelar solo tras BOOT_BTN_STABLE_MS de alto estable: un pico
            // de ruido no debe abortar el gesto a mitad de camino.
            if (!releaseSeen) {
                releaseSeen = true;
                releaseSinceMs = millis();
            } else if ((millis() - releaseSinceMs) >= BOOT_BTN_STABLE_MS) {
                displayShowFactoryReset("RESET CANCELADO", "Arranque normal", -1);
                delay(800);
                return false;
            }
        } else {
            releaseSeen = false;
        }
        delay(20);
    }

    // 2) Esperar a que se suelte Configuración antes de pedir Confirmar.
    //    TOPE OBLIGATORIO: con el pin en bajo permanente (pull-up ausente o
    //    cable en corto) este bucle nunca saldría y el equipo no arrancaría.
    const uint32_t releaseMs = millis();
    while (digitalRead(PIN_BTN_CONFIG) == LOW) {
        if ((millis() - releaseMs) >= BOOT_BTN_RELEASE_TIMEOUT_MS) {
            Serial.println("[BOOT] GPIO39 sigue en bajo tras soltar: falta el "
                           "pull-up externo. Gesto de reset cancelado.");
            displayShowFactoryReset("GPIO39 EN BAJO", "Revisar pull-up", -1);
            delay(1500);
            return false;
        }
        delay(20);
    }

    // 3) Pedir Confirmar dentro de los 5 s. Se exige haber leído el pin en ALTO
    //    alguna vez: sin eso no se puede distinguir "botón pulsado" de "pin en
    //    corto", y un pin en corto habría borrado la configuración solo.
    const uint32_t promptMs = millis();
    lastShown = -1;
    bool confirmReleasedSeen = false;
    bool confirmPressed = false;
    uint32_t confirmSinceMs = 0;

    while ((millis() - promptMs) < 5000UL) {
        const int remaining = 5 - (int)((millis() - promptMs) / 1000UL);
        if (remaining != lastShown) {
            displayShowFactoryReset("CONFIRMAR RESET", "Pulsa Confirmar", remaining);
            lastShown = remaining;
        }

        if (digitalRead(PIN_BTN_CONFIRM) == HIGH) {
            confirmReleasedSeen = true;
            confirmPressed = false;
        } else if (confirmReleasedSeen) {
            if (!confirmPressed) {
                confirmPressed = true;
                confirmSinceMs = millis();
            } else if ((millis() - confirmSinceMs) >= BOOT_BTN_STABLE_MS) {
                return true;  // press estable y pull-up verificado
            }
        }
        delay(20);
    }

    if (!confirmReleasedSeen) {
        Serial.println("[BOOT] GPIO36 nunca leyo alto: falta el pull-up externo. "
                       "Gesto de reset cancelado.");
        displayShowFactoryReset("GPIO36 EN BAJO", "Revisar pull-up", -1);
        delay(1500);
    }

    displayShowFactoryReset("RESET CANCELADO", "Arranque normal", -1);
    delay(800);
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(200);  // espera corta de arranque para el monitor (no es el loop)
    Serial.println();
    Serial.println("[VIVERO] Firmware iniciado (Portal completo + NVS)");

    settingsInit();  // NVS: WiFi, zonas, identidad (antes que el resto)
    rtcInit();
    buttonsInit();
    displayInit();

    // Reset de fábrica por gesto físico antes de energizar relés.
    if (factoryResetBootGesture()) {
        displayShowFactoryReset("BORRANDO...", "No apagar el equipo", -1);
        factoryResetAll();
        Serial.println("[RESET] Reiniciando en modo PORTAL...");
        Serial.flush();
        delay(300);
        ESP.restart();
    }

    stateMachineInit();
    relayInit();
    windowInit();
    channelControlInit();
    climateInit();
    schedulerInit();
    sensorsInit();
    irrigationInit();
    eventsInit();
    alarmInit();
    localProfileInit();  // perfil local: horarios, umbrales y límites (M5)
    manualUiInit();
    localCycleInit();
    wsInit();
    ntpSyncInit();

    // Sin credenciales WiFi guardadas → el dispositivo entra directo al portal
    // (decisión acordada: sin red no puede operar útilmente).
    if (!settingsHasWifiCredentials()) {
        s_autoOpenPortal = true;  // flag consultado en la primera pasada del loop
        Serial.println("[MAIN] Sin credenciales WiFi → abriré el portal");
    } else {
        wifiClientStart();  // conexión STA no bloqueante (modo Normal)
    }
}

void loop() {
    buttonsUpdate();
    windowUpdate();        // sensor, pulso toggle y transiciones de ventana
    relayUpdate();         // corte de seguridad del pulso (5 s) y canales deshabilitados
    alarmUpdate();         // vencimiento del silencio y relé de alarma
    channelControlUpdate();  // vencimiento de órdenes manuales por canal
    // Corte por duración y max_run del riego en el MISMO paso del loop: sin
    // esto dependía del ciclo local de 30 s y max_run_s podía excederse en 30 s
    // (o no cortarse nunca si el ciclo no corría).
    irrigationEnforceLimits();

    const OperationMode mode = stateMachineGetMode();

    // ------------------------------------------------------------------
    // MANTENIMIENTO: AP y portal activos, automatización y botones no.
    // ------------------------------------------------------------------
    if (mode == OperationMode::MANTENIMIENTO) {
        // La prueba de canal del portal es la única excepción al OFF forzado.
        // relayAnyOn() evita reescribir los 8 pines en cada pasada del loop
        // (corre a cientos de Hz y todos están apagados el resto del tiempo).
        if (!configPortalTestActive() && relayAnyOn()) {
            relayAllOff();   // incluye el canal de alarma
        }
        configPortalUpdate();

        if (configPortalIsTimeout()) {
            Serial.println("[MAIN] Mantenimiento por timeout → NORMAL sin reiniciar");
            configPortalStop();
            stateMachineExitMaintenance();
        }

        if (stateMachineGetMode() == OperationMode::NORMAL) {
            wsNotifyPortalExit();  // reanuda la sesión WebSocket tras el portal
            if (!wifiClientStart()) {
                Serial.println("[MAIN] Sin credenciales WiFi: sigue en NORMAL offline");
            }
        } else {
            displayShowMaintenance(configPortalGetIp(), configPortalGetSsid(),
                                   configPortalGetPassword());
        }
        return;
    }

    if (mode == OperationMode::NORMAL || mode == OperationMode::PORTAL) {
        // Transición Normal → Portal por botón físico (GPIO39).
        if (mode == OperationMode::NORMAL && buttonsWasPressedConfig()) {
            stateMachineEnterConfig();
            wsNotifyPortalEnter();  // cierra la sesión WS y registra el evento
            wifiClientStop();       // AP en lugar de STA (regla 2)
            configPortalStart();
            manualUiForceIdle();
        }

        if (stateMachineGetMode() == OperationMode::NORMAL) {
            // Auto-recuperación: sin credenciales o falla de WiFi > 60 s → portal.
            if (s_autoOpenPortal || wifiClientTimedOut()) {
                Serial.println("[MAIN] Auto-recuperación: abriendo el portal");
                stateMachineEnterConfig();
                wsNotifyPortalEnter();
                wifiClientStop();
                configPortalStart();
                manualUiForceIdle();
                s_autoOpenPortal = false;
            } else {
                // Mantiene viva la conexión STA (no bloqueante).
                wifiClientUpdate();
                // Mantiene el RTC sincronizado con NTP cuando hay WiFi.
                ntpSyncUpdate();
                // M6: sesión WebSocket con el backend (telemetría, eventos,
                // config y comandos). Sin red la operación local sigue igual.
                wsUpdate();
            }
        }

        // Automatización local activa en NORMAL y en PORTAL (docs/03 §2).
        // El portal solo configura; no suspende riego, clima ni alarmas.
        localCycleUpdate();

        // Silencio local: mantener Confirmar 3 s con una alarma activa.
        if (alarmIsActive() && buttonsWasHeldConfirm(ALARM_SILENCE_HOLD_MS)) {
            alarmSilence(0);
            Serial.println("[MAIN] Alarma silenciada con Confirmar (3 s)");
        }

        // UI de riego manual: se procesa en CADA pasada del loop (no atada al
        // ciclo de 30 s) para responder al instante a Selector/Confirmar.
        // Los botones están activos en NORMAL y PORTAL (docs/03 §3.2).
        manualUiUpdate();

        if (stateMachineGetMode() == OperationMode::PORTAL) {
            // Atiende el portal y revisa el timeout de inactividad.
            configPortalUpdate();

            if (configPortalIsTimeout()) {
                Serial.println("[MAIN] Portal por timeout → volviendo a NORMAL sin reiniciar");
                configPortalStop();
                stateMachineExitConfig();
            }

            // Solo si se volvió a NORMAL (guardar o timeout) se reanuda la STA.
            // Si se entró a MANTENIMIENTO, el AP debe seguir activo.
            if (stateMachineGetMode() == OperationMode::NORMAL) {
                wsNotifyPortalExit();  // nuevo intento de sesión WS
                if (!wifiClientStart()) {
                    Serial.println("[MAIN] Sin credenciales WiFi: sigue en NORMAL offline");
                }
            }
        }

        if (!manualUiIsSelecting()) {
            const struct tm t = rtcNowLocal();  // hora local (UTC-3) solo en pantalla
            const OperationMode nowMode = stateMachineGetMode();
            if (nowMode == OperationMode::PORTAL) {
                displayShowConfig(configPortalGetIp(), configPortalGetSsid(),
                                  configPortalGetPassword());
            } else if (nowMode == OperationMode::MANTENIMIENTO) {
                displayShowMaintenance(configPortalGetIp(), configPortalGetSsid(),
                                       configPortalGetPassword());
            } else {
                const SensorReadings& last = localCycleLastReadings();
                displayShowNormal(localCycleCount(), (uint8_t)t.tm_hour, (uint8_t)t.tm_min,
                                  (uint8_t)t.tm_sec, last,
                                  alarmIsActive(), alarmIsSilenced(), alarmGetActiveCondition(),
                                  windowGetState());
            }
        }
    }
}