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
//   clip/pre-gain — see `Track`), then **per-track stereo pan** (`TrackStereoPan.h`: linear balance,
//   center leaves L/R gains at unity vs pre-pan). With inserts (Phase 8 / Slice B), that fader multiplier is applied
//   on the scratch between Pre and Post chains; the dry path still applies gain at merge.
//
// PHASE 8 / Slice B (per-track VST3 Pre/Post): when `audioThread_hasActivePluginForTrack` reports an
//   active stereo insert, clip audio copies into `PluginInsertHost`'s stereo scratch at unity, runs the
//   **Pre** chain, applies effective track fader/mute gain in-place, runs the **Post** chain, applies pan,
//   then sums scratch into the device buffer at unity (same mono L+R blend rule as the dry path).
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

#include "app/ShortcutDiagnostics.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/RoutingPlanBuilder.h"
#include "engine/CountInClickOutput.h"
#include "engine/RecorderService.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "diagnostics/AudioThreadProfiler.h"
#include "diagnostics/ExperimentalPlaybackRoutingLog.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
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
#if !MINIDAW_DIAG_PLAYBACK_ROUTING
            return;
#else
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
#endif // MINIDAW_DIAG_PLAYBACK_ROUTING
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
    ensureMasterScratchCapacity(kOfflineMixdownBlockCapSamples);
    // Generation-stage worker pool: sized once here (message thread, no device running yet).
    // `--instrument-workers N` overrides the conservative machine default; 0 = serial.
    const int override = instrument_render::configuredWorkerCountOverride();
    const int workers = override >= 0 ? override : instrument_render::defaultWorkerCount();
    instrumentRenderPool_ = std::make_unique<instrument_render::InstrumentRenderPool>();
    instrumentRenderPool_->setWorkerCount(workers);
    // Experimental read-ahead (docs/READAHEAD_PROTOTYPE.md): exists ONLY with the CLI flag
    // `--experimental-readahead[=N]`; absent flag = no renderer = the exact A1/A2 paths.
    const int readAheadDepth = readahead::configuredReadAheadDepth();
    if (readAheadDepth > 0)
    {
        readahead::ReadAheadRenderer::Deps deps;
        deps.session = &session_;
        deps.pluginHost = pluginHost_;
        deps.preGainRamp = &preGainRampState_;
        deps.soloViewAtomic = &soloMuteView_;
        readAhead_ = std::make_unique<readahead::ReadAheadRenderer>(deps, readAheadDepth, true);
    }
}

void PlaybackEngine::enableExperimentalReadAheadForTests(const int depthBlocks)
{
    readahead::ReadAheadRenderer::Deps deps;
    deps.session = &session_;
    deps.pluginHost = pluginHost_;
    deps.preGainRamp = &preGainRampState_;
    deps.soloViewAtomic = &soloMuteView_;
    readAhead_ = std::make_unique<readahead::ReadAheadRenderer>(deps, depthBlocks, false);
}

void PlaybackEngine::pauseReadAheadWorkerAfterChainPublish() noexcept
{
    if (readAhead_ != nullptr)
    {
        // The new insert map is already published and the in-flight callback was waited out:
        // an acknowledged pause proves the worker is outside every chain render that could
        // still reference the retired instances; after the resume it only ever acquires the
        // newly published map (fresh per segment). Ownership and queues survive — chain edits
        // late-apply like every other control (model doc §6/§8).
        readAhead_->pauseWorkerAndWait();
        readAhead_->resumeWorker();
    }
}

void PlaybackEngine::beginPluginStateCaptureWindow() noexcept
{
    if (readAhead_ == nullptr)
    {
        return;
    }
    readAhead_->beginStateCaptureHold();
    // Gapless drain of every owned row — it needs playback consumption, so it is attempted only
    // while the transport is playing (paused/stopped saves capture with the worker paused and
    // nobody processing; model doc §9). Bounded message-thread wait, never touches the callback.
    if (transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (readAhead_->audioThread_anyOwned() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    readAhead_->pauseWorkerAndWait();
}

void PlaybackEngine::endPluginStateCaptureWindow() noexcept
{
    if (readAhead_ == nullptr)
    {
        return;
    }
    readAhead_->resumeWorker();
    readAhead_->endStateCaptureHold();
}

// Tear order (see the header): Main removes the audio callback before destroying the engine, so
// no dispatch can be in flight here; the pool's destructor joins its idle workers.
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

void PlaybackEngine::ensureMasterScratchCapacity(const int numSamples) noexcept
{
    if (numSamples <= 0)
    {
        return;
    }
    if (masterScratchCapacity_ >= numSamples && masterScratchPtrs_[0] != nullptr
        && masterScratchPtrs_[1] != nullptr)
    {
        return;
    }
    masterScratch_.setSize(2, numSamples, false, false, true);
    masterScratchPtrs_[0] = masterScratch_.getWritePointer(0);
    masterScratchPtrs_[1] = masterScratch_.getWritePointer(1);
    masterScratchCapacity_ = numSamples;
    ensurePostStripStageScratchCapacity(numSamples);
}

void PlaybackEngine::ensurePostStripStageScratchCapacity(const int numSamples) noexcept
{
    if (numSamples <= 0)
    {
        return;
    }
    if (postStripStageCapacity_ >= numSamples && postStripStagePtrs_[0] != nullptr
        && postStripStagePtrs_[1] != nullptr)
    {
        return;
    }
    postStripStageScratch_.setSize(2, numSamples, false, false, true);
    postStripStagePtrs_[0] = postStripStageScratch_.getWritePointer(0);
    postStripStagePtrs_[1] = postStripStageScratch_.getWritePointer(1);
    postStripStageCapacity_ = numSamples;
}

void PlaybackEngine::ensureAudioStripJobBuffersCapacity(const int numSamples) noexcept
{
    // [Message thread, device stopped] Stage A1 job buffers. All realtime access is gated on the
    // recorded capacities, so a block larger than prepared simply renders on the serial fallback.
    if (numSamples <= 0)
    {
        return;
    }
    using instrument_render::InstrumentRenderPool;
    // The lane model only works when "render-pool lane" and "insert-host processing lane" are the
    // same coordinate system (worker i = lane i, callback = last lane) and when every collected
    // audio payload fits the pool's job table even with zero instrument jobs.
    static_assert(InstrumentRenderPool::kNumLanes <= PluginInsertHost::kMaxProcessingLanes,
                  "every render-pool lane must map to an insert-host processing lane (the host "
                  "additionally reserves lane 16 for the experimental read-ahead worker)");
    static_assert(InstrumentRenderPool::kCallbackLane == PluginInsertHost::kCallbackProcessingLane,
                  "the callback must map to the same lane index in both subsystems");
    static_assert(kMaxAudioStripJobs <= InstrumentRenderPool::kMaxJobs,
                  "every collected audio strip payload must fit the pool's job table");
    // Stage A2: a full block of audio payloads plus a full set of combined instrument payloads
    // must still fit ONE job table (combined jobs REPLACE that host's generation job 1:1, so
    // this bound is the worst case, not an estimate).
    static_assert(kMaxAudioStripJobs + kMaxInstrumentStripJobs <= InstrumentRenderPool::kMaxJobs,
                  "audio strip payloads + combined instrument payloads must fit the job table");
    if (audioStripStageCapacity_ < numSamples)
    {
        audioStripStageBuffer_.setSize(2 * kMaxAudioStripJobs, numSamples, false, false, true);
        audioStripStageCapacity_ = numSamples;
    }
    if (instrumentStripStageCapacity_ < numSamples)
    {
        instrumentStripStageBuffer_.setSize(2 * kMaxInstrumentStripJobs, numSamples, false, false, true);
        instrumentStripStageCapacity_ = numSamples;
    }
    if (insertLaneScratchCapacity_ < numSamples)
    {
        insertLaneScratch_.setSize(2 * InstrumentRenderPool::kNumLanes, numSamples, false, false, true);
        for (int lane = 0; lane < InstrumentRenderPool::kNumLanes; ++lane)
        {
            insertLaneScratchPtrs_[lane][0] = insertLaneScratch_.getWritePointer(2 * lane);
            insertLaneScratchPtrs_[lane][1] = insertLaneScratch_.getWritePointer(2 * lane + 1);
        }
        insertLaneScratchCapacity_ = numSamples;
    }
    for (auto& midi : insertLaneMidi_)
    {
        // Same discipline as the host's shared `midiScratch_`: pre-sized here, `clear` (which
        // never shrinks or grows) after every processBlock on the job path.
        midi.ensureSize(1024);
    }
}

void PlaybackEngine::runAudioStripRenderJob(instrument_render::RenderJob& job, const int laneIndex) noexcept
{
    auto* const payload = static_cast<AudioStripJobPayload*>(job.p.context);
    if (payload != nullptr && payload->engine != nullptr)
    {
        payload->engine->audioThread_runAudioStripPayload(*payload, laneIndex);
    }
}

void PlaybackEngine::audioThread_runAudioStripPayload(AudioStripJobPayload& payload,
                                                      const int laneIndex) noexcept
{
    using instrument_render::InstrumentRenderPool;
    if (payload.sessionSnap == nullptr || payload.stageL == nullptr || payload.stageR == nullptr
        || payload.numSegments <= 0)
    {
        return;
    }
    const std::int64_t tStart = juce::Time::getHighResolutionTicks();
    const int lane = juce::jlimit(0, InstrumentRenderPool::kNumLanes - 1, laneIndex);

    // The job writes ONLY the stage regions its segments own (the fan reads exactly these), its
    // own lane's chain scratch / MIDI scratch, its own row's pre-gain ramp entry, and its own
    // row's duration slot — nothing shared with a concurrently running job (plan §3.2).
    playback_mix_helpers::AudioStripInsertAccess access;
    if (payload.chainEntry != nullptr && pluginHost_ != nullptr)
    {
        access.host = pluginHost_;
        access.entry = payload.chainEntry;
        access.chainScratch = insertLaneScratchPtrs_[lane];
        access.chainScratchCapacity = insertLaneScratchCapacity_;
        access.chainMidiScratch = &insertLaneMidi_[(size_t)lane];
        access.laneIndex = lane;
    }
    for (int s = 0; s < payload.numSegments; ++s)
    {
        const AudioStripSegmentDesc& seg = payload.segments[(size_t)s];
        if (seg.audibleRun <= 0)
        {
            continue;
        }
        playback_mix_helpers::clearStereoScratch(payload.stageL + seg.destFrame,
                                                 payload.stageR + seg.destFrame, seg.audibleRun);
        if (access.entry != nullptr)
        {
            // Per-segment transport context on this chain's OWN playhead — the same PositionInfo
            // sequence the serial path presents, regardless of which lane runs the job.
            PluginInsertHost::audioThread_setEntryTransportContext(*access.entry, seg.insertContext);
        }
        playback_mix_helpers::renderAudioTrackPostStripToStereoScratchWithChainAccess(
            *payload.sessionSnap,
            seg.timelineStartAudible,
            seg.audibleRun,
            seg.destFrame,
            payload.stageL,
            payload.stageR,
            access,
            seg.omitClipPlaybackForTrack,
            payload.timelineEnd,
            payload.trackIndex,
            &preGainRampState_,
            payload.soloView);
    }
    if (payload.trackIndex >= 0
        && payload.trackIndex < (int)audioStripLastRenderTicks_.size())
    {
        audioStripLastRenderTicks_[(size_t)payload.trackIndex]
            = juce::Time::getHighResolutionTicks() - tStart;
    }
}

void PlaybackEngine::runInstrumentStripRenderJob(instrument_render::RenderJob& job,
                                                 const int laneIndex) noexcept
{
    auto* const payload = static_cast<InstrumentStripJobPayload*>(job.p.context);
    if (payload != nullptr && payload->engine != nullptr)
    {
        payload->engine->audioThread_runInstrumentStripPayload(*payload, laneIndex);
    }
}

void PlaybackEngine::audioThread_runInstrumentStripPayload(InstrumentStripJobPayload& payload,
                                                           const int laneIndex) noexcept
{
    using instrument_render::InstrumentRenderPool;
    if (payload.host == nullptr || payload.sessionSnap == nullptr || payload.stageL == nullptr
        || payload.stageR == nullptr || payload.numSamples <= 0 || payload.trackIndex < 0
        || payload.trackIndex >= payload.sessionSnap->getNumTracks())
    {
        return;
    }
    const std::int64_t tStart = juce::Time::getHighResolutionTicks();
    const int lane = juce::jlimit(0, InstrumentRenderPool::kNumLanes - 1, laneIndex);

    // GENERATION first — exactly the pool's default generation job (idempotent per block via
    // `audioThread_beginAudioBlock`). The collection guarantees one job per host instance, so
    // this host's processBlock never runs concurrently with itself or a second job's strip.
    payload.host->audioThread_renderGenerationStageForBlock(payload.numSamples);

    // STRIP second — the same core the serial MIX ORDER row loop runs, on this lane's exclusive
    // chain scratch / MIDI scratch, into this job's OWN stage buffer (never shared buses).
    playback_mix_helpers::AudioStripInsertAccess access;
    if (payload.chainEntry != nullptr && pluginHost_ != nullptr)
    {
        access.host = pluginHost_;
        access.entry = payload.chainEntry;
        access.chainScratch = insertLaneScratchPtrs_[lane];
        access.chainScratchCapacity = insertLaneScratchCapacity_;
        access.chainMidiScratch = &insertLaneMidi_[(size_t)lane];
        access.laneIndex = lane;
    }
    if (access.entry != nullptr)
    {
        // Whole-block transport context on this chain's OWN playhead — the same PositionInfo
        // the serial path presents instrument chains (`setInsertProcessContext(t0)`, never a
        // sub-block segmentation), regardless of which lane runs the job.
        PluginInsertHost::audioThread_setEntryTransportContext(*access.entry, payload.insertContext);
    }
    playback_mix_helpers::renderInstrumentPostStripToStereoScratchWithChainAccess(
        payload.host,
        payload.sessionSnap->getTrack(payload.trackIndex),
        payload.stageL,
        payload.stageR,
        0,
        payload.numSamples,
        access,
        // Audition instances exist only while the transport is stopped; combine is active only
        // while playing, so a combined job never mixes an audition host (PID-008 unchanged).
        nullptr,
        payload.soloView);
    if (payload.trackIndex >= 0
        && payload.trackIndex < (int)audioStripLastRenderTicks_.size())
    {
        audioStripLastRenderTicks_[(size_t)payload.trackIndex]
            = juce::Time::getHighResolutionTicks() - tStart;
    }
}

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        const double sr = device->getCurrentSampleRate();
        const int bs = device->getCurrentBufferSizeSamples();
        const int nOut = device->getActiveOutputChannels().countNumberOfSetBits();
        deviceSampleRateForDiagnostics_.store(sr, std::memory_order_relaxed);
        // Input-selection slice: capture which PHYSICAL input channels are enabled — the callback
        // and the recording push map a track's physical input assignment to positions in the
        // packed active-channel array with this mask (sparse enables shift packed positions).
        {
            const juce::BigInteger activeIn = device->getActiveInputChannels();
            std::uint64_t mask = 0;
            for (int i = 0; i < 64; ++i)
            {
                if (activeIn[i])
                {
                    mask |= (1ull << i);
                }
            }
            activeInputPhysicalMask_.store(mask, std::memory_order_release);
        }
        ensureMasterScratchCapacity(juce::jmax(bs, kOfflineMixdownBlockCapSamples));
        ensureAudioStripJobBuffersCapacity(bs);
        if (readAhead_ != nullptr)
        {
            readAhead_->prepareForDevice(sr, bs);
        }
        // Pre-gain ramps start unprimed: the first block after prepare applies each track's
        // saved pre-gain directly (no unintended fade-in at playback start).
        preGainRampState_.reset();
        if (pluginHost_ != nullptr)
        {
            pluginHost_->prepareForDevice(sr, bs, nOut);
        }
        if (experimentalPrepareAllHosts_)
        {
            experimentalPrepareAllHosts_(sr, bs);
        }
        // Events that arrived while no callback was running describe gestures nobody heard —
        // drop them (and any ownership from before the stop) before the first block.
        if (live_midi::LiveMidiInputBus* const bus = liveMidiBus_.load(std::memory_order_acquire))
        {
            bus->audioThread_discardPendingAndForgetNotes();
        }
        rebuildRoutingPlanFromSession();
    }
}

void PlaybackEngine::setTrackInputMonitoringEnabled(const TrackId trackId, const bool enabled) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    using playback_mix_helpers::LiveInputMonitorSnapshot;
    const std::shared_ptr<const LiveInputMonitorSnapshot> current
        = liveInputMonitorSnapshot_.load(std::memory_order_acquire);
    const bool currentlyOn = current != nullptr && current->contains(trackId);
    if (currentlyOn == enabled)
    {
        return;
    }
    auto next = std::make_shared<LiveInputMonitorSnapshot>();
    if (current != nullptr)
    {
        for (int i = 0; i < current->count; ++i)
        {
            const TrackId id = current->trackIds[(size_t)i];
            if (id != trackId && next->count < LiveInputMonitorSnapshot::kMaxMonitoredTracks)
            {
                next->trackIds[(size_t)next->count++] = id;
            }
        }
    }
    if (enabled)
    {
        if (next->count >= LiveInputMonitorSnapshot::kMaxMonitoredTracks)
        {
            return; // capacity guard; existing state unchanged
        }
        next->trackIds[(size_t)next->count++] = trackId;
    }
    if (next->count == 0)
    {
        liveInputMonitorSnapshot_.store(nullptr, std::memory_order_release);
        return;
    }
    liveInputMonitorSnapshot_.store(
        std::shared_ptr<const LiveInputMonitorSnapshot>(std::move(next)), std::memory_order_release);
}

