#include "instruments/InstrumentTrackController.h"

#include "diagnostics/ExperimentalPlaybackRoutingLog.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/Vst3ChildProcessScan.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

[[nodiscard]] static const ProjectFileExperimentalInstrumentTrackV1* selectedEnabledGrooveAgentPayload(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept
{
    for (const auto& t : tracks)
    {
        if (!t.enabled || t.instrumentKind != "GrooveAgentSE")
        {
            continue;
        }
        return &t;
    }
    return nullptr;
}

[[nodiscard]] static TrackId resolveExperimentalInstrumentBindLaneId(
    Session* sessionMaybe,
    const TrackId dtoTrackField,
    const std::vector<ProjectFileTrackV1>* persistedSerializedTracksMaybe) noexcept
{
    std::shared_ptr<const SessionSnapshot> snap;
    if (sessionMaybe != nullptr)
    {
        snap = sessionMaybe->loadSessionSnapshotForAudioThread();
    }

    TrackId bindId = dtoTrackField;
    if (bindId == static_cast<TrackId>(0))
    {
        bindId = kInvalidTrackId;
    }

    if (snap != nullptr && bindId != kInvalidTrackId)
    {
        const int ixSnap = snap->findTrackIndexById(bindId);
        if (ixSnap < 0 || snap->getTrack(ixSnap).getKind() != TrackKind::Instrument)
        {
            bindId = kInvalidTrackId;
        }
    }

    if (bindId == kInvalidTrackId && snap != nullptr)
    {
        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            const Track& tr = snap->getTrack(ti);
            if (tr.getKind() == TrackKind::Instrument)
            {
                bindId = tr.getId();
                juce::Logger::writeToLog(
                    "[InstrumentTrackController] Bound experimental payload to Instrument lane id="
                    + juce::String((juce::int64)bindId));
                break;
            }
        }
    }

    if (bindId == kInvalidTrackId && persistedSerializedTracksMaybe != nullptr)
    {
        const TrackId rawDto
            = (dtoTrackField == static_cast<TrackId>(0)) ? kInvalidTrackId : dtoTrackField;
        if (rawDto != kInvalidTrackId)
        {
            for (const auto& trDto : *persistedSerializedTracksMaybe)
            {
                if (trDto.kind.equalsIgnoreCase("instrument") && trDto.id == rawDto)
                {
                    bindId = trDto.id;
                    break;
                }
            }
        }
        if (bindId == kInvalidTrackId)
        {
            for (const auto& trDto : *persistedSerializedTracksMaybe)
            {
                if (trDto.kind.equalsIgnoreCase("instrument"))
                {
                    bindId = trDto.id;
                    break;
                }
            }
        }
    }

    return bindId;
}

TrackId InstrumentTrackController::resolveExperimentalInstrumentLaneIdFromProjectFields(
    Session* sessionNullable,
    const TrackId dtoTrackField,
    const std::vector<ProjectFileTrackV1>* persistedSerializedTracksMaybe) noexcept
{
    return resolveExperimentalInstrumentBindLaneId(sessionNullable, dtoTrackField, persistedSerializedTracksMaybe);
}

InstrumentTrackController::InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept
    : host_(host)
{
    publishRenderSnapshot();
}

