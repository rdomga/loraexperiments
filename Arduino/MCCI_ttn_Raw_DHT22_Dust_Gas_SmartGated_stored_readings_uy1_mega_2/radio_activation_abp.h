#ifndef SMARTGATED_RADIO_ACTIVATION_ABP_H
#define SMARTGATED_RADIO_ACTIVATION_ABP_H

// ABP session establishment — the activation-specific half of initRadio().
// Requires credentials.h (NWKSKEY/APPSKEY/DEVADDR) to be included first.
// An eventual radio_activation_otaa.h sibling (join trigger + EV_JOINED
// handling, not built yet — see OTAA_Transition_And_Radio_Modularization_
// Handoff_2026-09-11.md) would be an #include swap for this file, nothing
// else in initRadio() would need to change.

static inline void establishRadioSession() {
    uint8_t appskey[sizeof(APPSKEY)];
    uint8_t nwkskey[sizeof(NWKSKEY)];
    memcpy_P(appskey, APPSKEY, sizeof(APPSKEY));
    memcpy_P(nwkskey, NWKSKEY, sizeof(NWKSKEY));
    LMIC_setSession(0x13, DEVADDR, nwkskey, appskey);
}

#endif
