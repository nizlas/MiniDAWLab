#pragma once

// =============================================================================
// ProxyPlaybackCoordinator — P1G per-destination playback-source coordination
// (steering §7.3 priority, §12.3 currency, §15.6 EOF/silent, PI-030/PI-031)
// =============================================================================
//
// MESSAGE-THREAD owner of the runtime proxy playback state for every instrument
// destination:
//   * evaluates the authoritative source selection (`decideProxyPlaybackSource`)
//     from live Primary availability + persisted v20 generation identity;
//   * builds/retires `ProxyPlaybackReader`s (derived cache state — discarding or
//     rebuilding them NEVER affects generation currency);
//   * publishes one immutable `ProxyPlaybackView` per destination into the
//     host's atomic slot (block-boundary source switch);
//   * owns the single shared `ProxyPlaybackIoService` fill thread.
//
// OWNERSHIP / LIFETIME: owned by the same project-runtime owner as
// `AppProxyRenderEngine` (the content view alongside the instrument runtime).
// It deliberately holds NO Session/host/controller/UI references — every model
// access goes through injected std::function dependencies, so the coordinator
// is deterministic-testable and can never dangle into UI state. `shutdown()`
// (or destruction) unpublishes every view, drains the audio callback, and only
// then releases readers — the file handles and rings are always destroyed OFF
// the audio thread.
//
// CURRENCY INPUTS (never guessed): recomputed expected fingerprint under the
// generation's RECORDED configuration (§12.3, missing-Primary case), persisted
// generation fingerprint + schema/algorithm versions, recorded Primary
// identity/version, persisted save-pairing (or in-session publication), and
// asset/silent-generation validity — all against the CURRENT session snapshot.
// Explicitly NOT inputs: latest filename, metadata presence alone, project
// dirty state, device/engine rate, fresh plugin-state blob hashes.
//
// ENGINE-RATE ADAPTATION (PI-030): the reader's consumption domain is the
// ENGINE rate (transport segments arrive in engine frames), so the mapping is
// ratio = assetRate / engineRate. An engine-rate change rebuilds readers as
// derived state via `notifyEngineRateMaybeChanged` — the generation itself
// stays Current; until the rebuilt reader is primed the honest runtime status
// is ProxyPreparing (silence, never wrong-speed or stale audio).

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/ProxyAssetStore.h"
#include "instruments/ProxyFingerprint.h"
#include "instruments/ProxyPlaybackReader.h"
#include "instruments/ProxyPlaybackSource.h"
#include "io/ProjectFile.h"

namespace proxy_playback
{

class ProxyPlaybackCoordinator final
{
public:
    /// All dependencies are message-thread callables; none may retain UI state.
    struct Dependencies
    {
        /// Current published session snapshot (authoritative musical content).
        std::function<std::shared_ptr<const SessionSnapshot>()> sessionSnapshotProvider;
        /// Project folder for resolving safe proxy-relative paths (invalid File when unsaved).
        std::function<juce::File()> projectFolderProvider;
        /// TLD-1 timeline reference rate with fallback (Session::timelineSampleRateOr).
        std::function<double(double)> timelineRateOrFallback;
        /// True when the destination still exists in the runtime (host lookup succeeds).
        std::function<bool(TrackId)> destinationExists;
        /// True when the destination's live Primary is loaded and usable.
        std::function<bool(TrackId)> primaryUsable;
        /// Publish one immutable view into the destination's audio-thread slot
        /// (ExperimentalInstrumentHost::setProxyPlaybackView; nullptr = live Primary).
        std::function<void(TrackId, std::shared_ptr<const ProxyPlaybackView>)> publishView;
        /// Persisted v20 proxy metadata of the destination (nullptr = none).
        std::function<const ProjectFileProxyMetadataV20*(TrackId)> proxyMetadataForTrack;
        /// True when the destination's generation was published in THIS session (§12.3 pairing).
        std::function<bool(TrackId)> proxyPublishedThisSession;
        /// Stored MIDI clips of any track id (destination + routed sources).
        std::function<std::vector<const InstrumentMidiClip*>(TrackId)> clipsForTrack;
        /// Current engine/device sample rate; <= 0 while no device is running.
        std::function<double()> engineRateProvider;
    };

