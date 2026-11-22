#include "energy_guard.h"

#if BATTERY_STRATEGY == BATTERY_STRATEGY_STORAGE

#include "sensor_manager.h"
#include "battery_sensor.h"
#include "battery_guard.h"
#include "power_manager.h"
#include "mode_controller.h"
#include "radio_failsafe.h"

static bool    s_critical  = false;
static uint8_t s_exitReads = 0;

enum EnergyEvent { ENERGY_NONE = 0, ENERGY_SAG_ENTERED, ENERGY_SAG_EXITED };

static EnergyEvent energyGuard_update(int battRaw, bool battValid) {
    uint8_t ovr = readingStore_override();
    if (ovr == SAG_OVR_ON) {
        if (!readingStore_active()) {
            readingStore_noteEnter();
            return ENERGY_SAG_ENTERED;
        }
        return ENERGY_NONE;
    }
    if (ovr == SAG_OVR_OFF) {
        if (readingStore_active()) {
            readingStore_noteExit();
            return ENERGY_SAG_EXITED;
        }
        return ENERGY_NONE;
    }

    if (!readingStore_active()) {
        if (battValid && battRaw < (int)g_sagEnterAdc) {
            readingStore_noteEnter();
            return ENERGY_SAG_ENTERED;
        }
    } else {
        if (battValid && battRaw >= (int)g_sagExitAdc) {
            s_exitReads++;
            if (s_exitReads >= 2) {
                readingStore_noteExit();
                s_exitReads = 0;
                return ENERGY_SAG_EXITED;
            }
        } else {
            s_exitReads = 0;
        }
    }
    return ENERGY_NONE;
}

bool energyGuard_allowBurst(int battRaw, bool battValid) {
    if (readingStore_active()) return false;
    if (battValid && battRaw < (int)g_sagExitAdc) return false;
    return true;
}

bool energyGuard_bootSilent(int battRaw, bool battValid) {
    bool belowCrit = (battValid && battRaw < g_battCritical);
    bool silent = false;
    if (readingStore_active() || (battValid && battRaw < (int)g_sagEnterAdc)) {
        readingStore_noteEnter();
        bootBurstFramesLeft = 0;
        silent = true;
    } else if (!energyGuard_allowBurst(battRaw, battValid)) {
        bootBurstFramesLeft = 0;
        Serial.println(F("Boot battery below sag exit - skipping boot burst"));
    }
    if (belowCrit) s_critical = true;
    return silent;
}

bool energyGuard_skipGasWarmup() {
    bool valid = false;
    int b = battery_getADC(&valid);
    return (valid && b < g_battLow);
}

bool energyGuard_criticalFloor() {
    if (!s_critical) return false;
    gasPowerSet(false);
    pmos1Set(false);
    exitAlwaysOnIfActive();
    Serial.println(F("CRITICAL floor: sleeping, battery peek only"));
    Serial.flush();
    smartSleep(applyJitter(LOW_BATTERY_SLEEP_SECONDS * 1000UL, JITTER_PERCENT_DEFAULT, JITTER_MIN_MS));
    bool peekValid;
    int peek = battery_getADC(&peekValid);
    g_batt = peek;
    g_batt_valid = peekValid;
    Serial.print(F("CRITICAL floor: battADC=")); Serial.println(peek);
    if (!peekValid || peek < g_battCritical + ENERGY_CRITICAL_HYST) return true;
    s_critical = false;
    Serial.println(F("CRITICAL floor cleared - resuming normal cycle"));
    return false;
}

unsigned long energyGuard_intervalS(unsigned long normalS) {
    if (readingStore_active() && s_exitReads == 0) {
        return (g_sagIntervalS > normalS) ? (unsigned long)g_sagIntervalS : normalS;
    }
    return normalS;
}

bool energyGuard_active() {
    return readingStore_active();
}

bool energyGuard_storeThisCycle() {
    EnergyEvent ev = energyGuard_update(g_batt, g_batt_valid);
    if (ev == ENERGY_SAG_ENTERED) {
        Serial.print(F("ENERGY SAG: storing readings from now (battADC="));
        Serial.print(g_batt);
        Serial.println(F(")"));
    }
    if (ev == ENERGY_SAG_EXITED) {
        Serial.print(F("ENERGY SAG CLEARED: resuming transmissions (battADC="));
        Serial.print(g_batt);
        Serial.println(F(")"));
        g_pingConfirmedPending = true;
        sensorManager_updateLastTransmitted();
    }
    if (g_batt_valid && g_batt < g_battCritical) s_critical = true;
    if (!readingStore_active()) return false;

    if (bootBurstFramesLeft > 0) {
        bootBurstFramesLeft = 0;
        gasPowerSet(false);
        Serial.println(F("Energy sag active - cancelling remaining burst frames"));
    }

    TelemetryData rec;
    rec.temp_s   = lastSensorValid ? (int16_t)(g_temp * 10) : INT16_MIN;
    rec.hum_s    = lastSensorValid ? (uint8_t)(g_hum  * 2) : 0xFF;
    rec.dust_adc = (uint16_t)g_adc;
    rec.gas_adc  = (uint16_t)g_gas;
    rec.batt_adc = (uint16_t)g_batt;
    readingStore_append(rec, RECORD_STATE_PERSISTENT);
    sensorManager_updateLastTransmitted();
    return true;
}

