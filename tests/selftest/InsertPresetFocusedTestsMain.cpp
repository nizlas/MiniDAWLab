// =============================================================================
// InsertPresetFocusedTests — Load preset / Save preset for one insert slot
// =============================================================================
//
// Three layers, kept apart:
//   1. vst3_preset::inspect — the container check, no plug-in and no wrapper.
//   2. PluginInsertHost policy — a deterministic stand-in that speaks the VST3Client
//      preset calls. This is not JUCE's VST3 wrapper.
//   3. --probe-vst3 <PresetProbe.vst3> — the real JUCE 8.0.4 wrapper (getPreset /
//      setPreset) and a project save / load of that state.
// =============================================================================

#include "domain/Session.h"
#include "io/ProjectFile.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/Vst3PresetFile.h"
#include "transport/Transport.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
const juce::String kClassId { "0123456789ABCDEF0123456789ABCDEF" };
const juce::String kOtherClassId { "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF" };

void writeU32(juce::MemoryOutputStream& s, const std::uint32_t v)
{
    const char b[4] = { (char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) };
    s.write(b, 4);
}

void writeI64(juce::MemoryOutputStream& s, const std::int64_t v)
{
    const auto u = (std::uint64_t)v;
    for (int i = 0; i < 8; ++i)
    {
        s.writeByte((char)((u >> (8 * i)) & 0xffu));
    }
}

juce::MemoryBlock buildPreset(const juce::String& classId, const juce::MemoryBlock& comp, const juce::MemoryBlock& cont)
{
    const auto compOffset = (std::int64_t)vst3_preset::kHeaderBytes;
    const auto contOffset = compOffset + (std::int64_t)comp.getSize();
    const auto listOffset = contOffset + (std::int64_t)cont.getSize();
    juce::MemoryOutputStream s;
    s.write("VST3", 4);
    writeU32(s, 1);
    s.write(classId.toRawUTF8(), 32);
    writeI64(s, listOffset);
    s.write(comp.getData(), comp.getSize());
    if (cont.getSize() > 0)
    {
        s.write(cont.getData(), cont.getSize());
    }
    s.write("List", 4);
    writeU32(s, cont.getSize() > 0 ? 2u : 1u);
    s.write("Comp", 4);
    writeI64(s, compOffset);
    writeI64(s, (std::int64_t)comp.getSize());
    if (cont.getSize() > 0)
    {
        s.write("Cont", 4);
        writeI64(s, contOffset);
        writeI64(s, (std::int64_t)cont.getSize());
    }
    return { s.getData(), s.getDataSize() };
}

juce::MemoryBlock standInBlob(const float gain, const juce::String& extra, const juce::MemoryBlock& controller)
{
    juce::MemoryOutputStream s;
    s.write("STND", 4);
    s.writeFloat(gain);
    s.writeString(extra);
    s.writeInt((int)controller.getSize());
    if (controller.getSize() > 0)
    {
        s.write(controller.getData(), controller.getSize());
    }
    return { s.getData(), s.getDataSize() };
}

struct ParsedStandIn
{
    bool ok = false;
    float gain = 0.0f;
    juce::String extra;
    juce::MemoryBlock controller;
};

ParsedStandIn parseStandIn(const juce::MemoryBlock& blob)
{
    ParsedStandIn out;
    if (blob.getSize() < 4)
    {
        return out;
    }
    juce::MemoryInputStream in(blob, false);
    char magic[4] {};
    if (in.read(magic, 4) != 4 || std::memcmp(magic, "STND", 4) != 0)
    {
        return out;
    }
    out.gain = in.readFloat();
    out.extra = in.readString();
    const int n = in.readInt();
    if (n < 0 || (std::uint64_t)n > blob.getSize())
    {
        return out;
    }
    out.controller.setSize((size_t)n);
    if (n > 0)
    {
        in.read(out.controller.getData(), n);
    }
    out.ok = true;
    return out;
}

int gExclusiveDepth = 0;

