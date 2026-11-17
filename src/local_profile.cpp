#include "local_profile.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "alarm.h"
#include "climate.h"
#include "scheduler.h"
#include "settings.h"

// ============================================================================
// local_profile.cpp — Perfil local persistido en NVS (M5)
// - Namespace "cfg", clave "local_prof" (blob binario versionado).
// - No guarda WiFi ni identidad: eso vive en settings.cpp.
// ============================================================================

static const char* NVS_NAMESPACE = "cfg";
static const char* NVS_KEY       = "local_prof";
static const char* BACKEND_KEY   = "backend_prof";

static bool         s_hasCache = false;
static LocalProfile s_cache = {};
static bool         s_hasBackendCache = false;
static LocalProfile s_backendCache = {};

static bool loadStoredProfile(const char* key, LocalProfile& out) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);

    bool ok = false;
    const size_t len = prefs.getBytesLength(key);
    if (len == sizeof(LocalProfile)) {
        LocalProfile p = {};
        if (prefs.getBytes(key, &p, sizeof(p)) == sizeof(p) &&
            p.magic == LOCAL_PROFILE_MAGIC && p.version == LOCAL_PROFILE_VERSION) {
            out = p;
            ok = true;
        }
    }
    prefs.end();
    return ok;
}

static bool storeStoredProfile(const char* key, const LocalProfile& profile) {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    const size_t written = prefs.putBytes(key, &profile, sizeof(profile));
    prefs.end();
    return written == sizeof(profile);
}

static LocalSchedule fromZoneSchedule(const ZoneSchedule& s) {
    LocalSchedule out = {};
    out.hour = s.hour;
    out.minute = s.minute;
    out.duration_s = s.duration_s;
    out.enabled = s.enabled;
    out.daysMask = s.daysMask;
    return out;
}

static ZoneSchedule toZoneSchedule(const LocalSchedule& s) {
    ZoneSchedule out = {};
    out.hour = s.hour;
    out.minute = s.minute;
    out.duration_s = s.duration_s;
    out.enabled = s.enabled;
    out.daysMask = s.daysMask;
    return out;
}

static LocalSchedule fromChannelSchedule(const ChannelSchedule& s) {
    LocalSchedule out = {};
    out.hour = s.hour;
    out.minute = s.minute;
    out.duration_s = s.duration_s;
    out.enabled = s.enabled;
    out.daysMask = s.daysMask;
    return out;
}

static ChannelSchedule toChannelSchedule(const LocalSchedule& s) {
    ChannelSchedule out = {};
    out.hour = s.hour;
    out.minute = s.minute;
    out.duration_s = s.duration_s;
    out.enabled = s.enabled;
    out.daysMask = s.daysMask;
    return out;
}

static void applyZone(ZoneType type, uint8_t zone, const LocalZoneProfile& p) {
    irrigationSetDuration(type, zone, p.duration_s);
    if (type == ZoneType::SUBSTRATE) {
        irrigationSetMinHumidity(zone, p.minHumidityPct);
        irrigationSetThresholdEnabled(zone, p.thresholdEnabled);
    }
    irrigationSetPaused(type, zone, p.paused);
    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        irrigationSetSchedule(type, zone, i, toZoneSchedule(p.schedules[i]));
    }
}

static void applyChannel(RelayChannel channel, const LocalSchedule* schedules) {
    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        schedulerSetSchedule(channel, i, toChannelSchedule(schedules[i]));
    }
}

