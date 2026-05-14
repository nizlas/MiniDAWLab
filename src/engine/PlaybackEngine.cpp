// =============================================================================
// PlaybackEngine.cpp  —  drive the speakers from Session + Transport (one file to read for audio)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   JUCE's audio callback. We fill the device's float buffers from `PlacedClip` data via the
//   immutable `SessionSnapshot` and advance `Transport`'s playhead. No file decode, no UI.
//
// PHASE 3 (minimal multi-track): **Within each track**, Phase 2 still applies: overlapping clips
//   are ordered; the **smallest** index in that **lane** that covers a timeline instant wins for
//   *that* lane. **Across** tracks, the audible samples for each lane for the same time window are
//   **added** into the device buffer — a minimal sum (no mixer UI). Each track contributes after
//   multiplying by its `Track::channelFaderGain` (mixer channel volume at the fader point; not
//   clip/pre-gain — see `Track`). With inserts (Phase 8 / Slice B), that fader multiplier is applied
//   on the scratch between Pre and Post chains; the dry path still applies gain at merge.
//
// PHASE 8 / Slice B (per-track VST3 Pre/Post): when `audioThread_hasActivePluginForTrack` reports an
//   active stereo insert, clip audio copies into `PluginInsertHost`'s stereo scratch at unity, runs the
//   **Pre** chain, applies effective track fader/mute gain in-place, runs the **Post** chain, then sums
//   scratch into the device buffer at unity (same mono L+R blend rule as the dry path).
//   Mono device: (L+R)*0.5 into output 0. Stereo+: L→0, R→1; higher channels unchanged by inserts.
//
// I1 (experimental instrument): after clip/insert summing (and even when transport is not
//   Playing), each `ExperimentalInstrumentHost` keyed in `ExperimentalInstrumentPlaybackSnapshot`
//   may add its stereo instrument bus to the same outputs in **session track order**.
//
// WHERE THIS SITS
//   `Session` publish → acquire-load of `const SessionSnapshot` (refcount) here; `Transport` seek
//   apply, playhead read/advance. See ARCHITECTURE_PRINCIPLES (Phase 2/3, snapshot handoff).
//
// REALTIME
//   [Audio thread] only in the callback. No allocation on this path beyond the existing `shared_ptr`
//   acquire; all scratch decisions use stack and fixed loops over clip and track count.
// =============================================================================

#include "engine/PlaybackEngine.h"

