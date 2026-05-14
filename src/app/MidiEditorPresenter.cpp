#include "app/MidiEditorPresenter.h"

#include <cmath>

#include <juce_audio_devices/juce_audio_devices.h>

#include "domain/Session.h"
#include "engine/RecorderService.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "transport/Transport.h"

#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

MidiEditorPresenter::MidiEditorPresenter(Transport& transport,
                                         Session& session,
                                         juce::AudioDeviceManager& deviceManager,
                                         RecorderService& recorder,
                                         TimelineViewportModel& timelineViewport,
                                         std::unique_ptr<ExperimentalMidiEditorWindow>& midiEditorWindow,
                                         Callbacks callbacks)
    : transport_(transport)
    , session_(session)
    , deviceManager_(deviceManager)
    , timelineViewport_(timelineViewport)
    , midiEditorWindow_(midiEditorWindow)
    , callbacks_(std::move(callbacks))
{
    juce::ignoreUnused(recorder);
}

[[nodiscard]] ExperimentalMidiTransportCommands MidiEditorPresenter::makeMidiEditorTransportCommands()
{
    ExperimentalMidiTransportCommands c;
    c.transport = &transport_;
    c.onTogglePlayPause = [this] { callbacks_.invokePlayPauseToggle(); };
    c.onStop = [this] { callbacks_.invokeStopOrSeek(); };
    c.onToggleRecord = [this] { callbacks_.invokeRecordToggle(); };
    c.onJumpToLeftLocator = [this] { callbacks_.invokeJumpToLeftLocator(); };
    c.onToggleCycle = [this] {
        if (callbacks_.isUiInputBlockedByRecording != nullptr && callbacks_.isUiInputBlockedByRecording())
        {
            juce::Logger::writeToLog("[Cycle] MIDI editor toggle ignored (recording or count-in)");
            return;
        }
        transport_.requestCycleEnabled(!transport_.readCycleEnabledForUi());
        juce::Logger::writeToLog(juce::String("[Cycle]")
                                 + (transport_.readCycleEnabledForUi() ? "on" : "off"));
    };
    if (callbacks_.isUiInputBlockedByRecording != nullptr)
    {
        c.isUiInputBlockedByRecording = callbacks_.isUiInputBlockedByRecording;
    }
    return c;
}

void MidiEditorPresenter::wireMidiEditorForOpenClip(const TrackId timelineInstrumentTrackId,
                                                    InstrumentMidiClip* clip)
{
    jassert(clip != nullptr);
    if (callbacks_.getInstrumentControllerForTrack == nullptr)
    {
        return;
    }
    InstrumentTrackController* ctl = callbacks_.getInstrumentControllerForTrack(timelineInstrumentTrackId);
    if (ctl == nullptr)
    {
        return;
    }
    if (midiEditorWindow_.get() == nullptr)
    {
        return;
    }
    ExperimentalMidiEditorWindow& w = *midiEditorWindow_;
    w.bindExternalPattern(&clip->pattern,
                          clip,
                          ctl,
                          &session_,
                          &transport_,
                          &deviceManager_,
                          &timelineViewport_,
                          clip->name);
    w.bindTransportCommands(makeMidiEditorTransportCommands());
    w.setInstrumentMusicalUndoUi(
        [this](const juce::String& lab, std::function<bool()> m) {
            callbacks_.executeUndoableInstrumentEdit(lab, std::move(m));
        },
        [this](const juce::String& lab, std::vector<InstrumentMusicalUndoSnapshot> before) {
            callbacks_.commitInstrumentMusicalUndoPair(lab, std::move(before));
        },
        [this] { return callbacks_.buildSortedInstrumentMusicalUndoSnapshot(); },
        [this] { callbacks_.invokeUndo(); },
        [this] { callbacks_.invokeRedo(); });
    w.syncInstrumentStateFromHost();
}

void MidiEditorPresenter::detachToScratchAfterMissingInstrumentClip(const juce::String& reasonForUser)
{
    if (midiEditorWindow_.get() == nullptr)
    {
        return;
    }
    ExperimentalMidiEditorWindow& w = *midiEditorWindow_;
    w.unbindExternalPattern();
    w.bindTransportCommands(makeMidiEditorTransportCommands());
    w.setInstrumentMusicalUndoUi(
        std::function<void(const juce::String&, std::function<bool()>)>{},
        std::function<void(const juce::String&, std::vector<InstrumentMusicalUndoSnapshot>)>{},
        std::function<std::vector<InstrumentMusicalUndoSnapshot>()>{},
        [this] { callbacks_.invokeUndo(); },
        [this] { callbacks_.invokeRedo(); });
    w.syncInstrumentStateFromHost();
    midiEditorOpenedForInstrumentTrackId_.reset();
    if (reasonForUser.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "MIDI editor", reasonForUser);
    }
}

