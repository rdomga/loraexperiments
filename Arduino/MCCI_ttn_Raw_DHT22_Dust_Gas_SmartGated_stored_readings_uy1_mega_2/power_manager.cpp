#include "power_manager.h"
#include "node_config.h"
#include <avr/wdt.h>

#ifdef LOW_POWER
#if defined(ARDUINO_AVR_PRO) || defined(ARDUINO_AVR_NANO) || defined(ARDUINO_AVR_UNO) || \
    defined(ARDUINO_AVR_MINI) || defined(ARDUINO_AVR_MEGA2560) || defined(__AVR_ATmega32U4__)
#include "LowPower.h"
#endif
#endif

void wdtSafeDelay(unsigned long ms) {
    while (ms > 500UL) {
        delay(500);
        wdt_reset();
        ms -= 500UL;
    }
    if (ms > 0) delay(ms);
    wdt_reset();
}

bool g_pmos1On   = false;
bool g_gasPmosOn = false;

void pmosInit() {
    pinMode(PMOS1_PIN, OUTPUT);
    pinMode(PMOS2_PIN, OUTPUT);
    digitalWrite(PMOS1_PIN, HIGH); // OFF
    digitalWrite(PMOS2_PIN, HIGH); // OFF
}

static void pmos1On()  { digitalWrite(PMOS1_PIN, LOW); }
static void pmos1Off() { digitalWrite(PMOS1_PIN, HIGH); }
static void pmos2On()  { digitalWrite(PMOS2_PIN, LOW); }
static void pmos2Off() { digitalWrite(PMOS2_PIN, HIGH); }

void pmos1Set(bool on) {
    if (on) pmos1On(); else pmos1Off();
    g_pmos1On = on;
}

void gasPowerSet(bool on) {
    if (on) pmos2On(); else pmos2Off();
    g_gasPmosOn = on;
}

bool isPmos1On() {
    return g_pmos1On;
}

bool isGasPmosOn() {
    return g_gasPmosOn;
}

unsigned long applyJitter(unsigned long base_ms, uint8_t percent, unsigned long minJitterMs) {
    long pctJitter = (long)((base_ms / 100UL) * percent);
    long jitterRange = max(pctJitter, (long)minJitterMs);
    long offset = random(-jitterRange, jitterRange + 1);
    long result = (long)base_ms + offset;
    if (result < (long)minJitterMs) result = (long)minJitterMs;
    return (unsigned long)result;
}

void lowPower(unsigned long ms) {
    lowPowerSleep(ms);
}

#ifdef WDT_SCALE_PERMILLE_NODE
#define WDT_DEFAULT_PERMILLE WDT_SCALE_PERMILLE_NODE   // this chip's measured factor (node_calibration.h)
#else
#define WDT_DEFAULT_PERMILLE 1147   // reference chip: no node value, so the boot measurement sets the factor
#endif
const uint16_t g_wdtScaleDefaultPermille = WDT_DEFAULT_PERMILLE;
uint16_t g_wdtScalePermille = WDT_DEFAULT_PERMILLE;
uint8_t  g_wakeCostMs       = 1;
static uint32_t g_sleptNominalMs = 0;
static uint32_t g_sleptRealMs    = 0;
static uint32_t g_sleepWakes     = 0;

static inline void clkAccount(uint16_t nominalMs) {
    g_sleptNominalMs += nominalMs;
    g_sleepWakes++;
    g_sleptRealMs += ((uint32_t)nominalMs * g_wdtScalePermille) / 1000UL + g_wakeCostMs;
}
uint32_t sleepNominalMs()       { return g_sleptNominalMs; }
uint32_t sleepWakeCount()       { return g_sleepWakes; }
uint32_t sleepEstimatedRealMs() { return g_sleptRealMs; }
uint32_t nowMs()                { return millis() + g_sleptRealMs; }

