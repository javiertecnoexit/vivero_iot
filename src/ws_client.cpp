#include "ws_client.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <esp_system.h>

#include "alarm.h"
#include "channel_control.h"
#include "climate.h"
#include "config.h"
#include "irrigation.h"
#include "local_profile.h"
#include "relay.h"
#include "rtc.h"
#include "settings.h"
#include "state_machine.h"
#include "window.h"

// ============================================================================
// ws_client.cpp — Cliente WebSocket del backend (M6)
// Fuente: docs/02_protocolo_ws.md.
// ----------------------------------------------------------------------------
// Este módulo NO gobierna la automatización: solo reporta y ejecuta comandos
// remotos con las mismas reglas locales (canal habilitado, interlocks,
// precedencia y seguridad). Si no hay red, el firmware sigue operando.
// ============================================================================

namespace {

// ----------------------------------------------------------------------------
// Estado de enlace
// ----------------------------------------------------------------------------

enum class LinkState : uint8_t {
    IDLE = 0,
    CLAIMING,
    CONNECTING,
    CONNECTED,
    BACKOFF
};

WebSocketsClient s_ws;
LinkState s_state = LinkState::IDLE;
bool s_closingSession = false;
bool s_netWasConnected = false;

// La librería reconecta sola cada _reconnectInterval; el estado BACKOFF del
// firmware ya programa los reintentos. Se desactiva la reconexión automática
// (valor muy alto) y se vuelve a 0 justo antes de begin().
constexpr unsigned long WS_AUTO_RECONNECT_OFF_MS = 24UL * 60UL * 60UL * 1000UL;

char s_bootId[9] = {0};
char s_sessionId[40] = {0};
uint32_t s_seq = 0;
uint32_t s_eventSeq = 0;
uint32_t s_backoffMs = 0;
uint32_t s_nextActionMs = 0;
uint32_t s_connectDeadlineMs = 0;
uint32_t s_lastRxMs = 0;
uint32_t s_lastTelemetryTxMs = 0;
uint32_t s_lastEventTxMs = 0;
uint32_t s_lastStateMs = 0;
uint32_t s_lastStateSig = 0;
bool s_haveStateSig = false;
uint16_t s_heartbeatS = WS_HEARTBEAT_DEFAULT_S;
uint16_t s_telemetryS = WS_TELEMETRY_DEFAULT_S;
uint32_t s_reconnects = 0;
uint32_t s_connectedSinceMs = 0;
uint32_t s_nextClaimMs = 0;
int64_t s_lastBackendSeenEpoch = 0;
bool s_claimNoCodeLogged = false;
bool s_noBackendLogged = false;
char s_lastError[80] = {0};

// ----------------------------------------------------------------------------
// Utilidades de tiempo
// ----------------------------------------------------------------------------

bool isoFromEpoch(int64_t epoch, char* buf, size_t len) {
    if (epoch <= 0) {
        return false;
    }
    const time_t sec = (time_t)epoch;
    struct tm t = {};
    gmtime_r(&sec, &t);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

bool parseIso(const char* text, int64_t& out) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d",
               &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }
    if (year < 2020 || year > 2100 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = second;
    out = rtcTmToEpoch(t);
    return out > 0;
}

const char* nowIsoOrNull(char* buf, size_t len) {
    if (!rtcIsTimeValid()) {
        return nullptr;
    }
    return isoFromEpoch(rtcNowEpoch(), buf, len) ? buf : nullptr;
}

// ----------------------------------------------------------------------------
// Envoltorio JSON
// ----------------------------------------------------------------------------

JsonObject beginEnvelope(JsonDocument& doc, const char* type, char* idBuf,
                         size_t idLen) {
    doc.clear();
    doc["v"] = 1;
    doc["type"] = type;
    snprintf(idBuf, idLen, "m-%06lu", (unsigned long)(++s_seq));
    doc["id"] = idBuf;
    char ts[24];
    const char* iso = nowIsoOrNull(ts, sizeof(ts));
    if (iso != nullptr) {
        doc["ts"] = iso;
    } else {
        doc["ts"] = nullptr;
    }
    return doc["payload"].to<JsonObject>();
}

bool sendDoc(JsonDocument& doc) {
    if (!s_ws.isConnected()) {
        return false;
    }
    String out;
    out.reserve(512);
    serializeJson(doc, out);
    const bool ok = s_ws.sendTXT(out.c_str(), out.length());
    if (!ok) {
        Serial.println("[WS] ERROR enviando frame");
    }
    return ok;
}

void sendError(const char* code, const char* message, const char* ref = nullptr,
               bool fatal = false) {
    JsonDocument doc;
    char id[16];
    JsonObject p = beginEnvelope(doc, "error", id, sizeof(id));
    p["code"] = code;
    p["message"] = message;
    if (ref != nullptr && ref[0] != '\0') {
        p["ref"] = ref;
    } else {
        p["ref"] = nullptr;
    }
    p["fatal"] = fatal;
    sendDoc(doc);
}

// ----------------------------------------------------------------------------
// Eventos
// ----------------------------------------------------------------------------

enum class EvCode : uint8_t {
    BOOT = 0,
    WIFI_CONNECTED,
    WIFI_LOST,
    WS_CONNECTED,
    WS_LOST,
    PORTAL_ENTER,
    PORTAL_EXIT,
    CONFIG_APPLIED,
    CONFIG_REJECTED,
    TIME_SYNCED,
    IRRIGATION_START,
    IRRIGATION_STOP,
    THERMOSTAT_ON,
    THERMOSTAT_OFF,
    LIGHT_ON,
    LIGHT_OFF,
    FAN_ON,
    FAN_OFF,
    WINDOW_OPEN,
    WINDOW_CLOSE,
    WINDOW_FAULT,
    ALARM_RAISED,
    ALARM_CLEARED,
    ALARM_SILENCED,
    COMMAND_REJECTED,
    NVS_ERROR,
    WATCHDOG_RESET
};

struct WsEvent {
    EvCode   code;
    int64_t  epoch;
    uint32_t seq;
    uint32_t ringSeq;   // 0 = no persistido en el ring NVS
    char     bootId[9];
    char     channel[20];
    char     reason[24];
    char     rule[24];
    bool     critical;
    bool     buffered;
    bool     timeValid;
};

static WsEvent s_events[WS_EVENT_QUEUE];
static uint16_t s_eventCount = 0;

const char* eventCodeName(EvCode code) {
    switch (code) {
        case EvCode::BOOT:              return "BOOT";
        case EvCode::WIFI_CONNECTED:    return "WIFI_CONNECTED";
        case EvCode::WIFI_LOST:         return "WIFI_LOST";
        case EvCode::WS_CONNECTED:      return "WS_CONNECTED";
        case EvCode::WS_LOST:           return "WS_LOST";
        case EvCode::PORTAL_ENTER:      return "PORTAL_ENTER";
        case EvCode::PORTAL_EXIT:       return "PORTAL_EXIT";
        case EvCode::CONFIG_APPLIED:    return "CONFIG_APPLIED";
        case EvCode::CONFIG_REJECTED:   return "CONFIG_REJECTED";
        case EvCode::TIME_SYNCED:       return "TIME_SYNCED";
        case EvCode::IRRIGATION_START:  return "IRRIGATION_START";
        case EvCode::IRRIGATION_STOP:   return "IRRIGATION_STOP";
        case EvCode::THERMOSTAT_ON:     return "THERMOSTAT_ON";
        case EvCode::THERMOSTAT_OFF:    return "THERMOSTAT_OFF";
        case EvCode::LIGHT_ON:          return "LIGHT_ON";
        case EvCode::LIGHT_OFF:         return "LIGHT_OFF";
        case EvCode::FAN_ON:            return "FAN_ON";
        case EvCode::FAN_OFF:           return "FAN_OFF";
        case EvCode::WINDOW_OPEN:       return "WINDOW_OPEN";
        case EvCode::WINDOW_CLOSE:      return "WINDOW_CLOSE";
        case EvCode::WINDOW_FAULT:      return "WINDOW_FAULT";
        case EvCode::ALARM_RAISED:      return "ALARM_RAISED";
        case EvCode::ALARM_CLEARED:     return "ALARM_CLEARED";
        case EvCode::ALARM_SILENCED:    return "ALARM_SILENCED";
        case EvCode::COMMAND_REJECTED:  return "COMMAND_REJECTED";
        case EvCode::NVS_ERROR:         return "NVS_ERROR";
        case EvCode::WATCHDOG_RESET:    return "WATCHDOG_RESET";
    }
    return "UNKNOWN";
}

const char* eventMessage(EvCode code) {
    switch (code) {
        case EvCode::BOOT:              return "Equipo iniciado";
        case EvCode::WIFI_CONNECTED:    return "WiFi conectado";
        case EvCode::WIFI_LOST:         return "WiFi perdido";
        case EvCode::WS_CONNECTED:      return "WebSocket conectado";
        case EvCode::WS_LOST:           return "WebSocket perdido";
        case EvCode::PORTAL_ENTER:      return "Entrada al portal cautivo";
        case EvCode::PORTAL_EXIT:       return "Salida del portal cautivo";
        case EvCode::CONFIG_APPLIED:    return "Configuracion del backend aplicada";
        case EvCode::CONFIG_REJECTED:   return "Configuracion del backend rechazada";
        case EvCode::TIME_SYNCED:       return "Hora sincronizada con el backend";
        case EvCode::IRRIGATION_START:  return "Riego iniciado";
        case EvCode::IRRIGATION_STOP:   return "Riego detenido";
        case EvCode::THERMOSTAT_ON:     return "Cama caliente encendida";
        case EvCode::THERMOSTAT_OFF:    return "Cama caliente apagada";
        case EvCode::LIGHT_ON:          return "Iluminacion encendida";
        case EvCode::LIGHT_OFF:         return "Iluminacion apagada";
        case EvCode::FAN_ON:            return "Ventilacion encendida";
        case EvCode::FAN_OFF:           return "Ventilacion apagada";
        case EvCode::WINDOW_OPEN:       return "Ventana abierta";
        case EvCode::WINDOW_CLOSE:      return "Ventana cerrada";
        case EvCode::WINDOW_FAULT:      return "Falla de ventana";
        case EvCode::ALARM_RAISED:      return "Alarma activada";
        case EvCode::ALARM_CLEARED:     return "Alarma normalizada";
        case EvCode::ALARM_SILENCED:    return "Alarma silenciada";
        case EvCode::COMMAND_REJECTED:  return "Comando remoto rechazado";
        case EvCode::NVS_ERROR:         return "Error de persistencia NVS";
        case EvCode::WATCHDOG_RESET:    return "Reinicio por watchdog";
    }
    return "";
}

int eventSeverity(EvCode code, bool critical) {
    switch (code) {
        case EvCode::CONFIG_REJECTED:
            return 2;  // error
        case EvCode::NVS_ERROR:
            return 2;  // error
        case EvCode::WATCHDOG_RESET:
            return 2;  // error
        case EvCode::WIFI_LOST:
        case EvCode::WS_LOST:
        case EvCode::WINDOW_FAULT:
        case EvCode::COMMAND_REJECTED:
            return 1;  // warning
        case EvCode::ALARM_RAISED:
            return critical ? 3 : 1;
        default:
            return 0;  // info
    }
}

const char* severityName(int severity) {
    switch (severity) {
        case 1: return "warning";
        case 2: return "error";
        case 3: return "critical";
        default: return "info";
    }
}

// ----------------------------------------------------------------------------
// Ring NVS de eventos críticos
// ----------------------------------------------------------------------------
// Los eventos críticos se persisten para sobrevivir un corte de energía. El
// ring es de tamaño fijo: al llenarse, se sobrescribe el más antiguo y se
// informa NVS_ERROR una sola vez por arranque. Al confirmarse el envío por
// WebSocket se libera la ranura; si el equipo reinicia antes, el evento se
// reenvía y el backend lo deduplica por event_id.

constexpr uint16_t CRIT_RING_MAGIC = 0x5753;  // "WS"
constexpr uint16_t CRIT_RING_VERSION = 1;
constexpr const char* CRIT_RING_NAMESPACE = "ws_crit";
constexpr const char* CRIT_RING_KEY = "ring";

struct CriticalEvent {
    uint32_t writeSeq;
    EvCode   code;
    int64_t  epoch;
    uint32_t seq;
    char     bootId[9];
    char     channel[20];
    char     reason[24];
    char     rule[24];
    bool     timeValid;
    bool     used;
};

struct CriticalRing {
    uint16_t magic;
    uint16_t version;
    uint32_t writeSeq;
    CriticalEvent slots[WS_CRITICAL_EVENT_QUEUE];
};

static CriticalRing s_critRing = {};
static bool s_nvsErrorQueued = false;

void copyField(char* dst, size_t len, const char* src) {
    if (dst == nullptr || len == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

bool criticalRingSave() {
    Preferences prefs;
    if (!prefs.begin(CRIT_RING_NAMESPACE, false)) {
        return false;
    }
    const size_t written = prefs.putBytes(CRIT_RING_KEY, &s_critRing,
                                          sizeof(s_critRing));
    prefs.end();
    return written == sizeof(s_critRing);
}

void criticalRingLoad() {
    memset(&s_critRing, 0, sizeof(s_critRing));
    s_critRing.magic = CRIT_RING_MAGIC;
    s_critRing.version = CRIT_RING_VERSION;

    Preferences prefs;
    if (!prefs.begin(CRIT_RING_NAMESPACE, true)) {
        return;
    }
    const size_t len = prefs.getBytesLength(CRIT_RING_KEY);
    if (len == sizeof(s_critRing)) {
        CriticalRing tmp = {};
        if (prefs.getBytes(CRIT_RING_KEY, &tmp, sizeof(tmp)) == sizeof(tmp) &&
            tmp.magic == CRIT_RING_MAGIC && tmp.version == CRIT_RING_VERSION) {
            s_critRing = tmp;
        }
    }
    prefs.end();
}

uint32_t criticalRingAppend(const WsEvent& evt, bool& evicted) {
    evicted = false;
    CriticalEvent* slot = nullptr;
    for (uint8_t i = 0; i < WS_CRITICAL_EVENT_QUEUE; i++) {
        if (!s_critRing.slots[i].used) {
            slot = &s_critRing.slots[i];
            break;
        }
    }
    if (slot == nullptr) {
        slot = &s_critRing.slots[0];
        for (uint8_t i = 1; i < WS_CRITICAL_EVENT_QUEUE; i++) {
            if (s_critRing.slots[i].writeSeq < slot->writeSeq) {
                slot = &s_critRing.slots[i];
            }
        }
        evicted = true;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->writeSeq = ++s_critRing.writeSeq;
    slot->code = evt.code;
    slot->epoch = evt.epoch;
    slot->seq = evt.seq;
    slot->timeValid = evt.timeValid;
    copyField(slot->bootId, sizeof(slot->bootId), evt.bootId);
    copyField(slot->channel, sizeof(slot->channel), evt.channel);
    copyField(slot->reason, sizeof(slot->reason), evt.reason);
    copyField(slot->rule, sizeof(slot->rule), evt.rule);

    if (!criticalRingSave()) {
        Serial.println("[WS] No se pudo guardar el ring de eventos criticos");
        return 0;
    }
    return slot->writeSeq;
}

void criticalRingRemove(uint32_t ringSeq) {
    if (ringSeq == 0) {
        return;
    }
    bool found = false;
    for (uint8_t i = 0; i < WS_CRITICAL_EVENT_QUEUE; i++) {
        if (s_critRing.slots[i].used &&
            s_critRing.slots[i].writeSeq == ringSeq) {
            s_critRing.slots[i].used = false;
            found = true;
            break;
        }
    }
    if (found && !criticalRingSave()) {
        Serial.println("[WS] No se pudo actualizar el ring de eventos criticos");
    }
}

void criticalRingRestore() {
    bool taken[WS_CRITICAL_EVENT_QUEUE] = {false};
    for (uint8_t n = 0; n < WS_CRITICAL_EVENT_QUEUE; n++) {
        int best = -1;
        for (uint8_t i = 0; i < WS_CRITICAL_EVENT_QUEUE; i++) {
            if (!s_critRing.slots[i].used || taken[i] ||
                s_critRing.slots[i].writeSeq == 0) {
                continue;
            }
            if (best < 0 ||
                s_critRing.slots[i].writeSeq < s_critRing.slots[best].writeSeq) {
                best = i;
            }
        }
        if (best < 0 || s_eventCount >= WS_EVENT_QUEUE) {
            break;
        }
        taken[best] = true;
        const CriticalEvent& src = s_critRing.slots[best];
        WsEvent& evt = s_events[s_eventCount++];
        evt = {};
        evt.code = src.code;
        evt.epoch = src.epoch;
        evt.seq = src.seq;
        evt.ringSeq = src.writeSeq;
        evt.critical = true;
        evt.buffered = true;
        evt.timeValid = src.timeValid;
        copyField(evt.bootId, sizeof(evt.bootId), src.bootId);
        copyField(evt.channel, sizeof(evt.channel), src.channel);
        copyField(evt.reason, sizeof(evt.reason), src.reason);
        copyField(evt.rule, sizeof(evt.rule), src.rule);
        Serial.printf("[WS] Evento critico restaurado: %s-%04lu\n",
                      evt.bootId[0] ? evt.bootId : "?",
                      (unsigned long)evt.seq);
    }
}

void queueEvent(EvCode code, const char* channel = nullptr,
                const char* reason = nullptr, const char* rule = nullptr,
                bool critical = false, bool persist = true) {
    if (s_eventCount >= WS_EVENT_QUEUE) {
        // Descartar el más antiguo no crítico; si todos son críticos, el más
        // antiguo igual. Los críticos siguen en el ring NVS.
        int drop = -1;
        for (uint16_t i = 0; i < s_eventCount; i++) {
            if (!s_events[i].critical) {
                drop = i;
                break;
            }
        }
        if (drop < 0) {
            drop = 0;
        }
        for (uint16_t i = drop; i + 1 < s_eventCount; i++) {
            s_events[i] = s_events[i + 1];
        }
        s_eventCount--;
    }

    WsEvent& evt = s_events[s_eventCount++];
    evt = {};
    evt.code = code;
    evt.epoch = rtcNowEpoch();
    evt.timeValid = rtcIsTimeValid();
    evt.seq = ++s_eventSeq;
    evt.critical = critical;
    evt.buffered = !s_ws.isConnected();
    copyField(evt.bootId, sizeof(evt.bootId), s_bootId);
    copyField(evt.channel, sizeof(evt.channel), channel);
    copyField(evt.reason, sizeof(evt.reason), reason);
    copyField(evt.rule, sizeof(evt.rule), rule);

    if (critical && persist) {
        bool evicted = false;
        const uint32_t ringSeq = criticalRingAppend(evt, evicted);
        if (ringSeq != 0) {
            evt.ringSeq = ringSeq;
        }
        if ((ringSeq == 0 || evicted) && !s_nvsErrorQueued) {
            s_nvsErrorQueued = true;
            queueEvent(EvCode::NVS_ERROR, "ws",
                       ringSeq == 0 ? "nvs_write" : "nvs_overflow",
                       nullptr, false, false);
        }
    }
}

bool sendOneEvent() {
    if (s_eventCount == 0) {
        return false;
    }
    const WsEvent& evt = s_events[0];
    const char* evtBootId = (evt.bootId[0] != '\0') ? evt.bootId : s_bootId;

    JsonDocument doc;
    char id[16];
    JsonObject p = beginEnvelope(doc, "event", id, sizeof(id));
    char eventId[32];
    snprintf(eventId, sizeof(eventId), "%s-%04lu", evtBootId,
             (unsigned long)evt.seq);
    p["event_id"] = eventId;
    char eventTs[24];
    if (evt.timeValid && isoFromEpoch(evt.epoch, eventTs, sizeof(eventTs))) {
        doc["ts"] = eventTs;
    } else {
        doc["ts"] = nullptr;
    }

    const int severity = eventSeverity(evt.code, evt.critical);
    p["code"] = eventCodeName(evt.code);
    p["severity"] = severityName(severity);
    if (evt.channel[0] != '\0') {
        p["channel"] = evt.channel;
    } else {
        p["channel"] = nullptr;
    }
    p["message"] = eventMessage(evt.code);
    JsonObject data = p["data"].to<JsonObject>();
    if (evt.reason[0] != '\0') {
        data["reason"] = evt.reason;
    }
    if (evt.rule[0] != '\0') {
        data["rule_id"] = evt.rule;
    }
    data["boot_id"] = evtBootId;
    p["buffered"] = evt.buffered;

    if (!sendDoc(doc)) {
        return false;
    }

    if (evt.ringSeq != 0) {
        criticalRingRemove(evt.ringSeq);
    }

    for (uint16_t i = 0; i + 1 < s_eventCount; i++) {
        s_events[i] = s_events[i + 1];
    }
    s_eventCount--;
    return true;
}

// ----------------------------------------------------------------------------
// Telemetría
// ----------------------------------------------------------------------------

struct Sample {
    int64_t  epoch;
    uint32_t seq;
    uint32_t uptimeS;
    float    airTempC;
    float    airRhPct;
    float    soilMoistPct[MAX_SUBSTRATE_ZONES];
    float    soilTempC[MAX_SUBSTRATE_ZONES];
    float    bedTempC;
    int16_t  rssi;
    bool     airValid;
    bool     soilValid[MAX_SUBSTRATE_ZONES];
    bool     bedValid;
    uint8_t  windowState;
    bool     timeValid;
};

static Sample s_tel[WS_TELEMETRY_QUEUE];
static uint16_t s_telHead = 0;
static uint16_t s_telCount = 0;
static uint32_t s_sampleSeq = 0;

void pushSample(const Sample& sample) {
    if (s_telCount >= WS_TELEMETRY_QUEUE) {
        s_telHead = (uint16_t)((s_telHead + 1) % WS_TELEMETRY_QUEUE);
        s_telCount--;
    }
    const uint16_t slot = (uint16_t)((s_telHead + s_telCount) % WS_TELEMETRY_QUEUE);
    s_tel[slot] = sample;
    s_telCount++;
}

void popSamples(uint16_t count) {
    for (uint16_t i = 0; i < count && s_telCount > 0; i++) {
        s_telHead = (uint16_t)((s_telHead + 1) % WS_TELEMETRY_QUEUE);
        s_telCount--;
    }
}

const char* windowStateCode(WindowState state) {
    switch (state) {
        case WindowState::CLOSED: return "closed";
        case WindowState::OPEN:   return "open";
        case WindowState::MOVING: return "moving";
        case WindowState::FAULT:  return "fault";
    }
    return "unknown";
}

void addSampleJson(JsonArray& samples, const Sample& sample) {
    JsonObject s = samples.add<JsonObject>();
    if (sample.timeValid) {
        char ts[24];
        if (isoFromEpoch(sample.epoch, ts, sizeof(ts))) {
            s["ts"] = ts;
        } else {
            s["ts"] = nullptr;
        }
    } else {
        s["ts"] = nullptr;
    }

    JsonObject air = s["air"].to<JsonObject>();
    if (sample.airValid) {
        air["temp_c"] = sample.airTempC;
        air["rh_pct"] = sample.airRhPct;
    } else {
        air["temp_c"] = nullptr;
        air["rh_pct"] = nullptr;
    }
    air["valid"] = sample.airValid;

    JsonObject substrate = s["substrate"].to<JsonObject>();
    const char* zoneNames[MAX_SUBSTRATE_ZONES] = { "z1", "z2" };
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        JsonObject zObj = substrate[zoneNames[z]].to<JsonObject>();
        if (sample.soilValid[z]) {
            zObj["moisture_pct"] = sample.soilMoistPct[z];
            zObj["temp_c"] = sample.soilTempC[z];
        } else {
            zObj["moisture_pct"] = nullptr;
            zObj["temp_c"] = nullptr;
        }
        zObj["valid"] = sample.soilValid[z];
    }

    JsonObject bed = s["bed"].to<JsonObject>();
    if (sample.bedValid) {
        bed["temp_c"] = sample.bedTempC;
    } else {
        bed["temp_c"] = nullptr;
    }
    bed["valid"] = sample.bedValid;

    JsonObject win = s["window"].to<JsonObject>();
    const WindowState ws = (WindowState)sample.windowState;
    win["state"] = windowStateCode(ws);
    win["limit_closed"] = (ws == WindowState::CLOSED);
    win["moving"] = (ws == WindowState::MOVING);
    win["direction"] = nullptr;

    s["rssi_dbm"] = sample.rssi;
    s["uptime_s"] = sample.uptimeS;
    s["time_valid"] = sample.timeValid;
}

bool sendTelemetryBatch() {
    if (s_telCount == 0) {
        return false;
    }
    const uint16_t batch = (s_telCount > WS_TELEMETRY_BATCH)
                               ? WS_TELEMETRY_BATCH : s_telCount;
    const bool buffered = (s_telCount > WS_TELEMETRY_BATCH);

    JsonDocument doc;
    char id[16];
    JsonObject p = beginEnvelope(doc, "telemetry", id, sizeof(id));
    p["boot_id"] = s_bootId;
    p["seq"] = (s_tel[(s_telHead + batch - 1) % WS_TELEMETRY_QUEUE]).seq;
    p["buffered"] = buffered;
    JsonArray samples = p["samples"].to<JsonArray>();
    for (uint16_t i = 0; i < batch; i++) {
        const uint16_t slot = (uint16_t)((s_telHead + i) % WS_TELEMETRY_QUEUE);
        addSampleJson(samples, s_tel[slot]);
    }

    if (!sendDoc(doc)) {
        return false;
    }
    popSamples(batch);
    s_lastTelemetryTxMs = millis();
    return true;
}

// ----------------------------------------------------------------------------
// Estado
// ----------------------------------------------------------------------------

const char* modeName(OperationMode mode) {
    switch (mode) {
        case OperationMode::NORMAL:        return "NORMAL";
        case OperationMode::PORTAL:        return "PORTAL";
        case OperationMode::MANTENIMIENTO: return "MANTENIMIENTO";
    }
    return "NORMAL";
}

const char* configSourceName() {
    return (settingsGetConfigSource() == ConfigSource::LOCAL) ? "local" : "backend";
}

bool zoneForChannel(RelayChannel channel, ZoneType& type, uint8_t& zone) {
    switch (channel) {
        case RelayChannel::RIEGO_Z1:
            type = ZoneType::SUBSTRATE; zone = 0; return true;
        case RelayChannel::RIEGO_Z2:
            type = ZoneType::SUBSTRATE; zone = 1; return true;
        case RelayChannel::ASPERSION:
            type = ZoneType::SPRINKLER; zone = 0; return true;
        default:
            return false;
    }
}

const char* irrigationSourceName(IrrigationTrigger trigger) {
    switch (trigger) {
        case IrrigationTrigger::MANUAL:    return "manual_local";
        case IrrigationTrigger::OVERRIDE:  return "manual_remote";
        case IrrigationTrigger::SCHEDULE:  return "auto";
        case IrrigationTrigger::THRESHOLD: return "auto";
        case IrrigationTrigger::NONE:      return "off";
    }
    return "off";
}

void addChannelState(JsonObject& channels, RelayChannel channel) {
    JsonObject c = channels[relayChannelName(channel)].to<JsonObject>();
    bool on = relayGetChannel(channel);
    const char* source = "off";
    uint32_t remainingMs = 0;

    ZoneType type;
    uint8_t zone = 0;
    if (zoneForChannel(channel, type, zone)) {
        const ZoneIrrigationState st = irrigationGetState(type, zone);
        on = st.active;
        source = irrigationSourceName(st.trigger);
        if (st.active && st.durationMs > 0) {
            const uint32_t elapsed = millis() - st.startedAtMs;
            remainingMs = (elapsed < st.durationMs) ? (st.durationMs - elapsed) : 0;
        }
    } else if (channel == RelayChannel::VENTANA) {
        source = "off";
    } else if (channel == RelayChannel::ALARMA) {
        source = alarmIsActive() ? "alarm" : "off";
    } else {
        source = channelControlSourceName(channelControlGetSource(channel));
        remainingMs = channelControlRemainingMs(channel);
    }

    c["enabled"] = settingsIsChannelEnabled(channel);
    c["on"] = on;
    c["source"] = source;
    char until[24];
    if (remainingMs > 0 && rtcIsTimeValid() &&
        isoFromEpoch(rtcNowEpoch() + (int64_t)(remainingMs / 1000UL),
                     until, sizeof(until))) {
        c["until"] = until;
    } else {
        c["until"] = nullptr;
    }
}

void buildStatePayload(JsonObject& p) {
    p["boot_id"] = s_bootId;
    p["mode"] = modeName(stateMachineGetMode());
    p["config_version"] = settingsGetConfigVersion();
    p["local_version"] = settingsGetLocalVersion();
    p["config_source"] = configSourceName();

    JsonObject clock = p["clock"].to<JsonObject>();
    char ts[24];
    const char* iso = nowIsoOrNull(ts, sizeof(ts));
    clock["utc"] = iso;
    clock["source"] = rtcIsReal() ? "rtc" : "sim";
    clock["valid"] = rtcIsTimeValid();
    clock["timezone_offset_min"] = settingsGetTimezoneOffsetMin();

    JsonObject wifi = p["wifi"].to<JsonObject>();
    const bool wifiConnected = (WiFi.status() == WL_CONNECTED);
    wifi["connected"] = wifiConnected;
    wifi["rssi_dbm"] = wifiConnected ? (int)WiFi.RSSI() : 0;
    wifi["ip"] = wifiConnected ? WiFi.localIP().toString() : String("");

    JsonObject ws = p["ws"].to<JsonObject>();
    ws["connected"] = (s_state == LinkState::CONNECTED);
    ws["session_id"] = (s_sessionId[0] != '\0') ? s_sessionId : (const char*)nullptr;
    ws["last_rx_s"] = (s_lastRxMs == 0) ? 0
                                        : (int)((millis() - s_lastRxMs) / 1000UL);

    JsonObject channels = p["channels"].to<JsonObject>();
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        addChannelState(channels, (RelayChannel)i);
    }

    JsonObject window = p["window"].to<JsonObject>();
    const WindowState wstate = windowGetState();
    window["state"] = windowStateCode(wstate);
    window["moving"] = windowIsBusy();
    const WindowTarget target = windowGetTarget();
    if (wstate == WindowState::MOVING && target == WindowTarget::OPEN) {
        window["direction"] = "opening";
    } else if (wstate == WindowState::MOVING && target == WindowTarget::CLOSE) {
        window["direction"] = "closing";
    } else {
        window["direction"] = nullptr;
    }
    window["fault"] = (wstate == WindowState::FAULT);
    window["last_move_ts"] = nullptr;

    JsonObject alarms = p["alarms"].to<JsonObject>();
    JsonArray active = alarms["active"].to<JsonArray>();
    JsonArray latched = alarms["latched"].to<JsonArray>();
    const AlarmMask activeMask = alarmGetActiveMask();
    const AlarmMask latchedMask = alarmGetLatchedMask();
    for (uint8_t i = 1; i < ALARM_CONDITION_COUNT; i++) {
        const AlarmCondition cond = (AlarmCondition)i;
        if (activeMask & ALARM_BIT(cond)) {
            active.add(alarmConditionKey(cond));
        }
        if (latchedMask & ALARM_BIT(cond)) {
            latched.add(alarmConditionKey(cond));
        }
    }
    if (alarmIsSilenced()) {
        char until[24];
        const uint32_t remaining = alarmSilenceRemainingMs();
        if (remaining > 0 && rtcIsTimeValid() &&
            isoFromEpoch(rtcNowEpoch() + (int64_t)(remaining / 1000UL),
                         until, sizeof(until))) {
            alarms["silenced_until"] = until;
        } else {
            alarms["silenced_until"] = nullptr;
        }
    } else {
        alarms["silenced_until"] = nullptr;
    }
}

uint32_t stateSignature() {
    uint32_t sig = 0;
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        if (relayGetChannel((RelayChannel)i)) {
            sig |= (1u << i);
        }
    }
    sig |= ((uint32_t)windowGetState() & 0x03u) << 8;
    sig |= ((uint32_t)alarmGetActiveMask() & 0xFFFFu) << 10;
    sig ^= (uint32_t)settingsGetConfigVersion() * 2654435761u;
    sig ^= (uint32_t)settingsGetLocalVersion() * 40503u;
    sig ^= (settingsGetConfigSource() == ConfigSource::LOCAL) ? 0x80000000u : 0u;
    sig ^= ((uint32_t)stateMachineGetMode()) << 28;
    return sig;
}

void sendState() {
    if (s_state != LinkState::CONNECTED) {
        return;
    }
    JsonDocument doc;
    char id[16];
    JsonObject p = beginEnvelope(doc, "state", id, sizeof(id));
    buildStatePayload(p);
    if (sendDoc(doc)) {
        s_lastStateMs = millis();
        s_lastStateSig = stateSignature();
        s_haveStateSig = true;
    }
}

// ----------------------------------------------------------------------------
// Telemetría / estado por transiciones
// ----------------------------------------------------------------------------

void trackTransitions() {
    static bool initialized = false;
    static uint8_t lastRelayBits = 0;
    static WindowState lastWindow = WindowState::CLOSED;
    static AlarmMask lastAlarmMask = 0;
    static bool lastSilenced = false;
    static bool lastIrrActive[MAX_SUBSTRATE_ZONES + MAX_SPRINKLER_ZONES] = { false };
    static IrrigationTrigger lastIrrTrigger[MAX_SUBSTRATE_ZONES + MAX_SPRINKLER_ZONES] =
        { IrrigationTrigger::NONE };

    uint8_t relayBits = 0;
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        if (relayGetChannel((RelayChannel)i)) {
            relayBits |= (uint8_t)(1u << i);
        }
    }
    const WindowState wstate = windowGetState();
    const AlarmMask alarmMask = alarmGetActiveMask();
    const bool silenced = alarmIsSilenced();

