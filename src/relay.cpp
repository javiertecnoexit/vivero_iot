#include "relay.h"

#include <Arduino.h>

#include "config.h"
#include "settings.h"

// ============================================================================
// relay.cpp — Control de los 8 canales fijos de relé
// Fuente: docs/01_hardware_pinout.md §1 y §3, docs/03_reglas_y_plan.md.
// - Canales fijos: RIEGO_Z1, RIEGO_Z2, ASPERSION, CAMA_CALIENTE, ILUMINACION,
//   VENTILACION, VENTANA y ALARMA.
// - El estado seguro al arrancar es APAGADO en los 8 canales.
// - El canal VENTANA tiene un corte automático a WINDOW_PULSE_MS.
// ============================================================================

static const uint8_t RELAY_PINS[RELAY_CHANNEL_COUNT] = {
    PIN_RELAY_RIEGO_Z1,
    PIN_RELAY_RIEGO_Z2,
    PIN_RELAY_ASPERSION,
    PIN_RELAY_CAMA,
    PIN_RELAY_ILUMINACION,
    PIN_RELAY_VENTILACION,
    PIN_RELAY_VENTANA,
    PIN_RELAY_ALARMA
};

static const char* RELAY_NAMES[RELAY_CHANNEL_COUNT] = {
    "riego_z1",
    "riego_z2",
    "aspersion",
    "cama_caliente",
    "iluminacion",
    "ventilacion",
    "ventana",
    "alarma"
};

static const char* RELAY_LABELS[RELAY_CHANNEL_COUNT] = {
    "Riego Z1",
    "Riego Z2",
    "Aspersion",
    "Cama caliente",
    "Iluminacion",
    "Ventilacion",
    "Ventana",
    "Alarma"
};

// Estado lógico de cada canal (true = activo/encendido).
static bool s_relayState[RELAY_CHANNEL_COUNT] = { false };

// Protección del pulso de ventana: nunca dejar el relé 7 continuo.
static bool s_windowPulseActive = false;
static uint32_t s_windowPulseStartMs = 0;

// Escribe el pin según RELAY_ACTIVE_LOW.
// true  → low-level trigger: 0 V = ON, 3.3 V = OFF.
// false → high-level trigger: 3.3 V = ON, 0 V = OFF.
static void writePin(uint8_t pin, bool active) {
    const bool level = RELAY_ACTIVE_LOW ? !active : active;
    digitalWrite(pin, level ? HIGH : LOW);
}

bool relayChannelIsValid(RelayChannel channel) {
    return (uint8_t)channel < RELAY_CHANNEL_COUNT;
}

uint8_t relayChannelPin(RelayChannel channel) {
    return relayChannelIsValid(channel) ? RELAY_PINS[(uint8_t)channel] : 0xFF;
}

const char* relayChannelName(RelayChannel channel) {
    return relayChannelIsValid(channel) ? RELAY_NAMES[(uint8_t)channel] : "invalid";
}

const char* relayChannelLabel(RelayChannel channel) {
    return relayChannelIsValid(channel) ? RELAY_LABELS[(uint8_t)channel] : "Invalid";
}

void relayInit() {
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        // Estado seguro ANTES de configurar como salida, para evitar un pulso
        // durante el arranque.
        writePin(RELAY_PINS[i], false);
        pinMode(RELAY_PINS[i], OUTPUT);
        writePin(RELAY_PINS[i], false);
        s_relayState[i] = false;
    }
    s_windowPulseActive = false;
    s_windowPulseStartMs = 0;

    Serial.println("[RELAY] 8 canales inicializados en estado seguro (apagados)");
}

void relayAllOff() {
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        relaySetChannel((RelayChannel)i, false);
    }
}

bool relayAnyOn() {
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        if (s_relayState[i]) {
            return true;
        }
    }
    return false;
}

bool relaySetChannel(RelayChannel channel, bool active) {
    if (!relayChannelIsValid(channel)) {
        return false;
    }

    // Un canal deshabilitado no se comanda. Apagar siempre se permite para
    // que ninguna orden de seguridad quede bloqueada.
    if (active && !settingsIsChannelEnabled(channel)) {
        Serial.printf("[RELAY] Canal %s deshabilitado: comando ignorado\n",
                      relayChannelName(channel));
        return false;
    }

    const uint8_t index = (uint8_t)channel;
    s_relayState[index] = active;
    writePin(RELAY_PINS[index], active);

    // El canal de ventana solo admite pulsos: arrancar/limpiar el temporizador.
    if (channel == RelayChannel::VENTANA) {
        if (active) {
            s_windowPulseActive = true;
            s_windowPulseStartMs = millis();
        } else {
            s_windowPulseActive = false;
        }
    }

    return true;
}

bool relayGetChannel(RelayChannel channel) {
    if (!relayChannelIsValid(channel)) {
        return false;
    }
    return s_relayState[(uint8_t)channel];
}

void relayUpdate() {
    // Un canal deshabilitado no puede quedar encendido.
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        const RelayChannel channel = (RelayChannel)i;
        if (s_relayState[i] && !settingsIsChannelEnabled(channel)) {
            relaySetChannel(channel, false);
            Serial.printf("[RELAY] Canal %s deshabilitado → apagado\n",
                          relayChannelName(channel));
        }
    }

    if (!s_windowPulseActive) {
        return;
    }
    if ((millis() - s_windowPulseStartMs) >= WINDOW_PULSE_MS) {
        s_windowPulseActive = false;
        relaySetChannel(RelayChannel::VENTANA, false);
        Serial.println("[RELAY] Pulso de ventana finalizado (corte de seguridad)");
    }
}

// ----------------------------------------------------------------------------
// Compatibilidad temporal con la base
// ----------------------------------------------------------------------------

static bool roleToChannel(RelayRole role, uint8_t zone, RelayChannel& out) {
    switch (role) {
        case RelayRole::SUBSTRATE:
            if (zone == 0 && settingsGetSubstrateZones() >= 1) {
                out = RelayChannel::RIEGO_Z1;
                return true;
            }
            if (zone == 1 && settingsGetSubstrateZones() >= 2) {
                out = RelayChannel::RIEGO_Z2;
                return true;
            }
            return false;
        case RelayRole::SPRINKLER:
            if (zone == 0 && settingsGetSprinklerZones() >= 1) {
                out = RelayChannel::ASPERSION;
                return true;
            }
            return false;
        case RelayRole::ALARM:
            if (zone == 0) {
                out = RelayChannel::ALARMA;
                return true;
            }
            return false;
    }
    return false;
}

void setRelay(RelayRole role, uint8_t zone, bool active) {
    RelayChannel channel;
    if (!roleToChannel(role, zone, channel)) {
        return;  // rol/zona inválido o excede la configuración local
    }
    relaySetChannel(channel, active);
}

bool getRelayState(RelayRole role, uint8_t zone) {
    RelayChannel channel;
    if (!roleToChannel(role, zone, channel)) {
        return false;
    }
    return relayGetChannel(channel);
}
