#pragma once

// =============================================================================
// MidiEditorAuditionModel — pure audition-note scheduling (Phase B.1)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   The timing and ownership rules behind every MIDI-editor audition gesture, with no JUCE, no
//   host, no clock and no timer: callers pass "now" in milliseconds and receive the MIDI events
//   to emit. `ExperimentalMidiPatternPlayer` owns one instance, feeds it the real clock and
//   forwards emitted events to the editor's bound `ExperimentalInstrumentHost` (for a
//   `TrackKind::Midi` row that host is the routed *destination*, so audition follows `MIDI To`).
//   Header-only and std-only so the gesture semantics are unit-testable with a fake clock
//   (`tests/selftest/`) — audition timing must never need a real-time sleep to verify.
//
// GESTURE SEMANTICS (matches the Cubase-informed Phase B.1 steer)
//   * **Arranged-note preview** (clicking a note rectangle): Note On at Mouse Down; the note stays
//     active for at least `kArrangedAuditionMs` (quick click), and a longer physical hold extends
//     it until Mouse Up. Duration = max(default, hold). The note's *arranged* length is ignored.
//   * **Held preview** (left piano key / drum-name row): Note On exactly at Mouse Down, Note Off
//     exactly at Mouse Up, no minimum — a very short click is a very short note.
//   * **One-shot preview** (create-note feedback, velocity-drag rattle): begin + end in one call;
//     the caller picks the gate (default audition length, or the short rattle gate).
//
// OWNERSHIP AND CLEANUP
//   Every preview is identified by (channel, pitch), with **at most one active voice per key**.
//   Retriggering a key emits Note Off + Note On immediately (restart the attack) and *replaces*
//   the older voice's scheduled Note Off, so a stale off can never cut the newer preview.
//   `releaseAllActive` emits immediate offs for everything still sounding — the player calls it on
//   destruction (editor close / rebind / destination change), so audition notes always release at
//   the destination and channel captured at Note On. Note Offs carry the off-velocity captured at
//   Note On.
//
// THREADING
//   Pure functions on caller state; used from the [Message thread] only.
// =============================================================================

#include <algorithm>
#include <vector>

namespace midi_audition
{
    /// Default arranged-note audition length ("slightly under one second"; Cubase-comparison
    /// compromise from the Phase B.1 steer, target band 700–900 ms).
    inline constexpr double kArrangedAuditionMs = 850.0;

    /// Short gate for the velocity-drag "rattle" re-audition (unchanged from the pre-B.1 editor).
    inline constexpr double kVelocityRattleMs = 100.0;

    /// One MIDI event the caller must deliver to the bound host, in order.
    struct AuditionEvent
    {
        bool noteOn = false;
        int channel = 1;
        int pitch = 60;
        /// Note-on velocity for ons; note-off (release) velocity for offs. 0 … 127.
        int velocity = 100;
    };

    /// See file header for the gesture semantics this implements.
    class AuditionScheduler
    {
    public:
        /// Begin a preview at Mouse Down (or a one-shot when the caller ends it immediately).
        /// `minHoldMs` is the minimum sounding time measured from now: `kArrangedAuditionMs` for
        /// arranged notes, `0` for exact piano-key / drum-row gestures. Emits a retrigger Note Off
        /// first when the same (channel, pitch) is already sounding, then the Note On.
        void beginPreview(const int channel,
                          const int pitch,
                          const int onVelocity,
                          const int offVelocity,
                          const double nowMs,
                          const double minHoldMs,
                          std::vector<AuditionEvent>& emitNow)
        {
            const int ch = clampChannel(channel);
            const int p = clampPitch(pitch);
            Active* existing = findActive(ch, p);
            if (existing != nullptr)
            {
                // Restart the attack; replacing the voice state discards the older scheduled off
                // (it is consumed by this immediate off instead), so it cannot cut the new note.
                emitNow.push_back({ false, ch, p, existing->offVelocity });
                existing->downMs = nowMs;
                existing->minHoldMs = minHoldMs;
                existing->offVelocity = clampVelocity(offVelocity, 0);
                existing->held = true;
            }
            else
            {
                Active a;
                a.channel = ch;
                a.pitch = p;
                a.downMs = nowMs;
                a.minHoldMs = minHoldMs;
                a.offVelocity = clampVelocity(offVelocity, 0);
                a.held = true;
                active_.push_back(a);
            }
            emitNow.push_back({ true, ch, p, clampVelocity(onVelocity, 1) });
        }

