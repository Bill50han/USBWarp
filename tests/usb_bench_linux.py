#!/usr/bin/env python3
"""Identify the authorized USB disk, then run the shared native read benchmark."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from usb_read_test import identify

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--label', required=True)
p.add_argument('--case', default='all')
p.add_argument('--metadata', action='store_true')
p.add_argument('--verify', action='store_true')
a = p.parse_args()
deadline = time.monotonic() + 15
while True:
    try:
        disk = identify('346d', '5678', 'FC131A137FFE8')
        break
    except RuntimeError:
        if time.monotonic() >= deadline:
            raise
        time.sleep(.2)
block = Path('/sys/class/block') / disk.name
capacity = int((block / 'size').read_text()) * 512
if capacity != 62914560000:
    raise RuntimeError('Unexpected USB capacity')
if a.metadata:
    def optional(path):
        try:
            return path.read_text().strip()
        except FileNotFoundError:
            return None
    print(json.dumps(dict(
        label=a.label, device=str(disk), kernel=platform.release(), cpus=os.cpu_count(),
        capacity=capacity,
        queue={k: optional(block/'queue'/k) for k in (
            'logical_block_size', 'physical_block_size', 'max_sectors_kb',
            'max_hw_sectors_kb', 'read_ahead_kb', 'scheduler', 'nr_requests')},
        scsi_queue_depth=optional(block/'device'/'queue_depth'),
        parameters={k: optional(Path('/sys/module/usbwarp/parameters')/k)
                    for k in ('poll_us', 'max_pending', 'debug', 'enable_iso')}
    )), flush=True)
elif a.verify:
    script = Path(__file__).resolve().parent.parent / 'usb_read_test.py'
    subprocess.run([sys.executable, str(script), '--vid', '346d', '--pid', '5678',
                    '--serial', 'FC131A137FFE8', '--rounds', '1', '--sha256',
                    '1baa61091127126000986ce692898179fc0b77c0dcece14f0e75b37d8391ee58'], check=True)
else:
    def poll_stats():
        for process in Path('/proc').glob('[0-9]*'):
            try:
                if (process/'comm').read_text().strip() == 'usbwarp-poll':
                    return process.name, [int(v) for v in (process/'schedstat').read_text().split()]
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                pass
        return None, None
    before_id, before = poll_stats()
    result = subprocess.run(['/tmp/usbwarp-read-bench', str(disk), a.label,
                             'FC131A137FFE8', str(capacity), a.case],
                            check=True, capture_output=True, text=True)
    after_id, after = poll_stats()
    for line in result.stdout.splitlines():
        row = json.loads(line)
        if a.case != 'all' and before is not None and before_id == after_id:
            row['guest_poll_cpu_s'] = (after[0] - before[0]) / 1e9
            row['guest_poll_wait_s'] = (after[1] - before[1]) / 1e9
            row['guest_poll_schedules'] = after[2] - before[2]
        print(json.dumps(row), flush=True)
