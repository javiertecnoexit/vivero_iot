#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <time.h>

#include "config.h"

// ============================================================================
// scheduler.h — Horarios locales por canal (iluminación y ventilación)
// Fuente: docs/03_reglas_y_plan.md §6 y §7.
// - Cada entrada tiene hora/minuto local + duración; se repite todos los días.
// - La ventana activa se calcula por minuto del día (soporta cruce de medianoche).
// - Iluminación: el horario gobierna directo (AUTO).
// - Ventilación: el horario es un pedido más; climate.cpp lo combina con OR.
// - Sin RTC válido no se ejecutan horarios.
// ============================================================================

struct ChannelSchedule {
    uint8_t  hour;
    uint8_t  minute;
    uint16_t duration_s;
    bool     enabled;
    uint8_t  daysMask;   // bit 0 = lunes ... bit 6 = domingo
};

void schedulerInit();

// Evalúa los horarios para la hora local dada y actualiza los pedidos.
void schedulerEvaluate(const struct tm& nowLocal);

// Pedido de ventilación por horario (para climate.cpp).
bool schedulerVentilationRequested();

// Configuración de horarios (el portal/backend los llenará en M5/M6).
void schedulerSetSchedule(RelayChannel channel, uint8_t index, const ChannelSchedule& sched);
ChannelSchedule schedulerGetSchedule(RelayChannel channel, uint8_t index);

#endif // SCHEDULER_H
