#include "radio_failsafe.h"
#include "node_config.h"
#include "power_manager.h"

uint8_t SCG_currentDR   = DR_SF9;
#ifdef BENCH_VARIANT
uint8_t SCG_currentTXP  = 2;   // bench: 2 dBm for a short, repeatable link margin
#else
uint8_t SCG_currentTXP  = 4;   // uy1-mega-2 field setting
#endif
uint8_t g_scgChannel    = 0;

uint8_t g_radioKnownGoodDR  = DR_SF9;
#ifdef BENCH_VARIANT
uint8_t g_radioKnownGoodTXP = 2;
#else
uint8_t g_radioKnownGoodTXP = 4;   // uy1-mega-2 field setting
#endif
uint8_t g_channelKnownGood  = 0;

bool g_radioVerifyPending = false;
bool g_radioVerifyViaPing = false;

uint8_t g_uplinkCountSinceDeadman = 0;
uint8_t g_deadmanFailCount        = 0;
bool g_deadmanPingPending         = false;
bool g_deadmanVerifyPending       = false;

bool g_inInitPhase       = false;
uint8_t g_initRetriesLeft = 0;
uint8_t bootBurstFramesLeft = 0;
unsigned long g_burstIntervalS = BOOT_BURST_INTERVAL_S;
bool radioInitialized = false;

uint8_t g_lastTxPort          = 0;
uint8_t g_uplinkCountSincePing = 0;
uint8_t g_pingRatio           = 10;
bool g_pingConfirmedPending   = false;
bool g_pingConfirmedDeferred  = false;

uint8_t sfToDr(uint8_t sf) {
    switch (sf) {
        case 7:  return DR_SF7;
        case 8:  return DR_SF8;
        case 9:  return DR_SF9;
        case 10: return DR_SF10;
        case 11: return DR_SF11;
        case 12: return DR_SF12;
        default: return SCG_currentDR;
    }
}

uint8_t drToSf(uint8_t dr) {
    switch (dr) {
        case DR_SF7:  return 7;
        case DR_SF8:  return 8;
        case DR_SF9:  return 9;
        case DR_SF10: return 10;
        case DR_SF11: return 11;
        case DR_SF12: return 12;
        default:      return 0;
    }
}

void radioFailsafe_requestVerify() {
    g_radioVerifyViaPing = true;
    g_pingConfirmedPending = true;
}

void radioFailsafe_deadmanRollback() {
    Serial.println(F("DEAD-MAN FAILSAFE TRIGGERED - reverting to hardcoded defaults"));
    SCG_setChannel(DEADMAN_FALLBACK_CHANNEL);
    SCG_currentDR = DR_SF9;
    SCG_currentTXP = DEADMAN_FALLBACK_TXP;
    SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
    g_radioKnownGoodDR = SCG_currentDR;
    g_radioKnownGoodTXP = SCG_currentTXP;
    TX_INTERVAL = DEFAULT_TX_INTERVAL;
    g_mode = MODE_NORMAL;
    g_deadmanFailCount = 0;
    g_uplinkCountSinceDeadman = 0;
#ifdef WITH_EEPROM
    config_clear_failsafe(DEFAULT_TX_INTERVAL);
    Serial.println(F("EEPROM: cleared committed profile/radio config"));
#endif
}

#define FALLBACK_PROBE_FAIL_LIMIT 14 // 14 probes = 48 cadence cycles = 24.0h at 30m cadence

static uint8_t s_fallbackProbeCount = 0;
static uint8_t s_fallbackFailedProbes = 0;

void radioFailsafe_radioOnlyRollback() {
    Serial.println(F("FALLBACK RADIO DEAD-MAN: 14 failed probes (48 cycles, ~24h) - reverting radio to CH0 SF9 2dBm"));
    bool differs = (SCG_currentDR != DR_SF9 || SCG_currentTXP != 2 || g_channelKnownGood != DEADMAN_FALLBACK_CHANNEL);
    SCG_setChannel(DEADMAN_FALLBACK_CHANNEL);
    SCG_currentDR = DR_SF9;
    SCG_currentTXP = 2; // Default 2 dBm to protect weak cell from 14 dBm sag
    SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
    g_radioKnownGoodDR = SCG_currentDR;
    g_radioKnownGoodTXP = SCG_currentTXP;
    g_channelKnownGood = DEADMAN_FALLBACK_CHANNEL;
#ifdef WITH_EEPROM
    if (differs) {
        config_commit_radio(SCG_currentDR, SCG_currentTXP, DEADMAN_FALLBACK_CHANNEL);
    }
#endif
}

