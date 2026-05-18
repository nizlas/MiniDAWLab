// =============================================================================
// ProjectFile.cpp — strict JSON v1; parse failure does not touch Session (caller)
// =============================================================================

#include "io/ProjectFile.h"

#include "domain/ProjectMusicalTime.h"
#include "domain/Track.h"
#include "domain/TrackStereoPan.h"
#include "ui/SnapSettings.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace
{
    [[nodiscard]] juce::var trackToVar(const ProjectFileTrackV1& t, const int fileVersion)
    {
        juce::Array<juce::var> clipVars;
        for (const auto& c : t.clips)
        {
            juce::DynamicObject::Ptr co = new juce::DynamicObject();
            co->setProperty("id", static_cast<std::int64_t>(c.id));
            co->setProperty("startSample", c.startSample);
            co->setProperty("sourcePath", c.sourcePath);
            if (c.visibleLengthSamples > 0)
            {
                co->setProperty("visibleLengthSamples", static_cast<std::int64_t>(c.visibleLengthSamples));
            }
            if (c.leftTrimSamples > 0)
            {
                co->setProperty("leftTrimSamples", static_cast<std::int64_t>(c.leftTrimSamples));
            }
            if (c.hasMaterialWindowInFile)
            {
                co->setProperty("materialWindowStartSamples", static_cast<std::int64_t>(c.materialWindowStartSamples));
                co->setProperty(
                    "materialWindowEndExclusiveSamples",
                    static_cast<std::int64_t>(c.materialWindowEndExclusiveSamples));
            }
            clipVars.add(juce::var(co.get()));
        }
        juce::DynamicObject::Ptr to = new juce::DynamicObject();
        to->setProperty("id", static_cast<std::int64_t>(t.id));
        to->setProperty("name", t.name);
        if (fileVersion >= 13)
        {
            const juce::String k = t.kind.isNotEmpty() ? t.kind : juce::String("audio");
            to->setProperty("kind", k);
        }
        to->setProperty("clips", juce::var(clipVars));
        constexpr double kUnityChannelFaderOmitEpsilon = 1.0e-6;
        if (std::fabs((double)t.channelFaderGain - 1.0) > kUnityChannelFaderOmitEpsilon)
        {
            to->setProperty("channelFaderGain", (double)t.channelFaderGain);
        }
        constexpr double kPanOmitEpsilon = 1.0e-6;
        if (std::fabs((double)t.stereoPan) > kPanOmitEpsilon)
        {
            to->setProperty("pan", (double)t.stereoPan);
        }
        if (t.off)
        {
            to->setProperty("off", true);
        }
        if (t.muted)
        {
            to->setProperty("muted", true);
        }
        if (fileVersion >= 14 && !t.kind.equalsIgnoreCase("master") && t.routedOutputTrackId != kInvalidTrackId)
        {
            juce::DynamicObject::Ptr outObj = new juce::DynamicObject();
            outObj->setProperty("trackId", static_cast<std::int64_t>(t.routedOutputTrackId));
            to->setProperty("output", juce::var(outObj.get()));
        }
        if (fileVersion >= 9)
        {
            if (!t.inserts.empty())
            {
                juce::Array<juce::var> insertVars;
                for (const auto& in : t.inserts)
                {
                    juce::DynamicObject::Ptr io = new juce::DynamicObject();
                    io->setProperty("slotId", static_cast<std::int64_t>(in.slotId));
                    io->setProperty("stage", in.stage == InsertStage::Pre ? "pre" : "post");
                    io->setProperty("pluginVst3Path", in.pluginVst3Path);
                    if (in.pluginIdentifier.isNotEmpty())
                    {
                        io->setProperty("pluginIdentifier", in.pluginIdentifier);
                    }
                    if (in.pluginStateBase64.isNotEmpty())
                    {
                        io->setProperty("pluginStateBase64", in.pluginStateBase64);
                    }
                    insertVars.add(juce::var(io.get()));
                }
                to->setProperty("inserts", juce::var(insertVars));
            }
        }
        else if (t.pluginVst3Path.isNotEmpty())
        {
            to->setProperty("pluginVst3Path", t.pluginVst3Path);
            if (t.pluginIdentifier.isNotEmpty())
            {
                to->setProperty("pluginIdentifier", t.pluginIdentifier);
            }
            if (t.pluginStateBase64.isNotEmpty())
            {
                to->setProperty("pluginStateBase64", t.pluginStateBase64);
            }
        }
        return juce::var(to.get());
    }

    [[nodiscard]] std::int64_t int64FromVarId(const juce::var& v, bool& ok) noexcept
    {
        ok = false;
        if (v.isInt64() || v.isInt())
        {
            ok = true;
            return static_cast<std::int64_t>(v);
        }
        if (v.isString())
        {
            const std::int64_t n = v.toString().getLargeIntValue();
            ok = true;
            return n;
        }
        if (v.isDouble())
        {
            ok = true;
            return static_cast<std::int64_t>((double)v);
        }
        return 0;
    }

    [[nodiscard]] juce::Result parseTrackInsertsFromVar(const juce::var& tv,
                                                      ProjectFileTrackV1& trk,
                                                      juce::String& err,
                                                      const int fileVersion)
    {
        if (fileVersion < 9)
        {
            return juce::Result::ok();
        }
        const juce::var& insVar = tv.getProperty("inserts", {});
        if (!insVar.isArray())
        {
            return juce::Result::ok();
        }
        const juce::Array<juce::var>* insArr = insVar.getArray();
        if (insArr == nullptr)
        {
            return juce::Result::ok();
        }
        std::unordered_set<InsertSlotId> seen;
        for (const juce::var& iv : *insArr)
        {
            if (!iv.isObject())
            {
                err = "Each insert must be a JSON object.";
                return juce::Result::fail(err);
            }
            bool sidOk = false;
            const std::int64_t sid64 = int64FromVarId(iv.getProperty("slotId", {}), sidOk);
            if (!sidOk || sid64 <= 0)
            {
                err = "Insert missing valid slotId.";
                return juce::Result::fail(err);
            }
            const InsertSlotId sid = static_cast<InsertSlotId>(sid64);
            if (!seen.insert(sid).second)
            {
                err = "Duplicate insert slotId within a track.";
                return juce::Result::fail(err);
            }
            ProjectFileInsertV1 ins;
            ins.slotId = sid;
            const juce::String st = iv.getProperty("stage", {}).toString().toLowerCase();
            if (st == "pre")
            {
                ins.stage = InsertStage::Pre;
            }
            else if (st == "post")
            {
                ins.stage = InsertStage::Post;
            }
            else
            {
                err = "Insert has invalid stage (expected \"pre\" or \"post\").";
                return juce::Result::fail(err);
            }
            ins.pluginVst3Path = iv.getProperty("pluginVst3Path", {}).toString();
            if (ins.pluginVst3Path.isEmpty())
            {
                err = "Insert missing pluginVst3Path.";
                return juce::Result::fail(err);
            }
            ins.pluginIdentifier = iv.getProperty("pluginIdentifier", {}).toString();
            ins.pluginStateBase64 = iv.getProperty("pluginStateBase64", {}).toString();
            trk.inserts.push_back(std::move(ins));
        }
        return juce::Result::ok();
    }

    void migrateLegacySinglePluginToInserts(ProjectFileTrackV1& trk)
    {
        if (!trk.inserts.empty() || trk.pluginVst3Path.isEmpty())
        {
            return;
        }
        ProjectFileInsertV1 mig;
        mig.slotId = 1;
        mig.stage = InsertStage::Post;
        mig.pluginVst3Path = trk.pluginVst3Path;
        mig.pluginIdentifier = trk.pluginIdentifier;
        mig.pluginStateBase64 = trk.pluginStateBase64;
        trk.inserts.push_back(std::move(mig));
    }

    /// Reads **\< v13** payloads that kept the experimental instrument “below” tracks only in the
    /// `experimentalInstrumentTracks` blob. We append one **Instrument** lane to `tracks` and bind `trackId`
    /// before the timeline is built (`Session::loadProjectFromFile`), while remaining on disk format v12.
    /// v13 saves carry real mixed order explicitly.
    /// All versions: dedupe `kind` = `"master"` (keep canonical row; demote others). Append when missing.
    void migrateProjectFileMasterTrackPreV14(ProjectFileV1& out) noexcept
    {
        auto isInstrumentLaneId = [&out](const TrackId id) noexcept -> bool {
            for (const auto& et : out.experimentalInstrumentTracks)
            {
                if (et.trackId == id)
                {
                    return true;
                }
            }
            return false;
        };

        std::vector<int> masterIndices;
        masterIndices.reserve(2U);
        for (int i = 0; i < (int)out.tracks.size(); ++i)
        {
            if (out.tracks[(size_t)i].kind.equalsIgnoreCase("master"))
            {
                masterIndices.push_back(i);
            }
        }

        if (masterIndices.size() == 1U)
        {
            auto& sole = out.tracks[(size_t)masterIndices[0U]];
            if (isInstrumentLaneId(sole.id))
            {
                sole.kind = "instrument";
                juce::Logger::writeToLog(
                    "[ProjectFile] load: demoted mis-tagged instrument master track id="
                    + juce::String((juce::int64)sole.id));
                masterIndices.clear();
            }
        }

        if (!masterIndices.empty())
        {
            int keep = masterIndices.back();
            for (const int mi : masterIndices)
            {
                if (out.tracks[(size_t)mi].name.equalsIgnoreCase(kMasterTrackDisplayName))
                {
                    keep = mi;
                    break;
                }
            }

            for (const int mi : masterIndices)
            {
                if (mi == keep)
                {
                    continue;
                }
                auto& tr = out.tracks[(size_t)mi];
                tr.kind = isInstrumentLaneId(tr.id) ? juce::String("instrument") : juce::String("audio");
                juce::Logger::writeToLog(
                    "[ProjectFile] load: demoted duplicate master track id="
                    + juce::String((juce::int64)tr.id) + " to kind=" + tr.kind);
            }

            out.tracks[(size_t)keep].name = kMasterTrackDisplayName;
            out.tracks[(size_t)keep].kind = "master";
            out.tracks[(size_t)keep].off = false;
        }

        for (const auto& tr : out.tracks)
        {
            if (tr.kind.equalsIgnoreCase("master"))
            {
                return;
            }
        }

        TrackId maxId = 0;
        for (const auto& tr : out.tracks)
        {
            maxId = juce::jmax(maxId, tr.id);
        }
        const TrackId masterId = juce::jmax(out.nextTrackId, maxId + 1);

        ProjectFileTrackV1 master;
        master.id = masterId;
        master.name = kMasterTrackDisplayName;
        master.kind = "master";
        master.channelFaderGain = kTrackChannelVolumeUnityGain;
        out.tracks.push_back(std::move(master));
        out.nextTrackId = juce::jmax(out.nextTrackId, masterId + 1);

        juce::Logger::writeToLog(
            "[ProjectFile] master migration: appended Stereo Out track id="
            + juce::String((juce::int64)masterId)
            + " (project version " + juce::String(out.version) + ")");
    }

    void migrateProjectFileExperimentalInstrumentLanePreV13(ProjectFileV1& out) noexcept
    {
        if (out.version >= 13)
        {
            return;
        }
        TrackId maxExisting = 0;
        for (const auto& tr : out.tracks)
        {
            maxExisting = juce::jmax(maxExisting, tr.id);
        }

        bool primaryBound = false;
        for (auto& et : out.experimentalInstrumentTracks)
        {
            if (!et.enabled || et.instrumentKind != "GrooveAgentSE")
            {
                continue;
            }

            if (primaryBound)
            {
                if (et.trackId == 0 || et.trackId == kInvalidTrackId)
                {
                    juce::Logger::writeToLog(
                        "[ProjectFile] v12 migration: ignoring extra experimentalInstrumentTracks "
                        "(single-instrument slice).");
                }
                continue;
            }

            TrackId nid = et.trackId;
            if (nid == 0 || nid == kInvalidTrackId)
            {
                nid = juce::jmax(out.nextTrackId, maxExisting + 1);
                maxExisting = nid;

                ProjectFileTrackV1 shell;
                shell.id = nid;
                shell.name = et.name.isNotEmpty() ? et.name : juce::String("Groove Agent SE");
                shell.kind = "instrument";
                shell.off = !et.powerOn;
                shell.muted = et.muted;
                shell.channelFaderGain = kTrackChannelVolumeUnityGain;

                out.tracks.push_back(std::move(shell));
                out.nextTrackId = juce::jmax(out.nextTrackId, nid + 1);

                juce::Logger::writeToLog(
                    "[ProjectFile] v12 migration: appended Instrument lane id="
                    + juce::String((juce::int64)nid));
            }

            et.trackId = nid;
            primaryBound = true;
        }
    }

    [[nodiscard]] juce::Result clipFromVar(
        const juce::var& v,
        ProjectFileClipV1& out,
        juce::String& err,
        const int fileVersion)
    {
        if (!v.isObject())
        {
            err = "Each clip must be a JSON object.";
            return juce::Result::fail(err);
        }
        bool idOk = false;
        const std::int64_t idv = int64FromVarId(v.getProperty("id", {}), idOk);
        if (!idOk || idv <= 0)
        {
            err = "Invalid or missing placed clip id.";
            return juce::Result::fail(err);
        }
        out.id = static_cast<PlacedClipId>(idv);

        const juce::var& ss = v.getProperty("startSample", {});
        if (!(ss.isInt64() || ss.isInt() || ss.isDouble()))
        {
            err = "Clip missing valid startSample.";
            return juce::Result::fail(err);
        }
        out.startSample = static_cast<std::int64_t>(static_cast<double>(ss));
        out.sourcePath = v.getProperty("sourcePath", {}).toString();
        if (out.sourcePath.isEmpty())
        {
            err = "Clip missing sourcePath.";
            return juce::Result::fail(err);
        }
        out.visibleLengthSamples = 0;
        const juce::var& vlen = v.getProperty("visibleLengthSamples", {});
        if (vlen.isInt64() || vlen.isInt() || vlen.isDouble())
        {
            out.visibleLengthSamples
                = static_cast<std::int64_t>(static_cast<double>(vlen));
        }
        out.leftTrimSamples = 0;
        const juce::var& ltr = v.getProperty("leftTrimSamples", {});
        if (ltr.isInt64() || ltr.isInt() || ltr.isDouble())
        {
            out.leftTrimSamples = static_cast<std::int64_t>(static_cast<double>(ltr));
        }
        out.materialWindowStartSamples = 0;
        out.materialWindowEndExclusiveSamples = 0;
        out.hasMaterialWindowInFile = false;
        if (fileVersion >= 7)
        {
            const juce::var& mws = v.getProperty("materialWindowStartSamples", {});
            const juce::var& mwe = v.getProperty("materialWindowEndExclusiveSamples", {});
            if ((mws.isInt64() || mws.isInt() || mws.isDouble())
                && (mwe.isInt64() || mwe.isInt() || mwe.isDouble()))
            {
                out.materialWindowStartSamples = static_cast<std::int64_t>(static_cast<double>(mws));
                out.materialWindowEndExclusiveSamples
                    = static_cast<std::int64_t>(static_cast<double>(mwe));
                out.hasMaterialWindowInFile = true;
            }
        }
        return juce::Result::ok();
    }

    [[nodiscard]] juce::Result parseExperimentalInstrumentTracksObject(
        const juce::var& root,
        ProjectFileV1& out,
        const int fileVersion)
    {
        if (fileVersion < 11)
        {
            return juce::Result::ok();
        }
        const juce::var& ex = root.getProperty("experimentalInstrumentTracks", {});
        if (!ex.isArray())
        {
            return juce::Result::ok();
        }
        const juce::Array<juce::var>* arr = ex.getArray();
        if (arr == nullptr)
        {
            return juce::Result::ok();
        }
        for (const juce::var& tv : *arr)
        {
            if (!tv.isObject())
            {
                continue;
            }
            ProjectFileExperimentalInstrumentTrackV1 et;
            const juce::var& ev = tv.getProperty("enabled", {});
            if (ev.isBool())
            {
                et.enabled = (bool)ev;
            }
            else if (ev.isInt() || ev.isInt64() || ev.isDouble())
            {
                et.enabled = static_cast<int>(static_cast<double>(ev) + 0.5) != 0;
            }
            et.instrumentKind = tv.getProperty("instrumentKind", {}).toString();
            if (et.instrumentKind.isEmpty())
            {
                et.instrumentKind = "GrooveAgentSE";
            }
            et.name = tv.getProperty("name", {}).toString();
            if (et.name.isEmpty() && et.instrumentKind == "GrooveAgentSE")
            {
                et.name = "Groove Agent SE";
            }
            et.requiredKitName = tv.getProperty("requiredKitName", {}).toString();
            if (et.requiredKitName.isEmpty() && et.instrumentKind == "GrooveAgentSE")
            {
                et.requiredKitName = "FiftySixDegreesModified";
            }
            et.pluginBundlePath = tv.getProperty("pluginBundlePath", {}).toString();
            et.pluginStateBase64 = tv.getProperty("pluginStateBase64", {}).toString();
            const juce::var& pwl = tv.getProperty("pluginWasLoadedOnSave", {});
            if (pwl.isBool())
            {
                et.pluginWasLoadedOnSave = (bool)pwl;
            }
            else if (pwl.isInt() || pwl.isInt64() || pwl.isDouble())
            {
                et.pluginWasLoadedOnSave = static_cast<int>(static_cast<double>(pwl) + 0.5) != 0;
            }
            et.powerOn = true;
            const juce::var& po = tv.getProperty("powerOn", {});
            if (po.isBool())
            {
                et.powerOn = (bool)po;
            }
            else if (po.isInt() || po.isInt64() || po.isDouble())
            {
                et.powerOn = static_cast<int>(static_cast<double>(po) + 0.5) != 0;
            }
            et.muted = false;
            const juce::var& mu = tv.getProperty("muted", {});
            if (mu.isBool())
            {
                et.muted = (bool)mu;
            }
            else if (mu.isInt() || mu.isInt64() || mu.isDouble())
            {
                et.muted = static_cast<int>(static_cast<double>(mu) + 0.5) != 0;
            }
            if (fileVersion >= 13)
            {
                bool tidOk = false;
                const std::int64_t tidRaw = int64FromVarId(tv.getProperty("trackId", {}), tidOk);
                et.trackId
                    = (tidOk && tidRaw > 0) ? static_cast<TrackId>(tidRaw) : kInvalidTrackId;
            }
            const juce::var& dnm = tv.getProperty("drumNoteNames", {});
            if (dnm.isObject())
            {
                if (const auto* dynObj = dnm.getDynamicObject())
                {
                    for (const auto& nv : dynObj->getProperties())
                    {
                        const int note = nv.name.toString().getIntValue();
                        if (note >= 0 && note <= 127)
                        {
                            const juce::String nm = nv.value.toString().trim();
                            if (nm.isNotEmpty())
                            {
                                et.drumNoteNameOverrides.push_back({note, nm});
                            }
                        }
                    }
                }
            }
            const juce::var& dna = tv.getProperty("drumNoteNamesAutoPlugin", {});
            if (dna.isObject())
            {
                if (const auto* dynObjA = dna.getDynamicObject())
                {
                    for (const auto& nv : dynObjA->getProperties())
                    {
                        const int note = nv.name.toString().getIntValue();
                        if (note >= 0 && note <= 127)
                        {
                            const juce::String nm = nv.value.toString().trim();
                            if (nm.isNotEmpty())
                            {
                                et.drumNoteNameAutoPlugin.push_back({note, nm});
                            }
                        }
                    }
                }
            }
            const juce::var& clipsV = tv.getProperty("clips", {});
            if (clipsV.isArray())
            {
                const juce::Array<juce::var>* clipArr = clipsV.getArray();
                if (clipArr != nullptr)
                {
                    for (const juce::var& cv : *clipArr)
                    {
                        if (!cv.isObject())
                        {
                            continue;
                        }
                        ProjectFileExperimentalInstrumentClipV1 c;
                        bool idOk = false;
                        c.id = static_cast<std::uint64_t>(
                            int64FromVarId(cv.getProperty("id", {}), idOk));
                        if (!idOk || c.id == 0)
                        {
                            continue;
                        }
                        c.name = cv.getProperty("name", {}).toString();
                        if (c.name.isEmpty())
                        {
                            c.name = "MIDI 1";
                        }
                        c.numSteps = 16;
                        const juce::var& ns = cv.getProperty("numSteps", {});
                        if (ns.isInt() || ns.isInt64() || ns.isDouble())
                        {
                            c.numSteps = juce::jmax(1, (int)static_cast<double>(ns));
                        }
                        c.stepDenom = 16;
                        const juce::var& sd = cv.getProperty("stepDenom", {});
                        if (sd.isInt() || sd.isInt64() || sd.isDouble())
                        {
                            c.stepDenom = juce::jmax(1, (int)static_cast<double>(sd));
                        }
                        c.bpm = 110.0;
                        const juce::var& bp = cv.getProperty("bpm", {});
                        if (bp.isDouble() || bp.isInt() || bp.isInt64())
                        {
                            c.bpm = (double)bp;
                        }
                        c.loop = true;
                        const juce::var& lp = cv.getProperty("loop", {});
                        if (lp.isBool())
                        {
                            c.loop = (bool)lp;
                        }
                        c.laneStartFractionPermille = 0;
                        const juce::var& ls = cv.getProperty("laneStartFractionPermille", {});
                        if (ls.isInt() || ls.isInt64() || ls.isDouble())
                        {
                            c.laneStartFractionPermille = (int)static_cast<double>(ls);
                        }
                        c.laneEndFractionPermille = 250;
                        const juce::var& le = cv.getProperty("laneEndFractionPermille", {});
                        if (le.isInt() || le.isInt64() || le.isDouble())
                        {
                            c.laneEndFractionPermille = (int)static_cast<double>(le);
                        }
                        c.startSamples = 0;
                        {
                            bool ssOk = false;
                            const std::int64_t ss
                                = int64FromVarId(cv.getProperty("startSamples", {}), ssOk);
                            if (ssOk)
                            {
                                c.startSamples = ss;
                            }
                        }
                        c.lengthSamples = 0;
                        {
                            bool lenOk = false;
                            const std::int64_t len
                                = int64FromVarId(cv.getProperty("lengthSamples", {}), lenOk);
                            if (lenOk)
                            {
                                c.lengthSamples = len;
                            }
                        }
                        {
                            bool taOk = false;
                            const std::int64_t ta
                                = int64FromVarId(cv.getProperty("timelineAnchorSamples", {}), taOk);
                            if (taOk)
                            {
                                c.timelineAnchorSamples = ta;
                            }
                        }
                        const juce::var& notesV = cv.getProperty("notes", {});
                        if (notesV.isArray())
                        {
                            const juce::Array<juce::var>* na = notesV.getArray();
                            if (na != nullptr)
                            {
                                for (const juce::var& nv : *na)
                                {
                                    if (!nv.isObject())
                                    {
                                        continue;
                                    }
                                    ProjectFileExperimentalInstrumentNoteV1 n;
                                    const juce::var& mn = nv.getProperty("midiNote", {});
                                    if (mn.isInt() || mn.isInt64() || mn.isDouble())
                                    {
                                        n.midiNote = (int)static_cast<double>(mn);
                                    }
                                    const juce::var& st = nv.getProperty("step", {});
                                    if (st.isInt() || st.isInt64() || st.isDouble())
                                    {
                                        n.step = (int)static_cast<double>(st);
                                    }
                                    const juce::var& vl = nv.getProperty("velocity", {});
                                    if (vl.isInt() || vl.isInt64() || vl.isDouble())
                                    {
                                        n.velocity = (int)static_cast<double>(vl);
                                    }
                                    const juce::var& ln = nv.getProperty("lengthSteps", {});
                                    if (ln.isInt() || ln.isInt64() || ln.isDouble())
                                    {
                                        n.lengthSteps = juce::jmax(1, (int)static_cast<double>(ln));
                                    }
                                    c.notes.push_back(n);
                                }
                            }
                        }
                        c.ticksPerQuarter = 960;
                        const juce::var& tpqV = cv.getProperty("ticksPerQuarter", {});
                        if (tpqV.isInt() || tpqV.isInt64() || tpqV.isDouble())
                        {
                            c.ticksPerQuarter = juce::jmax(1, (int)static_cast<double>(tpqV));
                        }
                        const juce::var& tnv = cv.getProperty("timelineNotes", {});
                        if (tnv.isArray())
                        {
                            const juce::Array<juce::var>* tnarr = tnv.getArray();
                            if (tnarr != nullptr)
                            {
                                for (const juce::var& tvn : *tnarr)
                                {
                                    if (!tvn.isObject())
                                    {
                                        continue;
                                    }
                                    ProjectFileExperimentalTimelineNoteV12 tnn;
                                    const juce::var& mn2 = tvn.getProperty("midiNote", {});
                                    if (mn2.isInt() || mn2.isInt64() || mn2.isDouble())
                                    {
                                        tnn.midiNote = (int)static_cast<double>(mn2);
                                    }
                                    const juce::var& vv = tvn.getProperty("velocity", {});
                                    if (vv.isInt() || vv.isInt64() || vv.isDouble())
                                    {
                                        tnn.velocity = (int)static_cast<double>(vv);
                                    }
                                    const juce::var& chv = tvn.getProperty("channel", {});
                                    if (chv.isInt() || chv.isInt64() || chv.isDouble())
                                    {
                                        tnn.channel = juce::jlimit(
                                            1, 16, (int)static_cast<double>(chv));
                                    }
                                    bool stOk = false;
                                    const std::int64_t stTick
                                        = int64FromVarId(tvn.getProperty("startTick", {}), stOk);
                                    if (stOk)
                                    {
                                        tnn.startTick = stTick;
                                    }
                                    bool dtOk = false;
                                    const std::int64_t dt
                                        = int64FromVarId(tvn.getProperty("durationTicks", {}), dtOk);
                                    if (dtOk)
                                    {
                                        tnn.durationTicks = juce::jmax<std::int64_t>(
                                            1,
                                            dt);
                                    }
                                    c.timelineNotes.push_back(tnn);
                                }
                            }
                        }
                        const juce::var& mrvs = cv.getProperty("midiRollVisibleStartSamples", {});
                        if (mrvs.isInt() || mrvs.isInt64() || mrvs.isDouble())
                        {
                            c.midiRollVisibleStartSamples
                                = static_cast<std::int64_t>(static_cast<double>(mrvs));
                        }
                        const juce::var& mrsp = cv.getProperty("midiRollSamplesPerPixel", {});
                        if (mrsp.isDouble() || mrsp.isInt() || mrsp.isInt64())
                        {
                            c.midiRollSamplesPerPixel = static_cast<double>(mrsp);
                        }
                        const juce::var& mrfo = cv.getProperty("midiRollFollowEnabled", {});
                        if (mrfo.isBool())
                        {
                            c.midiRollFollowEnabled = (bool)mrfo;
                        }
                        else if (mrfo.isInt() || mrfo.isInt64() || mrfo.isDouble())
                        {
                            c.midiRollFollowEnabled = static_cast<int>(static_cast<double>(mrfo) + 0.5) != 0;
                        }
                        et.clips.push_back(std::move(c));
                    }
                }
            }
            out.experimentalInstrumentTracks.push_back(std::move(et));
        }
        return juce::Result::ok();
    }
} // namespace

