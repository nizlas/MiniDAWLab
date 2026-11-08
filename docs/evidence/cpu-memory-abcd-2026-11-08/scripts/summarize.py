"""Markdown tables from parsed.json: per case the mean of the runs and the range (min–max).
CPU / memory come from the control runs (detail profiling off), callback figures from the profiled
runs. Load figures use all runs of a case (the profiler is not active while loading)."""
import json, os
S = os.path.dirname(os.path.abspath(__file__))
R = [r for r in json.load(open(os.path.join(S, 'parsed.json'))) if r['run'].startswith('r')]
CASES = ['A', 'B', 'C', 'D']
CORES = 24
WIN_EXT = ['never-played', 'playback', 'after-stop 0-20', 'after-stop 30-50']
WIN_APP = {'never-played': 'stopped/never-played', 'playback': 'configured',
           'after-stop 0-20': 'stopped/right-after-stop', 'after-stop 30-50': 'stopped/after-gap'}

def runs(case, mode=None):
    return [r for r in R if r['case'] == case and (mode is None or r['mode'] == mode)]

def stat(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    return (sum(vals) / len(vals), min(vals), max(vals))

def fmt(s, d=2, unit=''):
    if s is None:
        return 'n/a'
    m, lo, hi = s
    return f'{m:.{d}f}{unit} ({lo:.{d}f}–{hi:.{d}f})'

def rel(a, b):
    if a is None or b is None or a[0] == 0:
        return 'n/a'
    return f'{100.0 * (b[0] - a[0]) / a[0]:+.0f} %'

def row(label, f, d=2, unit=''):
    s = {c: f(c) for c in CASES}
    return f"| {label} | " + ' | '.join(fmt(s[c], d, unit) for c in CASES) + f" | {rel(s['A'], s['B'])} | {rel(s['B'], s['C'])} | {rel(s['B'], s['D'])} |"

HDR = '| | A: 8472feb, 261031 | B: 1.4.1, 261031 | C: 1.4.1, 261106 | D: 1.4.1, 261031 Off as C | A→B | B→C | B→D |\n|---|---|---|---|---|---|---|---|'
out = []
out.append('### Load (all four runs per case)\n')
out.append(HDR)
out.append(row('Process start → project loaded (s)', lambda c: stat([r['timeToPlayableS'] for r in runs(c)]), 1))
out.append(row('Load call itself (s)', lambda c: stat([r['loadCallMs'] / 1000 for r in runs(c)]), 1))
out.append(row('Peak private, DAL + helpers, simultaneous (MB), to loaded', lambda c: stat([r['load']['peakPrivTotal'] for r in runs(c)]), 0))
out.append(row('Peak working set, simultaneous (MB), to loaded', lambda c: stat([r['load']['peakWsTotal'] for r in runs(c)]), 0))
out.append(row('Peak private incl. 8 s settle (MB)', lambda c: stat([r['loadPlusSettle']['peakPrivTotal'] for r in runs(c)]), 0))
out.append(row('Peak working set incl. settle (MB)', lambda c: stat([r['loadPlusSettle']['peakWsTotal'] for r in runs(c)]), 0))
out.append(row('CPU during settle after load (cores)', lambda c: stat([r['postLoadCpu']['total'] for r in runs(c)]), 2))
for w in WIN_EXT:
    out.append(f'\n### {w}\n')
    out.append('CPU and memory: control runs (detail profiling off), external sampling, DAL + helpers.\n')
    out.append(HDR)
    out.append(row('CPU total (cores)', lambda c: stat([r['win'][w]['total'] for r in runs(c, 'ctl')]), 3))
    out.append(row('CPU total (% of 24 logical)', lambda c: stat([100 * r['win'][w]['total'] / CORES for r in runs(c, 'ctl')]), 2, ' %'))
    out.append(row('  of which DAL (cores)', lambda c: stat([r['win'][w]['dal'] for r in runs(c, 'ctl')]), 3))
    out.append(row('  of which helpers (cores)', lambda c: stat([r['win'][w]['helpers'] for r in runs(c, 'ctl')]), 3))
    out.append(row('Private total (MB)', lambda c: stat([r['win'][w]['privTotal'] for r in runs(c, 'ctl')]), 0))
    out.append(row('  DAL / helpers private (MB): DAL', lambda c: stat([r['win'][w]['privDal'] for r in runs(c, 'ctl')]), 0))
    out.append(row('  helpers', lambda c: stat([r['win'][w]['privHelpers'] for r in runs(c, 'ctl')]), 0))
    out.append(row('Working set total (MB)', lambda c: stat([r['win'][w]['wsTotal'] for r in runs(c, 'ctl')]), 0))
    out.append(row('CPU total in profiled runs (cores)', lambda c: stat([r['win'][w]['total'] for r in runs(c, 'prof')]), 3))
    a = WIN_APP[w]
    span = ' (in-app window 0–30 s after Stop)' if w == 'after-stop 0-20' else ' (in-app window 30–60 s after Stop)' if w == 'after-stop 30-50' else ''
    out.append(f'\nCallback (profiled runs{span}):\n')
    out.append(HDR)
    g = lambda k: (lambda c: stat([r['app'][a].get(k) for r in runs(c, 'prof')]))
    out.append(row('Callback mean (ms)', g('cbMean'), 3))
    out.append(row('Callback max (ms)', g('cbMax'), 3))
    out.append(row('Budget mean (%)', g('budMean'), 1))
    out.append(row('Budget max (%)', g('budMax'), 1))
    out.append(row('Blocks > 70 % budget', g('near'), 1))
    out.append(row('Overruns (≥ 100 %)', g('over'), 1))
    out.append(row('Late callback starts (> 1.25 period)', g('lateStarts'), 1))
    out.append(row('Callback mean, control runs (ms)', lambda c: stat([r['app'][a].get('cbMean') for r in runs(c, 'ctl')]), 3))
    out.append(row('Callback max, control runs (ms)', lambda c: stat([r['app'][a].get('cbMax') for r in runs(c, 'ctl')]), 3))
print('\n'.join(out))
print('\n### Per-run checks\n')
print('| run | exe | version | device | rate | buffer | workers | playable s | instr loaded | insert instances | live hosts | restingNow (play) | stage rested/processed (play) | instr rested/processed (play) | read-ahead owned / missed (play) | proxy selected / underruns | master peak / RMS L (play) | transport advance (play) | after-stop peak | exit |')
print('|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|')
for r in sorted(R, key=lambda r: r['run']):
    p = r['app'].get('configured', {})
    rd = p.get('restDelta') or {}
    adv = (p.get('ph1', 0) - p.get('ph0', 0)) / (r.get('sr') or 48000) if p.get('ph1') is not None else None
    s = r['app'].get('stopped/right-after-stop', {})
    print(f"| {r['run']} | {r['sha']} | {r.get('version')} | {r.get('device')} | {r.get('sr'):.0f} | {r.get('buf')} | {r.get('workers')} | {r['timeToPlayableS']:.1f} | {r.get('instrLoaded')} | {r.get('insertInstances')} | {p.get('liveHosts')} | {p.get('restingNow')} | {rd.get('rs')}/{rd.get('ps')} | {rd.get('ri')}/{rd.get('pi')} | {p.get('raOwned')} / {p.get('raMissedDelta')} | {p.get('proxySel')} / {p.get('proxyUnder')} | {p.get('peak')} / {p.get('rmsL')} | {adv:.2f} s in {p.get('wall')} s | {s.get('peak')} | {r['end']} |")
