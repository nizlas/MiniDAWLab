// =============================================================================
// ExperimentalInstrumentHost.cpp — global instrument slot for I1 feasibility
// =============================================================================
//
// Include juce_audio_basics before our header so MidiBuffer/Message resolve before
// juce_audio_processors + VST3 SDK (can confuse MSVC's `juce::` lookup).
// MIDI from UI uses CriticalSection + MidiBuffer, not MidiMessageCollector (same SDK issue).
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include "plugins/ExperimentalInstrumentHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <exception>

namespace
{
    [[nodiscard]] juce::File getExperimentalInstrumentLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("experimental-instrument.log");
    }

    [[nodiscard]] juce::File getExperimentalVst3ScanDiagnosticLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("experimental-vst3-scan-diagnostic.log");
    }

    void initExperimentalInstrumentSessionLog()
    {
        try
        {
            const juce::File f = getExperimentalInstrumentLogFile();
            if (!f.getParentDirectory().createDirectory())
            {
                if (!f.getParentDirectory().isDirectory())
                {
                    return;
                }
            }
            const juce::String ts = juce::Time::getCurrentTime().toISO8601(true);
            const juce::String header
                = ts + " === experimental instrument host session start ===\n";
            (void)f.replaceWithText(header, false, false);
        }
        catch (...)
        {
        }
    }

    void writeExperimentalInstrumentLogLine(const juce::String& message)
    {
        try
        {
            const juce::File f = getExperimentalInstrumentLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String ts = juce::Time::getCurrentTime().toISO8601(true);
            const juce::String line = ts + " " + message + "\n";
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
    }

    /// Scan / load boundary lines: append and close the stream each time so the log is not left buffered.
    void writeExperimentalInstrumentScanBoundaryLine(const juce::String& message)
    {
        try
        {
            const juce::File f = getExperimentalInstrumentLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
            const char* const utf8 = line.toRawUTF8();
            const size_t numBytes = (size_t)line.getNumBytesAsUTF8();
            if (utf8 != nullptr && numBytes > 0U)
            {
                if (f.appendData(utf8, numBytes))
                {
                    return;
                }
            }
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
    }

    void writeScanDiagnosticScanBoundaryLine(const juce::String& message)
    {
        try
        {
            const juce::File f = getExperimentalVst3ScanDiagnosticLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
            const char* const utf8 = line.toRawUTF8();
            const size_t numBytes = (size_t)line.getNumBytesAsUTF8();
            if (utf8 != nullptr && numBytes > 0U)
            {
                if (f.appendData(utf8, numBytes))
                {
                    return;
                }
            }
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] juce::String formatInstrumentHostThreadLine(const juce::String& prefix)
    {
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        const bool isMsgThread = mm != nullptr && mm->isThisTheMessageThread();
        return prefix + " threadId=0x"
             + juce::String::toHexString((juce::int64)(juce::pointer_sized_int)juce::Thread::getCurrentThreadId())
             + " MessageManager::isThisTheMessageThread()="
             + juce::String(isMsgThread ? "true" : "false");
    }

    /// Shared `findAllTypesForFile` loop + flushed BEFORE/AFTER boundary lines.
    template <typename WriteBoundary>
    void runVst3FindAllTypesForFileLoop(const juce::String& pathOrId,
                                        juce::AudioPluginFormatManager& fm,
                                        juce::OwnedArray<juce::PluginDescription>& list,
                                        WriteBoundary&& writeBoundary)
    {
        writeBoundary(formatInstrumentHostThreadLine("scan: boundary before findAllTypesForFile (enter scan) path=\""
                                                     + pathOrId + "\""));

        bool findAllTypesWasInvoked = false;
        for (int i = 0; i < fm.getNumFormats(); ++i)
        {
            juce::AudioPluginFormat* const fmt = fm.getFormat(i);
            if (fmt == nullptr || !fmt->fileMightContainThisPluginType(pathOrId))
            {
                continue;
            }

            const juce::String fmtName = fmt->getName();
            writeBoundary("findAllTypesForFile: BEFORE format=\"" + fmtName + "\" path=\"" + pathOrId
                          + "\" accumulatedCount=" + juce::String(list.size()));

            findAllTypesWasInvoked = true;
            fmt->findAllTypesForFile(list, pathOrId);

            writeBoundary("findAllTypesForFile: AFTER format=\"" + fmtName + "\" path=\"" + pathOrId
                          + "\" accumulatedCount=" + juce::String(list.size()));
        }

        if (list.isEmpty())
        {
            writeBoundary("findAllTypesForFile: scan finished with ZERO descriptions path=\"" + pathOrId
                          + "\" findAllTypesWasInvoked=" + juce::String(findAllTypesWasInvoked ? "true" : "false"));
        }
        else
        {
            writeBoundary("findAllTypesForFile: scan finished with non-zero descriptions count="
                          + juce::String(list.size()) + " path=\"" + pathOrId + "\"");
        }
    }

    [[nodiscard]] juce::String summarizeProcessorBuses(const juce::AudioProcessor& proc)
    {
        const juce::AudioProcessor::BusesLayout lay = proc.getBusesLayout();
        juce::String s = "buses inCount=" + juce::String(lay.inputBuses.size())
                         + " outCount=" + juce::String(lay.outputBuses.size());
        if (!lay.inputBuses.isEmpty())
        {
            s << " inCh[0]=" << lay.inputBuses.getReference(0).size();
        }
        if (!lay.outputBuses.isEmpty())
        {
            s << " outCh[0]=" << lay.outputBuses.getReference(0).size();
        }
        s << " totalIn=" << proc.getTotalNumInputChannels() << " totalOut=" << proc.getTotalNumOutputChannels()
          << " mainIn=" << proc.getMainBusNumInputChannels() << " mainOut=" << proc.getMainBusNumOutputChannels();
        return s;
    }

    [[nodiscard]] juce::PluginDescription scanVst3FileAndLogDescriptions(const juce::File& vst3File,
                                                                         juce::AudioPluginFormatManager& fm,
                                                                         juce::String& err)
    {
        try
        {
            err.clear();
            const juce::String pathOrId = vst3File.getFullPathName();
            juce::OwnedArray<juce::PluginDescription> list;

            runVst3FindAllTypesForFileLoop(
                pathOrId,
                fm,
                list,
                [](const juce::String& msg) { writeExperimentalInstrumentScanBoundaryLine(msg); });

            if (list.isEmpty())
            {
                err = "VST3 scan found no plugin types for this file.";
                return {};
            }

            writeExperimentalInstrumentLogLine(
                "findAllTypesForFile: path=\"" + pathOrId + "\" count=" + juce::String(list.size()));
            for (int i = 0; i < list.size(); ++i)
            {
                const juce::PluginDescription* d = list[i];
                if (d == nullptr)
                {
                    writeExperimentalInstrumentLogLine(
                        "  [" + juce::String(i) + "] (null description entry)");
                    continue;
                }
                juce::String line = "  [" + juce::String(i) + "] name=\"" + d->name + "\"";
                if (d->descriptiveName.isNotEmpty() && d->descriptiveName != d->name)
                {
                    line << " descriptiveName=\"" << d->descriptiveName << "\"";
                }
                if (d->category.isNotEmpty())
                {
                    line << " category=\"" << d->category << "\"";
                }
                if (d->manufacturerName.isNotEmpty())
                {
                    line << " manufacturer=\"" << d->manufacturerName << "\"";
                }
                writeExperimentalInstrumentLogLine(line);
            }

            return *list.getFirst();
        }
        catch (const std::exception& e)
        {
            writeExperimentalInstrumentScanBoundaryLine(
                juce::String("scan: EXCEPTION std::exception what=\"") + e.what() + "\"");
            err = "VST3 scan failed: " + juce::String(e.what());
            return {};
        }
        catch (...)
        {
            writeExperimentalInstrumentScanBoundaryLine("scan: EXCEPTION unknown (non-std)");
            err = "VST3 scan failed: unknown exception.";
            return {};
        }
    }

    [[nodiscard]] double effectiveSr(const double sr) noexcept
    {
        return sr > 0.0 ? sr : 48000.0;
    }

    [[nodiscard]] int effectiveBs(const int bs) noexcept
    {
        return bs > 0 ? bs : 512;
    }

    /// [Audio thread] Sum first stereo pair of scratch into device (same mono blend rule as PlaybackEngine).
    void addFirstStereoBusToDeviceOutputs(const float* L,
                                         const float* R,
                                         int run,
                                         int numOutputChannels,
                                         float* const* outputChannelData,
                                         float gain) noexcept
    {
        if (L == nullptr || R == nullptr || run <= 0 || numOutputChannels <= 0 || outputChannelData == nullptr)
        {
            return;
        }
        if (numOutputChannels == 1)
        {
            float* d = outputChannelData[0];
            if (d != nullptr)
            {
                const float halfGain = 0.5f * gain;
                juce::FloatVectorOperations::addWithMultiply(d, L, halfGain, run);
                juce::FloatVectorOperations::addWithMultiply(d, R, halfGain, run);
            }
        }
        else
        {
            if (float* d0 = outputChannelData[0])
            {
                juce::FloatVectorOperations::addWithMultiply(d0, L, gain, run);
            }
            if (numOutputChannels >= 2)
            {
                if (float* d1 = outputChannelData[1])
                {
                    juce::FloatVectorOperations::addWithMultiply(d1, R, gain, run);
                }
            }
        }
    }

    class ExperimentalPluginEditorWindow final : public juce::DocumentWindow
    {
    public:
        ExperimentalPluginEditorWindow(ExperimentalInstrumentHost& host,
                                         juce::AudioProcessor& proc,
                                         std::unique_ptr<juce::AudioProcessorEditor> editor)
            : DocumentWindow(proc.getName().isNotEmpty() ? proc.getName() : "Instrument",
                             juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                                 juce::ResizableWindow::backgroundColourId),
                             DocumentWindow::closeButton)
            , host_(host)
        {
            juce::ignoreUnused(proc);
            setUsingNativeTitleBar(true);
            setContentOwned(editor.release(), true);
            if (auto* c = getContentComponent())
            {
                const int tw = juce::jmax(200, c->getWidth());
                const int th = juce::jmax(120, c->getHeight());
                centreWithSize(tw + 20, th + 20);
            }
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            setVisible(false);
            ExperimentalInstrumentHost* const h = &host_;
            juce::MessageManager::callAsync([h] {
                if (h != nullptr)
                {
                    h->editorWindowClosing();
                }
            });
        }

    private:
        ExperimentalInstrumentHost& host_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPluginEditorWindow)
    };
} // namespace

