// =============================================================================
// LiveMidiInputCoordinator.cpp — see header for the role and thread story.
// =============================================================================

#include "app/LiveMidiInputCoordinator.h"

#include <algorithm>

#include "app/InstrumentRuntimeCoordinator.h"
#include "audio/LatencySettingsStore.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

namespace
{
    constexpr int kTimerHz = 30;
    constexpr double kActivityHoldMs = 150.0;
} // namespace

/// Per-slot MIDI thread entry: forwards into the bus and nothing else.
struct LiveMidiInputCoordinator::SlotCallback final : public juce::MidiInputCallback
{
    SlotCallback(live_midi::LiveMidiInputBus& bus, const int slot) noexcept : bus_(bus), slot_(slot) {}

    void handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message) override
    {
        // [MIDI device thread] Lock-free push; the audio thread consumes next block.
        bus_.deviceThread_push(slot_, message);
    }

    live_midi::LiveMidiInputBus& bus_;
    const int slot_;
};

LiveMidiInputCoordinator::LiveMidiInputCoordinator(Session& session,
                                                   juce::AudioDeviceManager& deviceManager,
                                                   PlaybackEngine& playbackEngine,
                                                   InstrumentRuntimeCoordinator& instrumentRuntime,
                                                   LatencySettingsStore& latencyStore,
                                                   Callbacks callbacks)
    : session_(session)
    , deviceManager_(deviceManager)
    , playbackEngine_(playbackEngine)
    , instrumentRuntime_(instrumentRuntime)
    , latencyStore_(latencyStore)
    , callbacks_(std::move(callbacks))
{
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        slots_[(size_t)i].callback = std::make_unique<SlotCallback>(bus_, i);
    }
    playbackEngine_.setLiveMidiInputBus(&bus_);
    playbackEngine_.setLiveMidiRecordPlacementOffsetSamples(
        -(std::int64_t)juce::jmax(0, latencyStore_.getReportedOutputLatencySamples()));

    deviceListConnection_ = std::make_unique<juce::MidiDeviceListConnection>(
        juce::MidiDeviceListConnection::make([this] { refreshDevicesAndRouting(); }));

    refreshDevicesAndRouting();
    startTimerHz(kTimerHz);
}

LiveMidiInputCoordinator::~LiveMidiInputCoordinator()
{
    stopTimer();
    deviceListConnection_.reset();
    // Detach from the engine FIRST and drain the callback that may still hold the bus pointer it
    // loaded this block, then unregister every device callback (the device manager's dispatch
    // lock guarantees no in-flight call survives `removeMidiInputDeviceCallback`), then let the
    // slots (and their callbacks) die.
    playbackEngine_.setLiveMidiInputBus(nullptr);
    (void)playbackEngine_.waitForAudioCallbackExit(100.0);
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        DeviceSlot& s = slots_[(size_t)i];
        if (s.enabledOnManager)
        {
            deviceManager_.removeMidiInputDeviceCallback(s.identifier, s.callback.get());
            s.enabledOnManager = false;
        }
    }
}

// ---------------------------------------------------------------------------- devices
int LiveMidiInputCoordinator::ensureSlotForDevice(const juce::String& identifier, const juce::String& name)
{
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        if (slots_[(size_t)i].identifier == identifier)
        {
            slots_[(size_t)i].name = name.isNotEmpty() ? name : slots_[(size_t)i].name;
            return i;
        }
    }
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        if (slots_[(size_t)i].identifier.isEmpty())
        {
            slots_[(size_t)i].identifier = identifier;
            slots_[(size_t)i].name = name;
            return i;
        }
    }
    return -1; // more than kMaxDeviceSlots devices referenced — extra ones are not routed
}

void LiveMidiInputCoordinator::enableSlotOnManager(DeviceSlot& slot, const int slotIndex, const bool enable)
{
    juce::ignoreUnused(slotIndex);
    if (enable == slot.enabledOnManager)
    {
        return;
    }
    if (enable)
    {
        if (!deviceManager_.isMidiInputDeviceEnabled(slot.identifier))
        {
            deviceManager_.setMidiInputDeviceEnabled(slot.identifier, true);
        }
        if (deviceManager_.isMidiInputDeviceEnabled(slot.identifier))
        {
            deviceManager_.addMidiInputDeviceCallback(slot.identifier, slot.callback.get());
            slot.enabledOnManager = true;
            if (callbacks_.logLine)
            {
                callbacks_.logLine("[LiveMidi] input opened: \"" + slot.name + "\" (" + slot.identifier + ")");
            }
        }
    }
    else
    {
        deviceManager_.removeMidiInputDeviceCallback(slot.identifier, slot.callback.get());
        deviceManager_.setMidiInputDeviceEnabled(slot.identifier, false);
        slot.enabledOnManager = false;
        if (callbacks_.logLine)
        {
            callbacks_.logLine("[LiveMidi] input closed: \"" + slot.name + "\"");
        }
    }
}

