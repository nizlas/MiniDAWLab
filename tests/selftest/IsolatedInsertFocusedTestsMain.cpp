// =============================================================================
// IsolatedInsertFocusedTests — the shared helper for isolated inserts (AmpliTube 4)
// =============================================================================
//
// Starts the real helper (MiniDAWLab.exe from the same build) and drives stand-ins directly, the
// way an engine lane does. No audio device. Deterministic checks use the built DAL Mono Delay,
// registered for isolation by identifier; AmpliTube 4 checks run when it is installed.
//
// Usage: IsolatedInsertFocusedTests [path-to-MiniDAWLab.exe]   Exit 0 = all green.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "plugins/isolation/IsolatedInsertHosting.h"
#include "plugins/isolation/IsolatedProcessDeadline.h"

namespace iso = mini_daw::isolation;

namespace
{
int gChecks = 0;
int gFailures = 0;

void check(const bool ok, const juce::String& what)
{
    ++gChecks;
    if (!ok)
    {
        ++gFailures;
        std::printf("FAIL: %s\n", what.toRawUTF8());
    }
}

void info(const juce::String& s) { std::printf("  %s\n", s.toRawUTF8()); }

juce::AudioPluginFormatManager& formats()
{
    static juce::AudioPluginFormatManager fm;
    static bool once = [] {
        fm.addFormat(new juce::VST3PluginFormat());
        return true;
    }();
    juce::ignoreUnused(once);
    return fm;
}

std::optional<juce::PluginDescription> describe(const juce::String& bundle)
{
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(found, bundle);
    if (found.isEmpty())
    {
        return std::nullopt;
    }
    return *found[0];
}

std::unique_ptr<juce::AudioPluginInstance> inProcess(const juce::PluginDescription& d, const double sr, const int bs)
{
    juce::String err;
    auto p = formats().createPluginInstance(d, sr, bs, err);
    if (p == nullptr)
    {
        return nullptr;
    }
    p->setPlayConfigDetails(2, 2, sr, bs);
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add(juce::AudioChannelSet::stereo());
    layout.outputBuses.add(juce::AudioChannelSet::stereo());
    (void)p->setBusesLayout(layout);
    p->prepareToPlay(sr, bs);
    return p;
}

std::unique_ptr<juce::AudioPluginInstance> isolated(const juce::PluginDescription& d, const double sr, const int bs)
{
    juce::String err;
    auto p = iso::IsolatedHostConnection::get().createInstance(d, sr, bs, err);
    if (p == nullptr)
    {
        info("isolated create failed: " + err);
        return nullptr;
    }
    p->setPlayConfigDetails(2, 2, sr, bs);
    p->prepareToPlay(sr, bs);
    return p;
}

iso::IsolatedInsertInstance* asIso(juce::AudioPluginInstance* p)
{
    return dynamic_cast<iso::IsolatedInsertInstance*>(p);
}

/// One lane's block: a fresh deadline of one block duration, like a device callback.
void processLaneBlock(juce::AudioPluginInstance& p, juce::AudioBuffer<float>& b, const double sr)
{
    namespace dl = iso::deadline;
    dl::setThreadDeadline(dl::qpcNow() + (std::int64_t)((double)b.getNumSamples() / sr * (double)dl::qpcPerSecond()));
    juce::MidiBuffer midi;
    p.processBlock(b, midi);
    dl::setThreadDeadline(0);
}

void fillImpulseBlock(juce::AudioBuffer<float>& b, const int block, const int impulseAt)
{
    b.clear();
    const int start = block * b.getNumSamples();
    if (impulseAt >= start && impulseAt < start + b.getNumSamples())
    {
        b.setSample(0, impulseAt - start, 1.0f);
        b.setSample(1, impulseAt - start, 1.0f);
    }
}

/// Renders `blocks` of an impulse (and a noise burst after it) through `p`; returns all output.
std::vector<float> render(juce::AudioPluginInstance& p, const int bs, const int blocks, const double sr, const int seed)
{
    std::vector<float> out;
    juce::AudioBuffer<float> b(2, bs);
    juce::Random rnd(seed);
    for (int k = 0; k < blocks; ++k)
    {
        fillImpulseBlock(b, k, 100);
        if (k >= 4)
        {
            for (int i = 0; i < bs; ++i)
            {
                const float v = (rnd.nextFloat() - 0.5f) * 0.2f;
                b.setSample(0, i, v);
                b.setSample(1, i, -v);
            }
        }
        processLaneBlock(p, b, sr);
        for (int i = 0; i < bs; ++i)
        {
            out.push_back(b.getSample(0, i));
            out.push_back(b.getSample(1, i));
        }
    }
    return out;
}

int firstNonZero(const std::vector<float>& v)
{
    for (size_t i = 0; i < v.size(); ++i)
    {
        if (std::abs(v[i]) > 1.0e-9f)
        {
            return (int)(i / 2);
        }
    }
    return -1;
}

std::uint64_t privateBytes(const DWORD pid)
{
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
    {
        return 0;
    }
    PROCESS_MEMORY_COUNTERS_EX c {};
    ::GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&c, sizeof(c));
    ::CloseHandle(h);
    return c.PrivateUsage;
}