bool InstrumentTrackController::computeInstrumentLoadedFromHost() const noexcept
{
    // Host-report name formatting varies (“Groove Agent …”, “GrooveAgent SE”, …). Treat any GrooveAgent
    // substring as loaded so UI subtitles and snapshots stay coherent with `hasInstrument()`.
    if (!host_.hasInstrument())
    {
        return false;
    }
    const juce::String n = host_.getInstrumentNameForUi();
    return n.containsIgnoreCase("Groove Agent") || n.containsIgnoreCase("GrooveAgent");
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

bool InstrumentTrackController::bootstrapGrooveAgentShellForSessionTrack(const TrackId sessionInstrumentTrackId) noexcept
{
    if (sessionInstrumentTrackId == kInvalidTrackId)
    {
        return false;
    }
    if (trackActive_ && experimentalDomainTrackId_ != sessionInstrumentTrackId)
    {
        return false;
    }
    if (session_ == nullptr)
    {
        return false;
    }
    if (const auto snap = session_->loadSessionSnapshotForAudioThread())
    {
        const int ix = snap->findTrackIndexById(sessionInstrumentTrackId);
        if (ix < 0 || snap->getTrack(ix).getKind() != TrackKind::Instrument)
        {
            return false;
        }
    }

    experimentalDomainTrackId_ = sessionInstrumentTrackId;

    trackActive_ = true;
    powerOn_ = true;
    muted_ = false;
    isActive_ = false;
    requiredKitName_ = "FiftySixDegreesModified";
    pendingProjectGrooveAutoload_ = false;
    pendingAdvisoryPluginBundlePath_.clear();
    pendingInstrumentKind_.clear();
    instrumentLoaded_ = computeInstrumentLoadedFromHost();

    publishRenderSnapshot();
    sendChangeMessage();
    return true;
}

bool InstrumentTrackController::tryAddGrooveAgentInstrumentTrackShell()
{
    if (session_ == nullptr)
    {
        return false;
    }
    if (trackActive_)
    {
        return false;
    }

    const std::optional<TrackId> newId
        = session_->appendExperimentalInstrumentShellTrack(juce::String("Groove Agent SE"));
    return newId.has_value() && bootstrapGrooveAgentShellForSessionTrack(*newId);
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

InstrumentMidiClipId InstrumentTrackController::getSelectedClipId() const noexcept
{
    return selectedClipIds_.empty() ? InstrumentMidiClipId{ 0 } : selectedClipIds_.back();
}

bool InstrumentTrackController::isClipSelected(const InstrumentMidiClipId id) const noexcept
{
    if (id == 0)
    {
        return false;
    }
    return std::find(selectedClipIds_.begin(), selectedClipIds_.end(), id) != selectedClipIds_.end();
}

void InstrumentTrackController::clearClipSelection() noexcept
{
    if (selectedClipIds_.empty())
    {
        return;
    }
    selectedClipIds_.clear();
    sendChangeMessage();
}

void InstrumentTrackController::setSelectedClipIdsExclusive(const InstrumentMidiClipId activeClipId) noexcept
{
    if (activeClipId == 0)
    {
        clearClipSelection();
        return;
    }
    if (selectedClipIds_.size() == 1 && selectedClipIds_.front() == activeClipId)
    {
        return;
    }
    selectedClipIds_.clear();
    selectedClipIds_.push_back(activeClipId);
    sendChangeMessage();
}

void InstrumentTrackController::addClipToSelection(const InstrumentMidiClipId id) noexcept
{
    if (id == 0)
    {
        return;
    }
    const auto it = std::find(selectedClipIds_.begin(), selectedClipIds_.end(), id);
    if (it == selectedClipIds_.end())
    {
        selectedClipIds_.push_back(id);
        sendChangeMessage();
        return;
    }
    if (it != selectedClipIds_.end() - 1)
    {
        selectedClipIds_.erase(it);
        selectedClipIds_.push_back(id);
        sendChangeMessage();
    }
}

void InstrumentTrackController::toggleClipSelection(const InstrumentMidiClipId id) noexcept
{
    if (id == 0)
    {
        return;
    }
    const auto it = std::find(selectedClipIds_.begin(), selectedClipIds_.end(), id);
    if (it != selectedClipIds_.end())
    {
        selectedClipIds_.erase(it);
        sendChangeMessage();
        return;
    }
    selectedClipIds_.push_back(id);
    sendChangeMessage();
}

void InstrumentTrackController::setActiveSelectedClipId(const InstrumentMidiClipId id) noexcept
{
    if (id == 0)
    {
        return;
    }
    const auto it = std::find(selectedClipIds_.begin(), selectedClipIds_.end(), id);
    jassert(it != selectedClipIds_.end());
    if (it == selectedClipIds_.end())
    {
        return;
    }
    if (it != selectedClipIds_.end() - 1)
    {
        selectedClipIds_.erase(it);
        selectedClipIds_.push_back(id);
        sendChangeMessage();
    }
}

void InstrumentTrackController::setSelectedClipId(const InstrumentMidiClipId id) noexcept
{
    setSelectedClipIdsExclusive(id);
}

void InstrumentTrackController::pruneInstrumentMidiClipSelectionToExistingClips() noexcept
{
    selectedClipIds_.erase(
        std::remove_if(
            selectedClipIds_.begin(),
            selectedClipIds_.end(),
            [this](const InstrumentMidiClipId cid) { return getClipById(cid) == nullptr; }),
        selectedClipIds_.end());
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

void InstrumentTrackController::setPowerOn(const bool on) noexcept
{
    if (powerOn_ == on)
    {
        return;
    }
    powerOn_ = on;
    if (session_ != nullptr && experimentalDomainTrackId_ != kInvalidTrackId)
    {
        session_->setTrackOff(experimentalDomainTrackId_, !on);
    }
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
    if (session_ != nullptr && experimentalDomainTrackId_ != kInvalidTrackId)
    {
        session_->setTrackMuted(experimentalDomainTrackId_, muted);
    }
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
    if (clip == nullptr)
    {
        const InstrumentMidiClipId active = getSelectedClipId();
        if (active != 0)
        {
            clip = getClipById(active);
        }
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
    selectedClipIds_.clear();
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
    experimentalDomainTrackId_ = kInvalidTrackId;
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
    dto.trackId = experimentalDomainTrackId_;
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
        if (!cptr->pattern.timelineNotes.empty())
        {
            c.timelineAnchorSamples.emplace(cptr->timelineAnchorSamples);
        }
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
        const bool unspecifiedTrackBinding = (t.trackId == static_cast<TrackId>(0));
        if (!unspecifiedTrackBinding && t.trackId != experimentalDomainTrackId_)
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
        clip->timelineAnchorSamples = cdto.timelineAnchorSamples.value_or(cdto.startSamples);
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
            if (tlen > 0 && cp->lengthSamples <= 0)
            {
                cp->lengthSamples = tlen;
            }
        }
        else if (cp->lengthSamples <= 0)
        {
            recomputeLockedClipLengthFromPatternGrid(*cp);
        }
    }
    nextClipId_ = maxId + 1;
    pruneInstrumentMidiClipSelectionToExistingClips();
    instrumentLoaded_ = computeInstrumentLoadedFromHost();
    publishRenderSnapshot();
    sendChangeMessage();
}

void InstrumentTrackController::restoreExperimentalInstrumentFromProject(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks,
    const std::vector<ProjectFileTrackV1>* persistedSerializedTrackRows)
{
    const ProjectFileExperimentalInstrumentTrackV1* chosen = selectedEnabledGrooveAgentPayload(tracks);
    if (chosen == nullptr)
    {
        clearExperimentalInstrumentStateForProjectLoad();
        sendChangeMessage();
        return;
    }
    restoreExperimentalInstrumentSingleProjectRow(*chosen, persistedSerializedTrackRows);
}

void InstrumentTrackController::restoreExperimentalInstrumentSingleProjectRow(
    const ProjectFileExperimentalInstrumentTrackV1& chosen,
    const std::vector<ProjectFileTrackV1>* persistedSerializedTrackRows)
{
    clearExperimentalInstrumentStateForProjectLoad();

    experimentalDomainTrackId_
        = resolveExperimentalInstrumentBindLaneId(session_, chosen.trackId, persistedSerializedTrackRows);

    std::shared_ptr<const SessionSnapshot> snap;
    if (session_ != nullptr)
    {
        snap = session_->loadSessionSnapshotForAudioThread();
    }

    trackActive_ = true;
    bool power = chosen.powerOn;
    bool mute = chosen.muted;
    if (snap != nullptr && experimentalDomainTrackId_ != kInvalidTrackId)
    {
        const int ix = snap->findTrackIndexById(experimentalDomainTrackId_);
        if (ix >= 0)
        {
            const Track& tr = snap->getTrack(ix);
            power = !tr.isTrackOff();
            mute = tr.isMuted();
        }
    }
    powerOn_ = power;
    muted_ = mute;
    isActive_ = false;
    requiredKitName_
        = chosen.requiredKitName.isNotEmpty() ? chosen.requiredKitName : juce::String("FiftySixDegreesModified");
    pendingProjectGrooveAutoload_ = chosen.pluginWasLoadedOnSave && chosen.instrumentKind == "GrooveAgentSE";
    pendingAdvisoryPluginBundlePath_ = chosen.pluginBundlePath;
    pendingInstrumentKind_ = chosen.instrumentKind;
    pendingPluginStateBase64_ = chosen.pluginStateBase64;

    drumLabels_.clear();
    for (const auto& kv : chosen.drumNoteNameOverrides)
    {
        if (kv.first >= 0 && kv.first <= 127 && kv.second.isNotEmpty())
        {
            drumLabels_[kv.first].manual = kv.second;
        }
    }
    for (const auto& kv : chosen.drumNoteNameAutoPlugin)
    {
        if (kv.first >= 0 && kv.first <= 127 && kv.second.isNotEmpty())
        {
            drumLabels_[kv.first].autoPlugin = kv.second;
        }
    }

    InstrumentMidiClipId maxId = 0;
    for (const auto& cdto : chosen.clips)
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
        clip->timelineAnchorSamples = cdto.timelineAnchorSamples.value_or(cdto.startSamples);
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
            if (tlen > 0 && cp->lengthSamples <= 0)
            {
                cp->lengthSamples = tlen;
            }
        }
        else if (cp->lengthSamples <= 0)
        {
            recomputeLockedClipLengthFromPatternGrid(*cp);
        }
    }
    nextClipId_ = maxId + 1;
    selectedClipIds_.clear();
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
        cp->startSamples = juce::jmax(std::int64_t{ 0 }, cp->startSamples);

        const std::int64_t tlen = timelinePatternLengthSamples(cp->pattern, sr);
        if (tlen <= 0)
        {
            continue;
        }
        const std::int64_t naturalEndEx = cp->timelineAnchorSamples + tlen;
        const std::int64_t maxLen = naturalEndEx - cp->startSamples;
        if (maxLen < 1)
        {
            // Visible start has moved past the pattern end; pull it back without moving `timelineAnchorSamples`.
            cp->startSamples = juce::jmax(std::int64_t{ 0 }, naturalEndEx - 1);
        }
        cp->lengthSamples = juce::jmax(std::int64_t{ 1 }, cp->lengthSamples);
    }
    publishRenderSnapshot();
    sendChangeMessage();
}