// Uses the LowPower library's own WDT_vect handler (it clears WDIE via wdt_disable()) as the end-of-period marker, so no
// second handler is defined. Call only while the hang watchdog is off (before wdt_enable() in setup()).
uint16_t calibrateWdtScale(uint8_t reps) {
#ifdef LOW_POWER
    uint32_t sumUs = 0; uint8_t good = 0;
    for (uint8_t i = 0; i < reps; i++) {
        const uint8_t unlock = _BV(WDCE) | _BV(WDE);
        const uint8_t cfg    = _BV(WDIE) | _BV(WDP2) | _BV(WDP1);   // interrupt only, 1 s nominal
        cli(); wdt_reset(); MCUSR &= ~_BV(WDRF);
        // both writes back to back: the AVR timed sequence allows 4 cycles between unlock and config
        asm volatile("sts %[r], %[u]\n\tsts %[r], %[c]\n\t" :: [r] "n"(_SFR_MEM_ADDR(WDTCSR)), [u] "r"(unlock), [c] "r"(cfg));
        sei();
        unsigned long t0 = micros();
        while ((WDTCSR & _BV(WDIE)) && (micros() - t0) < 2000000UL) {}
        unsigned long dt = micros() - t0;
        if (!(WDTCSR & _BV(WDIE)) && dt > 900000UL && dt < 1500000UL) { sumUs += dt; good++; }
        else { wdt_disable(); }                                      // timed out or implausible: leave the watchdog off
    }
    wdt_reset();
    return good ? (uint16_t)((sumUs / good) / 1000UL) : 0;
#else
    (void)reps; return 0;
#endif
}

void wdtScaleApplyAwake(uint16_t awakePermille) {
    if (awakePermille < 1050 || awakePermille > 1300) return;       // not plausible for a 128 kHz RC watchdog: keep the default
    g_wdtScalePermille = (uint16_t)(((uint32_t)awakePermille * 1000UL) / WDT_AWAKE_TO_ASLEEP_PERMILLE);
}

void lowPowerSleep(unsigned long ms) {
#ifdef LOW_POWER
    unsigned long remaining = ms;
    Serial.flush();
    while (remaining >= 8000) {
        wdt_reset();
        LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
        clkAccount(8000);
        remaining -= 8000;
    }
    if (remaining >= 4000) {
        wdt_reset();
        LowPower.powerDown(SLEEP_4S, ADC_OFF, BOD_OFF);
        clkAccount(4000);
        remaining -= 4000;
    }
    if (remaining >= 2000) {
        wdt_reset();
        LowPower.powerDown(SLEEP_2S, ADC_OFF, BOD_OFF);
        clkAccount(2000);
        remaining -= 2000;
    }
    if (remaining >= 1000) {
        wdt_reset();
        LowPower.powerDown(SLEEP_1S, ADC_OFF, BOD_OFF);
        clkAccount(1000);
        remaining -= 1000;
    }
    if (remaining >= 500) {
        wdt_reset();
        LowPower.powerDown(SLEEP_500MS, ADC_OFF, BOD_OFF);
        clkAccount(500);
        remaining -= 500;
    }
    if (remaining >= 250) {
        wdt_reset();
        LowPower.powerDown(SLEEP_250MS, ADC_OFF, BOD_OFF);
        clkAccount(250);
        remaining -= 250;
    }
    if (remaining >= 120) {
        wdt_reset();
        LowPower.powerDown(SLEEP_120MS, ADC_OFF, BOD_OFF);
        clkAccount(120);
        remaining -= 120;
    }
    if (remaining >= 60) {
        wdt_reset();
        LowPower.powerDown(SLEEP_60MS, ADC_OFF, BOD_OFF);
        clkAccount(60);
        remaining -= 60;
    }
    if (remaining >= 30) {
        wdt_reset();
        LowPower.powerDown(SLEEP_30MS, ADC_OFF, BOD_OFF);
        clkAccount(30);
        remaining -= 30;
    }
    if (remaining >= 15) {
        wdt_reset();
        LowPower.powerDown(SLEEP_15MS, ADC_OFF, BOD_OFF);
        clkAccount(15);
        remaining -= 15;
    }
    wdt_reset();
    wdt_enable(HANG_WATCHDOG_TIMEOUT);
#else
    wdtSafeDelay(ms);
#endif
}

void smartSleep(unsigned long ms) {
#ifdef LOW_POWER
    lowPowerSleep(ms);
#else
    wdtSafeDelay(ms);
#endif
}

void blinkResetReason(uint8_t mcusr) {
    pinMode(LED_BUILTIN, OUTPUT);
    if (mcusr & _BV(BORF)) {
        for (uint8_t i = 0; i < 10; i++) {
            digitalWrite(LED_BUILTIN, HIGH);
            delay(100);
            digitalWrite(LED_BUILTIN, LOW);
            delay(100);
        }
    } else {
        digitalWrite(LED_BUILTIN, HIGH);
        delay(1500);
        digitalWrite(LED_BUILTIN, LOW);
    }
}

void triggerSoftwareReset() {
    config_mark_intentional_reset();
    Serial.println(F("Software reset requested - restarting"));
    Serial.flush();
    delay(100);
    wdt_enable(WDTO_15MS);
    while (true);
}
