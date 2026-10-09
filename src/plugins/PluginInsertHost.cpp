// =============================================================================
// PluginInsertHost.cpp — VST3 instances, atomic processor map, scratch for the audio thread
// =============================================================================

#include "plugins/PluginInsertHost.h"

#include "diagnostics/AudioThreadProfiler.h"
#include "plugins/PluginEditorWindows.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    [[nodiscard]] juce::PluginDescription pickPrimaryDescription(const juce::File& vst3File,
                                                                 juce::AudioPluginFormatManager& fm,
                                                                 juce::String& err)
    {
        err.clear();
        const juce::String pathOrId = vst3File.getFullPathName();
        juce::OwnedArray<juce::PluginDescription> list;
        for (int i = 0; i < fm.getNumFormats(); ++i)
        {
            juce::AudioPluginFormat* const f = fm.getFormat(i);
            if (f != nullptr && f->fileMightContainThisPluginType(pathOrId))
            {
                f->findAllTypesForFile(list, pathOrId);
            }
        }
        if (list.isEmpty())
        {
            err = "No plugin types found in file (not a VST3 or scan failed).";
            return {};
        }
        return *list.getFirst();
    }

    /// JUCE identifier strings are `<format>-<name>-<hex(bundle path hash)>-<hex(uid)>`. Plugin
    /// identity for restore purposes is format + name + uid; the path hash only says where the
    /// bundle lived when saved, so a relocated copy of the same plugin still matches.
    [[nodiscard]] bool descriptionMatchesSavedIdentity(const juce::PluginDescription& pd,
                                                       const juce::String& savedIdentifier) noexcept
    {
        if (savedIdentifier.isEmpty())
        {
            return true;
        }
        const juce::String prefix = pd.pluginFormatName + "-" + pd.name + "-";
        if (!savedIdentifier.startsWith(prefix))
        {
            return false;
        }
        const juce::String savedUidHex = savedIdentifier.fromLastOccurrenceOf("-", false, false);
        if (savedUidHex.isEmpty())
        {
            return false;
        }
        if (savedUidHex.equalsIgnoreCase(juce::String::toHexString(pd.uniqueId)))
        {
            return true;
        }
        return pd.deprecatedUid != 0
               && savedUidHex.equalsIgnoreCase(juce::String::toHexString(pd.deprecatedUid));
    }

    /// Human-readable name for an unavailable placeholder: the `<name>` part of the saved JUCE
    /// identifier string, else the bundle file stem.
    [[nodiscard]] juce::String unavailableInsertDisplayName(const PluginInsertDescriptor& d)
    {
        juce::String name;
        const juce::String id = d.pluginIdentifier;
        // "<format>-<name>-<hash>-<uid>": drop the format prefix and the two trailing segments.
        const int firstDash = id.indexOfChar('-');
        const int lastDash = id.lastIndexOfChar('-');
        if (firstDash >= 0 && lastDash > firstDash)
        {
            const int secondLastDash = id.substring(0, lastDash).lastIndexOfChar('-');
            if (secondLastDash > firstDash)
            {
                name = id.substring(firstDash + 1, secondLastDash).trim();
            }
        }
        if (name.isEmpty())
        {
            name = juce::File::createFileWithoutCheckingPath(d.vst3AbsolutePath).getFileNameWithoutExtension();
        }
        return name.isEmpty() ? juce::String("VST3 insert") : name;
    }

    [[nodiscard]] double effectiveSr(const double sr) noexcept
    {
        return sr > 0.0 ? sr : 48000.0;
    }

    [[nodiscard]] int effectiveBs(const int bs) noexcept
    {
        return bs > 0 ? bs : 512;
    }

    void eraseLastHostAppliedStateForTrack(std::map<std::pair<TrackId, InsertSlotId>, juce::MemoryBlock>& map,
                                          const TrackId trackId)
    {
        for (auto it = map.begin(); it != map.end();)
        {
            if (it->first.first == trackId)
                it = map.erase(it);
            else
                ++it;
        }
    }

} // namespace

juce::PluginDescription PluginInsertHost::pickDescriptionForSavedIdentity(const juce::File& vst3File,
                                                                         juce::AudioPluginFormatManager& fm,
                                                                         const juce::String& savedIdentifier,
                                                                         juce::String& err)
{
    err.clear();
    const juce::String pathOrId = vst3File.getFullPathName();
    juce::OwnedArray<juce::PluginDescription> list;
    for (int i = 0; i < fm.getNumFormats(); ++i)
    {
        juce::AudioPluginFormat* const f = fm.getFormat(i);
        if (f != nullptr && f->fileMightContainThisPluginType(pathOrId))
        {
            f->findAllTypesForFile(list, pathOrId);
        }
    }
    if (list.isEmpty())
    {
        err = "No plugin types found in file (not a VST3 or scan failed).";
        return {};
    }
    if (savedIdentifier.isEmpty())
    {
        return *list.getFirst();
    }
    for (const juce::PluginDescription* pd : list)
    {
        if (pd != nullptr && descriptionMatchesSavedIdentity(*pd, savedIdentifier))
        {
            return *pd;
        }
    }
    err = "The plugin found at the saved path is not the saved plugin (" + savedIdentifier
          + "); the saved state is kept but not applied.";
    return {};
}

PluginInsertHost::PluginInsertHost()
{
    formatManager_.addFormat(new juce::VST3PluginFormat());
    for (int lane = 0; lane < kMaxProcessingLanes; ++lane)
    {
        laneInsertTrackId_[lane].store(-1, std::memory_order_relaxed);
        laneInsertSlotIndex_[lane].store(-1, std::memory_order_relaxed);
        laneInsertStage_[lane].store(-1, std::memory_order_relaxed);
    }
    auto empty = std::make_shared<PluginAudioThreadMap>();
    std::atomic_store_explicit(&audioThreadMap_, empty, std::memory_order_release);
}

PluginInsertHost::~PluginInsertHost()
{
    releaseResources();
    editorWindows_.clear();
    paramsWindows_.clear();
    chains_.clear();
}

void InsertProcessPlayHead::setContext(const PluginProcessTransportContext& context) noexcept
{
    // This playhead is a synchronous handoff: JUCE asks it for PositionInfo only while the
    // immediately following hosted processBlock is executing. Keep optional fields disengaged
    // unless DAL can describe them from the current render segment.
    position_ = {};

    const double validSampleRate = std::isfinite(context.sampleRate) && context.sampleRate > 0.0
                                       ? context.sampleRate
                                       : 48000.0;
    const double validBpm = std::isfinite(context.bpm) && context.bpm > 0.0 ? context.bpm : 120.0;
    const std::int64_t timelineSample = juce::jmax<std::int64_t>(0, context.timelineSample);
    const double elapsedSeconds = static_cast<double>(timelineSample) / validSampleRate;
    const double ppqPosition = elapsedSeconds * validBpm / 60.0;

    // VST3 requires projectTimeSamples when a ProcessContext exists. JUCE maps these engaged
    // values directly to kTempoValid, kTimeSigValid and kProjectTimeMusicValid.
    position_.setTimeInSamples(timelineSample);
    position_.setTimeInSeconds(elapsedSeconds);
    position_.setBpm(validBpm);
    position_.setTimeSignature(
        juce::AudioPlayHead::TimeSignature{ context.timeSignatureNumerator,
                                             context.timeSignatureDenominator });
    position_.setPpqPosition(ppqPosition);

    const double quartersPerBar = static_cast<double>(context.timeSignatureNumerator) * 4.0
                                  / static_cast<double>(context.timeSignatureDenominator);
    if (std::isfinite(quartersPerBar) && quartersPerBar > 0.0)
    {
        position_.setPpqPositionOfLastBarStart(
            std::floor(ppqPosition / quartersPerBar) * quartersPerBar);
    }

    if (context.isLooping && context.loopEndSample > context.loopStartSample)
    {
        const auto toPpq = [validSampleRate, validBpm](const std::int64_t sample) noexcept {
            return static_cast<double>(juce::jmax<std::int64_t>(0, sample)) / validSampleRate
                   * validBpm / 60.0;
        };
        position_.setLoopPoints(
            juce::AudioPlayHead::LoopPoints{ toPpq(context.loopStartSample), toPpq(context.loopEndSample) });
    }

    position_.setIsPlaying(context.isPlaying);
    position_.setIsRecording(context.isRecording);
    position_.setIsLooping(context.isLooping && context.loopEndSample > context.loopStartSample);
}

