// =============================================================================
// LiveMidiRecordingFocusedTests — live MIDI input, monitoring routes, take capture and persistence
// =============================================================================
//
// Deterministic, device-free regression of the live-MIDI slice through the PRODUCTION code:
//   * `live_midi::LiveMidiInputBus` — the realtime core: device-thread push, routing match
//     (device slot / channel filter), delivery with sample offsets and the row's output mapping
//     (Preserve / Force), live-note ownership across setting changes, Monitor off, device loss,
//     overflow handling, the capture ring stamps, the two time mappings (live offset vs recorded
//     timeline position with output-latency compensation);
//   * `live_midi_take::buildTakePattern` — note pairing, velocity 0 = Note Off, held keys at the
//     start boundary, controller / pitch-bend state restated at tick 0, notes and sustain closed
//     at the stop boundary, empty vs controller-only takes, 44.1 / 48 kHz and 120 / 180 BPM;
//   * project file v24 round trip of the per-track MIDI input assignment and clip pitch bend;
//   * MIDI export of pitch bend (raw 14-bit values, channel contract).
//
// Exit 0 = all checks green.
// =============================================================================

#include "app/LiveMidiTakeBuilder.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/LiveMidiInputBus.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "io/InstrumentMidiClipExport.h"
#include "io/ProjectFile.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
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

// ----------------------------------------------------------------------------- delivery recorder
struct Delivered
{
    ExperimentalInstrumentHost* host = nullptr;
    int offset = 0;
    juce::MidiMessage msg;
};
std::vector<Delivered> g_delivered;

void recordDelivery(void*, ExperimentalInstrumentHost* host, const int offset, const juce::MidiMessage& m) noexcept
{
    g_delivered.push_back({ host, offset, m });
}

// Fake host identities: the bus never dereferences hosts, it only hands pointers through.
ExperimentalInstrumentHost* const kHostA = reinterpret_cast<ExperimentalInstrumentHost*>(0x1000);
ExperimentalInstrumentHost* const kHostB = reinterpret_cast<ExperimentalInstrumentHost*>(0x2000);

constexpr TrackId kInstrumentTrack = 10;
constexpr TrackId kMidiLower = 11; // MIDI To → instrument, output Force 2
constexpr TrackId kMidiPedal = 12; // MIDI To → instrument, output Force 3

[[nodiscard]] std::shared_ptr<const SessionSnapshot> makeSession(const int instrumentOutputChannel = kTrackMidiOutputChannelAny)
{
    auto s = SessionSnapshot::withSingleEmptyTrack(1, "Audio");
    s = SessionSnapshot::withTrackAdded(*s, kInstrumentTrack, "VB3", TrackKind::Instrument);
    s = SessionSnapshot::withTrackAdded(*s, kMidiLower, "Lower", TrackKind::Midi);
    s = SessionSnapshot::withTrackAdded(*s, kMidiPedal, "Pedal", TrackKind::Midi);
    s = SessionSnapshot::withTrackMidiOutputChannel(*s, kInstrumentTrack, instrumentOutputChannel);
    s = SessionSnapshot::withTrackMidiDestination(*s, kMidiLower, kInstrumentTrack);
    s = SessionSnapshot::withTrackMidiOutputChannel(*s, kMidiLower, 2);
    s = SessionSnapshot::withTrackMidiDestination(*s, kMidiPedal, kInstrumentTrack);
    s = SessionSnapshot::withTrackMidiOutputChannel(*s, kMidiPedal, 3);
    return s;
}

[[nodiscard]] std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> makeInstruments(ExperimentalInstrumentHost* host)
{
    std::vector<ExperimentalInstrumentPlaybackEntry> entries;
    ExperimentalInstrumentPlaybackEntry e;
    e.trackId = kInstrumentTrack;
    e.host = host;
    entries.push_back(e);
    return std::make_shared<ExperimentalInstrumentPlaybackSnapshot>(std::move(entries));
}

[[nodiscard]] std::shared_ptr<const live_midi::RoutingSnapshot> makeRouting(const bool monitorInstrument,
                                                                           const bool monitorLower,
                                                                           const int instrumentSlot = 0,
                                                                           const bool captureAll = true)
{
    auto r = std::make_shared<live_midi::RoutingSnapshot>();
    live_midi::Route a;
    a.trackId = kInstrumentTrack;
    a.deviceSlot = instrumentSlot;
    a.channelFilter = kTrackMidiInputChannelAll;
    a.monitor = monitorInstrument;
    a.capture = captureAll;
    r->routes.push_back(a);
    live_midi::Route b;
    b.trackId = kMidiLower;
    b.deviceSlot = 0;
    b.channelFilter = 5;
    b.monitor = monitorLower;
    b.capture = captureAll;
    r->routes.push_back(b);
    live_midi::Route c;
    c.trackId = kMidiPedal;
    c.deviceSlot = live_midi::kDeviceSlotAll;
    c.channelFilter = 6;
    c.monitor = true;
    c.capture = false;
    r->routes.push_back(c);
    r->revision = 1;
    return r;
}

[[nodiscard]] juce::MidiMessage stamped(juce::MidiMessage m, const double seconds)
{
    m.setTimeStamp(seconds);
    return m;
}

