#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

// ============================================================================
// window.h — Control de la ventana (pulso toggle de 5 s)
// Fuente: docs/01_hardware_pinout.md §4.4 y §5, docs/03_reglas_y_plan.md §7.
// - El relé 7 entrega un pulso de 5 s a un controlador externo que ALTERNA el
//   sentido: un pulso abre, el siguiente cierra.
// - v1 tiene un solo sensor: "cerrada" (GPIO13, LOW = cerrada).
// - Estados: CLOSED, OPEN, MOVING, FAULT.
// - Para abrir se exige CLOSED; para cerrar se exige OPEN.
// - Nunca se pulsa en MOVING o FAULT. Mínimo WINDOW_MIN_INTERVAL_MS entre
//   pulsos. Si el sensor no cambia tras un pulso → FAULT (no se reintenta).
// - La recuperación de FAULT es explícita (portal / windowClearFault).
// ============================================================================

enum class WindowState : uint8_t {
    CLOSED = 0,
    OPEN   = 1,
    MOVING = 2,
    FAULT  = 3
};

enum class WindowTarget : uint8_t {
    NONE  = 0,
    OPEN  = 1,
    CLOSE = 2
};

// Inicializa el sensor y sincroniza el estado con la posición física.
void windowInit();

// Actualiza debounce, pulso y transiciones (llamar en cada loop).
void windowUpdate();

// Órdenes. Devuelven false si no se cumplen las precondiciones.
bool windowRequestOpen();   // solo desde CLOSED
bool windowRequestClose();  // solo desde OPEN

// Consultas.
WindowState windowGetState();
WindowTarget windowGetTarget();
bool windowIsClosed();
bool windowIsBusy();
const char* windowStateName(WindowState state);

// Milisegundos que faltan para poder emitir otro pulso (0 = listo).
uint32_t windowMinIntervalRemainingMs();

// Limpia un FAULT tras revisar el sensor (uso desde el portal).
void windowClearFault();

// Aborta un pulso o un viaje en curso y resincroniza con el sensor.
// Obligatorio al entrar en MANTENIMIENTO: relayAllOff() corta el pulso en seco
// y, sin esto, windowUpdate() seguiría esperando la transición del sensor y
// marcaría FAULT a los 20 s por un corte que hizo el propio firmware.
// Un FAULT preexistente no se limpia (es una falla real del sensor).
void windowAbortMotion();

#endif // WINDOW_H
