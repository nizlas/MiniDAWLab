// =============================================================================
// MixerFocusedTests — meter sharing, TrackId-bound strips, section layout, layout persistence
// =============================================================================
//
// Level-1 deterministic checks for the mixer slice, device-free and window-free:
//   * engine/TrackMeterBank: stable slots per TrackId, release / round-robin reuse, overflow,
//     per-row drain, audio-thread lookup against the published map;
//   * ui/LevelMeterHub: one drain per row per tick, every listener receives every window exactly
//     once (Inspector + mixer agree), the Master row arrives under its own id, hidden views cost
//     nothing, overload acknowledgement fans out;
//   * ui/mixer/MixerSectionLayout: identical bands for every strip, independent sections, lower
//     band minimum / stretch;
//   * ui/mixer/MixerChannelStrip (offscreen): a strip bound to track B edits ONLY B through the
//     bindings while another track is active; per-kind controls; Midi rows have no audio controls;
//   * ui/UiLayoutSettingsStore: mixer bounds + section flags round trip beside the header width.
// Exit 0 = all green.
// =============================================================================

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/TrackMeterBank.h"
#include "ui/LevelMeterHub.h"
#include "ui/TrackChannelOptions.h"
#include "ui/UiLayoutSettingsStore.h"
#include "ui/mixer/MixerChannelStrip.h"
#include "ui/mixer/MixerSectionLayout.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace
{
    int checks = 0;
    int failures = 0;

    void check(const bool ok, const char* what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
            std::printf("[FAIL] %s\n", what);
        }
        else
        {
            std::printf("[ ok ] %s\n", what);
        }
    }

    level_meter::BlockStats statsForPeak(const float peak)
    {
        std::vector<float> l(64, peak), r(64, peak * 0.5f);
        return level_meter::analyzeBlock(l.data(), r.data(), 64);
    }

    // --- TrackMeterBank ---------------------------------------------------------------------------
    void testTrackMeterBank()
    {
        level_meter::TrackMeterBank bank;
        check(bank.setMeteredTracks({ 10, 20, 30 }) == 0, "bank: three rows metered without overflow");
        check(bank.isMetered(20) && !bank.isMetered(40), "bank: membership follows the published set");
        bank.audioThread_beginBlock();
        check(bank.audioThread_isMetered(10) && !bank.audioThread_isMetered(99), "bank: audio-thread lookup uses the cached map");
        bank.audioThread_foldStats(20, statsForPeak(0.5f));
        bank.audioThread_foldStats(30, statsForPeak(0.25f));
        const auto r20 = bank.drainAndReset(20);
        const auto r30 = bank.drainAndReset(30);
        const auto r10 = bank.drainAndReset(10);
        check(std::abs(r20.peak[0] - 0.5f) < 1e-6f && r20.blocks == 1, "bank: row 20 drained its own window");
        check(std::abs(r30.peak[0] - 0.25f) < 1e-6f, "bank: row 30 drained its own window");
        check(!r10.hasSignalData(), "bank: a row nothing was folded into reads empty");
        check(!bank.drainAndReset(20).hasSignalData(), "bank: drain resets the window");
        // Keep 20, drop 10 / 30, add 40: 20 keeps its pending data, 40 starts empty.
        bank.audioThread_foldStats(20, statsForPeak(0.7f));
        check(bank.setMeteredTracks({ 20, 40 }) == 0, "bank: re-publish keeps / frees / assigns");
        check(bank.drainAndReset(20).peak[0] > 0.69f, "bank: a retained row keeps its pending window across re-publish");
        check(!bank.drainAndReset(40).hasSignalData() && !bank.isMetered(10), "bank: new row empty, dropped row gone");
        bank.audioThread_beginBlock();
        bank.audioThread_foldStats(10, statsForPeak(0.9f));
        check(!bank.drainAndReset(10).hasSignalData(), "bank: folding an unmetered row is a no-op");
        // Overflow: more rows than slots.
        std::vector<TrackId> many;
        for (TrackId id = 100; id < 100 + level_meter::TrackMeterBank::kMaxSlots + 5; ++id)
        {
            many.push_back(id);
        }
        const int overflow = bank.setMeteredTracks(many);
        check(overflow == 5, "bank: overflow reports the rows that could not be metered");
        check(bank.meteredTracks().size() == (size_t)level_meter::TrackMeterBank::kMaxSlots, "bank: exactly kMaxSlots rows metered when full");
        check(bank.setMeteredTracks({}) == 0 && bank.meteredTracks().empty(), "bank: empty publish frees everything");
    }

    // --- LevelMeterHub -----------------------------------------------------------------------------
    struct FakeListener final : public LevelMeterHub::Listener
    {
        std::vector<TrackId> interest;
        std::vector<std::pair<TrackId, float>> received;
        std::vector<TrackId> acknowledged;
        int ticks = 0;
        void collectMeterInterest(std::vector<TrackId>& out) override { out.insert(out.end(), interest.begin(), interest.end()); }
        void meterWindowArrived(const TrackId id, const level_meter::Reading& r, double) override { received.push_back({ id, r.peak[0] }); }
        void meterOverloadAcknowledged(const TrackId id) override { acknowledged.push_back(id); }
        void meterTick(double) override { ++ticks; }
    };

    void testLevelMeterHub()
    {
        level_meter::TrackMeterBank bank;
        level_meter::Accumulator master;
        std::vector<std::vector<TrackId>> published;
        LevelMeterHub hub;
        hub.setEngineHooks({
            [&](const std::vector<TrackId>& ids) { published.push_back(ids); return bank.setMeteredTracks(ids); },
            [&](const TrackId id) { return bank.drainAndReset(id); },
            [&] { return master.drainAndReset(); },
            [] { return TrackId{ 2 }; }, // the Master row's id
        });
        FakeListener inspector, mixer, hidden;
        inspector.interest = { 5 };
        mixer.interest = { 5, 7, 2 }; // 2 = master: must not claim a bank slot
        hub.addListener(&inspector);
        hub.addListener(&mixer);
        hub.addListener(&hidden); // a hidden view reports nothing
        hub.refreshInterestNow();
        check(hub.publishedInterest() == std::vector<TrackId>({ 5, 7 }), "hub: interest = union of the listeners, sorted, master excluded");

        bank.audioThread_beginBlock();
        bank.audioThread_foldStats(5, statsForPeak(0.6f));
        bank.audioThread_foldStats(7, statsForPeak(0.3f));
        master.audioThread_foldStats(statsForPeak(0.8f));
        hub.tickNowForTest();
        const auto countFor = [](const FakeListener& l, const TrackId id) {
            int n = 0;
            for (const auto& p : l.received) { n += p.first == id ? 1 : 0; }
            return n;
        };
        check(countFor(inspector, 5) == 1 && countFor(mixer, 5) == 1, "hub: both views received row 5's window exactly once");
        check(inspector.received.size() >= 1 && mixer.received.size() >= 1 && std::abs(inspector.received[0].second - mixer.received[0].second) < 1e-6f,
              "hub: the two views saw the identical window (no one drained the other's peak)");
        check(countFor(mixer, 7) == 1 && countFor(inspector, 7) == 1, "hub: every listener gets every drained row (views filter by their own id)");
        check(countFor(mixer, 2) == 1 && std::abs(mixer.received.back().second - 0.8f) < 1e-6f, "hub: the Stereo Out window arrives under the Master row's id");
        check(hidden.received.size() == inspector.received.size() && hidden.ticks == 1 && mixer.ticks == 1, "hub: one animation tick per listener per hub tick");
        // Second tick with nothing folded: no windows delivered (bars keep falling on the tick).
        const size_t before = mixer.received.size();
        hub.tickNowForTest();
        check(mixer.received.size() == before, "hub: an empty window is not delivered");
        // Acknowledgement fan-out.
        hub.acknowledgeOverload(5);
        check(inspector.acknowledged == std::vector<TrackId>({ 5 }) && mixer.acknowledged == std::vector<TrackId>({ 5 }), "hub: overload acknowledgement reaches every view");
        // The mixer hides: only the Inspector's row stays metered.
        mixer.interest.clear();
        hub.refreshInterestNow();
        check(hub.publishedInterest() == std::vector<TrackId>({ 5 }), "hub: a hidden mixer removes its rows from the metered set");
        hub.removeListener(&inspector);
        hub.refreshInterestNow();
        check(hub.publishedInterest().empty(), "hub: no listeners -> nothing metered");
        hub.removeListener(&mixer);
        hub.removeListener(&hidden);
    }

    // --- MixerSectionLayout ------------------------------------------------------------------------
    void testSectionLayout()
    {
        using namespace mixer_layout;
        SectionVisibility all;
        const ComputedLayout a = computeLayout(all, kStripWidthPx, 900);
        check(a.totalHeight == 900 && !a.lowerBand.isEmpty() && a.lowerBand.getHeight() > kLowerBandMinHeightPx,
              "layout: with room the lower band stretches to fill the strip");
        const ComputedLayout b = computeLayout(all, kStripWidthPx, 300);
        check(b.totalHeight == minimumStripHeight(all) && b.lowerBand.getHeight() == kLowerBandMinHeightPx,
              "layout: a low window yields the minimum stack (owner scrolls)");
        check(minimumStripHeight(all) <= 720 - 30 - 12, "layout: every section fits the default 720 px window above the scroll bar");
        check(!a.band(Section::Faders).isEmpty() && !a.band(Section::Meters).isEmpty()
                  && a.band(Section::Faders).getRight() <= a.band(Section::Meters).getX(),
              "layout: faders left, meters right in the lower band");
        SectionVisibility noRouting = all;
        noRouting.set(Section::Routing, false);
        const ComputedLayout c = computeLayout(noRouting, kStripWidthPx, 900);
        check(c.band(Section::Routing).isEmpty() && c.band(Section::PreGain).getY() < a.band(Section::PreGain).getY(),
              "layout: a hidden section takes no space and the following sections move up");
        SectionVisibility fadersOnly;
        for (int i = 0; i < kSectionCount; ++i) { fadersOnly.set(static_cast<Section>(i), false); }
        fadersOnly.set(Section::Faders, true);
        const ComputedLayout d = computeLayout(fadersOnly, kStripWidthPx, 500);
        check(d.band(Section::Faders) == d.lowerBand && d.band(Section::Meters).isEmpty(), "layout: faders alone take the whole lower band");
        SectionVisibility none;
        for (int i = 0; i < kSectionCount; ++i) { none.set(static_cast<Section>(i), false); }
        const ComputedLayout e = computeLayout(none, kStripWidthPx, 500);
        check(e.lowerBand.isEmpty() && e.totalHeight == kStripPadPx + kHeaderHeightPx + kStripPadPx, "layout: everything hidden leaves the header only");
        // Two strips with different content get identical bands: the layout depends on flags + height only.
        const ComputedLayout f = computeLayout(all, kStripWidthPx, 900);
        check(f.bands == a.bands && f.header == a.header, "layout: identical inputs -> identical bands (alignment across strips)");
    }

    // --- Section heights + dividers --------------------------------------------------------------------
    void testDividers()
    {
        using namespace mixer_layout;
        SectionVisibility all;
        SectionHeights h;
        check(h.get(Section::PreInserts) == kInsertsDefaultSectionHeightPx && h.get(Section::Routing) == kRoutingSectionHeightPx,
              "heights: defaults = two insert rows, routing / pre-gain / sends at their full content height");
        h.set(Section::PreInserts, 10);
        check(h.get(Section::PreInserts) == kInsertsMinSectionHeightPx, "heights: a too-small stored height clamps to the minimum (controls stay reachable)");
        h.set(Section::Routing, 50);
        check(h.get(Section::Routing) == kRoutingSectionHeightPx, "heights: routing can never shrink below its four rows");
        h.set(Section::Faders, 300);
        check(h.get(Section::Faders) == 0, "heights: the lower band has no stored height");
        h = SectionHeights{};

        const ComputedLayout L = computeLayout(all, h, kStripWidthPx, 900);
        const std::vector<Divider> d = dividersFor(L, all);
        check(d.size() == 5, "dividers: five with every section shown (4 between upper bands + 1 to the lower band)");
        check(d[0].above == Section::Routing && d[0].below == Section::PreGain && d[4].above == Section::Sends && d[4].below == Section::Count,
              "dividers: ordered top to bottom, last one against the lower band");
        check(d[2].lineY > L.band(Section::PreInserts).getBottom() - 1 && d[2].lineY < L.band(Section::PostInserts).getY(),
              "dividers: a line sits in the gap between its two bands");

        // Hide Post inserts: its divider disappears, no empty band remains.
        SectionVisibility noPost = all;
        noPost.set(Section::PostInserts, false);
        const ComputedLayout L2 = computeLayout(noPost, h, kStripWidthPx, 900);
        const std::vector<Divider> d2 = dividersFor(L2, noPost);
        check(d2.size() == 4 && d2[2].above == Section::PreInserts && d2[2].below == Section::Sends,
              "dividers: a hidden section leaves no divider and its neighbours meet");
        check(L2.band(Section::PostInserts).isEmpty() && L2.band(Section::Sends).getY() == L2.band(Section::PreInserts).getBottom() + kSectionGapPx,
              "dividers: no empty band where the hidden section was");

        // Drag Pre|Post down: Pre grows, Post shrinks, total constant, Post never below minimum.
        const SectionHeights after = applyDividerDrag(h, all, d[2], 54, 900);
        check(after.get(Section::PreInserts) + after.get(Section::PostInserts) == h.get(Section::PreInserts) + h.get(Section::PostInserts),
              "drag: height moves between the two adjacent bands (total constant)");
        check(after.get(Section::PostInserts) == kInsertsMinSectionHeightPx && after.get(Section::PreInserts) == 2 * kInsertsDefaultSectionHeightPx - kInsertsMinSectionHeightPx,
              "drag: the band below stops at its minimum, the band above gets the rest");
        const SectionHeights up = applyDividerDrag(after, all, d[2], -200, 900);
        check(up.get(Section::PreInserts) == kInsertsMinSectionHeightPx && up.get(Section::PostInserts) == 2 * kInsertsDefaultSectionHeightPx - kInsertsMinSectionHeightPx,
              "drag: upwards the band above stops at ITS minimum");
        // Routing | Pre-gain: both at their minimum -> nothing to redistribute.
        check(applyDividerDrag(h, all, d[0], 30, 900) == h && applyDividerDrag(h, all, d[0], -30, 900) == h,
              "drag: two bands at their minimum cannot move their divider");
        // Against the lower band: grow Sends into the fader band's spare height only.
        const int spare = L.lowerBand.getHeight() - kLowerBandMinHeightPx;
        check(spare > 0, "drag: a 900 px strip has spare fader height");
        const SectionHeights grown = applyDividerDrag(h, all, d[4], 10000, 900);
        check(grown.get(Section::Sends) == kSendsSectionHeightPx + spare, "drag: the last divider grows the band above only into the fader band's spare height");
        const ComputedLayout L3 = computeLayout(all, grown, kStripWidthPx, 900);
        check(L3.lowerBand.getHeight() == kLowerBandMinHeightPx && L3.totalHeight == 900, "drag: faders keep their usable minimum, the strip does not grow");
        // In a low window (no spare) the last divider can only shrink the band above.
        const SectionHeights low = applyDividerDrag(grown, all, d[4], 100, 300);
        check(low == grown, "drag: without spare height the band above cannot grow");
        const SectionHeights shrunk = applyDividerDrag(grown, all, d[4], -10000, 300);
        check(shrunk.get(Section::Sends) == kSendsSectionHeightPx, "drag: shrinking back down to the minimum always works");
        // Stored heights are display-clamped, never rewritten, by a low window.
        const ComputedLayout L4 = computeLayout(all, grown, kStripWidthPx, 300);
        check(L4.totalHeight == minimumStripHeight(all, grown) && L4.band(Section::Sends).getHeight() == grown.get(Section::Sends),
              "layout: a low window keeps the desired heights and grows the strip (owner scrolls)");
    }

    // --- MixerChannelStrip (offscreen) ------------------------------------------------------------
    struct StripHarness
    {
        Session session;
        MixerStripBindings bindings;
        std::vector<juce::String> calls;
        TrackId a = kInvalidTrackId, b = kInvalidTrackId, g = kInvalidTrackId, m = kInvalidTrackId, midi = kInvalidTrackId;

        StripHarness()
        {
            session.addTrack();
            a = session.getTrackIdAtIndex(0);
            session.addTrack();
            b = session.getTrackIdAtIndex(1);
            session.addGroupTrack();
            for (int i = 0; i < session.getNumTracks(); ++i)
            {
                if (session.getTrackKindAtIndex(i) == TrackKind::Group) { g = session.getTrackIdAtIndex(i); }
            }
            midi = session.addMidiTrack().value_or(kInvalidTrackId);
            m = session.findCanonicalMasterTrackId();
            session.setTrackName(a, "Alpha");
            session.setTrackName(b, "Bravo");
            session.setTrackName(g, "Bus");
            session.setActiveTrack(a);

            bindings.loadSnapshot = [this] { return session.loadSessionSnapshotForAudioThread(); };
            bindings.activeTrackId = [this] { return session.getActiveTrackId(); };
            bindings.audioInputDeviceSnapshot = [] {
                InspectorAudioInputDeviceSnapshot d;
                d.deviceAvailable = true;
                d.physicalInputNames = { "Mic 1", "Mic 2" };
                d.activeInputChannels.setRange(0, 2, true);
                return d;
            };
            bindings.midiInputSnapshot = [](TrackId) { return InspectorMidiInputSnapshot{}; };
            bindings.deviceOutputDescription = [] { return juce::String("Test device: Out 1 + Out 2"); };
            bindings.activateTrack = [this](const TrackId id) { calls.push_back("activate:" + juce::String((juce::int64)id)); session.setActiveTrack(id); };
            bindings.edits.setRoutedOutput = [this](const TrackId id, const TrackId dest) { calls.push_back("route:" + juce::String((juce::int64)id)); (void)session.setTrackRoutedOutput(id, dest); };
            bindings.edits.setPreGainDb = [this](const TrackId id, const float db) { calls.push_back("pregain:" + juce::String((juce::int64)id)); (void)session.setTrackPreGainDb(id, db); };
            bindings.edits.setAudioInput = [this](const TrackId id, const TrackInputAssignment as) { calls.push_back("input:" + juce::String((juce::int64)id)); (void)session.setTrackInputAssignment(id, as); };
            bindings.edits.setSendDestination = [this](const TrackId id, const int slot, const TrackId dest) {
                calls.push_back("send:" + juce::String((juce::int64)id));
                (void)session.insertTrackSend(id, slot, dest, kSendAmountUnityLinear);
            };
            bindings.edits.setSendAmount = [this](const TrackId id, const int slot, const float amt) { calls.push_back("sendamt:" + juce::String((juce::int64)id)); (void)session.setTrackSendAmount(id, slot, amt); };
            bindings.edits.setSendEnabled = [this](const TrackId id, const int slot, const bool en) { calls.push_back("senden:" + juce::String((juce::int64)id)); (void)session.setTrackSendEnabled(id, slot, en); };
            bindings.setChannelFaderGain = [this](const TrackId id, const float gain) { calls.push_back("fader:" + juce::String((juce::int64)id)); session.setTrackChannelFaderGain(id, gain); };
            bindings.setStereoPan = [this](const TrackId id, const float pan) { calls.push_back("pan:" + juce::String((juce::int64)id)); session.setTrackStereoPan(id, pan); };
            bindings.toggleMute = [this](const TrackId id) {
                calls.push_back("mute:" + juce::String((juce::int64)id));
                const auto snap = session.loadSessionSnapshotForAudioThread();
                const int idx = snap->findTrackIndexById(id);
                session.setTrackMuted(id, !snap->getTrack(idx).isMuted());
            };
            bindings.isPowerInteractable = [] { return true; };
            bindings.monitorAvailable = [](TrackId) { return true; };
            bindings.isMonitorOn = [](TrackId) { return false; };
            bindings.armAvailable = [](TrackId) { return true; };
            bindings.isRecordArmed = [](TrackId) { return false; };
            // Fake insert chain per track: `insertRows[tid]`; the seam records every action's ids.
            bindings.inserts.getInsertRows = [this](const TrackId id) {
                const auto it = insertRows.find(id);
                return it != insertRows.end() ? it->second : std::vector<InspectorInsertRow>{};
            };
            bindings.inserts.requestEdit = [this](const TrackId id, const InsertSlotId sid) { calls.push_back("edit:" + juce::String((juce::int64)id) + ":" + juce::String((juce::int64)sid)); };
            bindings.inserts.requestRemove = [this](const TrackId id, const InsertSlotId sid) {
                calls.push_back("remove:" + juce::String((juce::int64)id) + ":" + juce::String((juce::int64)sid));
                auto& rows = insertRows[id];
                rows.erase(std::remove_if(rows.begin(), rows.end(), [sid](const InspectorInsertRow& r) { return r.slotId == sid; }), rows.end());
            };
            bindings.inserts.requestReorderInStage = [this](const TrackId id, const InsertSlotId sid, const int gap) {
                calls.push_back("reorder:" + juce::String((juce::int64)id) + ":" + juce::String((juce::int64)sid) + ":" + juce::String(gap));
            };
            bindings.inserts.requestMoveToStageAtGap = [this](const TrackId id, const InsertSlotId sid, const InsertStage st, const int gap) {
                calls.push_back("stage:" + juce::String((juce::int64)id) + ":" + juce::String((juce::int64)sid) + ":" + juce::String(st == InsertStage::Pre ? "pre" : "post") + ":" + juce::String(gap));
            };
        }

        std::map<TrackId, std::vector<InspectorInsertRow>> insertRows;

        std::shared_ptr<const SessionSnapshot> snap() const { return session.loadSessionSnapshotForAudioThread(); }
        const Track& track(const TrackId id) const
        {
            const auto s = snap();
            return s->getTrack(s->findTrackIndexById(id));
        }
    };

    void testStripBinding()
    {
        StripHarness h;
        LevelMeterHub hub;
        const mixer_layout::SectionVisibility all;
        const auto layout = mixer_layout::computeLayout(all, mixer_layout::kStripWidthPx, 760);

        MixerChannelStrip strip(h.b, h.bindings, &hub);
        strip.setBounds(0, 0, mixer_layout::kStripWidthPx, 760);
        strip.applyLayout(layout);
        strip.refreshFromSession(*h.snap(), false);
        check(strip.nameText() == "Bravo" && strip.kindText() == "AUDIO", "strip: shows its own row's name and kind");
        check(h.session.getActiveTrackId() == h.a, "strip: track A stays the active track throughout");

        strip.fader().commitTypedValue("-6");
        check(std::abs(h.track(h.b).getChannelFaderGain() - 0.501f) < 0.01f, "strip: typed fader value reaches track B's session gain");
        check(std::abs(h.track(h.a).getChannelFaderGain() - 1.0f) < 1e-6f, "strip: the active track A is untouched by B's fader");
        strip.pan().setPan(0.5f, juce::sendNotificationSync);
        check(std::abs(h.track(h.b).getStereoPan() - 0.5f) < 1e-4f, "strip: pan reaches track B");
        strip.commitPreGainText("+3");
        check(std::abs(h.track(h.b).getPreGainDb() - 3.0f) < 1e-4f && strip.preGainText() == "+3.0", "strip: pre-gain commit reaches track B with canonical text");
        check(strip.routingCaption(0) == "Audio Input" && strip.routingCaption(1) == "Audio Output", "strip: audio row offers Audio Input + Audio Output");
        check(strip.chooseRoutingByText(0, "Stereo" + track_channel_options::labelDash() + "In 1: Mic 1 + In 2: Mic 2"), "strip: the stereo pair is offered like the Inspector lists it");
        check(h.track(h.b).getInputAssignment().kind == TrackInputKind::StereoPair, "strip: audio input pick reaches track B");
        check(strip.chooseRoutingByText(1, "Bus"), "strip: the group is offered as an output");
        check(h.track(h.b).getRoutedOutputTrackId() == h.g, "strip: output routing reaches track B");
        check(strip.chooseSendDestinationByText(0, "Bus"), "strip: the group is offered as a send destination");
        check(findTrackSendVectorIndexForUiSlot(h.track(h.b).getSends(), 0) >= 0, "strip: send destination pick inserts the send on track B");
        strip.refreshFromSession(*h.snap(), false);
        strip.commitSendAmountText(0, "-6");
        const int si = findTrackSendVectorIndexForUiSlot(h.track(h.b).getSends(), 0);
        check(si >= 0 && std::abs(h.track(h.b).getSend(si).amountLinear - 0.501f) < 0.01f && strip.sendAmountText(0).startsWith("-6.0"),
              "strip: send amount reaches track B's send with canonical text");
        strip.clickBaseButtonForTest(track_strip_glyphs::StripButtonKind::Mute);
        check(h.track(h.b).isMuted(), "strip: Mute toggles track B");
        bool allB = true;
        for (const auto& c : h.calls)
        {
            if (!c.startsWith("activate:") && !c.endsWith(":" + juce::String((juce::int64)h.b)))
            {
                allB = false;
            }
        }
        check(allB && !h.calls.empty(), "strip: every action carried track B's id (never the active track)");
        check(h.session.getActiveTrackId() == h.a, "strip: no action required changing the active track");
        strip.refreshFromSession(*h.snap(), false);
        juce::String report;
        const bool audioBandsOk = strip.verifyChildrenInsideBands(report);
        check(audioBandsOk, ("strip: every visible control lies inside its band (audio)\n" + report).toRawUTF8());

        // Refresh never fights the user: a value field with focus is left alone (simulated by the
        // guard path: dragging flag is private, so exercise the text path through commit instead).
        h.session.setTrackChannelFaderGain(h.b, 1.0f);
        strip.refreshFromSession(*h.snap(), true);
        check(strip.fader().getValueFieldText() == "0.00", "strip: a session change made elsewhere is mirrored by the strip (fader back to 0 dB)");

        // Meters: interest only while showing (offscreen component is not showing -> none).
        std::vector<TrackId> interest;
        strip.collectMeterInterest(interest);
        check(interest.empty(), "strip: an unseen strip reports no meter interest (no audio-thread cost)");
        // Overload acknowledgement round trip through the hub must not echo (no recursion): a
        // lamp click on this strip reaches the hub, the hub tells every view (this one included).
        level_meter::Reading over;
        over.peak[0] = 1.5f;
        over.overs[0] = 3;
        over.blocks = 1;
        over.samples = 64;
        over.channels = 2;
        strip.meter().pushReading(over, 1.0);
        check(strip.meter().isOverloadLatched(), "strip: an over-full-scale window latches the lamp");
        strip.meter().resetOverloadLatch(); // the click path -> hub.acknowledgeOverload(B) -> strip again (silently)
        check(!strip.meter().isOverloadLatched(), "strip: lamp click clears the latch and the hub round trip returns");
        strip.meter().pushReading(over, 2.0);
        hub.acknowledgeOverload(h.b);
        check(!strip.meter().isOverloadLatched(), "strip: an acknowledgement from another view clears this strip's latch");

        // Other kinds.
        MixerChannelStrip groupStrip(h.g, h.bindings, &hub);
        groupStrip.setBounds(0, 0, mixer_layout::kStripWidthPx, 760);
        groupStrip.applyLayout(layout);
        groupStrip.refreshFromSession(*h.snap(), false);
        check(groupStrip.kindText() == "GROUP" && groupStrip.routingCaption(0) == "Audio Output" && !groupStrip.routingRowVisible(1)
                  && !groupStrip.isPreGainVisible() && !groupStrip.baseButtonVisible(track_strip_glyphs::StripButtonKind::Arm)
                  && groupStrip.baseButtonVisible(track_strip_glyphs::StripButtonKind::Mute),
              "strip: group row = Audio Output, no pre-gain, no arm / monitor / power, Mute present");
        check(groupStrip.sendRowVisible(0), "strip: group row offers sends");

        MixerChannelStrip masterStrip(h.m, h.bindings, &hub);
        masterStrip.setBounds(0, 0, mixer_layout::kStripWidthPx, 760);
        masterStrip.applyLayout(layout);
        masterStrip.refreshFromSession(*h.snap(), false);
        check(masterStrip.kindText() == "STEREO OUT" && masterStrip.routingCaption(0) == "Device output" && masterStrip.routingText(0) == "Test device: Out 1 + Out 2",
              "strip: Stereo Out shows the device output as information");
        check(!masterStrip.sendRowVisible(0) && masterStrip.fader().isVisible() && masterStrip.meter().isVisible() && !masterStrip.isPreGainVisible(),
              "strip: Stereo Out has fader + meter, no sends, no pre-gain");
        std::vector<TrackId> masterInterest;
        masterStrip.collectMeterInterest(masterInterest);
        check(masterInterest.empty(), "strip: the Stereo Out strip never claims a bank slot (its window is the master's)");
        report.clear();
        const bool masterBandsOk = masterStrip.verifyChildrenInsideBands(report);
        check(masterBandsOk, ("strip: every visible control inside its band (Stereo Out)\n" + report).toRawUTF8());

        MixerChannelStrip midiStrip(h.midi, h.bindings, &hub);
        midiStrip.setBounds(0, 0, mixer_layout::kStripWidthPx, 760);
        midiStrip.applyLayout(layout);
        midiStrip.refreshFromSession(*h.snap(), false);
        check(midiStrip.kindText() == "MIDI" && midiStrip.routingCaption(0) == "MIDI Input" && midiStrip.routingCaption(1) == "Input Channel"
                  && midiStrip.routingCaption(2) == "MIDI To" && midiStrip.routingCaption(3) == "MIDI Channel",
              "strip: MIDI row = MIDI Input, Input Channel, MIDI To, MIDI Channel");
        check(!midiStrip.fader().isVisible() && !midiStrip.pan().isVisible() && !midiStrip.meter().isVisible() && !midiStrip.sendRowVisible(0)
                  && midiStrip.visibleInsertRowCount(InsertStage::Pre) == 0,
              "strip: MIDI row has no fader, pan, meter, sends or inserts");
        check(midiStrip.baseButtonVisible(track_strip_glyphs::StripButtonKind::Power) && midiStrip.baseButtonVisible(track_strip_glyphs::StripButtonKind::Monitor)
                  && midiStrip.baseButtonVisible(track_strip_glyphs::StripButtonKind::Arm),
              "strip: MIDI row keeps Power / Mute / Monitor / R");

        // Pan: the Inspector's component at the Inspector's 36 px; a handler-level drag on the stick
        // of this NON-active strip pans track B (press on the stick, drag to the x for -0.5, release).
        check(strip.pan().isVisible() && strip.pan().getHeight() == mixer_layout::kPanFieldHeightPx, "pan: the strip gives the Inspector's pan field its 36 px");
        {
            InspectorPanControl& pan = strip.pan();
            strip.pan().setPan(0.0f, juce::dontSendNotification);
            const juce::Rectangle<int> hit = pan.getMarkerHitRectForTest();
            check(!hit.isEmpty(), "pan: the stick has a hit zone at 36 px");
            const auto src = juce::Desktop::getInstance().getMainMouseSource();
            const auto now = juce::Time::getCurrentTime();
            const juce::Point<float> down = hit.getCentre().toFloat();
            pan.mouseDown(juce::MouseEvent(src, down, juce::ModifierKeys::leftButtonModifier, 0.f, 0.f, 0.f, 0.f, 0.f, &pan, &pan, now, down, now, 1, false));
            const juce::Point<float> to(pan.xForPanForTest(-0.5f), down.y);
            pan.mouseDrag(juce::MouseEvent(src, to, juce::ModifierKeys::leftButtonModifier, 0.f, 0.f, 0.f, 0.f, 0.f, &pan, &pan, now, down, now, 1, true));
            pan.mouseUp(juce::MouseEvent(src, to, juce::ModifierKeys(), 0.f, 0.f, 0.f, 0.f, 0.f, &pan, &pan, now, down, now, 1, false));
            check(std::abs(h.track(h.b).getStereoPan() + 0.5f) < 0.03f, "pan: a stick drag on the strip pans track B (not the active track A)");
            check(std::abs(h.track(h.a).getStereoPan()) < 1e-6f, "pan: track A untouched by B's drag");
            // Ctrl-click = centre, exactly as in the Inspector.
            pan.mouseDown(juce::MouseEvent(src, down, juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier, 0.f, 0.f, 0.f, 0.f, 0.f, &pan, &pan, now, down, now, 1, false));
            pan.mouseUp(juce::MouseEvent(src, down, juce::ModifierKeys(), 0.f, 0.f, 0.f, 0.f, 0.f, &pan, &pan, now, down, now, 1, false));
            check(std::abs(h.track(h.b).getStereoPan()) < 1e-6f, "pan: Ctrl-click resets track B to centre");
        }

        // Inserts: six Post rows → every row exists, the default band shows two, the list scrolls,
        // the last row's menu action acts on ITS slot (not a reused neighbour's).
        {
            std::vector<InspectorInsertRow> rows;
            for (int i = 0; i < 6; ++i)
            {
                InspectorInsertRow r;
                r.slotId = static_cast<InsertSlotId>(100 + i);
                r.stage = InsertStage::Post;
                r.displayName = "Delay " + juce::String(i + 1);
                r.unavailable = (i == 3);
                rows.push_back(r);
            }
            h.insertRows[h.b] = rows;
            strip.refreshFromSession(*h.snap(), false);
            check(strip.insertRowCount(InsertStage::Post) == 6, "inserts: all six rows exist in the strip");
            check(strip.visibleInsertRowCount(InsertStage::Post) == 2 && strip.isInsertListScrollable(InsertStage::Post),
                  "inserts: the default band shows two rows and the list scrolls");
            check(strip.insertAddButtonBounds(InsertStage::Post).getY() >= strip.insertListBounds(InsertStage::Post).getBottom(),
                  "inserts: '+ Add' sits below the list, outside it");
            check(strip.scrollInsertListToRow(InsertStage::Post, 5) && strip.visibleInsertRowCount(InsertStage::Post) == 2,
                  "inserts: scrolling to the last row keeps two rows visible");
            h.calls.clear();
            check(strip.performInsertRowAction(InsertStage::Post, 5, 1) && h.calls.back() == "edit:" + juce::String((juce::int64)h.b) + ":105",
                  "inserts: the last row opens ITS slot (105) on track B");
            check(strip.performInsertRowAction(InsertStage::Post, 5, 4) && h.calls.back() == "stage:" + juce::String((juce::int64)h.b) + ":105:pre:0",
                  "inserts: 'move to Pre' sends the row's slot to the end of the empty Pre chain");
            check(strip.performInsertRowAction(InsertStage::Post, 2, 3) && h.calls.back() == "reorder:" + juce::String((juce::int64)h.b) + ":102:4",
                  "inserts: 'move down' reorders the row's own slot");
            check(!strip.performInsertRowAction(InsertStage::Post, 5, 3), "inserts: 'move down' on the last row is refused");
            check(strip.performInsertRowAction(InsertStage::Post, 5, 5) && h.calls.back() == "remove:" + juce::String((juce::int64)h.b) + ":105",
                  "inserts: 'remove' on the last row removes slot 105");
            strip.refreshFromSession(*h.snap(), false);
            check(strip.insertRowCount(InsertStage::Post) == 5 && strip.insertRowText(InsertStage::Post, 4) == "Delay 5",
                  "inserts: after the remove the strip shows the five remaining rows in order");
            // A taller band shows more rows: grow Post inserts through the layout model.
            mixer_layout::SectionHeights tall;
            tall.set(mixer_layout::Section::PostInserts, mixer_layout::kInsertsFixedChromeHeightPx + 5 * mixer_layout::kInsertRowHeightPx);
            strip.setBounds(0, 0, mixer_layout::kStripWidthPx, 900);
            strip.applyLayout(mixer_layout::computeLayout(all, tall, mixer_layout::kStripWidthPx, 900));
            check(strip.visibleInsertRowCount(InsertStage::Post) == 5 && !strip.isInsertListScrollable(InsertStage::Post),
                  "inserts: a band of five rows shows all five without a scroll bar");
            juce::String report2;
            const bool tallOk = strip.verifyChildrenInsideBands(report2);
            check(tallOk, ("inserts: list and add button inside the band at every height\n" + report2).toRawUTF8());
            strip.setBounds(0, 0, mixer_layout::kStripWidthPx, 760);
            strip.applyLayout(layout);
        }

        // Orphaning: a deleted row marks its strip.
        h.session.removeTrack(h.b);
        strip.refreshFromSession(*h.snap(), false);
        check(strip.isOrphaned(), "strip: a row that left the snapshot marks its strip orphaned");
    }

    // --- UiLayoutSettingsStore ---------------------------------------------------------------------
    void testLayoutStore()
    {
        const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-mixer-tests-ui-layout.xml");
        (void)file.deleteFile();
        {
            UiLayoutSettingsStore store(file);
            store.loadFromFile();
            check(!store.getMixerWindowBounds().has_value() && !store.getMixerSectionShown("sends").has_value(), "store: absent file -> no mixer values");
            store.setTrackHeaderColumnWidthPx(149);
            store.setMixerWindowBounds({ 40, 50, 1000, 640 });
            store.setMixerSectionShown("sends", false);
            store.setMixerSectionShown("meters", true);
            store.save();
        }
        {
            UiLayoutSettingsStore store(file);
            store.loadFromFile();
            check(store.getTrackHeaderColumnWidthPx().value_or(0) == 149, "store: header width survives beside the mixer values");
            check(store.getMixerWindowBounds().has_value() && *store.getMixerWindowBounds() == juce::Rectangle<int>(40, 50, 1000, 640), "store: mixer bounds round trip");
            check(store.getMixerSectionShown("sends") == std::optional<bool>(false) && store.getMixerSectionShown("meters") == std::optional<bool>(true)
                      && !store.getMixerSectionShown("routing").has_value(),
                  "store: section flags round trip; unset keys stay absent");
        }
        // Malformed bounds are ignored, the rest still loads.
        (void)file.replaceWithText("<UI_LAYOUT version=\"1\"><TRACK_HEADER_COLUMN widthPx=\"150\"/><MIXER_WINDOW x=\"abc\" y=\"1\" width=\"0\" height=\"5\"/><MIXER_SECTIONS sends=\"maybe\" faders=\"0\"/></UI_LAYOUT>");
        {
            UiLayoutSettingsStore store(file);
            store.loadFromFile();
            check(!store.getMixerWindowBounds().has_value() && store.getTrackHeaderColumnWidthPx().value_or(0) == 150, "store: malformed mixer bounds ignored, header width kept");
            check(!store.getMixerSectionShown("sends").has_value() && store.getMixerSectionShown("faders") == std::optional<bool>(false), "store: non 0/1 flags ignored, valid ones kept");
        }
        (void)file.deleteFile();
    }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    testTrackMeterBank();
    testLevelMeterHub();
    testSectionLayout();
    testDividers();
    testStripBinding();
    testLayoutStore();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — `Session.cpp` references these plugin / instrument entry points; this harness never
// hosts a plug-in or an instrument, so the stubs are never executed.
// ---------------------------------------------------------------------------
#include "instruments/InstrumentTrackController.h"
#include "plugins/PluginInsertHost.h"

PluginTrackChain PluginInsertHost::exportChain(TrackId) const { jassertfalse; return {}; }
void PluginInsertHost::importChain(TrackId, const PluginTrackChain&) { jassertfalse; }
void PluginInsertHost::removeAllPlugins() noexcept { jassertfalse; }
ProjectFileExperimentalInstrumentTrackV1
InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
