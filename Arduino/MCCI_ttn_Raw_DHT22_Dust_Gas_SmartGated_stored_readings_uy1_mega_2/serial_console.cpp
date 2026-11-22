#include "serial_console.h"
#include <lmic.h>
#include "build_config.h"
#include "node_config.h"
#include "battery_guard.h"
#include "radio_failsafe.h"
#include "sensor_manager.h"
#include "gas_sensor.h"
#include "power_manager.h"
#include "network_clock.h"
#include "energy_guard.h"

static char serialCmdBuf[SERIAL_CMD_BUFFER_SIZE];
static uint8_t serialCmdLen = 0;

extern int __heap_start, *__brkval;
int freeRam() {
    int v;
    return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}

void printVerboseStatus() {
    Serial.println(F("---- Device status ----"));
    Serial.print(F("Uptime: "));       Serial.print(nowMs() / 1000UL);  Serial.println(F(" s"));
    Serial.print(F("Free RAM: "));     Serial.print(freeRam());          Serial.println(F(" bytes"));
    Serial.print(F("TX_INTERVAL: "));  Serial.print(TX_INTERVAL);        Serial.println(F(" s"));
    Serial.print(F("SF: SF"));         Serial.print(drToSf(SCG_currentDR));
    Serial.print(F("  TXP: "));        Serial.print(SCG_currentTXP);     Serial.print(F(" dBm"));
    Serial.print(F("  Channel: "));    Serial.println(g_scgChannel);
#ifdef NODE_NAME
    Serial.print(F("Node: ")); Serial.println(F(NODE_NAME));
#endif
    Serial.print(F("Sleep scale: ")); Serial.print(g_wdtScalePermille); Serial.print(F(" permille (node value ")); Serial.print(g_wdtScaleDefaultPermille);
    Serial.print(F(", network factor ")); Serial.print(netclk_factorPermille()); Serial.println(F(", 0 = none yet)"));
    Serial.print(F("Operating mode: ")); Serial.println(modeName(g_mode));
    Serial.print(F("seqnoUp (LMIC, live): ")); Serial.println(LMIC.seqnoUp);
#ifdef WITH_EEPROM
    Serial.print(F("seqnoUp (EEPROM, on-disk): ")); Serial.println(my_eeprom.seqnoUp);
    Serial.print(F("tx_interval (EEPROM): "));       Serial.print(my_eeprom.tx_interval);
    Serial.print(F("  saved flag: "));               Serial.println(my_eeprom.tx_interval_saved);
    Serial.print(F("EEPROM dirty: "));  Serial.println(eeprom_dirty ? F("yes") : F("no"));
    Serial.print(F("Profile committed: ")); Serial.println(my_eeprom.profile_saved == 1 ? F("yes") : F("no"));
#else
    Serial.println(F("EEPROM: disabled (WITH_EEPROM not defined)"));
#endif
    Serial.print(F("Battery guard mode: "));
    if (lowBatteryMode)      Serial.println(F("LOW (heartbeat-only)"));
    else if (powerSaveMode)  Serial.println(F("POWERSAVE (6h interval)"));
    else                     Serial.println(F("NORMAL"));
    Serial.print(F("Battery strategy: ")); Serial.println(batteryStrategyName());
    energyGuard_printStatus();
    Serial.print(F("bootBurstFramesLeft: "));  Serial.println(bootBurstFramesLeft);
    Serial.print(F("Burst interval: "));       Serial.print(g_burstIntervalS); Serial.println(F("s"));
    Serial.print(F("Sensor mask: 0x"));        Serial.println(g_sensorMask, HEX);
    Serial.print(F("Gas warmup: "));           Serial.print(gas_getWarmupSeconds()); Serial.println(F("s"));
    Serial.print(F("Ping ratio: every "));      Serial.print(g_pingRatio); Serial.print(F(" uplinks ("));
    Serial.print(g_uplinkCountSincePing);      Serial.println(F(" since last)"));
    Serial.print(F("Dead-man check: every "));  Serial.print(DEADMAN_PING_RATIO); Serial.print(F(" uplinks ("));
    Serial.print(g_uplinkCountSinceDeadman);   Serial.print(F(" since last), fail count "));
    Serial.print(g_deadmanFailCount);          Serial.print(F("/")); Serial.println(DEADMAN_FAIL_THRESHOLD);
    Serial.print(F("Battery thresholds: LOW="));       Serial.print(g_battLow);
    Serial.print(F(" REC="));                           Serial.print(g_battRecover);
    Serial.print(F(" CRIT="));                          Serial.print(g_battCritical);
    Serial.print(F(" PSAVE="));                         Serial.print(g_battPsave);
    Serial.print(F(" PSAVE_REC="));                     Serial.println(g_battPsaveRecover);
    Serial.print(F("Radio verify requested (awaiting ping): ")); Serial.println(g_radioVerifyViaPing ? F("yes") : F("no"));
    Serial.print(F("Radio verify pending (ping in flight): ")); Serial.println(g_radioVerifyPending ? F("yes") : F("no"));
#ifdef WITH_EEPROM
    Serial.print(F("Radio config committed: ")); Serial.println(my_eeprom.radio_saved == 1 ? F("yes") : F("no"));
#endif

    Serial.println(F("-- Last transmitted sample --"));
    Serial.print(F("  temp: "));
    if (last_temp_s == INT16_MIN) Serial.println(F("(no valid read)"));
    else { Serial.print(last_temp_s / 10.0, 1); Serial.println(F(" C")); }
    Serial.print(F("  hum: "));
    if (last_hum_s == 0xFF) Serial.println(F("(no valid read)"));
    else { Serial.print(last_hum_s / 2.0, 1); Serial.println(F(" %")); }
    Serial.print(F("  dust ADC: ")); Serial.println(last_adc_s);
    Serial.print(F("  gas ADC: "));  Serial.println(last_gas_s);
    Serial.print(F("  batt ADC: ")); Serial.println(last_batt_s);

    Serial.println(F("-- Current in-RAM sensor read --"));
    Serial.print(F("  temp: ")); Serial.print(g_temp, 1); Serial.println(F(" C"));
    Serial.print(F("  hum: "));  Serial.print(g_hum, 1);  Serial.println(F(" %"));
    Serial.print(F("  dust ADC: ")); Serial.print(g_adc); Serial.println(g_adc_valid ? F("") : F(" (flagged)"));
    Serial.print(F("  gas ADC: "));  Serial.print(g_gas); Serial.println(g_gas_valid ? F("") : F(" (flagged)"));
    Serial.print(F("  batt ADC: ")); Serial.print(g_batt); Serial.println(g_batt_valid ? F("") : F(" (flagged)"));

    Serial.print(F("Reset reason byte: 0x")); Serial.println(g_resetReasonByte, HEX);
    Serial.print(F("radioInitialized: ")); Serial.println(radioInitialized ? F("yes") : F("no"));
    Serial.println(F("------------------------"));
}

bool serialConsole_poll(DownlinkResult& outResult) {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') continue;
        if (serialCmdLen < sizeof(serialCmdBuf) - 1) {
            serialCmdBuf[serialCmdLen++] = c;
        }
        if (c == '#') {
#ifdef NIGHT_BENCH_ADC
            if (serialCmdLen > 4 && serialCmdBuf[0] == '/' && serialCmdBuf[1] == '@' && serialCmdBuf[2] == 'Y') {
                serialCmdBuf[serialCmdLen - 1] = 0;
                extern int g_benchAdc;
                g_benchAdc = atoi(serialCmdBuf + 3);
                Serial.print(F("BENCH: battery ADC set to ")); Serial.println(g_benchAdc);
                serialCmdLen = 0;
                return false;
            }
#endif
            outResult = parseDownlinkCommand((uint8_t*)serialCmdBuf, serialCmdLen);
            serialCmdLen = 0;
            return true;
        }
    }
    return false;
}
