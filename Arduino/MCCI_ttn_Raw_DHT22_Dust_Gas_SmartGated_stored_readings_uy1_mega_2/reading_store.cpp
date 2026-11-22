#include "reading_store.h"
#include <EEPROM.h>
#include <avr/wdt.h>
#include "power_manager.h"

#define SLOT_ADDR(slot)   (READING_STORE_ADDR + (uint16_t)(slot) * sizeof(ReadingRec))
#define SEQ_ADDR(slot)    (SLOT_ADDR(slot) + offsetof(ReadingRec, seq))
#define STATE_ADDR(slot)  (SLOT_ADDR(slot) + offsetof(ReadingRec, state))

uint16_t g_sagEnterAdc   = SAG_DEFAULT_ENTER_ADC;
uint16_t g_sagExitAdc    = SAG_DEFAULT_EXIT_ADC;
uint16_t g_sagIntervalS  = SAG_DEFAULT_INTERVAL_S;

static uint8_t  s_override   = SAG_OVR_AUTO;
static bool     s_active     = false;
static uint8_t  s_exitReads  = 0;
static uint8_t  s_boot       = 0;
static bool     s_bootAllocated = false;
static uint16_t s_nextSeq    = 0;
static uint16_t s_unsent     = 0;
static uint16_t s_pendFirst  = 0;
static uint8_t  s_pendCount  = 0;
static bool     s_haveStart  = false, s_haveEnd = false;
static uint32_t s_lastNowMs     = 0;
static uint32_t s_msRemainder   = 0;
static uint32_t s_totalMinutes  = 0;
static uint32_t s_epochBaseMin  = 0;
static uint32_t s_startTotalMin = 0;
static uint32_t s_endTotalMin   = 0;
static uint8_t  s_sagEvents  = 0;
static uint16_t s_lost       = 0;

static void ensureBootAllocated() {
    if (!s_bootAllocated) {
        s_boot = (uint8_t)(EEPROM.read(SAG_BOOT_ADDR) + 1);
        EEPROM.update(SAG_BOOT_ADDR, s_boot);
        s_bootAllocated = true;
    }
}

uint32_t readingStore_totalMin() {
    uint32_t cur = nowMs();
    uint32_t deltaMs = (uint32_t)(cur - s_lastNowMs); // unsigned 32-bit wrap-safe across 49.7-day rollover
    s_lastNowMs = cur;
    uint32_t accum = s_msRemainder + deltaMs;
    if (accum >= 60000UL) {
        s_totalMinutes += accum / 60000UL;
        s_msRemainder   = accum % 60000UL;
    } else {
        s_msRemainder   = accum;
    }
    return s_totalMinutes;
}

uint16_t readingStore_uptimeMin() {
    uint32_t tot    = readingStore_totalMin();
    uint32_t relMin = tot - s_epochBaseMin;
    if (relMin >= READING_UPTIME_EPOCH_MAX_MIN) {
        // Controlled boot-epoch rollover every 65,280 elapsed minutes (45.33 days), immune to 49.7-day nowMs() wrap
        s_epochBaseMin = tot;
        s_boot = (uint8_t)(EEPROM.read(SAG_BOOT_ADDR) + 1);
        EEPROM.update(SAG_BOOT_ADDR, s_boot);
        s_bootAllocated = true;
        relMin = 0;
    }
    return (uint16_t)relMin;
}

uint8_t readingStore_ensureBootId() {
    readingStore_uptimeMin(); // advance epoch before ensuring allocation so rollover does not double-increment
    ensureBootAllocated();
    return s_boot;
}

static uint16_t readSeq(uint8_t slot) {
    uint16_t v;
    EEPROM.get(SEQ_ADDR(slot), v);
    return v;
}

static bool slotValid(uint8_t slot, uint16_t seq) {
    return (seq != SEQ_EMPTY) && ((uint8_t)(seq & READING_SLOT_MASK) == slot);
}

static void persistFlag(bool on) {
    EEPROM.update(SAG_FLAG_ADDR, on ? SAG_FLAG_ON : 0x00);
}