struct ExperimentalInstrumentHost::MidiIoState
{
    juce::CriticalSection midiLock;
    /// Messages from UI with sample offset 0 (start of upcoming audio block).
    juce::MidiBuffer uiPendingMidi;
};

struct ExperimentalInstrumentHost::TestKickNoteOffTimer final : juce::Timer
{
    explicit TestKickNoteOffTimer(ExperimentalInstrumentHost& ownerIn) noexcept
        : owner(ownerIn)
    {
    }

    void timerCallback() override
    {
        static constexpr int kChannel = 1;
        static constexpr int kNote = 36;
        owner.queueMidiFromMessageThread(::juce::MidiMessage::noteOff(kChannel, kNote, 0.0f));
        stopTimer();
    }

    ExperimentalInstrumentHost& owner;
};

ExperimentalInstrumentHost::ExperimentalInstrumentHost()
    : midiIo_(std::make_unique<MidiIoState>())
{
    initExperimentalInstrumentSessionLog();
    formatManager_.addFormat(new juce::VST3PluginFormat());
    auto empty = std::shared_ptr<InstrumentOwner>{};
    std::atomic_store_explicit(&activeOwner_, empty, std::memory_order_release);
}

ExperimentalInstrumentHost::~ExperimentalInstrumentHost()
{
    unloadInstrument();
    writeExperimentalInstrumentLogLine("shutdown: ExperimentalInstrumentHost destroyed");
}

