#include "mode_controller.h"
#include "power_manager.h"
#include "battery_guard.h"
#include "gas_sensor.h"

uint8_t g_mode = MODE_NORMAL;
static unsigned g_savedTxIntervalBeforeAlwaysOn = DEFAULT_TX_INTERVAL;

// Applies a /@M<0-2># mode switch. A no-op if already in the requested mode.
// Entering/leaving ALWAYS_ON is the only transition with side effects: it
// latches both PMOS rails on/off outside the per-cycle gating and swaps
// TX_INTERVAL to/from its own short default.
void setOperatingMode(uint8_t newMode) {
    if (newMode > MODE_ALWAYS_ON) {
        Serial.println(F("Ignoring out-of-range mode request"));
        return;
    }
    if (newMode == g_mode) {
        Serial.print(F("Already in mode "));
        Serial.println(modeName(g_mode));
        return;
    }
    if (newMode == MODE_ALWAYS_ON && (lowBatteryMode || powerSaveMode)) {
        Serial.println(F("Ignoring ALWAYS_ON request - battery conservation active"));
        return;
    }

    if (g_mode == MODE_NORMAL && newMode != MODE_NORMAL) {
        gasPowerSet(false);
    }

    if (g_mode == MODE_ALWAYS_ON) {
        pmos1Set(false);
        gasPowerSet(false);
        TX_INTERVAL = g_savedTxIntervalBeforeAlwaysOn;
        Serial.print(F("Leaving ALWAYS_ON - TX_INTERVAL restored to "));
        Serial.print(TX_INTERVAL);
        Serial.println(F("s"));
    }

    if (newMode == MODE_ALWAYS_ON) {
        g_savedTxIntervalBeforeAlwaysOn = TX_INTERVAL;
        TX_INTERVAL = ALWAYS_ON_TX_INTERVAL_S;
        pmos1Set(true);
        gasPowerSet(true);
        gas_NotifyPoweredOn();
        Serial.println(F("Entering ALWAYS_ON - PMOS 1+2 latched on, settling"));
        smartSleep(PMOS1_SETTLE_MS);
    }

    g_mode = newMode;
    Serial.print(F("Operating mode -> "));
    Serial.println(modeName(g_mode));
}

// Idempotent — no-op if g_mode isn't MODE_ALWAYS_ON. Called from both the
// powerSaveMode and lowBatteryMode entry paths in sleepThenSend()'s battery
// guard (§4 of the handoff doc).
void exitAlwaysOnIfActive() {
    if (g_mode != MODE_ALWAYS_ON) return;
    pmos1Set(false);
    gasPowerSet(false);
    TX_INTERVAL = g_savedTxIntervalBeforeAlwaysOn;
    g_mode = MODE_NORMAL;
    Serial.println(F("ALWAYS_ON reverted to NORMAL - battery conservation active, TX_INTERVAL restored"));
}
