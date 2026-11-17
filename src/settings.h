#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#include "config.h"

// ============================================================================
// settings.h — Persistencia local en NVS
// Guarda en NVS (Preferences):
// - Credenciales WiFi (SSID/password, nunca en fuente)
// - Canales de relé habilitados (bitmask de 8 bits)
// - Calibración y configuración local (M2/M5)
// - device_id + token de autenticación del backend
// ============================================================================

// Máximos según estándar WPA2/WPA3 (256 bits).
#define SETTINGS_SSID_MAX      32
#define SETTINGS_PASS_MAX      63
#define SETTINGS_ID_MAX        64
#define SETTINGS_TOKEN_MAX     128
#define SETTINGS_AP_PASS_MAX   63
#define SETTINGS_CLAIM_MAX     32
// URL del backend (http://host:puerto o https://...). Máximo prudente.
#define SETTINGS_BACKEND_URL_MAX 200
// URL WebSocket devuelta por el claim/alta (ws://... o wss://...).
#define SETTINGS_WS_URL_MAX    200

// Carga la configuración desde NVS (o aplica valores de fábrica). Debe
// llamarse en setup() antes de usar el resto de la API.
void settingsInit();

// --- WiFi ---
// Devuelve true si hay credenciales WiFi guardadas (SSID no vacío).
bool settingsHasWifiCredentials();
const char* settingsGetWifiSsid();
const char* settingsGetWifiPassword();
void settingsSetWifi(const char* ssid, const char* password);

// Borra solo las credenciales WiFi (STA) y la clave del AP. Conserva el resto:
// identidad, perfiles, canales, calibración y alarmas. Tras el reinicio el
// equipo abre el portal por no tener credenciales.
void settingsEraseWifi();

// Borra TODO el namespace de configuración (WiFi, AP, identidad, claim,
// backend, perfiles, canales, cama y latches) y restaura los valores de
// fábrica en RAM. No reinicia: el llamador decide cuándo hacerlo.
void settingsFactoryReset();

// --- Zonas (fijas en firmware: 2 sustrato / 1 aspersión) ---
// Se conservan para compatibilidad con los módulos de la base; el portal ya
// no permite cambiarlas.
uint8_t settingsGetSubstrateZones();
uint8_t settingsGetSprinklerZones();
void settingsSetZones(uint8_t substrateZones, uint8_t sprinklerZones);

// --- Canales de relé habilitados (bitmask, bit 0 = RIEGO_Z1) ---
// Un canal deshabilitado no se automatiza, no se comanda y no aparece en el
// menú de botones. La habilitación es local (portal).
#define SETTINGS_DEFAULT_CHANNELS_MASK 0x00FF  // los 8 habilitados de fábrica
bool settingsIsChannelEnabled(RelayChannel channel);
uint16_t settingsGetEnabledChannelsMask();
void settingsSetEnabledChannelsMask(uint16_t mask);
void settingsSetChannelEnabled(RelayChannel channel, bool enabled);

// --- Cama caliente ---
// Setpoint local en °C (el corte absoluto BED_MAX_C no es configurable).
float settingsGetBedTargetC();
void settingsSetBedTargetC(float targetC);

// --- Alarmas ---
// Latches persistidos en NVS (bitmask AlarmMask). Se cargan al arrancar para
// que una alarma retenida siga sonando tras un reinicio.
uint16_t settingsGetAlarmLatches();
void settingsSetAlarmLatches(uint16_t mask);

// --- Punto de acceso (clave configurable desde el portal) ---
// Cadena vacía = usar la clave inicial derivada de la MAC.
const char* settingsGetApPassword();
void settingsSetApPassword(const char* password);  // "" o 8..63 caracteres

// --- Identidad del dispositivo (todo del backend en M4) ---
bool settingsHasDeviceIdentity();
const char* settingsGetDeviceId();
const char* settingsGetApiToken();
void settingsSetDeviceIdentity(const char* deviceId, const char* apiToken);

// --- Provisioning (M6: POST /devices/claim) ---
const char* settingsGetClaimCode();
void settingsSetClaimCode(const char* claimCode);

// --- Fuente de configuración activa ---
// BACKEND: manda la última config recibida (default).
// LOCAL: override explícito; el backend se guarda pero no se aplica.
enum class ConfigSource : uint8_t {
    BACKEND = 0,
    LOCAL   = 1
};
ConfigSource settingsGetConfigSource();
void settingsSetConfigSource(ConfigSource source);

// --- URL del backend (configurable desde el portal) ---
const char* settingsGetBackendUrl();
void settingsSetBackendUrl(const char* url);

// --- URL WebSocket (la devuelve el claim o se deriva del backend) ---
const char* settingsGetWsUrl();
void settingsSetWsUrl(const char* url);

// --- Zona horaria para horarios y pantalla (minutos respecto de UTC) ---
// Default -180 (UTC-3). Rango válido -720..840.
int16_t settingsGetTimezoneOffsetMin();
void settingsSetTimezoneOffsetMin(int16_t minutes);

// --- Versión de configuración (cacheada de la nube, sección 9) ---
// "siempre gana la versión más alta recibida" (especificacion_firmware.md §9).
// force=true permite aplicar una versión menor (campo force del servidor).
uint32_t settingsGetConfigVersion();
void settingsSetConfigVersion(uint32_t version, bool force = false);

// --- Versión del perfil local ---
// Se incrementa cada vez que el portal guarda un perfil local distinto.
uint32_t settingsGetLocalVersion();
void settingsBumpLocalVersion();

#endif // SETTINGS_H