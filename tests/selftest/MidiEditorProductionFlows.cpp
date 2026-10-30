#include "app/InstrumentMusicalUndoSnapshot.h"
#include "app/UndoRedoCoordinator.h"
#include "domain/Session.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"
#include "ui/SnapSettings.h"
#include "ui/TimelineViewportModel.h"
#include "ui/experimental/ExperimentalMidiEditorWindow.h"
#include "ui/experimental/ExperimentalMidiPattern.h"
#include "ui/experimental/ExperimentalPianoRollView.h"
#include "ui/experimental/MidiEditorCohort.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

bool isStabilityTestModeActive() noexcept
{
    return false;
}

struct MidiEditorProductionFlowAccess
{
    static juce::Rectangle<int> grid(const ExperimentalPianoRollView& roll) { return roll.gridBounds(); }
    static juce::Rectangle<int> keyboard(const ExperimentalPianoRollView& roll) { return roll.keyboardBounds(); }
    static std::optional<juce::Rectangle<float>> noteBounds(const ExperimentalPianoRollView& roll, const int index)
    {
        return roll.getTimelineNoteVisualBounds(index);
    }

    static std::optional<juce::Rectangle<float>> visualBounds(const ExperimentalPianoRollView& roll,
                                                              const InstrumentMidiClip& clip, const int index)
    {
        if (roll.timelineClip_ == &clip)
        {
            return roll.getTimelineNoteVisualBounds(index);
        }
        for (const auto& member : roll.cohort_)
        {
            if (member.clip == &clip)
            {
                return roll.cohortNoteBounds(member, index);
            }
        }
        return std::nullopt;
    }

    static bool activeClipIs(const ExperimentalPianoRollView& roll, const InstrumentMidiClip& clip)
    {
        return roll.timelineClip_ == &clip;
    }
};

