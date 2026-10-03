// =============================================================================
// LiveMidiInputBus.cpp — see header for the role, time model and realtime contract.
// =============================================================================

#include "engine/LiveMidiInputBus.h"

#include <algorithm>
#include <cmath>

#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"
#include "ui/experimental/ExperimentalMidiChannelDiagnostics.h"

namespace live_midi
{
    namespace
    {
        [[nodiscard]] bool rawCouldEndANote(const std::uint8_t* bytes, const int size) noexcept
        {
            if (size < 3)
            {
                return false;
            }
            const int status = bytes[0] & 0xf0;
            if (status == 0x80)
            {
                return true; // Note Off
            }
            if (status == 0x90 && bytes[2] == 0)
            {
                return true; // Note On velocity 0
            }
            if (status == 0xb0 && bytes[1] == 64 && bytes[2] < 64)
            {
                return true; // sustain released
            }
            return false;
        }

        [[nodiscard]] juce::MidiMessage rawToMessage(const RawEvent& e) noexcept
        {
            switch (e.size)
            {
                case 1: return juce::MidiMessage(e.bytes[0]);
                case 2: return juce::MidiMessage(e.bytes[0], e.bytes[1]);
                default: return juce::MidiMessage(e.bytes[0], e.bytes[1], e.bytes[2]);
            }
        }

        [[nodiscard]] juce::MidiMessage withChannel(const juce::MidiMessage& m, const int channel) noexcept
        {
            juce::MidiMessage copy(m);
            copy.setChannel(channel);
            return copy;
        }
    } // namespace

    LiveMidiInputBus::LiveMidiInputBus()
    {
        for (auto& d : devices_)
        {
            d.buffer.resize((size_t)kDeviceRingCapacity);
        }
        captureBuffer_.resize((size_t)kCaptureRingCapacity);
        for (auto& a : routeActivity_)
        {
            a.store(0, std::memory_order_relaxed);
        }
    }

    // ---------------------------------------------------------------------- pure helpers
    bool LiveMidiInputBus::isForwardedMessage(const juce::MidiMessage& m) noexcept
    {
        if (m.getRawDataSize() < 1 || m.getRawDataSize() > 3 || m.isSysEx() || m.isMetaEvent())
        {
            return false;
        }
        const int status = m.getRawData()[0];
        if (status >= 0xf0)
        {
            return false; // system common / realtime
        }
        if (m.isProgramChange())
        {
            return false; // never let a keyboard preset button re-program the instrument
        }
        return true;
    }

    bool LiveMidiInputBus::isCapturedMessage(const juce::MidiMessage& m) noexcept
    {
        return m.isNoteOnOrOff() || m.isController() || m.isPitchWheel();
    }

    int LiveMidiInputBus::offsetForTimestamp(const double timeStampSeconds, const double previousCallbackMs,
                                             const double nowMs, const int numSamples) noexcept
    {
        if (numSamples <= 1)
        {
            return 0;
        }
        const double windowMs = nowMs - previousCallbackMs;
        if (!(windowMs > 0.0) || !std::isfinite(windowMs))
        {
            return 0;
        }
        const double tMs = timeStampSeconds * 1000.0;
        const double frac = (tMs - previousCallbackMs) / windowMs; // 0 … 1 inside the window
        const double pos = frac * (double)numSamples;
        if (!std::isfinite(pos))
        {
            return 0;
        }
        return (int)juce::jlimit(0.0, (double)(numSamples - 1), std::floor(pos + 1.0e-6));
    }

    namespace
    {
        /// Samples the gesture lies before the callback entry (lookback clamped to 2 s: a gesture
        /// is never attributed further into the past — clock-skew guard — nor into the future).
        [[nodiscard]] std::int64_t lookbackSamples(const double timeStampSeconds, const BlockContext& ctx) noexcept
        {
            const double agoMs = ctx.nowMs - timeStampSeconds * 1000.0;
            const double clampedAgoMs = juce::jlimit(0.0, 2000.0, std::isfinite(agoMs) ? agoMs : 0.0);
            return (std::int64_t)std::llround(clampedAgoMs * 0.001 * ctx.sampleRate);
        }
    } // namespace

