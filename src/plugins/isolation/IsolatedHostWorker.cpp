#include "plugins/isolation/IsolatedHostWorker.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <map>
#include <memory>
#include <thread>

#include <windows.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

#include "plugins/isolation/IsolatedControlCodec.h"
#include "plugins/isolation/IsolatedProtocol.h"

namespace mini_daw::isolation
{
namespace
{
[[noreturn]] void terminateSelfNow(const UINT code) noexcept
{
    // Never unwinds and never runs static destructors or plug-in destructors.
    ::TerminateProcess(::GetCurrentProcess(), code);
    for (;;)
    {
    }
}

LONG WINAPI helperUnhandledException(EXCEPTION_POINTERS* info)
{
    terminateSelfNow(info != nullptr && info->ExceptionRecord != nullptr
                         ? (UINT)info->ExceptionRecord->ExceptionCode
                         : 0xDEADu);
}

/// Playhead of a hosted instance: exactly the segment transport DAL wrote into the request header.
struct SharedPlayHead final : juce::AudioPlayHead
{
    const SharedBlockHeader* header = nullptr;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        if (header == nullptr)
        {
            return p;
        }
        const std::uint32_t f = header->transportFlags;
        p.setIsPlaying((f & kPlaying) != 0);
        p.setIsRecording((f & kRecording) != 0);
        p.setIsLooping((f & kLooping) != 0);
        if ((f & kHasTimeSamples) != 0)
        {
            p.setTimeInSamples(header->timeInSamples);
        }
        if ((f & kHasPpq) != 0)
        {
            p.setPpqPosition(header->ppqPosition);
        }
        if ((f & kHasLastBar) != 0)
        {
            p.setPpqPositionOfLastBarStart(header->ppqLastBar);
        }
        if ((f & kHasBpm) != 0)
        {
            p.setBpm(header->bpm);
        }
        if ((f & kHasTimeSig) != 0)
        {
            p.setTimeSignature(juce::AudioPlayHead::TimeSignature { header->timeSigNumerator,
                                                                    header->timeSigDenominator });
        }
        if ((f & kHasLoop) != 0)
        {
            p.setLoopPoints(juce::AudioPlayHead::LoopPoints { header->loopStartPpq, header->loopEndPpq });
        }
        return p;
    }
};

class HostWorker;

struct HostedInstance final : juce::AudioProcessorListener
{
    std::int32_t id = 0;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    HANDLE mapping = nullptr;
    HANDLE requestEvent = nullptr;
    HANDLE doneEvent = nullptr;
    HANDLE quitEvent = nullptr;
    SharedBlockHeader* header = nullptr;
    float* channelData[kChannels] {};
    SharedPlayHead playHead;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> wideScratch; // only when the plug-in has more than two channels
    std::thread processingThread;
    std::atomic<bool> dirty { false };
    std::unique_ptr<juce::DocumentWindow> editorWindow;
    // Test faults (focused tests only).
    std::atomic<int> crashAfterBlocks { -1 };
    std::atomic<int> stallNextMs { 0 };

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override
    {
        dirty.store(true, std::memory_order_relaxed);
    }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& d) override
    {
        if (d.parameterInfoChanged || d.programChanged || d.nonParameterStateChanged)
        {
            dirty.store(true, std::memory_order_relaxed);
        }
    }

    void processingLoop()
    {
        DWORD taskIndex = 0;
        HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        const HANDLE waits[2] = { requestEvent, quitEvent };
        for (;;)
        {
            const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (r != WAIT_OBJECT_0)
            {
                break;
            }
            const std::uint32_t seq = header->requestSeq.load(std::memory_order_acquire);
            if (seq == header->doneSeq.load(std::memory_order_relaxed))
            {
                continue;
            }
            const int stall = stallNextMs.exchange(0, std::memory_order_relaxed);
            if (stall == INT32_MAX)
            {
                ::WaitForSingleObject(quitEvent, INFINITE);
                break;
            }
            if (stall > 0)
            {
                ::Sleep((DWORD)stall);
            }
            const int crashIn = crashAfterBlocks.load(std::memory_order_relaxed);
            if (crashIn == 0)
            {
                *(volatile int*)nullptr = 0;
            }
            if (crashIn > 0)
            {
                crashAfterBlocks.store(crashIn - 1, std::memory_order_relaxed);
            }
            processOne(juce::jlimit(0, header->maxBlock, header->numSamples));
            header->helperProgress.fetch_add(1, std::memory_order_relaxed);
            header->doneSeq.store(seq, std::memory_order_release);
            ::SetEvent(doneEvent);
        }
        if (mmcss != nullptr)
        {
            ::AvRevertMmThreadCharacteristics(mmcss);
        }
    }