[[nodiscard]] live_midi::BlockContext makeContext(const SessionSnapshot* session,
                                                 const ExperimentalInstrumentPlaybackSnapshot* instruments,
                                                 const double nowMs,
                                                 const bool playing,
                                                 const std::int64_t playhead,
                                                 const std::int64_t placementOffset = 0)
{
    live_midi::BlockContext ctx;
    ctx.session = session;
    ctx.instruments = instruments;
    ctx.numSamples = 256;
    ctx.sampleRate = 48000.0;
    ctx.nowMs = nowMs;
    ctx.playheadAtBlockStart = playhead;
    ctx.transportPlaying = playing;
    ctx.recordPlacementOffsetSamples = placementOffset;
    return ctx;
}

int countDelivered(const int channel, const int note, const bool noteOn)
{
    int n = 0;
    for (const auto& d : g_delivered)
    {
        if (d.msg.getChannel() == channel && d.msg.getNoteNumber() == note
            && ((noteOn && d.msg.isNoteOn()) || (!noteOn && d.msg.isNoteOff())))
        {
            ++n;
        }
    }
    return n;
}

// ----------------------------------------------------------------------------- bus tests
void testTimeMappings()
{
    using live_midi::LiveMidiInputBus;
    // Live offset: previous callback at 1000 ms, now 1005.333 ms (256 @ 48 kHz) → proportional.
    const double prevMs = 1000.0;
    const double nowMs = 1000.0 + 256.0 / 48.0;
    expect(LiveMidiInputBus::offsetForTimestamp(prevMs * 0.001, prevMs, nowMs, 256) == 0, "offset: at window start = 0");
    expect(std::abs(LiveMidiInputBus::offsetForTimestamp((prevMs + 256.0 / 48.0 * 0.5) * 0.001, prevMs, nowMs, 256) - 128) <= 1,
           "offset: mid-window = numSamples/2 (±1)");
    expect(LiveMidiInputBus::offsetForTimestamp((nowMs + 50.0) * 0.001, prevMs, nowMs, 256) == 255,
           "offset: after the window clamps to the last sample");
    expect(LiveMidiInputBus::offsetForTimestamp((prevMs - 50.0) * 0.001, prevMs, nowMs, 256) == 0,
           "offset: before the window clamps to 0");
    expect(LiveMidiInputBus::offsetForTimestamp(1.0, 0.0, 0.0, 256) == 0, "offset: unknown previous callback → 0");
    // Other block sizes / rates: 1024 @ 44.1 kHz (23.2 ms window) and 64 @ 96 kHz (0.67 ms).
    {
        const double w1024 = 1024.0 / 44.1;
        const int o = LiveMidiInputBus::offsetForTimestamp((prevMs + w1024 * 0.25) * 0.001, prevMs, prevMs + w1024, 1024);
        expect(std::abs(o - 256) <= 1, "offset: quarter of a 1024-sample window @ 44.1 kHz = 256 (±1)");
        const double w64 = 64.0 / 96.0;
        const int o2 = LiveMidiInputBus::offsetForTimestamp((prevMs + w64 * 0.75) * 0.001, prevMs, prevMs + w64, 64);
        expect(std::abs(o2 - 48) <= 1, "offset: three quarters of a 64-sample window @ 96 kHz = 48 (±1)");
        live_midi::BlockContext c441 = makeContext(nullptr, nullptr, 7000.0, true, 441000, -256);
        c441.sampleRate = 44100.0;
        c441.numSamples = 1024;
        expect(LiveMidiInputBus::timelineSampleForTimestamp((7000.0 - 100.0) * 0.001, c441) == 441000 - 4410 - 256,
               "timeline: 100 ms ago @ 44.1 kHz = 4410 samples back, minus a 256-sample output latency");
    }

    // Recorded position: 20 ms ago at 48 kHz = 960 samples back; output latency 128 subtracted.
    live_midi::BlockContext ctx = makeContext(nullptr, nullptr, 5000.0, true, 480000, -128);
    expect(LiveMidiInputBus::timelineSampleForTimestamp((5000.0 - 20.0) * 0.001, ctx) == 480000 - 960 - 128,
           "timeline: gesture 20 ms before the callback lands 960 samples back, minus output latency");
    expect(LiveMidiInputBus::timelineSampleForTimestamp(5000.0 * 0.001, ctx) == 480000 - 128,
           "timeline: gesture at callback entry = block start minus output latency");
    expect(LiveMidiInputBus::timelineSampleForTimestamp((5000.0 + 10.0) * 0.001, ctx) == 480000 - 128,
           "timeline: future timestamps clamp to the block start (never ahead)");
    expect(LiveMidiInputBus::timelineSampleForTimestamp((5000.0 - 9000.0) * 0.001, ctx) == 480000 - 96000 - 128,
           "timeline: clock-skew guard clamps to 2 s back");
    ctx.transportPlaying = false;
    expect(LiveMidiInputBus::timelineSampleForTimestamp(4.9, ctx) == 480000,
           "timeline: stopped transport reports the stopped playhead");

    expect(LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)), "forward: note on");
    expect(LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::controllerEvent(1, 64, 127)), "forward: CC");
    expect(LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::pitchWheel(1, 9000)), "forward: pitch wheel");
    expect(LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::channelPressureChange(1, 50)),
           "forward: channel pressure passes through for monitoring");
    expect(!LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::programChange(1, 5)), "drop: program change");
    const juce::uint8 sysex[] = { 0x7e, 0x7f, 0x09, 0x01 };
    expect(!LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::createSysExMessage(sysex, 4)), "drop: SysEx");
    expect(!LiveMidiInputBus::isForwardedMessage(juce::MidiMessage::midiClock()), "drop: realtime clock");
    expect(!LiveMidiInputBus::isCapturedMessage(juce::MidiMessage::channelPressureChange(1, 50)),
           "capture: aftertouch is monitored but not recorded (documented limitation)");
}