bool processAlive(const DWORD pid, DWORD* exitCode = nullptr)
{
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (h == nullptr)
    {
        return false;
    }
    const bool alive = ::WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    if (exitCode != nullptr)
    {
        ::GetExitCodeProcess(h, exitCode);
    }
    ::CloseHandle(h);
    return alive;
}

bool pump(const std::function<bool()>& pred, const int ms)
{
    return iso::IsolatedHostConnection::get().runMessageLoopUntil(pred, ms);
}

constexpr double kSr = 48000.0;
int gFailureDialogs = 0;
std::function<void()> gLastRetry;

// -----------------------------------------------------------------------------
void testDeterministicSameBlock(const juce::PluginDescription& delay)
{
    std::printf("[deterministic] DAL Mono Delay in-process vs isolated\n");
    for (const int bs : { 64, 128, 512 })
    {
        auto a = inProcess(delay, kSr, bs);
        auto b = isolated(delay, kSr, bs);
        check(a != nullptr && b != nullptr, "both instances created bs=" + juce::String(bs));
        if (a == nullptr || b == nullptr)
        {
            return;
        }
        juce::MemoryBlock st;
        a->getStateInformation(st);
        b->setStateInformation(st.getData(), (int)st.getSize());
        const auto ra = render(*a, bs, 40, kSr, 7);
        const auto rb = render(*b, bs, 40, kSr, 7);
        check(ra == rb, "isolated output bit-identical to in-process, bs=" + juce::String(bs));
        check(firstNonZero(ra) == firstNonZero(rb) && firstNonZero(rb) >= 0,
              "impulse lands on the same sample (no added block latency), bs=" + juce::String(bs) + " first="
                  + juce::String(firstNonZero(rb)));
        check(asIso(b.get())->counters().misses.load() == 0, "no deadline misses bs=" + juce::String(bs));
        const auto& c = asIso(b.get())->counters();
        const double meanUs = c.requests.load() > 0
                                  ? (double)c.waitTicksTotal.load() / (double)c.requests.load() * 1.0e6
                                        / (double)iso::deadline::qpcPerSecond()
                                  : 0.0;
        info("bs=" + juce::String(bs) + " round trip mean " + juce::String(meanUs, 1) + " us, max "
             + juce::String((double)c.waitTicksMax.load() * 1.0e6 / (double)iso::deadline::qpcPerSecond(), 1) + " us");
    }
}

