#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# layer3_test.sh — UsbWarp Layer 3 Stress Test Suite
set -u

DEVICE="/dev/ttyACM0"
BUSID=""
CONTROLLER=""
DURATION_3C=3600
COUNT_3B=50
VERBOSE=0
TEST=""
LOG_DIR="/tmp/usbwarp_layer3_$(date +%Y%m%d_%H%M%S)"
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
HELPER="$SCRIPT_DIR/pico_helper.py"
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
HELPER_PID=0

RED='\033[31m'; GREEN='\033[32m'; YELLOW='\033[33m'
CYAN='\033[36m'; BOLD='\033[1m'; RESET='\033[0m'

log()  { echo -e "${CYAN}[$(date +%H:%M:%S)]${RESET} $*"; }
pass() { echo -e "${GREEN}[PASS]${RESET} $*"; PASS_COUNT=$((PASS_COUNT+1)); }
fail() { echo -e "${RED}[FAIL]${RESET} $*"; FAIL_COUNT=$((FAIL_COUNT+1)); }
skip() { echo -e "${YELLOW}[SKIP]${RESET} $*"; SKIP_COUNT=$((SKIP_COUNT+1)); }
info() { echo -e "       $*"; }

start_helper() {
    [ $HELPER_PID -ne 0 ] && return 0
    [ ! -e "$DEVICE" ] && return 1
    local fin="$LOG_DIR/helper_in" fout="$LOG_DIR/helper_out"
    rm -f "$fin" "$fout"; mkfifo "$fin" "$fout"
    python3 "$HELPER" "$DEVICE" < "$fin" > "$fout" 2>"$LOG_DIR/helper_err.log" &
    HELPER_PID=$!
    exec 7>"$fin"; exec 8<"$fout"
    local resp; read -t 5 resp <&8 || resp="TIMEOUT"
    if [[ "$resp" == OK* ]]; then return 0; fi
    stop_helper; return 1
}

stop_helper() {
    if [ $HELPER_PID -ne 0 ]; then
        echo "QUIT" >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true; exec 8<&- 2>/dev/null || true
        wait $HELPER_PID 2>/dev/null || true; HELPER_PID=0
    fi
    rm -f "$LOG_DIR/helper_in" "$LOG_DIR/helper_out" 2>/dev/null || true
}

helper_eval() {
    echo "EVAL $1" >&7
    local resp; read -t 5 resp <&8 || resp="ERR timeout"
    if [[ "$resp" == OK\ * ]]; then echo "${resp#OK }"; return 0; fi
    echo "TIMEOUT"; return 1
}

wait_for_device()    { local i=0; while [ $i -lt "${1:-15}" ]; do [ -e "$DEVICE" ] && { sleep 0.5; return 0; }; sleep 1; i=$((i+1)); done; return 1; }
wait_for_no_device() { local i=0; while [ $i -lt "${1:-10}" ]; do [ ! -e "$DEVICE" ] && return 0; sleep 1; i=$((i+1)); done; return 1; }
do_bind()   { [ -n "$CONTROLLER" ] && "$CONTROLLER" bind "$BUSID" >/dev/null 2>&1; }
do_unbind() { [ -n "$CONTROLLER" ] && "$CONTROLLER" unbind "$BUSID" >/dev/null 2>&1; }
snapshot()  { { echo "=== $1 $(date) ==="; dmesg|tail -5; grep -E 'Slab|SUnreclaim' /proc/meminfo; echo; } >> "$LOG_DIR/snapshots.log"; }

test_3a() {
    echo ""; echo -e "${BOLD}══ Test 3A: Data Echo (100 computations) ══${RESET}"; echo ""
    if ! start_helper; then fail "3A: Cannot connect to $DEVICE"; return; fi
    local errors=0 success=0 start_time=$SECONDS
    for i in $(seq 1 100); do
        local a=$((RANDOM%1000)) b=$((RANDOM%1000)) expected result
        expected=$((a+b))
        result=$(helper_eval "$a+$b") || result="TIMEOUT"
        if [ "$result" = "$expected" ]; then success=$((success+1))
        else errors=$((errors+1)); info "$i: $a+$b=$expected got '$result'"; fi
        [ $((i%10)) -eq 0 ] && [ $VERBOSE -eq 0 ] && printf "  %3d/100  pass=%d fail=%d\r" "$i" "$success" "$errors"
    done; echo ""
    stop_helper
    local elapsed=$((SECONDS-start_time))
    if [ $errors -eq 0 ]; then pass "3A: 100/100 correct (${elapsed}s)"; else fail "3A: $errors/100 failed"; fi
}