    explicit ProxyPlaybackCoordinator(Dependencies deps) : deps_(std::move(deps)) {}

    ~ProxyPlaybackCoordinator() { shutdown(); }

    //==========================================================================
    // Refresh seams (message thread)
    //==========================================================================
    /// Re-evaluate and publish the source decision for one destination. Call on:
    /// project load, publication success, Primary load/unload/replacement,
    /// render-relevant edits (currency change), and engine-rate change.
    void refreshDestination(const TrackId destination)
    {
        purgeRetired(false);
        if (!deps_.destinationExists || !deps_.destinationExists(destination))
        {
            forget(destination);
            return;
        }
        publish(destination, evaluateDestination(destination));
    }

    /// Re-evaluate every `TrackKind::Instrument` destination in the session, and
    /// retire runtime state of destinations that no longer exist (track deletion
    /// funnels through here via the instrument-UI refresh).
    void refreshAllInstrumentDestinations()
    {
        const auto sessionSnap
            = deps_.sessionSnapshotProvider ? deps_.sessionSnapshotProvider() : nullptr;
        if (sessionSnap == nullptr)
        {
            return;
        }
        std::set<TrackId> live;
        for (int i = 0; i < sessionSnap->getNumTracks(); ++i)
        {
            const Track& t = sessionSnap->getTrack(i);
            if (t.getKind() == TrackKind::Instrument)
            {
                live.insert(t.getId());
                refreshDestination(t.getId());
            }
        }
        std::vector<TrackId> vanished;
        for (const auto& [tid, p] : published_)
        {
            if (live.count(tid) == 0)
            {
                vanished.push_back(tid);
            }
        }
        for (const TrackId tid : vanished)
        {
            notifyTrackRemoved(tid);
        }
    }

    /// Engine/device rate changed: readers are derived state under the OLD rate —
    /// rebuild them under the new rate (generation currency is untouched, PI-030).
    void notifyEngineRateMaybeChanged() { refreshAllInstrumentDestinations(); }

    /// Destination removed: unpublish and retire its runtime state.
    void notifyTrackRemoved(const TrackId destination)
    {
        if (deps_.publishView && deps_.destinationExists && deps_.destinationExists(destination))
        {
            deps_.publishView(destination, nullptr);
        }
        forget(destination);
    }

    //==========================================================================
    // Runtime status (message thread; immutable value for P1I)
    //==========================================================================
    /// Honest runtime source/status. Refines the published decision with LIVE
    /// reader health: stream failure -> ProxyCorrupt; underruns observed since
    /// the previous query -> PlaybackUnderrun (one-shot, then recovery shows
    /// ProxyCurrent again); window not primed at the transport position ->
    /// ProxyPreparing.
    [[nodiscard]] ProxyPlaybackSourceState runtimeStateForTrack(const TrackId destination)
    {
        const auto it = published_.find(destination);
        if (it == published_.end())
        {
            return (deps_.primaryUsable && deps_.primaryUsable(destination)
                    && !isForcedUnavailable(destination))
                       ? ProxyPlaybackSourceState::Primary
                       : ProxyPlaybackSourceState::MissingPrimary;
        }
        PublishedDestination& p = it->second;
        if (p.view == nullptr || !p.view->useProxy || p.view->reader == nullptr)
        {
            return p.state;
        }
        const ProxyPlaybackReader& r = *p.view->reader;
        if (r.streamFailed())
        {
            return ProxyPlaybackSourceState::ProxyCorrupt;
        }
        const std::uint64_t u = r.underrunCount();
        if (u > p.lastSeenUnderruns)
        {
            p.lastSeenUnderruns = u;
            return ProxyPlaybackSourceState::PlaybackUnderrun;
        }
        if (!r.isReadyAtDesired())
        {
            return ProxyPlaybackSourceState::ProxyPreparing;
        }
        return p.state;
    }

    /// The currently published view of a destination (message-thread reads; tests/P1I).
    [[nodiscard]] std::shared_ptr<const ProxyPlaybackView>
        publishedViewForTrack(const TrackId destination) const
    {
        const auto it = published_.find(destination);
        return it != published_.end() ? it->second.view : nullptr;
    }

