#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>
#include <time.h>

#include "config.h"
#include "irrigation.h"

// ============================================================================
// events.h — Buffer de eventos de riego para M4 (Milestone 3)
// Fuente: especificacion_firmware.md secciones 6, 10; CLINE.md regla 6.
// - Eventos de riego con client_event_id (idempotencia en reintentos).
// - Buffer FIFO acotado (MAX_IRRIGATION_EVENTS=32), descarta el más antiguo al llenarse.
// - trigger_type: horario/manual/umbral/override.
// - started_at/ended_at, suppressed.
// - M4 enviará al backend; en M3 solo se generan y registran por Serial.
// ============================================================================

struct IrrigationEvent {
    uint32_t clientEventId;      // ID local único para idempotencia (regla 6)
    ZoneType zoneType;           // SUBSTRATE / SPRINKLER
    uint8_t zone;                // índice 0-based
    IrrigationTrigger trigger;   // MANUAL / OVERRIDE / SCHEDULE / THRESHOLD
    struct tm startedAt;         // timestamp de inicio
    struct tm endedAt;           // timestamp de fin (vacío mientras activo)
    bool suppressed;             // true si perdió por precedencia
};

// Inicializa el buffer de eventos.
void eventsInit();

// Agrega un evento de riego al buffer FIFO.
// Si el buffer está lleno, descarta el evento más antiguo (FIFO).
void eventsPushIrrigation(const IrrigationEvent& evt);

// Obtiene el número de eventos pendientes en el buffer.
uint8_t eventsPendingCount();

// Extrae (pop) el evento más antiguo del buffer.
// Devuelve false si el buffer está vacío.
bool eventsPopIrrigation(IrrigationEvent& outEvt);

// Completa el evento de riego abierto más reciente de una zona, rellenando
// endedAt. Devuelve true si encontró y completó un evento abierto (endedAt
// con tm_year==0). Idempotente: un evento ya completado no se vuelve a tocar.
bool eventsCompleteIrrigation(ZoneType type, uint8_t zone, const struct tm& endedAt);

// Limpia el buffer (para tests o reinicio).
void eventsClear();

#endif // EVENTS_H