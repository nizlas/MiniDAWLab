#include "plugins/isolation/IsolatedInsertHosting.h"

#include <windows.h>

#include <algorithm>

#include "plugins/isolation/IsolatedControlCodec.h"
#include "plugins/isolation/IsolatedProcessDeadline.h"

namespace mini_daw::isolation
{
// =============================================================================
// Deadlines
// =============================================================================
namespace deadline
{
namespace
{
std::atomic<std::int64_t> gCallbackDeadline { 0 };
thread_local std::int64_t tThreadDeadline = 0;
std::atomic<bool> gOffline { false };
std::atomic<bool> gOfflineFailure { false };

std::int64_t frequency() noexcept
{
    static const std::int64_t f = [] {
        LARGE_INTEGER q;
        ::QueryPerformanceFrequency(&q);
        return (std::int64_t)q.QuadPart;
    }();
    return f;
}
} // namespace

std::int64_t qpcNow() noexcept
{
    LARGE_INTEGER q;
    ::QueryPerformanceCounter(&q);
    return (std::int64_t)q.QuadPart;
}
std::int64_t qpcPerSecond() noexcept { return frequency(); }
void publishCallbackDeadline(const std::int64_t d) noexcept { gCallbackDeadline.store(d, std::memory_order_release); }
void setThreadDeadline(const std::int64_t d) noexcept { tThreadDeadline = d; }
std::int64_t currentDeadline() noexcept
{
    return tThreadDeadline != 0 ? tThreadDeadline : gCallbackDeadline.load(std::memory_order_acquire);
}
void setOfflineRenderActive(const bool a) noexcept { gOffline.store(a, std::memory_order_release); }
bool offlineRenderActive() noexcept { return gOffline.load(std::memory_order_acquire); }
void noteOfflineFailure() noexcept { gOfflineFailure.store(true, std::memory_order_release); }
bool takeOfflineFailure() noexcept { return gOfflineFailure.exchange(false, std::memory_order_acq_rel); }
} // namespace deadline

namespace
{
constexpr int kCreateTimeoutMs = 60000;
constexpr int kPrepareTimeoutMs = 20000;
constexpr int kStateTimeoutMs = 10000;
constexpr int kDestroyTimeoutMs = 15000;
constexpr int kEditorTimeoutMs = 20000;
constexpr int kShortTimeoutMs = 5000;
/// Offline: no progress from the helper for this long counts as a hang (export fails).
constexpr int kOfflineHangMs = 30000;
/// Lanes spin this long on the done counter before sleeping on the event (plan D.2).
constexpr double kSpinSeconds = 0.000050;
/// Changes reported by the plug-in are captured once they have been quiet this long (stopped).
constexpr double kChangeSettleMs = 1000.0;

/// One high-resolution waitable timer per lane thread, made on the thread's first wait, so a
/// bounded wait ends at its deadline even with the default 15.6 ms timer resolution.
HANDLE laneTimer() noexcept
{
    thread_local HANDLE t = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                     TIMER_ALL_ACCESS);
    return t;
}

juce::String uniqueObjectName(const std::int32_t instanceId, const juce::String& what)
{
    static std::atomic<std::uint32_t> counter { 0 };
    return "Local\\MiniDAW-iso-" + juce::String((int)::GetCurrentProcessId()) + "-" + juce::String(instanceId) + "-"
           + juce::String((int)counter.fetch_add(1)) + "-" + what;
}

juce::String gExtraIdentifier;
bool gIsolationEnabled = true;
} // namespace

// =============================================================================
// Policy
// =============================================================================
bool IsolatedPluginPolicy::shouldIsolate(const juce::PluginDescription& d)
{
    if (!gIsolationEnabled || d.pluginFormatName != "VST3")
    {
        return false;
    }
    if (gExtraIdentifier.isNotEmpty() && d.createIdentifierString() == gExtraIdentifier)
    {
        return true;
    }
    // AmpliTube 4 (IK Multimedia), verified on this machine's scan: VST3 class unique id
    // 0x62218172, bundle "AmpliTube 4.vst3". Every field must match; the name alone never does.
    return d.name == "AmpliTube 4" && d.manufacturerName == "IK Multimedia" && (std::uint32_t)d.uniqueId == 0x62218172u
           && juce::File(d.fileOrIdentifier).getFileName().equalsIgnoreCase("AmpliTube 4.vst3");
}
void IsolatedPluginPolicy::setAdditionalIdentifierForTests(const juce::String& id) { gExtraIdentifier = id; }
void IsolatedPluginPolicy::setEnabled(const bool e) { gIsolationEnabled = e; }
bool IsolatedPluginPolicy::isEnabled() { return gIsolationEnabled; }

// =============================================================================
// IsolatedInsertInstance
// =============================================================================
namespace
{
struct RemoteVst3Client final : juce::ExtensionsVisitor::VST3Client
{
    IsolatedInsertInstance* owner = nullptr;
    Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override { return nullptr; }
    juce::MemoryBlock getPreset() const override
    {
        bool ok = false;
        return owner->getPresetBytes(ok);
    }
    bool setPreset(const juce::MemoryBlock& b) const override { return owner->setPresetBytes(b); }
};
} // namespace