    bool irrActive[3] = { false };
    IrrigationTrigger irrTrigger[3] = { IrrigationTrigger::NONE };
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        const ZoneIrrigationState st = irrigationGetState(ZoneType::SUBSTRATE, z);
        irrActive[z] = st.active;
        irrTrigger[z] = st.trigger;
    }
    {
        const ZoneIrrigationState st = irrigationGetState(ZoneType::SPRINKLER, 0);
        irrActive[2] = st.active;
        irrTrigger[2] = st.trigger;
    }

    if (!initialized) {
        initialized = true;
        lastRelayBits = relayBits;
        lastWindow = wstate;
        lastAlarmMask = alarmMask;
        lastSilenced = silenced;
        for (uint8_t i = 0; i < 3; i++) {
            lastIrrActive[i] = irrActive[i];
            lastIrrTrigger[i] = irrTrigger[i];
        }
        return;
    }

    // Canales gobernados por channel_control.
    const RelayChannel tracked[] = { RelayChannel::CAMA_CALIENTE,
                                     RelayChannel::ILUMINACION,
                                     RelayChannel::VENTILACION };
    for (RelayChannel ch : tracked) {
        const uint8_t bit = (uint8_t)(1u << (uint8_t)ch);
        const bool was = (lastRelayBits & bit) != 0;
        const bool now = (relayBits & bit) != 0;
        if (was == now) {
            continue;
        }
        if (ch == RelayChannel::CAMA_CALIENTE) {
            queueEvent(now ? EvCode::THERMOSTAT_ON : EvCode::THERMOSTAT_OFF,
                       relayChannelName(ch));
        } else if (ch == RelayChannel::ILUMINACION) {
            queueEvent(now ? EvCode::LIGHT_ON : EvCode::LIGHT_OFF,
                       relayChannelName(ch));
        } else {
            queueEvent(now ? EvCode::FAN_ON : EvCode::FAN_OFF,
                       relayChannelName(ch));
        }
    }

    // Riego por zona.
    for (uint8_t i = 0; i < 3; i++) {
        if (irrActive[i] == lastIrrActive[i]) {
            continue;
        }
        const RelayChannel ch = (i == 0) ? RelayChannel::RIEGO_Z1
                               : (i == 1) ? RelayChannel::RIEGO_Z2
                                          : RelayChannel::ASPERSION;
        if (irrActive[i]) {
            const char* reason = "manual_remote";
            if (irrTrigger[i] == IrrigationTrigger::MANUAL) reason = "manual_local";
            else if (irrTrigger[i] == IrrigationTrigger::SCHEDULE) reason = "schedule";
            else if (irrTrigger[i] == IrrigationTrigger::THRESHOLD) reason = "threshold";
            queueEvent(EvCode::IRRIGATION_START, relayChannelName(ch), reason);
        } else {
            queueEvent(EvCode::IRRIGATION_STOP, relayChannelName(ch), "done");
        }
        lastIrrActive[i] = irrActive[i];
        lastIrrTrigger[i] = irrTrigger[i];
    }

    // Ventana.
    if (wstate != lastWindow) {
        if (wstate == WindowState::OPEN) {
            queueEvent(EvCode::WINDOW_OPEN, "ventana");
        } else if (wstate == WindowState::CLOSED) {
            queueEvent(EvCode::WINDOW_CLOSE, "ventana");
        } else if (wstate == WindowState::FAULT) {
            queueEvent(EvCode::WINDOW_FAULT, "ventana");
        }
        lastWindow = wstate;
    }

    // Alarmas: una entrada/salida por condición.
    const AlarmMask changed = (AlarmMask)(alarmMask ^ lastAlarmMask);
    for (uint8_t i = 1; i < ALARM_CONDITION_COUNT; i++) {
        const AlarmCondition cond = (AlarmCondition)i;
        if ((changed & ALARM_BIT(cond)) == 0) {
            continue;
        }
        if (alarmMask & ALARM_BIT(cond)) {
            queueEvent(EvCode::ALARM_RAISED, nullptr, nullptr,
                       alarmConditionKey(cond), alarmConditionIsCritical(cond));
        } else {
            queueEvent(EvCode::ALARM_CLEARED, nullptr, nullptr,
                       alarmConditionKey(cond), false);
        }
    }
    lastAlarmMask = alarmMask;

    if (silenced != lastSilenced) {
        if (silenced) {
            queueEvent(EvCode::ALARM_SILENCED, "alarma", "manual");
        }
        lastSilenced = silenced;
    }

    lastRelayBits = relayBits;
}

