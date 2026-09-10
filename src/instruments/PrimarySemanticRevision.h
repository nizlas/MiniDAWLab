#pragma once

// =============================================================================
// PrimarySemanticRevision — the host-managed monotonic Primary identity counter
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §9.4.2, revision 5; P1D preflight)
// =============================================================================
// The semantic validity identity of a Primary instrument's sound state is this
// monotonically increasing host-managed revision — NEVER a raw plugin-state blob
// hash (§9.4.2). Publication and obsolete-job checks compare a captured revision
// with the current revision (PI-028).
//
// Bump sources (wired in ExperimentalInstrumentHost; every DAL-observable Primary
// sound-state change listed in §9.4):
//   * plugin assignment, replacement or removal (load/unload paths);
//   * preset/state restore performed by DAL (project load, saved-state autoload);
//   * host-observed parameter changes (juce::AudioProcessorListener
//     audioProcessorParameterChanged — may arrive on ANY thread, incl. audio)
//     EXCEPT when emitted synchronously from inside this host's own MIDI-bearing
//     live processBlock (see PrimaryLiveProcessScope below — Fix B);
//   * juce::AudioProcessorListener::audioProcessorChanged, including
//     nonParameterStateChanged / programChanged / latencyChanged hints;
//   * conservative lifecycle invalidation (native editor open/close, §9.4.2);
//   * descriptor/version changes (covered by the replacement bump).
//
// Deliberately NOT bump sources (task contract + §9.4.2 note): render-relevant
// MIDI/CC/routing edits are FINGERPRINT inputs — musical edits never convert into
// plugin-state revisions. No plugin-state blob polling, no fresh-blob hash compare.
//
// Thread safety: bump() is lock/allocation-free and callable from any thread
// (plugin notification callbacks may run on the audio thread). Reads are relaxed:
// the revision is a monotonic counter compared for equality/ordering only.

#include <atomic>
#include <cstdint>

namespace mini_daw
{

class PrimarySemanticRevision final
{
public:
    /// Any thread. Returns the new revision (monotonic, never reused).
    std::uint64_t bump() noexcept
    {
        return revision_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    /// Any thread. 0 = no observable change recorded yet for this host slot.
    [[nodiscard]] std::uint64_t current() const noexcept
    {
        return revision_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> revision_{ 0 };
};

// =============================================================================
// Fix B (§9.4.2): MIDI-derived parameter-notification suppression
// =============================================================================
// Measured evidence (RVD diagnostic on the real project, VB3-II "Organ"): during
// plain transport playback the plugin emitted `audioProcessorParameterChanged`
// synchronously from inside its own `processBlock`, on the audio thread,
// exclusively in blocks carrying host-delivered MIDI/CC (625 of 625 callbacks in
// 20 s; zero in the idle phases) — falsely turning a Current proxy Stale without
// any user edit. Genuine user parameter edits arrived on the message thread,
// outside processBlock.
//
// Such synchronous notifications are derived runtime activity: the plugin
// reacting to MIDI/CC that DAL itself delivered and that is ALREADY a
// fingerprint input (render-relevant musical content, §11). They are not new
// project edits, so they must not bump the semantic revision.
//
// Scope mechanism: an RAII guard publishes in thread_local storage WHICH host
// object is currently executing its live Primary processBlock on THIS thread,
// and whether that block's merged MIDI buffer is non-empty. Because the context
// is per-thread AND host-identity-checked:
//   * a message-thread user edit while the audio thread is processing can never
//     be misclassified (different thread => empty thread_local context);
//   * another host processing on the same thread never suppresses THIS host's
//     notifications (token mismatch);
//   * nested scopes restore the previous context on exit, so nesting and future
//     processing remain safe.
// Suppression applies ONLY to audioProcessorParameterChanged. audioProcessorChanged,
// editor open/close, assignment/replacement, removal and DAL state restore keep
// their unconditional conservative bumps (they call bump() directly).

class PrimaryLiveProcessScope final
{
public:
    /// [Audio thread] Enter: `hostToken` identifies the host object whose live Primary
    /// processBlock is about to run on the CALLING thread; `blockHasHostDeliveredMidi` is
    /// whether the merged per-block MIDI buffer handed to the instance is non-empty.
    /// Lock/allocation-free (two thread_local writes).
    PrimaryLiveProcessScope(const void* hostToken, const bool blockHasHostDeliveredMidi) noexcept
        : prevToken_(tlsToken_), prevBlockHasMidi_(tlsBlockHasMidi_)
    {
        tlsToken_ = hostToken;
        tlsBlockHasMidi_ = blockHasHostDeliveredMidi;
    }

    ~PrimaryLiveProcessScope() noexcept
    {
        tlsToken_ = prevToken_;
        tlsBlockHasMidi_ = prevBlockHasMidi_;
    }

    PrimaryLiveProcessScope(const PrimaryLiveProcessScope&) = delete;
    PrimaryLiveProcessScope& operator=(const PrimaryLiveProcessScope&) = delete;

    /// [Any thread] True ONLY when the CALLING thread is currently inside `hostToken`'s own
    /// live processBlock and that block carries host-delivered MIDI/CC.
    [[nodiscard]] static bool
    currentThreadInsideMidiBearingLiveProcessBlock(const void* hostToken) noexcept
    {
        return hostToken != nullptr && tlsToken_ == hostToken && tlsBlockHasMidi_;
    }

private:
    inline static thread_local const void* tlsToken_ = nullptr;
    inline static thread_local bool tlsBlockHasMidi_ = false;

    const void* const prevToken_;
    const bool prevBlockHasMidi_;
};

/// [Any thread] The Fix B bump decision for a host-observed `audioProcessorParameterChanged`
/// notification from `hostToken`'s live instance. Suppressed (returns false, no bump) ONLY when
/// the calling thread is synchronously inside that host's MIDI-bearing live processBlock — a
/// derived runtime consequence of already-fingerprinted MIDI/CC, not a project edit. Every
/// other case keeps the conservative bump (returns true).
inline bool bumpForHostObservedParameterChange(PrimarySemanticRevision& rev,
                                               const void* hostToken) noexcept
{
    if (PrimaryLiveProcessScope::currentThreadInsideMidiBearingLiveProcessBlock(hostToken))
    {
        return false;
    }
    (void)rev.bump();
    return true;
}

} // namespace mini_daw
