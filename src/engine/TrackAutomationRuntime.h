#pragma once

// =============================================================================
// TrackAutomationRuntime — live Volume/Pan gestures (audio thread + message thread)
// =============================================================================
//
// The message thread pushes values. The audio thread stamps them at the samples it
// actually applies and is the only writer of the commit record. Releasing the mouse
// does not hand the track back to read-ahead: adoption stays held until the message
// thread has published the curve revision and the audio thread has passed the return
// ramp. The callback never waits on the message thread.
// =============================================================================

#include "domain/Track.h"

#include <array>
#include <atomic>
#include <cstdint>

inline constexpr int kTrackAutomationLaneEvents = 256;
inline constexpr int kTrackAutomationSlots = 8;
inline constexpr int kTrackAutomationRingCapacity = 8192;

struct ChannelAutomationGestureLane
{
    TrackId trackId = kInvalidTrackId;
    int parameterId = 0;
    int count = 0;
    std::array<double, kTrackAutomationLaneEvents> timeSeconds{};
    std::array<float, kTrackAutomationLaneEvents> value{};
    bool holding = false;
    float holdValue = 0.0f;
    bool ramping = false;
    double rampStartSeconds = 0.0;
    double rampEndSeconds = 0.0;
    float rampStartValue = 0.0f;
    float rampEndValue = 0.0f;
};

struct ChannelAutomationClock
{
    double engineRate = 0.0;
    std::int64_t blockStartSample = 0;
    int laneCount = 0;
    std::array<ChannelAutomationGestureLane, kTrackAutomationSlots> lanes{};

    [[nodiscard]] const ChannelAutomationGestureLane* find(const TrackId trackId,
                                                           const int parameterId) const noexcept
    {
        for (int i = 0; i < laneCount; ++i)
        {
            if (lanes[static_cast<size_t>(i)].trackId == trackId
                && lanes[static_cast<size_t>(i)].parameterId == parameterId)
            {
                return &lanes[static_cast<size_t>(i)];
            }
        }
        return nullptr;
    }
};

[[nodiscard]] inline float channelAutomationLaneValue(const ChannelAutomationGestureLane& lane,
                                                      const double seconds) noexcept
{
    if (lane.ramping && seconds >= lane.rampStartSeconds)
    {
        const double span = lane.rampEndSeconds - lane.rampStartSeconds;
        if (!(span > kTrackAutomationTimeMergeSeconds) || seconds >= lane.rampEndSeconds)
        {
            return lane.rampEndValue;
        }
        const double u = (seconds - lane.rampStartSeconds) / span;
        return static_cast<float>((1.0 - u) * lane.rampStartValue + u * lane.rampEndValue);
    }
    if (lane.count <= 0)
    {
        return lane.holdValue;
    }
    int i = lane.count - 1;
    while (i > 0 && lane.timeSeconds[static_cast<size_t>(i)] > seconds)
    {
        --i;
    }
    if (lane.timeSeconds[static_cast<size_t>(i)] > seconds)
    {
        return lane.holdValue;
    }
    return lane.value[static_cast<size_t>(i)];
}

[[nodiscard]] inline float channelAutomationValueAt(const Track& track,
                                                    const int parameterId,
                                                    const ChannelAutomationClock* clock,
                                                    const double seconds) noexcept
{
    const float manual = parameterId == kTrackAutomationPanId ? track.getStereoPan()
                                                              : track.getChannelFaderGain();
    if (clock != nullptr)
    {
        if (const ChannelAutomationGestureLane* lane = clock->find(track.getId(), parameterId))
        {
            return sanitizeTrackAutomationValue(parameterId, channelAutomationLaneValue(*lane, seconds));
        }
    }
    const TrackAutomationState& state = track.getAutomation();
    if (!state.read)
    {
        return manual;
    }
    const TrackAutomationCurve& curve = parameterId == kTrackAutomationPanId ? state.pan : state.volume;
    return trackAutomationEvaluate(curve, seconds, manual);
}

struct TrackAutomationCommit
{
    bool valid = false;
    bool writeEnabled = false;
    bool readEnabled = false;
    bool restartStroke = false;
    uint32_t revision = 0;
    double touchStartSeconds = 0.0;
    double releaseSeconds = 0.0;
    double rampEndSeconds = 0.0;
    float releaseValue = 0.0f;
    float rampEndValue = 0.0f;
    float preTouchValue = 0.0f;
    int eventCount = 0;
    std::array<TrackAutomationPoint, kTrackAutomationRingCapacity> events{};
};