bool LiveMidiInputCoordinator::anyTrackUsesAllInputs() const
{
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return false;
    }
    for (int i = 0; i < snap->getNumTracks(); ++i)
    {
        const Track& t = snap->getTrack(i);
        if (trackKindAcceptsLiveMidiInput(t.getKind())
            && t.getMidiInputAssignment().mode == TrackMidiInputMode::AllEnabled)
        {
            return true;
        }
    }
    return false;
}

std::set<juce::String> LiveMidiInputCoordinator::deviceIdentifiersReferencedByTracks() const
{
    std::set<juce::String> ids;
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return ids;
    }
    for (int i = 0; i < snap->getNumTracks(); ++i)
    {
        const Track& t = snap->getTrack(i);
        const TrackMidiInputAssignment& mi = t.getMidiInputAssignment();
        if (trackKindAcceptsLiveMidiInput(t.getKind()) && mi.mode == TrackMidiInputMode::Device
            && mi.deviceIdentifier.isNotEmpty())
        {
            ids.insert(mi.deviceIdentifier);
        }
    }
    return ids;
}

void LiveMidiInputCoordinator::refreshDevicesAndRouting()
{
    const juce::Array<juce::MidiDeviceInfo> present = juce::MidiInput::getAvailableDevices();
    for (auto& s : slots_)
    {
        s.present = false;
    }
    // Present devices keep/get a slot (stable identity by JUCE identifier).
    for (const auto& d : present)
    {
        const int slot = ensureSlotForDevice(d.identifier, d.name);
        if (slot >= 0)
        {
            slots_[(size_t)slot].present = true;
        }
    }
    // Only referenced devices are opened: a track in Device mode opens that device; any track in
    // "All" mode opens every present device. Unreferenced devices are closed again.
    const bool all = anyTrackUsesAllInputs();
    const std::set<juce::String> referenced = deviceIdentifiersReferencedByTracks();
    lastReferencedDevices_ = referenced;
    lastAllMode_ = all;
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        DeviceSlot& s = slots_[(size_t)i];
        if (s.identifier.isEmpty())
        {
            continue;
        }
        const bool wanted = s.present && (all || referenced.count(s.identifier) != 0);
        enableSlotOnManager(s, i, wanted);
        if (!s.present && s.enabledOnManager)
        {
            // Device vanished: the manager dropped it; forget our registration so a return re-opens.
            deviceManager_.removeMidiInputDeviceCallback(s.identifier, s.callback.get());
            s.enabledOnManager = false;
        }
    }
    publishedRouting_ = nullptr; // force republish with the new slot table
    rebuildRoutingIfChanged();
}

std::vector<LiveMidiInputDeviceOption> LiveMidiInputCoordinator::availableDevicesFor(const TrackId forTrack) const
{
    std::vector<LiveMidiInputDeviceOption> out;
    for (const auto& s : slots_)
    {
        if (s.present && s.identifier.isNotEmpty())
        {
            out.push_back({ s.identifier, s.name, true });
        }
    }
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        const int ix = snap->findTrackIndexById(forTrack);
        if (ix >= 0)
        {
            const TrackMidiInputAssignment& mi = snap->getTrack(ix).getMidiInputAssignment();
            if (mi.mode == TrackMidiInputMode::Device && mi.deviceIdentifier.isNotEmpty())
            {
                const bool listed = std::any_of(out.begin(), out.end(), [&mi](const LiveMidiInputDeviceOption& o) {
                    return o.identifier == mi.deviceIdentifier;
                });
                if (!listed)
                {
                    out.push_back({ mi.deviceIdentifier, mi.deviceName, false });
                }
            }
        }
    }
    return out;
}