bool radioFailsafe_fallbackProbeDue() {
    s_fallbackProbeCount++;
    // Probe on cycles 1, 2, 4, then once every 4 cycles (every 2 hours at 30m)
    if (s_fallbackProbeCount <= 2 || s_fallbackProbeCount == 4 || (s_fallbackProbeCount % 4 == 0)) {
        return true;
    }
    return false;
}

void radioFailsafe_onFallbackProbeResult(bool ack) {
    if (ack) {
        s_fallbackProbeCount = 0;
        s_fallbackFailedProbes = 0;
    } else {
        s_fallbackFailedProbes++;
        Serial.print(F("Fallback probe failed ("));
        Serial.print(s_fallbackFailedProbes);
        Serial.print(F("/"));
        Serial.print(FALLBACK_PROBE_FAIL_LIMIT);
        Serial.println(F(")"));
        if (s_fallbackFailedProbes >= FALLBACK_PROBE_FAIL_LIMIT) {
            s_fallbackFailedProbes = 0;
            radioFailsafe_radioOnlyRollback();
        }
    }
}

bool radioFailsafe_checkPeriodicPingDue(bool isExtraTxComplete) {
    bool pingRatioReached = false;
    if (!isExtraTxComplete && g_lastTxPort == 1) {
        if (++g_uplinkCountSincePing >= g_pingRatio) {
            g_uplinkCountSincePing = 0;
            pingRatioReached = true;
        }
        if (++g_uplinkCountSinceDeadman >= DEADMAN_PING_RATIO) {
            g_uplinkCountSinceDeadman = 0;
            g_deadmanPingPending = true;
        }
    }
    return pingRatioReached;
}

static void revertRadioToKnownGood() {
    SCG_currentDR = g_radioKnownGoodDR;
    SCG_currentTXP = g_radioKnownGoodTXP;
    SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
    if (g_scgChannel != g_channelKnownGood) SCG_setChannel(g_channelKnownGood);
}

void radioFailsafe_onTxComplete(bool ack, bool lenErr) {
    if (lenErr) {
        // The frame never reached the air. Do not commit or revert the radio config,
        // and do not count a dead-man failure.
        if (g_radioVerifyPending) {
            g_radioVerifyPending = false;
            g_radioVerifyViaPing = true;
        }
        if (g_deadmanVerifyPending) {
            // Do NOT set g_deadmanPingPending here. The completion scheduler at
            // .ino:787 consumes that flag by scheduling an immediate do_ping.
            // A persistent length error would then spin the device in an
            // unthrottled immediate-TX loop. The regular dead-man cadence
            // re-arms the check at the next DEADMAN_PING_RATIO.
            g_deadmanVerifyPending = false;
        }
        Serial.println(F("TX length error: frame rejected before air; verification deferred"));
        return;
    }

    if (g_radioVerifyPending) {
        g_radioVerifyPending = false;
        if (ack) {
            Serial.println(F("Radio config verified - committing to EEPROM"));
            g_radioKnownGoodDR = SCG_currentDR;
            g_radioKnownGoodTXP = SCG_currentTXP;
            g_channelKnownGood = g_scgChannel;
            g_deadmanFailCount = 0;
#ifdef WITH_EEPROM
            config_commit_radio(SCG_currentDR, SCG_currentTXP, g_scgChannel);
            Serial.println(F("EEPROM saved (radio config)"));
#endif
        } else {
            Serial.println(F("Radio config NOT verified (no ACK after retries) - reverting to last known-good"));
            revertRadioToKnownGood();
        }
    }

    if (g_deadmanVerifyPending) {
        g_deadmanVerifyPending = false;
        if (ack) {
            if (g_deadmanFailCount != 0) {
                g_deadmanFailCount = 0;
                Serial.println(F("Dead-man check OK - fail count reset"));
#ifdef WITH_EEPROM
                config_set_deadman_fail_count(0);
#endif
            }
        } else {
            if (g_deadmanFailCount < DEADMAN_FAIL_THRESHOLD) g_deadmanFailCount++;
            Serial.print(F("Dead-man check failed - fail count now "));
            Serial.println(g_deadmanFailCount);
#ifdef WITH_EEPROM
            config_set_deadman_fail_count(g_deadmanFailCount);
#endif
            if (g_deadmanFailCount >= DEADMAN_FAIL_THRESHOLD) {
                radioFailsafe_deadmanRollback();
            }
        }
    }
}