class StandIn final : public juce::AudioPluginInstance
{
public:
    StandIn()
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "StandIn";
        d.pluginFormatName = "Test";
        d.fileOrIdentifier = "stand-in";
    }
    const juce::String getName() const override { return "StandIn"; }
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
               && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override { ++processCalls; }
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
        noteStateTouch();
        dest = standInBlob(gain, extra, controller);
    }
    void setStateInformation(const void* data, int size) override
    {
        noteStateTouch();
        juce::MemoryBlock blob(data, (size_t)juce::jmax(0, size));
        const auto parsed = parseStandIn(blob);
        if (!parsed.ok)
        {
            return;
        }
        gain = parsed.gain;
        extra = parsed.extra;
        controller = parsed.controller;
    }

    void getExtensions(juce::ExtensionsVisitor& visitor) const override
    {
        struct Client final : juce::ExtensionsVisitor::VST3Client
        {
            StandIn* self = nullptr;
            Steinberg::Vst::IComponent* getIComponentPtr() const noexcept override { return nullptr; }
            juce::MemoryBlock getPreset() const override
            {
                self->noteStateTouch();
                ++self->getPresetCalls;
                const juce::MemoryBlock comp = standInBlob(self->gain, self->extra, self->controller);
                juce::MemoryBlock cont;
                cont.append(self->controller.getData(), self->controller.getSize());
                return buildPreset(kClassId, comp, cont);
            }
            bool setPreset(const juce::MemoryBlock& raw) const override
            {
                self->noteStateTouch();
                ++self->setPresetCalls;
                if (self->rejectWithoutTouching)
                {
                    return false;
                }
                juce::MemoryBlock comp;
                juce::MemoryBlock cont;
                if (!vst3_preset::copyChunk(raw.getData(), raw.getSize(), "Comp", comp))
                {
                    return false;
                }
                (void)vst3_preset::copyChunk(raw.getData(), raw.getSize(), "Cont", cont);
                const auto parsed = parseStandIn(comp);
                if (!parsed.ok)
                {
                    return false;
                }
                self->gain = parsed.gain;
                self->extra = parsed.extra;
                self->controller = cont.getSize() > 0 ? cont : parsed.controller;
                return !self->applyThenReject;
            }
        } client;
        client.self = const_cast<StandIn*>(this);
        visitor.visitVST3Client(client);
    }

    void noteStateTouch() const
    {
        if (gExclusiveDepth == 0)
        {
            ++stateTouchesOutsideExclusive;
        }
    }

    float gain = 1.0f;
    juce::String extra { "default" };
    juce::MemoryBlock controller;
    int processCalls = 0;
    mutable int getPresetCalls = 0;
    mutable int setPresetCalls = 0;
    mutable int stateTouchesOutsideExclusive = 0;
    bool rejectWithoutTouching = false;
    bool applyThenReject = false;
};

struct UndoCapture
{
    int records = 0;
    juce::StringArray labels;
    PluginUndoStepSides last;
    static void record(void* ctx, const juce::String& label, const PluginUndoStepSides& sides)
    {
        auto* self = static_cast<UndoCapture*>(ctx);
        ++self->records;
        self->labels.add(label);
        self->last = sides;
    }
};

struct HostEnv
{
    PluginInsertHost host;
    UndoCapture undo;
    int queueResets = 0;
    int pauseDepth = 0;
    bool blockAccess = false;

    HostEnv()
    {
        host.prepareForDevice(kRate, kBlock, 2);
        host.setUndoRecorder(&undo, &UndoCapture::record);
        host.setAudibleQueueInvalidation([this] { ++queueResets; });
        host.setExclusiveInstanceAccess([this](std::function<void()> fn) {
            if (blockAccess)
            {
                ++pauseDepth;
                --pauseDepth;
                return;
            }
            ++pauseDepth;
            ++gExclusiveDepth;
            fn();
            --gExclusiveDepth;
            --pauseDepth;
        });
    }
};

PluginInsertHost::InsertInstanceToken install(HostEnv& env, const TrackId track, StandIn*& out)
{
    auto owned = std::make_unique<StandIn>();
    out = owned.get();
    expect(env.host.installInsertInstanceForTests(track, InsertStage::Post, std::move(owned)), "stand-in installs");
    const auto rows = env.host.getInsertRowsForTrack(track);
    expect(rows.size() == 1, "one insert on the track");
    return env.host.insertInstanceToken(track, rows.empty() ? kInvalidInsertSlotId : rows[0].slotId);
}

