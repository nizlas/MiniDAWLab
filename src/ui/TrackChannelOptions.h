#pragma once

// =============================================================================
// TrackChannelOptions — the selector item lists a channel offers (Inspector and mixer alike)
// =============================================================================
//
// ROLE
//   Pure builders for every routing / input selector of a track: audio input (device channels,
//   mono and stereo pairs), audio output (legal Master / Group targets), MIDI input device +
//   input-channel filter, MIDI output channel, MIDI destination ("MIDI To") and send destination
//   per UI slot. Each returns the labels, the parallel values and the index to select — exactly
//   the wording and the ordering the Inspector shows, so a mixer channel and the Inspector list
//   the same items for the same row. The builders read a published `SessionSnapshot` and the
//   device / MIDI snapshots the composition root supplies; they never touch the device manager.
//
// THREADING
//   [Message thread] (pure; thread-agnostic).
// =============================================================================

#include "domain/SessionRouting.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "ui/InspectorView.h" // InspectorAudioInputDeviceSnapshot / InspectorMidiInputSnapshot

#include <juce_core/juce_core.h>

#include <algorithm>
#include <vector>

namespace track_channel_options
{

/// One selector's content: `labels[i]` describes `values[i]`; `selectedIndex` is -1 when nothing
/// matches (callers then show the first item or leave the box blank, as the Inspector does).
template <typename ValueT>
struct OptionList
{
    juce::StringArray labels;
    std::vector<ValueT> values;
    int selectedIndex = -1;

