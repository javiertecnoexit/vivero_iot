#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"

// ============================================================================
// settings.cpp — Persistencia local en NVS (Preferences)
// Fuente: especificacion_firmware.md sección 9.
// - Namespace único "cfg" para todas las claves locales del dispositivo.
// - Strings se guardan con getString/putString (no acotadas por tamaño fijo).
// - Cantidad de zonas: defaults de fábrica 2/1 si no hay clave guardada.
// ============================================================================

static const char* NVS_NAMESPACE = "cfg";

static char s_wifiSsid[SETTINGS_SSID_MAX + 1] = {0};
static char s_wifiPass[SETTINGS_PASS_MAX + 1] = {0};
static uint8_t s_substrateZones = DEFAULT_SUBSTRATE_ZONES;
static uint8_t s_sprinklerZones = DEFAULT_SPRINKLER_ZONES;
static char s_deviceId[SETTINGS_ID_MAX + 1] = {0};
static char s_apiToken[SETTINGS_TOKEN_MAX + 1] = {0};
static char s_apPass[SETTINGS_AP_PASS_MAX + 1] = {0};
static char s_claimCode[SETTINGS_CLAIM_MAX + 1] = {0};
static char s_backendUrl[SETTINGS_BACKEND_URL_MAX + 1] = {0};
static char s_wsUrl[SETTINGS_WS_URL_MAX + 1] = {0};
static int16_t s_timezoneOffsetMin = -180;
static ConfigSource s_configSource = ConfigSource::BACKEND;
static uint32_t s_configVersion = 0;
static uint32_t s_localVersion = 0;
static uint16_t s_enabledChannelsMask = SETTINGS_DEFAULT_CHANNELS_MASK;
static float s_bedTargetC = DEFAULT_BED_TARGET_C;
static uint16_t s_alarmLatches = 0;

void settingsInit() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);  // read-only

    String ssid = prefs.getString("wifi_ssid", "");
    String pass = prefs.getString("wifi_pass", "");
    ssid.toCharArray(s_wifiSsid, sizeof(s_wifiSsid));
    pass.toCharArray(s_wifiPass, sizeof(s_wifiPass));

    s_substrateZones = (uint8_t)prefs.getUInt("sub_zones", DEFAULT_SUBSTRATE_ZONES);
    s_sprinklerZones = (uint8_t)prefs.getUInt("spr_zones", DEFAULT_SPRINKLER_ZONES);
    // Acotar a rangos físicos por si NVS quedó con un valor inválido.
    if (s_substrateZones < 1) s_substrateZones = 1;
    if (s_substrateZones > MAX_SUBSTRATE_ZONES) s_substrateZones = MAX_SUBSTRATE_ZONES;
    if (s_sprinklerZones > MAX_SPRINKLER_ZONES) s_sprinklerZones = MAX_SPRINKLER_ZONES;

    String devId = prefs.getString("device_id", "");
    String token = prefs.getString("api_token", "");
    devId.toCharArray(s_deviceId, sizeof(s_deviceId));
    token.toCharArray(s_apiToken, sizeof(s_apiToken));

    String apPass = prefs.getString("ap_pass", "");
    apPass.toCharArray(s_apPass, sizeof(s_apPass));
    String claim = prefs.getString("claim_code", "");
    claim.toCharArray(s_claimCode, sizeof(s_claimCode));

    String url = prefs.getString("backend_url", "");
    url.toCharArray(s_backendUrl, sizeof(s_backendUrl));
    String wsUrl = prefs.getString("ws_url", "");
    wsUrl.toCharArray(s_wsUrl, sizeof(s_wsUrl));
    s_timezoneOffsetMin = (int16_t)prefs.getShort("tz_off", -180);
    if (s_timezoneOffsetMin < -720 || s_timezoneOffsetMin > 840) {
        s_timezoneOffsetMin = -180;
    }
    const uint8_t source = prefs.getUChar("cfg_source", (uint8_t)ConfigSource::BACKEND);
    s_configSource = (source == (uint8_t)ConfigSource::LOCAL)
                         ? ConfigSource::LOCAL : ConfigSource::BACKEND;
    s_configVersion = prefs.getUInt("cfg_version", 0);
    s_localVersion = prefs.getUInt("local_ver", 0);

    // Bitmask de canales habilitados (8 bits). Valor inválido → todos ON.
    s_enabledChannelsMask = (uint16_t)prefs.getUInt("ch_en", SETTINGS_DEFAULT_CHANNELS_MASK);
    s_enabledChannelsMask &= 0x00FF;

    s_bedTargetC = prefs.getFloat("bed_target", DEFAULT_BED_TARGET_C);
    if (s_bedTargetC < 5.0f || s_bedTargetC >= BED_MAX_C) {
        s_bedTargetC = DEFAULT_BED_TARGET_C;
    }

    s_alarmLatches = (uint16_t)prefs.getUInt("alarm_latch", 0);

    prefs.end();

    Serial.printf("[CFG] Settings cargados: SSID='%s', zonas S%d/A%d, canales=0x%02X, cama=%.1fC\n",
                  s_wifiSsid[0] ? s_wifiSsid : "(vacío)",
                  s_substrateZones, s_sprinklerZones,
                  (unsigned int)s_enabledChannelsMask, s_bedTargetC);
}

