"""Where do the instrument rows have notes (seconds), and where are the cycle locators."""
import json, sys
p = json.load(open(sys.argv[1], encoding='utf-8'))
sr = p.get('timelineSampleRate', 48000.0)
print('cycleEnabled', p.get('cycleEnabled'), 'left', p.get('leftLocatorSamples', 0) / sr, 'right', p.get('rightLocatorSamples', 0) / sr, 'bpm', p.get('bpm'))
spt = sr * 60.0 / p['bpm'] / p['ticksPerQuarter']
names = {t['id']: t['name'] for t in p['tracks']}
for it in p['experimentalInstrumentTracks']:
    spans = []
    for c in it.get('clips', []):
        start = c.get('startSamples', c.get('timelineAnchorSamples', 0))
        anchor = c.get('timelineAnchorSamples', start)
        ns = c.get('timelineNotes', [])
        if ns:
            lo = min(anchor + n['startTick'] * spt for n in ns) / sr
            hi = max(anchor + (n['startTick'] + n['durationTicks']) * spt for n in ns) / sr
            spans.append((round(lo, 1), round(hi, 1), len(ns)))
    print(it['trackId'], names.get(it['trackId'], '')[:30], it.get('instrumentKind'), sorted(spans)[:12])
print('audio clips on tracks that are On in this file:')
for t in p['tracks']:
    if t['kind'] == 'audio' and not t.get('off', False):
        print(' ', t['id'], len(t.get('clips', [])))