    void processOne(const int n)
    {
        if (n <= 0 || plugin == nullptr)
        {
            return;
        }
        midi.clear();
        const int total = juce::jmax(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        if (total <= kChannels)
        {
            juce::AudioBuffer<float> io(channelData, kChannels, n);
            plugin->processBlock(io, midi);
            return;
        }
        // Wider plug-in layout: run on a prepared scratch with silent extra channels.
        const int ch = juce::jmin(total, wideScratch.getNumChannels());
        juce::AudioBuffer<float> io(wideScratch.getArrayOfWritePointers(), ch, n);
        io.clear();
        for (int c = 0; c < kChannels; ++c)
        {
            io.copyFrom(c, 0, channelData[c], n);
        }
        plugin->processBlock(io, midi);
        for (int c = 0; c < kChannels; ++c)
        {
            juce::FloatVectorOperations::copy(channelData[c], io.getReadPointer(c), n);
        }
    }

    void startProcessing()
    {
        processingThread = std::thread([this] { processingLoop(); });
    }

    void stopProcessing()
    {
        if (quitEvent != nullptr)
        {
            ::SetEvent(quitEvent);
        }
        if (processingThread.joinable())
        {
            processingThread.join();
        }
    }

    void closeHandles()
    {
        if (header != nullptr)
        {
            ::UnmapViewOfFile(header);
            header = nullptr;
        }
        for (HANDLE* h : { &mapping, &requestEvent, &doneEvent, &quitEvent })
        {
            if (*h != nullptr)
            {
                ::CloseHandle(*h);
                *h = nullptr;
            }
        }
    }
};

struct EditorWindow final : juce::DocumentWindow
{
    EditorWindow(const juce::String& title, std::function<void()> onClose)
        : juce::DocumentWindow(title, juce::Colours::darkgrey, juce::DocumentWindow::closeButton),
          onClose_(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
    }
    void closeButtonPressed() override
    {
        if (onClose_)
        {
            onClose_();
        }
    }
    std::function<void()> onClose_;
};

struct PresetVisitor final : juce::ExtensionsVisitor
{
    const juce::MemoryBlock* incoming = nullptr;
    juce::MemoryBlock outgoing;
    bool sawClient = false;
    bool writeOk = false;
    void visitVST3Client(const VST3Client& c) override
    {
        sawClient = true;
        if (incoming != nullptr)
        {
            writeOk = c.setPreset(*incoming);
        }
        else
        {
            outgoing = c.getPreset();
        }
    }
};

class HostWorker final : public juce::ChildProcessWorker, private juce::Timer
{
public:
    HostWorker()
    {
        formatManager_.addFormat(new juce::VST3PluginFormat());
    }

    void handleConnectionMade() override
    {
        sendMessageToCoordinator(ControlWriter(ControlKind::Hello, 0).i32((std::int32_t)::GetCurrentProcessId()).block());
        juce::MessageManager::callAsync([this] { startTimerHz(20); });
    }

    void handleConnectionLost() override
    {
        // DAL is gone or asked us to go: leave without running any plug-in destructor.
        terminateSelfNow(0);
    }

    void handleMessageFromCoordinator(const juce::MemoryBlock& m) override
    {
        // Pipe thread: hop to the message thread, where JUCE's VST3 wrapper wants every call.
        juce::MessageManager::callAsync([this, m] { handle(m); });
    }

private:
    void reply(const std::int32_t requestId, const bool ok, const std::function<void(ControlWriter&)>& payload = {})
    {
        ControlWriter w(ControlKind::Reply, requestId);
        w.i32(ok ? 1 : 0);
        if (payload)
        {
            payload(w);
        }
        sendMessageToCoordinator(w.block());
    }

