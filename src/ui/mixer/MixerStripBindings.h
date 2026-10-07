#pragma once

// =============================================================================
// MixerStripBindings — everything a mixer channel strip may read or do, keyed by explicit TrackId
// =============================================================================
//
// ROLE
//   A strip never reaches into Session, the engine, coordinators or the active-track state. The
//   composition root fills this struct once with the SAME production entry points the Inspector
//   and the track headers use (undoable edits from `TrackLanesEditCoordinator::trackEditActions`,
//   the direct fader / pan setters of the channel panel and pan field, the header cells'
//   mute / power / monitor / arm paths, the plugin-insert host seam), and every strip receives a
//   reference to it. All actions take the strip's own TrackId, so editing channel B never
//   requires making B the active track — and the Inspector, which polls the same snapshot, shows
//   the change on its next tick.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "app/TrackEditActions.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "ui/InspectorView.h" // InspectorPluginHost, InspectorAudioInputDeviceSnapshot, InspectorMidiInputSnapshot
#include "ui/SoloUiHooks.h"

#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <memory>

struct MixerStripBindings
{
    // --- read model -----------------------------------------------------------------------------
    /// The published session snapshot (strips refresh from it; never cached across ticks).
    std::function<std::shared_ptr<const SessionSnapshot>()> loadSnapshot;
    /// `Session::getActiveTrackId` — paints the active strip like the active header.
    std::function<TrackId()> activeTrackId;
    /// Device input channels for the Audio Input selector (composition root reads the device).
    std::function<InspectorAudioInputDeviceSnapshot()> audioInputDeviceSnapshot;
    /// MIDI devices + status for a row's MIDI Input selector.
    std::function<InspectorMidiInputSnapshot(TrackId)> midiInputSnapshot;
    /// One line naming the current audio device output (Stereo Out strip, information only).
    std::function<juce::String()> deviceOutputDescription;

    // --- selection ------------------------------------------------------------------------------
    /// Activate the row exactly like a header click (session active track, instrument controller
    /// activation, Inspector refresh, arrangement repaint).
    std::function<void(TrackId)> activateTrack;

    // --- undoable channel edits (identical functions to the Inspector's) ---------------------------
    TrackEditActions edits;

    // --- direct channel writes (same policy as the Inspector: no undo step) -----------------------
    std::function<void(TrackId, float linearGain)> setChannelFaderGain;
    std::function<void(TrackId, float pan)> setStereoPan;

    // --- base buttons (header semantics per kind) -------------------------------------------------
    std::function<void(TrackId)> toggleMute;
    /// Solo seam shared with the arrangement headers (`SoloUiHooks`): S cell on every strip except
    /// Stereo Out; display state also drives the locked-M rendering. Unwired ⇒ no S cell.
    SoloUiHooks solo;
    /// Returns false when refused (structural edit blocked while playing / recording / count-in).
    std::function<bool(TrackId)> togglePower;
    std::function<bool()> isPowerInteractable;
    std::function<bool(TrackId)> monitorAvailable;
    std::function<bool(TrackId)> isMonitorOn;
    std::function<void(TrackId)> toggleMonitor;
    std::function<bool(TrackId)> armAvailable;
    std::function<bool(TrackId)> isRecordArmed;
    std::function<void(TrackId)> toggleRecordArm;
    std::function<bool(TrackId)> instrumentEditorAvailable;
    std::function<void(TrackId)> openInstrumentEditor;
    std::function<bool(TrackId)> instrumentAlternativesAvailable;
    std::function<void(TrackId, juce::Rectangle<int> screenAnchor)> showInstrumentAlternatives;

    // --- inserts (TrackId + InsertSlotId explicit; the Inspector's own seam) ----------------------
    InspectorPluginHost inserts;
};
