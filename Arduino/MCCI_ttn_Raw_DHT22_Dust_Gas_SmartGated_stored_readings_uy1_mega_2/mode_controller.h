#ifndef MODE_CONTROLLER_H
#define MODE_CONTROLLER_H

#include <Arduino.h>
#include "build_config.h"

void setOperatingMode(uint8_t newMode);
void exitAlwaysOnIfActive();

#endif // MODE_CONTROLLER_H