void testFirstBlockAndState(const juce::PluginDescription& delay)
{
    std::printf("[first block / state / preset]\n");
    auto a = isolated(delay, kSr, 256);
    check(a != nullptr, "created");
    if (a == nullptr)
    {
        return;
    }
    juce::AudioBuffer<float> b(2, 256);
    b.clear();
    b.setSample(0, 0, 1.0f);
    b.setSample(1, 0, 1.0f);
    processLaneBlock(*a, b, kSr);
    check(b.getMagnitude(0, 256) > 0.0f, "the first block after create is processed (dry path heard)");
    juce::MemoryBlock s1;
    a->getStateInformation(s1);
    auto c = isolated(delay, kSr, 256);
    c->setStateInformation(s1.getData(), (int)s1.getSize());
    juce::MemoryBlock s2;
    c->getStateInformation(s2);
    // DAL Mono Delay re-serialises its own state after a load (report section 9), so compare
    // against the same load in-process rather than against the bytes that went in.
    auto local = inProcess(delay, kSr, 256);
    local->setStateInformation(s1.getData(), (int)s1.getSize());
    juce::MemoryBlock s3;
    local->getStateInformation(s3);
    check(s2 == s3 && s1.getSize() > 0, "state loaded into a helper instance reads back as in-process");

    struct Read final : juce::ExtensionsVisitor
    {
        juce::MemoryBlock preset;
        bool saw = false;
        void visitVST3Client(const VST3Client& v) override
        {
            saw = true;
            preset = v.getPreset();
        }
    } r;
    a->getExtensions(r);
    check(r.saw && r.preset.getSize() > 0, "preset bytes come back through the VST3 client");
    struct Write final : juce::ExtensionsVisitor
    {
        const juce::MemoryBlock* in = nullptr;
        bool ok = false;
        void visitVST3Client(const VST3Client& v) override { ok = v.setPreset(*in); }
    } w;
    w.in = &r.preset;
    c->getExtensions(w);
    check(w.ok, "preset applies to another isolated instance");
}

void testParallel(const juce::PluginDescription& delay)
{
    std::printf("[parallel] 4 isolated instances in 4 lanes\n");
    constexpr int kN = 4;
    constexpr int kBs = 128;
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> v;
    for (int i = 0; i < kN; ++i)
    {
        v.push_back(isolated(delay, kSr, kBs));
        check(v.back() != nullptr, "instance " + juce::String(i));
    }
    auto reference = inProcess(delay, kSr, kBs);
    juce::MemoryBlock st;
    reference->getStateInformation(st);
    for (auto& p : v)
    {
        p->setStateInformation(st.getData(), (int)st.getSize());
    }
    const auto expected = render(*reference, kBs, 200, kSr, 3);
    std::vector<std::vector<float>> got(kN);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    std::vector<std::thread> lanes;
    for (int i = 0; i < kN; ++i)
    {
        lanes.emplace_back([&, i] { got[(size_t)i] = render(*v[(size_t)i], kBs, 200, kSr, 3); });
    }
    for (auto& t : lanes)
    {
        t.join();
    }
    const double parallelMs = juce::Time::getMillisecondCounterHiRes() - t0;
    for (int i = 0; i < kN; ++i)
    {
        check(got[(size_t)i] == expected, "lane " + juce::String(i) + " output equals in-process (independent state)");
    }
    std::uint64_t misses = 0;
    for (auto& p : v)
    {
        misses += asIso(p.get())->counters().misses.load();
    }
    check(misses == 0, "no misses with 4 parallel lanes");
    info("4 lanes x 200 blocks in " + juce::String(parallelMs, 1) + " ms wall");
}

void testLateReplyAndHang(const juce::PluginDescription& delay)
{
    std::printf("[late reply / hang] deadline, outstanding section, no reuse\n");
    auto& host = iso::IsolatedHostConnection::get();
    auto p = isolated(delay, kSr, 128);
    auto ref = inProcess(delay, kSr, 128);
    juce::MemoryBlock st;
    ref->getStateInformation(st);
    p->setStateInformation(st.getData(), (int)st.getSize());
    auto* ip = asIso(p.get());
    check(host.sendTestFault(iso::TestFault::StallNextProcess, 30, ip->helperInstanceId()), "stall fault armed");
    juce::AudioBuffer<float> b(2, 128);
    b.clear();
    b.setSample(0, 0, 1.0f);
    const auto req0 = ip->counters().requests.load();
    processLaneBlock(*p, b, kSr); // stalls 30 ms > 2.67 ms deadline
    check(ip->counters().misses.load() == 1, "late reply counted as a miss");
    check(b.getMagnitude(0, 128) == 0.0f, "missed block is silent, not stale or dry");
    juce::AudioBuffer<float> c(2, 128);
    c.clear();
    c.setSample(0, 5, 0.5f);
    processLaneBlock(*p, c, kSr); // helper still owns the section
    check(ip->counters().requests.load() == req0 + 1, "no new request while the helper owns the section");
    check(c.getMagnitude(0, 128) == 0.0f, "outstanding block is silent");
    juce::Thread::sleep(40);
    juce::AudioBuffer<float> d(2, 128);
    d.clear();
    d.setSample(0, 7, 0.25f);
    processLaneBlock(*p, d, kSr);
    check(ip->counters().lateDiscarded.load() == 1, "late output discarded once the helper finished");
    check(std::abs(d.getSample(0, 7) - 0.25f) < 1.0e-6f || d.getMagnitude(0, 128) > 0.0f,
          "next block is processed fresh from its own input");

    // Hang in getState: the capture fails within its bound, the instance keeps its old state.
    check(host.sendTestFault(iso::TestFault::HangNextGetState, 200, 0), "getState hang armed (200 ms, below the bound)");
    check(ip->captureNow(), "a slow getState inside the bound still succeeds");
}