void ExperimentalInstrumentHost::closeNativeEditor()
{
    editorWindow_.reset();
}

void ExperimentalInstrumentHost::editorWindowClosing()
{
    writeExperimentalInstrumentLogLine("native editor: window closing (released)");
    editorWindow_.reset();
}

void ExperimentalInstrumentHost::queueMidiFromMessageThread(const ::juce::MidiMessage& message)
{
    if (midiIo_ == nullptr)
    {
        return;
    }
    const juce::ScopedLock sl(midiIo_->midiLock);
    midiIo_->uiPendingMidi.addEvent(message, 0);
}

void ExperimentalInstrumentHost::enqueueMidiMessageFromMessageThread(const juce::MidiMessage& message)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }
    if (!hasInstrument())
    {
        return;
    }
    queueMidiFromMessageThread(message);
}

bool ExperimentalInstrumentHost::tryPrepareInstrumentLayout(juce::AudioPluginInstance& inst,
                                                            const double sampleRate,
                                                            const int blockSize)
{
    writeExperimentalInstrumentScanBoundaryLine(
        formatInstrumentHostThreadLine("bus negotiation: BEFORE " + summarizeProcessorBuses(inst)));

    const int beforeMainIn = inst.getMainBusNumInputChannels();
    const int beforeMainOut = inst.getMainBusNumOutputChannels();
    const bool acceptExisting = (beforeMainIn == 0 && beforeMainOut >= kStereoChannels);

    writeExperimentalInstrumentScanBoundaryLine(
        "bus negotiation: decision acceptExisting=" + juce::String(acceptExisting ? "true" : "false")
        + " beforeMainIn=" + juce::String(beforeMainIn) + " beforeMainOut=" + juce::String(beforeMainOut));

    const double srU = effectiveSr(sampleRate);
    const int bsU = effectiveBs(blockSize);

    if (!acceptExisting)
    {
        writeExperimentalInstrumentScanBoundaryLine("bus negotiation: BEFORE releaseResources (coerce path)");
        inst.releaseResources();
        writeExperimentalInstrumentScanBoundaryLine("bus negotiation: AFTER  releaseResources (coerce path)");

        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: BEFORE setPlayConfigDetails(0, stereo, sr, bs) (coerce path)");
        inst.setPlayConfigDetails(0, kStereoChannels, srU, bsU);
        writeExperimentalInstrumentScanBoundaryLine("bus negotiation: AFTER  setPlayConfigDetails (coerce path)");

        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::disabled());
        layout.outputBuses.add(juce::AudioChannelSet::stereo());

        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: BEFORE setBusesLayout(0-in disabled, stereo main out)");
        const bool layoutSetOk = inst.setBusesLayout(layout);
        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: AFTER  setBusesLayout ok=" + juce::String(layoutSetOk ? "true" : "false"));

        if (!layoutSetOk)
        {
            writeExperimentalInstrumentScanBoundaryLine(
                "bus negotiation: BEFORE setPlayConfigDetails(stereo) recovery (coerce path)");
            inst.setPlayConfigDetails(0, kStereoChannels, srU, bsU);
            writeExperimentalInstrumentScanBoundaryLine(
                "bus negotiation: AFTER  setPlayConfigDetails recovery (coerce path)");
        }
    }
    else
    {
        writeExperimentalInstrumentScanBoundaryLine(
            "bus negotiation: SKIP setBusesLayout (accept existing instrument layout: 0-in, mainOut>=2)");
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "bus negotiation: BEFORE prepareToPlay sampleRate=" + juce::String(srU, 6)
        + " blockSize=" + juce::String(bsU));
    inst.prepareToPlay(srU, bsU);
    writeExperimentalInstrumentScanBoundaryLine("bus negotiation: AFTER  prepareToPlay");

    const int mainIn = inst.getMainBusNumInputChannels();
    const int mainOut = inst.getMainBusNumOutputChannels();
    const bool layoutOk = mainOut >= kStereoChannels;
    writeExperimentalInstrumentScanBoundaryLine(
        "bus negotiation: AFTER  mainIn=" + juce::String(mainIn) + " mainOut=" + juce::String(mainOut)
        + " " + summarizeProcessorBuses(inst) + " layoutOk=" + juce::String(layoutOk ? "true" : "false"));
    return layoutOk;
}

