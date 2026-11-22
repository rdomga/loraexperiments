#ifndef BATTERY_GUARD_H
#define BATTERY_GUARD_H

#include <Arduino.h>
#include "build_config.h"
#include "battery_sensor.h"

// Active runtime thresholds
extern int g_battLow;
extern int g_battRecover;
extern int g_battCritical;
extern int g_battPsave;
extern int g_battPsaveRecover;

extern bool lowBatteryMode;
extern bool powerSaveMode;
uint8_t batteryGuard_stateBits();   // declared state for the status frame: night bits (NIGHT) | bit2 ladder LOW | bit3 ladder POWERSAVE

// Checks battery against hysteresis thresholds during normal runs.
// Returns true if a transition into lowBatteryMode occurred (requires immediate PMOS rail shutdown).
bool batteryGuard_updateHysteresis(int battRaw, bool battValid);

// Chunked peek loop during 6-hour power save mode
void batteryGuard_powerSavePeekLoop();

#endif // BATTERY_GUARD_H