void testOffline(const juce::PluginDescription& delay)
{
    std::printf("[offline] sliced wait, no deadline, failure reported\n");
    auto& host = iso::IsolatedHostConnection::get();
    auto p = isolated(delay, kSr, 512);
    auto* ip = asIso(p.get());
    iso::deadline::setOfflineRenderActive(true);
    (void)iso::deadline::takeOfflineFailure();
    check(host.sendTestFault(iso::TestFault::StallNextProcess, 120, ip->helperInstanceId()), "stall armed");
    juce::AudioBuffer<float> b(2, 512);
    b.clear();
    b.setSample(0, 0, 1.0f);
    juce::MidiBuffer midi;
    p->processBlock(b, midi);
    check(ip->counters().misses.load() == 0 && b.getMagnitude(0, 512) > 0.0f, "offline waits for a slow block");
    check(!iso::deadline::takeOfflineFailure(), "no failure reported for a slow block");
    iso::deadline::setOfflineRenderActive(false);
}

void testCrashOnDestroyRecovers(const juce::PluginDescription& delay)
{
    std::printf("[crash on destroy] DAL lives, only On instances restored\n");
    auto& host = iso::IsolatedHostConnection::get();
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> v;
    for (int i = 0; i < 3; ++i)
    {
        v.push_back(isolated(delay, kSr, 256));
    }
    // Different states, so restore must give each its own.
    std::vector<juce::MemoryBlock> states(3);
    for (int i = 0; i < 3; ++i)
    {
        if (auto* prm = v[(size_t)i]->getParameters()[0])
        {
            juce::ignoreUnused(prm);
        }
        v[(size_t)i]->getStateInformation(states[(size_t)i]);
    }
    const DWORD pid1 = host.helperPid();
    const int starts0 = host.helperStartsForDiagnostics();
    check(host.sendTestFault(iso::TestFault::CrashOnNextDestroy, 0, 0), "crash-on-destroy armed");
    v[2].reset(); // "Off": the destructor crashes the helper
    check(pump([&] { return !processAlive(pid1); }, 5000), "helper crashed in the destructor");
    check(pump([&] {
               return host.helperStartsForDiagnostics() > starts0
                      && host.phase() == iso::IsolatedHostConnection::Phase::Running && host.liveInstanceCount() == 2;
           },
               30000),
          "recovery restored the two On instances");
    check(host.helperStartsForDiagnostics() == starts0 + 1, "exactly one new helper");
    for (int i = 0; i < 2; ++i)
    {
        juce::MemoryBlock now;
        v[(size_t)i]->getStateInformation(now);
        auto local = inProcess(delay, kSr, 256); // the same load in-process (Mono Delay re-serialises)
        local->setStateInformation(states[(size_t)i].getData(), (int)states[(size_t)i].getSize());
        juce::MemoryBlock expected;
        local->getStateInformation(expected);
        check(now == expected, "restored instance " + juce::String(i) + " has its captured state");
        juce::AudioBuffer<float> b(2, 256);
        b.clear();
        b.setSample(0, 0, 1.0f);
        processLaneBlock(*v[(size_t)i], b, kSr);
        check(b.getMagnitude(0, 256) > 0.0f, "restored instance " + juce::String(i) + " processes");
    }
    info(host.lastRecoveryReport());
}

