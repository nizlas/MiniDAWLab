#pragma once

// =============================================================================
// TrackMeterBank — concurrent per-TrackId level accumulators for the mixer and the Inspector
// =============================================================================
//
// ROLE
//   The engine meters ONE selected row through `PlaybackEngine::meteredTrackId_` (Inspector,
//   diagnostics). A mixer needs every channel's output level at the same time. This bank holds a
//   fixed set of `level_meter::Accumulator` slots and a published, immutable SLOT MAP that tells
//   the audio thread which TrackId is folded into which slot. Rows that are not in the map cost
//   nothing on the audio thread beyond one id comparison per slot.
//
// WHAT IS MEASURED
//   Exactly the engine's existing post-channel-strip stage of each row (after pre-gain, inserts,
//   fader / mute and pan, before the output bus and sends) — the fold is called from the same
//   four places as the single-track meter, so the mixer, the Inspector and the diagnostics see the
//   same signal point. The Master row is NOT in the bank: its level is the Stereo Out accumulator
//   of the engine (device output), distributed under the Master row's TrackId by the UI hub.
//
// PUBLICATION AND SLOT LIFETIME
//   [Message thread] `setMeteredTracks(ids)` keeps the slot of every id that stays metered,
//   frees the slots of rows that left, assigns fresh slots to new ids (round-robin over the
//   free slots, reset at assignment) and publishes a new map with one release-store. Round-robin
//   assignment means a slot freed this instant is not handed to a new row before every other free
//   slot has been used — the audio thread may still fold the OLD row into it for the block that is
//   in flight under the previous map; that stale block never lands in a freshly assigned slot in
//   practice and would at worst colour one 33 ms UI window.
//   [Audio thread] `audioThread_beginBlock` acquire-loads the map once per callback; the fold
//   functions use that cached view for the whole block (no per-fold atomics besides the
//   accumulator's own relaxed ones). No allocation, locks, logging or UI on that path.
//
// LIMITS
//   `kMaxSlots` rows can be metered at once; `setMeteredTracks` meters the first `kMaxSlots` ids
//   and reports the overflow count so the UI can say so instead of silently showing nothing.
// =============================================================================

