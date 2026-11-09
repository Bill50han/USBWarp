#!/usr/bin/env python3
"""Summarize paired read-only benchmark JSONL; medians and observed ranges."""
import argparse
import csv
import json
from pathlib import Path
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('inputs', nargs='+', type=Path)
p.add_argument('--out', required=True, type=Path)
a = p.parse_args()
rows = []
for path in a.inputs:
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        if line.strip():
            rows.append(json.loads(line))
seen = set()
for r in rows:
    key = (r['label'], r['case'])
    if key in seen or not r['direct'] or not r['read_only']:
        raise RuntimeError(f'Duplicate/invalid result: {key}')
    seen.add(key)
out = []
for case in dict.fromkeys(r['case'] for r in rows):
    sides = {}
    config = None
    for platform in ('windows', 'linux'):
        samples = [r for r in rows if r['case'] == case and r['platform'] == platform]
        if len(samples) != 3:
            raise RuntimeError(f'{case}/{platform}: expected three independent runs, got {len(samples)}')
        for r in samples:
            params = tuple(r[k] for k in ('block_bytes', 'queue_depth', 'offset_bytes', 'span_bytes', 'warmup_bytes'))
            if config is not None and params != config:
                raise RuntimeError(f'Mismatched workload: {case}')
            config = params
        sides[platform] = {metric: dict(
            median=statistics.median(r[metric] for r in samples),
            minimum=min(r[metric] for r in samples),
            maximum=max(r[metric] for r in samples),
            runs=[r[metric] for r in samples]
        ) for metric in ('mib_s', 'iops', 'mean_us', 'p50_us', 'p95_us', 'p99_us', 'process_cpu_s')}
    metric = 'iops' if case.startswith('rand') else 'mib_s'
    w = sides['windows'][metric]['median']
    l = sides['linux'][metric]['median']
    out.append(dict(case=case, unit='IOPS' if metric=='iops' else 'MiB/s',
                    windows=w, wsl=l, loss_percent=100*(1-l/w),
                    mean_latency_added_us=sides['linux']['mean_us']['median']-sides['windows']['mean_us']['median'],
                    details=sides))
a.out.mkdir(parents=True, exist_ok=True)
(a.out/'summary.json').write_text(json.dumps(out, indent=2), encoding='utf-8')
with (a.out/'summary.csv').open('w', newline='', encoding='utf-8-sig') as f:
    fields = ['case', 'unit', 'windows', 'wsl', 'loss_percent', 'mean_latency_added_us']
    writer = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
    writer.writeheader(); writer.writerows(out)
for r in out:
    print(f"{r['case']:20s}  Windows {r['windows']:9.2f}  WSL {r['wsl']:9.2f} {r['unit']:5s}  loss {r['loss_percent']:6.2f}%")

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator
groups = [([r for r in out if r['case'].startswith('seq')], 'Sustained sequential read (offset 1 GiB)', 'MiB/s'),
          ([r for r in out if r['case'].startswith('prefix')], 'First 64 MiB, cyclic read (1 GiB total)', 'MiB/s'),
          ([r for r in out if r['case'].startswith('rand')], '4 KiB random read (8 GiB window)', 'IOPS')]
groups = [g for g in groups if g[0]]
fig, axes = plt.subplots(len(groups), 1, figsize=(9, 3.1*len(groups)), squeeze=False)
labels = {'seq_1m_q1':'1 MiB / QD1', 'seq_1m_q4':'1 MiB / QD4', 'seq_64k_q1':'64 KiB / QD1',
          'prefix64m_1m_q1':'1 MiB / QD1', 'prefix64m_1m_q4':'1 MiB / QD4',
          'rand_4k_q1':'4 KiB / QD1', 'rand_4k_q4':'4 KiB / QD4'}
for ax, (group, title, unit) in zip(axes[:, 0], groups):
    metric = 'iops' if unit=='IOPS' else 'mib_s'
    for delta, side, color, label in ((-.18, 'windows', '#3577b9', 'Win11Debug Windows native'),
                                      (.18, 'linux', '#db8742', 'Nested WSL + USBWarp')):
        values = [r['details'][side][metric]['median'] for r in group]
        errors = [[v-r['details'][side][metric]['minimum'] for r, v in zip(group, values)],
                  [r['details'][side][metric]['maximum']-v for r, v in zip(group, values)]]
        positions = [i+delta for i in range(len(group))]
        bars = ax.barh(positions, values, .32, color=color, label=label, xerr=errors, capsize=3)
        ax.bar_label(bars, fmt='%.1f', padding=6, fontsize=9)
    ax.set_yticks(range(len(group)), [labels[r['case']] for r in group]); ax.invert_yaxis()
    ax.set_title(title, loc='left', fontsize=11, fontweight='bold')
    ax.set_xlabel(unit); ax.set_xlim(0, ax.get_xlim()[1]*1.12)
    ax.xaxis.set_major_locator(MaxNLocator(6)); ax.grid(axis='x', alpha=.18); ax.set_axisbelow(True)
    ax.spines[['top','right']].set_visible(False)
handles, legend_labels = axes[0, 0].get_legend_handles_labels()
fig.legend(handles, legend_labels, loc='upper center', bbox_to_anchor=(.5,.96), ncol=2, frameon=False, fontsize=9)
fig.suptitle('ASUS adol U300 — matched read-only comparison', fontsize=14)
fig.text(.5, .008, 'Median of 3 runs; bars show observed min–max. OS caches bypassed; hashing excluded.\n'
         'USB 5 Gb/s; Win11Debug 2 vCPU / 12 GiB; x64 Release host drivers; Linux debug=0.',
         ha='center', fontsize=8, color='#444444')
fig.tight_layout(rect=[0, .055, 1, .93])
fig.savefig(a.out/'comparison.png', dpi=180)
fig.savefig(a.out/'comparison.svg')
