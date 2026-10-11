#pragma once

// =============================================================================
// TrackAutomationRuntime — live Volume/Pan gestures (audio thread + message thread)
// =============================================================================
//
// The message thread pushes values stamped with a host clock. The audio thread
// maps each stamp across the block that first sees it, in proportion to when it
// arrived, and is the only writer of those applied times. Live playback lerps
// those points and then holds; the saved curve uses the same points and the same
// linear pieces. A release always stores the held value at the release time, so
// a still fader does not start its return early.
//
// The message thread drains applied points while the gesture is open so a long
// drag does not fill the ring. The audio thread never allocates, blocks, or
// waits. If the ring does fill, captured points are kept, the gesture is marked
// incomplete, and the next gesture can start.
//
// Re-adoption waits until the snapshot the callback actually loaded carries the
// revision published for this stroke, and the return ramp has ended. A rejected
// edit, a missing track, or a stopped device clears the hold without that ack.
// =============================================================================

#include "domain/Track.h"

#include <array>
#include <atomic>
#include <cstdint>

#if defined(MINIDAW_AUTOMATION_HANDOVER_PROBE)
/// Test-only gates. Production does not compile these. Each function runs on the
/// thread that hit the window; the test uses it to wake the other thread.
struct TrackAutomationHandoverProbe
{
    void (*beforeClosePublish)(void* context) = nullptr;
    void (*duringRetire)(void* context) = nullptr;
    void (*duringDrain)(void* context) = nullptr;
    void* context = nullptr;
};
#endif

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
        const ChannelAutomationGestureLane* found = nullptr;
        for (int i = 0; i < laneCount; ++i)
        {
            if (lanes[static_cast<size_t>(i)].trackId == trackId
                && lanes[static_cast<size_t>(i)].parameterId == parameterId)
            {
                found = &lanes[static_cast<size_t>(i)];
            }
        }
        return found;
    }
};