    HostedInstance* find(const std::int32_t id)
    {
        const auto it = instances_.find(id);
        return it == instances_.end() ? nullptr : it->second.get();
    }

    static bool prepareStereo(juce::AudioPluginInstance& p, const double sr, const int bs, const int maxBlock)
    {
        p.releaseResources();
        p.setPlayConfigDetails(kChannels, kChannels, sr, juce::jmax(bs, maxBlock));
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add(juce::AudioChannelSet::stereo());
        layout.outputBuses.add(juce::AudioChannelSet::stereo());
        (void)p.setBusesLayout(layout);
        p.prepareToPlay(sr, juce::jmax(bs, maxBlock));
        return p.getMainBusNumInputChannels() == 2 && p.getMainBusNumOutputChannels() == 2;
    }

    void prepareWide(HostedInstance& h)
    {
        const int total = juce::jmax(h.plugin->getTotalNumInputChannels(), h.plugin->getTotalNumOutputChannels());
        if (total > kChannels)
        {
            h.wideScratch.setSize(total, h.header->maxBlock, false, true, false);
        }
    }

    void handle(const juce::MemoryBlock& m)
    {
        ControlReader r(m);
        const std::int32_t rid = r.requestId();
        switch (r.kind())
        {
            case ControlKind::Create: return create(r, rid);
            case ControlKind::Prepare:
            {
                const auto id = r.i32();
                const double sr = r.f64();
                const int bs = r.i32();
                const bool nonRealtime = r.i32() != 0;
                HostedInstance* h = find(id);
                if (h == nullptr)
                {
                    return reply(rid, false);
                }
                // The processing thread is idle: DAL sends Prepare only with no request outstanding.
                h->plugin->setNonRealtime(nonRealtime);
                const bool ok = prepareStereo(*h->plugin, sr, bs, h->header->maxBlock);
                prepareWide(*h);
                return reply(rid, ok, [&](ControlWriter& w) {
                    w.i32(h->plugin->getLatencySamples()).f64(h->plugin->getTailLengthSeconds());
                });
            }
            case ControlKind::Release:
            {
                if (HostedInstance* h = find(r.i32()))
                {
                    h->plugin->releaseResources();
                }
                return reply(rid, true);
            }
            case ControlKind::Destroy: return destroy(r.i32(), rid);
            case ControlKind::GetState:
            {
                HostedInstance* h = find(r.i32());
                if (h == nullptr)
                {
                    return reply(rid, false);
                }
                if (hangNextGetStateMs_ > 0)
                {
                    const int ms = std::exchange(hangNextGetStateMs_, 0);
                    juce::Thread::sleep(ms);
                }
                juce::MemoryBlock state;
                h->plugin->getStateInformation(state);
                h->dirty.store(false, std::memory_order_relaxed);
                return reply(rid, true, [&](ControlWriter& w) { w.bytes(state); });
            }
            case ControlKind::SetState:
            {
                HostedInstance* h = find(r.i32());
                const juce::MemoryBlock state = r.bytes();
                if (h == nullptr || state.getSize() == 0)
                {
                    return reply(rid, false);
                }
                h->plugin->setStateInformation(state.getData(), (int)state.getSize());
                return reply(rid, true, [&](ControlWriter& w) {
                    w.i32(h->plugin->getLatencySamples()).f64(h->plugin->getTailLengthSeconds());
                });
            }
            case ControlKind::GetPreset:
            case ControlKind::SetPreset:
            {
                const bool write = r.kind() == ControlKind::SetPreset;
                HostedInstance* h = find(r.i32());
                const juce::MemoryBlock incoming = write ? r.bytes() : juce::MemoryBlock {};
                if (h == nullptr)
                {
                    return reply(rid, false);
                }
                PresetVisitor v;
                v.incoming = write ? &incoming : nullptr;
                h->plugin->getExtensions(v);
                const bool ok = v.sawClient && (write ? v.writeOk : !v.outgoing.isEmpty());
                return reply(rid, ok, [&](ControlWriter& w) { w.bytes(v.outgoing); });
            }
            case ControlKind::OpenEditor:
            {
                const auto id = r.i32();
                const juce::String title = r.str();
                return reply(rid, openEditor(id, title));
            }
            case ControlKind::CloseEditor:
            {
                closeEditor(r.i32(), false);
                return reply(rid, true);
            }
            case ControlKind::TestFault:
            {
                const auto kind = (TestFault)r.i32();
                const auto arg = r.i32();
                const auto id = r.i32();
                applyFault(kind, arg, id);
                return reply(rid, true);
            }
            case ControlKind::Ping: return reply(rid, true);
            default: return reply(rid, false);
        }
    }

