#include "dht22_sensor.h"

DHT dht22(DHT22_PIN, DHT22_TYPE);

void dht22_Init() {
    pinMode(DHT22_PIN, INPUT_PULLUP);
    dht22.begin();
    Serial.println(F("DHT22 initialized"));
}

float dht22_getTemperature() {
    return dht22.readTemperature();
}

float dht22_getHumidity() {
    return dht22.readHumidity();
}
