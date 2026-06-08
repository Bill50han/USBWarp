#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
pico_helper.py — Persistent MicroPython REPL helper for Layer 3 tests.

Keeps the serial port open for the entire test session.  Accepts
commands on stdin (one per line), sends them to the Pico, and prints
the result on stdout.

Protocol:
  stdin:   "EVAL 123+456"     → send print(123+456) to Pico, print result
  stdin:   "RAW print(42)"    → send raw line to Pico, print all output
  stdin:   "PING"             → verify connection is alive
  stdin:   "QUIT"             → close and exit
  stdout:  "OK <result>"      → success
  stdout:  "ERR <message>"    → failure

Usage:
  echo "EVAL 1+1" | python3 pico_helper.py /dev/ttyACM0
  # Output: OK 2
"""

import serial
import sys
import time
import os

def main():
    if len(sys.argv) < 2:
        print("Usage: pico_helper.py <device> [baud]", file=sys.stderr)
        sys.exit(1)

    device = sys.argv[1]
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

    try:
        ser = serial.Serial(device, baud, timeout=2)
    except Exception as e:
        print(f"ERR cannot open {device}: {e}")
        sys.exit(1)

    # Initial sync: send Ctrl-C to break any running program,
    # then clear the input buffer.
    time.sleep(0.3)
    ser.write(b'\x03\x03')
    time.sleep(0.3)
    ser.read(ser.in_waiting or 1)
    time.sleep(0.2)
    ser.read(ser.in_waiting or 1)

    # Verify we get a REPL prompt
    ser.write(b'\r\n')
    time.sleep(0.3)
    buf = ser.read(ser.in_waiting or 1).decode(errors='ignore')
    if '>>>' not in buf:
        # Try once more
        ser.write(b'\x03\r\n')
        time.sleep(0.5)
        buf = ser.read(ser.in_waiting or 1).decode(errors='ignore')

    print("OK ready", flush=True)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        if line == "QUIT":
            print("OK bye", flush=True)
            break

        if line == "PING":
            # Quick liveness check
            ser.write(b'print("PONG")\r\n')
            time.sleep(0.3)
            data = ser.read(ser.in_waiting or 1).decode(errors='ignore')
            if 'PONG' in data:
                print("OK PONG", flush=True)
            else:
                print(f"ERR no PONG: {repr(data)}", flush=True)
            continue

        if line.startswith("EVAL "):
            expr = line[5:]
            # Clear any pending data
            if ser.in_waiting:
                ser.read(ser.in_waiting)

            cmd = f"print({expr})\r\n".encode()
            ser.write(cmd)
            time.sleep(0.3)

            # Read response with retry
            data = b""
            for _ in range(10):  # up to 1 more second
                chunk = ser.read(ser.in_waiting or 1)
                if chunk:
                    data += chunk
                if b'\n>>>' in data or b'\r>>>' in data:
                    break
                time.sleep(0.1)

            text = data.decode(errors='ignore')
            # Parse: find the result line (not the echo, not the prompt)
            result = None
            for rline in text.split('\n'):
                rline = rline.strip().replace('\r', '')
                if not rline:
                    continue
                if rline.startswith('>>>'):
                    continue
                if rline == f"print({expr})":
                    continue
                if rline.lstrip('-').replace('.','',1).isdigit():
                    result = rline
                    break

            if result is not None:
                print(f"OK {result}", flush=True)
            else:
                print(f"ERR parse failed: {repr(text)}", flush=True)
            continue

        if line.startswith("RAW "):
            raw_cmd = line[4:]
            if ser.in_waiting:
                ser.read(ser.in_waiting)
            ser.write((raw_cmd + "\r\n").encode())
            time.sleep(0.5)
            data = ser.read(ser.in_waiting or 1).decode(errors='ignore')
            print(f"OK {repr(data)}", flush=True)
            continue

        print(f"ERR unknown command: {line}", flush=True)

    ser.close()

if __name__ == "__main__":
    main()
