// =============================================================================
// ProjectFile.cpp — strict JSON v1; parse failure does not touch Session (caller)
// =============================================================================

#include "io/ProjectFile.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
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
        to->setProperty("clips", juce::var(clipVars));
        constexpr double kUnityChannelFaderOmitEpsilon = 1.0e-6;
        if (std::fabs((double)t.channelFaderGain - 1.0) > kUnityChannelFaderOmitEpsilon)
        {
            to->setProperty("channelFaderGain", (double)t.channelFaderGain);
        }
        if (t.off)
        {
            to->setProperty("off", true);
        }
        if (t.muted)
        {
            to->setProperty("muted", true);
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
            et.name = tv.getProperty("name", {}).toString();
            if (et.name.isEmpty())
            {
                et.name = "Groove Agent SE";
            }
            et.instrumentKind = tv.getProperty("instrumentKind", {}).toString();
            if (et.instrumentKind.isEmpty())
            {
                et.instrumentKind = "GrooveAgentSE";
            }
            et.requiredKitName = tv.getProperty("requiredKitName", {}).toString();
            if (et.requiredKitName.isEmpty())
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
            || a.lengthSamples != b.lengthSamples || a.laneStartFractionPermille != b.laneStartFractionPermille
            || a.laneEndFractionPermille != b.laneEndFractionPermille || a.ticksPerQuarter != b.ticksPerQuarter
            || a.midiRollVisibleStartSamples != b.midiRollVisibleStartSamples
            || a.midiRollSamplesPerPixel != b.midiRollSamplesPerPixel
            || a.midiRollFollowEnabled != b.midiRollFollowEnabled)
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
            || a.requiredKitName != b.requiredKitName || a.powerOn != b.powerOn || a.muted != b.muted)
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
