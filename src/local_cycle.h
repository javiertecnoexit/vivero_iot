#ifndef LOCAL_CYCLE_H
#define LOCAL_CYCLE_H

#include <stdint.h>

#include "sensors.h"

// ============================================================================
// local_cycle.h — Ciclo local de 30 s (Milestone 1)
// Fuente: especificacion_firmware.md sección 5.
// Corre con millis() (regla 1 de CLINE.md) — sin delay() en el loop.
// En M1 solo cuenta ciclos y expone el contador; la lógica de riego/alarmas
// llega en M2/M3.
// ============================================================================

// Inicializa el ciclo local.
void localCycleInit();

// Procesa el ciclo local. Debe llamarse frecuentemente desde el loop.
// Dispara la acción del ciclo cada LOCAL_CYCLE_MS (30 s).
void localCycleUpdate();

// Devuelve el número de ciclos locales completados hasta ahora.
uint32_t localCycleCount();

// Devuelve las últimas lecturas de sensores del ciclo completado más reciente
// (para mostrarlas en pantalla entre ciclo y ciclo).
const SensorReadings& localCycleLastReadings();

#endif // LOCAL_CYCLE_H