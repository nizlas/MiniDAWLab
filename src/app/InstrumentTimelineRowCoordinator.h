#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"

#include "ui/SoloUiHooks.h"
#include "ui/TrackHeaderView.h"

class Session;
class Transport;
class TrackLanesView;
class InspectorView;
class TimelineViewportModel;
class InstrumentRuntimeCoordinator;

/// Owns instrument-row `TrackHeaderView` + MIDI event lane widgets embedded in `TrackLanesView`.
class InstrumentTimelineRowCoordinator final
{
public:
    struct Callbacks
    {
        std::function<void(TrackId)> runExperimentalInstrumentPluginDescriptionRescanForTrack;
        std::function<void()> refreshMidiEditorInstrumentUiIfOpen;
        std::function<void(TrackId, InstrumentMidiClipId)> openMidiEditorForInstrumentClip;
        std::function<void(TrackId)> runInstrumentMidiFileImportForTrack;
        /// P2: opens the "Instrument alternatives" popup for the track, anchored at the clicked
        /// header-strip cell (SCREEN coordinates).
        std::function<void(TrackId, juce::Rectangle<int> screenAnchorBounds)>
            showInstrumentAlternativesForTrack;
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableInstrumentEdit;
        std::function<void(TrackId)> clearAudioAndOtherInstrumentSelectionsForMidiTrack;
        std::function<void()> clearAllArrangementEventSelections;

        std::function<std::int64_t(std::int64_t timelineSample)> snapArrangementTimelineSample;

        // ---- Live MIDI (Instrument + Midi rows); all optional — absent = pre-live-MIDI chrome ----
        std::function<bool(TrackId)> isLiveMidiMonitorEnabled;
        std::function<void(TrackId)> toggleLiveMidiMonitor;
        std::function<bool(TrackId)> isLiveMidiRecordArmed;
        std::function<void(TrackId)> toggleLiveMidiRecordArm;
        std::function<bool(TrackId)> isLiveMidiActive;
        /// Geometry of the running MIDI take on this row (inactive when the row is not recording).
        /// The composition root derives it from the live-MIDI coordinator's take context and the
        /// transport's wrap count — never from a lane-local timer.
        struct LiveTakePreview
        {
            bool active = false;
            /// Start of the CURRENT pass (record boundary, or the left locator after a wrap).
            std::int64_t currentPassStartSample = 0;
            /// Material already recorded in earlier passes of this run (shown dimmer); empty when
            /// `completedEndExclusive <= completedStart`.
            std::int64_t completedStart = 0;
            std::int64_t completedEndExclusive = 0;
        };
        std::function<LiveTakePreview(TrackId)> liveMidiTakePreviewForTrack;
        /// The main window's playhead display position for THIS frame (the overlay's stored frame
        /// value) — the take preview's right edge uses the same position and transform as the
        /// playhead line. Falls back to the transport position when absent.
        std::function<double()> playheadDisplaySamplesForUi;

        /// Solo seam (same `SoloUiHooks` semantics as `TrackLanesView::setSoloUiHooks`): display
        /// state for the S cell / locked-M chrome, toggle for S clicks. Both optional — unwired
        /// keeps the pre-solo chrome on Instrument/Midi rows.
        SoloUiHooks soloUiHooks{};
    };

    /// Install the live-MIDI header / lane seam after construction (composition root).
    void setLiveMidiCallbacks(std::function<bool(TrackId)> isMonitorEnabled,
                              std::function<void(TrackId)> toggleMonitor,
                              std::function<bool(TrackId)> isRecordArmed,
                              std::function<void(TrackId)> toggleRecordArm,
                              std::function<bool(TrackId)> isActive,
                              std::function<Callbacks::LiveTakePreview(TrackId)> takePreviewForTrack,
                              std::function<double()> playheadDisplaySamplesForUi)
    {
        callbacks_.isLiveMidiMonitorEnabled = std::move(isMonitorEnabled);
        callbacks_.toggleLiveMidiMonitor = std::move(toggleMonitor);
        callbacks_.isLiveMidiRecordArmed = std::move(isRecordArmed);
        callbacks_.toggleLiveMidiRecordArm = std::move(toggleRecordArm);
        callbacks_.isLiveMidiActive = std::move(isActive);
        callbacks_.liveMidiTakePreviewForTrack = std::move(takePreviewForTrack);
        callbacks_.playheadDisplaySamplesForUi = std::move(playheadDisplaySamplesForUi);
    }

