#include "Arduino.h"
#include "EEPROM.h"
#include "reading_store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

FakeSerial Serial;
unsigned long g_fakeNowMs = 0;
FakeEEPROM EEPROM;
static int fails = 0;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static TelemetryData rec(int i) {
    TelemetryData t;
    t.temp_s   = 250 + i;
    t.hum_s    = 100;
    t.dust_adc = i;
    t.gas_adc  = 500 + i;
    t.batt_adc = 360;
    return t;
}

static void reboot(bool increment = true) {
    readingStore_begin(increment);
}

int main() {
    // 1. Fresh EEPROM: formats, empty, inactive
    reboot();
    CHECK(!readingStore_active());
    CHECK(readingStore_unsentCount() == 0);
    CHECK(readingStore_nextSeq() == 0);

    // 2. Active flag survives reboot, non-incrementing boot check
    readingStore_setActive(true);
    CHECK(readingStore_active());
    reboot(true);
    CHECK(readingStore_active());
    uint8_t bootBefore = readingStore_bootId();
    reboot(true);
    CHECK(readingStore_bootId() == (uint8_t)(bootBefore + 1));
    reboot(false);
    CHECK(readingStore_bootId() == (uint8_t)(bootBefore + 1));
    readingStore_setActive(false);

    // 3. Store 34 records (energy sag), survive reboot, flush in Port 3 frames
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot();
        for (int i = 0; i < 34; i++) {
            g_fakeNowMs = (unsigned long)i * 30 * 60000UL;
            readingStore_append(rec(i), RECORD_STATE_PERSISTENT);
        }
        CHECK(readingStore_unsentCount() == 34);
        reboot(false);
        CHECK(readingStore_unsentCount() == 34);
        CHECK(readingStore_nextSeq() == 34);

        g_fakeNowMs = 20UL * 60 * 60000;
        uint8_t buf[READING_FRAME_MAX_BYTES];
        int frames = 0, total = 0;
        uint16_t expectSeq = 0;
        while (true) {
            uint8_t len = readingStore_buildFrame(buf, 111, (uint16_t)(g_fakeNowMs / 60000UL), 3600);
            if (!len) break;
            uint8_t n = buf[6];
            uint16_t first = (buf[4] << 8) | buf[5];
            CHECK(len == 8 + n * 11);
            CHECK(buf[0] == 0x06); // Cross-boot sag frame version 0x06
            CHECK(buf[7] == 60);   // 3600 s / 60 = 60 min
            CHECK(len <= 111);
            CHECK(first == expectSeq);
            CHECK(n <= 9);
            readingStore_markFramed();
            expectSeq += n;
            total += n;
            frames++;
        }
        CHECK(total == 34);
        CHECK(frames == 4);
        CHECK(readingStore_unsentCount() == 0);
    }

    // 4. Dead-Man Pruning & Isolation: 0xFE pruned, 0xFF untouched
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot();
        // 5 persistent records (e.g. overnight sag or outage)
        for (int i = 0; i < 5; i++) {
            readingStore_append(rec(i), RECORD_STATE_PERSISTENT);
        }
        // 10 day store-before-transmit records (unconfirmed)
        for (int i = 5; i < 15; i++) {
            readingStore_append(rec(i), RECORD_STATE_UNCONFIRMED);
        }
        CHECK(readingStore_unsentCount() == 15);
        CHECK(readingStore_nextSeq() == 15);

        // Dead-man ping ACK arrives: prune daytime unconfirmed records
        uint16_t pruned = readingStore_pruneDayUnsent();
        CHECK(pruned == 10);
        CHECK(readingStore_unsentCount() == 5);

        // Verify that only the 5 persistent records remain and flush cleanly
        uint8_t buf[READING_FRAME_MAX_BYTES];
        uint8_t len = readingStore_buildFrame(buf, 111, 100, 3600);
        CHECK(len == 8 + 5 * 11);
        CHECK(buf[6] == 5);
        CHECK(((buf[4] << 8) | buf[5]) == 0); // Started at seq 0
        readingStore_markFramed();
        CHECK(readingStore_unsentCount() == 0);
    }

    // 5. Outage Frame Version 0x03 Check
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot();
        readingStore_append(rec(1), RECORD_STATE_UNCONFIRMED);
        readingStore_append(rec(2), RECORD_STATE_UNCONFIRMED);
        uint8_t buf[READING_FRAME_MAX_BYTES];
        uint8_t len = readingStore_buildFrame(buf, 111, 100, 1800);
        CHECK(len == 8 + 2 * 11);
        CHECK(buf[0] == 0x05); // Same-boot outage frame version 0x05
        CHECK(buf[7] == 30);   // 1800 s / 60 = 30 min
        readingStore_markFramed();
        CHECK(readingStore_unsentCount() == 0);
    }

    // 6. Boot Boundary Batch Stopping in buildFrame
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot(true);
        readingStore_append(rec(1), RECORD_STATE_PERSISTENT); // Boot 1
        readingStore_append(rec(2), RECORD_STATE_PERSISTENT); // Boot 1
        reboot(true); // Boot 2
        readingStore_append(rec(3), RECORD_STATE_PERSISTENT); // Boot 2
        CHECK(readingStore_unsentCount() == 3);

        uint8_t buf[READING_FRAME_MAX_BYTES];
        uint8_t len = readingStore_buildFrame(buf, 111, 10, 1800);
        CHECK(len == 8 + 2 * 11); // Stopped at boot boundary: 2 records from Boot 1
        CHECK(buf[6] == 2);
        readingStore_markFramed();
        CHECK(readingStore_unsentCount() == 1);

        len = readingStore_buildFrame(buf, 111, 10, 1800);
        CHECK(len == 8 + 1 * 11); // Remaining 1 record from Boot 2
        CHECK(buf[6] == 1);
        readingStore_markFramed();
        CHECK(readingStore_unsentCount() == 0);
    }

    // 7. Resend / Unsend: unsendFrom(seq) marks sent records unsent
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot();
        for (int i = 0; i < 10; i++) {
            readingStore_append(rec(i), RECORD_STATE_PERSISTENT);
        }
        uint8_t buf[READING_FRAME_MAX_BYTES];
        while (true) {
            uint8_t l = readingStore_buildFrame(buf, 200, 10, 3600);
            if (!l) break;
            readingStore_markFramed();
        }
        CHECK(readingStore_unsentCount() == 0);

        // Operator requests resend from sequence 7: records 7, 8, 9 re-queued
        uint16_t requeued = readingStore_unsendFrom(7);
        CHECK(requeued == 3);
        CHECK(readingStore_unsentCount() == 3);

        uint8_t len = readingStore_buildFrame(buf, 200, 15, 3600);
        CHECK(len == 8 + 3 * 11);
        CHECK(((buf[4] << 8) | buf[5]) == 7);
        readingStore_markFramed();
        CHECK(readingStore_unsentCount() == 0);
    }

    // 8. 256-Slot Full Capacity & Continuous Ring Wrap
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot();
        // Append 300 records to test full 256-slot capacity and ring wrap
        for (int i = 0; i < 300; i++) {
            readingStore_append(rec(i & 0x7F), RECORD_STATE_PERSISTENT);
        }
        CHECK(readingStore_unsentCount() == 256);
        CHECK(readingStore_lostRecords() == 44);
        reboot(false);
        CHECK(readingStore_unsentCount() == 256);

        uint8_t buf[READING_FRAME_MAX_BYTES];
        int totalFlushed = 0;
        uint16_t prev = 0;
        bool first = true;
        while (true) {
            uint8_t l = readingStore_buildFrame(buf, 111, 0, 3600);
            if (!l) break;
            uint16_t fseq = (buf[4] << 8) | buf[5];
            if (!first) CHECK(fseq == prev);
            first = false;
            prev = fseq + buf[6];
            totalFlushed += buf[6];
            readingStore_markFramed();
        }
        CHECK(totalFlushed == 256);
        CHECK(readingStore_unsentCount() == 0);
    }

    // 9. Legacy tMin sentinel at frame build
    {
        FakeEEPROM f;
        EEPROM = f;
        reboot();                                   // formats the ring, allocates boot B
        uint8_t b = readingStore_bootId();

        ReadingRec legacy;
        memset(&legacy, 0, sizeof legacy);
        legacy.boot  = b;
        legacy.tMin  = 0xFF10;
        legacy.temp_s = 250;
        legacy.hum_s  = 100;
        legacy.dust   = 1;
        legacy.gas    = 500;
        legacy.batt   = 360;
        legacy.state  = RECORD_STATE_PERSISTENT;
        legacy.seq    = 0;
        EEPROM.put(READING_STORE_ADDR + 0 * sizeof(ReadingRec), legacy);

        reboot(false);                              // rescans; s_bootAllocated = false

        uint8_t buf[READING_FRAME_MAX_BYTES];
        uint8_t len = readingStore_buildFrame(buf, 111, 40, 3600);
        CHECK(len == 8 + 1 * 11);
        CHECK(buf[6] == 1);
        CHECK(buf[17] == 0xFF);                     // first record tMin high byte
        CHECK(buf[18] == 0xFF);                     // first record tMin low byte
    }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nall unified reading store host checks passed (100%%)\n", fails);
    return fails ? 1 : 0;
}
