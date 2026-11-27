#ifndef BUILD_CONFIG_H
#define BUILD_CONFIG_H

#include <Arduino.h>
#include <avr/wdt.h>

#define HANG_WATCHDOG_TIMEOUT WDTO_8S

// Feature flags
#define WITH_EEPROM
//#define FORCE_DEFAULT_VALUE
// BENCH_VARIANT: keep the board awake (no sleep) and force a fast 90 s cadence
// at 2 dBm in setup(), so a serial session sees every cycle. Never ship this.
//#define BENCH_VARIANT
#define LOW_POWER
//#define SHOW_LOW_POWER_CYCLE
// Battery strategy: exactly one, chosen at compile time (override with -DBATTERY_STRATEGY=<n>). Each keeps its own parameters.
//   NONE    : depletion run. The battery profiles (/@V thresholds etc.) stay defined and reported, but nothing acts on them.
//   LADDER  : POWERSAVE / LOW / CRITICAL tiers (battery_guard.cpp). For healthy cells.
//   STORAGE : store readings while the supply sags, send them when healthy, CRITICAL floor (reading_store/energy_guard).
//             Guards against terminal voltage collapse during solar depletion or heavy load.
#define BATTERY_STRATEGY_NONE    0
#define BATTERY_STRATEGY_LADDER  1
#define BATTERY_STRATEGY_STORAGE 2
#ifndef BATTERY_STRATEGY
#define BATTERY_STRATEGY BATTERY_STRATEGY_STORAGE
#endif
#if BATTERY_STRATEGY != BATTERY_STRATEGY_NONE && BATTERY_STRATEGY != BATTERY_STRATEGY_LADDER && BATTERY_STRATEGY != BATTERY_STRATEGY_STORAGE
#error "BATTERY_STRATEGY must be 0 (NONE), 1 (LADDER) or 2 (STORAGE)"
#endif
#if BATTERY_STRATEGY == BATTERY_STRATEGY_LADDER
#define POWER_SAVE_ENABLED   // the ladder code in battery_guard.cpp and sleepThenSend() keys off this
#endif

// Operating modes (SmartGated)
enum OperatingMode : uint8_t {
    MODE_NORMAL         = 0,  // duty-cycled, PMOS-gated, attempts the gas-sensor cycle every read
    MODE_NORMAL_NO_GAS  = 1,  // duty-cycled, PMOS-gated, skips the gas sensor entirely
    MODE_ALWAYS_ON      = 2   // legacy behaviour: both rails powered continuously, short TX interval
};

static inline const __FlashStringHelper* modeName(uint8_t m) {
    switch (m) {
        case MODE_NORMAL:        return F("NORMAL");
        case MODE_NORMAL_NO_GAS: return F("NORMAL_NO_GAS");
        case MODE_ALWAYS_ON:     return F("ALWAYS_ON");
        default:                 return F("UNKNOWN");
    }
}

static inline const __FlashStringHelper* batteryStrategyName() {
    switch (BATTERY_STRATEGY) {
        case BATTERY_STRATEGY_LADDER:  return F("LADDER");
        case BATTERY_STRATEGY_STORAGE: return F("STORAGE");
        default:                       return F("NONE");
    }
}

#define ALWAYS_ON_TX_INTERVAL_S 60UL
#define GAS_SPLIT_THRESHOLD_S   600UL

const unsigned DEFAULT_TX_INTERVAL = 3600; // uy1-mega-2 field setting: 60-minute cadence

// Gap between Port 3 batches in a drain. 300 s keeps confirmed-downlink and
// airtime pressure spread out; +/-20% jitter avoids lock-step retries.
#define FLUSH_SPACING_DEFAULT_S 90UL
const unsigned MAX_SLEEP_MINUTES   = 120;

#define BOOT_BURST_FRAME_COUNT  9
#define BOOT_BURST_INTERVAL_S   90UL

#define LOW_BATTERY_SLEEP_SECONDS  1800UL
#define POWERSAVE_TX_INTERVAL_S    21600UL
#define POWERSAVE_CHECK_INTERVAL_S 1800UL

#define DEADMAN_FAIL_THRESHOLD     8
#define DEADMAN_FALLBACK_CHANNEL   0
#define DEADMAN_FALLBACK_TXP       14

// Init-phase connectivity gate: confirmed-ping attempts after a NACK, before
// the node declares the link down and enters outage fallback. Two retries is
// three attempts total, ~3 minutes worst case at BOOT_BURST_INTERVAL_S, and
// rides out two consecutive lost ACKs. The LENERR init retries are separate:
// they never reach the air, so they can afford three.
#define INIT_PHASE_ACK_RETRIES     2

// Firmware revision marker stored in EEPROM byte 44 (unused gap between
// SAG_PARAMS 37..43 and READING_MAGIC 62). Bump on each field release so the
// first boot after flashing clears latched sag/outage flags once and runs
// INIT_PHASE immediately without touching stored readings, boot_id, seqnoUp,
// or committed config. Override with -DFW_REV=<n> for bench tests.
#define FW_REV_EEPROM_ADDR         44
#ifndef FW_REV
#define FW_REV                     1
#endif

extern unsigned TX_INTERVAL;
extern unsigned g_flushSpacingS;
extern uint8_t g_mode;

#endif // BUILD_CONFIG_H
