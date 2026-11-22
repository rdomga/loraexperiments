/*******************************************************************************
 * single_channel_lmic.h
 *
 * User-space single-channel enforcement layer for the MCCI LoRaWAN LMIC library.
 * Library-update-safe: zero modifications to MCCI source files required.
 *
 * USAGE — include this header in your sketch and call the four functions:
 *
 *   setup()            → SCG_init()               after LMIC_reset() + channel setup
 *   EV_JOINED          → SCG_on_joined()
 *   EV_JOIN_TXCOMPLETE → SCG_on_join_txcomplete()
 *   loop()             → SCG_enforce()             before os_runloop_once()
 *
 * CONFIGURATION — override any define before #include <single_channel_lmic.h>:
 *
 *   #define SCG_CHANNEL       0          // channel index (0 = first channel)
 *   #define SCG_DATARATE      DR_SF7     // spreading factor
 *   #define SCG_TXPOWER       14         // dBm
 *   #define SCG_NUM_CHANNELS  9          // EU868/EU433: 9 ; US915: 72
 *   #define SCG_RX2_DR        DR_SF12    // RX2 window data rate
 *   #define SCG_RX2_FREQ      869525000  // RX2 window frequency (Hz)
 *
 *   #define SCG_ENABLE_DYNAMIC_CHANNEL     // v1.3, opt-in: enables
 *     SCG_setChannel(newIdx) for runtime channel retargeting (e.g. from a
 *     downlink handler). REQUIRES SCG_CHANNEL to already be aliased to a
 *     mutable variable, not left as the default literal — see
 *     SCG_setChannel()'s own comment below for why.
 *
 * KNOWN LIMITATION:
 *   LinkAdrReq and CFList MAC commands are processed inside os_runloop_once().
 *   SCG_enforce() and SCG_on_joined() correct state on the following tick or
 *   event. No actual wrong-channel transmission occurs because TX is always
 *   scheduled after the corrective call has run.
 *
 * Project 3 — MCCI LoRaWAN Library Modifications Catalog
 * University of Yaoundé 1, Department of Computer Sciences
 * Academic Year 2025-2026
 ******************************************************************************/

#ifndef SINGLE_CHANNEL_LMIC_H
#define SINGLE_CHANNEL_LMIC_H

// ─────────────────────────────────────────────────────────────────────────────
// CONFIGURATION DEFAULTS
// ─────────────────────────────────────────────────────────────────────────────

#ifndef SCG_CHANNEL
  #define SCG_CHANNEL      0            // Channel 0:
                                        //   EU868 → 868.1 MHz
                                        //   EU433 → 433.175 MHz
#endif

#ifndef SCG_DATARATE
  #define SCG_DATARATE     DR_SF7       // SF7 = highest data rate, shortest ToA
#endif

#ifndef SCG_TXPOWER
  #define SCG_TXPOWER      14           // 14 dBm — EU868 regulatory maximum
#endif

#ifndef SCG_NUM_CHANNELS
  #define SCG_NUM_CHANNELS 9            // EU868 / EU433: channels 0-8
                                        // US915: set to 72 (channels 0-71)
#endif

#ifndef SCG_RX2_DR
  #define SCG_RX2_DR       DR_SF12     // TTN EU868 RX2 default
#endif

#ifndef SCG_RX2_FREQ
  #define SCG_RX2_FREQ     869525000   // TTN EU868 RX2 default: 869.525 MHz
                                       // TTN EU433 RX2 default: 434.665 MHz → 434665000
#endif