        /// End the gesture at Mouse Up: schedules the Note Off at `max(now, down + minHold)`.
        /// For a held (piano-key) preview `minHoldMs` was 0, so the off is due immediately; the
        /// caller's `drainDue(now)` in the same tick emits it. Unknown keys are ignored (already
        /// released by retrigger/cleanup).
        void endPreview(const int channel, const int pitch, const double nowMs)
        {
            Active* a = findActive(clampChannel(channel), clampPitch(pitch));
            if (a == nullptr || !a->held)
            {
                return;
            }
            a->held = false;
            a->offDueMs = std::max(nowMs, a->downMs + a->minHoldMs);
        }

        /// Convenience for one-shot previews (create-note feedback, velocity rattle): Note On now,
        /// Note Off scheduled after `gateMs`.
        void oneShotPreview(const int channel,
                            const int pitch,
                            const int onVelocity,
                            const int offVelocity,
                            const double nowMs,
                            const double gateMs,
                            std::vector<AuditionEvent>& emitNow)
        {
            beginPreview(channel, pitch, onVelocity, offVelocity, nowMs, gateMs, emitNow);
            endPreview(channel, pitch, nowMs);
        }

        /// Emits every Note Off whose deadline has passed. Held previews (no Mouse Up yet) never
        /// fire here — a hold beyond the default duration keeps the note sounding by design.
        void drainDue(const double nowMs, std::vector<AuditionEvent>& emitNow)
        {
            for (size_t i = 0; i < active_.size();)
            {
                Active& a = active_[i];
                if (!a.held && a.offDueMs <= nowMs)
                {
                    emitNow.push_back({ false, a.channel, a.pitch, a.offVelocity });
                    active_[i] = active_.back();
                    active_.pop_back();
                }
                else
                {
                    ++i;
                }
            }
        }

        /// Immediate Note Off for everything still sounding (scheduled *and* held). Used for
        /// editor close, rebind and destination/channel change teardown.
        void releaseAllActive(std::vector<AuditionEvent>& emitNow)
        {
            for (const Active& a : active_)
            {
                emitNow.push_back({ false, a.channel, a.pitch, a.offVelocity });
            }
            active_.clear();
        }

        /// Focus-loss safety: immediate Note Off for previews whose Mouse Up could now be missed
        /// (still held), while quick-click previews keep their already-scheduled safe Note Off.
        void releaseHeldActive(std::vector<AuditionEvent>& emitNow)
        {
            for (size_t i = 0; i < active_.size();)
            {
                if (active_[i].held)
                {
                    emitNow.push_back({ false, active_[i].channel, active_[i].pitch,
                                        active_[i].offVelocity });
                    active_[i] = active_.back();
                    active_.pop_back();
                }
                else
                {
                    ++i;
                }
            }
        }

        /// True while any preview is sounding (drives the caller's drain timer).
        [[nodiscard]] bool hasActivePreviews() const noexcept { return !active_.empty(); }

    private:
        struct Active
        {
            int channel = 1;
            int pitch = 60;
            double downMs = 0.0;
            double minHoldMs = 0.0;
            /// Scheduled Note Off time; meaningful only once `held == false`.
            double offDueMs = 0.0;
            int offVelocity = 64;
            /// True until the gesture's Mouse Up arrives (`endPreview`).
            bool held = true;
        };

        [[nodiscard]] Active* findActive(const int channel, const int pitch) noexcept
        {
            for (Active& a : active_)
            {
                if (a.channel == channel && a.pitch == pitch)
                {
                    return &a;
                }
            }
            return nullptr;
        }

        [[nodiscard]] static int clampChannel(const int c) noexcept
        {
            return c < 1 ? 1 : (c > 16 ? 16 : c);
        }
        [[nodiscard]] static int clampPitch(const int p) noexcept
        {
            return p < 0 ? 0 : (p > 127 ? 127 : p);
        }
        [[nodiscard]] static int clampVelocity(const int v, const int lo) noexcept
        {
            return v < lo ? lo : (v > 127 ? 127 : v);
        }

        std::vector<Active> active_;
    };
} // namespace midi_audition
