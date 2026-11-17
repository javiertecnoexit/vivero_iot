#include "factory_reset.h"

#include <Arduino.h>
#include <Preferences.h>

#include "calibration.h"
#include "local_profile.h"
#include "settings.h"

// ============================================================================
// factory_reset.cpp — Borrado de configuración persistida (M5)
// Orden: perfiles → calibración → settings. Los perfiles y la calibración usan
// namespaces/keys propios; settings borra el namespace "cfg" completo.
// ============================================================================

void factoryResetAll() {
    Serial.println("[RESET] Iniciando reset de fabrica...");

    localProfileFactoryReset();
    calibrationFactoryReset();
    settingsFactoryReset();

    // Historial de eventos críticos pendientes de envío.
    Preferences crit;
    if (crit.begin("ws_crit", false)) {
        crit.clear();
        crit.end();
    }

    Serial.println("[RESET] Configuracion de fabrica restaurada; reiniciar para "
                   "abrir el portal");
}

void factoryResetWifiOnly() {
    Serial.println("[RESET] Borrando solo WiFi y clave del AP...");
    settingsEraseWifi();
    Serial.println("[RESET] WiFi borrado; el resto de la configuracion se conserva");
}
