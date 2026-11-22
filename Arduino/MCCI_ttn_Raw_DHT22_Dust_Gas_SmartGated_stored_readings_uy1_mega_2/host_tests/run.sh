#!/bin/bash
# Builds reading_store.cpp against fake Arduino/EEPROM stubs and runs the host checks.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
cp "$HERE"/test.cpp "$HERE"/Arduino.h "$HERE"/EEPROM.h "$HERE"/power_manager.h "$T"/
mkdir -p "$T/avr" && cp "$HERE"/avr/wdt.h "$T/avr/"
cp "$HERE"/../reading_store.h "$HERE"/../reading_store.cpp "$HERE"/../payload_encoder.h "$T"/
g++ -std=c++14 -O1 -I"$T" -o "$T/t" "$T/test.cpp" "$T/reading_store.cpp"
"$T/t"
python3 -c "import shutil, sys; shutil.rmtree(sys.argv[1], ignore_errors=True)" "$T"
