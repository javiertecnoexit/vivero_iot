#include "display.h"

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "irrigation.h"
#include "rtc.h"

// ============================================================================
// display.cpp — Pantalla OLED I2C (U8g2) — Milestone 1
// OLED 128x64 I2C (SDA GPIO21 / SCL GPIO22).
// ============================================================================

// Buffer COMPLETO (_F_) para 128x64: el modo página (_1_) solo renderiza la
// primera página si se dibuja a mano con clearBuffer()/sendBuffer (), y dejaba
// la mitad inferior de la pantalla en blanco. _F_ usa ~1 KB de SRAM (sobra).
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C s_u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_I2C_SCL, PIN_I2C_SDA);

// Throttle de refresco: ~1 Hz. Evita redibujar el OLED en cada pasada del loop
// (el loop gira muchas veces/segundo), ahorrando I2C y desgaste de la pantalla.
#define DISPLAY_REFRESH_MS 1000UL
#define DISPLAY_PAGE_MS    8000UL

static uint32_t s_lastDrawMs = 0UL;
static uint8_t  s_page = 0;
static uint32_t s_lastPageMs = 0UL;

static bool displayShouldRefresh() {
    const uint32_t now = millis();
    if (now - s_lastDrawMs < DISPLAY_REFRESH_MS) {
        return false;
    }
    s_lastDrawMs = now;
    return true;
}

void displayInit() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    s_u8g2.begin();
    s_u8g2.setFont(u8g2_font_6x10_tf);
    displayClear();
}

void displayClear() {
    s_u8g2.clearBuffer();
    s_u8g2.sendBuffer();
}

void displayShowNormal(uint32_t cycleCount, uint8_t hour, uint8_t minute, uint8_t second,
                       const SensorReadings& readings, bool alarmActive, bool alarmSilenced,
                       AlarmCondition alarmCondition, WindowState windowState) {
    if (!displayShouldRefresh()) {
        return;
    }

    // Rotación de páginas cada 8 s.
    const uint32_t now = millis();
    if ((now - s_lastPageMs) >= DISPLAY_PAGE_MS) {
        s_lastPageMs = now;
        s_page = (uint8_t)((s_page + 1) % 3);
    }

    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    char buf[40];
    const char* horaLabel = rtcIsReal() ? "" : "(sim) ";
    snprintf(buf, sizeof(buf), "NORMAL %s%02u:%02u:%02u",
             horaLabel, hour, minute, second);
    s_u8g2.drawStr(0, 10, buf);

    const AirReading& air = readings.air;
    switch (s_page) {
        case 0:
            if (air.valid) {
                snprintf(buf, sizeof(buf), "Aire: %.1fC %.0f%%", air.tempC, air.humidityPct);
            } else {
                snprintf(buf, sizeof(buf), "Aire: N/D");
            }
            s_u8g2.drawStr(0, 26, buf);
            snprintf(buf, sizeof(buf), "Suelo Z1/Z2: %.0f/%.0f %%",
                     readings.soil[0].humidityPct, readings.soil[1].humidityPct);
            s_u8g2.drawStr(0, 42, buf);
            break;

        case 1:
            if (readings.soil[0].valid) {
                snprintf(buf, sizeof(buf), "Suelo Z1: %.0f%% %.1fC",
                         readings.soil[0].humidityPct, readings.soilTemp[0].tempC);
            } else {
                snprintf(buf, sizeof(buf), "Suelo Z1: N/D");
            }
            s_u8g2.drawStr(0, 26, buf);
            if (readings.soil[1].valid) {
                snprintf(buf, sizeof(buf), "Suelo Z2: %.0f%% %.1fC",
                         readings.soil[1].humidityPct, readings.soilTemp[1].tempC);
            } else {
                snprintf(buf, sizeof(buf), "Suelo Z2: N/D");
            }
            s_u8g2.drawStr(0, 42, buf);
            break;

        default:
            if (readings.bedTemp.valid) {
                snprintf(buf, sizeof(buf), "Cama: %.1f C", readings.bedTemp.tempC);
            } else {
                snprintf(buf, sizeof(buf), "Cama: N/D");
            }
            s_u8g2.drawStr(0, 26, buf);
            snprintf(buf, sizeof(buf), "Ventana: %s", windowStateName(windowState));
            s_u8g2.drawStr(0, 42, buf);
            break;
    }

    // Alarma activa tiene prioridad sobre el pie de página.
    if (alarmActive) {
        s_u8g2.drawStr(0, 58, alarmSilenced ? "ALARMA SILENCIADA"
                                            : alarmConditionLabel(alarmCondition));
    } else {
        snprintf(buf, sizeof(buf), "Ciclo %lu  Pagina %u/3",
                 (unsigned long)cycleCount, (unsigned)(s_page + 1));
        s_u8g2.drawStr(0, 58, buf);
    }

    s_u8g2.sendBuffer();
}

