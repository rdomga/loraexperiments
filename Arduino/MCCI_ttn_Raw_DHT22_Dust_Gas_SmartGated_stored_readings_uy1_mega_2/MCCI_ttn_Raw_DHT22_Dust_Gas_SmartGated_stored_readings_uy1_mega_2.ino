#include <lmic.h>
#include <hal/hal.h>
#include <SPI.h>
#include <avr/wdt.h>

#include "sensor_manager.h"
#include "DownlinkParser.h"
#include "credentials.h"
#include "radio_activation_abp.h"
#include "payload_encoder.h"
#include "build_config.h"
#include "node_config.h"
#include "power_manager.h"
#include "network_clock.h"
#include "battery_guard.h"
#include "radio_failsafe.h"
#include "serial_console.h"
#include "mode_controller.h"
#include "downlink_handler.h"
#include "energy_guard.h"
#include "reading_store.h"

#define ARDUINO_LMIC_PROJECT_CONFIG_H_SUPPRESS_WARNING

void do_ping(osjob_t* j);
void do_send(osjob_t* j);
void do_flush(osjob_t* j);
void sampleAndDispatch(osjob_t* j);
static void scheduleNextCadence();

bool g_linkOnline = true;
static bool s_outageDrainPending = false;

// True from the moment a Port 3 frame is queued in LMIC until its
// EV_TXCOMPLETE/EV_TXCANCELED. LMIC can hold the frame for tens of seconds
// waiting for duty-cycle airtime, during which OP_TXRXPEND is not yet set.
// Without this flag a second do_flush() would rebuild the frame, reset
// txWasImmediate, and orphan the queued one - the ACK then arrives with no
// batch association and the records never commit.
static bool s_flushInFlight = false;
static uint8_t s_flushRetryCount = 0;   // one quick rebuild-and-retry after a lost batch ACK

// Gap between batches, +/-20% jitter. Runtime-tunable with /@G<seconds>#;
// the production default (300 s) keeps confirmed-downlink pressure spread.
static ostime_t flushSpacingTicks() {
    long s = (long)g_flushSpacingS;
    long jitter = s / 5;
    long v = s + random(-jitter, jitter + 1);
    if (v < 2) v = 2;
    return sec2osticks((u4_t)v);
}

// One gas decision per cycle, taken from the battery while the heater rail is
// in its resting state. Re-reading it inside the sensor read (with the heater
// on) produced a loaded-voltage false positive that could leave the rail
// energised; see sensor_manager_readGatedNormal().
static bool s_skipGasThisCycle = false;
static bool s_skipGasValid = false;

static inline bool skipGasDecision() {
    if (!s_skipGasValid) {
        s_skipGasThisCycle = energyGuard_active() || energyGuard_skipGasWarmup();
        s_skipGasValid = true;
    }
    return s_skipGasThisCycle;
}
static bool s_lastPingWasConfirmed = false;



// 14-byte positional payload: temp | hum | dust ADC | gas ADC | battery ADC,
// then the stored-reading anchor: boot id | seq | uptime minutes.
// Dust/gas/battery ADC fields always carry the real raw reading (0-1023) —
// validity for those three is the TTN decoder's call now, not this device's
// (see payload.txt). Temp/hum keep their own hard-failure sentinels, since a
// DHT22 read failure has no raw value to report in the first place.
static uint8_t mydata[PORT1_PAYLOAD_LEN];
static ReadingStamp g_lastAppendedStamp = {0, 0, 0};
static osjob_t sendjob;
static osjob_t immediateJob;
static osjob_t sleepJob;
static osjob_t retryJob;
static uint8_t txRetryCount = 0;
#define MAX_TX_RETRIES 5

static void scheduleNextCadence() {
#ifdef LOW_POWER
    os_setCallback(&sleepJob, sleepThenSend);
#else
    unsigned long nextSec;
    if (bootBurstFramesLeft > 0 && energyGuard_active()) {
        bootBurstFramesLeft = 0;
    }
    if (bootBurstFramesLeft > 0) {
        bootBurstFramesLeft--;
        nextSec = g_burstIntervalS;
    } else {
        nextSec = energyGuard_intervalS(TX_INTERVAL);
    }
    long jitter_ticks = sec2osticks(nextSec) / 10;
    long rand_jitter = (jitter_ticks > 0) ? random(-jitter_ticks, jitter_ticks + 1) : 0;
    ostime_t next_tx = os_getTime() + sec2osticks(nextSec) + rand_jitter;
    Serial.print(F("Next TX in "));
    Serial.print(nextSec);
    Serial.println(F(" s (+/-10%)"));
    os_setTimedCallback(&sendjob, next_tx, sampleAndDispatch);
#endif
}

// Sensor globals and mask are managed by sensor_manager.h/.cpp

// Hang-detection watchdog. Separate in purpose from softwareReset()'s
// one-shot WDTO_15MS trick below: this one stays armed continuously while
// the sketch is doing active work (setup(), loop(), blocking radio waits)
// and is pet at each checkpoint. If a blocking call downstream (radio SPI
// wait, sensor bit-bang loop, etc.) never returns — e.g. because the boost
// rail collapsed mid-transaction and left the RFM95/DHT22 unresponsive,
// per Field_Session_2026-07-25_Boot_Loop_Diagnosis.html — nothing pets it
// and the AVR resets on its own after this timeout, which a hardware
// brown-out reset can't be relied on to do (BOD trips at 2.7V; the
// measured rail sag during TX only reaches ~3.8V). Deliberately paused
// around every lowPower() sleep (see there) since those are intentional
// and can run far longer than this timeout, and LowPower.powerDown()
// already uses the watchdog itself to time the sleep.
#define HANG_WATCHDOG_TIMEOUT WDTO_8S

// Tells an intentional softwareReset() apart from a watchdog reset the hang
// detector above triggered on its own, so setup() can report which one


// Reset-reason byte appended to every port-2 status frame (do_ping()): low
// nibble is the raw MCUSR bits from this boot (PORF/EXTRF/BORF/WDRF, same
// bit positions as the AVR register — currently always 0x00, kept for when
// the bootloader-level fix lands), bit 4 is synthesized in software — set
// only when the EEPROM checkpoint above read BUSY at boot (last active-work
// window never closed) and noinitBootMarker doesn't show an intentional
// softwareReset().
//
// KNOWN LIMITATION: this bit means "the last active-work window didn't
// close cleanly" — it does NOT mean "the hang watchdog specifically caught a
// stuck firmware." A genuine power interruption (the boost rail collapsing
// and the MCU actually losing power, then coming back) produces the exact
// same signature: .noinit RAM is undefined on a true power-on so
// noinitBootMarker essentially never matches BOOT_MARKER_INTENTIONAL_RESET
// by chance, and the EEPROM checkpoint is left BUSY from before power was
// lost, same as a real hang. The one signal that could disambiguate the two
// (MCUSR's PORF bit — set only on a true power-on) is unavailable as long as
// MCUSR reads 0x00 on this board (see project_mcusr_always_zero_bug). Until
// that's fixed at the bootloader level, treat this bit as "died mid-cycle,
// cause unknown" rather than "confirmed hang," especially given the boost
// rail's collapse-during-TX behavior means real power loss is a live
// candidate for most reports.

unsigned TX_INTERVAL = DEFAULT_TX_INTERVAL;
unsigned g_flushSpacingS = FLUSH_SPACING_DEFAULT_S;
static bool sleepOverridePending = false;    // one-shot /@S<min># request, consumed by the next sleepThenSend() cycle
static unsigned long sleepOverrideSeconds = 0;
static bool txWasImmediate = false;
static bool scheduleAfterImmediateSend = false;
static unsigned long nextPeriodicDelayAfterImmediate = 0;
// Radio failsafe, dead-man, and init phase state managed in radio_failsafe.h/.cpp

// Sensor history and current readings managed in sensor_manager.h/.cpp

// Battery guard state
static bool weakBatteryBootSilent = false;   // booted in low-power sag mode: skip the init ping and the boot burst
static volatile bool txPending = false;     // set before a blocking heartbeat send, cleared on EV_TXCOMPLETE

// One-time random delay before the very first TX, so a batch of nodes
// powered on at the same instant don't stay phase-correlated through the
// whole boot burst (per-cycle jitter alone only decorrelates AFTER frame 1).
#define BOOT_STARTUP_STAGGER_MAX_MS  30000UL

// Jitter definitions and applyJitter() are now provided by power_manager.h.

const lmic_pinmap lmic_pins = {
    .nss  = 10,
    .rxtx = LMIC_UNUSED_PIN,
    .rst  = 9,
    .dio  = {2, 6, 7},
};



// REVERTED 2026-09-10: an LMIC-clock resync fix was tried here (manually advancing
// timer0_millis/timer0_overflow_count by the real elapsed LowPower.powerDown() time,
// to compensate for Timer0 halting during SLEEP_MODE_PWR_DOWN, which desyncs LMIC's
// micros()-based clock from real time). It compiled and flashed clean, but bench-
// measured real inter-uplink gaps got WORSE, not better (~85-93 min vs an assumed
// ~15-21 min pre-fix baseline that a later same-day reproduction did not confirm —
// see below) — a raw bulk splice of timer0's counters likely collides with other
// LMIC state computed from os_getTime() at fixed points (e.g. lmic.c's
// globalDutyAvail / lmic_eu_like.c:155's txbeg + airtime<<globalDutyRate, or
// processRx2DnData()'s txDelay(os_getTime()+...) in lmic.c:1800), which isn't
// designed to tolerate a sudden large jump rather than gradual real-time
// advancement. Reverted to the original (pre-fix) behavior.
//
// RESOLVED 2026-09-10 (later same day): a controlled reproduction using an
// independent timing source (Pi2 host clock, not this device's own Timer0) found
// the freeze described above is real and reproduces instantly, but does NOT carry
// through to real-world transmit cadence — across 40-400s sleep requests, real
// elapsed time stayed within a flat ~12% overshoot of the requested duration,
// matching ordinary AVR watchdog-timer oscillator tolerance (LowPower.powerDown()
// times off the WDT, not Timer0). The original multi-minute-gap report did not
// reproduce and is believed to have been a measurement artifact from that earlier
// session's test loop, not a firmware defect. No further fix needed here — do not
// re-attempt a Timer0 resync, it corrects a clock LowPower.powerDown() doesn't
// depend on. Full writeup: smartgated-clockdrift-reproduction-resolved-2026-09-10 /
// Clock_Drift_Reproduction_2026-09-10.html.
// lowPower() and smartSleep() are now provided by power_manager.h.