namespace
{
int gFail = 0;

void expect(const bool ok, const char* label)
{
    if (!ok)
    {
        ++gFail;
        std::cerr << "FAIL " << label << "\n";
    }
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

class SilentInstrument final : public juce::AudioPluginInstance
{
public:
    SilentInstrument()
        : juce::AudioPluginInstance(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "TestTone";
        d.pluginFormatName = "Test";
        d.isInstrument = true;
    }
    const juce::String getName() const override { return "TestTone"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
};

struct MidiSink final : ExperimentalInstrumentHost::MidiDeliveryCaptureSink
{
    struct Ev
    {
        bool on = false;
        bool off = false;
        int note = -1;
        int channel = 0;
        int cc = -1;
    };
    Ev events[48] {};
    int count = 0;

    void onMidiBlockDelivered(const juce::MidiBuffer& merged, int) override
    {
        for (const auto meta : merged)
        {
            if (count >= 48)
            {
                break;
            }
            const juce::MidiMessage m = meta.getMessage();
            Ev& ev = events[count++];
            ev.on = m.isNoteOn(true);
            ev.off = m.isNoteOff(true);
            ev.note = m.isNoteOnOrOff() ? m.getNoteNumber() : -1;
            ev.channel = m.getChannel();
            ev.cc = m.isController() ? m.getControllerNumber() : -1;
        }
    }

    void clear() noexcept { count = 0; }

    [[nodiscard]] int countKind(const bool noteOn, const int pitch) const noexcept
    {
        int n = 0;
        for (int i = 0; i < count; ++i)
        {
            if (events[i].note == pitch && events[i].channel == 1 && (noteOn ? events[i].on : events[i].off))
            {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] bool hasAllNotesOff() const noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (events[i].cc == 123)
            {
                return true;
            }
        }
        return false;
    }
};

TimelineMidiNote makeNote(const int tick, const int pitch, const int velocity, const int duration = 960)
{
    TimelineMidiNote n;
    n.startTick = tick;
    n.durationTicks = duration;
    n.midiNote = pitch;
    n.velocity = velocity;
    n.channel = 1;
    return n;
}

void pump(const int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

bool clickModalButton(juce::Component* c, const juce::String& text)
{
    if (c == nullptr)
    {
        return false;
    }
    if (auto* button = dynamic_cast<juce::TextButton*>(c))
    {
        if (button->getButtonText() == text)
        {
            button->triggerClick();
            return true;
        }
    }
    for (int i = 0; i < c->getNumChildComponents(); ++i)
    {
        if (clickModalButton(c->getChildComponent(i), text))
        {
            return true;
        }
    }
    return false;
}

void armDialog(const char* buttonText)
{
    const juce::String text(buttonText);
    juce::MessageManager::callAsync([text] {
        clickModalButton(juce::Component::getCurrentlyModalComponent(), text);
    });
}

juce::MouseEvent mouseEvent(juce::Component& c, const juce::Point<float> pos, const juce::Point<float> down,
                            const juce::ModifierKeys mods, const bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, &c, &c, now, down, now, 1, dragged);
}

void clickRoll(ExperimentalPianoRollView& roll, const juce::Point<float> p, const juce::ModifierKeys mods)
{
    roll.mouseDown(mouseEvent(roll, p, p, mods, false));
    roll.mouseUp(mouseEvent(roll, p, p, juce::ModifierKeys(), false));
}

void dragRoll(ExperimentalPianoRollView& roll, const juce::Point<float> from, const juce::Point<float> to,
              const char* dialogButton)
{
    const auto mods = juce::ModifierKeys::leftButtonModifier;
    roll.mouseDown(mouseEvent(roll, from, from, mods, false));
    roll.mouseDrag(mouseEvent(roll, to, from, mods, true));
    if (dialogButton != nullptr)
    {
        armDialog(dialogButton);
    }
    roll.mouseUp(mouseEvent(roll, to, from, juce::ModifierKeys(), false));
    pump(30);
}

ExperimentalPianoRollView* findRoll(juce::Component& root)
{
    if (auto* roll = dynamic_cast<ExperimentalPianoRollView*>(&root))
    {
        return roll;
    }
    for (int i = 0; i < root.getNumChildComponents(); ++i)
    {
        if (auto* found = findRoll(*root.getChildComponent(i)))
        {
            return found;
        }
    }
    return nullptr;
}

std::int64_t projectOf(const InstrumentMidiClip& clip, const TimelineMidiNote& note)
{
    return absoluteSampleForTimelineNote(clip.timelineAnchorSamples, note, clip.pattern, kRate);
}

const TimelineMidiNote* findPitch(const InstrumentMidiClip& clip, const int pitch)
{
    for (const auto& note : clip.pattern.timelineNotes)
    {
        if (note.midiNote == pitch)
        {
            return &note;
        }
    }
    return nullptr;
}

const TimelineMidiNote* findUid(const InstrumentMidiClip& clip, const std::uint64_t uid)
{
    for (const auto& note : clip.pattern.timelineNotes)
    {
        if (note.uid == uid)
        {
            return &note;
        }
    }
    return nullptr;
}

struct Member
{
    TrackId track = kInvalidTrackId;
    InstrumentMidiClipId clipId = 0;
    InstrumentTrackController* controller = nullptr;
    ExperimentalInstrumentHost* host = nullptr;
    int order = 0;
};

struct World
{
    Session session;
    PluginInsertHost inserts;
    Transport transport;
    juce::AudioDeviceManager devices;
    TimelineViewportModel timeline;
    ExperimentalInstrumentHost hostA;
    ExperimentalInstrumentHost hostB;
    InstrumentTrackController ctlA { &hostA };
    InstrumentTrackController ctlB { &hostB };
    InstrumentTrackController ctlMidi { nullptr };
    std::unique_ptr<UndoRedoCoordinator> undo;
    std::unique_ptr<ExperimentalMidiEditorWindow> window;
    std::vector<Member> members;
    int dirty = 0;
    TrackId idA = kInvalidTrackId;
    TrackId idB = kInvalidTrackId;
    TrackId idMidi = kInvalidTrackId;

    World()
    {
        SnapSettings snap;
        snap.enabled = false;
        session.setArrangementSnapSettings(snap);
        idA = session.appendExperimentalInstrumentShellTrack("Inst A").value_or(kInvalidTrackId);
        idB = session.appendExperimentalInstrumentShellTrack("Inst B").value_or(kInvalidTrackId);
        idMidi = session.addMidiTrack().value_or(kInvalidTrackId);
        ctlA.setSession(&session);
        ctlB.setSession(&session);
        ctlMidi.setSession(&session);
        ctlA.setTimelineSampleRate(kRate);
        ctlB.setTimelineSampleRate(kRate);
        ctlMidi.setTimelineSampleRate(kRate);
        expect(ctlA.bootstrapGenericCatalogInstrumentShellForSessionTrack(idA), "bootstrap A");
        expect(ctlB.bootstrapGenericCatalogInstrumentShellForSessionTrack(idB), "bootstrap B");
        expect(ctlMidi.bootstrapMidiContentShellForSessionTrack(idMidi), "bootstrap midi");

        UndoRedoCoordinator::Callbacks callbacks;
        callbacks.buildSortedInstrumentMusicalUndoSnapshot = [this] {
            const auto snapNow = session.loadSessionSnapshotForAudioThread();
            mini_daw_app_transport::InstrumentMusicalUndoSnapshotCallbacks lookup;
            lookup.getInstrumentControllerForTrack = [this](const TrackId id) -> InstrumentTrackController* {
                if (id == idA) return &ctlA;
                if (id == idB) return &ctlB;
                if (id == idMidi) return &ctlMidi;
                return nullptr;
            };
            return mini_daw_app_transport::buildSortedInstrumentMusicalUndoSnapshot(*snapNow, lookup);
        };
        callbacks.stableSortInstrumentMusicalUndoVector = [](std::vector<ProjectFileExperimentalInstrumentTrackV1>& rows) {
            mini_daw_app_transport::stableSortInstrumentMusicalUndoVector(rows);
        };
        callbacks.applyInstrumentMusicalUndoVectorToAllKeyedAndStaging =
            [this](const std::vector<ProjectFileExperimentalInstrumentTrackV1>& rows) {
                ctlA.applyExperimentalInstrumentMusicalUndoBlock(rows);
                ctlB.applyExperimentalInstrumentMusicalUndoBlock(rows);
                ctlMidi.applyExperimentalInstrumentMusicalUndoBlock(rows);
            };
        callbacks.rebindMidiEditorAfterInstrumentMusicalUndo = [this] { rebind(); };
        callbacks.markProjectDirty = [this] { ++dirty; };
        undo = std::make_unique<UndoRedoCoordinator>(session, inserts, std::move(callbacks));
        window = std::make_unique<ExperimentalMidiEditorWindow>(hostA);
        window->setInstrumentMusicalUndoUi(
            [this](const juce::String& label, std::function<bool()> mutator) {
                undo->executeUndoableInstrumentEdit(label, std::move(mutator));
            },
            [this] { undo->invokeUndoFromWindowShortcut(); },
            [this] { undo->invokeRedoFromWindowShortcut(); });
    }

    InstrumentMidiClipId addClip(InstrumentTrackController& ctl, const std::int64_t start, const std::int64_t length,
                                 const std::int64_t anchor, std::vector<TimelineMidiNote> notes, const char* name)
    {
        const auto id = ctl.appendImportedTimelineMidiClipAtSamples(std::move(notes), start, name);
        if (auto* clip = ctl.getClipById(id))
        {
            clip->startSamples = start;
            clip->lengthSamples = length;
            clip->timelineAnchorSamples = anchor;
        }
        return id;
    }

    void addMember(const TrackId track, const InstrumentMidiClipId clipId, InstrumentTrackController* ctl,
                   ExperimentalInstrumentHost* host)
    {
        members.push_back(Member { track, clipId, ctl, host, (int)members.size() });
    }

    std::vector<ExperimentalMidiEditorWindow::CohortMemberBind> bindsNow() const
    {
        std::vector<ExperimentalMidiEditorWindow::CohortMemberBind> rows;
        for (const auto& member : members)
        {
            ExperimentalMidiEditorWindow::CohortMemberBind row;
            row.trackId = member.track;
            row.controller = member.controller;
            row.clip = member.controller != nullptr ? member.controller->getClipById(member.clipId) : nullptr;
            row.auditionHost = member.host;
            row.trackOrder = member.order;
            rows.push_back(row);
        }
        return rows;
    }

    void rebind()
    {
        if (window == nullptr)
        {
            return;
        }
        const auto idOpt = window->getBoundInstrumentClipId();
        if (!idOpt.has_value() || *idOpt == 0)
        {
            return;
        }
        for (auto* ctl : { &ctlA, &ctlB, &ctlMidi })
        {
            if (auto* clip = ctl->getClipById(static_cast<InstrumentMidiClipId>(*idOpt)))
            {
                window->bindExternalPattern(&clip->pattern, clip, ctl, &session, &transport, &devices, &timeline,
                                            "MIDI");
                window->setOpenCohort(bindsNow());
                window->refreshArrangementSnapMirrorFromSession();
                if (auto* roll = rollOrNull())
                {
                    roll->setFollowPlayheadEnabled(false);
                    roll->setTimelineNotesDisplayComboId(2);
                }
                return;
            }
        }
    }

    void show(InstrumentTrackController& activeCtl, const InstrumentMidiClipId activeId)
    {
        auto* clip = activeCtl.getClipById(activeId);
        window->bindExternalPattern(&clip->pattern, clip, &activeCtl, &session, &transport, &devices, &timeline,
                                    "MIDI");
        window->setOpenCohort(bindsNow());
        window->refreshArrangementSnapMirrorFromSession();
        window->setSize(1400, 920);
        window->resized();
        pump(20);
        if (auto* roll = rollOrNull())
        {
            if (MidiEditorProductionFlowAccess::grid(*roll).getWidth() < 32)
            {
                window->addToDesktop(0);
                window->setVisible(true);
                pump(40);
            }
            roll->setFollowPlayheadEnabled(false);
            roll->setTimelineNotesDisplayComboId(2);
        }
    }

    ExperimentalPianoRollView* rollOrNull()
    {
        return window != nullptr && window->getContentComponent() != nullptr
                   ? findRoll(*window->getContentComponent())
                   : nullptr;
    }

    InstrumentMidiClip* clipOf(const Member& member)
    {
        return member.controller != nullptr ? member.controller->getClipById(member.clipId) : nullptr;
    }
};

juce::Point<float> pointFor(ExperimentalPianoRollView& roll, const std::int64_t projectSample, const int pitch)
{
    const auto grid = MidiEditorProductionFlowAccess::grid(roll);
    const double spp = roll.getViewportSamplesPerPixel();
    const float x = (float)grid.getX()
                    + (float)(((double)projectSample - (double)roll.getViewportVisibleStartSamples()) / spp);
    const float y = (float)grid.getY()
                    + (float)(roll.topVisibleMidiPitch() - pitch) * (float)ExperimentalPianoRollView::kRowHeight
                    + (float)ExperimentalPianoRollView::kRowHeight * 0.5f;
    return { x, y };
}

juce::Point<float> centreOf(World& world, ExperimentalPianoRollView& roll, const InstrumentMidiClip& clip, const int pitch)
{
    const auto* note = findPitch(clip, pitch);
    const std::int64_t mid = note != nullptr ? projectOf(clip, *note) + note->durationTicks * 25 / 2 : 0;
    const auto predicted = pointFor(roll, mid, pitch);
    const auto bound = world.window->getBoundInstrumentClipId();
    if (note != nullptr && bound.has_value() && *bound == static_cast<std::uint64_t>(clip.id))
    {
        const int index = (int)(note - clip.pattern.timelineNotes.data());
        if (const auto bounds = MidiEditorProductionFlowAccess::noteBounds(roll, index))
        {
            if (std::abs(bounds->getCentreX() - predicted.x) < 4.0f
                && std::abs(bounds->getCentreY() - predicted.y) < 4.0f)
            {
                return bounds->getCentre();
            }
        }
    }
    return predicted;
}

void publishLayout(ExperimentalPianoRollView& roll)
{
    (void)roll.createComponentSnapshot(roll.getLocalBounds(), false, 1.0f);
    pump(15);
}

juce::Colour pixelAt(const juce::Image& image, const int x, const int y)
{
    if (!image.isValid() || x < 0 || y < 0 || x >= image.getWidth() || y >= image.getHeight())
    {
        return juce::Colour();
    }
    return image.getPixelAt(x, y);
}

bool closeTo(const juce::Colour c, const midi_editor_cohort::Rgba expected, const int tol)
{
    return std::abs((int)c.getRed() - (int)expected.r) <= tol
           && std::abs((int)c.getGreen() - (int)expected.g) <= tol
           && std::abs((int)c.getBlue() - (int)expected.b) <= tol;
}

juce::Colour interiorFill(const juce::Image& image, const juce::Rectangle<float> rect)
{
    const int x = (int)std::lround(rect.getX() + 6.0f);
    const int y = (int)std::lround(rect.getCentreY());
    return pixelAt(image, x, y);
}

void drain(ExperimentalInstrumentHost& host)
{
    host.audioThread_beginAudioBlock(kBlock);
    float left[kBlock] {};
    float right[kBlock] {};
    float* channels[] = { left, right };
    host.audioThread_processBlockAndAddToOutputs(channels, 2, kBlock);
}

bool installTone(ExperimentalInstrumentHost& host)
{
    host.prepareForDevice(kRate, kBlock);
    return host.installInstrumentInstanceForTests(std::make_unique<SilentInstrument>());
}

juce::File previewDir()
{
    juce::File dir = juce::File::getCurrentWorkingDirectory();
    if (dir.getFileName() == "ninja-release")
    {
        dir = dir.getParentDirectory().getParentDirectory();
    }
    return dir.getChildFile("build").getChildFile("midi-cohort-preview");
}

void writePng(const juce::Image& image, const juce::String& name)
{
    // The snapshot is 24-bit RGB. JUCE's PNG writer does not reproduce those pixels,
    // so the file is written from Colour::getPixelAt, which the checks also use.
    const juce::File file = previewDir().getChildFile(name).withFileExtension("ppm");
    file.deleteFile();
    file.getParentDirectory().createDirectory();
    juce::FileOutputStream stream(file);
    const int w = image.getWidth();
    const int h = image.getHeight();
    stream.writeText("P6\n" + juce::String(w) + " " + juce::String(h) + "\n255\n", false, false, nullptr);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const auto c = image.getPixelAt(x, y);
            const unsigned char rgb[3] = { c.getRed(), c.getGreen(), c.getBlue() };
            stream.write(rgb, 3);
        }
    }
}

// 4/4, 120 BPM, 48 kHz, 960 ticks per quarter: a bar is 3840 ticks.
void testSnappedNoteEdits()
{
    constexpr std::int64_t kBarTicks = 3840;
    World world;
    const auto setSnap = [&world](const bool enabled, const SnapResolution resolution) {
        SnapSettings snap;
        snap.enabled = enabled;
        snap.resolution = resolution;
        world.session.setArrangementSnapSettings(snap);
    };
    setSnap(true, SnapResolution::Straight_1_1);
    const auto id = world.addClip(world.ctlA, 0, 960000, 0,
                                  { makeNote(7680, 60, 80), makeNote(8260, 64, 70) }, "A");
    world.addMember(world.idA, id, &world.ctlA, &world.hostA);
    world.show(world.ctlA, id);
    auto* roll = world.rollOrNull();
    auto* clip = world.ctlA.getClipById(id);
    expect(roll != nullptr && clip != nullptr, "snap fixture laid out");
    if (roll == nullptr || clip == nullptr)
    {
        return;
    }
    publishLayout(*roll);
    const double spp = roll->getViewportSamplesPerPixel();
    const auto dragPitch = [&](const int pitch, const double samples) {
        const auto from = centreOf(world, *roll, *clip, pitch);
        dragRoll(*roll, from, from.translated((float)(samples / spp), 0.0f), nullptr);
        clip = world.ctlA.getClipById(id);
        return findPitch(*clip, pitch)->startTick;
    };

    expect(dragPitch(60, 20000.0) == 7680, "1/1: a short drag stays on its bar");
    expect(dragPitch(60, 60000.0) == 7680 + kBarTicks, "1/1: a note shorter than the grid lands on the next bar");
    expect(dragPitch(60, -60000.0) == 7680, "1/1: and moves back");
    expect(findPitch(*clip, 60)->durationTicks == 960, "a move keeps the note length");

    // Multi-select: the grabbed note lands on the grid, the other keeps its distance.
    const std::int64_t gap = findPitch(*clip, 64)->startTick - findPitch(*clip, 60)->startTick;
    clickRoll(*roll, centreOf(world, *roll, *clip, 60), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clip, 64),
              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    expect(dragPitch(60, 60000.0) == 7680 + kBarTicks, "multi-select: the grabbed note lands on the bar");
    expect(findPitch(*clip, 64)->startTick - findPitch(*clip, 60)->startTick == gap,
           "multi-select: the notes keep their distance");
    clickRoll(*roll, pointFor(*roll, 500000, 40), juce::ModifierKeys::leftButtonModifier);

    // Off-grid note: its new start lands on the absolute grid, it does not keep its offset.
    expect(dragPitch(64, 60000.0) % kBarTicks == 0, "an off-grid note lands on the absolute grid");

    setSnap(true, SnapResolution::Straight_1_4);
    expect(dragPitch(60, 30000.0) == 7680 + kBarTicks + 960, "1/4: the note lands on a quarter");
    setSnap(false, SnapResolution::Straight_1_4);
    const auto free = dragPitch(60, 30000.0);
    expect(free != 7680 + kBarTicks + 960 && free % 960 != 0, "snap off: the note moves freely");

    // Drawing: with snap on the note fills the grid cell under the pointer.
    setSnap(true, SnapResolution::Straight_1_1);
    const auto draw = pointFor(*roll, 300000, 72);
    roll->mouseDown(mouseEvent(*roll, draw, draw, juce::ModifierKeys::leftButtonModifier, false));
    roll->mouseUp(mouseEvent(*roll, draw, draw, juce::ModifierKeys(), false));
    roll->mouseDoubleClick(mouseEvent(*roll, draw, draw, juce::ModifierKeys::leftButtonModifier, false));
    pump(30);
    clip = world.ctlA.getClipById(id);
    const auto* drawn = findPitch(*clip, 72);
    expect(drawn != nullptr && drawn->startTick == 3 * kBarTicks && drawn->durationTicks == kBarTicks,
           "1/1: a drawn note starts on the bar and fills it");
}

void testSharedEdits()
{
    World world;
    const auto aId = world.addClip(world.ctlA, 0, 192000, 0,
                                   { makeNote(960, 60, 80), makeNote(1920, 62, 70) }, "A");
    const auto bId = world.addClip(world.ctlB, 48000, 144000, 48000, { makeNote(480, 72, 90) }, "B");
    world.addMember(world.idA, aId, &world.ctlA, &world.hostA);
    world.addMember(world.idB, bId, &world.ctlB, &world.hostB);
    world.show(world.ctlA, aId);
    auto* roll = world.rollOrNull();
    auto* clipA = world.ctlA.getClipById(aId);
    auto* clipB = world.ctlB.getClipById(bId);
    expect(roll != nullptr && clipA != nullptr && clipB != nullptr && roll->getViewportSamplesPerPixel() > 0.0,
           "edit fixture laid out");
    if (roll == nullptr || clipA == nullptr || clipB == nullptr)
    {
        return;
    }
    publishLayout(*roll);

    const auto uidA = findPitch(*clipA, 60)->uid;
    const auto uidB = findPitch(*clipB, 72)->uid;
    const auto tickA = findPitch(*clipA, 60)->startTick;
    const auto tickB = findPitch(*clipB, 72)->startTick;
    const auto projectA = projectOf(*clipA, *findPitch(*clipA, 60));
    const auto projectB = projectOf(*clipB, *findPitch(*clipB, 72));
    const int undoBefore = world.undo->undoStackSizeForDiagnostics();
    const int dirtyBefore = world.dirty;

    clickRoll(*roll, centreOf(world, *roll, *clipA, 60), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipB, 72),
              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    expect(findUid(*clipA, uidA) != nullptr && findUid(*clipA, uidA)->editorSelected, "A stays selected");
    expect(findUid(*clipB, uidB) != nullptr && findUid(*clipB, uidB)->editorSelected, "B is selected");

    const auto from = centreOf(world, *roll, *clipB, 72);
    const float dx = (float)juce::jmax(12.0, 12000.0 / roll->getViewportSamplesPerPixel());
    dragRoll(*roll, from, from.translated(dx, 0.0f), nullptr);
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    const auto* movedA = findUid(*clipA, uidA);
    const auto* movedB = findUid(*clipB, uidB);
    expect(movedA != nullptr && movedB != nullptr, "move keeps both owners");
    if (movedA == nullptr || movedB == nullptr)
    {
        return;
    }
    const auto deltaA = movedA->startTick - tickA;
    const auto deltaB = movedB->startTick - tickB;
    expect(deltaA != 0 && deltaA == deltaB, "shared move keeps the same tick delta");
    expect(projectOf(*clipA, *movedA) - projectA == projectOf(*clipB, *movedB) - projectB,
           "shared move keeps relative project time");
    expect(world.undo->undoStackSizeForDiagnostics() == undoBefore + 1, "shared move is one undo step");

    world.undo->invokeUndoFromWindowShortcut();
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    expect(findUid(*clipA, uidA)->startTick == tickA && findUid(*clipB, uidB)->startTick == tickB,
           "undo restores both events");
    world.undo->invokeRedoFromWindowShortcut();
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    expect(findUid(*clipA, uidA)->startTick == tickA + deltaA && findUid(*clipB, uidB)->startTick == tickB + deltaB,
           "redo reapplies both events");
    world.undo->invokeUndoFromWindowShortcut();

    roll = world.rollOrNull();
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    clickRoll(*roll, pointFor(*roll, 8000, 50), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipA, 60), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipB, 72),
              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    const auto durA = findUid(*clipA, uidA)->durationTicks;
    const auto durB = findUid(*clipB, uidB)->durationTicks;
    const auto* lengthNote = findPitch(*clipB, 72);
    const std::int64_t endSample = projectOf(*clipB, *lengthNote) + lengthNote->durationTicks * 25;
    const auto edge = pointFor(*roll, endSample - (std::int64_t)(3.0 * roll->getViewportSamplesPerPixel()), 72);
    const int undoLen = world.undo->undoStackSizeForDiagnostics();
    dragRoll(*roll, edge, edge.translated(dx, 0.0f), nullptr);
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    const auto grownA = findUid(*clipA, uidA)->durationTicks - durA;
    const auto grownB = findUid(*clipB, uidB)->durationTicks - durB;
    expect(grownA > 0 && grownA == grownB, "shared length change");
    expect(world.undo->undoStackSizeForDiagnostics() == undoLen + 1, "length change is one undo step");
    world.undo->invokeUndoFromWindowShortcut();

