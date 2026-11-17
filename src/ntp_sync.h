#ifndef NTP_SYNC_H
#define NTP_SYNC_H

#include <time.h>

// ============================================================================
// ntp_sync.h — Sincronización de hora por NTP (Milestone: capa de red)
// Fuente: especificacion_tecnica_4.md sección 7 (punto 179) y
//         especificacion_firmware.md (mantener hora, resincronizar por NTP).
// - Usa SNTP del framework (configTime), no bloqueante.
// - Una vez que el RTC local quedó sincronizado (rtcSetTime), se marca hecho.
// ============================================================================

// Inicia el proceso SNTP (configTime) con el server por defecto. No bloquea.
void ntpSyncInit();

// Debe llamarse desde el loop cuando hay WiFi conectado. Internamente hace
// polling de la hora SNTP hasta que está válida, y luego la escribe al RTC
// (rtcSetTime). No bloquea (retorna rápido).
void ntpSyncUpdate();

// True si la hora ya fue sincronizada con éxito en esta sesión.
bool ntpSyncIsSynced();

#endif // NTP_SYNC_H