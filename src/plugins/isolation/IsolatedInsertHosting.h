#pragma once

// =============================================================================
// IsolatedInsertHosting — DAL's side of the shared AmpliTube 4 helper (plan B–D)
// =============================================================================
//
// `IsolatedInsertInstance` stands in for the plug-in in `LiveInsertSlot`. Everything above it
// (insert host, engine, render pool, read-ahead, rest, Off, undo, persistence) keeps handling a
// `juce::AudioPluginInstance`. Saved projects are unchanged: the descriptor is the real plug-in's
// and the state bytes are the real plug-in's.
//
// `IsolatedHostConnection` owns the one helper process per DAL process: start on demand, the
// control channel, death detection, recovery after a crash, and an exit that never runs the
// plug-in's destructor (project close, app exit, last instance gone).
//
// Threads:
//   * Message thread: create, prepare, state, preset, editor, destroy, recovery.
//   * Any audio lane (callback, render pool, read-ahead, offline): `processBlock` only. No
//     allocation, no lock, no pipe; the wait is bounded by the context deadline.
//   * Pipe thread and one death-watch thread per helper: replies and notifications only.
//
// State capture contract (plan C.2):
//   * `getStateInformation` asks the helper for fresh state on its message thread while the
//     instance keeps processing on its own thread. That is the VST3 rule in-process DAL already
//     relies on (IComponent::getState on the UI thread beside process()); no audio pause.
//   * Before an operation that can run a destructor in the helper (Off, delete), every live
//     isolated instance is captured with its processing paused and drained (`captureAllLive`),
//     because a crash takes all of them. A failed capture cancels that operation.
//   * Changes reported by the plug-in (parameter, editor) mark the instance "changed since
//     capture". They are captured after they settle while stopped, and deferred to Stop while
//     playing. Save captures at once. After a crash, changes since the last capture are lost and
//     the recovery message names the instances that had them.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "plugins/isolation/IsolatedProtocol.h"

namespace mini_daw::isolation
{
class IsolatedHostConnection;
using Handle = void*; // Win32 HANDLE, without windows.h in this header

/// Which plug-ins run in the shared helper. Only the verified AmpliTube 4 VST3 identity, plus an
/// identifier explicitly registered for tests or the diagnostic flag.
struct IsolatedPluginPolicy
{
    [[nodiscard]] static bool shouldIsolate(const juce::PluginDescription& d);
    /// Tests / `--isolate-plugin-for-test <identifier>` only. Empty clears.
    static void setAdditionalIdentifierForTests(const juce::String& createIdentifierString);
    /// Global switch (default on). Off = every plug-in in DAL's process, as before.
    static void setEnabled(bool enabled);
    [[nodiscard]] static bool isEnabled();
};

class IsolatedInsertInstance final : public juce::AudioPluginInstance
{
public:
    enum class Link : int
    {
        Detached = 0, // no helper instance (not created yet, or helper torn down on purpose)
        Ready = 1,
        Lost = 2,     // helper died or hung; silent until restored
    };

    struct Counters
    {
        std::atomic<std::uint64_t> requests { 0 };
        std::atomic<std::uint64_t> misses { 0 };      // deadline passed: stage output silenced
        std::atomic<std::uint64_t> silentLost { 0 };  // blocks while lost / paused / outstanding
        std::atomic<std::uint64_t> lateDiscarded { 0 };
        std::atomic<std::int64_t> waitTicksTotal { 0 };
        std::atomic<std::int64_t> waitTicksMax { 0 };
    };

    IsolatedInsertInstance(IsolatedHostConnection& connection, const juce::PluginDescription& desc);
    ~IsolatedInsertInstance() override;

    /// [Message thread] Create the helper-side instance, applying `lastCapturedState` if any.
    [[nodiscard]] bool attach(double sampleRate, int blockSize, juce::String& error);

    // juce::AudioProcessor / AudioPluginInstance
    const juce::String getName() const override { return description_.name; }
    void fillInPluginDescription(juce::PluginDescription& d) const override { d = description_; }
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioPluginInstance::processBlock;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    double getTailLengthSeconds() const override { return tailSeconds_.load(std::memory_order_relaxed); }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    void getExtensions(juce::ExtensionsVisitor& visitor) const override;

