#include "node_config.h"
#include "energy_guard.h"
#include "reading_store.h"
#include <EEPROM.h>
#include "power_manager.h"
#include "battery_sensor.h"
#include "gas_sensor.h"

lmic_eeprom_t my_eeprom;
uint8_t g_resetReasonByte = 0;
bool eeprom_dirty = false;
uint8_t eeprom_write_countdown = 0;
static uint8_t noinitBootMarker __attribute__((section(".noinit")));

static bool     s_ckReady = false;
static uint16_t s_ckSeq   = 0;
static uint8_t  s_ckSlot  = 0;
static uint8_t  s_ckState = HANG_CHECKPOINT_IDLE;

static void checkpoint_load() {
    uint16_t bestSeq = 0;
    int16_t  bestSlot = -1;
    uint8_t  bestState = HANG_CHECKPOINT_IDLE;
    for (uint8_t i = 0; i < CHECKPOINT_SLOTS; i++) {
        uint16_t a = (uint16_t)(EEPROM_CHECKPOINT_BASE + (uint16_t)i * CHECKPOINT_REC_BYTES);
        uint8_t lo  = EEPROM.read(a);
        uint8_t hi  = EEPROM.read(a + 1);
        uint8_t st  = EEPROM.read(a + 2);
        uint8_t inv = EEPROM.read(a + 3);
        if ((uint8_t)(~st) != inv) continue;             // unwritten / torn record
        uint16_t seq = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        if (bestSlot < 0 || (int16_t)(seq - bestSeq) > 0) {
            bestSeq = seq;
            bestSlot = (int16_t)i;
            bestState = st;
        }
    }
    if (bestSlot < 0) {
        s_ckSeq = 0;
        s_ckSlot = 0;
        s_ckState = HANG_CHECKPOINT_IDLE;
    } else {
        s_ckSeq = bestSeq;
        s_ckSlot = (uint8_t)((bestSlot + 1) % CHECKPOINT_SLOTS);
        s_ckState = bestState;
    }
    s_ckReady = true;
}

void config_set_checkpoint(uint8_t state) {
    if (!s_ckReady) checkpoint_load();
    s_ckSeq++;
    uint16_t a = (uint16_t)(EEPROM_CHECKPOINT_BASE + (uint16_t)s_ckSlot * CHECKPOINT_REC_BYTES);
    EEPROM.update(a,     (uint8_t)(s_ckSeq & 0xFF));
    EEPROM.update(a + 1, (uint8_t)(s_ckSeq >> 8));
    EEPROM.update(a + 2, state);
    EEPROM.update(a + 3, (uint8_t)~state);
    s_ckSlot = (uint8_t)((s_ckSlot + 1) % CHECKPOINT_SLOTS);
    s_ckState = state;
}

uint8_t config_read_checkpoint() {
    if (!s_ckReady) checkpoint_load();
    return s_ckState;
}

void config_put_seqnoUp(uint32_t seqno) {
    my_eeprom.seqnoUp = seqno;
    // Write high-byte-first (MSB down to LSB) to prevent backwards counter tears on carry
    uint16_t addr = offsetof(lmic_eeprom_t, seqnoUp);
    for (int8_t i = (int8_t)(sizeof(uint32_t) - 1); i >= 0; i--) {
        uint8_t b = (uint8_t)((seqno >> (i * 8)) & 0xFF);
        EEPROM.update(addr + i, b);
    }
}

void config_mark_intentional_reset() {
    noinitBootMarker = BOOT_MARKER_INTENTIONAL_RESET;
}

bool config_evaluate_reset_cause(uint8_t mcusr_boot, uint8_t& outResetReasonByte) {
    bool intentionalReset = (noinitBootMarker == BOOT_MARKER_INTENTIONAL_RESET);
    bool incompleteCycle = (config_read_checkpoint() == HANG_CHECKPOINT_BUSY) && !intentionalReset;
    noinitBootMarker = 0;
    outResetReasonByte = mcusr_boot | (incompleteCycle ? RESET_FLAG_INCOMPLETE_CYCLE : 0);
    return incompleteCycle;
}

