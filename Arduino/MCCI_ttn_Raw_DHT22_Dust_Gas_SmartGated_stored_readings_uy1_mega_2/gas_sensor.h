#ifndef GAS_SENSOR_H
#define GAS_SENSOR_H

#include "Arduino.h"

// Gas sensor slot. MQ-9 (combustible gas / CO, ~300mA measured heater draw)
// is being swapped out — it collapses the MT3608 boost regardless of
// duty-cycling (see MQ9_Boost_Power_Limitation_2026-09-01.html in the
// sibling AlwaysOn folder) — for a lower-draw MEMS CO sensor (DFRobot
// SEN0440/MiCS-5524 or SEN0564; both ordered, not yet bench-compared — see
// Node_Parts_BuyList_2026-09-01.html). Current draw is unmeasured for both:
// ~32mA is the MiCS-5524 chip's heater figure, the SEN0440 board lists 0.45W,
// the SEN0564 lists <20mA. SEN0440 also has an active-low EN pin (see the
// 2026-10-01 correction in Gas_Scheduling_And_LowBattery_Handoff_2026-09-04.md).
// Raw ADC only, no
// Rs/R0/ppm conversion — that math (and any baseline tracking) lives
// server-side, same as the dust sensor's baseline/density math (see
// GP2Y_Debug/Baseline_Ratchet_Notes.html).
//
// SmartGated variant: the sensor is behind PMOS#2 (D24 in the .ino). Gas
// power scheduling — when the rail turns on, whether it stays on across
// cycles or cycles per-frame — is decided entirely by the .ino's
// sleepThenSend(), not by this driver; see
// Gas_Scheduling_And_LowBattery_Handoff_2026-09-04.md for the regime A/B
// design. This file only owns the read itself and the warmup validity
// check.
//
// GAS_PIN needs a 100k pull-down to GND so an absent sensor reads near 0
// (caught by GAS_ADC_MIN_VALID) instead of floating to an arbitrary,
// possibly plausible-looking value. Once the module is populated, its own
// low-impedance output dominates the pull-down and reads normally; the same
// bounds then also catch a genuine future fault (shorted/open sensor).
#define GAS_PIN               A2
// gas_NotifyPoweredOn() resets the warmup reference to "now" — call it the
// instant PMOS#2 actually switches the sensor on, each cycle. Without this,
// the elapsed-since-reference check below would use gas_Init()'s boot-time
// timestamp instead, which is stale by the time the first duty-cycled read
// happens and would falsely report "warmed up" on a sensor that's been off
// the whole time in between. gas_getADC() reports "not ready" (this same -1
// pattern isn't used; see below) until gas_getWarmupSeconds() has elapsed since the
// last notify, so an unsettled reading is never silently treated as good.
//
// Datasheet-derived placeholder for the two MEMS candidates above, not
// bench-measured — unchallenged by any field report as of the
// 2026-09-04 research pass (see the handoff doc's §5). Runtime-tunable as
// of 2026-09-08 via /@W<warmup_s># (Field_Diagnostics_And_Remote_
// Configuration_2026-09-07.md §3.1) — gas_setWarmupSeconds()/
// gas_getWarmupSeconds() below back both this driver's own validity check
// and the .ino's regime B off-sleep scheduling from the same value, so the
// two can't drift out of sync. The .ino's off-sleep computation now guards
// against underflow at runtime instead of relying on this staying a fixed
// compile-time constant — see the regime B branch in sleepThenSend().
#define GAS_WARMUP_S_DEFAULT  60
#define GAS_SAMPLES           10   // plain-averaged samples per reading (no pulse timing needed, unlike the dust sensor)
#define GAS_SAMPLE_INTERVAL_MS 2   // spacing between samples, just enough to avoid back-to-back correlated ADC noise
#define GAS_ADC_MIN_VALID     5    // at/below this = pinned near GND (pull-down engaged, sensor absent)
#define GAS_ADC_MAX_VALID     1018 // at/above this = pinned near VCC (short/fault)

void  gas_Init();          // pin setup only — non-blocking, does not wait for warmup
void  gas_NotifyPoweredOn(); // resets the warmup reference to millis() now — call right after PMOS#2 turns on
void  gas_setWarmupSeconds(unsigned long seconds); // /@W# handler — RAM only, resets to GAS_WARMUP_S_DEFAULT on reboot
unsigned long gas_getWarmupSeconds(); // current warmup threshold — read by both this driver's validity check and the .ino's regime B scheduling
// Always returns the real averaged raw ADC (0-1023) — never hides it behind a
// sentinel. *valid reflects this device's own bounds/warmup check, kept only
// for on-device use; downstream (TTN decoder) makes its own validity call
// from the raw value, independent of this flag.
int   gas_getADC(bool* valid);

#endif
