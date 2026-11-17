#include "manual_ui.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "channel_control.h"
#include "climate.h"
#include "config.h"
#include "display.h"
#include "irrigation.h"
#include "relay.h"
#include "settings.h"
#include "window.h"

// ============================================================================
// manual_ui.cpp — Menú local con Selector/Confirmar
// Fuente: docs/03_reglas_y_plan.md §3.
// - Selector recorre solo canales habilitados (sin alarma).
// - Confirmar ejecuta la acción del canal mostrado.
// - Riego usa el motor de irrigation; cama/iluminación/ventilación usan un
//   temporizador local; ventana usa la máquina de estados de window.
// ============================================================================

#define NOTICE_MS 3000UL

static bool     s_selecting = false;
static bool     s_emptyMenu = false;
static RelayChannel s_selected = RelayChannel::RIEGO_Z1;
static uint32_t s_lastActivityMs = 0UL;

// Aviso breve en el OLED (motivo de un rechazo, tiempo cumplido, etc.).
static char     s_notice[40] = {0};
static bool     s_noticeActive = false;
static uint32_t s_noticeUntilMs = 0UL;

static uint8_t channelIndex(RelayChannel channel) {
    return (uint8_t)channel;
}

static uint32_t manualDurationMs(RelayChannel channel) {
    switch (channel) {
        case RelayChannel::CAMA_CALIENTE: return 1800000UL;  // 30 min
        case RelayChannel::ILUMINACION:   return 3600000UL;  // 60 min
        case RelayChannel::VENTILACION:   return 1800000UL;  // 30 min
        default:                          return 0UL;
    }
}

static bool menuEligible(RelayChannel channel) {
    if (channel == RelayChannel::ALARMA) {
        return false;
    }
    return settingsIsChannelEnabled(channel);
}

static bool firstMenuChannel(RelayChannel& out) {
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        const RelayChannel channel = (RelayChannel)i;
        if (menuEligible(channel)) {
            out = channel;
            return true;
        }
    }
    return false;
}

static bool nextMenuChannel(RelayChannel current, RelayChannel& out) {
    const uint8_t start = channelIndex(current);
    for (uint8_t step = 1; step <= RELAY_CHANNEL_COUNT; step++) {
        const uint8_t i = (uint8_t)((start + step) % RELAY_CHANNEL_COUNT);
        const RelayChannel channel = (RelayChannel)i;
        if (menuEligible(channel)) {
            out = channel;
            return true;
        }
    }
    return false;
}

static void setNotice(const char* message) {
    strncpy(s_notice, message ? message : "", sizeof(s_notice) - 1);
    s_notice[sizeof(s_notice) - 1] = '\0';
    s_noticeActive = true;
    s_noticeUntilMs = millis() + NOTICE_MS;
}

static void refreshDisplay() {
    if (s_emptyMenu) {
        displayShowChannel("Canales", "Ninguno habilitado", "Configura el portal");
        return;
    }

    char stateBuf[32];
    char detailBuf[40];
    const uint32_t now = millis();

    if (s_selected == RelayChannel::VENTANA) {
        snprintf(stateBuf, sizeof(stateBuf), "Ventana: %s", windowStateName(windowGetState()));
        if (s_noticeActive) {
            snprintf(detailBuf, sizeof(detailBuf), "%s", s_notice);
        } else if (windowIsBusy()) {
            snprintf(detailBuf, sizeof(detailBuf), "Pulso/recorrido en curso");
        } else {
            snprintf(detailBuf, sizeof(detailBuf), "Conf: abrir/cerrar");
        }
    } else {
        const bool on = relayGetChannel(s_selected);
        snprintf(stateBuf, sizeof(stateBuf), "Estado: %s", on ? "ACTIVO" : "INACTIVO");

        if (s_noticeActive) {
            snprintf(detailBuf, sizeof(detailBuf), "%s", s_notice);
        } else if (on) {
            uint32_t remainMs = 0;

            if (s_selected == RelayChannel::RIEGO_Z1) {
                const ZoneIrrigationState st = irrigationGetState(ZoneType::SUBSTRATE, 0);
                if (st.active && st.durationMs > (now - st.startedAtMs)) {
                    remainMs = st.durationMs - (now - st.startedAtMs);
                }
            } else if (s_selected == RelayChannel::RIEGO_Z2) {
                const ZoneIrrigationState st = irrigationGetState(ZoneType::SUBSTRATE, 1);
                if (st.active && st.durationMs > (now - st.startedAtMs)) {
                    remainMs = st.durationMs - (now - st.startedAtMs);
                }
            } else if (s_selected == RelayChannel::ASPERSION) {
                const ZoneIrrigationState st = irrigationGetState(ZoneType::SPRINKLER, 0);
                if (st.active && st.durationMs > (now - st.startedAtMs)) {
                    remainMs = st.durationMs - (now - st.startedAtMs);
                }
            } else {
                remainMs = channelControlRemainingMs(s_selected);
            }

            if (remainMs > 0) {
                snprintf(detailBuf, sizeof(detailBuf), "Restan: %lu s",
                         (unsigned long)((remainMs + 999UL) / 1000UL));
            } else {
                const char* src = channelControlSourceName(
                    channelControlGetSource(s_selected));
                snprintf(detailBuf, sizeof(detailBuf), "Origen: %s", src);
            }
        } else {
            snprintf(detailBuf, sizeof(detailBuf), "Conf: encender");
        }
    }

    displayShowChannel(relayChannelLabel(s_selected), stateBuf, detailBuf);
}

