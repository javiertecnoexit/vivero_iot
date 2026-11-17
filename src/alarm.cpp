#include "alarm.h"

#include <Arduino.h>

#include "config.h"
#include "irrigation.h"
#include "relay.h"
#include "rtc.h"
#include "sensors.h"
#include "settings.h"
#include "state_machine.h"
#include "window.h"
#include "ws_client.h"

// ============================================================================
// alarm.cpp — Motor de alarmas (M4)
// Fuente: docs/03_reglas_y_plan.md §9 y docs/02_protocolo_ws.md §7.
// - Condiciones simultáneas mediante bitmask (AlarmMask).
// - Relé ON si activeMask != 0, salvo silencio o MANTENIMIENTO.
// - Latch por regla con persistencia en NVS.
// - Un evento por condición al activarse y otro al resolverse.
// ============================================================================

// Buffer FIFO de eventos de alarma.
static const uint8_t MAX_ALARM_EVENTS = 32;
static AlarmEvent s_alarmBuffer[MAX_ALARM_EVENTS];
static uint8_t s_alarmHead = 0;
static uint8_t s_alarmTail = 0;
static uint8_t s_alarmCount = 0;

// Estado del motor.
static AlarmMask s_conditionMask = ALARM_MASK_NONE;  // condiciones verdaderas
static AlarmMask s_activeMask    = ALARM_MASK_NONE;  // + latches
static AlarmMask s_previousMask  = ALARM_MASK_NONE;  // para detectar flancos
static AlarmMask s_latchedMask   = ALARM_MASK_NONE;
static struct tm s_startedAt[ALARM_CONDITION_COUNT] = {};
static bool      s_relayOn = false;

// Silencio.
static uint32_t s_silenceUntilMs = 0UL;

// Umbrales runtime de aire: -1.0 = usar default de fábrica.
static float s_maxTemp = -1.0f;
static float s_minTemp = -1.0f;

// Contadores de ciclos con lectura inválida (3 ciclos = 90 s).
static uint16_t s_airInvalidCycles = 0;
static uint16_t s_soilInvalidCycles[MAX_SUBSTRATE_ZONES] = { 0 };
static uint16_t s_bedInvalidCycles = 0;

// Contadores de humedad persistentemente baja (60 min).
static uint16_t s_persistentLowCycles[MAX_SUBSTRATE_ZONES] = { 0 };

// Reglas críticas: una crítica NUEVA durante el silencio vuelve a energizar.
static const AlarmMask CRITICAL_CONDITIONS =
    ALARM_BIT(AlarmCondition::AIR_TEMP_HIGH) |
    ALARM_BIT(AlarmCondition::AIR_TEMP_LOW) |
    ALARM_BIT(AlarmCondition::BED_TEMP_HIGH) |
    ALARM_BIT(AlarmCondition::WINDOW_FAULT) |
    ALARM_BIT(AlarmCondition::IRRIGATION_TIMEOUT);

// Reglas con latch por defecto: fallas que requieren inspección y reset.
static const AlarmMask LATCHED_CONDITIONS =
    ALARM_BIT(AlarmCondition::BED_TEMP_HIGH) |
    ALARM_BIT(AlarmCondition::WINDOW_FAULT) |
    ALARM_BIT(AlarmCondition::IRRIGATION_TIMEOUT);

// Prioridad para el OLED (la primera activa gana).
static const AlarmCondition PRIORITY[] = {
    AlarmCondition::BED_TEMP_HIGH,
    AlarmCondition::AIR_TEMP_HIGH,
    AlarmCondition::AIR_TEMP_LOW,
    AlarmCondition::WINDOW_FAULT,
    AlarmCondition::IRRIGATION_TIMEOUT,
    AlarmCondition::SENSOR_FAULT_BED,
    AlarmCondition::SENSOR_FAULT_AIR,
    AlarmCondition::SENSOR_FAULT_SOIL_Z1,
    AlarmCondition::SENSOR_FAULT_SOIL_Z2,
    AlarmCondition::SOIL_HUMIDITY_PERSISTENT_LOW,
    AlarmCondition::NO_CONNECTION_60MIN
};
static const uint8_t PRIORITY_COUNT = sizeof(PRIORITY) / sizeof(PRIORITY[0]);

static float maxTempEffective() {
    return (s_maxTemp >= 0.0f) ? s_maxTemp : (float)DEFAULT_ALARM_MAX_TEMP;
}
static float minTempEffective() {
    return (s_minTemp >= 0.0f) ? s_minTemp : (float)DEFAULT_ALARM_MIN_TEMP;
}