    //==========================================================================
    // Test / integration hooks (message thread)
    //==========================================================================
    /// Simulate "Primary unavailable" WITHOUT unloading the plugin (keeps the
    /// expected musical identity intact — end-to-end integration step 4).
    void setPrimaryForcedUnavailableForTests(const TrackId destination, const bool unavailable)
    {
        if (unavailable)
        {
            forcedUnavailable_.insert(destination);
        }
        else
        {
            forcedUnavailable_.erase(destination);
        }
    }

    /// Tests: destroy retired readers immediately after the audio drain.
    void purgeRetiredForTests()
    {
        juce::Thread::sleep(kAudioDrainMs);
        purgeRetired(true);
    }

    [[nodiscard]] size_t retiredCountForTests() const { return retired_.size(); }
    [[nodiscard]] ProxyPlaybackIoService* ioServiceForTests() noexcept { return ioService_.get(); }

    //==========================================================================
    // Shutdown (message thread) — deterministic order
    //==========================================================================
    /// 1) unpublish every view; 2) drain the audio callback (in-flight callbacks
    /// hold view refs for at most one block); 3) unregister + destroy readers
    /// off-audio; 4) stop the I/O thread. Safe to call repeatedly.
    void shutdown()
    {
        bool anyLive = false;
        for (const auto& [tid, p] : published_)
        {
            if (deps_.publishView && deps_.destinationExists && deps_.destinationExists(tid))
            {
                deps_.publishView(tid, nullptr);
                anyLive = anyLive || (p.view != nullptr && p.view->useProxy);
            }
            if (p.reader != nullptr && ioService_ != nullptr)
            {
                ioService_->unregisterReader(p.reader.get());
            }
        }
        if (anyLive || !retired_.empty())
        {
            juce::Thread::sleep(kAudioDrainMs);
        }
        published_.clear();
        purgeRetired(true);
        if (ioService_ != nullptr)
        {
            ioService_->shutdown();
            ioService_.reset();
        }
    }

private:
    /// A worst-case realistic audio callback is ~21 ms (1024 @ 48 kHz); after the
    /// view swap plus this drain, no callback can still hold a retired view ref,
    /// so the last shared_ptr release (file close) happens on THIS thread.
    static constexpr int kAudioDrainMs = 30;

    struct Evaluation
    {
        ProxySourceDecision decision;
        std::shared_ptr<ProxyPlaybackReader> reader; ///< only when a WAV asset is used
        bool silentGeneration = false;
        juce::String generationId;
        juce::String cacheKey; ///< generation + engine rate (reader reuse/coalescing)
    };

    struct PublishedDestination
    {
        std::shared_ptr<const ProxyPlaybackView> view;
        std::shared_ptr<ProxyPlaybackReader> reader;
        ProxyPlaybackSourceState state = ProxyPlaybackSourceState::MissingPrimary;
        juce::String cacheKey;
        std::uint64_t lastSeenUnderruns = 0;
    };

    struct RetiredReader
    {
        std::shared_ptr<const ProxyPlaybackView> view;
        std::shared_ptr<ProxyPlaybackReader> reader;
        double retireDeadlineMs = 0.0;
    };

    [[nodiscard]] bool isForcedUnavailable(const TrackId t) const
    {
        return forcedUnavailable_.count(t) != 0;
    }

    /// Full §7.3/§12.3 evaluation for one destination (pure inputs -> decision +
    /// prepared reader). Reuses the existing reader when generation AND engine
    /// rate are unchanged (repeated refreshes must not churn derived state).
    [[nodiscard]] Evaluation evaluateDestination(const TrackId destination)
    {
        Evaluation ev;
        const bool primaryOk = deps_.primaryUsable && deps_.primaryUsable(destination)
                               && !isForcedUnavailable(destination);
        const ProjectFileProxyMetadataV20* const meta
            = deps_.proxyMetadataForTrack ? deps_.proxyMetadataForTrack(destination) : nullptr;

        // ---- currency (only decides anything when Primary is unavailable) ----
        ProxyCurrencyVerdict currency = ProxyCurrencyVerdict::NoMetadata;
        if (meta != nullptr)
        {
            currency = ProxyCurrencyVerdict::Stale;
            if (!primaryOk)
            {
                currency = evaluateMissingPrimaryCurrency(destination, *meta);
            }
        }

        // ---- asset availability (only for a Current generation) ----
        ProxyAssetAvailability asset = ProxyAssetAvailability::Missing;
        if (!primaryOk && meta != nullptr && currency == ProxyCurrencyVerdict::Current)
        {
            asset = evaluateAssetAvailability(destination, *meta, ev);
        }

        ev.decision = decideProxyPlaybackSource(primaryOk, currency, asset);
        if (meta != nullptr)
        {
            ev.generationId = meta->generationId;
        }
        if (!ev.decision.useProxy)
        {
            ev.reader = nullptr; // never hand a reader to a non-proxy decision
            ev.cacheKey.clear();
        }
        return ev;
    }

