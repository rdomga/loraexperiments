#ifndef PAYLOAD_ENCODER_H
#define PAYLOAD_ENCODER_H

#include <Arduino.h>

#define PORT1_PAYLOAD_LEN 14
#define PORT2_PAYLOAD_LEN 46

// Ping reasons for Port 2 byte 16, bits 2-4
#define PING_REASON_RATIO    0  // autonomous, ping-ratio cadence reached (normally unconfirmed)
#define PING_REASON_DEADMAN  1  // §4 dead-man cadence check (confirmed)
#define PING_REASON_VERIFY   2  // §3.8 SF/TXP/channel candidate verification (confirmed)
#define PING_REASON_MANUAL   3  // /@P1# operator request (confirmed)
#define PING_REASON_INIT     4  // boot connectivity gate, clean or dirty restart (confirmed)

// Reset reason flags
#define RESET_FLAG_INCOMPLETE_CYCLE 0x10

struct TelemetryData {
    int16_t  temp_s;   // scaled x10, INT16_MIN if invalid
    uint8_t  hum_s;    // scaled x2, 0xFF if invalid
    uint16_t dust_adc; // raw 0-1023
    uint16_t gas_adc;  // raw 0-1023
    uint16_t batt_adc; // raw 0-1023
    // Anchor of the stored reading this frame reports (Port 1 bytes 9-13).
    uint8_t  boot_id;
    uint16_t seq;
    uint16_t uptime_min;
};

struct StatusPingData {
    uint16_t interval_s;
    uint32_t seqno;
    int16_t  temp_s;
    uint8_t  hum_s;
    uint16_t dust_adc;
    uint16_t gas_adc;
    uint16_t batt_adc;
    uint8_t  resetReason;
    uint8_t  mode;          // bits 0-1
    uint8_t  pingReason;    // bits 2-4
    bool     profileSaved;  // bit 7
    uint8_t  sensorMask;
    uint16_t gasWarmup_s;
    uint16_t battLow;
    uint16_t battRecover;
    uint16_t battCritical;
    uint16_t battPsave;
    uint16_t battPsaveRecover;
    uint8_t  pingRatio;
    uint8_t  radioDR;
    uint8_t  radioTXP;
    uint8_t  radioChannel;
    // appended (bytes 29-33); bits 5-6 of byte 16 carry the battery strategy
    uint8_t  strategy;         // BATTERY_STRATEGY_NONE / LADDER / STORAGE
    uint8_t  guardState;       // bit0 sag active, bit1 CRITICAL floor, bit2 ladder LOW, bit3 ladder POWERSAVE, bit4 confirming exit, bit5 outage active
    uint8_t  sagEnterHalf;     // sag enter threshold / 2
    uint8_t  sagExitHalf;      // sag exit threshold / 2
    uint8_t  sagIntervalMin;   // sag reading interval, minutes
    uint8_t  bufferUnsent;     // saved readings waiting to be sent (capped at 255)
    // appended (bytes 34-41): the latest energy sag event
    uint8_t  bootId;           // boot counter (same id as in port 3 frames)
    uint16_t sagStartAgo;      // minutes since latest sag began, 0xFFFF unknown
    uint16_t sagEndAgo;        // minutes since it ended, 0 while it runs, 0xFFFF unknown
    uint8_t  sagCount;         // sag events entered since this boot
    uint16_t sagLost;          // unsent records overwritten by ring wrap
    // appended (bytes 42-45): Port 2 anchor, one atomic snapshot
    uint16_t uptime_min;       // sender uptime in minutes
    uint16_t next_seq;         // next reading sequence the node will assign
};

void encodePort1Telemetry(const TelemetryData& data, uint8_t* outBuf);
void encodePort2Status(const StatusPingData& data, uint8_t* outBuf);

#endif // PAYLOAD_ENCODER_H
