// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE. See Spike01StateCapturePanel.h.
// ============================================================================

#include "diagnostics/Spike01StateCapturePanel.h"

#include "diagnostics/Spike01MidiDeliveryCounters.h"
#include "diagnostics/Spike01ReportFormat.h"
#include "diagnostics/Spike01Sha256.h"
#include "instruments/ProxyAssetStore.h" // P1EF plan verification (path safety + asset check)
#include "instruments/ProxyPlaybackSource.h" // P1G plan verification (runtime source states)
#include "plugins/ExperimentalInstrumentHost.h"
#include "util/AsyncLifetimeToken.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace
{
    [[nodiscard]] juce::String nowIso()
    {
        return juce::Time::getCurrentTime().toISO8601(true);
    }

    [[nodiscard]] bool onMessageThreadNow()
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        return mm != nullptr && mm->isThisTheMessageThread();
    }

    /// Incremental sanitized session log so an app restart mid-procedure can never lose
    /// measurements (the panel's in-memory samples die with the process). Same privacy rules
    /// as the report: sizes/hashes/timings/metadata only — never raw state bytes.
    /// Appends are message-thread-only (callers ensure this); the lock is belt-and-braces.
    void appendSessionLog(const juce::String& line)
    {
        static juce::CriticalSection logLock;
        const juce::ScopedLock sl(logLock);
        const juce::File f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("MiniDAWLab")
                                 .getChildFile("spike01-capture-log.txt");
        f.getParentDirectory().createDirectory();
        f.appendText(nowIso() + "  " + line + "\n", false, false, "\n");
    }
} // namespace

//==============================================================================
class Spike01StateCapturePanel::Content final : public juce::Component,
                                                private juce::AudioProcessorListener,
                                                private juce::Timer
{
public:
    explicit Content(Spike01PanelCallbacks callbacks, juce::String autoPlanId)
        : callbacks_(std::move(callbacks)), autoPlanId_(std::move(autoPlanId))
    {
        auto initLabel = [this](juce::Label& l, const juce::String& text) {
            l.setText(text, juce::dontSendNotification);
            l.setFont(juce::Font(juce::FontOptions(13.0f)));
            addAndMakeVisible(l);
        };

        initLabel(headerLabel_,
                  "SPIKE-01 state-capture probe (diagnostic; writes sanitized report only)");
        headerLabel_.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));

        addAndMakeVisible(trackBox_);
        trackBox_.setTextWhenNothingSelected("(select instrument track)");

        refreshTracksButton_.setButtonText("Refresh tracks");
        refreshTracksButton_.onClick = [this] { refreshTrackList(); };
        addAndMakeVisible(refreshTracksButton_);

        addAndMakeVisible(phaseBox_);
        int itemId = 1;
        for (const auto& p : spike01::requiredPhases())
        {
            phaseBox_.addItem(juce::String(p.label), itemId++);
        }
        phaseBox_.addItem("custom (type below)", itemId);
        phaseBox_.setSelectedId(1, juce::dontSendNotification);

        customPhaseEditor_.setTextToShowWhenEmpty("custom phase id (when 'custom' selected)",
                                                  juce::Colours::grey);
        addAndMakeVisible(customPhaseEditor_);

        capture1Button_.setButtonText("Capture x1 (raw)");
        capture1Button_.onClick = [this] { captureRaw(1); };
        addAndMakeVisible(capture1Button_);

        capture10Button_.setButtonText("Capture x10 (raw)");
        capture10Button_.onClick = [this] { captureRaw(10); };
        addAndMakeVisible(capture10Button_);

        saveCaptureButton_.setButtonText("Capture via Save path (base64)");
        saveCaptureButton_.onClick = [this] { captureViaSavePath(); };
        addAndMakeVisible(saveCaptureButton_);

        checkpointButton_.setButtonText("F1 snapshot checkpoint (raw + Save path)");
        checkpointButton_.onClick = [this] { runSnapshotCheckpoint(); };
        addAndMakeVisible(checkpointButton_);

        listenerButton_.setButtonText("Attach parameter listener");
        listenerButton_.onClick = [this] { toggleListener(); };
        addAndMakeVisible(listenerButton_);

        noteEditor_.setTextToShowWhenEmpty("operator note (e.g. 'moved drawbar 16 to 0 and back')",
                                           juce::Colours::grey);
        addAndMakeVisible(noteEditor_);

        addNoteButton_.setButtonText("Add note");
        addNoteButton_.onClick = [this] {
            const juce::String t = noteEditor_.getText().trim();
            if (t.isNotEmpty())
            {
                notes_.push_back((nowIso() + "  " + t).toStdString());
                noteEditor_.clear();
                reportWrittenSinceLastData_ = false;
                appendSessionLog("note: " + t);
                setStatus("Note recorded (" + juce::String((int)notes_.size()) + " total).");
            }
        };
        addAndMakeVisible(addNoteButton_);

        writeReportButton_.setButtonText("Write sanitized report (Desktop)");
        writeReportButton_.onClick = [this] { writeReport(); };
        addAndMakeVisible(writeReportButton_);

        initLabel(statusLabel_, "Ready. Select the VB3-II track, then follow the procedure in "
                                "docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md.");
        statusLabel_.setJustificationType(juce::Justification::topLeft);
        initLabel(listenerLabel_, "Listener: detached.");
        listenerLabel_.setJustificationType(juce::Justification::topLeft);

        appendSessionLog("=== SPIKE-01 session start; appVersion="
                         + (juce::JUCEApplication::getInstance() != nullptr
                                ? juce::JUCEApplication::getInstance()->getApplicationVersion()
                                : juce::String("unknown"))
                         + (autoPlanId_.isNotEmpty() ? " autoPlan=" + autoPlanId_ : juce::String())
                         + " ===");
        refreshTrackList();
        setSize(640, 470);

        if (autoPlanId_.isNotEmpty())
        {
            buildAutoPlan();
            if (autoSteps_.empty())
            {
                appendSessionLog("auto: ABORT — unknown plan '" + autoPlanId_ + "'");
            }
            else
            {
                autoActive_ = true;
                setStatus("AUTO plan '" + autoPlanId_ + "' armed ("
                          + juce::String((int)autoSteps_.size())
                          + " steps). Do not interact until it reports COMPLETE.");
                startTimer(1500); // let startup/project-load settle before step 1
            }
        }
    }

    ~Content() override
    {
        clearMidiSinkIfInstalled();
        detachListenerIfPossible();
        // Loss-proofing: if measurements exist that were never (or not last) written to a
        // report, write one automatically before the window/app goes away.
        if (!reportWrittenSinceLastData_ && (!samples_.empty() || hasAnyEvents()))
        {
            appendSessionLog("session ending with unwritten data -> auto-writing report");
            writeReport();
        }
        appendSessionLog("=== SPIKE-01 session end ===");
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        auto row = [&area](int h) { return area.removeFromTop(h).reduced(0, 2); };

        headerLabel_.setBounds(row(24));
        {
            auto r = row(28);
            refreshTracksButton_.setBounds(r.removeFromRight(130));
            trackBox_.setBounds(r.reduced(2, 0));
        }
        {
            auto r = row(28);
            phaseBox_.setBounds(r.removeFromLeft(r.getWidth() / 2).reduced(2, 0));
            customPhaseEditor_.setBounds(r.reduced(2, 0));
        }
        {
            auto r = row(30);
            capture1Button_.setBounds(r.removeFromLeft(r.getWidth() / 2).reduced(2, 0));
            capture10Button_.setBounds(r.reduced(2, 0));
        }
        {
            auto r = row(30);
            saveCaptureButton_.setBounds(r.removeFromLeft(r.getWidth() / 2).reduced(2, 0));
            checkpointButton_.setBounds(r.reduced(2, 0));
        }
        listenerButton_.setBounds(row(30));
        {
            auto r = row(28);
            addNoteButton_.setBounds(r.removeFromRight(100));
            noteEditor_.setBounds(r.reduced(2, 0));
        }
        writeReportButton_.setBounds(row(30));
        listenerLabel_.setBounds(row(56));
        statusLabel_.setBounds(area);
    }

