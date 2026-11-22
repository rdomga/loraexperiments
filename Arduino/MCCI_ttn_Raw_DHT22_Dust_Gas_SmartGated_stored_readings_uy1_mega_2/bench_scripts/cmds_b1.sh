#!/bin/bash
D=$HOME/millis_diag; LOG=$D/serial.log; OUT=$HOME/cmds_b1.out
since(){ awk -v t="$1" '$1+0>=t' $LOG; }
send_until(){ local cmd="$1" pat="$2" to="$3" T; T=$(date +%s)
  while [ $(( $(date +%s) - T )) -lt "$to" ]; do
    "$D/mega.sh" send "$cmd" >/dev/null; sleep 1.5
    since $T | grep -a -q -- "$pat" && { since $T | grep -a -- "$pat" | head -1 | cut -c1-150 | tee -a $OUT; echo "PASS $cmd" | tee -a $OUT; return 0; }
  done; echo "FAIL $cmd (no '$pat')" | tee -a $OUT; return 1; }
: > $OUT
"$D/mega.sh" reset >/dev/null; sleep 25
send_until '/@H380,395#' 'Night thresholds -> enter<380 exit>=395' 90
send_until '/@L120#' 'Night interval -> 120' 90
send_until '/@O1#' 'Night override -> ' 90
send_until '/@O0#' 'Night override -> ' 90
send_until '/@O2#' 'Night override -> ' 90
send_until '/@H375,388#' 'Night thresholds -> enter<375 exit>=388' 90
send_until '/@L60#' 'Night interval -> 60' 90
T=$(date +%s)
send_until '/@U5#' 'Night resend:' 90
for i in $(seq 1 40); do since $T | grep -a -q "unsent left 0" && break; sleep 5; done
since $T | grep -a -E "Night resend|flushing|marked" | cut -c1-120 | tee -a $OUT
echo CMDS DONE | tee -a $OUT
