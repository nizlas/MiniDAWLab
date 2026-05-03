// =============================================================================
// PluginInsertHost.cpp — VST3 instances, atomic processor map, scratch for the audio thread
// =============================================================================

#include "plugins/PluginInsertHost.h"

#include "plugins/PluginEditorWindows.h"

#include <juce_audio_basics/juce_audio_basics.h>

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
    instances_.clear();
}

void PluginInsertHost::recordPluginSlotUndo(const juce::String& label, const PluginUndoStepSides& sides)
{
    if (undoRecorder_ != nullptr && undoContext_ != nullptr)
    {
        undoRecorder_(undoContext_, label, sides);
    }
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

juce::Result PluginInsertHost::loadVst3FromFile(const TrackId trackId, const juce::File& vst3File)
{
    if (trackId == kInvalidTrackId)
    {
        return juce::Result::fail("Invalid track id.");
    }
    if (!vst3File.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    const PluginTrackSlot before = exportSlot(trackId);
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
    insertLayoutOk_[trackId] = layoutOk;
    if (!layoutOk)
    {
        logStereoLayoutFailure(trackId);
    }
    logPluginInstanceLayout("load", *inst);

    closeEditorsForTrack(trackId);
    instances_[trackId] = std::move(inst);
    rebuildAudioThreadMapAndPublish();

    const PluginTrackSlot after = exportSlot(trackId);
    PluginUndoStepSides sides;
    sides.trackId = trackId;
    sides.before = before;
    sides.after = after;
    recordPluginSlotUndo("Load VST3", sides);
    return juce::Result::ok();
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
    insertLayoutOk_.clear();
    for (auto& kv : instances_)
    {
        if (kv.second != nullptr)
        {
            kv.second->releaseResources();
        }
    }
    instances_.clear();
    rebuildAudioThreadMapAndPublish();
}

void PluginInsertHost::evictPluginForTrackNoUndo(const TrackId trackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    closeEditorsForTrack(trackId);
    if (const auto it = instances_.find(trackId); it != instances_.end())
    {
        if (it->second != nullptr)
        {
            it->second->releaseResources();
        }
        instances_.erase(it);
    }
    editorOpenState_.erase(trackId);
    insertLayoutOk_.erase(trackId);
    rebuildAudioThreadMapAndPublish();
}

void PluginInsertHost::removePlugin(const TrackId trackId)
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const PluginTrackSlot before = exportSlot(trackId);
    closeEditorsForTrack(trackId);
    const auto it = instances_.find(trackId);
    if (it != instances_.end())
    {
        if (it->second != nullptr)
        {
            it->second->releaseResources();
        }
        instances_.erase(it);
    }
    editorOpenState_.erase(trackId);
    insertLayoutOk_.erase(trackId);
    rebuildAudioThreadMapAndPublish();
    const PluginTrackSlot after = exportSlot(trackId);
    PluginUndoStepSides sides;
    sides.trackId = trackId;
    sides.before = before;
    sides.after = after;
    if (!before.slotEquals(after))
    {
        recordPluginSlotUndo("Remove VST3", sides);
    }
}

void PluginInsertHost::rebuildAudioThreadMapAndPublish()
{
    auto next = std::make_shared<PluginAudioThreadMap>();
    next->entries.reserve(instances_.size());
    for (const auto& kv : instances_)
    {
        if (kv.second == nullptr)
        {
            continue;
        }
        PluginAudioThreadMap::Entry e;
        e.trackId = kv.first;
        e.processor = kv.second.get();
        if (const auto itOk = insertLayoutOk_.find(kv.first); itOk != insertLayoutOk_.end())
        {
            e.layoutOk = itOk->second;
        }
        next->entries.push_back(e);
    }
    std::atomic_store_explicit(&audioThreadMap_, std::move(next), std::memory_order_release);
}

void PluginInsertHost::closeEditorsForTrack(const TrackId trackId)
{
    editorOpenState_.erase(trackId);
    if (auto it = editorWindows_.find(trackId); it != editorWindows_.end())
    {
        if (it->second != nullptr)
        {
            it->second->setVisible(false);
        }
        editorWindows_.erase(it);
    }
    if (auto itp = paramsWindows_.find(trackId); itp != paramsWindows_.end())
    {
        if (itp->second != nullptr)
        {
            itp->second->setVisible(false);
        }
        paramsWindows_.erase(itp);
    }
}

void PluginInsertHost::openNativeEditor(const TrackId trackId)
{
    const auto it = instances_.find(trackId);
    if (it == instances_.end() || it->second == nullptr)
    {
        return;
    }
    if (editorWindows_.find(trackId) != editorWindows_.end())
    {
        editorWindows_[trackId]->toFront(true);
        return;
    }
    juce::AudioProcessor& proc = *it->second;
    juce::AudioProcessorEditor* const rawEd = proc.createEditor();
    if (rawEd == nullptr)
    {
        return;
    }
    auto ed = std::unique_ptr<juce::AudioProcessorEditor>(rawEd);
    juce::MemoryBlock snap;
    proc.getStateInformation(snap);
    editorOpenState_[trackId] = std::move(snap);
    editorWindows_[trackId] = std::make_unique<PluginEditorWindow>(*this, trackId, proc, std::move(ed));
}

void PluginInsertHost::openGenericParamsEditor(const TrackId trackId)
{
    const auto it = instances_.find(trackId);
    if (it == instances_.end() || it->second == nullptr)
    {
        return;
    }
    if (paramsWindows_.find(trackId) != paramsWindows_.end())
    {
        paramsWindows_[trackId]->toFront(true);
        return;
    }
    juce::AudioProcessor& proc = *it->second;
    juce::MemoryBlock snap;
    proc.getStateInformation(snap);
    editorOpenState_[trackId] = std::move(snap);
    auto ed = std::make_unique<juce::GenericAudioProcessorEditor>(proc);
    paramsWindows_[trackId] = std::make_unique<PluginParamsWindow>(*this, trackId, proc, std::move(ed));
}

void PluginInsertHost::editorWindowClosing(const TrackId trackId, const bool wasGenericEditor)
{
    const auto it = instances_.find(trackId);
    if (it != instances_.end() && it->second != nullptr)
    {
        const auto st = editorOpenState_.find(trackId);
        if (st != editorOpenState_.end())
        {
            juce::MemoryBlock now;
            it->second->getStateInformation(now);
            if (now != st->second)
            {
                PluginTrackSlot beforeSlot = exportSlot(trackId);
                beforeSlot.opaqueState = st->second;
                PluginTrackSlot afterSlot = exportSlot(trackId);
                PluginUndoStepSides sides;
                sides.trackId = trackId;
                sides.before = beforeSlot;
                sides.after = afterSlot;
                recordPluginSlotUndo("Plugin parameters", sides);
            }
        }
    }
    editorOpenState_.erase(trackId);
    if (wasGenericEditor)
    {
        paramsWindows_.erase(trackId);
    }
    else
    {
        editorWindows_.erase(trackId);
    }
}

PluginTrackSlot PluginInsertHost::exportSlot(const TrackId trackId) const
{
    PluginTrackSlot s;
    const auto it = instances_.find(trackId);
    if (it == instances_.end() || it->second == nullptr)
    {
        return s;
    }
    s.occupied = true;
    const juce::PluginDescription d = it->second->getPluginDescription();
    s.vst3AbsolutePath = d.fileOrIdentifier;
    s.pluginIdentifier = d.createIdentifierString();
    it->second->getStateInformation(s.opaqueState);
    return s;
}

void PluginInsertHost::importSlot(const TrackId trackId, const PluginTrackSlot& slot)
{
    closeEditorsForTrack(trackId);
    const auto itExisting = instances_.find(trackId);
    if (itExisting != instances_.end() && itExisting->second != nullptr)
    {
        itExisting->second->releaseResources();
    }
    instances_.erase(trackId);
    insertLayoutOk_.erase(trackId);

    if (!slot.occupied)
    {
        rebuildAudioThreadMapAndPublish();
        return;
    }

    const juce::File f(slot.vst3AbsolutePath);
    if (!f.exists())
    {
        juce::Logger::writeToLog("[plugin] Missing VST3 path: " + slot.vst3AbsolutePath);
        rebuildAudioThreadMapAndPublish();
        return;
    }

    juce::String err;
    const juce::PluginDescription desc = pickPrimaryDescription(f, formatManager_, err);
    if (desc.name.isEmpty())
    {
        juce::Logger::writeToLog("[plugin] Could not restore plugin: " + err);
        rebuildAudioThreadMapAndPublish();
        return;
    }
    if (slot.pluginIdentifier.isNotEmpty()
        && desc.createIdentifierString() != slot.pluginIdentifier)
    {
        juce::Logger::writeToLog(
            "[plugin] Identifier mismatch for track " + juce::String((juce::int64)trackId));
    }

    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);
    std::unique_ptr<juce::AudioPluginInstance> inst(
        formatManager_.createPluginInstance(desc, srU, bsU, err));
    if (inst == nullptr)
    {
        juce::Logger::writeToLog("[plugin] createPluginInstance failed: " + err);
        rebuildAudioThreadMapAndPublish();
        return;
    }
    bool layoutOk = tryPrepareStereoInsert(*inst, srU, bsU);
    if (slot.opaqueState.getSize() > 0)
    {
        inst->setStateInformation(slot.opaqueState.getData(), (int)slot.opaqueState.getSize());
        if (inst->getMainBusNumInputChannels() != 2 || inst->getMainBusNumOutputChannels() != 2)
        {
            inst->prepareToPlay(srU, bsU);
        }
        layoutOk = inst->getMainBusNumInputChannels() == 2 && inst->getMainBusNumOutputChannels() == 2;
    }
    insertLayoutOk_[trackId] = layoutOk;
    if (!layoutOk)
    {
        logStereoLayoutFailure(trackId);
    }
    logPluginInstanceLayout("import", *inst);
    instances_[trackId] = std::move(inst);
    rebuildAudioThreadMapAndPublish();
}