    void applyFault(const TestFault kind, const std::int32_t arg, const std::int32_t id)
    {
        switch (kind)
        {
            case TestFault::CrashOnNextDestroy: crashOnNextDestroy_ = true; break;
            case TestFault::CrashOnNextCreate: crashOnNextCreate_ = true; break;
            case TestFault::CrashInProcessAfterBlocks:
                if (HostedInstance* h = find(id))
                {
                    h->crashAfterBlocks.store(arg, std::memory_order_relaxed);
                }
                break;
            case TestFault::StallNextProcess:
                if (HostedInstance* h = find(id))
                {
                    h->stallNextMs.store(arg, std::memory_order_relaxed);
                }
                break;
            case TestFault::HangNextGetState: hangNextGetStateMs_ = arg; break;
            case TestFault::NudgeParameter:
                if (HostedInstance* h = find(id))
                {
                    const auto& params = h->plugin->getParameters();
                    if (arg >= 0 && arg < params.size())
                    {
                        juce::AudioProcessorParameter* prm = params[arg];
                        prm->beginChangeGesture();
                        prm->setValueNotifyingHost(std::fmod(prm->getValue() + 0.37f, 1.0f));
                        prm->endChangeGesture();
                    }
                }
                break;
            case TestFault::CloseEditorAsUser: closeEditor(id, true); break;
            case TestFault::None: break;
        }
    }

