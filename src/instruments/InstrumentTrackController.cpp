#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/Vst3ChildProcessScan.h"

#include <algorithm>
#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

InstrumentTrackController::InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept
    : host_(host)
{
    publishRenderSnapshot();
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
    clip->startSamples = 0;
    clip->lengthSamples = 0;
    selectedClipId_ = 0;
    clips_.push_back(std::move(clip));
    recomputeLockedClipLengthFromPatternGrid(*clips_.back());

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
    publishRenderSnapshot();
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
    publishRenderSnapshot();
    sendChangeMessage();
}

void InstrumentTrackController::setMuted(const bool muted) noexcept
{
    if (muted_ == muted)
    {
        return;
    }
    muted_ = muted;
    publishRenderSnapshot();
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

int InstrumentTrackController::pluginNoteNameQueryChannel(const InstrumentMidiClip* contextClip) const noexcept
{
    const InstrumentMidiClip* clip = contextClip;
    if (clip == nullptr && selectedClipId_ != 0)
    {
        clip = getClipById(selectedClipId_);
    }
    if (clip != nullptr && clip->pattern.usesTimelineNotes() && !clip->pattern.timelineNotes.empty())
    {
        int ch = -1;
        for (const auto& tn : clip->pattern.timelineNotes)
        {
            const int cn = juce::jlimit(1, 16, (int)tn.channel);
            if (ch < 0)
            {
                ch = cn;
            }
            else if (ch != cn)
            {
                ch = -2;
                break;
            }
        }
        if (ch >= 1 && ch <= 16)
        {
            return ch;
        }
    }
    return 10;
}

juce::String InstrumentTrackController::getDrumNoteUserOverride(const int midiNote) const noexcept
{
    if (midiNote < 0 || midiNote > 127)
    {
        return {};
    }
    const auto it = drumLabels_.find(midiNote);
    if (it == drumLabels_.end())
    {
        return {};
    }
    return it->second.manual;
}

void InstrumentTrackController::setDrumNoteUserOverride(const int midiNote, juce::String displayName) noexcept
{
    setDrumLabelManual(midiNote, std::move(displayName));
}

void InstrumentTrackController::setDrumLabelManual(const int midiNote, juce::String name) noexcept
{
    if (midiNote < 0 || midiNote > 127)
    {
        return;
    }
    name = name.trim();

    DrumLabelLayers& layers = drumLabels_[midiNote]; // inserts empty layers when first touched

    if (name.isEmpty())
    {
        if (layers.manual.isEmpty())
        {
            return;
        }
        layers.manual.clear();
        pruneDrumLabelLayersIfUnused(midiNote);
        sendChangeMessage();
        return;
    }

    if (layers.manual == name)
    {
        return;
    }
    layers.manual = std::move(name);
    sendChangeMessage();
}

void InstrumentTrackController::mergeAutoPluginDrumLabels(const std::map<int, juce::String>& discovered,
                                                           const juce::String& pluginIdentifier)
{
    juce::ignoreUnused(pluginIdentifier);
    bool anyChanged = false;
    for (const auto& kv : discovered)
    {
        const int note = kv.first;
        if (note < 0 || note > 127)
        {
            continue;
        }
        const juce::String trimmed = kv.second.trim();
        if (trimmed.isEmpty())
        {
            continue;
        }

        DrumLabelLayers& layers = drumLabels_[note];

        // Manual edits always win until cleared.
        if (layers.manual.isNotEmpty())
        {
            continue;
        }

        if (layers.autoPlugin == trimmed)
        {
            continue;
        }
        layers.autoPlugin = trimmed;
        anyChanged = true;
    }

    if (anyChanged)
    {
        sendChangeMessage();
    }
}

std::optional<std::pair<juce::String, DrumLabelSource>> InstrumentTrackController::getEffectiveDrumLabel(
    const int midiNote) const
{
    if (midiNote < 0 || midiNote > 127)
    {
        return std::nullopt;
    }
    const auto it = drumLabels_.find(midiNote);
    if (it == drumLabels_.end())
    {
        return std::nullopt;
    }
    const DrumLabelLayers& layer = it->second;
    if (layer.manual.isNotEmpty())
    {
        return std::make_pair(layer.manual, DrumLabelSource::manual);
    }
    if (layer.autoPlugin.isNotEmpty())
    {
        return std::make_pair(layer.autoPlugin, DrumLabelSource::autoPlugin);
    }
    return std::nullopt;
}

void InstrumentTrackController::pruneDrumLabelLayersIfUnused(const int midiNote) noexcept
{
    const auto it = drumLabels_.find(midiNote);
    if (it == drumLabels_.end())
    {
        return;
    }
    if (it->second.manual.isEmpty() && it->second.autoPlugin.isEmpty())
    {
        drumLabels_.erase(it);
    }
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
    pendingPluginStateBase64_.clear();
    drumLabels_.clear();
    publishRenderSnapshot();
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
    if (dto.pluginWasLoadedOnSave)
    {
        dto.pluginStateBase64 = host_.getCurrentInstrumentStateBase64();
    }
    dto.powerOn = powerOn_;
    dto.muted = muted_;
    for (const auto& kv : drumLabels_)
    {
        if (kv.second.manual.isNotEmpty())
        {
            dto.drumNoteNameOverrides.push_back({ kv.first, kv.second.manual });
        }
        if (kv.second.autoPlugin.isNotEmpty())
        {
            dto.drumNoteNameAutoPlugin.push_back({ kv.first, kv.second.autoPlugin });
        }
    }
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
        c.startSamples = cptr->startSamples;
        c.lengthSamples = cptr->lengthSamples;
        c.laneStartFractionPermille = cptr->laneStartFractionPermille;
        c.laneEndFractionPermille = cptr->laneEndFractionPermille;
        c.midiRollVisibleStartSamples = cptr->midiRollVisibleStartSamples;
        c.midiRollSamplesPerPixel = cptr->midiRollSamplesPerPixel;
        c.midiRollFollowEnabled = cptr->midiRollFollowEnabled;
        for (const auto& n : cptr->pattern.notes)
        {
            ProjectFileExperimentalInstrumentNoteV1 nn;
            nn.midiNote = n.midiNote;
            nn.step = n.step;
            nn.velocity = n.velocity;
            nn.lengthSteps = n.lengthSteps;
            c.notes.push_back(nn);
        }
        c.ticksPerQuarter = juce::jmax(1, cptr->pattern.ticksPerQuarter);
        for (const auto& nn : cptr->pattern.timelineNotes)
        {
            ProjectFileExperimentalTimelineNoteV12 t;
            t.midiNote = nn.midiNote;
            t.velocity = nn.velocity;
            t.channel = (int)juce::jlimit(1, 16, (int)nn.channel);
            t.startTick = nn.startTick;
            t.durationTicks = nn.durationTicks;
            c.timelineNotes.push_back(std::move(t));
        }
        dto.clips.push_back(std::move(c));
    }
    return dto;
}

