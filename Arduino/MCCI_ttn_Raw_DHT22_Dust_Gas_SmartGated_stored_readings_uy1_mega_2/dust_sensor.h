#ifndef DUST_SENSOR_H
#define DUST_SENSOR_H

#include "Arduino.h"

// GP2Y1010AU0F — Sharp optical dust sensor
// D2 is reserved by LMIC DIO0, so LED control is on D5
#define DUST_LED_PIN        5
#define DUST_ADC_PIN        A3
#define DUST_SAMPLES        100   // pulses averaged per transmission (~1s window)
#define DUST_LED_DELAY_US   280   // µs from LED on to sampling point (datasheet)
#define DUST_LED_HOLD_US    40    // µs to hold LED on after sampling, before turning it off (datasheet)
#define DUST_CYCLE_MS       10    // ms per sample cycle (10ms = LED pulse + rest)
#define DUST_TRIM_PERCENT   10    // % of samples dropped from each end (low + high) before averaging

// Fault detection: no pull resistor guarantees a deterministic "disconnected"
// rail here (unlike gas/battery), so a faulted/floating Vo can land anywhere —
// seen both pinned near 0 and floating mid-range (~1.3-1.4V) in practice. The
// one thing a live sensor never does is read perfectly flat: real pulses
// always carry some jitter (~20-80 raw counts of spread observed on healthy
// units). So flatness (near-zero spread), not just the absolute level, is
// the reliable tell. Bounds below combine both: absolute pin/short bounds
// (DUST_ADC_MIN/MAX_VALID) catch known extreme incidents (0V and the
// original 4.52V/raw~925 pegged-high fault), and DUST_SPREAD_MIN_VALID
// catches a flat reading regardless of which level it's flat at.
#define DUST_ADC_MIN_VALID     2    // at/below this = pinned near GND (dead/disconnected output)
#define DUST_ADC_MAX_VALID     900  // at/above this = pinned near VCC (floating/short)
#define DUST_SPREAD_MIN_VALID  5    // (max-min) at/below this across the 100-pulse window = suspiciously flat

void  dust_Init();
// Always returns the real averaged raw ADC (0-1023) — never hides it behind a
// sentinel. *valid reflects this device's own bounds/flatness check, kept
// only for on-device use; downstream (TTN decoder) makes its own validity
// call from the raw value, independent of this flag.
int   dust_getADC(bool* valid);

#endif