void testRoutingAndDelivery()
{
    live_midi::LiveMidiInputBus bus;
    const auto session = makeSession();
    const auto instruments = makeInstruments(kHostA);
    bus.publishRouting(makeRouting(true, true));

    const double t0 = 1000.0; // ms
    // First block establishes the callback clock.
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), t0, true, 48000), &recordDelivery, nullptr);

    // Events during the next block window (t0 … t0+5.33 ms).
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), (t0 + 1.0) * 0.001));
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(5, 62, (juce::uint8)90), (t0 + 2.0) * 0.001));
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(6, 64, (juce::uint8)80), (t0 + 3.0) * 0.001));
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(7, 65, (juce::uint8)70), (t0 + 4.0) * 0.001));
    bus.deviceThread_push(0, stamped(juce::MidiMessage::programChange(1, 3), (t0 + 4.0) * 0.001));
    const double t1 = t0 + 256.0 / 48.0;
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), t1, true, 48256), &recordDelivery, nullptr);

    expect(countDelivered(1, 60, true) == 1, "instrument row (Preserve, filter All): ch1 note reaches the host on ch1");
    expect(countDelivered(2, 62, true) == 1, "Lower row (filter 5, Force 2): ch5 note reaches the SAME host on ch2");
    expect(countDelivered(3, 64, true) == 1, "Pedal row (filter 6, Force 3, device All): ch6 note on ch3");
    // The instrument row (filter All) also forwards ch5/ch6/ch7 on their own channels (Preserve).
    expect(countDelivered(5, 62, true) == 1 && countDelivered(6, 64, true) == 1 && countDelivered(7, 65, true) == 1,
           "instrument row with filter All forwards every channel unchanged (Preserve)");
    expect(g_delivered.size() == 6, "exactly one delivery per matching (event, route) — nothing duplicated: "
                                        + juce::String((int)g_delivered.size()));
    bool allHostA = true;
    bool offsetsAscending = true;
    int lastOff = -1;
    for (const auto& d : g_delivered)
    {
        allHostA = allHostA && d.host == kHostA;
        offsetsAscending = offsetsAscending && d.offset >= lastOff;
        lastOff = d.offset;
    }
    expect(allHostA, "all deliveries go to the destination's transport host");
    expect(offsetsAscending && lastOff > 0 && lastOff < 256, "sample offsets follow timestamps inside the block");
    expect(bus.droppedUnsupportedCount() == 1, "program change dropped (counted)");

    // Captured events: instrument (All) captured 4 note-ons, Lower captured only ch5. Received
    // channels are kept as received (no remap in the recording).
    int capInstrument = 0, capLower = 0, capPedal = 0;
    bool channelsKept = true;
    live_midi::CapturedEvent ce;
    while (bus.popCaptured(ce))
    {
        if (ce.trackId == kInstrumentTrack)
        {
            ++capInstrument;
        }
        if (ce.trackId == kMidiLower)
        {
            ++capLower;
            channelsKept = channelsKept && ((ce.bytes[0] & 0x0f) + 1) == 5;
        }
        if (ce.trackId == kMidiPedal)
        {
            ++capPedal;
        }
        channelsKept = channelsKept && ce.transportPlaying && ce.timelineSample <= 48256 && ce.timelineSample > 48256 - 48000;
    }
    expect(capInstrument == 4 && capLower == 1 && capPedal == 0,
           "capture ring: armed rows receive their matching events (" + juce::String(capInstrument) + "/"
               + juce::String(capLower) + "/" + juce::String(capPedal) + "), unarmed row none");
    expect(channelsKept, "captured events keep the received channel and carry a playing timeline stamp");
    expect(bus.liveNoteCountForDiagnostics() == 6, "six live notes owned (one per delivered Note On)");

    // Ownership: change the Lower row's output channel to Force 4, then release ch5 62 → the Note
    // Off goes to channel 2 (where the Note On went), never to the new channel 4.
    const auto session2 = SessionSnapshot::withTrackMidiOutputChannel(*session, kMidiLower, 4);
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOff(5, 62, (juce::uint8)0), (t1 + 1.0) * 0.001));
    g_delivered.clear();
    const double t2 = t1 + 256.0 / 48.0;
    bus.audioThread_dispatch(makeContext(session2.get(), instruments.get(), t2, true, 48512), &recordDelivery, nullptr);
    expect(countDelivered(2, 62, false) == 1 && countDelivered(4, 62, false) == 0,
           "Note Off reaches the channel the Note On used (2), not the new output channel (4)");
    expect(countDelivered(5, 62, false) == 1, "instrument row's own ch5 copy is released too (Preserve)");

    // Monitor off on the instrument row while its notes are held: the next block releases exactly
    // that row's notes (60 ch1, 64 ch6, 65 ch7 and the ch5 copy already went) — Pedal/Lower stay.
    bus.publishRouting(makeRouting(false, true));
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session2.get(), instruments.get(), t2 + 5.0, true, 48768), &recordDelivery, nullptr);
    expect(countDelivered(1, 60, false) == 1 && countDelivered(6, 64, false) == 1 && countDelivered(7, 65, false) == 1,
           "Monitor off releases the row's own live notes with targeted Note Offs");
    expect(countDelivered(3, 64, false) == 0, "…and leaves the Pedal row's note (ch3) sounding");
    expect(bus.liveNoteCountForDiagnostics() == 1, "one live note remains (Pedal ch3)");

    // Device lost: route to a missing slot → its notes are released.
    auto lost = makeRouting(false, true);
    auto mutableLost = std::const_pointer_cast<live_midi::RoutingSnapshot>(lost);
    mutableLost->routes[2].deviceSlot = live_midi::kDeviceSlotMissing;
    bus.publishRouting(lost);
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session2.get(), instruments.get(), t2 + 10.0, true, 49000), &recordDelivery, nullptr);
    expect(countDelivered(3, 64, false) == 1 && bus.liveNoteCountForDiagnostics() == 0,
           "device loss releases the affected row's live notes");

    // Host swap: Note On to host A, then the destination's host becomes B → the Note Off goes to A
    // (still present in the snapshot as another entry), nothing to B.
    bus.publishRouting(makeRouting(true, true));
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(1, 70, (juce::uint8)100), (t2 + 11.0) * 0.001));
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session2.get(), instruments.get(), t2 + 15.0, true, 49200), &recordDelivery, nullptr);
    expect(countDelivered(1, 70, true) == 1, "note on to host A");
    std::vector<ExperimentalInstrumentPlaybackEntry> swapped;
    ExperimentalInstrumentPlaybackEntry eB;
    eB.trackId = kInstrumentTrack;
    eB.host = kHostB;
    eB.auditionHost = kHostA; // A still alive in the snapshot
    swapped.push_back(eB);
    const auto instrumentsB = std::make_shared<ExperimentalInstrumentPlaybackSnapshot>(std::move(swapped));
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session2.get(), instrumentsB.get(), t2 + 20.0, true, 49400), &recordDelivery, nullptr);
    bool offToA = false, anyToB = false;
    for (const auto& d : g_delivered)
    {
        offToA = offToA || (d.host == kHostA && d.msg.isNoteOff() && d.msg.getNoteNumber() == 70);
        anyToB = anyToB || d.host == kHostB;
    }
    expect(offToA && !anyToB, "host swap: the held note is released in the host that received the Note On");

    // Stopped transport: monitoring still works, captured events are flagged not playing.
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(1, 72, (juce::uint8)100), (t2 + 21.0) * 0.001));
    g_delivered.clear();
    bus.audioThread_dispatch(makeContext(session2.get(), instrumentsB.get(), t2 + 25.0, false, 49400), &recordDelivery, nullptr);
    expect(countDelivered(1, 72, true) == 1 && !g_delivered.empty() && g_delivered.front().host == kHostB,
           "transport stopped: live MIDI still reaches the (new) host");
    int stoppedCaptured = 0;
    while (bus.popCaptured(ce))
    {
        if (ce.trackId == kInstrumentTrack && !ce.transportPlaying && ce.timelineSample == 49400)
        {
            ++stoppedCaptured;
        }
    }
    expect(stoppedCaptured == 1, "stopped-transport capture carries playing=false and the stopped playhead");
}

