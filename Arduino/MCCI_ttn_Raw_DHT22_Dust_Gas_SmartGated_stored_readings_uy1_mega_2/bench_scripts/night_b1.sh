#!/bin/bash
# Night-saves bench on board 1 (bench build). Runs on Pi2, detached. Result lines go to $OUT.
D=$HOME/millis_diag; LOG=$D/serial.log; OUT=$HOME/night_b1.out
PROFILE='/@V335,360,315,350,380#'
say(){ echo "$(date +%T) $*" | tee -a $OUT; "$D/mega.sh" note "BENCH: $*" >/dev/null; }
since(){ awk -v t="$1" '$1+0>=t' $LOG; }
# send_until <cmd> <pattern> <timeout_s> : resend cmd every 1.5 s until pattern shows up after the first send
send_until(){ local cmd="$1" pat="$2" to="$3" T; T=$(date +%s)
  while [ $(( $(date +%s) - T )) -lt "$to" ]; do
    "$D/mega.sh" send "$cmd" >/dev/null; sleep 1.5
    since $T | grep -a -q -- "$pat" && return 0
  done; return 1; }
wait_for(){ local pat="$1" to="$2" T="$3"; local S; S=$(date +%s)
  while [ $(( $(date +%s) - S )) -lt "$to" ]; do since $T | grep -a -q -- "$pat" && return 0; sleep 3; done; return 1; }
count(){ since "$1" | grep -a -c -- "$2"; }

: > $OUT
say "PHASE A: day cycle, then force night (/@Y360#)"
T0=$(date +%s)
wait_for "EV_TXCOMPLETE" 120 $T0 && say "A1 day uplink seen" || say "A1 FAIL no uplink in 120 s"
send_until "$PROFILE" "Battery thresholds updated" 90 && say "A2 default profile applied" || say "A2 profile not acked"
TA=$(date +%s)
send_until "/@Y360#" "NIGHT MODE: storing" 200 && say "A3 PASS night mode entered" || { say "A3 FAIL night not entered"; exit 1; }
since $TA | grep -a -E "NIGHT MODE|BENCH: battery" | head -3 | tee -a $OUT

say "PHASE B: wait for 3 saved records, then reset mid-night"
wait_for "Night store: saved seq 2" 400 $TA && say "B1 3 records saved" || say "B1 FAIL records not accumulating"
TX_DURING=$(since $TA | grep -a -c "EV_TXSTART\|TXSTART")
say "B2 tx events during night so far: $TX_DURING"
TB=$(date +%s)
"$D/mega.sh" reset >/dev/null
wait_for "NIGHT BOOT" 60 $TB && say "B3 PASS silent night boot" || say "B3 FAIL no NIGHT BOOT line"
since $TB | grep -a -E "NIGHT BOOT|Night store: boot id|Boot battery ADC|init ping|burst" | head -6 | tee -a $OUT
send_until "/@Y360#" "BENCH: battery ADC set to 360" 120 && say "B4 override re-sent" || say "B4 FAIL"
send_until "$PROFILE" "Battery thresholds updated" 90 && say "B5 profile re-applied after reset"

say "PHASE C: store until 11 records (flush will need >1 frame)"
wait_for "Night store: saved seq 10" 1200 $TA && say "C1 PASS 11+ records stored" || say "C1 FAIL"
since $TB | grep -a -E "Night store: saved seq" | tail -3 | tee -a $OUT
say "C2 transmissions since reset (expect none besides operator): $(since $TB | grep -a -c 'EV_TXSTART')"

say "PHASE D: exit night (/@Y392#) and flush"
TD=$(date +%s)
send_until "/@Y392#" "NIGHT MODE OVER" 400 && say "D1 PASS night over" || { say "D1 FAIL night not over"; }
wait_for "Night store: flushing" 120 $TD && say "D2 flush started" || say "D2 FAIL no flush"
wait_for "unsent left 0" 400 $TD && say "D3 PASS flush complete" || say "D3 FAIL flush incomplete"
since $TD | grep -a -E "NIGHT MODE OVER|Night store: flushing|Night store: marked|Port 3|port 3" | tee -a $OUT

say "PHASE E: critical floor (/@Y300#)"
TE=$(date +%s)
send_until "/@Y300#" "CRITICAL floor" 200 && say "E1 PASS floor entered" || say "E1 FAIL floor not entered"
since $TE | grep -a -E "CRITICAL floor|EV_TXSTART" | head -5 | tee -a $OUT
say "ALL PHASES DONE"
