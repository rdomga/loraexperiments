#include "gas_sensor.h"
#include "power_manager.h"  // nowMs(): millis() freezes during sleep, so warm-up timing uses the sleep-aware clock

static unsigned long gasBootMs = 0;
static unsigned long gasWarmupSeconds = GAS_WARMUP_S_DEFAULT;

void gas_Init() {
    pinMode(GAS_PIN, INPUT);
    gasBootMs = nowMs();
}

void gas_NotifyPoweredOn() {
    gasBootMs = nowMs();
}

void gas_setWarmupSeconds(unsigned long seconds) {
    gasWarmupSeconds = seconds;
}

unsigned long gas_getWarmupSeconds() {
    return gasWarmupSeconds;
}

int gas_getADC(bool* valid) {
    long sum = 0;
    for (int i = 0; i < GAS_SAMPLES; i++) {
        sum += analogRead(GAS_PIN);
        delay(GAS_SAMPLE_INTERVAL_MS);
    }
    int avg = (int)(sum / GAS_SAMPLES);

    bool inRange = (avg > GAS_ADC_MIN_VALID) && (avg < GAS_ADC_MAX_VALID);
    bool warmedUp = (nowMs() - gasBootMs) >= gasWarmupSeconds * 1000UL;
    *valid = inRange && warmedUp; // sensor absent/faulted, or still settling
    return avg;
}
