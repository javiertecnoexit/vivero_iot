#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>
#include <time.h>

#include "config.h"

// ============================================================================
// sensors.h — API de lectura de sensores (M2)
// Fuente: docs/01_hardware_pinout.md §4, docs/02_protocolo_ws.md §4.2.
// - Humedad de sustrato: ADC1 GPIO34/35, alimentación conmutada GPIO12.
// - Temperatura: 3 DS18B20 con un pin por sonda (GPIO16 Z1, GPIO32 Z2,
//   GPIO33 cama). Sin bus compartido ni mapeo por dirección.
// - Aire: DHT21 en GPIO4.
// - Ventana: sensor de cerrada en GPIO13 (lo maneja window.cpp).
// ============================================================================

struct SoilReading {
    bool     valid;       // false si el ADC quedó en riel (sensor ausente)
    uint16_t rawAdc;      // valor crudo promediado (0-4095)
    float    humidityPct; // 0-100 %, interpolado entre dry_raw/wet_raw
};

struct SoilTempReading {
    bool  valid;
    float tempC;
};

struct AirReading {
    bool  valid;
    float tempC;
    float humidityPct;
};

struct SensorReadings {
    struct tm        timestamp;   // RTC (UTC)
    SoilReading      soil[MAX_SUBSTRATE_ZONES];
    SoilTempReading  soilTemp[MAX_SUBSTRATE_ZONES];
    SoilTempReading  bedTemp;     // DS18B20 de cama caliente
    AirReading       air;
};

// Inicializa calibración, los 3 buses DS18B20, el DHT21 y el primer request
// de temperatura (no bloqueante).
void sensorsInit();

// Lee todos los sensores y devuelve el estado completo. Se llama desde el
// ciclo local (cada 30 s). No bloquea salvo la estabilización del ADC.
SensorReadings sensorsRead();

// Imprime por Serial el estado completo de las lecturas.
void sensorsPrint(const SensorReadings& readings);

// Diagnóstico DS18B20 desde el portal, en 2 fases para no congelar el loop:
//   1) sensorsRequestAllTemps()  → dispara las 3 conversiones en paralelo.
//   2) esperar ~750 ms una sola vez mientras se leen otros sensores.
//   3) sensorsReadDs18b20Ready() → lee la conversión ya completada.
// index: 0 = Z1, 1 = Z2, 2 = cama caliente.
void sensorsRequestAllTemps();
bool sensorsReadDs18b20Ready(uint8_t index, float& tempC);

// Lee el ADC crudo promedio de una zona de sustrato, energizando GPIO12 y
// esperando la estabilización. Devuelve 0-4095; 0 si la zona es inválida.
uint16_t sensorsReadSoilRawAdc(uint8_t zone);

// Enciende/apaga la alimentación conmutada de los sensores de humedad.
void sensorsSoilPowerSet(bool on);

#endif // SENSORS_H
