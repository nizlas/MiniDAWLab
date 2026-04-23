#pragma once

// =============================================================================
// ClipWaveformView  —  picture of the clip + playhead; no audio, no transport ownership
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   presentation only: read Session to know *what* to draw, read Transport::readPlayheadSamplesForUi
//   to know *where* the playhead is. It never subscribes to PlaybackEngine and never touches
//   audio buffers on the device thread — that separation is explicit in ARCHITECTURE_PRINCIPLES.
//
// COORDINATE SYSTEM (Phase 1)
//   The clip spans the full width of the view. sample index 0 is the left edge, last sample
//   the right. Playhead x = (playhead / numSamples) * width. Click-to-seek uses the same mapping
//   in reverse and calls Transport::requestSeek (the audio thread will apply the seek).
//
// THREADING
//   juce::Component methods (paint, mouseDown) and juce::Timer::timerCallback run on the
//   [Message thread] (JUCE rule). A timer at ~20 Hz calls repaint() so the playhead line moves
//   without the audio code telling the UI “each sample”.
//
// NOT RESPONSIBLE FOR
//   Decoding, changing clip data, or owning Transport/Session (references only). Peaks are a
//   visual simplification, not a metering pipeline.
//
// ClipWaveformView.cpp uses plain-language notes next to the seek mapping, the peak precompute,
// and the playhead line so the “picture” and the transport stay mentally aligned with the engine.
// =============================================================================

#include "domain/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

class Transport;

class ClipWaveformView : public juce::Component, private juce::Timer
{
public:
    // [Message thread] session/transport outlive the view; used for read-only queries.
    ClipWaveformView(Session& session, Transport& transport);
    ~ClipWaveformView() override;

    // [Message thread] JUCE: full redraw; uses rebuildPeaksIfNeeded, then draws peaks + playhead.
    void paint(juce::Graphics& g) override;

    // [Message thread] Map x → sample index → requestSeek. Does not set playhead directly.
    void mouseDown(const juce::MouseEvent& e) override;

private:
    // [Message thread] Timer tick: schedule repaint; paint reads the latest playhead.
    void timerCallback() override;

    // [Message thread] Rebuilds peaks_ (max per column) when clip or width changes; O(columns*range).
    void rebuildPeaksIfNeeded();

    Session& session_;
    Transport& transport_;
    const AudioClip* lastClip_ = nullptr;
    int lastWidth_ = 0;
    std::vector<float> peaks_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipWaveformView)
};
