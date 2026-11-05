#pragma once

// =============================================================================
// IsolatedProtocol — what DAL and the isolated insert helper share
// =============================================================================
//
// Two channels (plan B.4):
//   * Control: JUCE ChildProcessCoordinator / ChildProcessWorker over a named pipe. Messages are
//     small binary records (`ControlKind`, request id, payload). Never used on an audio lane.
//   * Audio: one page-file-backed shared section per hosted instance (`SharedBlockHeader` followed
//     by `kChannels × maxBlock` floats, processed in place) plus an auto-reset event pair
//     (request, done). Created by DAL, opened by the helper.
//
// Ownership of a section (plan D.3): DAL owns it while `doneSeq == requestSeq`. DAL writes the
// input and header, then publishes `requestSeq + 1` (release) and signals "request". From then on
// the helper owns it until it stores `doneSeq = requestSeq` (release) after writing the output.
// A missed deadline does not hand the section back: DAL leaves it alone until it sees
// `doneSeq == requestSeq` again, so a late helper never writes into a buffer DAL is reusing.

#include <atomic>
#include <cstdint>

namespace mini_daw::isolation
{
inline constexpr const char* kHelperCommandLineUniqueId = "minidaw-isolated-insert-host";
inline constexpr int kChannels = 2;
/// Every section holds at least this many samples per channel, so ordinary device and export
/// block sizes never reallocate. Larger blocks are processed in chunks of this size.
inline constexpr int kMinSectionBlock = 8192;

enum class ControlKind : std::int32_t
{
    // DAL -> helper (each gets exactly one Reply with the same request id)
    Create = 1,          // payload: plugin description XML, sample rate, block size, state, section names
    Prepare = 2,         // id, sample rate, block size, non-realtime
    Release = 3,         // id
    Destroy = 4,         // id (runs the plug-in's destructor in the helper)
    GetState = 5,        // id -> state bytes
    SetState = 6,        // id, state bytes
    GetPreset = 7,       // id -> .vstpreset bytes
    SetPreset = 8,       // id, .vstpreset bytes
    OpenEditor = 9,      // id, window title
    CloseEditor = 10,    // id
    TestFault = 11,      // fault kind, argument (test builds and the focused tests only)
    Ping = 12,

    // helper -> DAL
    Reply = 100,         // request id, ok flag, payload
    Hello = 101,         // helper pid
    Dirty = 102,         // id: a parameter or the editor changed the plug-in
    EditorClosed = 103,  // id
};

enum class TestFault : std::int32_t
{
    None = 0,
    CrashOnNextDestroy = 1,
    CrashOnNextCreate = 2,
    CrashInProcessAfterBlocks = 3, // argument: blocks
    StallNextProcess = 4,          // argument: milliseconds; INT32_MAX = forever
    HangNextGetState = 5,          // argument: milliseconds
};

/// Bits of `SharedBlockHeader::transportFlags`.
enum TransportFlag : std::uint32_t
{
    kPlaying = 1u << 0,
    kRecording = 1u << 1,
    kLooping = 1u << 2,
    kHasPpq = 1u << 3,
    kHasBpm = 1u << 4,
    kHasTimeSig = 1u << 5,
    kHasLoop = 1u << 6,
    kHasTimeSamples = 1u << 7,
    kHasLastBar = 1u << 8,
};

/// Lives at the start of each shared section. Plain data plus lock-free 32-bit atomics, which
/// are address-free and therefore valid across processes on Windows x64.
struct SharedBlockHeader
{
    alignas(64) std::atomic<std::uint32_t> requestSeq { 0 }; // written by DAL
    alignas(64) std::atomic<std::uint32_t> doneSeq { 0 };    // written by the helper
    alignas(64) std::atomic<std::uint32_t> helperProgress { 0 }; // +1 per processed request
    std::int32_t maxBlock = 0;
    std::int32_t numSamples = 0;
    std::int32_t nonRealtime = 0;
    std::uint32_t transportFlags = 0;
    std::int64_t timeInSamples = 0;
    double ppqPosition = 0.0;
    double ppqLastBar = 0.0;
    double bpm = 120.0;
    std::int32_t timeSigNumerator = 4;
    std::int32_t timeSigDenominator = 4;
    double loopStartPpq = 0.0;
    double loopEndPpq = 0.0;
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared counters must be lock-free");

inline constexpr std::size_t headerBytes() noexcept
{
    return (sizeof(SharedBlockHeader) + 63u) & ~std::size_t(63u);
}

inline constexpr std::size_t sectionBytes(const int maxBlock) noexcept
{
    return headerBytes() + (std::size_t)kChannels * (std::size_t)maxBlock * sizeof(float);
}
} // namespace mini_daw::isolation
