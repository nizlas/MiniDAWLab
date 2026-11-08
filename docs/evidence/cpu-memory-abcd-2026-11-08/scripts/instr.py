import json, sys, base64, re, hashlib

def load(p):
    return json.load(open(p, encoding='utf-8'))

a, b = load(sys.argv[1]), load(sys.argv[2])
tn = {t['id']: t for t in a['tracks']}
print('== experimentalInstrumentTracks (A | B)')
for i, (x, y) in enumerate(zip(a['experimentalInstrumentTracks'], b['experimentalInstrumentTracks'])):
    tid = x.get('trackId')
    notes = len(x.get('timelineNotes', []))
    name = x.get('descriptiveName') or x.get('pluginName') or x.get('requiredKitName') or ''
    keys = [k for k in x if 'name' in k.lower() or 'kind' in k.lower()]
    print(f"[{i}] track {tid} {tn.get(tid, {}).get('name', '?')[:30]!r} plugin={x.get('pluginIdentifier', x.get('pluginFileOrIdentifier', ''))!s:.60} kind={x.get('instrumentKind')} kit={x.get('requiredKitName')} notes={notes}->{len(y.get('timelineNotes', []))}"
          f" muted={x.get('muted')}->{y.get('muted')} powerOn={x.get('powerOn')}->{y.get('powerOn')} proxyMode={x.get('proxyUpdateMode')}->{y.get('proxyUpdateMode')} proxyActive={x.get('proxy', {}).get('active')}->{y.get('proxy', {}).get('active')} trackOff={tn.get(tid, {}).get('off', False)}->{[t for t in b['tracks'] if t['id'] == tid][0].get('off', False)}")
    print('     keys:', ','.join(k for k in x.keys() if k not in ('timelineNotes', 'pluginStateBase64')))

def decode(s):
    raw = base64.b64decode(s)
    return raw

print('== insert state differences, byte level (GUID masked)')
for t in a['tracks']:
    u = [y for y in b['tracks'] if y['id'] == t['id']][0]
    for i, (p, q) in enumerate(zip(t.get('inserts', []), u.get('inserts', []))):
        if p.get('pluginStateBase64') != q.get('pluginStateBase64'):
            r1, r2 = decode(p['pluginStateBase64']), decode(q['pluginStateBase64'])
            d = [k for k in range(min(len(r1), len(r2))) if r1[k] != r2[k]]
            t1 = r1.decode('latin-1'); t2 = r2.decode('latin-1')
            m1 = re.sub(r'GUID="[^"]*"', 'GUID="*"', t1); m2 = re.sub(r'GUID="[^"]*"', 'GUID="*"', t2)
            print(f"  track {t['id']} insert {i} {p.get('pluginIdentifier','')[:50]} len {len(r1)}/{len(r2)} differingBytes={len(d)} span={d[0] if d else None}..{d[-1] if d else None} equalAfterGuidMask={m1 == m2}")
    if not t.get('inserts'):
        continue

print('== instrument plugin state (GUID masked)')
for i, (x, y) in enumerate(zip(a['experimentalInstrumentTracks'], b['experimentalInstrumentTracks'])):
    if x.get('pluginStateBase64') != y.get('pluginStateBase64'):
        r1, r2 = decode(x['pluginStateBase64']), decode(y['pluginStateBase64'])
        print(f"  [{i}] track {x.get('trackId')} len {len(r1)}/{len(r2)}")
print('== inserts per track (A)')
for t in a['tracks']:
    for p in t.get('inserts', []):
        print(f"  track {t['id']} stage={p.get('stage')} {p.get('pluginIdentifier','')[:70]}")