#include "engine/CountInClickOutput.h"
#include "engine/RecorderService.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "diagnostics/ExperimentalPlaybackRoutingLog.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    [[nodiscard]] float peakAbsMono(const float* p, const int n) noexcept
    {
        if (p == nullptr || n <= 0)
        {
            return 0.0f;
        }
        float m = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            m = juce::jmax(m, std::fabs(p[i]));
        }
        return m;
    }

    [[nodiscard]] float peakAbsStereoDevice(float* const* oc, const int numCh, const int n) noexcept
    {
        if (oc == nullptr || numCh <= 0 || n <= 0)
        {
            return 0.0f;
        }
        float pk = peakAbsMono(oc[0], n);
        if (numCh >= 2 && oc[1] != nullptr)
        {
            pk = juce::jmax(pk, peakAbsMono(oc[1], n));
        }
        return pk;
    }

    [[nodiscard]] int findCoveringRowIndexInLane(
        const std::vector<PlacedClip>& lane, const std::int64_t t) noexcept
    {
        for (int i = 0; i < (int)lane.size(); ++i)
        {
            const PlacedClip& p = lane[(size_t)i];
            const std::int64_t s = p.getStartSample();
            const std::int64_t e = s + p.getEffectiveLengthSamples();
            if (t >= s && t < e)
            {
                return i;
            }
        }
        return -1;
    }

    [[nodiscard]] std::int64_t minBoundaryStrictlyAfterInLane(
        const std::vector<PlacedClip>& lane,
        const std::int64_t t,
        const std::int64_t timelineEnd) noexcept
    {
        std::int64_t m = std::numeric_limits<std::int64_t>::max();
        for (int i = 0; i < (int)lane.size(); ++i)
        {
            const PlacedClip& p = lane[(size_t)i];
            const std::int64_t s = p.getStartSample();
            const std::int64_t e = s + p.getEffectiveLengthSamples();
            if (s > t)
            {
                m = juce::jmin(m, s);
            }
            if (e > t)
            {
                m = juce::jmin(m, e);
            }
        }
        if (timelineEnd > t)
        {
            m = juce::jmin(m, timelineEnd);
        }
        if (m == std::numeric_limits<std::int64_t>::max())
        {
            m = timelineEnd;
        }
        return m;
    }

    void addClipRunToOutputs(const AudioClip& clip,
                             int offInMaterial,
                             int run,
                             int outFrame0,
                             int numOutChannels,
                             float* const* outputChannelData,
                             float trackGain) noexcept
    {
        const int numSourceChannels = clip.getNumChannels();
        const juce::AudioBuffer<float>& buf = clip.getAudio();

        for (int outChannel = 0; outChannel < numOutChannels; ++outChannel)
        {
            float* d = outputChannelData[outChannel];
            if (d == nullptr)
            {
                continue;
            }
            float* const dest = d + outFrame0;
            const bool duplicateMono = (numSourceChannels == 1 && numOutChannels >= 2
                                        && (outChannel == 0 || outChannel == 1));
            if (duplicateMono)
            {
                juce::FloatVectorOperations::addWithMultiply(
                    dest, buf.getReadPointer(0) + offInMaterial, trackGain, run);
            }
            else if (outChannel < numSourceChannels)
            {
                juce::FloatVectorOperations::addWithMultiply(
                    dest, buf.getReadPointer(outChannel) + offInMaterial, trackGain, run);
            }
        }
    }

    /// Pre-fader: copies material into stereo scratch [L,R]. Mono duplicates; stereo maps 0→L, 1→R.
    void copyClipRunToStereoScratch(const AudioClip& clip,
                                    int offInMaterial,
                                    int run,
                                    float* scratchL,
                                    float* scratchR) noexcept
    {
        const int numSourceChannels = clip.getNumChannels();
        const juce::AudioBuffer<float>& buf = clip.getAudio();
        const float* src0 = buf.getReadPointer(0) + offInMaterial;
        if (numSourceChannels == 1)
        {
            juce::FloatVectorOperations::copy(scratchL, src0, run);
            juce::FloatVectorOperations::copy(scratchR, src0, run);
        }
        else
        {
            juce::FloatVectorOperations::copy(scratchL, src0, run);
            if (numSourceChannels >= 2)
            {
                juce::FloatVectorOperations::copy(scratchR, buf.getReadPointer(1) + offInMaterial, run);
            }
            else
            {
                juce::FloatVectorOperations::clear(scratchR, run);
            }
        }
    }

    /// Mix scratch into device; `trackGain` is applied here (dry path: fader; insert path: use 1.0 after in-scratch gain).
    void addStereoScratchToDeviceOutputs(float* const* scratchPtrs,
                                        int run,
                                        int outFrame0,
                                        int numOutputChannels,
                                        float* const* outputChannelData,
                                        float trackGain) noexcept
    {
        if (scratchPtrs == nullptr || run <= 0)
        {
            return;
        }
        const float* L = scratchPtrs[0];
        const float* R = scratchPtrs[1];
        if (L == nullptr || R == nullptr)
        {
            return;
        }
        if (numOutputChannels <= 0)
        {
            return;
        }
        if (numOutputChannels == 1)
        {
            float* d = outputChannelData[0];
            if (d != nullptr)
            {
                float* const dest = d + outFrame0;
                const float halfGain = 0.5f * trackGain;
                juce::FloatVectorOperations::addWithMultiply(dest, L, halfGain, run);
                juce::FloatVectorOperations::addWithMultiply(dest, R, halfGain, run);
            }
        }
        else
        {
            if (float* d0 = outputChannelData[0])
            {
                juce::FloatVectorOperations::addWithMultiply(d0 + outFrame0, L, trackGain, run);
            }
            if (numOutputChannels >= 2)
            {
                if (float* d1 = outputChannelData[1])
                {
                    juce::FloatVectorOperations::addWithMultiply(d1 + outFrame0, R, trackGain, run);
                }
            }
        }
    }

    void scaleStereoScratch(float* const* scratchPtrs, int run, float gain) noexcept
    {
        if (scratchPtrs == nullptr || run <= 0)
        {
            return;
        }
        if (float* L = scratchPtrs[0])
        {
            juce::FloatVectorOperations::multiply(L, gain, run);
        }
        if (float* R = scratchPtrs[1])
        {
            juce::FloatVectorOperations::multiply(R, gain, run);
        }
    }

    /// [Audio thread] One experimental instrument (I1); additive after tracks / inserts / prior instruments.
    void mixExperimentalInstrumentAfterTracks(ExperimentalInstrumentHost* host,
                                                float* const* outputChannelData,
                                                int numOutputChannels,
                                                int numSamples,
                                                float instrumentGain) noexcept
    {
        if (host == nullptr || numSamples <= 0)
        {
            return;
        }
        host->audioThread_processBlockAndAddToOutputs(
            outputChannelData, numOutputChannels, numSamples, instrumentGain);
    }

    [[nodiscard]] const ExperimentalInstrumentPlaybackEntry* findExperimentalInstrumentPlaybackEntry(
        const ExperimentalInstrumentPlaybackSnapshot& snap,
        TrackId trackId) noexcept
    {
        for (const auto& e : snap.entries)
        {
            if (e.trackId != trackId)
                continue;
            if (e.host == nullptr || e.midiController == nullptr)
                continue;
            return &e;
        }
        return nullptr;
    }

    struct InstPlayEdgeDiagSlot
    {
        TrackId sessionTrackId = kInvalidTrackId;
        bool sessionOff = false;
        bool sessionMuted = false;
        bool registryYes = false;
        TrackId entryTrackId = kInvalidTrackId;
        std::uintptr_t hostAddr = 0;
        std::uintptr_t ctlAddr = 0;
        ExperimentalInstrumentHost* hostLive = nullptr;
        TrackId ctlDomain = kInvalidTrackId;
        bool renderSnap = false;
        bool renderPb = false;
        int plans = -1;
        bool scheduleCalled = false;
        int emittedFirstSeg = 0;
        bool firstSegDiagCaptured = false;
        bool mixInvoked = false;
        bool mixSkipped = false;
        const char* mixSkipReason = "";
        float mixDevicePeakBefore = 0.f;
        float mixDevicePeakAfter = 0.f;
        float mixAddedPeakApprox = 0.f;
        float sessionFaderGain = 1.f;
    };

    [[nodiscard]] int indexOfInstrumentPlayEdgeDiagSlot(const InstPlayEdgeDiagSlot* slots,
                                                        int slotCount,
                                                        TrackId tid) noexcept
    {
        for (int i = 0; i < slotCount; ++i)
        {
            if (slots[i].sessionTrackId == tid)
            {
                return i;
            }
        }
        return -1;
    }

    struct RoutingInstrumentPeekDiagRow final
    {
        TrackId tid = kInvalidTrackId;
        ExperimentalInstrumentHost* host = nullptr;
    };

    struct ExperimentalPlaybackRoutingPlayEdgePoster
    {
        const bool armed;
        InstPlayEdgeDiagSlot* const slots;
        const int slotCount;
        const std::int64_t playEdgeT0;
        const bool transportPlaying;

        ExperimentalPlaybackRoutingPlayEdgePoster(bool arm,
                                                  InstPlayEdgeDiagSlot* sl,
                                                  int n,
                                                  std::int64_t tPlayback,
                                                  bool transportPlayingIn) noexcept
            : armed(arm)
            , slots(sl)
            , slotCount(n)
            , playEdgeT0(tPlayback)
            , transportPlaying(transportPlayingIn)
        {
        }

        ~ExperimentalPlaybackRoutingPlayEdgePoster()
        {
            if (!armed || slots == nullptr || slotCount <= 0
                || juce::MessageManager::getInstanceWithoutCreating() == nullptr)
            {
                return;
            }

            juce::String line = "playback-edge: playT0=";
            line << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(playEdgeT0)));

            line << " instRows="
                 << juce::String(slotCount)
                 << " transportPlaying="
                 << juce::String(transportPlaying ? "yes" : "no");

            std::vector<juce::String> routingLines;
            std::vector<RoutingInstrumentPeekDiagRow> peekRows;
            peekRows.reserve((size_t)slotCount);

            for (int i = 0; i < slotCount; ++i)
            {
                const InstPlayEdgeDiagSlot& s = slots[i];
                std::uint64_t midiDiscarded = 0;
                std::int64_t blockCap = -1;
                if (s.hostLive != nullptr)
                {
                    midiDiscarded = s.hostLive->getTransportMidiAddEventDiscardedCountRelaxed();
                    blockCap = static_cast<std::int64_t>(s.hostLive->audioThread_peekTransportMidiBlockCap());
                }

                line << " [tid="
                     << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(s.sessionTrackId)));
                line << " off=" << juce::String(s.sessionOff ? "yes" : "no") << " muted="
                     << juce::String(s.sessionMuted ? "yes" : "no") << " reg="
                     << juce::String(s.registryYes ? "yes" : "no");

                line << " entryTid="
                     << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(s.entryTrackId)));

                line << " h="
                     << ((s.hostAddr != 0)
                             ? (juce::String("0x")
                                + juce::String::toHexString(
                                      static_cast<juce::int64>(static_cast<std::int64_t>(s.hostAddr))))
                             : juce::String("null"));

                line << " ctl="
                     << ((s.ctlAddr != 0)
                             ? (juce::String("0x")
                                + juce::String::toHexString(
                                      static_cast<juce::int64>(static_cast<std::int64_t>(s.ctlAddr))))
                             : juce::String("null"));

                line << " dom="
                     << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(s.ctlDomain)));

                line << " renderSnap=" << juce::String(s.renderSnap ? "yes" : "no");
                line << " renderPb=" << juce::String(s.renderPb ? "yes" : "no");
                line << " plans=" << juce::String(s.plans);
                line << " sched=" << juce::String(s.scheduleCalled ? "yes" : "no");
                line << " emitted=" << juce::String(s.emittedFirstSeg);
                line << " mix=" << juce::String(s.mixInvoked ? "yes" : "no");
                line << " mixSkip=" << juce::String(s.mixSkipped ? "yes" : "no");
                if (const char* wy = s.mixSkipReason; s.mixSkipped && wy != nullptr && wy[0] != '\0')
                {
                    line << " mixSkipWhy=" << juce::String(wy);
                }
                else if (s.mixSkipped)
                {
                    line << " mixSkipWhy=(unknown)";
                }
                line << " blockCap=" << juce::String(static_cast<int>(blockCap));
                line << " midiDiscarded="
                     << juce::String(static_cast<juce::uint64>(midiDiscarded));
                line << ']';

                const bool masterHadApprox = (s.mixDevicePeakBefore > 1.0e-6f);

                juce::String mixLine = "instrument-mix: tid=";
                mixLine << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(s.sessionTrackId)));
                mixLine << " playT0="
                        << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(playEdgeT0)));
                mixLine << " transportPlaying="
                        << juce::String(transportPlaying ? "yes" : "no");
                mixLine << " h="
                        << ((s.hostAddr != 0)
                                ? (juce::String("0x")
                                   + juce::String::toHexString(static_cast<juce::int64>(
                                       static_cast<std::int64_t>(s.hostAddr))))
                                : juce::String("null"));
                mixLine << " ctl="
                        << ((s.ctlAddr != 0)
                                ? (juce::String("0x")
                                   + juce::String::toHexString(static_cast<juce::int64>(
                                       static_cast<std::int64_t>(s.ctlAddr))))
                                : juce::String("null"));

                mixLine << " mixInvoked=" << juce::String(s.mixInvoked ? "yes" : "no");
                mixLine << " mixSkip=" << juce::String(s.mixSkipped ? "yes" : "no");
                if (const char* wy = s.mixSkipReason; s.mixSkipped && wy != nullptr && wy[0] != '\0')
                {
                    mixLine << " mixSkipReason=" << juce::String(wy);
                }
                else if (s.mixSkipped)
                {
                    mixLine << " mixSkipReason=(unknown)";
                }
                else
                {
                    mixLine << " mixSkipReason=none";
                }

                mixLine << " peakDevStereoBefore=" << juce::String(s.mixDevicePeakBefore, 9);
                mixLine << " peakDevStereoAfter=" << juce::String(s.mixDevicePeakAfter, 9);
                mixLine << " approxAddedStereoPeak=" << juce::String(s.mixAddedPeakApprox, 9);
                mixLine << " faderApplied=" << juce::String(s.sessionFaderGain, 9);
                mixLine << " ctlRenderPb=" << juce::String(s.renderPb ? "yes" : "no");
                mixLine << " masterHadNonSilentAudioApprox="
                        << juce::String(masterHadApprox ? "yes" : "no");

                routingLines.emplace_back(std::move(mixLine));

                RoutingInstrumentPeekDiagRow prow;
                prow.tid = s.sessionTrackId;
                prow.host = s.hostLive;
                peekRows.emplace_back(std::move(prow));
            }

            struct PendingRoutingBurst
            {
                juce::String edge;
                std::vector<juce::String> mix;
                std::vector<RoutingInstrumentPeekDiagRow> peeks;
            };

            juce::MessageManager::callAsync([burst = PendingRoutingBurst{
                                                 std::move(line),
                                                 std::move(routingLines),
                                                 std::move(peekRows),
                                             }]() mutable {
                appendExperimentalPlaybackRoutingLogLine(std::move(burst.edge));
                for (juce::String& l : burst.mix)
                {
                    appendExperimentalPlaybackRoutingLogLine(std::move(l));
                }

                constexpr char kInstrumentAudioPrefix[] = "instrument-audio:";
                constexpr int kInstrumentAudioPrefixLen =
                    (int)((sizeof(kInstrumentAudioPrefix) / sizeof(kInstrumentAudioPrefix[0])) - 1);

                for (RoutingInstrumentPeekDiagRow& p : burst.peeks)
                {
                    if (p.host == nullptr)
                        continue;

                    juce::String trimmed = p.host->peekInstrumentAudioRoutingDiagLineForMessageThread().trim();
                    if (trimmed.startsWithIgnoreCase(kInstrumentAudioPrefix))
                        trimmed = trimmed.substring(kInstrumentAudioPrefixLen).trimStart();

                    appendExperimentalPlaybackRoutingLogLine(
                        juce::String("instrument-audio: tid="
                                     + juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(p.tid)))
                                     + " ")
                        + trimmed);
                }
            });
        }
    };
} // namespace

