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
//     audioProcessorParameterChanged — may arrive on ANY thread, incl. audio);
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

} // namespace mini_daw