juce::Optional<juce::AudioPlayHead::PositionInfo> InsertProcessPlayHead::getPosition() const
{
    return position_;
}

void PluginInsertHost::recordPluginSlotUndo(const juce::String& label, const PluginUndoStepSides& sides)
{
    if (undoRecorder_ != nullptr && undoContext_ != nullptr)
    {
        undoRecorder_(undoContext_, label, sides);
    }
}

void PluginInsertHost::pushPluginParameterUndoStep(const TrackId trackId,
                                                   const InsertSlotId slotId,
                                                   const juce::MemoryBlock& baselineOpaqueState)
{
    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        return;
    }
    PluginTrackChain afterChain = exportChain(trackId);
    PluginTrackChain beforeChain = afterChain;
    bool found = false;
    for (auto& d : beforeChain.slots)
    {
        if (d.slotId == slotId)
        {
            d.opaqueState = baselineOpaqueState;
            found = true;
            break;
        }
    }
    if (!found)
    {
        return;
    }
    PluginUndoStepSides sides;
    sides.trackId = trackId;
    sides.before = std::move(beforeChain);
    sides.after = std::move(afterChain);
    recordPluginSlotUndo("Plugin parameters", sides);
}

void PluginInsertHost::flushOpenEditorParameterUndoSteps()
{
    std::vector<std::pair<EditorKey, juce::MemoryBlock>> entries;
    entries.reserve(editorOpenState_.size());
    for (const auto& kv : editorOpenState_)
    {
        entries.push_back(kv);
    }
    for (const auto& e : entries)
    {
        const LiveInsertSlot* live = findLiveConst(e.first.first, e.first.second);
        if (live == nullptr || live->instance == nullptr)
        {
            continue;
        }
        juce::MemoryBlock now;
        live->instance->getStateInformation(now);
        if (now == e.second)
        {
            continue;
        }
        const auto hostIt = lastHostAppliedState_.find(e.first);
        if (hostIt != lastHostAppliedState_.end() && now == hostIt->second)
        {
            continue;
        }
        pushPluginParameterUndoStep(e.first.first, e.first.second, e.second);
        editorOpenState_[e.first] = std::move(now);
    }
}

bool PluginInsertHost::tryInPlaceParameterStateRestore(const TrackId trackId,
                                                      const PluginTrackChain& targetChain)
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }

    const auto itLive = chains_.find(trackId);
    const bool hasLive = itLive != chains_.end() && !itLive->second.empty();

    if (targetChain.slots.empty())
    {
        if (!hasLive)
        {
            return true;
        }
        return false;
    }

    if (!hasLive)
    {
        return false;
    }

    std::vector<LiveInsertSlot>& v = itLive->second;
    if (v.size() != targetChain.slots.size())
    {
        return false;
    }

    for (size_t i = 0; i < v.size(); ++i)
    {
        const LiveInsertSlot& live = v[i];
        const PluginInsertDescriptor& desc = targetChain.slots[i];
        if (!desc.occupied)
        {
            return false;
        }
        if (live.instance == nullptr)
        {
            return false;
        }
        if (live.slotId != desc.slotId)
        {
            return false;
        }
        if (live.stage != desc.stage)
        {
            return false;
        }
        if (desc.vst3AbsolutePath != live.instance->getPluginDescription().fileOrIdentifier)
        {
            return false;
        }
        const juce::String liveId = live.instance->getPluginDescription().createIdentifierString();
        if (desc.pluginIdentifier.isNotEmpty() && liveId != desc.pluginIdentifier)
        {
            return false;
        }
    }

    for (size_t i = 0; i < v.size(); ++i)
    {
        LiveInsertSlot& live = v[i];
        const PluginInsertDescriptor& desc = targetChain.slots[i];

        if (desc.opaqueState.getSize() > 0)
        {
            live.instance->setStateInformation(desc.opaqueState.getData(), (int)desc.opaqueState.getSize());
            if (live.instance->getMainBusNumInputChannels() != 2
                || live.instance->getMainBusNumOutputChannels() != 2)
            {
                live.instance->prepareToPlay(effectiveSr(sampleRate_), effectiveBs(blockSize_));
            }
            live.layoutOk = live.instance->getMainBusNumInputChannels() == 2
                            && live.instance->getMainBusNumOutputChannels() == 2;
        }

        const EditorKey ek{ trackId, live.slotId };
        juce::MemoryBlock postRestore;
        live.instance->getStateInformation(postRestore);
        lastHostAppliedState_[ek] = postRestore;
        if (auto st = editorOpenState_.find(ek); st != editorOpenState_.end())
        {
            st->second = postRestore;
        }
    }

    rebuildAudioThreadMapAndPublish();
    return true;
}

InsertSlotId PluginInsertHost::allocateSlotId() noexcept
{
    return nextInsertSlotId_++;
}

void PluginInsertHost::logPluginInstanceLayout(const char* context, juce::AudioPluginInstance& inst) const
{
    juce::String msg = "[plugin] prepare ";
    msg << (context != nullptr ? context : "?")
        << " name=\"" << inst.getName() << "\""
        << " id=" << inst.getPluginDescription().createIdentifierString()
        << " totalIn=" << inst.getTotalNumInputChannels()
        << " totalOut=" << inst.getTotalNumOutputChannels()
        << " busIn=" << inst.getBusCount(true)
        << " busOut=" << inst.getBusCount(false)
        << " mainIn=" << inst.getMainBusNumInputChannels()
        << " mainOut=" << inst.getMainBusNumOutputChannels()
        << " sr=" << juce::String(inst.getSampleRate(), 2)
        << " block=" << inst.getBlockSize()
        << " latency=" << inst.getLatencySamples()
        << " hostOutCh=" << numOutChannels_
        << " hostInsertCh=" << kInsertChannels
        << " hostScratchSamples=" << blockSize_
        << " hostSr=" << juce::String(sampleRate_, 2);
    juce::Logger::writeToLog(msg);
}

void PluginInsertHost::logStereoLayoutFailure(const TrackId trackId) const
{
    juce::Logger::writeToLog(
        juce::String("[plugin] Stereo insert layout could not be established for track ")
        + juce::String((juce::int64)trackId)
        + " — VST3 processing bypassed (expected main bus 2/2).");
}