InstrumentMidiClipId InstrumentTrackController::appendImportedTimelineMidiClipAtSamples(
    std::vector<TimelineMidiNote> timelineNotes,
    const double firstTempoBpmFromFile,
    const std::int64_t startSamples,
    juce::String suggestedName)
{
    if (!trackActive_)
    {
        return 0;
    }

    auto clip = std::make_unique<InstrumentMidiClip>();
    clip->id = nextClipId_++;
    const juce::String trimmedSuggested = suggestedName.trim();
    if (trimmedSuggested.isNotEmpty())
    {
        clip->name = trimmedSuggested;
    }
    else
    {
        clip->name = juce::String("MIDI ") + juce::String(clip->id);
    }

    clip->startSamples = juce::jmax(std::int64_t{0}, startSamples);

    clip->timelineAnchorSamples = clip->startSamples;

    clip->pattern.notes.clear();
    clip->pattern.timelineNotes = std::move(timelineNotes);
    clip->pattern.ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
    clip->pattern.numSteps = 16;
    clip->pattern.stepDenom = 16;
    clip->pattern.loop = true;
    if (firstTempoBpmFromFile > 0.0 && std::isfinite(firstTempoBpmFromFile))
    {
        clip->pattern.bpm = firstTempoBpmFromFile;
    }
    else
    {
        clip->pattern.bpm = 110.0;
    }

    clip->laneStartFractionPermille = 0;
    clip->laneEndFractionPermille = 250;

    double sr = timelineSampleRate_;
    if (sr <= 0.0 || !std::isfinite(sr))
    {
        sr = 48000.0;
    }

    if (clip->pattern.usesTimelineNotes())
    {
        const std::int64_t tlen = timelinePatternLengthSamples(clip->pattern, sr);
        if (tlen > 0)
        {
            clip->lengthSamples = tlen;
        }
    }

    if (clip->lengthSamples <= 0)
    {
        recomputeLockedClipLengthFromPatternGrid(*clip);
    }

    clip->midiRollVisibleStartSamples = 0;
    clip->midiRollSamplesPerPixel = 0.0;
    clip->midiRollFollowEnabled = false;

    const InstrumentMidiClipId outId = clip->id;
    clips_.push_back(std::move(clip));

    publishRenderSnapshot();
    sendChangeMessage();
    return outId;
}

