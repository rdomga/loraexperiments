Firmware for uy1-mega-2: stored-readings timestamp implementation (Rounds 5-8),
plus the bench WDT keepalive and the two-attempt init ACK retry gate.
Source of truth for this generation; the mega-1 bench folder is derived from it.

Identity
  credentials.h ............ LoRaWAN session keys (DevAddr, NwkSKey, AppSKey)
  node_calibration.h ....... NODE_NAME "uy1-mega-2", sleep factor 1147 permille

Build settings
  LOW_POWER ................ ON
  BENCH_VARIANT ............ OFF
  BATTERY_STRATEGY ......... STORAGE
  TX power ................. 4 dBm  (radio_failsafe.cpp, non-bench default)
  TX interval .............. 3600 s (DEFAULT_TX_INTERVAL)
  Fallbacks (deliberate, not the operating values): dead-man rollback TXP 14 dBm,
  radio-only rollback 2 dBm.

First flash needs a clean EEPROM
  With WITH_EEPROM, a committed radio/TX_INTERVAL profile from the old firmware
  is restored at boot and would override the 4 dBm / 3600 s defaults. Flash
  EEPROM_Full_Clear first, then this sketch. Starting fresh is expected; no
  stored readings need to survive.

Flash (arduino-cli)
  arduino-cli compile --fqbn arduino:avr:mega --upload -p <port> \
    Arduino/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_stored_readings_uy1_mega_2

Verify after flashing
  Boot log: "TX_INTERVAL: 3600s", no "BENCH VARIANT" line, "Node uy1-mega-2",
  "WDT scale: node value 1147 permille (calibrated, no boot measurement)".
  /@P# over USB: "Node: uy1-mega-2", "SF: SF9  TXP: 4 dBm", "Sleep scale: ...".

TTN Payload Formatter
  The TTN application payload formatter must decode frame versions 0x04-0x07
  (see payload.txt, SHA-256 3426a4dd...618f).

Network clock: every confirmed ping requests the network time; the first anchor
after boot is the reference, and the next anchor 30 minutes or more later
computes the factor that replaces the node value. Needs MAC 1.0.3 on TTN. With
no answer the node keeps the node_calibration.h value.
