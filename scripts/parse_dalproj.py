import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)
tracks = {t["id"]: t for t in data.get("tracks", [])}
print("tracks count:", len(tracks))
for t in data.get("tracks", []):
    print(f"  track id={t['id']} kind={t.get('kind')} name={t.get('name', '')}")
print("experimentalInstrumentTracks count:", len(data.get("experimentalInstrumentTracks", [])))
for i, et in enumerate(data.get("experimentalInstrumentTracks", [])):
    tid = et.get("trackId", 0)
    tr = tracks.get(tid, {})
    clips = et.get("clips", [])
    print(
        f"  [{i}] trackId={tid} kind={et.get('instrumentKind')} name={et.get('name', '')} "
        f"clips={len(clips)} enabled={et.get('enabled')} pluginWasLoaded={et.get('pluginWasLoadedOnSave')}"
    )
    print(f"      tracks[] match: kind={tr.get('kind', 'MISSING')} name={tr.get('name', 'MISSING')}")
