

**MCCI LoRaWAN LMIC Library**  
**Modifications Catalog for Single-Channel Operation**

Technical Manual — Version 1.3

| Project | Project 3 \- Master's 1 Research Project |
| :---- | :---- |
| **Institution** | University of Yaoundé 1 |
| **Department** | Department of Computer Sciences |
| **Author** | KOUEGA YOUMBI (M2 Researcher) |
| **Academic Year** | 2025-2026 |
| **Version** | 1.3 |
| **Date** | September 2026 (originally April 2026) |

## **Revision Note — v1.3 (September 2026)**

Triggered by design work for the SmartGated field-diagnostics project's §3.6 (locked-channel retargeting via downlink), which needed to know exactly what `LMIC_setupChannel()` and `LMIC_disableChannel()` do at the source level before proposing a runtime channel-switch mechanism. Two findings came directly from reading `lmic_eu868.c`/`lmic_eu_like.c`, not from field testing — no live confirmation yet, same evidentiary status Threat 1.4 had before its 2026-07-23 capture.

**Finding 1 — `LMIC_setupChannel()` refuses default channels.** `lmic_eu868.c` line ~170: `if (chidx < NUM_DEFAULT_CHANNELS) return 0;` (`NUM_DEFAULT_CHANNELS = 3` for EU868 — channels 0, 1, 2). This function cannot register or re-register a frequency for indices 0-2 under any circumstances. Any design that calls `LMIC_setupChannel()` to restore a channel after it has been disabled will silently no-op on the three default channels.

**Finding 2 — `LMIC_disableChannel()` zeroes the channel's registration, not just its enable bit.** `lmic_eu_like.c`: `LMIC.channelFreq[channel] = 0; LMIC.channelDrMap[channel] = 0;` alongside clearing the `channelMap` bit. `SCG_enforce()`'s v1.2 channelMap sweep already calls this unconditionally, every tick, on every channel except `SCG_CHANNEL`. Combined with Finding 1, this means: once any channel other than the active one has been swept even a single time, its frequency is gone, and if that channel happens to be 0, 1, or 2, the public API can never put it back. Only direct struct assignment (`LMIC.channelFreq[i] = ...`) can restore a default channel's registration post-boot.

**Finding 3 — Threat 1.5 (new): the active channel's own frequency is unguarded.** `SCG_enforce()` reasserts `channelMap`, `txChnl`, and `datarate` every tick, but never reasserts `LMIC.channelFreq[SCG_CHANNEL]` or `channelDrMap[SCG_CHANNEL]` itself. A `NewChannelReq` that names the *currently active* index (rather than some other one) and supplies a different frequency would rewrite it in place — same mechanism as Threat 1.4, just targeting the one channel the existing sweep assumes is safe because it's the locked one. Added to the catalog below as Threat 1.5.

