#ifndef ENERGY_GUARD_H
#define ENERGY_GUARD_H

#include <Arduino.h>
#include "build_config.h"
#include "reading_store.h"
#include "DownlinkParser.h"

#define ENERGY_CRITICAL_HYST 4 // raw ADC counts (approx 40 mV)

#if BATTERY_STRATEGY == BATTERY_STRATEGY_STORAGE

bool          energyGuard_bootSilent(int battRaw, bool battValid);
bool          energyGuard_allowBurst(int battRaw, bool battValid);
bool          energyGuard_criticalFloor();
bool          energyGuard_skipGasWarmup();
unsigned long energyGuard_intervalS(unsigned long normalS);
bool          energyGuard_storeThisCycle();
bool          energyGuard_active();

void          energyGuard_printStatus();

uint8_t       energyGuard_stateBits();
uint8_t       energyGuard_enterHalf();
uint8_t       energyGuard_exitHalf();
uint8_t       energyGuard_intervalMin();
uint8_t       energyGuard_unsent();
uint8_t       energyGuard_bootId();
uint16_t      energyGuard_startAgoMin();
uint16_t      energyGuard_endAgoMin();
uint8_t       energyGuard_sagEvents();
uint16_t      energyGuard_lostRecords();

void          energyGuard_applyDownlink(const DownlinkResult& r);
void          energyGuard_commit();
void          energyGuard_factoryReset();

#else

static inline bool          energyGuard_bootSilent(int, bool)          { return false; }
static inline bool          energyGuard_allowBurst(int, bool)          { return true; }
static inline bool          energyGuard_criticalFloor()                { return false; }
static inline bool          energyGuard_skipGasWarmup()                { return false; }
static inline unsigned long energyGuard_intervalS(unsigned long n)     { return n; }
static inline bool          energyGuard_storeThisCycle()               { return false; }
static inline bool          energyGuard_active()                       { return false; }
static inline void          energyGuard_printStatus()                  {}
static inline uint8_t       energyGuard_stateBits()                    { return 0; }
static inline uint8_t       energyGuard_enterHalf()                    { return 0; }
static inline uint8_t       energyGuard_exitHalf()                     { return 0; }
static inline uint8_t       energyGuard_intervalMin()                  { return 0; }
static inline uint8_t       energyGuard_unsent()                       { return 0; }
static inline uint8_t       energyGuard_bootId()                       { return 0; }
static inline uint16_t      energyGuard_startAgoMin()                  { return 0xFFFF; }
static inline uint16_t      energyGuard_endAgoMin()                    { return 0xFFFF; }
static inline uint8_t       energyGuard_sagEvents()                    { return 0; }
static inline uint16_t      energyGuard_lostRecords()                  { return 0; }
static inline void          energyGuard_applyDownlink(const DownlinkResult&) {}
static inline void          energyGuard_commit()                       {}
static inline void          energyGuard_factoryReset()                 {}

#endif

#endif // ENERGY_GUARD_H