juce::Result writeProjectFile(const juce::File& file, const ProjectFileV1& data)
{
    if (data.version != ProjectFileV1::kCurrentVersion)
    {
        return juce::Result::fail("Internal error: only the current project version is supported for writing.");
    }

    juce::Array<juce::var> trackVars;
    for (const auto& t : data.tracks)
    {
        trackVars.add(trackToVar(t, data.version));
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version", data.version);
    root->setProperty("nextPlacedClipId", static_cast<std::int64_t>(data.nextPlacedClipId));
    root->setProperty("nextTrackId", static_cast<std::int64_t>(data.nextTrackId));
    root->setProperty("activeTrackId", static_cast<std::int64_t>(data.activeTrackId));
    root->setProperty("playheadSamples", data.playheadSamples);
    root->setProperty("deviceSampleRateAtSave", data.deviceSampleRateAtSave);
    if (data.version >= 3)
    {
        root->setProperty("arrangementExtentSamples", data.arrangementExtentSamples);
    }
    if (data.version >= 6)
    {
        if (data.leftLocatorSamples != 0)
        {
            root->setProperty("leftLocatorSamples", data.leftLocatorSamples);
        }
        if (data.rightLocatorSamples != 0)
        {
            root->setProperty("rightLocatorSamples", data.rightLocatorSamples);
        }
    }
    if (data.version >= 10 && data.cycleEnabled)
    {
        root->setProperty("cycleEnabled", true);
    }
    root->setProperty("bpm", data.bpm);
    root->setProperty("timeSignatureNumerator", data.timeSignatureNumerator);
    root->setProperty("timeSignatureDenominator", data.timeSignatureDenominator);
    root->setProperty("ticksPerQuarter", data.ticksPerQuarter);
    root->setProperty("snapEnabled", data.snapEnabled);
    root->setProperty("snapResolution", data.snapResolution);
    if (data.hasMainWindowBounds && data.mainWindowBounds.width >= 320 && data.mainWindowBounds.height >= 240)
    {
        juce::DynamicObject::Ptr mw = new juce::DynamicObject();
        mw->setProperty("x", data.mainWindowBounds.x);
        mw->setProperty("y", data.mainWindowBounds.y);
        mw->setProperty("width", data.mainWindowBounds.width);
        mw->setProperty("height", data.mainWindowBounds.height);
        root->setProperty("mainWindow", juce::var(mw.get()));
    }
    root->setProperty("tracks", juce::var(trackVars));

    if (data.version >= 11 && !data.experimentalInstrumentTracks.empty())
    {
        juce::Array<juce::var> exTracks;
        for (const auto& et : data.experimentalInstrumentTracks)
        {
            juce::DynamicObject::Ptr eo = new juce::DynamicObject();
            if (!et.enabled)
            {
                eo->setProperty("enabled", false);
            }
            eo->setProperty("name", et.name);
            eo->setProperty("instrumentKind", et.instrumentKind);
            eo->setProperty("requiredKitName", et.requiredKitName);
            if (data.version >= 13 && et.trackId != 0 && et.trackId != kInvalidTrackId)
            {
                eo->setProperty("trackId", static_cast<std::int64_t>(et.trackId));
            }
            if (et.pluginBundlePath.isNotEmpty())
            {
                eo->setProperty("pluginBundlePath", et.pluginBundlePath);
            }
            if (et.pluginWasLoadedOnSave)
            {
                eo->setProperty("pluginWasLoadedOnSave", true);
            }
            if (et.pluginStateBase64.isNotEmpty())
            {
                eo->setProperty("pluginStateBase64", et.pluginStateBase64);
            }
            if (!et.powerOn)
            {
                eo->setProperty("powerOn", false);
            }
            if (et.muted)
            {
                eo->setProperty("muted", true);
            }
            if (!et.drumNoteNameOverrides.empty())
            {
                juce::DynamicObject::Ptr dnmObj = new juce::DynamicObject();
                for (const auto& kv : et.drumNoteNameOverrides)
                {
                    if (kv.first >= 0 && kv.first <= 127 && kv.second.isNotEmpty())
                    {
                        dnmObj->setProperty(juce::String(kv.first), kv.second);
                    }
                }
                if (dnmObj->getProperties().size() > 0)
                {
                    eo->setProperty("drumNoteNames", juce::var(dnmObj.get()));
                }
            }
            if (!et.drumNoteNameAutoPlugin.empty())
            {
                juce::DynamicObject::Ptr dnAutoObj = new juce::DynamicObject();
                for (const auto& kv : et.drumNoteNameAutoPlugin)
                {
                    if (kv.first >= 0 && kv.first <= 127 && kv.second.isNotEmpty())
                    {
                        dnAutoObj->setProperty(juce::String(kv.first), kv.second);
                    }
                }
                if (dnAutoObj->getProperties().size() > 0)
                {
                    eo->setProperty("drumNoteNamesAutoPlugin", juce::var(dnAutoObj.get()));
                }
            }
            juce::Array<juce::var> clipVars;
            for (const auto& cl : et.clips)
            {
                juce::DynamicObject::Ptr co = new juce::DynamicObject();
                co->setProperty("id", static_cast<std::int64_t>(cl.id));
                co->setProperty("name", cl.name);
                co->setProperty("numSteps", cl.numSteps);
                co->setProperty("stepDenom", cl.stepDenom);
                co->setProperty("bpm", cl.bpm);
                if (!cl.loop)
                {
                    co->setProperty("loop", false);
                }
                co->setProperty("startSamples", static_cast<juce::int64>(cl.startSamples));
                co->setProperty("lengthSamples", static_cast<juce::int64>(cl.lengthSamples));
                if (!cl.timelineNotes.empty())
                {
                    const std::int64_t anchorResolved = cl.timelineAnchorSamples.value_or(cl.startSamples);
                    co->setProperty("timelineAnchorSamples", static_cast<juce::int64>(anchorResolved));
                }
                else if (cl.timelineAnchorSamples.has_value())
                {
                    co->setProperty(
                        "timelineAnchorSamples", static_cast<juce::int64>(*cl.timelineAnchorSamples));
                }
                if (cl.laneStartFractionPermille != 0)
                {
                    co->setProperty("laneStartFractionPermille", cl.laneStartFractionPermille);
                }
                if (cl.laneEndFractionPermille != 250)
                {
                    co->setProperty("laneEndFractionPermille", cl.laneEndFractionPermille);
                }
                juce::Array<juce::var> noteVars;
                for (const auto& n : cl.notes)
                {
                    juce::DynamicObject::Ptr no = new juce::DynamicObject();
                    no->setProperty("midiNote", n.midiNote);
                    no->setProperty("step", n.step);
                    no->setProperty("velocity", n.velocity);
                    no->setProperty("lengthSteps", n.lengthSteps);
                    noteVars.add(juce::var(no.get()));
                }
                co->setProperty("notes", juce::var(noteVars));
                co->setProperty("ticksPerQuarter", cl.ticksPerQuarter);
                if (!cl.timelineNotes.empty())
                {
                    juce::Array<juce::var> tnoteVars;
                    for (const auto& tn : cl.timelineNotes)
                    {
                        juce::DynamicObject::Ptr tnO = new juce::DynamicObject();
                        tnO->setProperty("midiNote", tn.midiNote);
                        tnO->setProperty("velocity", tn.velocity);
                        tnO->setProperty("channel", tn.channel);
                        tnO->setProperty("startTick", static_cast<juce::int64>(tn.startTick));
                        tnO->setProperty("durationTicks", static_cast<juce::int64>(tn.durationTicks));
                        tnoteVars.add(juce::var(tnO.get()));
                    }
                    co->setProperty("timelineNotes", juce::var(tnoteVars));
                }
                if (cl.midiRollSamplesPerPixel > 0.0 && std::isfinite(cl.midiRollSamplesPerPixel))
                {
                    co->setProperty(
                        "midiRollVisibleStartSamples", static_cast<juce::int64>(cl.midiRollVisibleStartSamples));
                    co->setProperty("midiRollSamplesPerPixel", cl.midiRollSamplesPerPixel);
                    if (cl.midiRollFollowEnabled)
                    {
                        co->setProperty("midiRollFollowEnabled", true);
                    }
                }
                clipVars.add(juce::var(co.get()));
            }
            eo->setProperty("clips", juce::var(clipVars));
            exTracks.add(juce::var(eo.get()));
        }
        root->setProperty("experimentalInstrumentTracks", juce::var(exTracks));
    }

    if (data.hasAudioMixdown)
    {
        juce::DynamicObject::Ptr amo = new juce::DynamicObject();
        amo->setProperty("fileNameWithoutExtension", data.audioMixdown.fileNameWithoutExtension);
        amo->setProperty("outputDirectory", data.audioMixdown.outputDirectory);
        amo->setProperty("fileType", data.audioMixdown.fileType);
        amo->setProperty("wavBitDepth", data.audioMixdown.wavBitDepth);
        amo->setProperty("mp3BitRateKbps", data.audioMixdown.mp3BitRateKbps);
        root->setProperty("audioMixdown", juce::var(amo.get()));
    }

    const juce::String text = juce::JSON::toString(juce::var(root.get()), true);
    if (text.isEmpty())
    {
        return juce::Result::fail("Could not encode project to JSON.");
    }

    if (!file.replaceWithText(text, false, false, nullptr))
    {
        return juce::Result::fail("Could not write project file.");
    }
    return juce::Result::ok();
}

juce::Result readProjectFile(const juce::File& file, ProjectFileV1& out)
{
    out = ProjectFileV1{};
    if (!file.existsAsFile())
    {
        return juce::Result::fail("Project file does not exist.");
    }

    juce::String err;
    const juce::String text = file.loadFileAsString();
    if (text.isEmpty() && file.getSize() != 0)
    {
        return juce::Result::fail("Could not read project file.");
    }

    juce::var root;
    {
        juce::Result pr = juce::JSON::parse(text, root);
        if (pr.failed())
        {
            return juce::Result::fail("Invalid JSON: " + pr.getErrorMessage());
        }
    }

    if (!root.isObject())
    {
        return juce::Result::fail("Project root must be a JSON object.");
    }

    if (!root.hasProperty("version"))
    {
        return juce::Result::fail("Project missing version field.");
    }
    {
        const juce::var& vver = root["version"];
        if (!(vver.isInt() || vver.isInt64() || vver.isDouble()))
        {
            return juce::Result::fail("Project missing or invalid version field.");
        }
    }
    const int ver = (int)static_cast<double>(root["version"]);
    if (ver < 1 || ver > ProjectFileV1::kCurrentVersion)
    {
        return juce::Result::fail(juce::String("Unsupported project version (supported: 1–")
                                  + juce::String(ProjectFileV1::kCurrentVersion)
                                  + ").");
    }

    out.version = ver;

    auto toId = [](const juce::var& value) -> std::uint64_t
    {
        if (value.isString())
        {
            return static_cast<std::uint64_t>(value.toString().getLargeIntValue());
        }
        if (value.isInt() || value.isInt64() || value.isDouble())
        {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>((double)value));
        }
        return 0;
    };

    if (!root.hasProperty("nextPlacedClipId") || !root.hasProperty("nextTrackId")
        || !root.hasProperty("activeTrackId"))
    {
        return juce::Result::fail("Project missing id seed fields.");
    }
    out.nextPlacedClipId = toId(root.getProperty("nextPlacedClipId", {}));
    out.nextTrackId = toId(root.getProperty("nextTrackId", {}));
    out.activeTrackId = toId(root.getProperty("activeTrackId", {}));
    if (out.nextPlacedClipId == 0 || out.nextTrackId == 0 || out.activeTrackId == 0)
    {
        return juce::Result::fail("Project has invalid id fields (0 is not allowed).");
    }
    if (root.hasProperty("playheadSamples")
        && (root["playheadSamples"].isInt64() || root["playheadSamples"].isInt()
            || root["playheadSamples"].isDouble()))
    {
        out.playheadSamples = static_cast<std::int64_t>(static_cast<double>(root["playheadSamples"]));
    }
    else
    {
        return juce::Result::fail("Project missing or invalid playheadSamples.");
    }

    if (root.hasProperty("deviceSampleRateAtSave")
        && (root["deviceSampleRateAtSave"].isDouble() || root["deviceSampleRateAtSave"].isInt()
            || root["deviceSampleRateAtSave"].isInt64()))
    {
        out.deviceSampleRateAtSave = (double)root["deviceSampleRateAtSave"];
    }
    else
    {
        return juce::Result::fail("Project missing or invalid deviceSampleRateAtSave.");
    }

    out.arrangementExtentSamples = 0;
    if (ver >= 3)
    {
        const juce::var& aex = root.getProperty("arrangementExtentSamples", {});
        if (aex.isInt64() || aex.isInt() || aex.isDouble())
        {
            out.arrangementExtentSamples
                = static_cast<std::int64_t>(static_cast<double>(aex));
        }
    }

    out.leftLocatorSamples = 0;
    out.rightLocatorSamples = 0;
    if (ver >= 6)
    {
        const juce::var& ll = root.getProperty("leftLocatorSamples", {});
        if (ll.isInt64() || ll.isInt() || ll.isDouble())
        {
            out.leftLocatorSamples = static_cast<std::int64_t>(static_cast<double>(ll));
        }
        const juce::var& rl = root.getProperty("rightLocatorSamples", {});
        if (rl.isInt64() || rl.isInt() || rl.isDouble())
        {
            out.rightLocatorSamples = static_cast<std::int64_t>(static_cast<double>(rl));
        }
    }

    if (ver >= 10)
    {
        const juce::var& cv = root.getProperty("cycleEnabled", {});
        if (cv.isBool())
        {
            out.cycleEnabled = (bool)cv;
        }
        else if (cv.isInt() || cv.isInt64() || cv.isDouble())
        {
            out.cycleEnabled = static_cast<int>(static_cast<double>(cv) + 0.5) != 0;
        }
    }

    {
        const juce::var& bpmv = root.getProperty("bpm", {});
        if (bpmv.isDouble() || bpmv.isInt() || bpmv.isInt64())
        {
            out.bpm = static_cast<double>(bpmv);
        }
        const juce::var& tsn = root.getProperty("timeSignatureNumerator", {});
        if (tsn.isInt() || tsn.isInt64())
        {
            out.timeSignatureNumerator = static_cast<int>(static_cast<std::int64_t>((double)tsn));
        }
        else if (tsn.isDouble())
        {
            out.timeSignatureNumerator = static_cast<int>(static_cast<double>(tsn) + 0.5);
        }
        const juce::var& tsd = root.getProperty("timeSignatureDenominator", {});
        if (tsd.isInt() || tsd.isInt64())
        {
            out.timeSignatureDenominator = static_cast<int>(static_cast<std::int64_t>((double)tsd));
        }
        else if (tsd.isDouble())
        {
            out.timeSignatureDenominator = static_cast<int>(static_cast<double>(tsd) + 0.5);
        }
        const juce::var& tpqRoot = root.getProperty("ticksPerQuarter", {});
        if (tpqRoot.isInt() || tpqRoot.isInt64())
        {
            out.ticksPerQuarter = static_cast<int>(static_cast<std::int64_t>((double)tpqRoot));
        }
        else if (tpqRoot.isDouble())
        {
            out.ticksPerQuarter = static_cast<int>(static_cast<double>(tpqRoot) + 0.5);
        }
        ProjectMusicalTime tm;
        tm.bpm = out.bpm;
        tm.numerator = out.timeSignatureNumerator;
        tm.denominator = out.timeSignatureDenominator;
        tm.ticksPerQuarter = out.ticksPerQuarter;
        tm = sanitizeProjectMusicalTime(tm);
        out.bpm = tm.bpm;
        out.timeSignatureNumerator = tm.numerator;
        out.timeSignatureDenominator = tm.denominator;
        out.ticksPerQuarter = tm.ticksPerQuarter;
    }

    {
        const juce::var& snapEn = root.getProperty("snapEnabled", {});
        if (snapEn.isBool())
        {
            out.snapEnabled = (bool)snapEn;
        }
        else if (snapEn.isInt() || snapEn.isInt64() || snapEn.isDouble())
        {
            out.snapEnabled = static_cast<int>(static_cast<double>(snapEn) + 0.5) != 0;
        }
        juce::String snapKey = "1_4";
        const juce::var& snapRes = root.getProperty("snapResolution", {});
        if (snapRes.isString())
        {
            snapKey = snapRes.toString();
        }
        const SnapResolution decoded = snapResolutionFromProjectString(snapKey);
        out.snapResolution = snapResolutionToProjectString(decoded);
    }

    {
        const juce::var& mwVar = root.getProperty("mainWindow", {});
        if (mwVar.isObject())
        {
            const auto* dyn = mwVar.getDynamicObject();
            if (dyn != nullptr)
            {
                auto propInt = [](const juce::var& v, int fallback) -> int {
                    if (v.isInt() || v.isInt64())
                    {
                        return static_cast<int>(static_cast<juce::int64>(v));
                    }
                    if (v.isDouble())
                    {
                        return juce::roundToInt(static_cast<double>(v));
                    }
                    if (v.isString())
                    {
                        return v.toString().trim().getIntValue();
                    }
                    return fallback;
                };
                const int x = propInt(dyn->getProperty("x"), std::numeric_limits<int>::min());
                const int y = propInt(dyn->getProperty("y"), std::numeric_limits<int>::min());
                const int ww = propInt(dyn->getProperty("width"), 0);
                const int hh = propInt(dyn->getProperty("height"), 0);
                constexpr int kMinW = 320;
                constexpr int kMinH = 240;
                if (x != std::numeric_limits<int>::min() && y != std::numeric_limits<int>::min() && ww >= kMinW
                    && hh >= kMinH)
                {
                    out.hasMainWindowBounds = true;
                    out.mainWindowBounds.x = x;
                    out.mainWindowBounds.y = y;
                    out.mainWindowBounds.width = juce::jmin(ww, 10000);
                    out.mainWindowBounds.height = juce::jmin(hh, 10000);
                }
            }
        }
    }

    {
        const juce::var& am = root.getProperty("audioMixdown", {});
        if (am.isObject())
        {
            out.hasAudioMixdown = true;
            ProjectFileAudioMixdownV1& m = out.audioMixdown;
            m.fileNameWithoutExtension = am.getProperty("fileNameWithoutExtension", {}).toString();
            m.outputDirectory = am.getProperty("outputDirectory", {}).toString();
            m.fileType = am.getProperty("fileType", {}).toString().trim().toLowerCase();
            if (m.fileType.isEmpty())
            {
                m.fileType = "wave";
            }

            const juce::var& wbVar = am.getProperty("wavBitDepth", {});
            int wb = 0;
            if (wbVar.isInt() || wbVar.isInt64())
            {
                wb = static_cast<int>(static_cast<std::int64_t>((double)wbVar));
            }
            else if (wbVar.isDouble())
            {
                wb = static_cast<int>(static_cast<double>(wbVar) + 0.5);
            }
            if (wb == 16 || wb == 24 || wb == 32)
            {
                m.wavBitDepth = wb;
            }
            else
            {
                m.wavBitDepth = 0;
            }

            const juce::var& brVar = am.getProperty("mp3BitRateKbps", {});
            int br = 320;
            if (brVar.isInt() || brVar.isInt64())
            {
                br = static_cast<int>(static_cast<std::int64_t>((double)brVar));
            }
            else if (brVar.isDouble())
            {
                br = static_cast<int>(static_cast<double>(brVar) + 0.5);
            }
            constexpr int rates[] {128, 160, 192, 224, 256, 320};
            int best = rates[0];
            int bestD = std::abs(br - best);
            for (const int r : rates)
            {
                const int d = std::abs(br - r);
                if (d < bestD)
                {
                    bestD = d;
                    best = r;
                }
            }
            m.mp3BitRateKbps = best;
        }
    }

    const juce::var& tracksVar = root["tracks"];
    if (!tracksVar.isArray())
    {
        return juce::Result::fail("Project missing tracks array.");
    }
    const juce::Array<juce::var>* arr = tracksVar.getArray();
    if (arr == nullptr || arr->isEmpty())
    {
        return juce::Result::fail("Project has no tracks.");
    }

    for (const juce::var& tv : *arr)
    {
        if (!tv.isObject())
        {
            return juce::Result::fail("Each track must be a JSON object.");
        }
        ProjectFileTrackV1 trk;
        trk.id = toId(tv.getProperty("id", {}));
        if (trk.id == 0)
        {
            return juce::Result::fail("Track has invalid or missing id.");
        }
        for (const auto& existing : out.tracks)
        {
            if (existing.id == trk.id)
            {
                return juce::Result::fail("Duplicate track id in project file.");
            }
        }

        trk.name = tv.getProperty("name", {}).toString();
        if (trk.name.isEmpty())
        {
            trk.name = "Track " + juce::String(trk.id);
        }
        if (ver >= 13)
        {
            trk.kind = tv.getProperty("kind", {}).toString().trim().toLowerCase();
        }
        trk.channelFaderGain = kTrackChannelVolumeUnityGain;
        if (ver >= 5)
        {
            const juce::var& gv = tv.getProperty("channelFaderGain", {});
            if (gv.isDouble() || gv.isInt() || gv.isInt64())
            {
                trk.channelFaderGain = juce::jlimit(
                    0.0f, kTrackChannelFaderGainMax, (float)(double)gv);
            }
        }
        trk.stereoPan = 0.0f;
        {
            const juce::var& pv = tv.getProperty("pan", {});
            if (pv.isDouble() || pv.isInt() || pv.isInt64())
            {
                trk.stereoPan = sanitizeTrackStereoPan((double)pv);
            }
        }
        {
            const juce::var& ov = tv.getProperty("off", {});
            if (ov.isBool())
                trk.off = (bool)ov;
            else if (ov.isInt() || ov.isInt64() || ov.isDouble())
                trk.off = static_cast<int>(static_cast<double>(ov) + 0.5) != 0;
            const juce::var& mv = tv.getProperty("muted", {});
            if (mv.isBool())
                trk.muted = (bool)mv;
            else if (mv.isInt() || mv.isInt64() || mv.isDouble())
                trk.muted = static_cast<int>(static_cast<double>(mv) + 0.5) != 0;
        }
        if (trk.kind.equalsIgnoreCase("master"))
        {
            trk.off = false;
        }
        if (ver >= 14)
        {
            const juce::var& outV = tv.getProperty("output", {});
            if (outV.isObject())
            {
                if (const auto* outObj = outV.getDynamicObject())
                {
                    const TrackId parsed = static_cast<TrackId>((std::uint64_t)(std::int64_t)outObj->getProperty(
                        "trackId"));
                    if (parsed != kInvalidTrackId)
                    {
                        trk.routedOutputTrackId = parsed;
                    }
                }
            }
        }
        if (ver >= 8)
        {
            trk.pluginVst3Path = tv.getProperty("pluginVst3Path", {}).toString();
            trk.pluginIdentifier = tv.getProperty("pluginIdentifier", {}).toString();
            trk.pluginStateBase64 = tv.getProperty("pluginStateBase64", {}).toString();
        }
        if (ver >= 9)
        {
            const juce::Result ir = parseTrackInsertsFromVar(tv, trk, err, ver);
            if (ir.failed())
            {
                return juce::Result::fail(err);
            }
        }
        if (ver >= 8)
        {
            migrateLegacySinglePluginToInserts(trk);
        }

        const juce::var& clipsV = tv.getProperty("clips", {});
        if (clipsV.isArray())
        {
            const juce::Array<juce::var>* clipArr = clipsV.getArray();
            if (clipArr != nullptr)
            {
                for (const juce::var& cv : *clipArr)
                {
                    ProjectFileClipV1 c;
                    const juce::Result cr = clipFromVar(cv, c, err, ver);
                    if (cr.failed())
                    {
                        return juce::Result::fail(err);
                    }
                    for (const auto& ex : trk.clips)
                    {
                        if (ex.id == c.id)
                        {
                            return juce::Result::fail("Duplicate placed clip id within a track.");
                        }
                    }
                    trk.clips.push_back(std::move(c));
                }
            }
        }

        out.tracks.push_back(std::move(trk));
    }

    {
        const juce::Result exr = parseExperimentalInstrumentTracksObject(root, out, ver);
        if (exr.failed())
        {
            return exr;
        }
    }

    migrateProjectFileExperimentalInstrumentLanePreV13(out);
    migrateProjectFileMasterTrackPreV14(out);

    {
        std::unordered_set<PlacedClipId> globalClip;
        for (const auto& tr : out.tracks)
        {
            for (const auto& c : tr.clips)
            {
                if (!globalClip.insert(c.id).second)
                {
                    return juce::Result::fail("Duplicate placed clip id across tracks.");
                }
            }
        }
    }

    return juce::Result::ok();
}