// ----------------------------------------------------------------------------
// WiFi
// ----------------------------------------------------------------------------

bool settingsHasWifiCredentials() {
    return s_wifiSsid[0] != '\0';
}

const char* settingsGetWifiSsid() {
    return s_wifiSsid;
}

const char* settingsGetWifiPassword() {
    return s_wifiPass;
}

void settingsSetWifi(const char* ssid, const char* password) {
    strncpy(s_wifiSsid, ssid ? ssid : "", sizeof(s_wifiSsid) - 1);
    s_wifiSsid[sizeof(s_wifiSsid) - 1] = '\0';
    strncpy(s_wifiPass, password ? password : "", sizeof(s_wifiPass) - 1);
    s_wifiPass[sizeof(s_wifiPass) - 1] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("wifi_ssid", s_wifiSsid);
    prefs.putString("wifi_pass", s_wifiPass);
    prefs.end();

    Serial.println("[CFG] Credenciales WiFi guardadas en NVS");
}

void settingsEraseWifi() {
    s_wifiSsid[0] = '\0';
    s_wifiPass[0] = '\0';
    s_apPass[0] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove("wifi_ssid");
    prefs.remove("wifi_pass");
    prefs.remove("ap_pass");
    prefs.end();

    Serial.println("[CFG] WiFi y clave del AP borrados (resto de la config conservada)");
}

void settingsFactoryReset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();

    // Restaurar los valores de fábrica en RAM para que el estado en curso no
    // siga usando datos que ya no están en NVS (el llamador reinicia igual).
    s_wifiSsid[0] = '\0';
    s_wifiPass[0] = '\0';
    s_substrateZones = DEFAULT_SUBSTRATE_ZONES;
    s_sprinklerZones = DEFAULT_SPRINKLER_ZONES;
    s_deviceId[0] = '\0';
    s_apiToken[0] = '\0';
    s_apPass[0] = '\0';
    s_claimCode[0] = '\0';
    s_backendUrl[0] = '\0';
    s_wsUrl[0] = '\0';
    s_timezoneOffsetMin = -180;
    s_configSource = ConfigSource::BACKEND;
    s_configVersion = 0;
    s_localVersion = 0;
    s_enabledChannelsMask = SETTINGS_DEFAULT_CHANNELS_MASK;
    s_bedTargetC = DEFAULT_BED_TARGET_C;
    s_alarmLatches = 0;

    Serial.println("[CFG] Reset de fabrica: namespace 'cfg' borrado");
}

// ----------------------------------------------------------------------------
// Zonas
// ----------------------------------------------------------------------------

uint8_t settingsGetSubstrateZones() {
    return s_substrateZones;
}

uint8_t settingsGetSprinklerZones() {
    return s_sprinklerZones;
}

void settingsSetZones(uint8_t substrateZones, uint8_t sprinklerZones) {
    if (substrateZones < 1) substrateZones = 1;
    if (substrateZones > MAX_SUBSTRATE_ZONES) substrateZones = MAX_SUBSTRATE_ZONES;
    if (sprinklerZones > MAX_SPRINKLER_ZONES) sprinklerZones = MAX_SPRINKLER_ZONES;

    s_substrateZones = substrateZones;
    s_sprinklerZones = sprinklerZones;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("sub_zones", s_substrateZones);
    prefs.putUInt("spr_zones", s_sprinklerZones);
    prefs.end();

    Serial.printf("[CFG] Zonas guardadas: sustrato=%u, aspersión=%u\n",
                  s_substrateZones, s_sprinklerZones);
}

// ----------------------------------------------------------------------------
// Canales de relé habilitados
// ----------------------------------------------------------------------------