bool PluginInsertHost::tryPrepareStereoInsert(juce::AudioPluginInstance& inst, const double sr, const int bs)
{
    // AudioProcessor stores this raw pointer. A fresh instance gets the host-lifetime default
    // playhead so it is never without one; `rebuildAudioThreadMapAndPublish` re-points every
    // published instance at its CHAIN's playhead (Stage A1). An already-pointed instance is left
    // alone — re-preparing must not race a possible concurrent reader with a redundant store.
    if (inst.getPlayHead() == nullptr)
    {
        inst.setPlayHead(&processPlayHead_);
    }
    inst.releaseResources();
    const double srU = sr > 0.0 ? sr : 48000.0;
    const int bsU = bs > 0 ? bs : 512;
    inst.setPlayConfigDetails(kInsertChannels, kInsertChannels, srU, bsU);
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    (void) inst.setBusesLayout(layout);
    inst.prepareToPlay(srU, bsU);
    return inst.getMainBusNumInputChannels() == 2 && inst.getMainBusNumOutputChannels() == 2;
}

void PluginInsertHost::insertLiveSlotSorted(const TrackId trackId, LiveInsertSlot slot)
{
    auto& v = chains_[trackId];
    if (slot.stage == InsertStage::Pre)
    {
        auto it = std::find_if(
            v.begin(), v.end(), [](const LiveInsertSlot& s) { return s.stage == InsertStage::Post; });
        v.insert(it, std::move(slot));
    }
    else
    {
        v.push_back(std::move(slot));
    }
}

PluginInsertHost::LiveInsertSlot* PluginInsertHost::findLiveMutable(const TrackId trackId,
                                                                    const InsertSlotId slotId) noexcept
{
    auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return nullptr;
    }
    for (auto& s : it->second)
    {
        if (s.slotId == slotId)
        {
            return &s;
        }
    }
    return nullptr;
}

const PluginInsertHost::LiveInsertSlot* PluginInsertHost::findLiveConst(const TrackId trackId,
                                                                       const InsertSlotId slotId) const noexcept
{
    auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return nullptr;
    }
    for (const auto& s : it->second)
    {
        if (s.slotId == slotId)
        {
            return &s;
        }
    }
    return nullptr;
}

const PluginInsertHost::LiveInsertSlot* PluginInsertHost::findPrimaryUiSlotConst(const TrackId trackId) const noexcept
{
    auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return nullptr;
    }
    for (const auto& s : it->second)
    {
        if (s.stage == InsertStage::Post && s.instance != nullptr)
        {
            return &s;
        }
    }
    for (const auto& s : it->second)
    {
        if (s.instance != nullptr)
        {
            return &s;
        }
    }
    return nullptr;
}

juce::Result PluginInsertHost::addInsertFromVst3FileNoUndo(const TrackId trackId,
                                                          const InsertStage stage,
                                                          const juce::File& vst3File)
{
    if (trackId == kInvalidTrackId)
    {
        return juce::Result::fail("Invalid track id.");
    }
    if (!vst3File.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    juce::String err;
    const juce::PluginDescription desc = pickPrimaryDescription(vst3File, formatManager_, err);
    if (err.isNotEmpty() || desc.name.isEmpty())
    {
        return juce::Result::fail(err.isNotEmpty() ? err : "Could not read VST3 description.");
    }

    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);
    std::unique_ptr<juce::AudioPluginInstance> inst(
        formatManager_.createPluginInstance(desc, srU, bsU, err));
    if (inst == nullptr)
    {
        return juce::Result::fail(err.isNotEmpty() ? err : "createPluginInstance failed.");
    }

    const bool layoutOk = tryPrepareStereoInsert(*inst, srU, bsU);
    if (!layoutOk)
    {
        logStereoLayoutFailure(trackId);
    }
    logPluginInstanceLayout("load", *inst);

    LiveInsertSlot live;
    live.slotId = allocateSlotId();
    live.stage = stage;
    live.instance = std::move(inst);
    live.layoutOk = layoutOk;

    insertLiveSlotSorted(trackId, std::move(live));
    rebuildAudioThreadMapAndPublish();
    return juce::Result::ok();
}

juce::Result PluginInsertHost::addInsertFromVst3File(const TrackId trackId,
                                                    const InsertStage stage,
                                                    const juce::File& vst3File)
{
    const PluginTrackChain before = exportChain(trackId);
    const juce::Result r = addInsertFromVst3FileNoUndo(trackId, stage, vst3File);
    if (!r.wasOk())
    {
        return r;
    }
    const PluginTrackChain after = exportChain(trackId);
    if (!before.chainEquals(after))
    {
        PluginUndoStepSides sides;
        sides.trackId = trackId;
        sides.before = before;
        sides.after = after;
        recordPluginSlotUndo("Add VST3 insert", sides);
    }
    return juce::Result::ok();
}

juce::Result PluginInsertHost::loadVst3FromFile(const TrackId trackId, const juce::File& vst3File)
{
    const PluginTrackChain before = exportChain(trackId);
    importChainNoUndo(trackId, {});
    const juce::Result r = addInsertFromVst3FileNoUndo(trackId, InsertStage::Post, vst3File);
    if (!r.wasOk())
    {
        importChainNoUndo(trackId, before);
        return r;
    }
    const PluginTrackChain after = exportChain(trackId);
    PluginUndoStepSides sides;
    sides.trackId = trackId;
    sides.before = before;
    sides.after = after;
    recordPluginSlotUndo("Load VST3", sides);
    return juce::Result::ok();
}

void PluginInsertHost::removeInsert(const TrackId trackId, const InsertSlotId slotId)
{
    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        return;
    }
    const PluginTrackChain before = exportChain(trackId);
    const auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return;
    }
    auto& v = it->second;
    const auto found
        = std::find_if(v.begin(), v.end(), [&](const LiveInsertSlot& s) { return s.slotId == slotId; });
    if (found == v.end())
    {
        return;
    }
    closeEditorForSlot(trackId, slotId);
    lastHostAppliedState_.erase(EditorKey{ trackId, slotId });
    if (found->instance != nullptr)
    {
        found->instance->releaseResources();
    }
    v.erase(found);
    if (v.empty())
    {
        chains_.erase(trackId);
    }
    rebuildAudioThreadMapAndPublish();
    const PluginTrackChain after = exportChain(trackId);
    if (!before.chainEquals(after))
    {
        PluginUndoStepSides sides;
        sides.trackId = trackId;
        sides.before = before;
        sides.after = after;
        recordPluginSlotUndo("Remove VST3 insert", sides);
    }
}

void PluginInsertHost::moveInsertToStageAtGap(const TrackId trackId,
                                              const InsertSlotId slotId,
                                              const InsertStage targetStage,
                                              const int gapIndexInTargetStage)
{
    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        return;
    }

    const auto itChain = chains_.find(trackId);
    if (itChain == chains_.end())
    {
        return;
    }

    auto& v = itChain->second;
    const auto found = std::find_if(v.begin(), v.end(), [&](const LiveInsertSlot& s) {
        return s.slotId == slotId;
    });
    if (found == v.end())
    {
        return;
    }
    if (found->stage == targetStage)
    {
        return;
    }

    const PluginTrackChain before = exportChain(trackId);

    LiveInsertSlot moved = std::move(*found);
    v.erase(found);
    if (v.empty())
    {
        chains_.erase(trackId);
        moved.stage = targetStage;
        std::vector<LiveInsertSlot> nv;
        nv.push_back(std::move(moved));
        chains_[trackId] = std::move(nv);
        rebuildAudioThreadMapAndPublish();
        const PluginTrackChain after = exportChain(trackId);
        if (!before.chainEquals(after))
        {
            PluginUndoStepSides sides;
            sides.trackId = trackId;
            sides.before = before;
            sides.after = after;
            recordPluginSlotUndo("Move insert", sides);
        }
        return;
    }

    auto& v2 = chains_[trackId];
    int targetCount = 0;
    for (const auto& s : v2)
    {
        if (s.stage == targetStage)
        {
            ++targetCount;
        }
    }

    const int finalIdx = juce::jlimit(0, targetCount, gapIndexInTargetStage);
    moved.stage = targetStage;

    if (targetStage == InsertStage::Pre)
    {
        v2.insert(v2.begin() + finalIdx, std::move(moved));
    }
    else
    {
        const auto firstPost = std::find_if(
            v2.begin(), v2.end(), [](const LiveInsertSlot& s) { return s.stage == InsertStage::Post; });
        v2.insert(firstPost + finalIdx, std::move(moved));
    }

    rebuildAudioThreadMapAndPublish();
    const PluginTrackChain after = exportChain(trackId);
    if (!before.chainEquals(after))
    {
        PluginUndoStepSides sides;
        sides.trackId = trackId;
        sides.before = before;
        sides.after = after;
        recordPluginSlotUndo("Move insert", sides);
    }
}

