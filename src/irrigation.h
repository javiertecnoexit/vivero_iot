#ifndef IRRIGATION_H
#define IRRIGATION_H

#include <stdint.h>
#include <time.h>

#include "config.h"
#include "sensors.h"  // SensorReadings (las lecturas las hace el ciclo local, no este módulo)

// ============================================================================
// irrigation.h — Motor de riego y precedencia por zona (Milestone 3)
// Fuente: especificacion_firmware.md secciones 6.1-6.5.
// - Precedencia estricta: manual > override > (paused → idle) > schedule/threshold > idle.
// - El que pierde se IGNORA para ese ciclo (no encola, no reintenta).
// - Riego manual (6.3): Selector recorre zonas, Confirmar inicia/detiene, timeout 5 s.
// - Horario (6.2): array de horarios por zona, evaluación con RTC.
// - Umbral (6.4): suelo < min → riega zona; aire > max → riego compensatorio aspersión.
// - Pausada (6.5): no evalúa horario/umbral; sí responde a manual/override.
// ============================================================================

enum class IrrigationTrigger : uint8_t {
    NONE      = 0,
    MANUAL    = 1,
    OVERRIDE  = 2,
    SCHEDULE  = 3,
    THRESHOLD = 4
};

enum class ZoneType : uint8_t {
    SUBSTRATE = 0,
    SPRINKLER = 1
};

// Un horario individual para una zona.
struct ZoneSchedule {
    uint8_t hour;           // 0-23
    uint8_t minute;         // 0-59
    uint16_t duration_s;    // duración en segundos
    bool enabled;           // si este horario está activo
    uint8_t daysMask;       // bit 0 = lunes ... bit 6 = domingo
};

// Estado de riego de una zona (para tracking entre ciclos).
struct ZoneIrrigationState {
    bool active;                    // relé encendido ahora
    IrrigationTrigger trigger;      // qué lo disparó
    uint32_t startedAtMs;           // millis() de inicio
    uint32_t durationMs;            // duración total en ms
    bool suppressed;                // true si perdió por precedencia
};

// Inicializa el motor de riego: estados a idle, horarios vacíos, paused=false.
void irrigationInit();

// Evaluación principal: se llama cada ciclo local (30 s).
// - nowUtc: marca de tiempo de eventos (UTC).
// - nowLocal: hora local para evaluar horarios (offset de config.h).
// - readings: lecturas ya tomadas (evita releer sensores).
// - Evalúa precedencia por zona.
// - Actualiza relés vía setRelay().
// - Genera eventos de riego (con client_event_id) para el buffer de events.cpp.
void irrigationEvaluate(const struct tm& nowUtc, const struct tm& nowLocal,
                        const SensorReadings& readings);

// Corte por duración pedida y por max_run, sin esperar el ciclo de 30 s.
// Llamar en cada pasada del loop principal (junto a channelControlUpdate):
// garantiza que max_run_s se respete con precisión de milisegundos y que una
// zona no quede encendida si el ciclo local dejara de correr.
void irrigationEnforceLimits();

// Detiene todos los riegos activos sin disparar la alarma de max_run.
// Uso: entrada a MANTENIMIENTO (relayAllOff() apaga el relé pero dejaría el
// estado activo y el próximo ciclo levantaría IRRIGATION_TIMEOUT).
void irrigationStopAllActive();

// Riego manual (6.3): llamado desde manual_ui.cpp cuando Confirmar alterna.
// Devuelve true si cambió el estado (inició o detuvo).
bool irrigationManualToggle(ZoneType type, uint8_t zone);

// Consulta si hay override remoto pendiente para una zona.
// Los overrides llegan del backend (comandos/config de ws_client.cpp).
bool irrigationCheckOverride(ZoneType type, uint8_t zone);

// Marca un override como aplicado (lo consume el motor de riego).
void irrigationMarkOverrideApplied(ZoneType type, uint8_t zone);

// Activa el riego por override remoto ("iniciar ahora") para una zona.
// La evaluación del próximo ciclo local lo materializa. Devuelve true si la
// zona existe. Si un riego manual está activo, el override pierde por
// precedencia (spec 6.1) y el caller decide el ack.
bool irrigationTriggerOverride(ZoneType type, uint8_t zone);

// --- Comandos remotos WebSocket (M6) ---
// Arranca de inmediato un riego remoto con duración explícita. Devuelve false
// y completa reason si el canal está deshabilitado, hay manual local activo o
// un interlock lo impide. Si ya regaba por horario/umbral, lo interrumpe.
bool irrigationRemoteStart(ZoneType type, uint8_t zone, uint16_t durationS,
                           const char** reason);
// Detiene el riego de la zona (acción segura: también cancela manual local).
bool irrigationRemoteStop(ZoneType type, uint8_t zone);
// True si el operador local tiene el riego de esa zona activo.
bool irrigationManualIsActive(ZoneType type, uint8_t zone);

// Establece/consulta horarios (M4 los llenará desde /config).
void irrigationSetSchedule(ZoneType type, uint8_t zone, uint8_t index, const ZoneSchedule& sched);
ZoneSchedule irrigationGetSchedule(ZoneType type, uint8_t zone, uint8_t index);

// Establece/consulta estado pausado (M4 lo trae del backend; default false).
void irrigationSetPaused(ZoneType type, uint8_t zone, bool paused);
bool irrigationIsPaused(ZoneType type, uint8_t zone);

// Establece/consulta duración de riego configurada por zona (segundos).
// Default de fábrica: DEFAULT_IRRIGATION_S (60 s). El manual (6.3) usa el
// mismo valor que el horario de la zona.
void irrigationSetDuration(ZoneType type, uint8_t zone, uint16_t duration_s);
uint16_t irrigationGetDuration(ZoneType type, uint8_t zone);

// Interlocks y límites (perfil local / configuración del backend).
// - interlock: un solo riego a la vez entre Z1, Z2 y aspersión.
// - maxRunS: corte duro por zona (tope absoluto IRRIGATION_MAX_RUN_ABS_S).
// - minOffS: espera entre ciclos automáticos por umbral.
void irrigationSetInterlock(bool enabled);
bool irrigationGetInterlock();
void irrigationSetMaxRunS(ZoneType type, uint16_t seconds);
uint16_t irrigationGetMaxRunS(ZoneType type);
void irrigationSetMinOffS(uint16_t seconds);
uint16_t irrigationGetMinOffS();
// Espera entre zonas distintas tras un apagado (0..600 s).
void irrigationSetStaggerS(uint16_t seconds);
uint16_t irrigationGetStaggerS();

// Establece/consulta umbral mínimo de humedad de sustrato por zona (%).
// Default de fábrica: DEFAULT_MIN_HUMIDITY (20 %).
void irrigationSetMinHumidity(uint8_t zone, uint8_t minPct);
uint8_t irrigationGetMinHumidity(uint8_t zone);

// Habilita/deshabilita el riego automático por umbral de humedad (sustrato).
void irrigationSetThresholdEnabled(uint8_t zone, bool enabled);
bool irrigationGetThresholdEnabled(uint8_t zone);

// Consulta estado actual de riego de una zona.
ZoneIrrigationState irrigationGetState(ZoneType type, uint8_t zone);

#endif // IRRIGATION_H