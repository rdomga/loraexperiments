#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <Arduino.h>
#include "dht22_sensor.h"
#include "dust_sensor.h"
#include "gas_sensor.h"
#include "battery_sensor.h"
#include "payload_encoder.h"
#include "build_config.h"
#include "power_manager.h"

extern uint8_t g_sensorMask;
extern bool g_gasOffBeforeBatteryRead;

extern float g_temp;
extern float g_hum;
extern int g_adc;
extern bool g_adc_valid;
extern int g_gas;
extern bool g_gas_valid;
extern int g_batt;
extern bool g_batt_valid;

extern bool lastSensorValid;
extern int16_t last_temp_s;
extern uint8_t last_hum_s;
extern uint16_t last_adc_s;
extern uint16_t last_gas_s;
extern uint16_t last_batt_s;

void sensorManager_init();
void sensorManager_readAlwaysOn();
void sensorManager_readGatedNormal(bool includeGas);
void sensorManager_readForMode(uint8_t mode);

void sensorManager_updateLastTransmitted();

#endif // SENSOR_MANAGER_H