// ─────────────────────────────────────────────────────────────────────────────
// v1.3 CHANNEL TABLE — snapshot of every channel's registered frequency and
// DR-map, captured once by SCG_init() (see below) before anything gets
// disabled. Exists to support two things that LMIC_setupChannel() cannot do
// on its own, confirmed at the source level (lmic_eu868.c):
//
//   1. LMIC_setupChannel() unconditionally refuses EU868's three default
//      channel indices (chidx < NUM_DEFAULT_CHANNELS == 3) — it is a silent
//      no-op for channels 0-2. Their real registration comes from the MCCI
//      library's own internal LMICeu868_initDefaultChannels(), invoked
//      during LMIC_reset(), not from any call the sketch makes.
//   2. LMIC_disableChannel() zeroes channelFreq[]/channelDrMap[] for the
//      channel it disables, not just its channelMap enable bit — and
//      SCG_enforce() already calls this on every non-active channel, every
//      tick. So any channel other than the currently active one has no
//      live registration to fall back on if you ever need it again.
//
// Capturing the state once, right after setup()'s LMIC_setupChannel() calls
// have run (defaults included, via the library's internal init), gives a
// source of truth that survives both problems — see SCG_setChannel() below.
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t SCG_knownFreq[SCG_NUM_CHANNELS];
static uint16_t SCG_knownDrMap[SCG_NUM_CHANNELS];

// ─────────────────────────────────────────────────────────────────────────────
// SCG_init()
//
// Call once in setup(), AFTER LMIC_reset() and AFTER all LMIC_setupChannel()
// calls for your band. LMIC_setupChannel() enables channels in channelMap;
// this function must run after to selectively disable all but SCG_CHANNEL.
//
// Threats addressed:
//   [§2.2] Channel map       — disables channels 1 to SCG_NUM_CHANNELS-1
//   [§2.1] Shuffle map       — locks channelShuffleMap to (1 << SCG_CHANNEL)
//                              so LMIC_findNextChannel() can only return 0
//   [§3.6] ADR mode          — disables Adaptive Data Rate (multi-channel only)
//   [§3.1] Data rate         — asserts SCG_DATARATE and SCG_TXPOWER
//   [§5.1] RX2 window        — sets RX2 frequency and data rate
// ─────────────────────────────────────────────────────────────────────────────
static inline void SCG_init(void) {
    // v1.4 fix — OTAA channelFreq[] snapshot bug (found 2026-09-12, see
    // OTAA_Join_Fix_Bench_Verification_2026-09-12.html in
    // MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_Random_OTAA/): the comment below
    // ("LMIC_reset()'s own internal default-channel init") is only true for
    // channels 0-2 on an ABP sketch. LMICbandplan_resetDefaultChannels() is a
    // documented no-op for EU-like regions (lmic_eu_like.h) — channelFreq[]
    // for the 3 default channels only gets populated later, by
    // LMICbandplan_setSessionInitDefaultChannels(), which LMIC_setSession()
    // (ABP-only) calls. An OTAA sketch reaches this point with channelFreq[0]
    // still zero, the snapshot below captures that zero, and every join-
    // request TX for the life of the sketch goes out at LMIC.freq=0 — silent,
    // unreceivable, and easy to mistake for a hardware/antenna problem.
    // Populate channels 0-2 directly here (not via
    // LMICbandplan_initDefaultChannels(), which clears the WHOLE
    // channelFreq[]/channelDrMap[] array and would destroy whatever this
    // sketch's own LMIC_setupChannel(3..N,...) calls, which run before
    // SCG_init() per this function's own calling contract, already set up).
    // No-op for ABP: LMIC_setSession() already set channelFreq[0] to a
    // nonzero value by the time SCG_init() runs, so this guard skips it.
#if defined(CFG_eu868)
    if (LMIC.channelFreq[0] == 0) {
        LMIC.channelFreq[0]  = 868100000 | BAND_CENTI;
        LMIC.channelFreq[1]  = 868300000 | BAND_CENTI;
        LMIC.channelFreq[2]  = 868500000 | BAND_CENTI;
        LMIC.channelDrMap[0] = LMIC.channelDrMap[1] = LMIC.channelDrMap[2] = DR_RANGE_MAP(DR_SF12, DR_SF7);
        LMIC_setupBand(BAND_MILLI, 14, 1000);
        LMIC_setupBand(BAND_CENTI, 14,  100);
        LMIC_setupBand(BAND_DECI,  27,   10);
    }
#endif

    // v1.3: snapshot every channel's frequency/DR-map BEFORE anything below
    // disables a single one. This is the only safe window — setup()'s
    // LMIC_setupChannel() calls (and, for channels 0-2, LMIC_reset()'s own
    // internal default-channel init on ABP, or the fix above on OTAA) have
    // already run, and nothing has been torn down yet. See the
    // SCG_knownFreq/SCG_knownDrMap comment above.
    for (int i = 0; i < SCG_NUM_CHANNELS; i++) {
        SCG_knownFreq[i]  = LMIC.channelFreq[i];
        SCG_knownDrMap[i] = LMIC.channelDrMap[i];
    }

    // Disable all channels except the target channel.
    // Must run after LMIC_setupChannel() calls, which enable channels in channelMap.
    for (int i = 0; i < SCG_NUM_CHANNELS; i++) {
        if (i != SCG_CHANNEL)
            LMIC_disableChannel(i);
    }

    // Lock the shuffle map so LMIC_findNextChannel() cannot select other channels.
    // This guards against LMICeulike_initJoinLoop() resetting the map to 0x0007
    // (the 3 default EU868 channels) at the start of OTAA.
    LMIC.channelShuffleMap = (1 << SCG_CHANNEL);

    // Disable ADR — the ADR algorithm assumes multiple channels and will
    // attempt to adjust DR and channel mask based on network feedback.
    LMIC_setAdrMode(0);

    // Assert desired data rate and TX power.
    // Also called in SCG_on_joined() since join can reset these.
    LMIC_setDrTxpow(SCG_DATARATE, SCG_TXPOWER);

    // Set RX2 window parameters explicitly.
    // These are overwritten by Join Accept and RXParamSetupReq MAC commands;
    // SCG_on_joined() re-asserts them after join if needed.
    LMIC.dn2Dr   = SCG_RX2_DR;
    LMIC.dn2Freq = SCG_RX2_FREQ;

    // Disable link check validation.
    // For ABP this must be done here (no EV_JOINED fires after setSession).
    // For OTAA this is called again in SCG_on_joined() after join resets it.
    LMIC_setLinkCheckMode(0);

    Serial.print(F("[SCG] init — ch="));
    Serial.print(SCG_CHANNEL);
    Serial.print(F(" SF"));
    Serial.print(12 - SCG_DATARATE);
    Serial.print(F(" "));
    Serial.print(SCG_TXPOWER);
    Serial.println(F("dBm"));
}