void testOverflowAndDiscard()
{
    live_midi::LiveMidiInputBus bus;
    const auto session = makeSession();
    const auto instruments = makeInstruments(kHostA);
    bus.publishRouting(makeRouting(true, true));
    double t = 1000.0;
    bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), t, true, 0), &recordDelivery, nullptr);

    // Hold a note, then flood the ring beyond its capacity.
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), (t + 1.0) * 0.001));
    g_delivered.clear();
    t += 5.0;
    bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), t, true, 240), &recordDelivery, nullptr);
    expect(countDelivered(1, 60, true) == 1, "overflow test: note held");
    for (int i = 0; i < live_midi::kDeviceRingCapacity + 10; ++i)
    {
        bus.deviceThread_push(0, stamped(juce::MidiMessage::controllerEvent(1, 1, i & 0x7f), (t + 1.0) * 0.001));
    }
    expect(bus.deviceOverflowCount(0) >= 10, "device ring overflow counted: " + juce::String((int)bus.deviceOverflowCount(0)));
    g_delivered.clear();
    t += 5.0;
    bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), t, true, 480), &recordDelivery, nullptr);
    expect(countDelivered(1, 60, false) >= 1, "overflow releases the device's live notes (no hanging tone)");
    bool marker = false;
    live_midi::CapturedEvent ce;
    int drained = 0;
    while (bus.popCaptured(ce))
    {
        marker = marker || (ce.overflowMarker && ce.trackId == kInstrumentTrack);
        ++drained;
    }
    expect(marker, "overflow marker reaches the capture ring for armed rows");
    expect(drained > 0 && drained <= live_midi::kCaptureRingCapacity, "capture ring bounded");

    // Discard (export gate): pending events vanish and notes are forgotten without deliveries.
    bus.deviceThread_push(0, stamped(juce::MidiMessage::noteOn(1, 61, (juce::uint8)100), (t + 1.0) * 0.001));
    bus.audioThread_discardPendingAndForgetNotes();
    g_delivered.clear();
    t += 5.0;
    bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), t, true, 720), &recordDelivery, nullptr);
    expect(g_delivered.empty(), "discard: events received while gated never reach a host");
}