void PlaybackEngine::clearAllInputMonitoring() noexcept
{
    liveInputMonitorSnapshot_.store(nullptr, std::memory_order_release);
}

bool PlaybackEngine::isTrackInputMonitoringEnabled(const TrackId trackId) const noexcept
{
    const auto snap = liveInputMonitorSnapshot_.load(std::memory_order_acquire);
    return snap != nullptr && snap->contains(trackId);
}

void PlaybackEngine::publishSoloMuteView(std::shared_ptr<const SoloMuteView> view) noexcept
{
    // An inactive view is equivalent to null — normalize so the callback's null-check is enough.
    if (view != nullptr && !view->soloActive)
    {
        view = nullptr;
    }
    soloMuteView_.store(std::move(view), std::memory_order_release);
}

std::shared_ptr<const SoloMuteView> PlaybackEngine::currentSoloMuteView() const noexcept
{
    return soloMuteView_.load(std::memory_order_acquire);
}

void PlaybackEngine::setLiveMidiInputBus(live_midi::LiveMidiInputBus* bus) noexcept
{
    liveMidiBus_.store(bus, std::memory_order_release);
}

void PlaybackEngine::audioThread_deliverLiveMidiToHost(void* /*context*/, ExperimentalInstrumentHost* host,
                                                       const int sampleOffset,
                                                       const juce::MidiMessage& message) noexcept
{
    if (host != nullptr)
    {
        host->audioThread_addMidiEventForCurrentBlock(sampleOffset, message);
    }
}

void PlaybackEngine::audioDeviceStopped()
{
    // Stop the read-ahead worker touching chains BEFORE the host releases them. No callback is
    // running here (JUCE device-lifecycle contract), so the hard ownership reset is safe.
    if (readAhead_ != nullptr)
    {
        readAhead_->releaseForDevice();
    }
    if (pluginHost_ != nullptr)
    {
        pluginHost_->releaseResources();
    }
    if (experimentalReleaseAllHosts_)
    {
        experimentalReleaseAllHosts_();
    }
}

