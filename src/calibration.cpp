#include "calibration.h"

#include <Arduino.h>
#include <Preferences.h>

// ============================================================================
// calibration.cpp — Calibración de humedad por zona, persistida en NVS
// Fuente: especificacion_firmware.md sección 8 y 9.
// ============================================================================

static const char* NVS_NAMESPACE = "calib";

static ZoneCalibration s_cal[MAX_SUBSTRATE_ZONES];

static void loadFromNvs() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, true);  // read-only

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        char keyDry[16], keyWet[16], keyCal[16];
        snprintf(keyDry, sizeof(keyDry), "dry%u", z);
        snprintf(keyWet, sizeof(keyWet), "wet%u", z);
        snprintf(keyCal, sizeof(keyCal), "cal%u", z);

        // Si no existe la clave, getUInt devuelve el default (rango de fábrica).
        s_cal[z].dryRaw     = (uint16_t)prefs.getUInt(keyDry, SOIL_DEFAULT_DRY_RAW);
        s_cal[z].wetRaw     = (uint16_t)prefs.getUInt(keyWet, SOIL_DEFAULT_WET_RAW);
        s_cal[z].calibrated = prefs.getBool(keyCal, false);
    }

    prefs.end();
}

void calibrationInit() {
    loadFromNvs();
    Serial.println("[CALIB] Calibración cargada (NVS o defaults de fábrica)");
}

ZoneCalibration calibrationGet(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) {
        // Zona fuera de rango: devolver defaults de fábrica.
        return { SOIL_DEFAULT_DRY_RAW, SOIL_DEFAULT_WET_RAW, false };
    }
    return s_cal[zone];
}

void calibrationSet(uint8_t zone, const ZoneCalibration& cal) {
    if (zone >= MAX_SUBSTRATE_ZONES) {
        return;
    }

    s_cal[zone] = cal;
    // §8.4: al guardar por primera vez ambos valores, marcar calibrado=true.
    if (cal.dryRaw != 0 && cal.wetRaw != 0) {
        s_cal[zone].calibrated = true;
    }

    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);  // read-write

    char keyDry[16], keyWet[16], keyCal[16];
    snprintf(keyDry, sizeof(keyDry), "dry%u", zone);
    snprintf(keyWet, sizeof(keyWet), "wet%u", zone);
    snprintf(keyCal, sizeof(keyCal), "cal%u", zone);

    prefs.putUInt(keyDry, s_cal[zone].dryRaw);
    prefs.putUInt(keyWet, s_cal[zone].wetRaw);
    prefs.putBool(keyCal, s_cal[zone].calibrated);

    prefs.end();
}

bool calibrationIsCalibrated(uint8_t zone) {
    if (zone >= MAX_SUBSTRATE_ZONES) {
        return false;
    }
    return s_cal[zone].calibrated;
}

void calibrationFactoryReset() {
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();

    for (uint8_t z = 0; z < MAX_SUBSTRATE_ZONES; z++) {
        s_cal[z].dryRaw = SOIL_DEFAULT_DRY_RAW;
        s_cal[z].wetRaw = SOIL_DEFAULT_WET_RAW;
        s_cal[z].calibrated = false;
    }

    Serial.println("[CALIB] Reset de fabrica: calibracion borrada");
}