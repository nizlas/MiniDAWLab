// Focused checks for per-placement event gain and linear fades.
// Production mix helpers, the playback callback, pump-mode read-ahead, offline mixdown,
// trim/split/move/paste/undo/save, and the lane's mouse handlers. No stability matrix.

#include "domain/EventAmplitude.h"
#include "domain/Session.h"
#include "domain/SessionHistory.h"
#include "engine/PlaybackEngine.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/ReadAheadRenderer.h"
#include "io/AudioFileLoader.h"
#include "io/AudioWaveformCache.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"
#include "ui/ClipWaveformView.h"
#include "ui/TimelineClipEventChrome.h"
#include "ui/TimelineViewportModel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/ReadAheadStartupConfig.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "ui/experimental/ExperimentalMidiCcAutomation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 128;
int gFailures = 0;

void expect(const bool ok, const char* what)
{
    if (!ok)
    {
        ++gFailures;
        std::printf("[FAIL] %s\n", what);
    }
}

void expectNear(const float got, const float want, const float tol, const char* what)
{
    if (!(std::fabs(got - want) <= tol))
    {
        ++gFailures;
        std::printf("[FAIL] %s got=%g want=%g\n", what, static_cast<double>(got), static_cast<double>(want));
    }
}

[[nodiscard]] std::shared_ptr<const AudioClip> makeDcClip(const int numSamples,
                                                          const float left,
                                                          const float right,
                                                          const juce::String& path = "dc")
{
    juce::AudioBuffer<float> buf(2, numSamples);
    for (int i = 0; i < numSamples; ++i)
    {
        buf.setSample(0, i, left);
        buf.setSample(1, i, right);
    }
    return std::make_shared<const AudioClip>(std::move(buf), kRate, path);
}

[[nodiscard]] const PlacedClip* findClip(const SessionSnapshot& snap, const PlacedClipId id)
{
    for (int ti = 0; ti < snap.getNumTracks(); ++ti)
    {
        const Track& tr = snap.getTrack(ti);
        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
        {
            if (tr.getPlacedClip(ci).getId() == id)
            {
                return &tr.getPlacedClip(ci);
            }
        }
    }
    return nullptr;
}

[[nodiscard]] PlacedClipId frontClipId(Session& session, const TrackId tid)
{
    const auto snap = session.loadSessionSnapshotForAudioThread();
    const int idx = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
    if (snap == nullptr || idx < 0 || snap->getTrack(idx).getNumPlacedClips() <= 0)
    {
        return kInvalidPlacedClipId;
    }
    return snap->getTrack(idx).getPlacedClip(0).getId();
}

void renderSum(const SessionSnapshot& snap,
               const std::int64_t start,
               const int n,
               float* left,
               float* right)
{
    juce::AudioBuffer<float> buf(2, n);
    buf.clear();
    float* ptrs[2] = { buf.getWritePointer(0), buf.getWritePointer(1) };
    playback_mix_helpers::renderAudioTracksClipSummingForSegment(
        snap, start, n, 0, 2, ptrs, nullptr, kInvalidTrackId, snap.getArrangementExtentSamples(), -1);
    for (int i = 0; i < n; ++i)
    {
        left[i] = ptrs[0][i];
        right[i] = ptrs[1][i];
    }
}

void renderStrip(const SessionSnapshot& snap,
                 const int trackIndex,
                 const std::int64_t start,
                 const int n,
                 float* left,
                 float* right)
{
    std::vector<float> l(static_cast<size_t>(n), 0.0f);
    std::vector<float> r(static_cast<size_t>(n), 0.0f);
    playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
        snap, start, n, 0, l.data(), r.data(), nullptr, kInvalidTrackId,
        snap.getArrangementExtentSamples(), trackIndex);
    for (int i = 0; i < n; ++i)
    {
        left[i] = l[static_cast<size_t>(i)];
        right[i] = r[static_cast<size_t>(i)];
    }
}

void testMath()
{
    expect(event_amplitude::linearGain(0.0f) == 1.0f, "0 dB is exactly 1");
    expectNear(event_amplitude::envelopeAt(700, 4000, 1000, 1000), 0.7f, 1.0e-6f, "70% fade-in is 0.7");
    expectNear(event_amplitude::envelopeAt(0, 4000, 1000, 0), 0.0f, 1.0e-6f, "fade-in starts at 0");
    expectNear(event_amplitude::envelopeAt(1000, 4000, 1000, 1000), 1.0f, 1.0e-6f, "sample at fade-in end is 100%");
    expectNear(event_amplitude::envelopeAt(2000, 4000, 1000, 1000), 1.0f, 1.0e-6f, "sustain is 100%");
    expectNear(event_amplitude::envelopeAt(3000, 4000, 1000, 1000), 1.0f, 1.0e-6f, "fade-out starts at 100%");
    expectNear(event_amplitude::envelopeAt(3500, 4000, 1000, 1000), 0.5f, 1.0e-6f, "mid fade-out is 0.5");
    expect(event_amplitude::sanitizeGainDb(std::numeric_limits<float>::quiet_NaN()) == 0.0f, "NaN gain is 0 dB");
    expect(event_amplitude::sanitizeGainDb(std::numeric_limits<float>::infinity()) == 0.0f, "Inf gain is 0 dB");
    expect(event_amplitude::fitToLength(100.0f, -3, 4, 10).gainDb == 24.0f, "gain clamps to +24");
    expect(event_amplitude::fitToLength(0.0f, -8, 0, 10).fadeInSamples == 0, "negative fade is 0");
    const auto scaled = event_amplitude::fitToLength(0.0f, 1000, 1000, 1500);
    expect(scaled.fadeInSamples == 750 && scaled.fadeOutSamples == 750, "trim scales both fades");
    const event_amplitude::Params src { -3.0f, 2000, 800 };
    const auto left = event_amplitude::splitLeft(src, 1000);
    const auto right = event_amplitude::splitRight(src, 3000);
    expect(left.fadeInSamples == 1000 && left.fadeOutSamples == 0 && left.gainDb == -3.0f, "split keeps fade-in on the left");
    expect(right.fadeInSamples == 0 && right.fadeOutSamples == 800 && right.gainDb == -3.0f, "split keeps fade-out on the right");
    const auto rightShort = event_amplitude::splitRight(event_amplitude::Params { 1.0f, 0, 2000 }, 1000);
    expect(rightShort.fadeInSamples == 0 && rightShort.fadeOutSamples == 1000, "fade-out is limited to the right part");
}

