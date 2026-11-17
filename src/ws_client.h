#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdint.h>
#include <time.h>

#include "sensors.h"

// ============================================================================
// ws_client.h — Cliente WebSocket del backend (M6)
// Fuente: docs/02_protocolo_ws.md.
// - Claim HTTP (POST /api/v1/devices/claim) para obtener token + ws_url.
// - Sesión WebSocket con hello/welcome/config/command/request/ping/pong.
// - Telemetría en cola RAM (720 muestras), eventos en cola RAM (64).
// - Reconexión con backoff; sin conexión la automatización local sigue.
// - El portal y MANTENIMIENTO no usan la sesión: main llama a wsNotifyPortal.
// ============================================================================

// Inicializa estado, boot_id y colas. Llamar en setup() después de settings,
// alarm y localProfile.
void wsInit();

// Procesa claim, conexión, heartbeat, colas y transiciones. Llamar en el loop
// solo en modo NORMAL (el portal no mantiene la sesión).
void wsUpdate();

// Cierra la sesión (sin borrar colas) y registra PORTAL_ENTER/WS_LOST.
void wsNotifyPortalEnter();
// Registra PORTAL_EXIT y habilita un nuevo intento de conexión.
void wsNotifyPortalExit();

// Cierre inmediato sin eventos (p. ej. MANTENIMIENTO).
void wsDisconnect();

// Acumula una muestra de telemetría (la llama local_cycle cada 30 s).
void wsRecordSample(const struct tm& nowUtc, const SensorReadings& readings);

// Consultas para el portal/OLED.
bool wsIsConnected();
const char* wsStateName();
const char* wsLastError();
uint32_t wsReconnectCount();
uint16_t wsPendingSamples();
uint16_t wsPendingEvents();

// Epoch (UTC) del último frame válido del backend, o 0 si nunca hubo contacto.
// Lo usa alarm.cpp para la condición NO_CONNECTION_60MIN.
int64_t wsGetLastBackendSeenEpoch();

#endif // WS_CLIENT_H