juce::Result ExperimentalInstrumentHost::loadInstrumentFromVst3File(const juce::File& vst3File)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail("Internal error: load must run on the message thread.");
    }
    if (!vst3File.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    unloadInstrument();

    writeExperimentalInstrumentScanBoundaryLine(
        "load: selected VST3 path=\"" + vst3File.getFullPathName() + "\"");

    juce::String err;
    const juce::PluginDescription desc = scanVst3FileAndLogDescriptions(vst3File, formatManager_, err);
    if (err.isNotEmpty() || desc.name.isEmpty())
    {
        writeExperimentalInstrumentLogLine(
            "load: scan failed err=\"" + (err.isNotEmpty() ? err : juce::String{ "empty description" }) + "\"");
        return juce::Result::fail(err.isNotEmpty() ? err : "Could not read VST3 description.");
    }

    juce::String chosenLine = "load: chosen PluginDescription name=\"" + desc.name + "\"";
    if (desc.descriptiveName.isNotEmpty())
    {
        chosenLine << " descriptiveName=\"" << desc.descriptiveName << "\"";
    }
    if (desc.pluginFormatName.isNotEmpty())
    {
        chosenLine << " format=\"" << desc.pluginFormatName << "\"";
    }
    if (desc.fileOrIdentifier.isNotEmpty())
    {
        chosenLine << " fileOrIdentifier=\"" << desc.fileOrIdentifier << "\"";
    }
    if (desc.category.isNotEmpty())
    {
        chosenLine << " category=\"" << desc.category << "\"";
    }
    if (desc.manufacturerName.isNotEmpty())
    {
        chosenLine << " manufacturer=\"" << desc.manufacturerName << "\"";
    }
    writeExperimentalInstrumentLogLine(chosenLine);

    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);

    writeExperimentalInstrumentScanBoundaryLine(
        formatInstrumentHostThreadLine("createPluginInstance: START sampleRate=" + juce::String(srU, 6)
                                       + " blockSize=" + juce::String(bsU)));

    std::unique_ptr<juce::AudioPluginInstance> inst;
    try
    {
        inst = formatManager_.createPluginInstance(desc, srU, bsU, err);
    }
    catch (const std::exception& e)
    {
        const juce::String msg = juce::String{ "Exception during createPluginInstance: " } + e.what();
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=\"" + juce::String(e.what()) + "\"");
        return juce::Result::fail(msg);
    }
    catch (...)
    {
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=(unknown)");
        return juce::Result::fail("Unknown exception during createPluginInstance.");
    }

    if (inst == nullptr)
    {
        const juce::String juceErr = err.isNotEmpty() ? err : juce::String{ "createPluginInstance failed (no message)." };
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED juceError=\"" + juceErr + "\"");
        return juce::Result::fail(err.isNotEmpty() ? err : "createPluginInstance failed.");
    }

    writeExperimentalInstrumentLogLine(
        "createPluginInstance: OK instanceName=\"" + inst->getName() + "\"");

    const bool layoutOk = tryPrepareInstrumentLayout(*inst, sampleRate_, blockSize_);
    if (!layoutOk)
    {
        writeExperimentalInstrumentLogLine("load: FAILED bus layout after prepare (see bus negotiation lines above).");
        inst->releaseResources();
        return juce::Result::fail(
            "Could not use instrument bus layout (need at least stereo main output). "
            "Plugin reports main bus output channels: "
            + juce::String(inst->getMainBusNumOutputChannels()));
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: BEFORE active slot publish + scratch alloc totalOut=" + juce::String(inst->getTotalNumOutputChannels()));

    auto owner = std::make_shared<InstrumentOwner>();
    owner->inst = std::move(inst);
    owner->layoutOk = true;

    std::atomic_store_explicit(&activeOwner_, owner, std::memory_order_release);

    const int totalCh = owner->inst->getTotalNumOutputChannels();
    const int scratchCh = juce::jmax(kStereoChannels, totalCh);
    scratch_.setSize(scratchCh, blockSize_, false, true, true);
    scratchPtrs_.clear();
    scratchPtrs_.reserve((size_t)scratchCh);
    for (int c = 0; c < scratchCh; ++c)
    {
        scratchPtrs_.push_back(scratch_.getWritePointer(c));
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: AFTER  active slot publish + scratch alloc scratchCh=" + juce::String(scratchCh)
        + " blockSize=" + juce::String(blockSize_));

    juce::Logger::writeToLog(
        juce::String{ "[experimental-instrument] loaded " } + owner->inst->getName()
        + " totalOutCh=" + juce::String(totalCh));

    writeExperimentalInstrumentLogLine(
        "load: COMPLETED OK plugin=\"" + owner->inst->getName() + "\" totalOutCh=" + juce::String(totalCh));

    lastLoadedVst3OriginalPath_ = vst3File.getFullPathName();
    return juce::Result::ok();
}