static void actionConfirm() {
    switch (s_selected) {
        case RelayChannel::RIEGO_Z1:
            irrigationManualToggle(ZoneType::SUBSTRATE, 0);
            break;
        case RelayChannel::RIEGO_Z2:
            irrigationManualToggle(ZoneType::SUBSTRATE, 1);
            break;
        case RelayChannel::ASPERSION:
            irrigationManualToggle(ZoneType::SPRINKLER, 0);
            break;
        case RelayChannel::CAMA_CALIENTE:
            if (!climateBedManualToggle(manualDurationMs(s_selected))) {
                setNotice("Cama: no se pudo encender");
            }
            break;
        case RelayChannel::ILUMINACION:
        case RelayChannel::VENTILACION: {
            if (channelControlIsOn(s_selected)) {
                channelControlForceOff(s_selected);
            } else if (!channelControlRequest(s_selected, true,
                                              ChannelSource::MANUAL_LOCAL,
                                              manualDurationMs(s_selected))) {
                setNotice("Canal deshabilitado");
            }
            break;
        }
        case RelayChannel::VENTANA: {
            const WindowState state = windowGetState();
            const uint32_t waitMs = windowMinIntervalRemainingMs();

            if (state == WindowState::MOVING) {
                setNotice("Ventana en movimiento");
            } else if (state == WindowState::FAULT) {
                setNotice("FALLA: revisar sensor");
            } else if (waitMs > 0) {
                char msg[40];
                snprintf(msg, sizeof(msg), "Espera %lu s entre pulsos",
                         (unsigned long)((waitMs + 999UL) / 1000UL));
                setNotice(msg);
            } else if (state == WindowState::CLOSED) {
                if (!windowRequestOpen()) {
                    setNotice("No se pudo abrir");
                }
            } else if (state == WindowState::OPEN) {
                if (!windowRequestClose()) {
                    setNotice("No se pudo cerrar");
                }
            }
            break;
        }
        case RelayChannel::ALARMA:
            break;  // la alarma no está en el menú
    }
}

void manualUiInit() {
    s_selecting = false;
    s_emptyMenu = false;
    s_selected = RelayChannel::RIEGO_Z1;
    s_lastActivityMs = 0;
    s_noticeActive = false;
    s_notice[0] = '\0';

    Serial.println("[MANUAL_UI] Menú por canal inicializado");
}

void manualUiUpdate() {
    const uint32_t now = millis();

    if (s_noticeActive && (int32_t)(now - s_noticeUntilMs) >= 0) {
        s_noticeActive = false;
    }

    // Si el canal mostrado se deshabilitó, saltar al primero disponible.
    if (s_selecting && !s_emptyMenu && !menuEligible(s_selected)) {
        RelayChannel first;
        if (firstMenuChannel(first)) {
            s_selected = first;
            s_lastActivityMs = now;
        } else {
            s_emptyMenu = true;
        }
    }

    if (s_emptyMenu) {
        if (buttonsWasPressedSelector() || buttonsWasPressedConfirm() ||
            (now - s_lastActivityMs) >= MANUAL_TIMEOUT_MS) {
            s_selecting = false;
            s_emptyMenu = false;
            Serial.println("[MANUAL_UI] Sin canales habilitados → reposo");
        } else {
            refreshDisplay();
        }
        return;
    }

    if (s_selecting) {
        if (buttonsWasPressedSelector()) {
            RelayChannel next;
            if (nextMenuChannel(s_selected, next)) {
                s_selected = next;
            }
            s_lastActivityMs = now;
            Serial.printf("[MANUAL_UI] Selector → %s\n", relayChannelName(s_selected));
        }

        if (buttonsWasPressedConfirm()) {
            actionConfirm();
            s_lastActivityMs = now;
            Serial.printf("[MANUAL_UI] Confirmar → %s\n", relayChannelName(s_selected));
        }

        if ((now - s_lastActivityMs) >= MANUAL_TIMEOUT_MS) {
            s_selecting = false;
            Serial.println("[MANUAL_UI] Timeout 5 s → reposo");
            return;
        }

        refreshDisplay();
    } else {
        if (buttonsWasPressedSelector()) {
            RelayChannel first;
            if (firstMenuChannel(first)) {
                s_selecting = true;
                s_emptyMenu = false;
                s_selected = first;
                s_lastActivityMs = now;
                Serial.printf("[MANUAL_UI] Selector → menú en %s\n",
                              relayChannelName(s_selected));
                refreshDisplay();
            } else {
                s_selecting = true;
                s_emptyMenu = true;
                s_lastActivityMs = now;
                Serial.println("[MANUAL_UI] No hay canales habilitados");
                refreshDisplay();
            }
        }
    }
}

bool manualUiIsSelecting() {
    return s_selecting;
}

RelayChannel manualUiGetSelectedChannel() {
    return s_selected;
}

void manualUiForceIdle() {
    if (s_selecting) {
        s_selecting = false;
        s_emptyMenu = false;
        s_noticeActive = false;
        Serial.println("[MANUAL_UI] Force idle");
    }
}
