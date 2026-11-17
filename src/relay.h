#ifndef RELAY_H
#define RELAY_H

#include <stdint.h>

#include "config.h"

// ============================================================================
// relay.h — Control de los 8 canales fijos de relé
// Fuente: docs/01_hardware_pinout.md §1 y §3, docs/03_reglas_y_plan.md.
// - Canales fijos en firmware: la función y el GPIO no se reconfiguran.
// - El portal solo habilita/deshabilita y prueba canales (M1).
// - Polaridad resuelta con RELAY_ACTIVE_LOW.
// - Arranque seguro: los 8 canales quedan APAGADOS.
// - Ventana: red de seguridad que corta el canal 7 a los WINDOW_PULSE_MS;
//   el control real (toggle, límites, FAULT) vive en window.cpp (M3).
// ============================================================================

// ----------------------------------------------------------------------------
// API principal — canales fijos
// ----------------------------------------------------------------------------

// Inicializa los 8 pines como salida y los deja apagados (estado seguro).
void relayInit();

// Apaga todos los canales.
void relayAllOff();

// true si algún canal lógico está encendido. Sirve para no reescribir los 8
// pines en cada pasada del loop cuando ya están apagados (modo MANTENIMIENTO).
bool relayAnyOn();

// Mantiene las protecciones de tiempo de los canales (llamar en cada loop).
void relayUpdate();

// Activa/desactiva un canal. Devuelve false si el canal es inválido.
// Para VENTANA, relayUpdate() corta el pulso automáticamente a los 5 s.
bool relaySetChannel(RelayChannel channel, bool active);

// Estado lógico actual (true = activo/encendido).
bool relayGetChannel(RelayChannel channel);

// Metadatos de canal.
bool relayChannelIsValid(RelayChannel channel);
uint8_t relayChannelPin(RelayChannel channel);
const char* relayChannelName(RelayChannel channel);
const char* relayChannelLabel(RelayChannel channel);

// ----------------------------------------------------------------------------
// Compatibilidad temporal con la base (se retira al completar M1/M3)
// ----------------------------------------------------------------------------
// Mapeo:
//   SUBSTRATE 0/1 → RIEGO_Z1/RIEGO_Z2 (limitado por settingsGetSubstrateZones)
//   SPRINKLER 0   → ASPERSION (limitado por settingsGetSprinklerZones)
//   ALARM 0       → ALARMA
enum class RelayRole : uint8_t {
    SUBSTRATE = 0,
    SPRINKLER = 1,
    ALARM     = 2
};

void setRelay(RelayRole role, uint8_t zone, bool active);
bool getRelayState(RelayRole role, uint8_t zone);

#endif // RELAY_H