    roll = world.rollOrNull();
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    clickRoll(*roll, pointFor(*roll, 8000, 50), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipA, 60), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipB, 72),
              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    const int notesBeforeDelete = (int)clipA->pattern.timelineNotes.size() + (int)clipB->pattern.timelineNotes.size();
    roll->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    expect(findUid(*clipA, uidA) == nullptr && findUid(*clipB, uidB) == nullptr, "delete removes both owners");
    expect((int)clipA->pattern.timelineNotes.size() + (int)clipB->pattern.timelineNotes.size()
               == notesBeforeDelete - 2,
           "delete leaves the unselected note");
    world.undo->invokeUndoFromWindowShortcut();
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    expect(findUid(*clipA, uidA) != nullptr && findUid(*clipB, uidB) != nullptr, "undo restores deleted notes");
    expect(world.dirty > dirtyBefore, "an edit marks the project dirty");
    juce::ignoreUnused(dirtyBefore);
}

void testClipboard()
{
    World world;
    const auto aId = world.addClip(world.ctlA, 0, 192000, 0, { makeNote(960, 60, 80), makeNote(1920, 64, 100) }, "A");
    const auto bId = world.addClip(world.ctlB, 48000, 144000, 48000, { makeNote(480, 72, 90) }, "B");
    world.addMember(world.idA, aId, &world.ctlA, &world.hostA);
    world.addMember(world.idB, bId, &world.ctlB, &world.hostB);
    world.show(world.ctlA, aId);
    auto* roll = world.rollOrNull();
    expect(roll != nullptr, "clipboard roll");
    if (roll == nullptr)
    {
        return;
    }
    publishLayout(*roll);
    auto* clipA = world.ctlA.getClipById(aId);
    auto* clipB = world.ctlB.getClipById(bId);
    const auto sourceTick = findPitch(*clipA, 60)->startTick;
    const auto sourceCountA = (int)clipA->pattern.timelineNotes.size();
    clickRoll(*roll, centreOf(world, *roll, *clipA, 60), juce::ModifierKeys::leftButtonModifier);
    roll->keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier, 'c'));
    clickRoll(*roll, centreOf(world, *roll, *clipB, 72), juce::ModifierKeys::leftButtonModifier);
    world.transport.requestSeek(240000);
    world.window->notifyExternalTransportSeek(240000);
    armDialog("Keep event boundaries");
    roll->keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 'v'));
    pump(30);
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    expect((int)clipA->pattern.timelineNotes.size() == sourceCountA, "copy from A leaves A unchanged");
    expect(findPitch(*clipA, 60)->startTick == sourceTick, "source note ticks stay");
    expect((int)clipB->pattern.timelineNotes.size() == 2, "paste into B adds the copied note");

    int selectedOnB = 0;
    for (const auto& note : clipB->pattern.timelineNotes)
    {
        if (note.editorSelected && note.midiNote == 60)
        {
            ++selectedOnB;
        }
    }
    expect(selectedOnB == 1, "pasted note lands on B");

    clickRoll(*roll, pointFor(*roll, 8000, 50), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipA, 64), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipB, 72),
              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    const auto* sourceA = findPitch(*clipA, 64);
    const auto* sourceB = findPitch(*clipB, 72);
    expect(sourceA != nullptr && sourceB != nullptr && sourceA->editorSelected && sourceB->editorSelected,
           "copy selection owns one note from A and one from B");
    expect(clipA->startSamples != clipB->startSamples, "the two sources start at different times");
    if (sourceA == nullptr || sourceB == nullptr)
    {
        return;
    }
    const auto uidA = sourceA->uid;
    const auto uidB = sourceB->uid;
    const auto tickA = sourceA->startTick;
    const auto tickB = sourceB->startTick;
    const auto gap = projectOf(*clipB, *sourceB) - projectOf(*clipA, *sourceA);
    const auto countA = (int)clipA->pattern.timelineNotes.size();
    const auto countB = (int)clipB->pattern.timelineNotes.size();
    roll->keyPressed(juce::KeyPress('c', juce::ModifierKeys::commandModifier, 'c'));
    clickRoll(*roll, centreOf(world, *roll, *clipA, 64), juce::ModifierKeys::leftButtonModifier);
    clipA = world.ctlA.getClipById(aId);
    expect(MidiEditorProductionFlowAccess::activeClipIs(*roll, *clipA), "paste target is event A");
    world.window->notifyExternalTransportSeek(300000);
    armDialog("Keep event boundaries");
    roll->keyPressed(juce::KeyPress('v', juce::ModifierKeys::commandModifier, 'v'));
    pump(30);
    clipA = world.ctlA.getClipById(aId);
    clipB = world.ctlB.getClipById(bId);
    expect((int)clipA->pattern.timelineNotes.size() == countA + 2, "both copies land on the target event");
    expect((int)clipB->pattern.timelineNotes.size() == countB, "the other source is unchanged");
    expect(findUid(*clipA, uidA) != nullptr && findUid(*clipA, uidA)->startTick == tickA
               && findUid(*clipB, uidB) != nullptr && findUid(*clipB, uidB)->startTick == tickB,
           "source notes keep their ticks");
    std::vector<std::int64_t> pasted;
    for (const auto& note : clipA->pattern.timelineNotes)
    {
        if (note.editorSelected)
        {
            pasted.push_back(projectOf(*clipA, note));
        }
    }
    expect(pasted.size() == 2, "paste selects the new notes");
    if (pasted.size() == 2)
    {
        const auto seen = std::llabs(pasted[1] - pasted[0]);
        expect(seen == std::llabs(gap), "paste keeps the project gap");
    }
}