juce::String ExperimentalInstrumentHost::getLastLoadedVst3OriginalPath() const noexcept
{
    return lastLoadedVst3OriginalPath_;
}

void ExperimentalInstrumentHost::appendInstrumentHostLogLine(const juce::String& message)
{
    writeExperimentalInstrumentLogLine(message);
}

juce::String ExperimentalInstrumentHost::getCurrentInstrumentStateBase64() const
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return {};
    }

    writeExperimentalInstrumentLogLine("plugin-state: save begin");

    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr || !o->layoutOk)
    {
        writeExperimentalInstrumentLogLine("plugin-state: save skipped reason=no-instrument");
        return {};
    }

    juce::MemoryBlock mb;
    o->inst->getStateInformation(mb);
    const juce::int64 nBytes = (juce::int64)mb.getSize();
    if (nBytes <= 0)
    {
        writeExperimentalInstrumentLogLine("plugin-state: save ok bytes=0 base64Chars=0");
        return {};
    }

    const juce::String b64 = juce::Base64::toBase64(mb.getData(), (int)mb.getSize());
    writeExperimentalInstrumentLogLine("plugin-state: save ok bytes=" + juce::String(nBytes)
                                       + " base64Chars=" + juce::String(b64.length()));
    return b64;
}

juce::Result ExperimentalInstrumentHost::loadInstrumentFromDescription(const juce::PluginDescription& descIn,
                                                                         const juce::File& originalPath,
                                                                         const char* sourceTag,
                                                                         const juce::MemoryBlock* pluginStateToRestore,
                                                                         juce::String* outPluginStateRestoreWarning)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail("Internal error: load must run on the message thread.");
    }
    if (!originalPath.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    unloadInstrument();

    juce::PluginDescription desc = descIn;
    if (desc.fileOrIdentifier.isEmpty())
    {
        desc.fileOrIdentifier = originalPath.getFullPathName();
    }

    const juce::String sourceStr
        = (sourceTag != nullptr && sourceTag[0] != '\0') ? juce::String(sourceTag) : juce::String("oop-description");
    writeExperimentalInstrumentLogLine(
        "load: source=" + sourceStr + " name=\"" + desc.name + "\" manufacturer=\"" + desc.manufacturerName
        + "\" format=\"" + desc.pluginFormatName + "\" fileOrIdentifier=\"" + desc.fileOrIdentifier
        + "\" originalPath=\"" + originalPath.getFullPathName() + "\"");

    juce::String chosenLine = "load: chosen PluginDescription name=\"" + desc.name + "\"";
    if (desc.descriptiveName.isNotEmpty())
    {
        chosenLine << " descriptiveName=\"" << desc.descriptiveName << "\"";
    }
    if (desc.pluginFormatName.isNotEmpty())
    {
        chosenLine << " format=\"" << desc.pluginFormatName << "\"";
    }
    if (desc.fileOrIdentifier.isNotEmpty())
    {
        chosenLine << " fileOrIdentifier=\"" << desc.fileOrIdentifier << "\"";
    }
    if (desc.category.isNotEmpty())
    {
        chosenLine << " category=\"" << desc.category << "\"";
    }
    if (desc.manufacturerName.isNotEmpty())
    {
        chosenLine << " manufacturer=\"" << desc.manufacturerName << "\"";
    }
    writeExperimentalInstrumentLogLine(chosenLine);

    const double srU = effectiveSr(sampleRate_);
    const int bsU = effectiveBs(blockSize_);

    writeExperimentalInstrumentScanBoundaryLine(
        formatInstrumentHostThreadLine("createPluginInstance: START sampleRate=" + juce::String(srU, 6)
                                       + " blockSize=" + juce::String(bsU)));

    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst;
    try
    {
        inst = formatManager_.createPluginInstance(desc, srU, bsU, err);
    }
    catch (const std::exception& e)
    {
        const juce::String msg = juce::String{ "Exception during createPluginInstance: " } + e.what();
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=\"" + juce::String(e.what()) + "\"");
        return juce::Result::fail(msg);
    }
    catch (...)
    {
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED exception=(unknown)");
        return juce::Result::fail("Unknown exception during createPluginInstance.");
    }

    if (inst == nullptr)
    {
        const juce::String juceErr = err.isNotEmpty() ? err : juce::String{ "createPluginInstance failed (no message)." };
        writeExperimentalInstrumentLogLine("createPluginInstance: FAILED juceError=\"" + juceErr + "\"");
        return juce::Result::fail(err.isNotEmpty() ? err : "createPluginInstance failed.");
    }

    writeExperimentalInstrumentLogLine(
        "createPluginInstance: OK instanceName=\"" + inst->getName() + "\"");

    bool finalLayoutOk = tryPrepareInstrumentLayout(*inst, sampleRate_, blockSize_);
    if (!finalLayoutOk)
    {
        writeExperimentalInstrumentLogLine("load: FAILED bus layout after prepare (see bus negotiation lines above).");
        inst->releaseResources();
        return juce::Result::fail(
            "Could not use instrument bus layout (need at least stereo main output). "
            "Plugin reports main bus output channels: "
            + juce::String(inst->getMainBusNumOutputChannels()));
    }

    if (pluginStateToRestore != nullptr && pluginStateToRestore->getSize() > 0)
    {
        writeExperimentalInstrumentLogLine(
            "plugin-state: restore begin bytes=" + juce::String((juce::int64)pluginStateToRestore->getSize()));
        bool restoreOk = false;
        try
        {
            inst->setStateInformation(pluginStateToRestore->getData(), (int)pluginStateToRestore->getSize());
            if (inst->getMainBusNumOutputChannels() < kStereoChannels)
            {
                inst->prepareToPlay(srU, bsU);
            }
            restoreOk = inst->getMainBusNumOutputChannels() >= kStereoChannels;
            if (restoreOk)
            {
                writeExperimentalInstrumentLogLine("plugin-state: restore ok");
            }
        }
        catch (const std::exception& e)
        {
            writeExperimentalInstrumentLogLine(
                "plugin-state: restore failed message=\"" + juce::String(e.what()) + "\"");
            if (outPluginStateRestoreWarning != nullptr)
            {
                *outPluginStateRestoreWarning
                    = "Groove Agent could not apply saved plug-in state (" + juce::String(e.what())
                      + "). You may need to load the kit manually if audio is silent.";
            }
        }
        catch (...)
        {
            writeExperimentalInstrumentLogLine("plugin-state: restore failed message=\"unknown exception\"");
            if (outPluginStateRestoreWarning != nullptr)
            {
                *outPluginStateRestoreWarning = "Groove Agent could not apply saved plug-in state (unknown error). "
                                                "You may need to load the kit manually if audio is silent.";
            }
        }

        if (!restoreOk && outPluginStateRestoreWarning != nullptr && outPluginStateRestoreWarning->isEmpty())
        {
            writeExperimentalInstrumentLogLine(
                "plugin-state: restore failed message=\"invalid output bus layout after setState\"");
            *outPluginStateRestoreWarning
                = "Groove Agent saved state could not be applied (invalid bus layout after restore). "
                  "You may need to load the kit manually if audio is silent.";
        }

        if (!restoreOk)
        {
            writeExperimentalInstrumentLogLine("plugin-state: reset to default patch after failed restore");
            inst->releaseResources();
            finalLayoutOk = tryPrepareInstrumentLayout(*inst, sampleRate_, blockSize_);
            if (!finalLayoutOk)
            {
                writeExperimentalInstrumentLogLine(
                    "load: FAILED bus layout after failed state restore + reset (see bus negotiation lines above).");
                inst->releaseResources();
                return juce::Result::fail(
                    "Could not use instrument bus layout after attempted state restore reset. "
                    "Plugin reports main bus output channels: "
                    + juce::String(inst->getMainBusNumOutputChannels()));
            }
        }
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: BEFORE active slot publish + scratch alloc totalOut=" + juce::String(inst->getTotalNumOutputChannels()));

    auto owner = std::make_shared<InstrumentOwner>();
    owner->inst = std::move(inst);
    owner->layoutOk = finalLayoutOk;

    std::atomic_store_explicit(&activeOwner_, owner, std::memory_order_release);

    const int totalCh = owner->inst->getTotalNumOutputChannels();
    const int scratchCh = juce::jmax(kStereoChannels, totalCh);
    scratch_.setSize(scratchCh, blockSize_, false, true, true);
    scratchPtrs_.clear();
    scratchPtrs_.reserve((size_t)scratchCh);
    for (int c = 0; c < scratchCh; ++c)
    {
        scratchPtrs_.push_back(scratch_.getWritePointer(c));
    }

    writeExperimentalInstrumentScanBoundaryLine(
        "load: AFTER  active slot publish + scratch alloc scratchCh=" + juce::String(scratchCh)
        + " blockSize=" + juce::String(blockSize_));

    juce::Logger::writeToLog(
        juce::String{ "[experimental-instrument] loaded " } + owner->inst->getName()
        + " totalOutCh=" + juce::String(totalCh));

    writeExperimentalInstrumentLogLine(
        "load: COMPLETED OK plugin=\"" + owner->inst->getName() + "\" totalOutCh=" + juce::String(totalCh));

    lastLoadedVst3OriginalPath_ = originalPath.getFullPathName();
    return juce::Result::ok();
}

