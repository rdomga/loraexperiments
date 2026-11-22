#!/bin/bash
D=$HOME/millis_diag; A=$HOME/Documents/Arduino; OUT=$HOME/flash12.out; : > $OUT
echo "board 1 flash (discard build)" | tee -a $OUT
"$D/restore_prod.sh" b1 /dev/serial/by-id/usb-Arduino_Srl_Arduino_Mega_9553034363435121D002-if00 $A/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_uy1_mega_2_nightsaves 2>&1 | tail -n 2 | tee -a $OUT
echo "board 2 flash" | tee -a $OUT
"$D/restore_prod.sh" b2 /dev/serial/by-id/usb-Arduino_Srl_Arduino_Mega_95530343634351310212-if00 $A/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_nightsaves_uy1_mega_1 2>&1 | tail -n 2 | tee -a $OUT
echo "FLASH12 DONE" | tee -a $OUT
