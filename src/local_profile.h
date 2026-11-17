#ifndef LOCAL_PROFILE_H
#define LOCAL_PROFILE_H

#include <stdint.h>

#include "config.h"
#include "irrigation.h"

// ============================================================================
// local_profile.h — Perfiles persistidos en NVS (M5/M6)
// Fuente: docs/03_reglas_y_plan.md §11 y docs/02_protocolo_ws.md §5.
// - "local": horarios, duraciones, umbrales y límites que edita el portal.
// - "backend": última configuración recibida del servidor (aunque no se aplique
//   por estar activo el override local).
// - Fuente activa (settings): backend por defecto; local es override explícito.
// ============================================================================

#define LOCAL_PROFILE_MAGIC    0x56495632UL  // 'VIV2'
#define LOCAL_PROFILE_VERSION  2

struct LocalSchedule {
    uint8_t  hour;
    uint8_t  minute;
    uint16_t duration_s;
    bool     enabled;
    uint8_t  daysMask;   // bit 0 = lunes ... bit 6 = domingo
};

struct LocalZoneProfile {
    uint16_t duration_s;
    uint8_t  minHumidityPct;
    bool     paused;
    bool     thresholdEnabled;   // riego automático por humedad (sustrato)
    LocalSchedule schedules[MAX_SCHEDULES_PER_ZONE];
};

struct LocalProfile {
    uint32_t magic;
    uint16_t version;
    LocalZoneProfile substrate[MAX_SUBSTRATE_ZONES];
    LocalZoneProfile sprinkler[MAX_SPRINKLER_ZONES];
    LocalSchedule light[MAX_SCHEDULES_PER_ZONE];
    LocalSchedule vent[MAX_SCHEDULES_PER_ZONE];
    float alarmMaxTemp;
    float alarmMinTemp;
    bool interlock;
    uint16_t maxRunSubstrateS;
    uint16_t maxRunSprinklerS;
    uint16_t minOffS;
    uint16_t staggerS;
    float bedTargetC;
    int16_t timezoneOffsetMin;
    bool bedAutoEnabled;
    bool ventAutoEnabled;
    bool ventClimateEnabled;
    float ventOnTempC;
    float ventOffTempC;
    float ventOnRhPct;
    float ventOffRhPct;
};

// Carga el perfil de NVS y lo aplica a riego, horarios y alarmas.
// Debe llamarse en setup() después de irrigationInit(), schedulerInit() y
// alarmInit(). Si no hay perfil válido, guarda el estado de fábrica.
void localProfileInit();

// Toma una foto del estado runtime y la persiste como perfil local (solo si
// cambió). Lo llama el portal al guardar y el arranque para defaults.
void localProfileSave();

// Carga el perfil local guardado y lo aplica (sin modificarlo).
// Devuelve false si no hay un blob local válido.
bool localProfileApplyStored();

// Lee el perfil local guardado sin aplicarlo (para el editor del portal).
bool localProfileGetStored(LocalProfile& out);

// --- Perfil del backend (separado del local) ---
// Guarda una foto del runtime como perfil del backend (solo si cambió).
void backendProfileSave();
bool backendProfileExists();
bool backendProfileGetStored(LocalProfile& out);
// Carga y aplica el último perfil del backend. false si no hay ninguno.
bool backendProfileApply();
// Guarda un perfil validado como config del backend y, si applyNow, lo aplica
// (el llamador decide según la fuente de configuración activa).
bool localProfileStoreBackend(const LocalProfile& profile, bool applyNow);

// Aplica un perfil en RAM (sin guardar).
void localProfileApply(const LocalProfile& profile);

// Snapshot del estado runtime actual (sin guardar).
LocalProfile localProfileGetRuntime();

// True si NVS tiene un perfil válido para esta versión.
bool localProfileExists();

// Borra ambos perfiles (local y backend) de NVS y sus cachés en RAM.
// El llamador debe reiniciar para volver a los valores de fábrica.
void localProfileFactoryReset();

#endif // LOCAL_PROFILE_H