test_3b() {
    echo ""; echo -e "${BOLD}══ Test 3B: Bind/Unbind ($COUNT_3B cycles) ══${RESET}"; echo ""
    if [ -z "$BUSID" ]; then skip "3B: No BUSID (-b)"; return; fi
    local errors=0 success=0 start_time=$SECONDS
    for i in $(seq 1 "$COUNT_3B"); do
        if [ -e "$DEVICE" ]; then stop_helper; do_unbind || true; wait_for_no_device 10 || { errors=$((errors+1)); continue; }; fi
        sleep 1; do_bind || { errors=$((errors+1)); continue; }
        wait_for_device 15 || { errors=$((errors+1)); do_unbind || true; sleep 2; continue; }
        sleep 1
        if start_helper; then
            local a=$((RANDOM%100)) b=$((RANDOM%100)) expected result
            expected=$((a+b)); result=$(helper_eval "$a+$b") || result="TIMEOUT"; stop_helper
            if [ "$result" = "$expected" ]; then success=$((success+1)); else errors=$((errors+1)); info "$i: verify fail"; fi
        else errors=$((errors+1)); info "$i: connect fail"; fi
        [ $VERBOSE -eq 0 ] && printf "  %3d/%-3d  pass=%d fail=%d\r" "$i" "$COUNT_3B" "$success" "$errors"
        [ $((i%10)) -eq 0 ] && snapshot "3B #$i"
    done; echo ""
    do_unbind 2>/dev/null || true
    if [ $errors -eq 0 ]; then pass "3B: $COUNT_3B/$COUNT_3B OK ($((SECONDS-start_time))s)"; else fail "3B: $errors failures"; fi
}

test_3c() {
    local dm=$((DURATION_3C/60))
    echo ""; echo -e "${BOLD}══ Test 3C: Sustained Load (${dm}m) ══${RESET}"; echo ""
    if ! start_helper; then fail "3C: Cannot connect"; return; fi
    local errors=0 success=0 iteration=0 start_time=$SECONDS cfails=0 maxcf=0
    local islab; islab=$(grep 'SUnreclaim' /proc/meminfo|awk '{print $2}')
    snapshot "3C start"; log "Running ${dm}m (Ctrl+C to abort)..."
    local aborted=0; trap 'aborted=1' INT
    while [ $((SECONDS-start_time)) -lt "$DURATION_3C" ] && [ $aborted -eq 0 ]; do
        iteration=$((iteration+1))
        local a=$((RANDOM%10000)) b=$((RANDOM%10000)) expected result
        expected=$((a+b)); result=$(helper_eval "$a+$b") || result="TIMEOUT"
        if [ "$result" = "$expected" ]; then success=$((success+1)); cfails=0
        else errors=$((errors+1)); cfails=$((cfails+1)); [ $cfails -gt $maxcf ] && maxcf=$cfails
            [ $cfails -ge 10 ] && { fail "3C: 10 consecutive fails"; break; }; fi
        [ $((iteration%30)) -eq 0 ] && printf "  %5d iter %dm%02ds pass=%d fail=%d\r" \
            "$iteration" $(((SECONDS-start_time)/60)) $(((SECONDS-start_time)%60)) "$success" "$errors"
        [ $((iteration%60)) -eq 0 ] && snapshot "3C t=$((SECONDS-start_time))s"
        sleep 1
    done; trap - INT; echo ""
    stop_helper
    local elapsed=$((SECONDS-start_time)) fslab; fslab=$(grep 'SUnreclaim' /proc/meminfo|awk '{print $2}')
    local sdelta=$((fslab-islab)); snapshot "3C end"
    echo "  ${elapsed}s  iter=$iteration  pass=$success  fail=$errors  maxcf=$maxcf  slab=${sdelta}kB"
    if [ $errors -eq 0 ]; then pass "3C: $iteration iter, 0 errors, slab Δ=${sdelta}kB"; else fail "3C: $errors errors"; fi
}

