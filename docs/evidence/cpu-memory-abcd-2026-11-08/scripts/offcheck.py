"""Read-only: per-track Off and Mute in the original 261106, the measured C copy, D and the old all-Off
fixture, plus every field that differs between the original and C."""
import json, sys, hashlib, os

def load(p):
    return json.load(open(p, encoding='utf-8'))

orig, cpath, dpath, alloff = sys.argv[1:5]
o, c, d = load(orig), load(cpath), load(dpath)
a = load(alloff) if os.path.exists(alloff) else None
instr = lambda p: {it['trackId']: it for it in p.get('experimentalInstrumentTracks', [])}
oi, ci, di = instr(o), instr(c), instr(d)
ai = instr(a) if a else {}
at = {t['id']: t for t in a['tracks']} if a else {}
ct = {t['id']: t for t in c['tracks']}; dt = {t['id']: t for t in d['tracks']}
print('| TrackId | Name | Kind | Off (original) | Mute (original) | instrument powerOn (original) | Off (C copy) | Mute (C copy) | Off (D) | Off (old all-Off fixture) |')
print('|---|---|---|---|---|---|---|---|---|---|')
for t in o['tracks']:
    tid = t['id']
    pw = oi.get(tid, {}).get('powerOn', '—' if tid not in oi else 'absent')
    print(f"| {tid} | {t['name']} | {t['kind']} | {t.get('off', False)} | {t.get('muted', False)} | {pw} | {ct[tid].get('off', False)} | {ct[tid].get('muted', False)} | {dt[tid].get('off', False)} | {at.get(tid, {}).get('off', 'n/a') if a else 'n/a'} |")
print()
print('Off rows: original', sum(t.get('off', False) for t in o['tracks']), 'C', sum(t.get('off', False) for t in c['tracks']),
      'D', sum(t.get('off', False) for t in d['tracks']), 'all-Off fixture', sum(t.get('off', False) for t in a['tracks']) if a else 'n/a', 'of', len(o['tracks']))
print('Muted rows: original', sum(t.get('muted', False) for t in o['tracks']), 'C', sum(t.get('muted', False) for t in c['tracks']))

def flat(v, p=''):
    r = {}
    if isinstance(v, dict):
        for k, x in v.items(): r.update(flat(x, f'{p}.{k}' if p else k))
    elif isinstance(v, list):
        for i, x in enumerate(v): r.update(flat(x, f'{p}[{i}]'))
    else:
        r[p] = v
    return r
fo, fc = flat(o), flat(c)
diff = [k for k in sorted(set(fo) | set(fc)) if fo.get(k) != fc.get(k)]
print('\nFields that differ between the original and the C copy:', len(diff))
for k in diff:
    print(f'  {k}: {fo.get(k)!r} -> {fc.get(k)!r}')
# is C derived from 261031 (like D) or from 261106? compare the clip payload hashes
h = lambda p: hashlib.sha1(json.dumps([t.get('clips') for t in p['tracks']] + [it.get('clips') for it in p['experimentalInstrumentTracks']], sort_keys=True).encode()).hexdigest()[:12]
print('\nclip/notes payload hash: original', h(o), 'C', h(c), 'D', h(d))
print('track 40 muted: original', [t for t in o['tracks'] if t['id'] == 40][0].get('muted'), 'C', ct[40].get('muted'), 'D', dt[40].get('muted'))
print('playheadSamples: original', o.get('playheadSamples'), 'C', c.get('playheadSamples'), 'D', d.get('playheadSamples'))
