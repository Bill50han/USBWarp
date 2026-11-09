#!/usr/bin/env python3
"""Read-only USB experiments; queue changes and trace probes are restored."""
import argparse
import collections
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from usb_read_test import identify

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--label', required=True)
p.add_argument('--scenario', choices=('baseline','small','none','large','large-none','poll1','poll10','poll50','poll100','irq','polling','busy100','busy200','busy500','bulk1000'), default='baseline')
p.add_argument('--case', default='prefix64m_1m_q4')
p.add_argument('--trace', action='store_true')
a = p.parse_args()
deadline=time.monotonic()+15
while True:
    try:
        disk=identify('346d','5678','FC131A137FFE8')
        break
    except RuntimeError:
        if time.monotonic()>deadline: raise
        time.sleep(.2)
block=Path('/sys/class/block')/disk.name
if int((block/'size').read_text())*512 != 62914560000:
    raise RuntimeError('USB capacity mismatch')
queue=block/'queue'
def selected(path):
    s=path.read_text().strip()
    match=re.search(r'\[([^]]+)\]',s)
    return match.group(1) if match else s
settings={}
def change(path,value):
    if path not in settings: settings[path]=selected(path)
    path.write_text(str(value))
def metadata():
    return {key:(queue/key).read_text().strip() for key in
            ('max_sectors_kb','max_hw_sectors_kb','max_segments','max_segment_size','scheduler','nr_requests')}

def rx_counters():
    status=Path('/sys/kernel/debug/usbwarp/ring_status')
    text=status.read_text() if status.exists() else ''
    counters={}
    for line in text.splitlines():
        if line.startswith(('irq:','rx:','repoll:','busy:')):
            prefix=line.split(':',1)[0]
            for key,value in re.findall(r'(\w+)=([0-9]+)(?: |$)',line):
                counters[prefix+'_'+key]=int(value)
    return counters,text

def cpu_ticks():
    return [int(x) for x in Path('/proc/stat').read_text().splitlines()[0].split()[1:]]
def processes():
    found={}
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        try:
            name=(proc/'comm').read_text().strip()
            if name=='usbwarp-poll' or name.startswith('usb-storage'):
                found[int(proc.name)]=(name,[int(x) for x in (proc/'schedstat').read_text().split()])
        except FileNotFoundError: pass
    return found
def distribution(values):
    if not values: return dict(count=0)
    values=sorted(values)
    return dict(count=len(values),mean=sum(values)/len(values),p50=statistics.median(values),
                p95=values[int((len(values)-1)*.95)],p99=values[int((len(values)-1)*.99)],maximum=values[-1])

class Trace:
    def __init__(self):
        self.root=Path('/sys/kernel/tracing')
        self.instance=self.root/'instances'/'usbwarp_perf'
        self.active=False
    def start(self):
        if self.instance.exists():
            raise RuntimeError('Refusing to replace an existing trace instance')
        self.instance.mkdir()
        try:
            (self.instance/'trace_clock').write_text('mono')
            (self.instance/'buffer_size_kb').write_text('16384')
            # Global kprobe controls are unavailable in this WSL namespace.
            # Existing sched tracepoints expose sleep and runqueue intervals
            # without changing global probe/tracer state.
            dev=os.stat(disk).st_rdev
            event=self.instance/'events'/'block'/'block_rq_issue'
            (event/'filter').write_text(f'dev == {(os.major(dev)<<20)|os.minor(dev)}')
            (event/'enable').write_text('1')
            for event_name,expression in (
                ('sched_switch','prev_comm == "usbwarp-poll" || next_comm == "usbwarp-poll"'),
                ('sched_wakeup','comm == "usbwarp-poll"')):
                event=self.instance/'events'/'sched'/event_name
                (event/'filter').write_text(expression); (event/'enable').write_text('1')
            (self.instance/'tracing_on').write_text('1'); self.active=True
        except Exception:
            self.close()
            raise
    def finish(self):
        (self.instance/'tracing_on').write_text('0'); self.active=False
        data=(self.instance/'trace').read_text()
        # Trace includes only scheduling, request sizes/LBAs and sleep timing;
        # it never contains USB payload or user disk contents.
        out=Path(__file__).resolve().parent/(a.label+'-trace.txt')
        out.write_text(data)
        sleeps=[]; wake_delays=[]; sizes=collections.Counter()
        wake={}; running={}; sleeping={}; cpu_us=0.; pids=set()
        for line in data.splitlines():
            m=re.search(r'\s(\d+\.\d+):\s+(\w+):\s+(.*)',line)
            if not m: continue
            stamp=float(m[1]); event=m[2]; body=m[3]
            if event=='sched_wakeup':
                pid=int(re.search(r'pid=(\d+)',body)[1]); wake[pid]=stamp; pids.add(pid)
                if pid in sleeping: sleeps.append((stamp-sleeping.pop(pid))*1e6)
            elif event=='sched_switch':
                if 'prev_comm=usbwarp-poll ' in body:
                    pid=int(re.search(r'prev_pid=(\d+)',body)[1])
                    if pid in running: cpu_us+=(stamp-running.pop(pid))*1e6
                    state=re.search(r'prev_state=(\S+)',body)[1]
                    if state.startswith('D'): sleeping[pid]=stamp
                if 'next_comm=usbwarp-poll ' in body:
                    pid=int(re.search(r'next_pid=(\d+)',body)[1]); running[pid]=stamp; pids.add(pid)
                    if pid in wake: wake_delays.append((stamp-wake.pop(pid))*1e6)
            elif event=='block_rq_issue':
                m=re.match(r'\d+,\d+\s+\S+\s+\d+\s+\(.*?\)\s+\d+\s+\+\s+(\d+)',body)
                if m: sizes[str(int(m[1])*512)]+=1
        overruns={d.name:(d/'stats').read_text() for d in (self.instance/'per_cpu').iterdir() if d.is_dir()}
        return dict(sleep_us=distribution(sleeps),
                    wake_to_run_us=distribution(wake_delays),request_bytes=dict(sizes),buffer_stats=overruns,
                    poller_pids=sorted(pids),poller_cpu_seconds=cpu_us/1e6,
                    sleep_source='sched switch-out D to timer wakeup')
    def close(self):
        if self.instance.exists():
            (self.instance/'tracing_on').write_text('0')
            (self.instance/'events'/'enable').write_text('0')
            self.instance.rmdir()

