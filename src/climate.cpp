#include "climate.h"

#include <Arduino.h>

#include "channel_control.h"
#include "relay.h"
#include "settings.h"

// ============================================================================
// climate.cpp — Termostato de cama y clima de ventilación
// ============================================================================

// --- Cama ---
static bool     s_bedHasValid = false;
static float    s_bedLastTempC = 0.0f;
static bool     s_bedWasOn = false;
static bool     s_bedEverSwitched = false;
static uint32_t s_bedLastOnMs = 0UL;
static uint32_t s_bedLastOffMs = 0UL;
static bool     s_bedAutoEnabled = true;

// --- Ventilación ---
static uint8_t  s_ventOnCount = 0;
static uint8_t  s_ventOffCount = 0;
static bool     s_ventClimateOn = false;
static bool     s_ventWasOn = false;
static bool     s_ventEverSwitched = false;
static uint32_t s_ventLastOnMs = 0UL;
static uint32_t s_ventLastOffMs = 0UL;
static uint32_t s_ventRunStartMs = 0UL;
static uint32_t s_ventRestUntilMs = 0UL;
static bool     s_ventAutoEnabled = true;
static bool     s_ventClimateEnabled = true;
static float    s_ventOnTempC = DEFAULT_VENT_ON_TEMP_C;
static float    s_ventOffTempC = DEFAULT_VENT_OFF_TEMP_C;
static float    s_ventOnRhPct = DEFAULT_VENT_ON_RH_PCT;
static float    s_ventOffRhPct = DEFAULT_VENT_OFF_RH_PCT;

static void bedTrackTransition(uint32_t now) {
    const bool on = channelControlIsOn(RelayChannel::CAMA_CALIENTE);
    if (on == s_bedWasOn) {
        return;
    }

    s_bedWasOn = on;
    s_bedEverSwitched = true;
    if (on) {
        s_bedLastOnMs = now;
    } else {
        s_bedLastOffMs = now;
    }
}

static bool bedAutoOnAllowed(uint32_t now) {
    // Anti-ciclado: mínimo apagado antes de volver a encender.
    if (s_bedEverSwitched && !s_bedWasOn &&
        (now - s_bedLastOffMs) < BED_MIN_OFF_MS) {
        return false;
    }
    return true;
}

static bool bedAutoOffAllowed(uint32_t now) {
    // Anti-ciclado: mínimo encendido antes de apagar por banda.
    if (s_bedWasOn && (now - s_bedLastOnMs) < BED_MIN_ON_MS) {
        return false;
    }
    return true;
}

static void ventTrackTransition(uint32_t now) {
    const bool on = channelControlIsOn(RelayChannel::VENTILACION);
    if (on == s_ventWasOn) {
        return;
    }

    s_ventWasOn = on;
    s_ventEverSwitched = true;
    if (on) {
        s_ventLastOnMs = now;
        s_ventRunStartMs = now;
    } else {
        s_ventLastOffMs = now;
        s_ventRunStartMs = 0UL;
    }
}

void climateInit() {
    s_bedHasValid = false;
    s_bedLastTempC = 0.0f;
    s_bedWasOn = false;
    s_bedEverSwitched = false;
    s_bedLastOnMs = 0UL;
    s_bedLastOffMs = 0UL;

    s_ventOnCount = 0;
    s_ventOffCount = 0;
    s_ventClimateOn = false;
    s_ventWasOn = false;
    s_ventEverSwitched = false;
    s_ventLastOnMs = 0UL;
    s_ventLastOffMs = 0UL;
    s_ventRunStartMs = 0UL;
    s_ventRestUntilMs = 0UL;
    s_ventAutoEnabled = true;
    s_ventClimateEnabled = true;
    s_ventOnTempC = DEFAULT_VENT_ON_TEMP_C;
    s_ventOffTempC = DEFAULT_VENT_OFF_TEMP_C;
    s_ventOnRhPct = DEFAULT_VENT_ON_RH_PCT;
    s_ventOffRhPct = DEFAULT_VENT_OFF_RH_PCT;
    s_bedAutoEnabled = true;

    Serial.printf("[CLIMATE] Inicializado: cama setpoint=%.1fC max=%.1fC\n",
                  settingsGetBedTargetC(), BED_MAX_C);
}