**Design implication, carried into the SmartGated project's §3.6:** the fix for all three findings is the same move that already worked for Threat 1.4 — stop routing through the public API (`LMIC_setupChannel()`/`LMIC_disableChannel()`) for anything the enforcement layer needs to guarantee, and instead hold a sketch-side reference table of all channels' frequency/DR-map/band, writing `LMIC.channelFreq[]`/`channelDrMap[]`/`channelMap`/`channelShuffleMap`/`txChnl` directly from it. This sidesteps Finding 1 entirely (direct struct access doesn't consult `NUM_DEFAULT_CHANNELS`), makes Finding 2 harmless (the reference table, not the live LMIC struct, is now the source of truth, so a swept-and-zeroed slot is always restorable), and closes Finding 3 for free if the same per-tick sweep also reasserts `channelFreq[SCG_CHANNEL]`/`channelDrMap[SCG_CHANNEL]` from that table alongside the existing `channelMap` sweep. Not yet implemented in the shipped header — tracked as a design proposal in §8.8 below and in the SmartGated project's own field-diagnostics doc.

This also raises the priority of the §10.3 follow-up (txpow/dn2Dr/dn2Freq never re-checked): a concrete downlink feature (SmartGated §3.6 channel retargeting, plus a proposed RX2 freq/SF downlink) now depends on the same "sweep the real state every tick" pattern being extended to the RX2 fields, not just the channel-selection ones. See the updated §10.3 note below.

## **Revision Note — v1.2 (July 2026)**

The v1.1 countermeasure for Threat 1.4 (`DISABLE_MCMD_NewChannelReq`) has been reverted after field testing showed it breaks MAC command parsing. Root cause, confirmed at the source level: `macCmdSize[]` in `lmic.c` still lists `NewChannelReq` as a 6-byte command regardless of this define — it is not conditionally compiled — so the FOpts parser correctly walks up to the opcode, finds no matching `case` in the `switch` (that part *is* compiled out), and falls into `default:`, which does `olen = oidx; break;` — aborting the rest of the MAC command scan. No `NewChannelAns` is ever queued, so TTN never receives an acknowledgment and re-sends the same `NewChannelReq` on every subsequent downlink indefinitely. This was observed directly: `error processing mac commands for port 0 (len=0x1e, optendindex=0x6)` on every RX1 window, with downlinks arriving continuously.

The fix has moved into `SCG_enforce()` itself: it now unconditionally re-disables every non-`SCG_CHANNEL` channel in `LMIC.channelMap` on every tick (`LMIC_disableChannel()` is a couple of field writes — negligible cost), rather than only reacting after `txChnl`/`datarate` have already drifted. This closes the actual blind spot identified in §10.3 directly, lets the device answer `NewChannelReq` normally, and was field-confirmed (second capture, 2026-07-23) to stop both the wrong-channel TX and the downlink retry storm in the same test run — see Threat 1.4 for both log captures and §8.6 for the updated `SCG_enforce()` source.

## **Revision Note — v1.1 (July 2026)**

Section 8 of v1.0 documented an `SCG_apply()`-based design (a shared helper checking five LMIC state fields) that was never actually implemented in the shipped `single_channel_lmic.h`. The deployed header is simpler: `SCG_enforce()` checks only `txChnl` and `datarate`, has no `SCG_apply()` helper, and `SCG_on_joined()` does not re-assert `dn2Dr`/`dn2Freq`. Section 8 below has been rewritten to match the code that actually ships, not the originally-intended design.

Separately, field testing on an ABP deployment (`MCCI_ttn_Raw_DHT22_Dust_MQ9_AlwaysOn`, TTN v3 EU868, 2026-07-23) produced a **live, confirmed wrong-channel transmission** — not the theoretical "one-tick window" described in v1.0, but an actual frame sent on 867.3 MHz from a debug log capture. This traces to a threat category v1.0 did not catalog at all: `NewChannelReq` re-enabling a disabled channel in `LMIC.channelMap`. This is now Threat 1.4. It also means the risk argument in §6.4 point 2 and §10.1 ("the window is real; the risk is not") was wrong for at least this trigger path — see the correction inline in both sections. **(Superseded by v1.2 above — the countermeasure described here was reverted.)**

# **1\. Abstract**

This document is the technical manual produced by Project 3 of the Masters 1 research program at the University of Yaoundé 1, Department of Computer Sciences. The project systematically catalogs every behavior of the MCCI LoRaWAN LMIC library that conflicts with single-channel gateway operation, quantifies the risk of each behavior, and proposes two complementary solutions: a formal library-side patch architecture centered on a LMIC\_SINGLE\_CHANNEL\_GATEWAY preprocessor flag, and a library-update-safe user-space enforcement layer (the SingleChannelLMIC header library).

The MCCI LMIC library is designed for multi-channel LoRaWAN networks and contains twelve identified code paths that either actively rotate transmission channels, renegotiate data rates, or introduce new channels in response to network MAC commands. Each path is documented with its source location, trigger condition, observable consequence on single-channel gateways, and recommended countermeasure.

The user-space approach, implemented as the SingleChannelLMIC Arduino library, is the recommended solution for most deployments due to its robustness against library updates. The formal library patch proposal is documented as a contribution to the upstream MCCI project and as a reference for researchers who require a zero-overhead enforcement path.

# **2\. Table of Contents**

**1\.**  Abstract

**2\.**  Table of Contents

**3\.**  Introduction

**3.1**  Motivation and Context

**3.2**  Project Objectives

**3.3**  Hardware Stack

**3.4**  Document Scope

**4\.**  MCCI LMIC Architecture Overview

**4.1**  Relevant Internal Data Structures

**4.2**  Key Functions Involved in Channel and DR Management

**5\.**  Threat Catalog

**5.1**  Category 1 — Channel Selection Threats

**5.2**  Category 2 — Data Rate Threats

**5.3**  Category 3 — Join Procedure Threats

**5.4**  Category 4 — MAC Command Threats

**5.5**  Category 5 — RX Window Threats

**5.6**  Threat Summary Table

**6\.**  Approach Comparison

**6.1**  Approach A — Library-Side Patches

**6.2**  Approach B — User-Space Enforcement (SingleChannelLMIC)

**6.3**  Comparison Table

**6.4**  Recommendation and Argument

**7\.**  Formal LMIC\_SINGLE\_CHANNEL\_GATEWAY Proposal

**7.1**  Motivation

**7.2**  Proposed Preprocessor Interface

**7.3**  Required Library Changes

**7.4**  Risk Assessment

**8\.**  SingleChannelLMIC Library Design

**8.1**  Architecture Principles

**8.2**  Configuration System

**8.3**  SCG\_init()

**8.4**  SCG\_on\_joined()

**8.5**  SCG\_on\_join\_txcomplete()

**8.6**  SCG\_enforce()

**9\.**  Sketch Migration Guide

**9.1**  ABP Migration

**9.2**  OTAA Migration

**9.3**  Variable-DR (Characterization) Special Case

**10\.**  Known Limitations

**11\.**  Conclusions and Recommendations

**12\.**  Appendix A — Complete Threat Reference Table

**13\.**  Appendix B — Sketch Inventory

# **3\. Introduction**

## **3.1 Motivation and Context**

LoRaWAN networks are typically served by multi-channel gateways that receive and transmit on eight or more channels simultaneously. The end-device stack — in this project, the MCCI LoRaWAN LMIC library — is designed with this assumption baked in. It implements frequency hopping, adaptive data rate (ADR), join channel rotation, and dynamic channel addition through MAC commands, all of which assume that the gateway is listening on multiple channels at the same moment.

In developing regions and low-budget research deployments, full commercial gateways (USD 150–400) are often replaced by single-channel gateways based on a Raspberry Pi and an SX1276 LoRa module. These gateways cost USD 15–30, represent 10–20% of the cost of commercial alternatives, and are sufficient for small-scale, fixed-node sensor networks. However, they can only listen on one frequency at a time. Any frame transmitted by the end device on a channel other than the one the gateway is monitoring is silently lost.

The MCCI LMIC library contains twelve identified behaviors that cause the end device to transmit on channels other than the intended single channel, or to use data rates incompatible with the gateway configuration. This project catalogs all such behaviors, maps their source locations in the MCCI library, and proposes both a library-level fix and a library-update-safe user-space fix.

## **3.2 Project Objectives**

1. Systematically audit the MCCI LMIC library source to identify all code paths that modify the transmission channel, data rate, or RX window parameters.  
2. Classify each identified behavior by threat level (Critical / High / Medium / Low) based on whether it causes immediate frame loss or degraded reliability.  
3. Propose a formal library-side patch centered on a \#define LMIC\_SINGLE\_CHANNEL\_GATEWAY preprocessor flag, documenting every required change at source level.  
4. Design and implement a user-space alternative (SingleChannelLMIC Arduino library) that provides equivalent protection without modifying the MCCI library source.  
5. Apply the user-space enforcement to a set of sensor sketches used in the research deployment.  
6. Document the trade-offs between the two approaches and provide a reasoned recommendation.

## **3.3 Hardware Stack**

| Component | Model | Role |
| :---- | :---- | :---- |
| Gateway SBC | Raspberry Pi 3B+ | Single-channel LoRaWAN gateway host |
| Gateway Radio | SX1276 breakout board | LoRa transceiver, 868.1 MHz, SF7 |
| Gateway Software | Congduc Pham low-cost gateway | Packet forwarder to TTN v3 |
| End-device MCU | Arduino Mega 2560 | Sketch execution, sensor reading |
| End-device Radio | Dragino LoRa Shield v1.4 | SX1276 on SPI, pins NSS=10 RST=9 DIO0=2 |
| LoRa Library | MCCI LoRaWAN LMIC (catena) | LoRaWAN stack implementation |
| Network Server | TTN v3 (EU868) | Cloud LoRaWAN network server |

## **3.4 Document Scope**

This manual covers the EU868 band configuration exclusively. US915, AS923, and other regions share many of the same internal mechanisms, and the user-space library is parameterized to support them via the SCG\_NUM\_CHANNELS and SCG\_RX2\_DR configuration defines, but the threat analysis, source code references, and examples in this document are written for EU868.

The document does not cover the Congduc Pham gateway software or TTN v3 server-side configuration. It is focused entirely on the MCCI LMIC end-device library and the changes needed at the sketch and library level to achieve reliable single-channel operation.

# **4\. MCCI LMIC Architecture Overview**

## **4.1 Relevant Internal Data Structures**

The MCCI LMIC library maintains a global struct named LMIC (type lmic\_t) that holds all protocol state. The fields most relevant to single-channel enforcement are:

| Field | Type | Description |
| :---- | :---- | :---- |
| LMIC.txChnl | u1\_t | Index of the channel selected for the next transmission. Set by LMIC\_findNextChannel(). |
| LMIC.channelMap | u2\_t | Bitmask of enabled channels. Bit N is set if channel N is enabled. LMIC\_disableChannel() clears the bit. |
| LMIC.channelShuffleMap | u2\_t | Working copy of channelMap used by LMIC\_findNextChannel() to cycle through enabled channels without repetition. Reset to channelMap when exhausted. |
| LMIC.datarate | u1\_t | Current data rate (spreading factor). DR\_SF7 \= 5 in EU868. Set by LMIC\_setDrTxpow() or directly by the ADR and join mechanisms. |
| LMIC.txpow | s1\_t | Current TX power in dBm. Set by LMIC\_setDrTxpow(). |
| LMIC.dn2Dr | u1\_t | Data rate for the RX2 receive window. |
| LMIC.dn2Freq | u4\_t | Frequency for the RX2 receive window in Hz. |
| LMIC.channelDlFreq\[\] | u4\_t\[\] | Per-channel downlink frequency override, set by DlChannelReq MAC command. Non-zero redirects the RX1 window to a different frequency. |
| LMIC.opmode | u2\_t | Bitmask of current operation mode flags. OP\_TXRXPEND, OP\_NEXTCHNL, OP\_JOINING, etc. |

## **4.2 Key Functions Involved in Channel and DR Management**

The following functions in the MCCI library source are the primary actors in channel and data rate management. Understanding them is necessary to evaluate the threat catalog in Section 5\.

| Function | Source File | Role |
| :---- | :---- | :---- |
| LMIC\_findNextChannel() | lmic\_channelshuffle.c | Selects txChnl from channelShuffleMap. Called before every TX. |
| LMICeulike\_initJoinLoop() | lmic\_eu\_like.c | Initializes the join procedure. Sets channelShuffleMap \= 0x0007 (channels 0-2) regardless of channelMap. |
| LMICeulike\_nextJoinState() | lmic\_eu\_like.c | Called after each join TX. Advances to the next join channel and applies DR backoff after each full cycle. |
| LMICeulike\_mapChannels() | lmic\_eu\_like.c | Applies a LinkAdrReq MAC command to channelMap and datarate. May re-enable disabled channels. |
| LMICeulike\_processJoinAcceptCFList() | lmic\_eu\_like.c | Parses a CFList from the Join Accept frame. Adds up to 5 new channels to channelMap. |
| applyAdrRequests() | lmic.c | Applies ADR adjustments from network downlinks. May change datarate and channel mask even with ADR mode disabled. |
| LMIC\_setDrTxpow() | lmic.c | Public API to set datarate and txpow. Also sets OP\_NEXTCHNL, triggering a channel selection on the next tick. |
| LMIC\_disableChannel() | lmic.c | Public API to clear a channel bit in channelMap. Does not affect channelShuffleMap directly. |
| LMIC\_setLinkCheckMode() | lmic.c | Enables or disables link check validation. If enabled and LINK\_CHECK\_DEAD fires, triggers a re-join. |

# **5\. Threat Catalog**

This section catalogs all identified behaviors in the MCCI LMIC library that can cause a LoRaWAN end device to deviate from single-channel operation. Each threat is described by its MCCI source location, the condition that triggers it, the observable consequence on a single-channel gateway deployment, and the countermeasure applied.

Severity levels are defined as follows:

| Level | Definition |
| :---- | :---- |
| Critical | Causes immediate, guaranteed frame loss on every affected TX or join attempt. |
| High | Causes frame loss under normal operating conditions (e.g., after a successful join or a downlink). |
| Medium | Causes frame loss in specific but common conditions (e.g., after a link-check timeout). |
| Low | Degrades reliability without causing direct frame loss (e.g., wrong RX2 parameters reduce downlink success rate). |

## **5.1 Category 1 — Channel Selection Threats**

### **Threat 1.1 — LMIC\_findNextChannel() Channel Rotation**

Source: lmic\_channelshuffle.c — LMIC\_findNextChannel()

Trigger: Called before every TX scheduling. Selects txChnl from channelShuffleMap by finding the next set bit, then clears that bit. When channelShuffleMap is exhausted (all bits clear), it is reloaded from channelMap.

Consequence: If channelMap has more than one bit set (i.e., more than one channel enabled), txChnl will cycle through all enabled channels. On a single-channel gateway monitoring 868.1 MHz, any frame sent on channel 1 (868.3 MHz) or channel 2 (868.5 MHz) is lost. With the default EU868 three-channel setup (channels 0-2), two out of every three join attempts will be lost.

Severity: Critical — affects every TX when multiple channels are enabled.

// Countermeasure: lock channelShuffleMap to only one channel  
LMIC\_disableChannel(1);  // ... for all i \!= SCG\_CHANNEL  
LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);  // lock the working set

### **Threat 1.2 — LMICeulike\_initJoinLoop() Shuffle Map Reset**

Source: lmic\_eu\_like.c — LMICeulike\_initJoinLoop()

Trigger: Called at the start of every OTAA join procedure (when LMIC\_startJoining() is called or when a re-join is triggered by LINK\_CHECK\_DEAD). Resets channelShuffleMap \= 0x0007 (channels 0, 1, and 2\) regardless of the current state of channelMap.

Consequence: Even if SCG\_init() has correctly locked channelShuffleMap \= 0x0001, the moment a join attempt starts, the library resets the shuffle map to 0x0007, re-enabling channel rotation for all join transmissions until the application corrects it.

Severity: Critical — affects every OTAA join attempt.

// Countermeasure: correct txChnl and channelShuffleMap in EV\_JOIN\_TXCOMPLETE  
// (fires after each failed join TX, before the next retry is scheduled)  
LMIC.txChnl            \= SCG\_CHANNEL;  
LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);