bool settingsIsChannelEnabled(RelayChannel channel) {
    const uint8_t bit = (uint8_t)channel;
    if (bit >= RELAY_CHANNEL_COUNT) {
        return false;
    }
    return (s_enabledChannelsMask & (uint16_t)(1u << bit)) != 0;
}

uint16_t settingsGetEnabledChannelsMask() {
    return s_enabledChannelsMask;
}

void settingsSetEnabledChannelsMask(uint16_t mask) {
    s_enabledChannelsMask = mask & 0x00FF;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("ch_en", s_enabledChannelsMask);
    prefs.end();

    Serial.printf("[CFG] Canales habilitados guardados: 0x%02X\n",
                  (unsigned int)s_enabledChannelsMask);
}

void settingsSetChannelEnabled(RelayChannel channel, bool enabled) {
    const uint8_t bit = (uint8_t)channel;
    if (bit >= RELAY_CHANNEL_COUNT) {
        return;
    }
    uint16_t mask = s_enabledChannelsMask;
    if (enabled) {
        mask |= (uint16_t)(1u << bit);
    } else {
        mask &= (uint16_t)~(1u << bit);
    }
    settingsSetEnabledChannelsMask(mask);
}

// ----------------------------------------------------------------------------
// Cama caliente
// ----------------------------------------------------------------------------

float settingsGetBedTargetC() {
    return s_bedTargetC;
}

void settingsSetBedTargetC(float targetC) {
    if (targetC < 5.0f || targetC >= BED_MAX_C) {
        Serial.printf("[CFG] Setpoint de cama inválido: %.1f C\n", targetC);
        return;
    }
    s_bedTargetC = targetC;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putFloat("bed_target", s_bedTargetC);
    prefs.end();

    Serial.printf("[CFG] Setpoint de cama guardado: %.1f C\n", s_bedTargetC);
}

// ----------------------------------------------------------------------------
// Alarmas — latches persistidos
// ----------------------------------------------------------------------------

uint16_t settingsGetAlarmLatches() {
    return s_alarmLatches;
}

void settingsSetAlarmLatches(uint16_t mask) {
    if (mask == s_alarmLatches) {
        return;  // evitar desgaste de NVS si no cambió
    }
    s_alarmLatches = mask;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("alarm_latch", s_alarmLatches);
    prefs.end();

    Serial.printf("[CFG] Latches de alarma guardados: 0x%04X\n",
                  (unsigned int)s_alarmLatches);
}

// ----------------------------------------------------------------------------
// Punto de acceso (clave configurable)
// ----------------------------------------------------------------------------

const char* settingsGetApPassword() {
    return s_apPass;
}

void settingsSetApPassword(const char* password) {
    const size_t len = (password != nullptr) ? strlen(password) : 0;

    // "" restaura la clave derivada de la MAC; si no, exigir 8..63.
    if (len > 0 && (len < AP_PASSWORD_MIN_LEN || len > SETTINGS_AP_PASS_MAX)) {
        Serial.printf("[CFG] Clave AP invalida (%u caracteres): se conserva\n",
                      (unsigned)len);
        return;
    }
    if (strcmp(s_apPass, password ? password : "") == 0) {
        return;  // sin cambios: no desgastar NVS
    }

    strncpy(s_apPass, password ? password : "", sizeof(s_apPass) - 1);
    s_apPass[sizeof(s_apPass) - 1] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("ap_pass", s_apPass);
    prefs.end();

    Serial.printf("[CFG] Clave AP guardada (%u caracteres)\n", (unsigned)len);
}

// ----------------------------------------------------------------------------
// Identidad del dispositivo
// ----------------------------------------------------------------------------

bool settingsHasDeviceIdentity() {
    return s_deviceId[0] != '\0' && s_apiToken[0] != '\0';
}

const char* settingsGetDeviceId() {
    return s_deviceId;
}

const char* settingsGetApiToken() {
    return s_apiToken;
}

void settingsSetDeviceIdentity(const char* deviceId, const char* apiToken) {
    strncpy(s_deviceId, deviceId ? deviceId : "", sizeof(s_deviceId) - 1);
    s_deviceId[sizeof(s_deviceId) - 1] = '\0';
    strncpy(s_apiToken, apiToken ? apiToken : "", sizeof(s_apiToken) - 1);
    s_apiToken[sizeof(s_apiToken) - 1] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("device_id", s_deviceId);
    prefs.putString("api_token", s_apiToken);
    prefs.end();

    Serial.println("[CFG] Identidad del dispositivo guardada en NVS");
}