void climateEvaluate(const SensorReadings& readings, bool ventilationScheduleRequest) {
    const uint32_t now = millis();

    // ------------------------------------------------------------------
    // Cama caliente
    // ------------------------------------------------------------------
    if (!settingsIsChannelEnabled(RelayChannel::CAMA_CALIENTE)) {
        channelControlForceOff(RelayChannel::CAMA_CALIENTE);
        s_bedWasOn = false;
        s_bedHasValid = false;
    } else {
        const bool valid = readings.bedTemp.valid;

        if (!valid) {
            // Sensor inválido: OFF inmediato, sin respaldo por aire.
            s_bedHasValid = false;
            if (channelControlIsOn(RelayChannel::CAMA_CALIENTE)) {
                channelControlForceOff(RelayChannel::CAMA_CALIENTE);
                Serial.println("[CLIMATE] Cama OFF: sensor DS18B20 inválido");
            }
        } else {
            s_bedHasValid = true;
            s_bedLastTempC = readings.bedTemp.tempC;

            if (s_bedLastTempC >= BED_MAX_C) {
                // Corte absoluto: no se puede anular ni con manual.
                if (channelControlIsOn(RelayChannel::CAMA_CALIENTE)) {
                    channelControlForceOff(RelayChannel::CAMA_CALIENTE);
                    Serial.printf("[CLIMATE] Cama OFF por corte absoluto (%.1f >= %.1f C)\n",
                                  s_bedLastTempC, BED_MAX_C);
                }
            } else if (!channelControlIsManual(RelayChannel::CAMA_CALIENTE)) {
                if (!s_bedAutoEnabled) {
                    if (channelControlIsOn(RelayChannel::CAMA_CALIENTE)) {
                        channelControlForceOff(RelayChannel::CAMA_CALIENTE);
                    }
                } else {
                    const float target = settingsGetBedTargetC();
                    const bool on = channelControlIsOn(RelayChannel::CAMA_CALIENTE);

                    if (!on && s_bedLastTempC <= (target - BED_THERMOSTAT_HYST_C)) {
                        if (bedAutoOnAllowed(now)) {
                            channelControlSetAuto(RelayChannel::CAMA_CALIENTE, true);
                            Serial.printf("[CLIMATE] Cama ON (%.1f <= %.1f C)\n",
                                          s_bedLastTempC, target - BED_THERMOSTAT_HYST_C);
                        }
                    } else if (on && s_bedLastTempC >= target) {
                        if (bedAutoOffAllowed(now)) {
                            channelControlSetAuto(RelayChannel::CAMA_CALIENTE, false);
                            Serial.printf("[CLIMATE] Cama OFF (%.1f >= %.1f C)\n",
                                          s_bedLastTempC, target);
                        }
                    }
                }
            }
        }
    }

    bedTrackTransition(now);

    // ------------------------------------------------------------------
    // Ventilación: clima (OR temp/RH) + horario (OR)
    // ------------------------------------------------------------------
    if (!settingsIsChannelEnabled(RelayChannel::VENTILACION)) {
        channelControlForceOff(RelayChannel::VENTILACION);
        s_ventOnCount = 0;
        s_ventOffCount = 0;
        s_ventClimateOn = false;
        s_ventWasOn = false;
        s_ventEverSwitched = false;
        s_ventLastOnMs = 0UL;
        s_ventLastOffMs = 0UL;
        s_ventRunStartMs = 0UL;
        s_ventRestUntilMs = 0UL;
        return;
    }

    if (channelControlIsManual(RelayChannel::VENTILACION)) {
        // El manual manda; el clima se reevalúa al vencer.
        s_ventOnCount = 0;
        s_ventOffCount = 0;
        ventTrackTransition(now);
        return;
    }

    const AirReading& air = readings.air;
    if (!air.valid || !s_ventAutoEnabled || !s_ventClimateEnabled) {
        s_ventClimateOn = false;
        s_ventOnCount = 0;
        s_ventOffCount = 0;
    } else {
        const bool onCond = (air.tempC >= s_ventOnTempC) ||
                            (air.humidityPct >= s_ventOnRhPct);
        const bool offCond = (air.tempC <= s_ventOffTempC) &&
                             (air.humidityPct <= s_ventOffRhPct);

        if (onCond) {
            if (s_ventOnCount < VENT_SUSTAIN_CYCLES) {
                s_ventOnCount++;
            }
            s_ventOffCount = 0;
            if (s_ventOnCount >= VENT_SUSTAIN_CYCLES) {
                s_ventClimateOn = true;
            }
        } else if (offCond) {
            if (s_ventOffCount < VENT_SUSTAIN_CYCLES) {
                s_ventOffCount++;
            }
            s_ventOnCount = 0;
            if (s_ventOffCount >= VENT_SUSTAIN_CYCLES) {
                s_ventClimateOn = false;
            }
        } else {
            // Banda intermedia: mantener la decisión anterior.
            s_ventOnCount = 0;
            s_ventOffCount = 0;
        }
    }

    bool ventDesired = (s_ventAutoEnabled && s_ventClimateOn) ||
                       (s_ventAutoEnabled && ventilationScheduleRequest);
    const bool ventOn = channelControlIsOn(RelayChannel::VENTILACION);

    // Corte duro por max_run: OFF + descanso obligatorio antes de reintentar.
    if (ventOn && s_ventRunStartMs != 0UL &&
        (now - s_ventRunStartMs) >= VENT_MAX_RUN_MS) {
        channelControlForceOff(RelayChannel::VENTILACION);
        s_ventWasOn = false;
        s_ventEverSwitched = true;
        s_ventLastOffMs = now;
        s_ventRunStartMs = 0UL;
        s_ventRestUntilMs = now + VENT_MIN_OFF_MS;
        ventDesired = false;
        Serial.printf("[CLIMATE] Ventilación OFF por max_run (%lu min); descanso %lu s\n",
                      (unsigned long)(VENT_MAX_RUN_MS / 60000UL),
                      (unsigned long)(VENT_MIN_OFF_MS / 1000UL));
    }

    // Descanso posterior al max_run.
    if (s_ventRestUntilMs != 0UL) {
        if ((int32_t)(now - s_ventRestUntilMs) < 0) {
            ventDesired = false;
        } else {
            s_ventRestUntilMs = 0UL;
        }
    }

    // Tiempos mínimos ON/OFF (salvo corte por seguridad, que ya ocurrió).
    if (ventDesired && !ventOn) {
        if (s_ventEverSwitched && (now - s_ventLastOffMs) < VENT_MIN_OFF_MS) {
            ventDesired = false;
        }
    } else if (!ventDesired && ventOn) {
        if (s_ventEverSwitched && (now - s_ventLastOnMs) < VENT_MIN_ON_MS) {
            ventDesired = true;
        }
    }

    channelControlSetAuto(RelayChannel::VENTILACION, ventDesired);
    ventTrackTransition(now);
}

