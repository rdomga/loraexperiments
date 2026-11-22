#include "DownlinkParser.h"
#include <Arduino.h>
#include <stdlib.h>
#include "build_config.h"

// Parse a decimal integer from cmdstr starting at position i, stopping at
// either '#' or the given terminator (defaults to '#', so existing
// single-value call sites are unaffected).
static long getCmdValue(int &i, char* cmdstr, int maxLen, char term = '#') {
    char seqStr[10];
    int j = 0;

    while (i < maxLen &&
           cmdstr[i] != '#' &&
           cmdstr[i] != term &&
           j < (int)(sizeof(seqStr) - 1)) {
        seqStr[j++] = cmdstr[i++];
    }
    seqStr[j] = '\0';
    return atol(seqStr);
}

// Same scan as getCmdValue(), but reads hex digits (no "0x" prefix expected).
static long getCmdHexValue(int &i, char* cmdstr, int maxLen) {
    char seqStr[10];
    int j = 0;

    while (i < maxLen &&
           cmdstr[i] != '#' &&
           j < (int)(sizeof(seqStr) - 1)) {
        seqStr[j++] = cmdstr[i++];
    }
    seqStr[j] = '\0';
    return strtol(seqStr, NULL, 16);
}

// Parse a downlink command. message[] must be null-terminated.
DownlinkResult parseDownlinkCommand(uint8_t* message, uint8_t len) {
    DownlinkResult result = { DL_NONE, 0, 0 };

    Serial.print(F("Downlink hex: "));
    for (int j = 0; j < len; j++) {
        if (message[j] < 0x10) Serial.print('0');
        Serial.print(message[j], HEX);
        Serial.print(' ');
    }
    Serial.println();

    Serial.print(F("Downlink ASCII: "));
    for (int j = 0; j < len; j++) {
        Serial.print((message[j] >= 32 && message[j] <= 126) ? (char)message[j] : '.');
    }
    Serial.println();

    if (len < 3 || message[0] != '/' || message[1] != '@') {
        Serial.println(F("Not a command (expected /@X...)"));
        return result;
    }

    // Sized for the longest command this parser handles: /@V<3-digit>,
    // <3-digit>,<3-digit>,<3-digit>,<3-digit># = 23 chars + null. Bumped
    // from 20 to 28 on 2026-09-09 when /@V# shipped — a bench test caught
    // this truncating the command mid-parse (silently dropping the last
    // value and the closing '#', misreported as "missing #") because this
    // buffer wasn't sized alongside SERIAL_CMD_BUFFER_SIZE/dlBuf in the
    // .ino when the command was added. If a future command needs more than
    // 27 chars, all three (this, SERIAL_CMD_BUFFER_SIZE, and dlBuf in the
    // .ino's EV_TXCOMPLETE handler) need bumping together, or they'll
    // silently drift out of sync again the same way.
    char cmdstr[28];
    memset(cmdstr, 0, sizeof(cmdstr));

    uint8_t copyLen = min(len, (uint8_t)(sizeof(cmdstr) - 1));
    memcpy(cmdstr, message, copyLen);
    cmdstr[copyLen] = '\0';

    int i = 2;
    long cmdValue;

    Serial.print(F("Command: "));
    Serial.println(cmdstr);

    switch (cmdstr[i]) {
        case 'I':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed interval command (missing #)"));
                break;
            }
            if (cmdValue >= 30) {
                result.action = DL_INTERVAL_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Interval -> "));
                Serial.print(result.value);
                Serial.println(F(" s"));
            } else {
                Serial.println(F("Interval too short (min 30 s)"));
            }
            break;

        case 'S':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed sleep command (missing #)"));
                break;
            }
            if (cmdValue >= 1) {
                result.action = DL_SLEEP;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Sleep -> "));
                Serial.print(result.value);
                Serial.println(F(" min"));
            } else {
                Serial.println(F("Invalid sleep value"));
            }
            break;

        case 'T':
            result.action = DL_TX_NOW;
            Serial.println(F("Immediate transmit requested"));
            break;

        case 'P':
            i++;
            result.action = DL_PING;
            if (cmdstr[i] == '1' && cmdstr[i + 1] == '#') {
                result.value = 1;
                Serial.println(F("Confirmed ping requested"));
            } else {
                result.value = 0;
                Serial.println(F("Ping/status requested"));
            }
            break;

        case 'G':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed flush-spacing command (missing #)"));
                break;
            }
            if (cmdValue >= 15 && cmdValue <= 3600) {
                result.action = DL_FLUSH_SPACING_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Flush spacing -> "));
                Serial.print(result.value);
                Serial.println(F(" s"));
            } else {
                Serial.println(F("Invalid flush spacing (15-3600 s)"));
            }
            break;

        case 'B':
            i++;
            if (cmdstr[i] == '#') {
                // Bare /@B# - caller (.ino) fills in the default 9/90s.
                result.action = DL_BURST_START;
                result.value = 0;
                result.value2 = 0;
                Serial.println(F("Burst mode requested (default count/interval)"));
            } else {
                long count = getCmdValue(i, cmdstr, copyLen, ',');
                if (cmdstr[i] != ',') {
                    Serial.println(F("Malformed burst command (missing comma)"));
                    break;
                }
                i++;
                long interval = getCmdValue(i, cmdstr, copyLen);
                if (cmdstr[i] != '#') {
                    Serial.println(F("Malformed burst command (missing #)"));
                    break;
                }
                if (count >= 1 && count <= 250 && interval >= 30 && interval <= 3600) {
                    result.action = DL_BURST_START;
                    result.value = (unsigned long)count;
                    result.value2 = (unsigned long)interval;
                    Serial.print(F("Burst -> "));
                    Serial.print(count);
                    Serial.print(F(" frames @ "));
                    Serial.print(interval);
                    Serial.println(F("s"));
                    if (interval < 90) {
                        Serial.println(F("Warning: spacing below 90s has caused mid-burst brownouts on solar-only power - safe on USB, risky in the field"));
                    }
                } else {
                    Serial.println(F("Invalid burst count/interval (count 1-250, interval 30-3600s)"));
                }
            }
            break;

        case 'F':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed SF command (missing #)"));
                break;
            }
            if (cmdValue >= 7 && cmdValue <= 12) {
                result.action = DL_SF_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("SF -> SF"));
                Serial.println(cmdValue);
            } else {
                Serial.println(F("Invalid SF value"));
            }
            break;

        case 'X':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed TXPOWER command (missing #)"));
                break;
            }
            if (cmdValue >= 2 && cmdValue <= 20) {
                result.action = DL_TXPOWER_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("TXP -> "));
                Serial.println(cmdValue);
            } else {
                Serial.println(F("Invalid TX power value"));
            }
            break;

        case 'E':
            result.action = DL_EEPROM_SAVE;
            Serial.println(F("Explicit EEPROM save requested"));
            break;

        case 'M':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed mode command (missing #)"));
                break;
            }
            if (cmdValue >= 0 && cmdValue <= 2) {
                result.action = DL_MODE_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Mode -> "));
                Serial.println(cmdValue);
            } else {
                Serial.println(F("Invalid mode value (0=NORMAL, 1=NORMAL_NO_GAS, 2=ALWAYS_ON)"));
            }
            break;

        case 'R':
            result.action = DL_RESTART;
            Serial.println(F("Restart requested"));
            break;

        case 'Z':
            result.action = DL_FACTORY_RESET;
            Serial.println(F("Factory reset requested"));
            break;

        case 'W':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed warmup command (missing #)"));
                break;
            }
            if (cmdValue >= 10 && cmdValue <= 3600) {
                result.action = DL_WARMUP_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Gas warmup -> "));
                Serial.print(result.value);
                Serial.println(F(" s"));
            } else {
                Serial.println(F("Invalid warmup value (10-3600 s)"));
            }
            break;

        case 'N':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed ping-ratio command (missing #)"));
                break;
            }
            if (cmdValue >= 1 && cmdValue <= 250) {
                result.action = DL_PING_RATIO_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Ping ratio -> every "));
                Serial.print(result.value);
                Serial.println(F(" uplinks"));
            } else {
                Serial.println(F("Invalid ping ratio (1-250)"));
            }
            break;

        case 'K':
            i++;
            cmdValue = getCmdHexValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') {
                Serial.println(F("Malformed sensor mask command (missing #)"));
                break;
            }
            if (cmdValue >= 0 && cmdValue <= 0x0F) {
                result.action = DL_SENSOR_MASK_CHANGED;
                result.value = (unsigned long)cmdValue;
                Serial.print(F("Sensor mask -> 0x"));
                Serial.println(cmdValue, HEX);
            } else {
                Serial.println(F("Invalid sensor mask (must be 0x0-0xF)"));
            }
            break;

        case 'V':
        {
            i++;
            long low = getCmdValue(i, cmdstr, copyLen, ',');
            if (cmdstr[i] != ',') { Serial.println(F("Malformed threshold command (expected 5 comma-separated values)")); break; }
            i++;
            long rec = getCmdValue(i, cmdstr, copyLen, ',');
            if (cmdstr[i] != ',') { Serial.println(F("Malformed threshold command (expected 5 comma-separated values)")); break; }
            i++;
            long crit = getCmdValue(i, cmdstr, copyLen, ',');
            if (cmdstr[i] != ',') { Serial.println(F("Malformed threshold command (expected 5 comma-separated values)")); break; }
            i++;
            long psave = getCmdValue(i, cmdstr, copyLen, ',');
            if (cmdstr[i] != ',') { Serial.println(F("Malformed threshold command (expected 5 comma-separated values)")); break; }
            i++;
            long psaveRec = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') { Serial.println(F("Malformed threshold command (missing #)")); break; }

            // Bounds: plausible-battery-reading range, matching
            // BATTERY_ADC_MIN_VALID/MAX_VALID (battery_sensor.h) — a
            // threshold outside that band could never be crossed by a
            // reading this device would already trust.
            bool inRange = low > 5 && low < 440 && rec > 5 && rec < 440 &&
                           crit > 5 && crit < 440 && psave > 5 && psave < 440 &&
                           psaveRec > 5 && psaveRec < 440;
            // Ordering, not just distinct values — matches the field-tuned
            // defaults (315/335/350/360/380) and is load-bearing, not
            // cosmetic: CRITICAL>=LOW would silently disable the
            // heartbeat-skip check (every lowBatteryMode entry already
            // implies batt<CRITICAL), RECOVER<=LOW collapses that ladder's
            // hysteresis to zero, and POWERSAVE<=LOW makes powerSaveMode
            // unreachable on the way down (lowBatteryMode's check runs
            // first and never falls through to the powerSave check once
            // active). RECOVER<=POWERSAVE_RECOVER (non-strict) is the one
            // relation allowed to be equal — the two ladders sharing a
            // single recovery point is a legitimate config, not a bug.
            bool ordered = (crit < low) && (low < psave) && (psave < rec) && (rec <= psaveRec);

            if (inRange && ordered) {
                result.action = DL_BATTERY_THRESHOLDS_CHANGED;
                result.value  = (unsigned long)low;
                result.value2 = (unsigned long)rec;
                result.value3 = (unsigned long)crit;
                result.value4 = (unsigned long)psave;
                result.value5 = (unsigned long)psaveRec;
                Serial.print(F("Battery thresholds -> LOW="));
                Serial.print(low);
                Serial.print(F(" REC="));
                Serial.print(rec);
                Serial.print(F(" CRIT="));
                Serial.print(crit);
                Serial.print(F(" PSAVE="));
                Serial.print(psave);
                Serial.print(F(" PSAVE_REC="));
                Serial.println(psaveRec);
            } else if (!inRange) {
                Serial.println(F("Invalid threshold value(s) - each must be 6-439 (plausible ADC range)"));
            } else {
                Serial.println(F("Invalid threshold ordering - must satisfy CRITICAL < LOW < POWERSAVE < RECOVER < POWERSAVE_RECOVER"));
            }
            break;
        }

        case 'C':
            i++;
            if (cmdstr[i] == 'P' && cmdstr[i + 1] == '#') {
                result.action = DL_COMMIT_PARAMS;
                Serial.println(F("Commit parameters requested"));
            } else if (cmdstr[i] >= '0' && cmdstr[i] <= '9') {
                // §3.6 (Field_Diagnostics_And_Remote_Configuration_2026-09-07.md)
                cmdValue = getCmdValue(i, cmdstr, copyLen);
                if (cmdstr[i] != '#') {
                    Serial.println(F("Malformed channel command (missing #)"));
                    break;
                }
                if (cmdValue >= 0 && cmdValue <= 8) {
                    result.action = DL_CHANNEL_CHANGED;
                    result.value = (unsigned long)cmdValue;
                    Serial.print(F("Channel -> "));
                    Serial.println(cmdValue);
                } else {
                    Serial.println(F("Invalid channel index (0-8, EU868)"));
                }
            } else {
                Serial.println(F("Malformed C command (expected /@CP# or /@C<idx>#)"));
            }
            break;

