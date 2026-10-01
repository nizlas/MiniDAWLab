// =============================================================================
// InsertPersistenceFocusedTests — VST3 insert chains across project save / reload (production code)
// =============================================================================
//
// User report: "I added DAL Mono Delay as a Post insert, saved, closed and reopened — it was gone."
//
// Root cause (1.1.6 and earlier): `Session::saveProjectToFile` exported insert chains for
// `TrackKind::Audio` rows only, and `applyLoadedProjectModel` skipped `"instrument"` rows, while the
// Inspector offers Pre/Post inserts (and the engine runs them) on Audio, Instrument, Group and Master
// rows. Any insert on a non-audio row was silently dropped at save time. A second gap: a saved insert
// whose plugin could not be instantiated on load was dropped from the live chain, so the next save
// erased it (the "primary data-loss" pattern).
//
// Everything here runs the PRODUCTION implementations: `Session::saveProjectToFile` /
// `loadProjectFromFile` (the same entry points Save, Save As and autosave use), `ProjectFile`
// JSON, and the real `PluginInsertHost` hosting the real DAL Mono Delay VST3 plus one other
// installed VST3 effect. Parameters are set the way the project-load / undo path sets them
// (host-format state blobs from `getStateInformation`), and audio is verified through
// `audioThread_processChainForTrack` — the exact function every strip pass calls.
//
// Usage:
//   InsertPersistenceFocusedTests.exe [--delay-vst3 <DALMonoDelay.vst3>] [--other-vst3 <X.vst3>]
//                                     [--keep]   (keep the temp work folder for inspection)
//   InsertPersistenceFocusedTests.exe --make-fixture <dir>   (audio-only project for
//                                     `MiniDAWLab.exe --stability-inserts`)
// Exit 0 = all checks green.
// =============================================================================

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "io/MonoWavFileWriter.h"
#include "io/ProjectFile.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/PluginTrackSlot.h"
#include "transport/Transport.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool condition, const juce::String& label)
{
    ++checks;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label.toRawUTF8());
    std::fflush(stdout);
    if (!condition)
    {
        ++failures;
    }
}

void info(const juce::String& text)
{
    std::printf("       %s\n", text.toRawUTF8());
    std::fflush(stdout);
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

void pumpMessageLoop(const int ms)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil(ms);
}

[[nodiscard]] juce::File writeToneWav(const juce::File& dir, const double seconds)
{
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        pcm[static_cast<size_t>(i)]
            = static_cast<float>(0.25 * std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * i / kRate));
    }
    const juce::File audioDir = dir.getChildFile("Audio");
    (void)audioDir.createDirectory();
    const juce::File wav = audioDir.getChildFile("tone.wav");
    const float* chans[1] = { pcm.data() };
    const juce::Result r = MonoWavFileWriter::writeMulti24BitWavSegment(wav, chans, 1, n, kRate);
    if (!r.wasOk())
    {
        info("tone WAV write failed: " + r.getErrorMessage());
        return {};
    }
    return wav;
}

// ---------------------------------------------------------------------------------------------
// Host-format state blobs: what a DAL project stores for an insert, produced from a real hosted
// instance whose public parameters were set like a user would in the editor.
// ---------------------------------------------------------------------------------------------
struct DelaySettings
{
    float delayMs = 250.0f;
    float mixPercent = 50.0f;
    float feedbackPercent = 0.0f;
};

[[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> instantiateFirstType(const juce::File& bundle,
                                                                             juce::String& err)
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
        err = "no plugin types in " + bundle.getFullPathName();
        return nullptr;
    }
    return formats.createPluginInstance(*found[0], kRate, kBlock, err);
}

[[nodiscard]] bool setParamByName(juce::AudioPluginInstance& inst, const juce::String& name, const float normalised)
{
    for (auto* p : inst.getParameters())
    {
        if (p->getName(64) == name)
        {
            p->setValue(normalised);
            return true;
        }
    }
    return false;
}

/// Sets a hosted parameter from user text through the plug-in's own text parser (the VST3
/// controller's `getParamValueByString`) — the plug-in's skewed ranges stay its own business.
[[nodiscard]] bool setParamByText(juce::AudioPluginInstance& inst, const juce::String& name, const juce::String& text)
{
    for (auto* p : inst.getParameters())
    {
        if (p->getName(64) == name)
        {
            p->setValue(p->getValueForText(text));
            info("  param \"" + name + "\" <- \"" + text + "\" now reads \"" + p->getText(p->getValue(), 32) + "\"");
            return true;
        }
    }
    return false;
}

/// Sync off, free delay time, feedback / mix as given, both filters off.
[[nodiscard]] juce::MemoryBlock makeDelayStateBlob(const juce::File& delayBundle, const DelaySettings& s)
{
    juce::String err;
    auto inst = instantiateFirstType(delayBundle, err);
    if (inst == nullptr)
    {
        info("delay state blob: " + err);
        return {};
    }
    bool ok = setParamByName(*inst, "Sync", 0.0f);
    ok = setParamByText(*inst, "Delay time", juce::String(s.delayMs, 1) + " ms") && ok;
    ok = setParamByText(*inst, "Feedback", juce::String(s.feedbackPercent, 1) + " %") && ok;
    ok = setParamByText(*inst, "Mix", juce::String(s.mixPercent, 1) + " %") && ok;
    ok = setParamByName(*inst, "Low Cut enabled", 0.0f) && ok;
    ok = setParamByName(*inst, "High Cut enabled", 0.0f) && ok;
    if (!ok)
    {
        info("delay state blob: some parameter names were not found on the hosted delay");
    }
    juce::MemoryBlock blob;
    inst->getStateInformation(blob);
    return blob;
}