void stripExperimentalInstrumentTrackPluginFieldsForUndo(ProjectFileExperimentalInstrumentTrackV1& t) noexcept
{
    t.pluginStateBase64.clear();
    t.pluginWasLoadedOnSave = false;
    t.pluginBundlePath.clear();
    t.drumNoteNameOverrides.clear();
    t.drumNoteNameAutoPlugin.clear();
}

namespace
{
    [[nodiscard]] bool experimentalInstrumentClipMusicalEqual(const ProjectFileExperimentalInstrumentClipV1& a,
                                                             const ProjectFileExperimentalInstrumentClipV1& b) noexcept
    {
        if (a.id != b.id || a.name != b.name || a.numSteps != b.numSteps || a.stepDenom != b.stepDenom
            || a.bpm != b.bpm || a.loop != b.loop || a.startSamples != b.startSamples
            || a.lengthSamples != b.lengthSamples
            || a.timelineAnchorSamples.value_or(a.startSamples) != b.timelineAnchorSamples.value_or(b.startSamples)
            || a.laneStartFractionPermille != b.laneStartFractionPermille
            || a.laneEndFractionPermille != b.laneEndFractionPermille || a.ticksPerQuarter != b.ticksPerQuarter)
        {
            return false;
        }
        if (a.notes.size() != b.notes.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.notes.size(); ++i)
        {
            const auto& p = a.notes[i];
            const auto& q = b.notes[i];
            if (p.midiNote != q.midiNote || p.step != q.step || p.velocity != q.velocity
                || p.lengthSteps != q.lengthSteps)
            {
                return false;
            }
        }
        if (a.timelineNotes.size() != b.timelineNotes.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.timelineNotes.size(); ++i)
        {
            const auto& p = a.timelineNotes[i];
            const auto& q = b.timelineNotes[i];
            if (p.midiNote != q.midiNote || p.velocity != q.velocity || p.channel != q.channel
                || p.startTick != q.startTick || p.durationTicks != q.durationTicks)
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool experimentalInstrumentTrackMusicalEqualIgnoringPlugin(
        const ProjectFileExperimentalInstrumentTrackV1& a,
        const ProjectFileExperimentalInstrumentTrackV1& b) noexcept
    {
        if (a.enabled != b.enabled || a.name != b.name || a.instrumentKind != b.instrumentKind
            || a.requiredKitName != b.requiredKitName || a.powerOn != b.powerOn || a.muted != b.muted
            || a.trackId != b.trackId)
        {
            return false;
        }
        if (a.clips.size() != b.clips.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.clips.size(); ++i)
        {
            if (!experimentalInstrumentClipMusicalEqual(a.clips[i], b.clips[i]))
            {
                return false;
            }
        }
        return true;
    }
} // namespace

bool experimentalInstrumentTracksMusicalUndoEqual(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& a,
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& b) noexcept
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (!experimentalInstrumentTrackMusicalEqualIgnoringPlugin(a[i], b[i]))
        {
            return false;
        }
    }
    return true;
}