### **Threat 1.3 — LinkAdrReq Channel Map Renegotiation**

Source: lmic\_eu\_like.c — LMICeulike\_mapChannels()

Trigger: Called when a LinkAdrReq MAC command is received in a downlink frame, processed inside os\_runloop\_once() during RX window handling. The network server sends this command to instruct the device to enable or disable specific channels.

Consequence: LMICeulike\_mapChannels() writes directly to channelMap and may re-enable previously disabled channels. On a single-channel gateway, this silently re-enables channels 1-8, causing the next TX to be sent on a channel the gateway is not monitoring. The device will not receive an error — it simply loses the frame.

Severity: High — occurs after receiving any downlink containing a LinkAdrReq.

// Countermeasure: SCG\_enforce() in loop() catches the re-enabled channels  
// on the next tick after os\_runloop\_once() processes the downlink.  
if (LMIC.txChnl \!= SCG\_CHANNEL) {  
    LMIC.txChnl            \= SCG\_CHANNEL;  
    LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);  
}

### **Threat 1.4 — NewChannelReq Channel Re-Enable (field-confirmed)**

Source: lmic.c — MCMD\_NewChannelReq handler (\~line 1004), guarded by \#if \!defined(DISABLE\_MCMD\_NewChannelReq)

Trigger: The network server sends a NewChannelReq MAC command — piggybacked in the FOpts of any downlink, including one with zero application payload (LMIC.dataLen can read 0 while FOpts still carries the command). The handler calls LMIC\_setupChannel(chidx, freq, DR\_RANGE\_MAP(MinDR, MaxDR), \-1) for whatever channel index and frequency the network names, which sets that channel's bit in LMIC.channelMap — unconditionally, regardless of any earlier LMIC\_disableChannel() call.

Consequence: The re-enabled channel stays enabled in channelMap indefinitely (until reboot or another explicit disable). Neither the v1.0-documented SCG\_apply() design nor the actually-deployed SCG\_enforce() ever reads or restores channelMap directly — both only watch channelShuffleMap/txChnl (and, in the deployed version, datarate). So the corruption is invisible until channelShuffleMap is next exhausted and reloads from channelMap — which can happen inside the very same os\_runloop\_once() call that also dispatches the already-pending TX, i.e. strictly before SCG\_enforce() gets another chance to run. This defeats the "TX is always scheduled after the corrective call has run" assumption in §6.4 and §10.1: the corruption and the wrong-channel TX can share a single tick.

This is not theoretical. Field debug log, ABP node MCCI\_ttn\_Raw\_DHT22\_Dust\_MQ9\_AlwaysOn, TTN v3 EU868, 2026-07-23, LMIC\_DEBUG\_LEVEL=1:

```  
1443381: engineUpdate, opmode=0x900  
Sleeping 27.7s  
T:28.9C H:68.7% DUST_ADC:70 MQ9_ADC:FAULT BATT_ADC:363 [01 21 89 00 46 FF FF 01 6B]  
1581834: engineUpdate, opmode=0x908  
1582957: 1583075: TXMODE, freq=867300000, len=22, SF=7, BW=125, CR=4/5, IH=0  
```

867300000 Hz is channel 4 in this sketch's EU868 setup (LMIC\_setupChannel(4, 867300000, ...)) — SCG\_CHANNEL is 0 (868.1 MHz). This is a live radio transmission on a channel the single-channel gateway does not monitor, on a node whose enforcement layer was active and had not logged any txChnl/datarate drift.

Severity: **Critical** — the only threat in this catalog with direct field confirmation of a wrong-channel TX escaping the enforcement layer, rather than a source-level trigger condition reasoned about statically. **Status: mitigated as of v1.2** (see countermeasure history below).

**Countermeasure attempt 1 (reverted) — compile out the handler:**

// \#define DISABLE\_MCMD\_NewChannelReq   // in lmic\_project\_config.h

This was applied first, on the reasoning that the network could then never re-add a channel via this MAC command, regardless of enforcement timing. It stopped the channel corruption, but broke something else: macCmdSize\[\] in lmic.c still lists NewChannelReq as a 6-byte command unconditionally (that table is not gated by this define), so the FOpts parser correctly advances to the opcode, finds no case for it in the switch (which *is* compiled out), and falls into default: — olen \= oidx; break; — aborting the rest of the MAC command scan. No NewChannelAns is ever queued, so TTN never gets an acknowledgment and re-sends the same NewChannelReq on every downlink indefinitely. Observed directly, ABP node MCCI\_ttn\_Raw\_DHT22\_Dust\_MQ9\_AlwaysOn, TTN v3 EU868, 2026-07-23:

```
6869169: process mac commands for port 0 (olen=0x1e)
6869216: error processing mac commands for port 0 (len=0x1e, optendindex=0x6)
6870250: Received downlink, window=RX1, port=0, ack=0, txrxFlags=0x21
```

This pattern repeated on every single downlink in the capture — a permanent retry storm, and arguably a worse operational state than the original threat (continuous RX activity, no MAC command from the network ever succeeds again, not just NewChannelReq).

**Countermeasure attempt 2 (current) — sweep channelMap in SCG\_enforce():**

The fix moved to enforcement instead of suppression. SCG\_enforce() (§8.6) now unconditionally re-disables every non-SCG\_CHANNEL channel every tick:

for (int i \= 0; i \< SCG\_NUM\_CHANNELS; i++)  
    if (i \!= SCG\_CHANNEL) LMIC\_disableChannel(i);

This lets NewChannelReq be parsed and acknowledged normally (stopping the retry storm) while directly closing the blind spot that caused Threat 1.4 in the first place — channelMap is now actually monitored, not just txChnl/datarate as a proxy for it. LMIC\_disableChannel() is cheap enough (a couple of field writes, lmic\_eu\_like.c) that doing this unconditionally every tick, rather than only on detected drift, is not a meaningful cost.

Field-confirmed, second capture, same node, 2026-07-23, after reverting the define and deploying the hardened SCG\_enforce():

```
519346: process mac commands for port 0 (olen=0x1e)
519408: Received downlink, window=RX1, port=0, ack=0, txrxFlags=0x21
520266: EV_TXCOMPLETE
...
595998: 596114: TXMODE, freq=868100000, len=23, SF=7, BW=125, CR=4/5, IH=0
```

Two things to note: no "error processing" line — the command parsed cleanly this time — and it appears exactly once in the whole capture, not on every downlink, confirming TTN got its acknowledgment and stopped retrying. The next uplink's frame length also grew from the usual 22 to 23 bytes, consistent with the device appending a NewChannelAns MAC response into that uplink's FOpts. Every TXMODE line in this capture reads freq=868100000 — no recurrence of the wrong-channel transmission.

### **Threat 1.5 — Active-Channel Frequency Rewrite (not yet field-confirmed)**

Source: lmic.c — MCMD\_NewChannelReq handler (same handler as Threat 1.4), targeting `chidx == SCG_CHANNEL` instead of some other index.

Trigger: A NewChannelReq naming the *currently locked* channel index with a different frequency. `LMIC_setupChannel(SCG_CHANNEL, newFreq, ...)` succeeds unconditionally for any non-default channel (see §10.3 revision note and Finding 1 in the v1.3 revision note above — the call only refuses default indices 0-2), overwriting `LMIC.channelFreq[SCG_CHANNEL]` in place. The channel stays enabled throughout — `channelMap` never changes — so the v1.2 channelMap sweep (which fixed Threat 1.4) does not see anything to correct.

Consequence: The device continues transmitting on index `SCG_CHANNEL`, believing itself locked, while the actual radio frequency at that index has been silently redirected by the network. Identical failure mode to Threat 1.4 (wrong-frequency TX invisible to the single-channel gateway), reached through a different trigger condition that the v1.2 fix does not cover.

Severity: **Critical if triggered** — not yet observed in the field. Discovered by source review during SmartGated §3.6 design work (2026-09-10), not by a debug capture. Status matches Threat 1.4's pre-2026-07-23 state: a reasoned, source-level threat awaiting field confirmation.

// Countermeasure (proposed, not yet shipped — see §8.8):  
// SCG\_enforce() must also reassert channelFreq\[SCG\_CHANNEL\]/channelDrMap\[SCG\_CHANNEL\]  
// from a sketch-held reference table every tick, the same way it already sweeps channelMap.  
if (LMIC.channelFreq\[SCG\_CHANNEL\] \!= g\_scgKnownFreq\[SCG\_CHANNEL\])  
    LMIC.channelFreq\[SCG\_CHANNEL\] \= g\_scgKnownFreq\[SCG\_CHANNEL\];

## **5.2 Category 2 — Data Rate Threats**

### **Threat 2.1 — ADR Data Rate Adjustment**

Source: lmic.c — applyAdrRequests()

Trigger: Called when a downlink carries ADR commands. The MCCI library processes ADR requests from the network server. Critically, applyAdrRequests() can alter the datarate field even when LMIC\_setAdrMode(0) has been called by the application.

Consequence: The data rate is changed to a value chosen by the network server rather than the application. On a single-channel gateway configured for a specific spreading factor, a different SF means the gateway cannot decode the frame even if it is on the correct frequency.

Severity: High — occurs after downlinks in ADR-active networks.

// Countermeasure:  
LMIC\_setAdrMode(0);           // in SCG\_init()  
if (LMIC.datarate \!= SCG\_DATARATE)  // in SCG\_enforce()  
    LMIC.datarate \= SCG\_DATARATE;

### **Threat 2.2 — Join DR Backoff**

Source: lmic\_eu\_like.c — LMICeulike\_nextJoinState()