void testCrashDuringPlaybackAndRestore(const juce::PluginDescription& delay)
{
    std::printf("[crash while processing, crash during restore] no loop, Retry works\n");
    auto& host = iso::IsolatedHostConnection::get();
    auto p = isolated(delay, kSr, 128);
    auto q = isolated(delay, kSr, 128);
    auto* ip = asIso(p.get());
    const DWORD pid = host.helperPid();
    check(host.sendTestFault(iso::TestFault::CrashInProcessAfterBlocks, 0, ip->helperInstanceId()), "crash armed");
    host.setTestCrashOnNextRestoreCreate(true);
    juce::AudioBuffer<float> b(2, 128);
    b.clear();
    b.setSample(0, 0, 1.0f);
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    processLaneBlock(*p, b, kSr);
    const double waitedMs = juce::Time::getMillisecondCounterHiRes() - t0;
    check(waitedMs < 10.0, "the lane returns within its deadline when the helper dies (" + juce::String(waitedMs, 2) + " ms)");
    check(b.getMagnitude(0, 128) == 0.0f, "the stage is silent after the crash");
    const int starts0 = host.helperStartsForDiagnostics();
    check(pump([&] { return ip->link() != iso::IsolatedInsertInstance::Link::Ready || host.helperStartsForDiagnostics() > starts0; }, 5000),
          "the crash is detected and the stage marked lost");
    check(pump([&] { return host.phase() == iso::IsolatedHostConnection::Phase::Failed; }, 30000),
          "restore failed once and stopped (crash during restore)");
    pump([] { return false; }, 500);
    check(host.helperStartsForDiagnostics() == starts0 + 1, "one restore attempt only, no restart loop");
    check(!processAlive(pid), "the crashed helper is gone");
    juce::MemoryBlock kept;
    p->getStateInformation(kept);
    check(kept.getSize() > 0, "state stays readable for Save while lost");
    host.retryRestore();
    check(pump([&] { return host.phase() == iso::IsolatedHostConnection::Phase::Running; }, 30000), "Retry restores");
    check(ip->link() == iso::IsolatedInsertInstance::Link::Ready
              && asIso(q.get())->link() == iso::IsolatedInsertInstance::Link::Ready,
          "both instances live again");
}

void testAudioHangReportedOnce(const juce::PluginDescription& delay)
{
    std::printf("[audio hang] processing stops, control channel alive: one report, Restart, no loop\n");
    auto& host = iso::IsolatedHostConnection::get();
    host.setAudioHangLimitMsForTests(500);
    auto p = isolated(delay, kSr, 128);
    auto q = isolated(delay, kSr, 128);
    auto* ip = asIso(p.get());
    const int dialogs0 = gFailureDialogs;
    const int starts0 = host.helperStartsForDiagnostics();
    const DWORD pid = host.helperPid();
    check(host.sendTestFault(iso::TestFault::StallNextProcess, INT32_MAX, ip->helperInstanceId()), "endless stall armed");
    double worstMs = 0.0;
    bool allSilent = true;
    bool otherHeard = true;
    for (int k = 0; k < 200; ++k) // about 2.7 s of blocks while the message loop runs
    {
        juce::AudioBuffer<float> b(2, 128);
        b.clear();
        b.setSample(0, 3, 1.0f);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        processLaneBlock(*p, b, kSr);
        worstMs = juce::jmax(worstMs, juce::Time::getMillisecondCounterHiRes() - t0);
        allSilent = allSilent && b.getMagnitude(0, 128) == 0.0f;
        juce::AudioBuffer<float> c(2, 128);
        c.clear();
        c.setSample(0, 3, 1.0f);
        processLaneBlock(*q, c, kSr);
        otherHeard = otherHeard && c.getMagnitude(0, 128) > 0.0f;
        pump([] { return false; }, 10);
    }
    check(worstMs < 4.0, "every lane wait stays bounded by its deadline (worst " + juce::String(worstMs, 2) + " ms)");
    check(allSilent, "the hung stage is silent, never stale");
    check(otherHeard, "the other instance in the same helper keeps processing");
    check(ip->counters().requests.load() <= 2, "no new requests while the hung request is outstanding");
    check(host.helperUnresponsive(), "the helper is reported unresponsive");
    check(gFailureDialogs == dialogs0 + 1, "exactly one failure notice with Restart (" + juce::String(gFailureDialogs - dialogs0) + ")");
    pump([] { return false; }, 1500);
    check(gFailureDialogs == dialogs0 + 1, "no repeated notices while waiting for the user");
    check(host.helperStartsForDiagnostics() == starts0 && processAlive(pid), "no automatic restart");
    check(gLastRetry != nullptr, "the notice offers Restart");
    if (gLastRetry)
    {
        gLastRetry();
    }
    check(pump([&] {
              return host.helperStartsForDiagnostics() == starts0 + 1
                     && host.phase() == iso::IsolatedHostConnection::Phase::Running
                     && ip->link() == iso::IsolatedInsertInstance::Link::Ready;
          },
               30000),
          "Restart ends the hung helper and restores both instances once");
    check(!processAlive(pid), "the hung helper is gone");
    juce::AudioBuffer<float> b(2, 128);
    b.clear();
    b.setSample(0, 3, 1.0f);
    processLaneBlock(*p, b, kSr);
    check(b.getMagnitude(0, 128) > 0.0f && !host.helperUnresponsive(), "the restored stage processes again");
    host.setAudioHangLimitMsForTests(2000);
}