IsolatedInsertInstance::IsolatedInsertInstance(IsolatedHostConnection& connection, const juce::PluginDescription& desc)
    : juce::AudioPluginInstance(BusesProperties()
                                    .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                    .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      connection_(connection),
      description_(desc)
{
    connection_.registerInstance(this);
}

IsolatedInsertInstance::~IsolatedInsertInstance()
{
    connection_.retireInstance(*this);
    connection_.unregisterInstance(this);
    detachSection();
}

bool IsolatedInsertInstance::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
           && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

bool IsolatedInsertInstance::attach(const double sampleRate, const int blockSize, juce::String& error)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    detachSection();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    blockSize_ = blockSize > 0 ? blockSize : 512;
    if (instanceId_ == 0)
    {
        instanceId_ = connection_.nextInstanceId_++;
    }
    const int maxBlock = juce::jmax(kMinSectionBlock, blockSize_);
    const size_t bytes = sectionBytes(maxBlock);
    const juce::String secName = uniqueObjectName(instanceId_, "sec");
    const juce::String reqName = uniqueObjectName(instanceId_, "req");
    const juce::String doneName = uniqueObjectName(instanceId_, "done");
    Section s;
    s.mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)((std::uint64_t)bytes >> 32),
                                     (DWORD)(bytes & 0xffffffffu), secName.toWideCharPointer());
    s.requestEvent = ::CreateEventW(nullptr, FALSE, FALSE, reqName.toWideCharPointer());
    s.doneEvent = ::CreateEventW(nullptr, FALSE, FALSE, doneName.toWideCharPointer());
    void* view = s.mapping != nullptr ? ::MapViewOfFile(s.mapping, FILE_MAP_ALL_ACCESS, 0, 0, bytes) : nullptr;
    if (view == nullptr || s.requestEvent == nullptr || s.doneEvent == nullptr)
    {
        section_ = s;
        section_.header = static_cast<SharedBlockHeader*>(view);
        detachSection();
        error = "Could not create the shared audio section for the isolated plug-in.";
        return false;
    }
    s.header = new (view) SharedBlockHeader();
    s.header->maxBlock = maxBlock;
    auto* samples = reinterpret_cast<float*>(static_cast<char*>(view) + headerBytes());
    for (int c = 0; c < kChannels; ++c)
    {
        s.channel[c] = samples + (size_t)c * (size_t)maxBlock;
    }
    s.maxBlock = maxBlock;
    s.helperProcess = connection_.helperProcess_;
    section_ = s;

    std::unique_ptr<juce::XmlElement> xml = description_.createXml();
    const juce::String xmlText = xml != nullptr ? xml->toString(juce::XmlElement::TextFormat().singleLine()) : juce::String();
    int latency = 0;
    double tail = 0.0;
    bool layoutOk = false;
    juce::String helperError;
    const bool ok = connection_.call(
        ControlKind::Create,
        [&](ControlWriter& w) {
            w.i32(instanceId_).str(xmlText).f64(sampleRate_).i32(blockSize_).i32(maxBlock).str(secName).str(reqName)
                .str(doneName).bytes(lastCapturedState_);
        },
        kCreateTimeoutMs,
        [&](ControlReader& r) {
            helperError = r.str();
            latency = r.i32();
            tail = r.f64();
            layoutOk = r.i32() != 0;
        },
        "create " + description_.name);
    if (!ok)
    {
        detachSection();
        error = "The isolated plug-in host could not load " + description_.name
                + (helperError.isNotEmpty() ? ": " + helperError : juce::String("."));
        return false;
    }
    juce::ignoreUnused(layoutOk);
    setLatencySamples(latency);
    tailSeconds_.store(tail, std::memory_order_relaxed);
    sectionGeneration_.fetch_add(1, std::memory_order_acq_rel);
    link_.store((int)Link::Ready, std::memory_order_seq_cst);
    if (lastCapturedState_.isEmpty())
    {
        // A first capture right away, so a crash before any other capture still restores and
        // saves this instance's settings rather than the plug-in's defaults.
        (void)captureNow();
    }
    return true;
}

void IsolatedInsertInstance::markLost() noexcept
{
    link_.store((int)Link::Lost, std::memory_order_seq_cst);
}

void IsolatedInsertInstance::waitUntilNotInFlight() noexcept
{
    // A lane inside processBlock leaves within its own bounded wait (callback deadline, or the
    // offline slice, which also watches `link_`).
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (inFlight_.load(std::memory_order_seq_cst))
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > 5000.0)
        {
            jassertfalse; // a lane did not leave its wait; keep the section mapped (leak, not reuse)
            section_ = {};
            return;
        }
        ::SwitchToThread();
    }
}

void IsolatedInsertInstance::detachSection()
{
    if (link_.load(std::memory_order_seq_cst) == (int)Link::Ready)
    {
        link_.store((int)Link::Detached, std::memory_order_seq_cst);
    }
    waitUntilNotInFlight();
    if (section_.header != nullptr)
    {
        ::UnmapViewOfFile(section_.header);
    }
    for (Handle h : { section_.mapping, section_.requestEvent, section_.doneEvent })
    {
        if (h != nullptr)
        {
            ::CloseHandle(h);
        }
    }
    section_ = {};
    sectionGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

void IsolatedInsertInstance::prepareToPlay(const double sampleRate, const int maximumExpectedSamplesPerBlock)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : sampleRate_;
    blockSize_ = maximumExpectedSamplesPerBlock > 0 ? maximumExpectedSamplesPerBlock : blockSize_;
    nonRealtime_ = isNonRealtime();
    if (link() != Link::Ready)
    {
        return;
    }
    int latency = getLatencySamples();
    double tail = getTailLengthSeconds();
    if (connection_.call(
            ControlKind::Prepare,
            [&](ControlWriter& w) { w.i32(instanceId_).f64(sampleRate_).i32(blockSize_).i32(nonRealtime_ ? 1 : 0); },
            kPrepareTimeoutMs,
            [&](ControlReader& r) {
                latency = r.i32();
                tail = r.f64();
            },
            "prepare " + description_.name))
    {
        tailSeconds_.store(tail, std::memory_order_relaxed);
        if (latency != getLatencySamples())
        {
            setLatencySamples(latency);
        }
    }
}