bool LiveMidiInputCoordinator::isAssignedDevicePresent(const TrackId trackId) const
{
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    const int ix = snap != nullptr ? snap->findTrackIndexById(trackId) : -1;
    if (ix < 0)
    {
        return false;
    }
    const TrackMidiInputAssignment& mi = snap->getTrack(ix).getMidiInputAssignment();
    switch (mi.mode)
    {
    case TrackMidiInputMode::AllEnabled:
        return std::any_of(slots_.begin(), slots_.end(), [](const DeviceSlot& s) { return s.present; });
    case TrackMidiInputMode::Device:
        return std::any_of(slots_.begin(), slots_.end(), [&mi](const DeviceSlot& s) {
            return s.present && s.identifier == mi.deviceIdentifier;
        });
    case TrackMidiInputMode::None:
    default:
        return false;
    }
}

int LiveMidiInputCoordinator::slotForDeviceIdentifierForDiagnostics(const juce::String& identifier) const
{
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        if (slots_[(size_t)i].identifier == identifier && slots_[(size_t)i].present)
        {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------- monitor / arm
TrackId LiveMidiInputCoordinator::destinationForTrack(const TrackId trackId) const
{
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    const int ix = snap != nullptr ? snap->findTrackIndexById(trackId) : -1;
    if (ix < 0)
    {
        return kInvalidTrackId;
    }
    const Track& t = snap->getTrack(ix);
    if (t.getKind() == TrackKind::Instrument)
    {
        return trackId;
    }
    if (t.getKind() == TrackKind::Midi)
    {
        return t.getMidiDestinationTrackId();
    }
    return kInvalidTrackId;
}

void LiveMidiInputCoordinator::refreshProxyDestinationsForTrackChange(const TrackId trackId)
{
    if (!callbacks_.refreshProxyDestination)
    {
        return;
    }
    const TrackId dest = destinationForTrack(trackId);
    if (dest != kInvalidTrackId)
    {
        callbacks_.refreshProxyDestination(dest);
    }
}

void LiveMidiInputCoordinator::setMonitorEnabled(const TrackId trackId, const bool enabled)
{
    const bool was = monitor_.count(trackId) != 0;
    if (was == enabled)
    {
        return;
    }
    if (enabled)
    {
        monitor_.insert(trackId);
    }
    else
    {
        monitor_.erase(trackId);
    }
    // Order matters: publish the routing first (the audio thread releases this row's live notes
    // into the host that owns them), THEN let the proxy policy swap sources if it wants to.
    publishedRouting_ = nullptr;
    rebuildRoutingIfChanged();
    refreshProxyDestinationsForTrackChange(trackId);
    if (callbacks_.onUiStateChanged)
    {
        callbacks_.onUiStateChanged();
    }
}

void LiveMidiInputCoordinator::setRecordArmed(const TrackId trackId, const bool armed)
{
    const bool was = armed_.count(trackId) != 0;
    if (was == armed)
    {
        return;
    }
    if (armed)
    {
        armed_.insert(trackId);
    }
    else
    {
        armed_.erase(trackId);
    }
    publishedRouting_ = nullptr;
    rebuildRoutingIfChanged();
    if (callbacks_.onUiStateChanged)
    {
        callbacks_.onUiStateChanged();
    }
}

void LiveMidiInputCoordinator::clearRuntimeStateForProjectReplace()
{
    const std::set<TrackId> monitored = monitor_;
    monitor_.clear();
    armed_.clear();
    abortTake();
    rows_.clear();
    lastActivityMsByTrack_.clear();
    activeNow_.clear();
    publishedRouting_ = nullptr;
    refreshDevicesAndRouting();
    for (const TrackId t : monitored)
    {
        refreshProxyDestinationsForTrackChange(t);
    }
    if (callbacks_.onUiStateChanged)
    {
        callbacks_.onUiStateChanged();
    }
}

bool LiveMidiInputCoordinator::liveMonitorRequestedForDestination(const TrackId destination) const
{
    if (destination == kInvalidTrackId)
    {
        return false;
    }
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return false;
    }
    for (const TrackId tid : monitor_)
    {
        const int ix = snap->findTrackIndexById(tid);
        if (ix < 0)
        {
            continue;
        }
        const Track& t = snap->getTrack(ix);
        if (t.getMidiInputAssignment().mode == TrackMidiInputMode::None)
        {
            continue;
        }
        if (destinationForTrack(tid) == destination)
        {
            return true;
        }
    }
    return false;
}

std::vector<TrackId> LiveMidiInputCoordinator::armedTracksReadyToRecord() const
{
    std::vector<TrackId> out;
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return out;
    }
    for (int i = 0; i < snap->getNumTracks(); ++i)
    {
        const Track& t = snap->getTrack(i);
        if (!trackKindAcceptsLiveMidiInput(t.getKind()) || armed_.count(t.getId()) == 0)
        {
            continue;
        }
        if (t.getMidiInputAssignment().mode == TrackMidiInputMode::None)
        {
            continue;
        }
        out.push_back(t.getId());
    }
    return out;
}