std::vector<ProjectFileExperimentalInstrumentTrackV1> InstrumentTrackController::buildExperimentalInstrumentMusicalUndoBlock() const
{
    if (!trackActive_)
    {
        return {};
    }
    ProjectFileExperimentalInstrumentTrackV1 dto = buildExperimentalInstrumentProjectBlock();
    stripExperimentalInstrumentTrackPluginFieldsForUndo(dto);
    return { std::move(dto) };
}

void InstrumentTrackController::applyExperimentalInstrumentMusicalUndoBlock(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks)
{
    if (!trackActive_)
    {
        return;
    }
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
        return;
    }

    // `drumLabels_` (manual + auto layers) is not loaded from `chosen`: musical undo DTOs strip overrides and
    // equality ignores them, so row renames survive note undo/redo.

    powerOn_ = chosen->powerOn;
    muted_ = chosen->muted;
    if (chosen->requiredKitName.isNotEmpty())
    {
        requiredKitName_ = chosen->requiredKitName;
    }

    clips_.clear();
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
        clip->midiRollVisibleStartSamples = juce::jmax(std::int64_t{0}, cdto.midiRollVisibleStartSamples);
        clip->midiRollSamplesPerPixel = cdto.midiRollSamplesPerPixel;
        if (!std::isfinite(clip->midiRollSamplesPerPixel) || clip->midiRollSamplesPerPixel < 0.0)
        {
            clip->midiRollSamplesPerPixel = 0.0;
        }
        clip->midiRollFollowEnabled = cdto.midiRollFollowEnabled;
        clip->startSamples = juce::jmax(std::int64_t{0}, cdto.startSamples);
        clip->lengthSamples = cdto.lengthSamples;
        for (const auto& n : cdto.notes)
        {
            PrototypeMidiNote pn;
            pn.midiNote = n.midiNote;
            pn.step = n.step;
            pn.velocity = n.velocity;
            pn.lengthSteps = n.lengthSteps;
            clip->pattern.notes.push_back(pn);
        }
        clip->pattern.ticksPerQuarter = juce::jmax(1, cdto.ticksPerQuarter);
        for (const auto& tn : cdto.timelineNotes)
        {
            TimelineMidiNote n;
            n.midiNote = tn.midiNote;
            n.velocity = tn.velocity;
            n.channel = (std::uint8_t)juce::jlimit(1, 16, tn.channel);
            n.startTick = tn.startTick;
            n.durationTicks = juce::jmax<std::int64_t>(1, tn.durationTicks);
            clip->pattern.timelineNotes.push_back(n);
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
        clip->startSamples = 0;
        clip->lengthSamples = 0;
        clips_.push_back(std::move(clip));
        recomputeLockedClipLengthFromPatternGrid(*clips_.back());
        maxId = 1;
    }
    for (auto& cp : clips_)
    {
        if (cp == nullptr)
        {
            continue;
        }
        double sr = timelineSampleRate_;
        if (sr <= 0.0 || !std::isfinite(sr))
        {
            sr = 48000.0;
        }
        if (cp->pattern.usesTimelineNotes())
        {
            const std::int64_t tlen = timelinePatternLengthSamples(cp->pattern, sr);
            if (tlen > 0)
            {
                if (cp->lengthSamples <= 0)
                {
                    cp->lengthSamples = tlen;
                }
                else
                {
                    cp->lengthSamples = juce::jmax(cp->lengthSamples, tlen);
                }
            }
        }
        else if (cp->lengthSamples <= 0)
        {
            recomputeLockedClipLengthFromPatternGrid(*cp);
        }
    }
    nextClipId_ = maxId + 1;
    if (selectedClipId_ != 0 && getClipById(selectedClipId_) == nullptr)
    {
        selectedClipId_ = 0;
    }
    instrumentLoaded_ = computeInstrumentLoadedFromHost();
    publishRenderSnapshot();
    sendChangeMessage();
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
    pendingPluginStateBase64_ = chosen->pluginStateBase64;

    drumLabels_.clear();
    for (const auto& kv : chosen->drumNoteNameOverrides)
    {
        if (kv.first >= 0 && kv.first <= 127 && kv.second.isNotEmpty())
        {
            drumLabels_[kv.first].manual = kv.second;
        }
    }
    for (const auto& kv : chosen->drumNoteNameAutoPlugin)
    {
        if (kv.first >= 0 && kv.first <= 127 && kv.second.isNotEmpty())
        {
            drumLabels_[kv.first].autoPlugin = kv.second;
        }
    }

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
        clip->midiRollVisibleStartSamples = juce::jmax(std::int64_t{0}, cdto.midiRollVisibleStartSamples);
        clip->midiRollSamplesPerPixel = cdto.midiRollSamplesPerPixel;
        if (!std::isfinite(clip->midiRollSamplesPerPixel) || clip->midiRollSamplesPerPixel < 0.0)
        {
            clip->midiRollSamplesPerPixel = 0.0;
        }
        clip->midiRollFollowEnabled = cdto.midiRollFollowEnabled;
        clip->startSamples = juce::jmax(std::int64_t{0}, cdto.startSamples);
        clip->lengthSamples = cdto.lengthSamples;
        for (const auto& n : cdto.notes)
        {
            PrototypeMidiNote pn;
            pn.midiNote = n.midiNote;
            pn.step = n.step;
            pn.velocity = n.velocity;
            pn.lengthSteps = n.lengthSteps;
            clip->pattern.notes.push_back(pn);
        }
        clip->pattern.ticksPerQuarter = juce::jmax(1, cdto.ticksPerQuarter);
        for (const auto& tn : cdto.timelineNotes)
        {
            TimelineMidiNote n;
            n.midiNote = tn.midiNote;
            n.velocity = tn.velocity;
            n.channel = (std::uint8_t)juce::jlimit(1, 16, tn.channel);
            n.startTick = tn.startTick;
            n.durationTicks = juce::jmax<std::int64_t>(1, tn.durationTicks);
            clip->pattern.timelineNotes.push_back(n);
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
        clip->startSamples = 0;
        clip->lengthSamples = 0;
        clips_.push_back(std::move(clip));
        recomputeLockedClipLengthFromPatternGrid(*clips_.back());
        maxId = 1;
    }
    for (auto& cp : clips_)
    {
        if (cp == nullptr)
        {
            continue;
        }
        double sr = timelineSampleRate_;
        if (sr <= 0.0 || !std::isfinite(sr))
        {
            sr = 48000.0;
        }
        if (cp->pattern.usesTimelineNotes())
        {
            const std::int64_t tlen = timelinePatternLengthSamples(cp->pattern, sr);
            if (tlen > 0)
            {
                if (cp->lengthSamples <= 0)
                {
                    cp->lengthSamples = tlen;
                }
                else
                {
                    cp->lengthSamples = juce::jmax(cp->lengthSamples, tlen);
                }
            }
        }
        else if (cp->lengthSamples <= 0)
        {
            recomputeLockedClipLengthFromPatternGrid(*cp);
        }
    }
    nextClipId_ = maxId + 1;
    selectedClipId_ = 0;
    instrumentLoaded_ = computeInstrumentLoadedFromHost();
    publishRenderSnapshot();
    sendChangeMessage();
}