// ----------------------------------------------------------------------------
// Claim HTTP
// ----------------------------------------------------------------------------

String backendBase() {
    String base = settingsGetBackendUrl();
    base.trim();
    if (base.length() == 0) {
        return "";
    }
    if (!base.startsWith("http://") && !base.startsWith("https://")) {
        base = "http://" + base;
    }
    while (base.endsWith("/")) {
        base.remove(base.length() - 1);
    }
    return base;
}

String deriveWsUrl() {
    const char* stored = settingsGetWsUrl();
    if (stored != nullptr && stored[0] != '\0') {
        return String(stored);
    }
    String base = backendBase();
    if (base.length() == 0 || base.startsWith("https://")) {
        return "";
    }
    String hostPort = base.substring(7);  // quitar "http://"
    const int slash = hostPort.indexOf('/');
    if (slash >= 0) {
        hostPort = hostPort.substring(0, slash);
    }
    return "ws://" + hostPort + "/ws/device";
}

void rememberError(const char* message) {
    strncpy(s_lastError, message ? message : "", sizeof(s_lastError) - 1);
    s_lastError[sizeof(s_lastError) - 1] = '\0';
}

void markBackendSeen() {
    const int64_t now = rtcNowEpoch();
    if (now > 0) {
        s_lastBackendSeenEpoch = now;
    }
}

