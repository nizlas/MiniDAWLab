"""Parse the cmp3 runs: external CPU / memory per window for DAL + helpers, and the in-app engine,
rest, read-ahead, proxy and output lines. Writes per-run rows (JSON) and a summary table."""
import json, os, re, sys
from datetime import datetime, timedelta

S = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(S, 'runs')
CORES = 24

def ts(s):
    return datetime.fromisoformat(s)

def read(p):
    raw = open(p, 'rb').read()
    enc = 'utf-16' if raw[:2] in (b'\xff\xfe', b'\xfe\xff') else 'utf-8-sig'
    return raw.decode(enc, errors='replace').splitlines()

def num(pat, s, f=float):
    m = re.search(pat, s)
    return f(m[1]) if m else None

def samples(run):
    rows = {}
    for l in read(os.path.join(RUNS, run, 'samples.txt')):
        p = l.split()
        if len(p) != 6:
            continue
        t, pid, cpu, priv, ws, pk = ts(p[0]), int(p[1]), float(p[2]), int(p[3]), int(p[4]), int(p[5])
        rows.setdefault(t, {})[pid] = (cpu, priv, ws, pk)
    return sorted(rows.items())

def cpu_at(smp, pid, t):
    """Cumulative CPU seconds of pid at time t, linearly interpolated; 0 before first sight."""
    prev = None
    for st, d in smp:
        if pid in d:
            if st >= t:
                if prev is None:
                    return d[pid][0] if st - t < timedelta(seconds=0.3) else 0.0
                (pt, pc) = prev
                span = (st - pt).total_seconds()
                return pc + (d[pid][0] - pc) * ((t - pt).total_seconds() / span if span > 0 else 1)
            prev = (st, d[pid][0])
    return prev[1] if prev else 0.0

def window(smp, main, t0, t1):
    pids = sorted({pid for _, d in smp for pid in d})
    wall = (t1 - t0).total_seconds()
    dal = (cpu_at(smp, main, t1) - cpu_at(smp, main, t0)) / wall
    helpers = sum((cpu_at(smp, p, t1) - cpu_at(smp, p, t0)) for p in pids if p != main) / wall
    inside = [(t, d) for t, d in smp if t0 <= t <= t1]
    def avg(f):
        return sum(f(d) for _, d in inside) / len(inside) / 2**20 if inside else None
    return dict(wall=wall, dal=dal, helpers=helpers, total=dal + helpers,
                privDal=avg(lambda d: d.get(main, (0, 0, 0, 0))[1]),
                privHelpers=avg(lambda d: sum(v[1] for p, v in d.items() if p != main)),
                privTotal=avg(lambda d: sum(v[1] for v in d.values())),
                wsTotal=avg(lambda d: sum(v[2] for v in d.values())),
                wsDal=avg(lambda d: d.get(main, (0, 0, 0, 0))[2]),
                wsHelpers=avg(lambda d: sum(v[2] for p, v in d.items() if p != main)),
                nHelpers=max((len(d) - 1 for _, d in inside), default=0))

def peaks(smp, t0, t1):
    inside = [(t, d) for t, d in smp if t0 <= t <= t1]
    return dict(peakPrivTotal=max(sum(v[1] for v in d.values()) for _, d in inside) / 2**20,
                peakWsTotal=max(sum(v[2] for v in d.values()) for _, d in inside) / 2**20,
                osPeakWsPerProcess={str(p): v[3] / 2**20 for p, v in inside[-1][1].items()})