void InstrumentTrackController::runPendingGrooveAgentProjectAutoload(ExperimentalInstrumentHost& host,
                                                                    juce::String& outWarning)
{
    outWarning.clear();
    if (!trackActive_ || !pendingProjectGrooveAutoload_ || pendingInstrumentKind_ != "GrooveAgentSE")
    {
        pendingProjectGrooveAutoload_ = false;
        pendingPluginStateBase64_.clear();
        syncShellWithHostState();
        return;
    }

    pendingProjectGrooveAutoload_ = false;
    const juce::String pendingB64 = pendingPluginStateBase64_;
    pendingPluginStateBase64_.clear();

    mini_daw::Vst3GrooveCacheLoadCandidate v2Cand;
    mini_daw::Vst3GrooveCacheLoadCandidate v1Cand;
    juce::String info;
    const juce::File advisory(pendingAdvisoryPluginBundlePath_);

    if (!mini_daw::tryLoadGrooveAgentCacheCandidates(advisory, v2Cand, v1Cand, info))
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

    juce::MemoryBlock decodedState;
    const juce::MemoryBlock* statePtr = nullptr;
    juce::String stateRestoreHostWarning;

    if (pendingB64.isNotEmpty())
    {
        juce::MemoryOutputStream mos;
        if (!juce::Base64::convertFromBase64(mos, pendingB64))
        {
            ExperimentalInstrumentHost::appendInstrumentHostLogLine(
                "plugin-state: restore failed message=\"invalid base64 in project file\"");
            outWarning = "Groove Agent plug-in state in this project could not be decoded. "
                         "Open the instrument editor and load the kit manually if audio is silent.";
            if (requiredKitName_.isNotEmpty())
            {
                outWarning << " Kit hint: " << requiredKitName_ << ".";
            }
        }
        else
        {
            decodedState.replaceAll(mos.getData(), mos.getDataSize());
            if (decodedState.getSize() > 0)
            {
                statePtr = &decodedState;
            }
        }
    }
    else
    {
        ExperimentalInstrumentHost::appendInstrumentHostLogLine("plugin-state: restore skipped reason=no-state");
    }

    const auto buildTag = [](const mini_daw::Vst3GrooveCacheLoadCandidate& c) -> const char* {
        if (c.tier == mini_daw::Vst3ExperimentalCacheTier::V2)
        {
            return c.pathRepairUsed ? "project-autoload-repaired-cache-v2" : "project-autoload-cached-v2";
        }
        return c.pathRepairUsed ? "project-autoload-repaired-cache-v1" : "project-autoload-cached-v1";
    };

    const auto tryLoadFromCandidate = [&](const mini_daw::Vst3GrooveCacheLoadCandidate& cand) -> juce::Result {
        if (!cand.valid || cand.descriptions.empty())
        {
            return juce::Result::fail("no candidate");
        }
        return host.loadInstrumentFromDescription(
            cand.descriptions.front(),
            cand.resolvedBundle,
            buildTag(cand),
            statePtr,
            (statePtr != nullptr) ? &stateRestoreHostWarning : nullptr);
    };

    juce::Result loadResult = juce::Result::fail("");

    if (v2Cand.valid)
    {
        loadResult = tryLoadFromCandidate(v2Cand);
        if (loadResult.wasOk())
        {
            mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: cache source=v2 load=ok");
        }
        else
        {
            mini_daw::writeVst3OopScanDiagnosticLogLine(
                "project-autoload: cache source=v2 load=failed falling_back=v1 message=\""
                + loadResult.getErrorMessage() + "\"");
        }
    }

    if (!loadResult.wasOk() && !v2Cand.valid)
    {
        const juce::File scanTarget = [&]() -> juce::File {
            if (advisory.exists())
            {
                return advisory;
            }
            if (v1Cand.valid && v1Cand.resolvedBundle.exists())
            {
                return v1Cand.resolvedBundle;
            }
            return mini_daw::getGrooveAgentSeVst3BundlePathForOopScanFallback();
        }();

        if (scanTarget.exists())
        {
            mini_daw::writeVst3OopScanDiagnosticLogLine(
                "project-autoload: v2 cache miss, OOP scan start target=\"" + scanTarget.getFullPathName() + "\"");
            const mini_daw::Vst3OopScanResult scanOut = mini_daw::runVst3OopScanBlocking(scanTarget);
            const bool scanOk = (scanOut.outcome == mini_daw::Vst3OopScanOutcome::Success)
                                 && !scanOut.descriptions.empty();
            if (scanOk)
            {
                loadResult = host.loadInstrumentFromDescription(
                    scanOut.descriptions.front(),
                    scanTarget,
                    "project-autoload-oop-fresh-v2",
                    statePtr,
                    (statePtr != nullptr) ? &stateRestoreHostWarning : nullptr);
                if (loadResult.wasOk())
                {
                    mini_daw::writeVst3OopScanDiagnosticLogLine(
                        "project-autoload: cache source=v2-fresh-scan load=ok");
                }
                else
                {
                    mini_daw::writeVst3OopScanDiagnosticLogLine(
                        "project-autoload: v2 fresh scan load=failed message=\"" + loadResult.getErrorMessage()
                        + "\" falling_back=v1");
                }
            }
            else
            {
                mini_daw::writeVst3OopScanDiagnosticLogLine(
                    "project-autoload: OOP scan did not yield usable descriptions; falling_back=v1");
            }
        }
        else
        {
            mini_daw::writeVst3OopScanDiagnosticLogLine(
                "project-autoload: v2 cache miss, no OOP scan target on disk; falling_back=v1");
        }
    }

    if (!loadResult.wasOk() && v1Cand.valid)
    {
        loadResult = tryLoadFromCandidate(v1Cand);
        if (loadResult.wasOk())
        {
            mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: cache source=v1 load=ok");
        }
        else
        {
            mini_daw::writeVst3OopScanDiagnosticLogLine(
                "project-autoload: cache source=v1 load=failed message=\"" + loadResult.getErrorMessage() + "\"");
        }
    }

    if (!loadResult.wasOk())
    {
        if (outWarning.isNotEmpty())
        {
            outWarning << "\n\n";
        }
        outWarning << loadResult.getErrorMessage();
        mini_daw::writeVst3OopScanDiagnosticLogLine("project-autoload: failed, project remains editable");
        juce::Logger::writeToLog("[project-autoload] Groove Agent load failed: " + loadResult.getErrorMessage());
    }

    if (stateRestoreHostWarning.isNotEmpty())
    {
        if (outWarning.isNotEmpty())
        {
            outWarning << "\n\n";
        }
        outWarning << stateRestoreHostWarning;
        if (requiredKitName_.isNotEmpty())
        {
            outWarning << " Kit hint: " << requiredKitName_ << ".";
        }
    }

    syncShellWithHostState();
}