void testMultiThreadedProducers()
{
    // Two device threads pushing concurrently into their own rings while the consumer drains:
    // every pushed event is accounted for (delivered or counted), nothing corrupts.
    live_midi::LiveMidiInputBus bus;
    const auto session = makeSession();
    const auto instruments = makeInstruments(kHostA);
    auto r = std::make_shared<live_midi::RoutingSnapshot>();
    live_midi::Route a;
    a.trackId = kInstrumentTrack;
    a.deviceSlot = live_midi::kDeviceSlotAll;
    a.monitor = true;
    a.capture = false;
    r->routes.push_back(a);
    bus.publishRouting(r);
    constexpr int kPerThread = 3000;
    std::atomic<bool> go { false };
    auto producer = [&bus, &go](const int slot) {
        while (!go.load())
        {
            std::this_thread::yield();
        }
        for (int i = 0; i < kPerThread; ++i)
        {
            bus.deviceThread_push(slot, stamped(juce::MidiMessage::controllerEvent(1, 11, i & 0x7f),
                                                juce::Time::getMillisecondCounterHiRes() * 0.001));
            if ((i & 63) == 0)
            {
                std::this_thread::yield();
            }
        }
    };
    std::thread p0(producer, 0);
    std::thread p1(producer, 1);
    go.store(true);
    std::uint64_t delivered = 0;
    double now = juce::Time::getMillisecondCounterHiRes();
    for (int block = 0; block < 400 && delivered + bus.deviceOverflowCount(0) + bus.deviceOverflowCount(1) < 2 * kPerThread; ++block)
    {
        now += 256.0 / 48.0;
        g_delivered.clear();
        bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), now, false, 0), &recordDelivery, nullptr);
        delivered += g_delivered.size();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    p0.join();
    p1.join();
    for (int block = 0; block < 20; ++block)
    {
        now += 256.0 / 48.0;
        g_delivered.clear();
        bus.audioThread_dispatch(makeContext(session.get(), instruments.get(), now, false, 0), &recordDelivery, nullptr);
        delivered += g_delivered.size();
    }
    const std::uint64_t accounted = delivered + bus.deviceOverflowCount(0) + bus.deviceOverflowCount(1);
    expect(accounted == 2 * kPerThread,
           "two concurrent device threads: every event delivered or counted as overflow (" + juce::String((juce::int64)delivered)
               + " delivered, " + juce::String((int)(bus.deviceOverflowCount(0) + bus.deviceOverflowCount(1))) + " overflow)");
}

// ----------------------------------------------------------------------------- take builder
live_midi_take::TakeEvent ev(const std::int64_t sample, const juce::MidiMessage& m, const bool playing = true)
{
    live_midi_take::TakeEvent e;
    e.timelineSample = sample;
    e.size = (std::uint8_t)m.getRawDataSize();
    for (int i = 0; i < 3 && i < m.getRawDataSize(); ++i)
    {
        e.bytes[i] = m.getRawData()[i];
    }
    e.transportPlaying = playing;
    return e;
}