bool LiveMidiInputCoordinator::isTrackMidiActive(const TrackId trackId) const noexcept
{
    return activeNow_.count(trackId) != 0;
}

juce::String LiveMidiInputCoordinator::describeInputStatus(const TrackId trackId) const
{
    const auto snap = session_.loadSessionSnapshotForAudioThread();
    const int ix = snap != nullptr ? snap->findTrackIndexById(trackId) : -1;
    if (ix < 0)
    {
        return {};
    }
    const Track& t = snap->getTrack(ix);
    const TrackMidiInputAssignment& mi = t.getMidiInputAssignment();
    if (mi.mode == TrackMidiInputMode::None)
    {
        return {};
    }
    juce::StringArray parts;
    if (mi.mode == TrackMidiInputMode::Device && !isAssignedDevicePresent(trackId))
    {
        parts.add("MIDI device missing: " + (mi.deviceName.isNotEmpty() ? mi.deviceName : mi.deviceIdentifier)
                  + " (assignment kept)");
    }
    else if (mi.mode == TrackMidiInputMode::AllEnabled && !isAssignedDevicePresent(trackId))
    {
        parts.add("No MIDI input device present");
    }
    const TrackId dest = destinationForTrack(trackId);
    if (dest == kInvalidTrackId)
    {
        parts.add(t.getKind() == TrackKind::Midi ? "No MIDI To destination: live MIDI is recorded but not heard"
                                                 : "No destination");
    }
    else
    {
        ExperimentalInstrumentHost* const primary = instrumentRuntime_.getInstrumentHostForTrack(dest);
        const bool primaryOk = primary != nullptr && primary->hasInstrument();
        const bool secondaryActive = instrumentRuntime_.isSecondaryTransportActive(dest);
        const bool overrideActive = callbacks_.isLiveMonitorOverrideActive && callbacks_.isLiveMonitorOverrideActive(dest);
        if (!primaryOk && secondaryActive && overrideActive)
        {
            parts.add("Monitoring through the Secondary instrument (temporary, while Monitor is on)");
        }
        else if (!primaryOk && secondaryActive)
        {
            parts.add("Playing through the Secondary instrument");
        }
        else if (!primaryOk && !secondaryActive)
        {
            parts.add("No playable instrument: live MIDI is recorded but not heard");
        }
    }
    // Overflow of any device feeding this row (relaxed counters; informational).
    for (int i = 0; i < live_midi::kMaxDeviceSlots; ++i)
    {
        const DeviceSlot& s = slots_[(size_t)i];
        if (!s.enabledOnManager)
        {
            continue;
        }
        const bool feeds = mi.mode == TrackMidiInputMode::AllEnabled
                           || (mi.mode == TrackMidiInputMode::Device && s.identifier == mi.deviceIdentifier);
        if (feeds && bus_.deviceOverflowCount(i) > 0)
        {
            parts.add("MIDI input overflow on " + s.name + " (" + juce::String((int)bus_.deviceOverflowCount(i))
                      + " dropped)");
            break;
        }
    }
    return parts.joinIntoString("\n");
}

// ---------------------------------------------------------------------------- routing
std::shared_ptr<const live_midi::RoutingSnapshot> LiveMidiInputCoordinator::buildRoutingSnapshot()
{
    auto snap = std::make_shared<live_midi::RoutingSnapshot>();
    publishedRouteTracks_.clear();
    const auto session = session_.loadSessionSnapshotForAudioThread();
    if (session == nullptr)
    {
        return snap;
    }
    for (int i = 0; i < session->getNumTracks(); ++i)
    {
        const Track& t = session->getTrack(i);
        if (!trackKindAcceptsLiveMidiInput(t.getKind()))
        {
            continue;
        }
        const TrackMidiInputAssignment& mi = t.getMidiInputAssignment();
        if (mi.mode == TrackMidiInputMode::None)
        {
            continue;
        }
        if ((int)snap->routes.size() >= live_midi::kMaxRoutes)
        {
            break;
        }
        live_midi::Route r;
        r.trackId = t.getId();
        r.channelFilter = mi.channelFilter;
        r.monitor = monitor_.count(t.getId()) != 0;
        r.capture = armed_.count(t.getId()) != 0;
        if (mi.mode == TrackMidiInputMode::AllEnabled)
        {
            r.deviceSlot = live_midi::kDeviceSlotAll;
        }
        else
        {
            r.deviceSlot = live_midi::kDeviceSlotMissing;
            for (int si = 0; si < live_midi::kMaxDeviceSlots; ++si)
            {
                const DeviceSlot& s = slots_[(size_t)si];
                if (s.present && s.enabledOnManager && s.identifier == mi.deviceIdentifier)
                {
                    r.deviceSlot = si;
                    break;
                }
            }
        }
        snap->routes.push_back(r);
        publishedRouteTracks_.push_back(t.getId());
    }
    return snap;
}

