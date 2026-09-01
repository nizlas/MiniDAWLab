#include "app/MidiEditorPresenter.h"

#include <cmath>

#include <juce_audio_devices/juce_audio_devices.h>

#include "app/ProjectMainWindowBounds.h"
#include "domain/Session.h"
#include "engine/RecorderService.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "transport/Transport.h"

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "ui/experimental/MidiEditorTitleStatus.h"

namespace
{
/// Phase B.1 window title: the bound track's *current* name, TrackId-resolved (rename-safe).
[[nodiscard]] juce::String trackNameForMidiEditorTitle(Session& session, const TrackId tid) noexcept
{
    if (const auto snap = session.loadSessionSnapshotForAudioThread())
    {
        const int ix = snap->findTrackIndexById(tid);
        if (ix >= 0)
        {
            return snap->getTrack(ix).getName();
        }
    }
    return {};
}

void alignInstrumentClipSelectionForMidiEditor(InstrumentTrackController& ctl,
                                               InstrumentMidiClipId clipId) noexcept
{
    if (clipId == 0)
    {
        return;
    }
    if (ctl.isClipSelected(clipId))
    {
        ctl.setActiveSelectedClipId(clipId);
    }
    else
    {
        ctl.setSelectedClipIdsExclusive(clipId);
    }
}
} // namespace

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
    c.onSaveProject = [this] {
        if (callbacks_.invokeSaveProject)
        {
            callbacks_.invokeSaveProject();
        }
    };
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
                          trackNameForMidiEditorTitle(session_, timelineInstrumentTrackId));
    // Keep the title current across track renames while the editor stays open.
    w.setWindowTitleProvider([this, timelineInstrumentTrackId]() -> juce::String {
        return midi_editor_text::buildWindowTitle(
            trackNameForMidiEditorTitle(session_, timelineInstrumentTrackId));
    });
    w.bindTransportCommands(makeMidiEditorTransportCommands());
    w.setInstrumentMusicalUndoUi(
        [this](const juce::String& lab, std::function<bool()> m) {
            callbacks_.executeUndoableInstrumentEdit(lab, std::move(m));
        },
        [this] { callbacks_.invokeUndo(); },
        [this] { callbacks_.invokeRedo(); });
    w.setArrangementSnapToolbarSyncHandler([this] {
        if (callbacks_.syncArrangementSnapToolbarFromSession != nullptr)
        {
            callbacks_.syncArrangementSnapToolbarFromSession();
        }
    });
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
    w.setArrangementSnapToolbarSyncHandler({});
    w.bindTransportCommands(makeMidiEditorTransportCommands());
    w.setInstrumentMusicalUndoUi(
        std::function<void(const juce::String&, std::function<bool()>)>{},
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
    alignInstrumentClipSelectionForMidiEditor(*ctl, clipId);
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
        // Phase B: a TrackKind::Midi row has no host of its own — the editor borrows the routed
        // destination's host (audition and drum names then match what playback will sound like).
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int ix = snap->findTrackIndexById(timelineInstrumentTrackId);
            if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Midi)
            {
                const TrackId destId = snap->getTrack(ix).getMidiDestinationTrackId();
                if (destId != kInvalidTrackId)
                {
                    mh = callbacks_.getInstrumentHostForTrack(destId);
                }
            }
        }
    }
    if (mh == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "MIDI editor",
            "This MIDI track has no destination instrument.\n\nChoose an instrument track under "
            "\"MIDI To\" in the Inspector first, then open the editor.");
        return;
    }

    alignInstrumentClipSelectionForMidiEditor(*ctl, clipId);
    midiEditorOpenedForInstrumentTrackId_ = timelineInstrumentTrackId;
    rememberMidiEditorWindowBoundsIfWindowExists();
    midiEditorWindow_.reset();
    midiEditorWindow_ = std::make_unique<ExperimentalMidiEditorWindow>(*mh);
    wireMidiEditorForOpenClip(timelineInstrumentTrackId, clip);
    midiEditorWindow_->refreshArrangementSnapMirrorFromSession();
    if (midiEditorWindowBoundsMemo_.has_value())
    {
        (void)applyProjectWindowBoundsClamped(*midiEditorWindow_, *midiEditorWindowBoundsMemo_);
    }
    {
        // Velocity lane height is track-agnostic; pitch scroll and rows mode belong to the track
        // they were captured on (drum vs melodic ranges differ) and must not leak across tracks.
        MidiEditorWorkspaceUiState toApply = midiEditorUiStateMemo_;
        if (!midiEditorUiStateMemoTrackId_.has_value()
            || *midiEditorUiStateMemoTrackId_ != timelineInstrumentTrackId)
        {
            toApply.topVisibleMidiPitch = -1;
            toApply.rowLabelMode = 0;
        }
        midiEditorWindow_->applyWorkspaceUiState(toApply);
    }
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

