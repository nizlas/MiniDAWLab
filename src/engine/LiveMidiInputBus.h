#pragma once

// =============================================================================
// LiveMidiInputBus — realtime core of live MIDI input (keyboard → instrument hosts → take capture)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   The one place where MIDI received from device callback threads crosses into the audio
//   callback. It owns:
//     * one preallocated SPSC ring per device slot (producer: that device's JUCE MIDI thread,
//       consumer: the audio thread) — several ports never share a ring, so their callbacks never
//       contend with each other or with the audio thread;
//     * the published, immutable **routing snapshot** (which rows listen to which device /
//       channel filter, whether they monitor and/or capture) — message thread builds, audio thread
//       acquire-loads once per block, exactly like the session and instrument snapshots;
//     * **live-note ownership**: every Note On delivered to a host is remembered with the host and
//       the effective channel it was sent on, so the matching Note Off reaches the same place even
//       if routing, output channel or Monitor changed in between, and so cleanup for one row never
//       has to fall back to a global All Notes Off that would also silence other rows;
//     * the **capture ring** (producer: audio thread, consumer: message thread) that carries every
//       event of an armed row, stamped with the transport-timeline sample of the player's gesture,
//       to the message-thread take capture.
//
// TIME MODEL (documented here because it is the contract the recorder relies on)
//   Device callbacks stamp each message with `juce::Time::getMillisecondCounterHiRes()` (seconds,
//   see JUCE's Windows MIDI backend). The audio callback reads the same clock on entry
//   (`BlockContext::nowMs`) and remembers the previous entry time. Two different mappings are
//   derived from one timestamp:
//     * LIVE DELIVERY offset inside the current block: events that arrived in the interval
//       (previousCallbackMs, nowMs] are spread proportionally over the block's samples, so their
//       mutual timing is preserved at the cost of the inherent one-block latency (an event cannot
//       be rendered before the block it arrives in). Events arriving while the callback itself
//       runs clamp to the last sample.
//     * RECORDED POSITION on the transport timeline: `playheadAtBlockStart − (nowMs − t) · sr +
//       recordPlacementOffsetSamples` — the moment the key was actually pressed, projected back
//       onto the timeline the player was hearing. The placement offset is the device's reported
//       OUTPUT latency negated (what the player heard at time t was rendered that much earlier),
//       supplied by the coordinator; it is deliberately not the audio input recording offset and
//       involves no plugin-latency compensation.
//   Nothing here reads the UI playhead or repaint timing; while the transport is stopped the
//   captured events carry `transportPlaying == false` and the take capture ignores them except
//   for keeping its held-note / controller state current.
//
// REALTIME CONTRACT
//   `audioThread_*` functions allocate nothing, take no locks and never dereference a host: they
//   hand `ExperimentalInstrumentHost*` + sample offset + message to the caller-supplied delivery
//   function (the engine's adapter calls `audioThread_addMidiEventForCurrentBlock`). Host pointers
//   come from the instrument playback snapshot of the SAME block, so lifetime follows the
//   established publish-before-destroy / drain discipline. Overflow of a device ring is counted
//   and resolved by releasing that device's live notes and marking the take (never a silent loss
//   of a Note Off).
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "domain/Track.h"

class ExperimentalInstrumentHost;
class SessionSnapshot;
struct ExperimentalInstrumentPlaybackSnapshot;

namespace live_midi
{
    inline constexpr int kMaxDeviceSlots = 16;
    inline constexpr int kDeviceSlotAll = -1;
    /// A route whose assigned device is not present: matches nothing, keeps its configuration.
    inline constexpr int kDeviceSlotMissing = -2;
    inline constexpr int kMaxRoutes = 64;
    inline constexpr int kDeviceRingCapacity = 2048;
    inline constexpr int kCaptureRingCapacity = 16384;
    inline constexpr int kMaxLiveNotes = 512;
    /// Per-block drain budget per device (bounds callback work; the rest waits one block).
    inline constexpr int kMaxEventsPerDevicePerBlock = 512;

    /// One raw channel-voice message as received (1 … 3 bytes) with its device timestamp.
    struct RawEvent
    {
        double timeStampSeconds = 0.0;
        std::uint8_t bytes[3] = { 0, 0, 0 };
        std::uint8_t size = 0;
    };

    struct Route
    {
        TrackId trackId = kInvalidTrackId;
        /// `kDeviceSlotAll`, `kDeviceSlotMissing`, or 0 … kMaxDeviceSlots-1.
        int deviceSlot = kDeviceSlotMissing;
        /// `kTrackMidiInputChannelAll` or 1 … 16 — an INPUT filter, never a remap.
        int channelFilter = kTrackMidiInputChannelAll;
        /// Live monitoring: deliver to the destination host this block.
        bool monitor = false;
        /// Armed: forward to the capture ring (the take capture decides what is recorded).
        bool capture = false;
    };