#if BATTERY_STRATEGY == BATTERY_STRATEGY_STORAGE
        case 'H': {
            i++;
            long en = getCmdValue(i, cmdstr, copyLen, ',');
            if (cmdstr[i] != ',') { Serial.println(F("Malformed sag thresholds (expected /@H<enter>,<exit>#)")); break; }
            i++;
            long ex = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') { Serial.println(F("Malformed sag thresholds (missing #)")); break; }
            if (en >= 6 && ex <= 439 && ex >= en + 4) {
                result.action = DL_SAG_THRESHOLDS_CHANGED;
                result.value  = (unsigned long)en;
                result.value2 = (unsigned long)ex;
            } else {
                Serial.println(F("Invalid sag thresholds (6 <= enter, exit <= 439, exit >= enter + 4)"));
            }
            break;
        }

        case 'L':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') { Serial.println(F("Malformed sag interval (missing #)")); break; }
            if (cmdValue >= 30 && cmdValue <= 14400) {
                result.action = DL_SAG_INTERVAL_CHANGED;
                result.value = (unsigned long)cmdValue;
            } else {
                Serial.println(F("Invalid sag interval (30-14400 s)"));
            }
            break;

        case 'O':
            i++;
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') { Serial.println(F("Malformed sag override (missing #)")); break; }
            if (cmdValue >= 0 && cmdValue <= 2) {
                result.action = DL_SAG_OVERRIDE;
                result.value = (unsigned long)cmdValue;
            } else {
                Serial.println(F("Invalid sag override (0 = off, 1 = on, 2 = auto)"));
            }
            break;

        case 'U':
            i++;
            if (cmdstr[i] < '0' || cmdstr[i] > '9') { Serial.println(F("Malformed resend (expected /@U<seq>#)")); break; }
            cmdValue = getCmdValue(i, cmdstr, copyLen);
            if (cmdstr[i] != '#') { Serial.println(F("Malformed resend (missing #)")); break; }
            if (cmdValue >= 0 && cmdValue <= 65534) {
                result.action = DL_BUFFER_RESEND;
                result.value = (unsigned long)cmdValue;
            } else {
                Serial.println(F("Invalid resend sequence (0-65534)"));
            }
            break;
        case 'D':
            i++;
            if (cmdstr[i] != '#') { Serial.println(F("Malformed discard (expected /@D#)")); break; }
            result.action = DL_BUFFER_DISCARD;
            break;
#endif // STORAGE strategy commands

        default:
            Serial.print(F("Unknown command: "));
            Serial.println(cmdstr[i]);
            break;
    }

    return result;
}