    void add(const ValueT& value, const juce::String& label)
    {
        values.push_back(value);
        labels.add(label);
    }
    [[nodiscard]] int size() const noexcept { return static_cast<int>(values.size()); }
};

/// Audio Input (Audio rows): Default / No input / every ENABLED mono channel / even-aligned
/// enabled stereo pairs; an unresolvable saved assignment is appended as "(unavailable)".
/// The em dash used in the mono / stereo input labels ("Mono — In 1"), spelled as explicit UTF-8
/// so the text is identical regardless of the source file's encoding.
[[nodiscard]] inline juce::String labelDash()
{
    return juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 "));
}

[[nodiscard]] inline OptionList<TrackInputAssignment>
audioInputOptions(const Track& track, const InspectorAudioInputDeviceSnapshot& dev)
{
    OptionList<TrackInputAssignment> out;
    const TrackInputAssignment current = track.getInputAssignment();
    const juce::String dash = labelDash();
    const auto physicalLabel = [&dev](const int phys) {
        juce::String s = "In " + juce::String(phys + 1);
        if (phys >= 0 && phys < dev.physicalInputNames.size() && dev.physicalInputNames[phys].isNotEmpty())
        {
            s << ": " << dev.physicalInputNames[phys];
        }
        return s;
    };
    const auto addItem = [&out, &current](const TrackInputAssignment& value, const juce::String& label) {
        out.add(value, label);
        if (value == current)
        {
            out.selectedIndex = out.size() - 1;
        }
    };
    addItem({ TrackInputKind::DefaultFirstInput, -1, -1 }, "Default (first available input)");
    addItem({ TrackInputKind::None, -1, -1 }, "No input");
    if (dev.deviceAvailable)
    {
        for (int p = dev.activeInputChannels.findNextSetBit(0); p >= 0; p = dev.activeInputChannels.findNextSetBit(p + 1))
        {
            addItem({ TrackInputKind::Mono, p, -1 }, "Mono" + dash + physicalLabel(p));
        }
        const int highest = dev.activeInputChannels.getHighestBit();
        for (int p = 0; p + 1 <= highest; p += 2)
        {
            if (dev.activeInputChannels[p] && dev.activeInputChannels[p + 1])
            {
                addItem({ TrackInputKind::StereoPair, p, p + 1 }, "Stereo" + dash + physicalLabel(p) + " + " + physicalLabel(p + 1));
            }
        }
    }
    if (out.selectedIndex < 0)
    {
        juce::String label;
        switch (current.kind)
        {
        case TrackInputKind::Mono:
            label = "Mono" + dash + physicalLabel(current.physicalChannelA) + " (unavailable)";
            break;
        case TrackInputKind::StereoPair:
            label = "Stereo" + dash + physicalLabel(current.physicalChannelA) + " + " + physicalLabel(current.physicalChannelB) + " (unavailable)";
            break;
        case TrackInputKind::DefaultFirstInput:
        case TrackInputKind::None:
        default:
            label = "(unavailable)";
            break;
        }
        out.add(current, label);
        out.selectedIndex = out.size() - 1;
    }
    return out;
}

/// Audio Output (Audio / Instrument / Group rows): the legal Master + Group targets, Master
/// labelled with its display name. Empty for Master and Midi rows.
[[nodiscard]] inline OptionList<TrackId> audioOutputOptions(const SessionSnapshot& snap, const Track& track)
{
    OptionList<TrackId> out;
    const TrackId currentOut = track.getRoutedOutputTrackId();
    for (const TrackId destId : session_routing::legalOutputDestinations(snap, track.getId()))
    {
        juce::String label;
        const int dix = snap.findTrackIndexById(destId);
        if (dix >= 0 && snap.getTrack(dix).getKind() == TrackKind::Master)
        {
            label = juce::String(kMasterTrackDisplayName);
        }
        else if (dix >= 0)
        {
            label = snap.getTrack(dix).getName();
        }
        else
        {
            label = juce::String("Track ") + juce::String((juce::int64)destId);
        }
        out.add(destId, label);
        if (destId == currentOut)
        {
            out.selectedIndex = out.size() - 1;
        }
    }
    return out;
}

/// MIDI Input device (Instrument / Midi rows): None / All MIDI inputs / each device ("(missing)"
/// when not present) / the saved device even when the provider does not list it.
[[nodiscard]] inline OptionList<TrackMidiInputAssignment>
midiInputDeviceOptions(const Track& track, const InspectorMidiInputSnapshot& midi)
{
    OptionList<TrackMidiInputAssignment> out;
    const TrackMidiInputAssignment current = track.getMidiInputAssignment();
    const auto sameDevice = [&current](const TrackMidiInputAssignment& v) {
        return v.mode == current.mode && (v.mode != TrackMidiInputMode::Device || v.deviceIdentifier == current.deviceIdentifier);
    };
    const auto addItem = [&out, &sameDevice](const TrackMidiInputAssignment& value, const juce::String& label) {
        out.add(value, label);
        if (sameDevice(value))
        {
            out.selectedIndex = out.size() - 1;
        }
    };
    addItem({ TrackMidiInputMode::None, {}, {}, kTrackMidiInputChannelAll }, "None");
    addItem({ TrackMidiInputMode::AllEnabled, {}, {}, kTrackMidiInputChannelAll }, "All MIDI inputs");
    for (const auto& d : midi.devices)
    {
        TrackMidiInputAssignment v;
        v.mode = TrackMidiInputMode::Device;
        v.deviceIdentifier = d.identifier;
        v.deviceName = d.name;
        addItem(v, d.present ? d.name : (d.name + " (missing)"));
    }
    if (out.selectedIndex < 0 && current.mode == TrackMidiInputMode::Device)
    {
        out.add(current, (current.deviceName.isNotEmpty() ? current.deviceName : current.deviceIdentifier) + " (missing)");
        out.selectedIndex = out.size() - 1;
    }
    if (out.selectedIndex < 0)
    {
        out.selectedIndex = 0; // "None"
    }
    return out;
}

/// MIDI input-channel FILTER: All, 1 … 16 (value 0 = all). Disabled by callers when the mode is None.
[[nodiscard]] inline OptionList<int> midiInputChannelFilterOptions(const Track& track)
{
    OptionList<int> out;
    out.add(kTrackMidiInputChannelAll, "All");
    for (int ch = 1; ch <= 16; ++ch)
    {
        out.add(ch, juce::String(ch));
    }
    const int f = track.getMidiInputAssignment().channelFilter;
    out.selectedIndex = (f == kTrackMidiInputChannelAll) ? 0 : juce::jlimit(1, 16, f);
    return out;
}

/// MIDI output channel (Instrument / Midi rows): "Any (Preserve)", 1 … 16 with "10 (drums)".
[[nodiscard]] inline OptionList<int> midiOutputChannelOptions(const Track& track)
{
    OptionList<int> out;
    const int current = track.getMidiOutputChannel();
    const auto addItem = [&out, current](const int value, const juce::String& label) {
        out.add(value, label);
        if (value == current)
        {
            out.selectedIndex = out.size() - 1;
        }
    };
    addItem(kTrackMidiOutputChannelAny, "Any (Preserve)");
    for (int ch = kTrackMidiOutputChannelMin; ch <= kTrackMidiOutputChannelMax; ++ch)
    {
        addItem(ch, ch == kTrackMidiOutputChannelDrums ? juce::String("10 (drums)") : juce::String(ch));
    }
    return out;
}

/// MIDI destination ("MIDI To", Midi rows): "No destination (silent)" then every Instrument row.
/// A stale destination selects the first item.
[[nodiscard]] inline OptionList<TrackId> midiDestinationOptions(const SessionSnapshot& snap, const Track& track)
{
    OptionList<TrackId> out;
    const TrackId currentDest = track.getMidiDestinationTrackId();
    const auto addItem = [&out, currentDest](const TrackId destId, const juce::String& label) {
        out.add(destId, label);
        if (destId == currentDest)
        {
            out.selectedIndex = out.size() - 1;
        }
    };
    addItem(kInvalidTrackId, "No destination (silent)");
    for (int di = 0; di < snap.getNumTracks(); ++di)
    {
        const Track& cand = snap.getTrack(di);
        if (cand.getKind() == TrackKind::Instrument)
        {
            addItem(cand.getId(), cand.getName());
        }
    }
    if (out.selectedIndex < 0)
    {
        out.selectedIndex = 0;
    }
    return out;
}

/// Send destination for one UI slot: "(none)" then the legal Group destinations; a stored
/// destination that is no longer legal is appended as "<name> (stored)" and selected.
[[nodiscard]] inline OptionList<TrackId> sendDestinationOptions(const SessionSnapshot& snap, const Track& track, const int uiSlotIndex)
{
    OptionList<TrackId> out;
    out.add(kInvalidTrackId, "(none)");
    out.selectedIndex = 0;
    const std::vector<TrackId> legal = session_routing::legalSendDestinations(snap, track.getId());
    TrackId currentDest = kInvalidTrackId;
    const int sendIndex = findTrackSendVectorIndexForUiSlot(track.getSends(), uiSlotIndex);
    if (sendIndex >= 0)
    {
        currentDest = track.getSend(sendIndex).destTrackId;
    }
    const auto nameOf = [&snap](const TrackId id) {
        const int dix = snap.findTrackIndexById(id);
        return dix >= 0 ? snap.getTrack(dix).getName() : juce::String("Track ") + juce::String((juce::int64)id);
    };
    for (const TrackId destId : legal)
    {
        out.add(destId, nameOf(destId));
        if (destId == currentDest)
        {
            out.selectedIndex = out.size() - 1;
        }
    }
    if (sendIndex >= 0 && currentDest != kInvalidTrackId && std::find(legal.begin(), legal.end(), currentDest) == legal.end())
    {
        out.add(currentDest, nameOf(currentDest) + " (stored)");
        out.selectedIndex = out.size() - 1;
    }
    return out;
}

/// Fill a combo from an option list (item ids are 1-based indices), selecting without notifying.
template <typename ValueT>
inline void applyToCombo(juce::ComboBox& combo, const OptionList<ValueT>& list)
{
    combo.clear(juce::dontSendNotification);
    for (int i = 0; i < list.size(); ++i)
    {
        combo.addItem(list.labels[i], i + 1);
    }
    if (list.selectedIndex >= 0 && list.selectedIndex < list.size())
    {
        combo.setSelectedId(list.selectedIndex + 1, juce::dontSendNotification);
    }
    else
    {
        combo.setSelectedId(0, juce::dontSendNotification);
    }
}

} // namespace track_channel_options