// ----------------------------------------------------------------------------
// Provisioning (claim code)
// ----------------------------------------------------------------------------

const char* settingsGetClaimCode() {
    return s_claimCode;
}

void settingsSetClaimCode(const char* claimCode) {
    if (strcmp(s_claimCode, claimCode ? claimCode : "") == 0) {
        return;  // sin cambios: no desgastar NVS
    }
    strncpy(s_claimCode, claimCode ? claimCode : "", sizeof(s_claimCode) - 1);
    s_claimCode[sizeof(s_claimCode) - 1] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("claim_code", s_claimCode);
    prefs.end();

    Serial.printf("[CFG] claim_code guardado (%u caracteres)\n",
                  (unsigned)strlen(s_claimCode));
}

// ----------------------------------------------------------------------------
// Fuente de configuración
// ----------------------------------------------------------------------------

ConfigSource settingsGetConfigSource() {
    return s_configSource;
}

void settingsSetConfigSource(ConfigSource source) {
    if (source == s_configSource) {
        return;
    }
    s_configSource = source;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUChar("cfg_source", (uint8_t)s_configSource);
    prefs.end();

    Serial.printf("[CFG] Fuente de configuracion: %s\n",
                  (s_configSource == ConfigSource::LOCAL) ? "local" : "backend");
}

// ----------------------------------------------------------------------------
// URL del backend
// ----------------------------------------------------------------------------

const char* settingsGetBackendUrl() {
    return s_backendUrl;
}

void settingsSetBackendUrl(const char* url) {
    strncpy(s_backendUrl, url ? url : "", sizeof(s_backendUrl) - 1);
    s_backendUrl[sizeof(s_backendUrl) - 1] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("backend_url", s_backendUrl);
    prefs.end();

    Serial.printf("[CFG] backend_url guardada: '%s'\n", s_backendUrl);
}

// ----------------------------------------------------------------------------
// URL WebSocket (la devuelve el claim o se deriva del backend)
// ----------------------------------------------------------------------------

const char* settingsGetWsUrl() {
    return s_wsUrl;
}

void settingsSetWsUrl(const char* url) {
    const char* value = (url != nullptr) ? url : "";
    if (strcmp(s_wsUrl, value) == 0) {
        return;
    }
    strncpy(s_wsUrl, value, sizeof(s_wsUrl) - 1);
    s_wsUrl[sizeof(s_wsUrl) - 1] = '\0';

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("ws_url", s_wsUrl);
    prefs.end();

    Serial.printf("[CFG] ws_url guardada: '%s'\n", s_wsUrl);
}

// ----------------------------------------------------------------------------
// Zona horaria (minutos respecto de UTC)
// ----------------------------------------------------------------------------

int16_t settingsGetTimezoneOffsetMin() {
    return s_timezoneOffsetMin;
}

void settingsSetTimezoneOffsetMin(int16_t minutes) {
    if (minutes < -720 || minutes > 840) {
        Serial.printf("[CFG] Offset horario invalido: %d min\n", (int)minutes);
        return;
    }
    if (minutes == s_timezoneOffsetMin) {
        return;
    }
    s_timezoneOffsetMin = minutes;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putShort("tz_off", s_timezoneOffsetMin);
    prefs.end();

    Serial.printf("[CFG] Offset horario guardado: %d min\n", (int)s_timezoneOffsetMin);
}

// ----------------------------------------------------------------------------
// Versión de configuración (cacheada de la nube)
// ----------------------------------------------------------------------------

uint32_t settingsGetConfigVersion() {
    return s_configVersion;
}

void settingsSetConfigVersion(uint32_t version, bool force) {
    if (!force && version <= s_configVersion) {
        return;  // solo la versión más alta gana (§9)
    }
    if (version == s_configVersion) {
        return;
    }
    s_configVersion = version;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("cfg_version", s_configVersion);
    prefs.end();

    Serial.printf("[CFG] config_version actualizada a %lu%s\n",
                  (unsigned long)s_configVersion, force ? " (force)" : "");
}

uint32_t settingsGetLocalVersion() {
    return s_localVersion;
}

void settingsBumpLocalVersion() {
    s_localVersion++;

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("local_ver", s_localVersion);
    prefs.end();

    Serial.printf("[CFG] Perfil local version = %lu\n",
                  (unsigned long)s_localVersion);
}