void LiveMidiInputCoordinator::rebuildRoutingIfChanged()
{
    std::shared_ptr<const live_midi::RoutingSnapshot> next = buildRoutingSnapshot();
    bool same = publishedRouting_ != nullptr && publishedRouting_->routes.size() == next->routes.size();
    if (same)
    {
        for (size_t i = 0; i < next->routes.size(); ++i)
        {
            const auto& a = publishedRouting_->routes[i];
            const auto& b = next->routes[i];
            if (a.trackId != b.trackId || a.deviceSlot != b.deviceSlot || a.channelFilter != b.channelFilter
                || a.monitor != b.monitor || a.capture != b.capture)
            {
                same = false;
                break;
            }
        }
    }
    if (same)
    {
        return;
    }
    auto mutableNext = std::const_pointer_cast<live_midi::RoutingSnapshot>(next);
    mutableNext->revision = ++routingRevision_;
    publishedRouting_ = next;
    lastActivityCounts_.assign(next->routes.size(), 0);
    for (size_t i = 0; i < next->routes.size(); ++i)
    {
        lastActivityCounts_[i] = bus_.routeActivityCount((int)i);
    }
    bus_.publishRouting(next);
}

// ---------------------------------------------------------------------------- timer
void LiveMidiInputCoordinator::timerCallback()
{
    // Routing follows the session: any publish (input assignment, MIDI To, kind, delete) is
    // picked up here; identical routes never republish.
    const auto session = session_.loadSessionSnapshotForAudioThread();
    if (session.get() != lastSeenSessionSnapshot_)
    {
        lastSeenSessionSnapshot_ = session.get();
        // Device set changed (a track picked / dropped a device, "All" toggled, project loaded):
        // (re)open devices — otherwise just rebuild the cheap routing table.
        const std::set<juce::String> referenced = deviceIdentifiersReferencedByTracks();
        const bool all = anyTrackUsesAllInputs();
        if (referenced != lastReferencedDevices_ || all != lastAllMode_)
        {
            refreshDevicesAndRouting();
        }
        else
        {
            rebuildRoutingIfChanged();
        }
    }
    playbackEngine_.setLiveMidiRecordPlacementOffsetSamples(
        -(std::int64_t)juce::jmax(0, latencyStore_.getReportedOutputLatencySamples()));
    drainCaptureRing();
    updateActivityFromBus();
}

void LiveMidiInputCoordinator::drainCaptureRing()
{
    live_midi::CapturedEvent e;
    int budget = live_midi::kCaptureRingCapacity;
    while (budget-- > 0 && bus_.popCaptured(e))
    {
        RowCapture& row = rows_[e.trackId];
        live_midi_take::TakeEvent te;
        te.timelineSample = e.timelineSample;
        te.bytes[0] = e.bytes[0];
        te.bytes[1] = e.bytes[1];
        te.bytes[2] = e.bytes[2];
        te.size = e.size;
        te.transportPlaying = e.transportPlaying;
        te.overflowMarker = e.overflowMarker;
        row.tracker.feed(te);
        if (row.inTake)
        {
            row.takeEvents.push_back(te);
        }
    }
}

void LiveMidiInputCoordinator::updateActivityFromBus()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (publishedRouting_ != nullptr)
    {
        for (size_t i = 0; i < publishedRouting_->routes.size() && i < lastActivityCounts_.size(); ++i)
        {
            const std::uint32_t c = bus_.routeActivityCount((int)i);
            if (c != lastActivityCounts_[i])
            {
                lastActivityCounts_[i] = c;
                lastActivityMsByTrack_[publishedRouting_->routes[i].trackId] = nowMs;
            }
        }
    }
    std::set<TrackId> active;
    for (const auto& [tid, ms] : lastActivityMsByTrack_)
    {
        if (nowMs - ms <= kActivityHoldMs)
        {
            active.insert(tid);
        }
    }
    if (active != activeNow_)
    {
        activeNow_ = std::move(active);
        if (callbacks_.onUiStateChanged)
        {
            callbacks_.onUiStateChanged();
        }
    }
}

