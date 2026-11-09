#!/usr/bin/env python3
"""Read-only lab regression: switch RX consumers while I/O remains in flight."""
import json
from pathlib import Path
import re
import subprocess
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parent.parent))
from usb_read_test import identify

deadline=time.monotonic()+15
while True:
    try:
        disk=identify('346d','5678','FC131A137FFE8'); break
    except RuntimeError:
        if time.monotonic()>deadline: raise
        time.sleep(.2)
params=Path('/sys/module/usbwarp/parameters')
status=Path('/sys/kernel/debug/usbwarp/ring_status').read_text()
route=re.search(r'^irq: vector=(-?\d+) host=(\d+) guest=(\d+)',status,re.M)
if not route or int(route[1])<0 or route.group(2,3)!=('5','5'):
    raise RuntimeError('Live IRQ consumer switching requires a ready route; load with h2g_irq=1 debug=1')
saved={key:(params/key).read_text() for key in ('h2g_irq','irq_softirq','irq_affinity','rx_repoll_us','rx_repoll_count','rx_busy_us','rx_busy_bytes') if (params/key).exists()}
child=None; switches=0
try:
    child=subprocess.Popen(['/tmp/usbwarp-read-bench',str(disk),'rx-live-switch',
        'FC131A137FFE8','62914560000','rand_4k_q4'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    deadline=time.monotonic()+25
    modes=((1,1,0,1,0,128),(1,1,0,1,100,128),(1,1,0,1,200,128),
           (1,1,0,1,500,4096),(1,1,25,8,200,128),(1,1,50,8,0,128),
           (1,0,0,1,200,128),(0,0,0,1,200,128))
    while child.poll() is None:
        if time.monotonic()>deadline: raise TimeoutError('Read stalled during RX consumer switching')
        irq,soft,interval,count,busy_us,busy_bytes=modes[switches % len(modes)]
        if 'rx_repoll_us' in saved:
            (params/'rx_repoll_count').write_text(str(count))
            (params/'rx_repoll_us').write_text(str(interval))
        if 'rx_busy_us' in saved:
            (params/'rx_busy_bytes').write_text(str(busy_bytes))
            (params/'rx_busy_us').write_text(str(busy_us))
        (params/'irq_softirq').write_text(str(soft))
        (params/'h2g_irq').write_text(str(irq))
        switches+=1
        time.sleep(.025)
    output,error=child.communicate()
    if child.returncode: raise RuntimeError(error)
    result=json.loads(output)
    assert result['read_only'] and result['direct'] and result['ops']>0 and switches>40
    print(json.dumps(dict(test='live_consumer_switch',switches=switches,read=result)),flush=True)
finally:
    for key,value in saved.items(): (params/key).write_text(value)
    if child and child.poll() is None:
        child.terminate(); child.wait(timeout=15)