PlaybackEngine::PlaybackEngine(Transport& transport, Session& session, RecorderService* recorder,
                               CountInClickOutput* countIn, PluginInsertHost* pluginHost)
    : transport_(transport)
    , session_(session)
    , recorder_(recorder)
    , countIn_(countIn)
    , pluginHost_(pluginHost)
{
}

PlaybackEngine::~PlaybackEngine() = default;

void PlaybackEngine::publishExperimentalInstrumentPlaybackSnapshot(
    std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> snapshot) noexcept
{
    experimentalInstrumentPlaybackSnapshot_.store(std::move(snapshot), std::memory_order_release);
}

void PlaybackEngine::setExperimentalInstrumentDeviceLifecycleHooks(
    std::function<void(double sampleRate, int blockSizeSamples)> prepareAllHosts,
    std::function<void()> releaseAllHosts,
    std::function<void(int numSamples)> beginBlockAllHosts) noexcept
{
    experimentalPrepareAllHosts_ = std::move(prepareAllHosts);
    experimentalReleaseAllHosts_ = std::move(releaseAllHosts);
    experimentalBeginBlockAllHosts_ = std::move(beginBlockAllHosts);
}

void PlaybackEngine::setPlaybackOffsetSamples(const std::int64_t samples) noexcept
{
    playbackOffsetSamples_.store(samples, std::memory_order_release);
}

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        const double sr = device->getCurrentSampleRate();
        const int bs = device->getCurrentBufferSizeSamples();
        const int nOut = device->getActiveOutputChannels().countNumberOfSetBits();
        if (pluginHost_ != nullptr)
        {
            pluginHost_->prepareForDevice(sr, bs, nOut);
        }
        if (experimentalPrepareAllHosts_)
        {
            experimentalPrepareAllHosts_(sr, bs);
        }
    }
}