void readingStore_begin(bool incrementBoot) {
    s_haveStart = false;
    s_haveEnd = false;
    s_sagEvents = 0;
    s_lost = 0;
    s_lastNowMs     = nowMs();
    s_msRemainder   = 0;
    s_totalMinutes  = 0;
    s_epochBaseMin  = 0;
    s_startTotalMin = 0;
    s_endTotalMin   = 0;

    if (incrementBoot) {
        s_boot = (uint8_t)(EEPROM.read(SAG_BOOT_ADDR) + 1);
        EEPROM.update(SAG_BOOT_ADDR, s_boot);
        s_bootAllocated = true;
    } else {
        s_boot = EEPROM.read(SAG_BOOT_ADDR);
        s_bootAllocated = false;
    }

    s_active = (EEPROM.read(SAG_FLAG_ADDR) == SAG_FLAG_ON);
    s_override = SAG_OVR_AUTO;

    // Check formatting magic
    if (EEPROM.read(READING_MAGIC_ADDR) != READING_MAGIC) {
        Serial.println(F("Reading store: formatting 256-slot ring"));
        for (uint16_t a = READING_STORE_ADDR; a < READING_STORE_ADDR + READING_SLOTS * sizeof(ReadingRec); a++) {
            EEPROM.update(a, 0xFF);
            if ((a & 0x3F) == 0) wdt_reset();
        }
        EEPROM.update(READING_MAGIC_ADDR, READING_MAGIC);
    }

    // Load persisted energy-sag parameters if magic valid
    if (EEPROM.read(SAG_PARAMS_ADDR) == SAG_PARAMS_MAGIC) {
        uint16_t en, ex, iv;
        EEPROM.get(SAG_PARAMS_ADDR + 1, en);
        EEPROM.get(SAG_PARAMS_ADDR + 3, ex);
        EEPROM.get(SAG_PARAMS_ADDR + 5, iv);
        if (readingStore_setThresholds(en, ex) && readingStore_setIntervalS(iv)) {
            Serial.println(F("Reading store: committed parameters restored"));
        } else {
            Serial.println(F("Reading store: committed parameters invalid, using defaults"));
        }
    }

    // Scan all 256 slots to find global newest sequence and unsent count
    bool have = false;
    uint16_t newest = 0;
    s_unsent = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        uint16_t seq = readSeq(slot);
        if (!slotValid(slot, seq)) continue;
        if (!have || (int16_t)(seq - newest) > 0) {
            newest = seq;
            have = true;
        }
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        // If a power loss interrupted a drain batch, revert the draining state
        if (st == RECORD_STATE_DRAINING) {
            EEPROM.update(STATE_ADDR(slot), RECORD_STATE_PERSISTENT);
            st = RECORD_STATE_PERSISTENT;
        } else if (st == RECORD_STATE_DRAINING_OUTAGE) {
            EEPROM.update(STATE_ADDR(slot), RECORD_STATE_OUTAGE_PERSISTENT);
            st = RECORD_STATE_OUTAGE_PERSISTENT;
        }
        if (!readingStore_isSentState(st)) {
            s_unsent++;
        }
    }

    s_nextSeq = have ? (uint16_t)(newest + 1) : 0;
    if (s_nextSeq > SEQ_MAX) s_nextSeq = 0;

    Serial.print(F("Reading store: boot id ")); Serial.print(s_boot);
    Serial.print(F(", sag mode ")); Serial.print(s_active ? F("ACTIVE") : F("off"));
    Serial.print(F(", next seq ")); Serial.print(s_nextSeq);
    Serial.print(F(", unsent ")); Serial.println(s_unsent);
}

bool     readingStore_active()       { return s_active; }
void     readingStore_setActive(bool active) { s_active = active; persistFlag(active); }
bool     readingStore_exitPending()  { return s_active && s_exitReads > 0; }
uint8_t  readingStore_bootId()       { return s_boot; }
uint16_t readingStore_unsentCount()  { return s_unsent; }
uint16_t readingStore_persistentUnsentCount() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        if (!slotValid(slot, readSeq(slot))) continue;
        if (readingStore_isPersistentState(EEPROM.read(STATE_ADDR(slot)))) {
            n++;
        }
    }
    return n;
}
uint16_t readingStore_nextSeq()      { return s_nextSeq; }
uint16_t readingStore_lostRecords()  { return s_lost; }