bool parseWsUrl(const String& url, String& host, uint16_t& port, String& path,
                bool& secure) {
    secure = false;
    if (url.startsWith("wss://")) {
        secure = true;
        return false;  // TLS no implementado en v0.1
    }
    if (!url.startsWith("ws://")) {
        return false;
    }
    String rest = url.substring(5);
    const int slash = rest.indexOf('/');
    String hostPort = (slash >= 0) ? rest.substring(0, slash) : rest;
    path = (slash >= 0) ? rest.substring(slash) : "/";
    if (path.length() == 0) {
        path = "/";
    }
    const int colon = hostPort.indexOf(':');
    if (colon >= 0) {
        host = hostPort.substring(0, colon);
        const long parsed = hostPort.substring(colon + 1).toInt();
        port = (parsed > 0 && parsed <= 65535) ? (uint16_t)parsed : 80;
    } else {
        host = hostPort;
        port = 80;
    }
    return host.length() > 0;
}

String macSuffix() {
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    return String(buf);
}

bool claimDevice() {
    String base = backendBase();
    if (base.length() == 0) {
        rememberError("backend_url vacia");
        return false;
    }
    if (base.startsWith("https://")) {
        rememberError("https no soportado (usar LAN)");
        return false;
    }
    const char* claimCode = settingsGetClaimCode();
    if (claimCode == nullptr || claimCode[0] == '\0') {
        if (!s_claimNoCodeLogged) {
            Serial.println("[WS] Sin claim_code: no se puede reclamar el equipo");
            s_claimNoCodeLogged = true;
        }
        rememberError("sin claim_code");
        return false;
    }
    s_claimNoCodeLogged = false;

    JsonDocument doc;
    doc["device_id"] = settingsGetDeviceId()[0] ? settingsGetDeviceId() : "";
    doc["claim_code"] = claimCode;
    doc["fw_version"] = FIRMWARE_VERSION;
    doc["hw_revision"] = "rev-A";
    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.setConnectTimeout(WS_CLAIM_TIMEOUT_MS);
    http.setTimeout(WS_CLAIM_TIMEOUT_MS);
    const String url = base + WS_CLAIM_PATH;
    Serial.printf("[WS] Claim POST %s\n", url.c_str());
    if (!http.begin(url)) {
        rememberError("claim: URL invalida");
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(body);
    String response;
    if (code > 0) {
        response = http.getString();
    }
    http.end();

    Serial.printf("[WS] Claim HTTP %d: %s\n", code, response.c_str());
    if (code < 200 || code >= 300) {
        s_nextClaimMs = millis() + 5UL * 60UL * 1000UL;
        if (code == 401 || code == 403) {
            rememberError("claim rechazado (token/clave)");
        } else if (code == 404) {
            rememberError("claim no existe en el backend");
        } else if (code <= 0) {
            rememberError("claim: sin conexion HTTP");
            s_nextClaimMs = millis() + WS_CLAIM_RETRY_MS;
        } else {
            rememberError("claim: respuesta HTTP no OK");
        }
        return false;
    }

    JsonDocument respDoc;
    const DeserializationError err = deserializeJson(respDoc, response);
    if (err) {
        rememberError("claim: JSON invalido");
        s_nextClaimMs = millis() + WS_CLAIM_RETRY_MS;
        return false;
    }

    JsonVariant root = respDoc.as<JsonVariant>();
    if (respDoc["data"].is<JsonObject>()) {
        root = respDoc["data"].as<JsonVariant>();
    }

    const char* token = root["token"] | "";
    if (token[0] == '\0') {
        token = root["api_token"] | "";
    }
    if (token[0] == '\0') {
        rememberError("claim sin token");
        s_nextClaimMs = millis() + WS_CLAIM_RETRY_MS;
        return false;
    }

    const char* deviceId = root["device_id"] | "";
    String finalId = (deviceId[0] != '\0') ? String(deviceId)
                                           : String(settingsGetDeviceId());
    if (finalId.length() == 0) {
        finalId = "vivero-" + macSuffix();
    }
    const char* wsUrl = root["ws_url"] | "";
    settingsSetDeviceIdentity(finalId.c_str(), token);
    if (wsUrl[0] != '\0') {
        settingsSetWsUrl(wsUrl);
    }
    const uint32_t serverCfg = root["config_version"] | 0;
    Serial.printf("[WS] Claim OK: device_id=%s ws_url=%s cfg_srv=%lu\n",
                  finalId.c_str(),
                  settingsGetWsUrl()[0] ? settingsGetWsUrl() : "(derivada)",
                  (unsigned long)serverCfg);
    markBackendSeen();
    s_backoffMs = 0;
    s_nextActionMs = millis();
    return true;
}

// ----------------------------------------------------------------------------
// Configuración del backend
// ----------------------------------------------------------------------------

struct ConfigResult {
    bool ok = false;
    bool stale = false;
    bool applyNow = false;
    uint32_t version = 0;
    String error;
    String warnings;
};

void addWarning(ConfigResult& res, const char* warning) {
    if (res.warnings.length() > 0) {
        res.warnings += "; ";
    }
    res.warnings += warning;
}

void addWarning(ConfigResult& res, const String& warning) {
    addWarning(res, warning.c_str());
}

bool parseHhMm(const char* text, uint8_t& hour, uint8_t& minute) {
    if (text == nullptr) {
        return false;
    }
    int h = 0, m = 0;
    if (sscanf(text, "%2d:%2d", &h, &m) != 2) {
        return false;
    }
    if (h < 0 || h > 23 || m < 0 || m > 59) {
        return false;
    }
    hour = (uint8_t)h;
    minute = (uint8_t)m;
    return true;
}

bool parseDays(JsonVariantConst entry, uint8_t& mask, String& error) {
    mask = SCHEDULE_DAYS_ALL;
    if (entry["days"].isNull()) {
        return true;
    }
    JsonArrayConst days = entry["days"].as<JsonArrayConst>();
    if (days.isNull() || days.size() == 0) {
        error = "days vacio";
        return false;
    }
    mask = 0;
    for (JsonVariantConst day : days) {
        const int d = day.as<int>();
        if (d < 1 || d > 7) {
            error = "days fuera de 1..7";
            return false;
        }
        mask |= (uint8_t)(1u << (d - 1));
    }
    return true;
}

bool parseSchedule(JsonObject channelObj, LocalSchedule* out,
                   ConfigResult& res, const char* channelId) {
    if (!channelObj["schedule"].is<JsonObject>()) {
        return true;  // canal parcial: conservar horarios previos
    }
    JsonObject schedule = channelObj["schedule"].as<JsonObject>();
    const bool enabled = schedule["enabled"] | true;

    for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
        out[i].enabled = false;
        out[i].daysMask = SCHEDULE_DAYS_ALL;
    }

    if (!schedule["entries"].is<JsonArray>()) {
        return true;
    }
    JsonArray entries = schedule["entries"].as<JsonArray>();
    if (entries.size() > MAX_SCHEDULES_PER_ZONE) {
        res.error = String(channelId) + ": mas de " +
                    String(MAX_SCHEDULES_PER_ZONE) + " horarios";
        return false;
    }

    uint8_t index = 0;
    for (JsonObject entry : entries) {
        uint8_t hour = 0;
        uint8_t minute = 0;
        if (!parseHhMm(entry["start"] | "", hour, minute)) {
            res.error = String(channelId) + ": start invalido";
            return false;
        }
        const int duration = entry["duration_s"] | 0;
        if (duration < 1 || duration > 86400) {
            res.error = String(channelId) + ": duration_s invalido";
            return false;
        }
        uint8_t days = SCHEDULE_DAYS_ALL;
        String dayError;
        if (!parseDays(entry, days, dayError)) {
            res.error = String(channelId) + ": " + dayError;
            return false;
        }
        out[index].hour = hour;
        out[index].minute = minute;
        out[index].duration_s = (uint16_t)duration;
        out[index].enabled = enabled;
        out[index].daysMask = days;
        index++;
    }
    return true;
}

bool parseMoisture(JsonObject moisture, LocalZoneProfile& zone,
                   ConfigResult& res, const char* channelId) {
    if (moisture.isNull()) {
        return true;
    }
    if (moisture["start_below_pct"].is<float>() ||
        moisture["start_below_pct"].is<int>()) {
        const float start = moisture["start_below_pct"] | -1.0f;
        if (start < 0.0f || start > 100.0f) {
            res.error = String(channelId) + ": start_below_pct fuera de 0..100";
            return false;
        }
        zone.minHumidityPct = (uint8_t)(start + 0.5f);
    }
    if (moisture["enabled"].is<bool>()) {
        zone.thresholdEnabled = moisture["enabled"] | true;
    }
    if (moisture["stop_above_pct"].is<float>() ||
        moisture["stop_above_pct"].is<int>()) {
        addWarning(res, String(channelId) + ": stop_above_pct no soportado");
    }
    if (moisture["window_start"].is<const char*>() ||
        moisture["window_end"].is<const char*>()) {
        addWarning(res, String(channelId) + ": ventana horaria de umbral no soportada");
    }
    return true;
}