private:
    //==========================================================================
    // SPIKE-01B-M unattended auto plans (message-thread juce::Timer state machine).
    // Deviation note: the SPIKE-01B-M task said "document panel limitations rather
    // than extending it"; the operator explicitly requested automation instead
    // (recorded in the evidence report). Scaffolding-only; unreachable without
    // `--spike01-state-capture --spike01-auto=<plan>`.
    //==========================================================================
    struct AutoStep
    {
        int delayBeforeMs = 0;
        juce::String describe;
        std::function<bool()> run; // false => abort plan (partial report still written)
    };

    /// SPIKE-01B-M M2V: observes the exact merged MidiBuffer handed to the destination
    /// instrument's process boundary (ExperimentalInstrumentHost.cpp: the single choke point
    /// `audioThread_processBlockAndAddToOutputs`, called immediately before
    /// `inst.processBlock(view, blockMidi)`). Proves scheduled MIDI/CC actually reached the
    /// same instance `spike01LiveInstanceForDiagnostics()` returns, because both resolve from
    /// the host's single `activeOwner_->inst`. The classification/counting logic lives in
    /// `spike01::MidiDeliveryCounters` (Spike01MidiDeliveryCounters.h — RT-safe raw-byte
    /// parsing, no allocation; covered by deterministic selftests); this adapter only binds it
    /// to the host's sink interface.
    struct MidiSinkAdapter final : public ExperimentalInstrumentHost::MidiDeliveryCaptureSink
    {
        explicit MidiSinkAdapter(spike01::MidiDeliveryCounters& c) noexcept : counters_(c) {}

        void onMidiBlockDelivered(const juce::MidiBuffer& merged, const int numSamples) override
        {
            counters_.countBlock(merged, numSamples);
        }

        spike01::MidiDeliveryCounters& counters_;
    };

    void buildAutoPlan()
    {
        auto phaseAndCapture = [this](const juce::String& phaseId, const int n) {
            return [this, phaseId, n] {
                setCustomPhase(phaseId);
                const size_t before = samples_.size();
                captureRaw(n);
                return samples_.size() == before + (size_t)n;
            };
        };
        auto add = [this](int delayMs, juce::String desc, std::function<bool()> run) {
            autoSteps_.push_back({ delayMs, std::move(desc), std::move(run) });
        };

        if (autoPlanId_ == "M1X") // Groove Agent, untouched-first: captures only at 60 s / 120 s
        {
            addWaitForTrackStep("Groove Agent");
            add(60000, "M1X-60s x10", phaseAndCapture("M1X-60s", 10));
            add(60000, "M1X-120s x10", phaseAndCapture("M1X-120s", 10));
        }
        else if (autoPlanId_ == "M1Y") // Groove Agent, capture-heavy from t≈0
        {
            addWaitForTrackStep("Groove Agent");
            add(0, "M1Y-0s x10", phaseAndCapture("M1Y-0s", 10));
            add(30000, "M1Y-30s x10", phaseAndCapture("M1Y-30s", 10));
            add(30000, "M1Y-60s x10", phaseAndCapture("M1Y-60s", 10));
            add(60000, "M1Y-120s x10", phaseAndCapture("M1Y-120s", 10));
        }
        else if (autoPlanId_ == "M2" || autoPlanId_ == "M2O") // VB3-II post-transport settling,
        {                                                     // two passes. M2O targets the
                                                              // "Organ" track (arranged CC
                                                              // content); M2 hit trackId 4
                                                              // (notes only, no CC).
            addWaitForTrackStep(autoPlanId_ == "M2O" ? "Organ" : "VB3");
            add(1000, "attach listener", [this] {
                if (attachedInstance_ == nullptr)
                {
                    toggleListener();
                }
                return attachedInstance_ != nullptr;
            });
            for (int pass = 0; pass < 2; ++pass)
            {
                add(pass == 0 ? 500 : 2000, "start transport", [this] {
                    if (!callbacks_.startTransport)
                    {
                        return false;
                    }
                    callbacks_.startTransport();
                    return true;
                });
                add(4000, "M2-play x10 (a)", phaseAndCapture("M2-play", 10));
                add(4000, "M2-play x10 (b)", phaseAndCapture("M2-play", 10));
                add(1000, "stop transport + M2-stop x1 (t=0)", [this] {
                    if (!callbacks_.stopTransport)
                    {
                        return false;
                    }
                    setCustomPhase("M2-stop");
                    callbacks_.stopTransport();
                    const size_t before = samples_.size();
                    captureRaw(1);
                    return samples_.size() == before + 1;
                });
                add(100, "M2-stop x1 (~100ms)", phaseAndCapture("M2-stop", 1));
                add(150, "M2-stop x1 (~250ms)", phaseAndCapture("M2-stop", 1));
                add(250, "M2-stop x1 (~500ms)", phaseAndCapture("M2-stop", 1));
                add(500, "M2-stop x1 (~1s)", phaseAndCapture("M2-stop", 1));
                add(1000, "M2-stop x1 (~2s)", phaseAndCapture("M2-stop", 1));
                add(3000, "M2-late x10 (~5s)", phaseAndCapture("M2-late", 10));
                add(5000, "M2-late x10 (~10s)", phaseAndCapture("M2-late", 10));
            }
            add(500, "detach listener", [this] {
                detachListenerIfPossible();
                listenerButton_.setButtonText("Attach parameter listener");
                updateListenerLabel();
                return true;
            });
        }
        else if (autoPlanId_ == "M2P") // VB3-II settling after a deterministic parameter
        {                              // wiggle+revert. Parameter-round-trip test, deliberately
                                       // independent of arranged MIDI (the project DOES contain
                                       // arranged MIDI for Organ — the MIDI/CC playback path is
                                       // measured separately by the M2V plan).
            addWaitForTrackStep("VB3");
            add(1000, "attach listener", [this] {
                if (attachedInstance_ == nullptr)
                {
                    toggleListener();
                }
                return attachedInstance_ != nullptr;
            });
            add(500, "M2P-pre x10", phaseAndCapture("M2P-pre", 10));
            for (int pass = 0; pass < 2; ++pass)
            {
                add(pass == 0 ? 1000 : 3000, "perturb parameter", [this] {
                    return perturbFirstAutomatableParameter();
                });
                add(250, "M2P-mid x1 (perturbed)", phaseAndCapture("M2P-mid", 1));
                add(250, "revert parameter + M2P-post x1 (t=0)", [this, phaseAndCapture] {
                    if (!revertPerturbedParameter())
                    {
                        return false;
                    }
                    return phaseAndCapture("M2P-post", 1)();
                });
                add(100, "M2P-post x1 (~100ms)", phaseAndCapture("M2P-post", 1));
                add(150, "M2P-post x1 (~250ms)", phaseAndCapture("M2P-post", 1));
                add(250, "M2P-post x1 (~500ms)", phaseAndCapture("M2P-post", 1));
                add(500, "M2P-post x1 (~1s)", phaseAndCapture("M2P-post", 1));
                add(1000, "M2P-post x1 (~2s)", phaseAndCapture("M2P-post", 1));
                add(3000, "M2P-late x10 (~5s)", phaseAndCapture("M2P-late", 10));
                add(5000, "M2P-late x10 (~10s)", phaseAndCapture("M2P-late", 10));
            }
            add(500, "detach listener", [this] {
                detachListenerIfPossible();
                listenerButton_.setButtonText("Attach parameter listener");
                updateListenerLabel();
                return true;
            });
        }
        else if (autoPlanId_ == "P1EF") // P1E/P1F: background scheduler + atomic publication
        {
            buildP1efPlan();
        }
        else if (autoPlanId_ == "P1G") // P1G: missing-Primary proxy playback end to end
        {
            buildP1gPlan();
        }
        else if (autoPlanId_ == "M2V") // Corrected M2: VB3-II "Organ" (trackId 7) driven by the
        {                              // project's REAL arranged MIDI (ch1 clip notes + CC11,
                                       // ch2 from "Organ Lower", ch3 from "Organ pedal"), with a
                                       // delivery sink proving the events reached the instance.
            addWaitForTrackStep("Organ");
            add(500, "install MIDI delivery sink + attach listener", [this] {
                auto* host = resolveSelectedHost();
                if (host == nullptr)
                {
                    return false;
                }
                // Identity proof: record the destination instance pointer the sink boundary
                // will feed (same activeOwner_->inst on this host).
                auto* inst = host->spike01LiveInstanceForDiagnostics();
                m2vInstanceAtInstall_ = (const void*) inst;
                m2vBoundaryAtInstall_ = host->getMidiDeliveryBoundaryBlockCountRelaxed();
                host->installMidiDeliveryCaptureSinkForTests(&midiSinkAdapter_);
                m2vSinkHostTrackId_ = selectedTrackId();
                if (attachedInstance_ == nullptr)
                {
                    toggleListener();
                }
                appendSessionLog("auto: M2V sink installed; destInstance="
                                 + juce::String::toHexString((juce::pointer_sized_int) inst)
                                 + " boundaryBlocksAtInstall="
                                 + juce::String((juce::int64) m2vBoundaryAtInstall_)
                                 + " destTrackId=" + juce::String((int) m2vSinkHostTrackId_));
                return inst != nullptr;
            });
            add(500, "seek to sample 0", [this] {
                if (!callbacks_.seekTransport)
                {
                    return false;
                }
                callbacks_.seekTransport(0);
                return true;
            });
            add(500, "M2V-pre x10 (stopped, before playback)", phaseAndCapture("M2V-pre", 10));
            add(500, "start transport", [this] {
                if (!callbacks_.startTransport)
                {
                    return false;
                }
                callbacks_.startTransport();
                return true;
            });
            // Clips span ticks 960..11520 @ tpq960/180bpm => ~0.33..4.0 s; cycle is 0..288000
            // samples (~6 s). Sample the serialized blob DENSELY (every ~250 ms for ~9 s, past
            // one wrap) so a capture cannot systematically fall into a note gap and miss any
            // transient performance-state variation while MIDI/CC delivery is proven concurrently.
            for (int i = 0; i < 36; ++i)
            {
                add(250, "M2V-play x1 (~" + juce::String((i + 1) * 250) + "ms)",
                    phaseAndCapture("M2V-play", 1));
            }
            add(0, "log delivery mid-run", [this] {
                appendSessionLog("auto: M2V delivery (mid-run) " + midiCounters_.summary());
                return true;
            });
            add(1000, "stop transport + M2V-stop x1 (t=0)", [this] {
                if (!callbacks_.stopTransport)
                {
                    return false;
                }
                setCustomPhase("M2V-stop");
                callbacks_.stopTransport();
                const size_t before = samples_.size();
                captureRaw(1);
                return samples_.size() == before + 1;
            });
            add(100, "M2V-stop x1 (~100ms)", phaseAndCapture("M2V-stop", 1));
            add(150, "M2V-stop x1 (~250ms)", phaseAndCapture("M2V-stop", 1));
            add(250, "M2V-stop x1 (~500ms)", phaseAndCapture("M2V-stop", 1));
            add(500, "M2V-stop x1 (~1s)", phaseAndCapture("M2V-stop", 1));
            add(1000, "M2V-stop x1 (~2s)", phaseAndCapture("M2V-stop", 1));
            add(3000, "M2V-late x10 (~5s)", phaseAndCapture("M2V-late", 10));
            add(5000, "M2V-late x10 (~10s)", phaseAndCapture("M2V-late", 10));
            add(500, "log delivery final + wrap count", [this] {
                const juce::String wrap = callbacks_.readCycleWrapCount
                                              ? juce::String(callbacks_.readCycleWrapCount())
                                              : juce::String("n/a");
                appendSessionLog("auto: M2V delivery (final) " + midiCounters_.summary()
                                 + " cycleWraps=" + wrap);
                return true;
            });
            add(500, "clear sink + detach listener", [this] {
                if (auto* host = callbacks_.resolveHostForTrack
                                     ? callbacks_.resolveHostForTrack(m2vSinkHostTrackId_)
                                     : nullptr)
                {
                    host->installMidiDeliveryCaptureSinkForTests(nullptr);
                    appendSessionLog("auto: M2V sink cleared; boundaryBlocksNow="
                                     + juce::String((juce::int64)
                                           host->getMidiDeliveryBoundaryBlockCountRelaxed()));
                }
                detachListenerIfPossible();
                listenerButton_.setButtonText("Attach parameter listener");
                updateListenerLabel();
                return true;
            });
        }
    }

    //==========================================================================
    // P1EF integration plan (PRODUCTION background scheduler + atomic publication;
    // steering §13/§14/§16, roadmap P1E/P1F). Plan "P1EF" requests an explicit
    // render for the Organ destination through the NARROW service API (the panel
    // owns no job and sees no plugin instance), waits for the application-owned
    // scheduler to render on its low-priority worker through the P1D isolated
    // renderer, then verifies the publication contract: routed ch1/2/3 + CC11
    // delivery, distinct render-instance evidence, a validated immutable WAV
    // below <project>/InstrumentProxies/, safe relative metadata, a Current
    // destination state, swept temporaries and an operational transport. This
    // plan supersedes the P1D foreground plan (its coverage is included here).
    //==========================================================================

    void buildP1efPlan()
    {
        auto add = [this](int delayMs, juce::String desc, std::function<bool()> run) {
            autoSteps_.push_back({ delayMs, std::move(desc), std::move(run) });
        };

        addWaitForTrackStep("Organ");
        add(1000, "P1EF request explicit render through the service API", [this] {
            if (!callbacks_.requestProxyRender || !callbacks_.queryProxyJobStatus
                || !callbacks_.queryProxyDestinationState || !callbacks_.getPublishedProxyMetadata
                || !callbacks_.getProjectFolder)
            {
                appendSessionLog("p1ef: service callbacks missing");
                return false;
            }
            const auto s = callbacks_.requestProxyRender(selectedTrackId());
            if (!s.exists)
            {
                appendSessionLog("p1ef: request REJECTED: " + s.message);
                return false;
            }
            p1efGeneration_ = s.generation;
            appendSessionLog("p1ef: request queued generation=" + juce::String((juce::int64)s.generation)
                             + " phase=" + proxy_render::toString(s.phase)
                             + " fingerprint=" + s.expectedFingerprint
                             + " revision=" + juce::String((juce::int64)s.primarySemanticRevision));
            const auto destState = callbacks_.queryProxyDestinationState(selectedTrackId());
            appendSessionLog(juce::String("p1ef: destinationState=")
                             + proxy_render::toString(destState) + " (expect Rendering)");
            return s.phase == proxy_render::ProxyJobPhase::Queued
                   || s.phase == proxy_render::ProxyJobPhase::Preparing
                   || s.phase == proxy_render::ProxyJobPhase::Rendering;
        });
        autoSteps_.push_back(
            { 0, "wait: P1EF background render + publication", [this] {
                 waitProbeDesc_ = "P1EF background render + publication";
                 waitProbeDeadlineMs_ = juce::Time::getMillisecondCounterHiRes() + 600000.0;
                 waitProbe_ = [this] {
                     const auto s = callbacks_.queryProxyJobStatus(selectedTrackId());
                     const bool terminal
                         = s.exists
                           && (s.phase == proxy_render::ProxyJobPhase::Published
                               || s.phase == proxy_render::ProxyJobPhase::Obsolete
                               || s.phase == proxy_render::ProxyJobPhase::Cancelled
                               || s.phase == proxy_render::ProxyJobPhase::Failed);
                     return terminal ? 1 : 0;
                 };
                 return true;
             } });
        add(200, "P1EF verify publication contract", [this] {
            const auto s = callbacks_.queryProxyJobStatus(selectedTrackId());
            const auto& r = s.result;
            const auto& m = r.midi;
            appendSessionLog(juce::String("p1ef: job phase=") + proxy_render::toString(s.phase)
                             + " generation=" + juce::String((juce::int64)s.generation)
                             + " msg=\"" + s.message + "\"");
            appendSessionLog(juce::String("p1ef: result status=") + proxy_render::toString(r.status)
                             + " renderRate=" + juce::String(r.renderSampleRate, 0)
                             + " latencyStart=" + juce::String(r.pluginLatencySamplesAtStart)
                             + " spanEnd=" + juce::String(r.spanEndRenderSamples)
                             + " length=" + juce::String(r.renderedLengthSamples)
                             + " tail=" + juce::String(r.tailLengthSamples)
                             + " tailCompleted=" + (r.tailCompleted ? "true" : "false")
                             + " peak=" + juce::String(r.maxPeakLinear, 6)
                             + " blocks=" + juce::String((juce::int64)r.blocksProcessed)
                             + " workerThread=" + r.workerThreadId
                             + " wallMs=" + juce::String(r.wallMs, 1)
                             + " instanceDistinct="
                             + (r.renderInstanceDistinctFromLive ? "true" : "FALSE"));
            appendSessionLog("p1ef: midi ch1on=" + juce::String(m.noteOnsByChannel[0])
                             + " ch2on=" + juce::String(m.noteOnsByChannel[1])
                             + " ch3on=" + juce::String(m.noteOnsByChannel[2])
                             + " cc11=" + juce::String(m.ccByController[11])
                             + " totalEvents=" + juce::String(m.totalEvents));

            const bool published = s.phase == proxy_render::ProxyJobPhase::Published
                                   && s.generation == p1efGeneration_;
            const bool midiOk = m.noteOnsByChannel[0] > 0 && m.noteOnsByChannel[1] > 0
                                && m.noteOnsByChannel[2] > 0 && m.ccByController[11] > 0;

            // Published metadata: safe relative path below the temp project's
            // InstrumentProxies/, valid immutable WAV, matching generation identity.
            ProjectFileProxyMetadataV20 meta;
            const bool hasMeta = callbacks_.getPublishedProxyMetadata(selectedTrackId(), meta);
            const juce::File projectFolder = callbacks_.getProjectFolder();
            bool metaOk = false;
            bool assetOk = false;
            bool tempsSwept = false;
            if (hasMeta && projectFolder != juce::File())
            {
                metaOk = meta.generationId == s.expectedFingerprint && !meta.silentGeneration
                         && proxy_store::isSafeProxyRelativePath(meta.relativePath)
                         && meta.lengthSamples == r.renderedLengthSamples
                         && meta.sampleRate == r.renderSampleRate && meta.channels == r.channels
                         && meta.pluginLatencySamples
                                == juce::jmax(0, r.pluginLatencySamplesAtStart);
                const auto check = proxy_store::validatePublishedAsset(projectFolder, meta);
                assetOk = check.ok && check.file.existsAsFile() && check.file.getSize() > 0;
                appendSessionLog("p1ef: metadata relativePath=" + meta.relativePath
                                 + " generationId=" + meta.generationId
                                 + " length=" + juce::String(meta.lengthSamples)
                                 + " latency=" + juce::String(meta.pluginLatencySamples)
                                 + " renderedUtc=" + meta.renderedUtc
                                 + " assetBytes="
                                 + juce::String(check.file != juce::File() ? check.file.getSize() : 0)
                                 + (assetOk ? "" : (" assetError=" + check.error)));
                // Temp render targets must be consumed by the publication rename.
                const auto temps = proxy_store::proxyDirectory(projectFolder)
                                       .findChildFiles(juce::File::findFiles, false, "tmp_*.wav");
                tempsSwept = temps.isEmpty();
            }
            const auto destState = callbacks_.queryProxyDestinationState(selectedTrackId());
            const bool currentOk = destState == proxy_render::ProxyDestinationState::Current;
            appendSessionLog(juce::String("p1ef: VERIFY published=") + (published ? "PASS" : "FAIL")
                             + " routedCh123AndCc11=" + (midiOk ? "PASS" : "FAIL")
                             + " instanceDistinct="
                             + (r.renderInstanceDistinctFromLive ? "PASS" : "FAIL")
                             + " metadata=" + (metaOk ? "PASS" : "FAIL")
                             + " immutableWav=" + (assetOk ? "PASS" : "FAIL")
                             + " tempsSwept=" + (tempsSwept ? "PASS" : "FAIL")
                             + " destinationCurrent=" + (currentOk ? "PASS" : "FAIL"));
            return published && midiOk && r.renderInstanceDistinctFromLive && metaOk && assetOk
                   && tempsSwept && currentOk;
        });
        add(300, "P1EF transport still operational after render", [this] {
            if (!callbacks_.startTransport || !callbacks_.isTransportPlaying
                || !callbacks_.stopTransport)
            {
                return false;
            }
            callbacks_.startTransport();
            return true;
        });
        add(1500, "P1EF stop transport + record operational check", [this] {
            const bool playing = callbacks_.isTransportPlaying();
            callbacks_.stopTransport();
            appendSessionLog(juce::String("p1ef: transportOperationalAfterRender=")
                             + (playing ? "PASS" : "FAIL"));
            return playing;
        });
    }

    //==========================================================================
    // P1G integration plan (missing-Primary CURRENT-PROXY PLAYBACK end to end;
    // steering §7.3/§12/§15.6, roadmap P1G). Runs against a TEMPORARY project
    // copy only (the launcher copies the real Organ/VB3-II project). Sequence:
    // publish a fresh current proxy through the production scheduler -> save the
    // temp project (persists the §12.3 save pairing) -> force Primary
    // "unavailable" through the coordinator test hook (identity intact) ->
    // verify the proxy is SELECTED and audibly consumed through the normal
    // post-instrument track path (transport, loop wrap, seek, EOF silence) ->
    // verify offline mixdown consumes the proxy and that MUTE through the shared
    // strip gates it -> verify playback at a DIFFERENT engine rate -> verify a
    // render-relevant musical edit makes the generation Stale and deselects it.
    // Evidence sources: host proxy diagnostics (blocks-mixed counter + pre-strip
    // block peak), coordinator runtime state, and rendered mixdown WAV peaks.
    //==========================================================================

    [[nodiscard]] std::uint64_t p1gProxyBlocksNow()
    {
        auto* host = callbacks_.resolveHostForTrack
                         ? callbacks_.resolveHostForTrack(selectedTrackId())
                         : nullptr;
        return host != nullptr ? host->getProxyBlocksMixedCountRelaxed() : 0;
    }

    [[nodiscard]] float p1gProxyPeakNow()
    {
        auto* host = callbacks_.resolveHostForTrack
                         ? callbacks_.resolveHostForTrack(selectedTrackId())
                         : nullptr;
        return host != nullptr ? host->getProxyLastBlockPeakForDiagnostics() : -1.0f;
    }

    [[nodiscard]] juce::String p1gStateName()
    {
        const int s = callbacks_.queryProxyPlaybackRuntimeState
                          ? callbacks_.queryProxyPlaybackRuntimeState(selectedTrackId())
                          : -1;
        return s < 0 ? juce::String("(no coordinator)")
                     : juce::String(proxy_playback::proxyPlaybackSourceStateName(
                           (proxy_playback::ProxyPlaybackSourceState)s));
    }

    [[nodiscard]] bool p1gStateIs(const proxy_playback::ProxyPlaybackSourceState want)
    {
        return callbacks_.queryProxyPlaybackRuntimeState
               && callbacks_.queryProxyPlaybackRuntimeState(selectedTrackId()) == (int)want;
    }

    /// Settled state check: the runtime status reports a real underrun ONCE (P1I
    /// one-shot signal) — after a deliberate discontinuity (seek/loop/rate change)
    /// the plan consumes that transient report and asserts the recovered state.
    [[nodiscard]] bool p1gStateSettledIs(const proxy_playback::ProxyPlaybackSourceState want)
    {
        if (!callbacks_.queryProxyPlaybackRuntimeState)
        {
            return false;
        }
        const int first = callbacks_.queryProxyPlaybackRuntimeState(selectedTrackId());
        if (first == (int)want)
        {
            return true;
        }
        if (first != (int)proxy_playback::ProxyPlaybackSourceState::PlaybackUnderrun)
        {
            return false;
        }
        appendSessionLog("p1g: transient PlaybackUnderrun consumed (discontinuity)");
        return callbacks_.queryProxyPlaybackRuntimeState(selectedTrackId()) == (int)want;
    }

    /// Whole-file linear peak of a rendered mixdown WAV (message thread; small files).
    [[nodiscard]] static float p1gReadWavPeak(const juce::File& f)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
        if (r == nullptr || r->numChannels == 0)
        {
            return -1.0f;
        }
        juce::AudioBuffer<float> buf((int)r->numChannels, 8192);
        float peak = 0.0f;
        for (std::int64_t pos = 0; pos < r->lengthInSamples;)
        {
            const int n = (int)juce::jmin((std::int64_t)8192, r->lengthInSamples - pos);
            if (!r->read(&buf, 0, n, pos, true, true))
            {
                return -1.0f;
            }
            for (int c = 0; c < (int)r->numChannels; ++c)
            {
                peak = juce::jmax(peak, buf.getMagnitude(c, 0, n));
            }
            pos += n;
        }
        return peak;
    }

    void buildP1gPlan()
    {
        auto add = [this](int delayMs, juce::String desc, std::function<bool()> run) {
            autoSteps_.push_back({ delayMs, std::move(desc), std::move(run) });
        };

        addWaitForTrackStep("Organ");

        add(1000, "P1G preflight: required callbacks present", [this] {
            const bool ok = callbacks_.requestProxyRender && callbacks_.queryProxyJobStatus
                            && callbacks_.getPublishedProxyMetadata && callbacks_.getProjectFolder
                            && callbacks_.setProxyPrimaryForcedUnavailable
                            && callbacks_.queryProxyPlaybackRuntimeState
                            && callbacks_.isProxyViewSelected && callbacks_.saveProjectNow
                            && callbacks_.runOfflineMixdownWav && callbacks_.setTrackMuted
                            && callbacks_.appendStaleTestClip && callbacks_.getEngineSampleRate
                            && callbacks_.trySetEngineSampleRate && callbacks_.startTransport
                            && callbacks_.stopTransport && callbacks_.seekTransport
                            && callbacks_.readCycleWrapCount && callbacks_.resolveHostForTrack;
            if (!ok)
            {
                appendSessionLog("p1g: missing plan callbacks");
            }
            return ok;
        });

        // --- publish a fresh CURRENT generation through the production scheduler ---
        add(500, "P1G request proxy render (production scheduler)", [this] {
            const auto s = callbacks_.requestProxyRender(selectedTrackId());
            if (!s.exists)
            {
                appendSessionLog("p1g: render request REJECTED: " + s.message);
                return false;
            }
            p1efGeneration_ = s.generation;
            appendSessionLog("p1g: render queued generation="
                             + juce::String((juce::int64)s.generation)
                             + " fingerprint=" + s.expectedFingerprint);
            return true;
        });
        autoSteps_.push_back(
            { 0, "wait: P1G background render + publication", [this] {
                 waitProbeDesc_ = "P1G background render + publication";
                 waitProbeDeadlineMs_ = juce::Time::getMillisecondCounterHiRes() + 600000.0;
                 waitProbe_ = [this] {
                     const auto s = callbacks_.queryProxyJobStatus(selectedTrackId());
                     const bool terminal
                         = s.exists
                           && (s.phase == proxy_render::ProxyJobPhase::Published
                               || s.phase == proxy_render::ProxyJobPhase::Obsolete
                               || s.phase == proxy_render::ProxyJobPhase::Cancelled
                               || s.phase == proxy_render::ProxyJobPhase::Failed);
                     return terminal ? 1 : 0;
                 };
                 return true;
             } });
        add(200, "P1G verify published generation represents ch1/2/3 + CC11", [this] {
            const auto s = callbacks_.queryProxyJobStatus(selectedTrackId());
            const auto& m = s.result.midi;
            const bool published = s.phase == proxy_render::ProxyJobPhase::Published
                                   && s.generation == p1efGeneration_;
            const bool midiOk = m.noteOnsByChannel[0] > 0 && m.noteOnsByChannel[1] > 0
                                && m.noteOnsByChannel[2] > 0 && m.ccByController[11] > 0;
            ProjectFileProxyMetadataV20 meta;
            const bool hasMeta = callbacks_.getPublishedProxyMetadata(selectedTrackId(), meta);
            if (hasMeta)
            {
                p1gAssetLenSamples_ = meta.lengthSamples;
                p1gAssetRate_ = meta.sampleRate;
            }
            appendSessionLog(juce::String("p1g: VERIFY published=") + (published ? "PASS" : "FAIL")
                             + " routedCh123AndCc11=" + (midiOk ? "PASS" : "FAIL")
                             + " ch1on=" + juce::String(m.noteOnsByChannel[0])
                             + " ch2on=" + juce::String(m.noteOnsByChannel[1])
                             + " ch3on=" + juce::String(m.noteOnsByChannel[2])
                             + " cc11=" + juce::String(m.ccByController[11])
                             + " assetLen=" + juce::String(p1gAssetLenSamples_)
                             + " assetRate=" + juce::String(p1gAssetRate_, 0));
            return published && midiOk && hasMeta && p1gAssetLenSamples_ > 0
                   && p1gAssetRate_ > 0.0;
        });
        add(300, "P1G save temp project (persist §12.3 save pairing)", [this] {
            const bool ok = callbacks_.saveProjectNow();
            appendSessionLog(juce::String("p1g: projectSaved=") + (ok ? "PASS" : "FAIL"));
            return ok;
        });

        // --- simulate missing Primary; the identity/expected fingerprint is untouched ---
        add(300, "P1G force Primary unavailable (identity intact)", [this] {
            callbacks_.setProxyPrimaryForcedUnavailable(selectedTrackId(), true);
            const bool selected = callbacks_.isProxyViewSelected(selectedTrackId());
            const bool stateOk
                = p1gStateIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent)
                  || p1gStateIs(proxy_playback::ProxyPlaybackSourceState::ProxyPreparing);
            appendSessionLog(juce::String("p1g: primaryForcedUnavailable state=") + p1gStateName()
                             + " proxySelected=" + (selected ? "PASS" : "FAIL"));
            return selected && stateOk;
        });

        // --- transport: proxy audible through the normal post-instrument path ---
        add(300, "P1G seek 0 + start transport", [this] {
            callbacks_.seekTransport(0);
            p1gBlocksMark_ = p1gProxyBlocksNow();
            p1gWrapMark_ = callbacks_.readCycleWrapCount();
            callbacks_.startTransport();
            return true;
        });
        add(2500, "P1G verify non-silent proxy playback (normal track path)", [this] {
            const std::uint64_t blocks = p1gProxyBlocksNow();
            const float peak = p1gProxyPeakNow();
            const bool consumed = blocks > p1gBlocksMark_;
            const bool audible = peak > 0.0005f;
            const bool current
                = p1gStateSettledIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent);
            appendSessionLog("p1g: playback blocksMixedDelta="
                             + juce::String((juce::int64)(blocks - p1gBlocksMark_))
                             + " lastBlockPeak=" + juce::String(peak, 6) + " state="
                             + p1gStateName() + " -> proxyConsumed=" + (consumed ? "PASS" : "FAIL")
                             + " nonSilent=" + (audible ? "PASS" : "FAIL")
                             + " stateCurrent=" + (current ? "PASS" : "FAIL"));
            return consumed && audible && current;
        });
        autoSteps_.push_back({ 0, "wait: P1G transport loop wrap", [this] {
                                  waitProbeDesc_ = "P1G transport loop wrap";
                                  waitProbeDeadlineMs_
                                      = juce::Time::getMillisecondCounterHiRes() + 30000.0;
                                  waitProbe_ = [this] {
                                      return callbacks_.readCycleWrapCount() > p1gWrapMark_ ? 1 : 0;
                                  };
                                  return true;
                              } });
        add(1200, "P1G verify audible after loop wrap", [this] {
            const float peak = p1gProxyPeakNow();
            const bool ok
                = peak > 0.0005f
                  && p1gStateSettledIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent);
            appendSessionLog("p1g: loopWrap peak=" + juce::String(peak, 6) + " state="
                             + p1gStateName() + " -> " + (ok ? "PASS" : "FAIL"));
            return ok;
        });
        add(200, "P1G seek to 1.0 s while playing", [this] {
            const double rate = callbacks_.getEngineSampleRate();
            p1gBlocksMark_ = p1gProxyBlocksNow();
            callbacks_.seekTransport((std::int64_t)rate);
            return rate > 0.0;
        });
        add(1200, "P1G verify audible after seek", [this] {
            const std::uint64_t blocks = p1gProxyBlocksNow();
            const float peak = p1gProxyPeakNow();
            const bool ok = blocks > p1gBlocksMark_ && peak > 0.0005f;
            appendSessionLog("p1g: seek blocksDelta="
                             + juce::String((juce::int64)(blocks - p1gBlocksMark_)) + " peak="
                             + juce::String(peak, 6) + " -> " + (ok ? "PASS" : "FAIL"));
            return ok;
        });

        // --- EOF: published assets end at completed tail; beyond EOF is exact silence ---
        add(200, "P1G seek past proxy EOF", [this] {
            const double engineRate = callbacks_.getEngineSampleRate();
            const double eofSec = (double)p1gAssetLenSamples_ / p1gAssetRate_;
            const auto seekTo = (std::int64_t)((eofSec + 0.30) * engineRate);
            p1gBlocksMark_ = p1gProxyBlocksNow();
            callbacks_.seekTransport(seekTo);
            appendSessionLog("p1g: eof seekTo=" + juce::String(seekTo) + " (eofSec="
                             + juce::String(eofSec, 3) + " engineRate="
                             + juce::String(engineRate, 0) + ")");
            return engineRate > 0.0;
        });
        add(500, "P1G verify EOF silence (still ProxyCurrent, still consuming)", [this] {
            float maxPeak = 0.0f;
            for (int i = 0; i < 5; ++i)
            {
                maxPeak = juce::jmax(maxPeak, p1gProxyPeakNow());
                juce::Thread::sleep(60);
            }
            const std::uint64_t blocks = p1gProxyBlocksNow();
            const bool consuming = blocks > p1gBlocksMark_;
            const bool silent = maxPeak == 0.0f;
            const bool current
                = p1gStateSettledIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent);
            appendSessionLog("p1g: eof maxPeak=" + juce::String(maxPeak, 6) + " blocksDelta="
                             + juce::String((juce::int64)(blocks - p1gBlocksMark_)) + " state="
                             + p1gStateName() + " -> silence=" + (silent ? "PASS" : "FAIL")
                             + " consuming=" + (consuming ? "PASS" : "FAIL")
                             + " stateCurrent=" + (current ? "PASS" : "FAIL"));
            return silent && consuming && current;
        });
        add(100, "P1G stop transport", [this] {
            callbacks_.stopTransport();
            return true;
        });

        // --- offline mixdown consumes the proxy; MUTE gates it through the shared strip ---
        add(500, "P1G offline mixdown consumes the current proxy", [this] {
            // Isolation: mute every OTHER sound-producing track so the file peaks
            // measure exactly the proxy destination through its normal strip.
            p1gOtherMuted_.clear();
            if (callbacks_.listSoundProducingTracks)
            {
                for (const TrackId tid : callbacks_.listSoundProducingTracks())
                {
                    if (tid != selectedTrackId())
                    {
                        callbacks_.setTrackMuted(tid, true);
                        p1gOtherMuted_.push_back(tid);
                    }
                }
            }
            p1gMixdownA_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("p1g_mixdown_proxy.wav");
            p1gBlocksMark_ = p1gProxyBlocksNow();
            const auto res = callbacks_.runOfflineMixdownWav(p1gMixdownA_);
            const float peak = p1gReadWavPeak(p1gMixdownA_);
            const std::uint64_t blocks = p1gProxyBlocksNow();
            const bool consumed = blocks > p1gBlocksMark_;
            const bool ok = res.wasOk() && peak > 0.001f && consumed;
            appendSessionLog("p1g: mixdown result=" + (res.wasOk() ? "ok" : res.getErrorMessage())
                             + " filePeak=" + juce::String(peak, 6) + " proxyBlocksDelta="
                             + juce::String((juce::int64)(blocks - p1gBlocksMark_))
                             + " -> proxyMixdown=" + (ok ? "PASS" : "FAIL"));
            return ok;
        });
        add(300, "P1G mute gates proxy through the shared downstream strip", [this] {
            p1gMixdownB_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("p1g_mixdown_muted.wav");
            callbacks_.setTrackMuted(selectedTrackId(), true);
            const auto res = callbacks_.runOfflineMixdownWav(p1gMixdownB_);
            callbacks_.setTrackMuted(selectedTrackId(), false);
            for (const TrackId tid : p1gOtherMuted_)
            {
                callbacks_.setTrackMuted(tid, false);
            }
            p1gOtherMuted_.clear();
            const float peak = p1gReadWavPeak(p1gMixdownB_);
            const bool ok = res.wasOk() && peak >= 0.0f && peak < 1.0e-6f;
            appendSessionLog("p1g: mutedMixdown result="
                             + (res.wasOk() ? "ok" : res.getErrorMessage()) + " filePeak="
                             + juce::String(peak, 8) + " -> muteGatesProxy="
                             + (ok ? "PASS" : "FAIL"));
            return ok;
        });

        // --- cross-rate: same generation stays Current and playable at another rate.
        // When the CURRENT device cannot open the alternate rate at all, the live
        // repeat is skipped with an explicit reason (cross-rate conversion itself
        // is deterministically proven by the 44.1<->48 selftests + the coordinator
        // rebuild test) — a hardware capability gap must not fake a failure.
        add(300, "P1G switch engine sample rate", [this] {
            p1gOriginalRate_ = callbacks_.getEngineSampleRate();
            const double alt = p1gOriginalRate_ > 45000.0 ? 44100.0 : 48000.0;
            const bool switched = callbacks_.trySetEngineSampleRate(alt);
            if (!switched)
            {
                p1gRateSwitchSkipped_ = true;
                appendSessionLog("p1g: engineRate " + juce::String(p1gOriginalRate_, 0) + " -> "
                                 + juce::String(alt, 0)
                                 + " UNSUPPORTED by the current device -> SKIP live cross-rate "
                                   "repeat (covered by deterministic selftests)");
                return true;
            }
            appendSessionLog("p1g: engineRate " + juce::String(p1gOriginalRate_, 0) + " -> "
                             + juce::String(alt, 0) + " switched=PASS");
            return true;
        });
        autoSteps_.push_back(
            { 0, "wait: P1G proxy Current at the new engine rate", [this] {
                 if (p1gRateSwitchSkipped_)
                 {
                     return true;
                 }
                 waitProbeDesc_ = "P1G proxy Current at the new engine rate";
                 waitProbeDeadlineMs_ = juce::Time::getMillisecondCounterHiRes() + 20000.0;
                 waitProbe_ = [this] {
                     return (callbacks_.isProxyViewSelected(selectedTrackId())
                             && p1gStateIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent))
                                ? 1
                                : 0;
                 };
                 return true;
             } });
        add(300, "P1G start transport at the new rate", [this] {
            if (p1gRateSwitchSkipped_)
            {
                return true;
            }
            callbacks_.seekTransport((std::int64_t)(0.5 * callbacks_.getEngineSampleRate()));
            p1gBlocksMark_ = p1gProxyBlocksNow();
            callbacks_.startTransport();
            return true;
        });
        add(2000, "P1G verify audible cross-rate proxy playback", [this] {
            if (p1gRateSwitchSkipped_)
            {
                appendSessionLog("p1g: crossRate SKIPPED (device rate capability)");
                return true;
            }
            const std::uint64_t blocks = p1gProxyBlocksNow();
            const float peak = p1gProxyPeakNow();
            callbacks_.stopTransport();
            const bool ok
                = blocks > p1gBlocksMark_ && peak > 0.0005f
                  && p1gStateSettledIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent);
            appendSessionLog("p1g: crossRate rate=" + juce::String(callbacks_.getEngineSampleRate(), 0)
                             + " blocksDelta=" + juce::String((juce::int64)(blocks - p1gBlocksMark_))
                             + " peak=" + juce::String(peak, 6) + " state=" + p1gStateName()
                             + " -> " + (ok ? "PASS" : "FAIL"));
            return ok;
        });
        add(300, "P1G restore original engine rate", [this] {
            if (p1gRateSwitchSkipped_)
            {
                return true;
            }
            const bool switched = callbacks_.trySetEngineSampleRate(p1gOriginalRate_);
            appendSessionLog("p1g: engineRate restored=" + juce::String(p1gOriginalRate_, 0)
                             + " switched=" + (switched ? "PASS" : "FAIL"));
            return switched;
        });
        autoSteps_.push_back(
            { 0, "wait: P1G proxy Current again at the original rate", [this] {
                 if (p1gRateSwitchSkipped_)
                 {
                     return true;
                 }
                 waitProbeDesc_ = "P1G proxy Current again at the original rate";
                 waitProbeDeadlineMs_ = juce::Time::getMillisecondCounterHiRes() + 20000.0;
                 waitProbe_ = [this] {
                     return (callbacks_.isProxyViewSelected(selectedTrackId())
                             && p1gStateIs(proxy_playback::ProxyPlaybackSourceState::ProxyCurrent))
                                ? 1
                                : 0;
                 };
                 return true;
             } });

        // --- a render-relevant edit makes the generation Stale; it is never selected ---
        add(300, "P1G stale edit deselects the proxy", [this] {
            if (!callbacks_.appendStaleTestClip(selectedTrackId()))
            {
                appendSessionLog("p1g: stale-test clip append failed");
                return false;
            }
            const bool stale = p1gStateIs(proxy_playback::ProxyPlaybackSourceState::ProxyStale);
            const bool deselected = !callbacks_.isProxyViewSelected(selectedTrackId());
            appendSessionLog(juce::String("p1g: staleEdit state=") + p1gStateName()
                             + " proxyDeselected=" + (deselected ? "PASS" : "FAIL")
                             + " stateStale=" + (stale ? "PASS" : "FAIL"));
            return stale && deselected;
        });
        add(200, "P1G stale proxy is never consumed by transport", [this] {
            p1gBlocksMark_ = p1gProxyBlocksNow();
            callbacks_.seekTransport(0);
            callbacks_.startTransport();
            return true;
        });
        add(1500, "P1G verify zero proxy consumption while Stale", [this] {
            const std::uint64_t blocks = p1gProxyBlocksNow();
            callbacks_.stopTransport();
            const bool ok = blocks == p1gBlocksMark_;
            appendSessionLog("p1g: staleTransport proxyBlocksDelta="
                             + juce::String((juce::int64)(blocks - p1gBlocksMark_))
                             + " -> neverSelected=" + (ok ? "PASS" : "FAIL"));
            return ok;
        });

        // --- restore + cleanup (temp project itself is cleaned by the launcher) ---
        add(300, "P1G restore Primary availability + clean temp mixdowns", [this] {
            callbacks_.setProxyPrimaryForcedUnavailable(selectedTrackId(), false);
            const bool primary = p1gStateIs(proxy_playback::ProxyPlaybackSourceState::Primary);
            p1gMixdownA_.deleteFile();
            p1gMixdownB_.deleteFile();
            appendSessionLog(juce::String("p1g: restored state=") + p1gStateName()
                             + " backToPrimary=" + (primary ? "PASS" : "FAIL"));
            return primary;
        });
    }

    /// M2P: deterministically wiggle the first automatable non-bypass parameter through the
    /// same notify-host path the UI uses; the original value is stored for the revert step.
    [[nodiscard]] bool perturbFirstAutomatableParameter()
    {
        auto* host = resolveSelectedHost();
        auto* inst = host != nullptr ? host->spike01LiveInstanceForDiagnostics() : nullptr;
        if (inst == nullptr)
        {
            return false;
        }
        juce::AudioProcessorParameter* target = nullptr;
        for (auto* p : inst->getParameters())
        {
            if (p != nullptr && p->isAutomatable() && !p->getName(64).containsIgnoreCase("bypass"))
            {
                target = p;
                break;
            }
        }
        if (target == nullptr)
        {
            appendSessionLog("auto: no automatable parameter found");
            return false;
        }
        perturbParamIndex_ = target->getParameterIndex();
        perturbOriginalValue_ = target->getValue();
        const float v1 = perturbOriginalValue_ <= 0.5f ? juce::jmin(1.0f, perturbOriginalValue_ + 0.25f)
                                                       : juce::jmax(0.0f, perturbOriginalValue_ - 0.25f);
        appendSessionLog("auto: perturb idx=" + juce::String(perturbParamIndex_) + " \""
                         + target->getName(64) + "\" v0=" + juce::String(perturbOriginalValue_, 4)
                         + " -> v1=" + juce::String(v1, 4));
        target->setValueNotifyingHost(v1);
        return true;
    }

    [[nodiscard]] bool revertPerturbedParameter()
    {
        auto* host = resolveSelectedHost();
        auto* inst = host != nullptr ? host->spike01LiveInstanceForDiagnostics() : nullptr;
        if (inst == nullptr || perturbParamIndex_ < 0)
        {
            return false;
        }
        const auto& params = inst->getParameters();
        if (perturbParamIndex_ >= params.size())
        {
            return false;
        }
        params[perturbParamIndex_]->setValueNotifyingHost(perturbOriginalValue_);
        appendSessionLog("auto: revert idx=" + juce::String(perturbParamIndex_) + " -> v0="
                         + juce::String(perturbOriginalValue_, 4));
        return true;
    }

    void addWaitForTrackStep(const juce::String& needle)
    {
        autoSteps_.push_back({ 0, "wait for track containing '" + needle + "'", [this, needle] {
                                  waitTrackNeedle_ = needle;
                                  waitDeadlineMs_ = juce::Time::getMillisecondCounterHiRes()
                                                    + 120000.0; // plugin restore can be slow
                                  return true;
                              } });
    }

    void timerCallback() override
    {
        stopTimer();
        if (!autoActive_)
        {
            return;
        }
        // Waiting mode: poll for the requested runtime until it exists or times out.
        if (waitTrackNeedle_.isNotEmpty())
        {
            refreshTrackList();
            if (selectTrackContaining(waitTrackNeedle_))
            {
                appendSessionLog("auto: track found for '" + waitTrackNeedle_ + "'");
                waitTrackNeedle_.clear();
                startTimer(1); // proceed to the next step immediately
            }
            else if (juce::Time::getMillisecondCounterHiRes() > waitDeadlineMs_)
            {
                abortAuto("timeout waiting for track");
            }
            else
            {
                startTimer(500);
            }
            return;
        }
        // SPIKE-02 waiting mode: poll a generic predicate (render-worker done flag) until it
        // reports satisfied/failed or times out. Polling only reads atomics; the worker owns
        // the render instance exclusively until it reports done.
        if (waitProbe_)
        {
            const int probe = waitProbe_();
            if (probe > 0)
            {
                appendSessionLog("auto: wait satisfied — " + waitProbeDesc_);
                waitProbe_ = {};
                startTimer(autoIndex_ < autoSteps_.size()
                               ? juce::jmax(1, autoSteps_[autoIndex_].delayBeforeMs)
                               : 1);
            }
            else if (probe < 0)
            {
                waitProbe_ = {};
                abortAuto("wait failed: " + waitProbeDesc_);
            }
            else if (juce::Time::getMillisecondCounterHiRes() > waitProbeDeadlineMs_)
            {
                waitProbe_ = {};
                abortAuto("wait timeout: " + waitProbeDesc_);
            }
            else
            {
                startTimer(500);
            }
            return;
        }
        if (autoIndex_ >= autoSteps_.size())
        {
            finishAuto();
            return;
        }
        const auto& step = autoSteps_[autoIndex_++];
        appendSessionLog("auto: step " + juce::String((int)autoIndex_) + "/"
                         + juce::String((int)autoSteps_.size()) + " — " + step.describe);
        if (!step.run())
        {
            abortAuto("step failed: " + step.describe);
            return;
        }
        if (waitTrackNeedle_.isNotEmpty() || waitProbe_)
        {
            startTimer(500); // the step armed a waiting mode
        }
        else if (autoIndex_ < autoSteps_.size())
        {
            startTimer(juce::jmax(1, autoSteps_[autoIndex_].delayBeforeMs));
        }
        else
        {
            finishAuto();
        }
    }

    void finishAuto()
    {
        autoActive_ = false;
        writeReport();
        appendSessionLog("auto: plan " + autoPlanId_ + " COMPLETE");
        setStatus("AUTO plan '" + autoPlanId_ + "' COMPLETE — report written. Safe to close.");
    }

    void abortAuto(const juce::String& reason)
    {
        autoActive_ = false;
        clearMidiSinkIfInstalled();
        // P1EF: the panel owns no render job — in-flight background work is owned and cleaned
        // up by the application-owned scheduler (cancel/detach on app shutdown).
        appendSessionLog("auto: ABORT — " + reason);
        writeReport(); // partial data is still valuable
        setStatus("AUTO plan '" + autoPlanId_ + "' ABORTED: " + reason);
    }

    /// Defensive: never leave the audio thread holding a pointer to `midiSinkAdapter_` after the
    /// panel goes away (M2V installs it on a host that outlives the panel).
    void clearMidiSinkIfInstalled()
    {
        if (m2vSinkHostTrackId_ == kInvalidTrackId || !callbacks_.resolveHostForTrack)
        {
            return;
        }
        if (auto* host = callbacks_.resolveHostForTrack(m2vSinkHostTrackId_))
        {
            host->installMidiDeliveryCaptureSinkForTests(nullptr);
        }
        m2vSinkHostTrackId_ = kInvalidTrackId;
    }

    [[nodiscard]] bool selectTrackContaining(const juce::String& needle)
    {
        for (size_t i = 0; i < choices_.size(); ++i)
        {
            if (choices_[i].label.containsIgnoreCase(needle))
            {
                trackBox_.setSelectedId((int)i + 1, juce::dontSendNotification);
                return true;
            }
        }
        return false;
    }

    void setCustomPhase(const juce::String& phaseId)
    {
        const int customItemId = (int)spike01::requiredPhases().size() + 1;
        phaseBox_.setSelectedId(customItemId, juce::dontSendNotification);
        customPhaseEditor_.setText(phaseId, juce::dontSendNotification);
    }

    //==========================================================================
    // Track selection
    //==========================================================================
    void refreshTrackList()
    {
        choices_ = callbacks_.listInstrumentRuntimes ? callbacks_.listInstrumentRuntimes()
                                                     : std::vector<Spike01RuntimeChoice>{};
        trackBox_.clear(juce::dontSendNotification);
        int id = 1;
        for (const auto& c : choices_)
        {
            trackBox_.addItem(c.label, id++);
        }
        if (!choices_.empty())
        {
            trackBox_.setSelectedId(1, juce::dontSendNotification);
        }
        setStatus(choices_.empty() ? "No instrument tracks with a runtime found. Create an "
                                     "instrument track and load VB3-II, then Refresh tracks."
                                   : juce::String((int)choices_.size()) + " instrument runtime(s) listed.");
    }

    [[nodiscard]] TrackId selectedTrackId() const
    {
        const int idx = trackBox_.getSelectedId() - 1;
        if (idx < 0 || idx >= (int)choices_.size())
        {
            return kInvalidTrackId;
        }
        return choices_[(size_t)idx].trackId;
    }

    [[nodiscard]] ExperimentalInstrumentHost* resolveSelectedHost()
    {
        const TrackId tid = selectedTrackId();
        if (tid == kInvalidTrackId || !callbacks_.resolveHostForTrack)
        {
            setStatus("Select an instrument track first (Refresh tracks if the list is empty).");
            return nullptr;
        }
        auto* host = callbacks_.resolveHostForTrack(tid);
        if (host == nullptr || !host->hasInstrument())
        {
            setStatus("The selected track has no loaded instrument.");
            return nullptr;
        }
        return host;
    }

    [[nodiscard]] juce::String currentPhaseId() const
    {
        const auto& phases = spike01::requiredPhases();
        const int sel = phaseBox_.getSelectedId();
        if (sel >= 1 && sel <= (int)phases.size())
        {
            return juce::String(phases[(size_t)(sel - 1)].id);
        }
        const juce::String custom = customPhaseEditor_.getText().trim();
        return custom.isNotEmpty() ? custom : juce::String("custom-unnamed");
    }

    //==========================================================================
    // Captures (message thread only; raw bytes are hashed and discarded)
    //==========================================================================
    void captureRaw(const int repeats)
    {
        auto* host = resolveSelectedHost();
        if (host == nullptr)
        {
            return;
        }
        auto* inst = host->spike01LiveInstanceForDiagnostics();
        if (inst == nullptr)
        {
            setStatus("Live instance unavailable (no instrument / layout failed).");
            return;
        }

        const juce::String phase = currentPhaseId();
        juce::String last;
        for (int i = 0; i < repeats; ++i)
        {
            spike01::CaptureSample s;
            s.phaseId = phase.toStdString();
            s.capturePath = "raw-getStateInformation";
            s.editorOpen = host->spike01IsNativeEditorOpenForDiagnostics();
            s.transportPlaying = callbacks_.isTransportPlaying && callbacks_.isTransportPlaying();
            s.timestampIso = nowIso().toStdString();
            s.onMessageThread = onMessageThreadNow();

            juce::MemoryBlock mb; // raw state bytes: stack-scoped, hashed, then discarded
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            inst->getStateInformation(mb);
            const double t1 = juce::Time::getMillisecondCounterHiRes();

            s.durationMs = t1 - t0;
            s.blobBytes = (std::uint64_t)mb.getSize();
            s.sha256Hex = spike01::Sha256::hashHex(mb.getData(), mb.getSize());
            samples_.push_back(s);
            reportWrittenSinceLastData_ = false;
            appendSessionLog("sample: phase=" + phase + " path=raw plugin=\""
                             + host->getInstrumentNameForUi() + "\" ms="
                             + juce::String(s.durationMs, 3) + " bytes="
                             + juce::String((juce::int64)s.blobBytes) + " sha256="
                             + juce::String(s.sha256Hex)
                             + (s.editorOpen ? " editor=open" : " editor=closed")
                             + (s.transportPlaying ? " transport=playing" : " transport=stopped")
                             + (s.onMessageThread ? " thread=message" : " thread=OTHER"));
            last = describeSample(s);
        }
        setStatus("Captured x" + juce::String(repeats) + " [" + phase + "] raw. Last: " + last
                  + "\nTotal samples: " + juce::String((int)samples_.size()));
    }

    void captureViaSavePath()
    {
        auto* host = resolveSelectedHost();
        if (host == nullptr)
        {
            return;
        }
        const juce::String phase = currentPhaseId();

        spike01::CaptureSample s;
        s.phaseId = phase.toStdString();
        s.capturePath = "save-path-base64";
        s.editorOpen = host->spike01IsNativeEditorOpenForDiagnostics();
        s.transportPlaying = callbacks_.isTransportPlaying && callbacks_.isTransportPlaying();
        s.timestampIso = nowIso().toStdString();
        s.onMessageThread = onMessageThreadNow();

        const double t0 = juce::Time::getMillisecondCounterHiRes();
        const juce::String b64 = host->getCurrentInstrumentStateBase64();
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        s.durationMs = t1 - t0;

        juce::MemoryOutputStream decoded; // raw bytes: stack-scoped, hashed, then discarded
        if (b64.isNotEmpty())
        {
            juce::Base64::convertFromBase64(decoded, b64);
        }
        s.blobBytes = (std::uint64_t)decoded.getDataSize();
        s.sha256Hex = spike01::Sha256::hashHex(decoded.getData(), decoded.getDataSize());
        samples_.push_back(s);
        reportWrittenSinceLastData_ = false;
        appendSessionLog("sample: phase=" + phase + " path=save-b64 plugin=\""
                         + host->getInstrumentNameForUi() + "\" ms="
                         + juce::String(s.durationMs, 3) + " bytes="
                         + juce::String((juce::int64)s.blobBytes) + " sha256="
                         + juce::String(s.sha256Hex)
                         + (s.editorOpen ? " editor=open" : " editor=closed")
                         + (s.transportPlaying ? " transport=playing" : " transport=stopped")
                         + (s.onMessageThread ? " thread=message" : " thread=OTHER"));

        setStatus("Captured [" + phase + "] via production Save path. " + describeSample(s)
                  + "\nTotal samples: " + juce::String((int)samples_.size()));
    }

    /// F1 conceptual render-enqueue checkpoint: a fresh raw capture and a production
    /// Save-path capture back to back; the report compares their hashes for agreement.
    void runSnapshotCheckpoint()
    {
        const int before = (int)samples_.size();
        const int savedPhaseSel = phaseBox_.getSelectedId();
        // Force the F1 phase for both captures.
        const auto& phases = spike01::requiredPhases();
        for (size_t i = 0; i < phases.size(); ++i)
        {
            if (juce::String(phases[i].id) == "F1")
            {
                phaseBox_.setSelectedId((int)i + 1, juce::dontSendNotification);
            }
        }
        captureRaw(1);
        captureViaSavePath();
        phaseBox_.setSelectedId(savedPhaseSel, juce::dontSendNotification);

        if ((int)samples_.size() == before + 2)
        {
            const auto& a = samples_[samples_.size() - 2];
            const auto& b = samples_.back();
            setStatus(juce::String("F1 checkpoint: raw and Save-path hashes ")
                      + (a.sha256Hex == b.sha256Hex ? "MATCH" : "DIFFER") + ".\nraw:  "
                      + describeSample(a) + "\nsave: " + describeSample(b));
        }
    }

    [[nodiscard]] static juce::String describeSample(const spike01::CaptureSample& s)
    {
        return juce::String(s.durationMs, 2) + " ms, " + juce::String((juce::int64)s.blobBytes)
               + " bytes, sha " + juce::String(s.sha256Hex.substr(0, 12)) + "…, "
               + (s.onMessageThread ? "message thread" : "OTHER THREAD")
               + (s.editorOpen ? ", editor open" : ", editor closed")
               + (s.transportPlaying ? ", playing" : ", stopped");
    }

    //==========================================================================
    // Parameter/processor notification instrumentation (attach/detach explicit)
    //==========================================================================
    void toggleListener()
    {
        if (attachedInstance_ != nullptr)
        {
            detachListenerIfPossible();
            listenerButton_.setButtonText("Attach parameter listener");
            updateListenerLabel();
            return;
        }
        auto* host = resolveSelectedHost();
        if (host == nullptr)
        {
            return;
        }
        auto* inst = host->spike01LiveInstanceForDiagnostics();
        if (inst == nullptr)
        {
            setStatus("Live instance unavailable; cannot attach listener.");
            return;
        }
        inst->addListener(this);
        attachedInstance_ = inst;
        attachedTrackId_ = selectedTrackId();
        attachedHostGuard_ = host->asyncAliveGuard();
        listenerButton_.setButtonText("Detach parameter listener");
        setStatus("Parameter listener attached. Do not unload/replace the instrument while "
                  "attached; detach first.");
        updateListenerLabel();
    }

    void detachListenerIfPossible()
    {
        if (attachedInstance_ == nullptr)
        {
            return;
        }
        // The host (and with it the instance) may already be gone (track deleted /
        // instrument replaced). Only touch the instance when the same host still
        // reports the same live instance.
        if (attachedHostGuard_.isAlive() && callbacks_.resolveHostForTrack)
        {
            if (auto* host = callbacks_.resolveHostForTrack(attachedTrackId_))
            {
                if (host->spike01LiveInstanceForDiagnostics() == attachedInstance_)
                {
                    attachedInstance_->removeListener(this);
                }
            }
        }
        attachedInstance_ = nullptr;
        attachedTrackId_ = kInvalidTrackId;
        attachedHostGuard_ = {};
    }

    // juce::AudioProcessorListener — may be invoked from ANY thread by the plugin/wrapper;
    // record under a lock and never touch UI here except via the message-thread check.
    void audioProcessorParameterChanged(juce::AudioProcessor* processor,
                                        int parameterIndex,
                                        float newValue) override
    {
        recordEvent(processor, "paramChanged", parameterIndex, newValue, {});
    }

    void audioProcessorChanged(juce::AudioProcessor* processor,
                               const ChangeDetails& details) override
    {
        juce::String d;
        if (details.latencyChanged) d << "latencyChanged ";
        if (details.parameterInfoChanged) d << "parameterInfoChanged ";
        if (details.programChanged) d << "programChanged ";
        if (details.nonParameterStateChanged) d << "nonParameterStateChanged ";
        recordEvent(processor, "processorChanged", -1, 0.0f, d.trim());
    }

    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor* processor,
                                                   int parameterIndex) override
    {
        recordEvent(processor, "gestureBegin", parameterIndex, 0.0f, {});
    }

    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor* processor,
                                                 int parameterIndex) override
    {
        recordEvent(processor, "gestureEnd", parameterIndex, 0.0f, {});
    }

    void recordEvent(juce::AudioProcessor* processor,
                     const char* kind,
                     const int parameterIndex,
                     const float newValue,
                     const juce::String& detail)
    {
        spike01::ParamEvent e;
        e.kind = kind;
        e.parameterIndex = parameterIndex;
        e.newValue = newValue;
        e.onMessageThread = onMessageThreadNow();
        e.detail = detail.toStdString();
        e.timestampIso = nowIso().toStdString();
        // Resolve the display name only on the message thread; calling into the plugin
        // from an arbitrary notification thread is not lifecycle-safe.
        if (e.onMessageThread && processor != nullptr && parameterIndex >= 0)
        {
            const auto& params = processor->getParameters();
            if (parameterIndex < params.size())
            {
                e.parameterName = params[parameterIndex]->getName(64).toStdString();
            }
        }
        const juce::String logLine = "event: kind=" + juce::String(e.kind) + " idx="
                                     + juce::String(e.parameterIndex) + " name=\""
                                     + juce::String(e.parameterName) + "\" value="
                                     + juce::String(e.newValue, 4)
                                     + (e.onMessageThread ? " thread=message" : " thread=other")
                                     + (e.detail.empty() ? juce::String()
                                                         : " detail=" + juce::String(e.detail));
        {
            const juce::ScopedLock sl(eventsLock_);
            events_.push_back(std::move(e));
        }
        reportWrittenSinceLastData_ = false;
        // UI refresh and disk logging happen on the message thread only.
        if (onMessageThreadNow())
        {
            appendSessionLog(logLine);
            updateListenerLabel();
        }
        else
        {
            juce::Component::SafePointer<Content> safe(this);
            juce::MessageManager::callAsync([safe, logLine] {
                appendSessionLog(logLine);
                if (safe != nullptr)
                {
                    safe->updateListenerLabel();
                }
            });
        }
    }

    void updateListenerLabel()
    {
        size_t n = 0, offThread = 0;
        juce::String lastLine = "(none)";
        {
            const juce::ScopedLock sl(eventsLock_);
            n = events_.size();
            for (const auto& e : events_)
            {
                offThread += e.onMessageThread ? 0u : 1u;
            }
            if (!events_.empty())
            {
                const auto& e = events_.back();
                lastLine = juce::String(e.kind) + " idx=" + juce::String(e.parameterIndex)
                           + (e.parameterName.empty() ? juce::String()
                                                      : " \"" + juce::String(e.parameterName) + "\"")
                           + " val=" + juce::String(e.newValue, 3)
                           + (e.onMessageThread ? " [message]" : " [other thread]")
                           + (e.detail.empty() ? juce::String()
                                               : " (" + juce::String(e.detail) + ")");
            }
        }
        listenerLabel_.setText("Listener: " + juce::String(attachedInstance_ != nullptr
                                                               ? "ATTACHED"
                                                               : "detached")
                                   + " — " + juce::String((juce::int64)n) + " notification(s), "
                                   + juce::String((juce::int64)offThread)
                                   + " off the message thread.\nLast: " + lastLine,
                               juce::dontSendNotification);
    }

    //==========================================================================
    // Sanitized report
    //==========================================================================
    void writeReport()
    {
        spike01::ReportHeader hdr;
        hdr.appVersion = callbacks_.appVersion.toStdString();
        hdr.generatedAtIso = nowIso().toStdString();
        hdr.machineNotes = noteEditor_.getText().trim().toStdString();

        if (auto* host = resolveSelectedHost())
        {
            juce::PluginDescription desc;
            if (host->getLastLoadedPluginDescription(desc))
            {
                hdr.pluginName = desc.name.toStdString();
                hdr.pluginFormat = desc.pluginFormatName.toStdString();
                hdr.pluginVersion = desc.version.toStdString();
                hdr.pluginIdentifier = desc.fileOrIdentifier.toStdString();
            }
        }

        std::vector<spike01::ParamEvent> eventsCopy;
        {
            const juce::ScopedLock sl(eventsLock_);
            eventsCopy = events_;
        }
        const std::string md = spike01::buildReportMarkdown(hdr, samples_, eventsCopy, notes_);

        const juce::File out =
            juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
                .getChildFile("SPIKE01_STATE_CAPTURE_REPORT_"
                              + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ".md");
        if (out.replaceWithText(juce::String(md)))
        {
            reportWrittenSinceLastData_ = true;
            setStatus("Sanitized report written:\n" + out.getFullPathName()
                      + "\nContains sizes/hashes/timings only — no raw state bytes.");
        }
        else
        {
            setStatus("FAILED to write report to " + out.getFullPathName());
        }
    }

    void setStatus(const juce::String& s)
    {
        statusLabel_.setText(s, juce::dontSendNotification);
        appendSessionLog("status: " + s.replace("\n", " | "));
    }

    [[nodiscard]] bool hasAnyEvents()
    {
        const juce::ScopedLock sl(eventsLock_);
        return !events_.empty();
    }

    //==========================================================================
    Spike01PanelCallbacks callbacks_;
    std::vector<Spike01RuntimeChoice> choices_;

    // SPIKE-01B-M auto mode
    juce::String autoPlanId_;
    std::vector<AutoStep> autoSteps_;
    size_t autoIndex_ = 0;
    bool autoActive_ = false;
    juce::String waitTrackNeedle_;
    double waitDeadlineMs_ = 0.0;
    int perturbParamIndex_ = -1;
    float perturbOriginalValue_ = 0.0f;

    spike01::MidiDeliveryCounters midiCounters_;
    MidiSinkAdapter midiSinkAdapter_{ midiCounters_ };
    TrackId m2vSinkHostTrackId_ = kInvalidTrackId;
    const void* m2vInstanceAtInstall_ = nullptr;
    std::uint64_t m2vBoundaryAtInstall_ = 0;

    // P1EF service-integration plan (plan "P1EF" only; see buildP1efPlan). The panel holds
    // only the requested generation for verification — no job, no plugin instance.
    std::uint64_t p1efGeneration_ = 0;

    // P1G proxy-playback plan (plan "P1G" only; see buildP1gPlan): verification
    // bookkeeping only — the panel owns no reader, view or coordinator state.
    std::int64_t p1gAssetLenSamples_ = 0;
    double p1gAssetRate_ = 0.0;
    double p1gOriginalRate_ = 0.0;
    bool p1gRateSwitchSkipped_ = false;
    std::uint64_t p1gBlocksMark_ = 0;
    std::uint32_t p1gWrapMark_ = 0;
    std::vector<TrackId> p1gOtherMuted_;
    juce::File p1gMixdownA_, p1gMixdownB_;

    std::function<int()> waitProbe_; ///< 1 = satisfied, 0 = pending, -1 = failed
    double waitProbeDeadlineMs_ = 0.0;
    juce::String waitProbeDesc_;

    std::vector<spike01::CaptureSample> samples_;
    std::vector<spike01::ParamEvent> events_;   // guarded by eventsLock_
    juce::CriticalSection eventsLock_;
    std::vector<std::string> notes_;

    juce::AudioPluginInstance* attachedInstance_ = nullptr;
    TrackId attachedTrackId_ = kInvalidTrackId;
    mini_daw::AsyncCallbackGuard attachedHostGuard_;

    /// False whenever samples/events/notes exist that are newer than the last written report;
    /// the destructor then auto-writes one (loss-proofing). Atomic: cleared from recordEvent,
    /// which may run on a non-message thread.
    std::atomic<bool> reportWrittenSinceLastData_{ true };

    juce::Label headerLabel_, statusLabel_, listenerLabel_;
    juce::ComboBox trackBox_, phaseBox_;
    juce::TextButton refreshTracksButton_, capture1Button_, capture10Button_, saveCaptureButton_,
        checkpointButton_, listenerButton_, addNoteButton_, writeReportButton_;
    juce::TextEditor customPhaseEditor_, noteEditor_;
};

//==============================================================================
Spike01StateCapturePanel::Spike01StateCapturePanel(Spike01PanelCallbacks callbacks,
                                                   juce::String autoPlanId)
    : juce::DocumentWindow("SPIKE-01 state-capture probe (diagnostic)",
                           juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                               juce::ResizableWindow::backgroundColourId),
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    auto content = std::make_unique<Content>(std::move(callbacks), std::move(autoPlanId));
    content_ = content.get();
    setContentOwned(content.release(), true);
    setResizable(true, true);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
}

Spike01StateCapturePanel::~Spike01StateCapturePanel() = default;

void Spike01StateCapturePanel::closeButtonPressed()
{
    // Hide only: recorded samples/notes stay available until the app closes, so the
    // operator cannot lose measurements by accidentally closing the window.
    setVisible(false);
}
