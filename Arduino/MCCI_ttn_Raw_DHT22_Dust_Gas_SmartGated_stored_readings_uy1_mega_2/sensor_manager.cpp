#include "sensor_manager.h"

uint8_t g_sensorMask = 0x0F;
bool g_gasOffBeforeBatteryRead = false;

float g_temp = 0;
float g_hum  = 0;
int   g_adc  = 0;
bool  g_adc_valid = false;
int   g_gas  = 0;
bool  g_gas_valid = false;
int   g_batt = 0;
bool  g_batt_valid = false;

bool lastSensorValid = false;
int16_t last_temp_s = INT16_MIN;
uint8_t last_hum_s = 0xFF;
uint16_t last_adc_s = 0;
uint16_t last_gas_s = 0;
uint16_t last_batt_s = 0;

void sensorManager_init() {
    dht22_Init();
    dust_Init();
    gas_Init();
}

void sensorManager_readAlwaysOn() {
    if (g_sensorMask & 0x01) {
        g_temp = dht22_getTemperature();
        g_hum  = dht22_getHumidity();
    } else {
        g_temp = NAN;
        g_hum  = NAN;
    }
    if (g_sensorMask & 0x02) {
        g_adc = dust_getADC(&g_adc_valid);
    } else {
        g_adc = 0;
        g_adc_valid = false;
    }
    if (g_sensorMask & 0x04) {
        g_gas = gas_getADC(&g_gas_valid);
    } else {
        g_gas = 0;
        g_gas_valid = false;
    }
    if (g_sensorMask & 0x08) {
        g_batt = battery_getADC(&g_batt_valid);
    } else {
        g_batt = 0;
        g_batt_valid = false;
    }
    lastSensorValid = !isnan(g_temp) && !isnan(g_hum);
}

void sensorManager_readGatedNormal(bool includeGas) {
    pmos1Set(true);
    smartSleep(PMOS1_SETTLE_MS);
    if (g_sensorMask & 0x01) {
        g_temp = dht22_getTemperature();
        g_hum  = dht22_getHumidity();
    } else {
        g_temp = NAN;
        g_hum  = NAN;
    }
    if (g_sensorMask & 0x02) {
        g_adc = dust_getADC(&g_adc_valid);
    } else {
        g_adc = 0;
        g_adc_valid = false;
    }
    pmos1Set(false);

    bool turnGasOffNow = g_gasOffBeforeBatteryRead;
    g_gasOffBeforeBatteryRead = false;

    if (includeGas && (g_sensorMask & 0x04)) {
        if (!g_gasPmosOn) {
            gasPowerSet(true);
            gas_NotifyPoweredOn();
        }
        g_gas = gas_getADC(&g_gas_valid);
        if (turnGasOffNow) gasPowerSet(false);
    } else {
        g_gas = 0;
        g_gas_valid = false;
        // Never leave the heater energised when this cycle skipped gas.
        // The old `includeGas && turnGasOffNow` guard skipped the off action
        // precisely when includeGas was false, so the rail stayed on through
        // the battery read and the TX/RX window - the weak-cell brownout
        // condition this interlock exists to prevent.
        if (g_gasPmosOn) gasPowerSet(false);
        g_gasOffBeforeBatteryRead = false;
    }

    pmos1Set(false);
    smartSleep(BATTERY_POST_GATE_SETTLE_MS);
    if (g_sensorMask & 0x08) {
        g_batt = battery_getADC(&g_batt_valid);
    } else {
        g_batt = 0;
        g_batt_valid = false;
    }

    lastSensorValid = !isnan(g_temp) && !isnan(g_hum);
}

void sensorManager_readForMode(uint8_t mode) {
    switch (mode) {
        case MODE_ALWAYS_ON:     sensorManager_readAlwaysOn();         break;
        case MODE_NORMAL_NO_GAS: sensorManager_readGatedNormal(false); break;
        case MODE_NORMAL:
        default:                 sensorManager_readGatedNormal(true);  break;
    }
}

void sensorManager_updateLastTransmitted() {
    last_temp_s = lastSensorValid ? (int16_t)(g_temp * 10) : INT16_MIN;
    last_hum_s  = lastSensorValid ? (uint8_t)(g_hum  * 2) : 0xFF;
    last_adc_s  = (uint16_t)g_adc;
    last_gas_s  = (uint16_t)g_gas;
    last_batt_s = (uint16_t)g_batt;
}
