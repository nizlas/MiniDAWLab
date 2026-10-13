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

#include <cmath>
#include <cstdio>
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

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testMath();
    testSessionLifecycle();
    testProjectFile();
    testAudioPaths();
    testLaneHandles();
    if (gFailures == 0)
    {
        std::printf("[OK] event envelope\n");
        return 0;
    }
    std::printf("[FAIL] %d event-envelope checks\n", gFailures);
    return 1;
}
