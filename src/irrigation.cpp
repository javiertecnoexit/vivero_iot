#include "irrigation.h"

#include <Arduino.h>

#include "config.h"
#include "alarm.h"
#include "events.h"
#include "relay.h"
#include "rtc.h"
#include "sensors.h"
#include "settings.h"

// ============================================================================
// irrigation.cpp — Motor de riego y precedencia por zona (Milestone 3)
// Fuente: especificacion_firmware.md secciones 6.1-6.5.
// - Precedencia estricta: manual > override > (paused → idle) > schedule/threshold > idle.
// - El que pierde se IGNORA para ese ciclo (no encola, no reintenta).
// - Riego manual (6.3): Selector recorre zonas, Confirmar inicia/detiene, timeout 5 s.
// - Horario (6.2): array de horarios por zona, evaluación con RTC.
// - Umbral (6.4): suelo < min → riega zona; aire > max → riego compensatorio aspersión.
// - Pausada (6.5): no evalúa horario/umbral; sí responde a manual/override.
// ============================================================================

// Estado interno por zona.
static ZoneIrrigationState s_substrateState[MAX_SUBSTRATE_ZONES];
static ZoneIrrigationState s_sprinklerState[MAX_SPRINKLER_ZONES];

// Horarios por zona (array de MAX_SCHEDULES_PER_ZONE).
static ZoneSchedule s_substrateSchedules[MAX_SUBSTRATE_ZONES][MAX_SCHEDULES_PER_ZONE];
static ZoneSchedule s_sprinklerSchedules[MAX_SPRINKLER_ZONES][MAX_SCHEDULES_PER_ZONE];

// Estado pausado por zona (default false, M4 lo trae del backend).
static bool s_substratePaused[MAX_SUBSTRATE_ZONES] = { false };
static bool s_sprinklerPaused[MAX_SPRINKLER_ZONES] = { false };

// Overrides pendientes (llegan del backend vía GET /overrides).
static bool s_substrateOverridePending[MAX_SUBSTRATE_ZONES] = { false };
static bool s_sprinklerOverridePending[MAX_SPRINKLER_ZONES] = { false };

// --- Interlocks y límites (docs/03_reglas_y_plan.md §4.2 y §10.1) ---
#define IRRIGATION_ZONE_SLOTS (MAX_SUBSTRATE_ZONES + MAX_SPRINKLER_ZONES)

// Un solo riego a la vez (configurable por el backend en una etapa posterior).
static bool s_interlockEnabled = IRRIGATION_INTERLOCK_DEFAULT;
static uint16_t s_maxRunSubstrateS = IRRIGATION_MAX_RUN_S_DEFAULT;
static uint16_t s_maxRunSprinklerS = SPRINKLER_MAX_RUN_S_DEFAULT;
static uint16_t s_minOffS = IRRIGATION_MIN_OFF_S_DEFAULT;
static uint16_t s_staggerS = (uint16_t)(IRRIGATION_STAGGER_MS / 1000UL);

// Duración pedida por un comando remoto (0 = usar la configurada por zona).
static uint32_t s_overrideDurationMs[IRRIGATION_ZONE_SLOTS] = { 0 };

// Último apagado por zona (para min_off y stagger).
static bool     s_hasStopped[IRRIGATION_ZONE_SLOTS] = { false };
static uint32_t s_lastStopMs[IRRIGATION_ZONE_SLOTS] = { 0 };
static int8_t   s_lastStoppedSlot = -1;
static uint32_t s_lastStoppedAnyMs = 0;

// Evita repetir el log de bloqueo en cada ciclo.
static bool s_blockedLogged[IRRIGATION_ZONE_SLOTS] = { false };

static bool s_substrateDurationSet[MAX_SUBSTRATE_ZONES] = { false };
static uint16_t s_substrateDuration[MAX_SUBSTRATE_ZONES];
static bool s_sprinklerDurationSet[MAX_SPRINKLER_ZONES] = { false };
static uint16_t s_sprinklerDuration[MAX_SPRINKLER_ZONES];

// Riego manual activo (controlado por manual_ui.cpp).
static bool s_substrateManualActive[MAX_SUBSTRATE_ZONES] = { false };
static bool s_sprinklerManualActive[MAX_SPRINKLER_ZONES] = { false };

// Umbra mínimo de humedad por zona de sustrato (default fábrica 20 %).
static uint8_t s_substrateMinHumidity[MAX_SUBSTRATE_ZONES] = {
    DEFAULT_MIN_HUMIDITY, DEFAULT_MIN_HUMIDITY
};
// Riego automático por umbral de humedad (se puede desactivar por config).
static bool s_substrateThresholdEnabled[MAX_SUBSTRATE_ZONES] = { true, true };

