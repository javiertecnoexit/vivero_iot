#include "wifi_client.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "settings.h"

// ============================================================================
// wifi_client.cpp — Conexión WiFi STA para modo Normal
// Fuente: especificacion_firmware.md sección 4.4.
// - Inicia WiFi.begin() con credenciales de NVS (settings).
// - Sondeo no bloqueante de WiFi.status() desde el loop.
// - Expira a los 60 s (WIFI_CONNECT_TIMEOUT_MS) para disparar la
//   auto-recuperación del portal (§4.4).
// ============================================================================

static bool s_started = false;
static bool s_connected = false;
static uint32_t s_disconnectedSinceMs = 0UL;

bool wifiClientStart() {
    if (!settingsHasWifiCredentials()) {
        return false;
    }

    WiFi.mode(WIFI_STA);  // un solo modo a la vez (regla 2 de CLINE.md)
    WiFi.begin(settingsGetWifiSsid(), settingsGetWifiPassword());

    s_started = true;
    s_connected = false;
    s_disconnectedSinceMs = millis();

    Serial.printf("[WIFI] Conectando a '%s'...\n", settingsGetWifiSsid());
    return true;
}

bool wifiClientUpdate() {
    if (!s_started) {
        return false;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (!s_connected) {
            s_connected = true;
            s_disconnectedSinceMs = 0;
            Serial.printf("[WIFI] Conectado (IP: %s)\n",
                          WiFi.localIP().toString().c_str());
        }
        return true;
    }

    if (s_connected || s_disconnectedSinceMs == 0) {
        s_disconnectedSinceMs = millis();
    }
    s_connected = false;
    return false;
}

bool wifiClientTimedOut() {
    if (!s_started || s_connected || s_disconnectedSinceMs == 0) {
        return false;
    }
    return (millis() - s_disconnectedSinceMs) >= WIFI_CONNECT_TIMEOUT_MS;
}

bool wifiClientIsConnected() {
    return s_connected || (WiFi.status() == WL_CONNECTED);
}

void wifiClientStop() {
    if (!s_started) {
        return;
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_started = false;
    s_connected = false;
    s_disconnectedSinceMs = 0;
    Serial.println("[WIFI] STA detenido (modo Configuración)");
}