juce::Result ExperimentalInstrumentHost::diagnosticScanVst3FileOnly(const juce::File& vst3File)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return juce::Result::fail("Internal error: scan diagnostic must run on the message thread.");
    }
    if (!vst3File.exists())
    {
        return juce::Result::fail("VST3 file or bundle does not exist.");
    }

    const juce::String pathOrId = vst3File.getFullPathName();
    writeExperimentalInstrumentLogLine(
        "SCAN_DIAGNOSTIC: scan-only (no instance); details in experimental-vst3-scan-diagnostic.log path=\""
        + pathOrId + "\"");

    writeScanDiagnosticScanBoundaryLine("=== SCAN_DIAGNOSTIC run begin path=\"" + pathOrId + "\" ===");
    writeScanDiagnosticScanBoundaryLine(formatInstrumentHostThreadLine("SCAN_DIAGNOSTIC: context"));

    juce::OwnedArray<juce::PluginDescription> list;
    try
    {
        runVst3FindAllTypesForFileLoop(
            pathOrId,
            formatManager_,
            list,
            [](const juce::String& msg) { writeScanDiagnosticScanBoundaryLine(msg); });

        juce::String summary = "SCAN_DIAGNOSTIC run end outcome=completed reachedAfterAllFormats=true descriptionCount="
                               + juce::String(list.size());
        for (int i = 0; i < list.size(); ++i)
        {
            const juce::PluginDescription* d = list[i];
            if (d == nullptr)
            {
                summary << " [" << i << "]=null";
            }
            else
            {
                summary << " [" << i << "]=\"" << d->name << "\"";
            }
        }
        writeScanDiagnosticScanBoundaryLine(summary + " path=\"" + pathOrId + "\"");
        return juce::Result::ok();
    }
    catch (const std::exception& e)
    {
        writeScanDiagnosticScanBoundaryLine(
            juce::String("SCAN_DIAGNOSTIC run end outcome=exception std::exception=\"") + e.what() + "\" path=\""
            + pathOrId + "\"");
        return juce::Result::fail(juce::String("Scan threw: ") + e.what());
    }
    catch (...)
    {
        writeScanDiagnosticScanBoundaryLine(
            "SCAN_DIAGNOSTIC run end outcome=exception unknown path=\"" + pathOrId + "\"");
        return juce::Result::fail("Scan threw a non-std exception.");
    }
}