/// The other effect: its first automatable float parameter moved to a non-default value.
[[nodiscard]] juce::MemoryBlock makeOtherStateBlob(const juce::File& otherBundle, juce::String& changedParamName)
{
    juce::String err;
    auto inst = instantiateFirstType(otherBundle, err);
    if (inst == nullptr)
    {
        info("other state blob: " + err);
        return {};
    }
    juce::AudioProcessorParameter* chosen = nullptr;
    for (auto* p : inst->getParameters())
    {
        if (p->isAutomatable() && !p->isDiscrete())
        {
            chosen = p;
            break;
        }
    }
    if (chosen == nullptr && !inst->getParameters().isEmpty())
    {
        chosen = inst->getParameters().getFirst();
    }
    if (chosen != nullptr)
    {
        const float before = chosen->getValue();
        chosen->setValue(before < 0.5f ? 0.8f : 0.2f);
        changedParamName = chosen->getName(64);
        info("  other plugin param \"" + changedParamName + "\" " + juce::String(before, 3) + " -> "
             + juce::String(chosen->getValue(), 3) + " (\"" + chosen->getText(chosen->getValue(), 32) + "\")");
    }
    juce::MemoryBlock blob;
    inst->getStateInformation(blob);
    return blob;
}

// ---------------------------------------------------------------------------------------------
// Audio through the production chain processor
// ---------------------------------------------------------------------------------------------
struct ImpulseResponse
{
    std::vector<float> left;
    int firstPeakIndex = -1;    ///< dry impulse
    int echoPeakIndex = -1;     ///< strongest sample after the dry impulse
    float echoPeak = 0.0f;
};

/// Warm-up (silence) for `warmupSeconds`, then an impulse at sample 0 of the capture; all stages
/// present on the track run in Pre→Post order exactly like the engine's strip passes.
[[nodiscard]] ImpulseResponse captureImpulseResponse(PluginInsertHost& host,
                                                     const TrackId tid,
                                                     const double warmupSeconds,
                                                     const double captureSeconds)
{
    ImpulseResponse ir;
    const auto runBlock = [&](const bool impulse, const bool capture) {
        host.audioThread_clearScratch(2, kBlock);
        float* const* ptrs = host.audioThread_getScratchWritePointers();
        if (impulse)
        {
            ptrs[0][0] = 1.0f;
            ptrs[1][0] = 1.0f;
        }
        host.audioThread_processChainForTrack(tid, InsertStage::Pre, kBlock);
        host.audioThread_processChainForTrack(tid, InsertStage::Post, kBlock);
        if (capture)
        {
            ir.left.insert(ir.left.end(), ptrs[0], ptrs[0] + kBlock);
        }
    };
    const int warmBlocks = static_cast<int>(warmupSeconds * kRate / kBlock);
    for (int i = 0; i < warmBlocks; ++i)
    {
        runBlock(false, false);
    }
    const int capBlocks = static_cast<int>(captureSeconds * kRate / kBlock);
    for (int i = 0; i < capBlocks; ++i)
    {
        runBlock(i == 0, true);
    }
    // Dry impulse = sample 0; echo = strongest sample after a 2 ms guard.
    ir.firstPeakIndex = 0;
    const int guard = static_cast<int>(0.002 * kRate);
    for (int i = guard; i < static_cast<int>(ir.left.size()); ++i)
    {
        const float a = std::fabs(ir.left[static_cast<size_t>(i)]);
        if (a > ir.echoPeak)
        {
            ir.echoPeak = a;
            ir.echoPeakIndex = i;
        }
    }
    return ir;
}

[[nodiscard]] double maxAbsDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double d = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        d = std::max(d, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    }
    return d;
}

// ---------------------------------------------------------------------------------------------
// Project JSON helpers (read-only inspection + controlled rewrites that simulate an unavailable
// plugin WITHOUT touching the user's installed plugins)
// ---------------------------------------------------------------------------------------------
struct JsonInsert
{
    juce::int64 slotId = 0;
    juce::String stage;
    juce::String path;
    juce::String identifier;
    juce::String stateBase64;
};

