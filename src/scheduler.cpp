#include "scheduler.h"

#include <Arduino.h>

#include "channel_control.h"
#include "relay.h"
#include "rtc.h"
#include "settings.h"

// ============================================================================
// scheduler.cpp — Horarios locales por canal
// - Índice 0: iluminación; índice 1: ventilación.
// - Duración máxima de una entrada: 16 h (corte duro de iluminación).
// ============================================================================

#define SCHEDULER_CHANNELS 2
#define SCHEDULER_MAX_DURATION_S (16UL * 3600UL)

static ChannelSchedule s_schedules[SCHEDULER_CHANNELS][MAX_SCHEDULES_PER_ZONE];
static bool s_ventRequest = false;

// Corte duro de iluminación: 16 h de pedido continuo.
static bool     s_lightWasRequested = false;
static uint32_t s_lightRequestStartMs = 0UL;
static bool     s_lightLockedOut = false;

static int channelSlot(RelayChannel channel) {
    switch (channel) {
        case RelayChannel::ILUMINACION: return 0;
        case RelayChannel::VENTILACION: return 1;
        default:                        return -1;
    }
}

static bool scheduleActiveAt(const ChannelSchedule& sched, uint16_t nowMinute,
                             const struct tm& nowLocal) {
    if (!sched.enabled || sched.duration_s == 0) {
        return false;
    }
    if (sched.hour > 23 || sched.minute > 59) {
        return false;
    }
    const uint8_t mondayBit = (uint8_t)(((nowLocal.tm_wday + 6) % 7));
    if ((sched.daysMask & (uint8_t)(1u << mondayBit)) == 0) {
        return false;
    }

    const uint16_t start = (uint16_t)(sched.hour * 60 + sched.minute);
    uint32_t durationMin = (sched.duration_s + 59UL) / 60UL;
    if (durationMin > 24UL * 60UL) {
        durationMin = 24UL * 60UL;
    }

    // Delta positivo desde el inicio, con vuelta de medianoche.
    const uint16_t delta = (uint16_t)((nowMinute + 1440U - start) % 1440U);
    return (uint32_t)delta < durationMin;
}

static bool anyScheduleActive(int slot, uint16_t nowMinute, const struct tm& nowLocal) {
    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        if (scheduleActiveAt(s_schedules[slot][i], nowMinute, nowLocal)) {
            return true;
        }
    }
    return false;
}

void schedulerInit() {
    for (uint8_t c = 0; c < SCHEDULER_CHANNELS; c++) {
        for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
            s_schedules[c][i] = { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
        }
    }
    s_ventRequest = false;
    s_lightWasRequested = false;
    s_lightRequestStartMs = 0UL;
    s_lightLockedOut = false;
    Serial.println("[SCHED] Horarios inicializados (sin entradas activas)");
}

void schedulerEvaluate(const struct tm& nowLocal) {
    if (!rtcIsTimeValid()) {
        // Sin hora válida no se ejecutan horarios. El manual sigue disponible.
        s_ventRequest = false;
        return;
    }

    const uint16_t nowMinute = (uint16_t)(nowLocal.tm_hour * 60 + nowLocal.tm_min);

    // Iluminación: el horario gobierna directo (AUTO).
    bool lightRequest = anyScheduleActive(0, nowMinute, nowLocal);

    // Corte duro a las 16 h continuas. Se libera cuando el horario deja de
    // pedir iluminación; un nuevo ciclo vuelve a contar desde cero.
    if (!lightRequest) {
        s_lightLockedOut = false;
        s_lightRequestStartMs = 0UL;
    } else if (!s_lightWasRequested) {
        s_lightRequestStartMs = millis();
    }
    if (lightRequest && !s_lightLockedOut &&
        (millis() - s_lightRequestStartMs) >= LIGHT_MAX_ON_MS) {
        s_lightLockedOut = true;
        Serial.println("[SCHED] Iluminación OFF: 16 h continuas (corte duro)");
    }
    if (s_lightLockedOut) {
        lightRequest = false;
    }
    s_lightWasRequested = lightRequest;

    if (settingsIsChannelEnabled(RelayChannel::ILUMINACION) &&
        !channelControlIsManual(RelayChannel::ILUMINACION)) {
        channelControlSetAuto(RelayChannel::ILUMINACION, lightRequest);
    }

    // Ventilación: solo se publica el pedido; climate.cpp lo combina con OR.
    s_ventRequest = anyScheduleActive(1, nowMinute, nowLocal);
}

bool schedulerVentilationRequested() {
    return s_ventRequest;
}

void schedulerSetSchedule(RelayChannel channel, uint8_t index, const ChannelSchedule& sched) {
    const int slot = channelSlot(channel);
    if (slot < 0 || index >= MAX_SCHEDULES_PER_ZONE) {
        return;
    }

    ChannelSchedule copy = sched;
    if (copy.hour > 23 || copy.minute > 59) {
        copy.enabled = false;
    }
    if (copy.duration_s > SCHEDULER_MAX_DURATION_S) {
        copy.duration_s = (uint16_t)SCHEDULER_MAX_DURATION_S;
    }

    s_schedules[slot][index] = copy;
}

ChannelSchedule schedulerGetSchedule(RelayChannel channel, uint8_t index) {
    const int slot = channelSlot(channel);
    if (slot < 0 || index >= MAX_SCHEDULES_PER_ZONE) {
        return { 0, 0, 0, false, SCHEDULE_DAYS_ALL };
    }
    return s_schedules[slot][index];
}
