// =============================================================================
// InsertPresetFocusedTests — Load / Save preset and Copy / Paste settings for one insert slot
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
                return buildPreset(self->classId, comp, cont);
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
    juce::String classId { kClassId };
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

PluginInsertHost::InsertInstanceToken installAt(HostEnv& env,
                                                const TrackId track,
                                                const InsertStage stage,
                                                const juce::String& classId,
                                                StandIn*& out)
{
    auto owned = std::make_unique<StandIn>();
    owned->classId = classId;
    out = owned.get();
    std::vector<InsertSlotId> before;
    for (const auto& row : env.host.getInsertRowsForTrack(track))
    {
        before.push_back(row.slotId);
    }
    expect(env.host.installInsertInstanceForTests(track, stage, std::move(owned)), "stand-in installs");
    InsertSlotId created = kInvalidInsertSlotId;
    for (const auto& row : env.host.getInsertRowsForTrack(track))
    {
        bool seen = false;
        for (const InsertSlotId id : before)
        {
            if (id == row.slotId)
            {
                seen = true;
            }
        }
        if (!seen)
        {
            created = row.slotId;
        }
    }
    expect(created != kInvalidInsertSlotId, "the new insert has its own slot id");
    return env.host.insertInstanceToken(track, created);
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
    const char ctrlBytes[] = { 'c', 't', 'r', 'l', '\0', 'a' };
    first->controller.append(ctrlBytes, (size_t)sizeof(ctrlBytes));
    const juce::MemoryBlock controllerBytes = first->controller;
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
    expect(other->controller.getSize() == controllerBytes.getSize() && other->controller == controllerBytes,
           "controller state was applied in full, including a null byte");
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

void testSettingsClipboard()
{
    HostEnv env;
    juce::SystemClipboard::copyTextToClipboard("dal-clipboard-sentinel");
    StandIn* source = nullptr;
    StandIn* sameTrackPre = nullptr;
    StandIn* otherTrack = nullptr;
    StandIn* otherClass = nullptr;
    const auto sourceToken = installAt(env, 4, InsertStage::Post, kClassId, source);
    const auto preToken = installAt(env, 4, InsertStage::Pre, kClassId, sameTrackPre);
    const auto otherToken = installAt(env, 5, InsertStage::Pre, kClassId, otherTrack);
    const auto foreignToken = installAt(env, 6, InsertStage::Post, kOtherClassId, otherClass);
    expect(sourceToken.namesALiveInstance() && preToken.namesALiveInstance() && otherToken.namesALiveInstance()
               && foreignToken.namesALiveInstance(),
           "copy and paste address slots by token, with no active-track argument");
    expect(env.host.getInsertRowsForTrack(4).size() == 2
               && env.host.getInsertRowsForTrack(4)[0].stage == InsertStage::Pre
               && env.host.getInsertRowsForTrack(4)[1].stage == InsertStage::Post,
           "the source track holds a Pre insert and a Post insert");

    source->getPresetCalls = 0;
    sameTrackPre->getPresetCalls = 0;
    otherTrack->getPresetCalls = 0;
    otherClass->getPresetCalls = 0;
    const int recordsAtStart = env.undo.records;
    expect(!env.host.canPasteInsertSettings(preToken.trackId, preToken.slotId), "paste is off while the clipboard is empty");
    expect(!env.host.pasteInsertSettings(preToken).wasOk(), "an empty clipboard is refused");
    expect(sameTrackPre->extra == "default" && env.undo.records == recordsAtStart, "an empty paste changes nothing");
    expect(sameTrackPre->getPresetCalls == 0 && otherTrack->getPresetCalls == 0 && otherClass->getPresetCalls == 0,
           "the empty-clipboard check does not capture plug-in state");

    source->gain = 0.3f;
    source->extra = "copied-extra";
    const char ctrlBytes[] = { 's', 'i', 'd', 'e', '\0', 'z' };
    source->controller.append(ctrlBytes, (size_t)sizeof(ctrlBytes));
    const juce::MemoryBlock controllerBytes = source->controller;
    source->stateTouchesOutsideExclusive = 0;
    const int recordsBeforeCopy = env.undo.records;
    const int resetsBeforeCopy = env.queueResets;
    expect(env.host.copyInsertSettings(sourceToken).wasOk(), "copy stores the insert's preset");
    expect(env.undo.records == recordsBeforeCopy && env.queueResets == resetsBeforeCopy, "copy records no undo and does not mark a queue reset");
    expect(source->stateTouchesOutsideExclusive == 0, "copy reads state only inside exclusive access");
    expect(juce::SystemClipboard::getTextFromClipboard() == "dal-clipboard-sentinel",
           "copy does not use the system clipboard");

    source->extra = "mutated-after-copy";
    source->gain = 0.9f;
    sameTrackPre->getPresetCalls = 0;
    otherTrack->getPresetCalls = 0;
    otherClass->getPresetCalls = 0;
    expect(env.host.canPasteInsertSettings(preToken.trackId, preToken.slotId), "paste is offered for the same class on another stage");
    expect(env.host.canPasteInsertSettings(otherToken.trackId, otherToken.slotId), "paste is offered for the same class on another track");
    expect(!env.host.canPasteInsertSettings(foreignToken.trackId, foreignToken.slotId), "paste is off for a different VST3 class");
    expect(sameTrackPre->getPresetCalls == 0 && otherTrack->getPresetCalls == 0 && otherClass->getPresetCalls == 0,
           "the menu compatibility check does not capture plug-in state");

    env.host.setInsertHostBypassed(otherToken.trackId, otherToken.slotId, true);
    env.host.setInsertHostActive(otherToken.trackId, otherToken.slotId, false);
    otherTrack->extra = "replace-me";
    otherTrack->stateTouchesOutsideExclusive = 0;
    const int recordsBeforePaste = env.undo.records;
    const int resetsBeforePaste = env.queueResets;
    expect(env.host.pasteInsertSettings(otherToken).wasOk(), "paste applies the copied preset to the other track's Pre insert");
    expect(otherTrack->extra == "copied-extra" && std::abs(otherTrack->gain - 0.3f) < 1.0e-6f, "paste restores the copied parameter and side state");
    expect(otherTrack->controller == controllerBytes, "paste restores non-parameter state, including a null byte");
    expect(source->extra == "mutated-after-copy", "paste does not change the source");
    const auto pastedAudible = env.host.insertHostAudible(otherToken.trackId, otherToken.slotId);
    expect(!pastedAudible.active && pastedAudible.bypassed, "paste keeps the target's host Active and Bypass");
    expect(env.host.getInsertRowsForTrack(5).size() == 1 && env.host.getInsertRowsForTrack(5)[0].stage == InsertStage::Pre
               && env.host.getInsertRowsForTrack(5)[0].slotId == otherToken.slotId,
           "paste does not move the slot");
    expect(env.undo.records == recordsBeforePaste + 1 && env.undo.labels[env.undo.labels.size() - 1] == "Paste plugin settings",
           "a successful paste is one Paste plugin settings undo step");
    expect(env.queueResets == resetsBeforePaste + 1, "paste discards queued read-ahead audio");
    expect(otherTrack->stateTouchesOutsideExclusive == 0, "paste reads and writes state only inside exclusive access");

    env.host.importChain(otherToken.trackId, env.undo.last.before);
    expect(otherTrack->extra == "replace-me", "undo of paste restores the previous plug-in settings");
    expect(!env.host.insertHostAudible(otherToken.trackId, otherToken.slotId).active
               && env.host.insertHostAudible(otherToken.trackId, otherToken.slotId).bypassed,
           "undo of paste keeps the target's host flags");
    env.host.importChain(otherToken.trackId, env.undo.last.after);
    expect(otherTrack->extra == "copied-extra", "redo of paste applies the copied settings again");

    const PluginTrackChain pasted = env.host.exportChain(otherToken.trackId);
    expect(!pasted.slots.empty() && parseStandIn(pasted.slots[0].opaqueState).extra == "copied-extra",
           "the project blob after paste is the copied plug-in state");
    ProjectFileV1 project;
    project.version = ProjectFileV1::kCurrentVersion;
    project.timelineSampleRate = kRate;
    project.deviceSampleRateAtSave = kRate;
    ProjectFileTrackV1 track;
    track.id = otherToken.trackId;
    track.kind = "audio";
    ProjectFileInsertV1 row;
    row.slotId = pasted.slots[0].slotId;
    row.stage = pasted.slots[0].stage;
    row.pluginVst3Path = pasted.slots[0].vst3AbsolutePath;
    row.pluginIdentifier = pasted.slots[0].pluginIdentifier;
    row.hostActive = pasted.slots[0].hostActive;
    row.hostBypassed = pasted.slots[0].hostBypassed;
    row.pluginStateBase64 = juce::Base64::toBase64(pasted.slots[0].opaqueState.getData(), pasted.slots[0].opaqueState.getSize());
    track.inserts.push_back(row);
    project.tracks.push_back(track);
    const juce::File proj = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-insert-paste-project.dalproj");
    expect(writeProjectFile(proj, project).wasOk(), "project file records the pasted state");
    ProjectFileV1 back;
    const ProjectFileInsertV1* reopened = nullptr;
    if (readProjectFile(proj, back).wasOk())
    {
        for (const auto& tr : back.tracks)
        {
            if (tr.id == otherToken.trackId && !tr.inserts.empty())
            {
                reopened = &tr.inserts[0];
            }
        }
    }
    juce::MemoryOutputStream decoded;
    expect(reopened != nullptr && juce::Base64::convertFromBase64(decoded, reopened->pluginStateBase64),
           "project file reads the pasted insert back");
    const juce::MemoryBlock decodedState(decoded.getData(), decoded.getDataSize());
    expect(parseStandIn(decodedState).extra == "copied-extra" && reopened != nullptr && !reopened->hostActive && reopened->hostBypassed,
           "a reopened project keeps the pasted settings and the target's host flags");
    proj.deleteFile();

    expect(env.host.pasteInsertSettings(preToken).wasOk(), "the same clipboard also pastes onto the source track's Pre insert");
    expect(sameTrackPre->extra == "copied-extra" && std::abs(sameTrackPre->gain - 0.3f) < 1.0e-6f,
           "paste between Pre and Post on one track uses the copied state");

    const int recordsBeforeForeign = env.undo.records;
    const juce::String foreignExtra = otherClass->extra;
    otherClass->setPresetCalls = 0;
    expect(!env.host.pasteInsertSettings(foreignToken).wasOk(), "a different VST3 class is refused");
    expect(otherClass->extra == foreignExtra && otherClass->setPresetCalls == 0 && env.undo.records == recordsBeforeForeign,
           "a refused paste leaves the instance, dirty and undo unchanged");

    auto staleEpoch = otherToken;
    staleEpoch.instanceEpoch += 1;
    const int recordsBeforeStale = env.undo.records;
    expect(!env.host.pasteInsertSettings(staleEpoch).wasOk(), "a replaced instance epoch is refused");
    expect(otherTrack->extra == "copied-extra" && env.undo.records == recordsBeforeStale,
           "a stale paste does not write the new instance");

    source->extra = "should-not-replace-clipboard";
    env.blockAccess = true;
    const int recordsBeforeBusyCopy = env.undo.records;
    expect(!env.host.copyInsertSettings(sourceToken).wasOk(), "a busy worker blocks the copy");
    expect(env.pauseDepth == 0 && env.undo.records == recordsBeforeBusyCopy, "a failed copy holds no pause and records nothing");
    env.blockAccess = false;

    StandIn* afterBusy = nullptr;
    const auto afterBusyToken = installAt(env, 7, InsertStage::Post, kClassId, afterBusy);
    afterBusy->extra = "before-kept-clipboard";
    expect(env.host.pasteInsertSettings(afterBusyToken).wasOk(), "a failed copy leaves the previous clipboard");
    expect(afterBusy->extra == "copied-extra", "the paste after a failed copy is still the earlier copy");

    afterBusy->extra = "untouched";
    const int recordsBeforeBusyPaste = env.undo.records;
    env.blockAccess = true;
    expect(!env.host.pasteInsertSettings(afterBusyToken).wasOk(), "a busy worker blocks the paste");
    expect(afterBusy->extra == "untouched" && env.undo.records == recordsBeforeBusyPaste && env.pauseDepth == 0,
           "a failed paste leaves state, dirty and undo unchanged");
    env.blockAccess = false;

    env.host.removeInsert(sourceToken.trackId, sourceToken.slotId);
    StandIn* survivor = nullptr;
    const auto survivorToken = installAt(env, 8, InsertStage::Post, kClassId, survivor);
    survivor->extra = "waiting";
    expect(env.host.pasteInsertSettings(survivorToken).wasOk(), "the clipboard survives removal of the source insert");
    expect(survivor->extra == "copied-extra", "paste after the source is gone still restores the original copy");
    expect(!env.host.pasteInsertSettings(sourceToken).wasOk(), "the removed source token cannot receive the paste");
    expect(survivor->extra == "copied-extra", "a stale source token does not redirect the paste");

    const auto removedTarget = survivorToken;
    env.host.removeInsert(survivorToken.trackId, survivorToken.slotId);
    StandIn* replacement = nullptr;
    const auto replacementToken = installAt(env, 8, InsertStage::Post, kClassId, replacement);
    replacement->extra = "replacement";
    replacement->setPresetCalls = 0;
    const int recordsBeforeReplacement = env.undo.records;
    expect(!env.host.pasteInsertSettings(removedTarget).wasOk(), "a token captured before the slot was replaced is refused");
    expect(replacement->extra == "replacement" && replacement->setPresetCalls == 0 && env.undo.records == recordsBeforeReplacement,
           "the replacement instance does not receive the old menu's paste");
}

juce::MemoryBlock wrapperState(juce::AudioPluginInstance& instance)
{
    juce::MemoryBlock blob;
    instance.getStateInformation(blob);
    return blob;
}

/// Bytes of one child in the JUCE VST3 host blob (`IComponent` or `IEditController`).
/// Empty when that child is absent. Comparison is the whole decoded block.
juce::MemoryBlock wrapperChild(const juce::MemoryBlock& blob, const char* xmlName)
{
    juce::MemoryBlock raw;
    const std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(blob.getData(), (int)blob.getSize());
    if (xml == nullptr || xmlName == nullptr)
    {
        return raw;
    }
    if (auto* node = xml->getChildByName(xmlName))
    {
        raw.fromBase64Encoding(node->getAllSubText().trim());
    }
    return raw;
}

bool sameBlock(const juce::MemoryBlock& a, const juce::MemoryBlock& b)
{
    return a == b;
}

/// Processor bytes PresetProbe writes for gain 0.25 after one processBlock.
/// The hosted component state starts with this and may continue with the wrapper's private trailer.
juce::MemoryBlock expectedProbePayload()
{
    juce::MemoryOutputStream stream;
    stream.write("DALPRB1", 7);
    stream.writeFloat(0.25f);
    stream.writeString("side-state:" + juce::String(0.25f, 3));
    return { stream.getData(), stream.getDataSize() };
}

struct DirectPresetVisit final : juce::ExtensionsVisitor
{
    const juce::MemoryBlock* incoming = nullptr;
    bool writeOk = false;
    juce::MemoryBlock outgoing;

    void visitVST3Client(const VST3Client& client) override
    {
        if (incoming != nullptr)
        {
            writeOk = client.setPreset(*incoming);
        }
        else
        {
            outgoing = client.getPreset();
        }
    }
};

bool startsWithBlock(const juce::MemoryBlock& block, const juce::MemoryBlock& prefix)
{
    if (prefix.getSize() == 0 || block.getSize() < prefix.getSize())
    {
        return false;
    }
    return std::memcmp(block.getData(), prefix.getData(), prefix.getSize()) == 0;
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
    const juce::MemoryBlock sourceWrapper = wrapperState(*instance);
    const juce::MemoryBlock sourceComponent = wrapperChild(sourceWrapper, "IComponent");
    const juce::MemoryBlock sourceController = wrapperChild(sourceWrapper, "IEditController");
    const juce::MemoryBlock probePayload = expectedProbePayload();
    expect(startsWithBlock(sourceComponent, probePayload),
           "probe component state begins with the gain and the non-parameter side string");

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
    juce::MemoryBlock comp;
    juce::MemoryBlock cont;
    expect(vst3_preset::copyChunk(file.getData(), file.getSize(), "Comp", comp) && sameBlock(comp, sourceComponent),
           "wrapper preset's processor chunk is the component state");
    // PresetFile still writes a Cont entry when IEditController::getState returns
    // kNotImplemented. The host blob then has no IEditController child. Both payloads are empty.
    (void)vst3_preset::copyChunk(file.getData(), file.getSize(), "Cont", cont);
    expect(sameBlock(cont, sourceController), "wrapper preset's controller chunk is the controller state");

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
    if (!second->getParameters().isEmpty())
    {
        second->getParameters()[0]->setValueNotifyingHost(0.10f);
    }
    second->processBlock(buffer, midi);
    expect(!startsWithBlock(wrapperChild(wrapperState(*second), "IComponent"), probePayload),
           "the load target holds a different processor state than the preset");
    const int recordsBefore = undo.records;
    expect(host.loadInsertPresetFromFile(otherToken, preset).wasOk(), "wrapper load applies the preset to that slot");
    expect(undo.records == recordsBefore + 1, "wrapper load records one undo step");
    expect(queueResets >= 1, "wrapper load discards the read-ahead queue");
    expect(host.insertHostAudible(track, token.slotId).bypassed, "the source insert's Bypass is untouched");
    expect(!host.insertHostAudible(other, otherToken.slotId).bypassed && host.insertHostAudible(other, otherToken.slotId).active,
           "the loaded insert's host Active and Bypass stay at its own values");
    const juce::MemoryBlock loadedWrapper = wrapperState(*second);
    expect(sameBlock(wrapperChild(loadedWrapper, "IComponent"), sourceComponent)
               && sameBlock(wrapperChild(loadedWrapper, "IEditController"), sourceController),
           "wrapper load restores the whole component and controller state");
    expect(!second->getParameters().isEmpty() && std::abs(second->getParameters()[0]->getValue() - 0.25f) < 1.0e-4f,
           "wrapper load restores the gain parameter");
    expect(host.getInsertRowsForTrack(other)[0].slotId == otherToken.slotId
               && host.getInsertRowsForTrack(other)[0].stage == InsertStage::Pre,
           "wrapper load keeps the Pre slot where it was");

    auto foreign = file;
    auto* raw = static_cast<char*>(foreign.getData());
    raw[8] = raw[8] == '0' ? '1' : '0';
    const juce::File foreignFile = preset.getSiblingFile("dal-preset-probe-foreign.vstpreset");
    foreignFile.replaceWithData(foreign.getData(), foreign.getSize());
    const int recordsBeforeForeign = undo.records;
    expect(!host.loadInsertPresetFromFile(otherToken, foreignFile).wasOk(), "wrapper refuses a different class id");
    expect(sameBlock(wrapperState(*second), loadedWrapper) && undo.records == recordsBeforeForeign,
           "a refused wrapper preset leaves the instance and undo unchanged");

    auto chopped = file;
    chopped.setSize(juce::jmin((size_t)30, chopped.getSize()));
    const juce::File choppedFile = preset.getSiblingFile("dal-preset-probe-chopped.vstpreset");
    choppedFile.replaceWithData(chopped.getData(), chopped.getSize());
    expect(!host.loadInsertPresetFromFile(otherToken, choppedFile).wasOk(), "wrapper refuses a truncated preset");
    expect(sameBlock(wrapperState(*second), loadedWrapper), "a truncated preset leaves the wrapper state unchanged");

    const PluginTrackChain savedMaster = host.exportChain(other);
    const PluginTrackChain savedSource = host.exportChain(track);
    const juce::String storedPath = savedMaster.slots.empty() ? juce::String() : savedMaster.slots[0].vst3AbsolutePath;
    const juce::File project = preset.getSiblingFile("dal-preset-probe.dalproj");
    expect(session.saveProjectToFile(transport, project, kRate, &host).wasOk(), "Save writes the loaded probe state");
    ProjectFileV1 savedProject;
    const ProjectFileInsertV1* savedMasterInsert = nullptr;
    const ProjectFileInsertV1* savedSourceInsert = nullptr;
    if (readProjectFile(project, savedProject).wasOk())
    {
        for (const auto& tr : savedProject.tracks)
        {
            if (tr.inserts.empty())
            {
                continue;
            }
            if (tr.id == other)
            {
                savedMasterInsert = &tr.inserts[0];
            }
            if (tr.id == track)
            {
                savedSourceInsert = &tr.inserts[0];
            }
        }
    }
    auto decodedEquals = [](const ProjectFileInsertV1* row, const juce::MemoryBlock& expected) {
        if (row == nullptr)
        {
            return false;
        }
        juce::MemoryOutputStream decoded;
        if (!juce::Base64::convertFromBase64(decoded, row->pluginStateBase64))
        {
            return false;
        }
        const juce::MemoryBlock got(decoded.getData(), decoded.getDataSize());
        return sameBlock(got, expected);
    };
    expect(savedMaster.slots.size() == 1 && decodedEquals(savedMasterInsert, savedMaster.slots[0].opaqueState),
           "the project file's master insert state matches the live wrapper blob");
    expect(savedSource.slots.size() == 1 && decodedEquals(savedSourceInsert, savedSource.slots[0].opaqueState),
           "the project file's source insert state matches the live wrapper blob");
    expect(savedMasterInsert != nullptr && savedMasterInsert->hostActive && !savedMasterInsert->hostBypassed,
           "the project file keeps the loaded insert Active and not Bypassed");
    expect(savedSourceInsert != nullptr && savedSourceInsert->hostBypassed,
           "the project file keeps the source insert's Bypass");

    // JUCE 8.0.4's instance description on Linux is the .so inside the bundle. That path is not a
    // .vst3 file, so production load cannot instantiate it. A placeholder is not a restored plugin.
    // The bundle passed to this test is already the .vst3 directory; the host replaces it on export.
    if (!juce::File::createFileWithoutCheckingPath(storedPath).hasFileExtension(".vst3"))
    {
        std::printf("[SKIP] live project reopen was not verified. Stored path is \"%s\", not a .vst3 bundle. "
                    "Left for the Windows test.\n",
                    storedPath.toRawUTF8());
    }
    else
    {
        Session reopened;
        Transport transport2;
        PluginInsertHost host2;
        host2.prepareForDevice(kRate, 256, 2);
        juce::StringArray skipped;
        juce::String note;
        expect(reopened.loadProjectFromFile(transport2, project, kRate, skipped, note, &host2).wasOk(),
               "reopen loads the project");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
        auto* restored = host2.liveInstanceAtChainIndexForDiagnostics(other, 0);
        const auto restoredRows = host2.getInsertRowsForTrack(other);
        expect(restored != nullptr && !restoredRows.empty() && !restoredRows[0].unavailable,
               "reopen instantiates the Pre insert");
        if (restored != nullptr)
        {
            const juce::MemoryBlock restoredWrapper = wrapperState(*restored);
            expect(sameBlock(wrapperChild(restoredWrapper, "IComponent"), sourceComponent)
                       && sameBlock(wrapperChild(restoredWrapper, "IEditController"), sourceController),
                   "reopen restores the whole component and controller state");
        }
        expect(!restoredRows.empty() && restoredRows[0].hostActive && !restoredRows[0].hostBypassed,
               "reopen keeps host Active on and Bypass off");
        const auto sourceRows = host2.getInsertRowsForTrack(track);
        expect(!sourceRows.empty() && !sourceRows[0].unavailable && sourceRows[0].hostBypassed,
               "reopen keeps the other insert's Bypass on a live instance");
    }

    // Bare setPreset, then the same blob DAL captures for the second apply.
    // The 0.85 host value is written first so a blob taken before setPreset could not match.
    if (!second->getParameters().isEmpty())
    {
        second->getParameters()[0]->setValueNotifyingHost(0.85f);
    }
    second->processBlock(buffer, midi);
    const juce::MemoryBlock beforeBare = wrapperState(*second);
    expect(!startsWithBlock(wrapperChild(beforeBare, "IComponent"), probePayload),
           "before the bare setPreset the processor is not the loaded preset");
    DirectPresetVisit bare;
    bare.incoming = &file;
    second->getExtensions(bare);
    expect(bare.writeOk, "bare setPreset accepts the preset the wrapper just wrote");
    const float hostAfterBare = second->getParameters().isEmpty() ? -1.0f : second->getParameters()[0]->getValue();
    std::printf("host parameter immediately after bare setPreset: %.6f\n", hostAfterBare);
    const juce::MemoryBlock capturedAfterSetPreset = wrapperState(*second);
    const juce::MemoryBlock capturedComponent = wrapperChild(capturedAfterSetPreset, "IComponent");
    const juce::MemoryBlock capturedController = wrapperChild(capturedAfterSetPreset, "IEditController");
    expect(sameBlock(capturedComponent, sourceComponent),
           "the blob captured after setPreset is the newly loaded processor state");
    expect(capturedComponent.getSize() == sourceComponent.getSize()
               && capturedController.getSize() == sourceController.getSize()
               && sameBlock(capturedController, sourceController),
           "that blob's controller bytes match the preset, length included");
    expect(!sameBlock(capturedAfterSetPreset, beforeBare),
           "the captured blob is not the pre-preset wrapper state");
    second->setStateInformation(capturedAfterSetPreset.getData(), (int)capturedAfterSetPreset.getSize());
    const float hostAfterSecond = second->getParameters().isEmpty() ? -1.0f : second->getParameters()[0]->getValue();
    std::printf("host parameter after setStateInformation of that blob: %.6f\n", hostAfterSecond);
    expect(std::abs(hostAfterBare - 0.25f) < 1.0e-4f,
           "message-thread setPreset already refreshed the host parameter via the edit controller restart");
    expect(std::abs(hostAfterSecond - 0.25f) < 1.0e-4f
               && sameBlock(wrapperChild(wrapperState(*second), "IComponent"), sourceComponent),
           "the second apply keeps the loaded preset and does not restore the previous parameter");

    // JUCE 8.0.4's edit controller does not implement IEditController::getState (kNotImplemented).
    // PresetFile still writes a zero-length Cont. A juce_add_plugin cannot supply a distinct
    // controller stream. Inject one and check the real wrapper drops it while keeping Comp.
    juce::MemoryBlock injectedCont;
    const unsigned char contBytes[] = { 'C', 'o', 'n', 't', 0x00, 'Z', 0x7f };
    injectedCont.append(contBytes, sizeof(contBytes));
    expect(injectedCont.getSize() == sizeof(contBytes)
               && static_cast<const unsigned char*>(injectedCont.getData())[4] == 0
               && !(injectedCont == sourceComponent),
           "the injected controller state is non-empty, contains a null, and is not the processor state");
    if (!second->getParameters().isEmpty())
    {
        second->getParameters()[0]->setValueNotifyingHost(0.10f);
    }
    second->processBlock(buffer, midi);
    const juce::MemoryBlock injected = buildPreset(inspection.classId, sourceComponent, injectedCont);
    const juce::File injectedFile = preset.getSiblingFile("dal-preset-probe-cont.vstpreset");
    injectedFile.replaceWithData(injected.getData(), injected.getSize());
    const int recordsBeforeInjected = undo.records;
    expect(host.loadInsertPresetFromFile(otherToken, injectedFile).wasOk(),
           "production load accepts a preset whose controller chunk the probe did not write");
    expect(undo.records == recordsBeforeInjected + 1, "that load still records one undo step");
    const juce::MemoryBlock afterInjected = wrapperState(*second);
    expect(sameBlock(wrapperChild(afterInjected, "IComponent"), sourceComponent),
           "the injected preset keeps the processor state, including the side string");
    expect(!second->getParameters().isEmpty() && std::abs(second->getParameters()[0]->getValue() - 0.25f) < 1.0e-4f,
           "the injected preset keeps the gain parameter");
    DirectPresetVisit reread;
    second->getExtensions(reread);
    juce::MemoryBlock rereadCont;
    (void)vst3_preset::copyChunk(reread.outgoing.getData(), reread.outgoing.getSize(), "Cont", rereadCont);
    std::printf("controller payload after Save/Load of an injected Cont: %u bytes (injected %u). "
                "JUCE 8.0.4 EditController::getState returns kNotImplemented, so a juce_add_plugin "
                "cannot round-trip a distinct controller state.\n",
                (unsigned)rereadCont.getSize(), (unsigned)injectedCont.getSize());
    expect(rereadCont.getSize() == 0 && sameBlock(rereadCont, sourceController),
           "the real wrapper does not keep the injected controller bytes");

    expect(!host.canPasteInsertSettings(other, otherToken.slotId), "wrapper paste is off before any copy");
    if (!instance->getParameters().isEmpty())
    {
        instance->getParameters()[0]->setValueNotifyingHost(0.42f);
    }
    instance->processBlock(buffer, midi);
    const juce::MemoryBlock copiedComponent = wrapperChild(wrapperState(*instance), "IComponent");
    const int recordsBeforeWrapperCopy = undo.records;
    expect(host.copyInsertSettings(token).wasOk(), "wrapper copy captures the probe, including the side string");
    expect(undo.records == recordsBeforeWrapperCopy, "wrapper copy records no undo");
    expect(host.canPasteInsertSettings(other, otherToken.slotId),
           "wrapper paste is offered for the same class on the other track's Pre insert");

    if (!instance->getParameters().isEmpty())
    {
        instance->getParameters()[0]->setValueNotifyingHost(0.11f);
    }
    instance->processBlock(buffer, midi);
    const juce::MemoryBlock mutatedSource = wrapperChild(wrapperState(*instance), "IComponent");
    expect(!sameBlock(mutatedSource, copiedComponent), "changing the source after copy changes its processor state");
    if (!second->getParameters().isEmpty())
    {
        second->getParameters()[0]->setValueNotifyingHost(0.05f);
    }
    second->processBlock(buffer, midi);
    host.setInsertHostBypassed(other, otherToken.slotId, true);
    const int recordsBeforeWrapperPaste = undo.records;
    expect(host.pasteInsertSettings(otherToken).wasOk(), "wrapper paste applies the copy to that Pre insert");
    expect(undo.records == recordsBeforeWrapperPaste + 1
               && undo.labels[undo.labels.size() - 1] == "Paste plugin settings",
           "wrapper paste is one Paste plugin settings undo step");
    expect(sameBlock(wrapperChild(wrapperState(*second), "IComponent"), copiedComponent),
           "wrapper paste restores the copied processor state, including the side string");
    expect(sameBlock(wrapperChild(wrapperState(*instance), "IComponent"), mutatedSource),
           "wrapper paste does not change the source");
    expect(host.insertHostAudible(other, otherToken.slotId).active
               && host.insertHostAudible(other, otherToken.slotId).bypassed,
           "wrapper paste keeps the target Active and Bypassed");
    expect(!second->getParameters().isEmpty() && std::abs(second->getParameters()[0]->getValue() - 0.42f) < 1.0e-4f,
           "wrapper paste refreshes the host gain parameter");
    host.importChain(other, undo.last.before);
    expect(!sameBlock(wrapperChild(wrapperState(*second), "IComponent"), copiedComponent),
           "undo of wrapper paste restores the previous plug-in settings");
    host.importChain(other, undo.last.after);
    expect(sameBlock(wrapperChild(wrapperState(*second), "IComponent"), copiedComponent),
           "redo of wrapper paste applies the copied settings again");

    const juce::File pasteProject = preset.getSiblingFile("dal-preset-paste.dalproj");
    expect(session.saveProjectToFile(transport, pasteProject, kRate, &host).wasOk(), "save writes the pasted probe state");
    if (!juce::File::createFileWithoutCheckingPath(storedPath).hasFileExtension(".vst3"))
    {
        std::printf("[SKIP] live reopen of a pasted preset was not verified. Stored path is \"%s\".\n",
                    storedPath.toRawUTF8());
    }
    else
    {
        Session reopenedPaste;
        Transport transportPaste;
        PluginInsertHost hostPaste;
        hostPaste.prepareForDevice(kRate, 256, 2);
        juce::StringArray skippedPaste;
        juce::String notePaste;
        expect(reopenedPaste.loadProjectFromFile(transportPaste, pasteProject, kRate, skippedPaste, notePaste, &hostPaste).wasOk(),
               "reopen loads the pasted probe project");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
        auto* restoredPaste = hostPaste.liveInstanceAtChainIndexForDiagnostics(other, 0);
        const auto restoredPasteRows = hostPaste.getInsertRowsForTrack(other);
        expect(restoredPaste != nullptr && !restoredPasteRows.empty() && !restoredPasteRows[0].unavailable,
               "reopen instantiates the pasted Pre insert");
        if (restoredPaste != nullptr)
        {
            expect(sameBlock(wrapperChild(wrapperState(*restoredPaste), "IComponent"), copiedComponent),
                   "reopen restores the pasted processor state, including the side string");
        }
        expect(!restoredPasteRows.empty() && restoredPasteRows[0].hostActive && restoredPasteRows[0].hostBypassed,
               "reopen keeps the pasted insert's host Active and Bypass");
    }
    pasteProject.deleteFile();

    preset.deleteFile();
    injectedFile.deleteFile();
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
    testSettingsClipboard();
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
