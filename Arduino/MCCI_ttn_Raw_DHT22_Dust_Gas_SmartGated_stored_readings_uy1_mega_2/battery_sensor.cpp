#include "battery_sensor.h"

#ifdef NIGHT_BENCH_ADC
// Bench only (-DNIGHT_BENCH_ADC): the battery reading comes from g_benchAdc, set over serial with /@Y<adc># (serial_console.cpp),
// so a board with no divider (or no battery) can walk through day, night, critical, boost-sag and morning across resets.
// Stored in .noinit with a magic tag so /@Y<adc># persists across /@R# and DTR resets while USB stays powered.
#define BENCH_ADC_MAGIC 0xB5A7U
int      g_benchAdc      __attribute__((section(".noinit")));
uint16_t g_benchAdcMagic __attribute__((section(".noinit")));
#endif

void battery_Init() {
    pinMode(BATTERY_ADC_PIN, INPUT);
    Serial.println(F("Battery divider initialized"));
#ifdef NIGHT_BENCH_ADC
    if (g_benchAdcMagic != BENCH_ADC_MAGIC) {
        g_benchAdc = 392;
        g_benchAdcMagic = BENCH_ADC_MAGIC;
    }
    Serial.print(F("BENCH: simulated boot battery ADC = "));
    Serial.println(g_benchAdc);
#endif
}

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
