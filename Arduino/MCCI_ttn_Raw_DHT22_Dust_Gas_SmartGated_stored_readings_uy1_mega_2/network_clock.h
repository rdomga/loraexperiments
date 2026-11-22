#ifndef NETWORK_CLOCK_H
#define NETWORK_CLOCK_H
// Network-time sleep factor (millis diagnosis, 2026-11-01).
// At every CONFIRMED ping the node adds a DeviceTimeReq. The ACK downlink that confirmed ping already causes carries the
// answer, so this costs no extra downlink. Confirmed pings happen at boot (init check), at the end of the 9-frame boot burst
// (the dead-man ping, about 14 min after boot: too close to the boot anchor to use), then every 9 port-1 uplinks (about 4.5 h
// at 1800 s): the first network factor comes with that ping, and the boot calibration covers the time before it. Two anchors at least
// NETCLK_MIN_BASELINE_S apart give the real-vs-nominal factor of the watchdog sleep:
// real sleep = elapsed network time - awake time (millis) - wake cost, divided by the nominal sleep.
// The first anchor after boot is the reference, so the baseline (and the precision) grows until the next reboot.
// Needs MAC 1.0.3 on TTN. With no answer the node keeps the boot calibration or the node default.
#include <stdint.h>

#ifndef NETCLK_MIN_BASELINE_S
#define NETCLK_MIN_BASELINE_S 1800UL   // answers carry 0.3-0.5 s of noise on the 2 h bench runs: error vs the sweep had sd 1 per mille at 10-15 min, 0.3 at 30 min, 0.15 at 60 min
#endif
#ifndef NETCLK_MAX_JUMP_PERMILLE
#define NETCLK_MAX_JUMP_PERMILLE 30    // ignore an estimate more than 3 % away from the factor in use
#endif

struct NetAnchor {
    uint32_t netS;         // network (GPS) seconds from DeviceTimeAns
    uint8_t  netFrac;      // fraction, 1/256 s
    uint32_t millisAwake;  // millis(): awake time only
    uint32_t nominalMs;    // sleepNominalMs()
    uint32_t wakes;        // sleepWakeCount()
};

// Pure function so it can be compiled and tested on a host. Returns permille (real ms per 1000 nominal ms), 0 if not computable.
static inline uint16_t netclk_estimate(const NetAnchor& a, const NetAnchor& b, uint8_t wakeCostMs) {
    int32_t dNetMs = (int32_t)(b.netS - a.netS) * 1000L + ((int32_t)b.netFrac - (int32_t)a.netFrac) * 1000L / 256L;
    uint32_t dMil = b.millisAwake - a.millisAwake;
    uint32_t dNom = b.nominalMs - a.nominalMs;
    uint32_t dWk  = b.wakes - a.wakes;
    if (dNom < 60000UL || dNetMs <= 0) return 0;
    int32_t realSleepMs = dNetMs - (int32_t)dMil - (int32_t)(dWk * wakeCostMs);
    if (realSleepMs <= 0) return 0;
    uint32_t k = (uint32_t)(((uint64_t)realSleepMs * 1000ULL + dNom / 2) / dNom);
    return k > 0xFFFFUL ? 0 : (uint16_t)k;
}

#ifdef ARDUINO
void     netclk_request();                              // call just before a confirmed uplink is queued
void     netclk_onNetworkTime(void* user, int ok);      // LMIC callback; also called from the bench callback
uint16_t netclk_factorPermille();                       // factor from the network, 0 until one has been computed
uint8_t  netclk_anchorCount();
#endif
#endif