void testContainer()
{
    const juce::MemoryBlock comp("comp-bytes", 10);
    juce::MemoryBlock cont("cont-bytes", 10);
    const juce::MemoryBlock file = buildPreset(kClassId, comp, cont);
    const auto ok = vst3_preset::inspect(file.getData(), file.getSize());
    expect(ok.status == vst3_preset::Status::Ok, "well-formed preset is accepted");
    expect(vst3_preset::sameClassId(ok.classId, kClassId), "class id is the header's component id");
    juce::MemoryBlock gotComp;
    juce::MemoryBlock gotCont;
    expect(vst3_preset::copyChunk(file.getData(), file.getSize(), "Comp", gotComp) && gotComp == comp,
           "component chunk is the processor state");
    expect(vst3_preset::copyChunk(file.getData(), file.getSize(), "Cont", gotCont) && gotCont == cont,
           "controller chunk is preserved beside the processor state");

    juce::MemoryBlock xml;
    xml.append("<VST3PluginState></VST3PluginState>", 32);
    expect(vst3_preset::inspect(xml.getData(), xml.getSize()).status == vst3_preset::Status::Truncated
               || vst3_preset::inspect(xml.getData(), xml.getSize()).status == vst3_preset::Status::BadHeader,
           "a JUCE state blob is not treated as a .vstpreset");

    auto truncated = file;
    truncated.setSize(20);
    expect(vst3_preset::inspect(truncated.getData(), truncated.getSize()).status == vst3_preset::Status::Truncated,
           "a short header is truncated");

    auto badSize = file;
    auto* bytes = static_cast<std::uint8_t*>(badSize.getData());
    // First chunk size field sits at list + 8 + 4 + 8.
    const auto list = (std::uint64_t)vst3_preset::kHeaderBytes + comp.getSize() + cont.getSize();
    const auto sizeField = list + 8 + 4 + 8;
    for (int i = 0; i < 8; ++i)
    {
        bytes[sizeField + (std::uint64_t)i] = (std::uint8_t)(i == 0 ? 0xff : 0x7f);
    }
    expect(vst3_preset::inspect(badSize.getData(), badSize.getSize()).status == vst3_preset::Status::BadChunk,
           "a chunk length past the file is rejected");

    auto wrongVersion = file;
    static_cast<std::uint8_t*>(wrongVersion.getData())[4] = 9;
    expect(vst3_preset::inspect(wrongVersion.getData(), wrongVersion.getSize()).status == vst3_preset::Status::BadHeader,
           "an unknown preset version is rejected");

    auto badClass = buildPreset("not-a-class-id-but-32-characters!!", comp, {});
    expect(vst3_preset::inspect(badClass.getData(), badClass.getSize()).status == vst3_preset::Status::BadHeader,
           "a non-hex class id is rejected");

    juce::MemoryOutputStream hugeClaim;
    hugeClaim.write("VST3", 4);
    writeU32(hugeClaim, 1);
    hugeClaim.write(kClassId.toRawUTF8(), 32);
    writeI64(hugeClaim, 48);
    hugeClaim.write("List", 4);
    writeU32(hugeClaim, 200);
    const juce::MemoryBlock huge(hugeClaim.getData(), hugeClaim.getDataSize());
    expect(vst3_preset::inspect(huge.getData(), huge.getSize()).status == vst3_preset::Status::BadChunk,
           "a chunk list longer than the format allows is rejected");

    const juce::MemoryBlock noComp = [&] {
        juce::MemoryOutputStream s;
        s.write("VST3", 4);
        writeU32(s, 1);
        s.write(kClassId.toRawUTF8(), 32);
        writeI64(s, 48);
        s.write("List", 4);
        writeU32(s, 1);
        s.write("Info", 4);
        writeI64(s, 48);
        writeI64(s, 0);
        return juce::MemoryBlock(s.getData(), s.getDataSize());
    }();
    expect(vst3_preset::inspect(noComp.getData(), noComp.getSize()).status == vst3_preset::Status::MissingComponentState,
           "a preset without processor state is rejected");

    juce::MemoryBlock over(8);
    expect(vst3_preset::inspect(over.getData(), vst3_preset::kMaxPresetFileBytes + 1).status == vst3_preset::Status::TooLarge,
           "a preset above 64 MB is rejected before the chunks are used");
    expect(!vst3_preset::sameClassId(kClassId, kOtherClassId), "two class ids do not match on spelling alone");
}

