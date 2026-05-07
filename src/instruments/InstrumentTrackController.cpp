#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

InstrumentTrackController::InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept
    : host_(host)
{
}

bool InstrumentTrackController::computeInstrumentLoadedFromHost() const noexcept
{
    return host_.hasInstrument() && host_.getInstrumentNameForUi().containsIgnoreCase("Groove Agent");
}

juce::String InstrumentTrackController::getLaneHeaderTitle() const
{
    if (!trackActive_)
    {
        return {};
    }
    return "Groove Agent SE";
}

juce::String InstrumentTrackController::getLaneHeaderSubtitle() const
{
    if (!trackActive_)
    {
        return {};
    }
    return instrumentLoaded_ ? juce::String("Instrument Track - experimental, not saved")
                             : juce::String("(instrument unloaded)");
}

juce::String InstrumentTrackController::getLaneHeaderText() const
{
    const auto t = getLaneHeaderTitle();
    if (t.isEmpty())
    {
        return {};
    }
    return t + "\n" + getLaneHeaderSubtitle();
}

bool InstrumentTrackController::tryAddGrooveAgentInstrumentTrackShell()
{
    if (trackActive_)
    {
        return false;
    }

    trackActive_ = true;
    powerOn_ = true;
    muted_ = false;
    isActive_ = false;
    instrumentLoaded_ = computeInstrumentLoadedFromHost();

    auto clip = std::make_unique<InstrumentMidiClip>();
    clip->id = nextClipId_++;
    clip->name = "MIDI 1";
    clip->pattern.numSteps = 16;
    clip->pattern.stepDenom = 16;
    clip->pattern.bpm = 110.0;
    clip->pattern.loop = true;
    clip->laneStartFractionPermille = 0;
    clip->laneEndFractionPermille = 250;
    selectedClipId_ = 0;
    clips_.push_back(std::move(clip));

    sendChangeMessage();
    return true;
}

void InstrumentTrackController::syncShellWithHostState()
{
    if (!trackActive_)
    {
        return;
    }

    const bool now = computeInstrumentLoadedFromHost();
    if (now == instrumentLoaded_)
    {
        return;
    }

    instrumentLoaded_ = now;
    sendChangeMessage();
}

InstrumentMidiClip* InstrumentTrackController::getClipById(const InstrumentMidiClipId id) noexcept
{
    for (auto& c : clips_)
    {
        if (c->id == id)
        {
            return c.get();
        }
    }
    return nullptr;
}

const InstrumentMidiClip* InstrumentTrackController::getClipById(const InstrumentMidiClipId id) const noexcept
{
    for (const auto& c : clips_)
    {
        if (c->id == id)
        {
            return c.get();
        }
    }
    return nullptr;
}

InstrumentMidiClip* InstrumentTrackController::findClipAtLaneFraction(const float t) noexcept
{
    const float clamped = juce::jlimit(0.f, 1.f, t);
    for (auto& c : clips_)
    {
        const float s = (float)c->laneStartFractionPermille / 1000.f;
        const float e = (float)c->laneEndFractionPermille / 1000.f;
        if (clamped >= s && clamped <= e)
        {
            return c.get();
        }
    }
    return nullptr;
}

void InstrumentTrackController::setSelectedClipId(const InstrumentMidiClipId id) noexcept
{
    if (selectedClipId_ == id)
    {
        return;
    }
    selectedClipId_ = id;
    sendChangeMessage();
}

void InstrumentTrackController::setPowerOn(const bool on) noexcept
{
    if (powerOn_ == on)
    {
        return;
    }
    powerOn_ = on;
    sendChangeMessage();
}

void InstrumentTrackController::setMuted(const bool muted) noexcept
{
    if (muted_ == muted)
    {
        return;
    }
    muted_ = muted;
    sendChangeMessage();
}

void InstrumentTrackController::setActive(const bool active) noexcept
{
    if (isActive_ == active)
    {
        return;
    }
    isActive_ = active;
    sendChangeMessage();
}