void testSessionLifecycle()
{
    Session session;
    Transport transport;
    session.addTrack();
    const TrackId tid = session.getActiveTrackId();
    constexpr int kLen = 4000;
    const auto material = makeDcClip(kLen, 1.0f, 0.5f);
    expect(session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, tid).wasOk(), "add clip");
    session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate) * 10);
    const PlacedClipId id = frontClipId(session, tid);
    const auto snap0 = session.loadSessionSnapshotForAudioThread();
    const PlacedClip* clip0 = findClip(*snap0, id);
    expect(clip0 != nullptr && clip0->getEventGainDb() == 0.0f && clip0->getFadeInSamples() == 0
               && clip0->getFadeOutSamples() == 0,
           "defaults are 0 dB and no fades");

    session.setClipEventAmplitude(id, -3.0f, 1000, 500);
    session.moveClip(id, 250);
    const auto moved = findClip(*session.loadSessionSnapshotForAudioThread(), id);
    expect(moved != nullptr && moved->getStartSample() == 250 && moved->getEventGainDb() == -3.0f
               && moved->getFadeInSamples() == 1000 && moved->getFadeOutSamples() == 500,
           "move keeps gain and fades");

    Session trimSession;
    trimSession.addTrack();
    const TrackId trimTid = trimSession.getActiveTrackId();
    expect(trimSession.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, trimTid).wasOk(), "trim clip");
    const PlacedClipId trimId = frontClipId(trimSession, trimTid);
    trimSession.setClipEventAmplitude(trimId, -3.0f, 1000, 1000);
    trimSession.setClipRightEdgeVisibleLength(trimId, 1500);
    const auto trimmed = findClip(*trimSession.loadSessionSnapshotForAudioThread(), trimId);
    expect(trimmed != nullptr && trimmed->getFadeInSamples() == 750 && trimmed->getFadeOutSamples() == 750,
           "trim scales fades that no longer fit");

    Session splitSession;
    splitSession.addTrack();
    const TrackId splitTid = splitSession.getActiveTrackId();
    expect(splitSession.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, splitTid).wasOk(), "split clip");
    const PlacedClipId splitId = frontClipId(splitSession, splitTid);
    splitSession.setClipEventAmplitude(splitId, -3.0f, 2000, 800);
    const auto parts = splitSession.splitClip(splitId, 1000);
    expect(parts.has_value(), "split inside the fade-in");
    if (parts.has_value())
    {
        const auto* left = findClip(*splitSession.loadSessionSnapshotForAudioThread(), parts->first);
        const auto* right = findClip(*splitSession.loadSessionSnapshotForAudioThread(), parts->second);
        expect(left != nullptr && left->getFadeInSamples() == 1000 && left->getFadeOutSamples() == 0
                   && left->getEventGainDb() == -3.0f,
               "left part keeps a full fade-in and no fade-out");
        expect(right != nullptr && right->getFadeInSamples() == 0 && right->getFadeOutSamples() == 800
                   && right->getEventGainDb() == -3.0f,
               "right part starts full and keeps the fade-out");
    }

    Session fadeOutSession;
    fadeOutSession.addTrack();
    const TrackId tid2 = fadeOutSession.getActiveTrackId();
    expect(fadeOutSession.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, tid2).wasOk(), "add fade-out clip");
    const PlacedClipId id2 = frontClipId(fadeOutSession, tid2);
    fadeOutSession.setClipEventAmplitude(id2, 2.0f, 100, 2000);
    const auto cut = fadeOutSession.splitClip(id2, 3000);
    expect(cut.has_value(), "split inside the fade-out");
    if (cut.has_value())
    {
        const auto* left = findClip(*fadeOutSession.loadSessionSnapshotForAudioThread(), cut->first);
        const auto* right = findClip(*fadeOutSession.loadSessionSnapshotForAudioThread(), cut->second);
        expect(left != nullptr && left->getFadeOutSamples() == 0 && left->getFadeInSamples() == 100, "left loses the fade-out");
        expect(right != nullptr && right->getFadeInSamples() == 0 && right->getFadeOutSamples() == 1000,
               "right fade-out is limited to the right part");
    }

    const auto dup = session.duplicateTrack(tid);
    expect(dup.has_value(), "duplicate track");
    if (dup.has_value())
    {
        const auto snap = session.loadSessionSnapshotForAudioThread();
        const int srcIx = snap->findTrackIndexById(tid);
        const int dstIx = snap->findTrackIndexById(*dup);
        expect(srcIx >= 0 && dstIx >= 0 && snap->getTrack(dstIx).getNumPlacedClips() > 0, "duplicate has clips");
        if (srcIx >= 0 && dstIx >= 0 && snap->getTrack(dstIx).getNumPlacedClips() > 0)
        {
            const PlacedClip& copy = snap->getTrack(dstIx).getPlacedClip(0);
            expect(copy.getMaterial().get() == material.get(), "duplicate shares material");
            session.setClipEventAmplitude(copy.getId(), 4.0f, 10, 20);
            const auto* srcAfter = findClip(*session.loadSessionSnapshotForAudioThread(), 
                                            snap->getTrack(srcIx).getPlacedClip(0).getId());
            const auto* copyAfter = findClip(*session.loadSessionSnapshotForAudioThread(), copy.getId());
            expect(copyAfter != nullptr && copyAfter->getEventGainDb() == 4.0f, "copy can be edited");
            expect(srcAfter != nullptr && srcAfter->getEventGainDb() != 4.0f, "editing the copy leaves the source");
        }
    }

    SessionHistory history;
    Session undoSession;
    undoSession.addTrack();
    const TrackId ut = undoSession.getActiveTrackId();
    expect(undoSession.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, ut).wasOk(), "undo clip");
    const PlacedClipId uid = frontClipId(undoSession, ut);
    const auto before = undoSession.loadSessionSnapshotForAudioThread();
    undoSession.setClipEventAmplitude(uid, -12.0f, 40, 80);
    const auto after = undoSession.loadSessionSnapshotForAudioThread();
    history.record("Event envelope", before, after);
    expect(history.undoStackSize() == 1, "one undo step for the edit");
    const auto undone = history.popUndo();
    expect(undone.has_value(), "pop undo");
    if (undone.has_value())
    {
        undoSession.restoreSessionSnapshotForUndo(undone->timelineSnapshot);
    }
    const auto* restored = findClip(*undoSession.loadSessionSnapshotForAudioThread(), uid);
    expect(restored != nullptr && restored->getEventGainDb() == 0.0f && restored->getFadeInSamples() == 0,
           "undo restores the previous envelope");
    const auto redone = history.popRedo();
    if (redone.has_value())
    {
        undoSession.restoreSessionSnapshotForUndo(redone->timelineSnapshot);
    }
    const auto* again = findClip(*undoSession.loadSessionSnapshotForAudioThread(), uid);
    expect(again != nullptr && again->getEventGainDb() == -12.0f && again->getFadeInSamples() == 40,
           "redo restores the envelope");

    // Paste applies the copied envelope onto a new front clip that shares the material.
    const float pastedGain = again != nullptr ? again->getEventGainDb() : 0.0f;
    const std::int64_t pastedIn = again != nullptr ? again->getFadeInSamples() : 0;
    const std::int64_t pastedOut = again != nullptr ? again->getFadeOutSamples() : 0;
    expect(undoSession.addPlacedClipFromExistingMaterial(material, 8000, 0, kLen, ut).wasOk(), "paste clip");
    const PlacedClipId pasted = frontClipId(undoSession, ut);
    undoSession.setClipEventAmplitude(pasted, pastedGain, pastedIn, pastedOut);
    undoSession.setClipEventAmplitude(pasted, 6.0f, 1, 2);
    const auto* original = findClip(*undoSession.loadSessionSnapshotForAudioThread(), uid);
    const auto* pastedClip = findClip(*undoSession.loadSessionSnapshotForAudioThread(), pasted);
    expect(pastedClip != nullptr && pastedClip->getMaterial().get() == material.get(), "paste shares material");
    expect(original != nullptr && original->getEventGainDb() == -12.0f, "editing the paste leaves the original");
    expect(pastedClip != nullptr && pastedClip->getEventGainDb() == 6.0f, "paste has its own gain");
}

void testProjectFile()
{
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("dal-event-envelope-tests");
    root.deleteRecursively();
    root.getChildFile("Audio").createDirectory();
    const juce::File wav = root.getChildFile("Audio").getChildFile("tone.wav");
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
        expect(stream != nullptr, "wav stream");
        if (stream == nullptr)
        {
            return;
        }
        std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(), kRate, 2, 16, {}, 0));
        expect(writer != nullptr, "wav writer");
        if (writer == nullptr)
        {
            return;
        }
        stream.release();
        juce::AudioBuffer<float> buf(2, 2000);
        buf.clear();
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            buf.setSample(0, i, 0.25f);
            buf.setSample(1, i, -0.25f);
        }
        expect(writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples()), "wav written");
    }

    Session session;
    Transport transport;
    session.addTrack();
    const TrackId tid = session.getActiveTrackId();
    std::unique_ptr<AudioClip> loaded;
    expect(AudioFileLoader::loadFromFile(wav, kRate, loaded).wasOk(), "load wav");
    if (loaded == nullptr)
    {
        return;
    }
    const auto material = std::shared_ptr<const AudioClip>(std::move(loaded));
    expect(session.addPlacedClipFromExistingMaterial(material, 0, 0, material->getNumSamples(), tid).wasOk(),
           "place wav");
    const PlacedClipId id = frontClipId(session, tid);
    session.setClipEventAmplitude(id, -6.0f, 120, 240);
    session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate));
    const juce::File proj = root.getChildFile("env.dalproj");
    expect(session.saveProjectToFile(transport, proj, kRate).wasOk(), "save");

    Session restored;
    Transport transport2;
    juce::StringArray skipped;
    juce::String note;
    expect(restored.loadProjectFromFile(transport2, proj, kRate, skipped, note).wasOk(), "load");
    const auto snap = restored.loadSessionSnapshotForAudioThread();
    expect(snap != nullptr && snap->getNumTracks() > 0, "loaded tracks");
    bool found = false;
    if (snap != nullptr)
    {
        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            const Track& tr = snap->getTrack(ti);
            for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
            {
                const PlacedClip& p = tr.getPlacedClip(ci);
                if (p.getFadeInSamples() == 120 && p.getFadeOutSamples() == 240)
                {
                    found = true;
                    expectNear(p.getEventGainDb(), -6.0f, 1.0e-4f, "loaded gain");
                }
            }
        }
    }
    expect(found, "loaded fades");

    juce::String json = proj.loadFileAsString();
    json = json.replace("\"version\": 31", "\"version\": 28");
    json = json.replace("\"eventGainDb\": -6.0", "\"unusedGain\": 0");
    json = json.replace("\"fadeInSamples\": 120", "\"unusedIn\": 0");
    json = json.replace("\"fadeOutSamples\": 240", "\"unusedOut\": 0");
    const juce::File oldFile = root.getChildFile("v28.dalproj");
    expect(oldFile.replaceWithText(json), "write v28");
    Session oldSession;
    Transport oldTransport;
    juce::StringArray oldSkipped;
    juce::String oldNote;
    expect(oldSession.loadProjectFromFile(oldTransport, oldFile, kRate, oldSkipped, oldNote).wasOk(), "v28 loads");
    const auto oldSnap = oldSession.loadSessionSnapshotForAudioThread();
    if (oldSnap != nullptr)
    {
        for (int ti = 0; ti < oldSnap->getNumTracks(); ++ti)
        {
            for (int ci = 0; ci < oldSnap->getTrack(ti).getNumPlacedClips(); ++ci)
            {
                const PlacedClip& p = oldSnap->getTrack(ti).getPlacedClip(ci);
                expect(p.getEventGainDb() == 0.0f && p.getFadeInSamples() == 0 && p.getFadeOutSamples() == 0,
                       "v28 clip has no envelope");
            }
        }
    }

    for (const int bad : { 29, 30 })
    {
        const juce::String badJson = proj.loadFileAsString().replace("\"version\": 31",
                                                                     "\"version\": " + juce::String(bad));
        const juce::File badFile = root.getChildFile("v" + juce::String(bad) + ".dalproj");
        expect(badFile.replaceWithText(badJson), "write reserved version");
        Session badSession;
        Transport badTransport;
        juce::StringArray badSkipped;
        juce::String badNote;
        expect(badSession.loadProjectFromFile(badTransport, badFile, kRate, badSkipped, badNote).failed(),
               bad == 29 ? "version 29 is rejected" : "version 30 is rejected");
    }

    juce::String dirty = proj.loadFileAsString();
    dirty = dirty.replace("\"eventGainDb\": -6.0", "\"eventGainDb\": 1e20");
    dirty = dirty.replace("\"fadeInSamples\": 120", "\"fadeInSamples\": -4");
    const juce::File dirtyFile = root.getChildFile("dirty.dalproj");
    expect(dirtyFile.replaceWithText(dirty), "write dirty envelope");
    Session dirtySession;
    Transport dirtyTransport;
    juce::StringArray dirtySkipped;
    juce::String dirtyNote;
    expect(dirtySession.loadProjectFromFile(dirtyTransport, dirtyFile, kRate, dirtySkipped, dirtyNote).wasOk(),
           "dirty envelope loads");
    const auto dirtySnap = dirtySession.loadSessionSnapshotForAudioThread();
    if (dirtySnap != nullptr)
    {
        for (int ti = 0; ti < dirtySnap->getNumTracks(); ++ti)
        {
            for (int ci = 0; ci < dirtySnap->getTrack(ti).getNumPlacedClips(); ++ci)
            {
                const PlacedClip& p = dirtySnap->getTrack(ti).getPlacedClip(ci);
                expect(p.getEventGainDb() <= 24.0f && p.getFadeInSamples() == 0, "dirty gain clamped and bad fade cleared");
            }
        }
    }
    root.deleteRecursively();
}

