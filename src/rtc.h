#ifndef RTC_H
#define RTC_H

#include <time.h>

// ============================================================================
// rtc.h — Reloj (DS3231 real con fallback simulado)
// ----------------------------------------------------------------------------
// API ESTABLE para todos los callers: rtcNow()/rtcFormatTime().
// En esta etapa se lee el DS3231 real (RTClib) vía I2C. Si el módulo no está
// presente o falla, se cae a un reloj simulado (millis()) para que el resto
// del firmware siga funcionando sin hora correcta.
// ============================================================================

// Inicializa el reloj. Detecta el DS3231 en el bus I2C; si no responde, usa
// el fallback simulado. Devuelve true si el RTC real está operativo.
bool rtcInit();

// Hora actual en struct tm (time.h). Viene del DS3231 si está operativo,
// o del reloj simulado (millis()) en caso contrario. Siempre en UTC.
struct tm rtcNow();

// Hora actual como epoch Unix (segundos, UTC).
int64_t rtcNowEpoch();

// Hora local para pantalla y horarios: rtcNow() + offset configurado en NVS
// (settingsGetTimezoneOffsetMin(), default UTC-3). Aplica SOLO a la
// presentación/evaluación local: no modifica el RTC ni los payloads ISO 8601
// de la API, que siguen viajando en UTC.
struct tm rtcNowLocal();

// True si la hora es confiable (RTC con batería válida o sincronización
// NTP/servidor). Sin hora válida no se ejecutan horarios.
bool rtcIsTimeValid();

// Da formato HH:MM:SS al tm dado. Compatible con el buffer proporcionado.
void rtcFormatTime(const struct tm& t, char* buf, size_t bufLen);

// True si el RTC real está operativo (DS3231 detectado y respondiendo).
bool rtcIsReal();

// True si el chip de respaldo perdió la hora (batería agotada / primer
// encendido). Solo significativo si rtcIsReal().
bool rtcLostPower();

// Ajusta la hora del RTC real y del reloj simulado (sync NTP / servidor).
void rtcSetTime(const struct tm& t);

// Ajusta la hora a partir de un epoch Unix UTC.
void rtcSetEpoch(int64_t epochUtc);

// Convierte un struct tm (interpretado como UTC) a epoch Unix en segundos.
// No depende de la zona horaria del sistema ni de timegm().
int64_t rtcTmToEpoch(const struct tm& t);

#endif // RTC_H