void PlaybackEngine::audioThread_accumulateCallbackLoad(const int numSamples,
                                                        const std::int64_t startTicks) noexcept
{
    const double ticksPerSecond = (double)juce::Time::getHighResolutionTicksPerSecond();
    if (!(ticksPerSecond > 0.0))
    {
        return;
    }
    const double elapsedMs
        = (double)(juce::Time::getHighResolutionTicks() - startTicks) * 1000.0 / ticksPerSecond;
    const double sr = deviceSampleRateForDiagnostics_.load(std::memory_order_relaxed);
    const double budgetMs
        = (sr > 0.0 && numSamples > 0) ? ((double)numSamples / sr) * 1000.0 : 0.0;
    const double budgetPercent = budgetMs > 0.0 ? (elapsedMs / budgetMs) * 100.0 : 0.0;

    const std::uint64_t previousBlocks = loadWindowBlocks_.fetch_add(1, std::memory_order_relaxed);
    loadWindowSumMs_.store(loadWindowSumMs_.load(std::memory_order_relaxed) + elapsedMs,
                           std::memory_order_relaxed);
    loadWindowSumBudgetPercent_.store(
        loadWindowSumBudgetPercent_.load(std::memory_order_relaxed) + budgetPercent,
        std::memory_order_relaxed);
    if (previousBlocks == 0)
    {
        loadWindowMinMs_.store(elapsedMs, std::memory_order_relaxed);
        loadWindowMaxMs_.store(elapsedMs, std::memory_order_relaxed);
        loadWindowMaxBudgetPercent_.store(budgetPercent, std::memory_order_relaxed);
    }
    else
    {
        loadWindowMinMs_.store(
            juce::jmin(loadWindowMinMs_.load(std::memory_order_relaxed), elapsedMs),
            std::memory_order_relaxed);
        loadWindowMaxMs_.store(
            juce::jmax(loadWindowMaxMs_.load(std::memory_order_relaxed), elapsedMs),
            std::memory_order_relaxed);
        loadWindowMaxBudgetPercent_.store(
            juce::jmax(loadWindowMaxBudgetPercent_.load(std::memory_order_relaxed), budgetPercent),
            std::memory_order_relaxed);
    }
    if (budgetPercent >= 100.0)
    {
        loadWindowOverruns_.fetch_add(1, std::memory_order_relaxed);
    }
    else if (budgetPercent >= 70.0)
    {
        loadWindowNearOverruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PlaybackEngine::audioThread_foldOutputPeak(const float* const* outputChannelData,
                                                const int numOutputChannels,
                                                const int numSamples) noexcept
{
    if (outputChannelData == nullptr || numSamples <= 0)
    {
        return;
    }
    float peak = 0.0f;
    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        const float* const row = outputChannelData[ch];
        if (row == nullptr)
        {
            continue;
        }
        const juce::Range<float> r = juce::FloatVectorOperations::findMinAndMax(row, numSamples);
        if (!std::isfinite(r.getStart()) || !std::isfinite(r.getEnd()))
        {
            // NaN/inf never wins an ordinary max, so an invalid block would silently fold as "no
            // peak"; report it as +inf instead — an invalid mix is a diagnosis in itself.
            peak = std::numeric_limits<float>::infinity();
            break;
        }
        peak = juce::jmax(peak, std::abs(r.getStart()), std::abs(r.getEnd()));
    }
    float current = outputPeakHold_.load(std::memory_order_relaxed);
    while (peak > current
           && !outputPeakHold_.compare_exchange_weak(current, peak, std::memory_order_relaxed))
    {
    }
    // Stereo Out meter: the final device output (channels 0/1) — see the measuring-point note in
    // the header. A mono device folds one channel.
    if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
    {
        const level_meter::BlockStats stats = level_meter::analyzeBlock(
            outputChannelData[0], numOutputChannels >= 2 ? outputChannelData[1] : nullptr, numSamples);
        masterMeter_.audioThread_foldStats(stats);
        masterMeterDiag_.audioThread_foldStats(stats);
    }
}

// ---------------------------------------------------------------------------------------------
// Record run (message-thread side; see the header section)
// ---------------------------------------------------------------------------------------------
void PlaybackEngine::requestRecordRunStart() noexcept
{
    int expected = static_cast<int>(RecordRunState::Idle);
    if (recordRunState_.load(std::memory_order_acquire) == expected)
    {
        recordRunStartAcked_.store(false, std::memory_order_release);
    }
    (void)recordRunState_.compare_exchange_strong(expected, static_cast<int>(RecordRunState::StartRequested),
                                                  std::memory_order_acq_rel);
}

void PlaybackEngine::requestRecordRunStop() noexcept
{
    int expected = static_cast<int>(RecordRunState::Running);
    if (recordRunState_.compare_exchange_strong(expected, static_cast<int>(RecordRunState::StopRequested),
                                                std::memory_order_acq_rel))
    {
        return;
    }
    // Stop before the start was ever acknowledged (no callback ran since the start request): close
    // the run at the start position — nothing was captured.
    expected = static_cast<int>(RecordRunState::StartRequested);
    if (recordRunState_.compare_exchange_strong(expected, static_cast<int>(RecordRunState::StopRequested),
                                                std::memory_order_acq_rel))
    {
        return;
    }
}

bool PlaybackEngine::waitForRecordRunStop(const int timeoutMs, const RecordRunBoundary& fallbackBoundary) noexcept
{
    const double deadline = juce::Time::getMillisecondCounterHiRes() + (double)juce::jmax(0, timeoutMs);
    for (;;)
    {
        const auto s = recordRunState();
        if (s == RecordRunState::Stopped || s == RecordRunState::Idle)
        {
            return true;
        }
        if (juce::Time::getMillisecondCounterHiRes() >= deadline)
        {
            break;
        }
        juce::Thread::sleep(1);
    }
    // No acknowledgement: the device is stopped or lost. Close the run from here with the
    // caller's fallback boundary; a callback that resumes later finds the run already closed.
    recordRunStopMono_.store(fallbackBoundary.monoSample, std::memory_order_relaxed);
    recordRunStopTimeline_.store(fallbackBoundary.timelineSample, std::memory_order_relaxed);
    recordRunStopWrap_.store(fallbackBoundary.wrapSerial, std::memory_order_relaxed);
    int expected = static_cast<int>(RecordRunState::StopRequested);
    if (!recordRunState_.compare_exchange_strong(expected, static_cast<int>(RecordRunState::Stopped),
                                                 std::memory_order_acq_rel))
    {
        // Acknowledged in the meantime (or never started): keep the audio thread's values.
        return recordRunState() == RecordRunState::Stopped || recordRunState() == RecordRunState::Idle;
    }
    if (!recordRunStartAcked_.load(std::memory_order_acquire))
    {
        // No callback acknowledged the start either (device dead for the whole run): the start
        // fields still hold a previous run's values — close this run as empty at the fallback.
        recordRunStartMono_.store(fallbackBoundary.monoSample, std::memory_order_relaxed);
        recordRunStartTimeline_.store(fallbackBoundary.timelineSample, std::memory_order_relaxed);
        recordRunStartWrap_.store(fallbackBoundary.wrapSerial, std::memory_order_relaxed);
    }
    return false;
}

void PlaybackEngine::finishRecordRun() noexcept
{
    recordRunStartAcked_.store(false, std::memory_order_release);
    recordRunState_.store(static_cast<int>(RecordRunState::Idle), std::memory_order_release);
}

bool PlaybackEngine::audioThread_updateRecordRun(const std::int64_t monoSampleAtBlockStart,
                                                  const std::int64_t playheadAtBlockStart) noexcept
{
    // Runs BEFORE this block's intent load: the message thread requests the Playing / Stopped
    // intent first and the run transition second (both release-stores), so a callback that sees
    // the request also sees the matching intent — the start block is the first playing block,
    // the stop block does not advance the playhead. The boundary values are stored before the
    // state (release) so the message thread reads them coherently after an acquire of the state.
    const int s = recordRunState_.load(std::memory_order_acquire);
    if (s == static_cast<int>(RecordRunState::StartRequested))
    {
        recordRunStartMono_.store(monoSampleAtBlockStart, std::memory_order_relaxed);
        recordRunStartTimeline_.store(playheadAtBlockStart, std::memory_order_relaxed);
        recordRunStartWrap_.store(transport_.audioThread_relaxedLoadWrapPassCount(), std::memory_order_relaxed);
        recordRunStartAcked_.store(true, std::memory_order_release);
        recordRunState_.store(static_cast<int>(RecordRunState::Running), std::memory_order_release);
        return true;
    }
    if (s == static_cast<int>(RecordRunState::StopRequested))
    {
        const std::uint32_t wrap = transport_.audioThread_relaxedLoadWrapPassCount();
        if (!recordRunStartAcked_.load(std::memory_order_acquire))
        {
            // Stopped before any block captured: an empty run, start = stop.
            recordRunStartMono_.store(monoSampleAtBlockStart, std::memory_order_relaxed);
            recordRunStartTimeline_.store(playheadAtBlockStart, std::memory_order_relaxed);
            recordRunStartWrap_.store(wrap, std::memory_order_relaxed);
            recordRunStartAcked_.store(true, std::memory_order_release);
        }
        recordRunStopMono_.store(monoSampleAtBlockStart, std::memory_order_relaxed);
        recordRunStopTimeline_.store(playheadAtBlockStart, std::memory_order_relaxed);
        recordRunStopWrap_.store(wrap, std::memory_order_relaxed);
        recordRunState_.store(static_cast<int>(RecordRunState::Stopped), std::memory_order_release);
        return false; // the boundary block captures nothing
    }
    return s == static_cast<int>(RecordRunState::Running);
}

PlaybackEngine::RecordRunBoundary PlaybackEngine::recordRunStartBoundary() const noexcept
{
    RecordRunBoundary b;
    const auto s = recordRunState();
    b.valid = (s == RecordRunState::Running || s == RecordRunState::StopRequested || s == RecordRunState::Stopped)
              && recordRunStartAcked_.load(std::memory_order_acquire);
    b.monoSample = recordRunStartMono_.load(std::memory_order_relaxed);
    b.timelineSample = recordRunStartTimeline_.load(std::memory_order_relaxed);
    b.wrapSerial = recordRunStartWrap_.load(std::memory_order_relaxed);
    return b;
}

PlaybackEngine::RecordRunBoundary PlaybackEngine::recordRunStopBoundary() const noexcept
{
    RecordRunBoundary b;
    b.valid = recordRunState() == RecordRunState::Stopped;
    b.monoSample = recordRunStopMono_.load(std::memory_order_relaxed);
    b.timelineSample = recordRunStopTimeline_.load(std::memory_order_relaxed);
    b.wrapSerial = recordRunStopWrap_.load(std::memory_order_relaxed);
    return b;
}

void PlaybackEngine::setMeteredTrackForUi(const TrackId trackId) noexcept
{
    meteredTrackId_.store(static_cast<std::int64_t>(trackId), std::memory_order_relaxed);
    trackMeter_.reset();
    trackMeterDiag_.reset();
}

PlaybackEngine::AudioCallbackLoadSnapshot PlaybackEngine::snapshotAudioCallbackLoadAndReset() noexcept
{
    AudioCallbackLoadSnapshot s;
    s.blocks = loadWindowBlocks_.exchange(0, std::memory_order_relaxed);
    const double sumMs = loadWindowSumMs_.exchange(0.0, std::memory_order_relaxed);
    const double sumBudget = loadWindowSumBudgetPercent_.exchange(0.0, std::memory_order_relaxed);
    s.minMs = loadWindowMinMs_.exchange(0.0, std::memory_order_relaxed);
    s.maxMs = loadWindowMaxMs_.exchange(0.0, std::memory_order_relaxed);
    s.maxBudgetPercent = loadWindowMaxBudgetPercent_.exchange(0.0, std::memory_order_relaxed);
    s.nearOverruns = loadWindowNearOverruns_.exchange(0, std::memory_order_relaxed);
    s.overruns = loadWindowOverruns_.exchange(0, std::memory_order_relaxed);
    s.lastBlockSamples = audioCallbackLastBlockSamples_.load(std::memory_order_relaxed);
    s.sampleRate = deviceSampleRateForDiagnostics_.load(std::memory_order_relaxed);
    if (s.blocks > 0)
    {
        s.meanMs = sumMs / (double)s.blocks;
        s.meanBudgetPercent = sumBudget / (double)s.blocks;
    }
    return s;
}

void PlaybackEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                     int numInputChannels,
                                                     float* const* outputChannelData,
                                                     int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(context);

    // [Audio thread] Dekker-style pairing with `beginOfflineRenderGate`: publish "callback in flight"
    // *before* reading the offline gate (both seq_cst). Either this callback sees the gate and goes
    // silent, or the gating thread sees this flag and waits for the RAII clear below — so an offline
    // render can never overlap a callback that touches plugin hosts / scratch buffers.
    audioCallbackInProcessingSection_.store(true, std::memory_order_seq_cst);
    struct AudioCallbackSectionClear
    {
        std::atomic<bool>& flag_;
        std::atomic<int>& phase_;
        ~AudioCallbackSectionClear() noexcept
        {
            phase_.store(static_cast<int>(AudioCallbackPhase::Idle), std::memory_order_relaxed);
            flag_.store(false, std::memory_order_seq_cst);
        }
    };
    const AudioCallbackSectionClear callbackSectionClear { audioCallbackInProcessingSection_,
                                                           audioCallbackPhase_ };

    // Load diagnostics: two clock reads per block, folded into relaxed counters on the way out
    // (covers the early-return offline-gate path as well). Never read for synchronization.
    // Opt-in profiler (`--stability-perf-profile`): one relaxed load when off; when on, the
    // block's start is registered here and its category / phase breakdown folded on exit.
    audio_profiler::AudioThreadProfiler& profiler = audio_profiler::AudioThreadProfiler::get();
    const bool prof = profiler.audioThread_enabled();
    struct AudioCallbackLoadScope
    {
        PlaybackEngine& engine_;
        const int blockSamples_;
        const std::int64_t startTicks_;
        const bool profiling_;
        ~AudioCallbackLoadScope() noexcept
        {
            engine_.audioThread_accumulateCallbackLoad(blockSamples_, startTicks_);
            if (profiling_)
            {
                audio_profiler::AudioThreadProfiler::get().audioThread_endBlock(
                    startTicks_, blockSamples_,
                    engine_.deviceSampleRateForDiagnostics_.load(std::memory_order_relaxed));
            }
        }
    };
    const AudioCallbackLoadScope loadScope { *this, numSamples, juce::Time::getHighResolutionTicks(), prof };
    if (prof)
    {
        profiler.audioThread_beginBlock(loadScope.startTicks_);
    }

    // Output-peak diagnostics: runs on every return path (including the gate-silence path, where
    // the cleared buffers correctly fold a peak of 0). Relaxed atomics; never for synchronization.
    struct AudioCallbackOutputPeakScope
    {
        PlaybackEngine& engine_;
        const float* const* outputs_;
        const int numOutputs_;
        const int numSamples_;
        ~AudioCallbackOutputPeakScope() noexcept
        {
            engine_.audioThread_foldOutputPeak(outputs_, numOutputs_, numSamples_);
        }
    };
    const AudioCallbackOutputPeakScope outputPeakScope { *this,
                                                         outputChannelData,
                                                         numOutputChannels,
                                                         numSamples };

    // Stability C2B diagnostics: coarse phase marker (relaxed; never used for synchronization).
    const auto setCallbackPhase = [this](const AudioCallbackPhase p) noexcept {
        audioCallbackPhase_.store(static_cast<int>(p), std::memory_order_relaxed);
    };
    setCallbackPhase(AudioCallbackPhase::Begin);
    audioCallbackLastBlockSamples_.store(numSamples, std::memory_order_relaxed);
    audioCallbackEnterCount_.fetch_add(1, std::memory_order_relaxed);
    // Monotone device clock (live-MIDI time base, record-run boundaries): this block's first
    // sample, advanced on every return path below because it is advanced here, unconditionally.
    const std::int64_t monoSampleAtBlockStart = monoSampleClock_;
    monoSampleClock_ += juce::jmax(0, numSamples);
    monoSampleClockPublished_.store(monoSampleClock_, std::memory_order_relaxed);

    // [Audio thread] Input-selection slice: map the packed active-channel input array so both the
    // recording push and the monitoring pass can resolve PHYSICAL input assignments per block.
    // Positions in `inputChannelData` are packed enabled channels only — with sparse enables the
    // packed position of physical channel N is the popcount of enabled channels below N.
    const std::uint64_t activeInputMask = activeInputPhysicalMask_.load(std::memory_order_relaxed);
    const auto activeInputPointerForPhysical
        = [activeInputMask, inputChannelData, numInputChannels](const int physical) noexcept -> const float*
    {
        if (inputChannelData == nullptr)
        {
            return nullptr;
        }
        const int pos
            = playback_mix_helpers::packedActiveInputPositionForPhysical(activeInputMask, physical);
        return pos >= 0 && pos < numInputChannels ? inputChannelData[pos] : nullptr;
    };

    const int deviceBlockSizeInFrames = numSamples;
    setCallbackPhase(AudioCallbackPhase::TransportBeginBlock);
    transport_.audioThread_beginBlock();

    // Record run handshake (see the header): acknowledge a pending start / stop at THIS block's
    // first sample — one boundary (mono clock, transport position, wrap serial) shared by the
    // audio recorder and the MIDI take. Must precede the intent load below so the request's
    // matching intent is visible in the same block.
    const std::int64_t t0 = transport_.audioThread_loadPlayhead();
    const bool recordRunCapturing = audioThread_updateRecordRun(monoSampleAtBlockStart, t0);

    // [Audio thread] Route the take's SELECTED physical input channel(s) to the recorder SPSC path
    // only while the run captures (between the acknowledged start and stop boundaries); does not
    // access Session (the coordinator resolved the armed track's assignment to concrete physical
    // channels at record start). This is the RAW pre-strip capture point: pre-gain, inserts,
    // fader, pan and Monitor never affect recorded samples.
    if (recorder_ != nullptr && recordRunCapturing && recorder_->isRecording() && numSamples > 0)
    {
        setCallbackPhase(AudioCallbackPhase::RecorderPush);
        const float* const recInA
            = activeInputPointerForPhysical(recorder_->getRecordingInputPhysicalChannelA());
        const float* const recInB
            = recorder_->getRecordingNumChannels() == 2
                  ? activeInputPointerForPhysical(recorder_->getRecordingInputPhysicalChannelB())
                  : nullptr;
        recorder_->pushInputBlock(recInA, recInB, numSamples);
    }

    if (offlineRenderGateDepth_.load(std::memory_order_seq_cst) > 0)
    {
        setCallbackPhase(AudioCallbackPhase::OfflineGateSilence);
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (float* row = outputChannelData[ch])
            {
                juce::FloatVectorOperations::clear(row, numSamples);
            }
        }
        // Live MIDI must never leak into the offline render: drop whatever the keyboard sent
        // while the export owns the hosts (keeps the device rings from overflowing).
        if (live_midi::LiveMidiInputBus* const bus = liveMidiBus_.load(std::memory_order_acquire))
        {
            bus->audioThread_discardPendingAndForgetNotes();
        }
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    setCallbackPhase(AudioCallbackPhase::LoadSnapshot);
    const std::shared_ptr<const SessionSnapshot> sessionSnap = session_.loadSessionSnapshotForAudioThread();
    // Concurrent meters: one acquire-load of the slot map for this block (the fold points below
    // only compare ids against the cached view).
    trackMeterBank_.audioThread_beginBlock();
    /// [Audio thread] Same publish discipline as Session: acquire-load retains a const view for this block only.
    // Live input monitoring (Monitor button): one acquire-loaded immutable view per block. Used to
    // (a) suppress monitored tracks' clip playback in the segment renderers and (b) drive the
    // dedicated monitoring pass in `mixInstrumentsAndFinalizeMaster` (runs stopped or playing).
    const std::shared_ptr<const playback_mix_helpers::LiveInputMonitorSnapshot> monitorSnap
        = liveInputMonitorSnapshot_.load(std::memory_order_acquire);
    const playback_mix_helpers::LiveInputMonitorSnapshot* const monitorPtr
        = (monitorSnap != nullptr && monitorSnap->count > 0) ? monitorSnap.get() : nullptr;
    // Solo (derived listening view): ONE acquire per callback — every strip and every MIDI gate
    // in this block sees the same consistent decision (block-boundary publication, spec §6).
    const std::shared_ptr<const SoloMuteView> soloViewSnap
        = soloMuteView_.load(std::memory_order_acquire);
    const SoloMuteView* const soloView
        = (soloViewSnap != nullptr && soloViewSnap->soloActive) ? soloViewSnap.get() : nullptr;
    const bool allowInstrumentProcessing
        = !instrumentProcessingSuspended_.load(std::memory_order_acquire);

    struct AudioInstrumentSectionScope
    {
        std::atomic<bool>& insideFlag_;
        const bool active_;
        AudioInstrumentSectionScope(std::atomic<bool>& insideFlag, const bool active) noexcept
            : insideFlag_(insideFlag)
            , active_(active)
        {
            if (active_)
            {
                insideFlag_.store(true, std::memory_order_release);
            }
        }
        ~AudioInstrumentSectionScope() noexcept
        {
            if (active_)
            {
                insideFlag_.store(false, std::memory_order_release);
            }
        }
    };
    const AudioInstrumentSectionScope instrumentSectionScope { audioInsideInstrumentSection_,
                                                               allowInstrumentProcessing };

    const std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> instrumentSnap
        = allowInstrumentProcessing
              ? experimentalInstrumentPlaybackSnapshot_.load(std::memory_order_acquire)
              : nullptr;

    // Per-block RT MIDI delivery uses `audioCallbackBlockSamples_` (see `ExperimentalInstrumentHost`). The
    // snapshot is the engine's source of truth for which host(s) are driven this block — call `beginAudioBlock`
    // here before the optional map/staging lambda so message-thread map drift cannot skip the active host.
    if (allowInstrumentProcessing)
    {
        setCallbackPhase(AudioCallbackPhase::InstrumentBeginBlock);
        const std::int64_t tPhase = prof ? audio_profiler::AudioThreadProfiler::ticks() : 0;
        invokeExperimentalInstrumentBeginBlocks(instrumentSnap.get(), deviceBlockSizeInFrames);
        if (prof)
        {
            profiler.audioThread_addPhase(audio_profiler::Phase::InstrumentBeginBlock, tPhase);
        }
    }

    const PlaybackIntent playbackIntent = transport_.audioThread_loadIntent();
    const bool cycleOn = transport_.audioThread_loadCycleEnabled();
    const std::int64_t locL = sessionSnap != nullptr ? sessionSnap->getLeftLocatorSamples() : 0;
    const std::int64_t locR = sessionSnap != nullptr ? sessionSnap->getRightLocatorSamples() : 0;
    const bool validCycle = cycleOn && locR > locL && locR > 0;

    // [Audio thread] Live MIDI input: deliver this block's keyboard events into the hosts of the
    // snapshot just begun (sample offsets from device timestamps) and forward the armed rows'
    // events to the take capture, stamped with this block's transport position. Runs stopped or
    // playing — monitoring does not depend on the transport. With instrument processing
    // suspended the hosts are not driven, so pending input is dropped instead of piling up.
    if (live_midi::LiveMidiInputBus* const bus = liveMidiBus_.load(std::memory_order_acquire))
    {
        const std::int64_t tPhase = prof ? audio_profiler::AudioThreadProfiler::ticks() : 0;
        struct PhaseEnd
        {
            bool on;
            std::int64_t t0;
            ~PhaseEnd() noexcept
            {
                if (on)
                {
                    audio_profiler::AudioThreadProfiler::get().audioThread_addPhase(
                        audio_profiler::Phase::LiveMidi, t0);
                }
            }
        } const phaseEnd { prof, tPhase };
        if (allowInstrumentProcessing && instrumentSnap != nullptr)
        {
            live_midi::BlockContext ctx;
            ctx.session = sessionSnap.get();
            ctx.instruments = instrumentSnap.get();
            ctx.numSamples = numSamples;
            ctx.sampleRate = deviceSampleRateForDiagnostics_.load(std::memory_order_relaxed);
            ctx.nowMs = juce::Time::getMillisecondCounterHiRes();
            ctx.playheadAtBlockStart = t0;
            ctx.transportPlaying = playbackIntent == PlaybackIntent::Playing;
            ctx.recordPlacementOffsetSamples = liveMidiRecordPlacementOffsetSamples_.load(std::memory_order_relaxed);
            ctx.monoSampleAtBlockStart = monoSampleAtBlockStart;
            bus->audioThread_dispatch(ctx, &PlaybackEngine::audioThread_deliverLiveMidiToHost, nullptr);
        }
        else
        {
            bus->audioThread_discardPendingAndForgetNotes();
        }
    }

    // Every insert chain reads the same host-owned playhead during its synchronous processBlock.
    // Keep that context at the exact timeline segment for clip rendering, and at this callback's
    // transport position for full-block monitoring, instrument and bus processing.
    // Value builder: the serial paths publish it to every chain's playhead via the setter below;
    // the Stage A1 segment collection stores a COPY in each segment descriptor so a render-pool
    // job can set its own chain's playhead without touching shared mutable transport state.
    const auto makeInsertProcessContext = [&](const std::int64_t segmentStartSample) noexcept {
        PluginProcessTransportContext insertContext;
        insertContext.timelineSample = segmentStartSample;
        insertContext.sampleRate = deviceSampleRateForDiagnostics_.load(std::memory_order_relaxed);
        if (sessionSnap != nullptr)
        {
            const ProjectMusicalTime musicalTime = sessionSnap->getProjectMusicalTime();
            insertContext.bpm = musicalTime.bpm;
            insertContext.timeSignatureNumerator = musicalTime.numerator;
            insertContext.timeSignatureDenominator = musicalTime.denominator;
        }
        insertContext.isPlaying = playbackIntent == PlaybackIntent::Playing;
        insertContext.isRecording = recorder_ != nullptr && recorder_->isRecording();
        insertContext.isLooping = validCycle;
        insertContext.loopStartSample = locL;
        insertContext.loopEndSample = locR;
        return insertContext;
    };
    const auto setInsertProcessContext = [&](const std::int64_t segmentStartSample) noexcept {
        if (pluginHost_ == nullptr || sessionSnap == nullptr)
        {
            return;
        }
        if (readAhead_ != nullptr && readAheadExcludedCount_ > 0)
        {
            // Owned read-ahead rows: their entry playheads have exactly ONE writer (the worker);
            // the serial refresh skips them (docs/READAHEAD_PROTOTYPE.md §8).
            pluginHost_->audioThread_setProcessTransportContextExcept(
                makeInsertProcessContext(segmentStartSample),
                readAheadExcludedIds_.data(), readAheadExcludedCount_);
            return;
        }
        pluginHost_->audioThread_setProcessTransportContext(makeInsertProcessContext(segmentStartSample));
    };

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
            // MIDI sources first: emit their pending note-offs into their last destination and
            // clear their queues, then the destination flush below finishes with allNotesOff.
            for (const auto& src : instrumentSnap->midiSources)
            {
                if (src.midiController == nullptr)
                {
                    continue;
                }
                const TrackId lastDest = src.midiController->audioThread_getLastRoutedDestTrackId();
                const ExperimentalInstrumentPlaybackEntry* const destEntry =
                    (lastDest != kInvalidTrackId)
                        ? playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap,
                                                                                        lastDest)
                        : nullptr;
                if (destEntry != nullptr && destEntry->host != nullptr)
                {
                    src.midiController->audioThread_flushPendingTransportOffsInto(
                        *destEntry->host, 0, deviceBlockSizeInFrames);
                }
                else
                {
                    src.midiController->audioThread_dropPendingTransportOffs();
                }
            }
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
                                                                   ? playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(
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
    /// One-shot per transport PLAY edge (debug): every `TrackKind::Instrument` session row resolves
    /// via `ExperimentalInstrumentPlaybackSnapshot` **by TrackId**.
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
                e = playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap, tr.getId());
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

    setCallbackPhase(AudioCallbackPhase::MixPrep);
    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        if (float* row = outputChannelData[ch])
        {
            juce::FloatVectorOperations::clear(row, numSamples);
        }
    }

    const std::shared_ptr<const RoutingPlan> routingPlan
        = routingPlan_.load(std::memory_order_acquire);
    const RoutingPlan* const rp = routingPlan.get();

    const Track* masterTrackPtr = nullptr;
    float* mixBusL = nullptr;
    float* mixBusR = nullptr;
    if (sessionSnap != nullptr)
    {
        masterTrackPtr = playback_mix_helpers::findCanonicalMasterTrack(*sessionSnap);
    }
    if (rp != nullptr && !rp->busScratchL.empty() && rp->masterBusIndex < rp->busScratchL.size())
    {
        mixBusL = rp->busScratchL[rp->masterBusIndex];
        mixBusR = rp->busScratchR[rp->masterBusIndex];
        for (size_t bi = 0; bi < rp->busScratchL.size(); ++bi)
        {
            if (rp->busScratchL[bi] != nullptr)
            {
                juce::FloatVectorOperations::clear(rp->busScratchL[bi], numSamples);
            }
            if (rp->busScratchR[bi] != nullptr)
            {
                juce::FloatVectorOperations::clear(rp->busScratchR[bi], numSamples);
            }
        }
    }
    else if (sessionSnap != nullptr && masterScratchCapacity_ >= numSamples
             && masterScratchPtrs_[0] != nullptr && masterScratchPtrs_[1] != nullptr
             && masterTrackPtr != nullptr)
    {
        juce::FloatVectorOperations::clear(masterScratchPtrs_[0], numSamples);
        juce::FloatVectorOperations::clear(masterScratchPtrs_[1], numSamples);
        mixBusL = masterScratchPtrs_[0];
        mixBusR = masterScratchPtrs_[1];
    }

    float* const mixBusPtrs[2] = { mixBusL, mixBusR };
    float* const* mixSumTarget
        = (mixBusL != nullptr && mixBusR != nullptr) ? mixBusPtrs : outputChannelData;

    // ---- Stage A1: decide whether THIS block collects audio-row strip jobs ----
    // ONE acquire-load of the insert map per block. The shared_ptr lives on this stack frame for
    // the remainder of the callback, so every entry pointer a job payload carries stays valid
    // until well after the jobs have joined (publish-before-destroy, plan §3.2).
    const std::shared_ptr<const PluginAudioThreadMap> insertMapForBlock
        = pluginHost_ != nullptr ? pluginHost_->audioThread_acquireMapForBlock() : nullptr;
    audioStripPayloadCount_ = 0;
    audioStripCollectActive_ = false;
    instrumentStripPayloadCount_ = 0;
    // ---- Experimental read-ahead: ALL ownership transitions for this block happen HERE, so the
    // owned set is block-stable for the pre-count, the collect, the serial strip path, the
    // monitor pass and the global transport-context refresh (docs/READAHEAD_PROTOTYPE.md).
    readAheadExcludedCount_ = 0;
    readAheadSegCount_ = 0;
    if (readAhead_ != nullptr)
    {
        readahead::ReadAheadRenderer::BlockBeginInfo rbi;
        rbi.t0 = t0;
        rbi.numSamples = numSamples;
        rbi.playing = playbackIntent == PlaybackIntent::Playing;
        rbi.cycleActive = cycleOn && validCycle;
        rbi.locLeft = locL;
        rbi.locRight = locR;
        // Immediate-handover / pre-emptive-drain rows (model doc §5): the active capture row
        // and the record-armed row. Both ids are atomics the callback already reads.
        rbi.recordingTrackId = (recorder_ != nullptr
                                && (recordRunCapturing || recorder_->isRecording()))
                                   ? recorder_->getRecordingTrackId()
                                   : kInvalidTrackId;
        rbi.armedTrackId = recorder_ != nullptr ? recorder_->getArmedTrackId() : kInvalidTrackId;
        rbi.playbackShift = playbackOffsetSamples_.load(std::memory_order_acquire);
        rbi.arrangementEnd = sessionSnap != nullptr ? sessionSnap->getArrangementExtentSamples() : 0;
        // "Usable" must guarantee a consume-capable render branch: the plan-less and
        // stage-unprepared fallback branches have no owned-row skip, so ownership is released
        // (discard) before any block that could reach them renders anything.
        rbi.planUsable = rp != nullptr && !rp->sourceSteps.empty() && sessionSnap != nullptr
                         && postStripStagePtrs_[0] != nullptr && postStripStagePtrs_[1] != nullptr
                         && postStripStageCapacity_ >= numSamples;
        rbi.monitorView = monitorPtr;
        readAhead_->audioThread_beginBlock(rbi);
        if (sessionSnap != nullptr)
        {
            readAhead_->audioThread_publishContextTemplate(makeInsertProcessContext(t0));
        }
    }
    // ---- Stage A2: decide whether THIS block combines instrument generation + strip jobs ----
    // Needs no routing plan (instrument rows fan with or without one) — only the pool, prepared
    // job buffers, and a playing transport (stopped blocks keep today's generation-only dispatch
    // + serial strips: audition instances and live-MIDI monitoring while stopped are unchanged,
    // with no added block latency). Unprepared buffers = generation-only jobs, serial strips.
    const bool instrumentStripCombineActive
        = playbackIntent == PlaybackIntent::Playing && instrumentRenderPool_ != nullptr
          && numSamples > 0 && sessionSnap != nullptr && instrumentSnap != nullptr
          && !instrumentSnap->entries.empty() && instrumentStripStageCapacity_ >= numSamples
          && insertLaneScratchCapacity_ >= numSamples;
    if (rp != nullptr && sessionSnap != nullptr && !rp->sourceSteps.empty()
        && playbackIntent == PlaybackIntent::Playing && instrumentRenderPool_ != nullptr
        && numSamples > 0 && audioStripStageCapacity_ >= numSamples
        && insertLaneScratchCapacity_ >= numSamples)
    {
        // Pre-count with EXACTLY the per-step gates the collection in `renderRun` applies. All of
        // them are block-constant (plan fields, snapshot row kind, monitor view), so the count
        // decides capacity for every segment of this block.
        int eligible = 0;
        for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
        {
            if (step.destBusIndex < 0
                || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
            {
                continue;
            }
            if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
            {
                continue;
            }
            const Track& srcTr = sessionSnap->getTrack(step.trackIndex);
            if (srcTr.getKind() != TrackKind::Audio)
            {
                continue;
            }
            // Read-ahead-owned rows consume from their rings instead of taking a payload slot —
            // checked BEFORE the monitor gate, because a monitored row that is still DRAINING
            // keeps consuming its queue until the release (IDENTICAL gate order in the renderRun
            // collection, the serial strip path and the sum loop).
            if (readAhead_ != nullptr && readAhead_->audioThread_isOwnedForRender(srcTr.getId()))
            {
                continue;
            }
            if (monitorPtr != nullptr && monitorPtr->contains(srcTr.getId()))
            {
                continue;
            }
            ++eligible;
        }
        // Over job capacity: the WHOLE block renders on the serial strip path in `renderRun`
        // (same strip core on the callback lane) — rows are never dropped or split across modes.
        audioStripCollectActive_ = eligible > 0 && eligible <= kMaxAudioStripJobs;
    }
    // ---- Experimental read-ahead: adoption offers (rows become Scheduled, render live THIS
    // block through the normal A1 job, and are consumed from the next block on). Offered only on
    // collect-active blocks with the exact A1 eligibility gates; the renderer applies the model
    // gates (linear transport, headroom, capacity) itself.
    if (readAhead_ != nullptr && audioStripCollectActive_ && rp != nullptr && sessionSnap != nullptr)
    {
        for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
        {
            if (step.destBusIndex < 0
                || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
            {
                continue;
            }
            if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
            {
                continue;
            }
            const Track& srcTr = sessionSnap->getTrack(step.trackIndex);
            if (srcTr.getKind() != TrackKind::Audio)
            {
                continue;
            }
            if (monitorPtr != nullptr && monitorPtr->contains(srcTr.getId()))
            {
                continue;
            }
            readAhead_->audioThread_offerAdoption(srcTr.getId(), step.trackIndex);
        }
    }
    if (readAhead_ != nullptr)
    {
        readAheadExcludedCount_ = readAhead_->audioThread_exportExcludedTrackIds(readAheadExcludedIds_.data());
    }

    if (countIn_ != nullptr)
    {
        setCallbackPhase(AudioCallbackPhase::CountIn);
        countIn_->audioThread_mixInto(mixSumTarget, numOutputChannels, numSamples);
    }

    const auto finalizeRoutingToDevice = [&]() noexcept
    {
        // Buses process a complete device block after source accumulation, so their VST3 context
        // is the callback's transport position rather than the last clip sub-segment.
        setInsertProcessContext(t0);
        setCallbackPhase(AudioCallbackPhase::FinalizeRouting);
#if !defined(NDEBUG)
        if constexpr (shortcut_diagnostics::kShowMasterRoutingDiag)
        {
            static bool loggedOnce = false;
            if (!loggedOnce)
            {
                loggedOnce = true;
                const bool directSumFallback = (mixSumTarget == outputChannelData);
                juce::Logger::writeToLog(
                    juce::String("[MasterRoutingDiag] live block: canonicalMasterId=")
                    + (masterTrackPtr != nullptr
                           ? juce::String((juce::int64)masterTrackPtr->getId())
                           : juce::String("-1"))
                    + " name="
                    + (masterTrackPtr != nullptr ? masterTrackPtr->getName() : juce::String("(none)"))
                    + " directSumFallback="
                    + juce::String(directSumFallback ? "yes" : "no"));
            }
        }
#endif
        if (rp != nullptr && sessionSnap != nullptr && !rp->busSteps.empty()
            && postStripStagePtrs_[0] != nullptr && postStripStagePtrs_[1] != nullptr
            && postStripStageCapacity_ >= numSamples)
        {
            setCallbackPhase(AudioCallbackPhase::FinalizeStagedBusLoop);
            float* const stageStereo[2] = { postStripStagePtrs_[0], postStripStagePtrs_[1] };
            for (const RoutingPlan::BusStep& step : rp->busSteps)
            {
                if (step.sourceBusIndex < 0
                    || step.sourceBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                // C2B ROOT-CAUSE FIX: `RoutingPlan` and `SessionSnapshot` are published as two
                // independent atomics, so after a structural edit (track delete / undo / redo) this
                // callback can observe a *fresh* snapshot with a *stale* plan whose `trackIndex` is
                // out of range or points at a re-purposed row. `getTrack` uses `.at()` — an OOB
                // index throws, and a throw inside this noexcept lambda terminates (Debug: wedged
                // audio thread inside the CRT assert dialog; Release: process abort). Skip the step
                // instead: one block renders without this bus, then the rebuilt plan takes over.
                if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
                {
                    continue;
                }
                const Track& busTr = sessionSnap->getTrack(step.trackIndex);
                if (busTr.getKind() != TrackKind::Group && busTr.getKind() != TrackKind::Master)
                {
                    continue;
                }
                float* const busStereo[2] = { rp->busScratchL[(size_t)step.sourceBusIndex],
                                              rp->busScratchR[(size_t)step.sourceBusIndex] };
                playback_mix_helpers::applyBusPostChannelStripFromInputToStage(busTr,
                                                                               busStereo,
                                                                               postStripStagePtrs_[0],
                                                                               postStripStagePtrs_[1],
                                                                               0,
                                                                               numSamples,
                                                                               pluginHost_,
                                                                               soloView);
                // Group rows meter their bus strip output here; the Master row's meter is the
                // device-output Stereo Out meter (folded once per callback), never duplicated.
                if (busTr.getKind() == TrackKind::Group)
                {
                    audioThread_foldTrackMeterIfMetered(
                        busTr.getId(), postStripStagePtrs_[0], postStripStagePtrs_[1], numSamples);
                }
                if (step.destBusIndex < 0)
                {
                    playback_mix_helpers::addPostStripStageToDeviceOutputs(stageStereo,
                                                                           0,
                                                                           numSamples,
                                                                           numOutputChannels,
                                                                           outputChannelData);
                }
                else
                {
                    playback_mix_helpers::fanPostStripStageToDryAndSends(postStripStagePtrs_[0],
                                                                         postStripStagePtrs_[1],
                                                                         0,
                                                                         numSamples,
                                                                         step.destBusIndex,
                                                                         step.sends,
                                                                         *rp);
                }
            }
            return;
        }
        if (rp != nullptr && sessionSnap != nullptr && !rp->busSteps.empty())
        {
            setCallbackPhase(AudioCallbackPhase::FinalizeLegacyBusLoop);
            for (const RoutingPlan::BusStep& step : rp->busSteps)
            {
                if (step.sourceBusIndex < 0
                    || step.sourceBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                // C2B: stale-plan guard (see staged loop above).
                if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
                {
                    continue;
                }
                const Track& busTr = sessionSnap->getTrack(step.trackIndex);
                if (busTr.getKind() != TrackKind::Group && busTr.getKind() != TrackKind::Master)
                {
                    continue;
                }
                float* const busStereo[2] = { rp->busScratchL[(size_t)step.sourceBusIndex],
                                              rp->busScratchR[(size_t)step.sourceBusIndex] };
                if (step.destBusIndex < 0)
                {
                    playback_mix_helpers::processBusChannelStripToOutputs(busTr,
                                                                          busStereo,
                                                                          0,
                                                                          numSamples,
                                                                          numOutputChannels,
                                                                          outputChannelData,
                                                                          pluginHost_,
                                                                          soloView);
                }
                else if (step.destBusIndex < static_cast<int>(rp->busScratchL.size()))
                {
                    float* const destStereo[2] = { rp->busScratchL[(size_t)step.destBusIndex],
                                                   rp->busScratchR[(size_t)step.destBusIndex] };
                    playback_mix_helpers::processBusChannelStripToOutputs(
                        busTr, busStereo, 0, numSamples, 2, destStereo, pluginHost_, soloView);
                }
            }
            return;
        }
        if (masterTrackPtr != nullptr && mixSumTarget != outputChannelData)
        {
            setCallbackPhase(AudioCallbackPhase::FinalizeMasterFallback);
            playback_mix_helpers::processBusChannelStripToOutputs(*masterTrackPtr,
                                                                  mixSumTarget,
                                                                  0,
                                                                  numSamples,
                                                                  numOutputChannels,
                                                                  outputChannelData,
                                                                  pluginHost_,
                                                                  soloView);
        }
    };

    // ROW-JOB COLLECTION (shared): fill `instrumentRenderJobs_` from index 0 with one job per
    // live instrument host this block will mix — the rows the MIX ORDER loop below processes
    // (Off rows skipped, muted rows still process — same gate as the strip), plus the audition
    // host while stopped. Proxy-backed hosts stay on the callback thread. Used by BOTH dispatch
    // sites: the Stage A1/A2 combined batch (audio strip jobs appended after these) and the
    // legacy dispatch inside `mixKeyedInstrumentLanesIntoOutputsIfAny` (collect-inactive blocks).
    //
    // `combineStrips` (Stage A2, combined-batch dispatch only): a row whose host, payload slot
    // and job slot are all available gets a COMBINED job (generation + instrument strip into the
    // row's own stage buffer, consumed by the MIX ORDER loop without a second strip run).
    // Explicitly SERIAL-strip rows (generation-only job or no job, strip in the row loop):
    //   * proxy-selected hosts — no job at all (the add step mixes the proxy copy on the
    //     callback thread and must never start a Primary render from a job),
    //   * additional rows resolving to an already-claimed host instance — one generation per
    //     instance, never two jobs touching one host (multi-source rows fan serially),
    //   * audition hosts — exist only while stopped, when combine is never active,
    //   * payload/job-table overflow — generation-only job (or full serial self-render).
    const auto collectInstrumentRowJobs = [&](const bool combineStrips) noexcept -> int {
        using instrument_render::InstrumentRenderPool;
        if (instrumentRenderPool_ == nullptr || numSamples <= 0 || sessionSnap == nullptr
            || instrumentSnap == nullptr || instrumentSnap->entries.empty())
        {
            return 0;
        }
        const bool stripHasScratch = postStripStagePtrs_[0] != nullptr && postStripStagePtrs_[1] != nullptr
                                     && postStripStageCapacity_ >= numSamples;
        const bool auditionActive = stripHasScratch && playbackIntent != PlaybackIntent::Playing;
        // Instrument chains always process the host's complete block at the callback's position
        // (same PositionInfo as the serial `setInsertProcessContext(t0)` — never segmented).
        const PluginProcessTransportContext blockContext = makeInsertProcessContext(t0);
        int jobCount = 0;
        const auto addJob = [&](ExperimentalInstrumentHost* const h, const int combineTrackIndex) noexcept {
            if (h == nullptr || jobCount >= InstrumentRenderPool::kMaxJobs || h->audioThread_isProxySelectedNow())
            {
                return;
            }
            for (int k = 0; k < jobCount; ++k)
            {
                if (instrumentRenderJobs_[(size_t)k].p.host == h)
                {
                    return; // never two jobs for one instance
                }
            }
            auto& j = instrumentRenderJobs_[(size_t)jobCount++];
            j.p = instrument_render::RenderJob::Payload{};
            j.p.host = h;
            j.p.numSamples = numSamples;
            j.p.lastRenderTicks = h->audioThread_lastRenderTicksRelaxed();
            if (combineStrips && combineTrackIndex >= 0
                && instrumentStripPayloadCount_ < kMaxInstrumentStripJobs
                && instrumentStripStageCapacity_ >= numSamples)
            {
                InstrumentStripJobPayload& p
                    = instrumentStripPayloads_[(size_t)instrumentStripPayloadCount_];
                p = InstrumentStripJobPayload{};
                p.engine = this;
                p.sessionSnap = sessionSnap.get();
                p.soloView = soloView;
                p.chainEntry = insertMapForBlock != nullptr
                                   ? PluginInsertHost::audioThread_findEntry(
                                         *insertMapForBlock,
                                         sessionSnap->getTrack(combineTrackIndex).getId())
                                   : nullptr;
                p.host = h;
                p.stageL = instrumentStripStageBuffer_.getWritePointer(2 * instrumentStripPayloadCount_);
                p.stageR = instrumentStripStageBuffer_.getWritePointer(2 * instrumentStripPayloadCount_ + 1);
                p.trackIndex = combineTrackIndex;
                p.numSamples = numSamples;
                p.insertContext = blockContext;
                ++instrumentStripPayloadCount_;
                j.p.run = &PlaybackEngine::runInstrumentStripRenderJob;
                j.p.context = &p;
                // Ordering key covers the combined work: last generation time plus the last
                // combined/strip time recorded for this row (0 until the row first runs).
                if (combineTrackIndex < (int)audioStripLastRenderTicks_.size())
                {
                    j.p.lastRenderTicks = juce::jmax(
                        j.p.lastRenderTicks,
                        audioStripLastRenderTicks_[(size_t)combineTrackIndex]);
                }
            }
        };
        for (int ti = 0; ti < sessionSnap->getNumTracks(); ++ti)
        {
            const Track& tr = sessionSnap->getTrack(ti);
            if (tr.getKind() != TrackKind::Instrument || tr.isTrackOff())
            {
                continue;
            }
            const ExperimentalInstrumentPlaybackEntry* const entry
                = playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap, tr.getId());
            if (entry == nullptr || entry->host == nullptr)
            {
                continue;
            }
            addJob(entry->host, ti);
            if (auditionActive && entry->auditionHost != nullptr && entry->auditionHost != entry->host)
            {
                addJob(entry->auditionHost, -1);
            }
        }
        return jobCount;
    };
    // Set once the combined Stage A1 batch has run this block's generation jobs — the legacy
    // dispatch inside `mixKeyedInstrumentLanesIntoOutputsIfAny` must not run them a second time.
    bool generationStageAlreadyRan = false;

    /// [Audio thread] Sum each keyed instrument whose `trackId` matches a `TrackKind::Instrument`
    /// row in `sessionSnap`, in timeline row order (see MIX ORDER below). When `sessionSnap` is missing
    /// (tear / edge), mix every snapshot entry once so staged-only playback still audible.
    const auto mixKeyedInstrumentLanesIntoOutputsIfAny = [&]()
    {
        // Instrument inserts process their host's complete block at this callback's position.
        setInsertProcessContext(t0);
        setCallbackPhase(AudioCallbackPhase::InstrumentMix);
        if (instrumentSnap == nullptr || instrumentSnap->entries.empty())
        {
            return;
        }
        if (sessionSnap != nullptr)
        {
#if !defined(NDEBUG)
            static TrackId loggedMissingPlaybackBindingOnce = kInvalidTrackId;
#endif
            // GENERATION STAGE (parallel): every MIDI source of this block has been scheduled into
            // the hosts above (segments, MIDI rows, live input, stop-edge flushes), so each live
            // instrument's `processBlock` is now independent. One job per host renders into the
            // host's own scratch across the render pool; the callback thread joins before the row
            // loop below, which then applies inserts / fader / pan / meters / routing and sums in
            // the unchanged row order. On Stage A1 collect blocks these jobs already ran in the
            // combined batch (`dispatchRowJobsAndSumAudioStages`) — never a second processBlock.
            if (!generationStageAlreadyRan && instrumentRenderPool_ != nullptr && numSamples > 0)
            {
                const int jobCount = collectInstrumentRowJobs(false);
                if (jobCount > 0)
                {
                    instrumentRenderPool_->audioThread_runJobs(instrumentRenderJobs_.data(), jobCount,
                                                                instrumentRenderSerialHint_.load(std::memory_order_relaxed));
                    if (prof)
                    {
                        profiler.audioThread_noteGenerationSection(instrumentRenderPool_->lastRunWallMs(),
                                                                   instrumentRenderPool_->lastRunJoinWaitMs(),
                                                                   jobCount,
                                                                   instrumentRenderPool_->lastRunWasParallel());
                    }
                }
            }
            // MIX ORDER: iterate `sessionSnap` rows in timeline order — each instrument lane mixes in
            // placement order alongside audio tracks; lookup `instrumentSnap.entries` **by TrackId**
            // (snapshot may carry one entry per hosted instrument lane).
            for (int ti = 0; ti < sessionSnap->getNumTracks(); ++ti)
            {
                const Track& tr = sessionSnap->getTrack(ti);
                if (tr.getKind() != TrackKind::Instrument)
                {
                    continue;
                }
                const ExperimentalInstrumentPlaybackEntry* entry
                    = playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap, tr.getId());
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
                // Mute and a fader at −∞ are GAIN decisions, not "skip the instrument": the host
                // keeps processing (its transport MIDI is consumed, its state follows the
                // transport) and the strip folds the output with gain 0. Skipping the host here
                // (pre-1.1.9) let the scheduled MIDI pile up in the host's per-block buffer for as
                // long as the row stayed muted (audio-thread allocation, then a burst of stale
                // events on unmute) and froze the plug-in mid-state. Solo layers on top as an
                // effective-mute decision; stored flags untouched.
                const float fader = solo_mute_view::effectiveTrackMuted(soloView, tr)
                                        ? 0.0f
                                        : tr.getChannelFaderGain();

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

                const RoutingPlan::SourceStep* srcStep = nullptr;
                if (rp != nullptr)
                {
                    for (const RoutingPlan::SourceStep& st : rp->sourceSteps)
                    {
                        if (st.trackIndex == ti)
                        {
                            srcStep = &st;
                            break;
                        }
                    }
                }
                // Stage A2: a combined job already rendered this row's complete strip into its
                // own stage buffer BEFORE the join — consume that stage here (meter fold + fan
                // in the unchanged row order) and NEVER run the strip a second time. Rows
                // without a combined payload (combine inactive, proxy host, shared host
                // instance, overflow) render the identical strip serially right here.
                const InstrumentStripJobPayload* combined = nullptr;
                for (int k = 0; k < instrumentStripPayloadCount_; ++k)
                {
                    if (instrumentStripPayloads_[(size_t)k].trackIndex == ti)
                    {
                        combined = &instrumentStripPayloads_[(size_t)k];
                        break;
                    }
                }
                float* stripStageL = nullptr;
                float* stripStageR = nullptr;
                if (combined != nullptr && combined->stageL != nullptr
                    && combined->stageR != nullptr)
                {
                    stripStageL = combined->stageL;
                    stripStageR = combined->stageR;
                }
                else if (postStripStagePtrs_[0] != nullptr && postStripStagePtrs_[1] != nullptr
                         && postStripStageCapacity_ >= numSamples)
                {
                    playback_mix_helpers::renderInstrumentPostStripToStereoScratch(
                        entry->host,
                        tr,
                        postStripStagePtrs_[0],
                        postStripStagePtrs_[1],
                        0,
                        numSamples,
                        pluginHost_,
                        // P2 audition gate (PID-008): the Secondary audition instance sounds only
                        // while the transport is NOT playing — never layered over transport
                        // playback. Block-boundary decision on this thread; no republish races.
                        playbackIntent != PlaybackIntent::Playing ? entry->auditionHost : nullptr,
                        soloView);
                    stripStageL = postStripStagePtrs_[0];
                    stripStageR = postStripStagePtrs_[1];
                }
                if (stripStageL != nullptr && stripStageR != nullptr)
                {
                    audioThread_foldTrackMeterIfMetered(
                        tr.getId(), stripStageL, stripStageR, numSamples);
                    if (rp != nullptr && srcStep != nullptr && srcStep->destBusIndex >= 0
                        && srcStep->destBusIndex < static_cast<int>(rp->busScratchL.size()))
                    {
                        playback_mix_helpers::fanPostStripStageToDryAndSends(stripStageL,
                                                                               stripStageR,
                                                                               0,
                                                                               numSamples,
                                                                               srcStep->destBusIndex,
                                                                               srcStep->sends,
                                                                               *rp);
                    }
                    else
                    {
                        float* dryBusL = mixBusL;
                        float* dryBusR = mixBusR;
                        if (rp != nullptr && sessionSnap != nullptr)
                        {
                            const int destBi = destBusIndexForTrackInPlan(*rp, *sessionSnap, ti);
                            if (destBi >= 0 && destBi < static_cast<int>(rp->busScratchL.size())
                                && rp->busScratchL[(size_t)destBi] != nullptr
                                && rp->busScratchR[(size_t)destBi] != nullptr)
                            {
                                dryBusL = rp->busScratchL[(size_t)destBi];
                                dryBusR = rp->busScratchR[(size_t)destBi];
                            }
                        }
                        if (dryBusL != nullptr && dryBusR != nullptr)
                        {
                            playback_mix_helpers::addPostStripStageToBus(stripStageL,
                                                                           stripStageR,
                                                                           dryBusL,
                                                                           dryBusR,
                                                                           0,
                                                                           numSamples,
                                                                           1.0f);
                        }
                        else
                        {
                            float* const stageStereo[2] = { stripStageL, stripStageR };
                            playback_mix_helpers::addPostStripStageToDeviceOutputs(stageStereo,
                                                                                     0,
                                                                                     numSamples,
                                                                                     numOutputChannels,
                                                                                     outputChannelData);
                        }
                    }
                }
                else
                {
                    float* const* instMixTarget = mixSumTarget;
                    float* instBusPtrs[2] = { mixBusL, mixBusR };
                    if (rp != nullptr && sessionSnap != nullptr)
                    {
                        const int destBi = destBusIndexForTrackInPlan(*rp, *sessionSnap, ti);
                        if (destBi >= 0 && destBi < static_cast<int>(rp->busScratchL.size())
                            && rp->busScratchL[(size_t)destBi] != nullptr
                            && rp->busScratchR[(size_t)destBi] != nullptr)
                        {
                            instBusPtrs[0] = rp->busScratchL[(size_t)destBi];
                            instBusPtrs[1] = rp->busScratchR[(size_t)destBi];
                            instMixTarget = instBusPtrs;
                        }
                    }

                    playback_mix_helpers::mixExperimentalInstrumentAfterTracks(
                        entry->host, instMixTarget, numOutputChannels, numSamples, fader, tr.getStereoPan());
                }

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
                playback_mix_helpers::mixExperimentalInstrumentAfterTracks(
                    e.host, mixSumTarget, numOutputChannels, numSamples, 1.0f, kTrackStereoPanCenter);
            }
        }
    };

    // Live input monitoring pass — one full strip render per monitored Audio track, EVERY callback
    // (transport stopped, playing or recording): selected input → pre-gain → Pre inserts →
    // fader/mute → Post inserts → pan → the track's normal routing destination and sends. Runs
    // inside `mixInstrumentsAndFinalizeMaster` so every callback exit path renders it exactly once
    // (before bus finalize). The monitored track's clip playback is suppressed in the segment
    // renderers via `monitorPtr`, so the strip (and its plugins) processes exactly one source.
    // Unresolved/None assignments feed silence — never a substituted physical input.
    const auto renderLiveInputMonitoringPass = [&]() noexcept
    {
        if (monitorPtr == nullptr || sessionSnap == nullptr || numSamples <= 0)
        {
            return;
        }
        if (postStripStagePtrs_[0] == nullptr || postStripStagePtrs_[1] == nullptr
            || postStripStageCapacity_ < numSamples)
        {
            return;
        }
        setCallbackPhase(AudioCallbackPhase::MixPrep);
        for (int mi = 0; mi < monitorPtr->count; ++mi)
        {
            const TrackId tid = monitorPtr->trackIds[(size_t)mi];
            const int ti = sessionSnap->findTrackIndexById(tid);
            if (ti < 0)
            {
                continue;
            }
            const Track& tr = sessionSnap->getTrack(ti);
            if (tr.getKind() != TrackKind::Audio)
            {
                continue;
            }
            // Read-ahead-owned row still draining after Monitor was enabled: the worker may be
            // inside this chain, so monitoring onset waits for the drain to finish (<= depth
            // blocks; the queued clip blocks keep playing — docs/READAHEAD_PROTOTYPE.md §5).
            if (readAhead_ != nullptr && readAhead_->audioThread_isOwnedForRender(tr.getId()))
            {
                continue;
            }
            // Resolve the track's assignment to packed input pointers; any unresolved member of a
            // pair makes the whole assignment unresolved (silence — no half-substitution).
            const TrackInputAssignment& ia = tr.getInputAssignment();
            const float* inA = nullptr;
            const float* inB = nullptr;
            switch (ia.kind)
            {
            case TrackInputKind::DefaultFirstInput:
                inA = (inputChannelData != nullptr && numInputChannels > 0) ? inputChannelData[0]
                                                                            : nullptr;
                break;
            case TrackInputKind::Mono:
                inA = activeInputPointerForPhysical(ia.physicalChannelA);
                break;
            case TrackInputKind::StereoPair:
                inA = activeInputPointerForPhysical(ia.physicalChannelA);
                inB = activeInputPointerForPhysical(ia.physicalChannelB);
                if (inA == nullptr || inB == nullptr)
                {
                    inA = nullptr;
                    inB = nullptr;
                }
                break;
            case TrackInputKind::None:
            default:
                break;
            }

            playback_mix_helpers::clearStereoScratch(
                postStripStagePtrs_[0], postStripStagePtrs_[1], numSamples);
            // Monitoring has no timeline slice, but effects must still receive the project's
            // current tempo while stopped so tempo-synced processing remains usable.
            setInsertProcessContext(t0);
            playback_mix_helpers::renderLiveInputTrackPostStripToStereoScratch(
                tr,
                ti,
                inA,
                inB,
                numSamples,
                postStripStagePtrs_[0],
                postStripStagePtrs_[1],
                pluginHost_,
                &preGainRampState_,
                soloView);
            audioThread_foldTrackMeterIfMetered(
                tr.getId(), postStripStagePtrs_[0], postStripStagePtrs_[1], numSamples);

            // Route to the same destination as the track's clip playback: its routing-plan source
            // step (dry bus + sends) when a plan exists, otherwise the legacy direct mix target.
            bool fannedViaPlan = false;
            if (rp != nullptr && !rp->sourceSteps.empty())
            {
                for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
                {
                    if (step.trackIndex != ti)
                    {
                        continue;
                    }
                    if (step.destBusIndex >= 0
                        && step.destBusIndex < static_cast<int>(rp->busScratchL.size()))
                    {
                        playback_mix_helpers::fanPostStripStageToDryAndSends(postStripStagePtrs_[0],
                                                                             postStripStagePtrs_[1],
                                                                             0,
                                                                             numSamples,
                                                                             step.destBusIndex,
                                                                             step.sends,
                                                                             *rp);
                        fannedViaPlan = true;
                    }
                    break;
                }
                // Stale-plan block (C2B convention): skip this block; the rebuilt plan takes over.
                if (!fannedViaPlan)
                {
                    continue;
                }
            }
            else if (mixBusL != nullptr && mixBusR != nullptr)
            {
                juce::FloatVectorOperations::add(mixBusL, postStripStagePtrs_[0], numSamples);
                juce::FloatVectorOperations::add(mixBusR, postStripStagePtrs_[1], numSamples);
            }
            else
            {
                float* const stageStereo[2] = { postStripStagePtrs_[0], postStripStagePtrs_[1] };
                playback_mix_helpers::addPostStripStageToDeviceOutputs(
                    stageStereo, 0, numSamples, numOutputChannels, outputChannelData);
            }
        }
    };

    // ---- Stage A1 + A2: ONE batch, ONE barrier per block ----
    // Runs FIRST inside `mixInstrumentsAndFinalizeMaster`, i.e. after every `renderRun` segment
    // of the block has scheduled transport MIDI and collected strip segments, and BEFORE the
    // monitoring pass / instrument row strips / bus finalize — so the bus accumulation order
    // stays: audio-row fans → monitored fans → instrument fans → bus forwarding (plan §3.1).
    // Jobs write only their own stage buffers / host scratch (never shared buses); the callback
    // sums after the join on this thread. Every object a payload references (session snapshot,
    // solo view, routing plan, insert map) is retained on this stack frame past the join.
    const auto dispatchRowJobsAndSumAudioStages = [&]() noexcept
    {
        if (!audioStripCollectActive_ && !instrumentStripCombineActive)
        {
            return;
        }
        using instrument_render::InstrumentRenderPool;
        setCallbackPhase(AudioCallbackPhase::ClipRender);
        // Instrument jobs join the SAME batch, scheduled after all of this block's transport
        // MIDI (renderRun ran already). Stage A2: where possible each instrument row's job also
        // runs the row's strip into its own stage buffer (combined job); the remainder keep the
        // legacy generation-only payload and their strip runs serially in the MIX ORDER loop.
        int jobCount = collectInstrumentRowJobs(instrumentStripCombineActive);
        generationStageAlreadyRan = true;
        int inlineFrom = audioStripPayloadCount_;
        for (int k = 0; k < audioStripPayloadCount_; ++k)
        {
            if (jobCount >= InstrumentRenderPool::kMaxJobs)
            {
                inlineFrom = k;
                break;
            }
            AudioStripJobPayload& p = audioStripPayloads_[(size_t)k];
            auto& j = instrumentRenderJobs_[(size_t)jobCount++];
            j.p = instrument_render::RenderJob::Payload{};
            j.p.run = &PlaybackEngine::runAudioStripRenderJob;
            j.p.context = &p;
            j.p.numSamples = numSamples;
            j.p.lastRenderTicks = (p.trackIndex >= 0
                                   && p.trackIndex < (int)audioStripLastRenderTicks_.size())
                                      ? audioStripLastRenderTicks_[(size_t)p.trackIndex]
                                      : 0;
        }
        // Combined-batch overflow (more instrument hosts + audio rows than job slots): the
        // overflowing audio payloads run NOW on the callback lane through the SAME job code —
        // rows are never dropped (plan §3.3 capacity rule).
        for (int k = inlineFrom; k < audioStripPayloadCount_; ++k)
        {
            audioThread_runAudioStripPayload(audioStripPayloads_[(size_t)k],
                                             InstrumentRenderPool::kCallbackLane);
        }
        if (jobCount > 0)
        {
            instrumentRenderPool_->audioThread_runJobs(instrumentRenderJobs_.data(), jobCount,
                                                       instrumentRenderSerialHint_.load(std::memory_order_relaxed));
            if (prof)
            {
                profiler.audioThread_noteGenerationSection(instrumentRenderPool_->lastRunWallMs(),
                                                           instrumentRenderPool_->lastRunJoinWaitMs(),
                                                           jobCount,
                                                           instrumentRenderPool_->lastRunWasParallel());
            }
        }
        // Read-ahead: the join above was the callback's LAST chain touch for any row Scheduled
        // this block — publishing this block's serial is what allows its worker to start
        // (docs/READAHEAD_PROTOTYPE.md §1; monotone, so cycle wraps moving the timeline
        // backwards cannot confuse the gate. The finalize/monitor context refreshes below
        // exclude owned rows, and fans/meters only touch buffers).
        if (readAhead_ != nullptr && readAheadExcludedCount_ > 0)
        {
            readAhead_->audioThread_publishJoinedBlock();
        }
        // SUM (callback thread only, after the join): fan each AUDIO row's stage to its dry
        // bus + sends in `sourceSteps` order; each segment occupies its own disjoint frame
        // range, so this step-major loop accumulates bit-identically to the serial segment-major
        // order (plan §3.1). Meters fold exactly the regions the fan reads. Combined INSTRUMENT
        // stages are NOT summed here — the MIX ORDER row loop consumes them, keeping the
        // accumulation order audio fans → monitored fans → instrument fans → bus forwarding.
        // The walk repeats the EXACT collection gates so the payload cursor lines up with the
        // collected payloads and read-ahead-owned rows consume (or miss) at their plan position.
        if (rp == nullptr || sessionSnap == nullptr || !audioStripCollectActive_)
        {
            return; // instrument-only combine block (no plan / serial-path audio): nothing to sum
        }
        int payloadCursor = 0;
        for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
        {
            if (step.destBusIndex < 0
                || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
            {
                continue;
            }
            if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
            {
                continue;
            }
            const Track& srcTr = sessionSnap->getTrack(step.trackIndex);
            if (srcTr.getKind() != TrackKind::Audio)
            {
                continue;
            }
            const TrackId tid = srcTr.getId();
            // Owned BEFORE the monitor gate (identical order as the collection). Consume every
            // recorded segment of this block in order — a cycle-wrap block has two, each fanned
            // at its own destination frame; a failed consume already counted the miss and
            // advanced the row's expected sequence (silence for exactly that segment).
            if (readAhead_ != nullptr && readAhead_->audioThread_isOwnedForRender(tid))
            {
                const int segN = juce::jmin(readAheadSegCount_, kReadAheadMaxSegmentsPerBlock);
                for (int rs = 0; rs < segN; ++rs)
                {
                    if (readAheadSegRun_[rs] <= 0)
                    {
                        continue;
                    }
                    readahead::ReadAheadRenderer::ConsumeView rcv;
                    if (readAhead_->audioThread_tryConsume(tid, readAheadSegStart_[rs],
                                                           readAheadSegRun_[rs],
                                                           readAheadSegDestFrame_[rs], rcv))
                    {
                        audioThread_foldTrackMeterIfMetered(tid,
                                                            rcv.stageL + readAheadSegDestFrame_[rs],
                                                            rcv.stageR + readAheadSegDestFrame_[rs],
                                                            readAheadSegRun_[rs]);
                        playback_mix_helpers::fanPostStripStageToDryAndSends(
                            rcv.stageL, rcv.stageR, readAheadSegDestFrame_[rs],
                            readAheadSegRun_[rs], step.destBusIndex, step.sends, *rp);
                        readAhead_->audioThread_releaseConsumed(tid);
                    }
                }
                for (int rs = segN; rs < readAheadSegCount_; ++rs)
                {
                    readAhead_->audioThread_noteMiss(tid); // defensive: >2 segments can not occur
                }
                continue;
            }
            if (monitorPtr != nullptr && monitorPtr->contains(srcTr.getId()))
            {
                continue;
            }
            if (payloadCursor >= audioStripPayloadCount_)
            {
                continue; // no segment ever collected this block (e.g. no audible run)
            }
            const AudioStripJobPayload& p = audioStripPayloads_[(size_t)payloadCursor++];
            if (p.step == nullptr || p.trackIndex < 0 || p.trackIndex >= sessionSnap->getNumTracks())
            {
                continue;
            }
            for (int s = 0; s < p.numSegments; ++s)
            {
                const AudioStripSegmentDesc& seg = p.segments[(size_t)s];
                if (seg.audibleRun <= 0)
                {
                    continue;
                }
                audioThread_foldTrackMeterIfMetered(tid,
                                                    p.stageL + seg.destFrame,
                                                    p.stageR + seg.destFrame,
                                                    seg.audibleRun);
                playback_mix_helpers::fanPostStripStageToDryAndSends(p.stageL,
                                                                     p.stageR,
                                                                     seg.destFrame,
                                                                     seg.audibleRun,
                                                                     p.step->destBusIndex,
                                                                     p.step->sends,
                                                                     *rp);
            }
        }
    };

    const auto mixInstrumentsAndFinalizeMaster = [&]() noexcept
    {
        using audio_profiler::Phase;
        std::int64_t tPhase = prof ? audio_profiler::AudioThreadProfiler::ticks() : 0;
        dispatchRowJobsAndSumAudioStages();
        if (prof)
        {
            profiler.audioThread_addPhase(Phase::ClipRender, tPhase);
            tPhase = audio_profiler::AudioThreadProfiler::ticks();
        }
        renderLiveInputMonitoringPass();
        if (prof)
        {
            profiler.audioThread_addPhase(Phase::Monitor, tPhase);
            tPhase = audio_profiler::AudioThreadProfiler::ticks();
        }
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        if (prof)
        {
            profiler.audioThread_addPhase(Phase::InstrumentMix, tPhase);
            tPhase = audio_profiler::AudioThreadProfiler::ticks();
        }
        finalizeRoutingToDevice();
        if (prof)
        {
            profiler.audioThread_addPhase(Phase::Finalize, tPhase);
        }
    };

    if (sessionSnap == nullptr || deviceBlockSizeInFrames <= 0
        || playbackIntent != PlaybackIntent::Playing)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixInstrumentsAndFinalizeMaster();
        return;
    }

    // Run-end rule: playback stops advancing at the arrangement extent. A capturing record run
    // is the one exception — the transport keeps running through the take (the coordinator grows
    // the navigable extent off the audio thread; the engine never depends on that timing), so
    // audio-only, MIDI-only and combined takes can cross the previous end without a frozen
    // playhead or stranded MIDI positions.
    std::int64_t timelineEnd = sessionSnap->getArrangementExtentSamples();
    if (recordRunCapturing)
    {
        timelineEnd = juce::jmax(timelineEnd, t0 + static_cast<std::int64_t>(deviceBlockSizeInFrames));
    }
    if (timelineEnd <= 0 || t0 >= timelineEnd)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixInstrumentsAndFinalizeMaster();
        return;
    }

    const auto jmax0 = [](const std::int64_t x) noexcept -> std::int64_t
    {
        return x < 0 ? std::int64_t{ 0 } : x;
    };

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
        mixInstrumentsAndFinalizeMaster();
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

        setCallbackPhase(AudioCallbackPhase::ClipRender);
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
        std::int64_t tPhase = prof ? audio_profiler::AudioThreadProfiler::ticks() : 0;

        // Clip sources can be split at cycle boundaries. Refresh before their Pre/Post chains so
        // a plug-in sees the actual sub-block start, never a stale prior loop position.
        setInsertProcessContext(timelineStartAudible);

        TrackId omitClipPlaybackForTrack = kInvalidTrackId;
        if (recorder_ != nullptr && recorder_->isRecording())
        {
            omitClipPlaybackForTrack = recorder_->getRecordingTrackId();
        }

        if (audioStripCollectActive_ && rp != nullptr && sessionSnap != nullptr)
        {
            // COLLECT (Stage A1): describe this segment on each eligible row's payload instead of
            // rendering here; `dispatchRowJobsAndSumAudioStages` renders (render-pool jobs) and
            // sums (callback thread, sourceSteps order) after the block's last segment. The
            // per-step gates are block-constant and IDENTICAL to the activation pre-count, so a
            // cycle wrap's second call walks the same payloads in the same order.
            const int destFrame = outFrame0 + silencePrefix;
            const PluginProcessTransportContext segContext
                = makeInsertProcessContext(timelineStartAudible);
            if (readAhead_ != nullptr && readAheadExcludedCount_ > 0)
            {
                // Record this segment's consume key (row-independent; the engine renders at
                // most two segments per block — a cycle wrap). The sum loop consumes them in
                // the same order; the worker predicted identical keys from the same inputs.
                if (readAheadSegCount_ < kReadAheadMaxSegmentsPerBlock)
                {
                    readAheadSegStart_[readAheadSegCount_] = timelineStartAudible;
                    readAheadSegRun_[readAheadSegCount_] = audibleRun;
                    readAheadSegDestFrame_[readAheadSegCount_] = destFrame;
                }
                ++readAheadSegCount_;
            }
            int eligIdx = 0;
            for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
            {
                if (step.destBusIndex < 0
                    || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                // C2B: stale-plan guard (see finalize staged bus loop).
                if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
                {
                    continue;
                }
                const Track& srcTr = sessionSnap->getTrack(step.trackIndex);
                if (srcTr.getKind() != TrackKind::Audio)
                {
                    continue;
                }
                // Read-ahead-owned row: no live render — the sum loop consumes its ring block
                // (or counts a miss). Checked BEFORE the monitor gate (a monitored row still
                // DRAINING keeps consuming); IDENTICAL gate order as the activation pre-count.
                if (readAhead_ != nullptr && readAhead_->audioThread_isOwnedForRender(srcTr.getId()))
                {
                    continue;
                }
                // Monitor ON: this track's clip playback (and its insert pass here) is replaced by
                // the live-input monitoring pass in `mixInstrumentsAndFinalizeMaster`.
                if (monitorPtr != nullptr && monitorPtr->contains(srcTr.getId()))
                {
                    continue;
                }
                if (eligIdx >= kMaxAudioStripJobs)
                {
                    break; // unreachable: the activation pre-count bounded the eligible set
                }
                AudioStripJobPayload& p = audioStripPayloads_[(size_t)eligIdx];
                if (eligIdx >= audioStripPayloadCount_)
                {
                    p = AudioStripJobPayload{};
                    p.engine = this;
                    p.sessionSnap = sessionSnap.get();
                    p.soloView = soloView;
                    p.chainEntry = insertMapForBlock != nullptr
                                       ? PluginInsertHost::audioThread_findEntry(*insertMapForBlock,
                                                                                 srcTr.getId())
                                       : nullptr;
                    p.step = &step;
                    p.stageL = audioStripStageBuffer_.getWritePointer(2 * eligIdx);
                    p.stageR = audioStripStageBuffer_.getWritePointer(2 * eligIdx + 1);
                    p.trackIndex = step.trackIndex;
                    p.timelineEnd = timelineEnd;
                    audioStripPayloadCount_ = eligIdx + 1;
                }
                if (p.numSegments < AudioStripJobPayload::kMaxSegments)
                {
                    AudioStripSegmentDesc& seg = p.segments[(size_t)p.numSegments++];
                    seg.timelineStartAudible = timelineStartAudible;
                    seg.audibleRun = audibleRun;
                    seg.destFrame = destFrame;
                    // Recorder state CAN change between a block's renderRun calls — per segment.
                    seg.omitClipPlaybackForTrack = omitClipPlaybackForTrack;
                    seg.insertContext = segContext;
                }
                ++eligIdx;
            }
        }
        else if (rp != nullptr && !rp->sourceSteps.empty() && postStripStagePtrs_[0] != nullptr
            && postStripStagePtrs_[1] != nullptr && postStripStageCapacity_ >= audibleRun)
        {
            // SERIAL strip path (collect inactive: not playing, no pool, capacity exceeded, or
            // job buffers unprepared). Same strip core as the jobs, on the callback lane with the
            // host's shared scratch (via the serial wrapper).
            const int destFrame = outFrame0 + silencePrefix;
            for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
            {
                if (step.destBusIndex < 0
                    || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                // C2B: stale-plan guard (see finalize staged bus loop).
                if (step.trackIndex < 0 || step.trackIndex >= sessionSnap->getNumTracks())
                {
                    continue;
                }
                const Track& srcTr = sessionSnap->getTrack(step.trackIndex);
                if (srcTr.getKind() != TrackKind::Audio)
                {
                    continue;
                }
                // Read-ahead-owned row on a serial-path block (e.g. payload overflow): consume
                // the ring segment inline at the row's plan position so the accumulation order
                // matches the live path; a failed consume already counted the miss for exactly
                // this segment. BEFORE the monitor gate — identical gate order everywhere.
                if (readAhead_ != nullptr && readAhead_->audioThread_isOwnedForRender(srcTr.getId()))
                {
                    readahead::ReadAheadRenderer::ConsumeView rcv;
                    if (readAhead_->audioThread_tryConsume(srcTr.getId(), timelineStartAudible,
                                                           audibleRun, destFrame, rcv))
                    {
                        audioThread_foldTrackMeterIfMetered(srcTr.getId(), rcv.stageL + destFrame,
                                                            rcv.stageR + destFrame, audibleRun);
                        playback_mix_helpers::fanPostStripStageToDryAndSends(
                            rcv.stageL, rcv.stageR, destFrame, audibleRun, step.destBusIndex,
                            step.sends, *rp);
                        readAhead_->audioThread_releaseConsumed(srcTr.getId());
                    }
                    continue;
                }
                // Monitor ON: this track's clip playback (and its insert pass here) is replaced by
                // the live-input monitoring pass in `mixInstrumentsAndFinalizeMaster`.
                if (monitorPtr != nullptr && monitorPtr->contains(srcTr.getId()))
                {
                    continue;
                }
                // Clear the stage region the strip WRITES ([destFrame, destFrame+audibleRun)):
                // clearing at 0 while rendering/metering/fanning at destFrame fanned a stale
                // stage region on every cycle-wrap second segment (pre-existing defect, see the
                // final A1 report; the per-job stages fix it structurally on the parallel path).
                playback_mix_helpers::clearStereoScratch(postStripStagePtrs_[0] + destFrame,
                                                         postStripStagePtrs_[1] + destFrame,
                                                         audibleRun);
                playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
                    *sessionSnap,
                    timelineStartAudible,
                    audibleRun,
                    destFrame,
                    postStripStagePtrs_[0],
                    postStripStagePtrs_[1],
                    pluginHost_,
                    omitClipPlaybackForTrack,
                    timelineEnd,
                    step.trackIndex,
                    &preGainRampState_,
                    soloView);
                audioThread_foldTrackMeterIfMetered(srcTr.getId(),
                                                    postStripStagePtrs_[0] + destFrame,
                                                    postStripStagePtrs_[1] + destFrame,
                                                    audibleRun);
                playback_mix_helpers::fanPostStripStageToDryAndSends(postStripStagePtrs_[0],
                                                                       postStripStagePtrs_[1],
                                                                       destFrame,
                                                                       audibleRun,
                                                                       step.destBusIndex,
                                                                       step.sends,
                                                                       *rp);
            }
        }
        else if (rp != nullptr && !rp->sourceSteps.empty())
        {
            for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
            {
                if (step.destBusIndex < 0
                    || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                float* const destPtrs[2] = { rp->busScratchL[(size_t)step.destBusIndex],
                                             rp->busScratchR[(size_t)step.destBusIndex] };
                playback_mix_helpers::renderAudioTracksClipSummingForSegment(*sessionSnap,
                                                                             timelineStartAudible,
                                                                             audibleRun,
                                                                             outFrame0 + silencePrefix,
                                                                             2,
                                                                             destPtrs,
                                                                             pluginHost_,
                                                                             omitClipPlaybackForTrack,
                                                                             timelineEnd,
                                                                             step.trackIndex,
                                                                             &preGainRampState_,
                                                                             monitorPtr,
                                                                             soloView);
            }
        }
        else
        {
            playback_mix_helpers::renderAudioTracksClipSummingForSegment(*sessionSnap,
                                                                         timelineStartAudible,
                                                                         audibleRun,
                                                                         outFrame0 + silencePrefix,
                                                                         numOutputChannels,
                                                                         mixSumTarget,
                                                                         pluginHost_,
                                                                         omitClipPlaybackForTrack,
                                                                         timelineEnd,
                                                                         -1,
                                                                         &preGainRampState_,
                                                                         monitorPtr,
                                                                         soloView);
        }

        if (prof)
        {
            profiler.audioThread_addPhase(audio_profiler::Phase::ClipRender, tPhase);
            tPhase = audio_profiler::AudioThreadProfiler::ticks();
        }

        // Timeline order: dispatch transport MIDI toward each Instrument row that has a playback entry.
        if (playbackIntent == PlaybackIntent::Playing && instrumentSnap != nullptr)
        {
            setCallbackPhase(AudioCallbackPhase::TransportMidiSchedule);
            const bool segDisc = forceSegDiscontinuity || becamePlayingTransport;
            for (int instTi = 0; instTi < sessionSnap->getNumTracks(); ++instTi)
            {
                const Track& itr = sessionSnap->getTrack(instTi);
                if (itr.getKind() != TrackKind::Instrument)
                    continue;

                const ExperimentalInstrumentPlaybackEntry* const entry =
                    playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap, itr.getId());
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
                                                                                     emitPtr,
                                                                                     // Solo: an open CARRIER strip must not leak the
                                                                                     // destination's own clips (see SoloMuteView).
                                                                                     solo_mute_view::transportClipsSuppressedBySolo(
                                                                                         soloView, itr.getId()),
                                                                                     // Solo: explicitly soloed but base-muted lane
                                                                                     // still delivers its events.
                                                                                     solo_mute_view::trackForcedAudibleBySolo(
                                                                                         soloView, itr.getId()));

                // P1G: note the audible timeline segment for proxy substitution — consumed by
                // the host only when its published playback view selects Proxy for this block.
                if (entry->host != nullptr)
                {
                    entry->host->audioThread_noteProxyTimelineSegmentForCurrentBlock(
                        timelineStartAudible, outFrame0 + silencePrefix, audibleRun);
                    // Prepared loop wrapping: announce the active cycle (in the same
                    // shifted timeline domain as the segments) so the proxy reader
                    // prefetches the loop-start region before the wrap. Atomics only.
                    if (validCycle)
                    {
                        entry->host->audioThread_noteProxyLoopRangeForCurrentBlock(
                            locL + playbackShift, locR + playbackShift);
                    }
                }

                if (routePlayEdgeDiag && sx >= 0 && emitPtr != nullptr)
                {
                    routingInstSlots[sx].firstSegDiagCaptured = true;
                }
            }

            // TrackKind::Midi sources (Phase B), in snapshot order = deterministic merge order:
            // resolve each source's destination from the *current* session snapshot and schedule
            // into the destination's host after that destination's own events. Unresolvable
            // destinations are silent; a reroute first releases this source's sounding notes in
            // the old destination (never allNotesOff — the destination may sustain other sources).
            for (const auto& src : instrumentSnap->midiSources)
            {
                if (src.midiController == nullptr)
                {
                    continue;
                }
                TrackId destId = kInvalidTrackId;
                {
                    const int srcIdx = sessionSnap->findTrackIndexById(src.trackId);
                    if (srcIdx >= 0)
                    {
                        destId = sessionSnap->getTrack(srcIdx).getMidiDestinationTrackId();
                    }
                }
                const ExperimentalInstrumentPlaybackEntry* const destEntry =
                    (destId != kInvalidTrackId)
                        ? playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap,
                                                                                        destId)
                        : nullptr;

                const TrackId lastDest = src.midiController->audioThread_getLastRoutedDestTrackId();
                if (lastDest != kInvalidTrackId
                    && (destEntry == nullptr || lastDest != destEntry->trackId))
                {
                    const ExperimentalInstrumentPlaybackEntry* const oldEntry =
                        playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap,
                                                                                      lastDest);
                    if (oldEntry != nullptr && oldEntry->host != nullptr)
                    {
                        src.midiController->audioThread_flushPendingTransportOffsInto(
                            *oldEntry->host, outFrame0 + silencePrefix, deviceBlockSizeInFrames);
                    }
                    else
                    {
                        src.midiController->audioThread_dropPendingTransportOffs();
                    }
                    src.midiController->audioThread_setLastRoutedDestTrackId(kInvalidTrackId);
                }

                if (destEntry == nullptr || destEntry->host == nullptr)
                {
                    continue;
                }
                src.midiController->audioThread_scheduleTransportMidiForSegment(*destEntry->host,
                                                                                timelineStartAudible,
                                                                                audibleRun,
                                                                                outFrame0 + silencePrefix,
                                                                                segDisc,
                                                                                deviceBlockSizeInFrames,
                                                                                nullptr,
                                                                                // Solo: non-soloed MIDI source into an OPEN
                                                                                // destination strip must stay silent.
                                                                                solo_mute_view::routedMidiSourceSuppressedBySolo(
                                                                                    soloView, src.trackId),
                                                                                solo_mute_view::trackForcedAudibleBySolo(
                                                                                    soloView, src.trackId));
                src.midiController->audioThread_setLastRoutedDestTrackId(destEntry->trackId);
            }
        }
        if (prof)
        {
            profiler.audioThread_addPhase(audio_profiler::Phase::MidiSchedule, tPhase);
        }
    };

    // --- Linear playback (cycle off, invalid range, or playhead already at / past right locator). ---
    if (!validCycle || tWork >= locR)
    {
        const std::int64_t firstRun64 = juce::jmin(blockFrames, jmax0(availTimeline));
        if (firstRun64 <= 0)
        {
            transport_.audioThread_advancePlayheadIfPlaying(0);
            mixInstrumentsAndFinalizeMaster();
            return;
        }
        renderRun(tWork, static_cast<int>(firstRun64), 0, becamePlayingTransport);
        transport_.audioThread_advancePlayheadIfPlaying(firstRun64);
        mixInstrumentsAndFinalizeMaster();
        return;
    }

    // --- Cycle wrap: approached from tWork < locR ---
    const std::int64_t framesToR = locR - tWork;
    const std::int64_t maxPlayableThisBlock = juce::jmin(blockFrames, jmax0(availTimeline));
    const std::int64_t firstRun64 = juce::jmin(maxPlayableThisBlock, framesToR);

    if (firstRun64 <= 0)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixInstrumentsAndFinalizeMaster();
        return;
    }

    const int firstRun = static_cast<int>(firstRun64);
    renderRun(tWork, firstRun, 0, becamePlayingTransport);

    const bool reachedRightLocator = (tWork + firstRun64 >= locR);
    if (!reachedRightLocator)
    {
        transport_.audioThread_advancePlayheadIfPlaying(firstRun64);
        mixInstrumentsAndFinalizeMaster();
        return;
    }

    const std::int64_t remainingInBlock = blockFrames - firstRun64;
    const std::int64_t loopSpan = locR - locL;
    const std::int64_t secondRun64 = juce::jmin(
        jmax0(remainingInBlock),
        jmax0(loopSpan),
        jmax0(timelineEnd - locL));

    // Live-MIDI cycle recording: the wrap's exact position on the monotone clock delimits the
    // cycle passes of a take (anchor + wrap marker; see LiveMidiInputBus time model). Noted on
    // both wrap branches below, after the transport's own wrap bookkeeping.
    const auto noteLiveMidiWrap = [&]() noexcept {
        if (live_midi::LiveMidiInputBus* const bus = liveMidiBus_.load(std::memory_order_acquire))
        {
            bus->audioThread_noteCycleWrap(monoSampleAtBlockStart + firstRun64, locL,
                                           transport_.audioThread_relaxedLoadWrapPassCount());
        }
    };

    if (secondRun64 > 0)
    {
        const int sr = static_cast<int>(secondRun64);
        renderRun(locL, sr, firstRun, true);
        transport_.audioThread_storePlayheadOnWrap(locL + secondRun64);
        transport_.audioThread_signalCycleWrap();
        noteLiveMidiWrap();
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
        noteLiveMidiWrap();
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
    mixInstrumentsAndFinalizeMaster();
}

