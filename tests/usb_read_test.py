#!/usr/bin/env python3
"""Compare direct, read-only USB block reads with a separately captured hash."""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import stat
import time


def identify(vid, pid, serial):
    matches = []
    for block in Path('/sys/class/block').iterdir():
        if (block / 'partition').exists():
            continue
        device = (block / 'device').resolve()
        for parent in (device, *device.parents):
            try:
                identity = ((parent / 'idVendor').read_text().strip(),
                            (parent / 'idProduct').read_text().strip(),
                            (parent / 'serial').read_text().strip())
            except FileNotFoundError:
                continue
            if identity == (vid.lower(), pid.lower(), serial):
                matches.append(Path('/dev') / block.name)
            break
    if len(matches) != 1:
        raise RuntimeError(f'Expected one matching USB disk, found {matches}')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vid', required=True)
    parser.add_argument('--pid', required=True)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--sha256', required=True)
    parser.add_argument('--bytes', type=int, default=64 * 1024 * 1024)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    if args.bytes <= 0 or args.bytes % 4096 or args.rounds < 1:
        parser.error('bytes must be positive/aligned to 4096, rounds must be positive')
    path = identify(args.vid, args.pid, args.serial)
    if not stat.S_ISBLK(path.stat().st_mode):
        raise RuntimeError('Target is not a block device')
    capacity = int((Path('/sys/class/block') / path.name / 'size').read_text()) * 512
    if args.bytes > capacity:
        raise RuntimeError('Read range exceeds disk capacity')
    results = []
    # mmap supplies the alignment required by O_DIRECT. Each round reaches the
    # real storage path, rather than reporting a Linux page-cache throughput.
    with mmap.mmap(-1, 1024 * 1024) as buffer:
        for round_number in range(1, args.rounds + 1):
            fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_DIRECT)
            digest = hashlib.sha256()
            started = time.monotonic()
            try:
                offset = 0
                while offset < args.bytes:
                    with memoryview(buffer)[:min(len(buffer), args.bytes - offset)] as view:
                        count = os.preadv(fd, [view], offset)
                        if count <= 0:
                            raise RuntimeError('Unexpected end of device')
                        digest.update(view[:count])
                        offset += count
            finally:
                os.close(fd)
            elapsed = time.monotonic() - started
            actual = digest.hexdigest()
            result = dict(round=round_number, bytes=args.bytes, sha256=actual,
                          seconds=round(elapsed, 3), mib_per_second=round(args.bytes / elapsed / 1048576, 2))
            results.append(result)
            print(json.dumps(result), flush=True)
            if actual.lower() != args.sha256.lower():
                raise RuntimeError('Data mismatch against the independent baseline')
    print(json.dumps(dict(device=str(path), capacity=capacity, passed=len(results))))


if __name__ == '__main__':
    main()