    // Isolation-specific (message thread unless noted)
    [[nodiscard]] Link link() const noexcept { return (Link)link_.load(std::memory_order_acquire); }
    [[nodiscard]] bool lastCaptureOk() const noexcept { return lastCaptureOk_; }
    [[nodiscard]] bool changedSinceCapture() const noexcept { return changedSinceCapture_; }
    [[nodiscard]] const juce::MemoryBlock& lastCapturedState() const noexcept { return lastCapturedState_; }
    /// Fresh capture into `lastCapturedState`; false when the helper did not answer.
    bool captureNow();
    [[nodiscard]] bool openRemoteEditor(const juce::String& title);
    void closeRemoteEditor();
    [[nodiscard]] bool remoteEditorOpen() const noexcept { return editorOpen_; }
    /// Called on the message thread when the user closed the helper's editor window.
    std::function<void()> onRemoteEditorClosed;
    [[nodiscard]] std::int32_t helperInstanceId() const noexcept { return instanceId_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] juce::MemoryBlock getPresetBytes(bool& ok);
    /// [Message thread] The caller just ran `captureAllLive`; the destructor need not capture again.
    void skipDestroyCaptureOnce() noexcept { skipDestroyCapture_ = true; }
    /// [Message thread] The row went Off: its state is captured and it is no longer processed.
    /// Recovery never restores it, and it does not keep the helper alive on its own.
    void markRetiredForOff() noexcept { retiredForOff_ = true; }
    [[nodiscard]] bool retiredForOff() const noexcept { return retiredForOff_; }
    [[nodiscard]] bool setPresetBytes(const juce::MemoryBlock& preset);

private:
    friend class IsolatedHostConnection;

    struct Section
    {
        Handle mapping = nullptr;
        Handle requestEvent = nullptr;
        Handle doneEvent = nullptr;
        Handle helperProcess = nullptr; // owned by the connection
        SharedBlockHeader* header = nullptr;
        float* channel[kChannels] {};
        int maxBlock = 0;
    };

    void markLost() noexcept;            // any thread; audio stops touching the section
    void waitUntilNotInFlight() noexcept; // message thread, bounded by the lanes' own deadlines
    void detachSection();                 // message thread, after markLost / waitUntilNotInFlight
    void noteChanged();                   // message thread: plug-in reported a change
    bool processChunk(float* const* channels, int numChannels, int offset, int n) noexcept;
    void writeTransport(SharedBlockHeader& h) noexcept;

    IsolatedHostConnection& connection_;
    juce::PluginDescription description_;
    std::int32_t instanceId_ = 0;
    Section section_;
    std::atomic<int> link_ { (int)Link::Detached };
    std::atomic<bool> inFlight_ { false };
    std::atomic<std::uint32_t> sectionGeneration_ { 0 };
    // Audio-lane private (one lane at a time per row, the engine's invariant):
    std::uint32_t outstandingSeq_ = 0;
    bool outstanding_ = false;
    std::uint32_t seenGeneration_ = 0;
    std::atomic<double> tailSeconds_ { 0.0 };
    double sampleRate_ = 48000.0;
    int blockSize_ = 512;
    bool nonRealtime_ = false;
    juce::MemoryBlock lastCapturedState_;
    bool lastCaptureOk_ = true;
    bool changedSinceCapture_ = false;
    double changedAtMs_ = 0.0;
    bool editorOpen_ = false;
    bool skipDestroyCapture_ = false;
    bool retiredForOff_ = false;
    // Audio-side hang watch (message thread): progress seen while a request was outstanding.
    std::uint32_t stallProgress_ = 0;
    double stallSinceMs_ = 0.0;
    Counters counters_;
};

class IsolatedHostConnection final : private juce::ChildProcessCoordinator, private juce::Timer
{
public:
    enum class Phase
    {
        None,          // no helper
        Running,
        WaitingForStop, // helper died while the transport ran; restore after Stop
        Restoring,
        Failed,        // restore failed; Retry needed
    };

    struct UiHooks
    {
        std::function<bool()> transportBusy;
        std::function<void(const juce::String&)> showProgress; // also used for one-line notices
        std::function<void()> hideProgress;
        /// Failure with a Retry action (restore failed, or helper not responding).
        std::function<void(const juce::String& message, std::function<void()> retry)> showFailure;
        std::function<void()> blockingChanged;
    };

    static IsolatedHostConnection& get();

    void setUiHooks(UiHooks hooks);
    /// Path of the executable started as helper (default: this executable). Tests point it at DAL.
    void setHelperExecutable(const juce::File& exe);

    [[nodiscard]] Phase phase() const noexcept { return phase_; }
    /// Play, Record, Power, insert edits, project open/close and undo/redo are blocked while true.
    [[nodiscard]] bool isBlockingCommands() const noexcept { return phase_ == Phase::Restoring; }
    [[nodiscard]] bool helperRunning() const noexcept { return helperAlive_.load(std::memory_order_acquire); }
    [[nodiscard]] int liveInstanceCount() const noexcept;
    /// Live instances of rows that are On (not retired for Off).
    [[nodiscard]] int liveOnInstanceCount() const noexcept;
    [[nodiscard]] std::uint32_t helperPid() const noexcept { return helperPid_; }
    [[nodiscard]] int helperStartsForDiagnostics() const noexcept { return helperStarts_; }
    [[nodiscard]] juce::String lastRecoveryReport() const { return lastRecoveryReport_; }
    /// True after the helper stopped answering (control) or stopped processing (audio). Cleared
    /// only by `restartUnresponsiveHelper` (the user's Restart) or a new helper.
    [[nodiscard]] bool helperUnresponsive() const noexcept { return helperUnresponsive_; }
    /// Tests: how often the user was told about an unresponsive helper.
    [[nodiscard]] int unresponsiveReportsForDiagnostics() const noexcept { return unresponsiveReports_; }
    /// Tests: shorten the audio-side hang limit (default `kAudioHangMs`).
    void setAudioHangLimitMsForTests(int ms) noexcept { audioHangLimitMs_ = ms; }