[[nodiscard]] std::map<juce::int64, std::vector<JsonInsert>> readInsertsByTrack(const juce::File& proj)
{
    std::map<juce::int64, std::vector<JsonInsert>> out;
    const juce::var root = juce::JSON::parse(proj);
    const juce::var tracks = root.getProperty("tracks", {});
    if (!tracks.isArray())
    {
        return out;
    }
    for (const juce::var& t : *tracks.getArray())
    {
        const juce::int64 id = static_cast<juce::int64>(t.getProperty("id", 0));
        std::vector<JsonInsert> rows;
        const juce::var ins = t.getProperty("inserts", {});
        if (ins.isArray())
        {
            for (const juce::var& iv : *ins.getArray())
            {
                JsonInsert j;
                j.slotId = static_cast<juce::int64>(iv.getProperty("slotId", 0));
                j.stage = iv.getProperty("stage", "").toString();
                j.path = iv.getProperty("pluginVst3Path", "").toString();
                j.identifier = iv.getProperty("pluginIdentifier", "").toString();
                j.stateBase64 = iv.getProperty("pluginStateBase64", "").toString();
                rows.push_back(std::move(j));
            }
        }
        out[id] = std::move(rows);
    }
    return out;
}

/// Rewrites `pluginVst3Path` of the insert `slotId` on `trackId` and writes the result to `dest`.
[[nodiscard]] bool rewriteInsertPath(const juce::File& src,
                                     const juce::File& dest,
                                     const juce::int64 trackId,
                                     const juce::int64 slotId,
                                     const juce::String& newPath)
{
    juce::var root = juce::JSON::parse(src);
    const juce::var tracks = root.getProperty("tracks", {});
    if (!tracks.isArray())
    {
        return false;
    }
    bool done = false;
    for (const juce::var& t : *tracks.getArray())
    {
        if (static_cast<juce::int64>(t.getProperty("id", 0)) != trackId)
        {
            continue;
        }
        const juce::var ins = t.getProperty("inserts", {});
        if (!ins.isArray())
        {
            continue;
        }
        for (const juce::var& iv : *ins.getArray())
        {
            if (static_cast<juce::int64>(iv.getProperty("slotId", 0)) == slotId)
            {
                if (auto* obj = iv.getDynamicObject())
                {
                    obj->setProperty("pluginVst3Path", newPath);
                    done = true;
                }
            }
        }
    }
    if (!done)
    {
        return false;
    }
    return dest.replaceWithText(juce::JSON::toString(root));
}

struct TrackIds
{
    TrackId audio = kInvalidTrackId;
    TrackId instrument = kInvalidTrackId;
    TrackId group = kInvalidTrackId;
    TrackId master = kInvalidTrackId;
};

[[nodiscard]] TrackIds findTrackIds(const Session& session)
{
    TrackIds ids;
    const auto snap = session.loadSessionSnapshotForAudioThread();
    for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
    {
        const Track& t = snap->getTrack(i);
        switch (t.getKind())
        {
            case TrackKind::Audio: if (ids.audio == kInvalidTrackId) ids.audio = t.getId(); break;
            case TrackKind::Instrument: if (ids.instrument == kInvalidTrackId) ids.instrument = t.getId(); break;
            case TrackKind::Group: if (ids.group == kInvalidTrackId) ids.group = t.getId(); break;
            case TrackKind::Master: if (ids.master == kInvalidTrackId) ids.master = t.getId(); break;
            default: break;
        }
    }
    return ids;
}

[[nodiscard]] juce::String stageName(const InsertStage s)
{
    return s == InsertStage::Pre ? "pre" : "post";
}

[[nodiscard]] juce::String describeRows(const std::vector<InsertRowView>& rows)
{
    juce::String s;
    for (const auto& r : rows)
    {
        s << "[" << stageName(r.stage) << " slot" << juce::String((juce::int64)r.slotId) << " \"" << r.displayName
          << "\"" << (r.unavailable ? " UNAVAILABLE" : "") << "] ";
    }
    return s.isEmpty() ? juce::String("(none)") : s;
}

/// Undo recorder capture: `UndoRedoCoordinator::onPluginUndoRecord` marks the project dirty on
/// every record, so each counted record == one dirty mark in the app (verified by code review).
struct UndoCapture
{
    int records = 0;
    juce::StringArray labels;
    static void record(void* ctx, const juce::String& label, const PluginUndoStepSides&)
    {
        auto* self = static_cast<UndoCapture*>(ctx);
        ++self->records;
        self->labels.add(label);
    }
};

// ---------------------------------------------------------------------------------------------
// The scenario
// ---------------------------------------------------------------------------------------------
struct Expected
{
    std::map<TrackId, PluginTrackChain> chainsBeforeSave;
    DelaySettings instrumentDelay{ 250.0f, 50.0f, 0.0f };
    DelaySettings audioPreDelay{ 125.0f, 50.0f, 0.0f };
    DelaySettings groupDelay{ 375.0f, 40.0f, 0.0f };
    DelaySettings masterDelay{ 500.0f, 30.0f, 0.0f };
    juce::String otherChangedParam;
    ImpulseResponse instrumentIrBeforeSave;
    ImpulseResponse audioIrBeforeSave;
};