bool InstrumentTrackController::applyInstrumentMidiClipVisibleTrim(const InstrumentMidiClipId id,
                                                                   const std::int64_t newVisibleStartSamples,
                                                                   const std::int64_t newVisibleLengthSamples) noexcept
{
    InstrumentMidiClip* const c = getClipById(id);
    if (c == nullptr || !c->pattern.usesTimelineNotes())
    {
        return false;
    }

#if !defined(NDEBUG)
    const std::int64_t anchorTrimInvariant = c->timelineAnchorSamples;
#endif

    constexpr std::int64_t kMinVisibleSpanSamples = 1;
    const std::int64_t ns = juce::jmax(std::int64_t{ 0 }, newVisibleStartSamples);
    const std::int64_t nl = juce::jmax(kMinVisibleSpanSamples, newVisibleLengthSamples);

    if (ns == c->startSamples && nl == c->lengthSamples)
    {
        return false;
    }

    c->startSamples = ns;
    c->lengthSamples = nl;
#if !defined(NDEBUG)
    jassert(c->timelineAnchorSamples == anchorTrimInvariant);
#endif
    publishRenderSnapshot();
    sendChangeMessage();
    return true;
}

std::int64_t InstrumentTrackController::clampInstrumentMidiClipMoveDeltaForCurrentSelection(
    const std::int64_t deltaSamples) const noexcept
{
    if (deltaSamples == 0 || selectedClipIds_.empty())
    {
        return 0;
    }

    std::int64_t minStart = std::numeric_limits<std::int64_t>::max();
    for (const InstrumentMidiClipId id : selectedClipIds_)
    {
        const InstrumentMidiClip* const c = getClipById(id);
        if (c != nullptr)
        {
            minStart = juce::jmin(minStart, c->startSamples);
        }
    }

    if (minStart == std::numeric_limits<std::int64_t>::max())
    {
        return 0;
    }

    return juce::jmax(deltaSamples, -minStart);
}

