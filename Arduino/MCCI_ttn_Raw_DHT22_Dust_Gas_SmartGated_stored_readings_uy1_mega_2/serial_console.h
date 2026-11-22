#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <Arduino.h>
#include "DownlinkParser.h"

#define SERIAL_CMD_BUFFER_SIZE 28

int freeRam();
void printVerboseStatus();
bool serialConsole_poll(DownlinkResult& outResult);

#endif // SERIAL_CONSOLE_H