class LevelProbe final : public juce::AudioPluginInstance
{
public:
    LevelProbe()
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }
    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "LevelProbe";
        d.pluginFormatName = "Test";
    }
    const juce::String getName() const override { return "LevelProbe"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        last_.setSize(2, buffer.getNumSamples(), false, false, true);
        for (int ch = 0; ch < 2; ++ch)
        {
            last_.copyFrom(ch, 0, buffer, ch, 0, buffer.getNumSamples());
        }
        ++calls_;
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
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    juce::AudioBuffer<float> last_;
    int calls_ = 0;
};

/// Pure delay. Output is the sample from `delaySamples` ago; the written sample is
/// input + feedback * that output. No dry path, so a tail after the event is unambiguous.
class DelayTailProbe final : public juce::AudioPluginInstance
{
public:
    static constexpr int kDelay = 64;

    explicit DelayTailProbe(const float feedback, const int delaySamples = kDelay)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true))
        , feedback_(feedback)
        , delay_(juce::jlimit(1, 4096, delaySamples))
        , line_(2, std::vector<float>(static_cast<size_t>(delay_), 0.0f))
    {
    }
    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "DelayTailProbe";
        d.pluginFormatName = "Test";
    }
    const juce::String getName() const override { return "DelayTailProbe"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        const int already = inProcess_.fetch_add(1, std::memory_order_acq_rel);
        if (already != 0)
        {
            overlaps_.fetch_add(1, std::memory_order_relaxed);
        }
        if (hold_.load(std::memory_order_acquire) != 0)
        {
            held_.store(1, std::memory_order_release);
            const auto cap = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (hold_.load(std::memory_order_acquire) != 0 && std::chrono::steady_clock::now() < cap)
            {
                std::this_thread::yield();
            }
            held_.store(0, std::memory_order_release);
        }
        const int n = buffer.getNumSamples();
        const int chs = juce::jmin(2, buffer.getNumChannels());
        for (int i = 0; i < n; ++i)
        {
            for (int ch = 0; ch < chs; ++ch)
            {
                float* const d = buffer.getWritePointer(ch);
                const float in = d[i];
                const float y = line_[static_cast<size_t>(ch)][static_cast<size_t>(pos_)];
                line_[static_cast<size_t>(ch)][static_cast<size_t>(pos_)] = in + feedback_ * y;
                d[i] = y;
            }
            pos_ = (pos_ + 1) % delay_;
        }
        ++calls_;
        inProcess_.fetch_sub(1, std::memory_order_acq_rel);
    }
    void setHoldInProcessBlock(const bool hold) noexcept
    {
        hold_.store(hold ? 1 : 0, std::memory_order_release);
    }
    [[nodiscard]] bool isHeldInProcessBlock() const noexcept
    {
        return held_.load(std::memory_order_acquire) != 0;
    }
    [[nodiscard]] int overlaps() const noexcept { return overlaps_.load(std::memory_order_acquire); }
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
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    int calls_ = 0;

private:
    float feedback_ = 0.0f;
    int delay_ = kDelay;
    int pos_ = 0;
    std::vector<std::vector<float>> line_;
    std::atomic<int> hold_{ 0 };
    std::atomic<int> held_{ 0 };
    std::atomic<int> inProcess_{ 0 };
    std::atomic<int> overlaps_{ 0 };
};

class StubDevice final : public juce::AudioIODevice
{
public:
    StubDevice() : juce::AudioIODevice("StubDevice", "Stub") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    bool isPlaying() override { return playing_; }
    void start(juce::AudioIODeviceCallback* c) override
    {
        callback_ = c;
        playing_ = true;
    }
    void stop() override { playing_ = false; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override
    {
        juce::BigInteger b;
        b.setBit(0);
        b.setBit(1);
        return b;
    }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    juce::AudioIODeviceCallback* callback_ = nullptr;
    bool playing_ = false;
};

struct Engine
{
    Session session;
    Transport transport;
    PluginInsertHost pluginHost;
    PlaybackEngine playback;
    StubDevice device;
    explicit Engine(const int readAheadDepth)
        : playback(transport, session, nullptr, nullptr, &pluginHost)
    {
        if (readAheadDepth > 0)
        {
            playback.enableExperimentalReadAheadForTests(readAheadDepth);
        }
    }
    void start()
    {
        session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate) * 30);
        playback.rebuildRoutingPlanFromSession();
        playback.audioDeviceAboutToStart(&device);
    }
    void renderBlock(float* left, float* right)
    {
        juce::AudioBuffer<float> blk(2, kBlock);
        blk.clear();
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        juce::AudioIODeviceCallbackContext ctx;
        playback.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
        for (int i = 0; i < kBlock; ++i)
        {
            left[i] = ptrs[0][i];
            right[i] = ptrs[1][i];
        }
    }
    void pump()
    {
        if (readahead::ReadAheadRenderer* ra = playback.experimentalReadAhead())
        {
            for (int i = 0; i < 1024; ++i)
            {
                if (ra->testPumpWorkerOnce() == 0)
                {
                    break;
                }
            }
        }
    }
};