bool InstrumentTrackController::moveSelectedInstrumentMidiClipsByDeltaSamples(const std::int64_t deltaSamples) noexcept
{
    const std::int64_t d = clampInstrumentMidiClipMoveDeltaForCurrentSelection(deltaSamples);
    if (d == 0)
    {
        return false;
    }

    bool any = false;
    for (const InstrumentMidiClipId id : selectedClipIds_)
    {
        InstrumentMidiClip* const c = getClipById(id);
        if (c == nullptr)
        {
            continue;
        }
        const std::int64_t ns = c->startSamples + d;
        if (ns != c->startSamples)
        {
            c->startSamples = ns;
            c->timelineAnchorSamples += d;
            any = true;
        }
    }

    if (!any)
    {
        return false;
    }

    publishRenderSnapshot();
    sendChangeMessage();
    return true;
}

bool InstrumentTrackController::removeInstrumentMidiClipsByIds(const std::vector<InstrumentMidiClipId>& ids) noexcept
{
    if (!trackActive_ || ids.empty())
    {
        return false;
    }
    std::unordered_set<InstrumentMidiClipId> kill(ids.begin(), ids.end());
    const auto beforeSize = clips_.size();
    clips_.erase(
        std::remove_if(
            clips_.begin(),
            clips_.end(),
            [&](const std::unique_ptr<InstrumentMidiClip>& p) {
                return p != nullptr && kill.count(p->id) > 0;
            }),
        clips_.end());
    if (clips_.size() == beforeSize)
    {
        return false;
    }
    pruneInstrumentMidiClipSelectionToExistingClips();
    publishRenderSnapshot();
    sendChangeMessage();
    return true;
}