void checkChainsMatch(const juce::String& context,
                      PluginInsertHost& host,
                      const TrackIds& ids,
                      const Expected& ex)
{
    for (const auto& [tid, before] : ex.chainsBeforeSave)
    {
        const PluginTrackChain after = host.exportChain(tid);
        const juce::String who = context + " track " + juce::String((juce::int64)tid);
        expect(after.slots.size() == before.slots.size(),
               who + ": same number of inserts (" + juce::String((int)after.slots.size()) + ")");
        const size_t n = std::min(after.slots.size(), before.slots.size());
        for (size_t i = 0; i < n; ++i)
        {
            const auto& a = after.slots[i];
            const auto& b = before.slots[i];
            expect(a.slotId == b.slotId && a.stage == b.stage,
                   who + " slot " + juce::String((int)i) + ": same slot id + stage (" + stageName(a.stage) + ")");
            expect(a.vst3AbsolutePath == b.vst3AbsolutePath && a.pluginIdentifier == b.pluginIdentifier,
                   who + " slot " + juce::String((int)i) + ": same plugin identity + locate path");
            expect(a.opaqueState == b.opaqueState,
                   who + " slot " + juce::String((int)i) + ": restored state is byte-identical to the state saved ("
                       + juce::String((juce::int64)a.opaqueState.getSize()) + " bytes)");
        }
    }
    juce::ignoreUnused(ids);
}

void checkDelayAudio(const juce::String& context,
                     PluginInsertHost& host,
                     const TrackId tid,
                     const DelaySettings& s,
                     const ImpulseResponse* reference)
{
    const ImpulseResponse ir = captureImpulseResponse(host, tid, 1.5, 1.0);
    const int expectedEcho = static_cast<int>(std::lround(s.delayMs / 1000.0 * kRate));
    info(context + ": echo at " + juce::String(ir.echoPeakIndex) + " samples (" + juce::String(ir.echoPeakIndex / kRate * 1000.0, 1)
         + " ms), expected " + juce::String(expectedEcho) + " (" + juce::String(s.delayMs, 1) + " ms); echo peak="
         + juce::String(ir.echoPeak, 4) + " dry=" + juce::String(ir.left.empty() ? 0.0f : ir.left[0], 4));
    expect(std::abs(ir.echoPeakIndex - expectedEcho) <= static_cast<int>(0.003 * kRate),
           context + ": the echo lands at the saved delay time (+/- 3 ms)");
    expect(ir.echoPeak > 0.05f, context + ": the echo is audible (mix restored, not 0 %)");
    if (reference != nullptr)
    {
        const double d = maxAbsDifference(ir.left, reference->left);
        info(context + ": max |restored - before save| = " + juce::String(d, 7));
        expect(d < 1.0e-4, context + ": restored instance renders the same audio as before save");
    }
}