void PluginInsertHost::moveInsertToStage(const TrackId trackId,
                                         const InsertSlotId slotId,
                                         const InsertStage newStage)
{
    moveInsertToStageAtGap(
        trackId, slotId, newStage, std::numeric_limits<int>::max());
}

void PluginInsertHost::reorderInsertWithinStage(const TrackId trackId,
                                               const InsertSlotId slotId,
                                               const int gapIndexInStage)
{
    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        return;
    }

    const auto itChain = chains_.find(trackId);
    if (itChain == chains_.end())
    {
        return;
    }

    auto& v = itChain->second;
    const auto srcIt = std::find_if(v.begin(), v.end(), [&](const LiveInsertSlot& s) {
        return s.slotId == slotId;
    });
    if (srcIt == v.end())
    {
        return;
    }

    const InsertStage st = srcIt->stage;

    int srcIndexInStage = 0;
    for (auto it = v.begin(); it != srcIt; ++it)
    {
        if (it->stage == st)
        {
            ++srcIndexInStage;
        }
    }

    int stageCount = 0;
    for (const auto& s : v)
    {
        if (s.stage == st)
        {
            ++stageCount;
        }
    }

    const int gap = juce::jlimit(0, stageCount, gapIndexInStage);
    if (gap == srcIndexInStage || gap == srcIndexInStage + 1)
    {
        return;
    }

    const PluginTrackChain before = exportChain(trackId);

    const int finalIdx = (gap > srcIndexInStage) ? (gap - 1) : gap;

    LiveInsertSlot moved = std::move(*srcIt);
    v.erase(srcIt);
    if (v.empty())
    {
        chains_.erase(trackId);
        return;
    }

    if (st == InsertStage::Pre)
    {
        const auto insertPos = v.begin() + finalIdx;
        v.insert(insertPos, std::move(moved));
    }
    else
    {
        const auto firstPost = std::find_if(
            v.begin(), v.end(), [](const LiveInsertSlot& s) { return s.stage == InsertStage::Post; });
        const auto insertPos = firstPost + finalIdx;
        v.insert(insertPos, std::move(moved));
    }

    rebuildAudioThreadMapAndPublish();
    const PluginTrackChain after = exportChain(trackId);
    if (!before.chainEquals(after))
    {
        PluginUndoStepSides sides;
        sides.trackId = trackId;
        sides.before = before;
        sides.after = after;
        recordPluginSlotUndo("Reorder insert", sides);
    }
}

PluginTrackChain PluginInsertHost::exportChain(const TrackId trackId) const
{
    PluginTrackChain out;
    const auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return out;
    }
    for (const auto& live : it->second)
    {
        if (live.isUnavailablePlaceholder())
        {
            // Re-emit the saved identity + state untouched (only slot id / stage follow the live
            // row, which the user may have moved between Pre and Post while it was unavailable).
            PluginInsertDescriptor d = live.unavailableDescriptor;
            d.slotId = live.slotId;
            d.stage = live.stage;
            d.occupied = true;
            out.slots.push_back(std::move(d));
            continue;
        }
        if (live.instance == nullptr)
        {
            continue;
        }
        PluginInsertDescriptor d;
        d.slotId = live.slotId;
        d.stage = live.stage;
        d.occupied = true;
        const juce::PluginDescription pd = live.instance->getPluginDescription();
        d.vst3AbsolutePath = pd.fileOrIdentifier;
        d.pluginIdentifier = pd.createIdentifierString();
        live.instance->getStateInformation(d.opaqueState);
        out.slots.push_back(std::move(d));
    }
    return out;
}

void PluginInsertHost::importChainNoUndo(const TrackId trackId, const PluginTrackChain& chain)
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    eraseLastHostAppliedStateForTrack(lastHostAppliedState_, trackId);
    closeEditorsForTrack(trackId);
    // Publish-before-destroy (F5): same retire discipline as evict — the old live slots must not be
    // destroyed while a callback can still hold the previously published map.
    retireChainPublishDrainAndDestroy(trackId);

    std::vector<LiveInsertSlot> built;
    built.reserve(chain.slots.size());

    // Primary data-loss rule: a saved insert whose plugin cannot be brought back right now keeps
    // its chain position, identity and opaque state as an unavailable placeholder. Saving the
    // project afterwards re-emits it unchanged; only an explicit user removal drops it.
    const auto keepAsUnavailablePlaceholder = [&](const PluginInsertDescriptor& desc, const juce::String& why) {
        LiveInsertSlot placeholder;
        placeholder.slotId = desc.slotId != kInvalidInsertSlotId ? desc.slotId : allocateSlotId();
        placeholder.stage = desc.stage;
        placeholder.unavailableDescriptor = desc;
        placeholder.unavailableDescriptor.slotId = placeholder.slotId;
        placeholder.unavailableDescriptor.occupied = true;
        juce::Logger::writeToLog("[plugin] insert kept as unavailable placeholder: track "
                                 + juce::String((juce::int64)trackId) + " slot "
                                 + juce::String((juce::int64)placeholder.slotId) + " ("
                                 + (desc.stage == InsertStage::Pre ? "pre" : "post") + ") path=\""
                                 + desc.vst3AbsolutePath + "\" identifier=\"" + desc.pluginIdentifier
                                 + "\" stateBytes=" + juce::String((juce::int64)desc.opaqueState.getSize())
                                 + " reason: " + why);
        built.push_back(std::move(placeholder));
    };

    for (const auto& desc : chain.slots)
    {
        if (!desc.occupied)
        {
            continue;
        }

        if (desc.slotId != kInvalidInsertSlotId)
        {
            nextInsertSlotId_ = juce::jmax(nextInsertSlotId_, desc.slotId + 1);
        }

        if (desc.vst3AbsolutePath.isEmpty())
        {
            // Nothing identifies this row (not produced by any DAL writer) — nothing to preserve.
            continue;
        }

        const juce::File f(desc.vst3AbsolutePath);
        if (!f.exists())
        {
            keepAsUnavailablePlaceholder(desc, "VST3 bundle missing at saved path");
            continue;
        }

        juce::String err;
        const juce::PluginDescription pd
            = pickDescriptionForSavedIdentity(f, formatManager_, desc.pluginIdentifier, err);
        if (pd.name.isEmpty())
        {
            keepAsUnavailablePlaceholder(desc, err);
            continue;
        }

        const double srU = effectiveSr(sampleRate_);
        const int bsU = effectiveBs(blockSize_);
        std::unique_ptr<juce::AudioPluginInstance> inst(
            formatManager_.createPluginInstance(pd, srU, bsU, err));
        if (inst == nullptr)
        {
            keepAsUnavailablePlaceholder(desc, "createPluginInstance failed: " + err);
            continue;
        }
        bool layoutOk = tryPrepareStereoInsert(*inst, srU, bsU);
        if (desc.opaqueState.getSize() > 0)
        {
            inst->setStateInformation(desc.opaqueState.getData(), (int)desc.opaqueState.getSize());
            if (inst->getMainBusNumInputChannels() != 2 || inst->getMainBusNumOutputChannels() != 2)
            {
                inst->prepareToPlay(srU, bsU);
            }
            layoutOk = inst->getMainBusNumInputChannels() == 2 && inst->getMainBusNumOutputChannels() == 2;
        }
        if (!layoutOk)
        {
            logStereoLayoutFailure(trackId);
        }
        logPluginInstanceLayout("import", *inst);

        LiveInsertSlot live;
        live.slotId = desc.slotId != kInvalidInsertSlotId ? desc.slotId : allocateSlotId();
        live.stage = desc.stage;
        live.instance = std::move(inst);
        live.layoutOk = layoutOk;
        built.push_back(std::move(live));
    }

    if (!built.empty())
    {
        chains_[trackId] = std::move(built);
    }
    rebuildAudioThreadMapAndPublish();
}

