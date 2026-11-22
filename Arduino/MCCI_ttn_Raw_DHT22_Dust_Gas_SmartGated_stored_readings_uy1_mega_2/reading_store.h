#ifndef READING_STORE_H
#define READING_STORE_H

#include <Arduino.h>
#include <stdint.h>
#include "payload_encoder.h"

// 256 slots x 15 bytes = 3,840 bytes allocated in EEPROM (64..3903)
#define READING_STORE_ADDR      64
#define READING_SLOTS           256
#define READING_SLOT_MASK       (READING_SLOTS - 1) // 0xFF
#define READING_MAGIC_ADDR      62
#define READING_MAGIC           0xC6

#define SEQ_EMPTY               0xFFFF
#define SEQ_MAX                 0xFEFF // 65,279 (255 complete cycles of 256 slots)
static_assert((SEQ_MAX + 1) % READING_SLOTS == 0, "SEQ_MAX+1 must be a multiple of READING_SLOTS");

#define READING_UPTIME_EPOCH_MAX_MIN 0xFF00UL // 65,280 min (45.33 days): roll s_boot before uint16_t tMin wraps

// Record lifecycle states stored in ReadingRec.state (15-byte ReadingRec layout unchanged)
#define RECORD_STATE_PERSISTENT        0xFF // Energy sag reading: preserved for Port 3 flush (never pruned by dead-man ACK)
#define RECORD_STATE_UNCONFIRMED       0xFE // Daytime store-before-transmit: pruned to 0x00 on dead-man ACK
#define RECORD_STATE_DRAINING          0xFD // Energy sag record in current drain batch
#define RECORD_STATE_OUTAGE_PERSISTENT 0xFC // Daytime link-outage fallback (!g_linkOnline): preserved for Port 3 flush
#define RECORD_STATE_DRAINING_OUTAGE   0xFB // Outage record in current drain batch
#define RECORD_STATE_SENT_SAG          0x01 // Flushed energy-sag record (restores to 0xFF on /@U# resend)
#define RECORD_STATE_SENT              0x00 // Flushed outage or pruned live record (restores to 0xFC on /@U# resend)

static inline bool readingStore_isSentState(uint8_t st) {
    return (st == RECORD_STATE_SENT || st == RECORD_STATE_SENT_SAG);
}
static inline bool readingStore_isPersistentState(uint8_t st) {
    return (st == RECORD_STATE_PERSISTENT || st == RECORD_STATE_OUTAGE_PERSISTENT);
}
static inline bool readingStore_isDrainingState(uint8_t st) {
    return (st == RECORD_STATE_DRAINING || st == RECORD_STATE_DRAINING_OUTAGE);
}

// Energy-sag thresholds and intervals persisted at bytes 37..43
#define SAG_PARAMS_ADDR         37
#define SAG_PARAMS_MAGIC        0xD7
#define SAG_BOOT_ADDR           35
#define SAG_FLAG_ADDR           36
#define SAG_FLAG_ON             0x5A

#define SAG_DEFAULT_ENTER_ADC   375 // below this: start storing (one read is enough; storing is the safe direction)
#define SAG_DEFAULT_EXIT_ADC    388 // at or above this on two consecutive reads: resume sending and flush
#define SAG_DEFAULT_INTERVAL_S  3600

#define SAG_OVR_OFF             0
#define SAG_OVR_ON              1
#define SAG_OVR_AUTO            2

struct __attribute__((packed)) ReadingRec {
    uint8_t  boot;   // boot id when recorded
    uint16_t tMin;   // node uptime in minutes
    int16_t  temp_s; // scaled x10 (INT16_MIN if unreadable)
    uint8_t  hum_s;  // scaled x2 (0xFF if unreadable)
    uint16_t dust;   // raw dust ADC
    uint16_t gas;    // raw gas ADC
    uint16_t batt;   // raw battery ADC
    uint8_t  state;  // RECORD_STATE_*
    uint16_t seq;    // 0..0xFF7F
};
static_assert(sizeof(ReadingRec) == 15, "ReadingRec must be 15 bytes");
static_assert(READING_STORE_ADDR + READING_SLOTS * sizeof(ReadingRec) <= 3904, "Reading store must fit within 64..3903");

// Lifecycle & State
void     readingStore_begin(bool incrementBoot);
uint8_t  readingStore_bootId();
uint8_t  readingStore_ensureBootId();
uint16_t readingStore_uptimeMin();
uint32_t readingStore_totalMin();
bool     readingStore_active();
void     readingStore_setActive(bool active);
bool     readingStore_exitPending();
uint16_t readingStore_unsentCount();
uint16_t readingStore_persistentUnsentCount();
uint16_t readingStore_nextSeq();
uint16_t readingStore_lostRecords();

// Atomic stamp captured when a reading is appended, and one-shot anchor
// snapshot for the Port 2 status frame (boot/uptime/next-seq from one state).
struct ReadingStamp {
    uint8_t  boot;
    uint16_t seq;
    uint16_t tMin;
};

struct ReadingAnchor {
    uint8_t  boot;
    uint16_t uptimeMin;
    uint16_t nextSeq;
};

// Appending
ReadingStamp readingStore_append(const TelemetryData& t, uint8_t state);
ReadingAnchor readingStore_anchor();

// Pruning & Discarding
uint16_t readingStore_pruneDayUnsent();
uint16_t readingStore_discardAllUnsent();

// Resend
uint16_t readingStore_unsendFrom(uint16_t fromSeq);

// Port 3 batch frame generation
#define READING_FRAME_HDR_BYTES  8
#define READING_FRAME_REC_BYTES  11
#define READING_FRAME_MAX_RECS   9
#define READING_FRAME_MAX_BYTES  (READING_FRAME_HDR_BYTES + READING_FRAME_MAX_RECS * READING_FRAME_REC_BYTES) // 107 B
#define READING_FLUSH_SPACING_S  15 // pause between flush frames (seconds)

uint8_t  readingStore_buildFrame(uint8_t* buf, uint8_t maxLen, uint16_t nowMin, uint16_t intervalS,
                                 bool includeUnconfirmed = true);
void     readingStore_markFramed();
void     readingStore_unframePending();
void     readingStore_markFramedDraining();
void     readingStore_commitDrain();
void     readingStore_revertDrain();

// Energy sag parameters
extern uint16_t g_sagEnterAdc;
extern uint16_t g_sagExitAdc;
extern uint16_t g_sagIntervalS;

bool     readingStore_setThresholds(uint16_t enterAdc, uint16_t exitAdc);
bool     readingStore_setIntervalS(uint16_t seconds);
void     readingStore_setOverride(uint8_t ovr);
uint8_t  readingStore_override();
uint16_t readingStore_startAgoMin();
uint16_t readingStore_endAgoMin();
uint8_t  readingStore_sagEvents();
void     readingStore_noteEnter();
void     readingStore_noteExit();

void     readingStore_commitParams();
void     readingStore_factoryReset();

#endif // READING_STORE_H
