#ifndef CLIMATE_H
#define CLIMATE_H

#include <stdint.h>

#include "config.h"
#include "sensors.h"

// ============================================================================
// climate.h — Termostato de cama y clima de ventilación
// Fuente: docs/03_reglas_y_plan.md §5 y §7.
// - Cama: ON en T <= setpoint - h; OFF en T >= setpoint. Corte absoluto en
//   BED_MAX_C (no anulable). Sensor inválido → OFF. Anti-ciclado 60/120 s.
// - Ventilación: ON si T >= on_temp o RH >= on_rh, sostenido 2 ciclos.
//   OFF si T <= off_temp y RH <= off_rh, sostenido 2 ciclos. La demanda por
//   horario se suma con OR (se pasa como ventilationScheduleRequest).
// - Ambas salidas usan channel_control (AUTO) para respetar el manual.
// ============================================================================

void climateInit();

// Evaluación por ciclo local (30 s).
void climateEvaluate(const SensorReadings& readings, bool ventilationScheduleRequest);

// Confirmar en el menú para cama: apaga si está ON; si está OFF intenta un
// encendido manual sujeto a sensor válido, corte absoluto y anti-ciclado.
bool climateBedManualToggle(uint32_t durationMs);

// Setpoint de cama (persistido en NVS).
void climateSetBedTargetC(float targetC);
float climateGetBedTargetC();

// --- Comandos/Config remotos (M6) ---
// Encendido manual remoto de la cama, sujeto a sensor válido, corte absoluto
// y anti-ciclado. Devuelve false y completa reason si no se puede.
bool climateBedRemoteOn(uint32_t durationMs, const char** reason);
void climateBedRemoteOff();
// Vuelve al termostato (descarta la orden manual vigente).
void climateBedRemoteAuto();
bool climateBedSensorValid();
float climateBedLastTempC();

// Automatización configurable por el backend. El corte absoluto BED_MAX_C
// sigue siendo no anulable.
void climateSetBedAutoEnabled(bool enabled);
bool climateGetBedAutoEnabled();
void climateSetVentAutoEnabled(bool enabled);
bool climateGetVentAutoEnabled();
void climateSetVentClimateEnabled(bool enabled);
bool climateGetVentClimateEnabled();
void climateSetVentThresholds(float onTempC, float offTempC,
                              float onRhPct, float offRhPct);

// Umbrales activos del clima de ventilación (para el perfil local/portal).
float climateGetVentOnTempC();
float climateGetVentOffTempC();
float climateGetVentOnRhPct();
float climateGetVentOffRhPct();

#endif // CLIMATE_H