void testExtend()
{
    World world;
    const auto aId = world.addClip(world.ctlA, 48000, 96000, 48000,
                                   { makeNote(960, 60, 80), makeNote(1920, 64, 70) }, "A");
    world.addMember(world.idA, aId, &world.ctlA, &world.hostA);
    world.show(world.ctlA, aId);
    auto* roll = world.rollOrNull();
    auto* clip = world.ctlA.getClipById(aId);
    expect(roll != nullptr && clip != nullptr, "extend roll");
    if (roll == nullptr || clip == nullptr)
    {
        return;
    }
    publishLayout(*roll);
    const auto anchor = clip->timelineAnchorSamples;
    const auto start = clip->startSamples;
    const auto length = clip->lengthSamples;
    const auto stayTick = findPitch(*clip, 60)->startTick;
    const auto moveTick = findPitch(*clip, 64)->startTick;
    const auto stayProject = projectOf(*clip, *findPitch(*clip, 60));
    const int undoBefore = world.undo->undoStackSizeForDiagnostics();
    const int dirtyBefore = world.dirty;
    const auto from = centreOf(world, *roll, *clip, 64);
    const float dx = (float)(80000.0 / roll->getViewportSamplesPerPixel());

    dragRoll(*roll, from, from.translated(-dx, 0.0f), "Cancel");
    clip = world.ctlA.getClipById(aId);
    expect(findPitch(*clip, 64)->startTick == moveTick && findPitch(*clip, 60)->startTick == stayTick,
           "cancel keeps notes");
    expect(clip->startSamples == start && clip->lengthSamples == length && clip->timelineAnchorSamples == anchor,
           "cancel keeps bounds");
    expect(world.undo->undoStackSizeForDiagnostics() == undoBefore && world.dirty == dirtyBefore,
           "cancel does not record undo or dirty");

    dragRoll(*roll, from, from.translated(-dx, 0.0f), "Keep event boundaries");
    clip = world.ctlA.getClipById(aId);
    expect(clip->startSamples == start && clip->lengthSamples == length, "keep boundaries leaves the window");
    expect(findPitch(*clip, 64)->startTick != moveTick, "keep boundaries still moves the note");
    expect(world.undo->undoStackSizeForDiagnostics() == undoBefore + 1, "keep boundaries is one undo step");
    world.undo->invokeUndoFromWindowShortcut();

    roll = world.rollOrNull();
    clip = world.ctlA.getClipById(aId);
    const auto again = centreOf(world, *roll, *clip, 64);
    const int undoExtend = world.undo->undoStackSizeForDiagnostics();
    dragRoll(*roll, again, again.translated(-dx, 0.0f), "Extend event(s)");
    clip = world.ctlA.getClipById(aId);
    expect(clip->timelineAnchorSamples == anchor, "left extend keeps the anchor");
    expect(clip->startSamples < start, "left extend moves the window start");
    expect(projectOf(*clip, *findPitch(*clip, 60)) == stayProject, "existing note project time stays");
    expect(world.undo->undoStackSizeForDiagnostics() == undoExtend + 1, "note edit plus extend is one undo step");
    world.undo->invokeUndoFromWindowShortcut();
    clip = world.ctlA.getClipById(aId);
    expect(clip->startSamples == start && findPitch(*clip, 64)->startTick == moveTick,
           "undo restores the note and the window together");
}