Trigger: Called after each failed join attempt. After cycling through all three default join channels (0, 1, 2\) once, it decrements the data rate by one step (e.g., SF7 → SF8 → SF9 ...). This is a standard LoRaWAN mechanism to increase coverage probability with longer-range spreading factors after initial join failures.

Consequence: The device transmits join requests at progressively lower data rates. The single-channel gateway may be configured for a specific SF (e.g., SF7). A join request sent at SF12 will not be decoded by a gateway expecting SF7 (though SF12 is more sensitive, a gateway driver locked to a specific SF may reject other SFs).

Severity: Medium — affects only OTAA; ABP is immune. Caught by SCG\_enforce().

## **5.3 Category 3 — Join Procedure Threats**

### **Threat 3.1 — CFList Channel Injection**

Source: lmic\_eu\_like.c — LMICeulike\_processJoinAcceptCFList()

Trigger: Called when a Join Accept frame is received that includes a CFList (Channel Frequency List) field. TTN v3 sends a CFList for EU868 end-devices, adding channels 3 through 7 (867.1, 867.3, 867.5, 867.7, 867.9 MHz). This function runs before EV\_JOINED fires.

Consequence: By the time the application code in EV\_JOINED runs, channelMap already has bits 3-7 set, and channelShuffleMap has been reloaded. Without corrective action, the very first data uplink after a successful join may be sent on channel 3, 4, 5, 6, or 7\.

Severity: Critical — affects the first post-join uplink on every OTAA session with TTN v3.

// Countermeasure: SCG\_on\_joined() runs immediately in EV\_JOINED  
for (int i \= 0; i \< SCG\_NUM\_CHANNELS; i++)  
    if (i \!= SCG\_CHANNEL) LMIC\_disableChannel(i);  
LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);

### **Threat 3.2 — Join Loop Channel Cycling**

Source: lmic\_eu\_like.c — LMICeulike\_nextJoinState() \+ LMIC\_findNextChannel()

Trigger: Each join request TX completes (EV\_JOIN\_TXCOMPLETE fires, meaning no Join Accept was received). nextJoinState() calls LMIC\_findNextChannel() to advance to the next channel in the join channel map before scheduling the retry. txChnl is modified before the application's EV\_JOIN\_TXCOMPLETE handler runs.

Consequence: Join request retries are sent on channels 0, 1, 2 in rotation. On a single-channel gateway at 868.1 MHz, two out of every three join requests are lost. Since TTN v3 requires a successful uplink to complete the OTAA session, this triples the time to join in the best case and causes join failure if the retry limit is reached.

Severity: Critical — affects every OTAA join attempt under retry conditions.

## **5.4 Category 4 — MAC Command Threats**

### **Threat 4.1 — DlChannelReq Downlink Frequency Redirect**

Source: lmic.c (MAC command processing within onEvent RX handler)

Trigger: The network server sends a DlChannelReq MAC command, which sets channelDlFreq\[i\] for the uplink channel i to a different frequency. The LMIC library then uses this frequency for the RX1 downlink window on that channel instead of the default (uplink frequency \= downlink frequency in EU868 by default).

Consequence: The RX1 window is opened on a different frequency than the gateway is transmitting on, so downlinks are missed. This does not directly cause uplink loss, but missed downlinks prevent reception of MAC commands, confirmed-frame ACKs, and application downlinks.

Severity: Medium — downlinks missed, not uplinks.

// Countermeasure: zero all channelDlFreq entries in setup()  
// (compile-time conditional — only needed if DlChannelReq is enabled)  
\#if \!defined(DISABLE\_MCMD\_DlChannelReq)  
    for (uint8\_t i \= 0; i \< 9; i++) LMIC.channelDlFreq\[i\] \= 0;  
\#endif

### **Threat 4.2 — Link Check Re-Join Trigger**

Source: lmic.c — LMIC\_setLinkCheckMode() and LINK\_CHECK\_DEAD handling

Trigger: Link check mode is enabled by default after OTAA join. If approximately 32 consecutive uplinks are sent without a downlink being received (LINK\_CHECK\_DEAD condition), the MCCI library automatically initiates a re-join. The re-join restarts the join procedure, which resets channelShuffleMap \= 0x0007 (Threat 1.2) and begins channel rotation again.

Consequence: A functioning end device with a functioning gateway can silently enter a re-join loop if no downlinks are generated by the application or the network server within the link check window. The re-join rotates channels and the device is unreachable until it completes another successful join on the single channel.

Severity: High — can silently interrupt a running deployment.

LMIC\_setLinkCheckMode(0);  // called in SCG\_init() and SCG\_on\_joined()

## **5.5 Category 5 — RX Window Threats**

### **Threat 5.1 — RX2 Parameter Override by Join Accept**

Source: lmic.c (Join Accept processing)

Trigger: The Join Accept frame carries DLSettings (RX1 offset, RX2 DR). The MCCI library applies these values to LMIC.dn2Dr and possibly LMIC.dn2Freq. TTN v3 sends RX2 parameters in the Join Accept.

Consequence: The RX2 window uses a different data rate than the gateway expects. A single-channel gateway configured for RX2 at SF9 (TTN EU868 default) will fail to decode a frame sent at SF12 (MCCI LMIC default). Since RX2 is the fallback window, this reduces downlink reliability.

Severity: Low — reduces downlink success rate but does not cause uplink loss.

LMIC.dn2Dr   \= SCG\_RX2\_DR;     // in SCG\_init() and SCG\_on\_joined()  
LMIC.dn2Freq \= SCG\_RX2\_FREQ;

### **Threat 5.2 — RXParamSetupReq Override**

Source: lmic.c (MAC command processing)

Trigger: The network server sends an RXParamSetupReq MAC command to change the RX2 frequency and/or data rate. This is processed inside os\_runloop\_once() and overwrites LMIC.dn2Dr and LMIC.dn2Freq.

Consequence: Same as Threat 5.1 — downlinks on the RX2 window may be lost. Additionally, if the RX2 frequency is changed to a channel the gateway does not monitor, all RX2 downlinks are permanently lost.

Severity: Low-to-Medium — depends on whether the application relies on RX2 downlinks.

## **5.6 Threat Summary Table**

| ID | Threat Name | Source Location | Trigger | Impact | Severity |
| :---- | :---- | :---- | :---- | :---- | :---- |
| 1.1 | Channel rotation | lmic\_channelshuffle.cLMIC\_findNextChannel() | Every TX scheduling | Frame lost on wrong channel | **Critical** |
| 1.2 | Join shuffle map reset | lmic\_eu\_like.cLMICeulike\_initJoinLoop() | OTAA join start / re-join | Join requests on wrong channels | **Critical** |
| 1.3 | LinkAdrReq channel re-add | lmic\_eu\_like.cLMICeulike\_mapChannels() | Downlink with LinkAdrReq | Channels re-enabled post-downlink | **High** |
| 1.4 | NewChannelReq channel re-add (field-confirmed, mitigated v1.2) | lmic.cMCMD\_NewChannelReq handler | Downlink with NewChannelReq | Channel re-enabled in channelMap; confirmed live wrong-channel TX | **Critical** |
| 1.5 | Active-channel frequency rewrite (not yet field-confirmed, v1.3) | lmic.c, MCMD NewChannelReq handler (targets SCG_CHANNEL itself) | Downlink with NewChannelReq naming the active index | channelFreq[SCG_CHANNEL] silently overwritten; channelMap sweep doesn't see it | **Critical if triggered** |
| 2.1 | ADR DR adjustment | lmic.capplyAdrRequests() | Downlink with ADR commands | Wrong SF, frame not decoded | **High** |
| 2.2 | Join DR backoff | lmic\_eu\_like.cLMICeulike\_nextJoinState() | Failed join retries | Join at wrong SF | **Medium** |
| 3.1 | CFList channel injection | lmic\_eu\_like.cprocessJoinAcceptCFList() | Successful OTAA join (TTN v3) | Channels 3-7 added to channelMap | **Critical** |
| 3.2 | Join channel cycling | lmic\_eu\_like.cnextJoinState() | Failed join TX complete | 2/3 of join retries on wrong channel | **Critical** |
| 4.1 | DlChannelReq redirect | lmic.cMAC command handler | Downlink with DlChannelReq | RX1 window on wrong frequency | **Medium** |
| 4.2 | Link check re-join | lmic.cLINK\_CHECK\_DEAD handler | \~32 uplinks without downlink | Unexpected re-join, channel rotation | **High** |
| 5.1 | RX2 DR override (join) | lmic.cJoin Accept processing | Successful OTAA join | RX2 at wrong SF | **Low** |
| 5.2 | RXParamSetupReq override | lmic.cMAC command handler | Downlink with RXParamSetupReq | RX2 freq/DR changed | **Low** |

# **6\. Approach Comparison**

## **6.1 Approach A — Library-Side Patches**

The most direct solution is to modify the MCCI library source files to add conditional compilation blocks guarded by a preprocessor flag, for example \#define LMIC\_SINGLE\_CHANNEL\_GATEWAY. When this flag is defined, the channel-rotation and DR-backoff code paths would be disabled at compile time, and fixed values would be substituted.

Advantages:

* Zero runtime overhead — conditional blocks are compiled out entirely.  
* No one-tick state window (see Known Limitations) — the wrong-channel selection never happens.  
* Clean integration with the library public API — no direct struct access required.  
* Upstreamable — could be contributed back to the MCCI project.

Disadvantages:

* Library updates overwrite the patches. The developer must re-apply all changes after every update.  
* Patch maintenance burden — MCCI releases new versions; the patch set must be kept in sync.  
* Risk of merge conflicts if the modified functions are refactored upstream.  
* Not portable — patches are specific to one installation of the library.  
* Not suitable for teams or CI/CD workflows where the library is installed fresh from the package manager.

