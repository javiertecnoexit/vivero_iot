#include "state_machine.h"

#include <Arduino.h>

#include "buttons.h"

// ============================================================================
// state_machine.cpp — Máquina de estados Normal/Configuración (Milestone 1)
// Diseño: un solo modo activo a la vez (regla 2 de CLINE.md).
// ============================================================================

static OperationMode s_mode = OperationMode::NORMAL;

// Toda transición descarta las pulsaciones pendientes. En PORTAL y
// MANTENIMIENTO los botones no se atienden, y sin esto una pulsación hecha
// "ignorada" se ejecutaría al volver a NORMAL (p. ej. reabrir el portal
// justo al salir de él, o entrar al menú de canales).
static void changeMode(OperationMode next) {
    s_mode = next;
    buttonsClearPending();
}

void stateMachineInit() {
    s_mode = OperationMode::NORMAL;
    Serial.println("[STATE] Inicializada en modo NORMAL");
}

OperationMode stateMachineGetMode() {
    return s_mode;
}

void stateMachineEnterConfig() {
    if (s_mode != OperationMode::NORMAL) {
        return; // La transición Normal → Config solo es válida desde NORMAL
    }
    changeMode(OperationMode::CONFIG);
    Serial.println("[STATE] Transición a modo CONFIGURACION (botón Configuración)");
}

void stateMachineExitConfig() {
    if (s_mode != OperationMode::PORTAL) {
        return;
    }
    changeMode(OperationMode::NORMAL);
    Serial.println("[STATE] Transición a modo NORMAL (sin reiniciar)");
}

void stateMachineEnterMaintenance() {
    if (s_mode != OperationMode::PORTAL) {
        return;  // Solo se entra desde el portal con confirmación explícita.
    }
    changeMode(OperationMode::MANTENIMIENTO);
    Serial.println("[STATE] Transición a modo MANTENIMIENTO (relés OFF)");
}

void stateMachineExitMaintenance() {
    if (s_mode != OperationMode::MANTENIMIENTO) {
        return;
    }
    changeMode(OperationMode::NORMAL);
    Serial.println("[STATE] Transición a modo NORMAL (fin de mantenimiento)");
}

// Auto-recuperación (§4.4): la transición NORMAL → CONFIG por falla WiFi
// (o sin credenciales) la orquesta main.cpp con wifiClientTimedOut().

void stateMachineRestart() {
    Serial.println("[STATE] Reinicio solicitado (timeout o Guardar y Reiniciar)");
    ESP.restart();
}