static inline void readSensorsForMode() {
    bool skipGas = energyGuard_active() || skipGasDecision();
    if (skipGas && g_mode == MODE_NORMAL) {
        sensorManager_readForMode(MODE_NORMAL_NO_GAS);
    } else {
        sensorManager_readForMode(g_mode);
    }
}


// Battery-only wake loop for power-save mode: sleeps the full 6h TX interval
// in POWERSAVE_CHECK_INTERVAL_S chunks, waking only far enough to sample
// Sleep job to move blocking sleep out of event handler
void sleepThenSend(osjob_t* j) {
    unsigned long sleep_ms;
    const unsigned long cycleS = energyGuard_intervalS(TX_INTERVAL);   // fixed for the whole cycle (the sag state changes after the read)

    if (energyGuard_criticalFloor()) {   // the CRITICAL floor still holds: it slept and peeked, try again next cycle
        scheduleNextCadence();
        return;
    }

    // Baseline gas decision, taken here before any rail turn-on below. Regime A
    // keeps the heater on across cycles by design, so this is the operational
    // loaded value there — conservative, it can only skip gas earlier, never
    // later. Regime B re-reads it after its off-sleep below: this cached value
    // can be a full off-sleep old (about 25 min at TX_INTERVAL 1800) by the
    // time the rail would turn on, long enough for a declining cell to cross LOW.
    s_skipGasThisCycle = energyGuard_active() || energyGuard_skipGasWarmup();
    s_skipGasValid = true;
    bool cycleSkipGas = s_skipGasThisCycle;

#ifdef WITH_EEPROM
    config_set_checkpoint(HANG_CHECKPOINT_IDLE);
#endif

    if (sleepOverridePending) {
        sleep_ms = sleepOverrideSeconds * 1000UL;
        sleepOverridePending = false;
        Serial.print(F("Honoring /@S sleep override: "));
        Serial.print(sleepOverrideSeconds);
        Serial.println(F("s"));
        // Sleep means sleep: drop the gas rail (still on after a boot burst)
        // and leave ALWAYS_ON (PMOS 1+2 latched) so nothing stays powered
        // through the override. exitAlwaysOnIfActive() is a no-op outside
        // ALWAYS_ON and returns the node to NORMAL, same as the battery path.
        gasPowerSet(false);
        exitAlwaysOnIfActive();
    } else if (lowBatteryMode) {
        sleep_ms = applyJitter(LOW_BATTERY_SLEEP_SECONDS * 1000UL, JITTER_PERCENT_DEFAULT, JITTER_MIN_MS);
    } else if (powerSaveMode) {
        // PMOS#2-off + ALWAYS_ON revert, unconditionally, before the peek
        // loop starts — batteryGuard_powerSavePeekLoop() itself only ever peeks the
        // battery, it was never wired to force the gas rail off going in.
        // Covers both the mid-operation transition and a boot-time
        // powerSaveMode=true entry. See handoff doc §4.
        gasPowerSet(false);
        exitAlwaysOnIfActive();
        // Handled entirely by the check loop below - it does its own chunked
        // sleeping with battery-only peeks in between, so there's nothing
        // left for the shared smartSleep() call further down to do.
        batteryGuard_powerSavePeekLoop();
        sleep_ms = 0;
    } else if (bootBurstFramesLeft > 0 && !energyGuard_active()) {
        // Burst: gas rail (MODE_NORMAL only) behaves like regime A, including
        // the weak-cell skip decision — ensure-on, idempotent, stays on across
        // frames, no per-frame off. Sits in the same precedence tier burst
        // already occupied, so it bypasses regime B's split-sleep for free —
        // see handoff doc §2.
        if (g_mode == MODE_NORMAL && !cycleSkipGas && !g_gasPmosOn) {
            gasPowerSet(true);
            gas_NotifyPoweredOn();
        } else if (cycleSkipGas && g_gasPmosOn) {
            gasPowerSet(false);
        }
        sleep_ms = applyJitter(g_burstIntervalS * 1000UL, JITTER_PERCENT_BURST, JITTER_MIN_MS);
        bootBurstFramesLeft--;
    } else if (g_mode == MODE_NORMAL && cycleS >= GAS_SPLIT_THRESHOLD_S) {
        // Regime B: full split-sleep, self-contained per cycle, done here in
        // sleepThenSend()'s own chain rather than inside
        // readSensorsGatedNormal() — see handoff doc §1/§2 (this placement
        // is what keeps burst mode, above, from inheriting a multi-minute
        // wait on top of its own 90s cadence).
        if (g_gasPmosOn) gasPowerSet(false); // defensive off: may still be on from regime A/burst/ALWAYS_ON on a transition into regime B

        unsigned long warmupSegS = gas_getWarmupSeconds();
        // gas_getWarmupSeconds() is runtime-tunable (/@W#) as of 2026-09-08,
        // so the old compile-time underflow guarantee (GAS_SPLIT_THRESHOLD_S
        // fixed comfortably above a fixed warmup segment) no longer holds —
        // clamp explicitly instead. Floors to 0 (skips the off-sleep phase
        // entirely) if an operator sets a warmup at or beyond TX_INTERVAL;
        // the cycle then simply takes as long as the warmup segment itself,
        // exceeding the configured TX_INTERVAL — see the DL_WARMUP_CHANGED
        // warning in applyDownlink() for the operator-facing heads-up.
        unsigned long offSleep_s = (cycleS > warmupSegS) ? (cycleS - warmupSegS) : 0;
        unsigned long offSleep_ms = applyJitter(offSleep_s * 1000UL, JITTER_PERCENT_DEFAULT, JITTER_MIN_MS);
        Serial.print(F("Sleeping "));
        Serial.print(offSleep_ms / 1000.0, 1);
        Serial.println(F("s (regime B, gas off)"));
        Serial.flush();
        smartSleep(offSleep_ms);

        // Re-evaluate on a live, unloaded sample taken here, with the rail still
        // off. energyGuard_skipGasWarmup() calls battery_getADC(), so this is the
        // fresh reading the interlock used before Round 3; the cycle-start value
        // can be a full off-sleep stale.
        s_skipGasThisCycle = energyGuard_active() || energyGuard_skipGasWarmup();
        s_skipGasValid = true;
        bool skipGas = s_skipGasThisCycle;
        if (!skipGas) {
            gasPowerSet(true);
            gas_NotifyPoweredOn();
            Serial.print(F("Gas rail on - fixed "));
            Serial.print(warmupSegS);
            Serial.println(F("s warmup segment (regime B, not jittered)"));
            Serial.flush();
            smartSleep(warmupSegS * 1000UL);
            g_gasOffBeforeBatteryRead = true; // consumed by readSensorsGatedNormal(), below
        } else {
            Serial.println(F("Weak cell or energy-sag active - skipping gas warmup"));
            if (g_gasPmosOn) gasPowerSet(false);
            g_gasOffBeforeBatteryRead = false;
        }

        sleep_ms = 0; // both sleeps already done above
    } else {
        // Regime A (MODE_NORMAL, TX_INTERVAL < GAS_SPLIT_THRESHOLD_S), or
        // MODE_NORMAL_NO_GAS, or MODE_ALWAYS_ON (already latched by
        // setOperatingMode() — nothing to arrange here): plain cadence.
        bool skipGas = cycleSkipGas;
        if (g_mode == MODE_NORMAL && !skipGas && !g_gasPmosOn) {
            gasPowerSet(true);
            gas_NotifyPoweredOn();
        } else if (skipGas && g_gasPmosOn) {
            gasPowerSet(false);
        }
        sleep_ms = applyJitter(cycleS * 1000UL, JITTER_PERCENT_DEFAULT, JITTER_MIN_MS);
    }

    if (sleep_ms > 0) {
        Serial.print(F("Sleeping "));
        Serial.print(sleep_ms / 1000.0, 1);
        Serial.println(F("s"));
        Serial.flush();
        smartSleep(sleep_ms);
    }

#ifdef WITH_EEPROM
    config_set_checkpoint(HANG_CHECKPOINT_BUSY);
#endif

    sampleAndDispatch(j);
}

// Store the current sample and return the stamp captured with it, so the Port 1
// frame reports exactly the reading that was appended (boot/seq/uptime from
// the same atomic state as the EEPROM record).
static ReadingStamp appendCurrentSampleForTx(uint8_t state) {
    TelemetryData rec;
    rec.temp_s   = lastSensorValid ? (int16_t)(g_temp * 10) : INT16_MIN;
    rec.hum_s    = lastSensorValid ? (uint8_t)(g_hum  * 2) : 0xFF;
    rec.dust_adc = (uint16_t)g_adc;
    rec.gas_adc  = (uint16_t)g_gas;
    rec.batt_adc = (uint16_t)g_batt;
    g_lastAppendedStamp = readingStore_append(rec, state);
    sensorManager_updateLastTransmitted();
    return g_lastAppendedStamp;
}