void testTakeBuilder()
{
    using namespace live_midi_take;
    TakeBuildParams p;
    p.recordStartSample = 48000;
    p.recordStopSample = 48000 + 96000; // 2 s
    p.sampleRate = 48000.0;
    p.bpm = 120.0; // 1 beat = 24000 samples = 960 ticks
    p.ticksPerQuarter = 960;

    // 1. Simple note: on at +1 beat, off at +1.5 beats.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000 + 24000, juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)));
        es.push_back(ev(48000 + 36000, juce::MidiMessage::noteOff(1, 60, (juce::uint8)40)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.hasContent && r.pattern.timelineNotes.size() == 1, "take: one note recorded");
        if (r.pattern.timelineNotes.size() == 1)
        {
            const auto& n = r.pattern.timelineNotes[0];
            expect(n.startTick == 960 && n.durationTicks == 480 && n.velocity == 100 && n.offVelocity == 40
                       && n.channel == 1 && n.midiNote == 60,
                   "take: position 960, length 480, velocity 100, off 40, channel 1");
        }
        expect(r.pattern.bpm == 120.0 && r.pattern.ticksPerQuarter == 960, "take: project tempo / PPQ adopted");
    }
    // 2. Velocity 0 = Note Off; received channel kept (ch 5).
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000 + 24000, juce::MidiMessage::noteOn(5, 62, (juce::uint8)90)));
        es.push_back(ev(48000 + 30000, juce::MidiMessage::noteOn(5, 62, (juce::uint8)0)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.pattern.timelineNotes.size() == 1 && r.pattern.timelineNotes[0].durationTicks == 240
                   && r.pattern.timelineNotes[0].channel == 5 && r.pattern.timelineNotes[0].offVelocity == 64,
               "take: Note On velocity 0 ends the note (default off velocity), channel 5 kept");
    }
    // 3. Held before start (stopped count-in event) → enters at tick 0; released inside.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000, juce::MidiMessage::noteOn(1, 64, (juce::uint8)77), false)); // during count-in
        es.push_back(ev(48000 + 12000, juce::MidiMessage::noteOff(1, 64, (juce::uint8)0)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.pattern.timelineNotes.size() == 1 && r.pattern.timelineNotes[0].startTick == 0
                   && r.pattern.timelineNotes[0].durationTicks == 480 && r.pattern.timelineNotes[0].velocity == 77,
               "take: key held across the start boundary enters at tick 0 with its real velocity");
    }
    // 3b. Held through the whole take (no event inside): still a note, closed at stop.
    {
        TakeStateTracker init;
        init.heldNotes.push_back({ 2, 48, 64 });
        const TakeBuildResult r = buildTakePattern({}, init, p);
        expect(r.hasContent && r.pattern.timelineNotes.size() == 1 && r.pattern.timelineNotes[0].startTick == 0
                   && r.pattern.timelineNotes[0].durationTicks == 3840 && r.notesClosedAtStop == 1,
               "take: chord held through the whole take = note from 0 to the stop boundary (3840 ticks)");
    }
    // 4. Note crossing stop → closed at stopTick; sustain held at stop → CC64=0 at stop.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000 + 60000, juce::MidiMessage::controllerEvent(1, 64, 127)));
        es.push_back(ev(48000 + 72000, juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)));
        es.push_back(ev(48000 + 200000, juce::MidiMessage::noteOff(1, 60, (juce::uint8)0))); // after stop
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.pattern.timelineNotes.size() == 1 && r.pattern.timelineNotes[0].startTick == 2880
                   && r.pattern.timelineNotes[0].durationTicks == 960 && r.notesClosedAtStop == 1,
               "take: note held at stop ends exactly on the stop boundary");
        bool sustainOffAtStop = false;
        for (const auto& c : r.pattern.ccPoints)
        {
            sustainOffAtStop = sustainOffAtStop || (c.controller == 64 && c.value == 0 && c.startTick == 3840);
        }
        expect(sustainOffAtStop && r.sustainReleasedAtStop, "take: held sustain pedal is released at the stop boundary");
        expect(r.eventsOutsideWindow == 1, "take: the post-stop release is ignored");
        bool holdOnly = true;
        for (const auto& c : r.pattern.ccPoints)
        {
            holdOnly = holdOnly && c.interpolationToNext == MidiCcInterpolation::hold;
        }
        expect(holdOnly, "take: recorded CC points use Hold (no interpolation invented)");
    }
    // 5. Controller state before start restated at tick 0: sustain down, expression, pitch bend.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000, juce::MidiMessage::controllerEvent(1, 64, 127), false));
        es.push_back(ev(48000, juce::MidiMessage::controllerEvent(1, 11, 90), false));
        es.push_back(ev(48000, juce::MidiMessage::controllerEvent(1, 74, 10), false)); // not restated
        es.push_back(ev(48000, juce::MidiMessage::pitchWheel(1, 9000), false));
        es.push_back(ev(48000 + 24000, juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)));
        es.push_back(ev(48000 + 30000, juce::MidiMessage::noteOff(1, 60, (juce::uint8)0)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        bool sus = false, expr = false, cc74 = false, pb = false;
        for (const auto& c : r.pattern.ccPoints)
        {
            sus = sus || (c.controller == 64 && c.value == 127 && c.startTick == 0);
            expr = expr || (c.controller == 11 && c.value == 90 && c.startTick == 0);
            cc74 = cc74 || c.controller == 74;
        }
        for (const auto& b : r.pattern.pitchBendPoints)
        {
            pb = pb || (b.value == 9000 && b.startTick == 0 && b.channel == 1);
        }
        expect(sus && expr && pb && !cc74, "take: sustain / expression / pitch bend state restated at tick 0, others not");
        expect(r.sustainReleasedAtStop, "take: sustain still down at stop → released");
    }
    // 6. Empty take and controller-only take.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000, juce::MidiMessage::controllerEvent(1, 64, 127), false)); // only pre-start state
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(!r.hasContent && r.pattern.ccPoints.empty(), "take: boundary state alone is not content (empty take)");
        std::vector<TakeEvent> cc;
        cc.push_back(ev(48000 + 1000, juce::MidiMessage::controllerEvent(1, 11, 30)));
        cc.push_back(ev(48000 + 2000, juce::MidiMessage::controllerEvent(1, 11, 60)));
        cc.push_back(ev(48000 + 3000, juce::MidiMessage::pitchWheel(1, 12000)));
        const TakeBuildResult r2 = buildTakePattern(cc, TakeStateTracker{}, p);
        expect(r2.hasContent && r2.pattern.timelineNotes.empty() && r2.pattern.ccPoints.size() == 2
                   && r2.pattern.pitchBendPoints.size() == 1 && r2.pattern.pitchBendPoints[0].value == 12000,
               "take: controller-only take is valid content (CC + 14-bit pitch bend kept)");
    }
    // 7. Retrigger of a held pitch.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000 + 0, juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)));
        es.push_back(ev(48000 + 24000, juce::MidiMessage::noteOn(1, 60, (juce::uint8)110)));
        es.push_back(ev(48000 + 48000, juce::MidiMessage::noteOff(1, 60, (juce::uint8)0)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.pattern.timelineNotes.size() == 2 && r.pattern.timelineNotes[0].durationTicks == 960
                   && r.pattern.timelineNotes[1].startTick == 960 && r.pattern.timelineNotes[1].durationTicks == 960,
               "take: retrigger ends the earlier note where the new one starts");
    }
    // 8. 44.1 kHz / 180 BPM tick math.
    {
        TakeBuildParams q = p;
        q.sampleRate = 44100.0;
        q.bpm = 180.0; // beat = 14700 samples
        q.recordStartSample = 1000;
        q.recordStopSample = 1000 + 147000;
        std::vector<TakeEvent> es;
        es.push_back(ev(1000 + 14700, juce::MidiMessage::noteOn(3, 50, (juce::uint8)64)));
        es.push_back(ev(1000 + 14700 * 2, juce::MidiMessage::noteOff(3, 50, (juce::uint8)0)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, q);
        expect(r.pattern.timelineNotes.size() == 1 && r.pattern.timelineNotes[0].startTick == 960
                   && r.pattern.timelineNotes[0].durationTicks == 960 && r.pattern.bpm == 180.0,
               "take: 44.1 kHz @ 180 BPM — one beat = 960 ticks, no 120 BPM assumption");
    }
    // 9. Events after playback began but stopped (post-stop) never shape the start state.
    {
        std::vector<TakeEvent> es;
        es.push_back(ev(48000 + 1000, juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)));
        es.push_back(ev(48000 + 2000, juce::MidiMessage::noteOff(1, 60, (juce::uint8)0)));
        es.push_back(ev(48000 + 96000, juce::MidiMessage::noteOn(1, 99, (juce::uint8)100), false)); // after stop
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.pattern.timelineNotes.size() == 1 && r.pattern.timelineNotes[0].midiNote == 60,
               "take: a key pressed after the stop is not part of the take");
    }
    // 10. Overflow marker is reported.
    {
        std::vector<TakeEvent> es;
        TakeEvent m;
        m.overflowMarker = true;
        m.transportPlaying = true;
        m.timelineSample = 48000 + 500;
        es.push_back(m);
        es.push_back(ev(48000 + 1000, juce::MidiMessage::noteOn(1, 60, (juce::uint8)100)));
        const TakeBuildResult r = buildTakePattern(es, TakeStateTracker{}, p);
        expect(r.overflowSeen && r.notesClosedAtStop == 1, "take: overflow marker reported; unmatched note closed at stop");
    }
}

