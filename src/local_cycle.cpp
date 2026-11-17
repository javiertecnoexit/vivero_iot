#include "local_cycle.h"

#include <Arduino.h>

#include "alarm.h"
#include "config.h"
#include "display.h"
#include "irrigation.h"
#include "rtc.h"
#include "scheduler.h"
#include "climate.h"
#include "sensors.h"
#include "window.h"
#include "ws_client.h"

// ============================================================================
// local_cycle.cpp — Ciclo local de 30 s con millis() (regla 1 de CLINE.md)
// No bloqueante: el loop sigue atendiendo botones y servidor web mientras tanto.
// ============================================================================

static uint32_t s_lastTickMs = 0UL;
static uint32_t s_cycleCount = 0UL;
static SensorReadings s_lastReadings = {};

void localCycleInit() {
    s_lastTickMs = millis();
    s_cycleCount = 0UL;
}

void localCycleUpdate() {
    const uint32_t now = millis();
    // La resta con uint32_t es segura ante rollover de millis() (~49 días).
    if ((now - s_lastTickMs) >= LOCAL_CYCLE_MS) {
        s_lastTickMs = now;  // Se ancla al momento actual, no al tiempo teórico
        s_cycleCount++;
        Serial.printf("[CICLO] Ciclo local #%lu completado (30s)\n",
                      (unsigned long)s_cycleCount);

        // M2: leer los sensores y mostrar el estado completo por Serial.
        const SensorReadings readings = sensorsRead();
        sensorsPrint(readings);
        s_lastReadings = readings;

        // M6: acumular la muestra en la cola del WebSocket (se envía al
        // backend cuando hay sesión; sin red queda en RAM para reenvío).
        const struct tm tmReadings = rtcNow();
        wsRecordSample(tmReadings, readings);

        // M3: evaluar riego (precedencia, manual, horario, umbral, paused).
        // Horarios en hora local; eventos y telemetría en UTC.
        const struct tm tmUtc = rtcNow();
        const struct tm tmLocal = rtcNowLocal();
        irrigationEvaluate(tmUtc, tmLocal, readings);

        // M3: horarios de iluminación/ventilación y clima (cama/ventilación).
        schedulerEvaluate(tmLocal);
        climateEvaluate(readings, schedulerVentilationRequested());

        // M3: evaluar alarmas (OR lógico: temp aire > max ∨ temp aire < min)
        alarmEvaluate(tmUtc, readings);

        Serial.printf("[CICLO] Ventana: %s\n", windowStateName(windowGetState()));
    }
}

uint32_t localCycleCount() {
    return s_cycleCount;
}

const SensorReadings& localCycleLastReadings() {
    return s_lastReadings;
}