void PlaybackEngine::audioDeviceStopped()
{
    if (pluginHost_ != nullptr)
    {
        pluginHost_->releaseResources();
    }
    if (experimentalReleaseAllHosts_)
    {
        experimentalReleaseAllHosts_();
    }
}

void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context);

    // [Audio thread] Phase 4: route mono input[0] to the recorder SPSC path only while `isRecording()`
    // and valid input pointers; does not access Session. `pushInputBlock` still no-ops if not recording
    // — this call site avoids touching the recorder SPSC at all when idle.
    if (recorder_ != nullptr
        && recorder_->isRecording()
        && numInputChannels > 0
        && numSamples > 0
        && inputChannelData != nullptr
        && inputChannelData[0] != nullptr)
    {
        recorder_->pushInputBlock(inputChannelData[0], numSamples);
    }

    const int deviceBlockSizeInFrames = numSamples;
    transport_.audioThread_beginBlock();

    const std::shared_ptr<const SessionSnapshot> sessionSnap = session_.loadSessionSnapshotForAudioThread();
    /// [Audio thread] Same publish discipline as Session: acquire-load retains a const view for this block only.
    const std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> instrumentSnap
        = experimentalInstrumentPlaybackSnapshot_.load(std::memory_order_acquire);

    // Per-block RT MIDI delivery uses `audioCallbackBlockSamples_` (see `ExperimentalInstrumentHost`). The
    // snapshot is the engine's source of truth for which host(s) are driven this block — call `beginAudioBlock`
    // here before the optional map/staging lambda so message-thread map drift cannot skip the active host.
    if (instrumentSnap != nullptr)
    {
        for (const auto& e : instrumentSnap->entries)
        {
            if (e.host != nullptr)
            {
                e.host->audioThread_beginAudioBlock(deviceBlockSizeInFrames);
            }
        }
    }

    if (experimentalBeginBlockAllHosts_)
    {
        experimentalBeginBlockAllHosts_(deviceBlockSizeInFrames);
    }

    const PlaybackIntent playbackIntent = transport_.audioThread_loadIntent();
    const std::int64_t t0 = transport_.audioThread_loadPlayhead();

    struct StoreIntentAtScopeExit
    {
        PlaybackIntent v;
        PlaybackIntent* d;
        ~StoreIntentAtScopeExit() noexcept { *d = v; }
    };
    const StoreIntentAtScopeExit storePlaybackIntent { playbackIntent, &lastTransportIntentInCallback_ };

    const bool becameStopped = (playbackIntent != PlaybackIntent::Playing
                               && lastTransportIntentInCallback_ == PlaybackIntent::Playing);
    if (becameStopped)
    {
        if (instrumentSnap != nullptr)
        {
            for (const auto& e : instrumentSnap->entries)
            {
                if (e.midiController != nullptr && e.host != nullptr)
                {
                    e.midiController->audioThread_flushTransportMidi(*e.host, 0, deviceBlockSizeInFrames);
                }
            }
        }
    }

    const bool becamePlayingTransport = (playbackIntent == PlaybackIntent::Playing
                                        && lastTransportIntentInCallback_ != PlaybackIntent::Playing);

    constexpr int kRoutingInstSlotsCap = 24;
    InstPlayEdgeDiagSlot routingInstSlots[kRoutingInstSlotsCap];
    int routingInstSlotCount = 0;
    const bool routePlayEdgeDiag =
        becamePlayingTransport && sessionSnap != nullptr && deviceBlockSizeInFrames > 0;
    if (routePlayEdgeDiag)
    {
        for (int rti = 0; rti < sessionSnap->getNumTracks(); ++rti)
        {
            const Track& itr = sessionSnap->getTrack(rti);
            if (itr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            if (routingInstSlotCount >= kRoutingInstSlotsCap)
            {
                break;
            }
            InstPlayEdgeDiagSlot& s = routingInstSlots[routingInstSlotCount++];
            s.sessionTrackId = itr.getId();
            s.sessionOff = itr.isTrackOff();
            s.sessionMuted = itr.isMuted();
            const ExperimentalInstrumentPlaybackEntry* pe = instrumentSnap != nullptr
                                                                   ? findExperimentalInstrumentPlaybackEntry(
                                                                       *instrumentSnap, itr.getId())
                                                                   : nullptr;
            if (pe != nullptr && pe->host != nullptr && pe->midiController != nullptr)
            {
                s.registryYes = true;
                s.entryTrackId = pe->trackId;
                s.hostAddr = reinterpret_cast<std::uintptr_t>(static_cast<void*>(pe->host));
                s.ctlAddr = reinterpret_cast<std::uintptr_t>(static_cast<void*>(pe->midiController));
                s.hostLive = pe->host;
                s.ctlDomain = pe->midiController->getExperimentalInstrumentDomainTrackId();
                if (const auto rs = pe->midiController->loadRenderSnapshotForAudioThread())
                {
                    s.renderSnap = true;
                    s.renderPb = rs->playbackEnabled;
                    s.plans = static_cast<int>(rs->clips.size());
                }
            }
        }
    }

    ExperimentalPlaybackRoutingPlayEdgePoster routingPlaybackPlayEdgePoster {
        routePlayEdgeDiag, routingInstSlots, routingInstSlotCount, t0,
        playbackIntent == PlaybackIntent::Playing,
    };

#if !defined(NDEBUG)
    /// One-shot per transport PLAY edge: verifies `TrackKind::Instrument` rows resolve against I1 playback entries.
    if (becamePlayingTransport && sessionSnap != nullptr)
    {
        for (int ti = 0; ti < sessionSnap->getNumTracks(); ++ti)
        {
            const Track& tr = sessionSnap->getTrack(ti);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const ExperimentalInstrumentPlaybackEntry* e = nullptr;
            if (instrumentSnap != nullptr)
            {
                e = findExperimentalInstrumentPlaybackEntry(*instrumentSnap, tr.getId());
            }

            TrackId ctlDom = kInvalidTrackId;
            int clipPlanCount = -1;
            bool playbackGate = false;
            const void* hostPtr = nullptr;
            if (e != nullptr && e->host != nullptr && e->midiController != nullptr)
            {
                ctlDom = e->midiController->getExperimentalInstrumentDomainTrackId();
                hostPtr = static_cast<const void*>(e->host);
                if (const auto rs = e->midiController->loadRenderSnapshotForAudioThread())
                {
                    clipPlanCount = static_cast<int>(rs->clips.size());
                    playbackGate = rs->playbackEnabled;
                }
            }

            juce::Logger::writeToLog(juce::String("PlaybackEngine[I1-debug] transport PLAY instrument row ")
                                     + juce::String(ti) + " sessionTrackId="
                                     + juce::String((juce::int64)(std::int64_t) tr.getId())
                                     + " registryEntry="
                                     + juce::String(e != nullptr ? "yes" : "no")
                                     + " host="
                                     + (hostPtr != nullptr
                                            ? ("0x"
                                               + juce::String::toHexString(
                                                   (juce::int64) reinterpret_cast<std::uintptr_t>(hostPtr)))
                                            : juce::String("nullptr"))
                                     + " ctlDomain="
                                     + juce::String((juce::int64)(std::int64_t) ctlDom)
                                     + " ctlNonNull="
                                     + juce::String(e != nullptr && e->midiController != nullptr ? "yes" : "no")
                                     + " midiClipPlans="
                                     + juce::String(clipPlanCount)
                                     + " renderPlaybackEnabled="
                                     + juce::String(playbackGate ? "yes" : "no"));
        }
    }
#endif

    /// [Audio thread] Sum each keyed instrument whose `trackId` matches a `TrackKind::Instrument`
    /// row in `sessionSnap`, in timeline row order (see MIX ORDER below). When `sessionSnap` is missing
    /// (tear / edge), mix every snapshot entry once so staged-only playback still audible.
    const auto mixKeyedInstrumentLanesIntoOutputsIfAny = [&]()
    {
        if (instrumentSnap == nullptr || instrumentSnap->entries.empty())
        {
            return;
        }
        if (sessionSnap != nullptr)
        {
#if !defined(NDEBUG)
            static TrackId loggedMissingPlaybackBindingOnce = kInvalidTrackId;
#endif
            // MIX ORDER: iterate session rows so instrument buses follow mixed track order once multiple
            // instrument lanes exist — today `entries` holds at most one row (UI unchanged).
            for (int ti = 0; ti < sessionSnap->getNumTracks(); ++ti)
            {
                const Track& tr = sessionSnap->getTrack(ti);
                if (tr.getKind() != TrackKind::Instrument)
                {
                    continue;
                }
                const ExperimentalInstrumentPlaybackEntry* entry
                    = findExperimentalInstrumentPlaybackEntry(*instrumentSnap, tr.getId());
                if (entry == nullptr || entry->host == nullptr)
                {
#if !defined(NDEBUG)
                    if (loggedMissingPlaybackBindingOnce != tr.getId())
                    {
                        loggedMissingPlaybackBindingOnce = tr.getId();
                        juce::Logger::writeToLog(juce::String("PlaybackEngine: Instrument lane TrackId=")
                                                 + juce::String(static_cast<juce::int64>(std::int64_t(tr.getId())))
                                                 + " has no playback registry entry — instrument silent this block.");
                    }
#endif
                    continue;
                }

                const int sx = routePlayEdgeDiag ? indexOfInstrumentPlayEdgeDiagSlot(
                                       routingInstSlots, routingInstSlotCount, tr.getId())
                                                   : -1;

                auto fillPlayEdgePeekOnly = [&](const char* skipWhy) noexcept
                {
                    if (sx < 0)
                        return;
                    const float pkSnap = peakAbsStereoDevice(outputChannelData, numOutputChannels, numSamples);
                    routingInstSlots[sx].mixSkipped = true;
                    routingInstSlots[sx].mixSkipReason = skipWhy;
                    routingInstSlots[sx].mixInvoked = false;
                    routingInstSlots[sx].mixDevicePeakBefore = pkSnap;
                    routingInstSlots[sx].mixDevicePeakAfter = pkSnap;
                    routingInstSlots[sx].mixAddedPeakApprox = 0.0f;
                    routingInstSlots[sx].sessionFaderGain = tr.getChannelFaderGain();
                };

                if (tr.isTrackOff())
                {
                    fillPlayEdgePeekOnly("sessionOff");
                    continue;
                }
                if (tr.isMuted())
                {
                    fillPlayEdgePeekOnly("sessionMuted");
                    continue;
                }

                const float fader = tr.getChannelFaderGain();
                if (fader <= 0.0f)
                {
                    fillPlayEdgePeekOnly("faderZero");
                    continue;
                }

                const float pkBeforeThisMix = (routePlayEdgeDiag && sx >= 0)
                                                  ? peakAbsStereoDevice(outputChannelData, numOutputChannels, numSamples)
                                                  : 0.0f;
                if (sx >= 0)
                {
                    routingInstSlots[sx].mixDevicePeakBefore = pkBeforeThisMix;
                    routingInstSlots[sx].mixSkipped = false;
                    routingInstSlots[sx].mixSkipReason = "";
                    routingInstSlots[sx].sessionFaderGain = fader;
                }

                mixExperimentalInstrumentAfterTracks(
                    entry->host, outputChannelData, numOutputChannels, numSamples, fader);

                if (sx >= 0)
                {
                    routingInstSlots[sx].mixInvoked = true;
                    const float pkAfter = peakAbsStereoDevice(outputChannelData, numOutputChannels, numSamples);
                    routingInstSlots[sx].mixDevicePeakAfter = pkAfter;
                    routingInstSlots[sx].mixAddedPeakApprox = juce::jmax(0.0f, pkAfter - pkBeforeThisMix);
                }
            }
            return;
        }
        for (const auto& e : instrumentSnap->entries)
        {
            if (e.host != nullptr)
            {
                mixExperimentalInstrumentAfterTracks(
                    e.host, outputChannelData, numOutputChannels, numSamples, 1.0f);
            }
        }
    };

    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        if (float* row = outputChannelData[ch])
        {
            juce::FloatVectorOperations::clear(row, numSamples);
        }
    }
    if (countIn_ != nullptr)
    {
        countIn_->audioThread_mixInto(outputChannelData, numOutputChannels, numSamples);
    }

    if (sessionSnap == nullptr || deviceBlockSizeInFrames <= 0
        || playbackIntent != PlaybackIntent::Playing)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        return;
    }

    const std::int64_t timelineEnd = sessionSnap->getArrangementExtentSamples();
    if (timelineEnd <= 0 || t0 >= timelineEnd)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        return;
    }

    const auto jmax0 = [](const std::int64_t x) noexcept -> std::int64_t
    {
        return x < 0 ? std::int64_t{ 0 } : x;
    };

    const bool cycleOn = transport_.audioThread_loadCycleEnabled();
    const std::int64_t locL = sessionSnap->getLeftLocatorSamples();
    const std::int64_t locR = sessionSnap->getRightLocatorSamples();
    const bool validCycle = cycleOn && locR > locL && locR > 0;

