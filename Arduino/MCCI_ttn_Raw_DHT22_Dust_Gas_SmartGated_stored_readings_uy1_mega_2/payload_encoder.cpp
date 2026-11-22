#include "payload_encoder.h"

void encodePort1Telemetry(const TelemetryData& data, uint8_t* outBuf) {
    outBuf[0] = highByte(data.temp_s);
    outBuf[1] = lowByte(data.temp_s);
    outBuf[2] = data.hum_s;
    outBuf[3] = highByte(data.dust_adc);
    outBuf[4] = lowByte(data.dust_adc);
    outBuf[5] = highByte(data.gas_adc);
    outBuf[6] = lowByte(data.gas_adc);
    outBuf[7] = highByte(data.batt_adc);
    outBuf[8] = lowByte(data.batt_adc);
    outBuf[9]  = data.boot_id;
    outBuf[10] = highByte(data.seq);
    outBuf[11] = lowByte(data.seq);
    outBuf[12] = highByte(data.uptime_min);
    outBuf[13] = lowByte(data.uptime_min);
}

void encodePort2Status(const StatusPingData& data, uint8_t* outBuf) {
    outBuf[0] = highByte(data.interval_s);
    outBuf[1] = lowByte(data.interval_s);
    outBuf[2] = (data.seqno >> 24) & 0xFF;
    outBuf[3] = (data.seqno >> 16) & 0xFF;
    outBuf[4] = (data.seqno >> 8) & 0xFF;
    outBuf[5] = data.seqno & 0xFF;
    outBuf[6] = highByte(data.temp_s);
    outBuf[7] = lowByte(data.temp_s);
    outBuf[8] = data.hum_s;
    outBuf[9] = highByte(data.dust_adc);
    outBuf[10] = lowByte(data.dust_adc);
    outBuf[11] = highByte(data.gas_adc);
    outBuf[12] = lowByte(data.gas_adc);
    outBuf[13] = highByte(data.batt_adc);
    outBuf[14] = lowByte(data.batt_adc);
    outBuf[15] = data.resetReason;
    outBuf[16] = (data.mode & 0x03) | ((data.pingReason & 0x07) << 2) | ((data.strategy & 0x03) << 5) | (data.profileSaved ? 0x80 : 0x00);
    outBuf[17] = data.sensorMask;
    outBuf[18] = highByte(data.gasWarmup_s);
    outBuf[19] = lowByte(data.gasWarmup_s);
    outBuf[20] = (uint8_t)(data.battLow / 2);
    outBuf[21] = (uint8_t)(data.battRecover / 2);
    outBuf[22] = (uint8_t)(data.battCritical / 2);
    outBuf[23] = (uint8_t)(data.battPsave / 2);
    outBuf[24] = (uint8_t)(data.battPsaveRecover / 2);
    outBuf[25] = data.pingRatio;
    outBuf[26] = data.radioDR;
    outBuf[27] = data.radioTXP;
    outBuf[28] = data.radioChannel;
    outBuf[29] = data.guardState;
    outBuf[30] = data.sagEnterHalf;
    outBuf[31] = data.sagExitHalf;
    outBuf[32] = data.sagIntervalMin;
    outBuf[33] = data.bufferUnsent;
    outBuf[34] = data.bootId;
    outBuf[35] = highByte(data.sagStartAgo); outBuf[36] = lowByte(data.sagStartAgo);
    outBuf[37] = highByte(data.sagEndAgo);   outBuf[38] = lowByte(data.sagEndAgo);
    outBuf[39] = data.sagCount;
    outBuf[40] = highByte(data.sagLost);     outBuf[41] = lowByte(data.sagLost);
    outBuf[42] = highByte(data.uptime_min);  outBuf[43] = lowByte(data.uptime_min);
    outBuf[44] = highByte(data.next_seq);    outBuf[45] = lowByte(data.next_seq);
}