    struct RoutingSnapshot
    {
        std::vector<Route> routes;
        std::uint32_t revision = 0;
    };

    struct CapturedEvent
    {
        TrackId trackId = kInvalidTrackId;
        /// Transport-timeline sample of the gesture (see TIME MODEL). Meaningful only while
        /// `transportPlaying`; otherwise the stopped playhead.
        std::int64_t timelineSample = 0;
        double timeStampSeconds = 0.0;
        std::uint8_t bytes[3] = { 0, 0, 0 };
        std::uint8_t size = 0;
        bool transportPlaying = false;
        /// Synthesized marker: the source device ring overflowed before this point — events were
        /// lost, this row's live notes were released. The take reports it; nothing is invented.
        bool overflowMarker = false;
    };

    struct BlockContext
    {
        const SessionSnapshot* session = nullptr;
        const ExperimentalInstrumentPlaybackSnapshot* instruments = nullptr;
        int numSamples = 0;
        double sampleRate = 48000.0;
        /// `juce::Time::getMillisecondCounterHiRes()` at callback entry.
        double nowMs = 0.0;
        std::int64_t playheadAtBlockStart = 0;
        bool transportPlaying = false;
        /// Added to every recorded timeline position (normally −reported output latency).
        std::int64_t recordPlacementOffsetSamples = 0;
    };

    /// Delivery adapter: `host` is a snapshot pointer of this block; the engine forwards to
    /// `ExperimentalInstrumentHost::audioThread_addMidiEventForCurrentBlock`.
    using DeliverFn = void (*)(void* context, ExperimentalInstrumentHost* host, int sampleOffset,
                               const juce::MidiMessage& message) noexcept;

    class LiveMidiInputBus final
    {
    public:
        LiveMidiInputBus();

        // ------------------------------------------------------------------ device threads
        /// [Any MIDI device callback thread, one per slot] Enqueue a received message. SysEx and
        /// messages longer than three bytes are dropped (counted); on a full ring the message is
        /// dropped, the slot's overflow counter advances and — when the lost message could end a
        /// note (Note Off, Note On velocity 0, sustain release) — the "note-off lost" flag is set
        /// so the audio thread releases that device's live notes instead of leaving them hanging.
        void deviceThread_push(int slot, const juce::MidiMessage& message) noexcept;

        // ------------------------------------------------------------------ message thread
        /// Publish the routing (release-store). The audio thread reconciles live notes against
        /// the new routes on its next block (targeted Note Offs for rows that stopped monitoring,
        /// changed device/filter, or lost their destination).
        void publishRouting(std::shared_ptr<const RoutingSnapshot> snapshot) noexcept;
        [[nodiscard]] std::shared_ptr<const RoutingSnapshot> currentRouting() const noexcept;

        /// Pop one captured event (message-thread consumer of the capture ring).
        bool popCaptured(CapturedEvent& out) noexcept;
        /// Captured events dropped because the message thread drained too slowly (relaxed).
        [[nodiscard]] std::uint32_t captureOverflowCount() const noexcept
        {
            return captureOverflow_.load(std::memory_order_relaxed);
        }
        /// Per-route activity: count of events matched for `routeIndex` (relaxed, monotonic).
        [[nodiscard]] std::uint32_t routeActivityCount(int routeIndex) const noexcept;
        [[nodiscard]] std::uint32_t deviceOverflowCount(int slot) const noexcept;
        [[nodiscard]] std::uint32_t droppedUnsupportedCount() const noexcept
        {
            return droppedUnsupported_.load(std::memory_order_relaxed);
        }
        /// Number of live notes currently owned (relaxed snapshot; diagnostics).
        [[nodiscard]] int liveNoteCountForDiagnostics() const noexcept
        {
            return liveNoteCount_.load(std::memory_order_relaxed);
        }
        /// Total events delivered to hosts (relaxed; diagnostics / tests).
        [[nodiscard]] std::uint64_t deliveredEventCount() const noexcept
        {
            return deliveredEvents_.load(std::memory_order_relaxed);
        }

        // ------------------------------------------------------------------ audio thread
        /// [Audio thread] Drain every device ring, deliver monitored events into the resolved
        /// hosts with sample offsets, forward captured events, maintain live-note ownership.
        /// Must run after the hosts' `audioThread_beginAudioBlock` and before they process.
        void audioThread_dispatch(const BlockContext& ctx, DeliverFn deliver, void* deliverContext) noexcept;