void InstrumentTrackController::setTimelineSampleRate(const double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || !std::isfinite(sampleRate))
    {
        return;
    }
    if (timelineSampleRate_ == sampleRate)
    {
        return;
    }
    timelineSampleRate_ = sampleRate;
    publishRenderSnapshot();
}

void InstrumentTrackController::recomputeLockedClipLengthFromPatternGrid(InstrumentMidiClip& clip) noexcept
{
    if (clip.pattern.usesTimelineNotes())
    {
        publishRenderSnapshot();
        return;
    }
    double sr = timelineSampleRate_;
    if (sr <= 0.0 || !std::isfinite(sr))
    {
        sr = 48000.0;
    }
    const std::int64_t len = experimentalPatternMusicalLengthSamples(clip.pattern, sr);
    if (len > 0)
    {
        clip.lengthSamples = len;
    }
    publishRenderSnapshot();
}

void InstrumentTrackController::notifyClipPatternMutated(const InstrumentMidiClipId clipId) noexcept
{
    if (InstrumentMidiClip* c = getClipById(clipId))
    {
        if (c->pattern.usesTimelineNotes())
        {
            publishRenderSnapshot();
        }
        else
        {
            recomputeLockedClipLengthFromPatternGrid(*c);
        }
    }
    else
    {
        publishRenderSnapshot();
    }
    sendChangeMessage();
}