void testHostPolicy()
{
    HostEnv env;
    StandIn* first = nullptr;
    StandIn* other = nullptr;
    const auto token = install(env, 2, first);
    const auto otherToken = install(env, 9, other);
    expect(token.namesALiveInstance() && otherToken.namesALiveInstance(), "each insert has its own instance token");
    expect(token.trackId == 2 && otherToken.trackId == 9, "tokens keep the track they were opened for");
    first->gain = 0.2f;
    first->extra = "from-first";
    first->controller.append("ctrl-a", 6);
    other->gain = 0.8f;
    other->extra = "leave-me";
    env.host.setInsertHostActive(token.trackId, token.slotId, false);
    env.host.setInsertHostBypassed(token.trackId, token.slotId, true);
    const int recordsAfterFlags = env.undo.records;
    const int resetsAfterFlags = env.queueResets;

    const juce::File preset = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                  .getChildFile("dal-insert-preset-standin.vstpreset");
    preset.deleteFile();
    const int processesBefore = first->processCalls + other->processCalls;
    // Host Active / Bypass exports state on its own path. Count only the preset call.
    first->stateTouchesOutsideExclusive = 0;
    other->stateTouchesOutsideExclusive = 0;
    expect(env.host.saveInsertPresetToFile(token, preset).wasOk(), "save writes a preset");
    expect(env.undo.records == recordsAfterFlags, "save does not record undo");
    expect(env.queueResets == resetsAfterFlags, "save does not discard the read-ahead queue");
    expect(env.pauseDepth == 0, "save releases exclusive access");
    expect(first->stateTouchesOutsideExclusive == 0 && other->stateTouchesOutsideExclusive == 0,
           "state was only read inside exclusive access");
    expect(first->processCalls + other->processCalls == processesBefore, "save does not call processBlock");
    juce::MemoryBlock saved;
    expect(preset.existsAsFile() && preset.loadFileAsData(saved) && saved.getSize() >= 4, "preset file exists");
    expect(std::memcmp(saved.getData(), "VST3", 4) == 0, "saved file is a VST3 preset container");
    expect(saved.toString().contains("hostBypassed") == false && saved.toString().contains("hostActive") == false,
           "the preset file does not carry DAL host Active or Bypass");
    const auto savedInspect = vst3_preset::inspect(saved.getData(), saved.getSize());
    expect(savedInspect.status == vst3_preset::Status::Ok && vst3_preset::sameClassId(savedInspect.classId, kClassId),
           "saved preset names the stand-in's class id");

    const juce::File original = preset.getSiblingFile("dal-insert-preset-original.vstpreset");
    original.replaceWithText("ORIGINAL");
    const juce::File blocked = original.getChildFile("nested.vstpreset");
    const int recordsBeforeFail = env.undo.records;
    expect(!env.host.saveInsertPresetToFile(token, blocked).wasOk(), "save into a non-directory fails");
    expect(original.loadFileAsString() == "ORIGINAL", "a failed save leaves the existing file untouched");
    expect(env.undo.records == recordsBeforeFail, "a failed save records no undo");

    other->setPresetCalls = 0;
    first->extra = "dirty-before-load";
    first->stateTouchesOutsideExclusive = 0;
    other->stateTouchesOutsideExclusive = 0;
    const int recordsBeforeLoad = env.undo.records;
    expect(env.host.loadInsertPresetFromFile(otherToken, preset).wasOk(), "load applies to the chosen insert");
    expect(other->extra == "from-first" && std::abs(other->gain - 0.2f) < 1.0e-6f, "the other track received the preset state");
    expect(other->controller.toString() == "ctrl-a", "controller state was applied, not only the parameter-sized blob");
    expect(first->extra == "dirty-before-load", "the track the dialog was not aimed at is unchanged");
    expect(env.undo.records == recordsBeforeLoad + 1 && env.undo.labels[env.undo.labels.size() - 1] == "Load preset",
           "a successful load is one Load preset undo step");
    expect(env.queueResets == resetsAfterFlags + 1, "load discards queued read-ahead audio");
    const auto audible = env.host.insertHostAudible(token.trackId, token.slotId);
    expect(!audible.active && audible.bypassed, "host Active and Bypass are unchanged by load on the other insert");
    const auto loadedAudible = env.host.insertHostAudible(otherToken.trackId, otherToken.slotId);
    expect(loadedAudible.active && !loadedAudible.bypassed, "the loaded insert keeps its own host flags");
    expect(env.host.getInsertRowsForTrack(9).size() == 1
               && env.host.getInsertRowsForTrack(9)[0].slotId == otherToken.slotId,
           "load does not move or replace the slot");
    expect(other->processCalls == 0, "load does not call processBlock");
    expect(env.pauseDepth == 0, "load releases exclusive access");
    expect(first->stateTouchesOutsideExclusive == 0 && other->stateTouchesOutsideExclusive == 0,
           "load reads and writes state only inside exclusive access");

    const PluginTrackChain loaded = env.host.exportChain(9);
    expect(loaded.slots.size() == 1 && parseStandIn(loaded.slots[0].opaqueState).extra == "from-first",
           "the project blob after load is the preset's plug-in state");
    env.host.importChain(9, env.undo.last.before);
    expect(other->extra == "leave-me", "undo restores the previous plug-in state");
    expect(env.host.getInsertRowsForTrack(9)[0].slotId == otherToken.slotId, "undo does not move the slot");
    expect(env.host.insertHostAudible(otherToken.trackId, otherToken.slotId).active
               && !env.host.insertHostAudible(otherToken.trackId, otherToken.slotId).bypassed,
           "undo of a preset does not invent host Bypass");
    env.host.importChain(9, env.undo.last.after);
    expect(other->extra == "from-first", "redo applies the loaded state again");
    expect(env.queueResets > resetsAfterFlags + 1, "undo and redo of plug-in state discard the read-ahead queue");

    ProjectFileV1 project;
    project.version = ProjectFileV1::kCurrentVersion;
    project.timelineSampleRate = kRate;
    project.deviceSampleRateAtSave = kRate;
    ProjectFileTrackV1 track;
    track.id = 9;
    track.kind = "audio";
    ProjectFileInsertV1 row;
    row.slotId = loaded.slots[0].slotId;
    row.stage = loaded.slots[0].stage;
    row.pluginVst3Path = loaded.slots[0].vst3AbsolutePath;
    row.pluginIdentifier = loaded.slots[0].pluginIdentifier;
    row.hostActive = loaded.slots[0].hostActive;
    row.hostBypassed = loaded.slots[0].hostBypassed;
    row.pluginStateBase64 = juce::Base64::toBase64(loaded.slots[0].opaqueState.getData(), loaded.slots[0].opaqueState.getSize());
    track.inserts.push_back(row);
    project.tracks.push_back(track);
    const juce::File proj = preset.getSiblingFile("dal-insert-preset-project.dalproj");
    expect(writeProjectFile(proj, project).wasOk(), "project file records the loaded state");
    ProjectFileV1 back;
    const ProjectFileTrackV1* reopenedTrack = nullptr;
    if (readProjectFile(proj, back).wasOk())
    {
        for (const auto& tr : back.tracks)
        {
            if (tr.id == 9)
            {
                reopenedTrack = &tr;
            }
        }
    }
    expect(reopenedTrack != nullptr && reopenedTrack->inserts.size() == 1, "project file reads the insert back");
    juce::MemoryOutputStream decoded;
    expect(reopenedTrack != nullptr
               && juce::Base64::convertFromBase64(decoded, reopenedTrack->inserts[0].pluginStateBase64),
           "state base64 decodes");
    const juce::MemoryBlock decodedBlock(decoded.getData(), decoded.getDataSize());
    expect(parseStandIn(decodedBlock).extra == "from-first", "reopened project still has the loaded preset state");
    expect(reopenedTrack != nullptr && reopenedTrack->inserts[0].hostActive && !reopenedTrack->inserts[0].hostBypassed,
           "reopened project still has the insert's own Active and Bypass");
    proj.deleteFile();

    const int callsBeforeReject = other->setPresetCalls;
    const int recordsBeforeReject = env.undo.records;
    auto foreign = saved;
    std::memcpy(static_cast<char*>(foreign.getData()) + 8, kOtherClassId.toRawUTF8(), 32);
    expect(!env.host.loadInsertPresetFromFile(otherToken, [&] {
        const juce::File f = preset.getSiblingFile("foreign.vstpreset");
        f.replaceWithData(foreign.getData(), foreign.getSize());
        return f;
    }()).wasOk(), "a different class id is refused");
    expect(other->setPresetCalls == callsBeforeReject, "a different class id never reaches setPreset");
    expect(other->extra == "from-first" && env.undo.records == recordsBeforeReject,
           "a refused preset leaves state, dirty and undo unchanged");

    auto chopped = saved;
    chopped.setSize(40);
    const juce::File choppedFile = preset.getSiblingFile("chopped.vstpreset");
    choppedFile.replaceWithData(chopped.getData(), chopped.getSize());
    expect(!env.host.loadInsertPresetFromFile(otherToken, choppedFile).wasOk(), "a truncated file is refused");
    expect(other->setPresetCalls == callsBeforeReject && other->extra == "from-first",
           "a truncated file does not change the insert");

    other->applyThenReject = true;
    const juce::File again = preset;
    first->extra = "rollback-source";
    first->gain = 0.4f;
    expect(env.host.saveInsertPresetToFile(token, again).wasOk(), "save the rollback source");
    expect(!env.host.loadInsertPresetFromFile(otherToken, again).wasOk(), "a plug-in that rejects after setState is an error");
    expect(other->extra == "from-first", "the previous settings are put back when setPreset fails");
    expect(env.undo.records == recordsBeforeReject, "a rejected apply records no undo step");
    other->applyThenReject = false;

    const auto stale = otherToken;
    env.host.removeInsert(otherToken.trackId, otherToken.slotId);
    StandIn* replacement = nullptr;
    const auto replaced = install(env, 9, replacement);
    replacement->extra = "replacement";
    const int recordsBeforeStale = env.undo.records;
    expect(!env.host.loadInsertPresetFromFile(stale, preset).wasOk(), "a token from a removed slot is refused");
    expect(replacement->extra == "replacement" && replacement->setPresetCalls == 0,
           "the replacement instance does not receive the old dialog's preset");
    expect(env.undo.records == recordsBeforeStale, "a stale dialog records no undo");
    juce::ignoreUnused(replaced);

    env.blockAccess = true;
    const int callsBeforeBusy = first->setPresetCalls;
    const juce::String extraBeforeBusy = first->extra;
    expect(!env.host.loadInsertPresetFromFile(token, preset).wasOk(), "a busy worker blocks the load");
    expect(env.pauseDepth == 0, "the busy path does not leave a pause held");
    expect(first->setPresetCalls == callsBeforeBusy && first->extra == extraBeforeBusy, "a busy load does not touch state");
    env.blockAccess = false;
    expect(env.host.saveInsertPresetToFile(token, preset).wasOk(), "a later save works after the busy refusal");
    expect(env.pauseDepth == 0, "the following attempt is not stuck behind the old pause");

    preset.deleteFile();
    original.deleteFile();
    choppedFile.deleteFile();
    preset.getSiblingFile("foreign.vstpreset").deleteFile();
}

