// =============================================================================
// PluginInsertHost.cpp — VST3 instances, atomic processor map, scratch for the audio thread
// =============================================================================

#include "plugins/PluginInsertHost.h"

#include "plugins/PluginEditorWindows.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
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

    void logInsertDndDiag(const juce::String& line)
    {
        juce::Logger::writeToLog(line);
        const juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                   .getChildFile("MiniDAWLab");
        (void) dir.createDirectory();
        const juce::File logFile = dir.getChildFile("insert-dnd-diagnostic.log");
        (void) logFile.appendText(line + juce::newLine);
    }
} // namespace

PluginInsertHost::PluginInsertHost()
{
    formatManager_.addFormat(new juce::VST3PluginFormat());
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

void PluginInsertHost::moveInsertToStage(const TrackId trackId,
                                         const InsertSlotId slotId,
                                         const InsertStage newStage)
{
    logInsertDndDiag("[insert-dnd-diag] host move entry track="
                     + juce::String(static_cast<juce::int64>(trackId)) + " slot="
                     + juce::String(static_cast<juce::int64>(slotId)) + " target="
                     + juce::String(newStage == InsertStage::Pre ? "pre" : "post"));

    if (trackId == kInvalidTrackId || slotId == kInvalidInsertSlotId)
    {
        logInsertDndDiag("[insert-dnd-diag] host move noop reason=invalid");
        return;
    }
    LiveInsertSlot* const live = findLiveMutable(trackId, slotId);
    if (live == nullptr)
    {
        logInsertDndDiag("[insert-dnd-diag] host move noop reason=slot-not-found");
        return;
    }
    if (live->stage == newStage)
    {
        logInsertDndDiag("[insert-dnd-diag] host move noop reason=same-stage");
        return;
    }

    const PluginTrackChain before = exportChain(trackId);
    const auto it = chains_.find(trackId);
    if (it == chains_.end())
    {
        logInsertDndDiag("[insert-dnd-diag] host move noop reason=no-chain");
        return;
    }
    auto& v = it->second;
    const auto found
        = std::find_if(v.begin(), v.end(), [&](const LiveInsertSlot& s) { return s.slotId == slotId; });
    if (found == v.end())
    {
        logInsertDndDiag("[insert-dnd-diag] host move noop reason=slot-not-found");
        return;
    }

    LiveInsertSlot moved = std::move(*found);
    v.erase(found);
    if (v.empty())
    {
        chains_.erase(trackId);
    }

    moved.stage = newStage;
    insertLiveSlotSorted(trackId, std::move(moved));

    rebuildAudioThreadMapAndPublish();
    const PluginTrackChain after = exportChain(trackId);
    logInsertDndDiag("[insert-dnd-diag] host move success beforeSlots="
                     + juce::String(static_cast<int>(before.slots.size())) + " afterSlots="
                     + juce::String(static_cast<int>(after.slots.size())));
    if (!before.chainEquals(after))
    {
        PluginUndoStepSides sides;
        sides.trackId = trackId;
        sides.before = before;
        sides.after = after;
        recordPluginSlotUndo("Move insert", sides);
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
    chains_.erase(trackId);

    std::vector<LiveInsertSlot> built;
    built.reserve(chain.slots.size());

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
            continue;
        }

        const juce::File f(desc.vst3AbsolutePath);
        if (!f.exists())
        {
            juce::Logger::writeToLog("[plugin] Missing VST3 path: " + desc.vst3AbsolutePath);
            continue;
        }

        juce::String err;
        const juce::PluginDescription pd = pickPrimaryDescription(f, formatManager_, err);
        if (pd.name.isEmpty())
        {
            juce::Logger::writeToLog("[plugin] Could not restore plugin: " + err);
            continue;
        }
        if (desc.pluginIdentifier.isNotEmpty()
            && pd.createIdentifierString() != desc.pluginIdentifier)
        {
            juce::Logger::writeToLog(
                "[plugin] Identifier mismatch for track " + juce::String((juce::int64)trackId));
        }

        const double srU = effectiveSr(sampleRate_);
        const int bsU = effectiveBs(blockSize_);
        std::unique_ptr<juce::AudioPluginInstance> inst(
            formatManager_.createPluginInstance(pd, srU, bsU, err));
        if (inst == nullptr)
        {
            juce::Logger::writeToLog("[plugin] createPluginInstance failed: " + err);
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
        if (live.instance == nullptr)
        {
            continue;
        }
        rows.push_back(
            InsertRowView{ live.slotId, live.stage, live.instance->getName() });
    }
    return rows;
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
    chains_.clear();
    nextInsertSlotId_ = 1;
    rebuildAudioThreadMapAndPublish();
}

void PluginInsertHost::evictPluginForTrackNoUndo(const TrackId trackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    eraseLastHostAppliedStateForTrack(lastHostAppliedState_, trackId);
    closeEditorsForTrack(trackId);
    if (const auto it = chains_.find(trackId); it != chains_.end())
    {
        for (auto& live : it->second)
        {
            if (live.instance != nullptr)
            {
                live.instance->releaseResources();
            }
        }
        chains_.erase(it);
    }
    rebuildAudioThreadMapAndPublish();
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
        e.slots.reserve(kv.second.size());
        for (const auto& live : kv.second)
        {
            if (live.instance == nullptr)
            {
                continue;
            }
            PluginAudioThreadMap::SlotProc sp;
            sp.processor = live.instance.get();
            sp.layoutOk = live.layoutOk;
            sp.stage = live.stage;
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
    return std::any_of(
        it->second.begin(), it->second.end(), [](const LiveInsertSlot& s) { return s.instance != nullptr; });
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
    if (trackId == kInvalidTrackId || numSamples <= 0)
    {
        return;
    }

    if (numSamples > scratch_.getNumSamples())
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

    const int n = juce::jmin(numSamples, scratch_.getNumSamples());
    if (kInsertChannels <= 0 || n <= 0 || scratchPtrs_.size() < (size_t)kInsertChannels)
    {
        return;
    }

    std::shared_ptr<const PluginAudioThreadMap> m
        = std::atomic_load_explicit(&audioThreadMap_, std::memory_order_acquire);
    if (m == nullptr)
    {
        return;
    }

    const PluginAudioThreadMap::Entry* hit = nullptr;
    for (const auto& e : m->entries)
    {
        if (e.trackId == trackId)
        {
            hit = &e;
            break;
        }
    }
    if (hit == nullptr || hit->slots.empty())
    {
        return;
    }

    juce::AudioBuffer<float> view(scratchPtrs_.data(), kInsertChannels, n);
    juce::ScopedNoDenormals noDenormals;
    for (const auto& sp : hit->slots)
    {
        if (sp.processor == nullptr || !sp.layoutOk || sp.stage != stage)
        {
            continue;
        }
        sp.processor->processBlock(view, midiScratch_);
        midiScratch_.clear();
    }
}