// ----------------------------------------------------------------------------- persistence / export
void testProjectRoundTrip()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-live-midi-tests");
    (void)dir.createDirectory();
    const juce::File f = dir.getChildFile("v24-roundtrip.dalproj");

    ProjectFileV1 data;
    data.version = ProjectFileV1::kCurrentVersion;
    {
        ProjectFileTrackV1 inst;
        inst.id = 10;
        inst.name = "VB3";
        inst.kind = "instrument";
        inst.midiInputAssignment.mode = TrackMidiInputMode::Device;
        inst.midiInputAssignment.deviceIdentifier = "\\\\?\\usb#vid_1234&pid_5678#1";
        inst.midiInputAssignment.deviceName = "Keyboard 88";
        inst.midiInputAssignment.channelFilter = 5;
        data.tracks.push_back(inst);
        ProjectFileTrackV1 midi;
        midi.id = 11;
        midi.name = "Lower";
        midi.kind = "midi";
        midi.midiDestinationTrackId = 10;
        midi.midiInputAssignment.mode = TrackMidiInputMode::AllEnabled;
        data.tracks.push_back(midi);
        ProjectFileTrackV1 none;
        none.id = 12;
        none.name = "Pedal";
        none.kind = "midi";
        data.tracks.push_back(none);
        ProjectFileTrackV1 master;
        master.id = 2;
        master.name = "Stereo Out";
        master.kind = "master";
        data.tracks.push_back(master);
        data.nextTrackId = 13;

        ProjectFileExperimentalInstrumentTrackV1 et;
        et.trackId = 10;
        et.enabled = true;
        et.instrumentKind = "GenericVst3";
        ProjectFileExperimentalInstrumentClipV1 c;
        c.id = 1;
        c.name = "Take";
        c.bpm = 120.0;
        c.startSamples = 48000;
        c.lengthSamples = 96000;
        ProjectFileExperimentalMidiPitchBendPointV24 b1;
        b1.startTick = 0;
        b1.value = 9000;
        b1.channel = 1;
        ProjectFileExperimentalMidiPitchBendPointV24 b2;
        b2.startTick = 480;
        b2.value = 16383;
        b2.channel = 2;
        c.pitchBendPoints.push_back(b1);
        c.pitchBendPoints.push_back(b2);
        et.clips.push_back(c);
        data.experimentalInstrumentTracks.push_back(et);
    }
    const auto wr = writeProjectFile(f, data);
    expect(wr.wasOk(), "v24 project writes");
    ProjectFileV1 back;
    const auto rr = readProjectFile(f, back);
    expect(rr.wasOk() && back.version == ProjectFileV1::kCurrentVersion, "v24 project reads at current version");
    if (rr.wasOk() && back.tracks.size() == 4)
    {
        const auto& t0 = back.tracks[0].midiInputAssignment;
        expect(t0.mode == TrackMidiInputMode::Device && t0.deviceIdentifier == data.tracks[0].midiInputAssignment.deviceIdentifier
                   && t0.deviceName == "Keyboard 88" && t0.channelFilter == 5,
               "v24: device mode, identifier, readable name and channel filter round-trip");
        expect(back.tracks[1].midiInputAssignment.mode == TrackMidiInputMode::AllEnabled
                   && back.tracks[1].midiInputAssignment.channelFilter == kTrackMidiInputChannelAll,
               "v24: All-inputs mode round-trips with filter All");
        expect(back.tracks[2].midiInputAssignment.mode == TrackMidiInputMode::None, "v24: unassigned row stays None");
    }
    if (rr.wasOk() && back.experimentalInstrumentTracks.size() == 1 && back.experimentalInstrumentTracks[0].clips.size() == 1)
    {
        const auto& pb = back.experimentalInstrumentTracks[0].clips[0].pitchBendPoints;
        expect(pb.size() == 2 && pb[0].value == 9000 && pb[0].channel == 1 && pb[1].startTick == 480 && pb[1].value == 16383
                   && pb[1].channel == 2,
               "v24: pitch bend points round-trip with full 14-bit values");
    }
    // The JSON omits the keys for None rows and for clips without pitch bend.
    const juce::String json = f.loadFileAsString();
    expect(json.contains("\"midiInput\": \"device\"") || json.contains("\"midiInput\":\"device\""), "v24 json: device key written");
    expect(!json.contains("midiInputDeviceId\": \"\""), "v24 json: no empty identifiers");
    // Older reader simulation: a v23 file has no keys → None (no automatic coupling).
    const juce::String v23 = json.replace("\"version\": " + juce::String(ProjectFileV1::kCurrentVersion), "\"version\": 23")
                                 .replace("\"version\":" + juce::String(ProjectFileV1::kCurrentVersion), "\"version\":23");
    const juce::File f23 = dir.getChildFile("v23-sim.dalproj");
    (void)f23.replaceWithText(v23);
    ProjectFileV1 old;
    const auto r23 = readProjectFile(f23, old);
    expect(r23.wasOk() && old.tracks.size() == 4 && old.tracks[0].midiInputAssignment.mode == TrackMidiInputMode::None,
           "a pre-v24 file loads every row with MIDI input None (keys ignored below v24)");
    (void)dir.deleteRecursively();
}