void testAudioPaths()
{
    constexpr int kLen = 4096;
    const auto material = makeDcClip(kLen, 1.0f, 0.5f);
    Session session;
    session.addTrack();
    const TrackId tid = session.getActiveTrackId();
    expect(session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, tid).wasOk(), "audio clip");
    session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate));
    const PlacedClipId id = frontClipId(session, tid);
    const auto snap = session.loadSessionSnapshotForAudioThread();
    const int trackIndex = snap->findTrackIndexById(tid);
    std::vector<float> aL(kLen), aR(kLen), bL(kLen), bR(kLen);
    renderSum(*snap, 0, kLen, aL.data(), aR.data());
    renderStrip(*snap, trackIndex, 0, kLen, bL.data(), bR.data());
    bool same = true;
    for (int i = 0; i < kLen; ++i)
    {
        same = same && aL[static_cast<size_t>(i)] == bL[static_cast<size_t>(i)]
               && aR[static_cast<size_t>(i)] == bR[static_cast<size_t>(i)];
    }
    expect(same, "default summing and post-strip match");
    expect(aL[100] != 0.0f && aR[100] != 0.0f, "default clip is audible");
    const float panL = aL[100];
    const float panR = aR[100] / 0.5f;

    session.setClipEventAmplitude(id, -6.020599f, 1000, 1000);
    const auto shaped = session.loadSessionSnapshotForAudioThread();
    std::vector<float> sL(kLen), sR(kLen), pL(kLen), pR(kLen);
    renderSum(*shaped, 0, kLen, sL.data(), sR.data());
    renderStrip(*shaped, trackIndex, 0, kLen, pL.data(), pR.data());
    bool shapedSame = true;
    for (int i = 0; i < 64; ++i)
    {
        shapedSame = shapedSame && std::fabs(sL[static_cast<size_t>(i)] - pL[static_cast<size_t>(i)]) < 1.0e-6f;
    }
    expect(shapedSame, "fade matches on summing and post-strip");
    const float g = event_amplitude::linearGain(-6.020599f);
    expectNear(sL[0], 0.0f, 1.0e-5f, "fade-in start");
    expectNear(sL[700], panL * g * 0.7f, 1.0e-4f, "fade-in at 70%");
    expectNear(sL[2000], panL * g, 1.0e-4f, "sustain level");
    expectNear(sL[3500], panL * g * event_amplitude::envelopeAt(3500, kLen, 1000, 1000), 1.0e-4f,
               "fade-out middle");
    expectNear(sR[700] / 0.5f, panR * g * 0.7f, 1.0e-4f, "same factor on the right channel");

    std::vector<float> blockL(128), blockR(128), wholeL(128), wholeR(128);
    renderSum(*shaped, 0, 64, blockL.data(), blockR.data());
    renderSum(*shaped, 64, 64, blockL.data() + 64, blockR.data() + 64);
    renderSum(*shaped, 0, 128, wholeL.data(), wholeR.data());
    bool blocks = true;
    for (int i = 0; i < 128; ++i)
    {
        blocks = blocks && std::fabs(blockL[static_cast<size_t>(i)] - wholeL[static_cast<size_t>(i)]) < 1.0e-6f;
    }
    expect(blocks, "envelope is continuous across a block boundary");

    // Two placements of one file, then a newer clip covering the first.
    Session share;
    share.addTrack();
    const TrackId st = share.getActiveTrackId();
    expect(share.addPlacedClipFromExistingMaterial(material, 0, 0, 512, st).wasOk(), "shared a");
    const PlacedClipId older = frontClipId(share, st);
    share.setClipEventAmplitude(older, 6.0f, 0, 0);
    expect(share.addPlacedClipFromExistingMaterial(material, 0, 0, 512, st).wasOk(), "shared b");
    const PlacedClipId newer = frontClipId(share, st);
    share.setClipEventAmplitude(newer, -6.020599f, 0, 0);
    share.setArrangementExtentSamples(static_cast<std::int64_t>(kRate));
    const auto sharedSnap = share.loadSessionSnapshotForAudioThread();
    const auto* olderClip = findClip(*sharedSnap, older);
    const auto* newerClip = findClip(*sharedSnap, newer);
    expect(olderClip != nullptr && newerClip != nullptr
               && olderClip->getMaterial().get() == newerClip->getMaterial().get(),
           "two events share one file");
    std::vector<float> ovL(128), ovR(128);
    renderSum(*sharedSnap, 0, 128, ovL.data(), ovR.data());
    expectNear(ovL[10], panL * g, 1.0e-3f, "newest overlapping clip wins and is not summed");

    Engine direct(0);
    direct.session.addTrack();
    const TrackId et = direct.session.getActiveTrackId();
    expect(direct.session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, et).wasOk(), "engine clip");
    const PlacedClipId eid = frontClipId(direct.session, et);
    direct.session.setClipEventAmplitude(eid, -6.020599f, 1000, 1000);
    expect(direct.session.setTrackPreGainDb(et, 6.020599f), "set pre-gain");
    auto probeOwned = std::make_unique<LevelProbe>();
    LevelProbe* probe = probeOwned.get();
    expect(direct.pluginHost.installInsertInstanceForTests(et, InsertStage::Pre, std::move(probeOwned)),
           "install probe");
    direct.start();
    direct.transport.requestSeek(600);
    direct.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> outL(kBlock), outR(kBlock);
    direct.renderBlock(outL.data(), outR.data());
    expect(probe->calls_ > 0, "probe ran");
    if (probe->calls_ > 0 && probe->last_.getNumSamples() > 10)
    {
        const float env = event_amplitude::envelopeAt(610, kLen, 1000, 1000);
        const float pre = juce::Decibels::decibelsToGain(6.020599f);
        expectNear(probe->last_.getSample(0, 10), 1.0f * g * env * pre, 2.0e-3f,
                   "event gain and fade reach the first insert once, before pre-gain");
        expectNear(probe->last_.getSample(1, 10), 0.5f * g * env * pre, 2.0e-3f,
                   "stereo event factor before the insert");
    }

    Engine offline(0);
    offline.session.addTrack();
    const TrackId ot = offline.session.getActiveTrackId();
    expect(offline.session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, ot).wasOk(), "offline clip");
    const PlacedClipId oid = frontClipId(offline.session, ot);
    offline.session.setClipEventAmplitude(oid, -6.020599f, 1000, 1000);
    offline.start();
    juce::AudioBuffer<float> off(2, 256);
    off.clear();
    float* offPtrs[2] = { off.getWritePointer(0), off.getWritePointer(1) };
    const auto offSnap = offline.session.loadSessionSnapshotForAudioThread();
    offline.playback.renderOfflineMixdownBlock(*offSnap, nullptr, 600, 256, offPtrs, false);
    std::vector<float> refL(256), refR(256);
    renderStrip(*offSnap, offSnap->findTrackIndexById(ot), 600, 256, refL.data(), refR.data());
    bool offMatch = true;
    for (int i = 0; i < 256; ++i)
    {
        offMatch = offMatch && std::fabs(offPtrs[0][i] - refL[static_cast<size_t>(i)]) < 1.0e-4f
                   && std::fabs(offPtrs[1][i] - refR[static_cast<size_t>(i)]) < 1.0e-4f;
    }
    expect(offMatch, "offline mixdown uses the same envelope");

    Engine ahead(2);
    ahead.session.addTrack();
    const TrackId at = ahead.session.getActiveTrackId();
    expect(ahead.session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, at).wasOk(), "read-ahead clip");
    const PlacedClipId aid = frontClipId(ahead.session, at);
    ahead.start();
    ahead.pump();
    ahead.transport.requestSeek(2000);
    ahead.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> raL(kBlock), raR(kBlock);
    // The adopting block is rendered live and asks the worker for the following segments.
    ahead.renderBlock(raL.data(), raR.data());
    expectNear(raL[0], panL, 2.0e-3f, "adopting block plays the current unity envelope");
    ahead.pump();
    ahead.renderBlock(raL.data(), raR.data());
    expectNear(raL[0], panL, 2.0e-3f, "read-ahead consumes the segment produced before the edit");
    ahead.session.setClipEventAmplitude(aid, -6.020599f, 0, 0);
    ahead.renderBlock(raL.data(), raR.data());
    expectNear(raL[0], panL, 2.0e-3f, "an already queued read-ahead segment keeps the old envelope");
    ahead.pump();
    ahead.renderBlock(raL.data(), raR.data());
    expectNear(raL[0], panL * g, 2.0e-3f, "the next produced read-ahead segment hears the new gain");
    ahead.renderBlock(raL.data(), raR.data());
    expectNear(raL[0], panL * g, 2.0e-3f, "read-ahead does not stay on the old envelope");

    Engine cyc(0);
    cyc.session.addTrack();
    const TrackId ct = cyc.session.getActiveTrackId();
    expect(cyc.session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, ct).wasOk(), "cycle clip");
    const PlacedClipId cid = frontClipId(cyc.session, ct);
    cyc.session.setClipEventAmplitude(cid, 0.0f, 1000, 0);
    cyc.session.setLeftLocatorAtSample(100);
    cyc.session.setRightLocatorAtSample(500);
    cyc.start();
    cyc.transport.requestCycleEnabled(true);
    cyc.transport.requestSeek(484);
    cyc.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> cL(kBlock), cR(kBlock);
    cyc.renderBlock(cL.data(), cR.data());
    bool cycleOk = true;
    for (int i = 0; i < 16; ++i)
    {
        const float env = event_amplitude::envelopeAt(484 + i, kLen, 1000, 0);
        cycleOk = cycleOk && std::fabs(cL[static_cast<size_t>(i)] - panL * env) < 2.0e-3f;
    }
    for (int i = 0; i < 48; ++i)
    {
        const float env = event_amplitude::envelopeAt(100 + i, kLen, 1000, 0);
        cycleOk = cycleOk && std::fabs(cL[static_cast<size_t>(16 + i)] - panL * env) < 2.0e-3f;
    }
    expect(cycleOk, "cycle wrap keeps the envelope of each segment");
}

juce::MouseEvent makeMouse(juce::Component& c, const juce::Point<float> p, const juce::ModifierKeys mods)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                            p,
                            mods,
                            0.0f,
                            0.0f,
                            0.0f,
                            0.0f,
                            0.0f,
                            &c,
                            &c,
                            now,
                            p,
                            now,
                            1,
                            false);
}

