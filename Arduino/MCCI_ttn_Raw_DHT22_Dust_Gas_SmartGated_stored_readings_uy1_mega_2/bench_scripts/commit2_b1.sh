#!/bin/bash
D=$HOME/millis_diag; LOG=$D/serial.log; OUT=$HOME/commit2_b1.out
DEV=/dev/serial/by-id/usb-Arduino_Srl_Arduino_Mega_9553034363435121D002-if00
SK=/home/pi/Documents/Arduino/MCCI_ttn_Raw_DHT22_Dust_Gas_SmartGated_uy1_mega_2_nightsaves
since(){ awk -v t="$1" '$1+0>=t' $LOG; }
send_until(){ local cmd="$1" pat="$2" to="$3" T; T=$(date +%s)
  while [ $(( $(date +%s) - T )) -lt "$to" ]; do
    "$D/mega.sh" send "$cmd" >/dev/null; sleep 1.5
    since $T | grep -a -q -- "$pat" && { since $T | grep -a -- "$pat" | head -1 | cut -c1-150 | tee -a $OUT; return 0; }
  done; echo "FAIL $cmd (no '$pat')" | tee -a $OUT; return 1; }
: > $OUT
echo "2 apply and commit" | tee -a $OUT
send_until '/@V335,360,315,350,380#' 'Battery thresholds updated' 150 || exit 2
send_until '/@CP#' 'Night parameters committed' 150 || exit 3
send_until '/@CP#' 'Parameters committed to EEPROM' 150 || exit 3
send_until '/@P#' 'Night thresholds: enter<375 exit>=388 interval 3600s' 150 || exit 4
send_until '/@P#' 'Battery thresholds: LOW=335 REC=360 CRIT=315 PSAVE=350 PSAVE_REC=380' 150 || exit 4
echo "3 flash production" | tee -a $OUT
"$D/restore_prod.sh" b1 $DEV $SK 2>&1 | tail -n 2 | tee -a $OUT
echo "4 verify committed profile survives reset, then keep transmitting while on USB" | tee -a $OUT
sleep 5
T=$(date +%s); printf "RESET\n" > $D/ctl; sleep 1.6
for r in $(seq 1 8); do printf "SEND /@P#\n" > $D/ctl; sleep 0.3; done; sleep 3
since $T | grep -a -E "Battery thresholds:|Night thresholds|Profile committed|restoring committed|BENCH" | cut -c1-140 | awk '!s[$0]++' | tee -a $OUT
T2=$(date +%s); printf "RESET\n" > $D/ctl; sleep 1.6
for r in $(seq 1 8); do printf "SEND /@O0#\n" > $D/ctl; sleep 0.3; done; sleep 3
since $T2 | grep -a -E "Night override|NIGHT BOOT" | cut -c1-140 | awk '!s[$0]++' | tee -a $OUT
echo "COMMIT2 DONE" | tee -a $OUT