def parse(run):
    meta = read(os.path.join(RUNS, run, 'meta.txt'))
    start = ts(re.search(r'start=(\S+)', meta[1])[1]); main = int(re.search(r'pid=(\d+)', meta[1])[1])
    log = read(os.path.join(RUNS, run, 'run.log'))
    smp = samples(run)
    T = lambda l: ts(l.split(' ', 1)[0])
    r = dict(run=run, case=re.search(r'case=(\w)', meta[0])[1], mode=re.search(r'mode=(\w+)', meta[0])[1],
             sha=re.search(r'sha256=(\w+)', meta[0])[1][:12], end=meta[-1])
    marks = {}
    for l in log:
        m = re.search(r'step (begin|end ok): perf-profile: (.*?)( elapsedMs=(\d+))?$', l)
        if m:
            marks[(m[1], m[2])] = T(l)
            if m[1] == 'end ok' and m[2].startswith('load '):
                r['loadCallMs'] = int(m[4])
        if 'exe=' in l and 'cond' not in r:
            r['cond'] = re.sub(r'\s+', ' ', l[l.index('exe='):])[:400]
            r['device'] = num(r'\| "([^"]+)"', l, str); r['sr'] = num(r'\| (\d+) Hz', l); r['buf'] = num(r'buf (\d+)', l, int)
            r['version'] = num(r'version=(\S+)', l, str); r['config'] = num(r'config=(\S+)', l, str)
        if 'tracks: audio=' in l and 'tracksLoad' not in r:
            r['tracksLoad'] = l[l.index('tracks:'):][:330]
            r['instrLoaded'] = num(r'primaryLoaded=(\d+)', l, int); r['insertInstances'] = num(r'instances=(\d+)', l, int)
            r['offRows'] = num(r'off=(\d+)', l, int); r['proxySelectedAtLoad'] = num(r'proxySelected=(\d+)', l, int)
        if 'workers=' in l and 'workers' not in r:
            r['workers'] = num(r'workers=(\d+)', l, int)
    first = lambda k: next((v for (kind, name), v in marks.items() if kind == 'begin' and name.startswith(k)), None)
    loadEnd = next(v for (kind, name), v in marks.items() if kind == 'end ok' and name.startswith('load '))
    npBegin = first('stopped/never-played measurement window begins'); npEnd = first('stopped/never-played collect')
    plBegin = first('configured measurement window begins'); plEnd = first('configured collect')
    stop = first('stop playback (profiler stays on)')
    r['timeToPlayableS'] = (loadEnd - start).total_seconds()
    r['load'] = peaks(smp, start, loadEnd)
    r['loadPlusSettle'] = peaks(smp, start, npBegin)
    r['postLoadCpu'] = window(smp, main, loadEnd, npBegin)  # settle + runtime steps (incl. any proxy job)
    r['win'] = {
        'never-played': window(smp, main, npBegin, npEnd),
        'playback': window(smp, main, plBegin, plEnd),
        'after-stop 0-20': window(smp, main, stop, stop + timedelta(seconds=20)),
        'after-stop 30-50': window(smp, main, stop + timedelta(seconds=30), stop + timedelta(seconds=50)),
    }
    # in-app window blocks
    app = {}; cur = None; prevRest = None; prevRa = None
    for l in log:
        m = re.search(r'=== window: (\S+) ===', l)
        if m:
            cur = m[1]; app[cur] = {}; continue
        if 'window start: playhead=' in l:
            app.setdefault('_start', {})['ph'] = num(r'playhead=(\d+)', l, int)
        if cur is None:
            if 'read-ahead:' in l:
                prevRa = l
            continue
        w = app[cur]
        if 'window end:' in l:
            w['wall'] = num(r'wall=([\d.]+)', l); w['ph1'] = num(r'playhead=(\d+)', l, int); w['ph0'] = num(r'started (\d+)', l, int)
        elif 'engine: blocks=' in l:
            w['cbMean'] = num(r'ms\(min/mean/max\)=[\d.]+/([\d.]+)/', l); w['cbMax'] = num(r'ms\(min/mean/max\)=[\d.]+/[\d.]+/([\d.]+)', l)
            w['budMean'] = num(r'budget%\(mean/max\)=([\d.]+)/', l); w['budMax'] = num(r'budget%\(mean/max\)=[\d.]+/([\d.]+)', l)
            w['near'] = num(r'nearOverruns\(>70%\)=(\d+)', l, int); w['over'] = num(r'overruns\(>=100%\)=(\d+)', l, int)
            w['blocks'] = num(r'blocks=(\d+)', l, int)
        elif 'output: peakHold' in l:
            w['peak'] = num(r'masterPeakL/R=([\d.]+)', l); w['rmsL'] = num(r'rmsL/R=([\d.]+)', l); w['nonFinite'] = num(r'nonFinite=(\d+)', l, int)
        elif 'proxy: selectedDestinations' in l:
            w['proxySel'] = num(r'selectedDestinations=(\d+)', l, int); w['proxyUnder'] = num(r'underrunsInWindow=(\d+)', l, int)
        elif 'instruments: hosts' in l:
            w['liveHosts'] = num(r'live processBlock blocks=(\d+)', l, int)
        elif 'render pool:' in l:
            c = dict(rs=num(r'stageBlocks rested=(\d+)', l, int), ps=num(r'stageBlocks rested=\d+ processed=(\d+)', l, int),
                     ri=num(r'instrumentBlocks rested=(\d+)', l, int), pi=num(r'instrumentBlocks rested=\d+ processed=(\d+)', l, int))
            w['restingNow'] = num(r'restingNow=(\d+)', l, int); w['stages'] = num(r'insert stages=(\d+)', l, int); w['liveInserts'] = num(r'liveInserts=(\d+)', l, int)
            w['restCum'] = c
        elif 'read-ahead:' in l:
            w['raMissedCum'] = num(r'missed=(\d+)', l, int); w['raOwned'] = num(r'ownedNow=(\d+)', l, int); w['raProduced'] = num(r'produced=(\d+)', l, int)
        elif 'process: cpu=' in l:
            w['appCpuOneCore'] = num(r'cpu=([\d.]+)%', l); w['sysBusy'] = num(r'systemBusy=([\d.]+)%', l)
        elif 'callback start intervals' in l:
            w['lateStarts'] = num(r'lateStarts\(>1.25x period\)=(\d+)', l, int); w['intervalMax'] = num(r'ms\(mean/max\)=[\d.]+/([\d.]+)', l)
        elif 'by plug-in' in l:
            w['byPlugin'] = l[l.index('by plug-in'):][:500]
        elif 'step end ok' in l and 'collect' in l:
            cur = None
    # in-window deltas of cumulative counters: previous collect (or 0) -> this collect
    order = ['stopped/never-played', 'configured', 'stopped/right-after-stop', 'stopped/after-gap']
    prev = None; prevMiss = 0
    for l in log:  # baseline for the first window: the counters printed just before it
        if 'playing=no | rest:' in l:
            prev = dict(rs=num(r'stageBlocks rested=(\d+)', l, int), ps=num(r'stageBlocks rested=\d+ processed=(\d+)', l, int),
                        ri=num(r'instrumentBlocks rested=(\d+)', l, int), pi=num(r'instrumentBlocks rested=\d+ processed=(\d+)', l, int))
            break
    for k in order:
        w = app.get(k)
        if not w:
            continue
        c = w.get('restCum')
        if c and None not in c.values():
            if prev:
                w['restDelta'] = {x: c[x] - prev[x] for x in c}
            prev = c
        if w.get('raMissedCum') is not None:
            w['raMissedDelta'] = w['raMissedCum'] - prevMiss; prevMiss = w['raMissedCum']
    r['app'] = app
    r['proxyLines'] = [l[30:200] for l in log if re.search(r'proxy (render|job)|render(ing)? proxy|ProxyRender', l, re.I)][:10]
    r['crashOrWarn'] = [l[30:200] for l in log if re.search(r'crash|FAIL|exception|isolat.*(fail|lost|unrespons)', l, re.I)][:10]
    return r

