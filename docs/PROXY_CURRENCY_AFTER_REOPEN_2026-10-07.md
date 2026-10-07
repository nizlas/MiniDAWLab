# VB3-II proxy reads Stale after save / reopen although it was just rendered Current (2026-10-07)

**Project:** `TSE_pt2_261006\TSE_pt2.dalproj` (user's Dropbox copy, read only). **Track:** the
organ is **TrackId 5 "VB3-II"** (GenericVst3, VB3-II 2.3.1, update mode Manual, a Secondary
configured; 6 clips / 159 notes, 1 routed MIDI source without notes). Every reproduction ran on a
complete temp copy (`%TEMP%\dal-proxy-vb3`, Audio + InstrumentProxies, references verified) with
the existing `--stability-proxy-render-probe` diagnostic; nothing in Dropbox was written.

## Observation chain (Release 1.2.0, before the fix)

| Point | Evidence |
|---|---|
| Saved on disk (22:57) | `proxy.generationId = sha256:6f49a4f8…`, schema 2, `primaryStateRevisionAtPublish = 3`, `primaryStateRevisionAtSave = 3`, tail v2, render 48 kHz / 512, plugin VB3-II 2.3.1 / uid 1204027788, state blob 10 584 bytes (sha256 `4772bfb8…`), asset `track_5_sha256_6f49a4f8….wav` present (37 335 144 bytes). Two generations rendered that evening: `adcca050…` (22:56:45) and `6f49a4f8…` (22:57:42). |
| Reopen (new process), right after load | `destination=Stale`. Live identity: fingerprint **`sha256:adcca050…`**, **`stateRevision=1`**, same plugin, same policies (latency v1 / tail v2 / render v1 / format v1), same content (6 clips, 159 notes, canonical bytes 5 606 B), same blob hash `4772bfb8…` restored OK (`load: GenericVst3 state restore ok trackId=5`). |

The live fingerprint after reopen is **exactly the generation the user rendered first** at
22:56:45 — i.e. right after a load the host's revision is 1. Between that render and the second
one the revision moved to 3 (two DAL-observed Primary events in that session, e.g. opening /
closing the plug-in editor; a user edit is not required), so the second render published at
revision 3 and the Save stamped 3. After the reopen the freshly created host counts from 0 again
(1 after assignment + state restore): fingerprint(revision 1) ≠ generation(revision 3) ⇒ Stale.

**First actual deviation:** the F2 state-identity component (`primaryStateRevision`) of the live
fingerprint — 1 vs the published 3. Everything else (plugin identity + version, saved / restored
state bytes, policy versions, schema, clips, notes, CC, channels, clip order, span) was
identical. It is not a transient load-time state: the live revision stays 1 until the next
DAL-observed event, so the generation would never read Current again without a re-render — which
publishes at the new revision and recreates the problem for the next reopen.

So: **Save does not make it Stale** (the stamps are consistent), **serialization / loading does
not change the musical content** (same canonical bytes), **state restore succeeds with the same
bytes**; the defect is that the Primary-present currency check never adopts the persisted
pairing that the design (steering §9.4.2, "persisted proxy↔saved-state pairing restores
load-time validity by construction") relies on. **DC / tail policy had no part:** both sides were
tail v2 with identical recorded policies.

## Fix (1.2.1)

`InstrumentTrackController::adoptSavedStatePairingAfterLoadRestore` — called at the end of the
Groove Agent, HALion Sonic and GenericVst3 autoload paths, only after the saved plug-in state blob
was restored successfully — raises the host's `PrimarySemanticRevision` to
`primaryStateRevisionAtSave` (`raiseToAtLeast`: never lowered, 0 ⇒ nothing assumed) and records
that value as the loaded blob's in-session association. The blob just restored *is* the blob the
Save stamped with that revision, so the live identity reads as the saved state's identity again.
No status is forced, no comparison is skipped: a note edit still changes the fingerprint; a Save
made after a sound edit stamps a higher revision and the older generation stays Stale; later
bumps continue above the seeded value; the missing-Primary path is unchanged.

## Verification (handler level through the production load / render / publish / save paths)

| Sequence | Result |
|---|---|
| Reopen the user's saved copy (pub 3 / save 3) — **before** | Stale, live fingerprint `adcca050…`, stateRevision 1 |
| Reopen the same copy — **after** | **Current**, live fingerprint `6f49a4f8…` = published, stateRevision 3; diag `revisionAfterRestore=1 stampAtSave=3 stampAtPublish=3 liveRevision=3` |
| Render now → publish → Current → Save (×2 renders, probe copy saved after each) → reopen | Current, pub 3 / save 3 |
| Real musical change in the saved copy (one note +1 semitone) → reopen | **Stale** (fingerprint `1c31b8c2…`), pairing intact |
| Saved after a later sound edit (stamp 4 > publish 3) → reopen | **Stale** (live revision seeded to 4, fingerprint `7cc23385…`) |
| `MiniDAWSelftests` (3 478, incl. the proxy pairing / currency unit tests), `InstrumentParallelFocusedTests` (39) | green |

Not re-run: the no-Primary scenario (`--stability-proxy-recording`); the change does not touch
that path (it runs only after a live Primary restored its saved state).

## The original project after the update

Open it with 1.2.1: the existing generation `6f49a4f8…` reads **Current without a new Render
now** (verified on the copy — same stamps, same blob). The extra files `_2.wav` / `_3.wav` that
the probe published exist only in the temp copy.