// ----------------------------------------------------------------------------
// Eventos
// ----------------------------------------------------------------------------

static void raiseEvent(AlarmCondition condition, const struct tm& now) {
    const uint8_t idx = (uint8_t)condition;
    s_startedAt[idx] = now;

    AlarmEvent evt = {};
    evt.condition = condition;
    evt.startedAt = now;
    alarmPushEvent(evt);
}

static void resolveEvent(AlarmCondition condition, const struct tm& now) {
    const uint8_t idx = (uint8_t)condition;

    // Buscar el evento abierto en el buffer y completar resolvedAt.
    bool found = false;
    for (uint8_t i = 0; i < MAX_ALARM_EVENTS; i++) {
        AlarmEvent& evt = s_alarmBuffer[i];
        if (evt.condition == condition && evt.startedAt.tm_year > 0 &&
            evt.resolvedAt.tm_year == 0) {
            evt.resolvedAt = now;
            found = true;
            break;
        }
    }

    // Si ya se envió (pop), generar el evento de cierre para el historial.
    if (!found) {
        AlarmEvent evt = {};
        evt.condition = condition;
        evt.startedAt = s_startedAt[idx];
        evt.resolvedAt = now;
        alarmPushEvent(evt);
    }

    s_startedAt[idx] = {};
}

// ----------------------------------------------------------------------------
// Evaluación de condiciones
// ----------------------------------------------------------------------------

// Incrementa el contador de fallas hasta el umbral y lo resetea al validar.
static void updateInvalidCounter(uint16_t& counter, bool valid) {
    if (valid) {
        counter = 0;
    } else if (counter < ALARM_SENSOR_FAULT_CYCLES) {
        counter++;
    }
}

static AlarmMask evaluateSensorFaults(const SensorReadings& readings) {
    AlarmMask mask = ALARM_MASK_NONE;

    updateInvalidCounter(s_airInvalidCycles, readings.air.valid);
    if (s_airInvalidCycles >= ALARM_SENSOR_FAULT_CYCLES) {
        mask |= ALARM_BIT(AlarmCondition::SENSOR_FAULT_AIR);
    }

    updateInvalidCounter(s_bedInvalidCycles, readings.bedTemp.valid);
    if (s_bedInvalidCycles >= ALARM_SENSOR_FAULT_CYCLES) {
        mask |= ALARM_BIT(AlarmCondition::SENSOR_FAULT_BED);
    }

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        if (z >= settingsGetSubstrateZones()) {
            s_soilInvalidCycles[z] = 0;
            continue;
        }
        const bool valid = readings.soil[z].valid && readings.soilTemp[z].valid;
        updateInvalidCounter(s_soilInvalidCycles[z], valid);
        if (s_soilInvalidCycles[z] >= ALARM_SENSOR_FAULT_CYCLES) {
            mask |= (z == 0) ? ALARM_BIT(AlarmCondition::SENSOR_FAULT_SOIL_Z1)
                             : ALARM_BIT(AlarmCondition::SENSOR_FAULT_SOIL_Z2);
        }
    }

    return mask;
}

// Humedad persistentemente baja: alguna zona bajo el mínimo durante 60 min.
static AlarmMask evaluatePersistentLow(const SensorReadings& readings) {
    bool anyZone = false;
    for (uint8_t z = 0; z < settingsGetSubstrateZones() && z < MAX_SUBSTRATE_ZONES; z++) {
        const SoilReading& soil = readings.soil[z];
        if (soil.valid && soil.humidityPct < (float)irrigationGetMinHumidity(z)) {
            if (s_persistentLowCycles[z] < UINT16_MAX) {
                s_persistentLowCycles[z]++;
            }
            if (s_persistentLowCycles[z] >= SOIL_HUMIDITY_PERSISTENT_CYCLES) {
                anyZone = true;
            }
        } else {
            s_persistentLowCycles[z] = 0;  // subió o sensor inválido
        }
    }
    return anyZone ? ALARM_BIT(AlarmCondition::SOIL_HUMIDITY_PERSISTENT_LOW)
                   : ALARM_MASK_NONE;
}