bool climateBedManualToggle(uint32_t durationMs) {
    if (!settingsIsChannelEnabled(RelayChannel::CAMA_CALIENTE)) {
        return false;
    }

    // Si está ON (auto o manual), Confirmar lo apaga. El OFF siempre se permite.
    if (channelControlIsOn(RelayChannel::CAMA_CALIENTE)) {
        channelControlForceOff(RelayChannel::CAMA_CALIENTE);
        bedTrackTransition(millis());
        Serial.println("[CLIMATE] Cama OFF manual");
        return true;
    }

    if (!s_bedHasValid) {
        Serial.println("[CLIMATE] Cama ON rechazado: sensor inválido");
        return false;
    }
    if (s_bedLastTempC >= BED_MAX_C) {
        Serial.printf("[CLIMATE] Cama ON rechazado: %.1f >= max %.1f C\n",
                      s_bedLastTempC, BED_MAX_C);
        return false;
    }
    if (!bedAutoOnAllowed(millis())) {
        Serial.println("[CLIMATE] Cama ON rechazado: anti-ciclado (min off)");
        return false;
    }

    const bool ok = channelControlRequest(RelayChannel::CAMA_CALIENTE, true,
                                          ChannelSource::MANUAL_LOCAL, durationMs);
    if (ok) {
        bedTrackTransition(millis());
    }
    return ok;
}

