"""C2.dalproj = the copied, manually corrected 261106 with the same window normalisation as old C
(main window at 33/282; the file stores no MIDI editor workspace). Checks every audio/proxy ref."""
import json, os, sys
d = sys.argv[1]
src = os.path.join(d, 'TSE_pt2.dalproj')
p = json.load(open(src, encoding='utf-8'))
print('mainWindow before', p['mainWindow']['x'], p['mainWindow']['y'], 'midiEditorWorkspace' in p)
p['mainWindow']['x'] = 33
p['mainWindow']['y'] = 282
with open(os.path.join(d, 'C2.dalproj'), 'w', encoding='utf-8') as f:
    json.dump(p, f, ensure_ascii=False)
n = miss = 0
for t in p['tracks']:
    for c in t.get('clips', []):
        if 'sourcePath' in c:
            n += 1; miss += not os.path.exists(os.path.join(d, c['sourcePath']))
for it in p['experimentalInstrumentTracks']:
    for k in ('proxy', 'secondary'):
        r = it.get(k) or {}
        if isinstance(r, dict) and r.get('relativePath'):
            n += 1; miss += not os.path.exists(os.path.join(d, r['relativePath']))
print('refs', n, 'missing', miss, '| Off rows', sum(t.get('off', False) for t in p['tracks']))
