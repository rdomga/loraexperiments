#include "dust_sensor.h"
#include <stdlib.h>

static int dustSamples[DUST_SAMPLES];

static int dust_compareInt(const void* a, const void* b) {
    return (*(const int*)a) - (*(const int*)b);
}

void dust_Init() {
    pinMode(DUST_LED_PIN, OUTPUT);
    digitalWrite(DUST_LED_PIN, HIGH); // LED off (active LOW)
    pinMode(DUST_ADC_PIN, INPUT);
    Serial.println(F("GP2Y1010AU0F initialized"));
}

int dust_getADC(bool* valid) {
    unsigned long nextCycle = micros();
    const unsigned long cycleUs = DUST_CYCLE_MS * 1000UL;

    for (int i = 0; i < DUST_SAMPLES; i++) {
        // Pulse LED, sample at the 280µs sweet spot per Sharp datasheet
        digitalWrite(DUST_LED_PIN, LOW);
        delayMicroseconds(DUST_LED_DELAY_US);
        dustSamples[i] = analogRead(DUST_ADC_PIN);
        delayMicroseconds(DUST_LED_HOLD_US);
        digitalWrite(DUST_LED_PIN, HIGH);

        nextCycle += cycleUs;
        while ((long)(nextCycle - micros()) > 0) {
            // busy-wait to maintain a consistent 10ms cycle
        }
    }

    // Trim the lowest/highest DUST_TRIM_PERCENT% before averaging, to reject
    // outlier pulses without discarding genuine signal (validated against
    // real dust events in GP2Y_Debug before porting here).
    qsort(dustSamples, DUST_SAMPLES, sizeof(int), dust_compareInt);
    int trimCount = (DUST_SAMPLES * DUST_TRIM_PERCENT) / 100;
    int trimmedN = DUST_SAMPLES - (2 * trimCount);
    long sum = 0;
    for (int i = trimCount; i < DUST_SAMPLES - trimCount; i++) sum += dustSamples[i];
    int trimmedMean = (int)(sum / trimmedN);

    int rawMin = dustSamples[0];
    int rawMax = dustSamples[DUST_SAMPLES - 1];
    // Pinned to a rail, or flat with no pulse-to-pulse jitter: sensor
    // absent/faulted — but the reading is still real, so it's still returned.
    *valid = !(trimmedMean <= DUST_ADC_MIN_VALID || trimmedMean >= DUST_ADC_MAX_VALID ||
               (rawMax - rawMin) <= DUST_SPREAD_MIN_VALID);
    return trimmedMean;
}