void PluginInsertHost::importChain(const TrackId trackId, const PluginTrackChain& chain)
{
    if (tryInPlaceParameterStateRestore(trackId, chain))
    {
        return;
    }
    importChainNoUndo(trackId, chain);
}

std::vector<InsertRowView> PluginInsertHost::getInsertRowsForTrack(const TrackId trackId) const
{
    std::vector<InsertRowView> rows;
    const auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return rows;
    }
    rows.reserve(it->second.size());
    for (const auto& live : it->second)
    {
        if (live.isUnavailablePlaceholder())
        {
            rows.push_back(InsertRowView{ live.slotId,
                                          live.stage,
                                          unavailableInsertDisplayName(live.unavailableDescriptor)
                                              + " (unavailable)",
                                          true });
            continue;
        }
        if (live.instance == nullptr)
        {
            continue;
        }
        rows.push_back(
            InsertRowView{ live.slotId, live.stage, live.instance->getName(), false });
    }
    return rows;
}

void PluginInsertHost::retireChainPublishDrainAndDestroy(const TrackId trackId)
{
    std::vector<LiveInsertSlot> retired;
    if (const auto it = chains_.find(trackId); it != chains_.end())
    {
        retired = std::move(it->second);
        chains_.erase(it);
    }
    if (retired.empty())
    {
        rebuildAudioThreadMapAndPublish();
        return;
    }
    // Publish-before-destroy (F5): the audio callback may still be running with the previously
    // published map whose raw AudioProcessor pointers point into `retired`. Publish the map without
    // this track first, drain the in-flight callback, and only then release/destroy the instances.
    rebuildAudioThreadMapAndPublish();
    if (realtimeDrainAfterPublish_ != nullptr)
    {
        realtimeDrainAfterPublish_();
    }
    for (auto& live : retired)
    {
        if (live.instance != nullptr)
        {
            live.instance->releaseResources();
        }
    }
    retired.clear();
}

void PluginInsertHost::removeAllPlugins() noexcept
{
    for (const auto& kv : editorWindows_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setVisible(false);
        }
    }
    editorWindows_.clear();
    for (const auto& kv : paramsWindows_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setVisible(false);
        }
    }
    paramsWindows_.clear();
    editorOpenState_.clear();
    lastHostAppliedState_.clear();

    // Publish-before-destroy (F5): move every live chain out, publish the now-empty realtime map,
    // drain any in-flight audio callback still holding the old map, then release/destroy.
    std::unordered_map<TrackId, std::vector<LiveInsertSlot>> retired = std::move(chains_);
    chains_.clear();
    nextInsertSlotId_ = 1;
    rebuildAudioThreadMapAndPublish();
    if (!retired.empty() && realtimeDrainAfterPublish_ != nullptr)
    {
        realtimeDrainAfterPublish_();
    }
    for (auto& kv : retired)
    {
        for (auto& live : kv.second)
        {
            if (live.instance != nullptr)
            {
                live.instance->releaseResources();
            }
        }
    }
    retired.clear();
}

void PluginInsertHost::evictPluginForTrackNoUndo(const TrackId trackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    eraseLastHostAppliedStateForTrack(lastHostAppliedState_, trackId);
    closeEditorsForTrack(trackId);
    retireChainPublishDrainAndDestroy(trackId);
}

void PluginInsertHost::removePlugin(const TrackId trackId)
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const PluginTrackChain before = exportChain(trackId);
    importChainNoUndo(trackId, {});
    const PluginTrackChain after = exportChain(trackId);
    if (!before.chainEquals(after))
    {
        PluginUndoStepSides sides;
        sides.trackId = trackId;
        sides.before = before;
        sides.after = after;
        recordPluginSlotUndo("Remove VST3", sides);
    }
}

std::shared_ptr<InsertProcessPlayHead> PluginInsertHost::chainPlayHeadForTrack(const TrackId trackId)
{
    auto it = chainPlayHeads_.find(trackId);
    if (it != chainPlayHeads_.end())
    {
        return it->second;
    }
    auto ph = std::make_shared<InsertProcessPlayHead>();
    chainPlayHeads_.emplace(trackId, ph);
    return ph;
}

void PluginInsertHost::rebuildAudioThreadMapAndPublish()
{
    auto next = std::make_shared<PluginAudioThreadMap>();
    next->entries.reserve(chains_.size());
    for (const auto& kv : chains_)
    {
        if (kv.second.empty())
        {
            continue;
        }
        PluginAudioThreadMap::Entry e;
        e.trackId = kv.first;
        // Stage A1: the chain's own transport playhead, co-owned by the entry so an in-flight
        // callback holding this map keeps a valid pointer across any message-thread change.
        e.playHead = chainPlayHeadForTrack(kv.first);
        e.slots.reserve(kv.second.size());
        for (const auto& live : kv.second)
        {
            if (live.instance == nullptr)
            {
                continue;
            }
            // Point the instance at its chain's playhead. Only ever a WRITE for an instance that
            // is not yet published (new instances start on `processPlayHead_`); re-publishing an
            // unchanged chain is a pointer compare, so the audio thread never races a store.
            if (live.instance->getPlayHead() != e.playHead.get())
            {
                live.instance->setPlayHead(e.playHead.get());
            }
            PluginAudioThreadMap::SlotProc sp;
            sp.processor = live.instance.get();
            sp.layoutOk = live.layoutOk;
            sp.stage = live.stage;
            // Message thread: name the instance for the opt-in profiler (bounded table; −1 when full).
            sp.profileSlot = audio_profiler::AudioThreadProfiler::get().registerInstance(
                audio_profiler::Category::InsertPlugin, kv.first, live.slotId,
                live.instance->getName() + (live.stage == InsertStage::Pre ? " [Pre]" : " [Post]"));
            e.slots.push_back(sp);
        }
        if (!e.slots.empty())
        {
            next->entries.push_back(std::move(e));
        }
    }
    std::atomic_store_explicit(&audioThreadMap_, std::move(next), std::memory_order_release);
}