void IsolatedInsertInstance::releaseResources()
{
    if (link() == Link::Ready)
    {
        (void)connection_.call(
            ControlKind::Release, [&](ControlWriter& w) { w.i32(instanceId_); }, kShortTimeoutMs, {},
            "release " + description_.name);
    }
}

void IsolatedInsertInstance::writeTransport(SharedBlockHeader& h) noexcept
{
    std::uint32_t flags = 0;
    if (juce::AudioPlayHead* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            if (pos->getIsPlaying()) flags |= kPlaying;
            if (pos->getIsRecording()) flags |= kRecording;
            if (pos->getIsLooping()) flags |= kLooping;
            if (const auto t = pos->getTimeInSamples()) { h.timeInSamples = *t; flags |= kHasTimeSamples; }
            if (const auto p = pos->getPpqPosition()) { h.ppqPosition = *p; flags |= kHasPpq; }
            if (const auto p = pos->getPpqPositionOfLastBarStart()) { h.ppqLastBar = *p; flags |= kHasLastBar; }
            if (const auto b = pos->getBpm()) { h.bpm = *b; flags |= kHasBpm; }
            if (const auto ts = pos->getTimeSignature())
            {
                h.timeSigNumerator = ts->numerator;
                h.timeSigDenominator = ts->denominator;
                flags |= kHasTimeSig;
            }
            if (const auto lp = pos->getLoopPoints())
            {
                h.loopStartPpq = lp->ppqStart;
                h.loopEndPpq = lp->ppqEnd;
                flags |= kHasLoop;
            }
        }
    }
    h.transportFlags = flags;
}

bool IsolatedInsertInstance::processChunk(float* const* channels, const int numChannels, const int offset, const int n) noexcept
{
    SharedBlockHeader& h = *section_.header;
    for (int c = 0; c < kChannels; ++c)
    {
        const float* src = channels[juce::jmin(c, numChannels - 1)] + offset;
        std::memcpy(section_.channel[c], src, sizeof(float) * (size_t)n);
    }
    h.numSamples = n;
    const bool offline = deadline::offlineRenderActive();
    h.nonRealtime = offline ? 1 : 0;
    writeTransport(h);
    if (offset > 0 && (h.transportFlags & kHasTimeSamples) != 0)
    {
        h.timeInSamples += offset;
        if ((h.transportFlags & kHasPpq) != 0 && (h.transportFlags & kHasBpm) != 0 && sampleRate_ > 0.0)
        {
            h.ppqPosition += (double)offset / sampleRate_ * h.bpm / 60.0;
        }
    }
    const std::uint32_t seq = h.doneSeq.load(std::memory_order_relaxed) + 1u;
    h.requestSeq.store(seq, std::memory_order_release);
    ::SetEvent(section_.requestEvent);
    counters_.requests.fetch_add(1, std::memory_order_relaxed);

    const std::int64_t t0 = deadline::qpcNow();
    const std::int64_t freq = deadline::qpcPerSecond();
    const HANDLE waits[3] = { section_.doneEvent, section_.helperProcess, laneTimer() };
    const auto done = [&] { return h.doneSeq.load(std::memory_order_acquire) == seq; };
    const auto lost = [&] {
        markLost();
        return false;
    };

    if (offline)
    {
        // No real-time deadline, but never an unbounded wait: slices that watch the helper process,
        // this stage's link and the helper's progress counter.
        std::uint32_t progress = h.helperProgress.load(std::memory_order_relaxed);
        std::int64_t lastProgress = t0;
        while (!done())
        {
            const DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, 50);
            if (r == WAIT_OBJECT_0 + 1 || link() != Link::Ready)
            {
                deadline::noteOfflineFailure();
                return lost();
            }
            const std::uint32_t p = h.helperProgress.load(std::memory_order_relaxed);
            const std::int64_t now = deadline::qpcNow();
            if (p != progress)
            {
                progress = p;
                lastProgress = now;
            }
            else if ((now - lastProgress) * 1000 / freq > kOfflineHangMs)
            {
                outstanding_ = true;
                outstandingSeq_ = seq;
                deadline::noteOfflineFailure();
                return false;
            }
        }
    }
    else
    {
        std::int64_t end = deadline::currentDeadline();
        if (end <= t0)
        {
            // No context deadline published (tests, an expired one): one block's duration.
            end = t0 + (std::int64_t)((double)n / sampleRate_ * (double)freq);
        }
        const std::int64_t spinEnd = t0 + (std::int64_t)(kSpinSeconds * (double)freq);
        while (!done() && deadline::qpcNow() < spinEnd)
        {
            YieldProcessor();
        }
        while (!done())
        {
            const std::int64_t now = deadline::qpcNow();
            if (now >= end)
            {
                // Miss: the helper keeps the section until it reports this sequence (plan D.3).
                outstanding_ = true;
                outstandingSeq_ = seq;
                counters_.misses.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            DWORD count = 2;
            if (waits[2] != nullptr)
            {
                LARGE_INTEGER due;
                due.QuadPart = -juce::jmax<std::int64_t>(1, (end - now) * 10000000 / freq); // 100 ns, relative
                if (::SetWaitableTimer(waits[2], &due, 0, nullptr, nullptr, FALSE))
                {
                    count = 3;
                }
            }
            const DWORD r = ::WaitForMultipleObjects(count, waits, FALSE, count == 3 ? INFINITE : 1);
            if (r == WAIT_OBJECT_0 + 1)
            {
                return lost();
            }
        }
    }
    const std::int64_t waited = deadline::qpcNow() - t0;
    counters_.waitTicksTotal.fetch_add(waited, std::memory_order_relaxed);
    if (waited > counters_.waitTicksMax.load(std::memory_order_relaxed))
    {
        counters_.waitTicksMax.store(waited, std::memory_order_relaxed);
    }
    for (int c = 0; c < numChannels; ++c)
    {
        std::memcpy(channels[c] + offset, section_.channel[juce::jmin(c, kChannels - 1)], sizeof(float) * (size_t)n);
    }
    return true;
}