void MidiEditorPresenter::syncTimelineRulerFormatUiIfEditorOpen()
{
    if (midiEditorWindow_.get() != nullptr)
    {
        midiEditorWindow_->syncTimelineRulerFormatFromSession();
    }
}

void MidiEditorPresenter::refreshArrangementSnapMirrorFromSession() noexcept
{
    if (midiEditorWindow_.get() != nullptr)
    {
        midiEditorWindow_->refreshArrangementSnapMirrorFromSession();
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
    if (midiEditorWindow_ != nullptr)
    {
        rememberMidiEditorWindowBoundsIfWindowExists();
        midiEditorWindow_->prepareInstrumentUnloadFromHost();
    }
    midiEditorWindow_.reset();
    midiEditorOpenedForInstrumentTrackId_.reset();
}

void MidiEditorPresenter::rememberMidiEditorWindowBoundsIfWindowExists() noexcept
{
    if (midiEditorWindow_ == nullptr)
    {
        return;
    }
    const std::optional<ProjectFileMainWindowBoundsV1> b
        = captureProjectWindowBoundsForProjectSave(*midiEditorWindow_);
    if (b.has_value())
    {
        midiEditorWindowBoundsMemo_ = b;
    }
    else
    {
        // A live window whose bounds fail the capture guard must never be silent: this is exactly
        // how "saved but placement forgotten" reports start.
        const juce::Rectangle<int> r = midiEditorWindow_->getScreenBounds();
        appendProjectSaveDiagnosticLine("midi editor bounds capture rejected: x="
                                        + juce::String(r.getX()) + " y=" + juce::String(r.getY())
                                        + " w=" + juce::String(r.getWidth())
                                        + " h=" + juce::String(r.getHeight())
                                        + " (memo left unchanged)");
    }
    midiEditorUiStateMemo_ = midiEditorWindow_->captureWorkspaceUiState();
    midiEditorUiStateMemoTrackId_ = midiEditorOpenedForInstrumentTrackId_;
}

void MidiEditorPresenter::setMidiEditorWindowBoundsFromLoadedProject(
    const ProjectFileV1& projectFile) noexcept
{
    if (projectFile.hasMidiEditorWindowBounds)
    {
        midiEditorWindowBoundsMemo_ = projectFile.midiEditorWindowBounds;
    }
    else
    {
        midiEditorWindowBoundsMemo_.reset();
    }
    if (projectFile.hasMidiEditorWorkspace)
    {
        const ProjectFileMidiEditorWorkspaceV1& ws = projectFile.midiEditorWorkspace;
        midiEditorUiStateMemo_.topVisibleMidiPitch = ws.topVisibleMidiPitch;
        midiEditorUiStateMemo_.velocityLaneHeight = ws.velocityLaneHeight;
        midiEditorUiStateMemo_.rowLabelMode = ws.rowLabelMode;
        midiEditorUiStateMemoTrackId_ = ws.instrumentTrackId != 0
                                            ? std::optional<TrackId>(static_cast<TrackId>(ws.instrumentTrackId))
                                            : std::nullopt;
    }
    else
    {
        midiEditorUiStateMemo_ = {};
        midiEditorUiStateMemoTrackId_.reset();
    }
    if (midiEditorWindow_ != nullptr && midiEditorWindowBoundsMemo_.has_value())
    {
        (void)applyProjectWindowBoundsClamped(*midiEditorWindow_, *midiEditorWindowBoundsMemo_);
    }
}

std::optional<ProjectFileMainWindowBoundsV1>
MidiEditorPresenter::getMidiEditorWindowBoundsForProjectSave() noexcept
{
    rememberMidiEditorWindowBoundsIfWindowExists();
    return midiEditorWindowBoundsMemo_;
}

std::optional<ProjectFileMidiEditorWorkspaceV1>
MidiEditorPresenter::getMidiEditorWorkspaceForProjectSave() noexcept
{
    rememberMidiEditorWindowBoundsIfWindowExists();
    if (midiEditorWindow_ == nullptr || !midiEditorWindow_->isVisible())
    {
        // Closed editor: omit the workspace so load never auto-opens it. Bounds and the in-session
        // view memo still persist separately for the next manual open.
        return std::nullopt;
    }
    ProjectFileMidiEditorWorkspaceV1 ws;
    ws.open = true;
    ws.instrumentTrackId = midiEditorOpenedForInstrumentTrackId_.has_value()
                               ? static_cast<juce::int64>(*midiEditorOpenedForInstrumentTrackId_)
                               : 0;
    const std::optional<std::uint64_t> clipId = midiEditorWindow_->getBoundInstrumentClipId();
    ws.clipId = clipId.has_value() ? static_cast<juce::int64>(*clipId) : 0;
    ws.topVisibleMidiPitch = midiEditorUiStateMemo_.topVisibleMidiPitch;
    ws.velocityLaneHeight = midiEditorUiStateMemo_.velocityLaneHeight;
    ws.rowLabelMode = midiEditorUiStateMemo_.rowLabelMode;
    return ws;
}

void MidiEditorPresenter::tryRestoreMidiEditorWorkspaceAfterProjectLoad(
    const ProjectFileV1& projectFile) noexcept
{
    if (!projectFile.hasMidiEditorWorkspace || !projectFile.midiEditorWorkspace.open)
    {
        return;
    }
    const ProjectFileMidiEditorWorkspaceV1& ws = projectFile.midiEditorWorkspace;
    const auto tid = static_cast<TrackId>(ws.instrumentTrackId);
    const auto clipId = static_cast<InstrumentMidiClipId>(ws.clipId);
    appendProjectLoadDiagnosticLine("load: MIDI editor restore requested track="
                                    + juce::String(ws.instrumentTrackId)
                                    + " clip=" + juce::String(ws.clipId));
    if (tid == 0 || clipId == 0)
    {
        appendProjectLoadDiagnosticLine("load: MIDI editor restore skipped (no track/clip identity)");
        return;
    }
    if (callbacks_.getInstrumentControllerForTrack == nullptr
        || callbacks_.getInstrumentHostForTrack == nullptr)
    {
        appendProjectLoadDiagnosticLine("load: MIDI editor restore skipped (no runtime lookups)");
        return;
    }
    InstrumentTrackController* const ctl = callbacks_.getInstrumentControllerForTrack(tid);
    if (ctl == nullptr)
    {
        appendProjectLoadDiagnosticLine("load: MIDI editor restore skipped (instrument track "
                                        + juce::String(ws.instrumentTrackId) + " missing)");
        return;
    }
    if (ctl->getClipById(clipId) == nullptr)
    {
        appendProjectLoadDiagnosticLine("load: MIDI editor restore skipped (clip "
                                        + juce::String(ws.clipId) + " missing on track "
                                        + juce::String(ws.instrumentTrackId) + ")");
        return;
    }
    openMidiEditorForInstrumentClip(tid, clipId);
    if (midiEditorWindow_ != nullptr && midiEditorWindow_->isVisible())
    {
        appendProjectLoadDiagnosticLine(
            "load: MIDI editor restore applied bounds="
            + (midiEditorWindowBoundsMemo_.has_value()
                   ? juce::String(midiEditorWindowBoundsMemo_->x) + ","
                         + juce::String(midiEditorWindowBoundsMemo_->y) + " "
                         + juce::String(midiEditorWindowBoundsMemo_->width) + "x"
                         + juce::String(midiEditorWindowBoundsMemo_->height)
                   : juce::String("(default)"))
            + " topPitch=" + juce::String(ws.topVisibleMidiPitch)
            + " velLane=" + juce::String(ws.velocityLaneHeight)
            + " rowsMode=" + juce::String(ws.rowLabelMode));
    }
    else
    {
        appendProjectLoadDiagnosticLine("load: MIDI editor restore did not open a window");
    }
}

void MidiEditorPresenter::resetWindowAndBookingIfOpenOnTrack(const TrackId tid) noexcept
{
    bool mustClose = midiEditorOpenedForInstrumentTrackId_.has_value()
                     && midiEditorOpenedForInstrumentTrackId_.value() == tid;
    // Phase B: an editor open on a TrackKind::Midi row borrows the *destination* instrument's
    // host, so deleting that instrument track must also close this editor (dangling host ref).
    if (!mustClose && midiEditorOpenedForInstrumentTrackId_.has_value())
    {
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int ix = snap->findTrackIndexById(midiEditorOpenedForInstrumentTrackId_.value());
            if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Midi
                && snap->getTrack(ix).getMidiDestinationTrackId() == tid)
            {
                mustClose = true;
            }
        }
    }
    if (mustClose)
    {
        rememberMidiEditorWindowBoundsIfWindowExists();
        midiEditorWindow_.reset();
        midiEditorOpenedForInstrumentTrackId_.reset();
    }
}