void ExperimentalInstrumentHost::unloadInstrument()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine("unload: requested");

    lastLoadedVst3OriginalPath_.clear();

    closeNativeEditor();
    if (testKickNoteOffTimer_ != nullptr)
    {
        testKickNoteOffTimer_->stopTimer();
        testKickNoteOffTimer_.reset();
    }

    std::shared_ptr<InstrumentOwner> prev = std::atomic_exchange_explicit(
        &activeOwner_, std::shared_ptr<InstrumentOwner>{}, std::memory_order_acq_rel);

    juce::Thread::sleep(100);

    if (prev != nullptr && prev->inst != nullptr)
    {
        prev->inst->releaseResources();
    }

    prev.reset();
    scratch_.setSize(0, 0, false, true, false);
    scratchPtrs_.clear();

    juce::Logger::writeToLog("[experimental-instrument] unloaded (global slot)");

    writeExperimentalInstrumentLogLine("unload: completed");
}

bool ExperimentalInstrumentHost::hasInstrument() const noexcept
{
    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    return o != nullptr && o->inst != nullptr && o->layoutOk;
}

juce::String ExperimentalInstrumentHost::getInstrumentNameForUi() const
{
    auto o = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (o == nullptr || o->inst == nullptr)
    {
        return {};
    }
    return o->inst->getName();
}

void ExperimentalInstrumentHost::openNativeEditor()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine("native editor: open requested");

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner == nullptr || owner->inst == nullptr || !owner->layoutOk)
    {
        writeExperimentalInstrumentLogLine("native editor: FAILED (no instrument loaded)");
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "Experimental instrument", "No instrument is loaded.");
        return;
    }
    if (editorWindow_ != nullptr)
    {
        writeExperimentalInstrumentLogLine("native editor: toFront (already open)");
        editorWindow_->toFront(true);
        return;
    }
    std::unique_ptr<juce::AudioProcessorEditor> ed(owner->inst->createEditor());
    if (ed == nullptr)
    {
        writeExperimentalInstrumentLogLine("native editor: FAILED (createEditor returned null)");
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Experimental instrument",
            "This plug-in did not provide an editor.");
        return;
    }
    editorWindow_ = std::make_unique<ExperimentalPluginEditorWindow>(*this, *owner->inst, std::move(ed));
    writeExperimentalInstrumentLogLine("native editor: open succeeded");
}