    /// §12.3 missing-Primary currency: recompute the expected fingerprint UNDER
    /// THE GENERATION'S RECORDED CONFIGURATION and require the persisted
    /// save-pairing (or in-session publication). Schema/algorithm version drift
    /// makes recomputation incomparable -> conservatively Stale.
    [[nodiscard]] ProxyCurrencyVerdict
        evaluateMissingPrimaryCurrency(const TrackId destination,
                                       const ProjectFileProxyMetadataV20& meta) const
    {
        if (meta.fingerprintSchemaVersion != (int)proxy_fingerprint::kFingerprintSchemaVersion
            || meta.fingerprintAlgorithmId != (int)proxy_fingerprint::kFingerprintAlgorithmId)
        {
            return ProxyCurrencyVerdict::Stale;
        }
        const bool publishedThisSession
            = deps_.proxyPublishedThisSession && deps_.proxyPublishedThisSession(destination);
        if (!proxyStatePairingHolds(meta, publishedThisSession))
        {
            return ProxyCurrencyVerdict::Stale;
        }
        const auto sessionSnap
            = deps_.sessionSnapshotProvider ? deps_.sessionSnapshotProvider() : nullptr;
        if (sessionSnap == nullptr || !deps_.clipsForTrack)
        {
            return ProxyCurrencyVerdict::Stale;
        }
        const double fallbackRate
            = deps_.timelineRateOrFallback ? deps_.timelineRateOrFallback(48000.0) : 48000.0;
        const juce::String expected = computeExpectedFingerprintUnderRecordedConfig(
            *sessionSnap, destination, deps_.clipsForTrack, meta, fallbackRate);
        if (expected.isEmpty() || meta.generationId.isEmpty() || expected != meta.generationId)
        {
            return ProxyCurrencyVerdict::Stale;
        }
        return ProxyCurrencyVerdict::Current;
    }

    /// Asset validity for a CURRENT generation; fills `ev.reader` for WAV assets.
    [[nodiscard]] ProxyAssetAvailability
        evaluateAssetAvailability(const TrackId destination,
                                  const ProjectFileProxyMetadataV20& meta, Evaluation& ev)
    {
        if (meta.silentGeneration)
        {
            // §15.7: a valid silent generation carries NO path and a sane length.
            // Malformed silent metadata (fake path / negative length) is rejected.
            ev.silentGeneration = true;
            return (meta.relativePath.isEmpty() && meta.lengthSamples >= 0)
                       ? ProxyAssetAvailability::SilentGeneration
                       : ProxyAssetAvailability::Corrupt;
        }
        if (!proxy_store::isSafeProxyRelativePath(meta.relativePath))
        {
            return ProxyAssetAvailability::Corrupt;
        }
        const juce::File projectFolder
            = deps_.projectFolderProvider ? deps_.projectFolderProvider() : juce::File();
        const juce::File assetFile
            = proxy_store::resolveProxyRelativePath(projectFolder, meta.relativePath);
        if (assetFile == juce::File())
        {
            return ProxyAssetAvailability::Corrupt;
        }
        if (!assetFile.existsAsFile())
        {
            return ProxyAssetAvailability::Missing;
        }

        const double engineRate = deps_.engineRateProvider ? deps_.engineRateProvider() : 0.0;
        ProxyStreamMapping mapping;
        mapping.assetRate = meta.sampleRate;
        // Consumption domain = ENGINE rate (transport segments arrive in engine
        // frames). No device yet -> map 1:1 against the asset rate; the reader is
        // rebuilt via notifyEngineRateMaybeChanged when a device starts.
        mapping.timelineRate = engineRate > 0.0 ? engineRate : meta.sampleRate;
        mapping.assetLengthFrames = meta.lengthSamples;

        ev.cacheKey = meta.generationId + ":" + juce::String(mapping.timelineRate, 6);

        // Reuse the live reader when generation + engine rate are unchanged.
        const auto it = published_.find(destination);
        if (it != published_.end() && it->second.reader != nullptr
            && it->second.cacheKey == ev.cacheKey && !it->second.reader->streamFailed())
        {
            ev.reader = it->second.reader;
            return ProxyAssetAvailability::Available;
        }

        auto reader = std::make_shared<ProxyPlaybackReader>(assetFile, mapping);
        if (reader->openFailed())
        {
            return ProxyAssetAvailability::Corrupt;
        }
        ev.reader = std::move(reader);
        return ProxyAssetAvailability::Available;
    }