int run(const juce::File& delayBundle, const juce::File& otherBundle, const bool keepWork)
{
    const juce::File work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-insert-persistence-tests");
    (void)work.deleteRecursively();
    (void)work.createDirectory();
    const juce::File projDir = work.getChildFile("proj");
    (void)projDir.createDirectory();
    const juce::File wav = writeToneWav(projDir, 1.0);
    const juce::File proj = projDir.getChildFile("inserts.dalproj");
    const juce::File projSaveAs = projDir.getChildFile("inserts-save-as.dalproj");

    expect(delayBundle.exists(), "DAL Mono Delay bundle present: " + delayBundle.getFullPathName());
    expect(otherBundle.exists(), "other VST3 effect present: " + otherBundle.getFullPathName());
    if (!delayBundle.exists() || !otherBundle.exists())
    {
        return 1;
    }

    Expected ex;
    TrackIds ids;

    // ---- 1. Build the session + chains the way the app does, save (Save + Save As) ----------
    {
        Session session;
        Transport transport;
        PluginInsertHost host;
        host.prepareForDevice(kRate, kBlock, 2);
        UndoCapture undo;
        host.setUndoRecorder(&undo, &UndoCapture::record);

        ids.audio = session.getActiveTrackId();
        expect(session.addRecordedTakeAtSample(wav, kRate, 0, ids.audio, static_cast<std::int64_t>(kRate)).wasOk(),
               "fixture: audio take added");
        const auto instId = session.appendExperimentalInstrumentShellTrack("Organ");
        expect(instId.has_value(), "fixture: instrument track added");
        session.addGroupTrack();
        ids = findTrackIds(session);
        expect(ids.audio != kInvalidTrackId && ids.instrument != kInvalidTrackId && ids.group != kInvalidTrackId
                   && ids.master != kInvalidTrackId,
               "fixture: audio / instrument / group / master rows exist (ids " + juce::String((juce::int64)ids.audio) + "/"
                   + juce::String((juce::int64)ids.instrument) + "/" + juce::String((juce::int64)ids.group) + "/"
                   + juce::String((juce::int64)ids.master) + ")");

        // The user's case: DAL Mono Delay as a POST insert on the instrument track.
        expect(host.addInsertFromVst3File(ids.instrument, InsertStage::Post, delayBundle).wasOk(),
               "add: DAL Mono Delay as Post insert on the INSTRUMENT track");
        // Audio track: Mono Delay Pre + the other effect Post (chain with another vendor's plugin).
        expect(host.addInsertFromVst3File(ids.audio, InsertStage::Pre, delayBundle).wasOk(),
               "add: DAL Mono Delay as Pre insert on the AUDIO track");
        expect(host.addInsertFromVst3File(ids.audio, InsertStage::Post, otherBundle).wasOk(),
               "add: other VST3 effect as Post insert on the AUDIO track");
        // Group and Master: one Mono Delay Post each (third and fourth instance).
        expect(host.addInsertFromVst3File(ids.group, InsertStage::Post, delayBundle).wasOk(),
               "add: DAL Mono Delay as Post insert on the GROUP track");
        expect(host.addInsertFromVst3File(ids.master, InsertStage::Post, delayBundle).wasOk(),
               "add: DAL Mono Delay as Post insert on the MASTER track");
        expect(undo.records == 5, "add: every insert add recorded one plugin undo step (=> project marked dirty in the app)");

        // Distinct non-default settings per instance, applied through the production restore path.
        const auto applyState = [&](const TrackId tid, const int slotIndex, const juce::MemoryBlock& blob) {
            PluginTrackChain c = host.exportChain(tid);
            if (slotIndex < 0 || slotIndex >= (int)c.slots.size() || blob.getSize() == 0)
            {
                return false;
            }
            c.slots[(size_t)slotIndex].opaqueState = blob;
            host.importChain(tid, c);
            return true;
        };
        expect(applyState(ids.instrument, 0, makeDelayStateBlob(delayBundle, ex.instrumentDelay)),
               "settings: instrument delay 250 ms / mix 50 %");
        expect(applyState(ids.audio, 0, makeDelayStateBlob(delayBundle, ex.audioPreDelay)),
               "settings: audio Pre delay 125 ms / mix 50 %");
        expect(applyState(ids.audio, 1, makeOtherStateBlob(otherBundle, ex.otherChangedParam)),
               "settings: other effect parameter \"" + ex.otherChangedParam + "\" moved off default");
        expect(applyState(ids.group, 0, makeDelayStateBlob(delayBundle, ex.groupDelay)),
               "settings: group delay 375 ms / mix 40 %");
        expect(applyState(ids.master, 0, makeDelayStateBlob(delayBundle, ex.masterDelay)),
               "settings: master delay 500 ms / mix 30 %");

        for (const TrackId tid : { ids.audio, ids.instrument, ids.group, ids.master })
        {
            ex.chainsBeforeSave[tid] = host.exportChain(tid);
            info("before save: track " + juce::String((juce::int64)tid) + " rows " + describeRows(host.getInsertRowsForTrack(tid)));
        }
        expect(ex.chainsBeforeSave[ids.audio].slots.size() == 2 && ex.chainsBeforeSave[ids.audio].slots[0].stage == InsertStage::Pre
                   && ex.chainsBeforeSave[ids.audio].slots[1].stage == InsertStage::Post,
               "before save: audio chain is [Pre: Mono Delay][Post: other] in order");
        const auto& inst0 = ex.chainsBeforeSave[ids.instrument].slots;
        const auto& grp0 = ex.chainsBeforeSave[ids.group].slots;
        expect(inst0.size() == 1 && grp0.size() == 1 && inst0[0].opaqueState != grp0[0].opaqueState,
               "before save: two Mono Delay instances carry DIFFERENT states (separate state per instance)");

        checkDelayAudio("before save instrument", host, ids.instrument, ex.instrumentDelay, nullptr);
        ex.instrumentIrBeforeSave = captureImpulseResponse(host, ids.instrument, 0.5, 1.0);
        ex.audioIrBeforeSave = captureImpulseResponse(host, ids.audio, 1.5, 1.0);

        expect(session.saveProjectToFile(transport, proj, kRate, &host).wasOk(), "save: project saved (Save)");
        expect(session.saveProjectToFile(transport, projSaveAs, kRate, &host).wasOk(), "save: project saved (Save As)");
        host.removeAllPlugins();
    }

    // ---- 2. What the file contains -------------------------------------------------------------
    {
        const auto byTrack = readInsertsByTrack(proj);
        const auto rowsFor = [&](const TrackId t) { const auto it = byTrack.find((juce::int64)t); return it == byTrack.end() ? std::vector<JsonInsert>{} : it->second; };
        expect(rowsFor(ids.instrument).size() == 1 && rowsFor(ids.instrument)[0].stage == "post",
               "file: instrument row carries its Post insert");
        expect(rowsFor(ids.group).size() == 1 && rowsFor(ids.master).size() == 1,
               "file: group and master rows carry their Post inserts");
        expect(rowsFor(ids.audio).size() == 2 && rowsFor(ids.audio)[0].stage == "pre" && rowsFor(ids.audio)[1].stage == "post",
               "file: audio row carries [pre][post] in chain order");
        bool allHaveIdentityAndState = !byTrack.empty();
        for (const auto& [tid, rows] : byTrack)
        {
            for (const auto& r : rows)
            {
                allHaveIdentityAndState = allHaveIdentityAndState && r.path.isNotEmpty() && r.identifier.isNotEmpty()
                                          && r.stateBase64.isNotEmpty();
            }
        }
        expect(allHaveIdentityAndState, "file: every insert row has pluginVst3Path + pluginIdentifier + pluginStateBase64");
        expect(proj.loadFileAsString() == projSaveAs.loadFileAsString()
                   || readInsertsByTrack(projSaveAs).size() == byTrack.size(),
               "file: Save As wrote the same insert rows");
    }

    // ---- 3. Reload (Save) and (Save As) through the production loader -----------------------------
    for (const juce::File& f : { proj, projSaveAs })
    {
        Session session;
        Transport transport;
        PluginInsertHost host;
        host.prepareForDevice(kRate, kBlock, 2);
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, f, kRate, skipped, note, &host).wasOk(),
               "reload (" + f.getFileName() + "): project loaded");
        pumpMessageLoop(400); // the insert restore is deferred to the message loop exactly like in the app
        for (const auto& s : skipped)
        {
            info("reload note: " + s);
        }
        const TrackIds ids2 = findTrackIds(session);
        expect(ids2.audio == ids.audio && ids2.instrument == ids.instrument && ids2.group == ids.group && ids2.master == ids.master,
               "reload (" + f.getFileName() + "): track ids stable");
        for (const TrackId tid : { ids.audio, ids.instrument, ids.group, ids.master })
        {
            info("after reload: track " + juce::String((juce::int64)tid) + " rows " + describeRows(host.getInsertRowsForTrack(tid)));
        }
        checkChainsMatch("reload (" + f.getFileName() + ")", host, ids, ex);
        bool noneUnavailable = true;
        for (const TrackId tid : { ids.audio, ids.instrument, ids.group, ids.master })
        {
            for (const auto& r : host.getInsertRowsForTrack(tid))
            {
                noneUnavailable = noneUnavailable && !r.unavailable;
            }
            expect(host.audioThread_hasActivePluginForTrack(tid),
                   "reload (" + f.getFileName() + "): track " + juce::String((juce::int64)tid) + " has an active (stereo-ready) insert");
        }
        expect(noneUnavailable, "reload (" + f.getFileName() + "): no insert is marked unavailable");
        if (f == proj)
        {
            checkDelayAudio("after reload instrument", host, ids.instrument, ex.instrumentDelay, &ex.instrumentIrBeforeSave);
            checkDelayAudio("after reload group", host, ids.group, ex.groupDelay, nullptr);
            checkDelayAudio("after reload master", host, ids.master, ex.masterDelay, nullptr);
            const ImpulseResponse audioIr = captureImpulseResponse(host, ids.audio, 1.5, 1.0);
            const double d = maxAbsDifference(audioIr.left, ex.audioIrBeforeSave.left);
            info("after reload audio chain (delay Pre + other Post): max |restored - before save| = " + juce::String(d, 7));
            expect(d < 1.0e-4, "after reload: the two-plugin audio chain renders the same audio as before save");
        }
        host.removeAllPlugins();
    }

    // ---- 4. Temporarily unavailable plugin: config preserved across reload + save -----------------
    const juce::File projMissing = projDir.getChildFile("inserts-missing-plugin.dalproj");
    const juce::File projMissingResaved = projDir.getChildFile("inserts-missing-plugin-resaved.dalproj");
    const juce::String fakePath = "C:\\Program Files\\Common Files\\VST3\\__DAL_not_installed_here__\\DALMonoDelay.vst3";
    {
        const auto byTrack = readInsertsByTrack(proj);
        const juce::int64 slot = byTrack.at((juce::int64)ids.instrument)[0].slotId;
        expect(rewriteInsertPath(proj, projMissing, (juce::int64)ids.instrument, slot, fakePath),
               "unavailable: fixture rewritten so the instrument insert points at a non-existent bundle (user plugins untouched)");

        Session session;
        Transport transport;
        PluginInsertHost host;
        host.prepareForDevice(kRate, kBlock, 2);
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, projMissing, kRate, skipped, note, &host).wasOk(),
               "unavailable: project loads");
        pumpMessageLoop(400);
        const auto rows = host.getInsertRowsForTrack(ids.instrument);
        info("unavailable: instrument rows " + describeRows(rows));
        expect(rows.size() == 1 && rows[0].unavailable && rows[0].stage == InsertStage::Post,
               "unavailable: the insert is still listed, in its Post position, marked unavailable");
        expect(rows.size() == 1 && rows[0].displayName.contains("DAL Mono Delay") && rows[0].displayName.contains("(unavailable)"),
               "unavailable: shown as \"DAL Mono Delay (unavailable)\" (name from the saved identity)");
        expect(!host.audioThread_hasActivePluginForTrack(ids.instrument),
               "unavailable: nothing is published to the audio thread for that slot (silent, no crash)");
        expect(host.hasAnyInsertOnTrack(ids.instrument), "unavailable: the track still reports an insert (Inspector / delete-undo capture)");
        const PluginTrackChain chain = host.exportChain(ids.instrument);
        const auto& saved = ex.chainsBeforeSave[ids.instrument].slots[0];
        expect(chain.slots.size() == 1 && chain.slots[0].pluginIdentifier == saved.pluginIdentifier
                   && chain.slots[0].opaqueState == saved.opaqueState && chain.slots[0].vst3AbsolutePath == fakePath,
               "unavailable: exportChain re-emits identity + state byte-for-byte (path as saved)");
        // The other tracks (plugin available) restored normally alongside the placeholder.
        expect(host.exportChain(ids.group).slots.size() == 1 && host.audioThread_hasActivePluginForTrack(ids.group),
               "unavailable: other tracks' inserts restored live as usual");

        expect(session.saveProjectToFile(transport, projMissingResaved, kRate, &host).wasOk(),
               "unavailable: project saved again while the plugin is missing");
        const auto resaved = readInsertsByTrack(projMissingResaved);
        const auto original = readInsertsByTrack(projMissing);
        const auto& r0 = resaved.at((juce::int64)ids.instrument);
        const auto& o0 = original.at((juce::int64)ids.instrument);
        expect(r0.size() == 1 && o0.size() == 1 && r0[0].slotId == o0[0].slotId && r0[0].stage == o0[0].stage
                   && r0[0].path == o0[0].path && r0[0].identifier == o0[0].identifier && r0[0].stateBase64 == o0[0].stateBase64,
               "unavailable: the re-saved file keeps slot / stage / path / identifier / state EXACTLY (no data loss)");
        host.removeAllPlugins();
    }

    // ---- 5. The plugin "comes back": re-point the path at the real bundle, reload --------------------
    {
        const juce::File projReturned = projDir.getChildFile("inserts-plugin-returned.dalproj");
        const auto byTrack = readInsertsByTrack(projMissingResaved);
        const juce::int64 slot = byTrack.at((juce::int64)ids.instrument)[0].slotId;
        expect(rewriteInsertPath(projMissingResaved, projReturned, (juce::int64)ids.instrument, slot, delayBundle.getFullPathName()),
               "returned: fixture re-pointed at the installed bundle");
        Session session;
        Transport transport;
        PluginInsertHost host;
        host.prepareForDevice(kRate, kBlock, 2);
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, projReturned, kRate, skipped, note, &host).wasOk(), "returned: project loads");
        pumpMessageLoop(400);
        const auto rows = host.getInsertRowsForTrack(ids.instrument);
        expect(rows.size() == 1 && !rows[0].unavailable && rows[0].displayName == "DAL Mono Delay",
               "returned: the insert is live again");
        const PluginTrackChain chain = host.exportChain(ids.instrument);
        expect(chain.slots.size() == 1 && chain.slots[0].opaqueState == ex.chainsBeforeSave[ids.instrument].slots[0].opaqueState,
               "returned: the state that was preserved while unavailable is now applied (byte-identical)");
        checkDelayAudio("returned instrument", host, ids.instrument, ex.instrumentDelay, &ex.instrumentIrBeforeSave);
        host.removeAllPlugins();
    }

    // ---- 6. Identity mismatch: path points at a DIFFERENT plugin than the saved identity -------------
    {
        const juce::File projWrong = projDir.getChildFile("inserts-wrong-plugin-at-path.dalproj");
        const auto byTrack = readInsertsByTrack(proj);
        const juce::int64 slot = byTrack.at((juce::int64)ids.group)[0].slotId;
        expect(rewriteInsertPath(proj, projWrong, (juce::int64)ids.group, slot, otherBundle.getFullPathName()),
               "mismatch: fixture points the group's Mono Delay row at the OTHER plugin's bundle");
        Session session;
        Transport transport;
        PluginInsertHost host;
        host.prepareForDevice(kRate, kBlock, 2);
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, projWrong, kRate, skipped, note, &host).wasOk(), "mismatch: project loads");
        pumpMessageLoop(400);
        const auto rows = host.getInsertRowsForTrack(ids.group);
        info("mismatch: group rows " + describeRows(rows));
        expect(rows.size() == 1 && rows[0].unavailable,
               "mismatch: the saved Mono Delay state is NOT applied to the other plugin — slot kept as unavailable");
        const PluginTrackChain chain = host.exportChain(ids.group);
        expect(chain.slots.size() == 1 && chain.slots[0].opaqueState == ex.chainsBeforeSave[ids.group].slots[0].opaqueState
                   && chain.slots[0].pluginIdentifier == ex.chainsBeforeSave[ids.group].slots[0].pluginIdentifier,
               "mismatch: identity + state preserved for the save path");
        host.removeAllPlugins();
    }

    // ---- 7. Explicit removal stays removed (live insert and unavailable placeholder) ------------------
    {
        const juce::File projRemoved = projDir.getChildFile("inserts-after-remove.dalproj");
        {
            Session session;
            Transport transport;
            PluginInsertHost host;
            host.prepareForDevice(kRate, kBlock, 2);
            UndoCapture undo;
            host.setUndoRecorder(&undo, &UndoCapture::record);
            juce::StringArray skipped;
            juce::String note;
            expect(session.loadProjectFromFile(transport, projMissing, kRate, skipped, note, &host).wasOk(), "removal: project loads");
            pumpMessageLoop(400);
            const auto instRows = host.getInsertRowsForTrack(ids.instrument);
            const auto masterRows = host.getInsertRowsForTrack(ids.master);
            expect(instRows.size() == 1 && masterRows.size() == 1, "removal: fixture has the placeholder + a live master insert");
            if (instRows.size() == 1)
            {
                host.removeInsert(ids.instrument, instRows[0].slotId); // user removes the unavailable placeholder
            }
            if (masterRows.size() == 1)
            {
                host.removeInsert(ids.master, masterRows[0].slotId); // user removes a live insert
            }
            expect(undo.records == 2, "removal: both removals recorded a plugin undo step (=> dirty)");
            expect(host.getInsertRowsForTrack(ids.instrument).empty() && host.getInsertRowsForTrack(ids.master).empty(),
                   "removal: both chains are empty afterwards");
            expect(session.saveProjectToFile(transport, projRemoved, kRate, &host).wasOk(), "removal: saved");
            host.removeAllPlugins();
        }
        const auto byTrack = readInsertsByTrack(projRemoved);
        expect(byTrack.at((juce::int64)ids.instrument).empty() && byTrack.at((juce::int64)ids.master).empty(),
               "removal: the file no longer carries the removed inserts (placeholder and live)");
        expect(byTrack.at((juce::int64)ids.audio).size() == 2 && byTrack.at((juce::int64)ids.group).size() == 1,
               "removal: untouched tracks keep their inserts");
        {
            Session session;
            Transport transport;
            PluginInsertHost host;
            host.prepareForDevice(kRate, kBlock, 2);
            juce::StringArray skipped;
            juce::String note;
            expect(session.loadProjectFromFile(transport, projRemoved, kRate, skipped, note, &host).wasOk(), "removal: reload");
            pumpMessageLoop(400);
            expect(host.getInsertRowsForTrack(ids.instrument).empty() && host.getInsertRowsForTrack(ids.master).empty(),
                   "removal: removed inserts do not resurrect on reload");
            host.removeAllPlugins();
        }
    }

    // ---- 8. Pre/Post move of a placeholder survives save (chain placement is user config) -------------
    {
        const juce::File projMoved = projDir.getChildFile("inserts-placeholder-moved.dalproj");
        Session session;
        Transport transport;
        PluginInsertHost host;
        host.prepareForDevice(kRate, kBlock, 2);
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, projMissing, kRate, skipped, note, &host).wasOk(), "move: project loads");
        pumpMessageLoop(400);
        const auto rows = host.getInsertRowsForTrack(ids.instrument);
        if (rows.size() == 1)
        {
            host.moveInsertToStage(ids.instrument, rows[0].slotId, InsertStage::Pre);
        }
        const auto moved = host.getInsertRowsForTrack(ids.instrument);
        expect(moved.size() == 1 && moved[0].stage == InsertStage::Pre && moved[0].unavailable,
               "move: an unavailable placeholder can be moved Post -> Pre");
        expect(session.saveProjectToFile(transport, projMoved, kRate, &host).wasOk(), "move: saved");
        const auto byTrack = readInsertsByTrack(projMoved);
        const auto& r = byTrack.at((juce::int64)ids.instrument);
        expect(r.size() == 1 && r[0].stage == "pre" && r[0].stateBase64 == readInsertsByTrack(projMissing).at((juce::int64)ids.instrument)[0].stateBase64,
               "move: file carries the new stage with the untouched state");
        host.removeAllPlugins();
    }

    if (!keepWork)
    {
        (void)work.deleteRecursively();
    }
    else
    {
        info("work folder kept: " + work.getFullPathName());
    }
    return 0;
}
} // namespace

