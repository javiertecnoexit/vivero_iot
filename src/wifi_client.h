#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

// ============================================================================
// wifi_client.h — Conexión WiFi en modo Normal (STA)
// Fuente: especificacion_firmware.md sección 4.4 (auto-recuperación).
// No bloqueante: WiFi.begin() retorna de inmediato y se va sondeando status()
// desde el loop con millis(). El timeout de 60 s se mide desde la última
// desconexión (no desde el arranque): una caída breve no abre el portal; una
// caída sostenida sí.
// ============================================================================

// Inicia la conexión STA con las credenciales guardadas (si las hay).
// Devuelve false si no hay credenciales guardadas (el caller decide).
bool wifiClientStart();

// Sondea el estado de la conexión. Devuelve:
//   true  → conectado (WL_CONNECTED)
//   false → aún intentando, o no iniciado
// La conexión en curso expira sola a los 60 s (wifiClientTimedOut()).
bool wifiClientUpdate();

// True si la conexión lleva más de WIFI_CONNECT_TIMEOUT_MS sin lograrse desde
// la última desconexión.
bool wifiClientTimedOut();

// True si ya hay una conexión establecida y activa.
bool wifiClientIsConnected();

// Detiene la conexión STA y apaga el radio, devolviendo el ESP32 a un estado
// limpio (usado al pasar a modo Configuración — regla 2: un modo a la vez).
void wifiClientStop();

#endif // WIFI_CLIENT_H