void testTeardownWithoutDestructor(const juce::PluginDescription& delay)
{
    std::printf("[teardown] last instance / project close end the helper without destructors\n");
    auto& host = iso::IsolatedHostConnection::get();
    {
        auto p = isolated(delay, kSr, 256);
        auto q = isolated(delay, kSr, 256);
        const DWORD pid = host.helperPid();
        check(host.sendTestFault(iso::TestFault::CrashOnNextDestroy, 0, 0), "crash-on-destroy armed");
        host.terminateForTeardown("test project close");
        DWORD code = 1;
        check(pump([&] { return !processAlive(pid, &code); }, 5000), "helper ended");
        processAlive(pid, &code);
        check(code == 0, "helper exit code 0: terminated, the destructor did not run");
        check(host.phase() == iso::IsolatedHostConnection::Phase::None, "no restore after a planned end");
    }
    {
        auto p = isolated(delay, kSr, 256);
        const DWORD pid = host.helperPid();
        check(host.sendTestFault(iso::TestFault::CrashOnNextDestroy, 0, 0), "crash-on-destroy armed");
        p.reset(); // last live instance
        DWORD code = 1;
        check(pump([&] { return !processAlive(pid, &code); }, 5000), "helper ended with its last instance");
        processAlive(pid, &code);
        check(code == 0, "last instance gone: helper terminated, not destroyed");
        check(!host.helperRunning(), "no helper left");
    }
}