void localProfileApply(const LocalProfile& p) {
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        applyZone(ZoneType::SUBSTRATE, z, p.substrate[z]);
    }
    for (uint8_t z = 0; z < MAX_SPRINKLER_ZONES; z++) {
        applyZone(ZoneType::SPRINKLER, z, p.sprinkler[z]);
    }

    applyChannel(RelayChannel::ILUMINACION, p.light);
    applyChannel(RelayChannel::VENTILACION, p.vent);

    // Umbrales de aire: solo aplicar valores plausibles y con max > min.
    const bool tempOk = (p.alarmMaxTemp > p.alarmMinTemp) &&
                        (p.alarmMaxTemp >= -20.0f) && (p.alarmMaxTemp <= 80.0f) &&
                        (p.alarmMinTemp >= -50.0f) && (p.alarmMinTemp <= 40.0f);
    if (tempOk) {
        alarmSetMaxTemp(p.alarmMaxTemp);
        alarmSetMinTemp(p.alarmMinTemp);
    } else {
        alarmSetMaxTemp((float)DEFAULT_ALARM_MAX_TEMP);
        alarmSetMinTemp((float)DEFAULT_ALARM_MIN_TEMP);
    }

    irrigationSetInterlock(p.interlock);
    irrigationSetMaxRunS(ZoneType::SUBSTRATE, p.maxRunSubstrateS);
    irrigationSetMaxRunS(ZoneType::SPRINKLER, p.maxRunSprinklerS);
    irrigationSetMinOffS(p.minOffS);
    irrigationSetStaggerS(p.staggerS);

    // Hora local: offset de la configuración (validado por settings).
    if (p.timezoneOffsetMin >= -720 && p.timezoneOffsetMin <= 840) {
        settingsSetTimezoneOffsetMin(p.timezoneOffsetMin);
    }

    // Cama caliente: setpoint persistido + automatización.
    if (p.bedTargetC >= 5.0f && p.bedTargetC < BED_MAX_C) {
        climateSetBedTargetC(p.bedTargetC);
    }
    climateSetBedAutoEnabled(p.bedAutoEnabled);

    // Ventilación: automatización, clima y umbrales.
    climateSetVentAutoEnabled(p.ventAutoEnabled);
    climateSetVentClimateEnabled(p.ventClimateEnabled);
    climateSetVentThresholds(p.ventOnTempC, p.ventOffTempC,
                             p.ventOnRhPct, p.ventOffRhPct);
}

LocalProfile localProfileGetRuntime() {
    LocalProfile p = {};
    p.magic = LOCAL_PROFILE_MAGIC;
    p.version = LOCAL_PROFILE_VERSION;

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        p.substrate[z].duration_s = irrigationGetDuration(ZoneType::SUBSTRATE, z);
        p.substrate[z].minHumidityPct = irrigationGetMinHumidity(z);
        p.substrate[z].paused = irrigationIsPaused(ZoneType::SUBSTRATE, z);
        p.substrate[z].thresholdEnabled = irrigationGetThresholdEnabled(z);
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
            p.substrate[z].schedules[i] =
                fromZoneSchedule(irrigationGetSchedule(ZoneType::SUBSTRATE, z, i));
        }
    }

    for (uint8_t z = 0; z < MAX_SPRINKLER_ZONES; z++) {
        p.sprinkler[z].duration_s = irrigationGetDuration(ZoneType::SPRINKLER, z);
        p.sprinkler[z].minHumidityPct = 0;  // no aplica
        p.sprinkler[z].paused = irrigationIsPaused(ZoneType::SPRINKLER, z);
        p.sprinkler[z].thresholdEnabled = true;  // aspersión usa umbral de aire
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
            p.sprinkler[z].schedules[i] =
                fromZoneSchedule(irrigationGetSchedule(ZoneType::SPRINKLER, z, i));
        }
    }

    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        p.light[i] =
            fromChannelSchedule(schedulerGetSchedule(RelayChannel::ILUMINACION, i));
        p.vent[i] =
            fromChannelSchedule(schedulerGetSchedule(RelayChannel::VENTILACION, i));
    }

    p.alarmMaxTemp = alarmGetMaxTemp();
    p.alarmMinTemp = alarmGetMinTemp();
    p.interlock = irrigationGetInterlock();
    p.maxRunSubstrateS = irrigationGetMaxRunS(ZoneType::SUBSTRATE);
    p.maxRunSprinklerS = irrigationGetMaxRunS(ZoneType::SPRINKLER);
    p.minOffS = irrigationGetMinOffS();
    p.staggerS = irrigationGetStaggerS();
    p.bedTargetC = settingsGetBedTargetC();
    p.timezoneOffsetMin = settingsGetTimezoneOffsetMin();
    p.bedAutoEnabled = climateGetBedAutoEnabled();
    p.ventAutoEnabled = climateGetVentAutoEnabled();
    p.ventClimateEnabled = climateGetVentClimateEnabled();
    p.ventOnTempC = climateGetVentOnTempC();
    p.ventOffTempC = climateGetVentOffTempC();
    p.ventOnRhPct = climateGetVentOnRhPct();
    p.ventOffRhPct = climateGetVentOffRhPct();
    return p;
}

bool localProfileExists() {
    LocalProfile p = {};
    return loadStoredProfile(NVS_KEY, p);
}