void readingStore_noteEnter() {
    s_startTotalMin = readingStore_totalMin();
    s_haveStart = true;
    s_haveEnd = false;
    if (s_sagEvents < 255) s_sagEvents++;
    s_active = true;
    s_exitReads = 0;
    persistFlag(true);
}

void readingStore_noteExit() {
    s_endTotalMin = readingStore_totalMin();
    s_haveEnd = true;
    s_active = false;
    s_exitReads = 0;
    persistFlag(false);
}

ReadingStamp readingStore_append(const TelemetryData& t, uint8_t state) {
    uint16_t curMin = readingStore_uptimeMin(); // one epoch transition
    ensureBootAllocated();                      // no-op after a rollover
    uint16_t seq  = s_nextSeq;
    uint8_t  slot = (uint8_t)(seq & READING_SLOT_MASK);

    uint16_t oldSeq = readSeq(slot);
    if (slotValid(slot, oldSeq) && !readingStore_isSentState(EEPROM.read(STATE_ADDR(slot))) && s_unsent > 0) {
        s_unsent--;
        if (s_lost < 65535) s_lost++;
        Serial.println(F("Reading store: ring full, oldest unsent record overwritten"));
    }

    ReadingRec r;
    r.boot   = s_boot;
    r.tMin   = curMin;
    r.temp_s = t.temp_s;
    r.hum_s  = t.hum_s;
    r.dust   = t.dust_adc;
    r.gas    = t.gas_adc;
    r.batt   = t.batt_adc;
    r.state  = state;
    r.seq    = SEQ_EMPTY;

    EEPROM.put(SEQ_ADDR(slot), (uint16_t)SEQ_EMPTY); // Invalidate slot
    EEPROM.put(SLOT_ADDR(slot), r);                  // Write body
    EEPROM.put(SEQ_ADDR(slot), seq);                 // Commit seq

    s_nextSeq = (uint16_t)(seq + 1);
    if (s_nextSeq > SEQ_MAX) s_nextSeq = 0;
    s_unsent++;

    Serial.print(F("Reading store: saved seq "));
    Serial.print(seq);
    Serial.print(F(" state=0x"));
    Serial.print(state, HEX);
    Serial.print(F(" (unsent "));
    Serial.print(s_unsent);
    Serial.println(F(")"));

    ReadingStamp stamp;
    stamp.boot = r.boot;
    stamp.seq  = seq;
    stamp.tMin = curMin;
    return stamp;
}

ReadingAnchor readingStore_anchor() {
    // One epoch transition. uptimeMin() may roll 0xFF00 and allocate a boot;
    // ensureBootAllocated() is then a no-op for this snapshot.
    uint16_t up = readingStore_uptimeMin();
    ensureBootAllocated();

    ReadingAnchor a;
    a.boot      = s_boot;
    a.uptimeMin = up;
    a.nextSeq   = s_nextSeq;
    return a;
}

uint16_t readingStore_pruneDayUnsent() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        if (!slotValid(slot, readSeq(slot))) continue;
        if (EEPROM.read(STATE_ADDR(slot)) == RECORD_STATE_UNCONFIRMED) {
            EEPROM.update(STATE_ADDR(slot), RECORD_STATE_SENT);
            n++;
            if (s_unsent > 0) s_unsent--;
        }
    }
    s_pendCount = 0;
    return n;
}

uint16_t readingStore_discardAllUnsent() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        if (!slotValid(slot, readSeq(slot))) continue;
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        if (readingStore_isSentState(st)) continue;
        // Energy-sag records restore to the sag state, outage records to outage.
        uint8_t next = (st == RECORD_STATE_PERSISTENT || st == RECORD_STATE_DRAINING)
                     ? RECORD_STATE_SENT_SAG : RECORD_STATE_SENT;
        EEPROM.update(STATE_ADDR(slot), next);
        n++;
    }
    s_unsent = 0;
    s_pendCount = 0;
    return n;
}