bool PluginInsertHost::hasPluginOnTrack(const TrackId trackId) const noexcept
{
    const auto it = instances_.find(trackId);
    return it != instances_.end() && it->second != nullptr;
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

    for (auto& kv : instances_)
    {
        if (kv.second == nullptr)
        {
            continue;
        }
        const bool layoutOk = tryPrepareStereoInsert(*kv.second, sampleRate_, blockSize_);
        insertLayoutOk_[kv.first] = layoutOk;
        if (!layoutOk)
        {
            const bool alreadyWarned
                = stereoPrepareFailureOneShot_.exchange(true, std::memory_order_relaxed);
            if (!alreadyWarned)
            {
                logStereoLayoutFailure(kv.first);
            }
        }
        logPluginInstanceLayout("device", *kv.second);
    }
    rebuildAudioThreadMapAndPublish();
}

void PluginInsertHost::releaseResources()
{
    for (auto& kv : instances_)
    {
        if (kv.second != nullptr)
        {
            kv.second->releaseResources();
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
        if (e.trackId == trackId && e.processor != nullptr && e.layoutOk)
        {
            return true;
        }
    }
    return false;
}

void PluginInsertHost::audioThread_processForTrack(const TrackId trackId, const int numSamples) noexcept
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
        if (e.trackId == trackId && e.processor != nullptr)
        {
            hit = &e;
            break;
        }
    }
    if (hit == nullptr || !hit->layoutOk)
    {
        return;
    }

    juce::AudioBuffer<float> view(scratchPtrs_.data(), kInsertChannels, n);
    juce::ScopedNoDenormals noDenormals;
    hit->processor->processBlock(view, midiScratch_);
    midiScratch_.clear();
}