    /// [Message thread] Factory: a stand-in for `desc`, attached to the helper (started if needed).
    [[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> createInstance(const juce::PluginDescription& desc,
                                                                            double sampleRate,
                                                                            int blockSize,
                                                                            juce::String& error);

    /// [Message thread] Pause and drain every isolated stage, then capture each live instance's
    /// state. False (with the instance name) when any capture failed: nothing destructive may run.
    /// Processing resumes on return either way.
    [[nodiscard]] bool captureAllLive(juce::String& failedName);

    /// [Message thread] End the helper without running plug-in destructors and detach every
    /// stand-in (project close, app exit). Their last captured state stays readable.
    void terminateForTeardown(const juce::String& why);

    /// [Message thread] Capture every changed instance now if the transport is stopped.
    void captureChangedWhileStopped();

    /// [Message thread] Retry after a failed restore (the banner's button).
    void retryRestore();
    /// [Message thread] End an unresponsive helper and restore from the last captured state.
    void restartUnresponsiveHelper();

    /// Tests: send a fault to the helper (instance id 0 = helper-wide faults).
    bool sendTestFault(TestFault fault, int argument, std::int32_t instanceId);
    /// Tests: the next restore's helper crashes on its first create (crash during restore).
    void setTestCrashOnNextRestoreCreate(bool crash) noexcept { testCrashOnRestoreCreate_ = crash; }
    /// Tests: pump until `predicate` or `timeoutMs`; returns predicate().
    bool runMessageLoopUntil(const std::function<bool()>& predicate, int timeoutMs);

private:
    friend class IsolatedInsertInstance;

    IsolatedHostConnection();

    // juce::ChildProcessCoordinator (pipe thread)
    void handleMessageFromWorker(const juce::MemoryBlock& m) override;
    void handleConnectionLost() override;
    void timerCallback() override;

    struct Pending
    {
        juce::WaitableEvent done;
        juce::MemoryBlock reply;
        bool ok = false;
    };

    bool ensureRunning(juce::String& error);
    bool launchHelper(juce::String& error);
    /// Synchronous control request. False on timeout, helper death or a failure reply.
    bool call(ControlKind kind,
              const std::function<void(class ControlWriter&)>& payload,
              int timeoutMs,
              const std::function<void(class ControlReader&)>& onReply,
              const juce::String& what);
    void failAllPending();
    void onHelperDied(int generation, bool unresponsive);
    void beginRestore();
    void restoreNextStep();
    void finishRestore(bool ok, const juce::String& why);
    void setPhase(Phase p);
    void registerInstance(IsolatedInsertInstance* inst);
    void unregisterInstance(IsolatedInsertInstance* inst);
    IsolatedInsertInstance* findById(std::int32_t id) const;
    /// [Message thread] Called by a stand-in's destructor: destroy its helper instance, or end the
    /// helper when it was the last one.
    void retireInstance(IsolatedInsertInstance& inst);
    void endHelperProcess(bool expected);
    /// Mark the helper unresponsive and tell the user once per helper, with Restart.
    void reportUnresponsive(const juce::String& what);
    /// [Message thread, timer] A request outstanding with no helper progress for too long.
    void checkAudioHang();

    juce::File helperExe_;
    UiHooks hooks_;
    Phase phase_ = Phase::None;
    std::atomic<bool> helperAlive_ { false };
    std::atomic<bool> pausedAll_ { false };
    std::atomic<int> generation_ { 0 };
    int expectedDeathGeneration_ = -1;
    Handle helperProcess_ = nullptr;
    std::vector<Handle> retiredProcessHandles_; // kept open: a lane may still hold one in a wait
    Handle job_ = nullptr;
    std::uint32_t helperPid_ = 0;
    juce::WaitableEvent helloEvent_;
    std::atomic<std::uint32_t> helloPid_ { 0 };
    std::unique_ptr<std::thread> deathWatch_;
    Handle deathWatchStop_ = nullptr;
    std::mutex pendingLock_;
    std::unordered_map<std::int32_t, std::shared_ptr<Pending>> pending_;
    std::atomic<std::int32_t> nextRequestId_ { 1 };
    std::int32_t nextInstanceId_ = 1;
    std::vector<IsolatedInsertInstance*> instances_; // registration order = slot creation order
    int helperStarts_ = 0;
    // Recovery
    std::vector<IsolatedInsertInstance*> restoreQueue_;
    int restoreTotal_ = 0;
    int restoreDone_ = 0;
    juce::StringArray lostChangeNames_;
    juce::String lastRecoveryReport_;
    bool helperUnresponsive_ = false;
    bool testCrashOnRestoreCreate_ = false;
    int unresponsiveReports_ = 0;
    int audioHangLimitMs_ = 2000;
};
} // namespace mini_daw::isolation