void PlaybackEngine::invokeExperimentalInstrumentBeginBlocks(
    const ExperimentalInstrumentPlaybackSnapshot* instrumentSnap,
    const int numSamples) noexcept
{
    if (instrumentProcessingSuspended_.load(std::memory_order_acquire))
    {
        return;
    }
    if (instrumentSnap != nullptr)
    {
        for (const auto& e : instrumentSnap->entries)
        {
            if (e.host != nullptr)
            {
                e.host->audioThread_beginAudioBlock(numSamples);
            }
            if (e.auditionHost != nullptr && e.auditionHost != e.host)
            {
                e.auditionHost->audioThread_beginAudioBlock(numSamples);
            }
        }
    }
    if (experimentalBeginBlockAllHosts_)
    {
        experimentalBeginBlockAllHosts_(numSamples);
    }
}

bool PlaybackEngine::beginOfflineRenderGate() noexcept
{
    const bool becameActive = offlineRenderGateDepth_.fetch_add(1, std::memory_order_seq_cst) == 0;
    if (becameActive && readAhead_ != nullptr)
    {
        // The offline render processes the SAME live chains on the message thread: pause the
        // read-ahead worker for the whole gate and reset ownership at the next live block
        // (docs/READAHEAD_PROTOTYPE.md §8).
        readAhead_->pauseWorkerAndWait();
        readAhead_->requestFullReset();
    }
    return becameActive;
}