void IsolatedInsertInstance::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused(midi);
    inFlight_.store(true, std::memory_order_seq_cst);
    struct Clear
    {
        std::atomic<bool>& f;
        ~Clear() { f.store(false, std::memory_order_seq_cst); }
    } clearOnExit { inFlight_ };

    const int n = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    const std::uint32_t gen = sectionGeneration_.load(std::memory_order_acquire);
    if (gen != seenGeneration_)
    {
        seenGeneration_ = gen;
        outstanding_ = false;
    }
    if (n <= 0 || numChannels <= 0)
    {
        return;
    }
    if (link_.load(std::memory_order_seq_cst) != (int)Link::Ready || connection_.pausedAll_.load(std::memory_order_acquire)
        || section_.header == nullptr)
    {
        buffer.clear();
        counters_.silentLost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (outstanding_)
    {
        if (section_.header->doneSeq.load(std::memory_order_acquire) != outstandingSeq_)
        {
            buffer.clear();
            counters_.silentLost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        outstanding_ = false; // the late output is discarded; this block starts a fresh request
        counters_.lateDiscarded.fetch_add(1, std::memory_order_relaxed);
    }
    float* const* channels = buffer.getArrayOfWritePointers();
    for (int offset = 0; offset < n; offset += section_.maxBlock)
    {
        const int len = juce::jmin(section_.maxBlock, n - offset);
        if (!processChunk(channels, numChannels, offset, len))
        {
            // Silence, not dry signal: unprocessed DI at full level is worse than a gap (plan D.3).
            buffer.clear();
            return;
        }
    }
}

bool IsolatedInsertInstance::captureNow()
{
    if (link() != Link::Ready)
    {
        // Nothing newer than the last capture exists anywhere.
        lastCaptureOk_ = true;
        return true;
    }
    juce::MemoryBlock fresh;
    const bool ok = connection_.call(
        ControlKind::GetState, [&](ControlWriter& w) { w.i32(instanceId_); }, kStateTimeoutMs,
        [&](ControlReader& r) { fresh = r.bytes(); }, "get state of " + description_.name);
    lastCaptureOk_ = ok && fresh.getSize() > 0;
    if (lastCaptureOk_)
    {
        lastCapturedState_ = std::move(fresh);
        changedSinceCapture_ = false;
    }
    return lastCaptureOk_;
}

void IsolatedInsertInstance::getStateInformation(juce::MemoryBlock& destData)
{
    (void)captureNow();
    destData = lastCapturedState_;
}

void IsolatedInsertInstance::setStateInformation(const void* data, const int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
    {
        return;
    }
    lastCapturedState_.replaceAll(data, (size_t)sizeInBytes);
    changedSinceCapture_ = false;
    if (link() != Link::Ready)
    {
        return; // applied when the instance is created or restored
    }
    int latency = getLatencySamples();
    double tail = getTailLengthSeconds();
    if (connection_.call(
            ControlKind::SetState, [&](ControlWriter& w) { w.i32(instanceId_).bytes(data, (size_t)sizeInBytes); },
            kStateTimeoutMs,
            [&](ControlReader& r) {
                latency = r.i32();
                tail = r.f64();
            },
            "set state of " + description_.name))
    {
        tailSeconds_.store(tail, std::memory_order_relaxed);
        if (latency != getLatencySamples())
        {
            setLatencySamples(latency);
        }
    }
}

juce::MemoryBlock IsolatedInsertInstance::getPresetBytes(bool& ok)
{
    juce::MemoryBlock out;
    ok = link() == Link::Ready
         && connection_.call(
             ControlKind::GetPreset, [&](ControlWriter& w) { w.i32(instanceId_); }, kStateTimeoutMs,
             [&](ControlReader& r) { out = r.bytes(); }, "get preset of " + description_.name);
    return out;
}

bool IsolatedInsertInstance::setPresetBytes(const juce::MemoryBlock& preset)
{
    const bool ok = link() == Link::Ready
                    && connection_.call(
                        ControlKind::SetPreset, [&](ControlWriter& w) { w.i32(instanceId_).bytes(preset); },
                        kStateTimeoutMs, {}, "set preset of " + description_.name);
    if (ok)
    {
        (void)captureNow();
    }
    return ok;
}

void IsolatedInsertInstance::getExtensions(juce::ExtensionsVisitor& visitor) const
{
    if (link() != Link::Ready)
    {
        return;
    }
    RemoteVst3Client client;
    client.owner = const_cast<IsolatedInsertInstance*>(this);
    visitor.visitVST3Client(client);
}

bool IsolatedInsertInstance::openRemoteEditor(const juce::String& title)
{
    if (link() != Link::Ready)
    {
        return false;
    }
    if (connection_.helperPid_ != 0)
    {
        ::AllowSetForegroundWindow((DWORD)connection_.helperPid_);
    }
    editorOpen_ = connection_.call(
        ControlKind::OpenEditor, [&](ControlWriter& w) { w.i32(instanceId_).str(title); }, kEditorTimeoutMs, {},
        "open editor of " + description_.name);
    return editorOpen_;
}

void IsolatedInsertInstance::closeRemoteEditor()
{
    if (!editorOpen_)
    {
        return;
    }
    editorOpen_ = false;
    if (link() == Link::Ready)
    {
        (void)connection_.call(
            ControlKind::CloseEditor, [&](ControlWriter& w) { w.i32(instanceId_); }, kShortTimeoutMs, {},
            "close editor of " + description_.name);
    }
}

void IsolatedInsertInstance::noteChanged()
{
    changedSinceCapture_ = true;
    changedAtMs_ = juce::Time::getMillisecondCounterHiRes();
    // Wakes a resting stage (InsertChainRestRuntime listens for audioProcessorChanged).
    updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

// =============================================================================
// IsolatedHostConnection
// =============================================================================
IsolatedHostConnection& IsolatedHostConnection::get()
{
    // Never destroyed: the helper must be ended by `terminateForTeardown`, not by a destructor.
    static IsolatedHostConnection* const c = new IsolatedHostConnection();
    return *c;
}

IsolatedHostConnection::IsolatedHostConnection()
    : helperExe_(juce::File::getSpecialLocation(juce::File::currentExecutableFile))
{
}

void IsolatedHostConnection::setUiHooks(UiHooks hooks) { hooks_ = std::move(hooks); }
void IsolatedHostConnection::setHelperExecutable(const juce::File& exe) { helperExe_ = exe; }

int IsolatedHostConnection::liveInstanceCount() const noexcept
{
    return (int)std::count_if(instances_.begin(), instances_.end(), [](const IsolatedInsertInstance* i) {
        return i->link() == IsolatedInsertInstance::Link::Ready;
    });
}

int IsolatedHostConnection::liveOnInstanceCount() const noexcept
{
    return (int)std::count_if(instances_.begin(), instances_.end(), [](const IsolatedInsertInstance* i) {
        return i->link() == IsolatedInsertInstance::Link::Ready && !i->retiredForOff_;
    });
}

void IsolatedHostConnection::registerInstance(IsolatedInsertInstance* inst) { instances_.push_back(inst); }

void IsolatedHostConnection::unregisterInstance(IsolatedInsertInstance* inst)
{
    instances_.erase(std::remove(instances_.begin(), instances_.end(), inst), instances_.end());
    restoreQueue_.erase(std::remove(restoreQueue_.begin(), restoreQueue_.end(), inst), restoreQueue_.end());
}

IsolatedInsertInstance* IsolatedHostConnection::findById(const std::int32_t id) const
{
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->instanceId_ == id)
        {
            return i;
        }
    }
    return nullptr;
}

