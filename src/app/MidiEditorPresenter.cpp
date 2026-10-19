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
#include "ui/experimental/MidiEditorCohort.h"
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
    w.setWindowTitleProvider([this]() -> juce::String {
        const TrackId tid = midiEditorOpenedForInstrumentTrackId_.value_or(kInvalidTrackId);
        return midi_editor_text::buildWindowTitle(
            trackNameForMidiEditorTitle(session_, tid), static_cast<int>(openCohort_.size()));
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
    openCohort_.clear();
    windowHost_ = nullptr;
    midiEditorOpenedForInstrumentTrackId_.reset();
    if (reasonForUser.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "MIDI editor", reasonForUser);
    }
}

ExperimentalInstrumentHost* MidiEditorPresenter::auditionHostForTrack(const TrackId trackId) const
{
    if (callbacks_.getInstrumentHostForTrack == nullptr || trackId == kInvalidTrackId)
    {
        return nullptr;
    }
    auto notSuppressed = [this](ExperimentalInstrumentHost* host) -> ExperimentalInstrumentHost* {
        if (host != nullptr && host == suppressAuditionHost_)
        {
            return nullptr;
        }
        return host;
    };
    if (ExperimentalInstrumentHost* own = callbacks_.getInstrumentHostForTrack(trackId))
    {
        return notSuppressed(own);
    }
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        const int ix = snap->findTrackIndexById(trackId);
        if (ix >= 0 && snap->getTrack(ix).getKind() == TrackKind::Midi)
        {
            const TrackId destId = snap->getTrack(ix).getMidiDestinationTrackId();
            if (destId != kInvalidTrackId)
            {
                return notSuppressed(callbacks_.getInstrumentHostForTrack(destId));
            }
        }
    }
    return nullptr;
}

void MidiEditorPresenter::publishOpenCohortToWindow(ExperimentalInstrumentHost* suppressHost)
{
    if (midiEditorWindow_.get() == nullptr || callbacks_.getInstrumentControllerForTrack == nullptr)
    {
        return;
    }
    std::vector<ExperimentalMidiEditorWindow::CohortMemberBind> binds;
    std::vector<OpenCohortMember> live;
    for (const auto& member : openCohort_)
    {
        InstrumentTrackController* ctl = callbacks_.getInstrumentControllerForTrack(member.trackId);
        if (ctl == nullptr)
        {
            continue;
        }
        InstrumentMidiClip* clip = ctl->getClipById(member.clipId);
        if (clip == nullptr)
        {
            continue;
        }
        ExperimentalMidiEditorWindow::CohortMemberBind bind;
        bind.trackId = member.trackId;
        bind.clip = clip;
        bind.controller = ctl;
        ExperimentalInstrumentHost* host = auditionHostForTrack(member.trackId);
        if (suppressHost != nullptr && host == suppressHost)
        {
            host = nullptr;
        }
        bind.auditionHost = host;
        bind.trackOrder = member.trackOrder;
        binds.push_back(bind);
        OpenCohortMember kept = member;
        kept.startSamples = clip->startSamples;
        live.push_back(kept);
    }
    openCohort_ = std::move(live);
    midiEditorWindow_->setActiveCohortMemberHandler([this](const TrackId tid, InstrumentMidiClipId) {
        midiEditorOpenedForInstrumentTrackId_ = tid;
    });
    midiEditorWindow_->setOpenCohort(std::move(binds));
    if (midiEditorOpenedForInstrumentTrackId_.has_value())
    {
        midiEditorWindow_->setName(midi_editor_text::buildWindowTitle(
            trackNameForMidiEditorTitle(session_, *midiEditorOpenedForInstrumentTrackId_),
            static_cast<int>(openCohort_.size())));
    }
}