bool PlaybackEngine::endOfflineRenderGate() noexcept
{
    const int previous = offlineRenderGateDepth_.fetch_sub(1, std::memory_order_seq_cst);
    jassert(previous > 0);
    if (previous == 1 && readAhead_ != nullptr)
    {
        readAhead_->resumeWorker();
    }
    return previous == 1;
}

bool PlaybackEngine::isOfflineRenderInProgress() const noexcept
{
    return offlineRenderGateDepth_.load(std::memory_order_seq_cst) > 0;
}

bool PlaybackEngine::isAudioCallbackInProcessingSection() const noexcept
{
    return audioCallbackInProcessingSection_.load(std::memory_order_seq_cst);
}

juce::String PlaybackEngine::describeAudioCallbackStateForDiagnostics() const noexcept
{
    const auto phaseName = [](const int p) noexcept -> const char* {
        switch (static_cast<AudioCallbackPhase>(p))
        {
            case AudioCallbackPhase::Idle: return "idle";
            case AudioCallbackPhase::Begin: return "begin";
            case AudioCallbackPhase::RecorderPush: return "recorder-push";
            case AudioCallbackPhase::TransportBeginBlock: return "transport-begin-block";
            case AudioCallbackPhase::OfflineGateSilence: return "offline-gate-silence";
            case AudioCallbackPhase::LoadSnapshot: return "load-snapshot";
            case AudioCallbackPhase::InstrumentBeginBlock: return "instrument-begin-block";
            case AudioCallbackPhase::MixPrep: return "mix-prep";
            case AudioCallbackPhase::CountIn: return "count-in";
            case AudioCallbackPhase::ClipRender: return "clip-render";
            case AudioCallbackPhase::TransportMidiSchedule: return "transport-midi-schedule";
            case AudioCallbackPhase::InstrumentMix: return "instrument-mix";
            case AudioCallbackPhase::FinalizeRouting: return "finalize-routing";
            case AudioCallbackPhase::FinalizeStagedBusLoop: return "finalize-staged-bus-loop";
            case AudioCallbackPhase::FinalizeLegacyBusLoop: return "finalize-legacy-bus-loop";
            case AudioCallbackPhase::FinalizeMasterFallback: return "finalize-master-fallback";
        }
        return "unknown";
    };
    return juce::String("callbackPhase=")
           + phaseName(audioCallbackPhase_.load(std::memory_order_relaxed))
           + " inSection="
           + (audioCallbackInProcessingSection_.load(std::memory_order_seq_cst) ? "yes" : "no")
           + " lastBlockSamples="
           + juce::String(audioCallbackLastBlockSamples_.load(std::memory_order_relaxed))
           + " enterCount="
           + juce::String((juce::int64)audioCallbackEnterCount_.load(std::memory_order_relaxed))
           + " gateDepth=" + juce::String(offlineRenderGateDepth_.load(std::memory_order_seq_cst))
           + " intent="
           + juce::String(static_cast<int>(transport_.audioThread_loadIntent()))
           + " playhead=" + juce::String(transport_.audioThread_loadPlayhead())
           + " "
           + (pluginHost_ != nullptr ? pluginHost_->describeAudioThreadInsertStateForDiagnostics()
                                     : juce::String("insert=n/a"));
}