std::unique_ptr<juce::AudioPluginInstance> IsolatedHostConnection::createInstance(const juce::PluginDescription& desc,
                                                                                  const double sampleRate,
                                                                                  const int blockSize,
                                                                                  juce::String& error)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (phase_ == Phase::Restoring || phase_ == Phase::WaitingForStop)
    {
        error = "The plug-in host is being restored. Try again when it has finished.";
        return nullptr;
    }
    if (!ensureRunning(error))
    {
        return nullptr;
    }
    auto inst = std::make_unique<IsolatedInsertInstance>(*this, desc);
    if (!inst->attach(sampleRate, blockSize, error))
    {
        inst->skipDestroyCapture_ = true;
        return nullptr;
    }
    return inst;
}

bool IsolatedHostConnection::ensureRunning(juce::String& error)
{
    if (helperAlive_.load(std::memory_order_acquire))
    {
        return true;
    }
    if (phase_ == Phase::Failed)
    {
        // A new instance after a failed restore is a fresh start; the failed ones stay lost.
        setPhase(Phase::None);
    }
    return launchHelper(error);
}

bool IsolatedHostConnection::launchHelper(juce::String& error)
{
    if (job_ == nullptr)
    {
        // Every helper is in a job that ends with DAL: a DAL crash never leaves a helper behind.
        job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (job_ != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
    }
    helloEvent_.reset();
    helloPid_.store(0);
    ++helperStarts_;
    if (!launchWorkerProcess(helperExe_, kHelperCommandLineUniqueId, 10000, 0))
    {
        error = "Could not start the isolated plug-in host.";
        return false;
    }
    if (!helloEvent_.wait(10000) || helloPid_.load() == 0)
    {
        killWorkerProcess();
        error = "The isolated plug-in host did not start.";
        return false;
    }
    helperPid_ = helloPid_.load();
    HANDLE proc = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, (DWORD)helperPid_);
    if (proc == nullptr)
    {
        killWorkerProcess();
        error = "Could not open the isolated plug-in host process.";
        return false;
    }
    if (job_ != nullptr)
    {
        ::AssignProcessToJobObject(job_, proc);
    }
    if (helperProcess_ != nullptr)
    {
        retiredProcessHandles_.push_back(helperProcess_);
    }
    helperProcess_ = proc;
    const int gen = generation_.fetch_add(1) + 1;
    helperAlive_.store(true, std::memory_order_release);
    helperUnresponsive_ = false;

    if (deathWatch_ != nullptr && deathWatch_->joinable())
    {
        deathWatch_->join();
    }
    if (deathWatchStop_ == nullptr)
    {
        deathWatchStop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ::ResetEvent(deathWatchStop_);
    deathWatch_ = std::make_unique<std::thread>([this, proc, gen] {
        const HANDLE waits[2] = { proc, deathWatchStop_ };
        if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0)
        {
            return;
        }
        helperAlive_.store(false, std::memory_order_release);
        failAllPending();
        juce::MessageManager::callAsync([this, gen] { onHelperDied(gen, false); });
    });
    if (phase_ == Phase::None)
    {
        setPhase(Phase::Running);
    }
    startTimerHz(4);
    return true;
}