static AlarmMask evaluateConditions(const struct tm& now, const SensorReadings& readings) {
    AlarmMask mask = ALARM_MASK_NONE;

    // Temperatura de aire.
    if (readings.air.valid) {
        if (readings.air.tempC > maxTempEffective()) {
            mask |= ALARM_BIT(AlarmCondition::AIR_TEMP_HIGH);
        } else if (readings.air.tempC < minTempEffective()) {
            mask |= ALARM_BIT(AlarmCondition::AIR_TEMP_LOW);
        }
    }

    // Cama caliente por encima del corte absoluto.
    if (readings.bedTemp.valid && readings.bedTemp.tempC >= BED_MAX_C) {
        mask |= ALARM_BIT(AlarmCondition::BED_TEMP_HIGH);
    }

    // Ventana en FAULT.
    if (windowGetState() == WindowState::FAULT) {
        mask |= ALARM_BIT(AlarmCondition::WINDOW_FAULT);
    }

    // Sensores inválidos persistentes.
    mask |= evaluateSensorFaults(readings);

    // Humedad de sustrato persistentemente baja.
    mask |= evaluatePersistentLow(readings);

    // Sin conexión con el backend (no dispara si nunca hubo contacto).
    const int64_t nowEpoch = rtcTmToEpoch(now);
    const int64_t lastSeen = wsGetLastBackendSeenEpoch();
    if (nowEpoch > 0 && lastSeen > 0 &&
        (nowEpoch - lastSeen) >= (int64_t)ALARM_NO_CONNECTION_SEC) {
        mask |= ALARM_BIT(AlarmCondition::NO_CONNECTION_60MIN);
    }

    return mask;
}

// ----------------------------------------------------------------------------
// Relé y silencio
// ----------------------------------------------------------------------------

static void alarmDriveRelay() {
    bool on = (s_activeMask != ALARM_MASK_NONE);

    if (stateMachineGetMode() == OperationMode::MANTENIMIENTO) {
        on = false;
    }
    if (!settingsIsChannelEnabled(RelayChannel::ALARMA)) {
        on = false;
    }
    if (alarmIsSilenced()) {
        on = false;
    }

    // Evitar escrituras repetidas al pin.
    if (on == s_relayOn && relayGetChannel(RelayChannel::ALARMA) == on) {
        return;
    }

    relaySetChannel(RelayChannel::ALARMA, on);
    s_relayOn = on;
    Serial.printf("[ALARM] Relé %s\n", on ? "ON" : "OFF");
}

// ----------------------------------------------------------------------------
// API pública
// ----------------------------------------------------------------------------

void alarmInit() {
    s_alarmHead = 0;
    s_alarmTail = 0;
    s_alarmCount = 0;

    s_conditionMask = ALARM_MASK_NONE;
    s_activeMask = ALARM_MASK_NONE;
    s_previousMask = ALARM_MASK_NONE;
    s_latchedMask = (AlarmMask)settingsGetAlarmLatches() & LATCHED_CONDITIONS;
    s_silenceUntilMs = 0UL;
    s_relayOn = false;

    for (uint8_t i = 0; i < ALARM_CONDITION_COUNT; i++) {
        s_startedAt[i] = {};
    }
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        s_persistentLowCycles[z] = 0;
        s_soilInvalidCycles[z] = 0;
    }
    s_airInvalidCycles = 0;
    s_bedInvalidCycles = 0;

    // Un latch persistido vuelve a sonar al arrancar, aunque la condición ya
    // haya desaparecido: requiere inspección y reset.
    s_activeMask = s_latchedMask;
    s_previousMask = ALARM_MASK_NONE;  // el primer ciclo genera el evento
    if (s_latchedMask != (AlarmMask)settingsGetAlarmLatches()) {
        settingsSetAlarmLatches(s_latchedMask);  // limpiar bits inválidos
    }

    Serial.printf("[ALARM] Motor inicializado: latches=0x%04X, canal=%s\n",
                  (unsigned)s_latchedMask,
                  settingsIsChannelEnabled(RelayChannel::ALARMA) ? "OK" : "DESHABILITADO");
    alarmDriveRelay();
}

