#include "ExperimentalPianoRollView.h"
#include "ExperimentalMidiPatternPlayer.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace
{
    [[nodiscard]] bool isBlackKey(const int midiNote) noexcept
    {
        const int k = ((midiNote % 12) + 12) % 12;
        return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
    }
} // namespace

ExperimentalPianoRollView::ExperimentalPianoRollView(ExperimentalMidiPattern& pattern,
                                                   ExperimentalMidiPatternPlayer* player)
    : pattern_(pattern)
    , player_(player)
{
    setOpaque(true);
    startTimerHz(30);
}

void ExperimentalPianoRollView::timerCallback()
{
    if (player_ != nullptr && player_->isPlaying())
    {
        repaint();
    }
}

void ExperimentalPianoRollView::resized()
{
    Component::resized();
}

juce::Rectangle<int> ExperimentalPianoRollView::keyboardBounds() const
{
    auto r = getLocalBounds();
    return r.removeFromLeft(kKeyboardWidth);
}

juce::Rectangle<int> ExperimentalPianoRollView::gridBounds() const
{
    auto r = getLocalBounds();
    r.removeFromLeft(kKeyboardWidth);
    return r;
}

float ExperimentalPianoRollView::cellWidth() const
{
    const auto gr = gridBounds();
    const int n = juce::jmax(1, pattern_.numSteps);
    return (float)gr.getWidth() / (float)n;
}

int ExperimentalPianoRollView::pitchAtY(const int y) const
{
    const auto gr = gridBounds();
    const int relY = y - gr.getY();
    const int row = relY / kRowHeight;
    const int span = kPitchHigh - kPitchLow + 1;
    const int clampedRow = juce::jlimit(0, span - 1, row);
    return kPitchHigh - clampedRow;
}

int ExperimentalPianoRollView::stepAtX(const int x) const
{
    const auto gr = gridBounds();
    const int relX = x - gr.getX();
    const float cw = cellWidth();
    if (cw <= 0.0f)
    {
        return 0;
    }
    const int s = (int)((float)relX / cw);
    return juce::jlimit(0, juce::jmax(0, pattern_.numSteps - 1), s);
}

void ExperimentalPianoRollView::mouseDown(const juce::MouseEvent& e)
{
    const auto kb = keyboardBounds();
    const auto gr = gridBounds();
    if (gr.contains(e.position.toInt()))
    {
        const int step = stepAtX(e.x);
        const int pitch = pitchAtY(e.y);
        pattern_.toggleHit(pitch, step);
        repaint();
    }
    else
    {
        juce::ignoreUnused(kb);
    }
}

void ExperimentalPianoRollView::paint(juce::Graphics& g)
{
    const auto kb = keyboardBounds();
    const auto gr = gridBounds();

    g.fillAll(juce::Colour(0xff1a1a1e));

    const float cw = cellWidth();
    const int nSteps = juce::jmax(1, pattern_.numSteps);

    auto rowRect = [&](int midiNote) -> juce::Rectangle<int> {
        const int rowFromTop = kPitchHigh - midiNote;
        return gr.withY(gr.getY() + rowFromTop * kRowHeight).withHeight(kRowHeight);
    };

    for (int pitch = kPitchHigh; pitch >= kPitchLow; --pitch)
    {
        const auto rr = rowRect(pitch);
        if (isBlackKey(pitch))
        {
            g.setColour(juce::Colour(0xff25252d));
        }
        else
        {
            g.setColour(juce::Colour(0xff1f1f26));
        }
        g.fillRect(rr);
    }

    for (int s = 0; s <= nSteps; ++s)
    {
        const int x = gr.getX() + (int)((float)s * cw);
        juce::Colour c = juce::Colour(0xff333340);
        if (s % 4 == 0)
        {
            c = juce::Colour(0xff454552);
        }
        if (s % nSteps == 0)
        {
            c = juce::Colour(0xff505060);
        }
        g.setColour(c);
        g.drawVerticalLine(x, (float)gr.getY(), (float)gr.getBottom());
    }

    const float halfW = juce::jmax(3.0f, juce::jmin(cw * 0.55f, (float)kRowHeight * 0.85f) * 0.5f);
    const float halfH = juce::jmax(3.0f, (float)kRowHeight * 0.75f * 0.5f);

    for (const auto& hit : pattern_.notes)
    {
        if (hit.midiNote < kPitchLow || hit.midiNote > kPitchHigh)
        {
            continue;
        }
        const auto rr = rowRect(hit.midiNote);
        const float cx = (float)gr.getX() + ((float)hit.step + 0.5f) * cw;
        const float cy = (float)rr.getCentreY();

        juce::Path diamond;
        diamond.addQuadrilateral(cx, cy - halfH, cx + halfW, cy, cx, cy + halfH, cx - halfW, cy);

        g.setColour(juce::Colour(0xff8a2c46));
        g.strokePath(diamond, juce::PathStrokeType(1.2f));
        g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.92f));
        g.fillPath(diamond);
    }

    if (player_ != nullptr && player_->isPlaying() && cw > 0.0f)
    {
        const float ph = player_->getPlayheadNormalized();
        const float px = (float)gr.getX() + ph * (float)gr.getWidth();
        g.setColour(juce::Colour(0xffe85566));
        g.drawLine(px, (float)gr.getY(), px, (float)gr.getBottom(), 1.2f);
    }

    g.setColour(juce::Colour(0xff2a2a32));
    g.fillRect(kb);

    for (int pitch = kPitchHigh; pitch >= kPitchLow; --pitch)
    {
        const int rowFromTop = kPitchHigh - pitch;
        const int y = gr.getY() + rowFromTop * kRowHeight;
        auto wr = kb.withY(y).withHeight(kRowHeight);

        if (pitch == kPitchLow || pitch == kPitchHigh)
        {
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillRect(wr);
        }

        if (isBlackKey(pitch))
        {
            g.setColour(juce::Colour(0xff111118));
            const int bh = juce::jmax(8, (int)((float)kRowHeight * 0.72f));
            g.fillRoundedRectangle(wr.withSizeKeepingCentre(wr.getWidth() - 4, bh).toFloat(), 2.0f);
        }
        else
        {
            g.setColour(juce::Colour(0xfff0f0f5));
            g.fillRoundedRectangle(wr.reduced(2, 1).toFloat(), 2.0f);
            g.setColour(juce::Colour(0xff888899));
            g.drawRoundedRectangle(wr.reduced(2, 1).toFloat(), 2.0f, 1.0f);
        }

        const int k = ((pitch % 12) + 12) % 12;
        if (k == 0)
        {
            const juce::String label = juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
            g.setColour(isBlackKey(pitch) ? juce::Colours::lightgrey : juce::Colours::black);
            g.setFont(10.0f);
            g.drawText(label, wr.reduced(2, 0), juce::Justification::centredLeft, true);
        }
    }
}