## **6.2 Approach B — User-Space Enforcement (SingleChannelLMIC)**

The SingleChannelLMIC library is a single header file that the sketch includes alongside the MCCI library. It uses the MCCI public API (LMIC\_disableChannel, LMIC\_setAdrMode, LMIC\_setDrTxpow, LMIC\_setLinkCheckMode) and, where the public API is insufficient (specifically to avoid triggering OP\_NEXTCHNL in SCG\_enforce), direct struct field assignment. The MCCI library source files are never touched.

Advantages:

* Library-update-safe — the MCCI library can be updated without losing any enforcement.  
* Portable — the header file travels with the sketch or as a standalone Arduino library.  
* No re-apply burden — install once, works across library versions as long as the LMIC struct fields do not change names (they have not changed in the relevant MCCI versions).  
* Transparent — all enforcement is visible in the sketch or a single auditable header file.  
* Configurable — a define-based configuration system allows adapting to different bands, channels, and data rates without editing the header.

Disadvantages:

* One-tick state window — LinkAdrReq and CFList are processed inside os\_runloop\_once(); the corrective call runs on the following tick. No actual wrong-channel TX occurs (TX is always scheduled after the corrective call has run), but the internal state is temporarily incorrect.  
* Direct struct field access — LMIC.txChnl, LMIC.datarate, and LMIC.channelShuffleMap are internal fields. A major MCCI library refactor could rename or remove them.  
* Slightly more application-level boilerplate — four call sites (SCG\_init, SCG\_on\_joined, SCG\_on\_join\_txcomplete, SCG\_enforce) must be added correctly.

## **6.3 Comparison Table**

| Criterion | Approach A — Library Patches | Approach B — User-Space (SCG) |
| :---- | :---- | :---- |
| Library update safety | FAILS — patches overwritten | SAFE — library untouched |
| Runtime overhead | None (compiled out) | Minimal (one if-check per tick) |
| One-tick state window | None | Exists (no TX risk in practice) |
| Internal struct access | No | Yes (txChnl, datarate, shuffleMap) |
| Portability | Tied to library installation | Header travels with project |
| Team/CI compatibility | Requires manual patch on fresh install | Install library, include, call |
| Upstreamable to MCCI | Yes | No (user-space only) |
| Implementation effort | High (multiple source files) | Low (one header, four call sites) |
| Auditability | Requires reading MCCI source | Entirely in one readable header |
| Recommended for | Zero-overhead embedded products | Research, education, rapid deploy |

## **6.4 Recommendation and Argument**

For the research context of this project — a low-cost sensor network running on Arduino hardware under active development, maintained by researchers rather than embedded systems specialists — Approach B (SingleChannelLMIC) is the unambiguously correct choice for the following reasons:

1\. Library updates are routine. The MCCI library is actively maintained. In an academic or research environment, library updates may be applied by any team member at any time via the Arduino IDE Library Manager. Approach A would require the team to maintain a private fork or a patch application script, adding process overhead that is likely to be forgotten, especially across academic years when different student cohorts take over the project.

2\. **(Revised in v1.1, updated in v1.2.)** The one-tick state window was originally assessed as not a practical risk, on the reasoning that LoRaWAN TX is always scheduled via os\_setTimedCallback on a future tick, giving SCG\_enforce() a chance to correct state before the scheduler calls LMIC\_findNextChannel(). Field testing (Threat 1.4, 2026-07-23) showed this does not hold for channelMap corruption: the shipped SCG\_enforce() never monitored channelMap, so a NewChannelReq could leave it corrupted indefinitely, and the actual wrong-channel pick only became visible when channelShuffleMap was next exhausted and reloaded — which could happen inside the same os\_runloop\_once() call that dispatches an already-scheduled TX, after that tick's SCG\_enforce() had already run. A confirmed wrong-channel frame went out over the air. The first countermeasure tried (DISABLE\_MCMD\_NewChannelReq, removing the trigger at the MAC-command level) broke MAC command parsing instead (see Threat 1.4) and was reverted. The countermeasure that actually shipped, as of v1.2, closes the gap directly: SCG\_enforce() now unconditionally sweeps channelMap every tick, so the state it protects is no longer limited to txChnl/datarate. This was field-confirmed to stop the wrong-channel TX without the MAC-parsing side effect (Threat 1.4, second capture).

3\. Direct struct access is stable. The fields txChnl, datarate, channelShuffleMap, dn2Dr, and dn2Freq have not changed names or semantics across the MCCI LMIC versions examined (v3.x through v4.x). They are fundamental to the LoRaWAN protocol and are unlikely to be renamed without a major breaking change. If they are, the compiler will fail with a clear error rather than producing silent wrong behavior.

4\. The formal library proposal (Section 7\) can be pursued in parallel. Nothing prevents the project from contributing a LMIC\_SINGLE\_CHANNEL\_GATEWAY patch to MCCI while using the user-space enforcement in production. If the patch is accepted upstream, the SingleChannelLMIC library can be retired in favor of the native flag with no disruption to existing sketches.

# **7\. Formal LMIC\_SINGLE\_CHANNEL\_GATEWAY Proposal**

## **7.1 Motivation**

Single-channel gateways are widely used in constrained environments and represent a legitimate and increasingly common LoRaWAN deployment scenario. Native library support, activated by a single preprocessor flag, would benefit this community without any impact on standard multi-channel users (the flag is off by default).

This proposal documents the changes that would need to be made to the MCCI library to implement LMIC\_SINGLE\_CHANNEL\_GATEWAY at the library level. It is intended as a contribution proposal to the MCCI project and as a reference for researchers who choose the library-patch approach.

## **7.2 Proposed Preprocessor Interface**

The user would add the following to their project configuration file (lmic\_project\_config.h):

\#define LMIC\_SINGLE\_CHANNEL\_GATEWAY  1  
\#define LMIC\_SINGLE\_CHANNEL\_INDEX    0        // channel index (0 \= ch0)  
\#define LMIC\_SINGLE\_CHANNEL\_DR       DR\_SF7   // fixed data rate  
\#define LMIC\_SINGLE\_CHANNEL\_TXPOW    14       // fixed TX power

With these defines active, the library would automatically enforce single-channel behavior with zero user-space boilerplate. No call to SCG\_init(), SCG\_on\_joined(), or SCG\_enforce() would be needed.

## **7.3 Required Library Changes**

| File | Function / Location | Change Required |
| :---- | :---- | :---- |
| lmic\_channelshuffle.c | LMIC\_findNextChannel() | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: skip the shuffle; always return LMIC\_SINGLE\_CHANNEL\_INDEX. |
| lmic\_eu\_like.c | LMICeulike\_initJoinLoop() | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: set channelShuffleMap \= (1 \<\< LMIC\_SINGLE\_CHANNEL\_INDEX) instead of 0x0007. |
| lmic\_eu\_like.c | LMICeulike\_nextJoinState() | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: skip the DR backoff step (keep datarate fixed at LMIC\_SINGLE\_CHANNEL\_DR). |
| lmic\_eu\_like.c | LMICeulike\_mapChannels() | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: ignore channelMask bits except the single channel bit; reject LinkAdrReq channel-enable commands. |
| lmic\_eu\_like.c | LMICeulike\_processJoinAcceptCFList() | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: discard the CFList entirely; do not add channels from the Join Accept. |
| lmic.c | applyAdrRequests() | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: ignore ADR DR changes; keep LMIC\_SINGLE\_CHANNEL\_DR. |
| lmic.c | MAC command handler (DlChannelReq) | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: reject DlChannelReq (respond with failure, do not apply). |
| lmic.c | MAC command handler (RXParamSetupReq) | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: reject or apply conservatively (preserve dn2Dr \= LMIC\_SINGLE\_CHANNEL\_DR). |
| lmic.c | LMIC\_startJoining() / setup path | If LMIC\_SINGLE\_CHANNEL\_GATEWAY: automatically call LMIC\_setLinkCheckMode(0) and LMIC\_setAdrMode(0). |

## **7.4 Risk Assessment**

The proposed changes are all additive conditional blocks guarded by \#if LMIC\_SINGLE\_CHANNEL\_GATEWAY. They do not touch any code path that executes when the flag is not defined, so the risk to existing multi-channel users is zero provided the conditionals are correctly placed.

The main risk in maintaining the patch is divergence from upstream. If MCCI refactors any of the listed functions — renaming them, splitting them, or changing their internal logic — the patch must be updated. This is why the user-space approach (Section 8\) is preferred for the current research deployment, while the formal proposal is submitted as an upstream contribution.

# **8\. SingleChannelLMIC Library Design**

## **8.1 Architecture Principles**

The SingleChannelLMIC library is designed around five principles:

**Zero MCCI modification:** The MCCI library source files are never edited. All enforcement is in user space.

**Single header:** The entire library is one header file (single\_channel\_lmic.h). All functions are static inline to avoid multiple-definition errors when included across multiple translation units.

**Define-before-include configuration:** All behavior is configurable via \#define macros that the sketch sets before \#include \<single\_channel\_lmic.h\>. This avoids requiring the user to edit the header.

**Threat coverage:** Twelve threats are cataloged in Section 5, all addressed by at least one function in the library. Threat 1.4 (NewChannelReq) was initially handled outside the library, at the MAC-command level via DISABLE\_MCMD\_NewChannelReq — that approach broke MAC command parsing and was reverted (see Threat 1.4). As of v1.2, it is addressed inside SCG\_enforce() itself, which now sweeps channelMap every tick (see §8.6). The mapping is documented in the header source via §-reference comments.