void testLaneHandles()
{
    Session session;
    Transport transport;
    TimelineViewportModel viewport;
    AudioWaveformCache cache;
    session.addTrack();
    const TrackId tid = session.getActiveTrackId();
    constexpr int kLen = 400;
    const auto material = makeDcClip(kLen, 1.0f, 1.0f);
    expect(session.addPlacedClipFromExistingMaterial(material, 0, 0, kLen, tid).wasOk(), "lane clip");
    session.setArrangementExtentSamples(8000);
    const PlacedClipId id = frontClipId(session, tid);
    viewport.setSamplesPerPixelIfUnset(1.0);

    int records = 0;
    ClipWaveformLaneHost host;
    host.recordAppliedClipEdit = [&](const juce::String&, auto, auto) { ++records; };
    ClipWaveformView view(session, transport, tid, viewport, cache, host);
    view.setSize(800, 56);

    auto down = [&](juce::Point<float> p, juce::ModifierKeys mods = {}) {
        view.mouseDown(makeMouse(view, p, mods));
    };
    auto drag = [&](juce::Point<float> p, juce::ModifierKeys mods = {}) {
        view.mouseDrag(makeMouse(view, p, mods));
    };
    auto up = [&](juce::Point<float> p) { view.mouseUp(makeMouse(view, p, {})); };

    down({ 3.0f, 50.0f });
    expect(view.isClipTrimGestureInProgress() && !view.isClipEnvelopeGestureInProgress(),
           "bottom corner still trims");
    up({ 3.0f, 50.0f });

    const auto startBefore = findClip(*session.loadSessionSnapshotForAudioThread(), id)->getStartSample();
    down({ 200.0f, 30.0f });
    expect(view.isClipMoveGestureInProgress(), "event body still moves");
    drag({ 220.0f, 30.0f });
    up({ 220.0f, 30.0f });
    const auto* afterMove = findClip(*session.loadSessionSnapshotForAudioThread(), id);
    expect(afterMove != nullptr && afterMove->getStartSample() != startBefore, "move committed");
    session.moveClip(id, 0);

    down({ 3.0f, 6.0f });
    expect(view.isClipEnvelopeGestureInProgress(), "top-left is fade-in");
    drag({ 80.0f, 6.0f });
    drag({ 120.0f, 6.0f });
    up({ 120.0f, 6.0f });
    const auto* faded = findClip(*session.loadSessionSnapshotForAudioThread(), id);
    expect(faded != nullptr && faded->getFadeInSamples() > 50 && faded->getFadeOutSamples() == 0, "fade-in drag");
    expect(records == 1, "the whole fade drag is one undo step");

    session.setClipEventAmplitude(id, 0.0f, 0, 0);
    records = 0;
    down({ 397.0f, 6.0f });
    expect(view.isClipEnvelopeGestureInProgress(), "top-right is fade-out");
    drag({ 300.0f, 6.0f });
    view.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    const auto* cancelled = findClip(*session.loadSessionSnapshotForAudioThread(), id);
    expect(cancelled != nullptr && cancelled->getFadeOutSamples() == 0, "escape restores the fade");
    expect(records == 0 && !view.isClipEnvelopeGestureInProgress(), "escape records nothing");

    down({ 200.0f, 6.0f });
    expect(view.isClipEnvelopeGestureInProgress(), "top centre is event gain");
    drag({ 200.0f, -34.0f });
    up({ 200.0f, -34.0f });
    const auto* gained = findClip(*session.loadSessionSnapshotForAudioThread(), id);
    expect(gained != nullptr && gained->getEventGainDb() > 8.0f && gained->getEventGainDb() < 12.0f,
           "upward drag raises gain");

    session.setClipEventAmplitude(id, -6.0f, 0, 0);
    records = 0;
    down({ 200.0f, 6.0f }, juce::ModifierKeys(juce::ModifierKeys::commandModifier));
    up({ 200.0f, 6.0f });
    const auto* reset = findClip(*session.loadSessionSnapshotForAudioThread(), id);
    expect(reset != nullptr && reset->getEventGainDb() == 0.0f, "ctrl-click resets gain");
    expect(records == 1, "reset is one undo step");

    view.setSize(800, 28);
    down({ 3.0f, 6.0f });
    expect(!view.isClipEnvelopeGestureInProgress(), "micro hides the fade handle");
    up({ 3.0f, 6.0f });
}

/// Reference for DelayTailProbe: the same delay, fed the event source (gain × fade) and
/// zeros everywhere else. The tail is whatever the delay still holds; it is not scaled again.
[[nodiscard]] std::vector<float> referenceDelay(const int n,
                                                const float feedback,
                                                const float gainDb,
                                                const std::int64_t fadeOut,
                                                const int a0,
                                                const int aLen,
                                                const float aLevel,
                                                const int b0,
                                                const int bLen,
                                                const float bLevel)
{
    std::array<float, DelayTailProbe::kDelay> line{};
    int pos = 0;
    const float g = event_amplitude::linearGain(gainDb);
    std::vector<float> out(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n; ++i)
    {
        float src = 0.0f;
        if (i >= a0 && i < a0 + aLen)
        {
            src = aLevel * g * event_amplitude::envelopeAt(i - a0, aLen, 0, fadeOut);
        }
        else if (i >= b0 && i < b0 + bLen)
        {
            src = bLevel * g * event_amplitude::envelopeAt(i - b0, bLen, 0, 0);
        }
        const float y = line[static_cast<size_t>(pos)];
        line[static_cast<size_t>(pos)] = src + feedback * y;
        pos = (pos + 1) % DelayTailProbe::kDelay;
        out[static_cast<size_t>(i)] = y;
    }
    return out;
}

void expectRange(const float* got, const std::vector<float>& want, const int n, const char* what)
{
    for (int i = 0; i < n; ++i)
    {
        if (!(std::fabs(got[i] - want[static_cast<size_t>(i)]) <= 1.0e-4f))
        {
            ++gFailures;
            std::printf("[FAIL] %s at %d got=%g want=%g\n",
                        what,
                        i,
                        static_cast<double>(got[i]),
                        static_cast<double>(want[static_cast<size_t>(i)]));
            return;
        }
    }
}

void placeTailEvents(Session& session, const TrackId tid)
{
    expect(session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "a"), 0, 0, 64, tid).wasOk(),
           "tail event a");
    expect(session.addPlacedClipFromExistingMaterial(makeDcClip(64, 0.2f, 0.2f, "b"), 256, 0, 64, tid).wasOk(),
           "tail event b");
    session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate));
}

void installDelay(PluginInsertHost& host, const TrackId tid, const float feedback)
{
    expect(host.installInsertInstanceForTests(tid, InsertStage::Pre, std::make_unique<DelayTailProbe>(feedback)),
           "install delay");
}