/// Fixed slots. Message thread: begin / push / release / takeCommit / notePublished.
/// Audio thread: beginBlock / fillClock / adoptionHeld. No allocation on the audio thread.
class TrackAutomationRuntime
{
public:
    void begin(const TrackId trackId,
               const int parameterId,
               const bool writeEnabled,
               const bool readEnabled,
               const float value,
               const TrackAutomationCurve& frozenCurve,
               const float frozenManual) noexcept
    {
        Slot* slot = findMutable(trackId, parameterId);
        if (slot == nullptr)
        {
            slot = findFree();
        }
        if (slot == nullptr)
        {
            return;
        }
        if (slot->phase.load(std::memory_order_acquire) != 0)
        {
            slot->pending.store(true, std::memory_order_release);
            slot->pendingValue.store(value, std::memory_order_relaxed);
            return;
        }
        slot->trackId = trackId;
        slot->parameterId = parameterId;
        slot->writeEnabled = writeEnabled;
        slot->readEnabled = readEnabled;
        slot->frozen = frozenCurve;
        slot->frozenManual = frozenManual;
        slot->desiredRead.store(0, std::memory_order_relaxed);
        slot->desiredWrite.store(0, std::memory_order_relaxed);
        slot->appliedRead.store(0, std::memory_order_relaxed);
        slot->appliedWrite.store(0, std::memory_order_relaxed);
        slot->releaseRequested.store(false, std::memory_order_relaxed);
        slot->commitReady.store(false, std::memory_order_relaxed);
        slot->publishedRevision.store(0, std::memory_order_relaxed);
        slot->commitRevision = 0;
        slot->holdValue = sanitizeTrackAutomationValue(parameterId, value);
        slot->touchStartSeconds = -1.0;
        const uint32_t w = 0;
        slot->desired[w] = slot->holdValue;
        slot->desiredWrite.store(1, std::memory_order_release);
        slot->adoptionHeld.store(true, std::memory_order_release);
        slot->phase.store(1, std::memory_order_release);
    }

    [[nodiscard]] bool isCapturing(const TrackId trackId, const int parameterId) const noexcept
    {
        for (const Slot& slot : slots_)
        {
            if (slot.trackId == trackId && slot.parameterId == parameterId
                && slot.phase.load(std::memory_order_acquire) == 1)
            {
                return true;
            }
        }
        return false;
    }

    void push(const TrackId trackId, const int parameterId, const float value) noexcept
    {
        Slot* slot = findMutable(trackId, parameterId);
        if (slot == nullptr || slot->phase.load(std::memory_order_acquire) != 1)
        {
            return;
        }
        const uint32_t w = slot->desiredWrite.load(std::memory_order_relaxed);
        const uint32_t r = slot->desiredRead.load(std::memory_order_acquire);
        if (w - r >= kTrackAutomationRingCapacity)
        {
            slot->overflow.store(true, std::memory_order_relaxed);
            return;
        }
        slot->desired[w % kTrackAutomationRingCapacity] = sanitizeTrackAutomationValue(parameterId, value);
        slot->desiredWrite.store(w + 1, std::memory_order_release);
    }