void sampleAndDispatch(osjob_t* j) {
    if (lowBatteryMode) {
        // No PMOS powers in this mode, ever — not even on the transmit
        // cycle. Battery is a plain analogRead (no PMOS gating), so this
        // stays a cheap, real, every-cycle peek; temp/hum/dust/gas are
        // intentionally NOT re-read — readSensorsForMode() isn't called at
        // all here, so g_temp/g_hum/g_adc/g_gas simply retain whatever they
        // held before entering lowBatteryMode. do_ping() below reports
        // those as-is via last_*_s — see handoff doc §4.
        g_batt = battery_getADC(&g_batt_valid);
    } else {
        readSensorsForMode();
    }

#ifdef POWER_SAVE_ENABLED
    if (batteryGuard_updateHysteresis(g_batt, g_batt_valid)) {
        pmos1Set(false);
        gasPowerSet(false);
        exitAlwaysOnIfActive();
    }
#endif // POWER_SAVE_ENABLED
    // Energy sag strategy: while supply is weak, reading is stored locally and node sleeps again without radio TX.
    if (energyGuard_storeThisCycle()) {
        scheduleNextCadence();
        return;
    }

    // With POWER_SAVE_ENABLED off, lowBatteryMode/powerSaveMode are never set,
    // so the branches below always take the plain do_send() path — constant
    // cadence, no heartbeat-only mode, no near-dead-cell TX skip, all the way
    // to empty. Every path that transmits a live sample appends it first, so
    // the Port 1 anchor always names a record that exists in EEPROM.
    if (lowBatteryMode) {
        last_batt_s = (uint16_t)g_batt; // always the real raw value now
        if (g_batt_valid && g_batt >= g_battCritical) {
            os_setCallback(&sendjob, do_ping);
        } else {
            Serial.println(F("Below critical (or unreadable) - skipping heartbeat TX to save the last of the charge"));
            scheduleNextCadence();
        }
    } else if (!g_linkOnline) {
        // Network outage fallback: store as outage-persistent (flushed over Port 3 on recovery)
        appendCurrentSampleForTx(RECORD_STATE_OUTAGE_PERSISTENT);
        if (radioFailsafe_fallbackProbeDue()) {
            Serial.println(F("Fallback: probe due - sending confirmed ping"));
            g_pingConfirmedPending = true;
            os_setCallback(&sendjob, do_ping);
        } else {
            Serial.println(F("Fallback: probe skipped (backoff) - sleeping"));
            scheduleNextCadence();
        }
    } else {
        // Healthy link: store-before-transmit (pruned on confirmed dead-man ACK)
        appendCurrentSampleForTx(RECORD_STATE_UNCONFIRMED);
        os_setCallback(&sendjob, do_send);
    }
}

