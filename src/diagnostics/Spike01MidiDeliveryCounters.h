// ============================================================================
// SPIKE-01B-M DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE.
// Removable with the spike (see docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md §28.8).
// ============================================================================
//
// MIDI-delivery counters for the M2V measurement: classifies every event in the merged
// per-block `juce::MidiBuffer` that the instrument-processing boundary hands to the plugin.
//
// Real-time safety contract (the caller runs on the audio device callback thread):
//   * raw-byte classification of `meta.data` — NO `juce::MidiMessage` is constructed, so no
//     heap allocation is possible even for SysEx;
//   * fixed-size std::atomic counters only (relaxed ordering; single audio-thread writer,
//     message-thread reader) — no locks, no file I/O, no dynamic storage;
//   * `summary()` is message-thread-only (it builds a juce::String).
//
// Classification matches juce::MidiMessage semantics for 3-byte channel messages:
//   note-on  = 0x9n with velocity > 0
//   note-off = 0x8n, or 0x9n with velocity 0   (JUCE isNoteOff(true))
//   cc       = 0xBn
// Events without a MIDI channel (status >= 0xF0: system/SysEx) count as `other` and
// `channelless`, keeping the reconciliation identity exact:
//   noteOn + noteOff + cc + other == sum(channelHist[0..15]) + channelless
//
// This header is standalone (only juce_audio_basics) so the deterministic selftests can feed
// synthetic buffers through the exact classification code used in the measurement.

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>

namespace spike01
{
    struct MidiDeliveryCounters
    {
        std::atomic<std::uint64_t> blocks{ 0 };
        std::atomic<std::uint64_t> blocksWithMidi{ 0 };
        std::atomic<std::uint64_t> noteOn{ 0 };
        std::atomic<std::uint64_t> noteOff{ 0 };
        std::atomic<std::uint64_t> cc{ 0 };
        std::atomic<std::uint64_t> cc11{ 0 };
        std::atomic<std::uint64_t> other{ 0 };
        std::atomic<std::uint64_t> channelless{ 0 };
        std::atomic<std::uint64_t> channelHist[16];
        std::atomic<std::int64_t> absSample{ 0 };     // samples processed since install
        std::atomic<std::int64_t> firstEventAbs{ -1 }; // abs sample of first/last observed event
        std::atomic<std::int64_t> lastEventAbs{ -1 };

        MidiDeliveryCounters() noexcept
        {
            for (auto& c : channelHist)
            {
                c.store(0, std::memory_order_relaxed);
            }
        }

        /// [Audio/offline render thread] Classify one merged per-block buffer. RT-safe (see top).
        void countBlock(const juce::MidiBuffer& merged, const int numSamples) noexcept
        {
            const std::int64_t base
                = absSample.fetch_add((std::int64_t) numSamples, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
            bool any = false;
            for (const auto meta : merged)
            {
                any = true;
                const juce::uint8* const d = meta.data;
                const int n = meta.numBytes;
                const juce::uint8 status = n >= 1 ? d[0] : (juce::uint8) 0;
                const juce::uint8 type = (juce::uint8) (status & 0xF0);
                const std::int64_t pos = base + meta.samplePosition;

                std::int64_t f = firstEventAbs.load(std::memory_order_relaxed);
                if (f < 0)
                {
                    firstEventAbs.compare_exchange_strong(f, pos, std::memory_order_relaxed);
                }
                std::int64_t l = lastEventAbs.load(std::memory_order_relaxed);
                while (pos > l
                       && !lastEventAbs.compare_exchange_weak(l, pos, std::memory_order_relaxed))
                {
                }

                if (type == 0x90 && n >= 3 && d[2] > 0)
                {
                    noteOn.fetch_add(1, std::memory_order_relaxed);
                }
                else if (type == 0x80 || (type == 0x90 && n >= 3 && d[2] == 0))
                {
                    noteOff.fetch_add(1, std::memory_order_relaxed);
                }
                else if (type == 0xB0 && n >= 3)
                {
                    cc.fetch_add(1, std::memory_order_relaxed);
                    if (d[1] == 11)
                    {
                        cc11.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else
                {
                    other.fetch_add(1, std::memory_order_relaxed);
                }

                if (status >= 0x80 && status < 0xF0)
                {
                    channelHist[status & 0x0F].fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    channelless.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (any)
            {
                blocksWithMidi.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /// [Message thread] Human-readable snapshot for the sanitized session log.
        [[nodiscard]] juce::String summary() const
        {
            juce::String hist;
            for (int i = 0; i < 16; ++i)
            {
                const auto n = channelHist[i].load(std::memory_order_relaxed);
                if (n > 0)
                {
                    hist << "ch" << (i + 1) << "=" << (juce::int64) n << " ";
                }
            }
            return "blocks=" + juce::String((juce::int64) blocks.load())
                   + " blocksWithMidi=" + juce::String((juce::int64) blocksWithMidi.load())
                   + " noteOn=" + juce::String((juce::int64) noteOn.load())
                   + " noteOff=" + juce::String((juce::int64) noteOff.load())
                   + " cc=" + juce::String((juce::int64) cc.load())
                   + " cc11=" + juce::String((juce::int64) cc11.load())
                   + " other=" + juce::String((juce::int64) other.load())
                   + " channelless=" + juce::String((juce::int64) channelless.load())
                   + " firstEventAbs=" + juce::String((juce::int64) firstEventAbs.load())
                   + " lastEventAbs=" + juce::String((juce::int64) lastEventAbs.load())
                   + " channels[" + hist.trim() + "]";
        }
    };
} // namespace spike01