void PluginInsertHost::closeEditorForSlot(const TrackId trackId, const InsertSlotId slotId)
{
    const EditorKey key{ trackId, slotId };
    editorOpenState_.erase(key);
    if (auto it = editorWindows_.find(key); it != editorWindows_.end())
    {
        if (it->second != nullptr)
        {
            it->second->setVisible(false);
        }
        editorWindows_.erase(it);
    }
    if (auto itp = paramsWindows_.find(key); itp != paramsWindows_.end())
    {
        if (itp->second != nullptr)
        {
            itp->second->setVisible(false);
        }
        paramsWindows_.erase(itp);
    }
}

void PluginInsertHost::closeEditorsForTrack(const TrackId trackId)
{
    for (auto it = editorOpenState_.begin(); it != editorOpenState_.end();)
    {
        if (it->first.first == trackId)
        {
            it = editorOpenState_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = editorWindows_.begin(); it != editorWindows_.end();)
    {
        if (it->first.first == trackId)
        {
            if (it->second != nullptr)
            {
                it->second->setVisible(false);
            }
            it = editorWindows_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = paramsWindows_.begin(); it != paramsWindows_.end();)
    {
        if (it->first.first == trackId)
        {
            if (it->second != nullptr)
            {
                it->second->setVisible(false);
            }
            it = paramsWindows_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void PluginInsertHost::openNativeEditor(const TrackId trackId)
{
    const LiveInsertSlot* c = findPrimaryUiSlotConst(trackId);
    if (c == nullptr)
    {
        return;
    }
    openNativeEditor(trackId, c->slotId);
}

void PluginInsertHost::openNativeEditor(const TrackId trackId, const InsertSlotId slotId)
{
    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        return;
    }
    LiveInsertSlot* s = findLiveMutable(trackId, slotId);
    if (s == nullptr || s->instance == nullptr)
    {
        return;
    }
    const EditorKey key{ trackId, s->slotId };
    if (editorWindows_.find(key) != editorWindows_.end())
    {
        editorWindows_[key]->toFront(true);
        return;
    }
    juce::AudioProcessor& proc = *s->instance;
    juce::AudioProcessorEditor* const rawEd = proc.createEditor();
    if (rawEd == nullptr)
    {
        return;
    }
    auto ed = std::unique_ptr<juce::AudioProcessorEditor>(rawEd);
    juce::MemoryBlock snap;
    proc.getStateInformation(snap);
    editorOpenState_[key] = std::move(snap);
    editorWindows_[key] = std::make_unique<PluginEditorWindow>(
        *this, trackId, s->slotId, proc, std::move(ed), editorShortcutCallbacks_);
}

void PluginInsertHost::openGenericParamsEditor(const TrackId trackId)
{
    const LiveInsertSlot* c = findPrimaryUiSlotConst(trackId);
    if (c == nullptr)
    {
        return;
    }
    openGenericParamsEditor(trackId, c->slotId);
}

void PluginInsertHost::openGenericParamsEditor(const TrackId trackId, const InsertSlotId slotId)
{
    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        return;
    }
    LiveInsertSlot* s = findLiveMutable(trackId, slotId);
    if (s == nullptr || s->instance == nullptr)
    {
        return;
    }
    const EditorKey key{ trackId, s->slotId };
    if (paramsWindows_.find(key) != paramsWindows_.end())
    {
        paramsWindows_[key]->toFront(true);
        return;
    }
    juce::AudioProcessor& proc = *s->instance;
    juce::MemoryBlock snap;
    proc.getStateInformation(snap);
    editorOpenState_[key] = std::move(snap);
    auto ed = std::make_unique<juce::GenericAudioProcessorEditor>(proc);
    paramsWindows_[key] = std::make_unique<PluginParamsWindow>(
        *this, trackId, s->slotId, proc, std::move(ed), editorShortcutCallbacks_);
}

void PluginInsertHost::editorWindowClosing(const TrackId trackId,
                                           const InsertSlotId slotId,
                                           const bool wasGenericEditor)
{
    const EditorKey key{ trackId, slotId };
    const LiveInsertSlot* live = findLiveConst(trackId, slotId);
    if (live != nullptr && live->instance != nullptr)
    {
        if (const auto st = editorOpenState_.find(key); st != editorOpenState_.end())
        {
            juce::MemoryBlock now;
            live->instance->getStateInformation(now);
            if (now != st->second)
            {
                const auto hostIt = lastHostAppliedState_.find(key);
                if (hostIt == lastHostAppliedState_.end() || now != hostIt->second)
                {
                    pushPluginParameterUndoStep(trackId, slotId, st->second);
                }
            }
        }
    }
    editorOpenState_.erase(key);
    if (wasGenericEditor)
    {
        paramsWindows_.erase(key);
    }
    else
    {
        editorWindows_.erase(key);
    }
}

bool PluginInsertHost::hasPluginOnTrack(const TrackId trackId) const noexcept
{
    return hasAnyInsertOnTrack(trackId);
}

bool PluginInsertHost::hasAnyInsertOnTrack(const TrackId trackId) const noexcept
{
    const auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        return false;
    }
    // Unavailable placeholders count: they are user configuration the Inspector lists and track
    // deletion must capture for undo, even though they publish no processor.
    return std::any_of(it->second.begin(), it->second.end(), [](const LiveInsertSlot& s) {
        return s.instance != nullptr || s.isUnavailablePlaceholder();
    });
}

juce::String PluginInsertHost::getPluginDisplayNameForTrack(const TrackId trackId) const
{
    if (trackId == kInvalidTrackId)
    {
        return {};
    }
    const LiveInsertSlot* s = findPrimaryUiSlotConst(trackId);
    if (s == nullptr || s->instance == nullptr)
    {
        return {};
    }
    return s->instance->getName();
}

void PluginInsertHost::prepareForDevice(const double sampleRate, const int blockSize, const int numOutputChannels)
{
    scratchMismatchNotified_.store(false, std::memory_order_relaxed);
    stereoPrepareFailureOneShot_.store(false, std::memory_order_relaxed);
    sampleRate_ = sampleRate;
    blockSize_ = juce::jmax(1, blockSize);
    numOutChannels_ = juce::jmax(1, numOutputChannels);
    scratch_.setSize(kInsertChannels, blockSize_, false, true, true);
    scratchPtrs_.clear();
    scratchPtrs_.reserve((size_t)kInsertChannels);
    for (int c = 0; c < kInsertChannels; ++c)
    {
        scratchPtrs_.push_back(scratch_.getWritePointer(c));
    }

    for (auto& kv : chains_)
    {
        for (auto& live : kv.second)
        {
            if (live.instance == nullptr)
            {
                continue;
            }
            const bool layoutOk = tryPrepareStereoInsert(*live.instance, sampleRate_, blockSize_);
            live.layoutOk = layoutOk;
            if (!(live.layoutOk))
            {
                const bool alreadyWarned
                    = stereoPrepareFailureOneShot_.exchange(true, std::memory_order_relaxed);
                if (!alreadyWarned)
                {
                    logStereoLayoutFailure(kv.first);
                }
            }
            logPluginInstanceLayout("device", *live.instance);
        }
    }
    rebuildAudioThreadMapAndPublish();
}

void PluginInsertHost::releaseResources()
{
    for (auto& kv : chains_)
    {
        for (auto& live : kv.second)
        {
            if (live.instance != nullptr)
            {
                live.instance->releaseResources();
            }
        }
    }
}

void PluginInsertHost::audioThread_clearScratch(const int numChannels, const int numSamples) noexcept
{
    const int ch = juce::jmin(numChannels, scratch_.getNumChannels());
    const int n = juce::jmin(numSamples, scratch_.getNumSamples());
    for (int c = 0; c < ch; ++c)
    {
        if (float* p = scratch_.getWritePointer(c))
        {
            juce::FloatVectorOperations::clear(p, n);
        }
    }
}

void PluginInsertHost::audioThread_setProcessTransportContext(
    const PluginProcessTransportContext& context) noexcept
{
    // Serial-path refresh: every published chain's playhead observes the same segment context
    // (bus strips, monitoring, instrument strips, offline render — all run on the calling thread
    // with no render-pool job in flight, so no concurrent per-entry writer exists). The default
    // playhead is kept in step for not-yet-republished instances.
    processPlayHead_.setContext(context);
    const std::shared_ptr<const PluginAudioThreadMap> m
        = std::atomic_load_explicit(&audioThreadMap_, std::memory_order_acquire);
    if (m == nullptr)
    {
        return;
    }
    for (const auto& e : m->entries)
    {
        if (e.playHead != nullptr)
        {
            e.playHead->setContext(context);
        }
    }
}

const PluginAudioThreadMap::Entry* PluginInsertHost::audioThread_findEntry(const PluginAudioThreadMap& map,
                                                                           const TrackId trackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return nullptr;
    }
    for (const auto& e : map.entries)
    {
        if (e.trackId == trackId)
        {
            return &e;
        }
    }
    return nullptr;
}

void PluginInsertHost::audioThread_setEntryTransportContext(const PluginAudioThreadMap::Entry& entry,
                                                            const PluginProcessTransportContext& context) noexcept
{
    if (entry.playHead != nullptr)
    {
        entry.playHead->setContext(context);
    }
}

float* const* PluginInsertHost::audioThread_getScratchWritePointers() noexcept
{
    return scratchPtrs_.empty() ? nullptr : scratchPtrs_.data();
}

bool PluginInsertHost::audioThread_hasActivePluginForTrack(const TrackId trackId) const noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    std::shared_ptr<const PluginAudioThreadMap> m
        = std::atomic_load_explicit(&audioThreadMap_, std::memory_order_acquire);
    if (m == nullptr)
    {
        return false;
    }
    for (const auto& e : m->entries)
    {
        if (e.trackId != trackId)
        {
            continue;
        }
        for (const auto& sp : e.slots)
        {
            if (sp.processor != nullptr && sp.layoutOk)
            {
                return true;
            }
        }
        return false;
    }
    return false;
}

