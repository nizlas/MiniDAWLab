#pragma once

// =============================================================================
// SecondaryMidiMapping — P2 Secondary channel-mapping policy (steering §17,
// PID-009: "v1 Preserve channels + optional simple remap; CC (incl. CC11)
// forwarded unchanged").
// =============================================================================
//
// Pure, allocation-predictable helpers applied ONLY at the Secondary delivery
// boundary (the per-block MIDI buffer handed to the Secondary instance).
// Stored notes, Primary channel settings, and MIDI export are never rewritten.
//
//   * forcedChannel == 0  -> Preserve channels (default): every message passes
//     through untouched.
//   * forcedChannel 1..16 -> every CHANNEL message (notes, CC incl. CC11,
//     pitch bend, aftertouch, program change) is delivered on that channel with
//     data bytes — note number, velocity, CC number and CC value — unchanged.
//     Non-channel messages (sysex, clock, ...) pass through untouched.
//
// Used on the audio thread: `juce::MidiMessage::setChannel` rewrites the status
// byte in place (small channel messages use the inline internal buffer — no
// heap allocation), and the destination buffer follows the same per-block
// `juce::MidiBuffer` pattern as the existing delivery merge.

#include <juce_audio_basics/juce_audio_basics.h>

namespace secondary_midi
{

/// True for the valid Force range (1..16). 0 = Preserve; anything else invalid.
[[nodiscard]] inline bool isForcedChannelValid(const int forcedChannel) noexcept
{
    return forcedChannel >= 1 && forcedChannel <= 16;
}

/// One message under the mapping. Channel messages move to `forcedChannel`
/// (when valid); everything else — and every message under Preserve — is
/// returned unchanged.
[[nodiscard]] inline juce::MidiMessage remapForSecondaryDelivery(const juce::MidiMessage& m,
                                                                 const int forcedChannel)
{
    if (!isForcedChannelValid(forcedChannel) || m.getChannel() <= 0)
    {
        return m;
    }
    juce::MidiMessage remapped(m);
    remapped.setChannel(forcedChannel);
    return remapped;
}

/// Buffer-level application preserving event order and sample positions.
/// `dst` is cleared first.
inline void applySecondaryChannelMapping(juce::MidiBuffer& dst,
                                         const juce::MidiBuffer& src,
                                         const int forcedChannel)
{
    dst.clear();
    for (const juce::MidiMessageMetadata meta : src)
    {
        dst.addEvent(remapForSecondaryDelivery(meta.getMessage(), forcedChannel),
                     meta.samplePosition);
    }
}

} // namespace secondary_midi