// One-time LMIC/ABP session + single-channel bring-up. Split out of setup() so
// the low-power boot path can bring the radio up on-demand for a heartbeat
// without running the full normal boot sequence.
void initRadio() {
    os_init();              // LMIC clock starts HERE
    LMIC_reset();
    LMIC_setClockError(MAX_CLOCK_ERROR * 20 / 100);
    //LMIC_setClockError(0);

    establishRadioSession();  // radio_activation_abp.h — ABP-specific; swap for an OTAA equivalent later

#if defined(CFG_eu868)
    // DR_SF7B on ch0 allows BW250 (DR6) to stay on 868.1 MHz
    LMIC_setupChannel(0, 868100000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);
    LMIC_setupChannel(1, 868300000, DR_RANGE_MAP(DR_SF12, DR_SF7B), BAND_CENTI);
    LMIC_setupChannel(2, 868500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
    LMIC_setupChannel(3, 867100000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
    LMIC_setupChannel(4, 867300000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
    LMIC_setupChannel(5, 867500000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
    LMIC_setupChannel(6, 867700000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
    LMIC_setupChannel(7, 867900000, DR_RANGE_MAP(DR_SF12, DR_SF7),  BAND_CENTI);
    LMIC_setupChannel(8, 868800000, DR_RANGE_MAP(DR_FSK,  DR_FSK),  BAND_MILLI);
#elif defined(CFG_us915) || defined(CFG_au915)
    LMIC_selectSubBand(1);
#elif defined(CFG_as923)
#elif defined(CFG_kr920)
#elif defined(CFG_in866)
#else
# error Region not supported
#endif

    // Prevent MAC DlChannelReq from silently redirecting RX1 to another frequency
#if !defined(DISABLE_MCMD_DlChannelReq)
    for (uint8_t i = 0; i < 9; i++) LMIC.channelDlFreq[i] = 0;
#endif

    // Single-channel enforcement: disables ch 1-8, locks shuffle map,
    // disables ADR and link check, asserts DR_SF7 / 14 dBm, sets RX2.
    SCG_init();
}

void initRadioOnce() {
    if (radioInitialized) return;
    initRadio();
    radioInitialized = true;
}

// Blocking send used only from the low-power boot path, where the normal
// loop()/os_runloop_once() pump isn't running yet. Bounded by a timeout so a
// stuck radio can't hang the low-power loop forever.
void sendHeartbeatBlocking() {
    txPending = true;
    os_setCallback(&sendjob, do_ping);
    unsigned long start = millis();
    while (txPending && (millis() - start) < 20000UL) {
        wdt_reset(); // this loop can legitimately run past HANG_WATCHDOG_TIMEOUT
        SCG_enforce();
        os_runloop_once();
    }
}

// Entered from setup() when the battery reads below g_battLow at boot.
// Skips DHT22/dust/gas/LMIC init entirely (no point paying that current cost
// on a battery too low to sustain it) and sits in a sleep/recheck loop,
// bringing the radio up just long enough for an occasional port-2 heartbeat.
// Exits only via softwareReset() once the battery climbs back to
// g_battRecover, which re-enters setup() from a clean slate.
void lowPowerBoot(int battRaw, bool battValid) {
    Serial.println(F("Battery low at boot - entering low-power boot path"));
    last_temp_s = INT16_MIN;
    last_hum_s = 0xFF;
    last_adc_s = 0xFFFF; // dust/gas are never read on this path - no reading exists to report
    last_gas_s = 0xFFFF;
    lastSensorValid = false;

    while (true) {
        last_batt_s = (uint16_t)battRaw; // always the real raw value now
        Serial.print(F("Low-power battADC=")); Serial.println(battRaw);

        if (battValid && battRaw >= g_battRecover) {
            Serial.println(F("Battery recovered - restarting into normal mode"));
            softwareReset();
        }

        if (battValid && battRaw >= g_battCritical) {
            initRadioOnce();
            sendHeartbeatBlocking();
        } else {
            Serial.println(F("Below critical (or unreadable) - skipping heartbeat TX to save the last of the charge"));
        }

#ifdef LOW_POWER
        lowPower(LOW_BATTERY_SLEEP_SECONDS * 1000UL);
#else
        wdtSafeDelay(LOW_BATTERY_SLEEP_SECONDS * 1000UL);
#endif
        battRaw = battery_getADC(&battValid);
    }
}




void onEvent(ev_t ev) {
    Serial.print(os_getTime());
    Serial.print(F(": "));
    switch (ev) {
        case EV_SCAN_TIMEOUT:    Serial.println(F("EV_SCAN_TIMEOUT"));    break;
        case EV_BEACON_FOUND:    Serial.println(F("EV_BEACON_FOUND"));    break;
        case EV_BEACON_MISSED:   Serial.println(F("EV_BEACON_MISSED"));   break;
        case EV_BEACON_TRACKED:  Serial.println(F("EV_BEACON_TRACKED"));  break;
        case EV_JOINING:         Serial.println(F("EV_JOINING"));         break;
        case EV_JOINED:
            Serial.println(F("EV_JOINED"));
            // SCG_on_joined();
            // SCG_enforce();
            break;
        case EV_JOIN_FAILED:     Serial.println(F("EV_JOIN_FAILED"));     break;
        case EV_REJOIN_FAILED:   Serial.println(F("EV_REJOIN_FAILED"));   break;
        case EV_TXCOMPLETE: {
            Serial.println(F("EV_TXCOMPLETE"));
            digitalWrite(LED_BUILTIN, LOW); // unconditional final off — covers any path where an RX window was skipped
            txPending = false;
            bool ack = (LMIC.txrxFlags & TXRX_ACK);
            bool lenErr = (LMIC.txrxFlags & TXRX_LENERR) != 0;
            bool linkFailed = (!ack && !lenErr);
            if (ack)
                Serial.println(F("Received ACK"));

            radioFailsafe_onTxComplete(ack, lenErr);

            InitPhaseResult ipRes = radioFailsafe_handleInitPhase(ack, lenErr, g_burstIntervalS, &sendjob, do_ping);
            if (ipRes == INIT_PHASE_RETRYING) {
                return;
            } else if (ipRes == INIT_PHASE_COMPLETED) {
                if (ack) {
                    g_linkOnline = true;
                    if (my_eeprom.outage_fallback_active == 1) {
                        config_set_outage_fallback_active(0);
                        s_outageDrainPending = (readingStore_unsentCount() > 0);
                        Serial.println(F("Reboot recovery ACK: Buffer preserved for drain"));
                    } else {
                        // Do not prune pre-reboot unconfirmed records on boot sentinel ACK.
                        // Sentinel ACK only proves the link is up now; pre-reboot records are preserved
                        // until normal runtime dead-man ACKs or operator resends (@U) drain/prune them.
                        s_outageDrainPending = false;
                    }
                } else if (linkFailed) {
                    g_linkOnline = false;
                    config_set_outage_fallback_active(1);
                    s_outageDrainPending = false;
                    bootBurstFramesLeft = 0;
                    Serial.println(F("Init ping not ACK'd - entering OUTAGE_FALLBACK_ACTIVE (burst skipped)"));
                }
            } else {
                if (g_lastTxPort == 2 && s_lastPingWasConfirmed) {
                    s_lastPingWasConfirmed = false;
                    if (g_linkOnline && !s_outageDrainPending && ack) {
                        readingStore_pruneDayUnsent();
                    } else if (g_linkOnline && linkFailed) {
                        g_linkOnline = false;
                        config_set_outage_fallback_active(1);
                        s_outageDrainPending = false;
                        radioFailsafe_onFallbackProbeResult(false);
                        Serial.println(F("Confirmed ping failed - entering OUTAGE_FALLBACK_ACTIVE"));
                    } else if (!g_linkOnline && ack) {
                        g_linkOnline = true;
                        config_set_outage_fallback_active(0);
                        s_outageDrainPending = (readingStore_unsentCount() > 0);
                        radioFailsafe_onFallbackProbeResult(true);
                        Serial.println(F("Confirmed probe ping ACK'd - link recovered, exiting OUTAGE_FALLBACK_ACTIVE"));
                    } else if (!g_linkOnline && linkFailed) {
                        radioFailsafe_onFallbackProbeResult(false);
                    } else if (lenErr) {
                        // Frame refused before airtime: neither an ACK nor a link failure.
                        // Keep g_linkOnline and the dead-man counters untouched.
                        Serial.println(F("Confirmed ping: length error - link state unchanged"));
                    }
                }
            }

            Serial.print(F("LMIC.dataLen="));
            Serial.println(LMIC.dataLen);
            Serial.print(F("LMIC.rxtime="));
            Serial.println(LMIC.rxtime);

            DownlinkResult dlResult = { DL_NONE, 0 };
            bool isExtraTxComplete = txWasImmediate;
            if (isExtraTxComplete) {
                txWasImmediate = false;
                Serial.println(F("Extra transmit completed"));
            }

            if (LMIC.dataLen) {
                // Must stay >= DownlinkParser.h's cmdstr[] (currently 28) —
                // this buffer truncating a downlink before the parser even
                // sees it is the same bug class a bench test caught in
                // cmdstr itself when /@V# shipped (2026-09-09); this radio
                // path wasn't exercised by that test (it went through the
                // serial path instead), so it carried the same latent bug
                // undetected until reviewed directly.
                uint8_t dlBuf[28];
                uint8_t dlLen = min((uint8_t)sizeof(dlBuf), (uint8_t)LMIC.dataLen);
                memcpy(dlBuf, LMIC.frame + LMIC.dataBeg, dlLen);

                dlResult = parseDownlinkCommand(dlBuf, dlLen);
                applyDownlink(dlResult);

                // != DL_BURST_START so /@B# doesn't auto-cancel the very
                // burst it just armed.
                if (dlResult.action != DL_NONE && dlResult.action != DL_BURST_START && bootBurstFramesLeft > 0) {
                    Serial.println(F("Downlink received during boot burst - ending burst early"));
                    bootBurstFramesLeft = 0;
                }
            }
#ifdef WITH_EEPROM
            config_put_seqnoUp(LMIC.seqnoUp);
#endif

            bool pingRatioReached = radioFailsafe_checkPeriodicPingDue(isExtraTxComplete);


            // A port 3 confirmed flush frame just completed
            if (isExtraTxComplete && g_lastTxPort == 3) {
                s_flushInFlight = false;
                if (ack) {
                    s_flushRetryCount = 0;
                    Serial.println(F("Port 3 confirmed batch ACK'd: committing records to SENT"));
                    readingStore_markFramed(); // Commits this batch to SENT, decrements s_unsent

                    bool moreToFlush = (g_linkOnline && !energyGuard_active() && readingStore_unsentCount() > 0 &&
                                        (s_outageDrainPending || readingStore_persistentUnsentCount() > 0));
                    if (moreToFlush) {
                        txWasImmediate = true;   // next completion is again an "extra" one
                        os_setTimedCallback(&immediateJob, os_getTime() + flushSpacingTicks(), do_flush);
                        break;
                    } else {
                        // Drain done: either nothing unsent, or (only possible otherwise) leftover
                        // 0xFE unconfirmed telemetry outside an operator resend range - those are
                        // pending-prune safety copies, not undelivered outage data, so say so.
                        if (readingStore_unsentCount() == 0) {
                            Serial.println(F("Port 3 recovery drain complete: all batches delivered and committed"));
                        } else {
                            Serial.print(F("Port 3 recovery drain complete: persistent backlog delivered; "));
                            Serial.print(readingStore_unsentCount());
                            Serial.println(F(" unconfirmed record(s) remain (pruned on next confirmed ping)"));
                        }
                        // Emit the full status block here, while the store is still zero and the
                        // node is awake: a separate /@P# would race the 3600 s sleep and be lost.
                        printVerboseStatus();
                        s_outageDrainPending = false;
                        txWasImmediate = false;
                        scheduleAfterImmediateSend = false;
                        scheduleNextCadence();
                        break;
                    }
                } else {
                    Serial.println(F("Port 3 confirmed batch NOT ACK'd: retaining persistent records for retry"));
                    readingStore_unframePending(); // Leaves records in PERSISTENT, resets s_pendCount
                    printVerboseStatus();            // prove the retained count before the node sleeps
                    s_outageDrainPending = true;
                    txWasImmediate = false;
                    scheduleAfterImmediateSend = false;
                    if (lenErr) {
                        // The frame was refused before airtime. Retrying the same length
                        // in 60 s cannot help. Keep the records PERSISTENT, do not touch
                        // the link state, and rebuild the batch at the next cadence.
                        s_flushRetryCount = 0;
                        Serial.println(F("Port 3 TX length error: batch retained, retry at cadence"));
                        scheduleNextCadence();
                    } else if (s_flushRetryCount == 0) {
                        // One quick retry, rebuilt from EEPROM, then fall back to
                        // the normal cadence if the link is still down. This is
                        // the recovery path a lost ACK needs without waiting a
                        // full 30-minute cycle.
                        s_flushRetryCount = 1;
                        Serial.println(F("Port 3 NACK: rebuilding the batch for one retry in 60 s"));
                        txWasImmediate = true;
                        scheduleAfterImmediateSend = true;
                        os_setTimedCallback(&immediateJob, os_getTime() + sec2osticks(60), do_flush);
                    } else {
                        s_flushRetryCount = 0;
                        // The 60 s retry also NACKed: stop treating the link as up.
                        // Without this, every cadence re-runs the 8-attempt confirmed
                        // flush (plus its retry) until a dead-man ping happens to flip
                        // g_linkOnline, about 4.5 h at the production cadence. Records
                        // stay PERSISTENT; the outage drain resumes when a fallback
                        // probe or ping ACK restores g_linkOnline.
                        g_linkOnline = false;
                        config_set_outage_fallback_active(1);
                        s_outageDrainPending = false;
                        radioFailsafe_onFallbackProbeResult(false);
                        Serial.println(F("Port 3 NACK on retry - link marked down; records retained"));
                        scheduleNextCadence();
                    }
                    break;
                }
            }

            if (!isExtraTxComplete) {
                unsigned long nextSec = TX_INTERVAL;
                if (dlResult.action == DL_SLEEP) {
                    unsigned long sleepMin = dlResult.value;
                    if (sleepMin > MAX_SLEEP_MINUTES) {
                        Serial.print(F("Sleep clamped to "));
                        Serial.print(MAX_SLEEP_MINUTES);
                        Serial.println(F(" min"));
                        sleepMin = MAX_SLEEP_MINUTES;
                    }
                    nextSec = sleepMin * 60UL;
                    // Under LOW_POWER, sleepThenSend() (not this nextSec) picks the
                    // actual sleep duration — hand it the requested value directly.
                    sleepOverridePending = true;
                    sleepOverrideSeconds = nextSec;
                }

                if (dlResult.action == DL_TX_NOW) {
                    // Read sensors now so the immediate send has fresh data.
                    readSensorsForMode();
                    appendCurrentSampleForTx(g_linkOnline ? RECORD_STATE_UNCONFIRMED : RECORD_STATE_OUTAGE_PERSISTENT);
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_send);
                } else if (dlResult.action == DL_PING) {
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_ping);
                } else if (dlResult.action == DL_SF_CHANGED || dlResult.action == DL_TXPOWER_CHANGED
                           || dlResult.action == DL_CHANNEL_CHANGED) {
                    // §3.8 verification, real-downlink path: applyDownlink()
                    // above already set g_radioVerifyViaPing/
                    // g_pingConfirmedPending. Safe to fire immediately, no
                    // busy-check needed — we're already inside this job's
                    // own EV_TXCOMPLETE, so the TX path is guaranteed clear
                    // (same reasoning DL_PING above relies on). DL_CHANNEL_
                    // CHANGED added 2026-09-10 — bench testing caught this
                    // branch missing it: without it a real /@C#/{"channel"}
                    // downlink armed the flags but never fired the ping,
                    // leaving verification permanently pending until the
                    // next autonomous ping-ratio/dead-man cycle happened to
                    // fire one anyway.
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_ping);
                } else if (pingRatioReached) {
                    // A manual /@P# this same cycle (handled above) already
                    // sends the same status payload, so it takes precedence
                    // via this else-if chain — the counter reset above still
                    // happens either way, no redundant frame gets queued.
                    Serial.println(F("Ping ratio reached - sending autonomous status frame"));
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_ping);
                } else if (g_pingConfirmedDeferred) {
                    // Safe now — this completion (whatever raced ahead of
                    // the /@P1# serial command) just fully finished, so
                    // the TX path is guaranteed clear (same reasoning the
                    // DL_PING branch above already relies on).
                    g_pingConfirmedDeferred = false;
                    Serial.println(F("Confirmed ping (deferred) - sending now"));
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_ping);
                } else if (g_pingConfirmedPending) {
                    Serial.println(F("Confirmed ping (pending) - sending now"));
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_ping);
                } else if (g_deadmanPingPending) {
                    // §4: lowest priority — a routine/verify ping this same
                    // cycle already exercises the radio, so there's no rush;
                    // this fires on whichever completion reaches here first
                    // with nothing more urgent pending. Not reset until
                    // actually consumed inside do_ping(), so a cycle where
                    // something else wins the immediate slot doesn't lose it.
                    g_pingConfirmedPending = true;
                    Serial.println(F("Dead-man check ping - sending now"));
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setCallback(&immediateJob, do_ping);
                } else if (g_linkOnline && !energyGuard_active() && readingStore_unsentCount() > 0 &&
                           (s_outageDrainPending || readingStore_persistentUnsentCount() > 0)) {
                    // Lowest priority, like the dead-man ping: if another extra frame won this slot the flush simply
                    // waits one cycle (the records are safe in EEPROM).
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setTimedCallback(&immediateJob, os_getTime() + flushSpacingTicks(), do_flush);
                }

                if (!scheduleAfterImmediateSend) {
                    scheduleNextCadence();
                }
            } else if (scheduleAfterImmediateSend) {
                scheduleAfterImmediateSend = false;
                unsigned long nextSec = nextPeriodicDelayAfterImmediate;
                bool moreToFlush = (g_linkOnline && !energyGuard_active() && readingStore_unsentCount() > 0 &&
                                    (s_outageDrainPending || readingStore_persistentUnsentCount() > 0));
                if (moreToFlush) {
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = nextSec;
                    os_setTimedCallback(&immediateJob, os_getTime() + flushSpacingTicks(), do_flush);
                    break;
                }
                scheduleNextCadence();
            }
            break;
        }
        case EV_LOST_TSYNC:      Serial.println(F("EV_LOST_TSYNC"));     break;
        case EV_RESET:           Serial.println(F("EV_RESET"));           break;
        case EV_RXCOMPLETE:
            Serial.println(F("EV_RXCOMPLETE"));
            digitalWrite(LED_BUILTIN, LOW); // window closed; EV_RXSTART turns it back on if another one opens
            break;
        case EV_LINK_DEAD:       Serial.println(F("EV_LINK_DEAD"));       break;
        case EV_LINK_ALIVE:      Serial.println(F("EV_LINK_ALIVE"));      break;
        case EV_TXSTART: {
            Serial.println(F("EV_TXSTART"));
            digitalWrite(LED_BUILTIN, HIGH); // radio activity indicator: on for TX, and again for each RX window below
#ifdef WITH_EEPROM
            config_put_seqnoUp(LMIC.seqnoUp);
#endif
            break;
        }
        case EV_TXCANCELED:
            Serial.println(F("EV_TXCANCELED"));
            if (s_flushInFlight) {
                s_flushInFlight = false;
                txWasImmediate = false;
                scheduleAfterImmediateSend = false;
                scheduleNextCadence();
            }
            break;
        case EV_RXSTART:
            digitalWrite(LED_BUILTIN, HIGH); // RX1/RX2 window opened
            break;
        case EV_JOIN_TXCOMPLETE: Serial.println(F("EV_JOIN_TXCOMPLETE: no JoinAccept")); 
            // SCG_on_join_txcomplete();
            break;
        default:
            Serial.print(F("Unknown event: "));
            Serial.println((unsigned)ev);
            break;
    }
}

