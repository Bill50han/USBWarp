#!/usr/bin/env python3
"""Win11Debug lab: MSI-X mask/PBA replay and IRQ affinity, read-only USB I/O.

Requires the explicitly authorized ASUS test disk already captured. All PCI
mask and affinity settings are restored in finally, including failed checks.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from usb_read_test import identify

def status():
    text = Path('/sys/kernel/debug/usbwarp/ring_status').read_text()
    line = next(line for line in text.splitlines() if line.startswith('irq:'))
    fields = dict(re.findall(r'(\w+)=([0-9a-f]+)', line))
    return {key: int(value, 16 if key == 'status' else 10) for key, value in fields.items()}

def interrupt_cpus(vector):
    lines = Path('/proc/interrupts').read_text().splitlines()
    count = len(lines[0].split())
    line = next(line for line in lines[1:] if line.strip().startswith(str(vector) + ':'))
    return [int(value) for value in line.split()[1:1 + count]]

def wait_worker_affinity(cpu, active):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        text = Path('/sys/kernel/debug/usbwarp/ring_status').read_text()
        match = re.search(r'^irq_affinity: cpu=(-?\d+) active=(\d+)$', text, re.M)
        if match and (int(match[1]), int(match[2])) == (cpu, active):
            return
        time.sleep(.01)
    raise AssertionError('Completion worker did not follow/restore IRQ affinity')

deadline = time.monotonic() + 15
while True:
    try:
        disk = identify('346d', '5678', 'FC131A137FFE8')
        break
    except RuntimeError:
        if time.monotonic() >= deadline:
            raise
        time.sleep(.2)
assert int((Path('/sys/class/block')/disk.name/'size').read_text()) * 512 == 62914560000
before = status()
assert before['host'] == before['guest'] == 5 and before['failed'] == 0
devices = [p for p in Path('/sys/bus/pci/drivers/usbwarp').glob('*:*')
           if (p/'msi_irqs'/str(before['vector'])).exists()]
assert len(devices) == 1
device = devices[0]
cfg = os.open(device/'config', os.O_RDWR)
cap = os.pread(cfg, 1, 0x34)[0]
assert os.pread(cfg, 1, cap) == b'\x11'
original_control = os.pread(cfg, 2, cap + 2)
control = int.from_bytes(original_control, 'little')
assert control & 0x8000 and not control & 0x4000
affinity = Path('/proc/irq')/str(before['vector'])/'smp_affinity_list'
original_affinity = affinity.read_text()
follow_parameter = Path('/sys/module/usbwarp/parameters/irq_affinity')
irq_parameter = Path('/sys/module/usbwarp/parameters/h2g_irq')
original_follow = follow_parameter.read_text()
original_irq_mode = irq_parameter.read_text()
busy_parameter = Path('/sys/module/usbwarp/parameters/rx_busy_us')
original_busy = busy_parameter.read_text() if busy_parameter.exists() else None
child = None
try:
    # Force the masked replay/watchdog check through the IRQ path. A busy
    # consumer can legitimately drain completions before the watchdog fires.
    # Restore busy polling for the subsequent traffic/affinity checks.
    if original_busy is not None:
        busy_parameter.write_text('0')
    irq_parameter.write_text('1')
    follow_parameter.write_text('1')
    os.pwrite(cfg, (control | 0x4000).to_bytes(2, 'little'), cap + 2)
    time.sleep(.05)  # allow any pre-mask interrupt already in flight to finish
    masked_start = status()
    child = subprocess.Popen(['/tmp/usbwarp-read-bench', str(disk), 'masked-replay',
                              'FC131A137FFE8', '62914560000', 'rand_4k_q1'],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(.25)
    masked = status()
    # Linux reserves MSI-X pages, so sysfs resource mmap may reject them.
    # The diagnostic bit is the exact source of the HDV PBA read callback;
    # its read-only MMIO behavior is covered by service_hdv_test.cpp.
    pending = masked['pending']
    assert pending == 1 and masked['pending'] == 1
    assert masked['received'] == masked_start['received']
    assert masked['watchdog'] > masked_start['watchdog']
    os.pwrite(cfg, original_control, cap + 2)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and status()['received'] == masked['received']:
        time.sleep(.01)
    resumed = status()
    assert resumed['received'] > masked['received'] and resumed['failed'] == 0
    output, error = child.communicate(timeout=25)
    assert child.returncode == 0, error
    row = json.loads(output)
    assert row['read_only'] and row['direct'] and row['ops'] > 0
    print(json.dumps(dict(test='mask_pending_replay', before=before, masked=masked,
                         resumed=resumed, pending=pending, read=row)), flush=True)
    if original_busy is not None:
        busy_parameter.write_text(original_busy)
    # pci-hyperv owns remapping. Never decode the opaque MSI data as a vector.
    cpus = sorted(os.sched_getaffinity(0))
    assert len(cpus) >= 2
    for cpu in cpus[:2]:
        affinity.write_text(str(cpu))
        start = status()
        start_cpus = interrupt_cpus(before['vector'])
        # Generic IRQ migration may remain pending until the next interrupt.
        # Generate real traffic before waiting for effective affinity/worker.
        child = subprocess.Popen(['/tmp/usbwarp-read-bench', str(disk),
            'affinity-' + str(cpu), 'FC131A137FFE8', '62914560000', 'prefix64m_1m_q1'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        wait_worker_affinity(cpu, 1)
        output, error = child.communicate(timeout=25)
        assert child.returncode == 0, error
        after = status()
        after_cpus = interrupt_cpus(before['vector'])
        assert after['received'] > start['received'] and after['failed'] == 0
        assert after['watchdog'] == start['watchdog']
        assert affinity.read_text().strip() == str(cpu)
        assert after_cpus[cpu] > start_cpus[cpu]
        print(json.dumps(dict(test='affinity', cpu=cpu, before=start, after=after,
                              interrupt_cpus_before=start_cpus, interrupt_cpus_after=after_cpus,
                              read=json.loads(output))), flush=True)
    follow_parameter.write_text('0')
    wait_worker_affinity(-1, 0)
    follow_parameter.write_text('1')
    wait_worker_affinity(cpus[1], 1)
    irq_parameter.write_text('0')
    wait_worker_affinity(-1, 0)
    start = status()
    output = subprocess.check_output(['/tmp/usbwarp-read-bench', str(disk),
        'affinity-poll-fallback', 'FC131A137FFE8', '62914560000', 'prefix64m_1m_q1'], text=True)
    after = status()
    assert after['failed'] == 0 and after['received'] <= start['received'] + 1
    irq_parameter.write_text('1')
    wait_worker_affinity(cpus[1], 1)
    print(json.dumps(dict(test='affinity_poll_fallback', before=start, after=after,
                          read=json.loads(output))), flush=True)
finally:
    if original_busy is not None:
        busy_parameter.write_text(original_busy)
    irq_parameter.write_text(original_irq_mode)
    follow_parameter.write_text(original_follow)
    os.pwrite(cfg, original_control, cap + 2)
    affinity.write_text(original_affinity)
    if child and child.poll() is None:
        child.terminate()
        child.wait(timeout=10)
    os.close(cfg)
