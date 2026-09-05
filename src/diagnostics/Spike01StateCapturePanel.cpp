// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE. See Spike01StateCapturePanel.h.
// ============================================================================

#include "diagnostics/Spike01StateCapturePanel.h"

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
                                                private juce::AudioProcessorListener
{
public:
    explicit Content(Spike01PanelCallbacks callbacks) : callbacks_(std::move(callbacks))
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
                         + " ===");
        refreshTrackList();
        setSize(640, 470);
    }

    ~Content() override
    {
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
Spike01StateCapturePanel::Spike01StateCapturePanel(Spike01PanelCallbacks callbacks)
    : juce::DocumentWindow("SPIKE-01 state-capture probe (diagnostic)",
                           juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                               juce::ResizableWindow::backgroundColourId),
                           juce::DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    auto content = std::make_unique<Content>(std::move(callbacks));
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