#include "domain/Track.h"
#include "engine/LevelMeterAccumulator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace level_meter
{

class TrackMeterBank
{
public:
    static constexpr int kMaxSlots = 256;

    /// Immutable published view: which TrackId each slot meters (`kInvalidTrackId` = free).
    struct SlotMap
    {
        std::array<std::int64_t, kMaxSlots> trackIdForSlot{};
        int highestUsedSlotPlusOne = 0;

        SlotMap() noexcept { trackIdForSlot.fill(static_cast<std::int64_t>(kInvalidTrackId)); }

        /// [Any thread] Slot of `trackId` or -1. Linear over the used prefix (tens of rows).
        [[nodiscard]] int slotFor(const TrackId trackId) const noexcept
        {
            const std::int64_t wanted = static_cast<std::int64_t>(trackId);
            for (int s = 0; s < highestUsedSlotPlusOne; ++s)
            {
                if (trackIdForSlot[static_cast<size_t>(s)] == wanted)
                {
                    return s;
                }
            }
            return -1;
        }
    };

    TrackMeterBank()
    {
        auto empty = std::make_shared<const SlotMap>();
        messageMap_ = empty;
        published_.store(empty, std::memory_order_release);
    }
    TrackMeterBank(const TrackMeterBank&) = delete;
    TrackMeterBank& operator=(const TrackMeterBank&) = delete;

    /// [Message thread] Make exactly `ids` the metered rows (order irrelevant, duplicates and
    /// `kInvalidTrackId` ignored). Rows already metered keep their slot and their pending window;
    /// rows that left are freed; new rows get a reset slot. Publishes once. Returns the number of
    /// ids that could not be metered because the bank is full.
    int setMeteredTracks(const std::vector<TrackId>& ids)
    {
        SlotMap next = *messageMap_;
        // 1. Free the slots of rows that are no longer wanted.
        for (int s = 0; s < kMaxSlots; ++s)
        {
            const std::int64_t id = next.trackIdForSlot[static_cast<size_t>(s)];
            if (id == static_cast<std::int64_t>(kInvalidTrackId))
            {
                continue;
            }
            bool stillWanted = false;
            for (const TrackId wanted : ids)
            {
                if (static_cast<std::int64_t>(wanted) == id)
                {
                    stillWanted = true;
                    break;
                }
            }
            if (!stillWanted)
            {
                next.trackIdForSlot[static_cast<size_t>(s)] = static_cast<std::int64_t>(kInvalidTrackId);
            }
        }
        // 2. Assign fresh slots to new rows (round-robin from the last assignment point).
        int overflow = 0;
        for (const TrackId wanted : ids)
        {
            if (wanted == kInvalidTrackId || next.slotFor(wanted) >= 0)
            {
                continue;
            }
            const int slot = findFreeSlotRoundRobin(next);
            if (slot < 0)
            {
                ++overflow;
                continue;
            }
            next.trackIdForSlot[static_cast<size_t>(slot)] = static_cast<std::int64_t>(wanted);
            slots_[static_cast<size_t>(slot)].reset();
            nextAssignmentSlot_ = (slot + 1) % kMaxSlots;
        }
        // 3. Recompute the used prefix so the audio thread's lookup stays short.
        next.highestUsedSlotPlusOne = 0;
        for (int s = kMaxSlots - 1; s >= 0; --s)
        {
            if (next.trackIdForSlot[static_cast<size_t>(s)] != static_cast<std::int64_t>(kInvalidTrackId))
            {
                next.highestUsedSlotPlusOne = s + 1;
                break;
            }
        }
        auto publishedMap = std::make_shared<const SlotMap>(next);
        messageMap_ = publishedMap;
        published_.store(publishedMap, std::memory_order_release);
        return overflow;
    }

    /// [Message thread] The rows currently metered (message-thread view of the map).
    [[nodiscard]] std::vector<TrackId> meteredTracks() const
    {
        std::vector<TrackId> out;
        for (int s = 0; s < messageMap_->highestUsedSlotPlusOne; ++s)
        {
            const std::int64_t id = messageMap_->trackIdForSlot[static_cast<size_t>(s)];
            if (id != static_cast<std::int64_t>(kInvalidTrackId))
            {
                out.push_back(static_cast<TrackId>(id));
            }
        }
        return out;
    }

    [[nodiscard]] bool isMetered(const TrackId trackId) const noexcept { return messageMap_->slotFor(trackId) >= 0; }

    /// [Message thread] Everything folded for `trackId` since the previous drain; an empty
    /// reading (no blocks) for rows that are not metered.
    [[nodiscard]] Reading drainAndReset(const TrackId trackId) noexcept
    {
        const int slot = messageMap_->slotFor(trackId);
        return slot >= 0 ? slots_[static_cast<size_t>(slot)].drainAndReset() : Reading{};
    }

    /// [Audio thread] Once per callback, before any fold: cache the published map for this block.
    void audioThread_beginBlock() noexcept
    {
        audioMap_ = published_.load(std::memory_order_acquire);
    }

    /// [Audio thread] True when `trackId` has a slot in this block's map (callers compute the
    /// block statistics once and share them with the single-track meter).
    [[nodiscard]] bool audioThread_isMetered(const TrackId trackId) const noexcept
    {
        return audioMap_ != nullptr && audioMap_->slotFor(trackId) >= 0;
    }

    /// [Audio thread] Fold pre-computed statistics into the row's slot (no-op when not metered).
    void audioThread_foldStats(const TrackId trackId, const BlockStats& stats) noexcept
    {
        if (audioMap_ == nullptr)
        {
            return;
        }
        const int slot = audioMap_->slotFor(trackId);
        if (slot >= 0)
        {
            slots_[static_cast<size_t>(slot)].audioThread_foldStats(stats);
        }
    }

private:
    [[nodiscard]] int findFreeSlotRoundRobin(const SlotMap& map) const noexcept
    {
        for (int i = 0; i < kMaxSlots; ++i)
        {
            const int s = (nextAssignmentSlot_ + i) % kMaxSlots;
            if (map.trackIdForSlot[static_cast<size_t>(s)] == static_cast<std::int64_t>(kInvalidTrackId))
            {
                return s;
            }
        }
        return -1;
    }

    std::array<Accumulator, kMaxSlots> slots_;
    /// Published for the audio thread (release-store on every change).
    std::atomic<std::shared_ptr<const SlotMap>> published_;
    /// [Message thread] Current assignment (the same object that was last published).
    std::shared_ptr<const SlotMap> messageMap_;
    /// [Audio thread only] Map cached by `audioThread_beginBlock` for the block in flight.
    std::shared_ptr<const SlotMap> audioMap_;
    int nextAssignmentSlot_ = 0;
};

} // namespace level_meter
