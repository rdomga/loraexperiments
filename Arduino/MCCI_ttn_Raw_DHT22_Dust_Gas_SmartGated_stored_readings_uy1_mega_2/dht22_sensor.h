#ifndef DHT22_SENSOR_H
#define DHT22_SENSOR_H

#include "Arduino.h"
#include <DHT.h>

#define DHT22_PIN  3
#define DHT22_TYPE DHT22

extern DHT dht22;

void  dht22_Init();
float dht22_getTemperature();   // °C, NaN on read failure
float dht22_getHumidity();      // %, NaN on read failure

#endif
