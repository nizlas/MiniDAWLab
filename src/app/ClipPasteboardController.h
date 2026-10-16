#pragma once

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "domain/AudioClip.h"
#include "domain/SessionHistory.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "ui/ArrangementClipboard.h"

class Session;
class Transport;
class TrackLanesView;

/// Message-thread clipboard for arrangement Delete / Cut / Copy / Paste.
/// One selected event and several selected events share this path. Copy stores event
/// values (shared audio material, copied MIDI clips, relative time and track offsets),
/// not pointers into the live session. Delete, Cut and Paste are each one undo step.
/// Copy does not change the project.
class ClipPasteboardController
{
public:
    struct Callbacks
    {
        std::function<bool()> isRecording;
        std::function<bool()> isCountInActive;
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableArrangementEdit;
        std::function<InstrumentTrackController*(TrackId)> getInstrumentControllerForTrack;
        std::function<void()> syncViewportFromSession;
        std::function<void()> refreshInstrumentArrangementUi;
        std::function<void()> repaintRuler;
        std::function<void()> refreshInspector;
        std::function<std::int64_t(std::int64_t timelineSample)> snapArrangementTimelineSample;
        std::function<void(ArrangementSelectionUndoSides)> noteArrangementSelectionUndo;
    };

    ClipPasteboardController(Session& session,
                             Transport& transport,
                             TrackLanesView& trackLanesView,
                             Callbacks callbacks);

    void invokeDeleteSelectedPlacedClipFromWindowShortcut();
    void invokeCopySelectedClipFromWindowShortcut();
    void invokeCutSelectedClipsFromWindowShortcut();
    void invokePasteClipFromWindowShortcut();

private:
    struct ClipboardItem
    {
        ArrangementClipboardOffset offset;
        std::shared_ptr<const AudioClip> material;
        std::int64_t leftTrimSamples = 0;
        std::int64_t visibleLengthSamples = 0;
        std::int64_t materialWindowStartSamples = 0;
        std::int64_t materialWindowEndExclusiveSamples = 0;
        float eventGainDb = 0.0f;
        std::int64_t fadeInSamples = 0;
        std::int64_t fadeOutSamples = 0;
        juce::String displayName;
        InstrumentMidiClip midiClip;
    };

    struct ClipboardPayload
    {
        std::vector<ClipboardItem> items;
        /// Set when every item is MIDI on one source track, so a single-track paste can
        /// still use the previous target rule if that track still owns MIDI clips.
        TrackId midiSameTrackSourceId = kInvalidTrackId;
    };

    [[nodiscard]] std::optional<ClipboardPayload> captureSelection();
    [[nodiscard]] bool selectionIsMidiOnOneTrack(const ClipboardPayload& payload) const noexcept;
    [[nodiscard]] TrackId resolveInstrumentMidiPasteTargetTrack(TrackId sourceTrackFromPasteboard) const noexcept;
    void deleteCurrentSelection(const juce::String& labelOverride = {});
    void refreshAfterEdit() const;

    Session& session_;
    Transport& transport_;
    TrackLanesView& trackLanesView_;
    Callbacks callbacks_;
    std::optional<ClipboardPayload> clipboard_;
};