void radioFailsafe_startInitPhase() {
    g_inInitPhase = true;
    g_initRetriesLeft = INIT_PHASE_ACK_RETRIES;
    g_pingConfirmedPending = true;
}

InitPhaseResult radioFailsafe_handleInitPhase(bool ack, bool lenErr, unsigned long burstIntervalS, osjob_t* j, osjobcb_t retryCb) {
    if (!g_inInitPhase) return INIT_PHASE_NOT_ACTIVE;

    static uint8_t s_lenErrRetries = 0;

    if (lenErr) {
        if (s_lenErrRetries < 3) {
            s_lenErrRetries++;
            g_pingConfirmedPending = true;
            unsigned long retry_ms = applyJitter(burstIntervalS * 1000UL, JITTER_PERCENT_BURST, JITTER_MIN_MS);
            Serial.print(F("Init-phase TX length error - retrying ("));
            Serial.print(s_lenErrRetries);
            Serial.println(F("/3)"));
            Serial.flush();
            smartSleep(retry_ms);
            os_setCallback(j, retryCb);
            return INIT_PHASE_RETRYING;
        }
        s_lenErrRetries = 0;
        revertRadioToKnownGood();
        g_inInitPhase = false;
        Serial.println(F("Init-phase TX length error x3 - reverted to known-good radio config"));
        return INIT_PHASE_COMPLETED;
    }
    s_lenErrRetries = 0;

    if (!ack && g_initRetriesLeft > 0) {
        // Print before decrementing: the two retries read 2 then 1.
        Serial.print(F("Init-phase check not ACK'd - retrying ("));
        Serial.print(g_initRetriesLeft);
        Serial.println(F(" attempts left)"));
        g_initRetriesLeft--;
        g_pingConfirmedPending = true;
        unsigned long retry_ms = applyJitter(burstIntervalS * 1000UL, JITTER_PERCENT_BURST, JITTER_MIN_MS);
        Serial.print(F("Sleeping "));
        Serial.print(retry_ms / 1000.0, 1);
        Serial.println(F("s (init-phase retry)"));
        Serial.flush();
        smartSleep(retry_ms);
        os_setCallback(j, retryCb);
        return INIT_PHASE_RETRYING;
    }

    if (ack) {
        Serial.println(F("Init-phase check ACK'd - starting normal operation"));
    } else {
        Serial.println(F("Init-phase check exhausted retries - starting normal operation unconfirmed"));
    }
    g_inInitPhase = false;
    return INIT_PHASE_COMPLETED;
}

uint8_t radioFailsafe_computePingReason() {
    if (g_inInitPhase)               return PING_REASON_INIT;
    else if (g_radioVerifyViaPing)   return PING_REASON_VERIFY;
    else if (!g_linkOnline)          return PING_REASON_DEADMAN;
    else if (g_deadmanPingPending)   return PING_REASON_DEADMAN;
    else if (g_pingConfirmedPending) return PING_REASON_MANUAL;
    else                             return PING_REASON_RATIO;
}

bool radioFailsafe_armPingTx() {
    g_lastTxPort = 2;
    bool confirmedNow = g_pingConfirmedPending || g_deadmanPingPending || g_radioVerifyViaPing;
    g_pingConfirmedPending = false;
    if (g_radioVerifyViaPing) {
        g_radioVerifyViaPing = false;
        g_radioVerifyPending = true;
    }
    if (g_deadmanPingPending) {
        g_deadmanPingPending = false;
        g_deadmanVerifyPending = true;
    }
    return confirmedNow;
}
