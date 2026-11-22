#include <Arduino.h>
#include <lmic.h>
#include "network_clock.h"
#include "power_manager.h"

static NetAnchor s_first, s_last;
static uint8_t   s_anchors = 0;
static uint16_t  s_netFactor = 0;

void netclk_onNetworkTime(void* user, int ok) {
    (void)user;
    if (!ok) { Serial.println(F("NETCLK no answer")); return; }
    NetAnchor a;
    a.netS = LMIC.netDeviceTime;
    a.netFrac = LMIC.netDeviceTimeFrac;
    a.millisAwake = millis();
    a.nominalMs = sleepNominalMs();
    a.wakes = sleepWakeCount();
    // Counters are RAM only: after a reboot they restart from zero, so a smaller value means "start over".
    if (s_anchors && (a.millisAwake < s_last.millisAwake || a.nominalMs < s_last.nominalMs || a.netS < s_last.netS)) s_anchors = 0;
    if (s_anchors == 0) {
        s_first = a; s_last = a; s_anchors = 1;
        Serial.println(F("NETCLK reference anchor taken"));
        return;
    }
    s_last = a;
    if (s_anchors < 255) s_anchors++;
    uint32_t baselineS = a.netS - s_first.netS;
    if (baselineS < NETCLK_MIN_BASELINE_S) {
        Serial.print(F("NETCLK anchor ")); Serial.print(s_anchors);
        Serial.print(F(", baseline ")); Serial.print(baselineS); Serial.println(F(" s, too short"));
        return;
    }
    uint16_t k = netclk_estimate(s_first, a, g_wakeCostMs);
    int16_t jump = (int16_t)k - (int16_t)g_wdtScalePermille;
    if (k < 1050 || k > 1300 || jump > NETCLK_MAX_JUMP_PERMILLE || jump < -NETCLK_MAX_JUMP_PERMILLE) {
        Serial.print(F("NETCLK estimate ")); Serial.print(k); Serial.println(F(" rejected"));
        return;
    }
    Serial.print(F("NETCLK factor ")); Serial.print(k);
    Serial.print(F(" permille (was ")); Serial.print(g_wdtScalePermille);
    Serial.print(F("), anchors ")); Serial.print(s_anchors);
    Serial.print(F(", baseline ")); Serial.print(baselineS); Serial.println(F(" s"));
    g_wdtScalePermille = k;
    s_netFactor = k;
}

void netclk_request() {
    LMIC_requestNetworkTime(netclk_onNetworkTime, NULL);
}

uint16_t netclk_factorPermille() { return s_netFactor; }
uint8_t  netclk_anchorCount()    { return s_anchors; }