std::vector<InstrumentMidiClipId> InstrumentTrackController::appendDeepCopiedInstrumentMidiClips(
    const std::vector<InstrumentMidiClip>& snapshotsInOrder,
    const std::vector<std::pair<std::int64_t, std::int64_t>>& startAndAnchorsInOrder) noexcept
{
    if (!trackActive_ || snapshotsInOrder.size() != startAndAnchorsInOrder.size()
        || snapshotsInOrder.empty())
    {
        return {};
    }
    std::vector<InstrumentMidiClipId> out;
    out.reserve(snapshotsInOrder.size());
    for (std::size_t i = 0; i < snapshotsInOrder.size(); ++i)
    {
        const InstrumentMidiClip& src = snapshotsInOrder[i];
        const std::int64_t newStart = startAndAnchorsInOrder[i].first;
        const std::int64_t newAnchor = startAndAnchorsInOrder[i].second;
        auto clip = std::make_unique<InstrumentMidiClip>(src);
        clip->id = nextClipId_++;
        clip->startSamples = juce::jmax(std::int64_t{ 0 }, newStart);
        clip->timelineAnchorSamples = newAnchor;
        clip->midiRollVisibleStartSamples = 0;
        clip->midiRollSamplesPerPixel = 0.0;
        clip->midiRollFollowEnabled = false;
        const InstrumentMidiClipId nid = clip->id;
        clips_.push_back(std::move(clip));
        out.push_back(nid);
    }
    publishRenderSnapshot();
    sendChangeMessage();
    return out;
}

void InstrumentTrackController::replaceInstrumentMidiClipSelectionOrdered(
    std::vector<InstrumentMidiClipId> orderedIds) noexcept
{
    orderedIds.erase(
        std::remove_if(
            orderedIds.begin(),
            orderedIds.end(),
            [](const InstrumentMidiClipId id) noexcept { return id == 0; }),
        orderedIds.end());
    if (orderedIds.empty())
    {
        clearClipSelection();
        return;
    }
    selectedClipIds_ = std::move(orderedIds);
    pruneInstrumentMidiClipSelectionToExistingClips();
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
    // Transport MIDI must follow **host readiness** (`layoutOk` instrument). `instrumentLoaded_` mirrored
    // that via a Groove-shaped name heuristic and could stay false while the plug-in actually processed
    // audio — starving `audioThread_scheduleTransportMidiForSegment` even though clips paint from `clips_`.
    snap->playbackEnabled = trackActive_ && powerOn_ && !muted_ && host_.hasInstrument();

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
                ev.absSample = absoluteSampleForTimelineNote(clip.timelineAnchorSamples, tn, clip.pattern, sr);
                const std::int64_t durSam =
                    ticksToRelativeSamples(juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
                ev.noteOffAbsSample = ev.absSample + juce::jmax<std::int64_t>(1, durSam);
                ev.noteOffAbsSample = juce::jmin(ev.noteOffAbsSample, plan.endSamplesExclusive);
                if (ev.noteOffAbsSample <= ev.absSample)
                {
                    ev.noteOffAbsSample = ev.absSample + 1;
                }
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

    std::int64_t arrangeMinSample = 0;
    std::int64_t arrangeMaxExclusive = -1;
    int routedNoteRows = 0;
    if (!snap->clips.empty())
    {
        arrangeMinSample = std::numeric_limits<std::int64_t>::max();
        arrangeMaxExclusive = std::numeric_limits<std::int64_t>::min();
        for (const auto& pl : snap->clips)
        {
            arrangeMinSample = juce::jmin(arrangeMinSample, pl.startSamples);
            arrangeMaxExclusive = juce::jmax(arrangeMaxExclusive, pl.endSamplesExclusive);
            routedNoteRows += static_cast<int>(pl.notes.size());
        }
    }

    juce::String routingRenderFp;
    routingRenderFp << juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(experimentalDomainTrackId_)))
                    << '|' << juce::String(trackActive_ ? 1 : 0) << '|' << juce::String(powerOn_ ? 1 : 0)
                    << '|' << juce::String(muted_ ? 1 : 0) << '|' << juce::String(instrumentLoaded_ ? 1 : 0) << '|'
                    << juce::String(host_.hasInstrument() ? 1 : 0) << '|'
                    << juce::String(snap->playbackEnabled ? 1 : 0) << '|'
                    << juce::String(static_cast<int>(snap->clips.size()));

    routingRenderFp << '|' << juce::String(static_cast<juce::int64>(
                              static_cast<std::int64_t>(arrangeMinSample)))
                    << '|' << juce::String(static_cast<juce::int64>(
                               static_cast<std::int64_t>(arrangeMaxExclusive)))
                    << '|' << juce::String(routedNoteRows);

    if (routingRenderFp != lastExperimentalPlaybackRoutingRenderFingerprint_)
    {
        lastExperimentalPlaybackRoutingRenderFingerprint_ = routingRenderFp;
#if MINIDAW_DIAG_PLAYBACK_ROUTING
        const juce::String arrangeSpan
            = snap->clips.empty()
                  ? juce::String("plansEmpty=1")
                  : juce::String("timelineRange=[")
                        + juce::String(static_cast<juce::int64>(std::int64_t(arrangeMinSample)))
                        + juce::String(",")
                        + juce::String(static_cast<juce::int64>(std::int64_t(arrangeMaxExclusive)))
                        + juce::String(")");

        appendExperimentalPlaybackRoutingLogLine(
            juce::String("instrument-render-snapshot: tid=")
            + juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(experimentalDomainTrackId_)))
            + juce::String(" active=") + juce::String(trackActive_ ? "yes" : "no")
            + juce::String(" powerOn=") + juce::String(powerOn_ ? "yes" : "no")
            + juce::String(" muted=") + juce::String(muted_ ? "yes" : "no")
            + juce::String(" instrumentLoaded=") + juce::String(instrumentLoaded_ ? "yes" : "no")
            + juce::String(" hostHasInstrument=") + juce::String(host_.hasInstrument() ? "yes" : "no")
            + juce::String(" playbackEnabled=") + juce::String(snap->playbackEnabled ? "yes" : "no")
            + juce::String(" clipPlans=") + juce::String(static_cast<int>(snap->clips.size()))
            + juce::String(" noteEventsTotal=") + juce::String(routedNoteRows) + juce::String(" ") + arrangeSpan);