**Minimal footprint:** The functions do only what is necessary. No logging, no state machines, no dynamic allocation. The Serial debug prints can be removed without behavioral change.

## **8.2 Configuration System**

| Define | Default | Overridable Before Include? | Description |
| :---- | :---- | :---- | :---- |
| SCG\_CHANNEL | 0 | Yes | Channel index. EU868 ch0 \= 868.1 MHz. |
| SCG\_DATARATE | DR\_SF7 | Yes | Spreading factor to enforce throughout. |
| SCG\_TXPOWER | 14 | Yes | TX power in dBm. 14 \= EU868 maximum. |
| SCG\_NUM\_CHANNELS | 9 | Yes | Total channels in band. EU868 \= 9, US915 \= 72\. |
| SCG\_RX2\_DR | DR\_SF12 | Yes | RX2 data rate. TTN EU868 default \= DR\_SF9. |
| SCG\_RX2\_FREQ | 869525000 | Yes | RX2 frequency in Hz. TTN EU868 \= 869.525 MHz. |

**Note on this section (v1.1):** v1.0 documented a shared `SCG_apply()` helper that checked five LMIC state fields and was called from all four lifecycle functions. That helper was never actually implemented — `libraries/SingleChannelLMIC/single_channel_lmic.h` as shipped has no `SCG_apply()` at all. Each function below is transcribed directly from the current header source, not from the original design intent.

## **8.3 SCG\_init()**

Called once in setup(), after LMIC\_reset() and after all LMIC\_setupChannel() calls for the region. LMIC\_setupChannel() enables channels in channelMap; SCG\_init() must run after these calls to selectively disable all channels except SCG\_CHANNEL.

Threats addressed: 1.1 (channel rotation), 1.2 (shuffle map), 2.1 (ADR), 4.2 (link check re-join), 5.1 (initial RX2 params).

static inline void SCG\_init(void) {  
    for (int i \= 0; i \< SCG\_NUM\_CHANNELS; i++)  
        if (i \!= SCG\_CHANNEL) LMIC\_disableChannel(i);  
    LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);  
    LMIC\_setAdrMode(0);  
    LMIC\_setDrTxpow(SCG\_DATARATE, SCG\_TXPOWER);  
    LMIC.dn2Dr   \= SCG\_RX2\_DR;  
    LMIC.dn2Freq \= SCG\_RX2\_FREQ;  
    LMIC\_setLinkCheckMode(0);  
}

## **8.4 SCG\_on\_joined()**

Called in the EV\_JOINED case of onEvent(). Required only for OTAA. For ABP, EV\_JOINED does not fire in normal operation.

Threats addressed: 3.1 (CFList injection), 4.2 (link check). **Not addressed, contrary to v1.0:** this function does not touch dn2Dr or dn2Freq, so it does not re-assert RX2 parameters after a join — if Join Accept altered them (Threat 5.1), they are left as the join process set them, not reset to SCG\_RX2\_DR/SCG\_RX2\_FREQ.

static inline void SCG\_on\_joined(void) {  
    for (int i \= 0; i \< SCG\_NUM\_CHANNELS; i++)  
        if (i \!= SCG\_CHANNEL) LMIC\_disableChannel(i);  
    LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);  
    LMIC\_setDrTxpow(SCG\_DATARATE, SCG\_TXPOWER);  
    LMIC\_setLinkCheckMode(0);  
}

## **8.5 SCG\_on\_join\_txcomplete()**

Called in the EV\_JOIN\_TXCOMPLETE case of onEvent(). EV\_JOIN\_TXCOMPLETE fires after each join TX for which no Join Accept was received (i.e., each failed attempt). Required only for OTAA.

Threats addressed: 3.2 (join channel cycling / nextJoinState rotation).

static inline void SCG\_on\_join\_txcomplete(void) {  
    LMIC.txChnl            \= SCG\_CHANNEL;  
    LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);  
}

## **8.6 SCG\_enforce()**

Called in loop() before os\_runloop\_once(). **As of v1.2**, unconditionally sweeps LMIC.channelMap back down to SCG\_CHANNEL every tick, then checks two further LMIC state fields — txChnl and datarate — correcting channelShuffleMap alongside txChnl when it drifts.

Threats addressed: 1.3 (LinkAdrReq aftermath), 1.4 (NewChannelReq aftermath — field-confirmed fix, see Threat 1.4), 2.1 (ADR DR change), 2.2 (join DR backoff).

**v1.0/v1.1 history:** the originally-shipped version of this function (documented in v1.1) checked only txChnl and datarate, and never touched channelMap at all — that was the root cause of Threat 1.4. The v1.0 manual separately claimed a five-field check via a shared SCG\_apply() helper that was never actually implemented in any shipped version. Neither history matches the code below.

**Still not addressed:** txpow and dn2Dr/dn2Freq are not checked here. A LinkAdrReq that changes txpow via applyAdrRequests(), or an RXParamSetupReq that changes dn2Dr/dn2Freq, are still never corrected. Unlike channelMap, these have no field-confirmed failure mode yet — see §10.3.

static inline void SCG\_enforce(void) {  
    for (int i \= 0; i \< SCG\_NUM\_CHANNELS; i++) {  
        if (i \!= SCG\_CHANNEL) LMIC\_disableChannel(i);  
    }  
    if (LMIC.txChnl \!= SCG\_CHANNEL) {  
        LMIC.txChnl            \= SCG\_CHANNEL;  
        LMIC.channelShuffleMap \= (1 \<\< SCG\_CHANNEL);  
    }  
    if (LMIC.datarate \!= SCG\_DATARATE) {  
        LMIC.datarate \= SCG\_DATARATE;  
    }  
}

## **8.7 SCG\_setRadioConfig()**

Called at runtime inside downlink MAC command handlers or dynamic data rate adaptation logic (e.g., when a downlink command invokes DL\_SF\_CHANGED or DL\_TXPOWER\_CHANGED). It wraps LMIC\_setDrTxpow() while ignoring the channel argument (which is strictly enforced by SCG\_CHANNEL and SCG\_enforce()).

Threats addressed: Enables dynamic runtime reconfiguration of radio parameters without breaking single-channel enforcement. When used alongside sketches that map SCG\_DATARATE to a dynamic variable, changes persist across loop() iterations.

static inline void SCG\_setRadioConfig(int channel, int dr, int txpow) {  
    (void)channel;  
    LMIC\_setDrTxpow(dr, txpow);  
}

## **8.8 Proposed v1.3 design: direct-table channel and RX2 enforcement (not yet implemented)**

Design draft only — no code has shipped for this section. Written up here so the reasoning behind it (Findings 1-3 of the v1.3 revision note) is on record before implementation, per the SmartGated project's §3.6 (locked-channel retargeting) and proposed RX2 freq/SF downlink work.

