#include "rtc.h"

#include <Arduino.h>
#include <RTClib.h>
#include <Wire.h>

#include "config.h"
#include "settings.h"

// ============================================================================
// rtc.cpp — Reloj DS3231 real (RTClib) con fallback simulado
// ----------------------------------------------------------------------------
// - Se detecta el DS3231 en el bus I2C (GPIO21/22) al arrancar.
// - Si está presente, rtcNow() devuelve la hora real.
// - Si no (módulo ausente/roto), cae al reloj simulado con millis() desde la
//   hora de referencia, igual que el rtc_sim de M1, para no romper al resto
//   del firmware (espera un wire detectado).
// - rtcSetTime() permite fijar la hora (futuro sync NTP en M4).
// ============================================================================

static RTC_DS3231 s_rtc;
static bool s_real = false;

// --- Hora inicial de compilación (para primer arranque del RTC) ---
// Parsea __DATE__ ("MMM DD YYYY") y __TIME__ ("HH:MM:SS") de C.
static const char* const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static struct tm compileTime() {
    struct tm t = {};
    char mon[4] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    int m = 0;
    for (; m < 12; m++) {
        if (strncmp(mon, MONTHS[m], 3) == 0) break;
    }
    t.tm_year = y - 1900;
    t.tm_mon  = m;
    t.tm_mday = d;
    t.tm_hour = hh;
    t.tm_min  = mm;
    t.tm_sec  = ss;
    t.tm_wday = 5;  // se corrige con settimeofday/NTP en M4
    return t;
}

// --- Estado del fallback simulado (millis()) ---
// Base configurable por rtcSetEpoch(). Arranca con la hora de compilación,
// pero se marca inválida hasta que NTP o el backend la corrijan.
static int64_t  s_simBaseEpoch = 0;
static uint32_t s_simBaseMs = 0;
static bool     s_timeValid = false;

static struct tm simulatedNow() {
    const uint32_t nowMs = millis();
    const uint32_t elapsedMs = (nowMs >= s_simBaseMs) ? (nowMs - s_simBaseMs)
                                                      : (0UL - s_simBaseMs + nowMs);
    const int64_t epoch = s_simBaseEpoch + (int64_t)(elapsedMs / 1000UL);

    const time_t sec = (time_t)epoch;
    struct tm t = {};
    gmtime_r(&sec, &t);
    return t;
}

bool rtcInit() {
    s_simBaseMs = millis();
    s_simBaseEpoch = rtcTmToEpoch(compileTime());
    s_timeValid = false;

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);  // I2C estándar, seguro para bus compartido OLED+RTC

    if (s_rtc.begin()) {
        s_real = true;
        Serial.println("[RTC] DS3231 detectado en bus I2C (hora real)");
        const bool lost = s_rtc.lostPower();
        if (lost) {
            // Primer encendido / pila vacía: el oscilador está detenido y el
            // reloj no avanza hasta el primer adjust(). Fijamos la hora de
            // compilación para arrancarlo; NTP/backend la corregirán.
            Serial.println("[RTC] Hora perdida → programando hora de compilación");
            const struct tm initT = compileTime();
            rtcSetTime(initT);
            s_timeValid = false;  // hora de compilación: no confiable
        } else {
            s_timeValid = true;
        }
        return true;
    }

    s_real = false;
    Serial.println("[RTC] DS3231 no detectado → usando reloj simulado (millis())");
    return false;
}

struct tm rtcNow() {
    if (!s_real) {
        return simulatedNow();
    }
    DateTime dt = s_rtc.now();
    struct tm t = {};
    t.tm_year = dt.year() - 1900;
    t.tm_mon  = dt.month() - 1;
    t.tm_mday = dt.day();
    t.tm_hour = dt.hour();
    t.tm_min  = dt.minute();
    t.tm_sec  = dt.second();
    t.tm_wday = dt.dayOfTheWeek();
    return t;
}

// Días desde 1970-01-01 (algoritmo de Howard Hinnant). tm se interpreta como
// UTC (el RTC guarda UTC). Evita depender de timegm(), no disponible en el
// toolchain ESP32. Devuelve el epoch Unix.
int64_t rtcTmToEpoch(const struct tm& t) {
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int d = t.tm_mday;
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;                        // [0, 399]
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400LL + (int64_t)t.tm_hour * 3600LL +
           (int64_t)t.tm_min * 60LL + (int64_t)t.tm_sec;
}

struct tm rtcNowLocal() {
    // rtcNow() devuelve UTC. Aplicamos el offset configurado (NVS) sobre el
    // epoch y volvemos a componentes UTC (que aquí representan la hora local).
    const int64_t epoch = rtcNowEpoch() +
                          (int64_t)settingsGetTimezoneOffsetMin() * 60LL;
    const time_t sec = (time_t)epoch;
    struct tm local;
    gmtime_r(&sec, &local);
    return local;
}

int64_t rtcNowEpoch() {
    return rtcTmToEpoch(rtcNow());
}

bool rtcIsTimeValid() {
    return s_timeValid;
}

void rtcFormatTime(const struct tm& t, char* buf, size_t bufLen) {
    snprintf(buf, bufLen, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
}

bool rtcIsReal() {
    return s_real;
}

bool rtcLostPower() {
    if (!s_real) return false;
    return s_rtc.lostPower();
}

void rtcSetTime(const struct tm& t) {
    if (!s_real) {
        s_simBaseEpoch = rtcTmToEpoch(t);
        s_simBaseMs = millis();
        s_timeValid = true;
        Serial.printf("[RTC] Reloj simulado ajustado a %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
        return;
    }
    DateTime dt(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    s_rtc.adjust(dt);
    s_timeValid = true;
    Serial.printf("[RTC] Hora ajustada a %04d-%02d-%02d %02d:%02d:%02d\n",
                  dt.year(), dt.month(), dt.day(), dt.hour(), dt.minute(), dt.second());
}

void rtcSetEpoch(int64_t epochUtc) {
    const time_t sec = (time_t)epochUtc;
    struct tm utc = {};
    gmtime_r(&sec, &utc);
    rtcSetTime(utc);
}