    /// Publish the evaluation as one immutable view; retire any replaced runtime state.
    void publish(const TrackId destination, const Evaluation& ev)
    {
        PublishedDestination next;
        next.state = ev.decision.state;
        next.cacheKey = ev.cacheKey;
        next.reader = ev.reader;

        if (ev.decision.useProxy)
        {
            auto view = std::make_shared<ProxyPlaybackView>();
            view->selectedState = ev.decision.state;
            view->useProxy = true;
            view->silentGeneration = ev.silentGeneration;
            view->reader = ev.reader;
            view->generationId = ev.generationId;
            next.view = view;
        }

        // Retire the replaced runtime state. The audio thread may hold a ref to
        // the OLD VIEW for the remainder of one callback — if we dropped ours now
        // the audio thread could perform the final release (a free). Both the old
        // view and (when replaced) the old reader therefore go through the
        // retire list and are destroyed on this thread after the drain deadline.
        const auto it = published_.find(destination);
        if (it != published_.end())
        {
            PublishedDestination& old = it->second;
            next.lastSeenUnderruns = old.lastSeenUnderruns;
            const bool readerReplaced = old.reader != nullptr && old.reader != next.reader;
            if (readerReplaced && ioService_ != nullptr)
            {
                ioService_->unregisterReader(old.reader.get());
            }
            if (old.view != nullptr || readerReplaced)
            {
                retired_.push_back({ std::move(old.view),
                                     readerReplaced ? std::move(old.reader) : nullptr,
                                     juce::Time::getMillisecondCounterHiRes() + kAudioDrainMs });
            }
        }

        const bool newReader
            = next.reader != nullptr
              && (it == published_.end() || it->second.reader != next.reader);
        if (newReader)
        {
            if (ioService_ == nullptr)
            {
                ioService_ = std::make_unique<ProxyPlaybackIoService>();
            }
            ioService_->registerReader(next.reader);
        }

        if (deps_.publishView)
        {
            deps_.publishView(destination, next.view); // nullptr => live Primary semantics
        }
        published_[destination] = std::move(next);
    }

    /// Remove all runtime state for a destination (host view already cleared).
    void forget(const TrackId destination)
    {
        const auto it = published_.find(destination);
        if (it == published_.end())
        {
            return;
        }
        if (it->second.reader != nullptr && ioService_ != nullptr)
        {
            ioService_->unregisterReader(it->second.reader.get());
        }
        if (it->second.view != nullptr || it->second.reader != nullptr)
        {
            retired_.push_back({ std::move(it->second.view), std::move(it->second.reader),
                                 juce::Time::getMillisecondCounterHiRes() + kAudioDrainMs });
        }
        published_.erase(it);
    }

    /// Destroy retired readers whose audio-drain deadline passed (message thread —
    /// the destructor closes the file handle, so this must never run on audio).
    void purgeRetired(const bool force)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        for (size_t i = retired_.size(); i > 0; --i)
        {
            if (force || retired_[i - 1].retireDeadlineMs <= now)
            {
                retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i - 1));
            }
        }
    }

    Dependencies deps_;
    std::unique_ptr<ProxyPlaybackIoService> ioService_; ///< lazy; one thread total
    std::map<TrackId, PublishedDestination> published_;
    std::vector<RetiredReader> retired_;
    std::set<TrackId> forcedUnavailable_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProxyPlaybackCoordinator)
};

} // namespace proxy_playback