void PluginInsertHost::audioThread_processChainForTrack(const TrackId trackId,
                                                       const InsertStage stage,
                                                       const int numSamples) noexcept
{
    // Serial-path wrapper: the host-owned shared scratch + MIDI scratch on the callback lane.
    // Render-pool jobs call `audioThread_processEntryChain` directly with their own lane buffers.
    if (trackId == kInvalidTrackId || numSamples <= 0)
    {
        return;
    }
    if (kInsertChannels <= 0 || scratchPtrs_.size() < (size_t)kInsertChannels)
    {
        return;
    }
    const std::shared_ptr<const PluginAudioThreadMap> m
        = std::atomic_load_explicit(&audioThreadMap_, std::memory_order_acquire);
    if (m == nullptr)
    {
        return;
    }
    const PluginAudioThreadMap::Entry* const hit = audioThread_findEntry(*m, trackId);
    if (hit == nullptr)
    {
        return;
    }
    audioThread_processEntryChain(*hit, stage, numSamples, scratchPtrs_.data(),
                                  scratch_.getNumSamples(), midiScratch_, kCallbackProcessingLane);
}

void PluginInsertHost::audioThread_processEntryChain(const PluginAudioThreadMap::Entry& entry,
                                                     const InsertStage stage,
                                                     const int numSamples,
                                                     float* const* scratchChannels,
                                                     const int scratchCapacitySamples,
                                                     juce::MidiBuffer& midiScratch,
                                                     const int laneIndex) noexcept
{
    if (numSamples <= 0 || entry.slots.empty() || scratchChannels == nullptr
        || scratchChannels[0] == nullptr || scratchChannels[1] == nullptr)
    {
        return;
    }
    if (numSamples > scratchCapacitySamples)
    {
        const bool already = scratchMismatchNotified_.exchange(true, std::memory_order_relaxed);
        if (!already && juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        {
            juce::MessageManager::callAsync([]() {
                juce::Logger::writeToLog(
                    "[plugin] one-shot: callback exceeded prepared scratch sample count; "
                    "processBlock uses a clamped view — check device buffer size / reopen audio.");
            });
        }
    }
    const int n = juce::jmin(numSamples, scratchCapacitySamples);
    const int lane = juce::jlimit(0, kMaxProcessingLanes - 1, laneIndex);
    if (n <= 0)
    {
        return;
    }

    // Diagnostics tap: level entering the chain (Pre stage = right after pre-gain). Relaxed atomics,
    // only for the one tapped track, so untapped tracks pay a single compare per call.
    const bool tapped = insertLevelTapTrackId_.load(std::memory_order_relaxed)
                        == static_cast<std::int64_t>(entry.trackId);
    if (tapped && stage == InsertStage::Pre)
    {
        audioThread_foldScratchLevelsInto(scratchChannels, insertLevelTapPeakBefore_,
                                          insertLevelTapSumSqBefore_, insertLevelTapSamplesBefore_, n);
        for (int c = 0; c < 2 && c < kInsertChannels; ++c)
        {
            const float* const row = scratchChannels[c];
            double sum = 0.0;
            for (int i = 0; row != nullptr && i < n; ++i)
            {
                sum += static_cast<double>(row[i]);
            }
            insertLevelTapSumBefore_[c].fetch_add(sum, std::memory_order_relaxed);
        }
        insertLevelTapPreBlocks_.fetch_add(1, std::memory_order_relaxed);
    }

    juce::AudioBuffer<float> view(scratchChannels, kInsertChannels, n);
    juce::ScopedNoDenormals noDenormals;
    audio_profiler::AudioThreadProfiler& profiler = audio_profiler::AudioThreadProfiler::get();
    const bool prof = profiler.audioThread_enabled();
    int slotIndex = -1;
    for (const auto& sp : entry.slots)
    {
        ++slotIndex;
        if (sp.processor == nullptr || !sp.layoutOk || sp.stage != stage)
        {
            continue;
        }
        // Stability C2B diagnostics: mark which insert this LANE is processing so a wedged
        // processBlock can be identified from the message thread (gate-timeout logging).
        laneInsertTrackId_[lane].store(static_cast<std::int64_t>(entry.trackId),
                                       std::memory_order_relaxed);
        laneInsertSlotIndex_[lane].store(slotIndex, std::memory_order_relaxed);
        laneInsertStage_[lane].store(static_cast<int>(stage), std::memory_order_relaxed);
        const std::int64_t tProf = prof ? audio_profiler::AudioThreadProfiler::ticks() : 0;
        sp.processor->processBlock(view, midiScratch);
        if (prof)
        {
            profiler.audioThread_addInstance(sp.profileSlot, audio_profiler::Category::InsertPlugin, tProf);
        }
        laneInsertTrackId_[lane].store(-1, std::memory_order_relaxed);
        laneInsertSlotIndex_[lane].store(-1, std::memory_order_relaxed);
        laneInsertStage_[lane].store(-1, std::memory_order_relaxed);
        midiScratch.clear();
    }

    // Diagnostics tap: level leaving the chain (after the last Post insert, before pan).
    if (tapped && stage == InsertStage::Post)
    {
        audioThread_foldScratchLevelsInto(scratchChannels, insertLevelTapPeakAfter_,
                                          insertLevelTapSumSqAfter_, insertLevelTapSamplesAfter_, n);
        insertLevelTapPostBlocks_.fetch_add(1, std::memory_order_relaxed);
    }
}

void PluginInsertHost::audioThread_foldScratchLevelsInto(const float* const* scratchChannels,
                                                         std::atomic<float>& peakHold,
                                                         std::atomic<double>& sumSquares,
                                                         std::atomic<std::uint64_t>& sampleCount,
                                                         const int numSamples) noexcept
{
    float peak = 0.0f;
    double sumSq = 0.0;
    std::uint64_t counted = 0;
    for (int c = 0; c < kInsertChannels; ++c)
    {
        const float* const row = scratchChannels != nullptr ? scratchChannels[c] : nullptr;
        if (row == nullptr || numSamples <= 0)
        {
            continue;
        }
        const juce::Range<float> r = juce::FloatVectorOperations::findMinAndMax(row, numSamples);
        peak = juce::jmax(peak, std::abs(r.getStart()), std::abs(r.getEnd()));
        for (int i = 0; i < numSamples; ++i)
        {
            sumSq += static_cast<double>(row[i]) * static_cast<double>(row[i]);
        }
        counted += static_cast<std::uint64_t>(numSamples);
    }
    float current = peakHold.load(std::memory_order_relaxed);
    while (peak > current && !peakHold.compare_exchange_weak(current, peak, std::memory_order_relaxed))
    {
    }
    sumSquares.fetch_add(sumSq, std::memory_order_relaxed);
    sampleCount.fetch_add(counted, std::memory_order_relaxed);
}

void PluginInsertHost::setInsertLevelTapTrackForDiagnostics(const TrackId trackId) noexcept
{
    insertLevelTapTrackId_.store(trackId == kInvalidTrackId ? -1 : static_cast<std::int64_t>(trackId),
                                 std::memory_order_relaxed);
    (void)readAndResetInsertLevelTapForDiagnostics();
}

PluginInsertHost::InsertLevelTapSnapshot PluginInsertHost::readAndResetInsertLevelTapForDiagnostics() noexcept
{
    InsertLevelTapSnapshot s;
    s.peakBeforeFirstInsert = insertLevelTapPeakBefore_.exchange(0.0f, std::memory_order_relaxed);
    s.peakAfterLastInsert = insertLevelTapPeakAfter_.exchange(0.0f, std::memory_order_relaxed);
    s.preStageBlocks = insertLevelTapPreBlocks_.exchange(0, std::memory_order_relaxed);
    s.postStageBlocks = insertLevelTapPostBlocks_.exchange(0, std::memory_order_relaxed);
    const double sumBefore = insertLevelTapSumSqBefore_.exchange(0.0, std::memory_order_relaxed);
    const double sumAfter = insertLevelTapSumSqAfter_.exchange(0.0, std::memory_order_relaxed);
    const std::uint64_t nBefore = insertLevelTapSamplesBefore_.exchange(0, std::memory_order_relaxed);
    const std::uint64_t nAfter = insertLevelTapSamplesAfter_.exchange(0, std::memory_order_relaxed);
    s.rmsBeforeFirstInsert = nBefore > 0 ? std::sqrt(sumBefore / static_cast<double>(nBefore)) : 0.0;
    s.rmsAfterLastInsert = nAfter > 0 ? std::sqrt(sumAfter / static_cast<double>(nAfter)) : 0.0;
    // `nBefore` counts both channels' samples; each channel saw nBefore / kInsertChannels.
    const double perChannel = nBefore > 0 ? static_cast<double>(nBefore) / static_cast<double>(juce::jmax(1, kInsertChannels)) : 0.0;
    for (int c = 0; c < 2; ++c)
    {
        const double sum = insertLevelTapSumBefore_[c].exchange(0.0, std::memory_order_relaxed);
        s.dcBeforeFirstInsert[c] = perChannel > 0.0 ? sum / perChannel : 0.0;
    }
    return s;
}

std::vector<std::pair<TrackId, std::vector<const void*>>>
    PluginInsertHost::exportChainInstancePointersForDiagnostics() const
{
    std::vector<std::pair<TrackId, std::vector<const void*>>> out;
    out.reserve(chains_.size());
    for (const auto& [trackId, slots] : chains_)
    {
        std::vector<const void*> instances;
        instances.reserve(slots.size());
        for (const LiveInsertSlot& s : slots)
        {
            if (s.instance != nullptr)
            {
                instances.push_back(static_cast<const void*>(s.instance.get()));
            }
        }
        out.emplace_back(trackId, std::move(instances));
    }
    return out;
}

juce::AudioPluginInstance* PluginInsertHost::liveInstanceAtChainIndexForDiagnostics(const TrackId trackId,
                                                                                  const int chainIndex) const noexcept
{
    const auto it = chains_.find(trackId);
    if (it == chains_.end() || chainIndex < 0 || chainIndex >= (int) it->second.size())
    {
        return nullptr;
    }
    return it->second[(size_t) chainIndex].instance.get();
}

std::vector<std::pair<TrackId, std::vector<const void*>>>
    PluginInsertHost::exportPublishedMapPointersForDiagnostics() const
{
    std::vector<std::pair<TrackId, std::vector<const void*>>> out;
    const std::shared_ptr<const PluginAudioThreadMap> m
        = std::atomic_load_explicit(&audioThreadMap_, std::memory_order_acquire);
    if (m == nullptr)
    {
        return out;
    }
    out.reserve(m->entries.size());
    for (const PluginAudioThreadMap::Entry& e : m->entries)
    {
        std::vector<const void*> procs;
        procs.reserve(e.slots.size());
        for (const PluginAudioThreadMap::SlotProc& sp : e.slots)
        {
            if (sp.processor != nullptr)
            {
                procs.push_back(static_cast<const void*>(sp.processor));
            }
        }
        out.emplace_back(e.trackId, std::move(procs));
    }
    return out;
}

juce::String PluginInsertHost::describeAudioThreadInsertStateForDiagnostics() const noexcept
{
    juce::String out;
    for (int lane = 0; lane < kMaxProcessingLanes; ++lane)
    {
        const std::int64_t tid = laneInsertTrackId_[lane].load(std::memory_order_relaxed);
        if (tid < 0)
        {
            continue;
        }
        if (out.isNotEmpty())
        {
            out << " ";
        }
        out << "insert=processing lane=" << lane << " trackId=" << juce::String(tid)
            << " slot=" << juce::String(laneInsertSlotIndex_[lane].load(std::memory_order_relaxed))
            << " stage=" << juce::String(laneInsertStage_[lane].load(std::memory_order_relaxed));
    }
    return out.isEmpty() ? juce::String("insert=idle") : out;
}

bool PluginInsertHost::installInsertInstanceForTests(const TrackId trackId,
                                                     const InsertStage stage,
                                                     std::unique_ptr<juce::AudioPluginInstance> instance)
{
    if (trackId == kInvalidTrackId || instance == nullptr)
    {
        return false;
    }
    LiveInsertSlot slot;
    slot.slotId = allocateSlotId();
    slot.stage = stage;
    slot.instance = std::move(instance);
    slot.layoutOk = tryPrepareStereoInsert(*slot.instance, sampleRate_, blockSize_);
    if (!slot.layoutOk)
    {
        return false;
    }
    insertLiveSlotSorted(trackId, std::move(slot));
    rebuildAudioThreadMapAndPublish();
    return true;
}
