#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#include "alarm.h"
#include "config.h"
#include "sensors.h"
#include "window.h"

// ============================================================================
// display.h — Pantalla OLED I2C (U8g2) — Milestone 1
// Pantalla: OLED 128x64 I2C en GPIO21 (SDA) / GPIO22 (SCL).
// Fuente: CLINE.md tabla de hardware y especificacion_firmware.md sección 2.
// ============================================================================

// Inicializa la pantalla OLED.
void displayInit();

// Limpia el contenido de la pantalla.
void displayClear();

// Muestra el estado del modo Normal en 3 páginas rotativas:
//   0: aire + resumen de suelo, 1: humedad/temperatura Z1 y Z2, 2: cama y ventana.
void displayShowNormal(uint32_t cycleCount, uint8_t hour, uint8_t minute, uint8_t second,
                       const SensorReadings& readings, bool alarmActive, bool alarmSilenced,
                       AlarmCondition alarmCondition, WindowState windowState);

// Muestra el modo Portal: SSID, clave y IP del AP.
void displayShowConfig(const char* ip, const char* ssid, const char* password);

// Muestra el modo Mantenimiento: relés OFF y datos del AP.
void displayShowMaintenance(const char* ip, const char* ssid, const char* password);

// Pantalla del gesto de reset de fábrica al arrancar. Se dibuja sin throttle
// para poder mostrar la cuenta regresiva. secondsLeft < 0 oculta el contador.
void displayShowFactoryReset(const char* title, const char* detail, int secondsLeft);

// Muestra un canal del menú manual con su estado y detalle.
void displayShowChannel(const char* title, const char* state, const char* detail);

#endif // DISPLAY_H