bool MidiEditorPresenter::reconcileOpenCohort(const juce::String& detachReasonIfEmpty)
{
    if (midiEditorWindow_.get() == nullptr || !midiEditorOpenedForInstrumentTrackId_.has_value()
        || callbacks_.getInstrumentControllerForTrack == nullptr)
    {
        return false;
    }
    if (openCohort_.empty())
    {
        if (const std::optional<std::uint64_t> id = midiEditorWindow_->getBoundInstrumentClipId())
        {
            if (*id != 0)
            {
                OpenCohortMember seeded;
                seeded.trackId = *midiEditorOpenedForInstrumentTrackId_;
                seeded.clipId = static_cast<InstrumentMidiClipId>(*id);
                openCohort_.push_back(seeded);
            }
        }
    }

    std::vector<OpenCohortMember> live;
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    for (const auto& member : openCohort_)
    {
        InstrumentTrackController* ctl = callbacks_.getInstrumentControllerForTrack(member.trackId);
        if (ctl == nullptr)
        {
            continue;
        }
        InstrumentMidiClip* clip = ctl->getClipById(member.clipId);
        if (clip == nullptr)
        {
            continue;
        }
        OpenCohortMember kept = member;
        kept.startSamples = clip->startSamples;
        if (snap != nullptr)
        {
            const int ix = snap->findTrackIndexById(member.trackId);
            if (ix >= 0)
            {
                kept.trackOrder = ix;
            }
        }
        live.push_back(kept);
    }
    if (live.empty())
    {
        detachToScratchAfterMissingInstrumentClip(detachReasonIfEmpty);
        return false;
    }
    openCohort_ = std::move(live);

    const std::optional<std::uint64_t> bound = midiEditorWindow_->getBoundInstrumentClipId();
    int activeIndex = -1;
    for (int i = 0; i < static_cast<int>(openCohort_.size()); ++i)
    {
        if (openCohort_[(size_t)i].trackId == *midiEditorOpenedForInstrumentTrackId_ && bound.has_value()
            && openCohort_[(size_t)i].clipId == static_cast<InstrumentMidiClipId>(*bound))
        {
            activeIndex = i;
            break;
        }
    }
    if (activeIndex < 0)
    {
        std::vector<midi_editor_cohort::EventKey> keys;
        keys.reserve(openCohort_.size());
        for (const auto& member : openCohort_)
        {
            keys.push_back(midi_editor_cohort::EventKey{
                member.trackId, member.clipId, member.trackOrder, member.startSamples, false });
        }
        activeIndex = midi_editor_cohort::chooseInitialActiveIndex(keys);
    }
    if (activeIndex < 0)
    {
        detachToScratchAfterMissingInstrumentClip(detachReasonIfEmpty);
        return false;
    }

    const OpenCohortMember& active = openCohort_[(size_t)activeIndex];
    InstrumentTrackController* ctl = callbacks_.getInstrumentControllerForTrack(active.trackId);
    InstrumentMidiClip* clip = ctl != nullptr ? ctl->getClipById(active.clipId) : nullptr;
    if (ctl == nullptr || clip == nullptr)
    {
        detachToScratchAfterMissingInstrumentClip(detachReasonIfEmpty);
        return false;
    }
    midiEditorOpenedForInstrumentTrackId_ = active.trackId;
    alignInstrumentClipSelectionForMidiEditor(*ctl, active.clipId);
    wireMidiEditorForOpenClip(active.trackId, clip);
    publishOpenCohortToWindow(nullptr);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] rebindMidiAfterInstrumentUndo ok patternPtr="
            + juce::String::formatted("%p", (void*)&clip->pattern) + " timelineNotes="
            + juce::String((int)clip->pattern.timelineNotes.size()) + " cohort="
            + juce::String(static_cast<int>(openCohort_.size())));
    }
    return true;
}

void MidiEditorPresenter::rebindAfterInstrumentMusicalUndo()
{
    if (midiEditorWindow_.get() == nullptr)
    {
        return;
    }
    const std::optional<std::uint64_t> idOpt = midiEditorWindow_->getBoundInstrumentClipId();
    if (!idOpt.has_value() || *idOpt == 0)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] rebindMidiAfterInstrumentUndo skip: no stored clip id");
        }
        return;
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] rebindMidiAfterInstrumentUndo clipId="
                                   + juce::String(static_cast<juce::int64>(*idOpt)));
    }
    (void)reconcileOpenCohort(
        "The MIDI clip being edited is no longer available after undo.\n\n"
        "The editor was switched to scratch mode.");
}

void MidiEditorPresenter::detachOpenEditorIfBoundClipMissing() noexcept
{
    if (midiEditorWindow_.get() == nullptr || !midiEditorOpenedForInstrumentTrackId_.has_value())
    {
        return;
    }
    const std::optional<std::uint64_t> boundId = midiEditorWindow_->getBoundInstrumentClipId();
    if ((!boundId.has_value() || *boundId == 0) && openCohort_.empty())
    {
        return;
    }
    // Prune every member, not only the active clip. A deleted neighbour must not stay as a
    // dangling owner. When the active clip remains, wire's same-pointer path leaves the view put.
    (void)reconcileOpenCohort({});
}