void testAudition()
{
    World world;
    MidiSink sinkA;
    MidiSink sinkB;
    expect(installTone(world.hostA) && installTone(world.hostB), "test instruments install");
    world.hostA.installMidiDeliveryCaptureSinkForTests(&sinkA);
    world.hostB.installMidiDeliveryCaptureSinkForTests(&sinkB);

    const auto aId = world.addClip(world.ctlA, 0, 192000, 0, { makeNote(960, 60, 80) }, "A");
    const auto bId = world.addClip(world.ctlB, 48000, 144000, 48000, { makeNote(480, 72, 100) }, "B");
    const auto midiId = world.addClip(world.ctlMidi, 0, 192000, 0, { makeNote(1440, 67, 90) }, "M");
    world.addMember(world.idA, aId, &world.ctlA, &world.hostA);
    world.addMember(world.idB, bId, &world.ctlB, &world.hostB);
    world.addMember(world.idMidi, midiId, &world.ctlMidi, nullptr);
    world.show(world.ctlA, aId);
    auto* roll = world.rollOrNull();
    expect(roll != nullptr, "audition roll");
    if (roll == nullptr)
    {
        return;
    }
    publishLayout(*roll);

    auto* clipA = world.ctlA.getClipById(aId);
    auto* clipB = world.ctlB.getClipById(bId);
    auto* clipM = world.ctlMidi.getClipById(midiId);
    clickRoll(*roll, centreOf(world, *roll, *clipA, 60), juce::ModifierKeys::leftButtonModifier);
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkA.countKind(true, 60) == 1 && sinkA.countKind(false, 60) == 0, "A receives note-on");
    expect(sinkB.count == 0, "B is silent while A is auditioned");
    pump(950);
    sinkA.clear();
    sinkB.clear();
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkA.countKind(false, 60) == 1, "A receives the matching note-off");
    expect(sinkB.count == 0 && !sinkA.hasAllNotesOff(), "note-off stays on A without All Notes Off");

    sinkA.clear();
    sinkB.clear();
    clickRoll(*roll, centreOf(world, *roll, *clipB, 72), juce::ModifierKeys::leftButtonModifier);
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkB.countKind(true, 72) == 1 && sinkA.count == 0, "B receives its own note-on");
    pump(950);
    sinkA.clear();
    sinkB.clear();
    drain(world.hostB);
    expect(sinkB.countKind(false, 72) == 1, "B receives its own note-off");

    sinkA.clear();
    sinkB.clear();
    clickRoll(*roll, centreOf(world, *roll, *clipM, 67), juce::ModifierKeys::leftButtonModifier);
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkA.count == 0 && sinkB.count == 0, "a missing destination sends nothing");

    clickRoll(*roll, centreOf(world, *roll, *clipB, 72), juce::ModifierKeys::leftButtonModifier);
    pump(950);
    drain(world.hostA);
    drain(world.hostB);
    sinkA.clear();
    sinkB.clear();
    const auto held = centreOf(world, *roll, *clipB, 72);
    roll->mouseDown(mouseEvent(*roll, held, held, juce::ModifierKeys::leftButtonModifier, false));
    drain(world.hostB);
    expect(sinkB.countKind(true, 72) == 1, "held note-on is on B");
    sinkA.clear();
    sinkB.clear();
    world.members.pop_back();
    world.members.pop_back();
    world.window->setOpenCohort(world.bindsNow());
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkB.countKind(false, 72) == 1 && sinkA.count == 0, "removal note-off stays on B");
    expect(!sinkB.hasAllNotesOff(), "removal does not send All Notes Off");
    roll = world.rollOrNull();
    roll->mouseUp(mouseEvent(*roll, held, held, juce::ModifierKeys(), false));
    sinkA.clear();
    sinkB.clear();
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkA.count == 0 && sinkB.count == 0, "after removal nothing is still held");

    world.members.push_back(Member { world.idB, bId, &world.ctlB, nullptr, 1 });
    world.window->setOpenCohort(world.bindsNow());
    roll = world.rollOrNull();
    publishLayout(*roll);
    clickRoll(*roll, centreOf(world, *roll, *world.ctlB.getClipById(bId), 72),
              juce::ModifierKeys::leftButtonModifier);
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkA.count == 0 && sinkB.count == 0, "a destination that disappeared is silent");

    world.members.back().host = &world.hostB;
    world.window->setOpenCohort(world.bindsNow());
    roll = world.rollOrNull();
    publishLayout(*roll);
    sinkA.clear();
    sinkB.clear();
    clickRoll(*roll, centreOf(world, *roll, *world.ctlB.getClipById(bId), 72),
              juce::ModifierKeys::leftButtonModifier);
    drain(world.hostB);
    expect(sinkB.countKind(true, 72) == 1, "audition works again when the destination returns");
    pump(950);
    drain(world.hostA);
    drain(world.hostB);

    world.window->unbindExternalPattern();
    world.window->resized();
    roll = world.rollOrNull();
    roll->setFollowPlayheadEnabled(false);
    const auto key = MidiEditorProductionFlowAccess::keyboard(*roll).getCentre().toFloat();
    sinkA.clear();
    sinkB.clear();
    clickRoll(*roll, key, juce::ModifierKeys::leftButtonModifier);
    drain(world.hostA);
    drain(world.hostB);
    expect(sinkA.count > 0 && sinkB.count == 0, "scratch mode uses the window host");

    world.hostA.installMidiDeliveryCaptureSinkForTests(nullptr);
    world.hostB.installMidiDeliveryCaptureSinkForTests(nullptr);
}

