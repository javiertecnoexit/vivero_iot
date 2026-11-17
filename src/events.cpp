#include "events.h"

#include <Arduino.h>

#include "config.h"

// ============================================================================
// events.cpp — Buffer FIFO de eventos de riego (Milestone 3)
// Fuente: especificacion_firmware.md secciones 6, 10; CLINE.md regla 6.
// - Buffer FIFO acotado (MAX_IRRIGATION_EVENTS=32), descarta el más antiguo al llenarse.
// - M4 enviará al backend; en M3 solo se generan y registran por Serial.
// ============================================================================

static IrrigationEvent s_buffer[MAX_IRRIGATION_EVENTS];
static uint8_t s_head = 0;   // índice del más antiguo
static uint8_t s_tail = 0;   // índice donde escribir el siguiente
static uint8_t s_count = 0;  // elementos en el buffer

void eventsInit() {
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    Serial.println("[EVENTS] Buffer de eventos de riego inicializado");
}

void eventsPushIrrigation(const IrrigationEvent& evt) {
    if (s_count >= MAX_IRRIGATION_EVENTS) {
        // Buffer lleno: descartar el más antiguo (FIFO)
        s_head = (s_head + 1) % MAX_IRRIGATION_EVENTS;
        s_count--;
        Serial.println("[EVENTS] Buffer lleno: evento más antiguo descartado (FIFO)");
    }

    s_buffer[s_tail] = evt;
    s_tail = (s_tail + 1) % MAX_IRRIGATION_EVENTS;
    s_count++;

    Serial.printf("[EVENTS] Push: clientEventId=%lu zone=%s%u trigger=%d suppressed=%d (pendientes=%u)\n",
                  (unsigned long)evt.clientEventId,
                  (evt.zoneType == ZoneType::SUBSTRATE) ? "S" : "A",
                  evt.zone, (int)evt.trigger, evt.suppressed, s_count);
}

uint8_t eventsPendingCount() {
    return s_count;
}

bool eventsPopIrrigation(IrrigationEvent& outEvt) {
    if (s_count == 0) {
        return false;
    }

    outEvt = s_buffer[s_head];
    s_head = (s_head + 1) % MAX_IRRIGATION_EVENTS;
    s_count--;
    return true;
}

bool eventsCompleteIrrigation(ZoneType type, uint8_t zone, const struct tm& endedAt) {
    if (s_count == 0) {
        return false;
    }

    // Busca el evento abierto MÁS RECIENTE de la zona: se recorre el buffer
    // desde el tail hacia atrás (el tail es donde se escribió el último).
    for (uint8_t i = 0; i < s_count; i++) {
        const uint8_t idx = (s_tail + MAX_IRRIGATION_EVENTS - 1 - i) % MAX_IRRIGATION_EVENTS;
        IrrigationEvent& evt = s_buffer[idx];
        if (evt.zoneType == type && evt.zone == zone && evt.endedAt.tm_year == 0) {
            evt.endedAt = endedAt;
            Serial.printf("[EVENTS] Completado ended_at clientEventId=%lu zone=%s%u\n",
                          (unsigned long)evt.clientEventId,
                          (type == ZoneType::SUBSTRATE) ? "S" : "A", zone);
            return true;
        }
    }
    return false;
}

void eventsClear() {
    s_head = 0;
    s_tail = 0;
    s_count = 0;
}