#include "channel_control.h"

#include <Arduino.h>

#include "relay.h"
#include "settings.h"

// ============================================================================
// channel_control.cpp — Precedencia y vencimiento por canal
// - La alarma y la ventana quedan fuera: las gobiernan alarm.cpp y window.cpp.
// - El riego queda fuera: irrigation.cpp ya resuelve manual/override/auto.
// ============================================================================

struct ChannelState {
    ChannelSource source;
    uint32_t expiresAtMs;   // 0 = sin vencimiento
};

static ChannelState s_state[RELAY_CHANNEL_COUNT];

static uint8_t priorityOf(ChannelSource source) {
    switch (source) {
        case ChannelSource::TEST:          return 4;
        case ChannelSource::MANUAL_LOCAL:  return 3;
        case ChannelSource::MANUAL_REMOTE: return 2;
        case ChannelSource::AUTO:          return 1;
        case ChannelSource::NONE:          return 0;
    }
    return 0;
}

static bool isManaged(RelayChannel channel) {
    if (!relayChannelIsValid(channel)) {
        return false;
    }
    // Alarma, ventana y riego tienen controladores propios.
    if (channel == RelayChannel::ALARMA ||
        channel == RelayChannel::VENTANA ||
        channel == RelayChannel::RIEGO_Z1 ||
        channel == RelayChannel::RIEGO_Z2 ||
        channel == RelayChannel::ASPERSION) {
        return false;
    }
    return true;
}

static uint8_t indexOf(RelayChannel channel) {
    return (uint8_t)channel;
}

static void clearState(RelayChannel channel) {
    const uint8_t i = indexOf(channel);
    s_state[i].source = ChannelSource::NONE;
    s_state[i].expiresAtMs = 0;
}

void channelControlInit() {
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        s_state[i].source = ChannelSource::NONE;
        s_state[i].expiresAtMs = 0;
    }
    Serial.println("[CTRL] Control por canal inicializado");
}

bool channelControlRequest(RelayChannel channel, bool on,
                           ChannelSource source, uint32_t durationMs) {
    if (!isManaged(channel) || source == ChannelSource::NONE) {
        return false;
    }

    const uint8_t i = indexOf(channel);
    const uint8_t newPriority = priorityOf(source);
    const uint8_t currentPriority = priorityOf(s_state[i].source);

    // Una fuente de menor prioridad no puede cambiar el estado.
    if (currentPriority > newPriority) {
        return false;
    }

    if (!on) {
        if (!relaySetChannel(channel, false)) {
            return false;
        }
        clearState(channel);
        return true;
    }

    if (!settingsIsChannelEnabled(channel)) {
        return false;
    }
    if (!relaySetChannel(channel, true)) {
        return false;
    }

    s_state[i].source = source;
    s_state[i].expiresAtMs = (durationMs == 0) ? 0 : (millis() + durationMs);
    return true;
}

bool channelControlSetAuto(RelayChannel channel, bool on) {
    return channelControlRequest(channel, on, ChannelSource::AUTO, 0);
}

bool channelControlForceOff(RelayChannel channel) {
    if (!relayChannelIsValid(channel)) {
        return false;
    }
    relaySetChannel(channel, false);
    clearState(channel);
    return true;
}

void channelControlUpdate() {
    const uint32_t now = millis();

    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        const RelayChannel channel = (RelayChannel)i;
        if (!isManaged(channel)) {
            continue;
        }

        // Canal deshabilitado: apagar y limpiar la fuente.
        if (!settingsIsChannelEnabled(channel)) {
            if (s_state[i].source != ChannelSource::NONE || relayGetChannel(channel)) {
                relaySetChannel(channel, false);
                clearState(channel);
            }
            continue;
        }

        // Vencimiento de la orden.
        if (s_state[i].source != ChannelSource::NONE && s_state[i].expiresAtMs != 0 &&
            (int32_t)(now - s_state[i].expiresAtMs) >= 0) {
            Serial.printf("[CTRL] Vencimiento: %s (%s)\n",
                          relayChannelName(channel),
                          channelControlSourceName(s_state[i].source));
            relaySetChannel(channel, false);
            clearState(channel);
            continue;
        }

        // Sincronizar si el relé se apagó por otra vía.
        if (s_state[i].source != ChannelSource::NONE && !relayGetChannel(channel)) {
            clearState(channel);
        }
    }
}

bool channelControlIsOn(RelayChannel channel) {
    if (!relayChannelIsValid(channel)) {
        return false;
    }
    return relayGetChannel(channel);
}

ChannelSource channelControlGetSource(RelayChannel channel) {
    if (!relayChannelIsValid(channel)) {
        return ChannelSource::NONE;
    }
    return s_state[(uint8_t)channel].source;
}

bool channelControlIsManual(RelayChannel channel) {
    const ChannelSource source = channelControlGetSource(channel);
    return source == ChannelSource::MANUAL_LOCAL ||
           source == ChannelSource::MANUAL_REMOTE ||
           source == ChannelSource::TEST;
}

uint32_t channelControlRemainingMs(RelayChannel channel) {
    if (!relayChannelIsValid(channel)) {
        return 0;
    }
    const ChannelState& st = s_state[(uint8_t)channel];
    if (st.source == ChannelSource::NONE || st.expiresAtMs == 0) {
        return 0;
    }
    const uint32_t now = millis();
    if ((int32_t)(st.expiresAtMs - now) <= 0) {
        return 0;
    }
    return st.expiresAtMs - now;
}

const char* channelControlSourceName(ChannelSource source) {
    switch (source) {
        case ChannelSource::NONE:          return "off";
        case ChannelSource::AUTO:          return "auto";
        case ChannelSource::MANUAL_REMOTE: return "manual_remote";
        case ChannelSource::MANUAL_LOCAL:  return "manual_local";
        case ChannelSource::TEST:          return "test";
    }
    return "?";
}