    std::int64_t LiveMidiInputBus::timelineSampleForTimestamp(const double timeStampSeconds,
                                                              const BlockContext& ctx) noexcept
    {
        if (!ctx.transportPlaying)
        {
            return ctx.playheadAtBlockStart;
        }
        return ctx.playheadAtBlockStart - lookbackSamples(timeStampSeconds, ctx) + ctx.recordPlacementOffsetSamples;
    }

    std::int64_t LiveMidiInputBus::monoSampleForTimestamp(const double timeStampSeconds,
                                                          const BlockContext& ctx) noexcept
    {
        const std::int64_t mono = ctx.monoSampleAtBlockStart - lookbackSamples(timeStampSeconds, ctx)
                                  + ctx.recordPlacementOffsetSamples;
        return mono < 0 ? std::int64_t{ 0 } : mono;
    }

    // ---------------------------------------------------------------------- timeline anchors
    void LiveMidiInputBus::audioThread_pushAnchor(const TimelineAnchor& a) noexcept
    {
        if (anchorCount_ > 0 && anchors_[(size_t)anchorHead_].monoSample == a.monoSample)
        {
            anchors_[(size_t)anchorHead_] = a; // same instant: the later decision replaces the earlier
            return;
        }
        anchorHead_ = anchorCount_ == 0 ? 0 : (anchorHead_ + 1) % kMaxAnchors;
        anchors_[(size_t)anchorHead_] = a;
        anchorCount_ = anchorCount_ < kMaxAnchors ? anchorCount_ + 1 : kMaxAnchors;
    }

    void LiveMidiInputBus::audioThread_updateAnchorsForBlock(const BlockContext& ctx) noexcept
    {
        const std::int64_t mono0 = ctx.monoSampleAtBlockStart;
        const std::int64_t t0 = ctx.playheadAtBlockStart;
        bool needAnchor = anchorCount_ == 0;
        if (!needAnchor)
        {
            if (ctx.transportPlaying)
            {
                // Continuous playback predicts t0 from the previous block's end; a seek, a play
                // start or a gap (blocks without dispatch) breaks the prediction.
                const bool continues = lastBlockPlaying_ && lastBlockMonoEnd_ == mono0
                                       && lastBlockTimelineEnd_ == t0;
                needAnchor = !continues;
            }
            else
            {
                // Stopped: an anchor when playback just ended or the stopped playhead moved (seek).
                needAnchor = lastBlockPlaying_ || anchors_[(size_t)anchorHead_].timelineSample != t0;
            }
        }
        if (needAnchor)
        {
            audioThread_pushAnchor({ mono0, t0, ctx.transportPlaying });
        }
        lastBlockPlaying_ = ctx.transportPlaying;
        lastBlockMonoEnd_ = mono0 + ctx.numSamples;
        lastBlockTimelineEnd_ = ctx.transportPlaying ? t0 + ctx.numSamples : t0;
    }

    bool LiveMidiInputBus::audioThread_mapMonoToTimeline(const std::int64_t monoSample,
                                                         std::int64_t& outTimeline,
                                                         bool& outPlaying) const noexcept
    {
        if (anchorCount_ == 0)
        {
            return false;
        }
        // Newest first: the first anchor at or before the mono position is the mapping in force.
        for (int i = 0; i < anchorCount_; ++i)
        {
            const int idx = ((anchorHead_ - i) % kMaxAnchors + kMaxAnchors) % kMaxAnchors;
            const TimelineAnchor& a = anchors_[(size_t)idx];
            if (a.monoSample <= monoSample)
            {
                outPlaying = a.playing;
                outTimeline = a.playing ? a.timelineSample + (monoSample - a.monoSample) : a.timelineSample;
                return true;
            }
        }
        // Older than every anchor we still remember: use the oldest one (bounded history).
        const int oldest = ((anchorHead_ - (anchorCount_ - 1)) % kMaxAnchors + kMaxAnchors) % kMaxAnchors;
        const TimelineAnchor& a = anchors_[(size_t)oldest];
        outPlaying = a.playing;
        outTimeline = a.playing ? a.timelineSample - (a.monoSample - monoSample) : a.timelineSample;
        return true;
    }

