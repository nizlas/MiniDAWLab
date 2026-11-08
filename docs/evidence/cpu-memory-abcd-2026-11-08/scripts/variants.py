"""Make the measured temp copies from the two copied projects (never the originals).

AB.dalproj  = 261031 (A and B), window state normalised
C.dalproj   = 261106, window state normalised
D.dalproj   = 261031 with only the rows that are Off in 261106 set Off (track `off`, and the
              instrument entry's `powerOn` where 261106 has it False). Mutes, states, proxies as 261031.
Window normalisation (UI only, same in all): MIDI editor workspace closed, main window at 261031's
position. Mixer window state is identical in both files and the perf profile closes it anyway.
Also checks that every audio clip source and proxy file resolves inside the copy."""
import json, os, sys, copy

D = sys.argv[1]
a_dir, c_dir = os.path.join(D, 'TSE_pt2_261031'), os.path.join(D, 'TSE_pt2_261106')
load = lambda p: json.load(open(p, encoding='utf-8'))
a = load(os.path.join(a_dir, 'TSE_pt2.dalproj'))
c = load(os.path.join(c_dir, 'TSE_pt2.dalproj'))

def normalise(p):
    p = copy.deepcopy(p)
    mw = p.get('midiEditorWorkspace')
    if isinstance(mw, dict):
        mw['open'] = False
    p['mainWindow']['x'] = a['mainWindow']['x']
    p['mainWindow']['y'] = a['mainWindow']['y']
    return p

def write(p, path):
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(p, f, ensure_ascii=False)
    print('wrote', path)

def check_refs(p, root):
    missing = 0; n = 0
    for t in p['tracks']:
        for cl in t.get('clips', []):
            if 'sourcePath' in cl:
                n += 1
                if not os.path.exists(os.path.join(root, cl['sourcePath'])):
                    missing += 1; print('  MISSING', cl['sourcePath'])
    for it in p.get('experimentalInstrumentTracks', []):
        for key in ('proxy', 'secondary'):
            pr = it.get(key) or {}
            if isinstance(pr, dict) and pr.get('relativePath'):
                n += 1
                if not os.path.exists(os.path.join(root, pr['relativePath'])):
                    missing += 1; print('  MISSING', pr['relativePath'])
    print(f'  refs checked={n} missing={missing} in {root}')

ab = normalise(a)
cc = normalise(c)
dd = normalise(a)
coff = {t['id']: t.get('off', False) for t in c['tracks']}
switched = []
for t in dd['tracks']:
    if coff.get(t['id']) and not t.get('off', False):
        t['off'] = True
        switched.append(t['id'])
cpow = {it['trackId']: it.get('powerOn') for it in c['experimentalInstrumentTracks']}
for it in dd['experimentalInstrumentTracks']:
    if cpow.get(it['trackId']) is False:
        it['powerOn'] = False
print('D: switched Off track ids', switched)
print('D: On rows', [(t['id'], t['kind']) for t in dd['tracks'] if not t.get('off', False)])
print('C: On rows', [(t['id'], t['kind']) for t in c['tracks'] if not t.get('off', False)])
assert [t['id'] for t in dd['tracks'] if not t.get('off', False)] == [t['id'] for t in c['tracks'] if not t.get('off', False)]
write(ab, os.path.join(a_dir, 'AB.dalproj'))
write(dd, os.path.join(a_dir, 'D.dalproj'))
write(cc, os.path.join(c_dir, 'C.dalproj'))
check_refs(ab, a_dir)
check_refs(cc, c_dir)