// ─────────────────────────────────────────────────────────────────────────────
// SCG_on_joined()
//
// Call inside the EV_JOINED case of onEvent(), before break.
//
// Threats addressed:
//   [§2.8] CFList injection  — Join Accept can carry up to 5 new channels via
//                              CFList; LMICeulike_processJoinAcceptCFList()
//                              adds them to channelMap before EV_JOINED fires.
//                              This re-disables them.
//   [§3.4] DR reset          — join process can reset datarate; re-asserts it.
//   [§3.1] Link check        — auto-enabled during join; disabled here.
//                              LINK_CHECK_DEAD triggers re-join behaviour which
//                              would rotate join channels again.
//   [§5.1] RX2 window reset  — Join Accept can carry DLSettings (RX2 DR) and
//                              the join process re-inits LMIC's dn2Dr/dn2Freq
//                              to library defaults. Re-asserted here so a
//                              rejoin can't silently desync the device's RX2
//                              window from SCG_RX2_DR/SCG_RX2_FREQ.
// ─────────────────────────────────────────────────────────────────────────────
static inline void SCG_on_joined(void) {
    // Re-disable channels that the CFList in the Join Accept may have added.
    // TTN v3 sends a CFList for EU868, adding channels 3-7.
    for (int i = 0; i < SCG_NUM_CHANNELS; i++) {
        if (i != SCG_CHANNEL)
            LMIC_disableChannel(i);
    }

    // Re-lock shuffle map after join resets it.
    LMIC.channelShuffleMap = (1 << SCG_CHANNEL);

    // Re-assert DR and TX power — join may have altered these.
    LMIC_setDrTxpow(SCG_DATARATE, SCG_TXPOWER);

    // Re-assert RX2 window parameters — Join Accept DLSettings or the join
    // process itself can reset dn2Dr/dn2Freq to the library default
    // (DR_SF12 / 869525000), overwriting whatever SCG_init() set at boot.
    LMIC.dn2Dr   = SCG_RX2_DR;
    LMIC.dn2Freq = SCG_RX2_FREQ;

    // Disable link check validation.
    // Auto-enabled during join but not supported by TTN at this time.
    // If left enabled, LINK_CHECK_DEAD fires after ~32 unacknowledged uplinks,
    // triggering a re-join that rotates channels again.
    LMIC_setLinkCheckMode(0);

    Serial.println(F("[SCG] post-join enforcement applied"));
}