void MidiEditorPresenter::rebindAfterInstrumentMusicalUndo()
{
    if (midiEditorWindow_.get() == nullptr)
    {
        return;
    }
    ExperimentalMidiEditorWindow& w = *midiEditorWindow_;
    const std::optional<std::uint64_t> idOpt = w.getBoundInstrumentClipId();
    if (!idOpt.has_value() || *idOpt == 0)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] rebindMidiAfterInstrumentUndo skip: no stored clip id");
        }
        return;
    }
    const InstrumentMidiClipId clipId = static_cast<InstrumentMidiClipId>(*idOpt);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] rebindMidiAfterInstrumentUndo clipId="
                                   + juce::String(static_cast<juce::int64>(clipId)));
    }
    if (!midiEditorOpenedForInstrumentTrackId_.has_value())
    {
        detachToScratchAfterMissingInstrumentClip(
            "The MIDI clip being edited is no longer available after undo.\n\n"
            "The editor was switched to scratch mode.");
        return;
    }
    if (callbacks_.getInstrumentControllerForTrack == nullptr)
    {
        return;
    }
    InstrumentTrackController* ctl
        = callbacks_.getInstrumentControllerForTrack(*midiEditorOpenedForInstrumentTrackId_);
    if (ctl == nullptr)
    {
        detachToScratchAfterMissingInstrumentClip(
            "The MIDI clip being edited is no longer available after undo.\n\n"
            "The editor was switched to scratch mode.");
        return;
    }
    InstrumentMidiClip* const clip = ctl->getClipById(clipId);
    if (clip == nullptr)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] rebindMidiAfterInstrumentUndo clip missing -> detach scratch");
        }
        detachToScratchAfterMissingInstrumentClip(
            "The MIDI clip being edited is no longer available after undo.\n\n"
            "The editor was switched to scratch mode.");
        return;
    }
    ctl->setSelectedClipId(clipId);
    wireMidiEditorForOpenClip(*midiEditorOpenedForInstrumentTrackId_, clip);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] rebindMidiAfterInstrumentUndo ok patternPtr="
            + juce::String::formatted("%p", (void*)&clip->pattern) + " timelineNotes="
            + juce::String((int)clip->pattern.timelineNotes.size()));
    }
}

void MidiEditorPresenter::openMidiEditorForInstrumentClip(const TrackId timelineInstrumentTrackId,
                                                          const InstrumentMidiClipId clipId)
{
    if (callbacks_.getInstrumentControllerForTrack == nullptr
        || callbacks_.getInstrumentHostForTrack == nullptr)
    {
        return;
    }
    InstrumentTrackController* ctl = callbacks_.getInstrumentControllerForTrack(timelineInstrumentTrackId);
    if (ctl == nullptr)
    {
        return;
    }

    InstrumentMidiClip* clip = ctl->getClipById(clipId);
    if (clip == nullptr)
    {
        return;
    }

    ExperimentalInstrumentHost* mh = callbacks_.getInstrumentHostForTrack(timelineInstrumentTrackId);
    if (mh == nullptr)
    {
        return;
    }

    ctl->setSelectedClipId(clipId);
    midiEditorOpenedForInstrumentTrackId_ = timelineInstrumentTrackId;
    midiEditorWindow_.reset();
    midiEditorWindow_ = std::make_unique<ExperimentalMidiEditorWindow>(*mh);
    wireMidiEditorForOpenClip(timelineInstrumentTrackId, clip);
    midiEditorWindow_->setVisible(true);
    midiEditorWindow_->toFront(true);
}

void MidiEditorPresenter::refreshInstrumentUiIfOpen()
{
    if (midiEditorWindow_.get() != nullptr)
    {
        midiEditorWindow_->syncInstrumentStateFromHost();
    }
}

void MidiEditorPresenter::syncInstrumentClipTimelineFromDevice()
{
    double sr = 48000.0;
    if (juce::AudioIODevice* const d = deviceManager_.getCurrentAudioDevice())
    {
        const double r = d->getCurrentSampleRate();
        if (r > 0.0 && std::isfinite(r))
        {
            sr = r;
        }
    }
    if (callbacks_.applyInstrumentClipTimelineSampleRate != nullptr)
    {
        callbacks_.applyInstrumentClipTimelineSampleRate(sr);
    }
}

void MidiEditorPresenter::notifyMidiEditorExternalTransportSeekIfOpen(std::int64_t targetSample) noexcept
{
    if (midiEditorWindow_.get() != nullptr)
    {
        midiEditorWindow_->notifyExternalTransportSeek(targetSample);
    }
}

std::optional<TrackId> MidiEditorPresenter::openedTrackId() const noexcept
{
    return midiEditorOpenedForInstrumentTrackId_;
}

ExperimentalMidiEditorWindow* MidiEditorPresenter::midiEditorWindow() const noexcept
{
    return midiEditorWindow_.get();
}

void MidiEditorPresenter::snapshotOpenClipViewportFromRollIfOpen() noexcept
{
    if (midiEditorWindow_.get() != nullptr)
    {
        midiEditorWindow_->snapshotOpenClipViewportFromRoll();
    }
}

void MidiEditorPresenter::resetWindowAndBooking() noexcept
{
    midiEditorWindow_.reset();
    midiEditorOpenedForInstrumentTrackId_.reset();
}

void MidiEditorPresenter::resetWindowAndBookingIfOpenOnTrack(const TrackId tid) noexcept
{
    if (midiEditorOpenedForInstrumentTrackId_.has_value()
        && midiEditorOpenedForInstrumentTrackId_.value() == tid)
    {
        midiEditorWindow_.reset();
        midiEditorOpenedForInstrumentTrackId_.reset();
    }
}