    void requestRelease(const TrackId trackId, const int parameterId) noexcept
    {
        if (Slot* slot = findMutable(trackId, parameterId))
        {
            slot->releaseRequested.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] bool adoptionHeld(const TrackId trackId) const noexcept
    {
        for (const Slot& slot : slots_)
        {
            if (slot.trackId == trackId && slot.adoptionHeld.load(std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool takeCommit(const TrackId trackId,
                                  const int parameterId,
                                  TrackAutomationCommit& out) noexcept
    {
        Slot* slot = findMutable(trackId, parameterId);
        if (slot == nullptr || !slot->commitReady.load(std::memory_order_acquire))
        {
            return false;
        }
        out = {};
        out.valid = true;
        out.writeEnabled = slot->writeEnabled;
        out.readEnabled = slot->readEnabled;
        out.restartStroke = slot->restartStroke;
        out.revision = slot->commitRevision;
        out.touchStartSeconds = slot->touchStartSeconds;
        out.releaseSeconds = slot->releaseSeconds;
        out.rampEndSeconds = slot->rampEndSeconds;
        out.releaseValue = slot->releaseValue;
        out.rampEndValue = slot->rampEndValue;
        out.preTouchValue = slot->frozenManual;
        const uint32_t w = slot->appliedWrite.load(std::memory_order_acquire);
        const uint32_t r = slot->appliedRead.load(std::memory_order_relaxed);
        const uint32_t n = w - r;
        out.eventCount = static_cast<int>(std::min<uint32_t>(n, kTrackAutomationRingCapacity));
        for (uint32_t i = 0; i < static_cast<uint32_t>(out.eventCount); ++i)
        {
            const Applied& ev = slot->applied[(r + i) % kTrackAutomationRingCapacity];
            out.events[i] = { ev.timeSeconds, ev.value };
        }
        slot->appliedRead.store(w, std::memory_order_release);
        slot->commitReady.store(false, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool takeAnyCommit(TrackId& trackId,
                                     int& parameterId,
                                     TrackAutomationCommit& out) noexcept
    {
        for (Slot& slot : slots_)
        {
            if (!slot.commitReady.load(std::memory_order_acquire))
            {
                continue;
            }
            trackId = slot.trackId;
            parameterId = slot.parameterId;
            return takeCommit(trackId, parameterId, out);
        }
        return false;
    }

    void notePublished(const TrackId trackId, const int parameterId, const uint32_t revision) noexcept
    {
        if (Slot* slot = findMutable(trackId, parameterId))
        {
            slot->publishedRevision.store(revision, std::memory_order_release);
        }
    }

    /// [Audio thread] Drain new values, stamp them at this block, and close a stroke whose
    /// release was requested. `discontinuity` ends the stroke at the previous block end and
    /// does not draw a ramp across the jump.
    void audioThread_beginBlock(const double blockStartSeconds,
                                const double engineRate,
                                const bool discontinuity,
                                const double previousBlockEndSeconds) noexcept
    {
        for (Slot& slot : slots_)
        {
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase == 0)
            {
                continue;
            }
            if (phase == 1)
            {
                if (discontinuity && previousBlockEndSeconds > 0.0)
                {
                    closeStroke(slot, previousBlockEndSeconds, engineRate, false);
                    continue;
                }
                const uint32_t w = slot.desiredWrite.load(std::memory_order_acquire);
                uint32_t r = slot.desiredRead.load(std::memory_order_relaxed);
                int stamped = 0;
                while (r != w && stamped < kTrackAutomationLaneEvents)
                {
                    const float value = slot.desired[r % kTrackAutomationRingCapacity];
                    const double time = blockStartSeconds
                                        + static_cast<double>(stamped) / engineRate;
                    pushApplied(slot, time, value);
                    if (slot.touchStartSeconds < 0.0)
                    {
                        slot.touchStartSeconds = time;
                    }
                    slot.holdValue = value;
                    ++r;
                    ++stamped;
                }
                slot.desiredRead.store(r, std::memory_order_release);
                if (slot.releaseRequested.load(std::memory_order_acquire) && r == w)
                {
                    const double releaseTime = blockStartSeconds + static_cast<double>(stamped) / engineRate;
                    closeStroke(slot, releaseTime, engineRate, true);
                }
            }
            else if (phase == 2 || phase == 3)
            {
                const uint32_t published = slot.publishedRevision.load(std::memory_order_acquire);
                const bool rampDone = blockStartSeconds >= slot.rampEndSeconds - kTrackAutomationTimeMergeSeconds;
                if (published == slot.commitRevision && published != 0 && rampDone)
                {
                    slot.adoptionHeld.store(false, std::memory_order_release);
                    slot.phase.store(0, std::memory_order_release);
                    slot.trackId = kInvalidTrackId;
                }
            }
        }
    }

    void audioThread_fillClock(ChannelAutomationClock& clock, const double engineRate) const noexcept
    {
        clock.engineRate = engineRate;
        clock.laneCount = 0;
        for (const Slot& slot : slots_)
        {
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase == 0 || clock.laneCount >= kTrackAutomationSlots)
            {
                continue;
            }
            ChannelAutomationGestureLane& lane = clock.lanes[static_cast<size_t>(clock.laneCount++)];
            lane = {};
            lane.trackId = slot.trackId;
            lane.parameterId = slot.parameterId;
            lane.holding = true;
            lane.holdValue = slot.holdValue;
            lane.ramping = phase == 2;
            lane.rampStartSeconds = slot.releaseSeconds;
            lane.rampEndSeconds = slot.rampEndSeconds;
            lane.rampStartValue = slot.releaseValue;
            lane.rampEndValue = slot.rampEndValue;
            const uint32_t w = slot.appliedWrite.load(std::memory_order_acquire);
            const uint32_t r = slot.appliedRead.load(std::memory_order_acquire);
            const uint32_t available = w >= r ? w - r : 0;
            const int n = static_cast<int>(std::min<uint32_t>(available, kTrackAutomationLaneEvents));
            const uint32_t start = w - static_cast<uint32_t>(n);
            for (int i = 0; i < n; ++i)
            {
                const Applied& ev = slot.applied[(start + static_cast<uint32_t>(i)) % kTrackAutomationRingCapacity];
                lane.timeSeconds[static_cast<size_t>(i)] = ev.timeSeconds;
                lane.value[static_cast<size_t>(i)] = ev.value;
            }
            lane.count = n;
        }
    }

    [[nodiscard]] int audioThread_adoptionHoldTracks(TrackId* const out, const int capacity) const noexcept
    {
        int n = 0;
        for (const Slot& slot : slots_)
        {
            if (!slot.adoptionHeld.load(std::memory_order_acquire) || n >= capacity)
            {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < n; ++i)
            {
                if (out[i] == slot.trackId)
                {
                    seen = true;
                }
            }
            if (!seen)
            {
                out[n++] = slot.trackId;
            }
        }
        return n;
    }

private:
    struct Applied
    {
        double timeSeconds = 0.0;
        float value = 0.0f;
    };

    struct Slot
    {
        TrackId trackId = kInvalidTrackId;
        int parameterId = 0;
        bool writeEnabled = false;
        bool readEnabled = false;
        bool restartStroke = false;
        TrackAutomationCurve frozen{};
        float frozenManual = 0.0f;
        float holdValue = 0.0f;
        double touchStartSeconds = -1.0;
        double releaseSeconds = 0.0;
        double rampEndSeconds = 0.0;
        float releaseValue = 0.0f;
        float rampEndValue = 0.0f;
        uint32_t commitRevision = 0;
        std::atomic<int> phase{ 0 }; ///< 0 idle, 1 capture, 2 ramp, 3 hold until publish
        std::atomic<bool> adoptionHeld{ false };
        std::atomic<bool> releaseRequested{ false };
        std::atomic<bool> commitReady{ false };
        std::atomic<bool> pending{ false };
        std::atomic<bool> overflow{ false };
        std::atomic<float> pendingValue{ 0.0f };
        std::atomic<uint32_t> publishedRevision{ 0 };
        std::atomic<uint32_t> desiredWrite{ 0 };
        std::atomic<uint32_t> desiredRead{ 0 };
        std::atomic<uint32_t> appliedWrite{ 0 };
        std::atomic<uint32_t> appliedRead{ 0 };
        std::array<float, kTrackAutomationRingCapacity> desired{};
        std::array<Applied, kTrackAutomationRingCapacity> applied{};
    };

    Slot* findMutable(const TrackId trackId, const int parameterId) noexcept
    {
        for (Slot& slot : slots_)
        {
            if (slot.trackId == trackId && slot.parameterId == parameterId)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    Slot* findFree() noexcept
    {
        for (Slot& slot : slots_)
        {
            if (slot.phase.load(std::memory_order_acquire) == 0)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    static void pushApplied(Slot& slot, const double timeSeconds, const float value) noexcept
    {
        const uint32_t w = slot.appliedWrite.load(std::memory_order_relaxed);
        const uint32_t r = slot.appliedRead.load(std::memory_order_acquire);
        if (w - r >= kTrackAutomationRingCapacity)
        {
            slot.overflow.store(true, std::memory_order_relaxed);
            return;
        }
        slot.applied[w % kTrackAutomationRingCapacity] = { timeSeconds, value };
        slot.appliedWrite.store(w + 1, std::memory_order_release);
    }

    static void closeStroke(Slot& slot, const double releaseTime, const double engineRate, const bool allowRamp) noexcept
    {
        slot.releaseSeconds = releaseTime;
        slot.releaseValue = slot.holdValue;
        double rampEnd = releaseTime;
        float rampEndValue = slot.holdValue;
        if (allowRamp && slot.readEnabled && engineRate > 0.0)
        {
            rampEnd = releaseTime + kTrackAutomationReturnRampSeconds;
            rampEndValue = trackAutomationEvaluate(slot.frozen, rampEnd, slot.frozenManual);
        }
        slot.rampEndSeconds = rampEnd;
        slot.rampEndValue = rampEndValue;
        slot.restartStroke = !slot.releaseRequested.load(std::memory_order_acquire);
        slot.commitRevision += 1;
        if (slot.commitRevision == 0)
        {
            slot.commitRevision = 1;
        }
        slot.commitReady.store(true, std::memory_order_release);
        slot.phase.store(allowRamp && slot.readEnabled ? 2 : 3, std::memory_order_release);
    }

    std::array<Slot, kTrackAutomationSlots> slots_{};
};
