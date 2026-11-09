#!/usr/bin/env python3
"""Summarize controlled USBWarp performance experiments without mixing traces."""
import argparse
import csv
import json
from pathlib import Path
import statistics

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('input',type=Path)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
rows=[json.loads(line) for line in a.input.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
cases=('prefix64m_1m_q1','prefix64m_1m_q4','rand_4k_q1','rand_4k_q4')
summary=[]
for case in cases:
    metric='iops' if case.startswith('rand') else 'mib_s'
    groups={}
    for scenario in ('small','baseline'):
        samples=[r for r in rows if r['label'].startswith('limits_s') and r['scenario']==scenario and r['case']==case]
        if len(samples)!=3 or any(r['traced'] for r in samples): raise RuntimeError(f'Invalid sample set: {case}/{scenario}')
        expected='256' if scenario=='small' else '1024'
        if any(r['queue']['max_sectors_kb']!=expected for r in samples): raise RuntimeError('Wrong request limit')
        groups[scenario]={field:{'median':statistics.median(r[field] for r in samples),
            'minimum':min(r[field] for r in samples),'maximum':max(r[field] for r in samples),
            'runs':[r[field] for r in samples]} for field in (metric,'p50_us','p95_us','p99_us','mean_us')}
    old=groups['small'][metric]['median']; new=groups['baseline'][metric]['median']
    item=dict(case=case,unit='IOPS' if metric=='iops' else 'MiB/s',old_256k=old,new_1m=new,
              gain_percent=100*(new/old-1),samples=groups)
    summary.append(item)
a.out.mkdir(parents=True,exist_ok=True)
(a.out/'limit-summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
with (a.out/'limit-summary.csv').open('w',newline='',encoding='utf-8-sig') as f:
    writer=csv.DictWriter(f,fieldnames=('case','unit','old_256k','new_1m','gain_percent'),extrasaction='ignore')
    writer.writeheader(); writer.writerows(summary)
for r in summary: print(f"{r['case']}: {r['old_256k']:.2f} -> {r['new_1m']:.2f} {r['unit']}, {r['gain_percent']:+.2f}%")
traces=[{k:v for k,v in r.items() if k in ('label','case','queue','wall_seconds','trace')} for r in rows if r.get('traced')]
(a.out/'trace-summary.json').write_text(json.dumps(traces,indent=2),encoding='utf-8')

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
fig,axes=plt.subplots(2,1,figsize=(9,6.4))
for ax,group,metric,unit,title in (
    (axes[0],summary[:2],'mib_s','MiB/s','First 64 MiB cyclic read, 1 GiB total'),
    (axes[1],summary[2:],'iops','IOPS','4 KiB random read — no clear improvement')):
    for offset,key,color,label in ((-.18,'small','#7e8c9a','Old 256 KiB ceiling'),(.18,'baseline','#35976d','Correct 1 MiB ceiling')):
        values=[r['samples'][key][metric]['median'] for r in group]
        errors=[[v-r['samples'][key][metric]['minimum'] for r,v in zip(group,values)],
                [r['samples'][key][metric]['maximum']-v for r,v in zip(group,values)]]
        bars=ax.barh([i+offset for i in range(2)],values,.32,color=color,xerr=errors,capsize=3,label=label)
        ax.bar_label(bars,fmt='%.1f',padding=5)
    ax.set_yticks((0,1),('QD1','QD4')); ax.invert_yaxis(); ax.set_xlabel(unit)
    ax.set_xlim(0,ax.get_xlim()[1]*1.13); ax.set_title(title,loc='left',fontsize=11)
    ax.grid(axis='x',alpha=.18); ax.set_axisbelow(True); ax.spines[['top','right']].set_visible(False)
fig.suptitle('USBWarp request-size limit — controlled A/B',fontsize=14)
handles,labels=axes[0].get_legend_handles_labels()
fig.legend(handles,labels,loc='upper center',bbox_to_anchor=(.5,.95),ncol=2,frameon=False)
fig.text(.5,.01,'Same capture/session and baseline host; adaptive polling unchanged.\n'
         'Median of 3 interleaved runs; error bars show observed min–max. Read-only, direct I/O.',ha='center',fontsize=8)
fig.tight_layout(rect=(0,.065,1,.88))
fig.savefig(a.out/'limit-comparison.png',dpi=180)
fig.savefig(a.out/'limit-comparison.svg')
