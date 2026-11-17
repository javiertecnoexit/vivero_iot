#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdint.h>

#include "config.h"

// ============================================================================
// calibration.h — Calibración de sensores de humedad por zona (Milestone 2)
// Fuente: especificacion_firmware.md sección 8.
// - Cada zona guarda su propio par dry_raw/wet_raw + flag calibrado.
// - Persistido en NVS con Preferences. Defaults de fábrica: rango común
//   aproximado con calibrado=false (sección 11). La UI de calibración del
//   portal llega en un milestone posterior.
// ============================================================================

struct ZoneCalibration {
    uint16_t dryRaw;
    uint16_t wetRaw;
    bool     calibrated;
};

// Inicializa la calibración: carga desde NVS o aplica defaults de fábrica.
void calibrationInit();

// Devuelve la calibración de la zona indicada (0-based).
// La zona se valida contra MAX_SUBSTRATE_ZONES.
ZoneCalibration calibrationGet(uint8_t zone);

// Persiste la calibración de la zona indicada en NVS.
// Marca calibrado=true la primera vez que ambos valores quedan definidos (§8.4).
void calibrationSet(uint8_t zone, const ZoneCalibration& cal);

// Devuelve true si la zona tiene calibrado=true (rango propio definido).
bool calibrationIsCalibrated(uint8_t zone);

// Borra la calibración persistida y restaura los rangos de fábrica.
void calibrationFactoryReset();

#endif // CALIBRATION_H