bool localProfileApplyStored() {
    LocalProfile p = {};
    if (!loadStoredProfile(NVS_KEY, p)) {
        return false;
    }
    localProfileApply(p);
    s_cache = p;
    s_hasCache = true;
    return true;
}

bool localProfileGetStored(LocalProfile& out) {
    return loadStoredProfile(NVS_KEY, out);
}

bool backendProfileExists() {
    LocalProfile p = {};
    return loadStoredProfile(BACKEND_KEY, p);
}

bool backendProfileGetStored(LocalProfile& out) {
    return loadStoredProfile(BACKEND_KEY, out);
}

bool backendProfileApply() {
    LocalProfile p = {};
    if (!loadStoredProfile(BACKEND_KEY, p)) {
        return false;
    }
    localProfileApply(p);
    s_backendCache = p;
    s_hasBackendCache = true;
    Serial.println("[PROFILE] Perfil del backend aplicado");
    return true;
}

bool localProfileStoreBackend(const LocalProfile& profile, bool applyNow) {
    LocalProfile p = profile;
    p.magic = LOCAL_PROFILE_MAGIC;
    p.version = LOCAL_PROFILE_VERSION;
    p.staggerS = (p.staggerS > 600) ? 600 : p.staggerS;

    if (!storeStoredProfile(BACKEND_KEY, p)) {
        Serial.printf("[PROFILE] ERROR guardando perfil del backend (%u bytes)\n",
                      (unsigned)sizeof(p));
        return false;
    }
    s_backendCache = p;
    s_hasBackendCache = true;

    if (applyNow) {
        localProfileApply(p);
        Serial.println("[PROFILE] Config del backend guardada y aplicada");
    } else {
        Serial.println("[PROFILE] Config del backend guardada (override local activo)");
    }
    return true;
}

void localProfileInit() {
    LocalProfile local = {};
    if (loadStoredProfile(NVS_KEY, local)) {
        s_cache = local;
        s_hasCache = true;
        Serial.println("[PROFILE] Perfil local cargado desde NVS");
    } else {
        Serial.println("[PROFILE] Sin perfil local válido: guardando defaults");
        s_hasCache = false;
        localProfileSave();  // deja un blob válido para el portal
    }

    // Fuente activa: el perfil del backend manda por defecto; el local es el
    // respaldo offline y el override explícito.
    if (settingsGetConfigSource() == ConfigSource::BACKEND) {
        if (backendProfileApply()) {
            Serial.println("[PROFILE] Fuente backend: perfil del backend activo");
            return;
        }
        Serial.println("[PROFILE] Fuente backend sin perfil previo: uso perfil local");
    }

    if (localProfileApplyStored()) {
        Serial.println("[PROFILE] Perfil local activo");
    }
}

void localProfileSave() {
    LocalProfile p = localProfileGetRuntime();

    // Evitar escrituras de NVS si nada cambió.
    if (s_hasCache && memcmp(&p, &s_cache, sizeof(p)) == 0) {
        return;
    }

    if (storeStoredProfile(NVS_KEY, p)) {
        s_cache = p;
        s_hasCache = true;
        settingsBumpLocalVersion();
        Serial.printf("[PROFILE] Perfil local guardado (%u bytes)\n",
                      (unsigned)sizeof(p));
    } else {
        Serial.printf("[PROFILE] ERROR guardando perfil local (%u bytes)\n",
                      (unsigned)sizeof(p));
    }
}

void backendProfileSave() {
    LocalProfile p = localProfileGetRuntime();

    // El backend re-aplica su config en cada ciclo: evitar escrituras si no
    // cambió nada (misma comparación que el perfil local).
    if (s_hasBackendCache && memcmp(&p, &s_backendCache, sizeof(p)) == 0) {
        return;
    }

    if (storeStoredProfile(BACKEND_KEY, p)) {
        s_backendCache = p;
        s_hasBackendCache = true;
        Serial.printf("[PROFILE] Perfil del backend guardado (%u bytes)\n",
                      (unsigned)sizeof(p));
    } else {
        Serial.printf("[PROFILE] ERROR guardando perfil del backend (%u bytes)\n",
                      (unsigned)sizeof(p));
    }
}

void localProfileFactoryReset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.remove(NVS_KEY);
    prefs.remove(BACKEND_KEY);
    prefs.end();

    s_hasCache = false;
    s_cache = {};
    s_hasBackendCache = false;
    s_backendCache = {};

    Serial.println("[PROFILE] Reset de fabrica: perfiles local y backend borrados");
}