    void create(ControlReader& r, const std::int32_t rid)
    {
        const auto id = r.i32();
        const juce::String descXml = r.str();
        const double sr = r.f64();
        const int bs = r.i32();
        const int maxBlock = r.i32();
        const juce::String sectionName = r.str();
        const juce::String requestName = r.str();
        const juce::String doneName = r.str();
        const juce::MemoryBlock state = r.bytes();

        const auto fail = [&](const juce::String& why) {
            reply(rid, false, [&](ControlWriter& w) { w.str(why); });
        };
        if (crashOnNextCreate_)
        {
            *(volatile int*)nullptr = 0;
        }
        juce::PluginDescription desc;
        const std::unique_ptr<juce::XmlElement> xml = juce::parseXML(descXml);
        if (xml == nullptr || !desc.loadFromXml(*xml) || find(id) != nullptr)
        {
            return fail("bad description");
        }

        auto h = std::make_unique<HostedInstance>();
        h->id = id;
        h->mapping = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sectionName.toWideCharPointer());
        h->requestEvent = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, requestName.toWideCharPointer());
        h->doneEvent = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, doneName.toWideCharPointer());
        h->quitEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (h->mapping == nullptr || h->requestEvent == nullptr || h->doneEvent == nullptr || h->quitEvent == nullptr)
        {
            h->closeHandles();
            return fail("could not open the shared section");
        }
        h->header = static_cast<SharedBlockHeader*>(
            ::MapViewOfFile(h->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sectionBytes(maxBlock)));
        if (h->header == nullptr || h->header->maxBlock != maxBlock)
        {
            h->closeHandles();
            return fail("could not map the shared section");
        }
        auto* samples = reinterpret_cast<float*>(reinterpret_cast<char*>(h->header) + headerBytes());
        for (int c = 0; c < kChannels; ++c)
        {
            h->channelData[c] = samples + (size_t)c * (size_t)maxBlock;
        }
        h->playHead.header = h->header;
        h->midi.ensureSize(512);

        juce::String err;
        h->plugin = formatManager_.createPluginInstance(desc, sr, juce::jmax(bs, maxBlock), err);
        if (h->plugin == nullptr)
        {
            h->closeHandles();
            return fail(err.isNotEmpty() ? err : juce::String("createPluginInstance failed"));
        }
        h->plugin->setPlayHead(&h->playHead);
        bool layoutOk = prepareStereo(*h->plugin, sr, bs, maxBlock);
        if (state.getSize() > 0)
        {
            h->plugin->setStateInformation(state.getData(), (int)state.getSize());
            if (h->plugin->getMainBusNumInputChannels() != 2 || h->plugin->getMainBusNumOutputChannels() != 2)
            {
                layoutOk = prepareStereo(*h->plugin, sr, bs, maxBlock);
            }
        }
        prepareWide(*h);
        h->plugin->addListener(h.get());
        h->startProcessing();
        HostedInstance& ref = *h;
        instances_[id] = std::move(h);
        reply(rid, true, [&](ControlWriter& w) {
            w.str({})
                .i32(ref.plugin->getLatencySamples())
                .f64(ref.plugin->getTailLengthSeconds())
                .i32(layoutOk ? 1 : 0);
        });
    }

    void destroy(const std::int32_t id, const std::int32_t rid)
    {
        const auto it = instances_.find(id);
        if (it == instances_.end())
        {
            return reply(rid, true);
        }
        std::unique_ptr<HostedInstance> h = std::move(it->second);
        instances_.erase(it);
        h->editorWindow.reset();
        h->stopProcessing();
        h->plugin->removeListener(h.get());
        if (crashOnNextDestroy_)
        {
            *(volatile int*)nullptr = 0;
        }
        h->plugin->releaseResources();
        h->plugin.reset(); // the plug-in's destructor runs here, and only here
        h->closeHandles();
        reply(rid, true);
    }

    bool openEditor(const std::int32_t id, const juce::String& title)
    {
        HostedInstance* h = find(id);
        if (h == nullptr)
        {
            return false;
        }
        if (h->editorWindow != nullptr)
        {
            h->editorWindow->toFront(true);
            return true;
        }
        juce::AudioProcessorEditor* ed = h->plugin->hasEditor() ? h->plugin->createEditorIfNeeded() : nullptr;
        if (ed == nullptr)
        {
            return false;
        }
        auto w = std::make_unique<EditorWindow>(title, [this, id] { closeEditor(id, true); });
        w->setContentOwned(ed, true);
        w->setResizable(ed->isResizable(), false);
        w->centreWithSize(ed->getWidth(), ed->getHeight());
        w->setVisible(true);
        w->toFront(true);
        h->editorWindow = std::move(w);
        return true;
    }

    void closeEditor(const std::int32_t id, const bool byUser)
    {
        HostedInstance* h = find(id);
        if (h == nullptr || h->editorWindow == nullptr)
        {
            return;
        }
        // Defer the window's destruction out of its own close-button callback.
        juce::DocumentWindow* raw = h->editorWindow.release();
        raw->setVisible(false);
        juce::MessageManager::callAsync([raw] { delete raw; });
        if (byUser)
        {
            sendMessageToCoordinator(ControlWriter(ControlKind::EditorClosed, 0).i32(id).block());
        }
    }

    void timerCallback() override
    {
        for (auto& kv : instances_)
        {
            if (kv.second->dirty.exchange(false, std::memory_order_relaxed))
            {
                sendMessageToCoordinator(ControlWriter(ControlKind::Dirty, 0).i32(kv.first).block());
            }
        }
    }

    juce::AudioPluginFormatManager formatManager_;
    std::map<std::int32_t, std::unique_ptr<HostedInstance>> instances_;
    bool crashOnNextDestroy_ = false;
    bool crashOnNextCreate_ = false;
    int hangNextGetStateMs_ = 0;
};

HostWorker* gWorker = nullptr; // intentionally leaked: the helper leaves by TerminateProcess
} // namespace

bool isIsolatedHelperCommandLine(const juce::String& commandLine)
{
    return commandLine.contains(juce::String("--") + kHelperCommandLineUniqueId + ":");
}

bool startIsolatedHelper(const juce::String& commandLine)
{
    // A crash in a hosted plug-in ends this process quietly; DAL notices and recovers.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::SetUnhandledExceptionFilter(helperUnhandledException);
    gWorker = new HostWorker();
    return gWorker->initialiseFromCommandLine(commandLine, kHelperCommandLineUniqueId, 10000);
}
} // namespace mini_daw::isolation