void alarmEvaluate(const struct tm& now, const SensorReadings& readings) {
    const AlarmMask conditions = evaluateConditions(now, readings);

    // Latches: una regla latch que se cumple queda retenida hasta reset.
    const AlarmMask newlyLatched = conditions & LATCHED_CONDITIONS;
    if ((newlyLatched & ~s_latchedMask) != 0) {
        s_latchedMask |= newlyLatched;
        settingsSetAlarmLatches(s_latchedMask);
        Serial.printf("[ALARM] Latch actualizado: 0x%04X\n", (unsigned)s_latchedMask);
    }

    const AlarmMask active = conditions | s_latchedMask;

    // Flancos por condición → un evento por condición.
    const AlarmMask raised  = active & ~s_previousMask;
    const AlarmMask cleared = s_previousMask & ~active;

    for (uint8_t i = 1; i < ALARM_CONDITION_COUNT; i++) {
        const AlarmCondition condition = (AlarmCondition)i;
        const AlarmMask bit = ALARM_BIT(condition);
        if ((raised & bit) != 0) {
            raiseEvent(condition, now);
            Serial.printf("[ALARM] ACTIVADA %s%s\n", alarmConditionKey(condition),
                          alarmConditionIsCritical(condition) ? " (crítica)" : "");
        } else if ((cleared & bit) != 0) {
            resolveEvent(condition, now);
            Serial.printf("[ALARM] RESUELTA %s\n", alarmConditionKey(condition));
        }
    }

    // Una crítica NUEVA durante el silencio vuelve a energizar.
    if (alarmIsSilenced() && (raised & CRITICAL_CONDITIONS) != 0) {
        s_silenceUntilMs = 0UL;
        Serial.println("[ALARM] Alarma crítica nueva: silencio cancelado");
    }

    s_conditionMask = conditions;
    s_activeMask = active;
    s_previousMask = active;

    alarmDriveRelay();
}

void alarmUpdate() {
    if (s_silenceUntilMs != 0 && (int32_t)(millis() - s_silenceUntilMs) >= 0) {
        s_silenceUntilMs = 0UL;
        Serial.println("[ALARM] Silencio finalizado");
    }
    alarmDriveRelay();
}

bool alarmIsActive() {
    return s_activeMask != ALARM_MASK_NONE;
}

AlarmCondition alarmGetActiveCondition() {
    for (uint8_t i = 0; i < PRIORITY_COUNT; i++) {
        if ((s_activeMask & ALARM_BIT(PRIORITY[i])) != 0) {
            return PRIORITY[i];
        }
    }
    return AlarmCondition::NONE;
}

AlarmMask alarmGetActiveMask() {
    return s_activeMask;
}

AlarmMask alarmGetLatchedMask() {
    return s_latchedMask;
}

bool alarmIsSilenced() {
    if (s_silenceUntilMs == 0) {
        return false;
    }
    return (int32_t)(millis() - s_silenceUntilMs) < 0;
}

uint32_t alarmSilenceRemainingMs() {
    if (!alarmIsSilenced()) {
        return 0UL;
    }
    return s_silenceUntilMs - millis();
}

void alarmSilence(uint32_t durationMs) {
    const uint32_t duration = (durationMs == 0) ? ALARM_SILENCE_MS : durationMs;
    s_silenceUntilMs = millis() + duration;
    if (s_silenceUntilMs == 0) {
        s_silenceUntilMs = 1;  // evitar el valor centinela por wrap
    }
    Serial.printf("[ALARM] Silenciada %lu s\n", (unsigned long)(duration / 1000UL));
    alarmDriveRelay();
}

void alarmResetLatches() {
    s_latchedMask = ALARM_MASK_NONE;
    settingsSetAlarmLatches(0);

    // Quitar los latches del estado audible, pero conservar las condiciones
    // medidas: si siguen presentes, el próximo ciclo las reactiva (y vuelve a
    // latchear si corresponden). s_previousMask no se toca para que el evento
    // de cierre se genere normalmente.
    s_activeMask = s_conditionMask;
    Serial.println("[ALARM] Latches limpiados");
    alarmDriveRelay();
}

void alarmNotifyIrrigationTimeout() {
    const AlarmMask bit = ALARM_BIT(AlarmCondition::IRRIGATION_TIMEOUT);
    if ((s_latchedMask & bit) != 0) {
        return;
    }
    s_latchedMask |= bit;
    settingsSetAlarmLatches(s_latchedMask);
    s_activeMask |= bit;
    Serial.println("[ALARM] Timeout de riego notificado");
    alarmDriveRelay();
}

bool alarmChannelIsDisabled() {
    return !settingsIsChannelEnabled(RelayChannel::ALARMA);
}

bool alarmConditionIsCritical(AlarmCondition condition) {
    return (CRITICAL_CONDITIONS & ALARM_BIT(condition)) != 0;
}