ConfigResult applyBackendConfig(JsonObject payload) {
    ConfigResult res;
    const uint32_t version = payload["config_version"] | 0;
    if (version == 0) {
        res.error = "config_version ausente";
        return res;
    }
    res.version = version;
    const bool force = payload["force"] | false;
    if (!force && version <= settingsGetConfigVersion()) {
        res.ok = true;
        res.stale = true;
        res.applyNow = false;
        return res;
    }

    LocalProfile profile;
    if (!backendProfileGetStored(profile)) {
        profile = localProfileGetRuntime();
    }
    profile.magic = LOCAL_PROFILE_MAGIC;
    profile.version = LOCAL_PROFILE_VERSION;

    if (payload["timezone_offset_min"].is<int>()) {
        const int tz = payload["timezone_offset_min"] | -180;
        if (tz < -720 || tz > 840) {
            res.error = "timezone_offset_min fuera de -720..840";
            return res;
        }
        profile.timezoneOffsetMin = (int16_t)tz;
    }

    if (payload["irrigation"].is<JsonObject>()) {
        JsonObject irr = payload["irrigation"].as<JsonObject>();
        if (irr["interlock"].is<bool>()) {
            profile.interlock = irr["interlock"] | true;
        }
        if (irr["stagger_s"].is<int>()) {
            const int stagger = irr["stagger_s"] | 30;
            if (stagger < 0 || stagger > 600) {
                res.error = "irrigation.stagger_s fuera de 0..600";
                return res;
            }
            profile.staggerS = (uint16_t)stagger;
        }
    }

    if (!payload["channels"].is<JsonObject>()) {
        res.error = "channels ausente";
        return res;
    }
    JsonObject channels = payload["channels"].as<JsonObject>();

    // --- Riego sustrato Z1/Z2 ---
    const char* subIds[MAX_SUBSTRATE_ZONES] = { "riego_z1", "riego_z2" };
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        if (!channels[subIds[z]].is<JsonObject>()) {
            continue;
        }
        JsonObject ch = channels[subIds[z]].as<JsonObject>();
        LocalZoneProfile& zone = profile.substrate[z];
        if (ch["auto_enabled"].is<bool>()) {
            zone.paused = !(ch["auto_enabled"] | true);
        }
        if (ch["moisture"].is<JsonObject>()) {
            if (!parseMoisture(ch["moisture"].as<JsonObject>(), zone, res, subIds[z])) {
                return res;
            }
            JsonObject moisture = ch["moisture"].as<JsonObject>();
            if (moisture["max_run_s"].is<int>()) {
                const int maxRun = moisture["max_run_s"] | 900;
                if (maxRun < 1 || maxRun > IRRIGATION_MAX_RUN_ABS_S) {
                    res.error = String(subIds[z]) + ": max_run_s fuera de rango";
                    return res;
                }
                if (profile.maxRunSubstrateS != 0 &&
                    profile.maxRunSubstrateS != (uint16_t)maxRun) {
                    addWarning(res, "max_run_s distinto por zona: se usa el ultimo");
                }
                profile.maxRunSubstrateS = (uint16_t)maxRun;
            }
            if (moisture["min_off_s"].is<int>()) {
                const int minOff = moisture["min_off_s"] | 1800;
                if (minOff < 0 || minOff > IRRIGATION_MIN_OFF_MAX_S) {
                    res.error = String(subIds[z]) + ": min_off_s fuera de rango";
                    return res;
                }
                profile.minOffS = (uint16_t)minOff;
            }
        }
        if (!parseSchedule(ch, zone.schedules, res, subIds[z])) {
            return res;
        }
    }

    // --- Aspersión ---
    if (channels["aspersion"].is<JsonObject>()) {
        JsonObject ch = channels["aspersion"].as<JsonObject>();
        LocalZoneProfile& zone = profile.sprinkler[0];
        if (ch["auto_enabled"].is<bool>()) {
            zone.paused = !(ch["auto_enabled"] | true);
        }
        if (ch["max_run_s"].is<int>()) {
            const int maxRun = ch["max_run_s"] | 1800;
            if (maxRun < 1 || maxRun > IRRIGATION_MAX_RUN_ABS_S) {
                res.error = "aspersion.max_run_s fuera de rango";
                return res;
            }
            profile.maxRunSprinklerS = (uint16_t)maxRun;
        }
        if (!parseSchedule(ch, zone.schedules, res, "aspersion")) {
            return res;
        }
    }

    // --- Cama caliente ---
    if (channels["cama_caliente"].is<JsonObject>()) {
        JsonObject ch = channels["cama_caliente"].as<JsonObject>();
        if (ch["auto_enabled"].is<bool>()) {
            profile.bedAutoEnabled = ch["auto_enabled"] | true;
        }
        if (ch["thermostat"].is<JsonObject>()) {
            JsonObject th = ch["thermostat"].as<JsonObject>();
            if (th["setpoint_c"].is<float>() || th["setpoint_c"].is<int>()) {
                const float setpoint = th["setpoint_c"] | 25.0f;
                if (setpoint < 5.0f || setpoint >= BED_MAX_C) {
                    res.error = "cama: setpoint_c fuera de 5..34.9";
                    return res;
                }
                profile.bedTargetC = setpoint;
            }
            if (th["max_c"].is<float>() || th["max_c"].is<int>()) {
                const float maxC = th["max_c"] | BED_MAX_C;
                if (maxC > BED_MAX_C + 0.01f) {
                    res.error = "cama: max_c mayor que el corte absoluto 35 C";
                    return res;
                }
            }
            if (th["hysteresis_c"].is<float>() || th["hysteresis_c"].is<int>()) {
                addWarning(res, "cama: hysteresis_c fija en 1.0 C");
            }
            const char* sensor = th["sensor"] | "";
            if (sensor[0] != '\0' && strcmp(sensor, "bed") != 0) {
                res.error = "cama: sensor distinto de 'bed'";
                return res;
            }
        }
    }

    // --- Iluminación ---
    if (channels["iluminacion"].is<JsonObject>()) {
        JsonObject ch = channels["iluminacion"].as<JsonObject>();
        if (ch["auto_enabled"].is<bool>() && !(ch["auto_enabled"] | true)) {
            for (uint8_t i = 0; i < MAX_SCHEDULES_PER_ZONE; i++) {
                profile.light[i].enabled = false;
            }
        }
        if (ch["max_run_s"].is<int>()) {
            const int maxRun = ch["max_run_s"] | 57600;
            if (maxRun < 1 || maxRun > 86400) {
                res.error = "iluminacion.max_run_s fuera de rango";
                return res;
            }
            if (maxRun != 57600) {
                addWarning(res, "iluminacion: max_run_s fijo en 16 h");
            }
        }
        if (!parseSchedule(ch, profile.light, res, "iluminacion")) {
            return res;
        }
    }

    // --- Ventilación ---
    if (channels["ventilacion"].is<JsonObject>()) {
        JsonObject ch = channels["ventilacion"].as<JsonObject>();
        if (ch["auto_enabled"].is<bool>()) {
            profile.ventAutoEnabled = ch["auto_enabled"] | true;
        }
        if (ch["schedule"].is<JsonObject>()) {
            if (!parseSchedule(ch, profile.vent, res, "ventilacion")) {
                return res;
            }
        }
        if (ch["climate"].is<JsonObject>()) {
            JsonObject cl = ch["climate"].as<JsonObject>();
            if (cl["enabled"].is<bool>()) {
                profile.ventClimateEnabled = cl["enabled"] | true;
            }
            const bool hasOnT = cl["on_above_temp_c"].is<float>() ||
                                cl["on_above_temp_c"].is<int>();
            const bool hasOffT = cl["off_below_temp_c"].is<float>() ||
                                 cl["off_below_temp_c"].is<int>();
            const bool hasOnRh = cl["on_above_rh_pct"].is<float>() ||
                                 cl["on_above_rh_pct"].is<int>();
            const bool hasOffRh = cl["off_below_rh_pct"].is<float>() ||
                                  cl["off_below_rh_pct"].is<int>();
            if (hasOnT) profile.ventOnTempC = cl["on_above_temp_c"] | DEFAULT_VENT_ON_TEMP_C;
            if (hasOffT) profile.ventOffTempC = cl["off_below_temp_c"] | DEFAULT_VENT_OFF_TEMP_C;
            if (hasOnRh) profile.ventOnRhPct = cl["on_above_rh_pct"] | DEFAULT_VENT_ON_RH_PCT;
            if (hasOffRh) profile.ventOffRhPct = cl["off_below_rh_pct"] | DEFAULT_VENT_OFF_RH_PCT;
            if (profile.ventOnTempC < -20.0f || profile.ventOnTempC > 80.0f ||
                profile.ventOffTempC < -20.0f || profile.ventOffTempC > 80.0f ||
                profile.ventOnRhPct < 0.0f || profile.ventOnRhPct > 100.0f ||
                profile.ventOffRhPct < 0.0f || profile.ventOffRhPct > 100.0f) {
                res.error = "ventilacion: umbral de clima fuera de rango";
                return res;
            }
            if (cl["min_on_s"].is<int>() || cl["min_off_s"].is<int>() ||
                cl["max_run_s"].is<int>()) {
                addWarning(res, "ventilacion: min_on/min_off/max_run fijos en firmware");
            }
        }
    }

    // --- Ventana ---
    if (channels["ventana"].is<JsonObject>()) {
        JsonObject ch = channels["ventana"].as<JsonObject>();
        if (ch["pulse_ms"].is<int>()) {
            const int pulse = ch["pulse_ms"] | 5000;
            if (pulse < 1000 || pulse > 10000) {
                res.error = "ventana.pulse_ms fuera de 1000..10000";
                return res;
            }
            if (pulse != 5000) {
                addWarning(res, "ventana: pulse_ms fijo en 5000 ms");
            }
        }
        if (ch["min_interval_s"].is<int>()) {
            const int interval = ch["min_interval_s"] | 60;
            if (interval < 10 || interval > 300) {
                res.error = "ventana.min_interval_s fuera de 10..300";
                return res;
            }
            if (interval != 60) {
                addWarning(res, "ventana: min_interval_s fijo en 60 s");
            }
        }
        if (ch["travel_time_s"].is<int>() || ch["max_travel_s"].is<int>()) {
            const int travel = ch["travel_time_s"] | 20;
            const int maxTravel = ch["max_travel_s"] | 30;
            if (travel < 1 || maxTravel < travel || maxTravel > 300) {
                res.error = "ventana: tiempos de viaje invalidos";
                return res;
            }
            addWarning(res, "ventana: tiempos de viaje fijos en firmware");
        }
        if (ch["climate"].is<JsonObject>()) {
            addWarning(res, "ventana: automatizacion por clima no implementada");
        }
    }

    if (channels["alarma"].is<JsonObject>()) {
        addWarning(res, "alarma: auto_enabled no aplica (motor local fijo)");
    }

    // --- Alarmas ---
    if (payload["alarms"].is<JsonObject>()) {
        JsonObject alarms = payload["alarms"].as<JsonObject>();
        if (alarms["silence_max_s"].is<int>()) {
            addWarning(res, "alarms: silence_max_s fijo en 600 s");
        }
        if (alarms["rules"].is<JsonArray>()) {
            JsonArray rules = alarms["rules"].as<JsonArray>();
            for (JsonObject rule : rules) {
                const char* id = rule["id"] | "";
                const bool enabled = rule["enabled"] | true;
                if (!enabled) {
                    addWarning(res, "alarms: regla deshabilitada no soportada");
                    continue;
                }
                if (strcmp(id, "air_temp_high") == 0) {
                    const float threshold = rule["threshold"] | 45.0f;
                    if (threshold < -20.0f || threshold > 80.0f) {
                        res.error = "alarms: air_temp_high fuera de rango";
                        return res;
                    }
                    profile.alarmMaxTemp = threshold;
                } else if (strcmp(id, "air_temp_low") == 0) {
                    const float threshold = rule["threshold"] | 0.0f;
                    if (threshold < -50.0f || threshold > 40.0f) {
                        res.error = "alarms: air_temp_low fuera de rango";
                        return res;
                    }
                    profile.alarmMinTemp = threshold;
                } else if (strcmp(id, "bed_temp_high") == 0) {
                    addWarning(res, "alarms: bed_temp_high fijo en 35 C");
                } else {
                    addWarning(res, "alarms: regla no soportada por firmware");
                }
            }
        }
    }

    // Normalizaciones finales y validación de coherencia.
    if (profile.alarmMaxTemp <= profile.alarmMinTemp) {
        res.error = "alarms: max_temp debe ser mayor que min_temp";
        return res;
    }
    if (profile.maxRunSubstrateS < 1) profile.maxRunSubstrateS = IRRIGATION_MAX_RUN_S_DEFAULT;
    if (profile.maxRunSprinklerS < 1) profile.maxRunSprinklerS = SPRINKLER_MAX_RUN_S_DEFAULT;
    if (profile.staggerS > 600) profile.staggerS = 600;
    if (profile.bedTargetC < 5.0f || profile.bedTargetC >= BED_MAX_C) {
        profile.bedTargetC = settingsGetBedTargetC();
    }
    if (profile.timezoneOffsetMin < -720 || profile.timezoneOffsetMin > 840) {
        profile.timezoneOffsetMin = settingsGetTimezoneOffsetMin();
    }

    const bool applyNow = (settingsGetConfigSource() == ConfigSource::BACKEND);
    if (!localProfileStoreBackend(profile, applyNow)) {
        res.error = "fallo de NVS";
        return res;
    }
    settingsSetConfigVersion(version, force);

    res.ok = true;
    res.applyNow = applyNow;
    queueEvent(EvCode::CONFIG_APPLIED, nullptr,
               applyNow ? "applied" : "stored_local_override");
    Serial.printf("[WS] Config v%lu %s (stored=%s applied=%s)\n",
                  (unsigned long)version, force ? "force" : "",
                  "true", applyNow ? "true" : "false");
    return res;
}

