////////////////////////////////////////////////////////////////
// Downlink Parser for DHT22 + Dust + Gas SmartGated sketch
//
// Supported commands (ASCII), reachable both from a TTN downlink and from
// the USB serial console (see handleSerialCommands() in the main .ino) —
// same parser, same command set, either transport:
//   /@I<secs>#     — change TX interval (min 30 s)
//   /@S<minutes>#  — sleep (go silent) for N minutes
//   /@T#           — request an immediate transmission
//   /@P#           — downlink: ping/status reply on port 2.
//                    serial: verbose local state dump, no radio TX.
//   /@P1#          — confirmed variant: forces a real Port 2 ping TX,
//                    flagged as a LoRaWAN confirmed uplink, on BOTH
//                    transports (unlike bare /@P#, this radios out even
//                    over serial — the point is testing whether the
//                    network server ACKs it). What to do if no ACK comes
//                    back isn't handled specially yet — same as any other
//                    send that doesn't get through.
//   /@E#           — force an immediate EEPROM save (normally batched)
//   /@R#           — software restart (watchdog reset)
//   /@Z#           — factory reset (clear EEPROM, reset counters, restart)
//   /@M<0-2>#      — switch operating mode: 0=NORMAL, 1=NORMAL_NO_GAS,
//                    2=ALWAYS_ON. RAM-only unless followed by /@CP# (§3.5):
//                    a bare /@M reverts to the last *committed* mode (or
//                    NORMAL if none) on any reset. ALWAYS_ON is never
//                    committed — /@CP# silently saves NORMAL instead, for
//                    brownout-loop safety. See OperatingMode in the .ino.
//                    Rejected outright (not deferred) while lowBatteryMode
//                    or powerSaveMode is active — see setOperatingMode().
//   /@B#           — start a 9-frame/90s burst (same mechanism as the boot
//                    burst — see BOOT_BURST_FRAME_COUNT in the .ino).
//   /@B<count>,<interval_s>#  — start a burst with a custom frame count
//                    (1-250) and spacing (30-3600 s). Interval floor is the
//                    syntax minimum, not a safety guarantee: spacing below
//                    the default 90s is known to have caused mid-burst
//                    brownouts on a solar-only rail at night (see the
//                    BOOT_BURST_INTERVAL_S comment in the .ino) — the
//                    parser accepts it and logs a warning rather than
//                    rejecting it, since it's legitimate on USB power for a
//                    bench link-margin survey. RAM only, one-shot.
//   /@K<hex_mask>#  — peripheral keep/kill mask (RAM only, not yet EEPROM-
//                    persistable — see Field_Diagnostics_And_Remote_
//                    Configuration_2026-09-07.md §3.2). Bit0=DHT22,
//                    Bit1=dust, Bit2=gas, Bit3=battery ADC. Default 0x0F.
//   /@W<warmup_s>#  — gas sensor warmup threshold (10-3600 s, default 300).
//                    RAM only. Backs both gas_getADC()'s own validity check
//                    and regime B's off-sleep scheduling from the same
//                    value (gas_sensor.h). See Field_Diagnostics_And_
//                    Remote_Configuration_2026-09-07.md §3.1.
//   /@N<count>#     — autonomous Port 2 status-frame ratio: send one every
//                    <count> successful Port 1 uplinks (1-250, default 10).
//                    RAM only unless committed (see /@CP# below). See
//                    Field_Diagnostics_And_Remote_Configuration_2026-09-07.md
//                    §3.7.
//   /@C<channel_index>#  — retarget the single-channel lock to a new EU868
//                    channel index (0-8). Applies to RAM immediately, then
//                    (2026-09-10) rides the same §3.8 confirmed-ping verify
//                    as /@F#/@X#: ACK -> committed to EEPROM (radio_channel,
//                    under the shared radio_saved flag), no ACK after
//                    retries -> reverted to the last known-good channel in
//                    RAM. Retargets via direct struct writes inside
//                    SCG_setChannel() (single_channel_lmic.h) — never
//                    LMIC_setupChannel(), which silently refuses EU868's
//                    three default indices (0-2). See Field_Diagnostics_
//                    And_Remote_Configuration_2026-09-07.md §3.6/§3.8.
//   /@CP#           — commit the current mode/sensor-mask/gas-warmup/ping-
//                    ratio/battery-thresholds to EEPROM as the boot profile
//                    (Commit Parameters). Restored on every boot from here
//                    on, until the next /@CP# or a /@Z# factory reset clears
//                    it. ALWAYS_ON is never committed — it's saved as NORMAL
//                    instead, since persisting it would reintroduce the
//                    brownout-loop risk g_mode being RAM-only was designed
//                    to avoid in the first place. Burst count/interval stay
//                    RAM-only/one-shot by design — not part of this command.
//                    See Field_Diagnostics_And_Remote_Configuration_
//                    2026-09-07.md §3.5.
//   /@V<low>,<rec>,<crit>,<psave>,<psaveRec>#  — battery guard thresholds
//                    (raw ADC, 6-439). Must satisfy CRITICAL < LOW <
//                    POWERSAVE < RECOVER < POWERSAVE_RECOVER — see the
//                    ordering comment on the 'V' case for why each direction
//                    is load-bearing, not cosmetic. RAM only unless
//                    committed via /@CP#. See Field_Diagnostics_And_Remote_
//                    Configuration_2026-09-07.md §3.3.
//
// STORAGE battery strategy only (BATTERY_STRATEGY in build_config.h; other builds answer "Unknown command"):
//   /@H<enter>,<exit>#  — energy-sag thresholds (raw ADC, 6-439): store below <enter>, resume at or above <exit> on two
//                    consecutive reads. exit must be at least 4 counts above enter. RAM only unless committed via /@CP#.
//   /@L<seconds>#   — reading interval while storing (30-14400 s, default 3600). Never shorter than the normal interval.
//                    RAM only unless committed via /@CP#.
//   /@O<0|1|2>#     — operator override, RAM only: 0 = force sag OFF, 1 = force sag ON, 2 = automatic (default). The
//                    CRITICAL floor still applies. A reset returns to automatic.
//   /@U<seq>#       — re-send: mark every saved record from sequence number <seq> on as unsent again; they go out after the
//                    next port 1 uplink while sag mode is off.
////////////////////////////////////////////////////////////////

