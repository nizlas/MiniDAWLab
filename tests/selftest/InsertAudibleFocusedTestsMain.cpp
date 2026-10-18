// =============================================================================
// InsertAudibleFocusedTests — host Active / Bypass for one insert slot
// =============================================================================
//
// Drives the production PluginInsertHost. Direct rendering uses
// audioThread_processChainForTrack. The parallel, offline and read-ahead workers use
// audioThread_processEntryChain; both are checked on the same input. No audio device.
// A separate check loads the built DAL Tuner and DAL Mono Delay bundles when they are
// present and confirms the JUCE host sees a real bypass parameter.
// =============================================================================

#include "domain/Session.h"
#include "io/ProjectFile.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/PluginTrackSlot.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool condition, const char* label)
{
    ++checks;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
    {
        ++failures;
    }
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;

class MixProbe final : public juce::AudioPluginInstance
{
public:
    explicit MixProbe(const int latencySamples = 0, const float gain = -1.0f)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true))
        , latency_(latencySamples)
        , gain_(gain)
        , line_(2, std::vector<float>(static_cast<size_t>(juce::jmax(1, latencySamples)), 0.0f))
    {
        setLatencySamples(latencySamples);
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "MixProbe";
        d.pluginFormatName = "Test";
        d.fileOrIdentifier = "mix-probe";
    }
    const juce::String getName() const override { return "MixProbe"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void reset() override
    {
        state_[0] = state_[1] = 0.0f;
        for (auto& ch : line_)
        {
            std::fill(ch.begin(), ch.end(), 0.0f);
        }
        write_ = 0;
        ++resets_;
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        ++calls_;
        const int n = buffer.getNumSamples();
        for (int c = 0; c < juce::jmin(2, buffer.getNumChannels()); ++c)
        {
            float* d = buffer.getWritePointer(c);
            for (int i = 0; i < n; ++i)
            {
                state_[c] = 0.5f * state_[c] + 0.5f * d[i];
                float y = state_[c];
                if (latency_ > 0)
                {
                    line_[static_cast<size_t>(c)][static_cast<size_t>(write_)] = y;
                    int read = write_ - latency_;
                    if (read < 0)
                    {
                        read += latency_;
                    }
                    y = line_[static_cast<size_t>(c)][static_cast<size_t>(read)];
                }
                d[i] = gain_ * y;
            }
        }
        if (latency_ > 0 && ++write_ >= latency_)
        {
            write_ = 0;
        }
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override
    {
        const char marker[] = "probe";
        dest.append(marker, sizeof(marker));
    }
    void setStateInformation(const void*, int) override {}

    int calls_ = 0;
    int resets_ = 0;
    float state_[2] { 0.0f, 0.0f };

private:
    int latency_ = 0;
    float gain_ = -1.0f;
    std::vector<std::vector<float>> line_;
    int write_ = 0;
};

void render(PluginInsertHost& host, const TrackId tid, const InsertStage stage, float* const* chans, const int n)
{
    host.audioThread_clearScratch(2, n);
    float* const* scratch = host.audioThread_getScratchWritePointers();
    for (int c = 0; c < 2; ++c)
    {
        for (int i = 0; i < n; ++i)
        {
            scratch[c][i] = chans[c][i];
        }
    }
    host.audioThread_processChainForTrack(tid, stage, n);
    for (int c = 0; c < 2; ++c)
    {
        for (int i = 0; i < n; ++i)
        {
            chans[c][i] = scratch[c][i];
        }
    }
}

void fill(std::vector<float>& a, std::vector<float>& b, const float v)
{
    std::fill(a.begin(), a.end(), v);
    std::fill(b.begin(), b.end(), v);
}

[[nodiscard]] float lastOf(const std::vector<float>& v)
{
    return v.empty() ? 0.0f : v.back();
}

void settle(PluginInsertHost& host, const TrackId tid, MixProbe* probe, const float value, const int blocks)
{
    std::vector<float> l(static_cast<size_t>(kBlock), value);
    std::vector<float> r(static_cast<size_t>(kBlock), value);
    float* chans[2] = { l.data(), r.data() };
    for (int i = 0; i < blocks; ++i)
    {
        fill(l, r, value);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    juce::ignoreUnused(probe);
}

void testAudibleMix()
{
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    const TrackId tid = 1;
    auto owned = std::make_unique<MixProbe>();
    MixProbe* const probe = owned.get();
    expect(host.installInsertInstanceForTests(tid, InsertStage::Post, std::move(owned)), "probe installs");
    const auto rows = host.getInsertRowsForTrack(tid);
    expect(rows.size() == 1 && rows[0].hostActive && !rows[0].hostBypassed, "new insert is active and not bypassed");
    const InsertSlotId sid = rows[0].slotId;

    settle(host, tid, probe, 0.4f, 30);
    std::vector<float> l(static_cast<size_t>(kBlock), 0.4f);
    std::vector<float> r = l;
    float* chans[2] = { l.data(), r.data() };
    const int callsWet = probe->calls_;
    render(host, tid, InsertStage::Post, chans, kBlock);
    expect(probe->calls_ == callsWet + 1, "active insert processes");
    expect(std::abs(lastOf(l) - (-0.4f)) < 0.02f, "settled wet output is the smoothed, inverted tone");

    host.setInsertHostBypassed(tid, sid, true);
    const int callsAtBypass = probe->calls_;
    fill(l, r, 0.4f);
    render(host, tid, InsertStage::Post, chans, kBlock);
    const float midRamp = lastOf(l);
    expect(probe->calls_ == callsAtBypass + 1, "bypass still processes");
    expect(midRamp < 0.2f && midRamp > -0.4f, "the first bypass block is inside the crossfade");
    for (int i = 0; i < 12; ++i)
    {
        fill(l, r, 0.4f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    expect(std::abs(lastOf(l) - 0.4f) < 0.02f, "settled bypass output is the dry input");
    expect(probe->calls_ > callsAtBypass + 8, "processing continued through the bypass");

    // Feed silence while bypassed so the internal state decays, then leave bypass.
    for (int i = 0; i < 40; ++i)
    {
        fill(l, r, 0.0f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    expect(std::abs(probe->state_[0]) < 0.02f, "the effect state followed the dry input while bypassed");
    host.setInsertHostBypassed(tid, sid, false);
    for (int i = 0; i < 12; ++i)
    {
        fill(l, r, 0.0f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    expect(std::abs(lastOf(l)) < 0.05f, "leaving bypass uses the current decayed state, not the old wet tone");

    settle(host, tid, probe, 0.4f, 20);
    host.setInsertHostActive(tid, sid, false);
    for (int i = 0; i < 12; ++i)
    {
        fill(l, r, 0.4f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    const int callsAfterRamp = probe->calls_;
    fill(l, r, 0.4f);
    render(host, tid, InsertStage::Post, chans, kBlock);
    expect(probe->calls_ == callsAfterRamp, "active off stops processing after the ramp");
    expect(std::abs(lastOf(l) - 0.4f) < 0.001f, "active off output is the undelayed dry input");
    const auto held = host.insertHostAudible(tid, sid);
    expect(!held.active && !held.bypassed, "active off keeps a clear bypass choice");

    host.setInsertHostBypassed(tid, sid, true);
    const auto stored = host.insertHostAudible(tid, sid);
    expect(!stored.active && stored.bypassed, "bypass can be stored while inactive");
    fill(l, r, 0.25f);
    const int callsWhileOff = probe->calls_;
    render(host, tid, InsertStage::Post, chans, kBlock);
    expect(probe->calls_ == callsWhileOff, "stored bypass does not process while inactive");
    expect(std::abs(lastOf(l) - 0.25f) < 0.001f, "inactive output stays dry");

    const int resetsBefore = probe->resets_;
    probe->state_[0] = 0.9f;
    host.setInsertHostActive(tid, sid, true);
    expect(probe->resets_ == resetsBefore + 1, "reactivation resets the instance");
    expect(std::abs(probe->state_[0]) < 0.001f, "reset cleared the frozen state");
    const auto back = host.insertHostAudible(tid, sid);
    expect(back.active && back.bypassed, "reactivation keeps the stored bypass");
}

void testTwoSlotsAndPaths()
{
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    const TrackId tid = 3;
    auto aOwned = std::make_unique<MixProbe>(0, -1.0f);
    auto bOwned = std::make_unique<MixProbe>(0, 0.5f);
    MixProbe* const a = aOwned.get();
    MixProbe* const b = bOwned.get();
    expect(host.installInsertInstanceForTests(tid, InsertStage::Post, std::move(aOwned)), "slot A installs");
    expect(host.installInsertInstanceForTests(tid, InsertStage::Post, std::move(bOwned)), "slot B installs");
    const auto rows = host.getInsertRowsForTrack(tid);
    expect(rows.size() == 2, "two post inserts");
    const InsertSlotId aId = rows[0].slotId;
    const InsertSlotId bId = rows[1].slotId;

    std::vector<float> l(static_cast<size_t>(kBlock), 1.0f);
    std::vector<float> r = l;
    float* chans[2] = { l.data(), r.data() };
    for (int i = 0; i < 20; ++i)
    {
        fill(l, r, 1.0f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    expect(std::abs(lastOf(l) - (-0.5f)) < 0.05f, "A then B: inverted, then halved");

    host.setInsertHostBypassed(tid, bId, true);
    for (int i = 0; i < 12; ++i)
    {
        fill(l, r, 1.0f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    expect(std::abs(lastOf(l) - (-1.0f)) < 0.05f, "bypassing B leaves A's output");
    const int bCalls = b->calls_;
    fill(l, r, 1.0f);
    render(host, tid, InsertStage::Post, chans, kBlock);
    expect(b->calls_ == bCalls + 1, "bypassed B is still processed");
    host.setInsertHostActive(tid, aId, false);
    for (int i = 0; i < 12; ++i)
    {
        fill(l, r, 1.0f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    const int aAfterRamp = a->calls_;
    const int bAfterRamp = b->calls_;
    fill(l, r, 1.0f);
    render(host, tid, InsertStage::Post, chans, kBlock);
    expect(a->calls_ == aAfterRamp, "only slot A stopped");
    expect(b->calls_ == bAfterRamp + 1, "slot B kept processing");

    // Same samples through the serial wrapper and the entry function (parallel / offline / read-ahead).
    PluginInsertHost direct;
    PluginInsertHost entry;
    direct.prepareForDevice(kRate, kBlock, 2);
    entry.prepareForDevice(kRate, kBlock, 2);
    expect(direct.installInsertInstanceForTests(tid, InsertStage::Post, std::make_unique<MixProbe>(0, -1.0f)), "direct probe");
    expect(entry.installInsertInstanceForTests(tid, InsertStage::Post, std::make_unique<MixProbe>(0, -1.0f)), "entry probe");
    std::vector<float> dl(static_cast<size_t>(kBlock), 0.3f), dr = dl;
    std::vector<float> el = dl, er = dl;
    float* dCh[2] = { dl.data(), dr.data() };
    float* eCh[2] = { el.data(), er.data() };
    render(direct, tid, InsertStage::Post, dCh, kBlock);
    {
        const auto map = entry.audioThread_acquireMapForBlock();
        const auto* hit = map != nullptr ? PluginInsertHost::audioThread_findEntry(*map, tid) : nullptr;
        expect(hit != nullptr, "entry map resolves the track");
        if (hit != nullptr)
        {
            juce::AudioBuffer<float> buf(2, kBlock);
            buf.copyFrom(0, 0, el.data(), kBlock);
            buf.copyFrom(1, 0, er.data(), kBlock);
            float* ptrs[2] = { buf.getWritePointer(0), buf.getWritePointer(1) };
            juce::MidiBuffer midi;
            entry.audioThread_processEntryChain(*hit, InsertStage::Post, kBlock, ptrs, kBlock, midi, 0);
            for (int i = 0; i < kBlock; ++i)
            {
                el[static_cast<size_t>(i)] = ptrs[0][i];
                er[static_cast<size_t>(i)] = ptrs[1][i];
            }
        }
    }
    float maxDiff = 0.0f;
    for (int i = 0; i < kBlock; ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(dl[static_cast<size_t>(i)] - el[static_cast<size_t>(i)]));
    }
    expect(maxDiff < 1.0e-5f, "serial chain and entry chain match");
}

void testLatencyAlign()
{
    constexpr int kLatency = 8;
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    const TrackId tid = 4;
    expect(host.installInsertInstanceForTests(tid, InsertStage::Post, std::make_unique<MixProbe>(kLatency, -1.0f)),
           "latent probe installs");
    const InsertSlotId sid = host.getInsertRowsForTrack(tid)[0].slotId;
    host.setInsertHostBypassed(tid, sid, true);
    std::vector<float> l(static_cast<size_t>(kBlock), 1.0f);
    std::vector<float> r = l;
    float* chans[2] = { l.data(), r.data() };
    for (int i = 0; i < 16; ++i)
    {
        fill(l, r, 1.0f);
        render(host, tid, InsertStage::Post, chans, kBlock);
    }
    fill(l, r, 0.25f);
    render(host, tid, InsertStage::Post, chans, kBlock);
    expect(std::abs(l[0] - 1.0f) < 0.02f, "bypass dry is delayed by the plug-in latency");
    expect(std::abs(l[static_cast<size_t>(kLatency)] - 0.25f) < 0.02f, "the new level arrives one latency later");
}

void testUndoDuplicateAndProject()
{
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    const TrackId tid = 5;
    expect(host.installInsertInstanceForTests(tid, InsertStage::Post, std::make_unique<MixProbe>()), "undo probe");
    const InsertSlotId sid = host.getInsertRowsForTrack(tid)[0].slotId;
    PluginUndoStepSides step;
    host.setUndoRecorder(&step, [](void* ctx, const juce::String&, const PluginUndoStepSides& sides) {
        *static_cast<PluginUndoStepSides*>(ctx) = sides;
    });
    host.setInsertHostBypassed(tid, sid, true);
    expect(step.after.slots.size() == 1 && step.after.slots[0].hostBypassed, "bypass records an undo step");
    expect(step.before.slots.size() == 1 && !step.before.slots[0].hostBypassed, "undo before-state is not bypassed");
    host.importChain(tid, step.before);
    expect(!host.insertHostAudible(tid, sid).bypassed, "undo restores bypass off");
    host.importChain(tid, step.after);
    expect(host.insertHostAudible(tid, sid).bypassed && host.insertHostAudible(tid, sid).active, "redo restores bypass on");

    const TrackId copyId = 6;
    host.importChain(copyId, host.exportChain(tid));
    expect(host.insertHostAudible(copyId, sid).bypassed, "duplicate import copies bypass");
    host.setInsertHostBypassed(copyId, sid, false);
    expect(host.insertHostAudible(tid, sid).bypassed, "the copy does not change the source");
    const void* const src = host.liveInstanceAtChainIndexForDiagnostics(tid, 0);
    const void* const copy = host.liveInstanceAtChainIndexForDiagnostics(copyId, 0);
    expect(src != nullptr && copy == nullptr && !host.insertHostAudible(copyId, sid).available,
           "duplicate of a missing bundle keeps the slot without sharing the source instance");

    const juce::File delayBundle("C:/Users/nicla/development/DALMonoDelay/Builds/VisualStudio2022/x64/Release/VST3/DALMonoDelay.vst3");
    if (delayBundle.exists())
    {
        PluginInsertHost real;
        real.prepareForDevice(kRate, kBlock, 2);
        const TrackId realTid = 11;
        const TrackId realCopy = 12;
        expect(real.addInsertFromVst3File(realTid, InsertStage::Post, delayBundle).wasOk(),
               "real insert installs for duplicate");
        const InsertSlotId realSid = real.getInsertRowsForTrack(realTid)[0].slotId;
        real.setInsertHostBypassed(realTid, realSid, true);
        real.importChain(realCopy, real.exportChain(realTid));
        const void* const realSrc = real.liveInstanceAtChainIndexForDiagnostics(realTid, 0);
        const void* const realDst = real.liveInstanceAtChainIndexForDiagnostics(realCopy, 0);
        expect(realSrc != nullptr && realDst != nullptr && realSrc != realDst,
               "duplicate of a loaded plug-in is a separate instance");
        expect(real.insertHostAudible(realCopy, realSid).bypassed && real.insertHostAudible(realCopy, realSid).active,
               "duplicate of a loaded plug-in copies active and bypass");
        real.setInsertHostActive(realCopy, realSid, false);
        expect(real.insertHostAudible(realTid, realSid).active && real.insertHostAudible(realTid, realSid).bypassed,
               "the loaded copy does not change the source");
    }

    ProjectFileV1 file;
    file.version = ProjectFileV1::kCurrentVersion;
    file.timelineSampleRate = kRate;
    file.deviceSampleRateAtSave = kRate;
    ProjectFileTrackV1 track;
    track.id = tid;
    track.kind = "audio";
    ProjectFileInsertV1 ins;
    ins.slotId = sid;
    ins.stage = InsertStage::Post;
    ins.pluginVst3Path = "C:/plugins/MixProbe.vst3";
    ins.hostActive = false;
    ins.hostBypassed = true;
    track.inserts.push_back(ins);
    file.tracks.push_back(track);
    const juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("dal-insert-audible-v32.dalproj");
    expect(writeProjectFile(tmp, file).wasOk(), "v32 project writes");
    ProjectFileV1 back;
    expect(readProjectFile(tmp, back).wasOk(), "v32 project reads");
    const ProjectFileTrackV1* savedTrack = nullptr;
    for (const auto& tr : back.tracks)
    {
        if (tr.id == tid)
        {
            savedTrack = &tr;
        }
    }
    expect(savedTrack != nullptr && savedTrack->inserts.size() == 1, "insert survived");
    if (savedTrack != nullptr && !savedTrack->inserts.empty())
    {
        expect(!savedTrack->inserts[0].hostActive && savedTrack->inserts[0].hostBypassed,
               "active off and bypass on round-trip");
    }
    juce::String legacy = tmp.loadFileAsString();
    legacy = legacy.replace("\"version\": 32", "\"version\": 31");
    legacy = legacy.replace(", \"active\": false", "");
    legacy = legacy.replace(", \"bypassed\": true", "");
    const juce::File old = tmp.getSiblingFile("dal-insert-audible-v31.dalproj");
    expect(old.replaceWithText(legacy), "legacy file written");
    ProjectFileV1 oldData;
    const juce::Result oldRead = readProjectFile(old, oldData);
    expect(oldRead.wasOk(), "v31 project without the keys still loads");
    if (oldRead.wasOk() && !oldData.tracks.empty() && !oldData.tracks[0].inserts.empty())
    {
        expect(oldData.tracks[0].inserts[0].hostActive && !oldData.tracks[0].inserts[0].hostBypassed,
               "missing keys mean active on, bypass off");
    }
    tmp.deleteFile();
    old.deleteFile();
}

[[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> loadBundle(const juce::File& bundle, juce::String& err)
{
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        formats.getFormat(i)->findAllTypesForFile(found, bundle.getFullPathName());
    }
    if (found.isEmpty())
    {
        err = "no types in " + bundle.getFullPathName();
        return nullptr;
    }
    return formats.createPluginInstance(*found[0], kRate, 256, err);
}

void testPluginBypassExposure(const juce::File& bundle, const char* product)
{
    if (!bundle.exists())
    {
        std::printf("[FAIL] %s bundle missing: %s\n", product, bundle.getFullPathName().toRawUTF8());
        ++checks;
        ++failures;
        return;
    }
    juce::String err;
    auto inst = loadBundle(bundle, err);
    expect(inst != nullptr, product);
    if (inst == nullptr)
    {
        std::printf("       %s\n", err.toRawUTF8());
        return;
    }
    const juce::String identity = inst->getPluginDescription().createIdentifierString();
    auto* bypass = inst->getBypassParameter();
    expect(bypass != nullptr, "host reports a bypass parameter (kIsBypass), not merely a name");
    if (bypass == nullptr)
    {
        return;
    }
    expect(bypass->getName(64) == "Bypass", "bypass parameter is named Bypass");
    bool namedActive = false;
    for (auto* p : inst->getParameters())
    {
        if (p != nullptr && p->getName(64) == "Active")
        {
            namedActive = true;
        }
    }
    expect(!namedActive, "the plug-in does not add a host Active parameter");
    const float other = inst->getParameters().size() > 1 ? inst->getParameters()[0]->getValue() : 0.0f;
    juce::AudioBuffer<float> buf(2, 64);
    for (int i = 0; i < 64; ++i)
    {
        buf.setSample(0, i, 0.2f);
        buf.setSample(1, i, 0.2f);
    }
    bypass->setValueNotifyingHost(1.0f);
    juce::MidiBuffer midi;
    inst->prepareToPlay(kRate, 64);
    inst->processBlock(buf, midi);
    float maxDelta = 0.0f;
    for (int i = 0; i < 64; ++i)
    {
        maxDelta = std::max(maxDelta, std::abs(buf.getSample(0, i) - 0.2f));
    }
    expect(maxDelta < 1.0e-4f, "external bypass leaves the dry buffer untouched");
    expect(bypass->getValue() >= 0.5f, "bypass value stays set");
    if (inst->getParameters().size() > 1)
    {
        expect(std::abs(inst->getParameters()[0]->getValue() - other) < 1.0e-5f, "bypass does not rewrite another parameter");
    }
    juce::MemoryBlock state;
    inst->getStateInformation(state);
    bypass->setValueNotifyingHost(0.0f);
    inst->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    expect(inst->getPluginDescription().createIdentifierString() == identity, "state round-trip keeps the plug-in identity");
    inst->releaseResources();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    testAudibleMix();
    testTwoSlotsAndPaths();
    testLatencyAlign();
    testUndoDuplicateAndProject();
    testPluginBypassExposure(juce::File("C:/Users/nicla/development/DALTuner/Builds/VisualStudio2022/x64/Release/VST3/DALTuner.vst3"),
                             "DAL Tuner");
    testPluginBypassExposure(juce::File("C:/Users/nicla/development/DALMonoDelay/Builds/VisualStudio2022/x64/Release/VST3/DALMonoDelay.vst3"),
                             "DAL Mono Delay");
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// Session.cpp references these instrument entry points. This harness never executes them.
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

ProjectFileExperimentalInstrumentTrackV1
InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(
    float* const*, int, int, float, float) noexcept {}
