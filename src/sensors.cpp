#include "sensors.h"

#include <Arduino.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <OneWire.h>

#include "calibration.h"
#include "rtc.h"
#include "settings.h"

// ============================================================================
// sensors.cpp — Lectura de sensores (M2)
// - Humedad: ADC1 GPIO34/35, alimentación conmutada GPIO12, 12 muestras.
// - Temperatura: 3 DS18B20 con un pin por sonda, no bloqueante en 2 fases
//   (leer el request del ciclo anterior y disparar el del próximo).
// - Aire: DHT21 en GPIO4.
// ============================================================================

// Pines ADC de humedad por zona (0-based → GPIO34 = Z1, GPIO35 = Z2).
static const uint8_t SOIL_ADC_PINS[MAX_SUBSTRATE_ZONES] = {
    PIN_SOIL_ADC_1, PIN_SOIL_ADC_2
};

#if AIR_SENSOR_MODEL == 1
static DHT s_dht(PIN_DHT21, DHT11);    // solo banco/pruebas
#else
static DHT s_dht(PIN_DHT21, DHT21);    // sensor final del proyecto
#endif

// Un bus OneWire por sonda: el rol queda fijado por el pin.
static OneWire s_wireZ1(PIN_DS18B20_Z1);
static OneWire s_wireZ2(PIN_DS18B20_Z2);
static OneWire s_wireBed(PIN_DS18B20_CAMA);

static DallasTemperature s_dallasZ1(&s_wireZ1);
static DallasTemperature s_dallasZ2(&s_wireZ2);
static DallasTemperature s_dallasBed(&s_wireBed);

static DallasTemperature* const DALLAS[MAX_DS18B20] = {
    &s_dallasZ1, &s_dallasZ2, &s_dallasBed
};

static const char* const DS18B20_NAMES[MAX_DS18B20] = {
    "sustrato Z1", "sustrato Z2", "cama caliente"
};

static bool s_dhtInit = false;

// Hay un request de conversión en vuelo disparado en el ciclo anterior.
static bool s_requestOutstanding = false;

// ----------------------------------------------------------------------------
// Humedad de sustrato
// ----------------------------------------------------------------------------

// Convierte el ADC crudo a % interpolando entre dry/wet sin asumir cuál es
// mayor. Resultado limitado a 0-100 (%).
static float rawToPercent(int raw, const ZoneCalibration& cal) {
    const int span = (int)cal.dryRaw - (int)cal.wetRaw;
    if (span == 0) {
        return 50.0f;  // rango inválido: valor neutro
    }
    float pct = 100.0f * ((int)cal.dryRaw - raw) / (float)span;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return pct;
}

static uint16_t readSoilAdc(uint8_t pin) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < SOIL_ADC_SAMPLES; i++) {
        sum += analogRead(pin);
    }
    return (uint16_t)(sum / SOIL_ADC_SAMPLES);
}

static void readSoil(SoilReading readings[]) {
    // Encender alimentación de los sensores de humedad.
    digitalWrite(PIN_SENSOR_POWER, HIGH);

    // Estabilización tras energizar: 20-30 ms. delay() intencional y acotado.
    delay(SOIL_STABILIZE_MS);

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        const ZoneCalibration cal = calibrationGet(z);
        const uint16_t raw = readSoilAdc(SOIL_ADC_PINS[z]);

        // Un ADC en riel (0 o 4095) indica sensor ausente/cable roto.
        readings[z].valid       = (raw > 0 && raw < 4095);
        readings[z].rawAdc      = raw;
        readings[z].humidityPct = rawToPercent(raw, cal);
    }

    // Apagar alimentación de los sensores de humedad.
    digitalWrite(PIN_SENSOR_POWER, LOW);
}

// ----------------------------------------------------------------------------
// Temperatura — 3 DS18B20, un pin por sonda
// ----------------------------------------------------------------------------

static bool tempIsPlausible(float t) {
    if (t == DEVICE_DISCONNECTED_C) {
        return false;
    }
    return (t > -55.0f && t < 125.0f);
}

static void readOneTemp(DallasTemperature& dallas, SoilTempReading& out) {
    out.valid = false;
    out.tempC = 0.0f;

    const float t = dallas.getTempCByIndex(0);
    if (tempIsPlausible(t)) {
        out.valid = true;
        out.tempC = t;
    }
}

static void requestTempsNow() {
    for (uint8_t i = 0; i < MAX_DS18B20; i++) {
        DALLAS[i]->requestTemperatures();  // no bloqueante (wait=false)
    }
    s_requestOutstanding = true;
}

static void readTemps(SoilTempReading substrate[], SoilTempReading& bed) {
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        substrate[z].valid = false;
        substrate[z].tempC = 0.0f;
    }
    bed.valid = false;
    bed.tempC = 0.0f;

    if (s_requestOutstanding) {
        // La conversión del ciclo anterior (~750 ms) ya terminó.
        readOneTemp(s_dallasZ1, substrate[0]);
        readOneTemp(s_dallasZ2, substrate[1]);
        readOneTemp(s_dallasBed, bed);
        s_requestOutstanding = false;
    }

    // Disparar el request para el próximo ciclo.
    requestTempsNow();
}