uint16_t readingStore_unsendFrom(uint16_t fromSeq) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        uint16_t sseq = readSeq(slot);
        if (!slotValid(slot, sseq)) continue;
        if ((uint16_t)(sseq - fromSeq) >= 0x8000) continue; // older than fromSeq
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        if (st == RECORD_STATE_PERSISTENT || st == RECORD_STATE_OUTAGE_PERSISTENT) continue;
        bool wasSent = readingStore_isSentState(st);
        uint8_t next = (st == RECORD_STATE_SENT_SAG || st == RECORD_STATE_DRAINING)
                     ? RECORD_STATE_PERSISTENT : RECORD_STATE_OUTAGE_PERSISTENT;
        EEPROM.update(STATE_ADDR(slot), next);
        if (wasSent) {
            s_unsent++;
        }
        n++;
    }
    return n;
}

static inline bool recEligibleForBatch(uint8_t state, bool includeUnconfirmed) {
    if (readingStore_isSentState(state) || readingStore_isDrainingState(state)) return false;
    if (state == RECORD_STATE_UNCONFIRMED) return includeUnconfirmed;
    return true;
}

uint8_t readingStore_buildFrame(uint8_t* buf, uint8_t maxLen, uint16_t nowMin, uint16_t intervalS,
                                bool includeUnconfirmed) {
    s_pendCount = 0;
    if (maxLen < READING_FRAME_HDR_BYTES + READING_FRAME_REC_BYTES) return 0;

    // Allocate the sending boot before numbering the frame. A frame built from
    // records left behind by an earlier boot is cross-boot by definition, and
    // the header carries the boot that is sending it.
    uint8_t senderBoot = readingStore_ensureBootId();

    // Find oldest unsent record
    bool have = false;
    uint16_t first = 0, bestDist = 0;
    uint16_t newest = (s_nextSeq == 0) ? SEQ_MAX : (uint16_t)(s_nextSeq - 1);
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        uint16_t seq = readSeq(slot);
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        if (!slotValid(slot, seq) || !recEligibleForBatch(st, includeUnconfirmed)) continue;
        uint16_t d = (uint16_t)(newest - seq);
        if (!have || d > bestDist) {
            bestDist = d;
            first = seq;
            have = true;
        }
    }
    if (!have) return 0;

    uint8_t maxRecs = (uint8_t)((maxLen - READING_FRAME_HDR_BYTES) / READING_FRAME_REC_BYTES);
    if (maxRecs > READING_FRAME_MAX_RECS) maxRecs = READING_FRAME_MAX_RECS;

    uint8_t* p = buf + READING_FRAME_HDR_BYTES;
    uint8_t n = 0;
    uint8_t firstBoot = 0;
    uint8_t firstState = 0;
    bool sameBoot = false;
    bool isOutage = false;

    for (uint16_t seq = first; n < maxRecs; seq = (seq >= SEQ_MAX) ? 0 : (uint16_t)(seq + 1)) {
        uint8_t slot = (uint8_t)(seq & READING_SLOT_MASK);
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        if (readSeq(slot) != seq || !recEligibleForBatch(st, includeUnconfirmed)) break;
        ReadingRec r;
        EEPROM.get(SLOT_ADDR(slot), r);

        if (n == 0) {
            firstBoot  = r.boot;
            firstState = r.state;
            sameBoot   = (r.boot == senderBoot);
            isOutage   = (r.state != RECORD_STATE_PERSISTENT);
        } else if (r.boot != firstBoot || r.state != firstState || seq == 0) {
            break; // one (boot, state) class per frame; stop at the sequence wrap
        }

        uint16_t recTime = r.tMin;
        if (recTime >= READING_UPTIME_EPOCH_MAX_MIN) {
            // Legacy record from before the epoch model. Mark unknown; the
            // decoder emits an empty rec_t_min and [2C] places it by sequence alone.
            recTime = 0xFFFF;
        }
        p[0]  = highByte(r.temp_s); p[1] = lowByte(r.temp_s);
        p[2]  = r.hum_s;
        p[3]  = highByte(r.dust);   p[4] = lowByte(r.dust);
        p[5]  = highByte(r.gas);    p[6] = lowByte(r.gas);
        p[7]  = highByte(r.batt);   p[8] = lowByte(r.batt);
        p[9]  = highByte(recTime);  p[10] = lowByte(recTime);
        p += READING_FRAME_REC_BYTES;
        n++;
    }
    if (n == 0) return 0;

    // Header layout:
    // [0] version: 0x04 same-boot sag, 0x05 same-boot outage,
    //              0x06 cross-boot sag, 0x07 cross-boot outage
    // [1] sending boot id
    // [2-3] sender uptime min
    // [4-5] first sequence number
    // [6] count
    // [7] interval minutes
    buf[0] = (uint8_t)(0x04 | (isOutage ? 0x01 : 0x00) | (sameBoot ? 0x00 : 0x02));
    buf[1] = senderBoot;
    buf[2] = highByte(nowMin); buf[3] = lowByte(nowMin);
    buf[4] = highByte(first);  buf[5] = lowByte(first);
    buf[6] = n;
    uint16_t effIntervalS = isOutage ? intervalS
                                     : (uint16_t)((g_sagIntervalS > intervalS) ? g_sagIntervalS : intervalS);
    uint16_t nm = (uint16_t)((effIntervalS + 59U) / 60U);
    buf[7] = (uint8_t)(nm < 1 ? 1 : (nm > 255 ? 255 : nm));

    s_pendFirst = first;
    s_pendCount = n;
    return (uint8_t)(READING_FRAME_HDR_BYTES + n * READING_FRAME_REC_BYTES);
}

