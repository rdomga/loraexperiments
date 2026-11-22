// project-specific definitions
#define CFG_eu868 1
// #define CFG_us915 1
// #define CFG_au915 1
// #define CFG_as923 1
//  #define LMIC_COUNTRY_CODE LMIC_COUNTRY_CODE_JP      /* for as923-JP; also
//  define CFG_as923 */
// #define CFG_kr920 1
// #define CFG_in866 1
#define CFG_sx1276_radio 1
// #define CFG_sx1261_radio 1
// #define CFG_sx1262_radio 1
// #define ARDUINO_heltec_wifi_lora_32_V3
// #define LMIC_USE_INTERRUPTS

// Optimize memory by stripping unused Class B features (always safe for Class A sensors)
#define DISABLE_PING
#define DISABLE_BEACONS

// Tried DISABLE_MCMD_NewChannelReq here to stop TTN's NewChannelReq from
// re-enabling channel 4 in LMIC.channelMap (confirmed in the field: caused
// a live TX on 867.3 MHz). Reverted — macCmdSize[] in lmic.c still lists
// NewChannelReq as a 6-byte command regardless of this define, so the FOpts
// parser correctly walks up to it, finds no case for it in the switch, and
// aborts the whole mac-command scan (lmic.c default: case, "olen = oidx").
// That drops the NewChannelAns response entirely, so TTN never sees an ack
// and keeps re-sending the same NewChannelReq on every downlink forever.
// Fix moved to SCG_enforce() instead (single_channel_lmic.h) — it now
// unconditionally re-disables non-SCG_CHANNEL channels every tick, so the
// device answers NewChannelReq normally (stopping the retry storm) while
// channelMap gets swept clean again before the next TX.
// #define DISABLE_MCMD_NewChannelReq

// Debug output (RF settings on TX/RX) — LMIC_PRINTF_TO is required, not just
// LMIC_DEBUG_LEVEL: without it, hal_init() never binds stdout to Serial and
// the printf() calls the debug level gates go nowhere.
// Disabled 2026-07-28: this shared config applies to every sketch using this
// LMIC copy, including tight-RAM Uno builds (lora_uno_compressed_otaa was
// already over both flash and RAM budgets before this change, independent of
// it). LMIC's own internal debug printf machinery was one of the largest
// remaining levers on that sketch. Every actively-maintained sketch in this
// repo was recompiled after this change to confirm nothing else broke.
// Re-enabled 2026-09-09, kept on deliberately (not reverted after the
// §3.8 bench test that motivated it) — user's call: the Uno-class sketches
// this was disabled for (2026-07-28, see above) are no longer this
// project's bench-test target, current firmware (SmartGated and beyond)
// has outgrown that budget anyway. Needed to see LMIC's internal
// retry/DR-stepdown behavior on TXCONF_ATTEMPTS, which isn't visible from
// application-level Serial prints since LMIC.txCnt is already reset by
// the time EV_TXCOMPLETE fires. If an Uno-class sketch in this repo is
// ever compiled again, recheck its budget against this — it will likely
// need this un-defined again for that one build.
// Board-specific override (2026-09-12): Arduino IDE compiles libraries in isolation,
// so sketches cannot override LMIC_DEBUG_LEVEL from .ino. However, the toolchain
// passes board target defines globally. For Uno (ATmega328P), debug level is 0 and
// LMIC_PRINTF_TO is stripped to save ~3.4KB Flash and ~720B RAM. Mega/ESP32 retain level 1.
#if defined(ARDUINO_AVR_UNO) || defined(__AVR_ATmega328P__)
#define LMIC_DEBUG_LEVEL 0
#else
#define LMIC_DEBUG_LEVEL 1
#define LMIC_PRINTF_TO Serial
#endif