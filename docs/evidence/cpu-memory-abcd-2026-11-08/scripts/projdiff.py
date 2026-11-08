"""Compare two .dalproj files track by track: power, mute, routing, clips, notes, inserts, instrument.
Large strings (plug-in state) are compared by hash; AmpliTube's per-save GUID is masked first."""
import json, sys, hashlib, re, base64, zlib

def load(p):
    return json.load(open(p, encoding='utf-8'))

def h(v):
    s = json.dumps(v, sort_keys=True)
    return hashlib.sha1(s.encode()).hexdigest()[:10]

def mask_state(s):
    # AmpliTube writes a random preset GUID on every save; decode if possible and mask it.
    try:
        raw = base64.b64decode(s)
    except Exception:
        return s
    txt = raw.decode('latin-1')
    return re.sub(r'GUID="[^"]*"', 'GUID="*"', txt)

def flat(v, prefix=''):
    out = {}
    if isinstance(v, dict):
        for k, x in v.items():
            out.update(flat(x, f'{prefix}.{k}' if prefix else k))
    elif isinstance(v, list):
        if len(v) > 8 and all(isinstance(x, dict) for x in v) and prefix.endswith('Notes'):
            out[prefix] = f'<{len(v)} notes {h(v)}>'
        else:
            for i, x in enumerate(v):
                out.update(flat(x, f'{prefix}[{i}]'))
    elif isinstance(v, str) and len(v) > 200:
        out[prefix] = f'<str {len(v)} {hashlib.sha1(mask_state(v).encode("latin-1", "replace")).hexdigest()[:10]}>'
    else:
        out[prefix] = v
    return out

def summary(t):
    ins = [(i.get('stage'), (i.get('pluginIdentifier') or '').split('|')[0][-40:], i.get('powerOn', i.get('active'))) for i in t.get('inserts', [])]
    notes = sum(len(c.get('timelineNotes', [])) for c in t.get('clips', [])) + len(t.get('timelineNotes', []))
    return dict(name=t.get('name'), kind=t.get('kind'), off=t.get('off', False), muted=t.get('muted', False),
                out=json.dumps(t.get('output')), clips=len(t.get('clips', [])), notes=notes, inserts=ins,
                instr=(t.get('instrument') or {}).get('pluginIdentifier', t.get('pluginIdentifier', '')))

a, b = load(sys.argv[1]), load(sys.argv[2])
print('== top level differences (excluding tracks)')
fa = flat({k: v for k, v in a.items() if k != 'tracks'})
fb = flat({k: v for k, v in b.items() if k != 'tracks'})
for k in sorted(set(fa) | set(fb)):
    if fa.get(k) != fb.get(k):
        print(f'  {k}: {fa.get(k)!r} -> {fb.get(k)!r}')
ta = {t['id']: t for t in a['tracks']}
tb = {t['id']: t for t in b['tracks']}
print('== track order same:', [t['id'] for t in a['tracks']] == [t['id'] for t in b['tracks']])
print('== tracks only in A:', sorted(set(ta) - set(tb)), ' only in B:', sorted(set(tb) - set(ta)))
print('== per track (id name kind | A off/muted -> B off/muted | other differing fields)')
for tid in [t['id'] for t in a['tracks']]:
    if tid not in tb:
        continue
    x, y = ta[tid], tb[tid]
    sx, sy = summary(x), summary(y)
    fx, fy = flat(x), flat(y)
    diffs = [k for k in sorted(set(fx) | set(fy)) if fx.get(k) != fy.get(k) and k not in ('off', 'muted')]
    print(f"  {tid:>3} {sx['name'][:28]:<28} {sx['kind']:<10} off {sx['off']!s:<5}->{sy['off']!s:<5} muted {sx['muted']!s:<5}->{sy['muted']!s:<5} clips={sx['clips']} notes={sx['notes']} inserts={len(sx['inserts'])}")
    for k in diffs[:12]:
        print(f'        {k}: {str(fx.get(k))[:90]!s} -> {str(fy.get(k))[:90]!s}')
    if len(diffs) > 12:
        print(f'        ... {len(diffs) - 12} more')
