#pragma once

// =============================================================================
// AudioWaveformCache  —  in-memory min/max waveform pyramid per decoded `AudioClip` (UI thread)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Long session clips need **many** PCM samples drawn into **few** timeline pixels. Building a
//   fresh fixed-column peak vector in `paint` (or scanning raw audio per frame) scales poorly and
//   produces chunky zoom artefacts. This cache holds an immutable **pyramid** of pre-aggregated
//   min/max peaks over the **full** decoded buffer for each `AudioClip` instance (one pyramid per
//   material object — multiple `PlacedClip`s can share it).
//
// THREADING
//   [Message thread] for `getOrEnqueue` (called from `ClipWaveformView::paint`). A single JUCE
//   `ThreadPool` worker generates pyramids **off** the message thread; results publish as a new
//   `const WaveformPyramid` via `std::shared_ptr` (read-only after publish). **Audio thread never
//   touches this type.**
//
// PERSISTENCE (DEFERRED)
//   API is intentionally **material-keyed** (`const AudioClip*`) so a later `.peaks` sidecar could
//   map `getSourceFilePath()` → bytes on disk. This slice is **RAM only**: no sidecar I/O,
//   no `ProjectFile`, no invalidation beyond process lifetime.
//
// See: `AudioWaveformCache.cpp` (build + LOD query), `ClipWaveformView` (viewport-driven draw).
// =============================================================================

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <juce_core/juce_core.h>

class AudioClip;

using AudioWaveformCachePyramidReady = std::function<void(const AudioClip*)>;

// Immutable peak pyramid: level 0 = finest bins of width `baseSamplesPerBin`; each coarser level
// merges pairs of bins (min of mins, max of maxes) until one bin covers the tail.
class WaveformPyramid
{
public:
    // [Any thread] Half-open file indices [s0, s1) into the **decoded** `AudioClip` buffer. Clamps
    // to `[0, numSamples)` and returns a vertical span suitable for symmetric waveform fill.
    void queryMinMaxForFileRange(
        std::int64_t s0,
        std::int64_t s1Excl,
        float& outMinSample,
        float& outMaxSample) const noexcept;

    [[nodiscard]] int getNumSourceSamples() const noexcept { return numSamples_; }

    [[nodiscard]] int getBaseSamplesPerBin() const noexcept { return baseSamplesPerBin_; }

    [[nodiscard]] int getNumLevels() const noexcept { return (int)mins_.size(); }

private:
    friend class AudioWaveformCache;

    int numSamples_ = 0;
    int baseSamplesPerBin_ = 256;
    // `mins_[level][bin]`, `maxs_[level][bin]` cover disjoint half-open file ranges whose union is
    // [0, numSamples_) at level 0; coarser levels merge adjacent pairs.
    std::vector<std::vector<float>> mins_;
    std::vector<std::vector<float>> maxs_;

    [[nodiscard]] std::int64_t binWidthSamplesForLevel(const int level) const noexcept;
    [[nodiscard]] int numBinsOnLevel(const int level) const noexcept;
    [[nodiscard]] int pickLevelForSpanSamples(const std::int64_t spanSamples) const noexcept;
};

// ---------------------------------------------------------------------------
// AudioWaveformCache — schedules pyramid builds; cheap hot path on UI repaint
// ---------------------------------------------------------------------------
class AudioWaveformCache
{
public:
    AudioWaveformCache();

    ~AudioWaveformCache();

    AudioWaveformCache(const AudioWaveformCache&) = delete;
    AudioWaveformCache& operator=(const AudioWaveformCache&) = delete;

    // [Message thread] Returns the pyramid if ready, else `nullptr` and schedules a background build
    // (once per `AudioClip*` key until success). Callers should repaint periodically until non-null.
    // `material` keeps PCM alive while a worker may still be reading it.
    [[nodiscard]] std::shared_ptr<const WaveformPyramid> getOrEnqueue(
        const std::shared_ptr<const AudioClip>& material);

    // [Message thread] True if a pyramid is already available (does not schedule a build).
    [[nodiscard]] bool isPyramidReady(const AudioClip* material) const noexcept;

    // [Message thread] Optional: invoked asynchronously after a pyramid is published (worker thread
    // schedules this on the message thread). Used to repaint waveform lanes without a playhead timer.
    void setOnPyramidReady(AudioWaveformCachePyramidReady fn) { onPyramidReady_ = std::move(fn); }

    // [Message thread] Stops workers; clears slots. Used on shutdown (also runs in destructor).
    void shutdown() noexcept;

    // Thread pool job only (see .cpp): PCM build runs off the message thread; publishes under `mutex_`.
    void completeBackgroundPyramidBuild(const std::shared_ptr<const AudioClip>& material,
                                        std::shared_ptr<const WaveformPyramid> built) noexcept;

    [[nodiscard]] static std::shared_ptr<const WaveformPyramid> buildPyramidForMaterialUnlocked(
        const std::shared_ptr<const AudioClip>& material);

private:
    struct Slot
    {
        std::shared_ptr<const WaveformPyramid> ready;
        bool jobScheduled = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<const AudioClip*, Slot> slots_;
    std::unique_ptr<juce::ThreadPool> pool_;
    AudioWaveformCachePyramidReady onPyramidReady_;
};