// Genera un client_event_id simple (contador + millis para unicidad local).
// M4 usará esto para idempotencia en reintentos.
static uint32_t s_eventCounter = 0;
static uint32_t generateClientEventId() {
    return (millis() << 8) ^ (++s_eventCounter);
}

// Verifica si un horario coincide con la hora actual (minuto exacto).
static bool scheduleMatches(const ZoneSchedule& sched, const struct tm& now) {
    if (!sched.enabled) return false;
    // tm_wday: 0 = domingo. El bitmask usa 0 = lunes ... 6 = domingo.
    const uint8_t mondayBit = (uint8_t)(((now.tm_wday + 6) % 7));
    if ((sched.daysMask & (uint8_t)(1u << mondayBit)) == 0) {
        return false;
    }
    return (sched.hour == (uint8_t)now.tm_hour) && (sched.minute == (uint8_t)now.tm_min);
}

// Obtiene punteros al estado/horarios/paused/override/manual según tipo de zona.
static ZoneIrrigationState* getState(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateState[zone];
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
        return &s_sprinklerState[zone];
    }
}

static ZoneSchedule* getSchedules(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return s_substrateSchedules[zone];
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
        return s_sprinklerSchedules[zone];
    }
}

static bool* getPaused(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substratePaused[zone];
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
        return &s_sprinklerPaused[zone];
    }
}

static bool* getOverridePending(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateOverridePending[zone];
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
        return &s_sprinklerOverridePending[zone];
    }
}

static bool* getManualActive(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return nullptr;
        return &s_substrateManualActive[zone];
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return nullptr;
        return &s_sprinklerManualActive[zone];
    }
}

static RelayRole roleForType(ZoneType type) {
    return (type == ZoneType::SUBSTRATE) ? RelayRole::SUBSTRATE : RelayRole::SPRINKLER;
}

static RelayChannel channelForZone(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        return (zone == 0) ? RelayChannel::RIEGO_Z1 : RelayChannel::RIEGO_Z2;
    }
    return RelayChannel::ASPERSION;
}

// Duración configurada (s) para una zona, o el default de fábrica si aún no
// fue seteada por /config.
static uint16_t durationFor(ZoneType type, uint8_t zone) {
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return (uint16_t)DEFAULT_IRRIGATION_S;
        return (s_substrateDurationSet[zone]) ? s_substrateDuration[zone]
                                              : (uint16_t)DEFAULT_IRRIGATION_S;
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return (uint16_t)DEFAULT_IRRIGATION_S;
        return (s_sprinklerDurationSet[zone]) ? s_sprinklerDuration[zone]
                                              : (uint16_t)DEFAULT_IRRIGATION_S;
    }
}

// ----------------------------------------------------------------------------
// Interlocks, max_run y min_off
// ----------------------------------------------------------------------------

static uint8_t zoneSlot(ZoneType type, uint8_t zone) {
    return (type == ZoneType::SUBSTRATE) ? zone : (uint8_t)(MAX_SUBSTRATE_ZONES + zone);
}

static uint32_t maxRunMsFor(ZoneType type) {
    const uint16_t seconds = (type == ZoneType::SPRINKLER) ? s_maxRunSprinklerS
                                                           : s_maxRunSubstrateS;
    return (uint32_t)seconds * 1000UL;
}

static uint32_t minOffMsFor(ZoneType type) {
    // Solo el riego por umbral (sustrato) respeta min_off.
    return (type == ZoneType::SUBSTRATE) ? (uint32_t)s_minOffS * 1000UL : 0UL;
}

static void noteZoneStopped(ZoneType type, uint8_t zone) {
    const uint8_t slot = zoneSlot(type, zone);
    if (slot >= IRRIGATION_ZONE_SLOTS) {
        return;
    }
    s_hasStopped[slot] = true;
    s_lastStopMs[slot] = millis();
    s_lastStoppedSlot = (int8_t)slot;
    s_lastStoppedAnyMs = millis();
}

static bool anyOtherZoneActive(ZoneType type, uint8_t zone) {
    for (uint8_t z = 0; z < settingsGetSubstrateZones() && z < MAX_SUBSTRATE_ZONES; z++) {
        if (type == ZoneType::SUBSTRATE && z == zone) {
            continue;
        }
        const ZoneIrrigationState* st = getState(ZoneType::SUBSTRATE, z);
        if (st && st->active) {
            return true;
        }
    }
    for (uint8_t z = 0; z < settingsGetSprinklerZones() && z < MAX_SPRINKLER_ZONES; z++) {
        if (type == ZoneType::SPRINKLER && z == zone) {
            continue;
        }
        const ZoneIrrigationState* st = getState(ZoneType::SPRINKLER, z);
        if (st && st->active) {
            return true;
        }
    }
    return false;
}

