#include "window.h"

#include <Arduino.h>

#include "config.h"
#include "relay.h"
#include "settings.h"

// ============================================================================
// window.cpp — Control de la ventana (pulso toggle de 5 s)
// - Un solo sensor de cerrada en GPIO13 (LOW = cerrada), debounce 50 ms.
// - El controlador externo alterna el sentido en cada pulso.
// - El pulso lo corta este módulo y, como red de seguridad, relayUpdate().
// ============================================================================

static WindowState  s_state = WindowState::OPEN;
static WindowTarget s_target = WindowTarget::NONE;

// Debounce del sensor.
static bool     s_rawClosed = false;
static bool     s_lastRawClosed = false;
static bool     s_stableClosed = false;
static uint32_t s_lastRawChangeMs = 0;

// Pulso en curso.
static bool     s_pulseActive = false;
static uint32_t s_pulseStartMs = 0;
static uint32_t s_moveStartMs = 0;

// Intervalo mínimo entre pulsos.
static bool     s_hasPulsed = false;
static uint32_t s_lastPulseMs = 0;

static bool readClosedRaw() {
    return digitalRead(PIN_WINDOW_CLOSED) == WINDOW_CLOSED_LEVEL;
}

static bool minIntervalElapsed() {
    if (!s_hasPulsed) {
        return true;
    }
    return (millis() - s_lastPulseMs) >= WINDOW_MIN_INTERVAL_MS;
}

static bool startPulse(WindowTarget target) {
    if (s_pulseActive) {
        return false;
    }
    if (!minIntervalElapsed()) {
        Serial.printf("[WINDOW] Pulso rechazado: faltan %lu s del intervalo mínimo\n",
                      (unsigned long)((WINDOW_MIN_INTERVAL_MS -
                                       (millis() - s_lastPulseMs)) / 1000UL));
        return false;
    }
    if (!settingsIsChannelEnabled(RelayChannel::VENTANA)) {
        Serial.println("[WINDOW] Canal ventana deshabilitado");
        return false;
    }
    if (!relaySetChannel(RelayChannel::VENTANA, true)) {
        return false;
    }

    s_pulseActive  = true;
    s_pulseStartMs = millis();
    s_lastPulseMs  = s_pulseStartMs;
    s_hasPulsed    = true;
    s_state        = WindowState::MOVING;
    s_target       = target;

    Serial.printf("[WINDOW] Pulso de %lu ms iniciado (%s)\n",
                  (unsigned long)WINDOW_PULSE_MS,
                  (target == WindowTarget::OPEN) ? "abrir" : "cerrar");
    return true;
}

void windowInit() {
    pinMode(PIN_WINDOW_CLOSED, INPUT_PULLUP);

    s_rawClosed = s_lastRawClosed = s_stableClosed = readClosedRaw();
    s_lastRawChangeMs = millis();

    s_state = s_stableClosed ? WindowState::CLOSED : WindowState::OPEN;
    s_target = WindowTarget::NONE;
    s_pulseActive = false;
    s_pulseStartMs = 0;
    s_moveStartMs = 0;
    s_hasPulsed = false;
    s_lastPulseMs = 0;

    relaySetChannel(RelayChannel::VENTANA, false);

    Serial.printf("[WINDOW] Inicializada: sensor=%s, estado=%s\n",
                  s_stableClosed ? "CERRADA" : "NO CERRADA",
                  windowStateName(s_state));
}

void windowUpdate() {
    const uint32_t now = millis();

    // 1) Debounce del sensor de cerrada.
    const bool raw = readClosedRaw();
    if (raw != s_lastRawClosed) {
        s_lastRawClosed = raw;
        s_lastRawChangeMs = now;
    }
    if (s_stableClosed != raw && (now - s_lastRawChangeMs) >= WINDOW_SENSOR_DEBOUNCE_MS) {
        s_stableClosed = raw;
        Serial.printf("[WINDOW] Sensor estable: %s\n",
                      s_stableClosed ? "CERRADA" : "NO CERRADA");
        // Fuera de MOVING, el estado sigue al sensor (salvo FAULT, que se
        // mantiene hasta una limpieza explícita).
        if (s_state != WindowState::MOVING && s_state != WindowState::FAULT) {
            s_state = s_stableClosed ? WindowState::CLOSED : WindowState::OPEN;
        }
    }

    // 2) Fin del pulso de 5 s.
    if (s_pulseActive) {
        if ((now - s_pulseStartMs) >= WINDOW_PULSE_MS) {
            relaySetChannel(RelayChannel::VENTANA, false);
            s_pulseActive = false;
            s_moveStartMs = now;
            Serial.println("[WINDOW] Pulso finalizado; esperando transición");
        }
        return;  // durante el pulso no se evalúa timeout de viaje
    }

    // 3) Esperando que el sensor confirme el movimiento.
    if (s_state == WindowState::MOVING) {
        if (s_target == WindowTarget::OPEN && !s_stableClosed) {
            s_state = WindowState::OPEN;
            s_target = WindowTarget::NONE;
            Serial.println("[WINDOW] Confirmado ABIERTA");
        } else if (s_target == WindowTarget::CLOSE && s_stableClosed) {
            s_state = WindowState::CLOSED;
            s_target = WindowTarget::NONE;
            Serial.println("[WINDOW] Confirmada CERRADA");
        } else if ((now - s_moveStartMs) >= WINDOW_MOVE_TIMEOUT_MS) {
            s_state = WindowState::FAULT;
            s_target = WindowTarget::NONE;
            Serial.println("[WINDOW] FAULT: el sensor no cambió tras el pulso");
        }
        return;
    }

    // 4) FAULT: no se reintenta solo.
    if (s_state == WindowState::FAULT) {
        return;
    }

    // 5) Reposo: sincronizar con movimientos manuales del controlador.
    if (s_stableClosed && s_state != WindowState::CLOSED) {
        s_state = WindowState::CLOSED;
    } else if (!s_stableClosed && s_state != WindowState::OPEN) {
        s_state = WindowState::OPEN;
    }
}