/// Audio-only fixture for `MiniDAWLab.exe --stability-inserts <dir>\inserts-fixture.dalproj`: one
/// audio row with a 1 s tone take (the in-app scenario adds the instrument shell and the inserts).
int makeInsertsFixture(const juce::File& dir)
{
    (void)dir.deleteRecursively();
    if (!dir.createDirectory())
    {
        std::printf("could not create %s\n", dir.getFullPathName().toRawUTF8());
        return 1;
    }
    const juce::File wav = writeToneWav(dir, 1.0);
    Session session;
    Transport transport;
    const TrackId tid = session.getActiveTrackId();
    if (session.addRecordedTakeAtSample(wav, kRate, 0, tid, static_cast<std::int64_t>(kRate)).failed())
    {
        std::printf("fixture: add take failed\n");
        return 1;
    }
    const juce::File proj = dir.getChildFile("inserts-fixture.dalproj");
    const juce::Result saved = session.saveProjectToFile(transport, proj, kRate);
    if (saved.failed())
    {
        std::printf("fixture: save failed: %s\n", saved.getErrorMessage().toRawUTF8());
        return 1;
    }
    std::printf("%s\n", proj.getFullPathName().toRawUTF8());
    return 0;
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;

    juce::File delayBundle("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");
    juce::File otherBundle("C:\\Program Files\\Common Files\\VST3\\BassEvening.vst3");
    bool keep = false;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a(argv[i]);
        const auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String(argv[++i]) : juce::String{}; };
        if (a == "--make-fixture")
        {
            return makeInsertsFixture(juce::File(next()));
        }
        if (a == "--delay-vst3") { delayBundle = juce::File(next()); }
        else if (a == "--other-vst3") { otherBundle = juce::File(next()); }
        else if (a == "--keep") { keep = true; }
    }

    const int rc = run(delayBundle, otherBundle, keep);
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return (rc == 0 && failures == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — Session.cpp references these instrument entry points; this harness creates an
// instrument SHELL row without any instrument controller/host, so the stubs are never executed.
// ---------------------------------------------------------------------------
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