void config_init_defaults(unsigned defaultInterval) {
    my_eeprom.flag1                   = 0x12;
    my_eeprom.flag2                   = 0x35;
    my_eeprom.seqnoUp                 = 0;
    my_eeprom.tx_interval             = defaultInterval;
    my_eeprom.tx_interval_saved       = 0;
    my_eeprom.profile_mode             = MODE_NORMAL;
    my_eeprom.profile_sensorMask       = 0x0F;
    my_eeprom.profile_gasWarmupS       = GAS_WARMUP_S_DEFAULT;
    my_eeprom.profile_pingRatio        = 10;
    my_eeprom.profile_battLow          = BATTERY_LOW_RAW_DEFAULT;
    my_eeprom.profile_battRecover      = BATTERY_RECOVER_RAW_DEFAULT;
    my_eeprom.profile_battCritical     = BATTERY_CRITICAL_RAW_DEFAULT;
    my_eeprom.profile_battPsave        = BATTERY_POWERSAVE_RAW_DEFAULT;
    my_eeprom.profile_battPsaveRecover = BATTERY_POWERSAVE_RECOVER_RAW_DEFAULT;
    my_eeprom.profile_saved            = 0;
    my_eeprom.radio_sf                 = 3; // DR_SF9
    my_eeprom.radio_txp                = 14;
    my_eeprom.radio_saved              = 0;
    my_eeprom.deadman_fail_count       = 0;
    my_eeprom.radio_channel            = 0;
    my_eeprom.outage_fallback_active   = 0;
    config_save_all();
}

bool config_load_from_eeprom() {
    EEPROM.get(0, my_eeprom);
    if (my_eeprom.flag1 == 0x12 && my_eeprom.flag2 == 0x35) {
        if (my_eeprom.outage_fallback_active != 1) {
            my_eeprom.outage_fallback_active = 0;
        }
        return true;
    }
    return false;
}

void config_save_all() {
    EEPROM.put(0, my_eeprom);
}

void config_save_tx_interval(unsigned interval) {
    my_eeprom.tx_interval = interval;
    my_eeprom.tx_interval_saved = 1;
    config_save_all();
}

void config_commit_profile(uint8_t mode, uint8_t sensorMask, uint16_t warmupS, uint8_t pingRatio,
                           uint16_t battLow, uint16_t battRecover, uint16_t battCrit,
                           uint16_t battPsave, uint16_t battPsaveRec) {
    // Mode is never committed as MODE_ALWAYS_ON for safety
    my_eeprom.profile_mode             = (mode <= MODE_NORMAL_NO_GAS) ? mode : MODE_NORMAL;
    my_eeprom.profile_sensorMask       = sensorMask;
    my_eeprom.profile_gasWarmupS       = warmupS;
    my_eeprom.profile_pingRatio        = pingRatio;
    my_eeprom.profile_battLow          = battLow;
    my_eeprom.profile_battRecover      = battRecover;
    my_eeprom.profile_battCritical     = battCrit;
    my_eeprom.profile_battPsave        = battPsave;
    my_eeprom.profile_battPsaveRecover = battPsaveRec;
    my_eeprom.profile_saved            = 1;
    config_save_all();
}

void config_commit_radio(uint8_t dr, uint8_t txp, uint8_t channel) {
    my_eeprom.radio_sf            = dr;
    my_eeprom.radio_txp           = txp;
    my_eeprom.radio_channel       = channel;
    my_eeprom.radio_saved         = 1;
    my_eeprom.deadman_fail_count  = 0;
    config_save_all();
}

void config_set_deadman_fail_count(uint8_t count) {
    if (my_eeprom.deadman_fail_count != count) {
        my_eeprom.deadman_fail_count = count;
        config_save_all();
    }
}

void config_set_outage_fallback_active(uint8_t active) {
    uint8_t val = active ? 1 : 0;
    if (my_eeprom.outage_fallback_active != val) {
        my_eeprom.outage_fallback_active = val;
        EEPROM.update(offsetof(lmic_eeprom_t, outage_fallback_active), val);
    }
}

void config_clear_failsafe(unsigned defaultInterval) {
    my_eeprom.profile_saved       = 0;
    my_eeprom.radio_saved         = 0;
    my_eeprom.deadman_fail_count  = 0;
    config_save_all();
}

void config_factory_reset(unsigned defaultInterval) {
    config_init_defaults(defaultInterval);
}

void softwareReset() {
#ifdef WITH_EEPROM
    if (eeprom_dirty) {
        config_save_all();
        Serial.println(F("EEPROM flushed before reset"));
    }
#endif
    triggerSoftwareReset();
}

void factoryReset() {
    Serial.println(F("Factory reset..."));
#ifdef WITH_EEPROM
    config_factory_reset(DEFAULT_TX_INTERVAL);
    energyGuard_factoryReset();   // energy sag parameters and reading ring
    config_set_checkpoint(HANG_CHECKPOINT_IDLE);
    Serial.println(F("EEPROM cleared"));
    eeprom_dirty = false;
#endif
    softwareReset();
}