bool windowRequestOpen() {
    if (s_state == WindowState::FAULT) {
        Serial.println("[WINDOW] Abrir rechazado: FAULT activo");
        return false;
    }
    if (s_state == WindowState::MOVING || s_pulseActive) {
        Serial.println("[WINDOW] Abrir rechazado: ya está en movimiento");
        return false;
    }
    if (s_state != WindowState::CLOSED) {
        Serial.println("[WINDOW] Abrir rechazado: el sensor no indica cerrada");
        return false;
    }
    return startPulse(WindowTarget::OPEN);
}

bool windowRequestClose() {
    if (s_state == WindowState::FAULT) {
        Serial.println("[WINDOW] Cerrar rechazado: FAULT activo");
        return false;
    }
    if (s_state == WindowState::MOVING || s_pulseActive) {
        Serial.println("[WINDOW] Cerrar rechazado: ya está en movimiento");
        return false;
    }
    if (s_state != WindowState::OPEN) {
        Serial.println("[WINDOW] Cerrar rechazado: el sensor indica cerrada");
        return false;
    }
    return startPulse(WindowTarget::CLOSE);
}

WindowState windowGetState() {
    return s_state;
}

WindowTarget windowGetTarget() {
    return s_target;
}

bool windowIsClosed() {
    return s_state == WindowState::CLOSED;
}

bool windowIsBusy() {
    return s_pulseActive || s_state == WindowState::MOVING;
}

uint32_t windowMinIntervalRemainingMs() {
    if (!s_hasPulsed) {
        return 0;
    }
    const uint32_t elapsed = millis() - s_lastPulseMs;
    return (elapsed >= WINDOW_MIN_INTERVAL_MS) ? 0 : (WINDOW_MIN_INTERVAL_MS - elapsed);
}

const char* windowStateName(WindowState state) {
    switch (state) {
        case WindowState::CLOSED: return "CERRADA";
        case WindowState::OPEN:   return "ABIERTA";
        case WindowState::MOVING: return "MOVIENDO";
        case WindowState::FAULT:  return "FALLA";
    }
    return "?";
}

void windowClearFault() {
    if (s_state != WindowState::FAULT) {
        return;
    }
    s_state = s_stableClosed ? WindowState::CLOSED : WindowState::OPEN;
    s_target = WindowTarget::NONE;
    Serial.printf("[WINDOW] FAULT limpiado; estado=%s\n", windowStateName(s_state));
}

void windowAbortMotion() {
    // Cortar el pulso aquí y no dejar que relayAllOff() lo haga "en seco":
    // el estado del controlador se resincroniza con el sensor en el mismo paso.
    relaySetChannel(RelayChannel::VENTANA, false);
    s_pulseActive = false;
    s_pulseStartMs = 0;
    s_moveStartMs = 0;
    s_target = WindowTarget::NONE;

    // s_lastPulseMs NO se toca: el controlador externo puede haber recibido el
    // pulso, así que el intervalo mínimo entre pulsos sigue corriendo.

    if (s_state == WindowState::FAULT) {
        // Falla real previa: no se limpia por entrar en mantenimiento.
        Serial.println("[WINDOW] Mantenimiento: pulso abortado; FAULT pendiente");
        return;
    }

    s_state = s_stableClosed ? WindowState::CLOSED : WindowState::OPEN;
    Serial.printf("[WINDOW] Mantenimiento: movimiento abortado; estado=%s\n",
                  windowStateName(s_state));
}