void do_ping(osjob_t* j) {
    if (LMIC.opmode & OP_TXRXPEND) {
        if (++txRetryCount <= MAX_TX_RETRIES) {
            Serial.println(F("OP_TXRXPEND, retrying ping in 5s"));
            os_setTimedCallback(&retryJob, os_getTime() + sec2osticks(5), do_ping);
        } else {
            Serial.println(F("Ping retry limit reached, resuming schedule"));
            txRetryCount = 0;
            scheduleNextCadence();
        }
        return;
    }
    txRetryCount = 0;

    // Port 2 layout (26 bytes) — widened 2026-09-10 per Field_Diagnostics
    // §7.1/§7.2. Bytes 17-25 are new; 0-16 are unchanged from the original
    // 17-byte frame.
    //   [0-1]   interval
    //   [2-5]   seqnoUp
    //   [6-7]   temp                 (0x8000 = DHT22 read failure - no value to report)
    //   [8]     hum                  (0xFF = DHT22 read failure - no value to report)
    //   [9-10]  dust ADC             raw 0-1023, always the real reading
    //   [11-12] Gas sensor ADC        raw 0-1023, always the real reading
    //   [13-14] battery ADC          raw 0-1023, always the real reading
    //   [15]    reset reason         see RESET_FLAG_INCOMPLETE_CYCLE / MCUSR bits above (bit 4 is "didn't close cleanly," not confirmed-hang — see limitation note there)
    //   [16]    operating mode + ping reason + profile_saved flag — bits 0-1 =
    //           OperatingMode (0=NORMAL, 1=NORMAL_NO_GAS, 2=ALWAYS_ON), bits
    //           2-4 = PING_REASON_* (added 2026-09-11, see its declaration
    //           near the top of the file), bit 7 = profile_saved
    //           (my_eeprom.profile_saved, 1 = last /@CP# actually landed).
    //           §7.2 point 1: any consumer that read this byte as a bare
    //           mode value before 2026-09-10 must mask with & 0x03 now that
    //           other bits carry independent flags.
    //   [17]    g_sensorMask         bitmask, as-is
    //   [18-19] gas warmup seconds   uint16, gas_getWarmupSeconds()
    //   [20]    g_battLow / 2        TB/decoder side multiplies by 2 back to raw ADC counts
    //   [21]    g_battRecover / 2
    //   [22]    g_battCritical / 2
    //   [23]    g_battPsave / 2
    //   [24]    g_battPsaveRecover / 2
    //   [25]    g_pingRatio
    // Dust/gas/battery no longer use 0xFFFF to hide an out-of-range reading —
    // validity for those three is now the TTN decoder's call, made from the
    // raw value against its own thresholds (see payload.txt). Port 1 (9-byte
    // sensor telemetry) deliberately does NOT carry the mode byte — it's only
    // reported here on port 2, per explicit instruction, so the frequent
    // telemetry frame stays unchanged.

    uint8_t pingReason = radioFailsafe_computePingReason();

    // One atomic snapshot: boot, uptime and next sequence belong to the same state.
    ReadingAnchor anchor = readingStore_anchor();

    StatusPingData pingData;
    pingData.interval_s        = (uint16_t)TX_INTERVAL;
    pingData.seqno             = LMIC.seqnoUp;
    pingData.temp_s            = last_temp_s;
    pingData.hum_s             = last_hum_s;
    pingData.dust_adc          = last_adc_s;
    pingData.gas_adc           = last_gas_s;
    pingData.batt_adc          = last_batt_s;
    pingData.resetReason       = g_resetReasonByte;
    pingData.mode              = g_mode;
    pingData.pingReason        = pingReason;
    pingData.profileSaved      = (my_eeprom.profile_saved != 0);
    pingData.sensorMask        = g_sensorMask;
    pingData.gasWarmup_s       = (uint16_t)gas_getWarmupSeconds();
    pingData.battLow           = g_battLow;
    pingData.battRecover       = g_battRecover;
    pingData.battCritical      = g_battCritical;
    pingData.battPsave         = g_battPsave;
    pingData.battPsaveRecover  = g_battPsaveRecover;
    pingData.pingRatio         = g_pingRatio;
    pingData.radioDR           = SCG_currentDR;
    pingData.radioTXP          = SCG_currentTXP;
    pingData.radioChannel      = g_scgChannel;
    pingData.strategy          = BATTERY_STRATEGY;
    pingData.guardState        = batteryGuard_stateBits() | (g_linkOnline ? 0 : 0x20) | energyGuard_stateBits();
    pingData.sagEnterHalf      = energyGuard_enterHalf();
    pingData.sagExitHalf       = energyGuard_exitHalf();
    pingData.sagIntervalMin    = energyGuard_intervalMin();
    pingData.bufferUnsent      = energyGuard_unsent();
    pingData.bootId            = anchor.boot;
    pingData.sagStartAgo       = energyGuard_startAgoMin();
    pingData.sagEndAgo         = energyGuard_endAgoMin();
    pingData.sagCount          = energyGuard_sagEvents();
    pingData.sagLost           = energyGuard_lostRecords();
    pingData.uptime_min        = anchor.uptimeMin;
    pingData.next_seq          = anchor.nextSeq;

    uint8_t status[PORT2_PAYLOAD_LEN];
    encodePort2Status(pingData, status);

    if (!lastSensorValid) {
        Serial.println(F("Ping requested before first sensor sample; sending sentinel status"));
    } else {
        Serial.println(F("Ping requested; sending status payload"));
    }
    Serial.print(F("Ping reason: ")); Serial.println(pingReason);

    bool confirmedNow = radioFailsafe_armPingTx();
    s_lastPingWasConfirmed = confirmedNow;
    // The ACK downlink this ping causes also carries the network time. Skip the
    // 1-byte DeviceTimeReq at DR0..DR2, where the 46-byte frame has only the
    // 5-byte FOpts margin left; a length error there would cost the whole ping.
    if (confirmedNow && LMIC.datarate > DR_SF10) netclk_request();
    LMIC_setTxData2(2, status, sizeof(status), confirmedNow ? 1 : 0);
}

#ifdef MILLIS_BENCH_E
// TEMPORARY bench instrumentation (approach E, network time). Build with -DMILLIS_BENCH_E only.
static void benchNetTimeCb(void* pUserData, int flagSuccess) {
    netclk_onNetworkTime(pUserData, flagSuccess);   // bench builds feed their anchors to the network clock as well
    Serial.print(F("NETTIME ok=")); Serial.print(flagSuccess);
    if (flagSuccess) {
        lmic_time_reference_t ref;
        if (LMIC_getNetworkTimeReference(&ref) > 0) {
            Serial.print(F(" net_gps_s=")); Serial.print((unsigned long)ref.tNetwork);
            Serial.print(F(" local_ticks=")); Serial.print((long)ref.tLocal);
        }
    }
    Serial.print(F(" millis=")); Serial.println(millis());
    Serial.print(F("CLK nowMs=")); Serial.print(nowMs());
    Serial.print(F(" millis=")); Serial.print(millis());
    Serial.print(F(" slept_est=")); Serial.print(sleepEstimatedRealMs());
    Serial.print(F(" nominal=")); Serial.print(sleepNominalMs());
    Serial.print(F(" wakes=")); Serial.println(sleepWakeCount());
}
#endif