        /// [Audio thread] The callback is gated (offline export) or instruments are suspended:
        /// discard everything pending so the rings cannot overflow, forget live notes (the hosts
        /// are being driven by the offline render, which does not see live input).
        void audioThread_discardPendingAndForgetNotes() noexcept;

        /// [Audio thread] Release every live note into its owning host (e.g. before the engine
        /// stops). Hosts are only addressed through `instruments` of the current block.
        void audioThread_releaseAllLiveNotes(const ExperimentalInstrumentPlaybackSnapshot* instruments,
                                             DeliverFn deliver, void* deliverContext) noexcept;

        // ------------------------------------------------------------------ pure helpers (tests)
        /// Live-delivery sample offset for a timestamp inside the current block window.
        [[nodiscard]] static int offsetForTimestamp(double timeStampSeconds, double previousCallbackMs,
                                                    double nowMs, int numSamples) noexcept;
        /// Recorded timeline position for a timestamp (see TIME MODEL).
        [[nodiscard]] static std::int64_t timelineSampleForTimestamp(double timeStampSeconds,
                                                                     const BlockContext& ctx) noexcept;
        /// True for the channel-voice messages the bus forwards (notes, CC, pitch bend,
        /// aftertouch / channel pressure for monitoring); false for SysEx, program change,
        /// realtime and system messages.
        [[nodiscard]] static bool isForwardedMessage(const juce::MidiMessage& m) noexcept;
        /// True for the subset the take capture records (notes, CC, pitch bend).
        [[nodiscard]] static bool isCapturedMessage(const juce::MidiMessage& m) noexcept;

    private:
        struct DeviceRing
        {
            juce::AbstractFifo fifo { kDeviceRingCapacity };
            std::vector<RawEvent> buffer;
            std::atomic<std::uint32_t> overflow { 0 };
            std::atomic<bool> noteOffLost { false };
            std::uint32_t overflowSeenByAudio = 0; ///< audio-thread private
        };

        struct LiveNote
        {
            TrackId trackId = kInvalidTrackId;
            ExperimentalInstrumentHost* host = nullptr;
            std::uint8_t effectiveChannel = 1;
            std::uint8_t receivedChannel = 1;
            std::uint8_t note = 0;
            bool active = false;
        };

        struct ResolvedDestination
        {
            ExperimentalInstrumentHost* host = nullptr;
            int outputChannelSetting = kTrackMidiOutputChannelAny;
            bool trackPresent = false;
        };

        [[nodiscard]] static ResolvedDestination resolveDestination(TrackId sourceTrackId, const BlockContext& ctx) noexcept;
        [[nodiscard]] static bool hostIsInSnapshot(const ExperimentalInstrumentHost* host,
                                                   const ExperimentalInstrumentPlaybackSnapshot* instruments) noexcept;

        void audioThread_reconcileLiveNotes(const RoutingSnapshot* routing, const BlockContext& ctx,
                                            DeliverFn deliver, void* deliverContext) noexcept;
        void audioThread_releaseNotesForRoute(TrackId trackId, const ExperimentalInstrumentPlaybackSnapshot* instruments,
                                              DeliverFn deliver, void* deliverContext) noexcept;
        void audioThread_rememberNoteOn(TrackId trackId, ExperimentalInstrumentHost* host, int effectiveChannel,
                                        int receivedChannel, int note) noexcept;
        /// Returns the owning host/channel of a sounding note and forgets it; false if unknown.
        bool audioThread_takeNoteOff(TrackId trackId, int receivedChannel, int note,
                                     ExperimentalInstrumentHost*& outHost, int& outEffectiveChannel) noexcept;
        void audioThread_pushCaptured(const CapturedEvent& e) noexcept;

        std::array<DeviceRing, (size_t)kMaxDeviceSlots> devices_;

        std::shared_ptr<const RoutingSnapshot> routing_;
        std::uint32_t lastReconciledRevision_ = 0; ///< audio-thread private

        juce::AbstractFifo captureFifo_ { kCaptureRingCapacity };
        std::vector<CapturedEvent> captureBuffer_;
        std::atomic<std::uint32_t> captureOverflow_ { 0 };

        std::array<LiveNote, (size_t)kMaxLiveNotes> liveNotes_ {};
        std::atomic<int> liveNoteCount_ { 0 };

        std::array<std::atomic<std::uint32_t>, (size_t)kMaxRoutes> routeActivity_ {};
        std::atomic<std::uint32_t> droppedUnsupported_ { 0 };
        std::atomic<std::uint64_t> deliveredEvents_ { 0 };

        double previousCallbackMs_ = 0.0; ///< audio-thread private
        bool haveStaleNotesToDrop_ = false;
    };
} // namespace live_midi
