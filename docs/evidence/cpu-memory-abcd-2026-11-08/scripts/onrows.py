"""Read-only: On rows by arrangement position (1-based order of `tracks`), and per-row Off/Mute
differences against the previously measured 261106 copy."""
import json, sys
new = json.load(open(sys.argv[1], encoding='utf-8'))
old = json.load(open(sys.argv[2], encoding='utf-8'))
inst = {it['trackId']: it for it in new.get('experimentalInstrumentTracks', [])}
oldt = {t['id']: t for t in old['tracks']}
oldi = {it['trackId']: it for it in old.get('experimentalInstrumentTracks', [])}
print('On rows (position, name, TrackId, instrument, muted):')
for pos, t in enumerate(new['tracks'], 1):
    if not t.get('off', False):
        it = inst.get(t['id'], {})
        kind = it.get('instrumentKind', t['kind'])
        name = (it.get('genericVst3Descriptor') or {}).get('name') if isinstance(it.get('genericVst3Descriptor'), dict) else None
        print(f"  #{pos:>2} {t['name']!r} id={t['id']} kind={t['kind']} instrument={name or kind} muted={t.get('muted', False)} powerOn={it.get('powerOn', 'absent') if it else '-'}")
print('Changes against the previously measured file:')
n = 0
for pos, t in enumerate(new['tracks'], 1):
    o = oldt.get(t['id'], {})
    for k in ('off', 'muted'):
        if t.get(k, False) != o.get(k, False):
            n += 1; print(f"  #{pos} id={t['id']} {t['name']!r}: {k} {o.get(k, False)} -> {t.get(k, False)}")
    a, b = oldi.get(t['id'], {}).get('powerOn'), inst.get(t['id'], {}).get('powerOn')
    if a != b:
        n += 1; print(f"  #{pos} id={t['id']}: instrument powerOn {a} -> {b}")
print('  total', n)
print('order unchanged:', [t['id'] for t in new['tracks']] == [t['id'] for t in old['tracks']])
print('Off', sum(t.get('off', False) for t in new['tracks']), 'of', len(new['tracks']))