void readingStore_markFramed() {
    if (s_pendCount == 0) return;
    uint16_t seq = s_pendFirst;
    for (uint8_t i = 0; i < s_pendCount; i++) {
        uint8_t slot = (uint8_t)(seq & READING_SLOT_MASK);
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        uint8_t next = (st == RECORD_STATE_PERSISTENT || st == RECORD_STATE_DRAINING)
                     ? RECORD_STATE_SENT_SAG : RECORD_STATE_SENT;
        EEPROM.update(STATE_ADDR(slot), next);
        seq = (seq >= SEQ_MAX) ? 0 : (uint16_t)(seq + 1);
    }
    s_unsent = (s_unsent >= s_pendCount) ? (s_unsent - s_pendCount) : 0;
    Serial.print(F("Reading store: "));
    Serial.print(s_pendCount);
    Serial.print(F(" flushed (committed), unsent left "));
    Serial.println(s_unsent);
    s_pendCount = 0;
}

void readingStore_unframePending() {
    s_pendCount = 0;
}

void readingStore_markFramedDraining() {
    if (s_pendCount == 0) return;
    uint16_t seq = s_pendFirst;
    for (uint8_t i = 0; i < s_pendCount; i++) {
        uint8_t slot = (uint8_t)(seq & READING_SLOT_MASK);
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        uint8_t next = (st == RECORD_STATE_PERSISTENT || st == RECORD_STATE_DRAINING)
                     ? RECORD_STATE_DRAINING : RECORD_STATE_DRAINING_OUTAGE;
        EEPROM.update(STATE_ADDR(slot), next);
        seq = (seq >= SEQ_MAX) ? 0 : (uint16_t)(seq + 1);
    }
    s_unsent = (s_unsent >= s_pendCount) ? (s_unsent - s_pendCount) : 0;
    Serial.print(F("Reading store: "));
    Serial.print(s_pendCount);
    Serial.print(F(" flushed (draining), unsent left "));
    Serial.println(s_unsent);
    s_pendCount = 0;
}

