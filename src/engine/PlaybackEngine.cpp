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
}

void PlaybackEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device != nullptr)
    {
        const double sr = device->getCurrentSampleRate();
        const int bs = device->getCurrentBufferSizeSamples();
        const int nOut = device->getActiveOutputChannels().countNumberOfSetBits();
        ensureMasterScratchCapacity(juce::jmax(bs, kOfflineMixdownBlockCapSamples));
        if (pluginHost_ != nullptr)
        {
            pluginHost_->prepareForDevice(sr, bs, nOut);
        }
        if (experimentalPrepareAllHosts_)
        {
            experimentalPrepareAllHosts_(sr, bs);
        }
        rebuildRoutingPlanFromSession();
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

    if (offlineRenderInProgress_.load(std::memory_order_acquire))
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (float* row = outputChannelData[ch])
            {
                juce::FloatVectorOperations::clear(row, numSamples);
            }
        }
        transport_.audioThread_advancePlayheadIfPlaying(0);
        return;
    }

    const std::shared_ptr<const SessionSnapshot> sessionSnap = session_.loadSessionSnapshotForAudioThread();
    /// [Audio thread] Same publish discipline as Session: acquire-load retains a const view for this block only.
    const std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> instrumentSnap
        = experimentalInstrumentPlaybackSnapshot_.load(std::memory_order_acquire);

    // Per-block RT MIDI delivery uses `audioCallbackBlockSamples_` (see `ExperimentalInstrumentHost`). The
    // snapshot is the engine's source of truth for which host(s) are driven this block — call `beginAudioBlock`
    // here before the optional map/staging lambda so message-thread map drift cannot skip the active host.
    invokeExperimentalInstrumentBeginBlocks(instrumentSnap.get(), deviceBlockSizeInFrames);

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

    if (countIn_ != nullptr)
    {
        countIn_->audioThread_mixInto(mixSumTarget, numOutputChannels, numSamples);
    }

    const auto finalizeRoutingToDevice = [&]() noexcept
    {
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
        if (rp != nullptr && sessionSnap != nullptr && !rp->busSteps.empty())
        {
            for (const RoutingPlan::BusStep& step : rp->busSteps)
            {
                if (step.sourceBusIndex < 0
                    || step.sourceBusIndex >= static_cast<int>(rp->busScratchL.size()))
                {
                    continue;
                }
                const Track& busTr = sessionSnap->getTrack(step.trackIndex);
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
                                                                          pluginHost_);
                }
                else if (step.destBusIndex < static_cast<int>(rp->busScratchL.size()))
                {
                    float* const destStereo[2] = { rp->busScratchL[(size_t)step.destBusIndex],
                                                   rp->busScratchR[(size_t)step.destBusIndex] };
                    playback_mix_helpers::processBusChannelStripToOutputs(
                        busTr, busStereo, 0, numSamples, 2, destStereo, pluginHost_);
                }
            }
            return;
        }
        if (masterTrackPtr != nullptr && mixSumTarget != outputChannelData)
        {
            playback_mix_helpers::processBusChannelStripToOutputs(*masterTrackPtr,
                                                                  mixSumTarget,
                                                                  0,
                                                                  numSamples,
                                                                  numOutputChannels,
                                                                  outputChannelData,
                                                                  pluginHost_);
        }
    };

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

    const auto mixInstrumentsAndFinalizeMaster = [&]() noexcept
    {
        mixKeyedInstrumentLanesIntoOutputsIfAny();
        finalizeRoutingToDevice();
    };

    if (sessionSnap == nullptr || deviceBlockSizeInFrames <= 0
        || playbackIntent != PlaybackIntent::Playing)
    {
        transport_.audioThread_advancePlayheadIfPlaying(0);
        mixInstrumentsAndFinalizeMaster();
        return;
    }

    const std::int64_t timelineEnd = sessionSnap->getArrangementExtentSamples();
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

        TrackId omitClipPlaybackForTrack = kInvalidTrackId;
        if (recorder_ != nullptr && recorder_->isRecording())
        {
            omitClipPlaybackForTrack = recorder_->getRecordingTrackId();
        }

        if (rp != nullptr && !rp->sourceSteps.empty())
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
                                                                             step.trackIndex);
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
                                                                         timelineEnd);
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
    mixInstrumentsAndFinalizeMaster();
}