    void LiveMidiInputBus::audioThread_noteCycleWrap(const std::int64_t monoSampleAtWrap,
                                                     const std::int64_t loopStartSample,
                                                     const std::uint32_t wrapSerial) noexcept
    {
        audioThread_pushAnchor({ monoSampleAtWrap, loopStartSample, true });
        // The block that wrapped continues at the loop start: keep the continuity prediction for
        // the next block consistent with the jump (its end is loopStart + samples after the wrap).
        if (lastBlockMonoEnd_ >= monoSampleAtWrap)
        {
            lastBlockTimelineEnd_ = loopStartSample + (lastBlockMonoEnd_ - monoSampleAtWrap);
        }
        CapturedEvent marker;
        marker.trackId = kInvalidTrackId;
        marker.monoSample = monoSampleAtWrap;
        marker.timelineSample = loopStartSample;
        marker.transportPlaying = true;
        marker.wrapMarker = true;
        marker.wrapSerial = wrapSerial;
        audioThread_pushCaptured(marker);
    }

    // ---------------------------------------------------------------------- device threads
    void LiveMidiInputBus::deviceThread_push(const int slot, const juce::MidiMessage& message) noexcept
    {
        if (slot < 0 || slot >= kMaxDeviceSlots)
        {
            return;
        }
        if (!isForwardedMessage(message))
        {
            droppedUnsupported_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        DeviceRing& ring = devices_[(size_t)slot];
        const int size = message.getRawDataSize();
        const std::uint8_t* raw = message.getRawData();

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        ring.fifo.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 + size2 < 1)
        {
            ring.overflow.fetch_add(1, std::memory_order_relaxed);
            if (rawCouldEndANote(raw, size))
            {
                ring.noteOffLost.store(true, std::memory_order_relaxed);
            }
            return;
        }
        RawEvent& e = ring.buffer[(size_t)start1];
        e.timeStampSeconds = message.getTimeStamp();
        e.size = (std::uint8_t)size;
        for (int i = 0; i < 3; ++i)
        {
            e.bytes[i] = i < size ? raw[i] : (std::uint8_t)0;
        }
        ring.fifo.finishedWrite(1);
    }

    // ---------------------------------------------------------------------- message thread
    void LiveMidiInputBus::publishRouting(std::shared_ptr<const RoutingSnapshot> snapshot) noexcept
    {
        std::atomic_store_explicit(&routing_, std::move(snapshot), std::memory_order_release);
    }

    std::shared_ptr<const RoutingSnapshot> LiveMidiInputBus::currentRouting() const noexcept
    {
        return std::atomic_load_explicit(&routing_, std::memory_order_acquire);
    }

