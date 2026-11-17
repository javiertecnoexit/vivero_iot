#ifndef CHANNEL_CONTROL_H
#define CHANNEL_CONTROL_H

#include <stdint.h>

#include "config.h"

// ============================================================================
// channel_control.h — Precedencia y vencimiento por canal
// Fuente: docs/03_reglas_y_plan.md §1.1 y §3.3.
// - Prioridad: manual local > manual remoto > automático.
// - Una orden manual con duración vence sola y devuelve el canal a AUTO/NONE.
// - La automatización (setAuto) no puede pisar una orden manual vigente.
// - No gobierna alarma ni ventana (tienen sus propios controladores).
// - El riego (Z1/Z2/aspersión) sigue en irrigation.cpp, que ya aplica su
//   propia precedencia.
// ============================================================================

enum class ChannelSource : uint8_t {
    NONE          = 0,
    AUTO          = 1,
    MANUAL_REMOTE = 2,
    MANUAL_LOCAL  = 3,
    TEST          = 4
};

void channelControlInit();

// Solicita encender/apagar con una fuente. durationMs=0 → sin vencimiento.
// Devuelve false si la fuente no tiene prioridad, el canal no existe o está
// deshabilitado.
bool channelControlRequest(RelayChannel channel, bool on,
                           ChannelSource source, uint32_t durationMs);

// Atajo para la automatización (solo aplica si no hay manual vigente).
bool channelControlSetAuto(RelayChannel channel, bool on);

// Apagado forzado por seguridad (ignora prioridad y vencimiento).
bool channelControlForceOff(RelayChannel channel);

// Vence órdenes temporizadas y sincroniza canales deshabilitados.
void channelControlUpdate();

// Consultas.
bool channelControlIsOn(RelayChannel channel);
bool channelControlIsManual(RelayChannel channel);
ChannelSource channelControlGetSource(RelayChannel channel);
uint32_t channelControlRemainingMs(RelayChannel channel);
const char* channelControlSourceName(ChannelSource source);

#endif // CHANNEL_CONTROL_H