#if !defined(NDEBUG)
    {
        static bool s_loggedPastRlinearMode = false;
        if (!cycleOn || !validCycle || t0 < locR)
            s_loggedPastRlinearMode = false;
        else if (!s_loggedPastRlinearMode && t0 >= locR && validCycle)
        {
            s_loggedPastRlinearMode = true;
            juce::Logger::writeToLog(
                juce::String("PlaybackEngine diag: cycle on + valid [L,R) but playhead >= R ")
                + "(linear, no wrap this block). cycleOn="
                + juce::String(cycleOn ? "true" : "false")
                + " L="
                + juce::String(locL)
                + " R="
                + juce::String(locR)
                + " t0="
                + juce::String(t0));
        }
    }
#endif

    std::int64_t tWork = t0;

    const std::int64_t availTimeline = timelineEnd - tWork;
    if (availTimeline <= 0)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        return;
    }

    const std::int64_t blockFrames = static_cast<std::int64_t>(deviceBlockSizeInFrames);

    const std::int64_t playbackShift = playbackOffsetSamples_.load(std::memory_order_acquire);

    const auto renderRun = [&](const std::int64_t segT0, const int segRun, const int outFrame0,
                               const bool forceSegDiscontinuity) noexcept
    {
        if (segRun <= 0)
        {
            return;
        }

        jassert(segRun > 0);

        const std::int64_t renderBase = segT0 + playbackShift;
        std::int64_t silenceFrames = 0;
        if (renderBase < 0)
        {
            silenceFrames = juce::jmin(static_cast<std::int64_t>(segRun), -renderBase);
        }
        const int audibleRun = static_cast<int>(static_cast<std::int64_t>(segRun) - silenceFrames);
        if (audibleRun <= 0)
        {
            return;
        }
        const std::int64_t timelineStartAudible = renderBase + silenceFrames;
        jassert(timelineStartAudible >= 0);
        const int silencePrefix = static_cast<int>(silenceFrames);

        for (int ti = 0; ti < sessionSnap->getNumTracks(); ++ti)
        {
            const Track& tr = sessionSnap->getTrack(ti);
            if (tr.getKind() == TrackKind::Instrument)
            {
                continue;
            }
            if (recorder_ != nullptr && recorder_->isRecording()
                && tr.getId() == recorder_->getRecordingTrackId())
            {
                // Transient: do not play existing clips on the track being recorded; other tracks mix as usual.
                continue;
            }
            if (tr.isTrackOff())
            {
                continue;
            }
            const float storedFaderGain = tr.getChannelFaderGain();
            const float effectiveGain = tr.isMuted() ? 0.0f : storedFaderGain;
            if (!tr.isMuted() && storedFaderGain <= 0.0f)
            {
                continue;
            }
            const std::vector<PlacedClip>& lane = tr.getPlacedClips();
            const bool useInsert = pluginHost_ != nullptr
                                   && pluginHost_->audioThread_hasActivePluginForTrack(tr.getId());
            std::int64_t t = timelineStartAudible;
            int out0 = 0;
            while (out0 < audibleRun)
            {
                const int row = findCoveringRowIndexInLane(lane, t);
                const std::int64_t nextB = minBoundaryStrictlyAfterInLane(lane, t, timelineEnd);
                jassert(nextB > t);
                int run = static_cast<int>(juce::jmin(
                    static_cast<std::int64_t>(audibleRun - out0), nextB - t));
                jassert(run > 0);

                if (row >= 0)
                {
                    const PlacedClip& p = lane[(size_t)row];
                    const AudioClip& c = p.getAudioClip();
                    const std::int64_t rel = t - p.getStartSample();
                    jassert(rel >= 0);
                    jassert(rel + static_cast<std::int64_t>(run) <= p.getEffectiveLengthSamples());
                    const int off = static_cast<int>(rel + p.getLeftTrimSamples());
                    jassert(off >= 0);
                    jassert(off + run <= c.getNumSamples());
                    const int destFrame = outFrame0 + silencePrefix + out0;

                    if (useInsert && effectiveGain > 0.0f)
                    {
                        pluginHost_->audioThread_clearScratch(PluginInsertHost::kInsertChannels, run);
                        if (float* const* scratch = pluginHost_->audioThread_getScratchWritePointers())
                        {
                            copyClipRunToStereoScratch(c, off, run, scratch[0], scratch[1]);
                            pluginHost_->audioThread_processChainForTrack(tr.getId(), InsertStage::Pre, run);
                            scaleStereoScratch(scratch, run, effectiveGain);
                            pluginHost_->audioThread_processChainForTrack(tr.getId(), InsertStage::Post, run);
                            addStereoScratchToDeviceOutputs(scratch,
                                                            run,
                                                            destFrame,
                                                            numOutputChannels,
                                                            outputChannelData,
                                                            1.0f);
                        }
                    }
                    else
                    {
                        addClipRunToOutputs(
                            c, off, run, destFrame, numOutputChannels, outputChannelData, effectiveGain);
                    }
                }
                t += run;
                out0 += run;
            }
            jassert(out0 == audibleRun);
            jassert(t - timelineStartAudible == static_cast<std::int64_t>(audibleRun));
        }

        // Timeline order: dispatch transport MIDI toward each Instrument row that has a playback entry.
        if (playbackIntent == PlaybackIntent::Playing && instrumentSnap != nullptr)
        {
            const bool segDisc = forceSegDiscontinuity || becamePlayingTransport;
            for (int instTi = 0; instTi < sessionSnap->getNumTracks(); ++instTi)
            {
                const Track& itr = sessionSnap->getTrack(instTi);
                if (itr.getKind() != TrackKind::Instrument)
                    continue;

                const ExperimentalInstrumentPlaybackEntry* const entry =
                    findExperimentalInstrumentPlaybackEntry(*instrumentSnap, itr.getId());
                if (entry == nullptr)
                    continue;

                const int sx = routePlayEdgeDiag
                                   ? indexOfInstrumentPlayEdgeDiagSlot(
                                         routingInstSlots, routingInstSlotCount, itr.getId())
                                   : -1;

                if (routePlayEdgeDiag && sx >= 0)
                {
                    routingInstSlots[sx].scheduleCalled = true;
                }

                int* emitPtr = nullptr;
                if (routePlayEdgeDiag && sx >= 0 && !routingInstSlots[sx].firstSegDiagCaptured)
                {
                    emitPtr = &routingInstSlots[sx].emittedFirstSeg;
                }

                entry->midiController->audioThread_scheduleTransportMidiForSegment(*entry->host,
                                                                                     timelineStartAudible,
                                                                                     audibleRun,
                                                                                     outFrame0 + silencePrefix,
                                                                                     segDisc,
                                                                                     deviceBlockSizeInFrames,
                                                                                     emitPtr);

                if (routePlayEdgeDiag && sx >= 0 && emitPtr != nullptr)
                {
                    routingInstSlots[sx].firstSegDiagCaptured = true;
                }
            }
        }
    };

    // --- Linear playback (cycle off, invalid range, or playhead already at / past right locator). ---
    if (!validCycle || tWork >= locR)
    {
        const std::int64_t firstRun64 = juce::jmin(blockFrames, jmax0(availTimeline));
        if (firstRun64 <= 0)
        {
            transport_.audioThread_advancePlayheadIfPlaying(0);
            mixKeyedInstrumentLanesIntoOutputsIfAny();
            return;
        }
        renderRun(tWork, static_cast<int>(firstRun64), 0, becamePlayingTransport);
        transport_.audioThread_advancePlayheadIfPlaying(firstRun64);
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        return;
    }

    // --- Cycle wrap: approached from tWork < locR ---
    const std::int64_t framesToR = locR - tWork;
    const std::int64_t maxPlayableThisBlock = juce::jmin(blockFrames, jmax0(availTimeline));
    const std::int64_t firstRun64 = juce::jmin(maxPlayableThisBlock, framesToR);

    if (firstRun64 <= 0)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        return;
    }

    const int firstRun = static_cast<int>(firstRun64);
    renderRun(tWork, firstRun, 0, becamePlayingTransport);

    const bool reachedRightLocator = (tWork + firstRun64 >= locR);
    if (!reachedRightLocator)
    {
        transport_.audioThread_advancePlayheadIfPlaying(firstRun64);
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        return;
    }

    const std::int64_t remainingInBlock = blockFrames - firstRun64;
    const std::int64_t loopSpan = locR - locL;
    const std::int64_t secondRun64 = juce::jmin(
        jmax0(remainingInBlock),
        jmax0(loopSpan),
        jmax0(timelineEnd - locL));

    if (secondRun64 > 0)
    {
        const int sr = static_cast<int>(secondRun64);
        renderRun(locL, sr, firstRun, true);
        transport_.audioThread_storePlayheadOnWrap(locL + secondRun64);
        transport_.audioThread_signalCycleWrap();
#if !defined(NDEBUG)
        juce::Logger::writeToLog(
            juce::String("PlaybackEngine wrap: cycleOn=")
            + (cycleOn ? "1" : "0")
            + " valid=1"
            + " L="
            + juce::String(locL)
            + " R="
            + juce::String(locR)
            + " t0="
            + juce::String(t0)
            + " framesToR="
            + juce::String(framesToR)
            + " firstRun="
            + juce::String(firstRun64)
            + " wrapped=1 secondRun="
            + juce::String(secondRun64)
            + " storePlayhead="
            + juce::String(locL + secondRun64)
            + " wrapCount="
            + juce::String(transport_.audioThread_relaxedLoadWrapPassCount()));
#endif
    }
    else
    {
        transport_.audioThread_storePlayheadOnWrap(locL);
        transport_.audioThread_signalCycleWrap();
#if !defined(NDEBUG)
        juce::Logger::writeToLog(
            juce::String("PlaybackEngine wrap: cycleOn=")
            + (cycleOn ? "1" : "0")
            + " valid=1"
            + " L="
            + juce::String(locL)
            + " R="
            + juce::String(locR)
            + " t0="
            + juce::String(t0)
            + " framesToR="
            + juce::String(framesToR)
            + " firstRun="
            + juce::String(firstRun64)
            + " wrapped=1 secondRun=0 (block ends exactly at R)"
            + " storePlayhead=L="
            + juce::String(locL)
            + " wrapCount="
            + juce::String(transport_.audioThread_relaxedLoadWrapPassCount()));
#endif
    }
    mixKeyedInstrumentLanesIntoOutputsIfAny();
}