    bool LiveMidiInputBus::popCaptured(CapturedEvent& out) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        captureFifo_.prepareToRead(1, start1, size1, start2, size2);
        if (size1 + size2 < 1)
        {
            return false;
        }
        out = captureBuffer_[(size_t)start1];
        captureFifo_.finishedRead(1);
        return true;
    }

    std::uint32_t LiveMidiInputBus::routeActivityCount(const int routeIndex) const noexcept
    {
        if (routeIndex < 0 || routeIndex >= kMaxRoutes)
        {
            return 0;
        }
        return routeActivity_[(size_t)routeIndex].load(std::memory_order_relaxed);
    }

    std::uint32_t LiveMidiInputBus::deviceOverflowCount(const int slot) const noexcept
    {
        if (slot < 0 || slot >= kMaxDeviceSlots)
        {
            return 0;
        }
        return devices_[(size_t)slot].overflow.load(std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------------- audio thread
    LiveMidiInputBus::ResolvedDestination LiveMidiInputBus::resolveDestination(const TrackId sourceTrackId,
                                                                               const BlockContext& ctx) noexcept
    {
        ResolvedDestination out;
        if (ctx.session == nullptr || sourceTrackId == kInvalidTrackId)
        {
            return out;
        }
        const int ix = ctx.session->findTrackIndexById(sourceTrackId);
        if (ix < 0)
        {
            return out;
        }
        const Track& tr = ctx.session->getTrack(ix);
        out.trackPresent = true;
        out.outputChannelSetting = tr.getMidiOutputChannel();
        TrackId destId = kInvalidTrackId;
        if (tr.getKind() == TrackKind::Instrument)
        {
            destId = sourceTrackId;
        }
        else if (tr.getKind() == TrackKind::Midi)
        {
            destId = tr.getMidiDestinationTrackId(); // `MIDI To`, re-read every block like playback
        }
        if (destId == kInvalidTrackId || ctx.instruments == nullptr)
        {
            return out;
        }
        for (const auto& e : ctx.instruments->entries)
        {
            if (e.trackId == destId)
            {
                out.host = e.host;
                break;
            }
        }
        return out;
    }

    bool LiveMidiInputBus::hostIsInSnapshot(const ExperimentalInstrumentHost* host,
                                            const ExperimentalInstrumentPlaybackSnapshot* instruments) noexcept
    {
        if (host == nullptr || instruments == nullptr)
        {
            return false;
        }
        for (const auto& e : instruments->entries)
        {
            if (e.host == host || e.auditionHost == host)
            {
                return true;
            }
        }
        return false;
    }

    void LiveMidiInputBus::audioThread_rememberNoteOn(const TrackId trackId, ExperimentalInstrumentHost* host,
                                                      const int effectiveChannel, const int receivedChannel,
                                                      const int note) noexcept
    {
        // Same (track, received channel, note) already sounding: a retrigger — keep ONE owner so
        // the next Note Off ends the note (plugins treat the second Note On as a restart).
        for (auto& ln : liveNotes_)
        {
            if (ln.active && ln.trackId == trackId && ln.receivedChannel == receivedChannel && ln.note == note)
            {
                ln.host = host;
                ln.effectiveChannel = (std::uint8_t)effectiveChannel;
                return;
            }
        }
        for (auto& ln : liveNotes_)
        {
            if (!ln.active)
            {
                ln.active = true;
                ln.trackId = trackId;
                ln.host = host;
                ln.effectiveChannel = (std::uint8_t)effectiveChannel;
                ln.receivedChannel = (std::uint8_t)receivedChannel;
                ln.note = (std::uint8_t)note;
                liveNoteCount_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        // Table full: the note still sounds in the host, we simply cannot track it (bounded).
    }

    bool LiveMidiInputBus::audioThread_takeNoteOff(const TrackId trackId, const int receivedChannel, const int note,
                                                   ExperimentalInstrumentHost*& outHost,
                                                   int& outEffectiveChannel) noexcept
    {
        for (auto& ln : liveNotes_)
        {
            if (ln.active && ln.trackId == trackId && ln.receivedChannel == receivedChannel && ln.note == note)
            {
                outHost = ln.host;
                outEffectiveChannel = ln.effectiveChannel;
                ln.active = false;
                liveNoteCount_.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    void LiveMidiInputBus::audioThread_pushCaptured(const CapturedEvent& e) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        captureFifo_.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 + size2 < 1)
        {
            captureOverflow_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        captureBuffer_[(size_t)start1] = e;
        captureFifo_.finishedWrite(1);
    }

    void LiveMidiInputBus::audioThread_releaseNotesForRoute(const TrackId trackId,
                                                            const ExperimentalInstrumentPlaybackSnapshot* instruments,
                                                            DeliverFn deliver, void* deliverContext) noexcept
    {
        for (auto& ln : liveNotes_)
        {
            if (!ln.active || ln.trackId != trackId)
            {
                continue;
            }
            if (deliver != nullptr && hostIsInSnapshot(ln.host, instruments))
            {
                deliver(deliverContext, ln.host, 0, juce::MidiMessage::noteOff((int)ln.effectiveChannel, (int)ln.note));
                deliveredEvents_.fetch_add(1, std::memory_order_relaxed);
            }
            ln.active = false;
            liveNoteCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void LiveMidiInputBus::audioThread_releaseAllLiveNotes(const ExperimentalInstrumentPlaybackSnapshot* instruments,
                                                           DeliverFn deliver, void* deliverContext) noexcept
    {
        for (auto& ln : liveNotes_)
        {
            if (!ln.active)
            {
                continue;
            }
            if (deliver != nullptr && hostIsInSnapshot(ln.host, instruments))
            {
                deliver(deliverContext, ln.host, 0, juce::MidiMessage::noteOff((int)ln.effectiveChannel, (int)ln.note));
                deliveredEvents_.fetch_add(1, std::memory_order_relaxed);
            }
            ln.active = false;
            liveNoteCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void LiveMidiInputBus::audioThread_discardPendingAndForgetNotes() noexcept
    {
        for (auto& d : devices_)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            d.fifo.prepareToRead(kDeviceRingCapacity, start1, size1, start2, size2);
            if (size1 + size2 > 0)
            {
                d.fifo.finishedRead(size1 + size2);
            }
            d.overflowSeenByAudio = d.overflow.load(std::memory_order_relaxed);
            d.noteOffLost.store(false, std::memory_order_relaxed);
        }
        for (auto& ln : liveNotes_)
        {
            ln.active = false;
        }
        liveNoteCount_.store(0, std::memory_order_relaxed);
        previousCallbackMs_ = 0.0;
    }

    void LiveMidiInputBus::audioThread_reconcileLiveNotes(const RoutingSnapshot* routing, const BlockContext& ctx,
                                                          DeliverFn deliver, void* deliverContext) noexcept
    {
        // Every block (cheap: few live notes): a note keeps sounding only while its row still
        // monitors AND still resolves to the host that received the Note On. Otherwise the Note
        // Off goes to the ORIGINAL host (if it is still in the snapshot) — targeted, never global.
        for (auto& ln : liveNotes_)
        {
            if (!ln.active)
            {
                continue;
            }
            const Route* route = nullptr;
            if (routing != nullptr)
            {
                for (const auto& r : routing->routes)
                {
                    if (r.trackId == ln.trackId)
                    {
                        route = &r;
                        break;
                    }
                }
            }
            bool stillValid = route != nullptr && route->monitor && route->deviceSlot != kDeviceSlotMissing;
            if (stillValid)
            {
                const ResolvedDestination dest = resolveDestination(ln.trackId, ctx);
                stillValid = dest.host != nullptr && dest.host == ln.host;
            }
            if (stillValid)
            {
                continue;
            }
            if (deliver != nullptr && hostIsInSnapshot(ln.host, ctx.instruments))
            {
                deliver(deliverContext, ln.host, 0, juce::MidiMessage::noteOff((int)ln.effectiveChannel, (int)ln.note));
                deliveredEvents_.fetch_add(1, std::memory_order_relaxed);
            }
            ln.active = false;
            liveNoteCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void LiveMidiInputBus::audioThread_dispatch(const BlockContext& ctx, DeliverFn deliver, void* deliverContext) noexcept
    {
        const std::shared_ptr<const RoutingSnapshot> routingHold
            = std::atomic_load_explicit(&routing_, std::memory_order_acquire);
        const RoutingSnapshot* const routing = routingHold.get();

        audioThread_updateAnchorsForBlock(ctx);
        audioThread_reconcileLiveNotes(routing, ctx, deliver, deliverContext);

        const double previousMs = previousCallbackMs_;
        previousCallbackMs_ = ctx.nowMs;

        // Recorded position of a gesture: mono clock first, then the anchor mapping (exact across
        // wraps / seeks / stops); the single-segment projection only before any anchor exists.
        const auto recordedPosition = [this, &ctx](const double timeStampSeconds, std::int64_t& outMono,
                                                   std::int64_t& outTimeline, bool& outPlaying) noexcept {
            outMono = monoSampleForTimestamp(timeStampSeconds, ctx);
            if (!audioThread_mapMonoToTimeline(outMono, outTimeline, outPlaying))
            {
                outTimeline = timelineSampleForTimestamp(timeStampSeconds, ctx);
                outPlaying = ctx.transportPlaying;
            }
        };

        const int numRoutes = routing != nullptr ? (int)routing->routes.size() : 0;

        for (int slot = 0; slot < kMaxDeviceSlots; ++slot)
        {
            DeviceRing& ring = devices_[(size_t)slot];

            // Overflow since the previous block: release this device's live notes (a lost Note Off
            // must never leave a tone hanging) and mark captured rows.
            const std::uint32_t overflowNow = ring.overflow.load(std::memory_order_relaxed);
            if (overflowNow != ring.overflowSeenByAudio)
            {
                ring.overflowSeenByAudio = overflowNow;
                // Whether or not the dropped messages included a release, the sounding state of this
                // device's rows is unknown now — release them all (targeted per row, not global).
                (void)ring.noteOffLost.exchange(false, std::memory_order_relaxed);
                for (int ri = 0; ri < numRoutes; ++ri)
                {
                    const Route& r = routing->routes[(size_t)ri];
                    if (r.deviceSlot != slot && r.deviceSlot != kDeviceSlotAll)
                    {
                        continue;
                    }
                    audioThread_releaseNotesForRoute(r.trackId, ctx.instruments, deliver, deliverContext);
                    if (r.capture)
                    {
                        CapturedEvent marker;
                        marker.trackId = r.trackId;
                        marker.timelineSample = ctx.playheadAtBlockStart;
                        marker.monoSample = ctx.monoSampleAtBlockStart;
                        marker.timeStampSeconds = ctx.nowMs * 0.001;
                        marker.transportPlaying = ctx.transportPlaying;
                        marker.overflowMarker = true;
                        audioThread_pushCaptured(marker);
                    }
                }
            }

            int budget = kMaxEventsPerDevicePerBlock;
            while (budget-- > 0)
            {
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                ring.fifo.prepareToRead(1, start1, size1, start2, size2);
                if (size1 + size2 < 1)
                {
                    break;
                }
                const RawEvent raw = ring.buffer[(size_t)start1];
                ring.fifo.finishedRead(1);

                const juce::MidiMessage received = rawToMessage(raw);
                const int receivedChannel = received.getChannel(); // 0 for non-channel messages
                if (receivedChannel < 1 || numRoutes == 0)
                {
                    continue;
                }
                const int offset = offsetForTimestamp(raw.timeStampSeconds, previousMs, ctx.nowMs, ctx.numSamples);
                std::int64_t monoSample = 0;
                std::int64_t timelineSample = 0;
                bool gesturePlaying = false;
                recordedPosition(raw.timeStampSeconds, monoSample, timelineSample, gesturePlaying);
                const bool isNoteOff = received.isNoteOff(true);
                const bool isNoteOn = received.isNoteOn(false) && !isNoteOff;

                for (int ri = 0; ri < numRoutes; ++ri)
                {
                    const Route& r = routing->routes[(size_t)ri];
                    if (r.deviceSlot != slot && r.deviceSlot != kDeviceSlotAll)
                    {
                        continue;
                    }
                    if (r.channelFilter != kTrackMidiInputChannelAll && r.channelFilter != receivedChannel)
                    {
                        continue;
                    }
                    if (ri < kMaxRoutes)
                    {
                        routeActivity_[(size_t)ri].fetch_add(1, std::memory_order_relaxed);
                    }

                    if (r.monitor && deliver != nullptr)
                    {
                        if (isNoteOff)
                        {
                            // Ownership rule: the Note Off goes where the Note On went.
                            ExperimentalInstrumentHost* owner = nullptr;
                            int ownerChannel = 1;
                            if (audioThread_takeNoteOff(r.trackId, receivedChannel, received.getNoteNumber(), owner, ownerChannel)
                                && hostIsInSnapshot(owner, ctx.instruments))
                            {
                                deliver(deliverContext, owner, offset,
                                        juce::MidiMessage::noteOff(ownerChannel, received.getNoteNumber(),
                                                                   (juce::uint8)received.getVelocity()));
                                deliveredEvents_.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                        else
                        {
                            const ResolvedDestination dest = resolveDestination(r.trackId, ctx);
                            if (dest.host != nullptr)
                            {
                                const int effCh = midi_channel_diag::effectiveChannel(receivedChannel, dest.outputChannelSetting);
                                deliver(deliverContext, dest.host, offset, withChannel(received, effCh));
                                deliveredEvents_.fetch_add(1, std::memory_order_relaxed);
                                if (isNoteOn)
                                {
                                    audioThread_rememberNoteOn(r.trackId, dest.host, effCh, receivedChannel,
                                                               received.getNoteNumber());
                                }
                            }
                        }
                    }

                    if (r.capture && isCapturedMessage(received))
                    {
                        CapturedEvent ce;
                        ce.trackId = r.trackId;
                        ce.timelineSample = timelineSample;
                        ce.monoSample = monoSample;
                        ce.timeStampSeconds = raw.timeStampSeconds;
                        ce.size = raw.size;
                        ce.bytes[0] = raw.bytes[0];
                        ce.bytes[1] = raw.bytes[1];
                        ce.bytes[2] = raw.bytes[2];
                        ce.transportPlaying = gesturePlaying;
                        audioThread_pushCaptured(ce);
                    }
                }
            }
        }
    }
} // namespace live_midi