void testExportPitchBend()
{
    InstrumentMidiClip clip;
    clip.id = 1;
    clip.name = "PB";
    clip.startSamples = 0;
    clip.timelineAnchorSamples = 0;
    clip.lengthSamples = 96000;
    clip.pattern.bpm = 120.0;
    clip.pattern.ticksPerQuarter = 960;
    TimelineMidiNote n;
    n.midiNote = 60;
    n.channel = 1;
    n.startTick = 0;
    n.durationTicks = 480;
    clip.pattern.timelineNotes.push_back(n);
    clip.pattern.pitchBendPoints.push_back({ 0, 9000, 1 });
    clip.pattern.pitchBendPoints.push_back({ 240, 16383, 1 });
    clip.pattern.pitchBendPoints.push_back({ 300, 0, 5 });
    juce::MidiFile mf;
    const auto res = buildInstrumentMidiClipMidiFile(clip, kTrackMidiOutputChannelAny, 48000.0, mf);
    expect(res.ok && res.pitchBendEventsExported == 3, "export: three pitch wheel events written");
    int found = 0;
    bool valuesOk = true;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const juce::MidiMessageSequence* seq = mf.getTrack(t);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const juce::MidiMessage& m = seq->getEventPointer(i)->message;
            if (m.isPitchWheel())
            {
                ++found;
                valuesOk = valuesOk && ((m.getPitchWheelValue() == 9000 && m.getChannel() == 1 && m.getTimeStamp() == 0.0)
                                        || (m.getPitchWheelValue() == 16383 && m.getChannel() == 1)
                                        || (m.getPitchWheelValue() == 0 && m.getChannel() == 5));
            }
        }
    }
    expect(found == 3 && valuesOk, "export: raw 14-bit values and native channels preserved under Any (Preserve)");
    juce::MidiFile mf2;
    (void)buildInstrumentMidiClipMidiFile(clip, 4, 48000.0, mf2);
    bool allCh4 = true;
    for (int t = 0; t < mf2.getNumTracks(); ++t)
    {
        const juce::MidiMessageSequence* seq = mf2.getTrack(t);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const juce::MidiMessage& m = seq->getEventPointer(i)->message;
            if (m.isPitchWheel())
            {
                allCh4 = allCh4 && m.getChannel() == 4;
            }
        }
    }
    expect(allCh4, "export: fixed track channel remaps pitch wheel like notes and CC");
}
} // namespace

int main()
{
    testTimeMappings();
    testRoutingAndDelivery();
    testOverflowAndDiscard();
    testMultiThreadedProducers();
    testTakeBuilder();
    testProjectRoundTrip();
    testExportPitchBend();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
