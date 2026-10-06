#pragma once

// =============================================================================
// TrackEditActions — the undoable per-track channel edits, bound to an explicit TrackId
// =============================================================================
//
// ROLE
//   One bag of message-thread actions that `TrackLanesEditCoordinator::install` builds exactly
//   once: each wraps the Session setter in `UndoRedoCoordinator::executeUndoableSessionEdit`
//   with the same label, validation (Session / `session_routing`) and recording / count-in
//   refusal the Inspector has always used. The Inspector receives these functions through its
//   `set…Handler` setters; the mixer's channel strips receive the SAME functions — so a value
//   changed in either view takes the identical path, with one undo step, and nothing has to
//   become the "active track" first. Fader / pan / mute / power are not here: they are direct,
//   non-undoable Session writes (see the Inspector channel panel and the track headers).
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <functional>

struct TrackEditActions
{
    /// Undoable rename ("Rename track"); false when refused or a no-op (Master, empty, same).
    std::function<bool(TrackId, juce::String)> renameTrack;
    /// Undoable audio output routing ("Route track output") — legal Master / Group targets only.
    std::function<void(TrackId, TrackId destTrackId)> setRoutedOutput;
    /// Undoable pre-gain in dB ("Set pre-gain") — Audio rows.
    std::function<void(TrackId, float preGainDb)> setPreGainDb;
    /// Undoable audio input assignment ("Set audio input") — Audio rows.
    std::function<void(TrackId, TrackInputAssignment)> setAudioInput;
    /// Undoable MIDI output channel ("Set MIDI channel") — Instrument / Midi rows.
    std::function<void(TrackId, int channel)> setMidiOutputChannel;
    /// Undoable live MIDI input ("Set MIDI input") — Instrument / Midi rows.
    std::function<void(TrackId, TrackMidiInputAssignment)> setMidiInput;
    /// Undoable MIDI destination ("Set MIDI destination") — Midi rows.
    std::function<void(TrackId, TrackId destTrackId)> setMidiDestination;
    /// Undoable send edits per UI slot 0 … 3 (destination `kInvalidTrackId` clears the slot).
    std::function<void(TrackId, int uiSlotIndex, TrackId destTrackId)> setSendDestination;
    std::function<void(TrackId, int uiSlotIndex, float amountLinear)> setSendAmount;
    std::function<void(TrackId, int uiSlotIndex, bool enabled)> setSendEnabled;
};