**Motivating constraint:** `SCG_CHANNEL` can no longer be a compile-time constant if it is to change at runtime via downlink — it must alias a variable, the same pattern already used for `SCG_DATARATE`/`SCG_currentDR`. But once channel selection is runtime-mutable, the sketch needs a way to *retarget* that does not go through `LMIC_setupChannel()` (Finding 1: refuses defaults) or `LMIC_disableChannel()` alone (Finding 2: doesn't restore anything, only tears down).

**Proposed mechanism — a sketch-held reference table, never touched by the LMIC public API after boot:**

```
// Populated once in setup() from the same values already passed to
// LMIC_setupChannel() there — not a new source of truth, just kept
// around instead of being discarded after the initial registration.
static const uint32_t g_scgKnownFreq[SCG_NUM_CHANNELS]  = { ... };
static const uint16_t g_scgKnownDrMap[SCG_NUM_CHANNELS] = { ... };
```

`SCG_enforce()` (v1.3 proposed) extends the existing per-tick sweep to also reassert the active channel's own table entry, closing Threat 1.5:

```
static inline void SCG_enforce(void) {
    for (int i = 0; i < SCG_NUM_CHANNELS; i++) {
        if (i != SCG_CHANNEL) LMIC_disableChannel(i);   // unchanged, v1.2
    }
    // v1.3 addition — closes Threat 1.5: reassert the active channel's
    // OWN frequency/DR-map, not just whether it's enabled.
    LMIC.channelFreq[SCG_CHANNEL]  = g_scgKnownFreq[SCG_CHANNEL];
    LMIC.channelDrMap[SCG_CHANNEL] = g_scgKnownDrMap[SCG_CHANNEL];
    if (LMIC.txChnl != SCG_CHANNEL) { ... }             // unchanged, v1.2
    if (LMIC.datarate != SCG_DATARATE) { ... }          // unchanged, v1.2
}
```

A new `SCG_setChannel(newIdx)` performs the retarget itself, entirely via direct struct writes — no `LMIC_setupChannel()` call, so Finding 1's default-channel refusal never comes into play:

```
static inline void SCG_setChannel(uint8_t newIdx) {
    LMIC.channelFreq[newIdx]  = g_scgKnownFreq[newIdx];
    LMIC.channelDrMap[newIdx] = g_scgKnownDrMap[newIdx];
    LMIC.channelMap           = (1 << newIdx);
    LMIC.channelShuffleMap    = (1 << newIdx);
    LMIC.txChnl               = newIdx;
    SCG_CHANNEL_VAR           = newIdx;   // the runtime variable SCG_CHANNEL aliases
}
```

The next `SCG_enforce()` tick then disables the previously-active index (fine — its table entry survives in `g_scgKnownFreq`/`g_scgKnownDrMap` regardless of how many times it gets swept) and continuously reasserts the new one, including if it happens to be a default channel (0-2), which `LMIC_setupChannel()` could never have restored.

**Same pattern applied to RX2 (closes part of §10.3):** `SCG_RX2_DR`/`SCG_RX2_FREQ` alias runtime variables the same way, a downlink command sets them, and `SCG_enforce()` gains two more unconditional per-tick assertions — `LMIC.dn2Dr`/`LMIC.dn2Freq` — plus `SCG_on_joined()` gains the same two lines (it currently has neither, per §8.4). This is the extension §11 item 6 already called for; it has not been implemented in any shipped version as of v1.3. `LMIC.txpow` is not addressed by this proposal — still open, no concrete downlink feature depends on it yet.

**Not yet resolved:** how to verify RX2 retargeting actually works in the field. Unlike channel retargeting (verifiable via `TXMODE, freq=` lines in serial debug, the same method that confirmed Threat 1.4), RX2 activity isn't a TX event and TTN only attempts one downlink window per uplink — normally RX1, with RX2 as the network server's fallback choice, not something the device can request. Forcing a downlink onto RX2 for a controlled test has no settled method yet; see the SmartGated field-diagnostics doc's proposed §3.9 for the open question.

# **9\. Sketch Migration Guide**

This section describes how to migrate an existing MCCI LMIC sketch to use the SingleChannelLMIC library. Three patterns are documented: ABP, OTAA, and the special case of sketches that intentionally vary the data rate (characterization sketches).

## **9.1 ABP Migration**

Step 1 — Add the include block immediately after the suppress-warning define:

\#define ARDUINO\_LMIC\_PROJECT\_CONFIG\_H\_SUPPRESS\_WARNING

// Set RX2 DR to match your gateway (TTN EU868 default \= DR\_SF9)  
\#define SCG\_RX2\_DR  DR\_SF9  
\#include \<single\_channel\_lmic.h\>

\#include \<lmic.h\>  
\#include \<hal/hal.h\>  
\#include \<SPI.h\>

Step 2 — In setup(), after LMIC\_reset() and the channel setup block, replace all manual enforcement with the DlChannelReq guard and SCG\_init():

// REMOVE these lines:  
// for (int i \= 1; i \<= 9; i++) LMIC\_disableChannel(i);  
// LMIC\_setLinkCheckMode(0);  
// LMIC\_setAdrMode(0);  
// LMIC.dn2Dr \= DR\_SF9;  
// LMIC\_setDrTxpow(DR\_SF7, 14);

// REPLACE WITH:  
\#if \!defined(DISABLE\_MCMD\_DlChannelReq)  
    for (uint8\_t i \= 0; i \< 9; i++) LMIC.channelDlFreq\[i\] \= 0;  
\#endif  
SCG\_init();

Step 3 — In loop(), add SCG\_enforce() before os\_runloop\_once():

void loop() {  
    SCG\_enforce();  
    os\_runloop\_once();  
}

## **9.2 OTAA Migration**

Steps 1 and 2 are identical to ABP. Additionally:

Step 3 — Add calls in onEvent():

void onEvent(ev\_t ev) {  
    switch (ev) {  
        case EV\_JOINED:  
            // ... print keys ...  
            SCG\_on\_joined();  
            break;  
        case EV\_JOIN\_TXCOMPLETE:  
            SCG\_on\_join\_txcomplete();  
            break;  
        // ...  
    }  
}

Step 4 — In loop():

void loop() {  
    SCG\_enforce();  
    os\_runloop\_once();  
}

## **9.3 Variable-DR (Characterization) Special Case**

Some sketches intentionally change the data rate per transmission as part of a network characterization experiment (e.g., cycling through SF7 to SF12 and measuring packet delivery rate at each setting). Using SCG\_enforce() in these sketches would override the test DR, defeating the purpose of the experiment.

For these sketches, use SCG\_init() in setup() (which sets the initial state), then immediately override the DR with the test configuration:

SCG\_init();  
// Override DR with the current test config — intentional for characterization  
updateLoRaParams(testParams\[currentParamIndex\]);

In loop(), use channel-only enforcement instead of SCG\_enforce():

void loop() {  
    // Channel-only guard: DR is intentionally varied per test config  
    if (LMIC.txChnl \!= 0\) {  
        LMIC.txChnl            \= 0;  
        LMIC.channelShuffleMap \= 0x0001;  
    }  
    os\_runloop\_once();  
}

This ensures the device always transmits on channel 0 (868.1 MHz) while allowing the data rate to be controlled by the characterization test logic.

# **10\. Known Limitations**

## **10.1 One-Tick State Window (revised in v1.1, updated in v1.2)**

The most significant limitation of the user-space approach is that MAC commands (LinkAdrReq, NewChannelReq, RXParamSetupReq) and join accept processing (CFList) are handled inside os\_runloop\_once(). The corrective calls (SCG\_enforce() and SCG\_on\_joined()) run on the next tick of loop() or the next event, respectively. This means there is a window in which the LMIC internal state is inconsistent with single-channel enforcement.

v1.0 asserted that this window never causes an actual wrong-channel transmission, because TX is always scheduled via os\_setTimedCallback on a future tick, giving the corrective call time to run first. **This was false for channelMap corruption, and was observed in the field (Threat 1.4).** The mechanism: the SCG\_enforce() shipped through v1.1 only inspected txChnl and datarate — it never re-checked channelMap. A MAC command like NewChannelReq could re-enable a disabled channel in channelMap on one tick, with no visible symptom (txChnl and datarate still correct), so SCG\_enforce() did nothing. The corruption only became visible when channelShuffleMap was next exhausted and reloaded from the corrupted channelMap — and that reload, the resulting bad channel pick, and the actual radio transmission could all happen inside a single os\_runloop\_once() call, potentially the same call that dispatches a TX already scheduled from a prior tick, leaving no intervening loop() iteration for SCG\_enforce() to act on. A live frame went out on 867.3 MHz as a result (see Threat 1.4 for the debug log).

The distinction that matters: the "corrective, next-tick" model can only protect state that SCG\_enforce() actually reads. For txChnl/datarate drift caused by ADR or join DR backoff, the original argument held — those are checked every tick and TX is genuinely delayed until after the check. For channelMap drift, no version of SCG\_enforce() through v1.1 ever monitored it, so the model didn't apply at all; it wasn't that the window was "one tick too long," it's that this corruption path was never covered by the correction loop.

**v1.2 status:** the fix is now to make SCG\_enforce() read the state it was missing, not to suppress the trigger. It sweeps channelMap unconditionally every tick (§8.6), which converts channelMap into state SCG\_enforce() actually protects, same as txChnl/datarate always were — the one-tick-window argument now applies to it too, and closes the same-tick race for every case tested so far (see Threat 1.4, second capture). An earlier attempt to remove the trigger at the MAC-command level (DISABLE\_MCMD\_NewChannelReq) was tried first and reverted — it broke MAC command parsing instead (Threat 1.4). The library-patch approach (Section 7\) remains the only option that eliminates the window structurally rather than by reading more state, for every threat in the catalog at once.

## **10.2 Struct Field Stability**

SCG\_enforce() directly assigns to LMIC.txChnl, LMIC.datarate, and LMIC.channelShuffleMap. These are internal fields of the MCCI LMIC library struct and are not part of the public API. A major MCCI refactor could rename or reorganize them. This would produce a clear compile-time error rather than silent wrong behavior. The fields have been stable across MCCI LMIC v3.x and v4.x, but forward compatibility is not guaranteed.

## **10.3 No Downlink Enforcement (revised in v1.1, updated in v1.2)**

The library enforces single-channel behavior for uplinks. As of v1.2, that coverage includes channelMap (§8.6), closing the gap that caused Threat 1.4. What remains unchecked is asserted once, at init/join time, and never re-checked afterward:

* **LMIC.txpow** is set in SCG\_init()/SCG\_on\_joined() but never re-checked; a LinkAdrReq that alters it via applyAdrRequests() is not corrected. No field-confirmed failure has been observed for this path — unlike channelMap, it has not yet been prioritized.  
* **LMIC.dn2Dr / LMIC.dn2Freq** (RX2 window) are set once in SCG\_init() and are not re-checked in SCG\_enforce() at all. v1.0 claimed SCG\_enforce() checked dn2Dr on every tick via a shared SCG\_apply() helper; that helper does not exist in the shipped header (§8.6 note), so this claim was incorrect even before the NewChannelReq finding. RXParamSetupReq can alter dn2Dr/dn2Freq at runtime with no correction, and — per §8.4 — SCG\_on\_joined() does not re-assert them after a join either.

Closing these two remaining gaps would require either the library-level patch (Section 7\) or extending SCG\_enforce() to also check txpow and dn2Dr/dn2Freq every tick, following the same pattern now used for channelMap. Neither is currently implemented. Given that channelMap was the one of these four fields with an actual field-confirmed exploit, and the fix pattern (sweep unconditionally, every tick) is now proven, extending it to txpow/dn2Dr/dn2Freq would be a small, low-risk follow-up (see §11, item 6).

**Status update, v1.3 (2026-09-10):** the dn2Dr/dn2Freq half of this gap is no longer purely theoretical — the SmartGated project's field-diagnostics doc has a proposed downlink command to change RX2 frequency/SF at runtime (its own §3.9), which cannot be trusted to persist without exactly the enforcement extension described here. See §8.8 for the proposed design. txpow remains unprioritized; no concrete feature currently depends on it.

**Why the dn2Dr gap hasn't shown up in practice:** the production OTAA sketch (lora\_uno\_compressed\_otaa, Appendix B) has not exhibited any RX2 problem despite this gap, and the reason is traceable in the source rather than assumed. LMIC.dn2Dr \= LMIC.frame\[OFF\_JA\_DLSET\] & 0x0F (lmic.c, Join Accept processing) runs unconditionally on every OTAA join, overwriting dn2Dr with whatever TTN's DLSettings byte specifies — before EV\_JOINED even fires, i.e. before SCG\_on\_joined() gets a chance to run at all. TTN's EU868 default RX2 data rate is DR\_SF9, which is also this project's configured SCG\_RX2\_DR default, so the value the network sets and the value the sketch wants happen to coincide. There is no enforcement making them agree — SCG\_on\_joined() does not reassert dn2Dr — they simply haven't diverged yet. LMIC.dn2Freq is not touched by Join Accept processing at all (only by an explicit RXParamSetupReq, which TTN does not appear to have sent to this device); same story, no observed divergence because nothing has tried to change it. This is the same shape of latent gap channelMap was before a NewChannelReq actually arrived: dormant until a network-side default changes, RXParamSetupReq gets sent, or SCG\_RX2\_DR is configured away from DR\_SF9 — at which point the same field-tested fix pattern (unconditional per-tick sweep in SCG\_enforce()) is the one to apply.

## **10.4 ABP LMIC.seqnoUp Persistence**

This limitation is specific to ABP mode and is not related to single-channel enforcement, but it affects the operational reliability of ABP sketches. The MCCI LMIC library does not persist the frame counter (LMIC.seqnoUp) across power cycles for ABP. TTN v3 rejects frames with a lower counter than the last received frame by default. This means every power cycle causes frame rejection until the counter exceeds the last TTN-seen value. The standard workaround is to store seqnoUp in EEPROM and restore it on boot, which is not implemented in the current sketch set.

## **10.5 US915 and Other Region Support**

The library is parameterized for multi-region use via SCG\_NUM\_CHANNELS and SCG\_RX2\_FREQ. However, US915 has a significantly different channel plan (72 uplink channels, 8 downlink channels, sub-band selection via LMIC\_selectSubBand). The channelShuffleMap-based enforcement may not be sufficient for US915 without additional changes. The library has not been tested outside the EU868 band.

# **11\. Conclusions and Recommendations**

This project has produced a systematic, source-level catalog of twelve MCCI LMIC behaviors that conflict with single-channel gateway operation, classified by severity from Critical (guaranteed frame loss) to Low (reduced downlink reliability). For each threat, the source location, trigger condition, observable consequence, and countermeasure have been documented.

Two solutions have been designed and argued: a formal library-side patch proposal centered on \#define LMIC\_SINGLE\_CHANNEL\_GATEWAY, and the SingleChannelLMIC user-space enforcement library. The user-space library is implemented, deployed across seven production sketches, and available as a standalone Arduino library in the project repository.

The SingleChannelLMIC approach is recommended for all research and educational deployments because it is library-update-safe, portable, and auditable. The one-tick state window is a theoretical concern that does not translate to actual frame loss in practice.

The formal LMIC\_SINGLE\_CHANNEL\_GATEWAY proposal is recommended as an upstream contribution to the MCCI project. If accepted, it would provide a zero-overhead, zero-boilerplate solution for all single-channel gateway users and would eliminate the remaining theoretical concerns.

The following actions are recommended as next steps:

1\. Measure and document actual packet delivery rates with and without the SingleChannelLMIC library on the research deployment (Arduino Mega \+ Dragino Shield → Raspberry Pi gateway → TTN v3) to provide empirical validation of the solution.

2\. Implement EEPROM-based seqnoUp persistence for ABP sketches to resolve the frame counter reset problem (Known Limitation 10.4).

3\. Prepare the LMIC\_SINGLE\_CHANNEL\_GATEWAY patch as a pull request to the MCCI LoRaWAN LMIC repository with this Technical Manual as supporting documentation.

4\. Extend the library validation to include a re-join scenario test (disable the gateway for 32 uplinks to verify that LINK\_CHECK\_DEAD does not trigger channel rotation).

5\. Test single-channel enforcement with US915 sub-band 2 to validate the SCG\_NUM\_CHANNELS parameterization for the American market.

6\. **(Added v1.1, closed v1.2.)** v1.1 called for re-capturing debug output after deploying DISABLE\_MCMD\_NewChannelReq. That capture showed a new failure instead (MAC command parsing broken, continuous downlink retries — see Threat 1.4), so the countermeasure was reverted and replaced with an unconditional channelMap sweep inside SCG\_enforce(). A second capture confirmed both the wrong-channel TX and the retry storm stopped in the same test run. Remaining follow-up: extend the same per-tick sweep pattern to LMIC.txpow and LMIC.dn2Dr/LMIC.dn2Freq (§10.3), which are still asserted once and never re-checked — no field-confirmed exploit yet, but the fix pattern is now proven and the remaining risk is the same class.

# **12\. Appendix A — Complete Threat Reference Table**

| ID | Name | File | Function | Trigger | Countermeasure | Severity |
| :---- | :---- | :---- | :---- | :---- | :---- | :---- |
| 1.1 | Channel rotation | lmic\_channelshuffle.c | LMIC\_findNextChannel() | Every TX | SCG\_init \+ SCG\_enforce | **Critical** |
| 1.2 | Join shuffle reset | lmic\_eu\_like.c | LMICeulike\_initJoinLoop() | OTAA join start | SCG\_on\_join\_txcomplete | **Critical** |
| 1.3 | LinkAdrReq channel re-add | lmic\_eu\_like.c | LMICeulike\_mapChannels() | Downlink w/ LinkAdrReq | SCG\_enforce (per tick) | **High** |
| 1.4 | NewChannelReq channel re-add (field-confirmed, mitigated v1.2) | lmic.c | MCMD_NewChannelReq handler | Downlink w/ NewChannelReq | SCG_enforce channelMap sweep (per tick) | **Critical** |
| 1.5 | Active-channel frequency rewrite (not yet field-confirmed, v1.3) | lmic.c | MCMD_NewChannelReq handler (targets SCG_CHANNEL itself) | Downlink w/ NewChannelReq naming the active index | Proposed: SCG_enforce channelFreq[SCG_CHANNEL] sweep (per tick, §8.8) | **Critical if triggered** |
| 2.1 | ADR DR change | lmic.c | applyAdrRequests() | Downlink w/ ADR commands | SCG\_init ADR=0 \+ SCG\_enforce | **High** |
| 2.2 | Join DR backoff | lmic\_eu\_like.c | LMICeulike\_nextJoinState() | Failed join retries | SCG\_enforce (per tick) | **Medium** |
| 3.1 | CFList injection | lmic\_eu\_like.c | processJoinAcceptCFList() | Successful OTAA join | SCG\_on\_joined | **Critical** |
| 3.2 | Join channel cycling | lmic\_eu\_like.c | LMICeulike\_nextJoinState() | EV\_JOIN\_TXCOMPLETE | SCG\_on\_join\_txcomplete | **Critical** |
| 4.1 | DlChannelReq redirect | lmic.c | MAC cmd handler | Downlink w/ DlChannelReq | channelDlFreq\[\] \= 0 (sketch) | **Medium** |
| 4.2 | Link check re-join | lmic.c | LINK\_CHECK\_DEAD handler | \~32 uplinks w/o downlink | LMIC\_setLinkCheckMode(0) | **High** |
| 5.1 | RX2 DR override (join) | lmic.c | Join Accept processing | Successful OTAA join | SCG\_init \+ SCG\_on\_joined | **Low** |
| 5.2 | RXParamSetupReq override | lmic.c | MAC cmd handler | Downlink w/ RXParamSetupReq | SCG\_init sets initial RX2 | **Low** |

# **13\. Appendix B — Sketch Inventory**

The following sketches have been produced or migrated as part of this project. All sketches in the "Raw Sensor" group were edited in-place; the OTAA and ABP SCG sketches are new files that leave the original custom sketches untouched.

| Sketch Folder | Activation | Sensors | Payload (bytes) | SCG Integration | Status |
| :---- | :---- | :---- | :---- | :---- | :---- |
| MCCI\_ttn\_otaa\_SCG | OTAA | None (test) | — | Full (init \+ on\_joined \+ on\_join\_txcomplete \+ enforce) | New file |
| MCCI\_ttn\_abp\_SCG | ABP | None (test) | — | Full (init \+ enforce) | New file |
| MCCI\_ttn\_Raw\_DHT22\_MQ2 | ABP | DHT22 \+ MQ2 | 8 | Full (init \+ enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DHT22\_MQ9 | ABP | DHT22 \+ MQ9 | 8 | Full (init \+ enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DHT22\_Dust | ABP | DHT22 \+ Dust | 6 | Full (init \+ enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DHT22\_MQ9\_Dust | ABP | DHT22+MQ9+Dust | 10 | Full (init \+ enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DHT22\_MQ2\_Dust | ABP | DHT22+MQ2+Dust | 10 | Full (init \+ enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DS18B20\_MQ9 | ABP | DS18B20 \+ MQ9 | 6 | Full (init \+ enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DS18B20\_MQ9\_Characterization | ABP | DS18B20+MQ9 | Variable | Channel-only (init \+ channel guard, no enforce) | Edited in-place |
| MCCI\_ttn\_Raw\_DHT22\_Dust\_FakeTest | ABP | Fake DHT22+Dust | 6 | Full (init \+ enforce \+ setRadioConfig) | New file |
| lora\_uno\_compressed | ABP | Stubbed / None | 6 | Full (init \+ enforce \+ setRadioConfig) | New file |
| lora\_uno\_compressed\_otaa | OTAA | Stubbed / None | 6 | Full (init \+ on\_joined \+ on\_join\_txcomplete \+ enforce \+ setRadioConfig) | New file |