// El OLED mide 128 px y la fuente usa 6 px por caracter: 21 por linea. Antes
// se formaba la cadena en un buffer de 40 B y se dibujaba entera: el corte
// ocurria en silencio y el operador no sabia que faltaba texto (clave del AP,
// SSID largo). Ahora se corta en borde de caracter UTF-8 y se marca con "...".
// Ojo: los 3 caracteres de "..." se reservan SOLO cuando el texto no cabe;
// si no, se cortaria de mas una cadena que si entraba (p. ej. "Clave: " +
// la clave derivada de la MAC = 19 de 21 caracteres).
#define OLED_LINE_CHARS 21

static void drawLineFitted(uint8_t y, const char* label, const char* value) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s%s", label, value ? value : "?");

    if (strlen(buf) > OLED_LINE_CHARS) {
        size_t cut = OLED_LINE_CHARS - 3;  // deja lugar para "..."
        // No cortar a mitad de un caracter UTF-8: retrocede hasta que el byte
        // excluido sea el inicio de un caracter (o ASCII).
        while (cut > 0 && (((uint8_t)buf[cut] & 0xC0) == 0x80)) {
            cut--;
        }
        buf[cut] = '\0';
        strncat(buf, "...", sizeof(buf) - strlen(buf) - 1);
    }
    s_u8g2.drawStr(0, y, buf);
}

void displayShowConfig(const char* ip, const char* ssid, const char* password) {
    if (!displayShouldRefresh()) {
        return;
    }
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    s_u8g2.drawStr(0, 10, "MODO: PORTAL");
    drawLineFitted(24, "SSID: ", ssid);
    drawLineFitted(38, "Clave: ", password);
    drawLineFitted(52, "IP: ", ip ? ip : "192.168.4.1");

    s_u8g2.sendBuffer();
}

void displayShowMaintenance(const char* ip, const char* ssid, const char* password) {
    if (!displayShouldRefresh()) {
        return;
    }
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    s_u8g2.drawStr(0, 10, "MANTENIMIENTO");
    s_u8g2.drawStr(0, 22, "Reles: OFF");
    drawLineFitted(34, "SSID: ", ssid);
    drawLineFitted(46, "Clave: ", password);
    drawLineFitted(58, "IP: ", ip ? ip : "192.168.4.1");

    s_u8g2.sendBuffer();
}

void displayShowChannel(const char* title, const char* state, const char* detail) {
    if (!displayShouldRefresh()) {
        return;
    }
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    s_u8g2.drawStr(0, 10, title ? title : "");
    s_u8g2.drawStr(0, 26, state ? state : "");
    s_u8g2.drawStr(0, 42, detail ? detail : "");
    s_u8g2.drawStr(0, 58, "Sel:cambia Conf:OK");

    s_u8g2.sendBuffer();
}

void displayShowFactoryReset(const char* title, const char* detail, int secondsLeft) {
    s_u8g2.clearBuffer();
    s_u8g2.setFont(u8g2_font_6x10_tf);

    s_u8g2.drawStr(0, 12, title ? title : "");
    s_u8g2.drawStr(0, 30, detail ? detail : "");
    if (secondsLeft >= 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Confirmar: %d s", secondsLeft);
        s_u8g2.drawStr(0, 50, buf);
    }

    s_u8g2.sendBuffer();
    s_lastDrawMs = millis();
}