void do_send(osjob_t* j) {
    if (LMIC.opmode & OP_TXRXPEND) {
        if (++txRetryCount <= MAX_TX_RETRIES) {
            Serial.println(F("OP_TXRXPEND, retrying in 5s"));
            os_setTimedCallback(&retryJob, os_getTime() + sec2osticks(5), do_send);
        } else {
            Serial.println(F("TX retry limit reached, resuming schedule"));
            txRetryCount = 0;
            scheduleNextCadence();
        }
        return;
    }
    txRetryCount = 0;

    int16_t  temp_s = lastSensorValid ? (int16_t)(g_temp * 10) : INT16_MIN;
    uint8_t  hum_s  = lastSensorValid ? (uint8_t)(g_hum  * 2) : 0xFF;
    uint16_t adc_s  = (uint16_t)g_adc;  // always the real raw value now
    uint16_t gas_s  = (uint16_t)g_gas;
    uint16_t batt_s = (uint16_t)g_batt;

    last_temp_s = temp_s;
    last_hum_s = hum_s;
    last_adc_s = adc_s;
    last_gas_s = gas_s;
    last_batt_s = batt_s;
    TelemetryData telem;
    telem.temp_s     = temp_s;
    telem.hum_s      = hum_s;
    telem.dust_adc   = adc_s;
    telem.gas_adc    = gas_s;
    telem.batt_adc   = batt_s;
    telem.boot_id    = g_lastAppendedStamp.boot;
    telem.seq        = g_lastAppendedStamp.seq;
    telem.uptime_min = g_lastAppendedStamp.tMin;
    encodePort1Telemetry(telem, mydata);

    Serial.print(F("T:")); Serial.print(g_temp, 1);
    Serial.print(F("C H:")); Serial.print(g_hum, 1);
    Serial.print(F("% DUST_ADC:")); Serial.print(g_adc);
    if (!g_adc_valid) Serial.print(F("(flagged)"));
    Serial.print(F(" GAS_ADC:")); Serial.print(g_gas);
    if (!g_gas_valid) Serial.print(F("(flagged)"));
    Serial.print(F(" BATT_ADC:")); Serial.print(g_batt);
    if (!g_batt_valid) Serial.print(F("(flagged)"));
    Serial.print(F(" ["));
    for (int i = 0; i < PORT1_PAYLOAD_LEN; i++) {
        if (mydata[i] < 0x10) Serial.print("0");
        Serial.print(mydata[i], HEX);
        if (i < PORT1_PAYLOAD_LEN - 1) Serial.print(" ");
    }
    Serial.println(F("]"));

    g_lastTxPort = 1;
    // §3.8 verification moved to Port 2 (do_ping()) as of 2026-09-09 — root
    // cause and full explanation in the g_radioVerifyPending comment block
    // near the top of this file. Port 1 telemetry is always unconfirmed now;
    // do not wire a confirmed flag back in here without re-reading that
    // comment first — the same async-set-vs-already-sealed-frame race will
    // reproduce.
#ifdef MILLIS_BENCH_E
    LMIC_requestNetworkTime(benchNetTimeCb, NULL);
    Serial.println(F("BENCH_E: DeviceTimeReq queued"));
#endif
    LMIC_setTxData2(1, mydata, sizeof(mydata), 0);
}

// Max application payload at the current data rate (EU868: DR0-2 51 B, DR3 115 B, DR4+ 222 B) minus a small margin for
// MAC commands piggybacked on the frame. If LMIC still rejects the length, do_flush() retries with one record fewer.
static uint8_t flushMaxPayload() {
    uint8_t dr = LMIC.datarate;
    uint8_t m = (dr <= 2) ? 51 : (dr == 3) ? 115 : 222;
    return m - 4;
}

// Port 3: saved readings, unconfirmed. Format (version 1), see payload.txt:
//   [0] 0x01  [1] boot id  [2-3] sender uptime min  [4-5] seq of first record  [6] count n
//   then n x 11 B: temp i16 (x10) | hum u8 (x2) | dust u16 | gas u16 | batt u16 | age u16 (min before the sender uptime, 0xFFFF unknown)
void do_flush(osjob_t* j) {
    if (s_flushInFlight) {
        // A batch is already queued and waiting for airtime. Leave the
        // scheduling flags alone; its EV_TXCOMPLETE will advance the chain.
        Serial.println(F("Flush already queued - waiting for completion"));
        return;
    }
    if (LMIC.opmode & OP_TXRXPEND) {
        if (++txRetryCount <= MAX_TX_RETRIES) {
            Serial.println(F("OP_TXRXPEND, retrying flush in 5s"));
            os_setTimedCallback(&retryJob, os_getTime() + sec2osticks(5), do_flush);
            return;
        }
        txRetryCount = 0;
        txWasImmediate = false; scheduleAfterImmediateSend = false;
        Serial.println(F("Flush retry limit reached, resuming schedule (records stay saved)"));
        scheduleNextCadence();
        return;
    }
    txRetryCount = 0;

    static uint8_t flushBuf[READING_FRAME_MAX_BYTES];
    uint8_t maxLen = flushMaxPayload();
    uint16_t nowMin = readingStore_uptimeMin();

    while (true) {
        uint8_t len = 0;
        if (g_linkOnline && !energyGuard_active() && readingStore_unsentCount() > 0 &&
            (s_outageDrainPending || readingStore_persistentUnsentCount() > 0)) {
            uint16_t intervalS = s_outageDrainPending ? TX_INTERVAL : energyGuard_intervalS(TX_INTERVAL);
            len = readingStore_buildFrame(flushBuf, maxLen, nowMin, intervalS, s_outageDrainPending);
        }

        if (len == 0) break;
        if (LMIC_setTxData2(3, flushBuf, len, 1) == 0) {
            g_lastTxPort = 3;
            s_flushInFlight = true;
            txWasImmediate = true;
            scheduleAfterImmediateSend = true;
            Serial.print(F("Flush frame: ")); Serial.print(flushBuf[6]);
            Serial.print(F(" records from seq "));
            Serial.print(((uint16_t)flushBuf[4] << 8) | flushBuf[5]);
            Serial.print(F(", ")); Serial.print(len); Serial.println(F(" B"));
            return;
        }
        if (maxLen < READING_FRAME_HDR_BYTES + 2 * READING_FRAME_REC_BYTES) break;   // cannot shrink further
        maxLen -= READING_FRAME_REC_BYTES;
    }
    // Nothing to send, or LMIC refused every size: leave the records saved and go back to the normal schedule.
    txWasImmediate = false; scheduleAfterImmediateSend = false;
    Serial.println(F("Flush: nothing sent, resuming schedule"));
    scheduleNextCadence();
}

// ---- Serial command mirror -------------------------------------------
// Reuses parseDownlinkCommand()/applyDownlink() so the exact same command
// set works identically over USB serial and over a TTN downlink. Only
// serviced from loop(), so it only sees bytes typed while the device is
// awake between sleeps — same visibility window an operator already gets
// from the boot burst / TX_INTERVAL cadence, no wake-from-sleep involved.
// Must stay >= DownlinkParser.h's cmdstr[] (currently 28) for the same
// reason dlBuf above does — 24 happened to be exactly enough for /@V#'s
// 23 characters (no margin), which is how the cmdstr[20] truncation bug
// went undetected until a bench test caught it serial-side first.

// Applies a parsed command that arrived over serial. Actions that are safe
// regardless of transport (interval/SF/TX-power change, restart, factory
// reset, EEPROM save) go straight through the shared applyDownlink(). PING,
// TX_NOW and SLEEP are special-cased here because their downlink versions
// are normally driven from inside onEvent(EV_TXCOMPLETE)'s scheduling logic
// (see the comments in applyDownlink() itself), which a serial command
// isn't part of.
void handleSerialDownlinkResult(const DownlinkResult& r) {
    // Serial commands are meant to act exactly like a real downlink — see the
    // matching check on the radio path (onEvent()'s LMIC.dataLen handling,
    // below applyDownlink()). That path already ends the boot burst early on
    // any real downlink; this one didn't, so a serial-issued command during
    // boot burst silently kept ticking on the slow boot-burst cadence instead
    // of taking effect immediately. Same DL_BURST_START exception as the
    // radio path — /@B# shouldn't auto-cancel the burst it just armed.
    if (r.action != DL_NONE && r.action != DL_BURST_START && bootBurstFramesLeft > 0) {
        Serial.println(F("Serial command received during boot burst - ending burst early"));
        bootBurstFramesLeft = 0;
    }
    switch (r.action) {
        case DL_PING:
            if (r.value == 1) {
                // /@P1# over serial: unlike bare /@P#, this one actually
                // radios out — the whole point is testing the confirmed-
                // uplink ACK path, which a local dump can't do.
                g_pingConfirmedPending = true;
                if (LMIC_queryTxReady()) {
                    txWasImmediate = true;
                    scheduleAfterImmediateSend = true;
                    nextPeriodicDelayAfterImmediate = TX_INTERVAL;
                    os_setCallback(&immediateJob, do_ping);
                    Serial.println(F("Confirmed ping requested - sending now"));
                } else {
                    // A send is already in flight (this cycle's own regular
                    // telemetry, most likely, since handleSerialCommands()
                    // runs before os_runloop_once() and can land right as
                    // that job is about to fire) — firing immediateJob now
                    // would race it and silently lose. Defer to
                    // EV_TXCOMPLETE instead, once the TX path is
                    // guaranteed clear.
                    g_pingConfirmedDeferred = true;
                    Serial.println(F("Confirmed ping requested - TX path busy, deferring until current send completes"));
                }
            } else {
                // Local-only dump — no radio ping, no airtime cost, and it
                // deliberately does NOT touch the compact 16-byte downlink
                // status frame, which stays exactly as-is.
                printVerboseStatus();
            }
            break;

        case DL_TX_NOW:
            readSensorsForMode();
            appendCurrentSampleForTx(g_linkOnline ? RECORD_STATE_UNCONFIRMED : RECORD_STATE_OUTAGE_PERSISTENT);
            txWasImmediate = true;
            scheduleAfterImmediateSend = true;
            nextPeriodicDelayAfterImmediate = TX_INTERVAL;
            os_setCallback(&immediateJob, do_send);
            break;

        case DL_SF_CHANGED:
        case DL_TXPOWER_CHANGED:
        case DL_CHANNEL_CHANGED:
            // §3.8 verification, serial path: applyDownlink() applies the
            // change and arms g_radioVerifyViaPing/g_pingConfirmedPending;
            // same busy-check/defer pattern as DL_PING above — this is the
            // path the original race lived on (handleSerialCommands() runs
            // before os_runloop_once(), so it can land right as a scheduled
            // sendjob is about to fire). DL_CHANNEL_CHANGED added 2026-09-10
            // — caught missing during bench test: /@C0# applied to RAM and
            // armed the flags (confirmed via /@P#'s "awaiting ping: yes")
            // but no ping ever fired since this case block was the only
            // thing standing between "flag armed" and "ping actually sent"
            // on the serial path, and it fell through to the bare
            // applyDownlink()-only default instead.
            applyDownlink(r);
            if (LMIC_queryTxReady()) {
                txWasImmediate = true;
                scheduleAfterImmediateSend = true;
                nextPeriodicDelayAfterImmediate = TX_INTERVAL;
                os_setCallback(&immediateJob, do_ping);
                Serial.println(F("Verification ping - sending now"));
            } else {
                g_pingConfirmedDeferred = true;
                Serial.println(F("Verification ping - TX path busy, deferring until current send completes"));
            }
            break;

        case DL_SLEEP: {
            unsigned long sleepMin = r.value;
            if (sleepMin > MAX_SLEEP_MINUTES) {
                Serial.print(F("Sleep clamped to "));
                Serial.print(MAX_SLEEP_MINUTES);
                Serial.println(F(" min"));
                sleepMin = MAX_SLEEP_MINUTES;
            }
            sleepOverridePending = true;
            sleepOverrideSeconds = sleepMin * 60UL;
            Serial.println(F("Sleep override armed for the next cycle"));
            break;
        }

        default:
            applyDownlink(r);
            break;
    }
}