// ---------------------------------------------------------------------------- take capture
void LiveMidiInputCoordinator::beginTake(const std::int64_t recordStartSample, const double sampleRate)
{
    abortTake();
    drainCaptureRing(); // everything received so far belongs to the pre-take state
    takeTracks_ = armedTracksReadyToRecord();
    if (takeTracks_.empty())
    {
        return;
    }
    takeActive_ = true;
    takeStartSample_ = recordStartSample;
    takeSampleRate_ = sampleRate;
    captureOverflowAtTakeStart_ = bus_.captureOverflowCount();
    for (const TrackId tid : takeTracks_)
    {
        RowCapture& row = rows_[tid];
        row.trackerAtTakeStart = row.tracker;
        row.takeEvents.clear();
        row.inTake = true;
    }
    if (callbacks_.logLine)
    {
        callbacks_.logLine("[LiveMidi] take begins at sample " + juce::String((juce::int64)recordStartSample)
                           + " for " + juce::String((int)takeTracks_.size()) + " row(s)");
    }
    if (callbacks_.onUiStateChanged)
    {
        callbacks_.onUiStateChanged();
    }
}

void LiveMidiInputCoordinator::abortTake()
{
    if (!takeActive_)
    {
        return;
    }
    takeActive_ = false;
    for (auto& [tid, row] : rows_)
    {
        row.inTake = false;
        row.takeEvents.clear();
    }
    takeTracks_.clear();
    if (callbacks_.onUiStateChanged)
    {
        callbacks_.onUiStateChanged();
    }
}

LiveMidiTakeCommitResult LiveMidiInputCoordinator::commitTake(const std::int64_t recordStopSample)
{
    LiveMidiTakeCommitResult result;
    if (!takeActive_)
    {
        return result;
    }
    drainCaptureRing(); // include everything the audio thread captured up to the stop
    result.captureOverflowSeen = bus_.captureOverflowCount() != captureOverflowAtTakeStart_;

    ProjectMusicalTime musicalTime = sanitizeProjectMusicalTime(session_.getProjectMusicalTime());
    live_midi_take::TakeBuildParams params;
    params.recordStartSample = takeStartSample_;
    params.recordStopSample = juce::jmax(takeStartSample_ + 1, recordStopSample);
    params.sampleRate = takeSampleRate_ > 0.0 ? takeSampleRate_ : 48000.0;
    params.bpm = musicalTime.bpm;
    params.ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;

    const std::vector<TrackId> tracks = takeTracks_;
    for (const TrackId tid : tracks)
    {
        LiveMidiTakeCommitEntry entry;
        entry.trackId = tid;
        auto it = rows_.find(tid);
        if (it != rows_.end())
        {
            entry.build = live_midi_take::buildTakePattern(it->second.takeEvents, it->second.trackerAtTakeStart, params);
            if (entry.build.hasContent)
            {
                if (InstrumentTrackController* const ctl = instrumentRuntime_.getMidiClipControllerForTrack(tid))
                {
                    const juce::String name = "Take " + juce::Time::getCurrentTime().formatted("%H:%M:%S");
                    entry.clipId = ctl->appendRecordedTimelineMidiClip(entry.build.pattern, params.recordStartSample,
                                                                       params.recordStopSample - params.recordStartSample,
                                                                       name);
                    if (entry.clipId != 0)
                    {
                        ++result.clipsCreated;
                    }
                }
            }
            if (callbacks_.logLine)
            {
                callbacks_.logLine("[LiveMidi] take row " + juce::String((juce::int64)tid) + ": notes="
                                   + juce::String(entry.build.notesRecorded) + " closedAtStop="
                                   + juce::String(entry.build.notesClosedAtStop) + " cc="
                                   + juce::String(entry.build.controllerEventsRecorded) + " pb="
                                   + juce::String(entry.build.pitchBendEventsRecorded) + " outside="
                                   + juce::String(entry.build.eventsOutsideWindow) + " overflow="
                                   + juce::String(entry.build.overflowSeen ? "yes" : "no") + " clipId="
                                   + juce::String((juce::int64)entry.clipId));
            }
        }
        result.entries.push_back(std::move(entry));
    }
    abortTake();
    return result;
}
