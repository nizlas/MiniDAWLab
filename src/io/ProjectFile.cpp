// =============================================================================
// ProjectFile.cpp — strict JSON v1; parse failure does not touch Session (caller)
// =============================================================================

#include "io/ProjectFile.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <unordered_set>

namespace
{
    [[nodiscard]] juce::var trackToVar(const ProjectFileTrackV1& t)
    {
        juce::Array<juce::var> clipVars;
        for (const auto& c : t.clips)
        {
            juce::DynamicObject::Ptr co = new juce::DynamicObject();
            co->setProperty("id", static_cast<std::int64_t>(c.id));
            co->setProperty("startSample", c.startSample);
            co->setProperty("sourcePath", c.sourcePath);
            clipVars.add(juce::var(co.get()));
        }
        juce::DynamicObject::Ptr to = new juce::DynamicObject();
        to->setProperty("id", static_cast<std::int64_t>(t.id));
        to->setProperty("name", t.name);
        to->setProperty("clips", juce::var(clipVars));
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

    [[nodiscard]] juce::Result clipFromVar(
        const juce::var& v, ProjectFileClipV1& out, juce::String& err)
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
        return juce::Result::ok();
    }
} // namespace

juce::Result writeProjectFile(const juce::File& file, const ProjectFileV1& data)
{
    if (data.version != ProjectFileV1::kCurrentVersion)
    {
        return juce::Result::fail("Internal error: only version 1 is supported for writing.");
    }

    juce::Array<juce::var> trackVars;
    for (const auto& t : data.tracks)
    {
        trackVars.add(trackToVar(t));
    }

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version", data.version);
    root->setProperty("nextPlacedClipId", static_cast<std::int64_t>(data.nextPlacedClipId));
    root->setProperty("nextTrackId", static_cast<std::int64_t>(data.nextTrackId));
    root->setProperty("activeTrackId", static_cast<std::int64_t>(data.activeTrackId));
    root->setProperty("playheadSamples", data.playheadSamples);
    root->setProperty("deviceSampleRateAtSave", data.deviceSampleRateAtSave);
    root->setProperty("tracks", juce::var(trackVars));

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
    if (ver != ProjectFileV1::kCurrentVersion)
    {
        return juce::Result::fail("Unsupported project version (expected "
                                  + juce::String(ProjectFileV1::kCurrentVersion) + ").");
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

        const juce::var& clipsV = tv.getProperty("clips", {});
        if (clipsV.isArray())
        {
            const juce::Array<juce::var>* clipArr = clipsV.getArray();
            if (clipArr != nullptr)
            {
                for (const juce::var& cv : *clipArr)
                {
                    ProjectFileClipV1 c;
                    const juce::Result cr = clipFromVar(cv, c, err);
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