// Polled once per loop() iteration. Accumulates bytes until '#' closes a
// command, then feeds the exact same parser a real downlink uses. Only ever
// sees bytes while loop() is running, i.e. while the device is awake — see
// the header comment on SERIAL_CMD_BUFFER_SIZE above.
void handleSerialCommands() {
    DownlinkResult r;
    // Drain every complete command already buffered, not just one per loop() pass. A night boot queues the sleep job
    // before the first pass, so with one command per pass only the first command typed at boot was ever read; the rest
    // waited in the UART buffer through the whole sleep. Send setters first and blocking commands (/@S /@T /@R) last.
    while (serialConsole_poll(r)) {
        handleSerialDownlinkResult(r);
        wdt_reset();
    }
}

void setup() {
    // Capture + clear the AVR reset-cause register as the very first thing in
    // setup(), before this sketch's own wdt_enable()-based softwareReset()
    // (or anything else) can touch it, then immediately disable the watchdog
    // so a watchdog-triggered reset (used intentionally by softwareReset())
    // can't re-arm into a boot loop on its own.
    uint8_t mcusr_boot = MCUSR;
    MCUSR = 0;
    wdt_disable();
    // Offline fallback: measure the watchdog against the crystal now (hang watchdog is still off). ~2.3 s once per boot.
#ifdef WDT_SCALE_PERMILLE_NODE
    uint16_t wdtAwakePermille = 0;   // this chip's factor is built in (node_calibration.h): no boot measurement
#else
    uint16_t wdtAwakePermille = calibrateWdtScale(2);
    wdtScaleApplyAwake(wdtAwakePermille);
#endif

    // Read the checkpoint left by the previous boot's active-work window
    // before we overwrite it below. BUSY means that window never closed
    // cleanly — see EEPROM_HANG_CHECKPOINT_ADDR above. mcusr_boot's WDRF bit
    // reads 0x00 on every boot on this board (bootloader clears it before
    // the sketch runs), so it can't be trusted to tell a watchdog reset from
    // any other kind. Consume both markers immediately so a later, unrelated
    // reset can't misread them.
    //
    // incompleteCycle does NOT mean "confirmed hang" — see the limitation
    // note on RESET_FLAG_INCOMPLETE_CYCLE above. A genuine power interruption
    // mid-cycle sets the exact same signature (BUSY checkpoint, non-matching
    // RAM marker) as a watchdog-caught hang; nothing here can tell them apart
    // without MCUSR's PORF bit, which isn't available.
    bool incompleteCycle = config_evaluate_reset_cause(mcusr_boot, g_resetReasonByte);

    // Arm the hang-detection watchdog for the rest of setup().
    wdt_enable(HANG_WATCHDOG_TIMEOUT);
    config_set_checkpoint(HANG_CHECKPOINT_BUSY);

    Serial.begin(38400);
#ifdef WDT_SCALE_PERMILLE_NODE
    Serial.print(F("Node ")); Serial.print(F(NODE_NAME)); Serial.print(F(": WDT scale: node value ")); Serial.print(g_wdtScalePermille); Serial.println(F(" permille (calibrated, no boot measurement)"));
#else
    Serial.print(F("WDT calibration: awake=")); Serial.print(wdtAwakePermille);
    Serial.print(F(" permille -> sleep scale=")); Serial.print(g_wdtScalePermille); Serial.println(F(" permille (no node value)"));
#endif
    // XOR in micros() (free-running since power-up) alongside the floating-pin
    // read: a batch of near-identical boards powered on together can land on
    // very similar A0 noise, but each board's own reset/bootloader timing
    // skew still differs, which decorrelates the seed across the batch.
    randomSeed(analogRead(A0) ^ micros());
    Serial.println(F("DHT22 + GP2Y1010AU0F Dust + Gas SmartGated | ABP | Single-CH 868.1 MHz"));

#ifdef WITH_EEPROM
    // Deliberately ahead of pmosInit()/readSensorsForMode() below: a
    // committed NORMAL_NO_GAS profile (the whole point of §3.5 — see
    // Field_Diagnostics_And_Remote_Configuration_2026-09-07.md) has to gate
    // the gas rail starting with THIS boot's first cycle, not the one after.
    // The main config_load_from_eeprom() later in setup() re-reads the same
    // bytes (harmless — nothing changes them in between) for seqnoUp/
    // tx_interval, which do need LMIC initialized first; this early read
    // only consumes the profile_* fields.
    if (config_load_from_eeprom() && my_eeprom.profile_saved == 1) {
        // profile_mode is never written as MODE_ALWAYS_ON (see
        // DL_COMMIT_PARAMS), but re-check on the read side too — a
        // struct-size-changed reflash landing on stale/garbage bytes that
        // happen to pass the profile_saved==1 gate should still never boot
        // into ALWAYS_ON, since that mode's brownout-loop risk is exactly
        // why g_mode is RAM-only in the first place (project_smartgated_
        // firmware_2026-09-01).
        if (my_eeprom.profile_mode <= MODE_NORMAL_NO_GAS) g_mode = my_eeprom.profile_mode;
        if (my_eeprom.profile_sensorMask <= 0x0F) g_sensorMask = my_eeprom.profile_sensorMask;
        if (my_eeprom.profile_gasWarmupS == 300) {
            my_eeprom.profile_gasWarmupS = GAS_WARMUP_S_DEFAULT;
            config_save_all();
        }
        if (my_eeprom.profile_gasWarmupS >= 10 && my_eeprom.profile_gasWarmupS <= 3600) {
            gas_setWarmupSeconds(my_eeprom.profile_gasWarmupS);
        }
        if (my_eeprom.profile_pingRatio >= 1 && my_eeprom.profile_pingRatio <= 250) g_pingRatio = my_eeprom.profile_pingRatio;
        // Same ordering re-check as DL_BATTERY_THRESHOLDS_CHANGED's parser-
        // side validation — a struct-size-changed reflash landing on stale/
        // garbage bytes that happen to pass profile_saved==1 should still
        // never install a broken (e.g. inverted-hysteresis) threshold set.
        if (my_eeprom.profile_battCritical > 5 && my_eeprom.profile_battCritical < 440 &&
            my_eeprom.profile_battLow > 5 && my_eeprom.profile_battLow < 440 &&
            my_eeprom.profile_battPsave > 5 && my_eeprom.profile_battPsave < 440 &&
            my_eeprom.profile_battRecover > 5 && my_eeprom.profile_battRecover < 440 &&
            my_eeprom.profile_battPsaveRecover > 5 && my_eeprom.profile_battPsaveRecover < 440 &&
            my_eeprom.profile_battCritical < my_eeprom.profile_battLow &&
            my_eeprom.profile_battLow < my_eeprom.profile_battPsave &&
            my_eeprom.profile_battPsave < my_eeprom.profile_battRecover &&
            my_eeprom.profile_battRecover <= my_eeprom.profile_battPsaveRecover) {
            g_battLow          = my_eeprom.profile_battLow;
            g_battRecover      = my_eeprom.profile_battRecover;
            g_battCritical     = my_eeprom.profile_battCritical;
            g_battPsave        = my_eeprom.profile_battPsave;
            g_battPsaveRecover = my_eeprom.profile_battPsaveRecover;
        }
        Serial.println(F("EEPROM: restoring committed profile (mode/mask/warmup/ping-ratio/battery-thresholds)"));
    }
#endif

    Serial.print(F("Boot mode: ")); Serial.println(modeName(g_mode));
    Serial.print(F("Battery strategy: ")); Serial.println(batteryStrategyName());

    // Reset-cause diagnostic: BORF is the one that matters for tonight's
    // investigation (a real brown-out, independent of this sketch's own
    // watchdog-based resets). Blinked out on LED_BUILTIN so it's readable
    // with zero extra hardware — see blinkResetReason() above.
    Serial.print(F("Reset cause MCUSR=0x")); Serial.println(mcusr_boot, HEX);
    Serial.print(F("  BORF(brownout)=")); Serial.println((mcusr_boot & _BV(BORF)) ? 1 : 0);
    Serial.print(F("  WDRF(watchdog, expected on every softwareReset())=")); Serial.println((mcusr_boot & _BV(WDRF)) ? 1 : 0);
    Serial.print(F("  EXTRF(external/reset pin)=")); Serial.println((mcusr_boot & _BV(EXTRF)) ? 1 : 0);
    Serial.print(F("  PORF(power-on)=")); Serial.println((mcusr_boot & _BV(PORF)) ? 1 : 0);
    if (incompleteCycle) Serial.println(F("  -> EEPROM checkpoint was BUSY with no intentional-reset marker: previous active-work window never closed (hang caught by the watchdog, OR power lost mid-cycle - can't tell which without MCUSR's PORF bit)"));
    blinkResetReason(mcusr_boot);

    // Battery guard: checked before touching DHT22/dust/gas/LMIC init. A
    // confirmed-low reading skips straight to the low-power path (never
    // returns until the battery recovers and softwareReset()'s back into a
    // clean boot). An unreadable/implausible reading fails open — same
    // "don't gate the whole node on an uncertain sensor" policy already used
    // for dust/gas faults elsewhere in this sketch.
    // PMOS gates default OFF from the earliest possible point in boot,
    // ahead of even the battery guard check below — a GPIO pinMode/write
    // doesn't need either rail powered, so there's no reason to leave the
    // gates floating any longer than necessary.
    pmosInit();

    battery_Init();
    int battRaw = battery_getADC(&g_batt_valid);
    g_batt = battRaw;
    Serial.print(F("Boot battery ADC: ")); Serial.println(battRaw);
#ifdef POWER_SAVE_ENABLED
    if (g_batt_valid && battRaw < g_battLow) {
        lowPowerBoot(battRaw, g_batt_valid);
        // never reached — lowPowerBoot() only exits via softwareReset()
    }
    if (g_batt_valid && battRaw < g_battPsave) {
        // Battery's already in the gentler power-save tier at boot: skip the
        // boot burst (10 fast frames would undercut the point of slowing
        // down) and start straight in power-save cadence instead of waiting
        // a cycle for the mid-run hysteresis check to notice.
        powerSaveMode = true;
        Serial.println(F("Battery <~25% at boot - skipping boot burst, starting in power-save mode"));
    } else {
        bootBurstFramesLeft = BOOT_BURST_FRAME_COUNT;
    }
#else
    bootBurstFramesLeft = BOOT_BURST_FRAME_COUNT;
#endif // POWER_SAVE_ENABLED — depletion test: always proceed into the normal boot path regardless of boot-time battery level

    // Boot on weak cell must stay silent: the usual boot sequence (confirmed init ping, 9-frame burst)
    // is the heaviest TX load the node has, and a reset loop on a weak cell would repeat it every boot.
    bool belowCrit = (g_batt_valid && battRaw < g_battCritical);
    readingStore_begin(!belowCrit);
    weakBatteryBootSilent = energyGuard_bootSilent(battRaw, g_batt_valid);

    sensorManager_init();
    wdt_reset();
    // No boot-time sensor read here anymore (2026-09-11) — deferred until
    // the confirmed init-phase gate below actually resolves (ACK or
    // exhausted retries), in EV_TXCOMPLETE's g_inInitPhase handling. Nothing
    // needs a real reading before then: do_ping()'s existing sentinel path
    // (lastSensorValid == false) already covers "no reading yet" for
    // whatever status frames the gate sends while it's still resolving.

    initRadioOnce();
    wdt_reset();

#ifdef WITH_EEPROM
    if (config_load_from_eeprom()) {
        Serial.println(F("EEPROM: restoring config"));
        Serial.print(F("  seqnoUp : ")); Serial.println(my_eeprom.seqnoUp);
#ifdef FORCE_DEFAULT_VALUE
        Serial.println(F("  FORCE_DEFAULT_VALUE: resetting"));
        my_eeprom.seqnoUp          = 0;
        my_eeprom.tx_interval      = TX_INTERVAL;
        my_eeprom.tx_interval_saved = 0;
        config_save_all();
#else
        LMIC.seqnoUp = my_eeprom.seqnoUp;
        Serial.print(F("  seqnoUp restored to: ")); Serial.println(LMIC.seqnoUp);
        if (my_eeprom.tx_interval_saved == 1 && my_eeprom.tx_interval >= 30) {
            TX_INTERVAL = my_eeprom.tx_interval;
            Serial.print(F("  TX_INTERVAL: ")); Serial.print(TX_INTERVAL); Serial.println(F("s"));
        }
        // §3.8: restored here rather than the early profile-restore block
        // above — SCG_setRadioConfig()/LMIC_setDrTxpow()/SCG_setChannel()
        // need LMIC already initialized (initRadioOnce() just ran above,
        // which populates SCG_knownFreq/SCG_knownDrMap that SCG_setChannel()
        // reads from), unlike mode/mask/warmup/thresholds which had to land
        // before the first sensor read.
        if (my_eeprom.radio_saved == 1) {
            SCG_currentDR = my_eeprom.radio_sf;
            SCG_currentTXP = my_eeprom.radio_txp;
            SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
            g_radioKnownGoodDR = SCG_currentDR;
            g_radioKnownGoodTXP = SCG_currentTXP;
            // Added 2026-09-10: channel restored alongside DR/TXP, same
            // radio_saved gate — see the struct field's comment.
            SCG_setChannel(my_eeprom.radio_channel);
            g_channelKnownGood = my_eeprom.radio_channel;
            Serial.print(F("  Radio config restored: DR=")); Serial.print(SCG_currentDR);
            Serial.print(F(" TXP=")); Serial.print(SCG_currentTXP);
            Serial.print(F("dBm Channel=")); Serial.println(my_eeprom.radio_channel);
        }
        // §4 dead-man failsafe (2026-09-09) — range check rather than a
        // dedicated _saved flag, see the struct field's comment. Garbage
        // (> DEADMAN_FAIL_THRESHOLD) from a struct-size-changed reflash is
        // silently rejected, leaving g_deadmanFailCount at its RAM default
        // (0) rather than trusting an out-of-range byte.
        if (my_eeprom.deadman_fail_count <= DEADMAN_FAIL_THRESHOLD) {
            g_deadmanFailCount = my_eeprom.deadman_fail_count;
            if (g_deadmanFailCount > 0) {
                Serial.print(F("  Dead-man fail count restored: "));
                Serial.println(g_deadmanFailCount);
            }
        }
#endif
    } else {
        config_init_defaults(TX_INTERVAL);
        Serial.println(F("EEPROM: initialized"));
    }
    if (my_eeprom.outage_fallback_active == 1) {
        g_linkOnline = false;
    }
#endif

#ifdef BENCH_VARIANT
    // Bench build: deterministic and fast. Applied AFTER the EEPROM profile
    // restore so a committed radio/interval profile cannot override it.
    // Known-good values are moved too, otherwise a verify rollback would jump
    // straight back to 14 dBm.
    TX_INTERVAL = 90;
    g_sagIntervalS = 90;   // sag storage cadence too, otherwise a weak cell falls back to 3600 s
    SCG_currentTXP = 2;
    g_radioKnownGoodTXP = 2;
    SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
    Serial.println(F("BENCH VARIANT: low power off, TX_INTERVAL 90 s, TXP 2 dBm"));
#endif

#ifdef MILLIS_BENCH_TX_S
    // TEMPORARY dev-bench override (build with -DMILLIS_BENCH_TX_S=30). Forces a short fixed cadence and skips the
    // 90 s boot burst so timing experiments need no serial commands. The periodic EEPROM save will write this
    // interval back: restore TX_INTERVAL (1800 s) when the bench work ends.
    TX_INTERVAL = MILLIS_BENCH_TX_S;
    bootBurstFramesLeft = 0;
    Serial.print(F("BENCH: TX_INTERVAL forced to ")); Serial.print(TX_INTERVAL); Serial.println(F("s, boot burst off"));
#endif

    if (weakBatteryBootSilent) {
        Serial.println(F("WEAK BATTERY BOOT: no init ping, no boot burst - going straight to sag storage cycle"));
        scheduleNextCadence();
        return;
    }

    // One-time random stagger before the very first TX, so a batch of nodes
    // powered on together don't stay phase-correlated through the boot burst
    // (per-cycle jitter alone only kicks in starting with frame 2).
    unsigned long startupStagger_ms = random(0, BOOT_STARTUP_STAGGER_MAX_MS + 1);
    Serial.print(F("Startup stagger: "));
    Serial.print(startupStagger_ms / 1000.0, 1);
    Serial.println(F("s"));
#ifdef LOW_POWER
    lowPower(startupStagger_ms);
#else
    wdtSafeDelay(startupStagger_ms);
#endif

    // Confirmed connectivity boot gate (2026-09-11) — unified for both a
    // clean boot and a dirty restart (incompleteCycle no longer branches
    // here; it still only feeds g_resetReasonByte above and the diagnostic
    // print). Neither case is trusted to have actually reached the gateway
    // until this confirmed do_ping() gets ACK'd or exhausts its retry
    // budget — see g_inInitPhase's declaration near the top of the file and
    // its EV_TXCOMPLETE handling for the rest of this. The reset-reason
    // byte rides along automatically since it's a permanent field in
    // do_ping()'s payload, so a dirty restart is still reported on the very
    // first frame either way.
    radioFailsafe_startInitPhase();
    Serial.println(F("Init-phase check - confirming gateway reachability before starting normal operation"));
    os_setCallback(&sendjob, do_ping);
}

void loop() {
    wdt_reset(); // pet the hang watchdog — see HANG_WATCHDOG_TIMEOUT
    SCG_enforce();
    handleSerialCommands();
    os_runloop_once();
}
