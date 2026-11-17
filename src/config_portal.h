#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include <stdint.h>

// ============================================================================
// config_portal.h — Esqueleto del portal local (Milestone 1)
// Fuente: especificacion_firmware.md sección 4.
// Levanta AP + servidor web con una ruta raíz placeholder y timeout por
// inactividad de 15 min. El portal completo (formularios, calibración) llega
// en un milestone posterior.
// ============================================================================

// Levanta el AP y el servidor web del portal. Debe llamarse al entrar en modo
// Configuración.
void configPortalStart();

// Detiene el AP y el servidor web. Debe llamarse al salir de modo Configuración.
void configPortalStop();

// Procesa el servidor web (atender requests) y revisa el timeout de
// inactividad. Debe llamarse frecuentemente desde el loop mientras el modo
// Configuración esté activo.
void configPortalUpdate();

// Devuelve true si el timeout de inactividad de 15 min se ha excedido y el
// dispositivo debe volver a modo Normal (reiniciar).
bool configPortalIsTimeout();

// Devuelve la IP del AP del portal. Cadena vacía si el portal no está corriendo.
const char* configPortalGetIp();

// SSID y clave WPA2 del AP (para mostrarlos en la OLED).
const char* configPortalGetSsid();
const char* configPortalGetPassword();

// True mientras hay una prueba de canal en curso. En MANTENIMIENTO el loop
// principal no fuerza relés OFF durante la prueba (auto-off del portal).
bool configPortalTestActive();

#endif // CONFIG_PORTAL_H