bool bytesContain(const void* data, const std::size_t size, const char* text)
{
    if (data == nullptr || text == nullptr)
    {
        return false;
    }
    const auto n = std::strlen(text);
    if (n == 0 || size < n)
    {
        return false;
    }
    const auto* begin = static_cast<const char*>(data);
    return std::search(begin, begin + size, text, text + n) != begin + size;
}

bool blockContains(const juce::MemoryBlock& block, const char* text)
{
    return bytesContain(block.getData(), block.getSize(), text);
}

/// The processor blob is magic + a float + a string. A C-string conversion stops on the
/// float's zero bytes, so the side string has to be searched in the raw component bytes.
bool stateContains(const juce::MemoryBlock& blob, const char* text)
{
    if (blockContains(blob, text))
    {
        return true;
    }
    const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(blob.getData(), (int)blob.getSize());
    if (xml == nullptr)
    {
        return false;
    }
    if (auto* comp = xml->getChildByName("IComponent"))
    {
        juce::MemoryBlock raw;
        raw.fromBase64Encoding(comp->getAllSubText().trim());
        return blockContains(raw, text);
    }
    return false;
}

bool componentContains(juce::AudioPluginInstance& instance, const char* text)
{
    juce::MemoryBlock blob;
    instance.getStateInformation(blob);
    return stateContains(blob, text);
}