void MidiEditorPresenter::openMidiEditorForInstrumentClip(const TrackId timelineInstrumentTrackId,
                                                          const InstrumentMidiClipId clipId)
{
    openMidiEditorForInstrumentClips(
        timelineInstrumentTrackId, clipId, { { timelineInstrumentTrackId, clipId } });
}

void MidiEditorPresenter::openMidiEditorForInstrumentClips(
    const TrackId activeTrack,
    const InstrumentMidiClipId activeClip,
    const std::vector<std::pair<TrackId, InstrumentMidiClipId>>& members)
{
    if (callbacks_.getInstrumentControllerForTrack == nullptr
        || callbacks_.getInstrumentHostForTrack == nullptr)
    {
        return;
    }

    struct Resolved
    {
        TrackId trackId = kInvalidTrackId;
        InstrumentMidiClip* clip = nullptr;
        InstrumentTrackController* controller = nullptr;
        ExperimentalInstrumentHost* host = nullptr;
        int trackOrder = 0;
    };
    std::vector<Resolved> resolved;
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    auto consider = [&](const TrackId trackId, const InstrumentMidiClipId clipId) {
        if (trackId == kInvalidTrackId || clipId == 0)
        {
            return;
        }
        for (const auto& have : resolved)
        {
            if (have.trackId == trackId && have.clip != nullptr && have.clip->id == clipId)
            {
                return;
            }
        }
        InstrumentTrackController* ctl = callbacks_.getInstrumentControllerForTrack(trackId);
        if (ctl == nullptr)
        {
            return;
        }
        InstrumentMidiClip* clip = ctl->getClipById(clipId);
        if (clip == nullptr)
        {
            return;
        }
        int order = 0;
        if (snap != nullptr)
        {
            const int ix = snap->findTrackIndexById(trackId);
            order = ix >= 0 ? ix : 0;
        }
        resolved.push_back(Resolved{ trackId, clip, ctl, auditionHostForTrack(trackId), order });
    };
    for (const auto& member : members)
    {
        consider(member.first, member.second);
    }
    consider(activeTrack, activeClip);
    if (resolved.empty())
    {
        return;
    }

    int activeIndex = -1;
    for (int i = 0; i < static_cast<int>(resolved.size()); ++i)
    {
        if (resolved[(size_t)i].trackId == activeTrack && resolved[(size_t)i].clip->id == activeClip)
        {
            activeIndex = i;
            break;
        }
    }
    if (activeIndex < 0)
    {
        std::vector<midi_editor_cohort::EventKey> keys;
        keys.reserve(resolved.size());
        for (const auto& row : resolved)
        {
            keys.push_back(midi_editor_cohort::EventKey{
                row.trackId, row.clip->id, row.trackOrder, row.clip->startSamples, false });
        }
        activeIndex = midi_editor_cohort::chooseInitialActiveIndex(keys);
    }
    if (activeIndex < 0)
    {
        return;
    }

    ExperimentalInstrumentHost* windowHost = resolved[(size_t)activeIndex].host;
    if (windowHost == nullptr)
    {
        for (const auto& row : resolved)
        {
            if (row.host != nullptr)
            {
                windowHost = row.host;
                break;
            }
        }
    }
    if (windowHost == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "MIDI editor",
            "This MIDI track has no destination instrument.\n\nChoose an instrument track under "
            "\"MIDI To\" in the Inspector first, then open the editor.");
        return;
    }

    const Resolved& active = resolved[(size_t)activeIndex];
    alignInstrumentClipSelectionForMidiEditor(*active.controller, active.clip->id);
    midiEditorOpenedForInstrumentTrackId_ = active.trackId;
    openCohort_.clear();
    openCohort_.reserve(resolved.size());
    for (const auto& row : resolved)
    {
        OpenCohortMember member;
        member.trackId = row.trackId;
        member.clipId = row.clip->id;
        member.trackOrder = row.trackOrder;
        member.startSamples = row.clip->startSamples;
        openCohort_.push_back(member);
    }

    rememberMidiEditorWindowBoundsIfWindowExists();
    midiEditorWindow_.reset();
    windowHost_ = windowHost;
    midiEditorWindow_ = std::make_unique<ExperimentalMidiEditorWindow>(*windowHost);
    wireMidiEditorForOpenClip(active.trackId, active.clip);
    publishOpenCohortToWindow(nullptr);
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
            || *midiEditorUiStateMemoTrackId_ != active.trackId)
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
    if (midiEditorWindow_.get() == nullptr)
    {
        openCohort_.clear();
        windowHost_ = nullptr;
        midiEditorOpenedForInstrumentTrackId_.reset();
        return;
    }

    ExperimentalInstrumentHost* doomedHost = nullptr;
    if (callbacks_.getInstrumentHostForTrack != nullptr)
    {
        doomedHost = callbacks_.getInstrumentHostForTrack(tid);
    }
    const bool windowHostDies = doomedHost != nullptr && windowHost_ == doomedHost;

    std::vector<OpenCohortMember> keep;
    keep.reserve(openCohort_.size());
    for (const auto& member : openCohort_)
    {
        if (member.trackId != tid)
        {
            keep.push_back(member);
        }
    }
    if (openCohort_.empty() && midiEditorOpenedForInstrumentTrackId_.has_value()
        && *midiEditorOpenedForInstrumentTrackId_ == tid)
    {
        keep.clear();
    }

    auto closeEditor = [this] {
        rememberMidiEditorWindowBoundsIfWindowExists();
        midiEditorWindow_.reset();
        openCohort_.clear();
        windowHost_ = nullptr;
        midiEditorOpenedForInstrumentTrackId_.reset();
    };

    if (keep.empty())
    {
        if (midiEditorOpenedForInstrumentTrackId_.has_value()
            && *midiEditorOpenedForInstrumentTrackId_ == tid)
        {
            closeEditor();
        }
        else if (windowHostDies)
        {
            closeEditor();
        }
        return;
    }

    openCohort_ = std::move(keep);
    suppressAuditionHost_ = doomedHost;
    if (windowHostDies)
    {
        // The window's host reference is about to be destroyed. Reopen on a surviving destination
        // so the remaining events stay editable. The viewport is reseeded with that reopen.
        bool anyLiveHost = false;
        for (const auto& member : openCohort_)
        {
            if (auditionHostForTrack(member.trackId) != nullptr)
            {
                anyLiveHost = true;
                break;
            }
        }
        if (!anyLiveHost)
        {
            suppressAuditionHost_ = nullptr;
            closeEditor();
            return;
        }
        TrackId nextTrack = openCohort_.front().trackId;
        InstrumentMidiClipId nextClip = openCohort_.front().clipId;
        if (midiEditorOpenedForInstrumentTrackId_.has_value())
        {
            for (const auto& member : openCohort_)
            {
                if (member.trackId == *midiEditorOpenedForInstrumentTrackId_)
                {
                    nextTrack = member.trackId;
                    nextClip = member.clipId;
                    break;
                }
            }
        }
        std::vector<std::pair<TrackId, InstrumentMidiClipId>> members;
        members.reserve(openCohort_.size());
        for (const auto& member : openCohort_)
        {
            members.emplace_back(member.trackId, member.clipId);
        }
        openMidiEditorForInstrumentClips(nextTrack, nextClip, members);
        suppressAuditionHost_ = nullptr;
        return;
    }

    if (midiEditorOpenedForInstrumentTrackId_.has_value() && *midiEditorOpenedForInstrumentTrackId_ == tid)
    {
        midiEditorOpenedForInstrumentTrackId_ = openCohort_.front().trackId;
    }
    publishOpenCohortToWindow(doomedHost);
    if (auto* window = midiEditorWindow_.get())
    {
        if (const std::optional<std::uint64_t> bound = window->getBoundInstrumentClipId())
        {
            bool activeStillThere = false;
            for (const auto& member : openCohort_)
            {
                if (midiEditorOpenedForInstrumentTrackId_.has_value()
                    && member.trackId == *midiEditorOpenedForInstrumentTrackId_
                    && member.clipId == static_cast<InstrumentMidiClipId>(*bound))
                {
                    activeStillThere = true;
                    break;
                }
            }
            if (!activeStillThere)
            {
                (void)reconcileOpenCohort({});
            }
        }
    }
    suppressAuditionHost_ = nullptr;
}