// ─────────────────────────────────────────────────────────────────────────────
// SCG_on_join_txcomplete()
//
// Call inside the EV_JOIN_TXCOMPLETE case of onEvent(), before break.
// EV_JOIN_TXCOMPLETE fires when a join request TX completes with no Join Accept
// received (i.e. each failed join attempt).
//
// Threats addressed:
//   [§6.2] Join channel rotation — LMICeulike_nextJoinState() calls
//                                  LMIC_findNextChannel() to advance to the
//                                  next join channel BEFORE this event fires.
//                                  txChnl may therefore be 1 or 2 at this point.
//                                  This corrects it for the next attempt.
// ─────────────────────────────────────────────────────────────────────────────
static inline void SCG_on_join_txcomplete(void) {
    // Force channel back to SCG_CHANNEL for the next join attempt.
    // nextJoinState() has already selected the next channel internally;
    // we override it here before the scheduler queues the retry.
    LMIC.txChnl            = SCG_CHANNEL;
    LMIC.channelShuffleMap = (1 << SCG_CHANNEL);

    Serial.print(F("[SCG] join retry forced to ch="));
    Serial.println(SCG_CHANNEL);
}

// ─────────────────────────────────────────────────────────────────────────────
// SCG_enforce()
//
// Call in loop(), BEFORE os_runloop_once().
//
// Threats addressed:
//   [§2.5] LinkAdrReq aftermath   — LMICeulike_mapChannels() may re-enable
//                                   channels in LMIC.channelMap.
//   [§1.4] NewChannelReq aftermath — same class of threat: the MAC command
//                                   handler calls LMIC_setupChannel() and
//                                   re-enables whatever channel index the
//                                   network names. Confirmed in the field to
//                                   produce a live TX on the re-enabled
//                                   channel. Unlike LinkAdrReq/DR drift, this
//                                   leaves no symptom in txChnl/datarate until
//                                   channelShuffleMap is later exhausted and
//                                   reloaded from the corrupted channelMap —
//                                   so it is swept every tick unconditionally
//                                   below, not just reacted to after the fact.
//   [§3.2] ADR DR change          — applyAdrRequests() can reset datarate even
//                                   with LMIC_setAdrMode(0). Caught here.
//   [§3.4] Join DR backoff        — nextJoinState() drops DR after each
//                                   shuffle cycle. Caught here on the
//                                   following tick.
//
// Note: this function deliberately uses direct struct access (LMIC.txChnl,
// LMIC.datarate) rather than the public API. The public LMIC_setDrTxpow()
// sets OP_NEXTCHNL which would trigger another channel selection cycle;
// direct assignment avoids that overhead for the continuous enforcement case.
// LMIC_disableChannel() is cheap (a couple of field writes, see
// lmic_eu_like.c), so sweeping it unconditionally every tick is not a
// measurable cost — unlike txChnl/datarate, channelMap corruption has no
// cheap "did it drift" check, so there's nothing to gate this behind.
// ─────────────────────────────────────────────────────────────────────────────
static inline void SCG_enforce(void) {
    for (int i = 0; i < SCG_NUM_CHANNELS; i++) {
        if (i != SCG_CHANNEL) LMIC_disableChannel(i);
    }

    // v1.3 — Threat 1.5 fix (Technical Manual §5.1): reassert the ACTIVE
    // channel's own frequency/DR-map every tick, not just whether it's
    // enabled. Without this, a NewChannelReq naming SCG_CHANNEL itself
    // (rather than some other index) could silently rewrite its frequency
    // and the map-only sweep above would never notice, since channelMap
    // never changes in that case. Cheap (two array writes), same
    // unconditional-sweep reasoning already used for the loop above.
    LMIC.channelFreq[SCG_CHANNEL]  = SCG_knownFreq[SCG_CHANNEL];
    LMIC.channelDrMap[SCG_CHANNEL] = SCG_knownDrMap[SCG_CHANNEL];

    if (LMIC.txChnl != SCG_CHANNEL) {
        LMIC.txChnl            = SCG_CHANNEL;
        LMIC.channelShuffleMap = (1 << SCG_CHANNEL);
    }

    if (LMIC.datarate != SCG_DATARATE) {
        LMIC.datarate = SCG_DATARATE;
    }
}