bool PlaybackEngine::waitForAudioCallbackExit(const double maxWaitMs, double* const waitedMsOut) noexcept
{
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool drained = true;
    while (audioCallbackInProcessingSection_.load(std::memory_order_seq_cst))
    {
        if (juce::Time::getMillisecondCounterHiRes() - startMs >= maxWaitMs)
        {
            drained = false;
            break;
        }
        juce::Thread::sleep(1);
    }
    if (waitedMsOut != nullptr)
    {
        *waitedMsOut = juce::Time::getMillisecondCounterHiRes() - startMs;
    }
    return drained;
}

void PlaybackEngine::setInstrumentProcessingSuspended(const bool suspended) noexcept
{
    instrumentProcessingSuspended_.store(suspended, std::memory_order_release);
}

bool PlaybackEngine::isAudioInsideInstrumentSection() const noexcept
{
    return audioInsideInstrumentSection_.load(std::memory_order_acquire);
}

std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot>
PlaybackEngine::loadExperimentalInstrumentPlaybackSnapshotForAudioThread() const noexcept
{
    return experimentalInstrumentPlaybackSnapshot_.load(std::memory_order_acquire);
}

void PlaybackEngine::renderOfflineMixdownBlock(const SessionSnapshot& sessionSnap,
                                               const ExperimentalInstrumentPlaybackSnapshot* instrumentSnap,
                                               const std::int64_t timelineSegStartSample,
                                               const int numSamples,
                                               float* const* stereoOutputLR,
                                               const bool instrumentForceDiscontinuity)
{
    jassert(numSamples > 0);
    jassert(stereoOutputLR != nullptr && stereoOutputLR[0] != nullptr && stereoOutputLR[1] != nullptr);

    // Offline export is gated against the device callback, so this is the sole processor caller.
    // Publish the export timeline position and project tempo rather than borrowing live Transport:
    // a stopped or separately positioned UI playhead must not leak into the rendered VST3 context.
    if (pluginHost_ != nullptr)
    {
        const ProjectMusicalTime musicalTime = sessionSnap.getProjectMusicalTime();
        PluginProcessTransportContext insertContext;
        insertContext.timelineSample = timelineSegStartSample;
        insertContext.sampleRate = deviceSampleRateForDiagnostics_.load(std::memory_order_relaxed);
        insertContext.bpm = musicalTime.bpm;
        insertContext.timeSignatureNumerator = musicalTime.numerator;
        insertContext.timeSignatureDenominator = musicalTime.denominator;
        insertContext.isPlaying = true;
        pluginHost_->audioThread_setProcessTransportContext(insertContext);
    }

    if (!instrumentProcessingSuspended_.load(std::memory_order_acquire))
    {
        invokeExperimentalInstrumentBeginBlocks(instrumentSnap, numSamples);
    }
    // Solo: offline mixdown follows the CURRENT audible solo picture (spec §6) through the same
    // derived view the realtime path consumes — one acquire per offline block. Proxy RENDERING
    // (ProxyRenderExecutor) is a separate path and never sees this view: a proxy always renders
    // its intended full material.
    const std::shared_ptr<const SoloMuteView> soloViewSnap
        = soloMuteView_.load(std::memory_order_acquire);
    const SoloMuteView* const soloView
        = (soloViewSnap != nullptr && soloViewSnap->soloActive) ? soloViewSnap.get() : nullptr;
    const int offlineCap = juce::jmax(numSamples, kOfflineMixdownBlockCapSamples);
    ensureMasterScratchCapacity(offlineCap);
    ensurePostStripStageScratchCapacity(offlineCap);

    juce::FloatVectorOperations::clear(stereoOutputLR[0], numSamples);
    juce::FloatVectorOperations::clear(stereoOutputLR[1], numSamples);

    std::size_t offlineBusCount = 0;
    for (int i = 0; i < sessionSnap.getNumTracks(); ++i)
    {
        const TrackKind k = sessionSnap.getTrack(i).getKind();
        if (k == TrackKind::Group || k == TrackKind::Master)
        {
            ++offlineBusCount;
        }
    }
    ensureRoutingBusScratchPool(offlineBusCount, juce::jmax(numSamples, kOfflineMixdownBlockCapSamples));
    // C4B: the pool is grow-only, so only the first offlineBusCount slots belong to this render.
    std::vector<std::pair<float*, float*>> offlineScratchPairs;
    std::vector<std::shared_ptr<void>> offlineScratchOwners;
    offlineScratchPairs.reserve(offlineBusCount);
    offlineScratchOwners.reserve(offlineBusCount);
    for (std::size_t i = 0; i < offlineBusCount && i < routingBusScratch_.size(); ++i)
    {
        const std::shared_ptr<RoutingBusScratchSlot>& slot = routingBusScratch_[i];
        offlineScratchPairs.emplace_back(slot->ptrs[0], slot->ptrs[1]);
        offlineScratchOwners.push_back(slot);
    }
    const std::shared_ptr<const RoutingPlan> offlinePlan
        = routing_plan_builder::build(sessionSnap, offlineScratchPairs, std::move(offlineScratchOwners));
    const RoutingPlan* const rp = offlinePlan.get();

    const Track* masterTrackPtr = playback_mix_helpers::findCanonicalMasterTrack(sessionSnap);
    float* mixBusL = nullptr;
    float* mixBusR = nullptr;
    if (rp != nullptr && !rp->busScratchL.empty() && rp->masterBusIndex < rp->busScratchL.size())
    {
        mixBusL = rp->busScratchL[rp->masterBusIndex];
        mixBusR = rp->busScratchR[rp->masterBusIndex];
        for (size_t bi = 0; bi < rp->busScratchL.size(); ++bi)
        {
            if (rp->busScratchL[bi] != nullptr)
            {
                juce::FloatVectorOperations::clear(rp->busScratchL[bi], numSamples);
            }
            if (rp->busScratchR[bi] != nullptr)
            {
                juce::FloatVectorOperations::clear(rp->busScratchR[bi], numSamples);
            }
        }
    }
    else if (masterScratchCapacity_ >= numSamples && masterScratchPtrs_[0] != nullptr
             && masterScratchPtrs_[1] != nullptr && masterTrackPtr != nullptr)
    {
        juce::FloatVectorOperations::clear(masterScratchPtrs_[0], numSamples);
        juce::FloatVectorOperations::clear(masterScratchPtrs_[1], numSamples);
        mixBusL = masterScratchPtrs_[0];
        mixBusR = masterScratchPtrs_[1];
    }
    float* const mixBusPtrs[2] = { mixBusL, mixBusR };
    float* const* mixSumTarget
        = (mixBusL != nullptr && mixBusR != nullptr) ? mixBusPtrs : stereoOutputLR;

    const std::int64_t playbackShift = playbackOffsetSamples_.load(std::memory_order_acquire);
    const std::int64_t renderBase = timelineSegStartSample + playbackShift;
    std::int64_t silenceFrames = 0;
    if (renderBase < 0)
    {
        silenceFrames = juce::jmin(static_cast<std::int64_t>(numSamples), -renderBase);
    }
    const int audibleRun = static_cast<int>(static_cast<std::int64_t>(numSamples) - silenceFrames);
    if (audibleRun > 0)
    {
        const std::int64_t timelineStartAudible = renderBase + silenceFrames;
        jassert(timelineStartAudible >= 0);
        const int silencePrefix = static_cast<int>(silenceFrames);

        if (rp != nullptr && !rp->sourceSteps.empty() && postStripStagePtrs_[0] != nullptr
            && postStripStagePtrs_[1] != nullptr && postStripStageCapacity_ >= audibleRun)
        {
            const int destFrame = silencePrefix;
            for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
            {
                if (step.destBusIndex < 0
                    || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                // C2B: stale-plan guard (see live finalize staged bus loop).
                if (step.trackIndex < 0 || step.trackIndex >= sessionSnap.getNumTracks())
                {
                    continue;
                }
                const Track& srcTr = sessionSnap.getTrack(step.trackIndex);
                if (srcTr.getKind() != TrackKind::Audio)
                {
                    continue;
                }
                playback_mix_helpers::clearStereoScratch(
                    postStripStagePtrs_[0], postStripStagePtrs_[1], audibleRun);
                playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
                    sessionSnap,
                    timelineStartAudible,
                    audibleRun,
                    destFrame,
                    postStripStagePtrs_[0],
                    postStripStagePtrs_[1],
                    pluginHost_,
                    kInvalidTrackId,
                    sessionSnap.getArrangementExtentSamples(),
                    step.trackIndex,
                    nullptr,
                    soloView);
                playback_mix_helpers::fanPostStripStageToDryAndSends(postStripStagePtrs_[0],
                                                                       postStripStagePtrs_[1],
                                                                       destFrame,
                                                                       audibleRun,
                                                                       step.destBusIndex,
                                                                       step.sends,
                                                                       *rp);
            }
        }
        else if (rp != nullptr && !rp->sourceSteps.empty())
        {
            for (const RoutingPlan::SourceStep& step : rp->sourceSteps)
            {
                if (step.destBusIndex < 0
                    || step.destBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                float* const destPtrs[2] = { rp->busScratchL[(size_t)step.destBusIndex],
                                             rp->busScratchR[(size_t)step.destBusIndex] };
                playback_mix_helpers::renderAudioTracksClipSummingForSegment(sessionSnap,
                                                                             timelineStartAudible,
                                                                             audibleRun,
                                                                             silencePrefix,
                                                                             2,
                                                                             destPtrs,
                                                                             pluginHost_,
                                                                             kInvalidTrackId,
                                                                             sessionSnap.getArrangementExtentSamples(),
                                                                             step.trackIndex,
                                                                             nullptr,
                                                                             nullptr,
                                                                             soloView);
            }
        }
        else
        {
            playback_mix_helpers::renderAudioTracksClipSummingForSegment(sessionSnap,
                                                                         timelineStartAudible,
                                                                         audibleRun,
                                                                         silencePrefix,
                                                                         2,
                                                                         mixSumTarget,
                                                                         pluginHost_,
                                                                         kInvalidTrackId,
                                                                         sessionSnap.getArrangementExtentSamples(),
                                                                         -1,
                                                                         nullptr,
                                                                         nullptr,
                                                                         soloView);
        }

        if (instrumentSnap != nullptr)
        {
            for (int instTi = 0; instTi < sessionSnap.getNumTracks(); ++instTi)
            {
                const Track& itr = sessionSnap.getTrack(instTi);
                if (itr.getKind() != TrackKind::Instrument)
                {
                    continue;
                }

                const ExperimentalInstrumentPlaybackEntry* const entry =
                    playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap, itr.getId());
                if (entry == nullptr)
                {
                    continue;
                }

                entry->midiController->audioThread_scheduleTransportMidiForSegment(*entry->host,
                                                                                   timelineStartAudible,
                                                                                   audibleRun,
                                                                                   silencePrefix,
                                                                                   instrumentForceDiscontinuity,
                                                                                   numSamples,
                                                                                   nullptr,
                                                                                   solo_mute_view::transportClipsSuppressedBySolo(
                                                                                       soloView, itr.getId()),
                                                                                   solo_mute_view::trackForcedAudibleBySolo(
                                                                                       soloView, itr.getId()));

                // P1G: offline mixdown uses the same authoritative source selection — the host
                // substitutes the current proxy for this segment when its view selects Proxy.
                // Offline runs off the audio thread faster than realtime, so block briefly
                // until the proxy range is resident (avoids artificial underruns in the file).
                if (entry->host != nullptr)
                {
                    (void)entry->host->messageThread_prefetchProxyRangeForOffline(
                        timelineStartAudible, audibleRun, 2000);
                    entry->host->audioThread_noteProxyTimelineSegmentForCurrentBlock(
                        timelineStartAudible, audibleRun > 0 ? silencePrefix : 0, audibleRun);
                }
            }

            // TrackKind::Midi sources: same destination resolution and merge order as the
            // realtime path, so offline mixdown renders routed MIDI identically.
            for (const auto& src : instrumentSnap->midiSources)
            {
                if (src.midiController == nullptr)
                {
                    continue;
                }
                TrackId destId = kInvalidTrackId;
                {
                    const int srcIdx = sessionSnap.findTrackIndexById(src.trackId);
                    if (srcIdx >= 0)
                    {
                        destId = sessionSnap.getTrack(srcIdx).getMidiDestinationTrackId();
                    }
                }
                const ExperimentalInstrumentPlaybackEntry* const destEntry =
                    (destId != kInvalidTrackId)
                        ? playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap,
                                                                                        destId)
                        : nullptr;
                if (destEntry == nullptr || destEntry->host == nullptr)
                {
                    continue;
                }
                src.midiController->audioThread_scheduleTransportMidiForSegment(*destEntry->host,
                                                                                timelineStartAudible,
                                                                                audibleRun,
                                                                                silencePrefix,
                                                                                instrumentForceDiscontinuity,
                                                                                numSamples,
                                                                                nullptr,
                                                                                solo_mute_view::routedMidiSourceSuppressedBySolo(
                                                                                    soloView, src.trackId),
                                                                                solo_mute_view::trackForcedAudibleBySolo(
                                                                                    soloView, src.trackId));
            }
        }
    }

    if (instrumentSnap != nullptr)
    {
        for (int ti = 0; ti < sessionSnap.getNumTracks(); ++ti)
        {
            const Track& tr = sessionSnap.getTrack(ti);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const ExperimentalInstrumentPlaybackEntry* entry =
                playback_mix_helpers::findExperimentalInstrumentPlaybackEntry(*instrumentSnap, tr.getId());
            if (entry == nullptr || entry->host == nullptr)
            {
                continue;
            }

            if (tr.isTrackOff())
            {
                continue;
            }
            // Same as the realtime path: mute / fader −∞ are gain 0, the host still processes so
            // the offline render consumes exactly the MIDI it schedules (no leftover burst into
            // the realtime callback after the export). Solo layers on top as effective mute.
            const float fader = solo_mute_view::effectiveTrackMuted(soloView, tr)
                                    ? 0.0f
                                    : tr.getChannelFaderGain();

            const RoutingPlan::SourceStep* srcStep = nullptr;
            if (rp != nullptr)
            {
                for (const RoutingPlan::SourceStep& st : rp->sourceSteps)
                {
                    if (st.trackIndex == ti)
                    {
                        srcStep = &st;
                        break;
                    }
                }
            }
            if (postStripStagePtrs_[0] != nullptr && postStripStagePtrs_[1] != nullptr
                && postStripStageCapacity_ >= numSamples)
            {
                playback_mix_helpers::renderInstrumentPostStripToStereoScratch(entry->host,
                                                                               tr,
                                                                               postStripStagePtrs_[0],
                                                                               postStripStagePtrs_[1],
                                                                               0,
                                                                               numSamples,
                                                                               pluginHost_,
                                                                               nullptr,
                                                                               soloView);
                if (rp != nullptr && srcStep != nullptr && srcStep->destBusIndex >= 0
                    && srcStep->destBusIndex < static_cast<int>(rp->busScratchL.size()))
                {
                    playback_mix_helpers::fanPostStripStageToDryAndSends(postStripStagePtrs_[0],
                                                                           postStripStagePtrs_[1],
                                                                           0,
                                                                           numSamples,
                                                                           srcStep->destBusIndex,
                                                                           srcStep->sends,
                                                                           *rp);
                }
                else
                {
                    float* dryBusL = mixBusL;
                    float* dryBusR = mixBusR;
                    if (rp != nullptr)
                    {
                        const int destBi = destBusIndexForTrackInPlan(*rp, sessionSnap, ti);
                        if (destBi >= 0 && destBi < static_cast<int>(rp->busScratchL.size())
                            && rp->busScratchL[(size_t)destBi] != nullptr
                            && rp->busScratchR[(size_t)destBi] != nullptr)
                        {
                            dryBusL = rp->busScratchL[(size_t)destBi];
                            dryBusR = rp->busScratchR[(size_t)destBi];
                        }
                    }
                    if (dryBusL != nullptr && dryBusR != nullptr)
                    {
                        playback_mix_helpers::addPostStripStageToBus(postStripStagePtrs_[0],
                                                                       postStripStagePtrs_[1],
                                                                       dryBusL,
                                                                       dryBusR,
                                                                       0,
                                                                       numSamples,
                                                                       1.0f);
                    }
                    else
                    {
                        float* const stageStereo[2] = { postStripStagePtrs_[0], postStripStagePtrs_[1] };
                        playback_mix_helpers::addPostStripStageToDeviceOutputs(stageStereo,
                                                                               0,
                                                                               numSamples,
                                                                               2,
                                                                               stereoOutputLR);
                    }
                }
            }
            else
            {
                float* const* instMixTarget = mixSumTarget;
                float* instBusPtrs[2] = { mixBusL, mixBusR };
                if (rp != nullptr)
                {
                    const int destBi = destBusIndexForTrackInPlan(*rp, sessionSnap, ti);
                    if (destBi >= 0 && destBi < static_cast<int>(rp->busScratchL.size())
                        && rp->busScratchL[(size_t)destBi] != nullptr
                        && rp->busScratchR[(size_t)destBi] != nullptr)
                    {
                        instBusPtrs[0] = rp->busScratchL[(size_t)destBi];
                        instBusPtrs[1] = rp->busScratchR[(size_t)destBi];
                        instMixTarget = instBusPtrs;
                    }
                }

                playback_mix_helpers::mixExperimentalInstrumentAfterTracks(
                    entry->host, instMixTarget, 2, numSamples, fader, tr.getStereoPan());
            }
        }
    }

    ensurePostStripStageScratchCapacity(juce::jmax(numSamples, kOfflineMixdownBlockCapSamples));
    if (rp != nullptr && !rp->busSteps.empty() && postStripStagePtrs_[0] != nullptr
        && postStripStagePtrs_[1] != nullptr && postStripStageCapacity_ >= numSamples)
    {
        float* const stageStereo[2] = { postStripStagePtrs_[0], postStripStagePtrs_[1] };
        for (const RoutingPlan::BusStep& step : rp->busSteps)
        {
            if (step.sourceBusIndex < 0
                || step.sourceBusIndex >= static_cast<int>(rp->busScratchL.size()))
            {
                continue;
            }
            // C2B: stale-plan guard (see live finalize staged bus loop).
            if (step.trackIndex < 0 || step.trackIndex >= sessionSnap.getNumTracks())
            {
                continue;
            }
            const Track& busTr = sessionSnap.getTrack(step.trackIndex);
            if (busTr.getKind() != TrackKind::Group && busTr.getKind() != TrackKind::Master)
            {
                continue;
            }
            float* const busStereo[2] = { rp->busScratchL[(size_t)step.sourceBusIndex],
                                          rp->busScratchR[(size_t)step.sourceBusIndex] };
            playback_mix_helpers::applyBusPostChannelStripFromInputToStage(busTr,
                                                                           busStereo,
                                                                           postStripStagePtrs_[0],
                                                                           postStripStagePtrs_[1],
                                                                           0,
                                                                           numSamples,
                                                                           pluginHost_,
                                                                           soloView);
            if (step.destBusIndex < 0)
            {
                playback_mix_helpers::addPostStripStageToDeviceOutputs(
                    stageStereo, 0, numSamples, 2, stereoOutputLR);
            }
            else
            {
                playback_mix_helpers::fanPostStripStageToDryAndSends(postStripStagePtrs_[0],
                                                                       postStripStagePtrs_[1],
                                                                       0,
                                                                       numSamples,
                                                                       step.destBusIndex,
                                                                       step.sends,
                                                                       *rp);
            }
        }
    }
    else if (rp != nullptr && !rp->busSteps.empty())
    {
        for (const RoutingPlan::BusStep& step : rp->busSteps)
        {
            if (step.sourceBusIndex < 0
                || step.sourceBusIndex >= static_cast<int>(rp->busScratchL.size()))
            {
                continue;
            }
            // C2B: stale-plan guard (see live finalize staged bus loop).
            if (step.trackIndex < 0 || step.trackIndex >= sessionSnap.getNumTracks())
            {
                continue;
            }
            const Track& busTr = sessionSnap.getTrack(step.trackIndex);
            if (busTr.getKind() != TrackKind::Group && busTr.getKind() != TrackKind::Master)
            {
                continue;
            }
            float* const busStereo[2] = { rp->busScratchL[(size_t)step.sourceBusIndex],
                                          rp->busScratchR[(size_t)step.sourceBusIndex] };
            if (step.destBusIndex < 0)
            {
                playback_mix_helpers::processBusChannelStripToOutputs(busTr,
                                                                      busStereo,
                                                                      0,
                                                                      numSamples,
                                                                      2,
                                                                      stereoOutputLR,
                                                                      pluginHost_,
                                                                      soloView);
            }
            else if (step.destBusIndex < static_cast<int>(rp->busScratchL.size()))
            {
                float* const destStereo[2] = { rp->busScratchL[(size_t)step.destBusIndex],
                                               rp->busScratchR[(size_t)step.destBusIndex] };
                playback_mix_helpers::processBusChannelStripToOutputs(
                    busTr, busStereo, 0, numSamples, 2, destStereo, pluginHost_, soloView);
            }
        }
    }
    else if (masterTrackPtr != nullptr && mixSumTarget != stereoOutputLR)
    {
        playback_mix_helpers::processBusChannelStripToOutputs(*masterTrackPtr,
                                                              mixSumTarget,
                                                              0,
                                                              numSamples,
                                                              2,
                                                              stereoOutputLR,
                                                              pluginHost_,
                                                              soloView);
    }
}

