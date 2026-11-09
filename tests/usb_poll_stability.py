#!/usr/bin/env python3
"""Read-only stability checks for the explicitly authorized Win11Debug USB disk."""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from usb_read_test import identify

SERIAL = 'FC131A137FFE8'
CAPACITY = 62914560000
PREFIX_BYTES = 64 * 1024 * 1024
# Independent Windows baseline after the bounded 2026-11-09 file-write test.
PREFIX_SHA256 = '1baa61091127126000986ce692898179fc0b77c0dcece14f0e75b37d8391ee58'
BENCH = '/tmp/usbwarp-read-bench'
PARAMS = Path('/sys/module/usbwarp/parameters')
STATUS = Path('/sys/kernel/debug/usbwarp/ring_status')


def emit(event, **fields):
    print(json.dumps(dict(event=event, **fields)), flush=True)


def snapshot():
    params = {name: (PARAMS / name).read_text().strip()
              for name in ('h2g_irq', 'poll_us', 'rx_busy_us', 'rx_repoll_us')}
    assert params['h2g_irq'] in ('N', '0') and all(params[n] == '0' for n in params if n != 'h2g_irq'), params
    text = STATUS.read_text()
    groups = {}
    for line in text.splitlines():
        if ':' not in line:
            continue
        name, body = line.split(':', 1)
        if name == 'irq_identity':
            groups[name] = dict(re.findall(r'(\w+)=([0-9a-f]+)(?: |$)', body))
            continue
        groups[name] = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)(?: |$)', body)}
    assert groups['irq']['vector'] == -1
    assert groups['irq']['received'] == groups['irq']['sent'] == 0
    assert groups['busy']['windows'] == groups['repoll']['arms'] == 0
    memory = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        if key in ('MemAvailable', 'Slab', 'SUnreclaim', 'SReclaimable', 'PageTables', 'KernelStack'):
            memory[key] = int(value.split()[0])
    return dict(parameters=params, transport=groups, memory_kb=memory)


def drained():
    deadline = time.monotonic() + 5
    while True:
        state = snapshot()
        p = state['transport']['inflight']
        if p['urbs'] == p['queued'] == p['iso'] == 0 and p['free'] == p['bitmap_free'] == p['slots']:
            return state
        if time.monotonic() > deadline:
            raise RuntimeError(f'URB/shared-slot resources did not drain: {p}')
        time.sleep(.02)


def target():
    deadline = time.monotonic() + 15
    while True:
        try:
            disk = identify('346d', '5678', SERIAL)
            break
        except RuntimeError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.1)
    assert int((Path('/sys/class/block') / disk.name / 'size').read_text()) * 512 == CAPACITY
    return disk


def benchmark(disk, label, case):
    run = subprocess.run([BENCH, str(disk), label, SERIAL, str(CAPACITY), case],
                         capture_output=True, text=True, timeout=45)
    if run.returncode:
        raise RuntimeError(run.stderr.strip())
    row = json.loads(run.stdout)
    if case != 'fingerprint':
        assert row['read_only'] and row['direct'] and row['ops'] > 0
    return row


def verify(disk, label, expected_fingerprint=None):
    started = time.monotonic()
    digest = hashlib.sha256()
    fd = os.open(disk, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECT)
    try:
        with mmap.mmap(-1, 1024 * 1024) as buffer:
            with memoryview(buffer)[:4096] as first:
                before = time.monotonic_ns()
                if os.preadv(fd, [first], 0) != 4096:
                    raise RuntimeError('Short first raw read')
                first_read_us = (time.monotonic_ns() - before) / 1000
                first_bytes = bytes(first)
            for offset in range(0, PREFIX_BYTES, len(buffer)):
                count = os.preadv(fd, [buffer], offset)
                if count != len(buffer):
                    raise RuntimeError(f'Short raw read: {count}')
                if offset == 0 and buffer[:4096] != first_bytes:
                    raise RuntimeError('First-read data changed during verification')
                digest.update(buffer)
    finally:
        os.close(fd)
    actual = digest.hexdigest()
    assert actual == PREFIX_SHA256, actual
    result = dict(prefix_bytes=PREFIX_BYTES, sha256=actual,
                  seconds=time.monotonic() - started, first_4k_read_us=first_read_us)
    if expected_fingerprint:
        row = benchmark(disk, label, 'fingerprint')
        assert row['hash'] == expected_fingerprint, (row, expected_fingerprint)
        result['range_fingerprint'] = row
    return result


def active(disk, label, marker):
    marker = Path(marker)
    child = None
    try:
        while True:
            child = subprocess.Popen([BENCH, str(disk), label, SERIAL, str(CAPACITY), 'rand_4k_q4'],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(.25)
            if child.poll() is not None:
                out, error = child.communicate()
                raise RuntimeError('Reader ended before fault injection: ' + error)
            marker.write_text(str(child.pid))
            out, error = child.communicate(timeout=45)
            if child.returncode:
                sys.stderr.write(error)
                return child.returncode
            emit('active_interval', benchmark=json.loads(out))
    finally:
        if child and child.poll() is None:
            child.terminate()
            child.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('snapshot', 'verify', 'soak', 'idle', 'active'), required=True)
    parser.add_argument('--seconds', type=int, default=600)
    parser.add_argument('--fingerprint')
    parser.add_argument('--marker')
    parser.add_argument('--label', default='poll-stability')
    args = parser.parse_args()
    if args.seconds < 1 or args.seconds > 86400:
        parser.error('seconds must be 1..86400')
    if args.fingerprint and not re.fullmatch('[0-9a-f]{16}', args.fingerprint):
        parser.error('Invalid independent fingerprint')
    if args.mode == 'snapshot':
        emit('drained', label=args.label, state=drained())
        return 0
    disk = target()
    snapshot()
    if args.mode == 'active':
        if not args.marker:
            parser.error('active requires an owned marker path')
        return active(disk, args.label, args.marker)
    if args.mode == 'verify':
        emit('verified', label=args.label, data=verify(disk, args.label, args.fingerprint), state=drained())
        return 0
    if args.mode == 'idle':
        before = drained()
        time.sleep(args.seconds)
        emit('idle_resume', label=args.label, idle_seconds=args.seconds, before=before,
             data=verify(disk, args.label, args.fingerprint), after=drained())
        return 0
    started = time.monotonic()
    checked = verify(disk, args.label, args.fingerprint)
    emit('soak_start', data=checked, state=drained())
    rounds = 0
    total_bytes = 0
    cases = ('seq_1m_q4', 'rand_4k_q4', 'seq_64k_q1', 'rand_4k_q1')
    while time.monotonic() - started < args.seconds:
        row = benchmark(disk, args.label + '-' + str(rounds + 1), cases[rounds % len(cases)])
        total_bytes += row['bytes']
        rounds += 1
        emit('soak_sample', round=rounds, elapsed=time.monotonic() - started,
             benchmark=row, state=drained())
        if rounds % 8 == 0:
            emit('soak_integrity', round=rounds, data=verify(disk, args.label, args.fingerprint), state=drained())
    emit('soak_complete', rounds=rounds, seconds=time.monotonic() - started,
         benchmark_bytes=total_bytes, data=verify(disk, args.label, args.fingerprint), state=drained())
    return 0


if __name__ == '__main__':
    sys.exit(main())
