#!/bin/bash
# Builds the bench folder for board 2 (uy1-mega-1, ABP Option A) from this staged sketch: same code, board 2's credentials
# and calibration. Build flags for the bench: -DNIGHT_BENCH_ADC -DMILLIS_BENCH_TX_S=30  (see Night_Saves_Handoff).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; A="$(dirname "$HERE")"
B="$A/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_nightsaves_bench_mega1"
rm -rf "$B"; mkdir "$B"
rsync -a --exclude host_tests --exclude '*.md' --exclude make_bench_b2.sh --exclude README_NODE.txt "$HERE"/ "$B"/
mv "$B"/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_uy1_mega_2_nightsaves.ino "$B"/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_nightsaves_bench_mega1.ino
cp "$A"/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_Random_uy1_mega_1/credentials.h "$B"/credentials.h
cp "$A"/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_Random_uy1_mega_1/node_calibration.h "$B"/node_calibration.h
echo "built $B"