// ----------------------------------------------------------------------------
// Comandos
// ----------------------------------------------------------------------------

struct CommandOutcome {
    const char* status = "ok";
    const char* reason = nullptr;
    bool hasOn = false;
    bool on = false;
    bool noop = false;
    bool silenced = false;
    bool latchesCleared = false;
    int64_t untilEpoch = 0;
    String warnings;
};

struct CmdCacheEntry {
    char id[40];
    char ack[256];
    uint32_t atMs;
    bool used;
};

static CmdCacheEntry s_cmdCache[WS_COMMAND_CACHE];
static uint8_t s_cmdCacheNext = 0;
static uint16_t s_cmdMinuteCount = 0;
static uint32_t s_cmdMinuteStartMs = 0;
static uint32_t s_lastCmdMs[RELAY_CHANNEL_COUNT] = { 0 };

bool findCachedAck(const char* id, String& ack) {
    if (id == nullptr || id[0] == '\0') {
        return false;
    }
    const uint32_t now = millis();
    for (uint8_t i = 0; i < WS_COMMAND_CACHE; i++) {
        const CmdCacheEntry& e = s_cmdCache[i];
        if (!e.used) continue;
        if ((now - e.atMs) > WS_COMMAND_CACHE_MS) continue;
        if (strcmp(e.id, id) == 0) {
            ack = e.ack;
            return true;
        }
    }
    return false;
}

void cacheAck(const char* id, const String& ack) {
    if (id == nullptr || id[0] == '\0') {
        return;
    }
    CmdCacheEntry& e = s_cmdCache[s_cmdCacheNext];
    s_cmdCacheNext = (uint8_t)((s_cmdCacheNext + 1) % WS_COMMAND_CACHE);
    memset(&e, 0, sizeof(e));
    strncpy(e.id, id, sizeof(e.id) - 1);
    strncpy(e.ack, ack.c_str(), sizeof(e.ack) - 1);
    e.atMs = millis();
    e.used = true;
}

bool rateLimited(RelayChannel channel, bool hasChannel) {
    const uint32_t now = millis();
    if (s_cmdMinuteStartMs == 0 || (now - s_cmdMinuteStartMs) >= 60000UL) {
        s_cmdMinuteStartMs = now;
        s_cmdMinuteCount = 0;
    }
    if (s_cmdMinuteCount >= WS_RATE_MAX_PER_MIN) {
        return true;
    }
    if (hasChannel) {
        const uint8_t idx = (uint8_t)channel;
        if (idx < RELAY_CHANNEL_COUNT && s_lastCmdMs[idx] != 0 &&
            (now - s_lastCmdMs[idx]) < WS_RATE_CHANNEL_MS) {
            return true;
        }
    }
    s_cmdMinuteCount++;
    if (hasChannel) {
        s_lastCmdMs[(uint8_t)channel] = now;
    }
    return false;
}

bool channelIsLocalManual(RelayChannel channel) {
    ZoneType type;
    uint8_t zone = 0;
    if (zoneForChannel(channel, type, zone)) {
        return irrigationManualIsActive(type, zone);
    }
    if (!relayChannelIsValid(channel)) {
        return false;
    }
    const ChannelSource src = channelControlGetSource(channel);
    return src == ChannelSource::MANUAL_LOCAL || src == ChannelSource::TEST;
}

bool findChannel(const char* id, RelayChannel& out) {
    if (id == nullptr) {
        return false;
    }
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        if (strcmp(id, relayChannelName((RelayChannel)i)) == 0) {
            out = (RelayChannel)i;
            return true;
        }
    }
    return false;
}

CommandOutcome reject(const char* reason) {
    CommandOutcome out;
    out.status = "rejected";
    out.reason = reason;
    return out;
}

CommandOutcome executeCommand(JsonObject payload, const char* channelId,
                              const char* action) {
    RelayChannel channel = RelayChannel::ALARMA;
    const bool hasChannel = findChannel(channelId, channel);

    if (strcmp(action, "report_state") == 0) {
        CommandOutcome out;
        out.hasOn = false;
        sendState();
        return out;
    }

    if (!hasChannel) {
        return reject("unsupported");
    }
    if (!settingsIsChannelEnabled(channel)) {
        return reject("channel_disabled");
    }

    ZoneType zoneType;
    uint8_t zoneIndex = 0;
    const bool isIrrigation = zoneForChannel(channel, zoneType, zoneIndex);

    // Las acciones que encienden respetan el manual local; las seguras no.
    const bool isOnAction = (strcmp(action, "on") == 0) ||
                            (strcmp(action, "open") == 0);
    if (isOnAction && channelIsLocalManual(channel)) {
        return reject("local_manual_active");
    }

    if (strcmp(action, "off") == 0) {
        CommandOutcome out;
        if (isIrrigation) {
            irrigationRemoteStop(zoneType, zoneIndex);
        } else if (channel == RelayChannel::CAMA_CALIENTE) {
            climateBedRemoteOff();
        } else if (channel == RelayChannel::VENTANA) {
            return reject("unsupported");
        } else if (channel == RelayChannel::ALARMA) {
            alarmResetLatches();
            out.latchesCleared = true;
        } else {
            channelControlForceOff(channel);
        }
        return out;
    }

    if (strcmp(action, "on") == 0) {
        if (channel == RelayChannel::VENTANA || channel == RelayChannel::ALARMA) {
            return reject("unsupported");
        }
        const long requested = payload["duration_s"] | 0L;
        if (isIrrigation) {
            if (requested <= 0) {
                return reject("invalid_payload");
            }
            const uint16_t maxRun = irrigationGetMaxRunS(zoneType);
            uint16_t duration = (uint16_t)requested;
            CommandOutcome out;
            out.hasOn = true;
            if (requested > (long)maxRun) {
                if (payload["strict"] | false) {
                    return reject("invalid_payload");
                }
                duration = maxRun;
                out.warnings = "duration_s recortada a max_run_s";
            }
            const char* reason = nullptr;
            if (!irrigationRemoteStart(zoneType, zoneIndex, duration, &reason)) {
                return reject(reason ? reason : "safety_interlock");
            }
            const ZoneIrrigationState st = irrigationGetState(zoneType, zoneIndex);
            out.on = st.active;
            if (out.on && rtcIsTimeValid()) {
                out.untilEpoch = rtcNowEpoch() + (int64_t)duration;
            }
            return out;
        }

        uint32_t durationMs = 0;
        if (channel == RelayChannel::CAMA_CALIENTE) {
            long duration = (requested > 0) ? requested : 1800;
            if (duration > 14400) duration = 14400;
            const char* reason = nullptr;
            if (!climateBedRemoteOn((uint32_t)duration * 1000UL, &reason)) {
                return reject(reason ? reason : "safety_interlock");
            }
            CommandOutcome out;
            out.hasOn = true;
            out.on = channelControlIsOn(channel);
            if (out.on && rtcIsTimeValid()) {
                out.untilEpoch = rtcNowEpoch() + (int64_t)duration;
            }
            return out;
        }

        long duration = (requested > 0) ? requested : 600;
        long maxRun = (channel == RelayChannel::ILUMINACION) ? 57600 : 3600;
        CommandOutcome out;
        out.hasOn = true;
        if (duration > maxRun) {
            if (payload["strict"] | false) {
                return reject("invalid_payload");
            }
            duration = maxRun;
            out.warnings = "duration_s recortada a max_run_s";
        }
        durationMs = (uint32_t)duration * 1000UL;
        if (!channelControlRequest(channel, true, ChannelSource::MANUAL_REMOTE, durationMs)) {
            return reject("busy");
        }
        out.on = channelControlIsOn(channel);
        if (out.on && rtcIsTimeValid()) {
            out.untilEpoch = rtcNowEpoch() + (int64_t)duration;
        }
        return out;
    }

    if (strcmp(action, "auto") == 0) {
        if (channel != RelayChannel::CAMA_CALIENTE) {
            return reject("unsupported");
        }
        climateBedRemoteAuto();
        CommandOutcome out;
        out.hasOn = true;
        out.on = channelControlIsOn(channel);
        return out;
    }

    if (strcmp(action, "open") == 0 || strcmp(action, "close") == 0) {
        if (channel != RelayChannel::VENTANA) {
            return reject("unsupported");
        }
        const bool wantOpen = (strcmp(action, "open") == 0);
        const WindowState state = windowGetState();
        if (state == WindowState::FAULT) {
            return reject("fault");
        }
        if (windowIsBusy()) {
            return reject("busy");
        }
        if (wantOpen && state == WindowState::OPEN) {
            CommandOutcome out;
            out.noop = true;
            return out;
        }
        if (!wantOpen && state == WindowState::CLOSED) {
            CommandOutcome out;
            out.noop = true;
            return out;
        }
        if (windowMinIntervalRemainingMs() > 0) {
            return reject("busy");
        }
        const bool ok = wantOpen ? windowRequestOpen() : windowRequestClose();
        if (!ok) {
            return reject("busy");
        }
        CommandOutcome out;
        out.hasOn = true;
        out.on = true;
        return out;
    }

    if (strcmp(action, "silence") == 0) {
        if (channel != RelayChannel::ALARMA) {
            return reject("unsupported");
        }
        long duration = payload["duration_s"] | 600L;
        if (duration < 1) duration = 1;
        if (duration > 3600) duration = 3600;
        alarmSilence((uint32_t)duration * 1000UL);
        CommandOutcome out;
        out.silenced = true;
        return out;
    }

    if (strcmp(action, "reset") == 0) {
        if (channel != RelayChannel::ALARMA) {
            return reject("unsupported");
        }
        alarmResetLatches();
        CommandOutcome out;
        out.latchesCleared = true;
        return out;
    }

    return reject("unsupported");
}

