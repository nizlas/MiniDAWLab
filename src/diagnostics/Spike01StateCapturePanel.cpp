// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE. See Spike01StateCapturePanel.h.
// ============================================================================

#include "diagnostics/Spike01StateCapturePanel.h"

#include "diagnostics/Spike01MidiDeliveryCounters.h"
#include "diagnostics/Spike01ReportFormat.h"
#include "diagnostics/Spike01Sha256.h"
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
        if (waitTrackNeedle_.isNotEmpty())
        {
            startTimer(500); // the step armed waiting mode
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