// Devuelve true si la zona puede arrancar. reason recibe el motivo del bloqueo.
static bool startAllowed(ZoneType type, uint8_t zone, IrrigationTrigger trigger,
                         const char** reason) {
    if (reason) *reason = nullptr;
    if (!s_interlockEnabled) {
        return true;
    }

    if (anyOtherZoneActive(type, zone)) {
        if (reason) *reason = "interlock";
        return false;
    }

    const uint8_t slot = zoneSlot(type, zone);
    const uint32_t now = millis();

    // min_off solo aplica al ciclo automático por umbral. El manual puede
    // arrancar sin esperar porque hay un operador presente.
    if (trigger == IrrigationTrigger::THRESHOLD && s_hasStopped[slot] &&
        (now - s_lastStopMs[slot]) < minOffMsFor(type)) {
        if (reason) *reason = "min_off";
        return false;
    }

    // Stagger entre zonas distintas tras un apagado.
    if (s_lastStoppedSlot >= 0 && s_lastStoppedSlot != (int8_t)slot &&
        (now - s_lastStoppedAnyMs) < ((uint32_t)s_staggerS * 1000UL)) {
        if (reason) *reason = "stagger";
        return false;
    }

    return true;
}

// Apaga una zona y cierra su evento. maxRunCut dispara la alarma de timeout.
static void stopZone(ZoneType type, uint8_t zone, const struct tm& now, bool maxRunCut) {
    ZoneIrrigationState* state = getState(type, zone);
    if (!state) {
        return;
    }

    const IrrigationTrigger finished = state->trigger;

    if (finished == IrrigationTrigger::MANUAL) {
        bool* manualActive = getManualActive(type, zone);
        if (manualActive) {
            *manualActive = false;
        }
    }

    state->active = false;
    state->trigger = IrrigationTrigger::NONE;
    state->suppressed = false;
    setRelay(roleForType(type), zone, false);
    noteZoneStopped(type, zone);

    if (finished != IrrigationTrigger::NONE) {
        eventsCompleteIrrigation(type, zone, now);
    }
    if (maxRunCut) {
        Serial.printf("[IRRIG] Zona %s%u CORTE por max_run (%lu s)\n",
                      (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                      (unsigned long)(maxRunMsFor(type) / 1000UL));
        alarmNotifyIrrigationTimeout();
    }
    Serial.printf("[IRRIG] Zona %s%u FIN trigger=%d%s\n",
                  (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                  (int)finished, maxRunCut ? " (max_run)" : "");
}

// Evalúa una zona individual aplicando la precedencia (6.1).
// Devuelve el trigger que ganó (o NONE) y si fue suprimido.
static IrrigationTrigger evaluateZone(ZoneType type, uint8_t zone, const struct tm& now,
                                      const SensorReadings& readings,
                                      bool& outSuppressed, uint32_t& outDurationMs) {
    ZoneIrrigationState* state = getState(type, zone);
    if (!state) return IrrigationTrigger::NONE;

    outSuppressed = false;
    outDurationMs = 0;

    // 1. MANUAL (prioridad más alta) - botón físico
    bool* manualActive = getManualActive(type, zone);
    if (manualActive && *manualActive) {
        outDurationMs = (uint32_t)durationFor(type, zone) * 1000UL;
        return IrrigationTrigger::MANUAL;
    }

    // 2. OVERRIDE remoto
    // Se considera override activo si hay uno pendiente O si el riego en curso
    // ya es por override (para que el riego complete su duración sin cortarse
    // en el ciclo siguiente al INICIO). El override se CONSUME al terminar.
    bool* overridePending = getOverridePending(type, zone);
    const bool overrideActive =
        (overridePending && *overridePending) ||
        (state->active && state->trigger == IrrigationTrigger::OVERRIDE);
    if (overrideActive) {
        const uint8_t slot = zoneSlot(type, zone);
        outDurationMs = (slot < IRRIGATION_ZONE_SLOTS && s_overrideDurationMs[slot] > 0)
                            ? s_overrideDurationMs[slot]
                            : (uint32_t)durationFor(type, zone) * 1000UL;
        return IrrigationTrigger::OVERRIDE;
    }

    // 3. PAUSADA → idle (no evalúa horario ni umbral)
    bool* paused = getPaused(type, zone);
    if (paused && *paused) {
        return IrrigationTrigger::NONE;  // idle
    }

    // 4. HORARIO (schedule) — solo con hora válida (RTC/NTP/servidor).
    ZoneSchedule* schedules = getSchedules(type, zone);
    if (schedules && rtcIsTimeValid()) {
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
            if (scheduleMatches(schedules[i], now)) {
                outDurationMs = (uint32_t)schedules[i].duration_s * 1000UL;
                return IrrigationTrigger::SCHEDULE;
            }
        }
    }

    // 5. UMBRAL (threshold) - solo si no hay horario que coincida
    // 5a. Sustrato: humedad < min → riega esa zona
    if (type == ZoneType::SUBSTRATE) {
        if (zone < settingsGetSubstrateZones()) {
            const SoilReading& soil = readings.soil[zone];
            if (s_substrateThresholdEnabled[zone] && soil.valid &&
                soil.humidityPct < (float)irrigationGetMinHumidity(zone)) {
                outDurationMs = (uint32_t)durationFor(type, zone) * 1000UL;
                return IrrigationTrigger::THRESHOLD;
            }
        }
    }
    // 5b. Aspersión: temp aire > max → riego compensatorio
    else if (type == ZoneType::SPRINKLER) {
        const AirReading& air = readings.air;
        if (air.valid && air.tempC > alarmGetMaxTemp()) {
            outDurationMs = (uint32_t)durationFor(type, zone) * 1000UL;
            return IrrigationTrigger::THRESHOLD;
        }
    }

    // 6. IDLE - nada disparó
    return IrrigationTrigger::NONE;
}

// Corte por duración pedida y por max_run (docs/03 §4.2 y §10.1).
//
// Es la única red que apaga un riego por tiempo, así que se comparte entre el
// ciclo local (30 s) y el corte inmediato del loop principal. Antes solo
// corría dentro de irrigationEvaluate(), con lo que:
//   - max_run_s (900 s) podía llegar a 930 s;
//   - una duración de 60 s podía llegar a 90 s;
//   - si el ciclo local no corría, la zona nunca se apagaba.
static bool enforceZoneLimits(ZoneType type, uint8_t zone) {
    ZoneIrrigationState* state = getState(type, zone);
    if (!state || !state->active || state->suppressed) {
        return false;
    }

    const uint32_t elapsed = millis() - state->startedAtMs;
    if (elapsed >= state->durationMs) {
        stopZone(type, zone, rtcNow(), false);
        return true;
    }
    if (elapsed >= maxRunMsFor(type)) {
        stopZone(type, zone, rtcNow(), true);
        return true;
    }
    return false;
}

// Actualiza el relé y genera evento si cambió el estado.
static void updateZoneRelayAndEvent(ZoneType type, uint8_t zone,
                                    IrrigationTrigger trigger, bool suppressed,
                                    uint32_t durationMs, const struct tm& now) {
    ZoneIrrigationState* state = getState(type, zone);
    if (!state) return;

    const bool shouldBeActive = (trigger != IrrigationTrigger::NONE);
    const RelayRole role = roleForType(type);
    const uint8_t slot = zoneSlot(type, zone);

    // Si el pedido desaparece, permitir que un bloqueo previo se registre de nuevo.
    if (!shouldBeActive && slot < IRRIGATION_ZONE_SLOTS) {
        s_blockedLogged[slot] = false;
    }

    if (shouldBeActive != state->active) {
        if (shouldBeActive) {
            // Interlock / min_off / stagger: si no puede arrancar, no se
            // enciende y se registra el motivo una sola vez.
            const char* reason = nullptr;
            if (!startAllowed(type, zone, trigger, &reason)) {
                if (slot < IRRIGATION_ZONE_SLOTS && !s_blockedLogged[slot]) {
                    s_blockedLogged[slot] = true;
                    Serial.printf("[IRRIG] Zona %s%u bloqueada: %s\n",
                                  (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                                  reason ? reason : "?");
                }
                return;
            }
            if (slot < IRRIGATION_ZONE_SLOTS) {
                s_blockedLogged[slot] = false;
            }

            // INICIO de riego
            state->active = true;
            state->trigger = trigger;
            state->startedAtMs = millis();
            state->durationMs = durationMs;
            state->suppressed = suppressed;
            setRelay(role, zone, true);

            // Consumir el override al materializarlo.
            if (trigger == IrrigationTrigger::OVERRIDE) {
                irrigationMarkOverrideApplied(type, zone);
            }

            IrrigationEvent evt = {};
            evt.clientEventId = generateClientEventId();
            evt.zoneType = type;
            evt.zone = zone;
            evt.trigger = trigger;
            evt.startedAt = now;
            evt.endedAt = {};  // se llena al terminar
            evt.suppressed = suppressed;
            eventsPushIrrigation(evt);

            Serial.printf("[IRRIG] Zona %s%u INICIO trigger=%d dur=%lums suppressed=%d\n",
                          (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                          (int)trigger, (unsigned long)durationMs, suppressed);
        } else {
            // FIN de riego porque el pedido desapareció.
            stopZone(type, zone, now, false);
        }
        return;
    }

    // Sigue activo: duración pedida y corte duro por max_run. Se delega en
    // enforceZoneLimits() para que el loop principal aplique el mismo corte
    // sin esperar el próximo ciclo local de 30 s.
    if (state->active) {
        enforceZoneLimits(type, zone);
    }
}

void irrigationInit() {
    // Inicializar estados a idle
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        s_substrateState[z] = { false, IrrigationTrigger::NONE, 0, 0, false };
        s_substratePaused[z] = false;
        s_substrateOverridePending[z] = false;
        s_substrateManualActive[z] = false;
        s_substrateThresholdEnabled[z] = true;
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
            s_substrateSchedules[z][i] = { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
        }
    }
    for (uint8_t z = 0; z < MAX_SPRINKLER_ZONES; z++) {
        s_sprinklerState[z] = { false, IrrigationTrigger::NONE, 0, 0, false };
        s_sprinklerPaused[z] = false;
        s_sprinklerOverridePending[z] = false;
        s_sprinklerManualActive[z] = false;
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
            s_sprinklerSchedules[z][i] = { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
        }
    }
    s_interlockEnabled = IRRIGATION_INTERLOCK_DEFAULT;
    s_maxRunSubstrateS = IRRIGATION_MAX_RUN_S_DEFAULT;
    s_maxRunSprinklerS = SPRINKLER_MAX_RUN_S_DEFAULT;
    s_minOffS = IRRIGATION_MIN_OFF_S_DEFAULT;
    for (uint8_t i = 0; i < IRRIGATION_ZONE_SLOTS; i++) {
        s_hasStopped[i] = false;
        s_lastStopMs[i] = 0UL;
        s_blockedLogged[i] = false;
    }
    s_lastStoppedSlot = -1;
    s_lastStoppedAnyMs = 0UL;
    Serial.println("[IRRIG] Motor de riego inicializado (precedencia: manual > override > paused > schedule/threshold)");
}

void irrigationEvaluate(const struct tm& nowUtc, const struct tm& nowLocal,
                        const SensorReadings& readings) {
    // Evaluar zonas de sustrato configuradas
    for (uint8_t z = 0; z < settingsGetSubstrateZones(); z++) {
        if (!settingsIsChannelEnabled(channelForZone(ZoneType::SUBSTRATE, z))) {
            // Canal deshabilitado: no evaluar ni dejar estado activo colgado.
            ZoneIrrigationState* st = getState(ZoneType::SUBSTRATE, z);
            if (st && st->active) {
                st->active = false;
                st->trigger = IrrigationTrigger::NONE;
                st->suppressed = false;
                noteZoneStopped(ZoneType::SUBSTRATE, z);
                eventsCompleteIrrigation(ZoneType::SUBSTRATE, z, nowUtc);
            }
            setRelay(RelayRole::SUBSTRATE, z, false);
            continue;
        }

        bool suppressed = false;
        uint32_t durationMs = 0;
        IrrigationTrigger trigger = evaluateZone(ZoneType::SUBSTRATE, z, nowLocal,
                                                 readings, suppressed, durationMs);
        updateZoneRelayAndEvent(ZoneType::SUBSTRATE, z, trigger, suppressed,
                                durationMs, nowUtc);
    }

    // Evaluar zonas de aspersión configuradas
    for (uint8_t z = 0; z < settingsGetSprinklerZones(); z++) {
        if (!settingsIsChannelEnabled(channelForZone(ZoneType::SPRINKLER, z))) {
            ZoneIrrigationState* st = getState(ZoneType::SPRINKLER, z);
            if (st && st->active) {
                st->active = false;
                st->trigger = IrrigationTrigger::NONE;
                st->suppressed = false;
                noteZoneStopped(ZoneType::SPRINKLER, z);
                eventsCompleteIrrigation(ZoneType::SPRINKLER, z, nowUtc);
            }
            setRelay(RelayRole::SPRINKLER, z, false);
            continue;
        }

        bool suppressed = false;
        uint32_t durationMs = 0;
        IrrigationTrigger trigger = evaluateZone(ZoneType::SPRINKLER, z, nowLocal,
                                                 readings, suppressed, durationMs);
        updateZoneRelayAndEvent(ZoneType::SPRINKLER, z, trigger, suppressed,
                                durationMs, nowUtc);
    }
}

// Corte por tiempo llamado desde el loop principal (junto a
// channelControlUpdate). No depende de la cadencia de 30 s del ciclo local:
// por zona solo hace 3 comparaciones y llama a rtcNow() únicamente si algo se
// va a apagar.
void irrigationEnforceLimits() {
    for (uint8_t z = 0; z < settingsGetSubstrateZones() && z < MAX_SUBSTRATE_ZONES; z++) {
        enforceZoneLimits(ZoneType::SUBSTRATE, z);
    }
    for (uint8_t z = 0; z < settingsGetSprinklerZones() && z < MAX_SPRINKLER_ZONES; z++) {
        enforceZoneLimits(ZoneType::SPRINKLER, z);
    }
}

// Detiene todos los riegos activos. Se usa al entrar en MANTENIMIENTO: ahí
// relayAllOff() apaga el relé pero dejaría state->active en true, y el próximo
// ciclo lo vería vencido y levantaría IRRIGATION_TIMEOUT por un corte que hizo
// el propio modo. maxRunCut=false porque el corte lo pide el modo, no el tiempo.
void irrigationStopAllActive() {
    const struct tm now = rtcNow();
    bool stopped = false;

    for (uint8_t z = 0; z < settingsGetSubstrateZones() && z < MAX_SUBSTRATE_ZONES; z++) {
        const ZoneIrrigationState* st = getState(ZoneType::SUBSTRATE, z);
        if (st && st->active) {
            stopZone(ZoneType::SUBSTRATE, z, now, false);
            stopped = true;
        }
    }
    for (uint8_t z = 0; z < settingsGetSprinklerZones() && z < MAX_SPRINKLER_ZONES; z++) {
        const ZoneIrrigationState* st = getState(ZoneType::SPRINKLER, z);
        if (st && st->active) {
            stopZone(ZoneType::SPRINKLER, z, now, false);
            stopped = true;
        }
    }

    if (stopped) {
        Serial.println("[IRRIG] Riegos activos detenidos (entrada a MANTENIMIENTO)");
    }
}

bool irrigationManualToggle(ZoneType type, uint8_t zone) {
    bool* manualActive = getManualActive(type, zone);
    ZoneIrrigationState* state = getState(type, zone);
    if (!manualActive || !state) return false;

    if (!settingsIsChannelEnabled(channelForZone(type, zone))) {
        Serial.printf("[IRRIG] Manual rechazado: canal %s deshabilitado\n",
                      relayChannelName(channelForZone(type, zone)));
        return false;
    }

    const bool turnOn = !(*manualActive);
    if (turnOn) {
        const char* reason = nullptr;
        if (!startAllowed(type, zone, IrrigationTrigger::MANUAL, &reason)) {
            Serial.printf("[IRRIG] Manual zona %s%u rechazado: %s\n",
                          (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                          reason ? reason : "?");
            return false;
        }
    }
    *manualActive = turnOn;

    if (*manualActive) {
        // ON: aplicar relé al instante (no esperar el tick de 30 s).
        // El manual tiene máxima precedencia, así que no hay conflicto.
        state->active = true;
        state->trigger = IrrigationTrigger::MANUAL;
        state->startedAtMs = millis();
        state->durationMs = (uint32_t)durationFor(type, zone) * 1000UL;
        state->suppressed = false;
        setRelay(roleForType(type), zone, true);

        IrrigationEvent evt = {};
        evt.clientEventId = generateClientEventId();
        evt.zoneType = type;
        evt.zone = zone;
        evt.trigger = IrrigationTrigger::MANUAL;
        evt.startedAt = rtcNow();
        evt.suppressed = false;
        eventsPushIrrigation(evt);

        Serial.printf("[IRRIG] Manual zona %s%u ON (inmediato, dur=%us)\n",
                      (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                      durationFor(type, zone));
    } else {
        // OFF: apagar relé al instante.
        state->active = false;
        state->trigger = IrrigationTrigger::NONE;
        state->suppressed = false;
        setRelay(roleForType(type), zone, false);
        noteZoneStopped(type, zone);
        // Completar ended_at del evento abierto de la zona (el manual ON
        // generó un evento con ended_at vacío).
        eventsCompleteIrrigation(type, zone, rtcNow());
        Serial.printf("[IRRIG] Manual zona %s%u OFF\n",
                      (type == ZoneType::SUBSTRATE) ? "S" : "A", zone);
    }
    return true;
}

bool irrigationCheckOverride(ZoneType type, uint8_t zone) {
    bool* overridePending = getOverridePending(type, zone);
    if (!overridePending) return false;
    return *overridePending;
}

void irrigationMarkOverrideApplied(ZoneType type, uint8_t zone) {
    bool* overridePending = getOverridePending(type, zone);
    if (overridePending) {
        *overridePending = false;
    }
}

bool irrigationTriggerOverride(ZoneType type, uint8_t zone) {
    bool* overridePending = getOverridePending(type, zone);
    if (!overridePending) return false;
    *overridePending = true;
    return true;
}

bool irrigationManualIsActive(ZoneType type, uint8_t zone) {
    const bool* manualActive = getManualActive(type, zone);
    return manualActive ? *manualActive : false;
}

bool irrigationRemoteStart(ZoneType type, uint8_t zone, uint16_t durationS,
                           const char** reason) {
    if (reason) *reason = nullptr;

    const RelayChannel channel = channelForZone(type, zone);
    if (!relayChannelIsValid(channel)) {
        if (reason) *reason = "invalid_payload";
        return false;
    }
    if (!settingsIsChannelEnabled(channel)) {
        if (reason) *reason = "channel_disabled";
        return false;
    }

    ZoneIrrigationState* state = getState(type, zone);
    bool* manualActive = getManualActive(type, zone);
    bool* overridePending = getOverridePending(type, zone);
    if (!state || !manualActive || !overridePending) {
        if (reason) *reason = "invalid_payload";
        return false;
    }
    if (*manualActive) {
        if (reason) *reason = "local_manual_active";
        return false;
    }
    if (durationS == 0) {
        if (reason) *reason = "invalid_payload";
        return false;
    }

    const char* blockReason = nullptr;
    if (!startAllowed(type, zone, IrrigationTrigger::OVERRIDE, &blockReason)) {
        if (reason) *reason = blockReason ? blockReason : "safety_interlock";
        return false;
    }

    const uint8_t slot = zoneSlot(type, zone);
    const uint32_t durationMs = (uint32_t)durationS * 1000UL;

    // Si ya está regando por override, extender/reiniciar la duración sin
    // cerrar el evento. Si estaba regando por horario/umbral, el override
    // remoto lo interrumpe (mayor precedencia).
    if (state->active && state->trigger != IrrigationTrigger::OVERRIDE) {
        stopZone(type, zone, rtcNow(), false);
    }

    *manualActive = false;
    *overridePending = false;
    if (slot < IRRIGATION_ZONE_SLOTS) {
        s_overrideDurationMs[slot] = durationMs;
    }

    const bool wasActive = state->active;
    state->active = true;
    state->trigger = IrrigationTrigger::OVERRIDE;
    state->startedAtMs = millis();
    state->durationMs = durationMs;
    state->suppressed = false;
    setRelay(roleForType(type), zone, true);

    if (!wasActive) {
        IrrigationEvent evt = {};
        evt.clientEventId = generateClientEventId();
        evt.zoneType = type;
        evt.zone = zone;
        evt.trigger = IrrigationTrigger::OVERRIDE;
        evt.startedAt = rtcNow();
        evt.endedAt = {};
        evt.suppressed = false;
        eventsPushIrrigation(evt);
    }

    Serial.printf("[IRRIG] Remoto zona %s%u ON dur=%us\n",
                  (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                  (unsigned)durationS);
    return true;
}

bool irrigationRemoteStop(ZoneType type, uint8_t zone) {
    const RelayChannel channel = channelForZone(type, zone);
    if (!relayChannelIsValid(channel)) {
        return false;
    }

    ZoneIrrigationState* state = getState(type, zone);
    bool* manualActive = getManualActive(type, zone);
    bool* overridePending = getOverridePending(type, zone);
    const uint8_t slot = zoneSlot(type, zone);

    if (manualActive) *manualActive = false;
    if (overridePending) *overridePending = false;
    if (slot < IRRIGATION_ZONE_SLOTS) {
        s_overrideDurationMs[slot] = 0;
    }

    const bool wasActive = (state && state->active);
    if (wasActive) {
        stopZone(type, zone, rtcNow(), false);
    } else {
        setRelay(roleForType(type), zone, false);
    }

    Serial.printf("[IRRIG] Remoto zona %s%u OFF%s\n",
                  (type == ZoneType::SUBSTRATE) ? "S" : "A", zone,
                  wasActive ? "" : " (ya estaba apagada)");
    return true;
}

void irrigationSetSchedule(ZoneType type, uint8_t zone, uint8_t index, const ZoneSchedule& sched) {
    ZoneSchedule* schedules = getSchedules(type, zone);
    if (schedules && index < MAX_SCHEDULES_PER_ZONE) {
        schedules[index] = sched;
    }
}

ZoneSchedule irrigationGetSchedule(ZoneType type, uint8_t zone, uint8_t index) {
    ZoneSchedule* schedules = getSchedules(type, zone);
    if (schedules && index < MAX_SCHEDULES_PER_ZONE) {
        return schedules[index];
    }
    return { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
}

void irrigationSetPaused(ZoneType type, uint8_t zone, bool paused) {
    bool* p = getPaused(type, zone);
    if (p) *p = paused;
}

bool irrigationIsPaused(ZoneType type, uint8_t zone) {
    bool* p = getPaused(type, zone);
    return p ? *p : false;
}

ZoneIrrigationState irrigationGetState(ZoneType type, uint8_t zone) {
    ZoneIrrigationState* state = getState(type, zone);
    if (state) return *state;
    return { false, IrrigationTrigger::NONE, 0, 0, false };
}

void irrigationSetDuration(ZoneType type, uint8_t zone, uint16_t duration_s) {
    if (duration_s < 1) {
        duration_s = 1;
    }
    // Tope absoluto no configurable (docs/03 §10.1).
    if (duration_s > IRRIGATION_MAX_RUN_ABS_S) {
        duration_s = (uint16_t)IRRIGATION_MAX_RUN_ABS_S;
    }
    if (type == ZoneType::SUBSTRATE) {
        if (zone >= MAX_SUBSTRATE_ZONES) return;
        s_substrateDurationSet[zone] = true;
        s_substrateDuration[zone] = duration_s;
    } else {
        if (zone >= MAX_SPRINKLER_ZONES) return;
        s_sprinklerDurationSet[zone] = true;
        s_sprinklerDuration[zone] = duration_s;
    }
}

uint16_t irrigationGetDuration(ZoneType type, uint8_t zone) {
    return durationFor(type, zone);
}

void irrigationSetMinHumidity(uint8_t zone, uint8_t minPct) {
    if (zone >= MAX_SUBSTRATE_ZONES) return;
    s_substrateMinHumidity[zone] = minPct;
}

uint8_t irrigationGetMinHumidity(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) return DEFAULT_MIN_HUMIDITY;
    return s_substrateMinHumidity[zone];
}

void irrigationSetThresholdEnabled(uint8_t zone, bool enabled) {
    if (zone >= MAX_SUBSTRATE_ZONES) return;
    s_substrateThresholdEnabled[zone] = enabled;
}

bool irrigationGetThresholdEnabled(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) return false;
    return s_substrateThresholdEnabled[zone];
}

// ----------------------------------------------------------------------------
// Interlocks y límites
// ----------------------------------------------------------------------------

void irrigationSetInterlock(bool enabled) {
    s_interlockEnabled = enabled;
    Serial.printf("[IRRIG] Interlock %s\n", enabled ? "ON" : "OFF");
}

bool irrigationGetInterlock() {
    return s_interlockEnabled;
}

void irrigationSetMaxRunS(ZoneType type, uint16_t seconds) {
    if (seconds < 1) seconds = 1;
    if (seconds > IRRIGATION_MAX_RUN_ABS_S) seconds = IRRIGATION_MAX_RUN_ABS_S;
    if (type == ZoneType::SPRINKLER) {
        s_maxRunSprinklerS = seconds;
    } else {
        s_maxRunSubstrateS = seconds;
    }
    Serial.printf("[IRRIG] max_run %s = %u s\n",
                  (type == ZoneType::SPRINKLER) ? "aspersion" : "sustrato",
                  (unsigned)seconds);
}

uint16_t irrigationGetMaxRunS(ZoneType type) {
    return (type == ZoneType::SPRINKLER) ? s_maxRunSprinklerS : s_maxRunSubstrateS;
}

void irrigationSetMinOffS(uint16_t seconds) {
    if (seconds > IRRIGATION_MIN_OFF_MAX_S) seconds = IRRIGATION_MIN_OFF_MAX_S;
    s_minOffS = seconds;
    Serial.printf("[IRRIG] min_off = %u s\n", (unsigned)seconds);
}

uint16_t irrigationGetMinOffS() {
    return s_minOffS;
}

void irrigationSetStaggerS(uint16_t seconds) {
    if (seconds > 600) seconds = 600;
    s_staggerS = seconds;
    Serial.printf("[IRRIG] stagger = %u s\n", (unsigned)seconds);
}

uint16_t irrigationGetStaggerS() {
    return s_staggerS;
}