String buildAck(const char* id, const char* channelId,
                const CommandOutcome& outcome) {
    JsonDocument doc;
    char msgId[16];
    JsonObject p = beginEnvelope(doc, "ack", msgId, sizeof(msgId));
    p["ref"] = (id != nullptr && id[0] != '\0') ? id : (const char*)nullptr;
    p["status"] = outcome.status;
    if (channelId != nullptr && channelId[0] != '\0') {
        p["channel"] = channelId;
    } else {
        p["channel"] = nullptr;
    }
    if (outcome.reason != nullptr) {
        p["reason"] = outcome.reason;
    }
    if (strcmp(outcome.status, "ok") == 0) {
        JsonObject applied = p["applied"].to<JsonObject>();
        if (outcome.hasOn) applied["on"] = outcome.on;
        if (outcome.noop) applied["noop"] = true;
        if (outcome.silenced) applied["silenced"] = true;
        if (outcome.latchesCleared) applied["latches_cleared"] = true;
        char until[24];
        if (outcome.untilEpoch > 0 &&
            isoFromEpoch(outcome.untilEpoch, until, sizeof(until))) {
            applied["until"] = until;
        } else {
            applied["until"] = nullptr;
        }
    }
    JsonArray warnings = p["warnings"].to<JsonArray>();
    if (outcome.warnings.length() > 0) {
        warnings.add(outcome.warnings);
    }
    String out;
    serializeJson(doc, out);
    return out;
}

void handleCommand(JsonObject payload, const char* id) {
    const char* channelId = payload["channel"] | "";
    const char* action = payload["action"] | "";

    if (action[0] == '\0') {
        const CommandOutcome out = reject("invalid_payload");
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        return;
    }

    String cached;
    if (findCachedAck(id, cached)) {
        s_ws.sendTXT(cached.c_str(), cached.length());
        return;
    }

    RelayChannel channel = RelayChannel::ALARMA;
    const bool hasChannel = findChannel(channelId, channel);
    const bool reportState = (strcmp(action, "report_state") == 0);
    if (!reportState && !hasChannel) {
        CommandOutcome out = reject("unsupported");
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        cacheAck(id, ack);
        queueEvent(EvCode::COMMAND_REJECTED, channelId, "unsupported");
        return;
    }

    if (rateLimited(channel, hasChannel && !reportState)) {
        CommandOutcome out = reject("rate_limited");
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        cacheAck(id, ack);
        return;
    }

    // Vencimiento: expires_at es obligatorio para comandos.
    const char* expires = payload["expires_at"] | "";
    int64_t expiresEpoch = 0;
    const bool hasExpires = parseIso(expires, expiresEpoch);
    if (!reportState && expires[0] == '\0') {
        CommandOutcome out = reject("invalid_payload");
        out.reason = "missing_expires_at";
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        cacheAck(id, ack);
        return;
    }
    if (!reportState && expires[0] != '\0' && !hasExpires && rtcIsTimeValid()) {
        CommandOutcome out = reject("invalid_payload");
        out.reason = "invalid_expires_at";
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        cacheAck(id, ack);
        queueEvent(EvCode::COMMAND_REJECTED, channelId, "invalid_expires_at");
        return;
    }

    const bool safeAction = (strcmp(action, "off") == 0) ||
                            (strcmp(action, "close") == 0) ||
                            (strcmp(action, "silence") == 0) ||
                            (strcmp(action, "reset") == 0) ||
                            (strcmp(action, "auto") == 0) ||
                            reportState;
    if (!rtcIsTimeValid() && !safeAction) {
        CommandOutcome out = reject("clock_invalid");
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        cacheAck(id, ack);
        queueEvent(EvCode::COMMAND_REJECTED, channelId, "clock_invalid");
        return;
    }
    if (!reportState && hasExpires && rtcIsTimeValid() &&
        rtcNowEpoch() >= expiresEpoch) {
        CommandOutcome out;
        out.status = "expired";
        out.reason = "expired";
        const String ack = buildAck(id, channelId, out);
        s_ws.sendTXT(ack.c_str(), ack.length());
        cacheAck(id, ack);
        queueEvent(EvCode::COMMAND_REJECTED, channelId, "expired");
        return;
    }

    const CommandOutcome outcome = executeCommand(payload, channelId, action);
    const String ack = buildAck(id, channelId, outcome);
    s_ws.sendTXT(ack.c_str(), ack.length());
    cacheAck(id, ack);

    if (strcmp(outcome.status, "ok") != 0 && outcome.reason != nullptr) {
        queueEvent(EvCode::COMMAND_REJECTED, channelId, outcome.reason);
    }
    if (strcmp(outcome.status, "ok") == 0) {
        if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0 ||
            strcmp(action, "open") == 0 || strcmp(action, "close") == 0 ||
            strcmp(action, "auto") == 0) {
            sendState();
        }
    }
}

// ----------------------------------------------------------------------------
// Mensajes del servidor
// ----------------------------------------------------------------------------

void syncClockFromIso(const char* iso) {
    int64_t serverEpoch = 0;
    if (!parseIso(iso, serverEpoch)) {
        return;
    }
    const int64_t localEpoch = rtcNowEpoch();
    int64_t diff = serverEpoch - localEpoch;
    if (diff < 0) diff = -diff;
    if (!rtcIsTimeValid() || diff > 5) {
        rtcSetEpoch(serverEpoch);
        queueEvent(EvCode::TIME_SYNCED, nullptr, nullptr, nullptr, false);
        Serial.printf("[WS] Hora sincronizada con servidor (%+lld s)\n",
                      (long long)(serverEpoch - localEpoch));
    }
}

void sendDiagnostics() {
    JsonDocument doc;
    char id[16];
    JsonObject p = beginEnvelope(doc, "event", id, sizeof(id));
    char eventId[32];
    snprintf(eventId, sizeof(eventId), "%s-%04lu", s_bootId,
             (unsigned long)(++s_eventSeq));
    p["event_id"] = eventId;
    p["code"] = "DIAGNOSTICS";
    p["severity"] = "info";
    p["channel"] = nullptr;
    p["message"] = "Diagnostico del equipo";
    JsonObject data = p["data"].to<JsonObject>();
    data["free_heap_bytes"] = ESP.getFreeHeap();
    data["uptime_s"] = millis() / 1000UL;
    data["reconnects"] = s_reconnects;
    data["pending_telemetry"] = s_telCount;
    data["pending_events"] = s_eventCount;
    data["fw_version"] = FIRMWARE_VERSION;
    data["boot_id"] = s_bootId;
    data["rssi_dbm"] = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
    p["buffered"] = false;
    sendDoc(doc);
}

void handleWelcome(JsonObject payload) {
    const char* session = payload["session_id"] | "";
    if (session[0] != '\0') {
        strncpy(s_sessionId, session, sizeof(s_sessionId) - 1);
    }
    const int heartbeat = payload["heartbeat_s"] | WS_HEARTBEAT_DEFAULT_S;
    if (heartbeat >= 5 && heartbeat <= 300) {
        s_heartbeatS = (uint16_t)heartbeat;
    }
    const int telemetry = payload["telemetry_s"] | WS_TELEMETRY_DEFAULT_S;
    if (telemetry >= WS_TELEMETRY_MIN_S && telemetry <= 3600) {
        s_telemetryS = (uint16_t)telemetry;
    }
    syncClockFromIso(payload["server_time"] | "");

    const char* token = payload["token"] | "";
    if (token[0] != '\0' && strcmp(token, settingsGetApiToken()) != 0) {
        settingsSetDeviceIdentity(settingsGetDeviceId(), token);
        Serial.println("[WS] Token rotado por el backend");
    }

    s_backoffMs = 0;
    s_lastRxMs = millis();
    s_connectedSinceMs = millis();
    s_lastTelemetryTxMs = 0;
    s_lastEventTxMs = 0;
    markBackendSeen();
    Serial.printf("[WS] welcome: session=%s heartbeat=%us telemetry=%us\n",
                  s_sessionId[0] ? s_sessionId : "(sin id)",
                  (unsigned)s_heartbeatS, (unsigned)s_telemetryS);

    sendState();
    s_lastEventTxMs = 0;  // drena eventos pendientes apenas sea posible
}

void handleConfig(JsonObject payload, const char* id) {
    const ConfigResult res = applyBackendConfig(payload);

    JsonDocument doc;
    char msgId[16];
    JsonObject p = beginEnvelope(doc, "ack", msgId, sizeof(msgId));
    p["ref"] = (id != nullptr && id[0] != '\0') ? id : (const char*)nullptr;
    p["status"] = res.ok ? "ok" : "rejected";
    p["channel"] = nullptr;
    if (res.ok) {
        p["stored"] = !res.stale;
        p["applied"] = res.applyNow && !res.stale;
        p["active_source"] = configSourceName();
        p["config_version"] = settingsGetConfigVersion();
        if (res.stale) {
            p["reason"] = "stale_version";
        }
    } else {
        p["reason"] = "invalid_payload";
        p["errors"].to<JsonArray>().add(res.error);
        queueEvent(EvCode::CONFIG_REJECTED, nullptr, "invalid_config",
                   nullptr, true);
        Serial.printf("[WS] Config rechazada: %s\n", res.error.c_str());
    }
    JsonArray warnings = p["warnings"].to<JsonArray>();
    if (res.warnings.length() > 0) {
        warnings.add(res.warnings);
    }
    sendDoc(doc);
}

void handleRequest(JsonObject payload) {
    const char* what = payload["what"] | "";
    if (strcmp(what, "state") == 0) {
        sendState();
    } else if (strcmp(what, "diagnostics") == 0) {
        sendState();
        sendDiagnostics();
    } else {
        sendError("invalid_payload", "request.what desconocido");
    }
}

void handleText(const char* text, size_t length) {
    if (length > WS_FRAME_MAX_BYTES) {
        sendError("invalid_payload", "frame demasiado grande");
        return;
    }
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, text, length);
    if (err) {
        sendError("invalid_json", "frame no parseable");
        return;
    }
    const int version = doc["v"] | 0;
    if (version != 1) {
        sendError("unsupported_version", "version de protocolo no soportada");
        return;
    }
    const char* type = doc["type"] | "";
    const char* id = doc["id"] | "";
    JsonObject payload = doc["payload"].as<JsonObject>();

    s_lastRxMs = millis();
    markBackendSeen();

    if (strcmp(type, "welcome") == 0) {
        handleWelcome(payload);
    } else if (strcmp(type, "config") == 0) {
        handleConfig(payload, id);
    } else if (strcmp(type, "command") == 0) {
        handleCommand(payload, id);
    } else if (strcmp(type, "request") == 0) {
        handleRequest(payload);
    } else if (strcmp(type, "ping") == 0) {
        syncClockFromIso(payload["server_time"] | "");
        JsonDocument pong;
        char pid[16];
        JsonObject p = beginEnvelope(pong, "pong", pid, sizeof(pid));
        const char* serverTime = payload["server_time"] | "";
        p["server_time_echo"] = (serverTime[0] != '\0') ? serverTime : (const char*)nullptr;
        p["uptime_s"] = millis() / 1000UL;
        sendDoc(pong);
    } else if (strcmp(type, "error") == 0) {
        Serial.printf("[WS] error del servidor: %s (%s)\n",
                      payload["code"] | "?",
                      payload["message"] | "");
    } else {
        sendError("unknown_type", "tipo de mensaje desconocido", id);
    }
}

// ----------------------------------------------------------------------------
// Conexión
// ----------------------------------------------------------------------------

void scheduleRetry(const char* reason) {
    rememberError(reason);
    if (s_backoffMs == 0) {
        s_backoffMs = WS_BACKOFF_BASE_MS;
    } else {
        s_backoffMs *= 2;
        if (s_backoffMs > WS_BACKOFF_MAX_MS) {
            s_backoffMs = WS_BACKOFF_MAX_MS;
        }
    }
    const uint32_t jitterMax = s_backoffMs / 5UL + 1UL;
    const uint32_t jitter = esp_random() % jitterMax;
    s_nextActionMs = millis() + s_backoffMs + jitter;
    s_state = LinkState::BACKOFF;
    Serial.printf("[WS] Reintento en %lu ms (%s)\n",
                  (unsigned long)(s_backoffMs + jitter),
                  reason ? reason : "?");
}