test_3d() {
    echo ""; echo -e "${BOLD}══ Test 3D: Abnormal Recovery ══${RESET}"; echo ""
    if [ -z "$BUSID" ]; then skip "3D: No BUSID (-b)"; return; fi
    [ ! -e "$DEVICE" ] && { do_bind || true; wait_for_device 15 || { fail "3D: bind failed"; return; }; sleep 1; }

    log "3D.1: Double bind"
    do_bind 2>/dev/null || true; sleep 2
    if start_helper; then
        local r; r=$(helper_eval "42+0") || r="TIMEOUT"; stop_helper
        [ "$r" = "42" ] && pass "3D.1: OK after double bind" || fail "3D.1: got '$r'"
    else fail "3D.1: connect fail"; fi

    log "3D.2: Double unbind"; stop_helper
    do_unbind || true; wait_for_no_device 5 || true; sleep 1; do_unbind 2>/dev/null || true
    pass "3D.2: No crash from double unbind"

    log "3D.3: 5x rapid bind/unbind"; sleep 1
    for _ in $(seq 1 5); do do_bind 2>/dev/null||true; sleep 0.5; do_unbind 2>/dev/null||true; sleep 0.5; done
    sleep 3; do_bind 2>/dev/null || true
    if wait_for_device 15; then sleep 1
        if start_helper; then r=$(helper_eval "99+1")||r="T"; stop_helper; [ "$r" = "100" ] && pass "3D.3: OK" || fail "3D.3: got '$r'"
        else fail "3D.3: connect fail"; fi
    else fail "3D.3: device missing"; fi

    log "3D.4: Unbind during I/O"
    if [ -e "$DEVICE" ]; then
        python3 -c "
import serial,time
try:
 s=serial.Serial('$DEVICE',115200,timeout=1)
 for i in range(20): s.write(b'print(1)\r\n'); time.sleep(0.2)
 s.close()
except: pass" &
        local bp=$!; sleep 1; do_unbind 2>/dev/null||true; wait $bp 2>/dev/null||true; sleep 2
        do_bind 2>/dev/null||true
        if wait_for_device 15; then sleep 1
            if start_helper; then r=$(helper_eval "7+3")||r="T"; stop_helper; [ "$r" = "10" ] && pass "3D.4: recovered" || fail "3D.4: got '$r'"
            else fail "3D.4: connect fail"; fi
        else fail "3D.4: device missing"; fi
    else skip "3D.4: not bound"; fi

    log "3D.5: 20x serial open/close"
    if [ -e "$DEVICE" ]; then
        python3 -c "
import serial,time
e=0
for i in range(20):
 try: s=serial.Serial('$DEVICE',115200,timeout=0.3); time.sleep(0.1); s.close(); time.sleep(0.1)
 except: e+=1
print(e)" > "$LOG_DIR/3d5.txt" 2>/dev/null; sleep 2
        if start_helper; then r=$(helper_eval "50+50")||r="T"; stop_helper; [ "$r" = "100" ] && pass "3D.5: OK" || fail "3D.5: got '$r'"
        else fail "3D.5: connect fail"; fi
    else skip "3D.5: not bound"; fi

    stop_helper; do_unbind 2>/dev/null || true
}

while getopts "at:d:b:s:c:n:v" opt; do
    case $opt in a)TEST="all";;t)TEST="$OPTARG";;d)DEVICE="$OPTARG";;b)BUSID="$OPTARG";;
        s)CONTROLLER="$OPTARG";;c)DURATION_3C="$OPTARG";;n)COUNT_3B="$OPTARG";;v)VERBOSE=1;;
        *)echo "Usage: $0 [-a|-t 3a|3b|3c|3d] [-b busid] [-s ctrl] [-d dev] [-c sec] [-n N] [-v]";exit 1;;esac; done
[ -z "$TEST" ] && { echo "Usage: $0 -t 3a|3b|3c|3d [-b BUSID] [-s CTRL]"; exit 1; }

mkdir -p "$LOG_DIR"
echo ""; echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  UsbWarp Layer 3 Stress Tests                               ║"
echo "╚══════════════════════════════════════════════════════════════╝"; echo ""
echo "  Device: $DEVICE  BUSID: ${BUSID:-(none)}  Log: $LOG_DIR"; echo ""
snapshot "start"

case "$TEST" in all)test_3a;test_3b;test_3c;test_3d;;3a)test_3a;;3b)test_3b;;3c)test_3c;;3d)test_3d;;
    *)echo "Unknown: $TEST";exit 1;;esac

snapshot "end"; stop_helper; echo ""
echo "══════════════════════════════════════════════════════════════"
echo -e "  Pass:${GREEN}${PASS_COUNT}${RESET} Fail:$([ $FAIL_COUNT -gt 0 ]&&echo -e "${RED}${FAIL_COUNT}${RESET}"||echo -e "${GREEN}${FAIL_COUNT}${RESET}") Skip:${SKIP_COUNT}  Log:$LOG_DIR"
echo "══════════════════════════════════════════════════════════════"
[ $FAIL_COUNT -eq 0 ] && echo -e "  ${GREEN}${BOLD}Layer 3 PASSED${RESET}" || echo -e "  ${RED}${BOLD}Layer 3 FAILED${RESET}"
echo ""; exit $FAIL_COUNT