#ifndef DOWNLINK_PARSER_H
#define DOWNLINK_PARSER_H

#include <Arduino.h>
#include <stdint.h>

enum DownlinkAction : uint8_t {
    DL_NONE = 0,
    DL_INTERVAL_CHANGED,
    DL_SLEEP,
    DL_TX_NOW,
    DL_PING,
    DL_RESTART,
    DL_FACTORY_RESET,
    DL_SF_CHANGED,
    DL_TXPOWER_CHANGED,
    DL_EEPROM_SAVE,
    DL_MODE_CHANGED,
    DL_BURST_START,
    DL_SENSOR_MASK_CHANGED,
    DL_WARMUP_CHANGED,
    DL_PING_RATIO_CHANGED,
    DL_COMMIT_PARAMS,
    DL_BATTERY_THRESHOLDS_CHANGED,
    DL_CHANNEL_CHANGED,
    DL_SAG_THRESHOLDS_CHANGED,   // /@H<enter>,<exit>#   (value = enter, value2 = exit)    STORAGE strategy only
    DL_SAG_INTERVAL_CHANGED,     // /@L<seconds>#
    DL_SAG_OVERRIDE,             // /@O<0|1|2>#          0 = forced off, 1 = forced on, 2 = automatic
    DL_BUFFER_RESEND,            // /@U<seq>#            mark records from seq on as unsent again
    DL_BUFFER_DISCARD,           // /@D#                 mark all unsent records as sent without transmitting
    DL_FLUSH_SPACING_CHANGED     // /@G<seconds>#        gap between Port 3 batches in a drain
};

struct DownlinkResult {
    DownlinkAction action;
    unsigned long  value;   // new interval (s), sleep duration (min), sensor
                             // mask, burst frame count, or battery LOW
    unsigned long  value2;  // burst interval_s, or battery RECOVER; 0 if unused
    unsigned long  value3;  // battery CRITICAL; 0 if unused
    unsigned long  value4;  // battery POWERSAVE; 0 if unused
    unsigned long  value5;  // battery POWERSAVE_RECOVER; 0 if unused
};


DownlinkResult parseDownlinkCommand(uint8_t* message, uint8_t len);

#endif // DOWNLINK_PARSER_H