void onDisconnected() {
    s_ws.setReconnectInterval(WS_AUTO_RECONNECT_OFF_MS);
    if (s_closingSession) {
        s_closingSession = false;
        return;
    }
    const bool wasConnected = (s_state == LinkState::CONNECTED);
    if (wasConnected) {
        queueEvent(EvCode::WS_LOST, nullptr, "socket");
        s_reconnects++;
    }
    scheduleRetry(wasConnected ? "sesion perdida" : "conexion fallida");
}

void onConnected() {
    s_ws.setReconnectInterval(WS_AUTO_RECONNECT_OFF_MS);
    s_state = LinkState::CONNECTED;
    s_lastRxMs = millis();
    s_connectedSinceMs = millis();
    s_backoffMs = 0;
    s_sessionId[0] = '\0';
    s_lastTelemetryTxMs = 0;
    s_lastEventTxMs = 0;
    s_lastStateMs = 0;
    s_haveStateSig = false;
    rememberError("");
    queueEvent(EvCode::WS_CONNECTED, nullptr, "socket");
    Serial.printf("[WS] Conectado a %s\n", settingsGetWsUrl()[0] ? "ws_url" : "URL derivada");

    // hello
    JsonDocument doc;
    char id[16];
    JsonObject p = beginEnvelope(doc, "hello", id, sizeof(id));
    p["device_id"] = settingsGetDeviceId();
    p["fw_version"] = FIRMWARE_VERSION;
    p["hw_revision"] = "rev-A";
    p["protocol"] = 1;
    p["boot_id"] = s_bootId;
    p["uptime_s"] = millis() / 1000UL;
    p["config_version"] = settingsGetConfigVersion();
    p["local_version"] = settingsGetLocalVersion();
    p["config_source"] = configSourceName();
    p["mode"] = modeName(stateMachineGetMode());
    p["time_valid"] = rtcIsTimeValid();
    p["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
    p["rssi_dbm"] = (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
    JsonObject caps = p["capabilities"].to<JsonObject>();
    JsonArray all = caps["channels"].to<JsonArray>();
    JsonArray enabled = caps["enabled_channels"].to<JsonArray>();
    for (uint8_t i = 0; i < RELAY_CHANNEL_COUNT; i++) {
        const RelayChannel ch = (RelayChannel)i;
        all.add(relayChannelName(ch));
        if (settingsIsChannelEnabled(ch)) {
            enabled.add(relayChannelName(ch));
        }
    }
    caps["window_actuator"] = "pulse_toggle_5s";
    caps["window_limits"] = 1;
    caps["window_stop"] = false;
    sendDoc(doc);
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            onConnected();
            break;
        case WStype_DISCONNECTED:
            onDisconnected();
            break;
        case WStype_TEXT:
            if (payload != nullptr) {
                handleText((const char*)payload, length);
            }
            break;
        case WStype_BIN:
            sendError("invalid_payload", "frame binario no soportado");
            break;
        case WStype_ERROR:
            rememberError("error de socket");
            break;
        case WStype_PING:
        case WStype_PONG:
            s_lastRxMs = millis();
            break;
        default:
            break;
    }
}

void startConnection() {
    String wsUrl = deriveWsUrl();
    if (wsUrl.length() == 0) {
        rememberError("ws_url vacia");
        scheduleRetry("sin URL WebSocket");
        return;
    }
    String host;
    String path;
    uint16_t port = 80;
    bool secure = false;
    if (!parseWsUrl(wsUrl, host, port, path, secure)) {
        rememberError(secure ? "wss no soportado (usar LAN)" : "ws_url invalida");
        scheduleRetry(secure ? "wss no soportado" : "URL WebSocket invalida");
        return;
    }

    String headers = "X-Device-Id: ";
    headers += settingsGetDeviceId();
    headers += "\r\nAuthorization: Bearer ";
    headers += settingsGetApiToken();

    s_ws.disconnect();
    s_closingSession = false;
    s_ws.setReconnectInterval(0);  // el firmware decide cuándo reintentar
    s_ws.begin(host.c_str(), port, path.c_str(), "");  // sin subprotocolo
    s_ws.setExtraHeaders(headers.c_str());
    s_state = LinkState::CONNECTING;
    s_connectDeadlineMs = millis() + WS_CONNECT_TIMEOUT_MS;
    s_lastRxMs = millis();
    rememberError("");
    Serial.printf("[WS] Conectando a %s:%u%s\n", host.c_str(), port, path.c_str());
}

void flushQueues() {
    const uint32_t now = millis();

    if (s_eventCount > 0 && (s_lastEventTxMs == 0 ||
                             (now - s_lastEventTxMs) >= 250UL)) {
        if (sendOneEvent()) {
            s_lastEventTxMs = now;
        }
    }

    if (s_telCount > 0) {
        const bool due = (s_lastTelemetryTxMs == 0) ||
                         ((now - s_lastTelemetryTxMs) >=
                          (uint32_t)s_telemetryS * 1000UL);
        if (due) {
            sendTelemetryBatch();
        }
    }

    if (now - s_lastStateMs >= WS_STATE_REFRESH_MS) {
        sendState();
    } else if (!s_haveStateSig || stateSignature() != s_lastStateSig) {
        sendState();
    }
}

}  // namespace

// ============================================================================
// API pública
// ============================================================================

void wsInit() {
    const uint32_t rnd = esp_random();
    snprintf(s_bootId, sizeof(s_bootId), "%08lX", (unsigned long)rnd);
    s_state = LinkState::IDLE;
    s_closingSession = false;
    s_netWasConnected = false;
    s_eventCount = 0;
    s_telHead = 0;
    s_telCount = 0;
    s_sampleSeq = 0;
    s_eventSeq = 0;
    s_seq = 0;
    s_backoffMs = 0;
    s_nextActionMs = 0;
    s_lastError[0] = '\0';
    s_reconnects = 0;
    s_heartbeatS = WS_HEARTBEAT_DEFAULT_S;
    s_telemetryS = WS_TELEMETRY_DEFAULT_S;
    s_nvsErrorQueued = false;

    // Los eventos críticos de arranques anteriores se recuperan antes del BOOT
    // para conservar el orden cronológico y reenviarlos al reconectar.
    criticalRingLoad();
    criticalRingRestore();

    s_ws.onEvent(onWsEvent);
    queueEvent(EvCode::BOOT, nullptr, "power_on");

    const esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_INT_WDT ||
        resetReason == ESP_RST_WDT) {
        queueEvent(EvCode::WATCHDOG_RESET, nullptr, "watchdog", nullptr, true);
    }

    Serial.printf("[WS] Cliente WebSocket inicializado (boot_id=%s)\n", s_bootId);
}

void wsUpdate() {
    const bool netConnected = (WiFi.status() == WL_CONNECTED);
    if (netConnected != s_netWasConnected) {
        queueEvent(netConnected ? EvCode::WIFI_CONNECTED : EvCode::WIFI_LOST,
                   nullptr, "wifi");
        s_netWasConnected = netConnected;
    }

    trackTransitions();

    s_ws.loop();

    if (!netConnected) {
        if (s_state == LinkState::CONNECTED) {
            queueEvent(EvCode::WS_LOST, nullptr, "wifi");
            s_closingSession = true;
            s_ws.disconnect();
        }
        s_state = LinkState::IDLE;
        return;
    }

    if (s_state == LinkState::CONNECTED) {
        const uint32_t now = millis();
        const uint32_t idleLimit = (uint32_t)s_heartbeatS * 2000UL;
        if ((now - s_lastRxMs) > idleLimit) {
            queueEvent(EvCode::WS_LOST, nullptr, "heartbeat");
            rememberError("sin frames del servidor");
            s_closingSession = true;
            s_ws.disconnect();
            scheduleRetry("heartbeat perdido");
            return;
        }
        if (s_backoffMs != 0 && (now - s_connectedSinceMs) > 60000UL) {
            s_backoffMs = 0;  // sesión estable: reiniciar backoff
        }
        flushQueues();
        return;
    }

    if (s_state == LinkState::CONNECTING) {
        if (millis() > s_connectDeadlineMs) {
            s_closingSession = true;
            s_ws.disconnect();
            scheduleRetry("timeout de conexion");
        }
        return;
    }

    if (s_state == LinkState::CLAIMING) {
        // El claim es bloqueante y se ejecuta en su propia rama; este estado
        // solo se ve si una versión futura lo hace asíncrono.
        s_state = LinkState::IDLE;
    }

    if (s_state == LinkState::BACKOFF) {
        if ((int32_t)(millis() - s_nextActionMs) < 0) {
            return;
        }
        s_state = LinkState::IDLE;
    }

    if (s_state != LinkState::IDLE) {
        return;
    }

    const String base = backendBase();
    if (base.length() == 0) {
        if (!s_noBackendLogged) {
            Serial.println("[WS] Sin backend_url: operacion local only");
            s_noBackendLogged = true;
        }
        rememberError("backend_url vacia");
        return;
    }
    s_noBackendLogged = false;

    if (!settingsHasDeviceIdentity()) {
        if (millis() < s_nextClaimMs) {
            return;
        }
        s_state = LinkState::CLAIMING;
        const bool ok = claimDevice();
        s_state = LinkState::IDLE;
        if (!ok) {
            if (s_nextClaimMs == 0) {
                s_nextClaimMs = millis() + WS_CLAIM_RETRY_MS;
            }
        }
        return;
    }

    startConnection();
}

void wsNotifyPortalEnter() {
    if (s_state == LinkState::CONNECTED) {
        queueEvent(EvCode::WS_LOST, nullptr, "portal");
    }
    queueEvent(EvCode::PORTAL_ENTER, nullptr, "portal");
    s_closingSession = true;
    s_ws.disconnect();
    s_state = LinkState::IDLE;
    s_backoffMs = 0;
    s_nextActionMs = 0;
    s_lastError[0] = '\0';
}

void wsNotifyPortalExit() {
    queueEvent(EvCode::PORTAL_EXIT, nullptr, "portal");
    s_backoffMs = 0;
    s_nextActionMs = millis();
    s_state = LinkState::IDLE;
    s_netWasConnected = (WiFi.status() == WL_CONNECTED);
}

void wsDisconnect() {
    s_closingSession = true;
    s_ws.disconnect();
    s_state = LinkState::IDLE;
}

void wsRecordSample(const struct tm& nowUtc, const SensorReadings& readings) {
    Sample s = {};
    s.epoch = rtcTmToEpoch(nowUtc);
    s.timeValid = rtcIsTimeValid();
    s.seq = ++s_sampleSeq;
    s.uptimeS = millis() / 1000UL;
    s.rssi = (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : 0;
    s.windowState = (uint8_t)windowGetState();

    s.airValid = readings.air.valid;
    if (s.airValid) {
        s.airTempC = readings.air.tempC;
        s.airRhPct = readings.air.humidityPct;
    }

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        s.soilValid[z] = readings.soil[z].valid && readings.soilTemp[z].valid;
        if (s.soilValid[z]) {
            s.soilMoistPct[z] = readings.soil[z].humidityPct;
            s.soilTempC[z] = readings.soilTemp[z].tempC;
        }
    }

    s.bedValid = readings.bedTemp.valid;
    if (s.bedValid) {
        s.bedTempC = readings.bedTemp.tempC;
    }

    pushSample(s);
    Serial.printf("[WS] Muestra encolada seq=%lu (pendientes=%u)\n",
                  (unsigned long)s.seq, (unsigned)s_telCount);
}

bool wsIsConnected() {
    return s_state == LinkState::CONNECTED;
}

const char* wsStateName() {
    switch (s_state) {
        case LinkState::IDLE:       return "idle";
        case LinkState::CLAIMING:   return "claim";
        case LinkState::CONNECTING: return "connecting";
        case LinkState::CONNECTED:  return "connected";
        case LinkState::BACKOFF:    return "backoff";
    }
    return "?";
}

const char* wsLastError() {
    return s_lastError;
}

uint32_t wsReconnectCount() {
    return s_reconnects;
}

uint16_t wsPendingSamples() {
    return s_telCount;
}

uint16_t wsPendingEvents() {
    return s_eventCount;
}

int64_t wsGetLastBackendSeenEpoch() {
    return s_lastBackendSeenEpoch;
}