void testColourAndSnapshots()
{
    World world;
    const auto aId = world.addClip(
        world.ctlA, 24000, 72000, 24000,
        { makeNote(-480, 55, 100, 480), makeNote(480, 60, 20), makeNote(1440, 64, 120), makeNote(1920, 67, 40) },
        "A");
    const auto bId = world.addClip(
        world.ctlB, 48000, 96000, 48000,
        { makeNote(0, 72, 25), makeNote(960, 76, 127), makeNote(960, 67, 110), makeNote(2400, 79, 90) }, "B");
    world.addMember(world.idA, aId, &world.ctlA, &world.hostA);
    world.addMember(world.idB, bId, &world.ctlB, &world.hostB);
    world.show(world.ctlB, bId);
    auto* roll = world.rollOrNull();
    expect(roll != nullptr && MidiEditorProductionFlowAccess::grid(*roll).getWidth() >= 32, "colour roll laid out");
    if (roll == nullptr)
    {
        return;
    }
    publishLayout(*roll);
    auto* clipA = world.ctlA.getClipById(aId);
    auto* clipB = world.ctlB.getClipById(bId);

    clickRoll(*roll, centreOf(world, *roll, *clipB, 72), juce::ModifierKeys::leftButtonModifier);
    clickRoll(*roll, centreOf(world, *roll, *clipB, 76),
              juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::commandModifier);
    publishLayout(*roll);
    const auto start = roll->getViewportVisibleStartSamples();
    const auto spp = roll->getViewportSamplesPerPixel();
    const auto top = roll->topVisibleMidiPitch();
    auto boundsOf = [&](const InstrumentMidiClip& clip, const int pitch) -> std::optional<juce::Rectangle<float>> {
        const auto* note = findPitch(clip, pitch);
        if (note == nullptr)
        {
            return std::nullopt;
        }
        return MidiEditorProductionFlowAccess::noteBounds(*roll, (int)(note - clip.pattern.timelineNotes.data()));
    };
    juce::Image later = roll->createComponentSnapshot(roll->getLocalBounds(), false, 1.0f);
    writePng(later, "real-active-later.png");

    const auto lowRect = boundsOf(*clipB, 72);
    const auto highRect = boundsOf(*clipB, 76);
    expect(lowRect.has_value() && highRect.has_value(), "selected notes are on screen");
    if (!lowRect.has_value() || !highRect.has_value())
    {
        return;
    }
    const auto low = interiorFill(later, *lowRect);
    const auto high = interiorFill(later, *highRect);
    expect(low == high, "low and high velocity share a fill in one role");
    expect(closeTo(low, midi_editor_cohort::kSelectedNoteFill, 2), "selected fill is the fixed black");
    const auto borderLow = pixelAt(later, (int)lowRect->getCentreX(), (int)lowRect->getY());
    const auto borderHigh = pixelAt(later, (int)highRect->getCentreX(), (int)highRect->getY());
    expect(borderLow == borderHigh, "selected border ignores velocity");
    expect(closeTo(borderLow, midi_editor_cohort::kSelectedNoteBorder, 28), "selected border is the fixed red");

    const auto* otherNote = findPitch(*clipA, 60);
    const auto otherBounds = otherNote != nullptr
                                 ? MidiEditorProductionFlowAccess::visualBounds(
                                       *roll, *clipA, (int)(otherNote - clipA->pattern.timelineNotes.data()))
                                 : std::nullopt;
    expect(otherBounds.has_value(), "the other event's note has production bounds");
    if (otherBounds.has_value())
    {
        expect(closeTo(interiorFill(later, *otherBounds), midi_editor_cohort::kOtherNoteFill, 12),
               "another event stays muted red on the light background");
    }
    const auto* heldNote = findPitch(*clipB, 72);
    const int heldIndex = heldNote != nullptr ? (int)(heldNote - clipB->pattern.timelineNotes.data()) : -1;
    const auto beforeSwitch = heldIndex >= 0
                                  ? MidiEditorProductionFlowAccess::visualBounds(*roll, *clipB, heldIndex)
                                  : std::nullopt;

    clickRoll(*roll, centreOf(world, *roll, *clipA, 60), juce::ModifierKeys::leftButtonModifier);
    roll = world.rollOrNull();
    expect(roll->getViewportVisibleStartSamples() == start && roll->getViewportSamplesPerPixel() == spp
               && roll->topVisibleMidiPitch() == top,
           "active switch keeps zoom and scroll");
    publishLayout(*roll);
    juce::Image earlier = roll->createComponentSnapshot(roll->getLocalBounds(), false, 1.0f);
    writePng(earlier, "real-active-earlier.png");
    const auto activeRect = boundsOf(*world.ctlA.getClipById(aId), 64);
    expect(activeRect.has_value(), "active note is on screen");
    if (activeRect.has_value())
    {
        expect(closeTo(interiorFill(earlier, *activeRect), midi_editor_cohort::kActiveNoteFill, 8),
               "the active event uses DAL red");
    }
    const auto afterSwitch = heldIndex >= 0
                                 ? MidiEditorProductionFlowAccess::visualBounds(*roll, *clipB, heldIndex)
                                 : std::nullopt;
    expect(beforeSwitch.has_value() && afterSwitch.has_value()
               && std::abs(beforeSwitch->getX() - afterSwitch->getX()) < 1.0f
               && std::abs(beforeSwitch->getY() - afterSwitch->getY()) < 1.0f,
           "active switch does not move the note");

    const auto inside = pointFor(*roll, 60000, 90);
    const auto outside = pointFor(*roll, 12000, 90);
    const auto lum = [](const juce::Colour c) { return c.getRed() + c.getGreen() + c.getBlue(); };
    expect(lum(pixelAt(earlier, (int)inside.x, (int)inside.y)) < lum(pixelAt(earlier, (int)outside.x, (int)outside.y)),
           "the active audible span is darker than the outside");

    roll->setTimelineNotesDisplayComboId(1);
    publishLayout(*roll);
    juce::Image diamonds = roll->createComponentSnapshot(roll->getLocalBounds(), false, 1.0f);
    writePng(diamonds, "real-diamonds.png");
    const auto diamond = boundsOf(*world.ctlA.getClipById(aId), 64);
    expect(diamond.has_value() && closeTo(pixelAt(diamonds, (int)diamond->getCentreX(), (int)diamond->getCentreY()),
                                          midi_editor_cohort::kActiveNoteFill, 12),
           "drum diamonds use the same active red");
}
} // namespace

int runMidiEditorProductionFlows()
{
    testSnappedNoteEdits();
    testSharedEdits();
    testClipboard();
    testExtend();
    testAudition();
    testColourAndSnapshots();
    if (gFail != 0)
    {
        std::cerr << gFail << " production-flow failures\n";
        return 1;
    }
    std::cout << "MidiEditorProductionFlows ok\n";
    return 0;
}

int runMidiEditorCohortHelperChecks();

int main(int, char**)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const int helper = runMidiEditorCohortHelperChecks();
    const int flows = runMidiEditorProductionFlows();
    return helper != 0 || flows != 0 ? 1 : 0;
}
