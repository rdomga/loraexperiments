#include "battery_sensor.h"

void battery_Init() {
    pinMode(BATTERY_ADC_PIN, INPUT);
    Serial.println(F("Battery divider initialized"));
}

#ifdef NIGHT_BENCH_ADC
// Bench only (-DNIGHT_BENCH_ADC): the battery reading comes from g_benchAdc, set over serial with /@Y<adc># (serial_console.cpp),
// so a board with no divider (or no battery) can walk through day, night, critical and morning. Resets to 392 (a
// daytime, unloaded cell) at every boot, like the real reading right after a reset.
int g_benchAdc = 392;
#endif

int battery_getADC(bool* valid) {
#ifdef NIGHT_BENCH_ADC
    *valid = (g_benchAdc > BATTERY_ADC_MIN_VALID) && (g_benchAdc < BATTERY_ADC_MAX_VALID);
    return g_benchAdc;
#endif
    analogRead(BATTERY_ADC_PIN); // discard, kicks off the mux switch to this channel
    delay(BATTERY_SETTLE_MS);    // let the 100nF filter cap charge through the ~50k divider before trusting any reading
    long sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        sum += analogRead(BATTERY_ADC_PIN);
        delay(BATTERY_SAMPLE_INTERVAL_MS);
    }
    int avg = (int)(sum / BATTERY_SAMPLES);

    // avg <= MIN: R1 (battery+) path open — R2 alone pulls the pin near GND.
    // avg >= MAX: implausible for this divider/cell — brownout-adjacent ADC
    // artifact, not a real voltage. Either way, the reading is still real and
    // still returned; *valid is just this device's own plausibility call.
    *valid = (avg > BATTERY_ADC_MIN_VALID) && (avg < BATTERY_ADC_MAX_VALID);
    return avg;
}