bool IsolatedHostConnection::call(const ControlKind kind,
                                  const std::function<void(ControlWriter&)>& payload,
                                  const int timeoutMs,
                                  const std::function<void(ControlReader&)>& onReply,
                                  const juce::String& what)
{
    if (!helperAlive_.load(std::memory_order_acquire) || helperUnresponsive_)
    {
        return false;
    }
    const std::int32_t rid = nextRequestId_.fetch_add(1);
    auto p = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lock(pendingLock_);
        pending_[rid] = p;
    }
    ControlWriter w(kind, rid);
    if (payload)
    {
        payload(w);
    }
    const bool sent = sendMessageToWorker(w.block());
    const bool answered = sent && p->done.wait(timeoutMs);
    {
        std::lock_guard<std::mutex> lock(pendingLock_);
        pending_.erase(rid);
    }
    if (!answered)
    {
        if (helperAlive_.load(std::memory_order_acquire))
        {
            // Alive but silent: treat as a hang. Audio is protected by its own deadlines; the user
            // decides whether to restart (the helper may hold newer state than DAL).
            helperUnresponsive_ = true;
            juce::Logger::writeToLog("[isolation] helper did not answer: " + what);
            if (hooks_.showFailure)
            {
                hooks_.showFailure("The AmpliTube 4 host is not responding (" + what
                                       + "). Restart it from the last saved plug-in state?",
                                   [this] { restartUnresponsiveHelper(); });
            }
        }
        return false;
    }
    if (!p->ok)
    {
        if (p->reply.getSize() > 0 && onReply)
        {
            ControlReader r(p->reply);
            (void)r.i32();
            if (kind == ControlKind::Create)
            {
                onReply(r);
            }
        }
        return false;
    }
    if (onReply)
    {
        ControlReader r(p->reply);
        (void)r.i32(); // ok flag
        onReply(r);
    }
    return true;
}

void IsolatedHostConnection::failAllPending()
{
    std::lock_guard<std::mutex> lock(pendingLock_);
    for (auto& kv : pending_)
    {
        kv.second->ok = false;
        kv.second->done.signal();
    }
}

void IsolatedHostConnection::handleMessageFromWorker(const juce::MemoryBlock& m)
{
    ControlReader r(m);
    switch (r.kind())
    {
        case ControlKind::Reply:
        {
            std::shared_ptr<Pending> p;
            {
                std::lock_guard<std::mutex> lock(pendingLock_);
                const auto it = pending_.find(r.requestId());
                if (it != pending_.end())
                {
                    p = it->second;
                }
            }
            if (p != nullptr)
            {
                ControlReader okReader(m);
                p->ok = okReader.i32() != 0;
                p->reply = m; // readers skip kind, request id and the ok flag
                p->done.signal();
            }
            return;
        }
        case ControlKind::Hello:
            helloPid_.store((std::uint32_t)r.i32());
            helloEvent_.signal();
            return;
        case ControlKind::Dirty:
        {
            const std::int32_t id = r.i32();
            juce::MessageManager::callAsync([this, id] {
                if (IsolatedInsertInstance* i = findById(id))
                {
                    i->noteChanged();
                }
            });
            return;
        }
        case ControlKind::EditorClosed:
        {
            const std::int32_t id = r.i32();
            juce::MessageManager::callAsync([this, id] {
                if (IsolatedInsertInstance* i = findById(id))
                {
                    i->editorOpen_ = false;
                    if (!(hooks_.transportBusy && hooks_.transportBusy()))
                    {
                        (void)i->captureNow();
                    }
                    if (i->onRemoteEditorClosed)
                    {
                        i->onRemoteEditorClosed();
                    }
                }
            });
            return;
        }
        default: return;
    }
}

void IsolatedHostConnection::handleConnectionLost()
{
    // The process watch is the authority; a lost pipe with a live process is a hang.
    if (helperAlive_.load(std::memory_order_acquire))
    {
        failAllPending();
    }
}

void IsolatedHostConnection::endHelperProcess(const bool expected)
{
    if (helperProcess_ == nullptr)
    {
        return;
    }
    if (expected)
    {
        expectedDeathGeneration_ = generation_.load();
    }
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->link() == IsolatedInsertInstance::Link::Ready)
        {
            i->markLost();
        }
    }
    // TerminateProcess: AmpliTube's destructor never runs. The OS reclaims the memory.
    ::TerminateProcess(helperProcess_, 0);
    ::WaitForSingleObject(helperProcess_, 5000);
    helperAlive_.store(false, std::memory_order_release);
    failAllPending();
    killWorkerProcess(); // closes the pipe; the helper is already gone
    if (deathWatch_ != nullptr && deathWatch_->joinable())
    {
        deathWatch_->join();
    }
    deathWatch_.reset();
}