const char* alarmConditionKey(AlarmCondition condition) {
    switch (condition) {
        case AlarmCondition::AIR_TEMP_HIGH:                return "temp_max";
        case AlarmCondition::AIR_TEMP_LOW:                 return "temp_min";
        case AlarmCondition::NO_CONNECTION_60MIN:          return "sin_conexion";
        case AlarmCondition::SOIL_HUMIDITY_PERSISTENT_LOW: return "humedad_persistente_baja";
        case AlarmCondition::BED_TEMP_HIGH:                return "bed_temp_high";
        case AlarmCondition::WINDOW_FAULT:                 return "window_fault";
        case AlarmCondition::SENSOR_FAULT_AIR:             return "sensor_fault_aire";
        case AlarmCondition::SENSOR_FAULT_SOIL_Z1:         return "sensor_fault_humedad_z1";
        case AlarmCondition::SENSOR_FAULT_SOIL_Z2:         return "sensor_fault_humedad_z2";
        case AlarmCondition::SENSOR_FAULT_BED:             return "sensor_fault_cama";
        case AlarmCondition::IRRIGATION_TIMEOUT:           return "irrigation_timeout";
        default:                                           return "desconocida";
    }
}

const char* alarmConditionLabel(AlarmCondition condition) {
    switch (condition) {
        case AlarmCondition::AIR_TEMP_HIGH:                return "ALARMA TEMP MAX";
        case AlarmCondition::AIR_TEMP_LOW:                 return "ALARMA TEMP MIN";
        case AlarmCondition::NO_CONNECTION_60MIN:          return "ALARMA SIN CONEX";
        case AlarmCondition::SOIL_HUMIDITY_PERSISTENT_LOW: return "ALARMA HUM. BAJA";
        case AlarmCondition::BED_TEMP_HIGH:                return "ALARMA CAMA MAX";
        case AlarmCondition::WINDOW_FAULT:                 return "ALARMA VENTANA";
        case AlarmCondition::SENSOR_FAULT_AIR:             return "FALLA SENSOR AIRE";
        case AlarmCondition::SENSOR_FAULT_SOIL_Z1:         return "FALLA HUM Z1";
        case AlarmCondition::SENSOR_FAULT_SOIL_Z2:         return "FALLA HUM Z2";
        case AlarmCondition::SENSOR_FAULT_BED:             return "FALLA SENSOR CAMA";
        case AlarmCondition::IRRIGATION_TIMEOUT:           return "ALARMA RIEGO";
        default:                                           return "ALARMA?";
    }
}

// ----------------------------------------------------------------------------
// Buffer de eventos
// ----------------------------------------------------------------------------

void alarmPushEvent(const AlarmEvent& evt) {
    if (s_alarmCount >= MAX_ALARM_EVENTS) {
        // Buffer lleno: descartar el más antiguo (FIFO).
        s_alarmHead = (s_alarmHead + 1) % MAX_ALARM_EVENTS;
        s_alarmCount--;
        Serial.println("[ALARM] Buffer lleno: evento más antiguo descartado (FIFO)");
    }

    s_alarmBuffer[s_alarmTail] = evt;
    s_alarmTail = (s_alarmTail + 1) % MAX_ALARM_EVENTS;
    s_alarmCount++;

    Serial.printf("[ALARM] Push event: condition=%s (pendientes=%u)\n",
                  alarmConditionKey(evt.condition), s_alarmCount);
}

uint8_t alarmEventsPendingCount() {
    return s_alarmCount;
}

bool alarmPopEvent(struct AlarmEvent& outEvt) {
    if (s_alarmCount == 0) {
        return false;
    }

    outEvt = s_alarmBuffer[s_alarmHead];
    // Limpiar el slot para que una resolución posterior no encuentre fantasmas.
    s_alarmBuffer[s_alarmHead] = AlarmEvent{};
    s_alarmHead = (s_alarmHead + 1) % MAX_ALARM_EVENTS;
    s_alarmCount--;
    return true;
}

// ----------------------------------------------------------------------------
// Umbrales de aire
// ----------------------------------------------------------------------------

float alarmGetMaxTemp() {
    return maxTempEffective();
}

float alarmGetMinTemp() {
    return minTempEffective();
}

void alarmSetMaxTemp(float maxTemp) {
    s_maxTemp = maxTemp;
    Serial.printf("[ALARM] Max temp runtime = %.1f °C\n", maxTemp);
}

void alarmSetMinTemp(float minTemp) {
    s_minTemp = minTemp;
    Serial.printf("[ALARM] Min temp runtime = %.1f °C\n", minTemp);
}
