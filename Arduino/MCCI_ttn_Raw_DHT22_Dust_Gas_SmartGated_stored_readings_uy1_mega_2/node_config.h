#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

#include <Arduino.h>
#include <EEPROM.h>
#include "build_config.h"
#include "payload_encoder.h"

#define EEPROM_HANG_CHECKPOINT_ADDR 32   // legacy single-byte slot, now unused
#define HANG_CHECKPOINT_IDLE        0x00
#define HANG_CHECKPOINT_BUSY        0xBB

// Active-work checkpoint records rotate through the spare EEPROM tail
// (3904..4095) so each byte sees ~2 writes/day instead of 96. Record:
// [seq lo][seq hi][state][~state]. The complement rejects uninitialised
// 0xFF bytes; the sequence picks the newest record after a wrap.
#define EEPROM_CHECKPOINT_BASE      3904
#define CHECKPOINT_SLOTS            48
#define CHECKPOINT_REC_BYTES        4
static_assert(EEPROM_CHECKPOINT_BASE + CHECKPOINT_SLOTS * CHECKPOINT_REC_BYTES <= 4096,
              "checkpoint rotation must fit in the EEPROM tail");

#define BOOT_MARKER_INTENTIONAL_RESET 0xA5
#define EEPROM_WRITE_INTERVAL         20

struct lmic_eeprom_t {
    uint8_t  flag1;
    uint8_t  flag2;
    uint32_t seqnoUp;
    unsigned tx_interval;
    uint8_t  tx_interval_saved;
    uint8_t  profile_mode;
    uint8_t  profile_sensorMask;
    uint16_t profile_gasWarmupS;
    uint8_t  profile_pingRatio;
    uint16_t profile_battLow;
    uint16_t profile_battRecover;
    uint16_t profile_battCritical;
    uint16_t profile_battPsave;
    uint16_t profile_battPsaveRecover;
    uint8_t  profile_saved;
    uint8_t  radio_sf;
    uint8_t  radio_txp;
    uint8_t  radio_saved;
    uint8_t  deadman_fail_count;
    uint8_t  radio_channel;
    uint8_t  outage_fallback_active;
};
static_assert(sizeof(lmic_eeprom_t) <= EEPROM_HANG_CHECKPOINT_ADDR, "lmic_eeprom_t overflow into hang checkpoint");

extern lmic_eeprom_t my_eeprom;
extern uint8_t g_resetReasonByte;
extern bool eeprom_dirty;
extern uint8_t eeprom_write_countdown;

// Active-work window checkpointing
void config_set_checkpoint(uint8_t state);
uint8_t config_read_checkpoint();
void config_put_seqnoUp(uint32_t seqno);
void config_mark_intentional_reset();
bool config_evaluate_reset_cause(uint8_t mcusr_boot, uint8_t& outResetReasonByte);

// Configuration management
void config_init_defaults(unsigned defaultInterval);
bool config_load_from_eeprom();
void config_save_all();

void config_save_tx_interval(unsigned interval);

void config_commit_profile(uint8_t mode, uint8_t sensorMask, uint16_t warmupS, uint8_t pingRatio,
                           uint16_t battLow, uint16_t battRecover, uint16_t battCrit,
                           uint16_t battPsave, uint16_t battPsaveRec);

void config_commit_radio(uint8_t dr, uint8_t txp, uint8_t channel);
void config_set_deadman_fail_count(uint8_t count);
void config_set_outage_fallback_active(uint8_t active);
void config_clear_failsafe(unsigned defaultInterval);
void config_factory_reset(unsigned defaultInterval);

void softwareReset();
void factoryReset();

#endif // NODE_CONFIG_H
