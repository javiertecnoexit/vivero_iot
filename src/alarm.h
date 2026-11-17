#ifndef ALARM_H
#define ALARM_H

#include <stdint.h>
#include <time.h>

#include "config.h"

// ============================================================================
// alarm.h — Motor de alarmas (M4)
// Fuente: docs/03_reglas_y_plan.md §9 y docs/02_protocolo_ws.md §7.
// - Varias condiciones pueden estar activas a la vez (bitmask).
// - El relé de alarma suena si hay al menos una condición activa y no está
//   silenciado ni en MANTENIMIENTO.
// - Latch: algunas reglas quedan activas hasta un reset explícito; los latches
//   sobreviven un reinicio (NVS).
// - Silencio: apaga el relé 10 min sin borrar condiciones. Una condición
//   crítica nueva durante el silencio vuelve a energizar.
// - Cada condición genera su propio evento (ALARM_RAISED / ALARM_CLEARED).
// ============================================================================

enum class AlarmCondition : uint8_t {
    NONE                            = 0,
    AIR_TEMP_HIGH                   = 1,   // temp aire > max
    AIR_TEMP_LOW                    = 2,   // temp aire < min
    NO_CONNECTION_60MIN             = 3,   // sin backend > ALARM_NO_CONNECTION_SEC
    SOIL_HUMIDITY_PERSISTENT_LOW    = 4,   // sustrato < min durante 60 min
    BED_TEMP_HIGH                   = 5,   // cama >= BED_MAX_C
    WINDOW_FAULT                    = 6,   // ventana en FAULT
    SENSOR_FAULT_AIR                = 7,   // DHT21 inválido persistente
    SENSOR_FAULT_SOIL_Z1            = 8,
    SENSOR_FAULT_SOIL_Z2            = 9,
    SENSOR_FAULT_BED                = 10,
    IRRIGATION_TIMEOUT              = 11,  // riego cortado por max_run
    COUNT                           = 12
};

#define ALARM_CONDITION_COUNT ((uint8_t)AlarmCondition::COUNT)

// Bitmask de condiciones: el bit N corresponde a AlarmCondition N. El bit 0
// (NONE) no se usa. Con 11 condiciones alcanza uint16_t.
typedef uint16_t AlarmMask;

#define ALARM_BIT(cond) ((AlarmMask)(1u << (uint8_t)(cond)))
#define ALARM_MASK_NONE ((AlarmMask)0)

// Inicializa el módulo: buffer de eventos, contadores y latches desde NVS.
void alarmInit();

// Evaluación principal: se llama cada ciclo local (30 s).
// - Evalúa TODAS las condiciones con las lecturas actuales.
// - Actualiza latches y genera eventos por condición.
// - Actualiza el relé de alarma.
void alarmEvaluate(const struct tm& nowUtc, const struct SensorReadings& readings);

// Llamar en cada pasada del loop: vence el silencio y mantiene el relé.
void alarmUpdate();

// Consultas.
bool alarmIsActive();                       // hay al menos una condición activa
AlarmCondition alarmGetActiveCondition();   // condición de mayor prioridad
AlarmMask alarmGetActiveMask();
AlarmMask alarmGetLatchedMask();
bool alarmIsSilenced();
uint32_t alarmSilenceRemainingMs();

// Acciones.
// durationMs = 0 → ALARM_SILENCE_MS (10 min). No borra condiciones ni latches.
void alarmSilence(uint32_t durationMs = 0);
// Limpia los latches. Si la condición persiste, se reactiva en la próxima
// evaluación (y vuelve a latchear si la regla es latch).
void alarmResetLatches();

// La llama irrigation.cpp al cortar una zona por max_run.
void alarmNotifyIrrigationTimeout();

// Advertencia: el canal físico de alarma está deshabilitado (no suena).
bool alarmChannelIsDisabled();

// Metadatos de condición.
bool alarmConditionIsCritical(AlarmCondition condition);
const char* alarmConditionKey(AlarmCondition condition);    // clave para la API
const char* alarmConditionLabel(AlarmCondition condition);  // texto para OLED

// --- Eventos de alarma (FIFO) ---
struct AlarmEvent {
    // NO client_event_id para alarmas (especificacion_api.md §3.3, alarm_events)
    AlarmCondition condition;   // qué condición la disparó
    struct tm startedAt;        // cuándo se activó (UTC)
    struct tm resolvedAt;       // cuándo se resolvió (vacío mientras activa)
};

// Obtiene el número de eventos de alarma pendientes en el buffer.
uint8_t alarmEventsPendingCount();

// Extrae (pop) el evento de alarma más antiguo.
bool alarmPopEvent(struct AlarmEvent& outEvt);

// Agrega un evento de alarma al buffer FIFO (interno / reencolado en ws_client).
void alarmPushEvent(const AlarmEvent& evt);

// Umbrales de temperatura de aire (el backend los sobrescribe desde /config).
float alarmGetMaxTemp();
float alarmGetMinTemp();
void alarmSetMaxTemp(float maxTemp);
void alarmSetMinTemp(float minTemp);

#endif // ALARM_H
