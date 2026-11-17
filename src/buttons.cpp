#include "buttons.h"

#include <Arduino.h>

#include "config.h"

// ============================================================================
// buttons.cpp — Debounce no bloqueante con millis() (regla 1 de CLINE.md)
// Esquema: lectura periódica, confirmación de flanco tras estabilidad.
// ============================================================================

#define DEBOUNCE_MS      40UL       // Ventana de estabilización del flanco
#define SAMPLE_INTERVAL  5UL        // Intervalo entre muestras de lectura

struct ButtonState {
    uint8_t pin;
    bool    stableLevel;        // Nivel estable actual (true = liberado)
    bool    pendingPress;       // Flanco de bajada confirmado sin consumir
    uint32_t lastChangeMs;      // Última vez que cambió el nivel estable
    uint32_t lastSampleMs;      // Última vez que se muestreó el pin
    uint32_t pressedAtMs;       // millis() cuando quedó presionado estable
    bool    holdReported;       // La pulsación larga ya se informó
};

static ButtonState s_btnSelector;
static ButtonState s_btnConfirm;
static ButtonState s_btnConfig;

static void initButton(ButtonState& btn, uint8_t pin, uint8_t mode) {
    btn.pin             = pin;
    btn.stableLevel     = true;   // Con pull-up/pull externo, reposo = HIGH
    btn.pendingPress    = false;
    btn.lastChangeMs    = 0;
    btn.lastSampleMs    = 0;
    btn.pressedAtMs     = 0;
    btn.holdReported    = false;
    pinMode(pin, mode);
}

void buttonsInit() {
    // GPIO25: tiene pull-up interno → INPUT_PULLUP
    initButton(s_btnSelector, PIN_BTN_SELECTOR, INPUT_PULLUP);
    // GPIO36 y GPIO39: entrada-only, sin pull interno → INPUT
    // (pull-up externo ya presente en hardware).
    initButton(s_btnConfirm, PIN_BTN_CONFIRM, INPUT);
    initButton(s_btnConfig, PIN_BTN_CONFIG, INPUT);
    Serial.println("[BTN] Botones inicializados");
}

static void updateButton(ButtonState& btn) {
    const uint32_t now = millis();
    if (now - btn.lastSampleMs < SAMPLE_INTERVAL) {
        return;
    }
    btn.lastSampleMs = now;

    const bool level = (digitalRead(btn.pin) == HIGH);

    // Si el nivel leído es estable durante DEBOUNCE_MS, se acepta como tal.
    if (level != btn.stableLevel) {
        if (now - btn.lastChangeMs >= DEBOUNCE_MS) {
            btn.stableLevel = level;
            // Flanco de bajada (HIGH → LOW) = botón presionado.
            if (!btn.stableLevel) {
                btn.pendingPress = true;
                btn.pressedAtMs = now;
                btn.holdReported = false;
            } else {
                btn.pressedAtMs = 0;
                btn.holdReported = false;
            }
        }
    } else {
        btn.lastChangeMs = now;
    }
}

void buttonsUpdate() {
    updateButton(s_btnSelector);
    updateButton(s_btnConfirm);
    updateButton(s_btnConfig);
}

bool buttonsWasPressedSelector() {
    const bool p = s_btnSelector.pendingPress;
    s_btnSelector.pendingPress = false;
    return p;
}

bool buttonsWasPressedConfirm() {
    const bool p = s_btnConfirm.pendingPress;
    s_btnConfirm.pendingPress = false;
    return p;
}

bool buttonsWasHeldConfirm(uint32_t holdMs) {
    ButtonState& btn = s_btnConfirm;
    if (btn.stableLevel || btn.holdReported || btn.pressedAtMs == 0) {
        return false;
    }
    if ((millis() - btn.pressedAtMs) >= holdMs) {
        btn.holdReported = true;
        return true;
    }
    return false;
}

bool buttonsWasPressedConfig() {
    const bool p = s_btnConfig.pendingPress;
    s_btnConfig.pendingPress = false;
    return p;
}

static void clearPending(ButtonState& btn) {
    btn.pendingPress = false;
    btn.pressedAtMs = 0;
    btn.holdReported = false;
}

void buttonsClearPending() {
    clearPending(s_btnSelector);
    clearPending(s_btnConfirm);
    clearPending(s_btnConfig);
}