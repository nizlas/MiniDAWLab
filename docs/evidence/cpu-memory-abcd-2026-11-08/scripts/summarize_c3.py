"""Old C (2026-11-08 series, runs r?-C-*) against the manually updated C (runs c2-*).
Mean (min–max); CPU / memory from control runs, callback from profiled runs, load from all runs."""
import json, os
S = os.path.dirname(os.path.abspath(__file__))
R = json.load(open(os.path.join(S, 'parsed.json')))
G = {'old': [r for r in R if r['run'].startswith('r') and r['case'] == 'C'],
     'new': [r for r in R if r['run'].startswith('c3-')]}
CORES = 24
APP = {'never-played': 'stopped/never-played', 'playback': 'configured',
       'after-stop 0-20': 'stopped/right-after-stop', 'after-stop 30-50': 'stopped/after-gap'}

def st(v):
    v = [x for x in v if x is not None]
    return (sum(v) / len(v), min(v), max(v)) if v else None

def f(s, d):
    return 'n/a' if s is None else f'{s[0]:.{d}f} ({s[1]:.{d}f}–{s[2]:.{d}f})'

def row(label, fn, d=2):
    o, n = st(fn(G['old'])), st(fn(G['new']))
    delta = 'n/a' if not o or not n or o[0] == 0 else f'{100 * (n[0] - o[0]) / o[0]:+.0f} %'
    overlap = 'overlaps' if o and n and not (n[2] < o[1] or n[1] > o[2]) else 'separate'
    print(f'| {label} | {f(o, d)} | {f(n, d)} | {delta} | {overlap} |')

ctl = lambda rs: [r for r in rs if r['mode'] == 'ctl']
prof = lambda rs: [r for r in rs if r['mode'] == 'prof']
print('| | Old C (2026-11-08, 4 runs) | New C, re-run with the machine quiet (4 runs) | Change | Ranges |')
print('|---|---|---|---|---|')
row('Process start → loaded (s)', lambda rs: [r['timeToPlayableS'] for r in rs], 1)
row('Peak private, simultaneous total (MB)', lambda rs: [r['load']['peakPrivTotal'] for r in rs], 0)
row('Peak working set, simultaneous total (MB)', lambda rs: [r['load']['peakWsTotal'] for r in rs], 0)
row('Peak private incl. settle (MB)', lambda rs: [r['loadPlusSettle']['peakPrivTotal'] for r in rs], 0)
for w, a in APP.items():
    row(f'CPU {w} (cores, control)', lambda rs, w=w: [r['win'][w]['total'] for r in ctl(rs)], 3)
    row(f'CPU {w} (% of 24, control)', lambda rs, w=w: [100 * r['win'][w]['total'] / CORES for r in ctl(rs)], 2)
    row(f'Private {w} (MB, control)', lambda rs, w=w: [r['win'][w]['privTotal'] for r in ctl(rs)], 0)
    row(f'Helpers CPU {w} (cores)', lambda rs, w=w: [r['win'][w]['helpers'] for r in ctl(rs)], 3)
    row(f'CPU {w} (cores, profiled)', lambda rs, w=w: [r['win'][w]['total'] for r in prof(rs)], 3)
    row(f'Callback mean {w} (ms, profiled)', lambda rs, a=a: [r['app'][a]['cbMean'] for r in prof(rs)], 3)
    row(f'Callback max {w} (ms, profiled)', lambda rs, a=a: [r['app'][a]['cbMax'] for r in prof(rs)], 3)
    row(f'Overruns / >70 % blocks {w}', lambda rs, a=a: [r['app'][a]['over'] + r['app'][a]['near'] for r in rs], 0)
    row(f'systemBusy {w} (%)', lambda rs, a=a: [r['app'][a].get('sysBusy') for r in rs], 1)
    row(f'Late starts {w} (profiled)', lambda rs, a=a: [r['app'][a]['lateStarts'] for r in prof(rs)], 0)
    row(f'Read-ahead misses {w}', lambda rs, a=a: [r['app'][a].get('raMissedDelta') for r in rs], 0)
print()
print('| run | exe | device / rate / buffer / workers | instr loaded | insert instances | live hosts | proxy selected / underruns | master peak / RMS (play) | transport (play) | exit |')
print('|---|---|---|---|---|---|---|---|---|---|')
for r in G['old'] + G['new']:
    p = r['app']['configured']
    print(f"| {r['run']} | {r['sha']} | {r['device']} / {r['sr']:.0f} / {r['buf']} / {r['workers']} | {r['instrLoaded']} | {r['insertInstances']} | {p.get('liveHosts')} | {p.get('proxySel')} / {p.get('proxyUnder')} | {p.get('peak')} / {p.get('rmsL')} | {(p['ph1'] - p['ph0']) / r['sr']:.2f} s in {p['wall']} s | {r['end']} |")