trace=None
try:
    if a.scenario in ('small','large','large-none'):
        settings[block/'device'/'max_sectors']=selected(block/'device'/'max_sectors')
        settings[queue/'max_sectors_kb']=selected(queue/'max_sectors_kb')
        change(block/'device'/'max_sectors',512 if a.scenario=='small' else 2048)
        change(queue/'max_sectors_kb',256 if a.scenario=='small' else 1024)
    if a.scenario in ('none','large-none'): change(queue/'scheduler','none')
    if a.scenario in ('irq','polling','busy100','busy200','busy500','bulk1000'):
        params=Path('/sys/module/usbwarp/parameters')
        for key,value in (('irq_affinity',1),('irq_softirq',1),('rx_repoll_us',0),('rx_busy_us',0)):
            if (params/key).exists(): change(params/key,value)
        change(params/'h2g_irq',int(a.scenario!='polling'))
        if a.scenario.startswith('busy') or a.scenario=='bulk1000':
            change(params/'rx_busy_bytes',1048576 if a.scenario=='bulk1000' else 128)
            change(params/'rx_busy_us',1000 if a.scenario=='bulk1000' else int(a.scenario[4:]))
        time.sleep(.05)
        if a.scenario!='polling':
            _,ready_text=rx_counters()
            ready=re.search(r'^irq: vector=(-?\d+) host=(\d+) guest=(\d+)',ready_text,re.M)
            if not ready or int(ready[1])<0 or ready.group(2,3)!=('5','5'):
                raise RuntimeError('IRQ comparison requires debug=1, h2g_irq=1 at module load and a ready IRQ route; a polling-only load cannot allocate MSI-X by a runtime parameter write')
    elif a.scenario.startswith('poll'): change(Path('/sys/module/usbwarp/parameters/poll_us'),int(a.scenario[4:]))
    before=processes(); config=metadata()
    counters_before,rx_before=rx_counters(); cpu_before=cpu_ticks()
    if a.trace:
        pids=[pid for pid,(name,_) in before.items() if name=='usbwarp-poll']
        if len(pids)>1: raise RuntimeError(f'Ambiguous poller: {pids}')
        trace=Trace(); trace.start()
    started=time.monotonic()
    run=subprocess.run(['/tmp/usbwarp-read-bench',str(disk),a.label,'FC131A137FFE8','62914560000',a.case],capture_output=True,text=True,timeout=45)
    elapsed=time.monotonic()-started; after=processes()
    if run.returncode: raise RuntimeError(run.stderr)
    result=json.loads(run.stdout)
    counters_after,rx_after=rx_counters(); cpu_after=cpu_ticks()
    delta={key:value-counters_before.get(key,0) for key,value in counters_after.items()}
    ticks=os.sysconf('SC_CLK_TCK')
    result.update(scenario=a.scenario,traced=a.trace,queue=config,wall_seconds=elapsed,
        counter_delta=delta,rx_before=rx_before,rx_after=rx_after,
        guest_busy_cpu_s=sum(cpu_after[i]-cpu_before[i] for i in (0,1,2,5,6))/ticks,
        guest_softirq_cpu_s=(cpu_after[6]-cpu_before[6])/ticks,
        busy_completion_fraction=delta.get('busy_completions',0)/max(1,delta.get('rx_completions',0)))
    result['threads']={name:dict(cpu_seconds=(after[pid][1][0]-values[0])/1e9,
        runqueue_seconds=(after[pid][1][1]-values[1])/1e9,
        switches=after[pid][1][2]-values[2]) for pid,(name,values) in before.items() if pid in after}
    if trace: result['trace']=trace.finish()
finally:
    try:
        if trace: trace.close()
    finally:
        # Restore the hardware ceiling BEFORE the software limit. Restoring
        # 1024 KiB while the temporary ceiling is 256 KiB is rejected by sysfs.
        hardware=block/'device'/'max_sectors'
        if hardware in settings:
            hardware.write_text(settings.pop(hardware))
        for path,value in reversed(list(settings.items())): path.write_text(value)
print(json.dumps(result),flush=True)