void ExperimentalInstrumentHost::triggerTestKick()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }
    if (!hasInstrument())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "Experimental instrument", "Load an instrument first.");
        return;
    }

    static constexpr int kChannel = 1;
    static constexpr int kNote = 36;
    static constexpr float kVelFloat = 0.9f;
    const int velMidi = juce::jlimit(1, 127, juce::roundToInt(kVelFloat * 127.0f));
    writeExperimentalInstrumentLogLine(
        "Test Kick: requested note=" + juce::String(kNote) + " velocityFloat=" + juce::String(kVelFloat, 4)
        + " velocityMidi~=" + juce::String(velMidi));

    queueMidiFromMessageThread(::juce::MidiMessage::noteOn(kChannel, kNote, kVelFloat));

    if (testKickNoteOffTimer_ == nullptr)
    {
        testKickNoteOffTimer_ = std::make_unique<TestKickNoteOffTimer>(*this);
    }
    testKickNoteOffTimer_->stopTimer();
    testKickNoteOffTimer_->startTimer(250);
}

void ExperimentalInstrumentHost::prepareForDevice(const double sampleRate, const int blockSize)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine(
        "prepareForDevice: sampleRate=" + juce::String(sampleRate, 6) + " blockSize=" + juce::String(blockSize));

    sampleRate_ = sampleRate;
    blockSize_ = juce::jmax(1, blockSize);
    if (midiIo_ != nullptr)
    {
        const juce::ScopedLock sl(midiIo_->midiLock);
        midiIo_->uiPendingMidi.clear();
    }

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner != nullptr && owner->inst != nullptr && owner->layoutOk)
    {
        if (!tryPrepareInstrumentLayout(*owner->inst, sampleRate_, blockSize_))
        {
            writeExperimentalInstrumentLogLine(
                "prepareForDevice: FAILED bus negotiation - unloading instrument");
            juce::Logger::writeToLog(
                "[experimental-instrument] prepareForDevice failed — unloading instrument.");
            unloadInstrument();
            owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
        }
    }

    owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner != nullptr && owner->inst != nullptr && owner->layoutOk)
    {
        const int totalCh = owner->inst->getTotalNumOutputChannels();
        const int scratchCh = juce::jmax(kStereoChannels, totalCh);
        scratch_.setSize(scratchCh, blockSize_, false, true, true);
        scratchPtrs_.clear();
        scratchPtrs_.reserve((size_t)scratchCh);
        for (int c = 0; c < scratchCh; ++c)
        {
            scratchPtrs_.push_back(scratch_.getWritePointer(c));
        }
    }
    else
    {
        scratch_.setSize(kStereoChannels, blockSize_, false, true, true);
        scratchPtrs_.clear();
        scratchPtrs_.reserve((size_t)kStereoChannels);
        for (int c = 0; c < kStereoChannels; ++c)
        {
            scratchPtrs_.push_back(scratch_.getWritePointer(c));
        }
    }
}

void ExperimentalInstrumentHost::releaseResources()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    writeExperimentalInstrumentLogLine("releaseResources: requested (device stopping)");

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner != nullptr && owner->inst != nullptr)
    {
        owner->inst->releaseResources();
        writeExperimentalInstrumentLogLine(
            "releaseResources: plugin releaseResources completed name=\"" + owner->inst->getName() + "\"");
    }
    else
    {
        writeExperimentalInstrumentLogLine("releaseResources: no loaded plugin instance");
    }
}

void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(float* const* outputChannelData,
                                                                         const int numOutputChannels,
                                                                         const int numSamples) noexcept
{
    if (numSamples <= 0 || outputChannelData == nullptr || numOutputChannels <= 0)
    {
        return;
    }

    auto owner = std::atomic_load_explicit(&activeOwner_, std::memory_order_acquire);
    if (owner == nullptr || owner->inst == nullptr || !owner->layoutOk)
    {
        return;
    }

    juce::AudioPluginInstance& inst = *owner->inst;
    const int totalCh = inst.getTotalNumOutputChannels();
    if (totalCh < kStereoChannels || scratch_.getNumChannels() < kStereoChannels
        || scratch_.getNumSamples() < numSamples)
    {
        return;
    }

    if (midiIo_ == nullptr)
    {
        return;
    }

    juce::MidiBuffer blockMidi;
    {
        const juce::ScopedLock sl(midiIo_->midiLock);
        blockMidi.addEvents(midiIo_->uiPendingMidi, 0, numSamples, 0);
        midiIo_->uiPendingMidi.clear();
    }

    const int scratchCh = juce::jmin(scratch_.getNumChannels(), juce::jmax(kStereoChannels, totalCh));
    const int n = juce::jmin(numSamples, scratch_.getNumSamples());
    for (int c = 0; c < scratchCh; ++c)
    {
        if (float* p = scratch_.getWritePointer(c))
        {
            juce::FloatVectorOperations::clear(p, n);
        }
    }

    juce::AudioBuffer<float> view(
        scratchPtrs_.empty() ? nullptr : scratchPtrs_.data(), scratchCh, n);

    {
        juce::ScopedNoDenormals noDenormals;
        inst.processBlock(view, blockMidi);
    }

    const float* L = scratch_.getReadPointer(0);
    const float* R = scratch_.getReadPointer(1);
    addFirstStereoBusToDeviceOutputs(L, R, n, numOutputChannels, outputChannelData, 1.0f);
}
