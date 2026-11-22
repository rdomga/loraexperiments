#ifndef RADIO_FAILSAFE_H
#define RADIO_FAILSAFE_H

#include <Arduino.h>
#include <lmic.h>
#include "build_config.h"
#include "payload_encoder.h"

#define SCG_RX2_DR   DR_SF9
#define SCG_RX2_FREQ 869500000

extern uint8_t SCG_currentDR;
extern uint8_t SCG_currentTXP;
extern uint8_t g_scgChannel;

#define SCG_DATARATE SCG_currentDR
#define SCG_TXPOWER  SCG_currentTXP
#define SCG_CHANNEL  g_scgChannel
#define SCG_ENABLE_DYNAMIC_CHANNEL

#include <single_channel_lmic.h>

// Fallback / known-good RF parameters
extern uint8_t g_radioKnownGoodDR;
extern uint8_t g_radioKnownGoodTXP;
extern uint8_t g_channelKnownGood;

// §3.8 candidate verification
extern bool g_radioVerifyPending;
extern bool g_radioVerifyViaPing;

// §4 dead-man failsafe
#define DEADMAN_PING_RATIO 9
extern uint8_t g_uplinkCountSinceDeadman;
extern uint8_t g_deadmanFailCount;
extern bool g_deadmanPingPending;
extern bool g_deadmanVerifyPending;

// Boot connectivity gate and burst
extern bool g_inInitPhase;
extern uint8_t g_initRetriesLeft;
extern uint8_t bootBurstFramesLeft;
extern unsigned long g_burstIntervalS;
extern bool radioInitialized;

// Port 2 and Ping tracking
extern uint8_t g_lastTxPort;
extern uint8_t g_uplinkCountSincePing;
extern uint8_t g_pingRatio;
extern bool g_pingConfirmedPending;
extern bool g_pingConfirmedDeferred;
extern bool g_linkOnline;

// Rate conversions
uint8_t sfToDr(uint8_t sf);
uint8_t drToSf(uint8_t dr);

// Verification and failsafe actions
void radioFailsafe_requestVerify();
void radioFailsafe_deadmanRollback();
void radioFailsafe_radioOnlyRollback();
bool radioFailsafe_fallbackProbeDue();
void radioFailsafe_onFallbackProbeResult(bool ack);
bool radioFailsafe_checkPeriodicPingDue(bool isExtraTxComplete);
void radioFailsafe_onTxComplete(bool ack, bool lenErr);

enum InitPhaseResult {
    INIT_PHASE_NOT_ACTIVE = 0,
    INIT_PHASE_RETRYING   = 1,
    INIT_PHASE_COMPLETED  = 2
};

void radioFailsafe_startInitPhase();
InitPhaseResult radioFailsafe_handleInitPhase(bool ack, bool lenErr, unsigned long burstIntervalS,
                                              osjob_t* j, osjobcb_t retryCb);

// Ping helpers
uint8_t radioFailsafe_computePingReason();
bool radioFailsafe_armPingTx();

#endif // RADIO_FAILSAFE_H
