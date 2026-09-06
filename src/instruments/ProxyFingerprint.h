#pragma once

// =============================================================================
// ProxyFingerprint — canonical deterministic fingerprint serialization + hash (R5)
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §11; P1C slice)
// =============================================================================
// The fingerprint is a SHA-256 over a canonical byte serialization of a ProxyRenderSnapshot,
// following the Locked §11.4 rules:
//   * fixed F-number field order, tagged with {algorithm id, fingerprint schema version};
//   * clips in the bake's plan order (stable sort by startSamples, stored-order ties — the ORD-1
//     delivery tie-break); notes in stored vector order; CC in repository-canonical normalized
//     order; sources strictly in session order, destination content first;
//   * integers fixed-width little-endian; doubles as IEEE 754 bit patterns; UTF-8
//     length-prefixed strings; plugin path normalized to forward slashes;
//   * NO raw plugin-state blob bytes — the F2 identity component is the host-managed state
//     revision + save-pairing (hybrid contract §9.4, revision 5);
//   * forbidden inputs (PI-019): pointer identity, unordered-container order, display names,
//     locale formatting.
// Included (per the canonical table): F1–F13 plus the relevant event span and the
// silent-generation eligibility inputs (§15.6/§15.7). Excluded by construction (§11.2): fader,
// pan, DAL insert chains, sends, audio routing, downstream strips, latency-settings offsets —
// none of them exist inside the snapshot. No partial-render dirty region exists (regional
// re-rendering is deferred P4).
// =============================================================================