void InstrumentTrackController::notifyClipExperimentalMusicalTimingChanged() noexcept
{
    double sr = timelineSampleRate_;
    if (sr <= 0.0 || !std::isfinite(sr))
    {
        sr = 48000.0;
    }
    for (auto& cp : clips_)
    {
        if (cp == nullptr || !cp->pattern.usesTimelineNotes())
        {
            continue;
        }
        const std::int64_t tlen = timelinePatternLengthSamples(cp->pattern, sr);
        if (tlen > 0)
        {
            cp->lengthSamples = tlen;
        }
    }
    publishRenderSnapshot();
    sendChangeMessage();
}

void InstrumentTrackController::publishRenderSnapshot()
{
    auto snap = std::make_shared<InstrumentTrackRenderSnapshot>();
    snap->revision = nextSnapshotRevision_++;
    snap->midiChannel = 1;
    double sr = timelineSampleRate_;
    if (sr <= 0.0 || !std::isfinite(sr))
    {
        sr = 48000.0;
    }
    snap->gateSamples = juce::jmax(1, (int)std::llround(0.001 * 100.0 * sr));
    snap->playbackEnabled = trackActive_ && powerOn_ && !muted_ && instrumentLoaded_ && host_.hasInstrument();

    for (const auto& cptr : clips_)
    {
        if (cptr == nullptr)
        {
            continue;
        }
        const InstrumentMidiClip& clip = *cptr;
        if (clip.lengthSamples <= 0)
        {
            continue;
        }
        InstrumentClipRenderPlan plan;
        plan.startSamples = clip.startSamples;
        plan.endSamplesExclusive = clip.startSamples + clip.lengthSamples;
        if (clip.pattern.usesTimelineNotes())
        {
            const double bpm = clip.pattern.bpm > 0.0 ? clip.pattern.bpm : 120.0;
            const int tpq = experimentalEffectiveTicksPerQuarter(clip.pattern);
            for (const auto& tn : clip.pattern.timelineNotes)
            {
                InstrumentNoteRenderEvent ev;
                ev.absSample = absoluteSampleForTimelineNote(clip.startSamples, tn, clip.pattern, sr);
                const std::int64_t durSam =
                    ticksToRelativeSamples(juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
                ev.noteOffAbsSample = ev.absSample + juce::jmax<std::int64_t>(1, durSam);
                ev.midiNote = (std::uint8_t)juce::jlimit(0, 127, tn.midiNote);
                ev.velocity = (std::uint8_t)juce::jlimit(1, 127, tn.velocity);
                ev.midiChannel = (std::uint8_t)juce::jlimit(1, 16, (int)tn.channel);
                if (ev.absSample < plan.startSamples || ev.absSample >= plan.endSamplesExclusive)
                {
                    continue;
                }
                plan.notes.push_back(ev);
            }
        }
        else
        {
            const int ns = juce::jmax(1, clip.pattern.numSteps);
            for (const auto& n : clip.pattern.notes)
            {
                if (n.step < 0 || n.step >= clip.pattern.numSteps)
                {
                    continue;
                }
                InstrumentNoteRenderEvent ev;
                ev.absSample = absoluteSampleForNoteInClip(clip.startSamples, n.step, ns, clip.lengthSamples);
                ev.noteOffAbsSample = 0;
                ev.midiNote = (std::uint8_t)juce::jlimit(0, 127, n.midiNote);
                ev.velocity = (std::uint8_t)juce::jlimit(1, 127, n.velocity);
                ev.midiChannel = 1;
                plan.notes.push_back(ev);
            }
        }
        std::sort(plan.notes.begin(), plan.notes.end(), [](const InstrumentNoteRenderEvent& a,
                                                           const InstrumentNoteRenderEvent& b) {
            return a.absSample < b.absSample;
        });
        snap->clips.push_back(std::move(plan));
    }
    std::sort(snap->clips.begin(), snap->clips.end(), [](const InstrumentClipRenderPlan& a,
                                                         const InstrumentClipRenderPlan& b) {
        return a.startSamples < b.startSamples;
    });
    std::atomic_store_explicit(&renderSnapshot_, std::shared_ptr<const InstrumentTrackRenderSnapshot>(std::move(snap)),
                               std::memory_order_release);
}

void InstrumentTrackController::audioThread_flushTransportMidi(ExperimentalInstrumentHost& host,
                                                               const int offsetInDevice,
                                                               const int deviceBlockNumSamples) noexcept
{
    if (deviceBlockNumSamples <= 0)
    {
        rtPendingOffCount_ = 0;
        rtLastSegEndTimeline_ = -1;
        return;
    }
    const int off0 = juce::jlimit(0, deviceBlockNumSamples - 1, offsetInDevice);
    for (int i = 0; i < rtPendingOffCount_; ++i)
    {
        const auto& p = rtPendingOffs_[(size_t)i];
        const int c = juce::jlimit(1, 16, p.midiChannel);
        host.audioThread_addMidiEventForCurrentBlock(
            off0, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
    }
    rtPendingOffCount_ = 0;
    rtLastSegEndTimeline_ = -1;
    for (int c = 1; c <= 16; ++c)
    {
        host.audioThread_addMidiEventForCurrentBlock(off0, juce::MidiMessage::allNotesOff(c));
    }
}

void InstrumentTrackController::audioThread_scheduleTransportMidiForSegment(
    ExperimentalInstrumentHost& host,
    const std::int64_t timelineSegStart,
    const int segNumSamples,
    const int bufferOffsetInDevice,
    const bool forceDiscontinuity,
    const int deviceBlockNumSamples) noexcept
{
    if (segNumSamples <= 0 || deviceBlockNumSamples <= 0)
    {
        return;
    }

    const auto snap = std::atomic_load_explicit(&renderSnapshot_, std::memory_order_acquire);
    if (snap == nullptr)
    {
        return;
    }

    const int off0 = juce::jlimit(0, deviceBlockNumSamples - 1, bufferOffsetInDevice);
    const std::int64_t segEnd = timelineSegStart + static_cast<std::int64_t>(segNumSamples);

    const bool revBump = (snap->revision != rtLastSnapshotRevision_);
    if (revBump)
    {
        rtLastSnapshotRevision_ = snap->revision;
    }

    const bool gap = (rtLastSegEndTimeline_ >= 0 && timelineSegStart != rtLastSegEndTimeline_);
    const bool discontinuity = forceDiscontinuity || revBump || gap;

    if (discontinuity)
    {
        for (int i = 0; i < rtPendingOffCount_; ++i)
        {
            const auto& p = rtPendingOffs_[(size_t)i];
            const int c = juce::jlimit(1, 16, p.midiChannel);
            host.audioThread_addMidiEventForCurrentBlock(
                off0, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
        }
        rtPendingOffCount_ = 0;
    }
    else
    {
        int w = 0;
        for (int i = 0; i < rtPendingOffCount_; ++i)
        {
            const PendingTransportNoteOff p = rtPendingOffs_[(size_t)i];
            const int c = juce::jlimit(1, 16, p.midiChannel);
            if (p.dueAbsSample >= segEnd)
            {
                rtPendingOffs_[(size_t)w++] = p;
                continue;
            }
            if (p.dueAbsSample >= timelineSegStart && p.dueAbsSample < segEnd)
            {
                const int rel = static_cast<int>(p.dueAbsSample - timelineSegStart);
                const int o = juce::jlimit(0, deviceBlockNumSamples - 1, rel + bufferOffsetInDevice);
                host.audioThread_addMidiEventForCurrentBlock(
                    o, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
                continue;
            }
            host.audioThread_addMidiEventForCurrentBlock(
                off0, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
        }
        rtPendingOffCount_ = w;
    }

    rtLastSegEndTimeline_ = segEnd;

    if (!snap->playbackEnabled)
    {
        return;
    }

    const int gate = juce::jmax(1, snap->gateSamples);

    for (const auto& plan : snap->clips)
    {
        if (plan.endSamplesExclusive <= timelineSegStart || plan.startSamples >= segEnd)
        {
            continue;
        }

        auto it = std::lower_bound(plan.notes.begin(), plan.notes.end(), timelineSegStart,
                                   [](const InstrumentNoteRenderEvent& e, const std::int64_t s) {
                                       return e.absSample < s;
                                   });

        for (; it != plan.notes.end() && it->absSample < segEnd; ++it)
        {
            const InstrumentNoteRenderEvent& ev = *it;
            if (ev.absSample < timelineSegStart)
            {
                continue;
            }

            const int onRel = static_cast<int>(ev.absSample - timelineSegStart);
            const int onOffset = juce::jlimit(0, deviceBlockNumSamples - 1, onRel + bufferOffsetInDevice);
            const float vel = static_cast<float>(ev.velocity) / 127.0f;
            const int noteCh = juce::jlimit(1, 16, (int)ev.midiChannel);
            host.audioThread_addMidiEventForCurrentBlock(
                onOffset, juce::MidiMessage::noteOn(noteCh, (int)ev.midiNote, vel));

            const std::int64_t dueAbs = (ev.noteOffAbsSample > ev.absSample)
                                            ? ev.noteOffAbsSample
                                            : (ev.absSample + static_cast<std::int64_t>(gate));
            if (dueAbs >= timelineSegStart && dueAbs < segEnd)
            {
                const int offRel = static_cast<int>(dueAbs - timelineSegStart);
                const int o = juce::jlimit(0, deviceBlockNumSamples - 1, offRel + bufferOffsetInDevice);
                host.audioThread_addMidiEventForCurrentBlock(
                    o, juce::MidiMessage::noteOff(noteCh, (int)ev.midiNote, 0.0f));
            }
            else if (dueAbs >= segEnd)
            {
                if (rtPendingOffCount_ < kMaxPendingTransportOffs)
                {
                    rtPendingOffs_[(size_t)rtPendingOffCount_++] = { dueAbs, (int)ev.midiNote, noteCh };
                }
                else
                {
                    host.audioThread_addMidiEventForCurrentBlock(
                        juce::jmax(0, deviceBlockNumSamples - 1),
                        juce::MidiMessage::noteOff(noteCh, (int)ev.midiNote, 0.0f));
                }
            }
        }
    }
}