void PlaybackEngine::ensureRoutingBusScratchPool(const std::size_t numBuses,
                                                 const int numSamples) noexcept
{
    // Stability C4B: grow-only, replace-not-mutate. A previously published RoutingPlan may still be
    // in use on the audio thread with pointers into these slots, so this function must never free
    // or reallocate an existing slot's buffer. Slots that are too small are swapped out for fresh
    // ones; the retired slot stays alive as long as any plan co-owns it (RoutingPlan::busScratchOwners).
    if (numBuses == 0 || numSamples <= 0)
    {
        return; // keep existing capacity; unused slots are retained deliberately
    }
    if (routingBusScratch_.size() < numBuses)
    {
        routingBusScratch_.resize(numBuses); // moves shared_ptrs only; slot objects never relocate
    }
    for (std::size_t i = 0; i < numBuses; ++i)
    {
        std::shared_ptr<RoutingBusScratchSlot>& slot = routingBusScratch_[i];
        if (slot == nullptr || slot->buf.getNumSamples() < numSamples
            || slot->buf.getNumChannels() < 2)
        {
            auto fresh = std::make_shared<RoutingBusScratchSlot>();
            fresh->buf.setSize(2, numSamples, false, false, true);
            fresh->ptrs[0] = fresh->buf.getWritePointer(0);
            fresh->ptrs[1] = fresh->buf.getWritePointer(1);
            slot = std::move(fresh);
        }
    }
}

