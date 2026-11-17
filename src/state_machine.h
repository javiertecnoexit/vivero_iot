#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include "config.h"

// ============================================================================
// state_machine.h — Máquina de estados Normal/Configuración (Milestone 1)
// Fuente: especificacion_firmware.md sección 3 (modos mutuamente excluyentes).
// ============================================================================

// Inicializa la máquina de estados en modo NORMAL (arranque encendido).
void stateMachineInit();

// Devuelve el modo actual de operación.
OperationMode stateMachineGetMode();

// Transición Normal → Configuración (botón Configuración presionado).
// Solo efectiva si el modo actual es NORMAL.
void stateMachineEnterConfig();

// Transición Configuración → Normal sin reiniciar (guardar o timeout).
// Vuelve a NORMAL desde PORTAL/CONFIG.
void stateMachineExitConfig();

// Transición Portal → Mantenimiento (solo desde PORTAL). El AP y el portal
// siguen activos; la automatización y los botones quedan suspendidos.
void stateMachineEnterMaintenance();

// Transición Mantenimiento → Normal (salida explícita o timeout del portal).
void stateMachineExitMaintenance();

// Reinicia el dispositivo (solo para recuperación explícita).
void stateMachineRestart();

#endif // STATE_MACHINE_H