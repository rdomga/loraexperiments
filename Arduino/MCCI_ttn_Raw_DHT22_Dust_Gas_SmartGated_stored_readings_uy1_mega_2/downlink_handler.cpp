#include "downlink_handler.h"
#include <lmic.h>
#include "build_config.h"
#include "node_config.h"
#include "power_manager.h"
#include "battery_guard.h"
#include "radio_failsafe.h"
#include "sensor_manager.h"
#include "gas_sensor.h"
#include "mode_controller.h"
#include "energy_guard.h"

void applyDownlink(const DownlinkResult& r) {
    switch (r.action) {
        case DL_INTERVAL_CHANGED:
            TX_INTERVAL = (unsigned)r.value;
            // Accept-and-warn, not reject: with the default now 600, a
            // request below GAS_SPLIT_THRESHOLD_S in MODE_NORMAL is a
            // deliberate operator choice, not an accidental default — see
            // Gas_Scheduling_And_LowBattery_Handoff_2026-09-04.md §1. The
            // existing 30s syntax floor (DownlinkParser.h) is untouched;
            // this is a different, regime-aware question.
            if (g_mode == MODE_NORMAL && TX_INTERVAL < GAS_SPLIT_THRESHOLD_S) {
                Serial.println(F("Warning: interval below gas split threshold - gas rail will stay continuously powered (regime A)"));
            }
#ifdef WITH_EEPROM
            config_save_tx_interval(TX_INTERVAL);  // intentional — rare event
            Serial.println(F("EEPROM saved (interval change)"));
#endif
            Serial.println(F("Interval updated"));
            break;

        case DL_SLEEP:
            Serial.println(F("Sleep command received"));
            break;

        case DL_TX_NOW:
            // Do not schedule immediate send here; scheduling is centralized
            // in onEvent(EV_TXCOMPLETE) to avoid duplicate scheduling.
            break;

        case DL_PING:
            if (r.value == 1) g_pingConfirmedPending = true;
            // Do not schedule immediate ping here; onEvent will schedule it.
            break;

        case DL_SF_CHANGED:
        {
            // Only snapshot known-good if nothing's already pending — if a
            // second /@F#/@X#/@C# lands before the first one's verification
            // TX has resolved, the fallback on failure must still be the
            // last value actually proven to work, not the first change's
            // (also-unverified) candidate. All three snapshot together since
            // they resolve on the same single ping (see g_radioVerifyPending).
            if (!g_radioVerifyViaPing) {
                g_radioKnownGoodDR = SCG_currentDR;
                g_radioKnownGoodTXP = SCG_currentTXP;
                g_channelKnownGood = g_scgChannel;
            }
            uint8_t sf = (uint8_t)r.value;
            uint8_t newDR = sfToDr(sf);
            SCG_currentDR = newDR;
            SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
            // Rides Port 2 now, not Port 1 — see the g_radioVerifyPending
            // comment block above. Caller (real downlink: onEvent's
            // scheduling chain below; serial: handleSerialDownlinkResult())
            // is responsible for actually firing the confirmed ping.
            g_radioVerifyViaPing = true;
            g_pingConfirmedPending = true;
            Serial.print(F("SF -> SF"));
            Serial.print(sf);
            Serial.print(F(" (DR="));
            Serial.print(newDR);
            Serial.println(F(") - RAM only, pending confirmed-ping verification before EEPROM commit"));
            break;
        }

        case DL_TXPOWER_CHANGED:
            if (!g_radioVerifyViaPing) {
                g_radioKnownGoodDR = SCG_currentDR;
                g_radioKnownGoodTXP = SCG_currentTXP;
                g_channelKnownGood = g_scgChannel;
            }
            SCG_currentTXP = (uint8_t)r.value;
            SCG_setRadioConfig(SCG_CHANNEL, SCG_currentDR, SCG_currentTXP);
            g_radioVerifyViaPing = true;
            g_pingConfirmedPending = true;
            Serial.print(F("TXP -> "));
            Serial.print(r.value);
            Serial.println(F(" dBm - RAM only, pending confirmed-ping verification before EEPROM commit"));
            break;

        case DL_CHANNEL_CHANGED:
            // Range (0-8) already validated by the parser. SCG_setChannel()
            // does the actual retarget via direct struct writes — see
            // single_channel_lmic.h and Field_Diagnostics_And_Remote_
            // Configuration_2026-09-07.md §3.6 for why it's not routed
            // through LMIC_setupChannel(). Added 2026-09-10: now rides the
            // same §3.8 verify-via-confirmed-ping mechanism as SF/TXP
            // instead of applying unverified — a retarget onto a channel
            // the gateway doesn't listen on would otherwise strand the node
            // until the (much slower) §4 dead-man failsafe catches it.
            if (!g_radioVerifyViaPing) {
                g_radioKnownGoodDR = SCG_currentDR;
                g_radioKnownGoodTXP = SCG_currentTXP;
                g_channelKnownGood = g_scgChannel;
            }
            SCG_setChannel((uint8_t)r.value);
            g_radioVerifyViaPing = true;
            g_pingConfirmedPending = true;
            Serial.print(F("Channel -> "));
            Serial.print(r.value);
            Serial.println(F(" - RAM only, pending confirmed-ping verification before EEPROM commit"));
            break;

        case DL_RESTART:
            softwareReset();
            break;

        case DL_FACTORY_RESET:
            factoryReset();
            break;

        case DL_EEPROM_SAVE:
#ifdef WITH_EEPROM
            // Sync the in-RAM struct with LMIC's live counter first — this can
            // be requested at any point, not just right after an EV_TXCOMPLETE
            // where my_eeprom.seqnoUp is normally kept current.
            config_put_seqnoUp(LMIC.seqnoUp);
            config_save_all();
            eeprom_dirty = false;
            eeprom_write_countdown = 0;
            Serial.println(F("EEPROM saved (explicit request)"));
#else
            Serial.println(F("EEPROM save requested but WITH_EEPROM is disabled"));
#endif
            break;

        case DL_MODE_CHANGED:
            setOperatingMode((uint8_t)r.value);
            break;

        case DL_WARMUP_CHANGED:
            gas_setWarmupSeconds(r.value);
            Serial.print(F("Gas warmup -> "));
            Serial.print(r.value);
            Serial.println(F(" s"));
            // Regime B only: warmup segment = warmup + GAS_WARMUP_MARGIN_S
            // eats into TX_INTERVAL's off-sleep. At/above TX_INTERVAL, the
            // off-sleep floors at 0 (see sleepThenSend()) and the cycle
            // simply takes as long as the warmup segment — longer than the
            // TX_INTERVAL the operator configured, not a crash, but worth
            // flagging since it's a silent cadence change otherwise.
            if (g_mode == MODE_NORMAL && TX_INTERVAL >= GAS_SPLIT_THRESHOLD_S &&
                gas_getWarmupSeconds() >= TX_INTERVAL) {
                Serial.println(F("Warning: warmup segment now >= TX_INTERVAL - regime B off-sleep will floor at 0, cycle time will stretch to the warmup segment length"));
            }
            break;

        case DL_PING_RATIO_CHANGED:
            g_pingRatio = (uint8_t)r.value;
            g_uplinkCountSincePing = 0; // restart the count under the new ratio, not mid-way through the old one
            Serial.print(F("Ping ratio -> every "));
            Serial.print(g_pingRatio);
            Serial.println(F(" uplinks"));
            break;

        case DL_SENSOR_MASK_CHANGED:
            g_sensorMask = (uint8_t)r.value;
            Serial.print(F("Sensor mask -> 0x"));
            Serial.println(g_sensorMask, HEX);
            break;

        case DL_BURST_START:
            if (!energyGuard_allowBurst(g_batt, g_batt_valid)) {
                Serial.println(F("Burst rejected - energy sag active or battery below sag exit"));
                break;
            }
            // Same counter the boot burst already drives — see §2 of the
            // handoff doc: one mechanism, two trigger sites, so burst mode
            // gets sleepThenSend()'s existing burst-tier handling (including
            // its gas ensure-on behavior) for free. r.value==0 means a bare
            // /@B# (parser didn't see a count/interval) — fall back to the
            // compiled-in defaults; otherwise r.value/r.value2 are already
            // range-checked by the parser (count 1-250, interval 30-3600s).
            if (r.value == 0) {
                bootBurstFramesLeft = BOOT_BURST_FRAME_COUNT;
                g_burstIntervalS = BOOT_BURST_INTERVAL_S;
                Serial.println(F("Burst mode armed (default 9 frames @ 90s)"));
            } else {
                bootBurstFramesLeft = (uint8_t)r.value;
                g_burstIntervalS = r.value2;
                Serial.print(F("Burst mode armed: "));
                Serial.print(bootBurstFramesLeft);
                Serial.print(F(" frames @ "));
                Serial.print(g_burstIntervalS);
                Serial.println(F("s"));
            }
            break;

        case DL_BATTERY_THRESHOLDS_CHANGED:
            // Ordering/range already validated by the parser (DownlinkParser.h
            // 'V' case) — applyDownlink() trusts what it's handed, same as
            // every other DL_* case here.
            g_battLow          = (int)r.value;
            g_battRecover      = (int)r.value2;
            g_battCritical     = (int)r.value3;
            g_battPsave        = (int)r.value4;
            g_battPsaveRecover = (int)r.value5;
            Serial.println(F("Battery thresholds updated"));
            break;

        case DL_SAG_THRESHOLDS_CHANGED:
        case DL_SAG_INTERVAL_CHANGED:
        case DL_SAG_OVERRIDE:
        case DL_BUFFER_RESEND:
        case DL_BUFFER_DISCARD:
            energyGuard_applyDownlink(r);   // only ever parsed in STORAGE builds; a no-op elsewhere
            break;

        case DL_FLUSH_SPACING_CHANGED:
            g_flushSpacingS = (unsigned)r.value;
            Serial.print(F("Flush spacing -> "));
            Serial.print(g_flushSpacingS);
            Serial.println(F(" s"));
            break;

        case DL_COMMIT_PARAMS:
            energyGuard_commit();           // no-op unless the STORAGE strategy is built in
#ifdef WITH_EEPROM
            // ALWAYS_ON is deliberately never committed — persisting it would
            // reintroduce exactly the brownout-loop risk g_mode being RAM-only
            // was designed to prevent (project_smartgated_firmware_2026-09-01):
            // a solar dip resets the board, ALWAYS_ON reloads from EEPROM,
            // full-power draw brownouts again before the next TX. NORMAL/
            // NORMAL_NO_GAS carry no such risk and are exactly what §3.5's own
            // physical-failure scenario (a shorted gas rail surviving a
            // reboot) needs persisted.
            config_commit_profile(g_mode, g_sensorMask, (uint16_t)gas_getWarmupSeconds(), g_pingRatio,
                                   (uint16_t)g_battLow, (uint16_t)g_battRecover, (uint16_t)g_battCritical,
                                   (uint16_t)g_battPsave, (uint16_t)g_battPsaveRecover);  // intentional — rare event, same pattern as DL_INTERVAL_CHANGED
            Serial.println(F("Parameters committed to EEPROM (mode/mask/warmup/ping-ratio/battery-thresholds)"));
            if (g_mode == MODE_ALWAYS_ON) {
                Serial.println(F("Note: ALWAYS_ON is never persisted (brownout-loop guard) - committed mode saved as NORMAL"));
            }
#else
            Serial.println(F("Commit requested but WITH_EEPROM is disabled"));
#endif
            break;

        default:
            break;
    }
}
