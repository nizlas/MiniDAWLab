#pragma once

// =============================================================================
// ProxyPlaybackMix — P1G audio-thread proxy substitution mix step (header-only)
// =============================================================================
//
// The exact instrument-generation replacement performed inside
// `ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs` when the
// published `ProxyPlaybackView` selects Proxy. Extracted as a pure function so
// the deterministic selftests exercise the REAL production mix step (segment
// bounds, ring fetch vs silent generation, zeros outside playing segments,
// peak) without linking the plugin-hosting translation unit.
//
// AUDIO-THREAD CONTRACT (PI-031): no filesystem access, no locks, no
// allocation, no file open/close, no destruction. Only memset/memcpy/ring reads
// through `ProxyPlaybackReader::audioThread_fetch` plus float math.

#include <cstdint>
#include <cstring>
#include <cmath>

#include "instruments/ProxyPlaybackSource.h"

namespace proxy_playback
{

/// One playing timeline segment of the current block (engine-rate frames).
struct ProxyTimelineSegment
{
    std::int64_t timelineStart = 0;
    int outOffset = 0;
    int numSamples = 0;
};

struct ProxyMixOutcome
{
    bool producedBlock = false; ///< >= 1 valid segment (also true for silent generations)
    float peak = 0.0f;          ///< max |sample| over the whole block (pre-strip)
};

/// [Audio thread] Render the block's proxy output into the preallocated stereo
/// scratch (COPY semantics; frames outside playing segments are exactly zero).
/// A silent generation (`view.reader == nullptr`) yields zeros by definition
/// but still counts as produced output (§15.7: intentional silence IS the
/// generation's audio). EOF/pre-readiness inside the reader is silence too.
[[nodiscard]] inline ProxyMixOutcome
    renderProxySegmentsToStereoScratch(const ProxyPlaybackView& view,
                                       const ProxyTimelineSegment* segments,
                                       const int segmentCount,
                                       float* scratchL,
                                       float* scratchR,
                                       const int numSamples) noexcept
{
    ProxyMixOutcome out;
    if (scratchL == nullptr || scratchR == nullptr || numSamples <= 0)
    {
        return out;
    }
    std::memset(scratchL, 0, sizeof(float) * static_cast<size_t>(numSamples));
    std::memset(scratchR, 0, sizeof(float) * static_cast<size_t>(numSamples));
    if (!view.useProxy)
    {
        return out;
    }
    for (int i = 0; i < segmentCount; ++i)
    {
        const ProxyTimelineSegment& seg = segments[i];
        if (seg.numSamples <= 0 || seg.outOffset < 0
            || seg.outOffset + seg.numSamples > numSamples)
        {
            continue;
        }
        out.producedBlock = true;
        if (view.reader != nullptr)
        {
            // Bounded ring copy or zero-fill; EOF and pre-readiness are silence by design.
            (void)view.reader->audioThread_fetch(seg.timelineStart,
                                                 scratchL + seg.outOffset,
                                                 scratchR + seg.outOffset,
                                                 seg.numSamples);
        }
        // Silent generation: zeros already in place — intentional ProxyCurrent silence.
    }
    if (out.producedBlock)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::fabs(scratchL[i]);
            const float b = std::fabs(scratchR[i]);
            out.peak = a > out.peak ? a : out.peak;
            out.peak = b > out.peak ? b : out.peak;
        }
    }
    return out;
}

} // namespace proxy_playback