void testInsertTailAfterEvent()
{
    constexpr int kN = 384;
    constexpr float kFb = 0.5f;
    const std::vector<float> want = referenceDelay(kN, kFb, 0.0f, 0, 0, 64, 1.0f, 256, 64, 0.2f);
    expect(want[64] > 0.9f && want[128] < want[64] && want[192] < want[128],
           "reference tail is present and decays");
    expect(want[256] < 0.2f && want[256] > 0.05f, "next event continues the decayed tail");
    expect(std::fabs(want[256] - 1.0f) > 0.5f, "next event does not revive the frozen event level");

    Engine direct(0);
    direct.session.addTrack();
    const TrackId dt = direct.session.getActiveTrackId();
    placeTailEvents(direct.session, dt);
    installDelay(direct.pluginHost, dt, kFb);
    direct.start();
    direct.transport.requestSeek(0);
    direct.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> played(static_cast<size_t>(kN), 0.0f);
    std::vector<float> playedR(static_cast<size_t>(kN), 0.0f);
    for (int start = 0; start < kN; start += kBlock)
    {
        std::vector<float> blockL(static_cast<size_t>(kBlock));
        std::vector<float> blockR(static_cast<size_t>(kBlock));
        direct.renderBlock(blockL.data(), blockR.data());
        for (int i = 0; i < kBlock; ++i)
        {
            played[static_cast<size_t>(start + i)] = blockL[static_cast<size_t>(i)];
            playedR[static_cast<size_t>(start + i)] = blockR[static_cast<size_t>(i)];
        }
    }
    expectRange(played.data(), want, kN, "direct playback tail across the event boundary");
    expectNear(playedR[64], played[64], 1.0e-4f, "tail uses the same factor on both channels");
    const std::int64_t playheadAtStop = direct.transport.readPlayheadSamplesForUi();
    direct.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    const std::vector<float> stoppedWant = referenceDelay(kN + kBlock, kFb, 0.0f, 0, 0, 64, 1.0f, 256, 64, 0.2f);
    std::vector<float> heldL(static_cast<size_t>(kBlock));
    std::vector<float> heldR(static_cast<size_t>(kBlock));
    direct.renderBlock(heldL.data(), heldR.data());
    expect(direct.transport.readPlayheadSamplesForUi() == playheadAtStop,
           "playhead stays still while the stopped tail runs");
    expectRange(heldL.data(), std::vector<float>(stoppedWant.begin() + kN, stoppedWant.begin() + kN + kBlock),
                kBlock, "stop keeps the delay tail moving");
    expectNear(heldR[0], heldL[0], 1.0e-4f, "stopped tail is on both channels");
    float frozen = 0.0f;
    for (int i = kN - kBlock; i < kN; ++i)
    {
        frozen = std::max(frozen, std::fabs(want[static_cast<size_t>(i)]));
    }
    float continued = 0.0f;
    for (float s : heldL)
    {
        continued = std::max(continued, std::fabs(s));
    }
    expect(continued > 0.01f && continued < frozen, "stopped tail decays instead of repeating the frozen block");

    Engine strip(0);
    strip.session.addTrack();
    const TrackId st = strip.session.getActiveTrackId();
    placeTailEvents(strip.session, st);
    installDelay(strip.pluginHost, st, kFb);
    strip.start();
    const auto stripSnap = strip.session.loadSessionSnapshotForAudioThread();
    const int stripIndex = stripSnap->findTrackIndexById(st);
    std::vector<float> stripL(static_cast<size_t>(kN), 0.0f);
    std::vector<float> stripR(static_cast<size_t>(kN), 0.0f);
    for (int start = 0; start < kN; start += kBlock)
    {
        playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
            *stripSnap, start, kBlock, 0, stripL.data() + start, stripR.data() + start,
            &strip.pluginHost, kInvalidTrackId, stripSnap->getArrangementExtentSamples(), stripIndex);
    }
    expectRange(stripL.data(), want, kN, "post-strip tail across the event boundary");

    Engine summing(0);
    summing.session.addTrack();
    const TrackId mt = summing.session.getActiveTrackId();
    placeTailEvents(summing.session, mt);
    installDelay(summing.pluginHost, mt, kFb);
    summing.start();
    const auto sumSnap = summing.session.loadSessionSnapshotForAudioThread();
    std::vector<float> sumL(static_cast<size_t>(kN), 0.0f);
    for (int start = 0; start < kN; start += kBlock)
    {
        juce::AudioBuffer<float> buf(2, kBlock);
        buf.clear();
        float* ptrs[2] = { buf.getWritePointer(0), buf.getWritePointer(1) };
        playback_mix_helpers::renderAudioTracksClipSummingForSegment(
            *sumSnap, start, kBlock, 0, 2, ptrs, &summing.pluginHost, kInvalidTrackId,
            sumSnap->getArrangementExtentSamples(), sumSnap->findTrackIndexById(mt));
        for (int i = 0; i < kBlock; ++i)
        {
            sumL[static_cast<size_t>(start + i)] = ptrs[0][i];
        }
    }
    expectRange(sumL.data(), want, kN, "summing path tail across the event boundary");

    Engine offline(0);
    offline.session.addTrack();
    const TrackId ot = offline.session.getActiveTrackId();
    placeTailEvents(offline.session, ot);
    installDelay(offline.pluginHost, ot, kFb);
    offline.start();
    const auto offSnap = offline.session.loadSessionSnapshotForAudioThread();
    std::vector<float> offL(static_cast<size_t>(kN), 0.0f);
    for (int start = 0; start < kN; start += kBlock)
    {
        juce::AudioBuffer<float> buf(2, kBlock);
        buf.clear();
        float* ptrs[2] = { buf.getWritePointer(0), buf.getWritePointer(1) };
        offline.playback.renderOfflineMixdownBlock(*offSnap, nullptr, start, kBlock, ptrs, false);
        for (int i = 0; i < kBlock; ++i)
        {
            offL[static_cast<size_t>(start + i)] = ptrs[0][i];
        }
    }
    expectRange(offL.data(), want, kN, "offline tail across the event boundary");

    juce::AudioBuffer<float> eventOnly(2, 64);
    eventOnly.clear();
    float* eventPtrs[2] = { eventOnly.getWritePointer(0), eventOnly.getWritePointer(1) };
    Engine exportEnd(0);
    exportEnd.session.addTrack();
    const TrackId xt = exportEnd.session.getActiveTrackId();
    placeTailEvents(exportEnd.session, xt);
    installDelay(exportEnd.pluginHost, xt, kFb);
    exportEnd.start();
    const auto exportSnap = exportEnd.session.loadSessionSnapshotForAudioThread();
    exportEnd.playback.renderOfflineMixdownBlock(*exportSnap, nullptr, 0, 64, eventPtrs, false);
    expect(eventOnly.getNumSamples() == 64, "export block stays at the requested length");
    expectNear(eventPtrs[0][63], want[63], 1.0e-4f, "rendering only the event does not append the tail");
    juce::AudioBuffer<float> afterEvent(2, 64);
    afterEvent.clear();
    float* afterPtrs[2] = { afterEvent.getWritePointer(0), afterEvent.getWritePointer(1) };
    exportEnd.playback.renderOfflineMixdownBlock(*exportSnap, nullptr, 64, 64, afterPtrs, false);
    expectNear(afterPtrs[0][0], want[64], 1.0e-4f, "the next requested range still carries the tail");

    Engine ahead(2);
    ahead.session.addTrack();
    const TrackId at = ahead.session.getActiveTrackId();
    placeTailEvents(ahead.session, at);
    installDelay(ahead.pluginHost, at, kFb);
    ahead.start();
    ahead.transport.requestSeek(0);
    ahead.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> ra(static_cast<size_t>(kN), 0.0f);
    for (int start = 0; start < kN; start += kBlock)
    {
        if (start > 0)
        {
            ahead.pump();
        }
        std::vector<float> blockL(static_cast<size_t>(kBlock));
        std::vector<float> blockR(static_cast<size_t>(kBlock));
        ahead.renderBlock(blockL.data(), blockR.data());
        for (int i = 0; i < kBlock; ++i)
        {
            ra[static_cast<size_t>(start + i)] = blockL[static_cast<size_t>(i)];
        }
    }
    expectRange(ra.data(), want, kN, "read-ahead tail across the event boundary");

    Engine shaped(0);
    shaped.session.addTrack();
    const TrackId ht = shaped.session.getActiveTrackId();
    expect(shaped.session.addPlacedClipFromExistingMaterial(makeDcClip(128, 1.0f, 1.0f, "fade"), 0, 0, 128, ht).wasOk(),
           "fade event");
    const PlacedClipId hid = frontClipId(shaped.session, ht);
    shaped.session.setClipEventAmplitude(hid, -6.020599f, 0, 128);
    shaped.session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate));
    installDelay(shaped.pluginHost, ht, 0.0f);
    shaped.start();
    const auto shapedSnap = shaped.session.loadSessionSnapshotForAudioThread();
    std::vector<float> shapedL(256, 0.0f);
    std::vector<float> shapedR(256, 0.0f);
    const std::vector<float> shapedWant = referenceDelay(256, 0.0f, -6.020599f, 128, 0, 128, 1.0f, -1, 0, 0.0f);
    for (int start = 0; start < 256; start += kBlock)
    {
        playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
            *shapedSnap, start, kBlock, 0, shapedL.data() + start, shapedR.data() + start,
            &shaped.pluginHost, kInvalidTrackId, shapedSnap->getArrangementExtentSamples(),
            shapedSnap->findTrackIndexById(ht));
    }
    expectRange(shapedL.data(), shapedWant, 256, "fade and event gain scale the source, not the tail");
    const float g = event_amplitude::linearGain(-6.020599f);
    const float delayedSource = g * event_amplitude::envelopeAt(64, 128, 0, 128);
    expectNear(shapedL[128], delayedSource, 1.0e-4f, "first tail sample is the delayed faded source");
    expect(std::fabs(shapedL[128]) > 1.0e-3f, "fade-out does not gate the tail to silence");
    const float during = g * event_amplitude::envelopeAt(16, 128, 0, 128);
    expectNear(shapedL[80], during, 1.0e-4f, "fade is not applied a second time to the chain output");
}

void renderOne(Engine& engine, float* left, float* right)
{
    engine.renderBlock(left, right);
}

