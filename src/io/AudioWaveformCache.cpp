// =============================================================================
// AudioWaveformCache.cpp  —  build + query rules for the PCM min/max pyramid
// =============================================================================
//
// BUILD
//   Level 0 walks the float `AudioClip` buffer in half-open segments [b*k, min(b*(k+1), N)). Each
//   segment stores **true** waveform min and max across all channels (not rectified). Coarser
//   levels pair-wise merge: `min = min(left.min, right.min)`, `max = max(left.max, right.max)`.
//   The odd tail at any level is promoted without a partner.
//
// QUERY (paint-driven)
//   For a half-open file range corresponding to **one screen pixel column** (or a merge of a few
//   neighbouring columns), we pick the **coarsest** pyramid level whose bin width is still small
//   enough that at most `kMaxBinsPerPixel` bins intersect the range, then merge those bins. That
//   keeps work O(1) per pixel while avoiding “stretched” mega-bins when zoomed in.
//
// THREADING
//   Workers build with `buildPyramidForMaterialUnlocked` (no mutex), then publish under `mutex_`.
//
// JUCE
//   `juce::ThreadPool` + `ThreadPoolJob` owns the background build; `getOrEnqueue` stays on the
//   message thread (matches `ClipWaveformView::paint`).
// =============================================================================

#include "io/AudioWaveformCache.h"

#include "domain/AudioClip.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    struct WaveformCacheBackgroundJob : public juce::ThreadPoolJob
    {
        WaveformCacheBackgroundJob(AudioWaveformCache& ownerIn, std::shared_ptr<const AudioClip> materialIn)
            : juce::ThreadPoolJob("AudioWaveformPyramid")
            , owner(ownerIn)
            , material(std::move(materialIn))
        {
        }

        JobStatus runJob() override
        {
            if (material == nullptr)
            {
                return jobHasFinished;
            }
            std::shared_ptr<const WaveformPyramid> built
                = AudioWaveformCache::buildPyramidForMaterialUnlocked(material);
            owner.completeBackgroundPyramidBuild(material, std::move(built));
            return jobHasFinished;
        }

        AudioWaveformCache& owner;
        std::shared_ptr<const AudioClip> material;
    };

    constexpr int kDefaultPoolThreads = 1;
    constexpr int kDefaultBaseSamplesPerBin = 256;
    // How many pyramid bins we allow to contribute to one pixel column before switching to a
    // coarser level — keeps zoomed-out work bounded without smearing a single bin across overly
    // many pixels when zoomed in.
    constexpr int kMaxBinsPerPixel = 8;

    [[nodiscard]] float finiteOrZero(const float x) noexcept
    {
        return (std::isfinite(x) ? x : 0.0f);
    }
} // namespace

std::int64_t WaveformPyramid::binWidthSamplesForLevel(const int level) const noexcept
{
    if (level < 0 || mins_.empty())
    {
        return (std::int64_t)baseSamplesPerBin_;
    }
    const int L = juce::jmin(level, juce::jmax(0, (int)mins_.size() - 1));
    return (std::int64_t)baseSamplesPerBin_ << L;
}

int WaveformPyramid::numBinsOnLevel(const int level) const noexcept
{
    if (level < 0 || level >= (int)mins_.size())
    {
        return 0;
    }
    return (int)mins_[(size_t)level].size();
}

int WaveformPyramid::pickLevelForSpanSamples(const std::int64_t spanSamples) const noexcept
{
    const int nl = getNumLevels();
    if (nl <= 0)
    {
        return 0;
    }
    const std::int64_t span = juce::jmax(std::int64_t{ 1 }, spanSamples);
    const std::int64_t minBinW
        = juce::jmax(std::int64_t{ 1 }, (span + (std::int64_t)kMaxBinsPerPixel - 1)
                                           / (std::int64_t)kMaxBinsPerPixel);
    int level = 0;
    while (level < nl - 1)
    {
        const std::int64_t wNext = (std::int64_t)baseSamplesPerBin_ << (level + 1);
        if (wNext <= minBinW)
        {
            ++level;
        }
        else
        {
            break;
        }
    }
    return juce::jlimit(0, nl - 1, level);
}

void WaveformPyramid::queryMinMaxForFileRange(
    const std::int64_t s0,
    const std::int64_t s1Excl,
    float& outMinSample,
    float& outMaxSample) const noexcept
{
    outMinSample = 0.0f;
    outMaxSample = 0.0f;
    if (mins_.empty() || numSamples_ <= 0 || s0 >= s1Excl)
    {
        return;
    }
    const std::int64_t a0 = juce::jlimit(std::int64_t{ 0 }, (std::int64_t)numSamples_, s0);
    const std::int64_t a1 = juce::jlimit(std::int64_t{ 0 }, (std::int64_t)numSamples_, s1Excl);
    if (a0 >= a1)
    {
        return;
    }
    const int level = pickLevelForSpanSamples(a1 - a0);
    const std::int64_t w = juce::jmax(std::int64_t{ 1 }, binWidthSamplesForLevel(level));
    const int nb = numBinsOnLevel(level);
    if (nb <= 0)
    {
        return;
    }
    const std::int64_t b0 = a0 / w;
    const std::int64_t b1 = (a1 + w - 1) / w; // first bin index **not** overlapping
    bool any = false;
    float accMn = 0.0f;
    float accMx = 0.0f;
    for (std::int64_t b = b0; b < b1; ++b)
    {
        if (b < 0 || b >= (std::int64_t)nb)
        {
            continue;
        }
        const int bi = (int)b;
        const float mn = mins_[(size_t)level][(size_t)bi];
        const float mx = maxs_[(size_t)level][(size_t)bi];
        if (!any)
        {
            accMn = mn;
            accMx = mx;
            any = true;
        }
        else
        {
            accMn = std::min(accMn, mn);
            accMx = std::max(accMx, mx);
        }
    }
    if (any)
    {
        outMinSample = finiteOrZero(accMn);
        outMaxSample = finiteOrZero(accMx);
    }
}