void energyGuard_printStatus() {
    Serial.print(F("Energy guard: "));
    if (s_critical)                Serial.println(F("CRITICAL floor (sleep and peek only)"));
    else if (readingStore_active()) Serial.println(F("ACTIVE (energy sag storage)"));
    else                           Serial.println(F("healthy (normal TX)"));
    Serial.print(F("Reading store: unsent ")); Serial.print(readingStore_unsentCount());
    Serial.print(F(", next seq ")); Serial.print(readingStore_nextSeq());
    Serial.print(F(", boot id ")); Serial.println(readingStore_bootId());
    Serial.print(F("Sag thresholds: enter<")); Serial.print(g_sagEnterAdc);
    Serial.print(F(" exit>=")); Serial.print(g_sagExitAdc);
    Serial.print(F(" interval ")); Serial.print(g_sagIntervalS); Serial.println(F("s"));
    if (readingStore_override() != SAG_OVR_AUTO) {
        Serial.print(F("Sag override: forced "));
        Serial.println(readingStore_override() == SAG_OVR_ON ? F("ON") : F("OFF"));
    }
}

uint8_t energyGuard_stateBits() {
    return (readingStore_active() ? 0x01 : 0) | (s_critical ? 0x02 : 0) | (s_exitReads > 0 ? 0x10 : 0);
}

uint8_t  energyGuard_enterHalf()    { return (uint8_t)(g_sagEnterAdc / 2); }
uint8_t  energyGuard_exitHalf()     { return (uint8_t)(g_sagExitAdc / 2); }
uint8_t  energyGuard_intervalMin()  { uint16_t m = g_sagIntervalS / 60; return (uint8_t)(m > 255 ? 255 : m); }
uint8_t  energyGuard_unsent()       { uint16_t u = readingStore_unsentCount(); return (uint8_t)(u > 255 ? 255 : u); }
uint8_t  energyGuard_bootId()       { return readingStore_bootId(); }
uint16_t energyGuard_startAgoMin()  { return readingStore_startAgoMin(); }
uint16_t energyGuard_endAgoMin()    { return readingStore_endAgoMin(); }
uint8_t  energyGuard_sagEvents()    { return readingStore_sagEvents(); }
uint16_t energyGuard_lostRecords()  { return readingStore_lostRecords(); }

void energyGuard_applyDownlink(const DownlinkResult& r) {
    switch (r.action) {
        case DL_SAG_THRESHOLDS_CHANGED:
            if (readingStore_setThresholds((uint16_t)r.value, (uint16_t)r.value2)) {
                Serial.print(F("Sag thresholds -> enter<")); Serial.print(g_sagEnterAdc);
                Serial.print(F(" exit>=")); Serial.println(g_sagExitAdc);
                if (g_sagEnterAdc <= (uint16_t)g_battCritical + ENERGY_CRITICAL_HYST) {
                    Serial.println(F("Warning: enter threshold is not above CRITICAL + hysteresis"));
                }
            } else {
                Serial.println(F("Sag thresholds rejected"));
            }
            break;

        case DL_SAG_INTERVAL_CHANGED:
            if (readingStore_setIntervalS((uint16_t)r.value)) {
                Serial.print(F("Sag interval -> ")); Serial.print(g_sagIntervalS); Serial.println(F(" s"));
            } else {
                Serial.println(F("Sag interval rejected"));
            }
            break;

        case DL_SAG_OVERRIDE:
            readingStore_setOverride((uint8_t)r.value);
            Serial.print(F("Sag override -> "));
            Serial.println(r.value == SAG_OVR_ON ? F("forced ON") : r.value == SAG_OVR_OFF ? F("forced OFF") : F("automatic"));
            break;

        case DL_BUFFER_RESEND: {
            uint16_t n = readingStore_unsendFrom((uint16_t)r.value);
            Serial.print(F("Buffer resend: ")); Serial.print(n);
            Serial.println(F(" records marked unsent"));
            break;
        }

        case DL_BUFFER_DISCARD: {
            uint16_t n = readingStore_discardAllUnsent();
            Serial.print(F("Buffer discard: ")); Serial.print(n);
            Serial.println(F(" unsent records cleared"));
            break;
        }

        default: break;
    }
}

void energyGuard_commit() {
    readingStore_commitParams();
    Serial.println(F("Energy sag parameters committed to EEPROM"));
}

void energyGuard_factoryReset() {
    readingStore_factoryReset();
}

#endif // BATTERY_STRATEGY_STORAGE