void climateSetBedTargetC(float targetC) {
    settingsSetBedTargetC(targetC);
}

float climateGetBedTargetC() {
    return settingsGetBedTargetC();
}

bool climateBedSensorValid() {
    return s_bedHasValid;
}

float climateBedLastTempC() {
    return s_bedLastTempC;
}

bool climateBedRemoteOn(uint32_t durationMs, const char** reason) {
    if (reason) *reason = nullptr;
    if (!settingsIsChannelEnabled(RelayChannel::CAMA_CALIENTE)) {
        if (reason) *reason = "channel_disabled";
        return false;
    }
    if (!s_bedHasValid) {
        if (reason) *reason = "sensor";
        return false;
    }
    if (s_bedLastTempC >= BED_MAX_C) {
        if (reason) *reason = "safety_interlock";
        return false;
    }
    if (!bedAutoOnAllowed(millis())) {
        if (reason) *reason = "busy";
        return false;
    }

    const bool ok = channelControlRequest(RelayChannel::CAMA_CALIENTE, true,
                                          ChannelSource::MANUAL_REMOTE, durationMs);
    if (!ok) {
        if (reason) *reason = "busy";
        return false;
    }
    bedTrackTransition(millis());
    Serial.printf("[CLIMATE] Cama ON remoto dur=%lu ms\n", (unsigned long)durationMs);
    return true;
}

void climateBedRemoteOff() {
    channelControlForceOff(RelayChannel::CAMA_CALIENTE);
    bedTrackTransition(millis());
    Serial.println("[CLIMATE] Cama OFF remoto");
}

void climateBedRemoteAuto() {
    channelControlForceOff(RelayChannel::CAMA_CALIENTE);
    bedTrackTransition(millis());
    Serial.println("[CLIMATE] Cama AUTO remoto (termostato)");
}

void climateSetBedAutoEnabled(bool enabled) {
    s_bedAutoEnabled = enabled;
    if (!enabled) {
        s_bedWasOn = channelControlIsOn(RelayChannel::CAMA_CALIENTE);
    }
}

bool climateGetBedAutoEnabled() {
    return s_bedAutoEnabled;
}

void climateSetVentAutoEnabled(bool enabled) {
    s_ventAutoEnabled = enabled;
}

bool climateGetVentAutoEnabled() {
    return s_ventAutoEnabled;
}

void climateSetVentClimateEnabled(bool enabled) {
    s_ventClimateEnabled = enabled;
}

bool climateGetVentClimateEnabled() {
    return s_ventClimateEnabled;
}

void climateSetVentThresholds(float onTempC, float offTempC,
                              float onRhPct, float offRhPct) {
    // Validación defensiva: el parser de config ya validó los rangos.
    if (onTempC < -20.0f || onTempC > 80.0f || offTempC < -20.0f ||
        offTempC > 80.0f || onRhPct < 0.0f || onRhPct > 100.0f ||
        offRhPct < 0.0f || offRhPct > 100.0f) {
        return;
    }
    s_ventOnTempC = onTempC;
    s_ventOffTempC = offTempC;
    s_ventOnRhPct = onRhPct;
    s_ventOffRhPct = offRhPct;
}

float climateGetVentOnTempC() {
    return s_ventOnTempC;
}

float climateGetVentOffTempC() {
    return s_ventOffTempC;
}

float climateGetVentOnRhPct() {
    return s_ventOnRhPct;
}

float climateGetVentOffRhPct() {
    return s_ventOffRhPct;
}
