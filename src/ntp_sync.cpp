#include "ntp_sync.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <time.h>

#include "rtc.h"

// ============================================================================
// ntp_sync.cpp — Sincronización de hora por NTP
// - SNTP del ESP32 (configTime) contra pool.ntp.org. No bloqueante.
// - Cuando time(nullptr) y la hora SNTP son válidas, se ajusta el DS3231 vía
//   rtcSetTime(), lo que además limpia el flag "hora perdida".
// ============================================================================

#define NTP_SERVER "pool.ntp.org"
#define NTP_POLL_INTERVAL_MS 5000UL   // cuánto esperar entre intentos SNTP

static bool s_initialized = false;
static bool s_synced = false;
static uint32_t s_lastPollMs = 0UL;

// Hora local: para este dispositivo usamos UTC (los payloads de API viajan en
// UTC ISO 8601, ver especificacion_api.md). Sin offset de zona horaria local.
#define TZ_OFFSET_SEC 0
#define TZ_DST_SEC    0

void ntpSyncInit() {
    if (s_initialized) {
        return;
    }
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, const_cast<char*>(NTP_SERVER));
    sntp_init();

    s_initialized = true;
    s_synced = false;
    s_lastPollMs = millis();
    Serial.println("[NTP] SNTP iniciado (pool.ntp.org, hora UTC)");
}

void ntpSyncUpdate() {
    if (!s_initialized || s_synced) {
        return;
    }

    const uint32_t now = millis();
    if (now - s_lastPollMs < NTP_POLL_INTERVAL_MS) {
        return;
    }
    s_lastPollMs = now;

    time_t nowSec = time(nullptr);
    // La hora SNTP es inválida hasta ser (1970) o antes de 2000.
    if (nowSec < 0 || nowSec < 946684800) {  // < 2000-01-01
        Serial.println("[NTP] Hora SNTP aún no válida, reintento en 5s");
        return;
    }

    // Convertir a UTC (gmtime) y programar el DS3231.
    struct tm utc;
    gmtime_r(&nowSec, &utc);

    // Corregir tm_wday que rtcSetTime necesita (lo deriva del tm interno).
    rtcSetTime(utc);

    // Cerrar SNTP para no seguir consumiendo.
    sntp_stop();
    s_synced = true;

    Serial.printf("[NTP] RTC sincronizado: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
}

bool ntpSyncIsSynced() {
    return s_synced;
}