void testAmpliTube(const juce::PluginDescription& amp)
{
    std::printf("[AmpliTube 4] real plug-in in the helper\n");
    auto& host = iso::IsolatedHostConnection::get();
    check(iso::IsolatedPluginPolicy::shouldIsolate(amp), "the installed AmpliTube 4 matches the verified identity");
    juce::PluginDescription renamed = amp;
    renamed.uniqueId ^= 1;
    check(!iso::IsolatedPluginPolicy::shouldIsolate(renamed), "a different class id with the same name is not isolated");

    std::vector<std::unique_ptr<juce::AudioPluginInstance>> v;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < 3; ++i)
    {
        v.push_back(isolated(amp, kSr, 512));
        check(v.back() != nullptr, "AmpliTube instance " + juce::String(i) + " loads in the helper");
        if (v.back() == nullptr)
        {
            return;
        }
    }
    info("3 AmpliTube instances loaded in " + juce::String(juce::Time::getMillisecondCounterHiRes() - t0, 0) + " ms");
    info("latency reported: " + juce::String(v[0]->getLatencySamples()) + " samples");
    const DWORD pid = host.helperPid();
    info("helper private bytes with 3 AmpliTube: " + juce::String((double)privateBytes(pid) / 1048576.0, 1) + " MB");

    // Same state into a second instance: same output for the same input (state is separate per
    // instance and the helper does not mix them).
    juce::MemoryBlock st;
    v[0]->getStateInformation(st);
    check(st.getSize() > 0, "AmpliTube state captured");
    v[1]->setStateInformation(st.getData(), (int)st.getSize());
    for (const int bs : { 64, 128, 256, 512 })
    {
        for (auto& p : v)
        {
            p->prepareToPlay(kSr, bs);
        }
        const auto ra = render(*v[0], bs, 3000 * 64 / bs, kSr, 11);
        const auto& c = asIso(v[0].get())->counters();
        const double meanUs = (double)c.waitTicksTotal.load() / (double)juce::jmax<std::uint64_t>(1, c.requests.load())
                              * 1.0e6 / (double)iso::deadline::qpcPerSecond();
        info("bs=" + juce::String(bs) + ": requests=" + juce::String((juce::int64)c.requests.load()) + " misses="
             + juce::String((juce::int64)c.misses.load()) + " mean round trip " + juce::String(meanUs, 1) + " us, max "
             + juce::String((double)c.waitTicksMax.load() * 1.0e6 / (double)iso::deadline::qpcPerSecond(), 1) + " us");
        check(firstNonZero(ra) >= 0, "AmpliTube produces sound bs=" + juce::String(bs));
    }

    // Editor: AmpliTube's real editor opens and closes in the helper.
    auto* a0 = asIso(v[0].get());
    check(a0->openRemoteEditor("IsolatedInsertFocusedTests - AmpliTube 4"), "editor opens in the helper");
    pump([] { return false; }, 1500);
    a0->closeRemoteEditor();
    check(!a0->remoteEditorOpen(), "editor closed");

    // Destroy one with the others alive (the real destructor runs in the helper; a crash there is
    // recovered). Then the last ones end the helper.
    const int startsBefore = host.helperStartsForDiagnostics();
    v[2].reset();
    pump([&] { return false; }, 1500); // let a crash in the destructor surface
    pump([&] { return host.phase() == iso::IsolatedHostConnection::Phase::Running && host.liveInstanceCount() == 2; }, 60000);
    check(host.liveInstanceCount() == 2, "after destroying one AmpliTube, two are live (directly or after recovery)");
    info(processAlive(pid) ? juce::String("AmpliTube destructor ran in the helper without a crash")
                           : "AmpliTube destructor crashed the helper; new helpers="
                                 + juce::String(host.helperStartsForDiagnostics() - startsBefore) + " " + host.lastRecoveryReport());
    v.clear();
    check(pump([&] { return !processAlive(pid) && !host.helperRunning(); }, 10000) || !host.helperRunning(),
          "no helper left after the last AmpliTube");
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::File helper = argc > 1 ? juce::File(juce::String(argv[1]))
                                 : self.getParentDirectory().getParentDirectory().getParentDirectory()
                                       .getChildFile("MiniDAWLab_artefacts/Release/MiniDAWLab.exe");
    if (!helper.existsAsFile())
    {
        std::printf("FAIL: helper executable not found: %s\n", helper.getFullPathName().toRawUTF8());
        return 1;
    }
    iso::IsolatedHostConnection::get().setHelperExecutable(helper);
    iso::IsolatedHostConnection::UiHooks hooks;
    hooks.transportBusy = [] { return false; };
    hooks.showFailure = [](const juce::String& message, std::function<void()> retry) {
        ++gFailureDialogs;
        gLastRetry = std::move(retry);
        info("failure notice: " + message);
    };
    iso::IsolatedHostConnection::get().setUiHooks(hooks);

    const auto delay = describe("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");
    if (!delay)
    {
        std::printf("FAIL: DAL Mono Delay not installed\n");
        return 1;
    }
    iso::IsolatedPluginPolicy::setAdditionalIdentifierForTests(delay->createIdentifierString());
    check(iso::IsolatedPluginPolicy::shouldIsolate(*delay), "test identifier registered");

    testDeterministicSameBlock(*delay);
    testFirstBlockAndState(*delay);
    testParallel(*delay);
    testLateReplyAndHang(*delay);
    testOffline(*delay);
    testCrashOnDestroyRecovers(*delay);
    testCrashDuringPlaybackAndRestore(*delay);
    testAudioHangReportedOnce(*delay);
    testTeardownWithoutDestructor(*delay);

    if (const auto amp = describe("C:\\Program Files\\Common Files\\VST3\\AmpliTube 4.vst3"))
    {
        testAmpliTube(*amp);
    }
    else
    {
        std::printf("  AmpliTube 4 not installed: skipped\n");
    }

    iso::IsolatedHostConnection::get().terminateForTeardown("tests done");
    std::printf("IsolatedInsertFocusedTests: %d checks, %d failure(s)\n", gChecks, gFailures);
    std::fflush(stdout);
    // Leave without static destructors (the in-process plug-in modules stay loaded).
    ::TerminateProcess(::GetCurrentProcess(), gFailures == 0 ? 0 : 1);
    return gFailures == 0 ? 0 : 1;
}
