#ifndef BATTERY_SENSOR_H
#define BATTERY_SENSOR_H

#include "Arduino.h"

// Battery voltage via resistor divider: R1=99.7k (batt+ -> ADC pin),
// R2=100.1k (ADC pin -> GND), meter-verified 2026-07-24. Board runs off an
// MT3608 boost (battery -> 5.168V measured live into the Mega's 5V pin),
// which also doubles as AREF since analogReference() is never called
// (DEFAULT = Vcc). Raw ADC only, no voltage/SoC conversion here — that math
// lives server-side (see ThingsBoard_RuleChain.md §4), same as the dust and
// gas sensors.
//
// 2026-07-24: divider/resistors/Vref all check out (measuring battery
// post-fuse — the node the divider actually taps — gives a ratio within 1%
// of the meter-verified resistors). Raw ADC read ~5% low vs. predicted
// (343 vs ~360) before any fix, consistent with the divider's ~50k Thevenin
// source impedance exceeding the AVR's <10k guidance for full S/H
// capacitor settling within the sampling window — not a wiring defect.
// Fix: 100nF filter cap added across A1/GND (in parallel with R2, via
// jumper) to supply that fast charge demand, rather than lowering R1/R2
// (which would fix the same impedance issue but multiply continuous
// divider current draw, working against the reason 100k/100k was chosen).
// That cap raises the node's RC to ~50k*100nF = 5ms, so full settle after
// an ADC mux channel switch (from whichever channel — gas/dust — was read
// before A1) now needs ~5*RC ≈ 25ms, not the near-instant settle a bare
// resistive divider had. The single discarded analogRead() below used to
// be enough; now it isn't, and averaging samples taken before the node
// settles biases the result low (confirmed in the field: 338 vs ~371
// predicted right after the cap went in, worse than before it was added).
// BATTERY_SETTLE_MS below is the fix — let it settle before sampling.
//
// R2 (ADC pin -> GND) doubles as a built-in fault detector: if the wire to
// Battery+/R1 comes loose, R2 alone pulls the pin near GND, which reads as
// implausibly low rather than as a real (near-zero) battery voltage.
#define BATTERY_ADC_PIN            A1
#define BATTERY_SAMPLES            20
#define BATTERY_SAMPLE_INTERVAL_MS 2
#define BATTERY_SETTLE_MS          30  // let the 100nF filter cap (RC~5ms) settle after an ADC mux channel switch before sampling
#define BATTERY_ADC_MIN_VALID      5   // at/below this = R1/battery+ path broken

// Above this = implausible for a 1S Li-ion on this divider (true full charge,
// 4.2V, tops out around raw ~433 — see 2026-07-23 field log analysis). Readings
// here showed up in clusters right around brownout/reboot events, not as a real
// voltage, so they're rejected the same way as the low-side wiring fault.
#define BATTERY_ADC_MAX_VALID      440

// Bench + field derived cutoffs (2026-07-23 log: dead-bootloop cluster at
// 318-330, catastrophic last-gasp readings at 246-249, stable working band
// 352-393 all day). LOW sits just above the proven dead zone; RECOVER gives
// ~25-count hysteresis inside the stable band so the guard doesn't chatter.
// Runtime-tunable as of 2026-09-09 via /@V<low>,<rec>,<crit>,<psave>,
// <psaveRec># (Field_Diagnostics_And_Remote_Configuration_2026-09-07.md
// §3.3) — these five #defines are the boot/factory-reset defaults only; the
// live values are the g_batt* globals in the .ino.
#define BATTERY_LOW_RAW_DEFAULT            335
#define BATTERY_RECOVER_RAW_DEFAULT        360

// Below this, skip even the low-power heartbeat TX attempt (still recheck +
// sleep normally). A full-power (14 dBm) TX draws a real current spike;
// on a near-dead cell that spike risks tripping the exact brownout the guard
// exists to prevent, for a heartbeat unlikely to complete anyway.
//
// Raised 300->315 after the 2026-07-27 full depletion test (POWER_SAVE_ENABLED
// off, no charger, MQ9 not attached — see Battery_Guard_Analysis_2026-07-27_
// Depletion.html): the real brownout floor was 292 raw, only 8 counts below
// the old 300 cutoff. The guard reads resting voltage *before* the TX current
// pulse, which sags the rail further, and the decay rate roughly triples in
// this exact zone right before the floor — 8 counts wasn't much cushion for
// that. 315 gives ~23 counts of margin above the measured floor while staying
// well clear of LOW (335). Also reconciles the old field-derived 318-330
// dead-bootloop cluster and 246-249 last-gasp floor once corrected for the
// ~28-count charger bias found the same day — see the report for the math.
#define BATTERY_CRITICAL_RAW_DEFAULT       315

// Power-save tier — separate from, and above, the emergency LOW/RECOVER/
// CRITICAL ladder: a gentler first response while there's still real
// charge left, so an unsupervised device visibly slows down instead of
// staying at the aggressive 30s interval all the way to empty. ~25%/~50%
// are rough Li-ion SoC landmarks (not precise) — fine for a non-production
// experiment.
#define BATTERY_POWERSAVE_RAW_DEFAULT         350   // below this: drop to 6h sends (~25%), see POWERSAVE_TX_INTERVAL_S
#define BATTERY_POWERSAVE_RECOVER_RAW_DEFAULT 380   // at/above this: restore the normal TX_INTERVAL (~50%)

void battery_Init();
// Always returns the real averaged raw ADC (0-1023) — never hides it behind
// a sentinel. *valid reflects this device's own plausibility check (used
// on-device to drive the battery guard ladder in real time, since that can't
// wait on a downlink round trip); downstream (TTN decoder) makes its own
// validity call from the raw value, independent of this flag.
int  battery_getADC(bool* valid);

#endif
