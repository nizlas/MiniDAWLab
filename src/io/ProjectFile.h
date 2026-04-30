#pragma once

// =============================================================================
// ProjectFile — v1 JSON encode/decode for minimal project persistence (io layer)
// =============================================================================
// `sourcePath` is the absolute on-disk path last used to decode the clip
// (AudioClip::getSourceFilePath, derived from the loader at load time). Not relative paths.
// =============================================================================

#include "domain/PlacedClip.h"
#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

struct ProjectFileClipV1
{
    PlacedClipId id = kInvalidPlacedClipId;
    std::int64_t startSample = 0;
    juce::String sourcePath;
    // v2: 0 = full material (read path uses `PlacedClip` default); if >0, right-edge **visible** span
    // in samples (clamped to material on load). Omitted in v1 JSON; treated as 0.
    std::int64_t visibleLengthSamples = 0;
    // v4: non-destructive left trim (file indices skipped); 0 = absent in JSON.
    std::int64_t leftTrimSamples = 0;
    // v7: bounds on permissible trim within shared material [start, end) in file indices; omit for full-material.
    std::int64_t materialWindowStartSamples = 0;
    std::int64_t materialWindowEndExclusiveSamples = 0;
    bool hasMaterialWindowInFile = false;
};

struct ProjectFileTrackV1
{
    TrackId id = kInvalidTrackId;
    juce::String name;
    std::vector<ProjectFileClipV1> clips;
    // v5: linear gain at channel-fader point (mixer). Omitted in JSON when ~ unity (see writer).
    float channelFaderGain = kTrackChannelVolumeUnityGain;
};

// Minimal project snapshot: multi-track, placed clips, monotonic id seeds, transport hints.
struct ProjectFileV1
{
    static constexpr int kCurrentVersion = 7;

    int version = kCurrentVersion;
    PlacedClipId nextPlacedClipId = 1;
    TrackId nextTrackId = 2;
    TrackId activeTrackId = 1;
    std::int64_t playheadSamples = 0;
    double deviceSampleRateAtSave = 0.0;
    // v3: effective arrangement extent in samples (optional in JSON; 0 = treat as “absent / floor
    // from content only” on load — see `SessionSnapshot::withTracks`).
    std::int64_t arrangementExtentSamples = 0;
    // v6: timeline locators (samples). Omitted in JSON when 0; `right == 0` = right locator unset.
    std::int64_t leftLocatorSamples = 0;
    std::int64_t rightLocatorSamples = 0;
    std::vector<ProjectFileTrackV1> tracks;
};

[[nodiscard]] juce::Result writeProjectFile(const juce::File& file, const ProjectFileV1& data);
[[nodiscard]] juce::Result readProjectFile(const juce::File& file, ProjectFileV1& outData);