AudioWaveformCache::AudioWaveformCache()
    : pool_(std::make_unique<juce::ThreadPool>(kDefaultPoolThreads))
{
}

AudioWaveformCache::~AudioWaveformCache()
{
    shutdown();
}

std::shared_ptr<const WaveformPyramid> AudioWaveformCache::buildPyramidForMaterialUnlocked(
    const std::shared_ptr<const AudioClip>& material)
{
    if (material == nullptr)
    {
        return nullptr;
    }
    const juce::AudioBuffer<float>& audio = material->getAudio();
    const int nCh = audio.getNumChannels();
    const int nSamp = juce::jmin(material->getNumSamples(), audio.getNumSamples());
    if (nCh <= 0 || nSamp <= 0)
    {
        auto empty = std::make_shared<WaveformPyramid>();
        empty->numSamples_ = 0;
        empty->baseSamplesPerBin_ = kDefaultBaseSamplesPerBin;
        return empty;
    }

    auto built = std::make_shared<WaveformPyramid>();
    built->numSamples_ = nSamp;
    built->baseSamplesPerBin_ = kDefaultBaseSamplesPerBin;
    const int base = kDefaultBaseSamplesPerBin;
    const int bins0 = (nSamp + base - 1) / base;
    built->mins_.clear();
    built->maxs_.clear();
    built->mins_.resize(1);
    built->maxs_.resize(1);
    built->mins_[0].resize((size_t)bins0);
    built->maxs_[0].resize((size_t)bins0);

    for (int b = 0; b < bins0; ++b)
    {
        const int s0 = b * base;
        const int s1 = juce::jmin(nSamp, s0 + base);
        float mn = std::numeric_limits<float>::infinity();
        float mx = -std::numeric_limits<float>::infinity();
        for (int s = s0; s < s1; ++s)
        {
            for (int c = 0; c < nCh; ++c)
            {
                const float v = audio.getSample(c, s);
                if (!std::isfinite(v))
                {
                    continue;
                }
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
        }
        if (!std::isfinite(mn) || !std::isfinite(mx))
        {
            mn = 0.0f;
            mx = 0.0f;
        }
        built->mins_[0][(size_t)b] = mn;
        built->maxs_[0][(size_t)b] = mx;
    }

    int prevBins = bins0;
    while (prevBins > 1)
    {
        const int nextBins = (prevBins + 1) / 2;
        std::vector<float> nextMin((size_t)nextBins);
        std::vector<float> nextMax((size_t)nextBins);
        const size_t prevLevel = built->mins_.size() - 1;
        for (int i = 0; i < nextBins; ++i)
        {
            const int k = 2 * i;
            float mn = built->mins_[prevLevel][(size_t)k];
            float mx = built->maxs_[prevLevel][(size_t)k];
            if (k + 1 < prevBins)
            {
                mn = std::min(mn, built->mins_[prevLevel][(size_t)k + 1]);
                mx = std::max(mx, built->maxs_[prevLevel][(size_t)k + 1]);
            }
            nextMin[(size_t)i] = mn;
            nextMax[(size_t)i] = mx;
        }
        built->mins_.push_back(std::move(nextMin));
        built->maxs_.push_back(std::move(nextMax));
        prevBins = nextBins;
    }

    return built;
}

void AudioWaveformCache::shutdown() noexcept
{
    if (pool_ != nullptr)
    {
        pool_->removeAllJobs(true, 60000);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.clear();
}

void AudioWaveformCache::completeBackgroundPyramidBuild(
    const std::shared_ptr<const AudioClip>& material,
    std::shared_ptr<const WaveformPyramid> built) noexcept
{
    if (material == nullptr)
    {
        return;
    }
    AudioWaveformCachePyramidReady callbackCopy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = slots_.find(material.get());
        if (it != slots_.end())
        {
            if (built != nullptr)
            {
                it->second.ready = std::move(built);
            }
            it->second.jobScheduled = false;
        }
        callbackCopy = onPyramidReady_;
    }
    if (callbackCopy != nullptr && built != nullptr)
    {
        const AudioClip* key = material.get();
        juce::MessageManager::callAsync(
            [cb = std::move(callbackCopy), key, guard = asyncLifetime_.guard()] {
                if (!guard.isAlive())
                {
                    juce::Logger::writeToLog("[stale-async] skipped: waveform pyramid-ready notify");
                    return;
                }
                cb(key);
            });
    }
}

std::shared_ptr<const WaveformPyramid> AudioWaveformCache::getOrEnqueue(
    const std::shared_ptr<const AudioClip>& material)
{
    if (material == nullptr)
    {
        return nullptr;
    }
    const AudioClip* key = material.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& slot = slots_[key];
        if (slot.ready != nullptr)
        {
            return slot.ready;
        }
        if (!slot.jobScheduled)
        {
            slot.jobScheduled = true;
            (void)pool_->addJob(new WaveformCacheBackgroundJob(*this, material), true);
        }
    }
    return nullptr;
}

bool AudioWaveformCache::isPyramidReady(const AudioClip* material) const noexcept
{
    if (material == nullptr)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = slots_.find(material);
    if (it == slots_.end())
    {
        return false;
    }
    return it->second.ready != nullptr;
}