#endif
    }

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
    const int deviceBlockNumSamples,
    int* outMidiEventsEmitted) noexcept
{
    if (segNumSamples <= 0 || deviceBlockNumSamples <= 0)
    {
        return;
    }

    const auto emitCounted = [&](const int offset, const juce::MidiMessage& message) noexcept {
        host.audioThread_addMidiEventForCurrentBlock(offset, message);
        if (outMidiEventsEmitted != nullptr)
        {
            ++(*outMidiEventsEmitted);
        }
    };

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
            emitCounted(off0, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
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
                emitCounted(o, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
                continue;
            }
            emitCounted(off0, juce::MidiMessage::noteOff(c, p.midiNote, 0.0f));
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
            emitCounted(onOffset, juce::MidiMessage::noteOn(noteCh, (int)ev.midiNote, vel));

            const std::int64_t dueAbs = (ev.noteOffAbsSample > ev.absSample)
                                            ? ev.noteOffAbsSample
                                            : (ev.absSample + static_cast<std::int64_t>(gate));
            if (dueAbs >= timelineSegStart && dueAbs < segEnd)
            {
                const int offRel = static_cast<int>(dueAbs - timelineSegStart);
                const int o = juce::jlimit(0, deviceBlockNumSamples - 1, offRel + bufferOffsetInDevice);
                emitCounted(o, juce::MidiMessage::noteOff(noteCh, (int)ev.midiNote, 0.0f));
            }
            else if (dueAbs >= segEnd)
            {
                if (rtPendingOffCount_ < kMaxPendingTransportOffs)
                {
                    rtPendingOffs_[(size_t)rtPendingOffCount_++] = { dueAbs, (int)ev.midiNote, noteCh };
                }
                else
                {
                    emitCounted(juce::jmax(0, deviceBlockNumSamples - 1),
                                juce::MidiMessage::noteOff(noteCh, (int)ev.midiNote, 0.0f));
                }
            }
        }
    }
}

bool InstrumentTrackController::serializedProjectUsesEnabledGrooveAgentRow(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept
{
    return selectedEnabledGrooveAgentPayload(tracks) != nullptr;
}

TrackId InstrumentTrackController::peekExperimentalInstrumentBindLaneId(
    Session* sessionNullable,
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& payloads,
    const std::vector<ProjectFileTrackV1>& persistedTracks) noexcept
{
    const ProjectFileExperimentalInstrumentTrackV1* chosen = selectedEnabledGrooveAgentPayload(payloads);
    if (chosen == nullptr)
    {
        return kInvalidTrackId;
    }
    return resolveExperimentalInstrumentBindLaneId(sessionNullable, chosen->trackId, &persistedTracks);
}
