#include "battery_guard.h"
#include "power_manager.h"
#include "energy_guard.h"

int g_battLow          = BATTERY_LOW_RAW_DEFAULT;
int g_battRecover      = BATTERY_RECOVER_RAW_DEFAULT;
int g_battCritical     = BATTERY_CRITICAL_RAW_DEFAULT;
int g_battPsave        = BATTERY_POWERSAVE_RAW_DEFAULT;
int g_battPsaveRecover = BATTERY_POWERSAVE_RECOVER_RAW_DEFAULT;

bool lowBatteryMode = false;
bool powerSaveMode = false;

bool batteryGuard_updateHysteresis(int battRaw, bool battValid) {
#ifdef POWER_SAVE_ENABLED
    bool transitionedToLow = false;
    if (battValid && battRaw < g_battLow) {
        if (!lowBatteryMode) {
            Serial.println(F("Battery low - switching to heartbeat-only mode"));
            transitionedToLow = true;
        }
        lowBatteryMode = true;
    } else if (battValid && battRaw >= g_battRecover) {
        if (lowBatteryMode) Serial.println(F("Battery recovered - resuming normal sends"));
        lowBatteryMode = false;
    }

    if (!lowBatteryMode) {
        if (battValid && battRaw < g_battPsave) {
            if (!powerSaveMode) Serial.println(F("Battery <~25% - switching to 6h power-save interval"));
            powerSaveMode = true;
        } else if (battValid && battRaw >= g_battPsaveRecover) {
            if (powerSaveMode) Serial.println(F("Battery >=~50% - restoring normal TX interval"));
            powerSaveMode = false;
        }
    }
    return transitionedToLow;
#else
    (void)battRaw;
    (void)battValid;
    return false;
#endif
}

void batteryGuard_powerSavePeekLoop() {
    unsigned long slept_s = 0;
    uint8_t checks = 0;
    while (slept_s < POWERSAVE_TX_INTERVAL_S) {
        unsigned long chunk_s = POWERSAVE_TX_INTERVAL_S - slept_s;
        if (chunk_s > POWERSAVE_CHECK_INTERVAL_S) chunk_s = POWERSAVE_CHECK_INTERVAL_S;

#ifdef LOW_POWER
        lowPower(chunk_s * 1000UL);
#else
        wdtSafeDelay(chunk_s * 1000UL);
#endif
        slept_s += chunk_s;
        checks++;

        bool peekValid;
        int peekRaw = battery_getADC(&peekValid);
        Serial.print(F("Power-save check #")); Serial.print(checks);
        Serial.print(F(" battADC=")); Serial.println(peekRaw);

        if (peekValid && (peekRaw < g_battLow || peekRaw >= g_battPsaveRecover)) {
            Serial.println(F("Power-save peek crossed a threshold - waking fully now instead of waiting out the rest of the 6h"));
            break;
        }
    }
}

uint8_t batteryGuard_stateBits() {
    return energyGuard_stateBits() | (lowBatteryMode ? 0x04 : 0) | (powerSaveMode ? 0x08 : 0);
}