void readingStore_commitDrain() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        if (!slotValid(slot, readSeq(slot))) continue;
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        uint8_t next;
        if (st == RECORD_STATE_DRAINING)        next = RECORD_STATE_SENT_SAG;
        else if (st == RECORD_STATE_DRAINING_OUTAGE) next = RECORD_STATE_SENT;
        else continue;
        EEPROM.update(STATE_ADDR(slot), next);
        n++;
    }
    if (n > 0) {
        Serial.print(F("Reading store: drain committed ("));
        Serial.print(n);
        Serial.println(F(" records ACK'd)"));
    }
}

void readingStore_revertDrain() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < READING_SLOTS; i++) {
        uint8_t slot = (uint8_t)i;
        if (!slotValid(slot, readSeq(slot))) continue;
        uint8_t st = EEPROM.read(STATE_ADDR(slot));
        uint8_t next;
        if (st == RECORD_STATE_DRAINING)             next = RECORD_STATE_PERSISTENT;
        else if (st == RECORD_STATE_DRAINING_OUTAGE) next = RECORD_STATE_OUTAGE_PERSISTENT;
        else continue;
        EEPROM.update(STATE_ADDR(slot), next);
        n++;
    }
    if (n > 0) {
        s_unsent += n;
        Serial.print(F("Reading store: drain unACK'd - reverted "));
        Serial.print(n);
        Serial.println(F(" records to persistent"));
    }
}

bool readingStore_setThresholds(uint16_t enterAdc, uint16_t exitAdc) {
    if (enterAdc < 6 || exitAdc > 439 || exitAdc < enterAdc + 4) return false;
    g_sagEnterAdc = enterAdc;
    g_sagExitAdc  = exitAdc;
    return true;
}

bool readingStore_setIntervalS(uint16_t seconds) {
    if (seconds < 30 || seconds > 14400) return false;
    g_sagIntervalS = seconds;
    return true;
}

void readingStore_setOverride(uint8_t ovr) {
    s_override = (ovr <= SAG_OVR_AUTO) ? ovr : SAG_OVR_AUTO;
}

uint8_t readingStore_override() { return s_override; }
uint16_t readingStore_startAgoMin() {
    if (!s_haveStart) return 0xFFFFU;
    uint32_t elapsed = readingStore_totalMin() - s_startTotalMin;
    return (elapsed >= 0xFFFEUL) ? 0xFFFEU : (uint16_t)elapsed;
}
uint16_t readingStore_endAgoMin() {
    if (s_active) return 0U;
    if (!s_haveEnd) return 0xFFFFU;
    uint32_t elapsed = readingStore_totalMin() - s_endTotalMin;
    return (elapsed >= 0xFFFEUL) ? 0xFFFEU : (uint16_t)elapsed;
}
uint8_t  readingStore_sagEvents()   { return s_sagEvents; }

void readingStore_commitParams() {
    EEPROM.update(SAG_PARAMS_ADDR, SAG_PARAMS_MAGIC);
    EEPROM.put(SAG_PARAMS_ADDR + 1, g_sagEnterAdc);
    EEPROM.put(SAG_PARAMS_ADDR + 3, g_sagExitAdc);
    EEPROM.put(SAG_PARAMS_ADDR + 5, g_sagIntervalS);
}

void readingStore_factoryReset() {
    EEPROM.update(SAG_PARAMS_ADDR, 0xFF);
    EEPROM.update(READING_MAGIC_ADDR, 0xFF);
    persistFlag(false);
    g_sagEnterAdc  = SAG_DEFAULT_ENTER_ADC;
    g_sagExitAdc   = SAG_DEFAULT_EXIT_ADC;
    g_sagIntervalS = SAG_DEFAULT_INTERVAL_S;
    s_override     = SAG_OVR_AUTO;
    s_active       = false;
    s_unsent       = 0;
    s_pendCount    = 0;
    s_nextSeq      = 0;
    s_lost         = 0;
    s_lastNowMs     = nowMs();
    s_msRemainder   = 0;
    s_totalMinutes  = 0;
    s_epochBaseMin  = 0;
    s_startTotalMin = 0;
    s_endTotalMin   = 0;
    s_haveStart     = false;
    s_haveEnd       = false;
    s_bootAllocated = false;
}
