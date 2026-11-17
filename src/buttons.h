#ifndef BUTTONS_H
#define BUTTONS_H

#include <stdint.h>

// ============================================================================
// buttons.h — Botones físicos con debounce no bloqueante (Milestone 1)
// Botones: Selector (GPIO25), Confirmar (GPIO36), Configuración (GPIO39).
// Fuente: CLINE.md tabla de hardware.
// ============================================================================

// Inicializa los pines de los botones según su tipo de pull:
//   - Configuración (GPIO39): INPUT (sin pull interno, GPIO34-39 no lo tienen;
//     pull-up externo ya presente en hardware).
//   - Confirmar (GPIO36): INPUT (ídem).
//   - Selector (GPIO25): INPUT_PULLUP (tiene pull interno).
void buttonsInit();

// Procesa el debounce no bloqueante de todos los botones. Debe llamarse
// frecuentemente desde el loop (sin delay()).
void buttonsUpdate();

// Devuelve true si el botón Selector fue presionado (flanco de bajada) desde
// la última llamada. El estado se consume al leerlo.
bool buttonsWasPressedSelector();

// Devuelve true si el botón Confirmar fue presionado (flanco de bajada) desde
// la última llamada. El estado se consume al leerlo.
bool buttonsWasPressedConfirm();

// Devuelve true UNA VEZ cuando Confirmar se mantuvo presionado al menos
// holdMs. Se usa para silenciar la alarma sin la web (3 s).
bool buttonsWasHeldConfirm(uint32_t holdMs);

// Devuelve true si el botón Configuración fue presionado (flanco de bajada)
// desde la última llamada. El estado se consume al leerlo.
bool buttonsWasPressedConfig();

// Descarta las pulsaciones pendientes sin consumirlas. Se llama en cada
// transición de modo: en MANTENIMIENTO y en PORTAL los botones no se atienden,
// y sin esto una pulsación "ignorada" se ejecutaría al volver a NORMAL (por
// ejemplo, reabrir el portal justo al salir de él).
void buttonsClearPending();

#endif // BUTTONS_H