void testStopPauseCycleAndReadAhead()
{
    std::printf("\n-- stop, pause, cycle, read-ahead tail --\n");
    constexpr float kFb = 0.5f;

    Engine gap(0);
    gap.session.addTrack();
    const TrackId gt = gap.session.getActiveTrackId();
    expect(gap.session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "impulse"), 0, 0, 64, gt).wasOk(),
           "impulse event");
    expect(gap.pluginHost.installInsertInstanceForTests(gt, InsertStage::Pre,
                                                        std::make_unique<DelayTailProbe>(0.0f, 256)),
           "install long delay");
    gap.start();
    gap.transport.requestSeek(0);
    gap.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> block(static_cast<size_t>(kBlock));
    std::vector<float> blockR(static_cast<size_t>(kBlock));
    renderOne(gap, block.data(), blockR.data());
    float earlyPeak = 0.0f;
    for (float s : block)
    {
        earlyPeak = std::max(earlyPeak, std::fabs(s));
    }
    expect(earlyPeak < 0.05f, "echo has not arrived during the first block");
    const std::int64_t heldAt = gap.transport.readPlayheadSamplesForUi();
    gap.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    renderOne(gap, block.data(), blockR.data());
    float silentPeak = 0.0f;
    for (float s : block)
    {
        silentPeak = std::max(silentPeak, std::fabs(s));
    }
    expect(silentPeak < 0.05f, "stopped transport still waits out the delay");
    expect(gap.transport.readPlayheadSamplesForUi() == heldAt, "playhead stays during the silent gap");
    renderOne(gap, block.data(), blockR.data());
    float echoPeak = 0.0f;
    for (int i = 0; i < 64; ++i)
    {
        echoPeak = std::max(echoPeak, std::fabs(block[static_cast<size_t>(i)]));
    }
    expect(echoPeak > 0.9f, "the delayed impulse arrives on time after stop");

    Engine paused(0);
    paused.session.addTrack();
    const TrackId pt = paused.session.getActiveTrackId();
    expect(paused.session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "p"), 0, 0, 64, pt).wasOk(),
           "pause event");
    installDelay(paused.pluginHost, pt, kFb);
    paused.start();
    paused.transport.requestSeek(0);
    paused.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    renderOne(paused, block.data(), blockR.data());
    const float frozenSample = block[64];
    paused.transport.requestPlaybackIntent(PlaybackIntent::Paused);
    const std::int64_t pausedAt = paused.transport.readPlayheadSamplesForUi();
    renderOne(paused, block.data(), blockR.data());
    expect(paused.transport.readPlayheadSamplesForUi() == pausedAt, "pause does not move the playhead");
    expect(std::fabs(block[0] - 0.5f) < 1.0e-3f, "pause continues the delay instead of freezing it");
    expect(std::fabs(block[0] - frozenSample) > 0.2f, "paused output is not a repeat of the last playing block");
    for (int i = 0; i < 16; ++i)
    {
        renderOne(paused, block.data(), blockR.data());
    }
    paused.transport.requestSeek(0);
    paused.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    renderOne(paused, block.data(), blockR.data());
    float revived = 0.0f;
    for (int i = 0; i < 64; ++i)
    {
        revived = std::max(revived, std::fabs(block[static_cast<size_t>(i)]));
    }
    expect(revived < 0.05f, "play after the wait does not revive the pre-pause delay state");

    Engine cyc(0);
    cyc.session.addTrack();
    const TrackId ct = cyc.session.getActiveTrackId();
    expect(cyc.session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "c"), 0, 0, 64, ct).wasOk(),
           "cycle event");
    installDelay(cyc.pluginHost, ct, kFb);
    cyc.start();
    cyc.session.setLeftLocatorAtSample(0);
    cyc.session.setRightLocatorAtSample(256);
    cyc.transport.requestCycleEnabled(true);
    cyc.transport.requestSeek(0);
    cyc.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> first(static_cast<size_t>(kBlock));
    std::vector<float> second(static_cast<size_t>(kBlock));
    std::vector<float> third(static_cast<size_t>(kBlock));
    renderOne(cyc, first.data(), blockR.data());
    renderOne(cyc, second.data(), blockR.data());
    renderOne(cyc, third.data(), blockR.data());
    expect(std::fabs(first[0]) < 0.05f, "cycle starts with an empty delay");
    expect(std::fabs(first[64] - 1.0f) < 1.0e-3f, "cycle event reaches the delay");
    expect(std::fabs(second[0] - 0.5f) < 1.0e-3f, "the tail continues up to the locator");
    expect(std::fabs(third[0] - 0.125f) < 1.0e-3f, "cycle wrap keeps the decayed tail");
    expect(std::fabs(third[0]) > 0.05f, "cycle wrap does not reset the delay");
    float cyclePeak = 0.0f;
    for (float s : second)
    {
        cyclePeak = std::max(cyclePeak, std::fabs(s));
    }
    expect(cyclePeak < 1.6f, "cycle does not stack the event on itself");

    Engine ahead(2);
    ahead.session.addTrack();
    const TrackId at = ahead.session.getActiveTrackId();
    expect(ahead.session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "near"), 0, 0, 64, at).wasOk(),
           "read-ahead near event");
    expect(ahead.session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "far"), 200000, 0, 64, at).wasOk(),
           "read-ahead far event");
    installDelay(ahead.pluginHost, at, kFb);
    ahead.start();
    ahead.transport.requestSeek(0);
    ahead.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    ahead.pump();
    renderOne(ahead, block.data(), blockR.data());
    const std::int64_t aheadAt = ahead.transport.readPlayheadSamplesForUi();
    ahead.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    renderOne(ahead, block.data(), blockR.data());
    float stoppedPeak = 0.0f;
    for (float s : block)
    {
        stoppedPeak = std::max(stoppedPeak, std::fabs(s));
    }
    expect(ahead.transport.readPlayheadSamplesForUi() == aheadAt, "read-ahead stop leaves the playhead");
    expect(stoppedPeak > 0.2f && stoppedPeak < 1.2f, "read-ahead stop rings the tail and does not play the far event");
    const float firstStopped = stoppedPeak;
    renderOne(ahead, block.data(), blockR.data());
    float laterPeak = 0.0f;
    for (float s : block)
    {
        laterPeak = std::max(laterPeak, std::fabs(s));
    }
    expect(laterPeak < firstStopped, "read-ahead tail keeps decaying after the queue is dropped");

    Engine sum(0);
    sum.session.addTrack();
    const TrackId st = sum.session.getActiveTrackId();
    expect(sum.session.addPlacedClipFromExistingMaterial(makeDcClip(64, 1.0f, 1.0f, "s"), 0, 0, 64, st).wasOk(),
           "summing event");
    installDelay(sum.pluginHost, st, kFb);
    sum.start();
    sum.transport.requestSeek(0);
    sum.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    renderOne(sum, block.data(), blockR.data());
    sum.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    const auto sumSnap = sum.session.loadSessionSnapshotForAudioThread();
    juce::AudioBuffer<float> sumBuf(2, kBlock);
    sumBuf.clear();
    float* sumPtrs[2] = { sumBuf.getWritePointer(0), sumBuf.getWritePointer(1) };
    playback_mix_helpers::renderAudioTracksClipSummingForSegment(
        *sumSnap, 128, kBlock, 0, 2, sumPtrs, &sum.pluginHost, kInvalidTrackId,
        sumSnap->getArrangementExtentSamples(), sumSnap->findTrackIndexById(st), nullptr, nullptr, nullptr, true);
    expect(std::fabs(sumPtrs[0][0] - 0.5f) < 1.0e-3f, "summing silence feed continues the same delay");
}

class ReleaseProbe final : public juce::AudioPluginInstance
{
public:
    static constexpr int kRelease = 64;

    ReleaseProbe()
        : juce::AudioPluginInstance(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }
    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "ReleaseProbe";
        d.pluginFormatName = "Test";
        d.isInstrument = true;
    }
    const juce::String getName() const override { return "ReleaseProbe"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        const int n = buffer.getNumSamples();
        float* left = buffer.getNumChannels() > 0 ? buffer.getWritePointer(0) : nullptr;
        float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        auto midiIt = midi.begin();
        const auto midiEnd = midi.end();
        for (int i = 0; i < n; ++i)
        {
            while (midiIt != midiEnd && (*midiIt).samplePosition <= i)
            {
                const juce::MidiMessage m = (*midiIt++).getMessage();
                if (m.isAllNotesOff() || m.isAllSoundOff())
                {
                    sawAllNotesOff_ = true;
                }
                else if (m.isNoteOn())
                {
                    const int note = juce::jlimit(0, 127, m.getNoteNumber());
                    peak_[static_cast<size_t>(note)] = m.getFloatVelocity();
                    releaseLeft_[static_cast<size_t>(note)] = 0;
                }
                else if (m.isNoteOff())
                {
                    const int note = juce::jlimit(0, 127, m.getNoteNumber());
                    if (peak_[static_cast<size_t>(note)] > 0.0f && releaseLeft_[static_cast<size_t>(note)] == 0)
                    {
                        releaseLeft_[static_cast<size_t>(note)] = kRelease;
                        ++noteOffs_;
                    }
                }
            }
            float s = independent_;
            for (int note = 0; note < 128; ++note)
            {
                if (releaseLeft_[static_cast<size_t>(note)] > 0)
                {
                    s += peak_[static_cast<size_t>(note)]
                         * ((float)releaseLeft_[static_cast<size_t>(note)] / (float)kRelease);
                    if (--releaseLeft_[static_cast<size_t>(note)] == 0)
                    {
                        peak_[static_cast<size_t>(note)] = 0.0f;
                    }
                }
                else
                {
                    s += peak_[static_cast<size_t>(note)];
                }
            }
            if (left != nullptr)
            {
                left[i] = s;
            }
            if (right != nullptr)
            {
                right[i] = s;
            }
            if (i == 0)
            {
                sample0_ = s;
            }
            if (i == 96)
            {
                sample96_ = s;
            }
        }
        ++blocks_;
    }
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

    void holdIndependent(const float level) noexcept { independent_ = level; }
    [[nodiscard]] bool sawAllNotesOff() const noexcept { return sawAllNotesOff_; }
    [[nodiscard]] int noteOffs() const noexcept { return noteOffs_; }
    [[nodiscard]] float sample0() const noexcept { return sample0_; }
    [[nodiscard]] float sample96() const noexcept { return sample96_; }
    [[nodiscard]] int blocks() const noexcept { return blocks_; }

private:
    std::array<float, 128> peak_{};
    std::array<int, 128> releaseLeft_{};
    float independent_ = 0.0f;
    float sample0_ = 0.0f;
    float sample96_ = 0.0f;
    bool sawAllNotesOff_ = false;
    int noteOffs_ = 0;
    int blocks_ = 0;
};