// ----------------------------------------------------------------------------
// Aire (DHT21)
// ----------------------------------------------------------------------------

static void readAir(AirReading& air) {
    air.valid       = false;
    air.tempC       = 0.0f;
    air.humidityPct = 0.0f;

    if (!s_dhtInit) {
        return;
    }

    const float h = s_dht.readHumidity();
    const float t = s_dht.readTemperature();
    if (!isnan(h) && !isnan(t)) {
        if (h >= 0.0f && h <= 100.0f && t > -40.0f && t < 80.0f) {
            air.valid       = true;
            air.tempC       = t;
            air.humidityPct = h;
        }
    }
}

// ----------------------------------------------------------------------------
// API pública
// ----------------------------------------------------------------------------

void sensorsInit() {
    calibrationInit();

    // GPIO12 (strapping MTDI): dejarlo bajo ANTES de configurarlo como salida.
    digitalWrite(PIN_SENSOR_POWER, LOW);
    pinMode(PIN_SENSOR_POWER, OUTPUT);
    digitalWrite(PIN_SENSOR_POWER, LOW);

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        pinMode(SOIL_ADC_PINS[z], INPUT);
    }

    // 3 DS18B20: no bloqueante (setWaitForConversion(false)).
    for (uint8_t i = 0; i < MAX_DS18B20; i++) {
        DALLAS[i]->begin();
        DALLAS[i]->setWaitForConversion(false);
        DALLAS[i]->setResolution(12);
    }

    // DHT21.
    s_dht.begin();
    s_dhtInit = true;

    // Primer request de temperatura (se leerá en el primer ciclo local).
    s_requestOutstanding = false;
    requestTempsNow();

    Serial.println("[SENS] Sensores inicializados (DHT21, 2 humedad, 3 DS18B20)");
}

SensorReadings sensorsRead() {
    SensorReadings r = {};
    r.timestamp = rtcNow();

    readSoil(r.soil);
    readTemps(r.soilTemp, r.bedTemp);
    readAir(r.air);

    return r;
}

void sensorsPrint(const SensorReadings& r) {
    Serial.println("--- Lecturas de sensores ---");

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        const SoilReading& s = r.soil[z];
        Serial.printf("  Humedad Z%u: %s ADC=%u (%.1f%%)\n",
                      z + 1, s.valid ? "OK" : "FAIL", s.rawAdc, s.humidityPct);
    }
    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        const SoilTempReading& t = r.soilTemp[z];
        Serial.printf("  Temp sustrato Z%u: %s %.1f C\n",
                      z + 1, t.valid ? "OK" : "FAIL", t.tempC);
    }
    Serial.printf("  Temp cama: %s %.1f C\n",
                  r.bedTemp.valid ? "OK" : "FAIL", r.bedTemp.tempC);

    const AirReading& a = r.air;
    Serial.printf("  Aire: %s T=%.1f C H=%.1f %%\n",
                  a.valid ? "OK" : "FAIL", a.tempC, a.humidityPct);
}

// Diagnóstico desde el portal: las 3 sondas tienen bus OneWire propio, así que
// convierten EN PARALELO. Un solo request y UNA sola espera cubren a las tres;
// antes se esperaba 800 ms por sonda (2,4 s) y eso congelaba el loop principal
// (botones, corte de relés y alarmas) durante todo el diagnóstico.
void sensorsRequestAllTemps() {
    for (uint8_t i = 0; i < MAX_DS18B20; i++) {
        DALLAS[i]->requestTemperatures();
    }
    // Se deja el request "en vuelo" para que el próximo ciclo local lea esta
    // conversión. Marcarlo como consumido hacía que el ciclo siguiente
    // reportara las 3 sondas como inválidas.
    s_requestOutstanding = true;
}

// Lee la conversión ya completada (~750 ms tras el request). No dispara ni
// espera: el caller controla la espera.
bool sensorsReadDs18b20Ready(uint8_t index, float& tempC) {
    tempC = 0.0f;
    if (index >= MAX_DS18B20) {
        return false;
    }

    const float t = DALLAS[index]->getTempCByIndex(0);

    Serial.printf("[SENS] DS18B20 %s: %s %.1f C\n",
                  DS18B20_NAMES[index], tempIsPlausible(t) ? "OK" : "FAIL", t);
    if (!tempIsPlausible(t)) {
        return false;
    }
    tempC = t;
    return true;
}

uint16_t sensorsReadSoilRawAdc(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) {
        return 0;
    }
    digitalWrite(PIN_SENSOR_POWER, HIGH);
    delay(SOIL_STABILIZE_MS);  // excepción acotada, igual que readSoil()
    const uint16_t raw = readSoilAdc(SOIL_ADC_PINS[zone]);
    digitalWrite(PIN_SENSOR_POWER, LOW);
    return raw;
}

void sensorsSoilPowerSet(bool on) {
    digitalWrite(PIN_SENSOR_POWER, on ? HIGH : LOW);
}
