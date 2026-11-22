#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <Arduino.h>
#include "build_config.h"

#define PMOS1_PIN   22   // DHT22 + GP2Y1010AU0F dust sensor VCC
#define PMOS2_PIN   24   // MQ-9 heater/VCC

#define PMOS1_SETTLE_MS             2000UL   // DHT22/dust settle time after PMOS#1 turns on
#define BATTERY_POST_GATE_SETTLE_MS 30UL     // ADC mux settle before reading the battery divider

#define JITTER_PERCENT_DEFAULT      10
#define JITTER_PERCENT_BURST        33
#define JITTER_MIN_MS               7000UL

void pmosInit();
void pmos1Set(bool on);
void gasPowerSet(bool on);
bool isPmos1On();
bool isGasPmosOn();

extern bool g_pmos1On;
extern bool g_gasPmosOn;

unsigned long applyJitter(unsigned long base_ms, uint8_t percent, unsigned long minJitterMs);

// ---- Sleep-aware software clock (approach A, 2026-11-01) ---------------------------------------------------
// millis() and LMIC's clock freeze during LowPower.powerDown(), and the watchdog that times each chunk runs
// slow by a per-chip factor (bench board uy1-mega-2: real = 1.1473 x nominal, ~1 ms extra per wake; see
// Millis_Diag_Tools/README.md). lowPowerSleep() adds the estimated real length of every chunk to g_sleptRealMs,
// and nowMs() = millis() + g_sleptRealMs. Timer0 and LMIC are NOT touched (a Timer0 splice was tried and reverted).
#if __has_include("node_calibration.h")
#include "node_calibration.h"   // this chip's measured factor: when WDT_SCALE_PERMILLE_NODE is defined the boot measurement is skipped
#endif
extern const uint16_t g_wdtScaleDefaultPermille;  // node value if defined, else the reference chip's 1147
extern uint16_t g_wdtScalePermille;  // real ms per 1000 nominal watchdog ms; per chip, per supply, per temperature
extern uint8_t  g_wakeCostMs;        // real time per wake not counted by millis()
uint32_t sleepNominalMs();           // sum of nominal chunk lengths slept since boot (bench diagnostics)
uint32_t sleepWakeCount();           // number of power-down chunks since boot (bench diagnostics)
uint32_t sleepEstimatedRealMs();     // estimated real time spent asleep since boot
uint32_t nowMs();                    // millis() + sleepEstimatedRealMs(); wraps after 49.7 days

// Offline fallback calibration (no network/server): measure the watchdog oscillator against the crystal while awake,
// then apply the awake-to-asleep offset measured on two boards (awake reads ~0.8 % slower than asleep). A network/server
// factor (fitted from real elapsed time between contacts) is preferred whenever one is available and should overwrite this.
#define WDT_AWAKE_TO_ASLEEP_PERMILLE 1008     // awake factor / asleep factor, x1000 (1.0083 and 1.0081 on boards 1 and 2)
uint16_t calibrateWdtScale(uint8_t reps);     // returns awake real ms per 1000 nominal (permille), 0 on failure; blocks ~1.15 s per rep
void     wdtScaleApplyAwake(uint16_t awakePermille);  // converts and stores into g_wdtScalePermille if plausible

void lowPower(unsigned long ms);
void lowPowerSleep(unsigned long ms);
void smartSleep(unsigned long ms);

// Blocking wait that feeds the 8 s hang watchdog every 500 ms. Use this instead
// of delay() for any wait that can exceed HANG_WATCHDOG_TIMEOUT while the
// watchdog is armed, which is every !LOW_POWER build.
void wdtSafeDelay(unsigned long ms);

void blinkResetReason(uint8_t mcusr);
void triggerSoftwareReset();

#endif // POWER_MANAGER_H