#include "instruments/ProxyRenderSnapshot.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace proxy_fingerprint
{
    /// Bumping either value deterministically marks every existing proxy stale (§11.4).
    inline constexpr std::uint32_t kFingerprintAlgorithmId = 1;   // 1 = SHA-256 over this layout
    inline constexpr std::uint32_t kFingerprintSchemaVersion = 1; // field-layout version

    namespace detail
    {
        struct Writer
        {
            juce::MemoryOutputStream out;

            // juce::MemoryOutputStream::writeInt/writeInt64/writeDouble are little-endian by
            // contract — the deterministic encoding §11.4 requires (no locale, no printf).
            void u8(const bool v) { out.writeByte(v ? 1 : 0); }
            void i32(const std::int32_t v) { out.writeInt((int)v); }
            void u32(const std::uint32_t v) { out.writeInt((int)v); }
            void i64(const std::int64_t v) { out.writeInt64((juce::int64)v); }
            void u64(const std::uint64_t v) { out.writeInt64((juce::int64)v); }
            void f64(const double v) { out.writeDouble(v); } // IEEE 754 bit pattern, little-endian

            /// UTF-8 bytes, u32 length prefix (identity strings only; never display names).
            void str(const juce::String& s)
            {
                const auto utf8 = s.toUTF8();
                const std::uint32_t len = (std::uint32_t)utf8.sizeInBytes() - 1; // exclude NUL
                u32(len);
                out.write(utf8.getAddress(), (size_t)len);
            }

            /// Plugin path: normalized to forward slashes (§11.4 string/path normalization).
            void path(const juce::String& p) { str(p.replaceCharacter('\\', '/')); }
        };

        inline void writeClip(Writer& w, const proxy_snapshot::SnapshotClip& c)
        {
            // F3 — sample-domain integers raw (interpretation rate = F11 timeline reference).
            w.u64(c.clipId); // data field, never an ordering key (§11.4)
            w.i64(c.startSamples);
            w.i64(c.timelineAnchorSamples);
            w.i64(c.lengthSamples);
            w.f64(c.bpm);
            w.i32(c.ticksPerQuarter);
            // F4 — notes in STORED vector order (the ORD-1 equal-time delivery tie-break).
            w.u32((std::uint32_t)c.notes.size());
            for (const auto& n : c.notes)
            {
                w.i32(n.midiNote);
                w.i32(n.velocity);
                w.i32(n.offVelocity);
                w.i32(n.channel);
                w.i64(n.startTick);
                w.i64(n.durationTicks);
            }
            // F5 — CC points in repository-canonical normalized order (normalizePoints last-wins;
            // no sort invented here).
            w.u32((std::uint32_t)c.ccPoints.size());
            for (const auto& p : c.ccPoints)
            {
                w.i64(p.startTick);
                w.i32(p.controller);
                w.i32(p.value);
                w.i32(p.channel);
                w.i32(p.interpolationToNext);
            }
        }

        /// Clips serialize in the bake's PLAN order: stable sort by `startSamples` over the stored
        /// sequence — equal-start ties keep stored order (§11.4; matches live scheduling).
        inline void writeClipsInPlanOrder(Writer& w, const std::vector<proxy_snapshot::SnapshotClip>& clips)
        {
            std::vector<const proxy_snapshot::SnapshotClip*> plan;
            plan.reserve(clips.size());
            for (const auto& c : clips)
            {
                plan.push_back(&c);
            }
            std::stable_sort(plan.begin(), plan.end(),
                             [](const proxy_snapshot::SnapshotClip* a, const proxy_snapshot::SnapshotClip* b) {
                                 return a->startSamples < b->startSamples;
                             });
            w.u32((std::uint32_t)plan.size());
            for (const auto* c : plan)
            {
                writeClip(w, *c);
            }
        }
    } // namespace detail

    /// Canonical fingerprint bytes of a snapshot (before hashing). Deterministic: repeated calls
    /// on the same snapshot produce identical bytes.
    [[nodiscard]] inline juce::MemoryBlock serializeCanonicalFingerprintBytes(
        const proxy_snapshot::ProxyRenderSnapshot& s)
    {
        detail::Writer w;
        // Version tags first (§11.4).
        w.u32(kFingerprintAlgorithmId);
        w.u32(kFingerprintSchemaVersion);
        // F1 — plugin identity (path-normalized), F1v — version.
        w.path(s.pluginIdentity.fileOrIdentifier);
        w.i32(s.pluginIdentity.uniqueId);
        w.i32(s.pluginIdentity.deprecatedUid);
        w.str(s.pluginIdentity.format);
        w.u8(s.pluginIdentity.isInstrument);
        w.str(s.pluginIdentity.version);
        // F2 — identity component ONLY: host-managed revision + save pairing (§9.4.2). The raw
        // state blob bytes are deliberately NOT serialized (never general validity identity).
        w.u64(s.stateIdentity.primaryStateRevision);
        w.u8(s.stateIdentity.pairedWithSavedState);
        // F3/F4/F5 — destination's own content first (Locked merge rule).
        detail::writeClipsInPlanOrder(w, s.destinationClips);
        // F6 — destination MIDI output channel.
        w.i32(s.destinationMidiOutputChannel);
        // F7/F8/F9 — routed sources strictly in session order; order itself is data.
        w.u32((std::uint32_t)s.sources.size());
        for (const auto& src : s.sources)
        {
            w.i64((std::int64_t)src.trackId);
            w.i32(src.midiOutputChannel);
            w.u8(src.trackOff);
            w.u8(src.muted);
            detail::writeClipsInPlanOrder(w, src.clips);
        }
        // F10 — note-off gate rule inputs.
        w.i32(s.renderConfig.noteOffGateMs);
        w.f64(s.renderConfig.renderSampleRate);
        // F11 — render sample rate, block policy, timeline reference rate (generation identity).
        w.f64(s.renderConfig.renderSampleRate);
        w.i32(s.renderConfig.renderBlockSize);
        w.f64(s.renderConfig.timelineReferenceRate);
        // F12 — tail/latency policy versions; F13 — proxy format/render policy versions.
        w.i32(s.policies.latencyPolicyVersion);
        w.i32(s.policies.tailPolicyVersion);
        w.i32(s.policies.renderPolicyVersion);
        w.i32(s.policies.proxyFormatVersion);
        // Relevant event span + silent-generation eligibility inputs (§15.6/§15.7; PID-007).
        w.i64(s.spanAndSilence.lastRelevantEventReferenceSample);
        w.u8(s.spanAndSilence.hasHostScheduledEvents);
        w.u8(s.spanAndSilence.instrumentClassifiedHostEventDriven);
        w.u8(s.spanAndSilence.silentGenerationEligible);
        return w.out.getMemoryBlock();
    }

    /// The generation identity: "sha256:<hex>" over the canonical bytes. Doubles as the proxy
    /// asset `generationId` (§12.2/§16).
    [[nodiscard]] inline juce::String computeFingerprint(const proxy_snapshot::ProxyRenderSnapshot& s)
    {
        const juce::MemoryBlock bytes = serializeCanonicalFingerprintBytes(s);
        return "sha256:" + juce::SHA256(bytes.getData(), bytes.getSize()).toHexString();
    }
} // namespace proxy_fingerprint