void IsolatedHostConnection::terminateForTeardown(const juce::String& why)
{
    juce::Logger::writeToLog("[isolation] end helper without destructors: " + why);
    endHelperProcess(true);
    for (IsolatedInsertInstance* i : instances_)
    {
        i->link_.store((int)IsolatedInsertInstance::Link::Detached, std::memory_order_seq_cst);
        i->skipDestroyCapture_ = true;
    }
    restoreQueue_.clear();
    setPhase(Phase::None);
}

void IsolatedHostConnection::retireInstance(IsolatedInsertInstance& inst)
{
    if (inst.link() != IsolatedInsertInstance::Link::Ready || !helperAlive_.load(std::memory_order_acquire))
    {
        return;
    }
    // Instances of rows that are Off do not keep the helper: their state is already captured.
    const bool last = std::none_of(instances_.begin(), instances_.end(), [&](const IsolatedInsertInstance* i) {
        return i != &inst && i->link() == IsolatedInsertInstance::Link::Ready && !i->retiredForOff_;
    });
    inst.closeRemoteEditor();
    if (last)
    {
        // No one else lives in the helper: end it. Frees all of its memory and never runs the
        // plug-in's destructor.
        endHelperProcess(true);
        setPhase(Phase::None);
        return;
    }
    if (!inst.skipDestroyCapture_)
    {
        juce::String failed;
        if (!captureAllLive(failed))
        {
            // Without fresh state of the others, a destructor crash could lose their settings:
            // leave this instance in the helper (unreachable, not processed) instead.
            juce::Logger::writeToLog("[isolation] capture failed (" + failed + "); " + inst.getName()
                                     + " left in the helper until it ends");
            inst.markLost();
            return;
        }
    }
    inst.markLost();
    inst.waitUntilNotInFlight();
    (void)call(
        ControlKind::Destroy, [&](ControlWriter& w) { w.i32(inst.instanceId_); }, kDestroyTimeoutMs, {},
        "destroy " + inst.getName());
    // A crash in the destructor surfaces through the process watch and starts recovery of the
    // others from the states captured just above.
}

bool IsolatedHostConnection::captureAllLive(juce::String& failedName)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    pausedAll_.store(true, std::memory_order_seq_cst);
    struct Resume
    {
        std::atomic<bool>& f;
        ~Resume() { f.store(false, std::memory_order_seq_cst); }
    } resume { pausedAll_ };
    // Drain: no lane inside a request, and the helper owns no section.
    for (IsolatedInsertInstance* i : instances_)
    {
        i->waitUntilNotInFlight();
        if (i->link() == IsolatedInsertInstance::Link::Ready && i->section_.header != nullptr)
        {
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            while (i->section_.header->doneSeq.load(std::memory_order_acquire)
                   != i->section_.header->requestSeq.load(std::memory_order_acquire))
            {
                if (juce::Time::getMillisecondCounterHiRes() - t0 > 2000.0)
                {
                    failedName = i->getName() + " (still processing)";
                    return false;
                }
                juce::Thread::sleep(1);
            }
        }
    }
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->link() == IsolatedInsertInstance::Link::Ready && !i->retiredForOff_ && !i->captureNow())
        {
            failedName = i->getName();
            return false;
        }
    }
    return true;
}

void IsolatedHostConnection::captureChangedWhileStopped()
{
    if (hooks_.transportBusy && hooks_.transportBusy())
    {
        return;
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->changedSinceCapture_ && !i->editorOpen_ && now - i->changedAtMs_ >= kChangeSettleMs)
        {
            (void)i->captureNow();
        }
    }
}

void IsolatedHostConnection::timerCallback()
{
    if (phase_ == Phase::Running)
    {
        captureChangedWhileStopped();
    }
    else if (phase_ == Phase::WaitingForStop)
    {
        if (!(hooks_.transportBusy && hooks_.transportBusy()))
        {
            beginRestore();
        }
    }
    else if (phase_ == Phase::Restoring)
    {
        restoreNextStep();
    }
}

void IsolatedHostConnection::setPhase(const Phase p)
{
    const bool wasBlocking = isBlockingCommands();
    phase_ = p;
    if (wasBlocking != isBlockingCommands() && hooks_.blockingChanged)
    {
        hooks_.blockingChanged();
    }
}

void IsolatedHostConnection::onHelperDied(const int generation, const bool unresponsive)
{
    juce::ignoreUnused(unresponsive);
    if (generation == expectedDeathGeneration_ || generation != generation_.load())
    {
        return; // ended on purpose, or an older helper
    }
    if (phase_ == Phase::Failed)
    {
        return; // never a second automatic attempt; Retry starts the next one
    }
    juce::Logger::writeToLog("[isolation] helper process ended unexpectedly");
    lostChangeNames_.clear();
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->link() == IsolatedInsertInstance::Link::Ready)
        {
            i->markLost();
        }
        if (i->changedSinceCapture_ || i->editorOpen_)
        {
            lostChangeNames_.addIfNotAlreadyThere(i->getName());
        }
        i->editorOpen_ = false;
    }
    killWorkerProcess();
    if (phase_ == Phase::Restoring)
    {
        finishRestore(false, "The plug-in host stopped again while restoring.");
        return;
    }
    if (std::all_of(instances_.begin(), instances_.end(), [](const IsolatedInsertInstance* i) { return i->retiredForOff_; }))
    {
        setPhase(Phase::None); // nothing to restore: no helper is started
        if (hooks_.showProgress)
        {
            hooks_.showProgress("AmpliTube 4 host stopped. No plug-ins needed restoring.");
        }
        return;
    }
    if (hooks_.transportBusy && hooks_.transportBusy())
    {
        setPhase(Phase::WaitingForStop);
        if (hooks_.showProgress)
        {
            hooks_.showProgress("AmpliTube 4 host stopped. Its plug-ins are silent and will be restored after Stop.");
        }
        return;
    }
    beginRestore();
}