void testInstrumentWindowAndRelease()
{
    std::printf("\n-- instrument window, release, second source --\n");
    Engine eng(0);
    const auto tid = eng.session.appendExperimentalInstrumentShellTrack("Probe");
    expect(tid.has_value(), "instrument track");
    if (!tid.has_value())
    {
        return;
    }
    InstrumentTrackController ctl(nullptr);
    ctl.setSession(&eng.session);
    expect(ctl.bootstrapGrooveAgentShellForSessionTrack(*tid), "instrument shell");
    TimelineMidiNote held;
    held.midiNote = 60;
    held.velocity = 127;
    held.channel = 1;
    held.startTick = 0;
    held.durationTicks = 9600;
    TimelineMidiNote later = held;
    later.midiNote = 64;
    later.startTick = 960;
    const InstrumentMidiClipId id = ctl.appendImportedTimelineMidiClipAtSamples({ held, later }, 0, "Take");
    expect(id != 0, "midi clip");
    expect(ctl.applyInstrumentMidiClipVisibleTrim(id, 0, 64), "right trim");
    const auto trimmed = ctl.loadRenderSnapshotForAudioThread();
    expect(trimmed != nullptr && trimmed->notes.size() == 1, "only the note that starts inside the window");
    if (trimmed != nullptr && !trimmed->notes.empty())
    {
        expect(trimmed->notes[0].absSample == 0, "note-on at the window start");
        expect(trimmed->notes[0].noteOffAbsSample == 64, "note-off at the trimmed end");
        expect(trimmed->notes[0].midiNote == 60, "the later note-on was not scheduled");
    }
    expect(ctl.applyInstrumentMidiClipVisibleTrim(id, 100, 500), "left trim");
    const auto dropped = ctl.loadRenderSnapshotForAudioThread();
    int outside = 0;
    if (dropped != nullptr)
    {
        for (const auto& n : dropped->notes)
        {
            if (n.absSample < 100 || n.absSample >= 600)
            {
                ++outside;
            }
        }
    }
    expect(outside == 0, "left trim emits no note-on outside the window");

    InstrumentTrackController sustainOnly(nullptr);
    sustainOnly.setSession(&eng.session);
    const auto tid2 = eng.session.appendExperimentalInstrumentShellTrack("Sustain");
    expect(tid2.has_value() && sustainOnly.bootstrapGrooveAgentShellForSessionTrack(*tid2), "sustain shell");
    if (tid2.has_value())
    {
        TimelineMidiNote pedalNote = held;
        pedalNote.durationTicks = 9600;
        const InstrumentMidiClipId sid = sustainOnly.appendImportedTimelineMidiClipAtSamples({ pedalNote }, 0, "Pedal");
        if (InstrumentMidiClip* clip = sustainOnly.getClipById(sid))
        {
            MidiCcPoint down;
            down.startTick = 0;
            down.controller = 64;
            down.value = 127;
            down.channel = 1;
            down.interpolationToNext = MidiCcInterpolation::linear;
            clip->pattern.ccPoints.push_back(down);
            sustainOnly.notifyClipPatternMutated(sid);
        }
        expect(sustainOnly.applyInstrumentMidiClipVisibleTrim(sid, 0, 400), "trim under a held pedal");
        const auto pedalSnap = sustainOnly.loadRenderSnapshotForAudioThread();
        bool pedalUp = false;
        bool pedalDown = false;
        if (pedalSnap != nullptr)
        {
            for (const auto& stream : pedalSnap->ccStreams)
            {
                if ((int)stream.controller != 64)
                {
                    continue;
                }
                for (const auto& ev : stream.events)
                {
                    if (ev.value > 0)
                    {
                        pedalDown = true;
                    }
                    if (ev.absSample == 400 && ev.value == 0)
                    {
                        pedalUp = true;
                    }
                }
            }
            for (const auto& n : pedalSnap->notes)
            {
                expect(n.absSample >= 0 && n.absSample < 400 && n.noteOffAbsSample <= 400,
                       "pedal clip stays inside its window");
            }
        }
        expect(pedalDown && pedalUp, "a held pedal is released at the clip's own end");
    }

    expect(ctl.applyInstrumentMidiClipVisibleTrim(id, 0, 64), "restore the sounding window");
    auto host = std::make_unique<ExperimentalInstrumentHost>();
    ExperimentalInstrumentHost* const hostPtr = host.get();
    eng.playback.setExperimentalInstrumentDeviceLifecycleHooks(
        [hostPtr](const double sr, const int bs) { hostPtr->prepareForDevice(sr, bs); },
        [hostPtr] { hostPtr->releaseResources(); },
        {});
    eng.session.setArrangementExtentSamples(static_cast<std::int64_t>(kRate) * 30);
    eng.playback.setInstrumentRenderSerialForDiagnostics(true);
    eng.playback.rebuildRoutingPlanFromSession();
    eng.playback.audioDeviceAboutToStart(&eng.device);
    auto probeOwner = std::make_unique<ReleaseProbe>();
    ReleaseProbe* probe = probeOwner.get();
    expect(host->installInstrumentInstanceForTests(std::move(probeOwner)), "install release probe");
    ExperimentalInstrumentPlaybackSnapshot published;
    published.entries.push_back(ExperimentalInstrumentPlaybackEntry{ *tid, host.get(), &ctl, nullptr });
    InstrumentTrackController other(nullptr);
    other.setSession(&eng.session);
    const auto otherTid = eng.session.appendExperimentalInstrumentShellTrack("Other");
    if (otherTid.has_value())
    {
        expect(other.bootstrapGrooveAgentShellForSessionTrack(*otherTid), "other midi shell");
        published.midiSources.push_back(ExperimentalMidiSourcePlaybackEntry{ *otherTid, &other });
    }
    eng.playback.publishExperimentalInstrumentPlaybackSnapshot(
        std::make_shared<const ExperimentalInstrumentPlaybackSnapshot>(std::move(published)));
    eng.playback.rebuildRoutingPlanFromSession();
    expect(eng.pluginHost.installInsertInstanceForTests(*tid, InsertStage::Post,
                                                        std::make_unique<DelayTailProbe>(0.5f)),
           "instrument delay");
    eng.transport.requestSeek(0);
    eng.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> played(static_cast<size_t>(kBlock));
    std::vector<float> playedR(static_cast<size_t>(kBlock));
    renderOne(eng, played.data(), playedR.data());
    expect(probe->sample0() > 0.9f, "the note sounds inside the window");
    expect(probe->sample96() > 0.05f && probe->sample96() < probe->sample0(),
           "release continues after the note-off at the window end");
    expect(probe->noteOffs() >= 1, "the crossing note received a note-off");
    expect(std::fabs(played[96]) > 0.5f, "the instrument insert delay is heard on the device");
    probe->holdIndependent(0.2f);
    const std::int64_t at = eng.transport.readPlayheadSamplesForUi();
    const int blocksAfterPlay = probe->blocks();
    eng.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    renderOne(eng, played.data(), playedR.data());
    expect(eng.transport.readPlayheadSamplesForUi() == at, "instrument stop holds the playhead");
    expect(!probe->sawAllNotesOff(), "stop does not send All Notes Off");
    expect(probe->blocks() > blocksAfterPlay, "the instrument keeps processing while stopped");
    expect(probe->sample0() >= 0.2f, "an independent note is not cleared by stop");
    float stoppedPeak = 0.0f;
    for (float s : played)
    {
        stoppedPeak = std::max(stoppedPeak, std::fabs(s));
    }
    expect(stoppedPeak > 0.05f, "the instrument insert tail continues after stop");
}

void testReadAheadHandoverWhileWorkerHeld()
{
    std::printf("\n-- read-ahead handover while processBlock is in flight --\n");
    readahead::setConfiguredReadAheadDepth(2);
    Engine eng(0);
    readahead::setConfiguredReadAheadDepth(0);
    readahead::ReadAheadRenderer* const ra = eng.playback.experimentalReadAhead();
    expect(ra != nullptr, "thread-mode read-ahead");
    if (ra == nullptr)
    {
        return;
    }
    eng.session.addTrack();
    const TrackId tid = eng.session.getActiveTrackId();
    expect(eng.session.addPlacedClipFromExistingMaterial(makeDcClip(48000, 1.0f, 1.0f, "long"), 0, 0, 48000, tid).wasOk(),
           "long event");
    auto probeOwner = std::make_unique<DelayTailProbe>(0.0f);
    DelayTailProbe* probe = probeOwner.get();
    expect(eng.pluginHost.installInsertInstanceForTests(tid, InsertStage::Pre, std::move(probeOwner)),
           "held delay");
    eng.start();
    eng.transport.requestSeek(0);
    eng.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    std::vector<float> block(static_cast<size_t>(kBlock));
    std::vector<float> blockR(static_cast<size_t>(kBlock));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ra->countersSnapshot().adopted < 1 && std::chrono::steady_clock::now() < deadline)
    {
        renderOne(eng, block.data(), blockR.data());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(ra->countersSnapshot().adopted >= 1, "a row was adopted before the hold");
    probe->setHoldInProcessBlock(true);
    renderOne(eng, block.data(), blockR.data());
    const auto heldDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!probe->isHeldInProcessBlock() && std::chrono::steady_clock::now() < heldDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(probe->isHeldInProcessBlock(), "worker is inside processBlock");
    eng.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    for (int i = 0; i < 4; ++i)
    {
        renderOne(eng, block.data(), blockR.data());
    }
    expect(probe->overlaps() == 0, "stop does not process the instance the worker still holds");
    probe->setHoldInProcessBlock(false);
    const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (probe->isHeldInProcessBlock() && std::chrono::steady_clock::now() < releaseDeadline)
    {
        std::this_thread::yield();
    }
    const int callsAfterRelease = probe->calls_;
    float peaks[6] = {};
    for (int i = 0; i < 6; ++i)
    {
        renderOne(eng, block.data(), blockR.data());
        for (float s : block)
        {
            peaks[i] = std::max(peaks[i], std::fabs(s));
        }
    }
    expect(probe->overlaps() == 0, "the callback processes the instance only after the worker left");
    expect(probe->calls_ > callsAfterRelease, "the stopped callback runs the chain after the handover");
    float early = 0.0f;
    for (int i = 0; i < 5; ++i)
    {
        early = std::max(early, peaks[i]);
    }
    expect(early > 0.5f && peaks[5] < 0.05f, "silence after the handover empties the one-shot delay");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testMath();
    testSessionLifecycle();
    testProjectFile();
    testAudioPaths();
    testLaneHandles();
    testInsertTailAfterEvent();
    testStopPauseCycleAndReadAhead();
    testInstrumentWindowAndRelease();
    testReadAheadHandoverWhileWorkerHeld();
    if (gFailures == 0)
    {
        std::printf("[OK] event envelope\n");
        return 0;
    }
    std::printf("[FAIL] %d event-envelope checks\n", gFailures);
    return 1;
}
