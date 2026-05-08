#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/Vst3ChildProcessScan.h"

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
    requiredKitName_ = "FiftySixDegreesModified";
    pendingProjectGrooveAutoload_ = false;
    pendingAdvisoryPluginBundlePath_.clear();
    pendingInstrumentKind_.clear();
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

void InstrumentTrackController::setRequiredKitName(juce::String name) noexcept
{
    requiredKitName_ = std::move(name);
}

void InstrumentTrackController::clearExperimentalInstrumentStateForProjectLoad()
{
    trackActive_ = false;
    clips_.clear();
    nextClipId_ = 1;
    selectedClipId_ = 0;
    powerOn_ = true;
    muted_ = false;
    isActive_ = false;
    instrumentLoaded_ = false;
    requiredKitName_.clear();
    pendingProjectGrooveAutoload_ = false;
    pendingAdvisoryPluginBundlePath_.clear();
    pendingInstrumentKind_.clear();
}

ProjectFileExperimentalInstrumentTrackV1 InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    ProjectFileExperimentalInstrumentTrackV1 dto;
    if (!trackActive_)
    {
        return dto;
    }
    dto.enabled = true;
    dto.name = "Groove Agent SE";
    dto.instrumentKind = "GrooveAgentSE";
    dto.requiredKitName = requiredKitName_.isNotEmpty() ? requiredKitName_ : juce::String("FiftySixDegreesModified");
    dto.pluginBundlePath = host_.getLastLoadedVst3OriginalPath();
    dto.pluginWasLoadedOnSave = host_.hasInstrument();
    dto.powerOn = powerOn_;
    dto.muted = muted_;
    for (const auto& cptr : clips_)
    {
        if (cptr == nullptr)
        {
            continue;
        }
        ProjectFileExperimentalInstrumentClipV1 c;
        c.id = cptr->id;
        c.name = cptr->name;
        c.numSteps = cptr->pattern.numSteps;
        c.stepDenom = cptr->pattern.stepDenom;
        c.bpm = cptr->pattern.bpm;
        c.loop = cptr->pattern.loop;
        c.laneStartFractionPermille = cptr->laneStartFractionPermille;
        c.laneEndFractionPermille = cptr->laneEndFractionPermille;
        for (const auto& n : cptr->pattern.notes)
        {
            ProjectFileExperimentalInstrumentNoteV1 nn;
            nn.midiNote = n.midiNote;
            nn.step = n.step;
            nn.velocity = n.velocity;
            nn.lengthSteps = n.lengthSteps;
            c.notes.push_back(nn);
        }
        dto.clips.push_back(std::move(c));
    }
    return dto;
}

void InstrumentTrackController::restoreExperimentalInstrumentFromProject(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks)
{
    clearExperimentalInstrumentStateForProjectLoad();

    const ProjectFileExperimentalInstrumentTrackV1* chosen = nullptr;
    for (const auto& t : tracks)
    {
        if (!t.enabled || t.instrumentKind != "GrooveAgentSE")
        {
            continue;
        }
        chosen = &t;
        break;
    }
    if (chosen == nullptr)
    {
        sendChangeMessage();
        return;
    }

    trackActive_ = true;
    powerOn_ = chosen->powerOn;
    muted_ = chosen->muted;
    isActive_ = false;
    requiredKitName_ = chosen->requiredKitName.isNotEmpty() ? chosen->requiredKitName : juce::String("FiftySixDegreesModified");
    pendingProjectGrooveAutoload_ = chosen->pluginWasLoadedOnSave && chosen->instrumentKind == "GrooveAgentSE";
    pendingAdvisoryPluginBundlePath_ = chosen->pluginBundlePath;
    pendingInstrumentKind_ = chosen->instrumentKind;

    InstrumentMidiClipId maxId = 0;
    for (const auto& cdto : chosen->clips)
    {
        auto clip = std::make_unique<InstrumentMidiClip>();
        clip->id = static_cast<InstrumentMidiClipId>(cdto.id);
        maxId = juce::jmax(maxId, clip->id);
        clip->name = cdto.name;
        clip->pattern.numSteps = cdto.numSteps;
        clip->pattern.stepDenom = cdto.stepDenom;
        clip->pattern.bpm = cdto.bpm;
        clip->pattern.loop = cdto.loop;
        clip->laneStartFractionPermille = cdto.laneStartFractionPermille;
        clip->laneEndFractionPermille = cdto.laneEndFractionPermille;
        for (const auto& n : cdto.notes)
        {
            PrototypeMidiNote pn;
            pn.midiNote = n.midiNote;
            pn.step = n.step;
            pn.velocity = n.velocity;
            pn.lengthSteps = n.lengthSteps;
            clip->pattern.notes.push_back(pn);
        }
        clips_.push_back(std::move(clip));
    }
    if (clips_.empty())
    {
        auto clip = std::make_unique<InstrumentMidiClip>();
        clip->id = 1;
        clip->name = "MIDI 1";
        clip->pattern.numSteps = 16;
        clip->pattern.stepDenom = 16;
        clip->pattern.bpm = 110.0;
        clip->pattern.loop = true;
        clips_.push_back(std::move(clip));
        maxId = 1;
    }
    nextClipId_ = maxId + 1;
    selectedClipId_ = 0;
    instrumentLoaded_ = computeInstrumentLoadedFromHost();
    sendChangeMessage();
}

void InstrumentTrackController::runPendingGrooveAgentProjectAutoload(ExperimentalInstrumentHost& host,
                                                                    juce::String& outWarning)
{
    outWarning.clear();
    if (!trackActive_ || !pendingProjectGrooveAutoload_ || pendingInstrumentKind_ != "GrooveAgentSE")
    {
        pendingProjectGrooveAutoload_ = false;
        syncShellWithHostState();
        return;
    }

    pendingProjectGrooveAutoload_ = false;

    std::vector<juce::PluginDescription> descs;
    juce::File bundle;
    juce::String info;
    bool pathRepairUsed = false;
    const juce::File advisory(pendingAdvisoryPluginBundlePath_);

    if (!mini_daw::tryLoadExperimentalVst3DescriptionsFromCacheWithPathRepair(
            advisory, pendingInstrumentKind_, descs, bundle, info, &pathRepairUsed))
    {
        if (info.isNotEmpty())
        {
            outWarning = info;
        }
        mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: failed, project remains editable");
        juce::Logger::writeToLog("[project-autoload] Groove Agent SE autoload skipped: " + info);
        syncShellWithHostState();
        return;
    }
    if (descs.empty())
    {
        syncShellWithHostState();
        return;
    }

    const char* const tag = pathRepairUsed ? "project-autoload-repaired-cache" : "project-autoload-cached";
    const juce::Result loadResult = host.loadInstrumentFromDescription(descs.front(), bundle, tag);
    if (loadResult.wasOk())
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: load result ok");
        if (pathRepairUsed)
        {
            mini_daw::mergeExperimentalVst3DescriptionsCacheBundle(bundle, descs);
            mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: cache updated with repaired path");
        }
    }
    else
    {
        outWarning = loadResult.getErrorMessage();
        mini_daw::writeVst3OopScanDiagnosticLogLine(
            "project-autoload: load result failed message=\"" + loadResult.getErrorMessage() + "\"");
        mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: failed, project remains editable");
        juce::Logger::writeToLog("[project-autoload] Groove Agent load failed: " + loadResult.getErrorMessage());
    }
    syncShellWithHostState();
}