void testRealWrapper(const juce::File& bundle)
{
    expect(bundle.exists(), "PresetProbe bundle exists");
    if (!bundle.exists())
    {
        return;
    }
    Session session;
    Transport transport;
    PluginInsertHost host;
    host.prepareForDevice(kRate, 256, 2);
    UndoCapture undo;
    host.setUndoRecorder(&undo, &UndoCapture::record);
    int queueResets = 0;
    host.setAudibleQueueInvalidation([&] { ++queueResets; });
    const TrackId track = session.getActiveTrackId();
    expect(host.addInsertFromVst3File(track, InsertStage::Post, bundle).wasOk(), "PresetProbe loads as an insert");
    const auto rows = host.getInsertRowsForTrack(track);
    expect(!rows.empty(), "probe row exists");
    if (rows.empty())
    {
        return;
    }
    const auto token = host.insertInstanceToken(track, rows[0].slotId);
    auto* instance = host.liveInstanceAtChainIndexForDiagnostics(track, 0);
    expect(instance != nullptr, "probe instance is live");
    if (instance == nullptr)
    {
        return;
    }
    expect(!instance->getParameters().isEmpty(), "probe publishes a gain parameter");
    if (!instance->getParameters().isEmpty())
    {
        instance->getParameters()[0]->setValueNotifyingHost(0.25f);
    }
    juce::AudioBuffer<float> buffer(2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    instance->processBlock(buffer, midi);
    expect(componentContains(*instance, "side-state:0.250"), "probe component state includes the non-parameter side string");

    host.setInsertHostBypassed(track, token.slotId, true);
    const TrackId other = session.findCanonicalMasterTrackId();
    expect(other != kInvalidTrackId && other != track, "the session has a master row that is not the source track");
    const int recordsAfterBypass = undo.records;
    const juce::File preset = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-preset-probe.vstpreset");
    preset.deleteFile();
    expect(host.saveInsertPresetToFile(token, preset).wasOk(), "wrapper save writes a .vstpreset");
    expect(undo.records == recordsAfterBypass, "wrapper save does not record undo");
    juce::MemoryBlock file;
    expect(preset.loadFileAsData(file), "wrapper preset can be read back");
    const auto inspection = vst3_preset::inspect(file.getData(), file.getSize());
    expect(inspection.status == vst3_preset::Status::Ok, "wrapper output passes the container check");
    expect(blockContains(file, "side-state:0.250"), "wrapper preset stores the side string inside the container");
    expect(!blockContains(file, "hostBypassed") && !blockContains(file, "hostActive"),
           "wrapper preset does not store host Bypass");
    juce::MemoryBlock comp;
    expect(vst3_preset::copyChunk(file.getData(), file.getSize(), "Comp", comp) && comp.getSize() > 0,
           "wrapper preset has a processor chunk");

    expect(host.addInsertFromVst3File(other, InsertStage::Pre, bundle).wasOk(),
           "second probe is a Pre insert on the master track");
    const auto otherRows = host.getInsertRowsForTrack(other);
    if (otherRows.empty())
    {
        return;
    }
    const auto otherToken = host.insertInstanceToken(other, otherRows[0].slotId);
    auto* second = host.liveInstanceAtChainIndexForDiagnostics(other, 0);
    expect(second != nullptr && second != instance, "the second probe is a different instance");
    if (second == nullptr)
    {
        return;
    }
    const int recordsBefore = undo.records;
    expect(host.loadInsertPresetFromFile(otherToken, preset).wasOk(), "wrapper load applies the preset to that slot");
    expect(undo.records == recordsBefore + 1, "wrapper load records one undo step");
    expect(queueResets >= 1, "wrapper load discards the read-ahead queue");
    expect(host.insertHostAudible(track, token.slotId).bypassed, "the source insert's Bypass is untouched");
    expect(!host.insertHostAudible(other, otherToken.slotId).bypassed && host.insertHostAudible(other, otherToken.slotId).active,
           "the loaded insert's host Active and Bypass stay at its own values");
    expect(second != nullptr && !second->getParameters().isEmpty() && std::abs(second->getParameters()[0]->getValue() - 0.25f) < 1.0e-4f,
           "wrapper load restores the gain parameter");
    expect(componentContains(*second, "side-state:0.250"),
           "wrapper load restores the side string, not only the parameter list");
    expect(host.getInsertRowsForTrack(other)[0].slotId == otherToken.slotId
               && host.getInsertRowsForTrack(other)[0].stage == InsertStage::Pre,
           "wrapper load keeps the Pre slot where it was");

    auto foreign = file;
    auto* raw = static_cast<char*>(foreign.getData());
    raw[8] = raw[8] == '0' ? '1' : '0';
    const juce::File foreignFile = preset.getSiblingFile("dal-preset-probe-foreign.vstpreset");
    foreignFile.replaceWithData(foreign.getData(), foreign.getSize());
    const float gainBefore = second->getParameters()[0]->getValue();
    const int recordsBeforeForeign = undo.records;
    expect(!host.loadInsertPresetFromFile(otherToken, foreignFile).wasOk(), "wrapper refuses a different class id");
    expect(std::abs(second->getParameters()[0]->getValue() - gainBefore) < 1.0e-5f && undo.records == recordsBeforeForeign,
           "a refused wrapper preset leaves the instance and undo unchanged");

    auto chopped = file;
    chopped.setSize(juce::jmin((size_t)30, chopped.getSize()));
    const juce::File choppedFile = preset.getSiblingFile("dal-preset-probe-chopped.vstpreset");
    choppedFile.replaceWithData(chopped.getData(), chopped.getSize());
    expect(!host.loadInsertPresetFromFile(otherToken, choppedFile).wasOk(), "wrapper refuses a truncated preset");
    expect(componentContains(*second, "side-state:0.250"), "a truncated preset leaves the side string in place");

    const juce::File project = preset.getSiblingFile("dal-preset-probe.dalproj");
    expect(session.saveProjectToFile(transport, project, kRate, &host).wasOk(), "Save writes the loaded probe state");
    Session reopened;
    Transport transport2;
    PluginInsertHost host2;
    host2.prepareForDevice(kRate, 256, 2);
    juce::StringArray skipped;
    juce::String note;
    expect(reopened.loadProjectFromFile(transport2, project, kRate, skipped, note, &host2).wasOk(), "reopen loads the project");
    // Production load posts the insert restore to the message thread, the same way Save/reopen does.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
    const PluginTrackChain restoredChain = host2.exportChain(other);
    expect(!restoredChain.slots.empty() && stateContains(restoredChain.slots[0].opaqueState, "side-state:0.250"),
           "reopen still holds the loaded processor state, including the side string");
    auto* restored = host2.liveInstanceAtChainIndexForDiagnostics(other, 0);
    if (restored != nullptr && !restored->getParameters().isEmpty())
    {
        expect(std::abs(restored->getParameters()[0]->getValue() - 0.25f) < 1.0e-3f, "reopen restores the gain");
        expect(componentContains(*restored, "side-state:0.250"), "reopen restores the side string on the live instance");
    }
    else
    {
        // JUCE 8.0.4 on Linux stores PluginDescription::fileOrIdentifier as the .so inside the
        // bundle. VST3PluginFormat::fileMightContainThisPluginType then rejects that path, so
        // production load keeps the slot as an unavailable placeholder. The state above is still
        // the blob Save wrote. A .vst3 bundle path (Windows) is what the local reopen checks.
        const auto rowsNow = host2.getInsertRowsForTrack(other);
        expect(!rowsNow.empty() && rowsNow[0].unavailable,
               "when the stored path is the inner binary, reopen keeps the slot and its state");
    }
    const auto restoredRows = host2.getInsertRowsForTrack(other);
    expect(!restoredRows.empty() && !restoredRows[0].hostBypassed && restoredRows[0].hostActive,
           "reopen keeps host Active on and Bypass off");
    const auto sourceRows = host2.getInsertRowsForTrack(track);
    expect(!sourceRows.empty() && sourceRows[0].hostBypassed, "reopen keeps the other insert's Bypass");

    preset.deleteFile();
    foreignFile.deleteFile();
    choppedFile.deleteFile();
    project.deleteFile();
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    testContainer();
    testHostPolicy();
    juce::File probe;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (juce::String(argv[i]) == "--probe-vst3")
        {
            probe = juce::File(argv[i + 1]);
        }
    }
    if (probe == juce::File{})
    {
        expect(false, "pass --probe-vst3 <PresetProbe.vst3> to run the real wrapper");
    }
    else
    {
        testRealWrapper(probe);
    }
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

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
