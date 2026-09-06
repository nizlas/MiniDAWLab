// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE. See Spike01StateCapturePanel.h.
// ============================================================================

#include "diagnostics/Spike01StateCapturePanel.h"

#include "diagnostics/Spike01MidiDeliveryCounters.h"
#include "diagnostics/Spike01ReportFormat.h"
#include "diagnostics/Spike01Sha256.h"
#include "diagnostics/Spike02RenderHarness.h" // SPIKE-02 isolated-render harness (S2* plans only)
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
        else if (autoPlanId_.startsWith("S2")) // SPIKE-02: isolated render-instance measurements
        {
            buildSpike02Plan();
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
    // SPIKE-02 (isolated render-instance lifecycle / throughput / contention /
    // snapshot initial-condition / latency / tail; steering §9.4.4, §14, §15,
    // §21 PID-004/PID-005, §22 P1D). Plans: S2A, S2B, S2C (S2CN = nonRealtime
    // off), S2D, S2E, S2F, S2G. All heavy machinery lives in
    // diagnostics/Spike02RenderHarness.h; the panel only sequences steps on the
    // message thread and polls the worker's done flag through waitProbe_.
    //==========================================================================

    static constexpr double kS2SampleRate = 48000.0;

    [[nodiscard]] static std::int64_t s2Samples(const double seconds)
    {
        return (std::int64_t)std::llround(seconds * kS2SampleRate);
    }

    void ensureS2()
    {
        if (s2_ == nullptr)
        {
            s2_ = std::make_unique<spike02::Controller>();
        }
    }

    /// Capture the SELECTED track's live-instance state into a named controller slot
    /// (message thread, Save-path precedent) and remember the live pointer for identity checks.
    [[nodiscard]] bool s2CaptureLive(const juce::String& slot)
    {
        ensureS2();
        auto* host = resolveSelectedHost();
        auto* inst = host != nullptr ? host->spike01LiveInstanceForDiagnostics() : nullptr;
        if (inst == nullptr)
        {
            return false;
        }
        s2LivePtr_ = (const void*)inst;
        return s2_->captureStateToSlot(*inst, slot);
    }

    /// Create + restore + configure + prepare an isolated instance of the SELECTED track's
    /// plugin (message thread). Logs the live-vs-render identity comparison.
    [[nodiscard]] bool s2CreateFromSelected(const spike02::RenderConfig& cfg,
                                            const juce::String& slot,
                                            const bool resetAfterPrepare)
    {
        ensureS2();
        auto* host = resolveSelectedHost();
        juce::PluginDescription desc;
        if (host == nullptr || !host->getLastLoadedPluginDescription(desc))
        {
            return false;
        }
        if (!s2_->createIsolatedPlugin(desc, cfg, slot, resetAfterPrepare))
        {
            return false;
        }
        auto* iso = s2_->isolatedForMessageThreadChecks();
        spike02::log("identity: liveInstance=" + spike02::ptrHex(s2LivePtr_) + " renderInstance="
                     + spike02::ptrHex(iso)
                     + (iso != nullptr && (const void*)iso != s2LivePtr_ ? " DIFFERENT (required)"
                                                                         : " SAME (VIOLATION)"));
        return iso != nullptr && (const void*)iso != s2LivePtr_;
    }

    /// Arm the generic wait probe for "current render job finished".
    void addS2WaitJob(const juce::String& desc, const double timeoutSec)
    {
        autoSteps_.push_back({ 0, "wait: " + desc, [this, desc, timeoutSec] {
                                  waitProbeDesc_ = desc;
                                  waitProbeDeadlineMs_ = juce::Time::getMillisecondCounterHiRes()
                                                         + timeoutSec * 1000.0;
                                  waitProbe_ = [this] {
                                      return (s2_ != nullptr && !s2_->jobRunning()) ? 1 : 0;
                                  };
                                  return true;
                              } });
    }

    [[nodiscard]] bool s2FinishJob()
    {
        return s2_ != nullptr && s2_->finishJob(s2LastResult_);
    }

    void s2LogPerSecond(const spike02::RenderResult& r, const int maxSeconds)
    {
        const int n = juce::jmin((int)r.perSecond.size(), maxSeconds);
        for (int i = 0; i < n; ++i)
        {
            spike02::log("persec[" + r.label + "]: sec=" + juce::String(i) + " peakDb="
                         + juce::String(spike02::dbfsFromLinear(r.perSecond[(size_t)i].peak), 1)
                         + " rmsDb="
                         + juce::String(spike02::dbfsFromLinear(r.perSecond[(size_t)i].rms), 1));
        }
    }

    void s2EvaluateTailAndLog(const spike02::RenderResult& r, const juce::String& tag)
    {
        const double blockSec = (double)s2_->lastConfig().blockSize / kS2SampleRate;
        const auto idle = spike02::summarizeLevels(r.blockLevels);
        spike02::log("tailSeries[" + tag + "]: blocks=" + juce::String((juce::int64)idle.blocks)
                     + " maxPeakDb=" + juce::String(idle.maxPeakDb, 1) + " meanRmsDb="
                     + juce::String(idle.meanRmsDb, 1) + " maxRmsDb="
                     + juce::String(idle.maxRmsDb, 1) + " blockSec=" + juce::String(blockSec, 4));
        for (const auto& c : spike02::evaluateTailCandidateGrid(r.blockLevels, blockSec))
        {
            spike02::log("tailCandidate[" + tag + "]: X=" + juce::String(c.candidate.thresholdDb, 0)
                         + "dB Y=" + juce::String(c.candidate.windowSec, 1) + "s Z="
                         + juce::String(c.candidate.capSec, 0) + "s -> "
                         + (c.completed ? "COMPLETED" : "FAILED_AT_CAP") + " tailSec="
                         + juce::String(c.tailSec, 2) + " decisionSec="
                         + juce::String(c.decisionSec, 2) + " peakAfterDb="
                         + juce::String(c.peakAfterDecisionDb, 1) + " roseAgain="
                         + (c.roseAboveThresholdAfterDecision ? "TRUE" : "false"));
        }
    }

    void s2LogAudioLoad(const juce::String& tag)
    {
        if (!callbacks_.snapshotAudioLoad)
        {
            spike02::log("audioLoad[" + tag + "]: unavailable (no callback)");
            return;
        }
        const auto s = callbacks_.snapshotAudioLoad();
        spike02::log("audioLoad[" + tag + "]: blocks=" + juce::String((juce::int64)s.blocks)
                     + " meanMs=" + juce::String(s.meanMs, 3) + " maxMs=" + juce::String(s.maxMs, 3)
                     + " meanBudget%=" + juce::String(s.meanBudgetPercent, 1) + " maxBudget%="
                     + juce::String(s.maxBudgetPercent, 1) + " nearOverruns(>=70%)="
                     + juce::String((int)s.nearOverruns) + " overruns(>=100%)="
                     + juce::String((int)s.overruns) + " blockSamples="
                     + juce::String(s.lastBlockSamples) + " sr=" + juce::String(s.sampleRate, 0));
    }

    void s2DiscardAudioLoad()
    {
        if (callbacks_.snapshotAudioLoad)
        {
            (void)callbacks_.snapshotAudioLoad();
        }
    }

    /// Message-thread scheduling-jitter probe (SPIKE-02 D: UI responsiveness under contention).
    struct S2JitterProbe final : juce::Timer
    {
        std::vector<double> deltasMs;
        double lastMs = 0.0;

        void begin()
        {
            deltasMs.clear();
            lastMs = juce::Time::getMillisecondCounterHiRes();
            startTimer(50);
        }

        void timerCallback() override
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            deltasMs.push_back(now - lastMs);
            lastMs = now;
        }

        void endAndLog(const juce::String& tag)
        {
            stopTimer();
            const auto st = spike02::computeTimingStats(deltasMs);
            spike02::log("uiJitter[" + tag + "]: ticks=" + juce::String((juce::int64)st.count)
                         + " target=50ms meanMs=" + juce::String(st.meanMs, 1) + " medianMs="
                         + juce::String(st.medianMs, 1) + " p95Ms=" + juce::String(st.p95Ms, 1)
                         + " maxMs=" + juce::String(st.maxMs, 1));
        }
    };

    void buildSpike02Plan()
    {
        auto add = [this](int delayMs, juce::String desc, std::function<bool()> run) {
            autoSteps_.push_back({ delayMs, std::move(desc), std::move(run) });
        };
        const auto organSchedule = [](const double seconds) {
            return spike02::makeOrganPatternSchedule(kS2SampleRate, s2Samples(seconds));
        };
        const auto vb3Cfg = [](const int bs, const bool nrt) {
            spike02::RenderConfig c;
            c.sampleRate = kS2SampleRate;
            c.blockSize = bs;
            c.nonRealtime = nrt;
            return c;
        };

        if (autoPlanId_ == "S2A") // lifecycle + isolation (synthetic, then VB3 during live playback)
        {
            addWaitForTrackStep("Organ");
            add(1000, "A1 synthetic create+prepare", [this, vb3Cfg] {
                ensureS2();
                return s2_->createIsolatedSynthetic(vb3Cfg(512, true));
            });
            add(100, "A1 synthetic 10s render start", [this] {
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(10.0);
                c.label = "A1-synth";
                std::vector<spike02::ScheduledMidi> sched;
                sched.push_back(spike02::noteOn(s2Samples(1.0), 1, 60, 100));
                sched.push_back(spike02::noteOff(s2Samples(1.5), 1, 60));
                return s2_->startJob(c, std::move(sched));
            });
            addS2WaitJob("A1 synthetic render", 60.0);
            add(100, "A1 finish + latency-position check", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                const std::int64_t expected =
                    s2Samples(1.0) + spike02::SyntheticLatencyInstrument::kFixedLatencySamples;
                spike02::log(juce::String("check[A1]: firstNonZero=")
                             + juce::String(s2LastResult_.firstNonZeroSample) + " expected="
                             + juce::String(expected)
                             + (s2LastResult_.firstNonZeroSample == expected ? " OK" : " MISMATCH"));
                return true;
            });
            add(100, "A2 synthetic cancellation target start", [this, vb3Cfg] {
                if (!s2_->createIsolatedSynthetic(vb3Cfg(512, true)))
                {
                    return false;
                }
                spike02::RenderConfig c = vb3Cfg(512, true);
                c.totalSamples = s2Samples(600.0);
                c.realtimePaced = true; // guarantee it is still running when we cancel
                c.label = "A2-synth-cancel";
                return s2_->startJob(c, {});
            });
            add(500, "A2 request cancel", [this] {
                s2_->requestCancel();
                return true;
            });
            addS2WaitJob("A2 cancelled render", 30.0);
            add(100, "A2 finish (cancellation latency in log)", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                s2_->teardownIsolated("A2-after-cancel");
                return s2LastResult_.cancelled;
            });
            add(500, "A3 capture VB3 state (quiescent)", [this] { return s2CaptureLive("A"); });
            add(100, "A3 create isolated VB3 (restore A)", [this, vb3Cfg] {
                return s2CreateFromSelected(vb3Cfg(512, true), "A", false);
            });
            add(200, "A3 start live transport playback", [this] {
                if (!callbacks_.startTransport)
                {
                    return false;
                }
                callbacks_.startTransport();
                return true;
            });
            add(1000, "A3 render 30s on worker during live playback", [this, organSchedule] {
                s2DiscardAudioLoad();
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(30.0);
                c.label = "A3-vb3-during-playback";
                return s2_->startJob(c, organSchedule(30.0));
            });
            addS2WaitJob("A3 VB3 render", 180.0);
            add(100, "A3 finish + isolation evidence", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                const bool playing = callbacks_.isTransportPlaying && callbacks_.isTransportPlaying();
                const juce::String wraps = callbacks_.readCycleWrapCount
                                               ? juce::String(callbacks_.readCycleWrapCount())
                                               : juce::String("n/a");
                s2LogAudioLoad("A3-during-render-playback");
                auto* host = resolveSelectedHost();
                auto* liveNow = host != nullptr ? host->spike01LiveInstanceForDiagnostics() : nullptr;
                spike02::log(juce::String("check[A3]: transportStillPlaying=")
                             + (playing ? "true" : "FALSE") + " cycleWraps=" + wraps
                             + " livePtrUnchanged="
                             + ((const void*)liveNow == s2LivePtr_ ? "true" : "FALSE")
                             + " liveHasInstrument="
                             + (host != nullptr && host->hasInstrument() ? "true" : "FALSE"));
                if (callbacks_.stopTransport)
                {
                    callbacks_.stopTransport();
                }
                s2_->teardownIsolated("A3-done");
                return true;
            });
        }
        else if (autoPlanId_ == "S2B") // offline modes x block sizes (VB3, 60 s each)
        {
            addWaitForTrackStep("Organ");
            add(1000, "B capture VB3 state", [this] { return s2CaptureLive("B"); });
            struct ModeDef
            {
                const char* tag;
                bool nonRealtime;
                bool paced;
            };
            static constexpr ModeDef kModes[] = { { "nrtOff-fast", false, false },
                                                  { "nrtOn-fast", true, false },
                                                  { "paced", false, true } };
            for (const auto& mode : kModes)
            {
                for (const int bs : { 256, 512, 1024 })
                {
                    const juce::String label =
                        "B-" + juce::String(mode.tag) + "-bs" + juce::String(bs);
                    add(200, label + " create", [this, mode, bs] {
                        spike02::RenderConfig c;
                        c.sampleRate = kS2SampleRate;
                        c.blockSize = bs;
                        c.nonRealtime = mode.nonRealtime;
                        return s2CreateFromSelected(c, "B", false);
                    });
                    add(100, label + " render 60s", [this, mode, bs, label, organSchedule] {
                        spike02::RenderConfig c;
                        c.sampleRate = kS2SampleRate;
                        c.blockSize = bs;
                        c.nonRealtime = mode.nonRealtime;
                        c.realtimePaced = mode.paced;
                        c.totalSamples = s2Samples(60.0);
                        c.label = label;
                        return s2_->startJob(c, organSchedule(60.0));
                    });
                    addS2WaitJob(label, mode.paced ? 150.0 : 300.0);
                    add(100, label + " finish", [this] { return s2FinishJob(); });
                }
            }
            add(100, "B teardown", [this] {
                s2_->teardownIsolated("B-done");
                return true;
            });
        }
        else if (autoPlanId_ == "S2C" || autoPlanId_ == "S2CN") // ten-minute benchmark (+WAV) + tail
        {
            const bool nrt = autoPlanId_ == "S2C";
            addWaitForTrackStep("Organ");
            add(1000, "C capture VB3 state", [this] { return s2CaptureLive("C"); });
            add(100, "C create isolated (bs=512)", [this, vb3Cfg, nrt] {
                return s2CreateFromSelected(vb3Cfg(512, nrt), "C", false);
            });
            add(100, "C render 600s with WAV", [this, organSchedule, nrt] {
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = nrt;
                c.totalSamples = s2Samples(600.0);
                c.label = nrt ? "C-600s-nrtOn" : "C-600s-nrtOff";
                c.wavPath = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("MiniDAWLab-spike02")
                                .getChildFile("s2c-render.wav")
                                .getFullPathName();
                return s2_->startJob(c, organSchedule(600.0));
            });
            addS2WaitJob("C ten-minute render", 1500.0);
            add(100, "C finish + delete WAV artifact", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                const juce::File wav = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                           .getChildFile("MiniDAWLab-spike02")
                                           .getChildFile("s2c-render.wav");
                spike02::log("artifact[C]: wavBytes=" + juce::String(wav.getSize()) + " deleted="
                             + (wav.deleteFile() ? "true" : "FALSE"));
                (void)wav.getParentDirectory().deleteRecursively();
                return true;
            });
            add(100, "C tail (silence-fed, 60s cap)", [this] {
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = s2_->lastConfig().nonRealtime;
                c.totalSamples = s2Samples(60.0);
                c.levelsFromSample = 0;
                c.label = "C-tail";
                return s2_->startJob(c, {});
            });
            addS2WaitJob("C tail render", 300.0);
            add(100, "C tail finish + evaluate", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                s2EvaluateTailAndLog(s2LastResult_, "C-tail");
                s2_->teardownIsolated("C-done");
                return true;
            });
        }
        else if (autoPlanId_ == "S2D") // playback contention + resource policy
        {
            addWaitForTrackStep("Organ");
            add(1000, "D capture VB3 state", [this] { return s2CaptureLive("D"); });
            add(100, "D create isolated (512, nrtOn)", [this, vb3Cfg] {
                return s2CreateFromSelected(vb3Cfg(512, true), "D", false);
            });
            add(200, "D1 baseline: start transport + probes", [this] {
                if (!callbacks_.startTransport)
                {
                    return false;
                }
                callbacks_.startTransport();
                s2DiscardAudioLoad();
                s2Jitter_.begin();
                return true;
            });
            add(30000, "D1 baseline read (30s playback alone)", [this] {
                s2LogAudioLoad("D1-baseline-playback-alone");
                s2Jitter_.endAndLog("D1-baseline");
                return true;
            });
            add(500, "D2 unpaced render start (during playback)", [this, organSchedule] {
                s2DiscardAudioLoad();
                s2Jitter_.begin();
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(3600.0); // cancelled after the observation window
                c.label = "D2-unpaced";
                return s2_->startJob(c, organSchedule(3600.0));
            });
            add(45000, "D2 read (45s contention) + cancel", [this] {
                s2LogAudioLoad("D2-playback-plus-unpaced-render");
                s2Jitter_.endAndLog("D2-unpaced");
                s2_->requestCancel();
                return true;
            });
            addS2WaitJob("D2 cancelled render", 30.0);
            add(100, "D2 finish", [this] { return s2FinishJob(); });
            add(200, "D3 recreate isolated (clean state)", [this, vb3Cfg] {
                return s2CreateFromSelected(vb3Cfg(512, true), "D", false);
            });
            add(200, "D3 yielding render start (during playback)", [this, organSchedule] {
                s2DiscardAudioLoad();
                s2Jitter_.begin();
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(3600.0);
                c.yieldEveryBlocks = 4;
                c.yieldMs = 3;
                c.label = "D3-yield";
                return s2_->startJob(c, organSchedule(3600.0));
            });
            add(45000, "D3 read (45s cooperative) + cancel", [this] {
                s2LogAudioLoad("D3-playback-plus-yielding-render");
                s2Jitter_.endAndLog("D3-yield");
                s2_->requestCancel();
                return true;
            });
            addS2WaitJob("D3 cancelled render", 30.0);
            add(100, "D3 finish + stop transport", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                if (callbacks_.stopTransport)
                {
                    callbacks_.stopTransport();
                }
                s2_->teardownIsolated("D-done");
                return true;
            });
        }
        else if (autoPlanId_ == "S2E") // snapshot initial-condition validation (P1D lifecycle)
        {
            addWaitForTrackStep("Organ");
            add(1000, "E capture Q (host-observable quiescence)", [this] {
                return s2CaptureLive("Q");
            });
            add(200, "E install delivery sink + seek 0 + start transport", [this] {
                auto* host = resolveSelectedHost();
                if (host == nullptr || !callbacks_.seekTransport || !callbacks_.startTransport)
                {
                    return false;
                }
                host->installMidiDeliveryCaptureSinkForTests(&midiSinkAdapter_);
                m2vSinkHostTrackId_ = selectedTrackId();
                callbacks_.seekTransport(0);
                callbacks_.startTransport();
                return true;
            });
            add(3500, "E capture M during PROVEN MIDI/CC delivery", [this] {
                const auto n = midiCounters_.noteOn.load(std::memory_order_relaxed);
                const auto c = midiCounters_.cc.load(std::memory_order_relaxed);
                spike02::log("delivery[E]: " + midiCounters_.summary());
                if (n == 0 || c == 0)
                {
                    spike02::log("delivery[E]: INSUFFICIENT (noteOn=" + juce::String((juce::int64)n)
                                 + " cc=" + juce::String((juce::int64)c) + ") — aborting");
                    return false;
                }
                const bool ok = s2CaptureLive("M");
                spike02::log(juce::String("capture[E-M]: duringTransportPlaying=")
                             + ((callbacks_.isTransportPlaying && callbacks_.isTransportPlaying())
                                    ? "true"
                                    : "FALSE"));
                return ok;
            });
            add(300, "E stop transport + clear sink", [this] {
                if (callbacks_.stopTransport)
                {
                    callbacks_.stopTransport();
                }
                clearMidiSinkIfInstalled();
                return true;
            });
            // Identical P1D lifecycle for both snapshots: restore -> prepare -> reset ->
            // deterministic chase prefix -> render from "project start".
            struct ERun
            {
                const char* slot;
                const char* label;
                bool reset;
                bool chase;
            };
            static constexpr ERun kERuns[] = { { "Q", "E-RQ", true, true },
                                               { "M", "E-RM", true, true },
                                               { "M", "E-RM-noreset", false, false } };
            for (const auto& run : kERuns)
            {
                add(500, juce::String(run.label) + " create+render 30s", [this, run, vb3Cfg,
                                                                          organSchedule] {
                    if (!s2CreateFromSelected(vb3Cfg(512, true), run.slot, run.reset))
                    {
                        return false;
                    }
                    spike02::RenderConfig c = vb3Cfg(512, true);
                    c.totalSamples = s2Samples(30.0);
                    c.label = run.label;
                    auto sched = organSchedule(30.0);
                    if (run.chase)
                    {
                        auto chase = spike02::makeChasePrefix();
                        sched.insert(sched.begin(), chase.begin(), chase.end());
                    }
                    return s2_->startJob(c, std::move(sched));
                });
                addS2WaitJob(juce::String(run.label) + " render", 300.0);
                add(100, juce::String(run.label) + " finish", [this] {
                    if (!s2FinishJob())
                    {
                        return false;
                    }
                    s2LogPerSecond(s2LastResult_, 30);
                    return true;
                });
            }
            add(100, "E compare per-second profiles", [this] {
                const auto& all = s2_->allResults();
                const spike02::RenderResult* rq = nullptr;
                const spike02::RenderResult* rm = nullptr;
                const spike02::RenderResult* rmNr = nullptr;
                for (const auto& r : all)
                {
                    if (r.label == "E-RQ") rq = &r;
                    else if (r.label == "E-RM") rm = &r;
                    else if (r.label == "E-RM-noreset") rmNr = &r;
                }
                const auto compare = [](const spike02::RenderResult& a,
                                        const spike02::RenderResult& b) {
                    const size_t n = juce::jmin(a.perSecond.size(), b.perSecond.size());
                    double maxD = 0.0, sumD = 0.0;
                    for (size_t i = 0; i < n; ++i)
                    {
                        const double d = std::abs(spike02::dbfsFromLinear(a.perSecond[i].rms)
                                                  - spike02::dbfsFromLinear(b.perSecond[i].rms));
                        maxD = juce::jmax(maxD, d);
                        sumD += d;
                    }
                    return juce::String("secs=") + juce::String((juce::int64)n) + " maxRmsDeltaDb="
                           + juce::String(maxD, 2) + " meanRmsDeltaDb="
                           + juce::String(n > 0 ? sumD / (double)n : 0.0, 2);
                };
                if (rq != nullptr && rm != nullptr)
                {
                    spike02::log("compare[E]: RQ-vs-RM " + compare(*rq, *rm));
                }
                if (rq != nullptr && rmNr != nullptr)
                {
                    spike02::log("compare[E]: RQ-vs-RM-noreset " + compare(*rq, *rmNr));
                }
                s2_->teardownIsolated("E-done");
                return rq != nullptr && rm != nullptr;
            });
        }
        else if (autoPlanId_ == "S2F") // latency reporting + preservation (synthetic + VB3)
        {
            addWaitForTrackStep("Organ");
            add(500, "F capture VB3 state", [this] { return s2CaptureLive("F"); });
            for (const bool nrt : { false, true })
            {
                for (const int bs : { 256, 512, 1024 })
                {
                    const juce::String tag =
                        "F-synth-bs" + juce::String(bs) + (nrt ? "-nrtOn" : "-nrtOff");
                    add(100, tag + " create+render 3s", [this, bs, nrt, tag] {
                        spike02::RenderConfig c;
                        c.sampleRate = kS2SampleRate;
                        c.blockSize = bs;
                        c.nonRealtime = nrt;
                        if (!s2_->createIsolatedSynthetic(c))
                        {
                            return false;
                        }
                        c.totalSamples = s2Samples(3.0);
                        c.label = tag;
                        std::vector<spike02::ScheduledMidi> sched;
                        sched.push_back(spike02::noteOn(s2Samples(1.0), 1, 60, 100));
                        sched.push_back(spike02::noteOff(s2Samples(1.5), 1, 60));
                        return s2_->startJob(c, std::move(sched));
                    });
                    addS2WaitJob(tag, 60.0);
                    add(100, tag + " finish + verify", [this, bs] {
                        if (!s2FinishJob())
                        {
                            return false;
                        }
                        const std::int64_t expected =
                            s2Samples(1.0)
                            + spike02::SyntheticLatencyInstrument::kFixedLatencySamples;
                        const bool latencyOk =
                            s2LastResult_.latencySamplesAtStart
                                == spike02::SyntheticLatencyInstrument::kFixedLatencySamples
                            && s2LastResult_.latencySamplesAtEnd
                                   == spike02::SyntheticLatencyInstrument::kFixedLatencySamples;
                        spike02::log("check[" + s2LastResult_.label + "]: latencyReported="
                                     + juce::String(s2LastResult_.latencySamplesAtStart)
                                     + (latencyOk ? " OK" : " CHANGED(!)") + " firstNonZero="
                                     + juce::String(s2LastResult_.firstNonZeroSample) + " expected="
                                     + juce::String(expected)
                                     + (s2LastResult_.firstNonZeroSample == expected
                                            ? " PRESERVED"
                                            : " SHIFTED(!)")
                                     + " blocks=" + juce::String((juce::int64)s2LastResult_.blocks)
                                     + " expectedBlocks="
                                     + juce::String((s2Samples(3.0) + bs - 1) / bs));
                        return true;
                    });
                }
            }
            for (const bool nrt : { false, true })
            {
                for (const int bs : { 256, 512, 1024 })
                {
                    const juce::String tag =
                        "F-vb3-bs" + juce::String(bs) + (nrt ? "-nrtOn" : "-nrtOff");
                    add(100, tag + " create+render 2s", [this, bs, nrt, tag, organSchedule] {
                        spike02::RenderConfig c;
                        c.sampleRate = kS2SampleRate;
                        c.blockSize = bs;
                        c.nonRealtime = nrt;
                        if (!s2CreateFromSelected(c, "F", false))
                        {
                            return false;
                        }
                        c.totalSamples = s2Samples(2.0);
                        c.label = tag;
                        return s2_->startJob(c, organSchedule(2.0));
                    });
                    addS2WaitJob(tag, 60.0);
                    add(100, tag + " finish", [this] { return s2FinishJob(); });
                }
            }
            add(100, "F teardown", [this] {
                s2_->teardownIsolated("F-done");
                return true;
            });
        }
        else if (autoPlanId_ == "S2G") // idle noise floor + tail measurement (VB3 + optional GA)
        {
            addWaitForTrackStep("Organ");
            add(1000, "G capture VB3 state", [this] { return s2CaptureLive("G"); });
            add(100, "G create isolated (512, nrtOn)", [this, vb3Cfg] {
                return s2CreateFromSelected(vb3Cfg(512, true), "G", false);
            });
            add(100, "G idle floor 5s (no MIDI)", [this] {
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(5.0);
                c.levelsFromSample = 0;
                c.label = "G-idle-vb3";
                return s2_->startJob(c, {});
            });
            addS2WaitJob("G idle floor", 60.0);
            add(100, "G idle floor summarize", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                const auto s = spike02::summarizeLevels(s2LastResult_.blockLevels);
                spike02::log("idleFloor[vb3]: maxPeakDb=" + juce::String(s.maxPeakDb, 1)
                             + " meanRmsDb=" + juce::String(s.meanRmsDb, 1) + " maxRmsDb="
                             + juce::String(s.maxRmsDb, 1) + " blocks="
                             + juce::String((juce::int64)s.blocks));
                return true;
            });
            add(100, "G phrase + 60s tail", [this] {
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(63.0); // 3 s phrase + 60 s tail window
                c.levelsFromSample = s2Samples(3.0); // final event (all offs + CC64 0) at 3.0 s
                c.label = "G-tail-vb3";
                return s2_->startJob(c, spike02::makeTailPhraseSchedule(kS2SampleRate, 3.0));
            });
            addS2WaitJob("G tail render", 300.0);
            add(100, "G tail evaluate", [this] {
                if (!s2FinishJob())
                {
                    return false;
                }
                s2EvaluateTailAndLog(s2LastResult_, "vb3");
                s2_->teardownIsolated("G-vb3-done");
                return true;
            });
            add(200, "G optional: select Groove Agent track", [this] {
                refreshTrackList();
                s2GaAvailable_ = selectTrackContaining("Groove");
                spike02::log(juce::String("ga[G]: available=")
                             + (s2GaAvailable_ ? "true" : "false — skipping second plugin"));
                return true;
            });
            add(200, "G GA capture+create", [this, vb3Cfg] {
                if (!s2GaAvailable_)
                {
                    return true;
                }
                return s2CaptureLive("GA") && s2CreateFromSelected(vb3Cfg(512, true), "GA", false);
            });
            add(100, "G GA idle floor 5s", [this] {
                if (!s2GaAvailable_)
                {
                    return true;
                }
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(5.0);
                c.levelsFromSample = 0;
                c.label = "G-idle-ga";
                return s2_->startJob(c, {});
            });
            addS2WaitJob("G GA idle floor", 60.0);
            add(100, "G GA idle summarize", [this] {
                if (!s2GaAvailable_)
                {
                    return true;
                }
                if (!s2FinishJob())
                {
                    return false;
                }
                const auto s = spike02::summarizeLevels(s2LastResult_.blockLevels);
                spike02::log("idleFloor[ga]: maxPeakDb=" + juce::String(s.maxPeakDb, 1)
                             + " meanRmsDb=" + juce::String(s.meanRmsDb, 1) + " blocks="
                             + juce::String((juce::int64)s.blocks));
                return true;
            });
            add(100, "G GA hits + 60s tail", [this] {
                if (!s2GaAvailable_)
                {
                    return true;
                }
                spike02::RenderConfig c;
                c.sampleRate = kS2SampleRate;
                c.blockSize = 512;
                c.nonRealtime = true;
                c.totalSamples = s2Samples(61.0); // 1 s hits + 60 s tail window
                c.levelsFromSample = s2Samples(1.0);
                c.label = "G-tail-ga";
                return s2_->startJob(c, spike02::makeDrumHitSchedule(kS2SampleRate, 1.0));
            });
            addS2WaitJob("G GA tail render", 300.0);
            add(100, "G GA tail evaluate + teardown", [this] {
                if (!s2GaAvailable_)
                {
                    return true;
                }
                if (!s2FinishJob())
                {
                    return false;
                }
                s2EvaluateTailAndLog(s2LastResult_, "ga");
                s2_->teardownIsolated("G-ga-done");
                return true;
            });
        }
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
        // SPIKE-02: never leave a render worker running unattended — cancel it; the
        // controller destructor joins the worker and tears the instance down on the
        // message thread (abort/shutdown lifecycle evidence in the spike02 log).
        if (s2_ != nullptr)
        {
            s2_->requestCancel();
            spike02::log("panel abort: '" + reason + "' — cancel requested on in-flight job");
        }
        s2Jitter_.stopTimer();
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

    // SPIKE-02 (S2* plans only; see buildSpike02Plan)
    std::unique_ptr<spike02::Controller> s2_;
    spike02::RenderResult s2LastResult_;
    const void* s2LivePtr_ = nullptr;
    bool s2GaAvailable_ = false;
    S2JitterProbe s2Jitter_;
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
