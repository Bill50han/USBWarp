#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# run_fuzz.sh — Layer 2 fuzz test runner
#
# Usage:
#   sudo ./run_fuzz.sh          # normal run
#   sudo ./run_fuzz.sh -v       # verbose (show each injection)

set -e

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
FUZZ_BIN="$SCRIPT_DIR/ring_fuzz"
INJECT_PATH="/sys/kernel/debug/usbwarp/inject_g2h"
STATUS_PATH="/sys/kernel/debug/usbwarp/ring_status"

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  UsbWarp Layer 2 Fuzz Test Runner                           ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

# Check prerequisites
if [ "$(id -u)" != "0" ]; then
    echo "ERROR: Must run as root (debugfs requires root access)."
    exit 1
fi

if ! lsmod | grep -q usbwarp; then
    echo "ERROR: usbwarp.ko not loaded."
    echo "  Run: sudo insmod usbwarp.ko debug=1"
    exit 1
fi

if [ ! -e "$INJECT_PATH" ]; then
    echo "ERROR: debugfs injection interface not found at $INJECT_PATH"
    echo "  Reload with debug=1:"
    echo "    sudo rmmod usbwarp"
    echo "    sudo insmod usbwarp.ko debug=1"
    exit 1
fi

# Build if needed
if [ ! -x "$FUZZ_BIN" ] || [ "$SCRIPT_DIR/ring_fuzz.c" -nt "$FUZZ_BIN" ]; then
    echo "Building ring_fuzz..."
    gcc -O2 -Wall -Wextra -Wno-unused-function \
        -I"$SCRIPT_DIR/../include" \
        -o "$FUZZ_BIN" "$SCRIPT_DIR/ring_fuzz.c"
    echo "Build OK."
fi

# Save pre-test kernel log position
DMESG_BEFORE=$(dmesg | wc -l)

PAUSE_PATH="/sys/kernel/debug/usbwarp/pause_g2h"

# Pause G2H production (prevent SPSC violation)
if [ -e "$PAUSE_PATH" ]; then
    echo "Pausing Guest G2H production..."
    echo 1 > "$PAUSE_PATH"
    sleep 0.5  # let in-flight heartbeats drain
fi

# Run fuzz
echo ""
"$FUZZ_BIN" "$@"

# Resume G2H production
if [ -e "$PAUSE_PATH" ]; then
    echo "Resuming Guest G2H production..."
    echo 0 > "$PAUSE_PATH"
fi

# Check for kernel errors after test
echo "Checking for kernel errors..."
DMESG_AFTER=$(dmesg | wc -l)
NEW_LINES=$((DMESG_AFTER - DMESG_BEFORE))

if [ "$NEW_LINES" -gt 0 ]; then
    ERRORS=$(dmesg | tail -"$NEW_LINES" | grep -ciE 'oops|panic|bug|error|fault|invalid' || true)
    if [ "$ERRORS" -gt 0 ]; then
        echo ""
        echo "  ⚠️  Found $ERRORS potential issues in dmesg:"
        dmesg | tail -"$NEW_LINES" | grep -iE 'oops|panic|bug|error|fault|invalid' | head -10
        echo ""
    else
        echo "  ✅ No kernel errors detected in dmesg."
    fi
fi

echo ""
echo "Check Host side:"
echo "  1. WinDbg console — any break-ins or exceptions?"
echo "  2. Service log — still polling? (look for recent poll line)"
echo "  3. If Host is alive and service is polling → Layer 2 PASSED"
echo ""