#if defined(SCG_ENABLE_DYNAMIC_CHANNEL)
// ─────────────────────────────────────────────────────────────────────────────
// SCG_setChannel()
//
// Retargets the single-channel lock to a new index, entirely via direct
// struct writes — never LMIC_setupChannel(), which unconditionally refuses
// EU868 default channel indices 0-2 (Technical Manual v1.3, Finding 1).
// Works identically for default and custom channels since it never calls
// the API that refuses defaults; the frequency/DR-map it writes come from
// SCG_knownFreq/SCG_knownDrMap, captured once by SCG_init().
//
// REQUIRES SCG_CHANNEL to be aliased to a mutable variable before
// #include <single_channel_lmic.h> — this function assigns to SCG_CHANNEL
// directly (same pattern SCG_DATARATE already needs for /@F# downlinks).
// Defining SCG_ENABLE_DYNAMIC_CHANNEL without also aliasing SCG_CHANNEL to
// a variable is a compile-time error (assignment to a literal constant).
//
// SCG_enforce() continuously protects whatever channel is active, so no
// further calls are needed after this one — the very next tick's sweep
// disables the previous channel and reasserts the new one's own frequency
// (Threat 1.5 fix above), same as it always would have.
// ─────────────────────────────────────────────────────────────────────────────
static inline void SCG_setChannel(uint8_t newIdx) {
    if (newIdx >= SCG_NUM_CHANNELS) return;

    LMIC.channelFreq[newIdx]  = SCG_knownFreq[newIdx];
    LMIC.channelDrMap[newIdx] = SCG_knownDrMap[newIdx];
    LMIC.channelMap           = (1 << newIdx);
    LMIC.channelShuffleMap    = (1 << newIdx);
    LMIC.txChnl               = newIdx;
    SCG_CHANNEL               = newIdx;

    Serial.print(F("[SCG] channel retargeted -> "));
    Serial.println(newIdx);
}
#endif // SCG_ENABLE_DYNAMIC_CHANNEL

// ─────────────────────────────────────────────────────────────────────────────
// SCG_setRadioConfig()
//
// Dynamically reconfigures the LoRa radio data rate and TX power.
// Useful when sketches process remote downlink MAC commands (e.g. DL_SF_CHANGED).
// Note: Ensure SCG_DATARATE is mapped to a variable if dynamic DR changes must
// persist across SCG_enforce() calls in loop().
// ─────────────────────────────────────────────────────────────────────────────
static inline void SCG_setRadioConfig(int channel, int dr, int txpow) {
    (void)channel;
    LMIC_setDrTxpow(dr, txpow);
}

#endif // SINGLE_CHANNEL_LMIC_H
