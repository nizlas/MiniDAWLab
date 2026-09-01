// =============================================================================
// ExperimentalMidiPatternPlayerHostBinding — production delivery boundary
// =============================================================================
// The player's scheduling/dispatch TU (ExperimentalMidiPatternPlayer.cpp) is shared with
// MiniDAWSelftests and must not reference ExperimentalInstrumentHost, so the host-bound
// constructor lives here (app target only). The host outlives the player by design: the MIDI
// editor destroys/rebuilds its player before a rebind or destination change.
// =============================================================================

#include "ExperimentalMidiPatternPlayer.h"
#include "plugins/ExperimentalInstrumentHost.h"

ExperimentalMidiPatternPlayer::ExperimentalMidiPatternPlayer(ExperimentalInstrumentHost& host) noexcept
    : deliver_([&host](const juce::MidiMessage& m) { host.enqueueMidiMessageFromMessageThread(m); })
    , deliveryAvailable_([&host] { return host.hasInstrument(); })
{
}
