#!/usr/bin/env python3
"""One bounded file-write test on the authorized USBWarp lab disk."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from usb_read_test import identify

CAPACITY = 62914560000
SIZE = 16 * 1024 * 1024
PATCH_SIZE = 4096
PATCH_OFFSET = 8 * 1024 * 1024
ORIGINAL = '华硕a豆售后服务条例.pdf'
ORIGINAL_SHA = '5acc2804d9535507b293982131e823a850023ad0725349320312c93a4b1b3ef2'


def command(*args):
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'{args[0]} failed: {result.stderr.strip()}')
    return result.stdout.strip()


def digest_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def write_all(fd, data):
    done = 0
    while done < len(data):
        n = os.write(fd, data[done:])
        if n <= 0:
            raise RuntimeError('Short file write')
        done += n
    return done


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--result', required=True)
    args = parser.parse_args()
    if not re.fullmatch('[0-9a-f]{32}', args.run_id):
        parser.error('run-id must be a fresh UUID in hexadecimal')
    result_path = Path(args.result)
    attempt = result_path.with_suffix('.attempt')
    with attempt.open('x') as stream:
        stream.write(args.run_id)  # Refuse accidental retries of the same write.
    state = dict(run_id=args.run_id, filename=f'USBWarp-write-test-{args.run_id}.bin',
                 size=SIZE, patch_bytes=PATCH_SIZE, patch_offset=PATCH_OFFSET,
                 data_write_budget=SIZE + PATCH_SIZE, successful_write_bytes=0,
                 mounted=False, unmounted=False, exfat_unloaded=False)
    def save():
        result_path.write_text(json.dumps(state, indent=2) + '\n')
    save()
    mountpoint = Path('/mnt') / ('usbwarp-writecheck-' + args.run_id)
    owns_mountpoint = False
    try:
        params = Path('/sys/module/usbwarp/parameters')
        assert (params/'h2g_irq').read_text().strip() in ('0', 'N')
        assert all((params/n).read_text().strip() == '0' for n in ('poll_us', 'rx_busy_us', 'rx_repoll_us'))
        deadline = time.monotonic() + 15
        while True:
            try:
                disk = identify('346d', '5678', 'FC131A137FFE8')
                break
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.1)
        block = Path('/sys/class/block') / disk.name
        assert int((block/'size').read_text()) * 512 == CAPACITY
        chain = (block/'device').resolve()
        assert any((p/'driver').resolve().name == 'usbwarp' for p in (chain, *chain.parents)), 'Not a USBWarp disk'
        partitions = [p for p in Path('/sys/class/block').iterdir()
                      if (p/'partition').exists() and p.resolve().parent == block.resolve()]
        assert len(partitions) == 1
        partition = partitions[0]
        assert int((partition/'partition').read_text()) == 1
        assert int((partition/'start').read_text()) * 512 == 16384
        assert int((partition/'size').read_text()) * 512 == CAPACITY - 16384
        device = '/dev/' + partition.name
        assert command('blkid', '-o', 'value', '-s', 'TYPE', device) == 'exfat'
        present = subprocess.run(['findmnt', '-rn', '-S', device], capture_output=True, text=True)
        assert present.returncode == 1, 'Partition is already mounted or mount lookup failed'
        mountpoint.mkdir(mode=0o700)
        owns_mountpoint = True
        state.update(device=str(disk), partition=device, mountpoint=str(mountpoint))
        command('mount', '-t', 'exfat', '-o', 'ro,noatime,nodev,nosuid,noexec', device, str(mountpoint))
        state['mounted'] = True
        assert digest_file(mountpoint/ORIGINAL) == ORIGINAL_SHA
        state['original_sha256_before'] = ORIGINAL_SHA
        state['directory_before'] = sorted(p.name for p in mountpoint.iterdir())
        command('umount', str(mountpoint))
        state['mounted'] = False
        command('mount', '-t', 'exfat', '-o', 'rw,noatime,nodev,nosuid,noexec', device, str(mountpoint))
        state['mounted'] = True
        path = mountpoint/state['filename']
        planned = bytearray(hashlib.shake_256(('USBWarp/' + args.run_id).encode()).digest(SIZE))
        state['expected_initial_sha256'] = hashlib.sha256(planned).hexdigest()
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        state['created'] = True
        save()
        try:
            began = time.monotonic()
            for offset in range(0, SIZE, 1024 * 1024):
                with memoryview(planned)[offset:offset+1024*1024] as chunk:
                    state['successful_write_bytes'] += write_all(fd, chunk)
            os.fsync(fd)
            state['sequential_write_fsync_seconds'] = time.monotonic() - began
            state['linux_initial_sha256'] = digest_file(path)
            assert state['linux_initial_sha256'] == state['expected_initial_sha256']
            patch = hashlib.shake_256(('USBWarp/patch/' + args.run_id).encode()).digest(PATCH_SIZE)
            planned[PATCH_OFFSET:PATCH_OFFSET+PATCH_SIZE] = patch
            state['expected_final_sha256'] = hashlib.sha256(planned).hexdigest()
            os.lseek(fd, PATCH_OFFSET, os.SEEK_SET)
            began = time.monotonic()
            state['successful_write_bytes'] += write_all(fd, patch)
            os.fsync(fd)
            state['patch_fsync_seconds'] = time.monotonic() - began
        finally:
            os.close(fd)
            save()
        assert state['successful_write_bytes'] == state['data_write_budget']
        assert path.stat().st_size == SIZE
        state['linux_final_sha256'] = digest_file(path)
        assert state['linux_final_sha256'] == state['expected_final_sha256']
        state['original_sha256_after'] = digest_file(mountpoint/ORIGINAL)
        assert state['original_sha256_after'] == ORIGINAL_SHA
        directory = os.open(mountpoint, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        state['passed'] = True
    except Exception as error:
        state['error'] = repr(error)
        raise
    finally:
        try:
            if state['mounted']:
                command('umount', str(mountpoint))
                state['mounted'] = False
            state['unmounted'] = True
            if owns_mountpoint:
                mountpoint.rmdir()
            command('/sbin/rmmod', 'exfat')
            state['exfat_unloaded'] = True
        finally:
            save()
    print(json.dumps(state), flush=True)


if __name__ == '__main__':
    main()