void PlaybackEngine::rebuildRoutingPlanFromSession() noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        routingPlan_.store(nullptr, std::memory_order_release);
        return;
    }
    std::size_t busCount = 0;
    for (int i = 0; i < snap->getNumTracks(); ++i)
    {
        const TrackKind k = snap->getTrack(i).getKind();
        if (k == TrackKind::Group || k == TrackKind::Master)
        {
            ++busCount;
        }
    }
    const int cap = juce::jmax(masterScratchCapacity_, kOfflineMixdownBlockCapSamples);
    ensureRoutingBusScratchPool(busCount, cap);
    ensurePostStripStageScratchCapacity(cap);
    // C4B: the plan co-owns its slots so the audio thread can outlive later pool changes.
    std::vector<std::pair<float*, float*>> scratchPairs;
    std::vector<std::shared_ptr<void>> scratchOwners;
    scratchPairs.reserve(busCount);
    scratchOwners.reserve(busCount);
    for (std::size_t i = 0; i < busCount && i < routingBusScratch_.size(); ++i)
    {
        const std::shared_ptr<RoutingBusScratchSlot>& slot = routingBusScratch_[i];
        scratchPairs.emplace_back(slot->ptrs[0], slot->ptrs[1]);
        scratchOwners.push_back(slot);
    }
    const std::shared_ptr<const RoutingPlan> plan
        = routing_plan_builder::build(*snap, scratchPairs, std::move(scratchOwners));
    routingPlan_.store(plan, std::memory_order_release);
}

int PlaybackEngine::destBusIndexForTrackInPlan(const RoutingPlan& plan,
                                               const SessionSnapshot& snap,
                                               const int trackIndex) const noexcept
{
    juce::ignoreUnused(snap);
    for (const RoutingPlan::SourceStep& step : plan.sourceSteps)
    {
        if (step.trackIndex == trackIndex)
        {
            return step.destBusIndex;
        }
    }
    return static_cast<int>(plan.masterBusIndex);
}