if __name__ == '__main__':
    runs = sorted(d for d in os.listdir(RUNS) if os.path.isdir(os.path.join(RUNS, d)) and (len(sys.argv) < 2 or re.match(sys.argv[1], d)))
    out = [parse(x) for x in runs]
    json.dump(out, open(os.path.join(S, 'parsed.json'), 'w'), indent=1, default=str)
    for r in out:
        print('=====', r['run'], r['case'], r['mode'], r['sha'], r.get('version'), r.get('config'), r.get('device'), r.get('sr'), r.get('buf'), 'workers', r.get('workers'), '|', r['end'])
        print('  load: playable', round(r['timeToPlayableS'], 1), 's loadCall', r.get('loadCallMs'), 'ms peakPriv', round(r['load']['peakPrivTotal']), 'peakWs', round(r['load']['peakWsTotal']),
              '| incl settle peakPriv', round(r['loadPlusSettle']['peakPrivTotal']), 'peakWs', round(r['loadPlusSettle']['peakWsTotal']), '| instr', r.get('instrLoaded'), 'inserts', r.get('insertInstances'))
        print('  post-load (settle) cpu total', round(r['postLoadCpu']['total'], 3))
        for k, w in r['win'].items():
            print(f"  {k:<17} cpu dal={w['dal']:.3f} helpers={w['helpers']:.3f} total={w['total']:.3f} cores ({100*w['total']/CORES:.2f}%) priv dal={w['privDal']:.0f} help={w['privHelpers']:.0f} tot={w['privTotal']:.0f} ws tot={w['wsTotal']:.0f} helpers={w['nHelpers']}")
        for k, w in r['app'].items():
            if k.startswith('_'):
                continue
            print(f"  [{k}] cb mean/max={w.get('cbMean')}/{w.get('cbMax')} bud%={w.get('budMean')}/{w.get('budMax')} near={w.get('near')} over={w.get('over')} late={w.get('lateStarts')} peak={w.get('peak')} rms={w.get('rmsL')} liveHosts={w.get('liveHosts')} restingNow={w.get('restingNow')} rest={w.get('restDelta')} raMiss={w.get('raMissedDelta')} raOwned={w.get('raOwned')} proxySel={w.get('proxySel')} ph={w.get('ph0')}->{w.get('ph1')}")
        if r['proxyLines']:
            print('  proxy lines:', r['proxyLines'])
        if r['crashOrWarn']:
            print('  warn:', r['crashOrWarn'])