void IsolatedHostConnection::beginRestore()
{
    restoreQueue_.clear();
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->link() != IsolatedInsertInstance::Link::Ready && !i->retiredForOff_)
        {
            restoreQueue_.push_back(i); // desired On only; Off rows stay unloaded
        }
    }
    restoreTotal_ = (int)restoreQueue_.size();
    restoreDone_ = 0;
    if (restoreTotal_ == 0)
    {
        setPhase(helperAlive_.load() ? Phase::Running : Phase::None);
        return;
    }
    setPhase(Phase::Restoring);
    if (hooks_.showProgress)
    {
        hooks_.showProgress("Restoring plugins... 0/" + juce::String(restoreTotal_));
    }
    juce::String err;
    if (!helperAlive_.load() && !launchHelper(err))
    {
        finishRestore(false, err);
        return;
    }
    if (std::exchange(testCrashOnRestoreCreate_, false))
    {
        (void)sendTestFault(TestFault::CrashOnNextCreate, 0, 0);
    }
    startTimer(1); // one instance per message-loop turn
}

void IsolatedHostConnection::restoreNextStep()
{
    if (restoreQueue_.empty())
    {
        finishRestore(true, {});
        return;
    }
    IsolatedInsertInstance* i = restoreQueue_.front();
    restoreQueue_.erase(restoreQueue_.begin());
    i->detachSection();
    juce::String err;
    if (!i->attach(i->sampleRate_, i->blockSize_, err))
    {
        finishRestore(false, err);
        return;
    }
    ++restoreDone_;
    if (hooks_.showProgress)
    {
        hooks_.showProgress("Restoring plugins... " + juce::String(restoreDone_) + "/" + juce::String(restoreTotal_));
    }
}

void IsolatedHostConnection::finishRestore(const bool ok, const juce::String& why)
{
    restoreQueue_.clear();
    startTimerHz(4);
    if (ok)
    {
        lastRecoveryReport_ = "AmpliTube 4 host restarted; " + juce::String(restoreDone_) + " instance(s) restored.";
        if (lostChangeNames_.size() > 0)
        {
            lastRecoveryReport_ << " Changes made since the last capture were lost for: " << lostChangeNames_.joinIntoString(", ")
                                << ".";
        }
        setPhase(Phase::Running);
        juce::Logger::writeToLog("[isolation] " + lastRecoveryReport_);
        if (hooks_.showProgress)
        {
            hooks_.showProgress(lastRecoveryReport_);
        }
        return;
    }
    // No second automatic attempt. The lost stages stay silent with their state kept for Save.
    if (helperAlive_.load())
    {
        endHelperProcess(true);
    }
    lastRecoveryReport_ = "Restoring the AmpliTube 4 host failed: " + why
                          + " The affected inserts are silent; their settings are kept and saved.";
    setPhase(Phase::Failed);
    juce::Logger::writeToLog("[isolation] " + lastRecoveryReport_);
    if (hooks_.hideProgress)
    {
        hooks_.hideProgress();
    }
    if (hooks_.showFailure)
    {
        hooks_.showFailure(lastRecoveryReport_, [this] { retryRestore(); });
    }
}

void IsolatedHostConnection::retryRestore()
{
    if (phase_ != Phase::Failed)
    {
        return;
    }
    if (hooks_.transportBusy && hooks_.transportBusy())
    {
        setPhase(Phase::WaitingForStop);
        return;
    }
    beginRestore();
}

void IsolatedHostConnection::restartUnresponsiveHelper()
{
    if (!helperUnresponsive_)
    {
        return;
    }
    lostChangeNames_.clear();
    for (IsolatedInsertInstance* i : instances_)
    {
        if (i->changedSinceCapture_ || i->editorOpen_)
        {
            lostChangeNames_.addIfNotAlreadyThere(i->getName());
        }
        i->editorOpen_ = false;
    }
    endHelperProcess(true);
    helperUnresponsive_ = false;
    setPhase(Phase::None);
    if (hooks_.transportBusy && hooks_.transportBusy())
    {
        setPhase(Phase::WaitingForStop);
        return;
    }
    beginRestore();
}

bool IsolatedHostConnection::sendTestFault(const TestFault fault, const int argument, const std::int32_t instanceId)
{
    return call(
        ControlKind::TestFault, [&](ControlWriter& w) { w.i32((int)fault).i32(argument).i32(instanceId); },
        kShortTimeoutMs, {}, "test fault");
}

bool IsolatedHostConnection::runMessageLoopUntil(const std::function<bool()>& predicate, const int timeoutMs)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (!predicate())
    {
        if (juce::Time::getMillisecondCounterHiRes() - t0 > timeoutMs)
        {
            return predicate();
        }
#if JUCE_MODAL_LOOPS_PERMITTED
        juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
#else
        juce::Thread::sleep(5);
#endif
    }
    return true;
}
} // namespace mini_daw::isolation