void PlaybackEngine::invokeExperimentalInstrumentBeginBlocks(
    const ExperimentalInstrumentPlaybackSnapshot* instrumentSnap,
    const int numSamples) noexcept
{
    if (instrumentSnap != nullptr)
    {
        for (const auto& e : instrumentSnap->entries)
        {
            if (e.host != nullptr)
            {
                e.host->audioThread_beginAudioBlock(numSamples);
            }
        }
    }
    if (experimentalBeginBlockAllHosts_)
    {
        experimentalBeginBlockAllHosts_(numSamples);
    }
}

void PlaybackEngine::setOfflineRenderInProgress(const bool on) noexcept
{
    offlineRenderInProgress_.store(on, std::memory_order_release);
}

bool PlaybackEngine::isOfflineRenderInProgress() const noexcept
{
    return offlineRenderInProgress_.load(std::memory_order_acquire);
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

    invokeExperimentalInstrumentBeginBlocks(instrumentSnap, numSamples);
    ensureMasterScratchCapacity(juce::jmax(numSamples, kOfflineMixdownBlockCapSamples));

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
    std::vector<std::pair<float*, float*>> offlineScratchPairs;
    offlineScratchPairs.reserve(offlineBusCount);
    for (const RoutingBusScratchSlot& slot : routingBusScratch_)
    {
        offlineScratchPairs.emplace_back(slot.ptrs[0], slot.ptrs[1]);
    }
    const std::shared_ptr<const RoutingPlan> offlinePlan
        = routing_plan_builder::build(sessionSnap, offlineScratchPairs);
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

        if (rp != nullptr && !rp->sourceSteps.empty())
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
                                                                             step.trackIndex);
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
                                                                         sessionSnap.getArrangementExtentSamples());
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
                                                                                   nullptr);
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
            if (tr.isMuted())
            {
                continue;
            }

            const float fader = tr.getChannelFaderGain();
            if (fader <= 0.0f)
            {
                continue;
            }

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

    if (rp != nullptr && !rp->busSteps.empty())
    {
        for (const RoutingPlan::BusStep& step : rp->busSteps)
        {
            if (step.sourceBusIndex < 0
                || step.sourceBusIndex >= static_cast<int>(rp->busScratchL.size()))
            {
                continue;
            }
            const Track& busTr = sessionSnap.getTrack(step.trackIndex);
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
                                                                      pluginHost_);
            }
            else if (step.destBusIndex < static_cast<int>(rp->busScratchL.size()))
            {
                float* const destStereo[2] = { rp->busScratchL[(size_t)step.destBusIndex],
                                               rp->busScratchR[(size_t)step.destBusIndex] };
                playback_mix_helpers::processBusChannelStripToOutputs(
                    busTr, busStereo, 0, numSamples, 2, destStereo, pluginHost_);
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
                                                              pluginHost_);
    }
}

void PlaybackEngine::ensureRoutingBusScratchPool(const std::size_t numBuses,
                                                 const int numSamples) noexcept
{
    if (numBuses == 0 || numSamples <= 0)
    {
        routingBusScratch_.clear();
        return;
    }
    routingBusScratch_.resize(numBuses);
    for (RoutingBusScratchSlot& slot : routingBusScratch_)
    {
        if (slot.buf.getNumSamples() < numSamples || slot.buf.getNumChannels() < 2)
        {
            slot.buf.setSize(2, numSamples, false, false, true);
        }
        slot.ptrs[0] = slot.buf.getWritePointer(0);
        slot.ptrs[1] = slot.buf.getWritePointer(1);
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
    std::vector<std::pair<float*, float*>> scratchPairs;
    scratchPairs.reserve(busCount);
    for (const RoutingBusScratchSlot& slot : routingBusScratch_)
    {
        scratchPairs.emplace_back(slot.ptrs[0], slot.ptrs[1]);
    }
    const std::shared_ptr<const RoutingPlan> plan = routing_plan_builder::build(*snap, scratchPairs);
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