/// Lerp between stamped points, then hold. A return ramp uses the same linear
/// span the saved curve stores from the release point to the ramp end.
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
        return static_cast<float>((1.0 - u) * static_cast<double>(lane.rampStartValue)
                                  + u * static_cast<double>(lane.rampEndValue));
    }
    if (lane.count <= 0)
    {
        return lane.holdValue;
    }
    if (seconds <= lane.timeSeconds[0])
    {
        return lane.value[0];
    }
    const int last = lane.count - 1;
    if (seconds >= lane.timeSeconds[static_cast<size_t>(last)])
    {
        return lane.value[static_cast<size_t>(last)];
    }
    int hi = last;
    int lo = 0;
    while (lo + 1 < hi)
    {
        const int mid = lo + (hi - lo) / 2;
        if (lane.timeSeconds[static_cast<size_t>(mid)] <= seconds)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    const double t0 = lane.timeSeconds[static_cast<size_t>(lo)];
    const double t1 = lane.timeSeconds[static_cast<size_t>(hi)];
    const double span = t1 - t0;
    if (!(span > kTrackAutomationTimeMergeSeconds))
    {
        return lane.value[static_cast<size_t>(hi)];
    }
    const double u = (seconds - t0) / span;
    return static_cast<float>((1.0 - u) * static_cast<double>(lane.value[static_cast<size_t>(lo)])
                              + u * static_cast<double>(lane.value[static_cast<size_t>(hi)]));
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
    /// The pointer was still down. The stroke ended at a jump and another stroke
    /// of the same gesture may follow. This commit is not an undo boundary.
    bool restartStroke = false;
    /// The ring filled. Captured points are in `events`; the gesture is not complete.
    bool incomplete = false;
    uint32_t gestureId = 0;
    /// Slot-local id of this stroke. The snapshot revision is assigned later.
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

/// Fixed slots. The audio callback never allocates, locks, or waits.
///
/// Ownership of one slot:
/// - phase 0 free: payload is inert. Only `begin` may claim it, by swapping
///   0 → arming. It publishes phase 1 only after the payload is complete.
/// - phase 1 capture: message thread owns the desired ring and the release
///   request. Audio thread owns applied times, the hold value, and closes.
/// - phase 2 ramp / 3 hold: audio thread reads the ramp. Message thread takes
///   closes and notes the snapshot revision.
/// - phase 4 retiring: one thread owns the wipe. phase 0 is stored last, so a
///   claim cannot see a half-cleared slot. A reader that loaded an older phase
///   copies values out and drops them if the phase changed.
/// - phase 5 arming: the claiming `begin` owns every field.
///
/// Closes are a two-deep queue. The audio thread fills a `CloseMark` and then
/// release-stores `closeWrite`. `takeCommit` acquire-loads that index, copies
/// the mark, and only then release-stores `closeRead`. The mark is not visible
/// before the release, and it is not overwritten before the ack. A full queue
/// keeps the captured points and marks the gesture incomplete; it does not
/// drop a close that was already published.
/// The applied ring is the same kind of queue: the audio thread publishes
/// `appliedWrite`, `drainBefore` copies and then release-stores `appliedRead`.
/// `waitingLocal`, `requiredSnapshot` and `rejected` are written by the message
/// thread after it owns the close, and read by the audio thread in `observe`.
/// The audio thread does not write them. `clearSlot` refuses an untaken close.
/// `snapshotReplaced` retires a ramp or hold whose close was already taken.
/// `reset` advances the read index to the write index first, because the
/// project is gone, and then retires. `deviceStopped` runs with no callback
/// in flight: it closes an open capture, keeps an untaken close, and retires
/// the rest.
class TrackAutomationRuntime
{
public:
    /// Test hook. The audio thread reads this as the ring limit. Production stays
    /// at `kTrackAutomationRingCapacity`.
    void setCapacityForTest(const int capacity) noexcept
    {
        const int clamped = capacity < 2 ? 2 : (capacity > kTrackAutomationRingCapacity
                                                    ? kTrackAutomationRingCapacity
                                                    : capacity);
        capacity_.store(clamped, std::memory_order_relaxed);
    }

    void begin(const TrackId trackId,
               const int parameterId,
               const bool writeEnabled,
               const bool readEnabled,
               const float value,
               const double hostSeconds,
               const uint32_t gestureId,
               const TrackAutomationCurve& frozenCurve,
               const float frozenManual) noexcept
    {
        if (findCapturing(trackId, parameterId) != nullptr)
        {
            return;
        }
        Slot* slot = claimFree();
        if (slot == nullptr)
        {
            overflowTrack_.store(trackId, std::memory_order_relaxed);
            overflowParam_.store(parameterId, std::memory_order_relaxed);
            overflowPending_.store(true, std::memory_order_release);
            return;
        }
        slot->trackId.store(trackId, std::memory_order_relaxed);
        slot->parameterId.store(parameterId, std::memory_order_relaxed);
        slot->gestureId.store(gestureId == 0 ? 1u : gestureId, std::memory_order_relaxed);
        slot->writeEnabled.store(writeEnabled, std::memory_order_relaxed);
        slot->readEnabled.store(readEnabled, std::memory_order_relaxed);
        slot->frozen = frozenCurve;
        slot->frozenManual = frozenManual;
        slot->desiredRead.store(0, std::memory_order_relaxed);
        slot->desiredWrite.store(0, std::memory_order_relaxed);
        slot->appliedRead.store(0, std::memory_order_relaxed);
        slot->appliedWrite.store(0, std::memory_order_relaxed);
        slot->closeRead.store(0, std::memory_order_relaxed);
        slot->closeWrite.store(0, std::memory_order_relaxed);
        slot->releaseRequested.store(false, std::memory_order_relaxed);
        slot->incomplete.store(false, std::memory_order_relaxed);
        slot->rejected.store(false, std::memory_order_relaxed);
        slot->requiredSnapshot.store(0, std::memory_order_relaxed);
        slot->waitingLocal.store(0, std::memory_order_relaxed);
        slot->publishRequired.store(false, std::memory_order_relaxed);
        slot->retireRequested.store(false, std::memory_order_relaxed);
        const float sanitized = sanitizeTrackAutomationValue(parameterId, value);
        slot->holdValue.store(sanitized, std::memory_order_relaxed);
        slot->touchStartSeconds = -1.0;
        slot->localRevision = 0;
        const uint32_t w = 0;
        slot->desired[w] = { sanitized, hostSeconds };
        slot->desiredWrite.store(1, std::memory_order_release);
        slot->adoptionHeld.store(true, std::memory_order_release);
        slot->phase.store(kPhaseCapture, std::memory_order_release);
    }

    [[nodiscard]] bool isCapturing(const TrackId trackId, const int parameterId) const noexcept
    {
        return findCapturingConst(trackId, parameterId) != nullptr;
    }

    int capturingParameters(TrackId* const ids,
                            int* const params,
                            uint32_t* const gestures,
                            const int capacity) const noexcept
    {
        int n = 0;
        if (ids == nullptr || params == nullptr || gestures == nullptr || capacity <= 0)
        {
            return 0;
        }
        for (const Slot& slot : slots_)
        {
            if (slot.phase.load(std::memory_order_acquire) != kPhaseCapture || n >= capacity)
            {
                continue;
            }
            ids[n] = slot.trackId.load(std::memory_order_relaxed);
            params[n] = slot.parameterId.load(std::memory_order_relaxed);
            gestures[n] = slot.gestureId.load(std::memory_order_relaxed);
            ++n;
        }
        return n;
    }

    [[nodiscard]] bool gestureBusy(const uint32_t gestureId) const noexcept
    {
        if (gestureId == 0)
        {
            return false;
        }
        for (const Slot& slot : slots_)
        {
            if (slot.gestureId.load(std::memory_order_relaxed) != gestureId)
            {
                continue;
            }
            const int phase = slot.phase.load(std::memory_order_acquire);
            const uint32_t published = slot.closeWrite.load(std::memory_order_acquire);
            const uint32_t taken = slot.closeRead.load(std::memory_order_acquire);
            if (phase == kPhaseCapture || published != taken)
            {
                return true;
            }
        }
        return false;
    }

    void push(const TrackId trackId, const int parameterId, const float value, const double hostSeconds) noexcept
    {
        Slot* slot = findCapturing(trackId, parameterId);
        if (slot == nullptr)
        {
            return;
        }
        const uint32_t w = slot->desiredWrite.load(std::memory_order_relaxed);
        const uint32_t r = slot->desiredRead.load(std::memory_order_acquire);
        if (w - r >= static_cast<uint32_t>(capacity_.load(std::memory_order_relaxed)))
        {
            markOverflow(*slot);
            return;
        }
        slot->desired[w % kTrackAutomationRingCapacity] = { sanitizeTrackAutomationValue(parameterId, value),
                                                            hostSeconds };
        slot->desiredWrite.store(w + 1, std::memory_order_release);
    }

    void requestRelease(const TrackId trackId, const int parameterId, const double hostSeconds) noexcept
    {
        if (Slot* slot = findCapturing(trackId, parameterId))
        {
            slot->releaseHostSeconds = hostSeconds;
            slot->releaseRequested.store(true, std::memory_order_release);
        }
    }

    /// Copy applied points older than `horizonSeconds`, keeping the newest of
    /// those so the live lerp still has a left endpoint. Returns how many were
    /// copied. The caller owns them; the ring slot is then free.
    int drainBefore(const TrackId trackId,
                    const int parameterId,
                    const double horizonSeconds,
                    TrackAutomationPoint* const out,
                    const int maxOut) noexcept
    {
        Slot* slot = findCapturing(trackId, parameterId);
        if (slot == nullptr || out == nullptr || maxOut <= 0
            || slot->closeWrite.load(std::memory_order_acquire) != slot->closeRead.load(std::memory_order_relaxed))
        {
            return 0;
        }
        const uint32_t w = slot->appliedWrite.load(std::memory_order_acquire);
        uint32_t r = slot->appliedRead.load(std::memory_order_relaxed);
        uint32_t keep = r;
        uint32_t lastBefore = r;
        bool any = false;
        for (uint32_t i = r; i != w; ++i)
        {
            const Applied& ev = slot->applied[i % kTrackAutomationRingCapacity];
            if (ev.timeSeconds >= horizonSeconds)
            {
                break;
            }
            lastBefore = i;
            any = true;
        }
        if (!any)
        {
            return 0;
        }
        keep = lastBefore;
        int copied = 0;
        for (uint32_t i = r; i != keep && copied < maxOut; ++i)
        {
            const Applied& ev = slot->applied[i % kTrackAutomationRingCapacity];
            out[copied++] = { ev.timeSeconds, ev.value };
        }
#if defined(MINIDAW_AUTOMATION_HANDOVER_PROBE)
        if (handoverProbe_ != nullptr && handoverProbe_->duringDrain != nullptr)
        {
            handoverProbe_->duringDrain(handoverProbe_->context);
        }
#endif
        slot->appliedRead.store(r + static_cast<uint32_t>(copied), std::memory_order_release);
        return copied;
    }

    [[nodiscard]] bool adoptionHeld(const TrackId trackId) const noexcept
    {
        for (const Slot& slot : slots_)
        {
            if (slot.trackId.load(std::memory_order_relaxed) == trackId && slot.adoptionHeld.load(std::memory_order_acquire))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool consumeOverflow(TrackId& trackId, int& parameterId) noexcept
    {
        if (!overflowPending_.exchange(false, std::memory_order_acq_rel))
        {
            return false;
        }
        trackId = overflowTrack_.load(std::memory_order_relaxed);
        parameterId = overflowParam_.load(std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool takeCommit(TrackId& trackId, int& parameterId, TrackAutomationCommit& out) noexcept
    {
        for (Slot& slot : slots_)
        {
            const uint32_t published = slot.closeWrite.load(std::memory_order_acquire);
            const uint32_t taken = slot.closeRead.load(std::memory_order_relaxed);
            if (taken == published)
            {
                continue;
            }
            const CloseMark mark = slot.closes[taken % 2u];
            trackId = slot.trackId.load(std::memory_order_relaxed);
            parameterId = slot.parameterId.load(std::memory_order_relaxed);
            out = {};
            out.valid = true;
            out.writeEnabled = mark.writeEnabled;
            out.readEnabled = mark.readEnabled;
            out.restartStroke = mark.restartStroke;
            out.incomplete = mark.incomplete;
            out.gestureId = mark.gestureId;
            out.revision = mark.localRevision;
            out.touchStartSeconds = mark.touchStartSeconds;
            out.releaseSeconds = mark.releaseSeconds;
            out.rampEndSeconds = mark.rampEndSeconds;
            out.releaseValue = mark.releaseValue;
            out.rampEndValue = mark.rampEndValue;
            out.preTouchValue = mark.preTouchValue;
            const uint32_t end = mark.endWrite;
            uint32_t r = slot.appliedRead.load(std::memory_order_relaxed);
            while (r != end && out.eventCount < kTrackAutomationRingCapacity)
            {
                const Applied& ev = slot.applied[r % kTrackAutomationRingCapacity];
                out.events[static_cast<size_t>(out.eventCount++)] = { ev.timeSeconds, ev.value };
                ++r;
            }
            if (r != end)
            {
                out.incomplete = true;
            }
            slot.appliedRead.store(r, std::memory_order_release);
            slot.waitingLocal.store(mark.localRevision, std::memory_order_release);
            slot.closeRead.store(taken + 1u, std::memory_order_release);
            return true;
        }
        return false;
    }

    /// The edit published `snapshotRevision` into the snapshot. The audio thread
    /// clears the hold only after it loads that revision and the ramp has ended.
    void noteRequiredRevision(const TrackId trackId,
                              const int parameterId,
                              const uint32_t localRevision,
                              const uint32_t snapshotRevision) noexcept
    {
        if (Slot* slot = findWaiting(trackId, parameterId, localRevision))
        {
            slot->requiredSnapshot.store(snapshotRevision, std::memory_order_release);
        }
    }

    void noteRejected(const TrackId trackId, const int parameterId, const uint32_t localRevision) noexcept
    {
        if (Slot* slot = findWaiting(trackId, parameterId, localRevision))
        {
            slot->rejected.store(true, std::memory_order_release);
        }
    }

    /// No callback is running. Close an open stroke, drop holds, and keep any
    /// commit the message thread has not taken yet.
    void messageThread_deviceStopped() noexcept
    {
        for (Slot& slot : slots_)
        {
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase == 1)
            {
                const double at = slot.touchStartSeconds >= 0.0 ? slot.touchStartSeconds : 0.0;
                pushApplied(slot, at, slot.holdValue.load(std::memory_order_relaxed));
                closeStroke(slot, at, false, false);
            }
            slot.adoptionHeld.store(false, std::memory_order_release);
            if (slot.closeWrite.load(std::memory_order_acquire) != slot.closeRead.load(std::memory_order_acquire))
            {
                slot.phase.store(kPhaseHold, std::memory_order_release);
            }
            else if (slot.phase.load(std::memory_order_acquire) != kPhaseFree)
            {
                clearSlot(slot);
            }
        }
    }

    /// Undo/redo replaced the snapshot. Slots waiting on a publish follow the
    /// restored curve instead of keeping read-ahead locked to the old stroke.
    void messageThread_snapshotReplaced() noexcept
    {
        for (Slot& slot : slots_)
        {
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase == kPhaseRamp || phase == kPhaseHold)
            {
                clearSlot(slot);
            }
        }
    }

    void messageThread_reset() noexcept
    {
        for (Slot& slot : slots_)
        {
            const uint32_t published = slot.closeWrite.load(std::memory_order_relaxed);
            slot.closeRead.store(published, std::memory_order_relaxed);
            clearSlot(slot);
        }
        overflowPending_.store(false, std::memory_order_release);
    }

    /// [Audio thread] Map newly arrived UI values across this block by host time.
    /// `discontinuity` ends the stroke at `previousBlockEndSeconds` with no ramp
    /// across the jump. A pointer that is still down starts another stroke.
    void audioThread_beginBlock(const double blockStartSeconds,
                                const double blockDurationSeconds,
                                const double blockHostSeconds,
                                const bool discontinuity,
                                const double previousBlockEndSeconds) noexcept
    {
        double previousHost = previousHostSeconds_;
        if (!(previousHost >= 0.0))
        {
            previousHost = blockHostSeconds - blockDurationSeconds;
        }
        for (Slot& slot : slots_)
        {
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase != kPhaseCapture && phase != kPhaseRamp && phase != kPhaseHold)
            {
                continue;
            }
            if (phase == kPhaseCapture)
            {
                if (discontinuity && previousBlockEndSeconds >= 0.0)
                {
                    const bool released = slot.releaseRequested.load(std::memory_order_acquire);
                    pushApplied(slot, previousBlockEndSeconds, slot.holdValue.load(std::memory_order_relaxed));
                    closeStroke(slot, previousBlockEndSeconds, false, !released);
                    continue;
                }
                stampDesired(slot, blockStartSeconds, blockDurationSeconds, blockHostSeconds, previousHost);
                if (slot.releaseRequested.load(std::memory_order_acquire)
                    && slot.desiredRead.load(std::memory_order_acquire)
                           == slot.desiredWrite.load(std::memory_order_acquire))
                {
                    const double releaseTime = mapHost(slot.releaseHostSeconds,
                                                       blockStartSeconds,
                                                       blockDurationSeconds,
                                                       blockHostSeconds,
                                                       previousHost);
                    if (slot.releaseHostSeconds <= blockHostSeconds + 1.0e-4)
                    {
                        const double at = releaseTime >= slot.touchStartSeconds ? releaseTime : slot.touchStartSeconds;
                        const uint32_t w = slot.appliedWrite.load(std::memory_order_relaxed);
                        const bool empty = w == slot.appliedRead.load(std::memory_order_acquire);
                        const Applied last = empty ? Applied{} : slot.applied[(w - 1) % kTrackAutomationRingCapacity];
                        if (empty || at > last.timeSeconds + kTrackAutomationTimeMergeSeconds
                            || std::fabs(last.value - slot.holdValue.load(std::memory_order_relaxed)) > kTrackAutomationValueTolerance)
                        {
                            pushApplied(slot, at, slot.holdValue.load(std::memory_order_relaxed));
                        }
                        closeStroke(slot, at, true, false);
                    }
                }
            }
        }
        previousHostSeconds_ = blockHostSeconds;
        drainHorizon_.store(blockStartSeconds, std::memory_order_release);
    }

    /// [Audio thread] The snapshot just loaded. `present == false` means the track
    /// is gone. A hold clears only when this revision is the one the edit published,
    /// or the edit was rejected / had nothing to publish, and the ramp has ended.
    void audioThread_observe(const TrackId trackId,
                             const bool present,
                             const uint32_t snapshotRevision,
                             const double blockStartSeconds) noexcept
    {
        for (Slot& slot : slots_)
        {
            if (slot.trackId.load(std::memory_order_relaxed) != trackId)
            {
                continue;
            }
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase != kPhaseRamp && phase != kPhaseHold)
            {
                if (!present && phase != kPhaseCapture && phase != kPhaseFree && phase != kPhaseArming)
                {
                    clearSlot(slot);
                }
                continue;
            }
            if (!present)
            {
                clearSlot(slot);
                continue;
            }
            const bool rampDone = blockStartSeconds >= slot.rampEndSeconds.load(std::memory_order_relaxed) - kTrackAutomationTimeMergeSeconds;
            if (!rampDone)
            {
                continue;
            }
            const bool rejected = slot.rejected.load(std::memory_order_acquire);
            const uint32_t required = slot.requiredSnapshot.load(std::memory_order_acquire);
            const bool published = slot.publishRequired.load(std::memory_order_acquire) && required != 0 && snapshotRevision == required;
            if (rejected || published || !slot.writeEnabled.load(std::memory_order_relaxed))
            {
                clearSlot(slot);
            }
        }
    }

    void audioThread_fillClock(ChannelAutomationClock& clock, const double timelineRate) const noexcept
    {
        clock.engineRate = timelineRate;
        clock.laneCount = 0;
        for (const Slot& slot : slots_)
        {
            const int phase = slot.phase.load(std::memory_order_acquire);
            if (phase != kPhaseCapture && phase != kPhaseRamp && phase != kPhaseHold)
            {
                continue;
            }
            if (clock.laneCount >= kTrackAutomationSlots)
            {
                continue;
            }
            ChannelAutomationGestureLane laneCopy;
            laneCopy.trackId = slot.trackId.load(std::memory_order_relaxed);
            laneCopy.parameterId = slot.parameterId.load(std::memory_order_relaxed);
            laneCopy.holding = true;
            laneCopy.holdValue = slot.holdValue.load(std::memory_order_relaxed);
            laneCopy.ramping = phase == kPhaseRamp;
            laneCopy.rampStartSeconds = slot.releaseSeconds.load(std::memory_order_relaxed);
            laneCopy.rampEndSeconds = slot.rampEndSeconds.load(std::memory_order_relaxed);
            laneCopy.rampStartValue = slot.releaseValue.load(std::memory_order_relaxed);
            laneCopy.rampEndValue = slot.rampEndValue.load(std::memory_order_relaxed);
            if (slot.phase.load(std::memory_order_acquire) != phase)
            {
                continue;
            }
            ChannelAutomationGestureLane& lane = clock.lanes[static_cast<size_t>(clock.laneCount++)];
            lane = {};
            lane.trackId = laneCopy.trackId;
            lane.parameterId = laneCopy.parameterId;
            lane.holding = true;
            lane.holdValue = laneCopy.holdValue;
            lane.ramping = laneCopy.ramping;
            lane.rampStartSeconds = laneCopy.rampStartSeconds;
            lane.rampEndSeconds = laneCopy.rampEndSeconds;
            lane.rampStartValue = laneCopy.rampStartValue;
            lane.rampEndValue = laneCopy.rampEndValue;
            const uint32_t w = slot.appliedWrite.load(std::memory_order_acquire);
            const uint32_t r = slot.appliedRead.load(std::memory_order_acquire);
            const uint32_t available = w >= r ? w - r : 0u;
            const int n = static_cast<int>(std::min<uint32_t>(available, static_cast<uint32_t>(kTrackAutomationLaneEvents)));
            const uint32_t start = w - static_cast<uint32_t>(n);
            for (int i = 0; i < n; ++i)
            {
                const Applied& ev = slot.applied[(start + static_cast<uint32_t>(i)) % kTrackAutomationRingCapacity];
                lane.timeSeconds[static_cast<size_t>(i)] = ev.timeSeconds;
                lane.value[static_cast<size_t>(i)] = ev.value;
            }
            lane.count = n;
            if (slot.phase.load(std::memory_order_acquire) != phase)
            {
                --clock.laneCount;
            }
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
                if (out[i] == slot.trackId.load(std::memory_order_relaxed))
                {
                    seen = true;
                }
            }
            if (!seen)
            {
                out[n++] = slot.trackId.load(std::memory_order_relaxed);
            }
        }
        return n;
    }

    [[nodiscard]] int audioThread_activeTracks(TrackId* const out, const int capacity) const noexcept
    {
        int n = 0;
        for (const Slot& slot : slots_)
        {
            const int activePhase = slot.phase.load(std::memory_order_acquire);
            if ((activePhase != kPhaseCapture && activePhase != kPhaseRamp && activePhase != kPhaseHold) || n >= capacity)
            {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < n; ++i)
            {
                if (out[i] == slot.trackId.load(std::memory_order_relaxed))
                {
                    seen = true;
                }
            }
            if (!seen)
            {
                out[n++] = slot.trackId.load(std::memory_order_relaxed);
            }
        }
        return n;
    }

    [[nodiscard]] double drainHorizon() const noexcept
    {
        return drainHorizon_.load(std::memory_order_acquire);
    }

#if defined(MINIDAW_AUTOMATION_HANDOVER_PROBE)
    void setHandoverProbeForTest(TrackAutomationHandoverProbe* const probe) noexcept
    {
        handoverProbe_ = probe;
    }
#endif

private:
    struct Desired
    {
        float value = 0.0f;
        double hostSeconds = 0.0;
    };

    struct Applied
    {
        double timeSeconds = 0.0;
        float value = 0.0f;
    };

    struct CloseMark
    {
        bool writeEnabled = false;
        bool readEnabled = false;
        bool restartStroke = false;
        bool incomplete = false;
        uint32_t gestureId = 0;
        uint32_t localRevision = 0;
        uint32_t endWrite = 0;
        double touchStartSeconds = 0.0;
        double releaseSeconds = 0.0;
        double rampEndSeconds = 0.0;
        float releaseValue = 0.0f;
        float rampEndValue = 0.0f;
        float preTouchValue = 0.0f;
    };

    static constexpr int kPhaseFree = 0;
    static constexpr int kPhaseCapture = 1;
    static constexpr int kPhaseRamp = 2;
    static constexpr int kPhaseHold = 3;
    static constexpr int kPhaseRetiring = 4;
    static constexpr int kPhaseArming = 5;

    struct Slot
    {
        std::atomic<TrackId> trackId{ kInvalidTrackId };
        std::atomic<int> parameterId{ 0 };
        std::atomic<uint32_t> gestureId{ 0 };
        std::atomic<bool> writeEnabled{ false };
        std::atomic<bool> readEnabled{ false };
        std::atomic<bool> incomplete{ false };
        std::atomic<bool> publishRequired{ false };
        std::atomic<bool> retireRequested{ false };
        TrackAutomationCurve frozen{};
        float frozenManual = 0.0f;
        std::atomic<float> holdValue{ 0.0f };
        double touchStartSeconds = -1.0;
        double releaseHostSeconds = 0.0;
        std::atomic<double> releaseSeconds{ 0.0 };
        std::atomic<double> rampEndSeconds{ 0.0 };
        std::atomic<float> releaseValue{ 0.0f };
        std::atomic<float> rampEndValue{ 0.0f };
        uint32_t localRevision = 0;
        std::atomic<uint32_t> waitingLocal{ 0 };
        std::atomic<uint32_t> closeWrite{ 0 };
        std::atomic<uint32_t> closeRead{ 0 };
        std::array<CloseMark, 2> closes{};
        std::atomic<int> phase{ 0 };
        std::atomic<bool> adoptionHeld{ false };
        std::atomic<bool> releaseRequested{ false };
        std::atomic<bool> rejected{ false };
        std::atomic<uint32_t> requiredSnapshot{ 0 };
        std::atomic<uint32_t> desiredWrite{ 0 };
        std::atomic<uint32_t> desiredRead{ 0 };
        std::atomic<uint32_t> appliedWrite{ 0 };
        std::atomic<uint32_t> appliedRead{ 0 };
        std::array<Desired, kTrackAutomationRingCapacity> desired{};
        std::array<Applied, kTrackAutomationRingCapacity> applied{};
    };

    Slot* findCapturing(const TrackId trackId, const int parameterId) noexcept
    {
        for (Slot& slot : slots_)
        {
            if (slot.trackId.load(std::memory_order_relaxed) == trackId
                && slot.parameterId.load(std::memory_order_relaxed) == parameterId
                && slot.phase.load(std::memory_order_acquire) == kPhaseCapture)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    const Slot* findCapturingConst(const TrackId trackId, const int parameterId) const noexcept
    {
        for (const Slot& slot : slots_)
        {
            if (slot.trackId.load(std::memory_order_relaxed) == trackId
                && slot.parameterId.load(std::memory_order_relaxed) == parameterId
                && slot.phase.load(std::memory_order_acquire) == kPhaseCapture)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    Slot* claimFree() noexcept
    {
        for (Slot& slot : slots_)
        {
            int expected = kPhaseFree;
            if (slot.phase.compare_exchange_strong(expected, kPhaseArming, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                return &slot;
            }
        }
        return nullptr;
    }

    Slot* findWaiting(const TrackId trackId, const int parameterId, const uint32_t localRevision) noexcept
    {
        for (Slot& slot : slots_)
        {
            if (slot.trackId.load(std::memory_order_relaxed) == trackId
                && slot.parameterId.load(std::memory_order_relaxed) == parameterId
                && slot.waitingLocal.load(std::memory_order_acquire) == localRevision
                && localRevision != 0)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    void markOverflow(Slot& slot) noexcept
    {
        slot.incomplete.store(true, std::memory_order_relaxed);
        overflowTrack_.store(slot.trackId.load(std::memory_order_relaxed), std::memory_order_relaxed);
        overflowParam_.store(slot.parameterId.load(std::memory_order_relaxed), std::memory_order_relaxed);
        overflowPending_.store(true, std::memory_order_release);
    }

    void pushApplied(Slot& slot, const double timeSeconds, const float value) noexcept
    {
        const uint32_t w = slot.appliedWrite.load(std::memory_order_relaxed);
        const uint32_t r = slot.appliedRead.load(std::memory_order_acquire);
        if (w - r >= static_cast<uint32_t>(capacity_.load(std::memory_order_relaxed)))
        {
            markOverflow(slot);
            return;
        }
        slot.applied[w % kTrackAutomationRingCapacity] = { timeSeconds, value };
        slot.appliedWrite.store(w + 1, std::memory_order_release);
        slot.holdValue.store(value, std::memory_order_relaxed);
        if (slot.touchStartSeconds < 0.0)
        {
            slot.touchStartSeconds = timeSeconds;
        }
    }

    static double mapHost(const double hostSeconds,
                          const double blockStartSeconds,
                          const double blockDurationSeconds,
                          const double blockHostSeconds,
                          const double previousHostSeconds) noexcept
    {
        const double span = blockHostSeconds - previousHostSeconds;
        double rel = 0.0;
        if (span > 1.0e-6)
        {
            rel = (hostSeconds - previousHostSeconds) / span;
        }
        if (rel < 0.0)
        {
            rel = 0.0;
        }
        else if (rel > 1.0)
        {
            rel = 1.0;
        }
        return blockStartSeconds + rel * blockDurationSeconds;
    }

    void stampDesired(Slot& slot,
                      const double blockStartSeconds,
                      const double blockDurationSeconds,
                      const double blockHostSeconds,
                      const double previousHostSeconds) noexcept
    {
        const uint32_t w = slot.desiredWrite.load(std::memory_order_acquire);
        uint32_t r = slot.desiredRead.load(std::memory_order_relaxed);
        while (r != w)
        {
            const Desired& ev = slot.desired[r % kTrackAutomationRingCapacity];
            if (ev.hostSeconds > blockHostSeconds + 1.0e-4)
            {
                break;
            }
            const double time = mapHost(ev.hostSeconds,
                                        blockStartSeconds,
                                        blockDurationSeconds,
                                        blockHostSeconds,
                                        previousHostSeconds);
            pushApplied(slot, time, ev.value);
            ++r;
        }
        slot.desiredRead.store(r, std::memory_order_release);
    }

    void closeStroke(Slot& slot, const double releaseTime, const bool allowRamp, const bool restart) noexcept
    {
        const uint32_t write = slot.closeWrite.load(std::memory_order_relaxed);
        const uint32_t read = slot.closeRead.load(std::memory_order_acquire);
        if (write - read >= 2u)
        {
            markOverflow(slot);
            slot.phase.store(kPhaseHold, std::memory_order_release);
            slot.releaseRequested.store(false, std::memory_order_release);
            return;
        }
        const float held = slot.holdValue.load(std::memory_order_relaxed);
        const bool readEnabled = slot.readEnabled.load(std::memory_order_relaxed);
        double rampEnd = releaseTime;
        float rampEndValue = held;
        if (allowRamp && readEnabled)
        {
            rampEnd = releaseTime + kTrackAutomationReturnRampSeconds;
            rampEndValue = trackAutomationEvaluate(slot.frozen, rampEnd, slot.frozenManual);
        }
        slot.releaseSeconds.store(releaseTime, std::memory_order_relaxed);
        slot.releaseValue.store(held, std::memory_order_relaxed);
        slot.rampEndSeconds.store(rampEnd, std::memory_order_relaxed);
        slot.rampEndValue.store(rampEndValue, std::memory_order_relaxed);
        slot.localRevision += 1;
        if (slot.localRevision == 0)
        {
            slot.localRevision = 1;
        }
        CloseMark mark;
        mark.writeEnabled = slot.writeEnabled.load(std::memory_order_relaxed);
        mark.readEnabled = readEnabled;
        mark.restartStroke = restart;
        mark.incomplete = slot.incomplete.load(std::memory_order_relaxed);
        mark.gestureId = slot.gestureId.load(std::memory_order_relaxed);
        mark.localRevision = slot.localRevision;
        mark.endWrite = slot.appliedWrite.load(std::memory_order_relaxed);
        mark.touchStartSeconds = slot.touchStartSeconds >= 0.0 ? slot.touchStartSeconds : releaseTime;
        mark.releaseSeconds = releaseTime;
        mark.rampEndSeconds = rampEnd;
        mark.releaseValue = held;
        mark.rampEndValue = rampEndValue;
        mark.preTouchValue = slot.frozenManual;
        slot.closes[write % 2u] = mark;
#if defined(MINIDAW_AUTOMATION_HANDOVER_PROBE)
        if (handoverProbe_ != nullptr && handoverProbe_->beforeClosePublish != nullptr)
        {
            handoverProbe_->beforeClosePublish(handoverProbe_->context);
        }
#endif
        slot.closeWrite.store(write + 1u, std::memory_order_release);
        slot.releaseRequested.store(false, std::memory_order_release);
        if (restart)
        {
            slot.touchStartSeconds = -1.0;
            slot.phase.store(kPhaseCapture, std::memory_order_release);
            return;
        }
        slot.publishRequired.store(mark.writeEnabled, std::memory_order_relaxed);
        slot.phase.store(allowRamp && readEnabled ? kPhaseRamp : kPhaseHold, std::memory_order_release);
    }

    /// Exclusive wipe. Refuses a slot another thread is arming, and refuses to
    /// drop a close the message thread has not taken. phase 0 is published last.
    void clearSlot(Slot& slot) noexcept
    {
        const int phase = slot.phase.load(std::memory_order_acquire);
        if (phase == kPhaseFree || phase == kPhaseArming || phase == kPhaseRetiring)
        {
            return;
        }
        if (slot.closeWrite.load(std::memory_order_acquire) != slot.closeRead.load(std::memory_order_acquire))
        {
            return;
        }
        int expected = phase;
        if (!slot.phase.compare_exchange_strong(expected, kPhaseRetiring, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return;
        }
        slot.adoptionHeld.store(false, std::memory_order_relaxed);
        slot.trackId.store(kInvalidTrackId, std::memory_order_relaxed);
        slot.gestureId.store(0, std::memory_order_relaxed);
        slot.waitingLocal.store(0, std::memory_order_relaxed);
        slot.publishRequired.store(false, std::memory_order_relaxed);
        slot.requiredSnapshot.store(0, std::memory_order_relaxed);
        slot.rejected.store(false, std::memory_order_relaxed);
        slot.incomplete.store(false, std::memory_order_relaxed);
#if defined(MINIDAW_AUTOMATION_HANDOVER_PROBE)
        if (handoverProbe_ != nullptr && handoverProbe_->duringRetire != nullptr)
        {
            handoverProbe_->duringRetire(handoverProbe_->context);
        }
#endif
        slot.phase.store(kPhaseFree, std::memory_order_release);
    }

#if defined(MINIDAW_AUTOMATION_HANDOVER_PROBE)
    TrackAutomationHandoverProbe* handoverProbe_ = nullptr;
#endif
    std::array<Slot, kTrackAutomationSlots> slots_{};
    std::atomic<int> capacity_{ kTrackAutomationRingCapacity };
    std::atomic<double> drainHorizon_{ 0.0 };
    std::atomic<bool> overflowPending_{ false };
    std::atomic<TrackId> overflowTrack_{ kInvalidTrackId };
    std::atomic<int> overflowParam_{ 0 };
    double previousHostSeconds_ = -1.0;
};
