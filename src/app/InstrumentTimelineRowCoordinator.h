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
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableInstrumentEdit;
        std::function<void(TrackId)> clearAudioAndOtherInstrumentSelectionsForMidiTrack;
        std::function<void()> clearAllArrangementEventSelections;

        std::function<std::int64_t(std::int64_t timelineSample)> snapArrangementTimelineSample;
    };

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