    /// Install the solo seam after construction (composition root, next to the lanes' hooks).
    void setSoloUiHooks(SoloUiHooks hooks) noexcept { callbacks_.soloUiHooks = std::move(hooks); }

    /// [Message thread, once per playhead frame] Invalidate only the strip a running take's
    /// preview grew by since the previous frame on the recording lanes (the playhead overlay
    /// invalidates its own columns; lanes never blanket-repaint at playhead cadence).
    void repaintLiveTakePreviewGrowth(double displaySamples) noexcept;

    /// [Tests / stability] Pixel geometry of the running take preview on `tid`'s lane in LANE
    /// coordinates (empty when no take runs on it): `{x0, x1}` of the current-pass region.
    [[nodiscard]] std::optional<std::pair<int, int>> liveTakePreviewPixelSpanForDiagnostics(TrackId tid) const;

    InstrumentTimelineRowCoordinator(Session& session,
                                    Transport& transport,
                                    TrackLanesView& trackLanesView,
                                    InspectorView& inspectorView,
                                    TimelineViewportModel& timelineViewport,
                                    InstrumentRuntimeCoordinator& instrumentRuntime,
                                    Callbacks callbacks);
    ~InstrumentTimelineRowCoordinator();

    void tearDownExperimentalInstrumentTimelineUiForTrack(TrackId tid) noexcept;
    void syncInstrumentTimelineRowAttachmentToSession() noexcept;
    void ensureInstrumentTimelineHeaderAndLaneForTrack(TrackId tid);
    void repaintInstrumentTrackRow();

    /// Clears header + MIDI lane widgets only (caller typically syncs attachments empty first).
    void clearInstrumentTimelineLanesAndHeaders() noexcept;

    /// Periodic timer hook (hosted on `TransportControlsContent`): repaint instrument headers when structural-edit lock toggles.
    void tickStructuralEditBlockedHeaderStripRepaint(bool structuralTimelineEditBlockedUi) noexcept;

    void refreshMidiEditorInstrumentUiIfOpen();
    void openMidiEditorForInstrumentClip(TrackId timelineInstrumentTrackId, InstrumentMidiClipId clipId);

    /// Screen point in global pixels: instrument MIDI event lane row under the point, if any.
    [[nodiscard]] std::optional<TrackId> instrumentMidiLaneHitAtScreen(juce::Point<float> screenPt) const noexcept;

    /// Cross-lane MIDI move: transient drop ghost (session start + length per clip), matching `ClipWaveformView` drag ghost.
    void clearInstrumentMidiCrossTrackDropGhosts() noexcept;
    void syncInstrumentMidiCrossTrackDropGhostPreview(
        TrackId dragSourceTrackId,
        std::optional<TrackId> hoverDestTrackId,
        std::vector<std::pair<std::int64_t, std::int64_t>> sessionStartLenSamples) noexcept;

    /// After `TrackLanesEditCoordinator::install()` wires rename; patches headers built earlier at startup.
    void rewireInstrumentTrackRenameHandlers() noexcept;

private:
    struct MidiEventLane;
    friend struct MidiEventLane;

    Session& session_;
    Transport& transport_;
    TrackLanesView& trackLanes_;
    InspectorView& inspector_;
    TimelineViewportModel& timelineViewport_;
    InstrumentRuntimeCoordinator& instrumentRuntime_;
    Callbacks callbacks_;

    std::unordered_map<TrackId, std::unique_ptr<TrackHeaderView>> instrumentTrackHeadersByTrackId_;
    std::unordered_map<TrackId, std::unique_ptr<MidiEventLane>> instrumentMidiEventLanesByTrackId_;
    bool lastStructuralTimelineBlockedForHeaderStripUi_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentTimelineRowCoordinator)
};
