#pragma once

// =============================================================================
// ExperimentalInstrumentHost — single global VST3 instrument slot (I1 feasibility)
// =============================================================================
//
// ROLE
//   Message-thread owner of one optional juce::AudioPluginInstance intended for instruments
//   (e.g. Groove Agent SE): MIDI in from the message thread via a small locked MidiBuffer
//   queue (I1), stereo audio out mixed by PlaybackEngine after the normal clip/insert sum.
//
// NOT in Session / ProjectFile; no undo; experimental UX only.
//
// THREADING
//   load / unload / editor / prepareForDevice: [Message thread].
//   audioThread_processBlockAndAddToOutputs: [Audio thread] — reads atomic plugin holder;
//   drains UI MIDI into a block buffer under a short CriticalSection, then processBlock.
//   (We avoid juce::MidiMessageCollector here: VST3 host headers can break `juce::` lookup on MSVC.)
//
// See docs/PHASE_PLAN.md — this is a narrow vertical slice before MIDI tracks / instruments.
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

class ExperimentalInstrumentHost
{
public:
    static constexpr int kStereoChannels = 2;

    ExperimentalInstrumentHost();
    ~ExperimentalInstrumentHost();

    ExperimentalInstrumentHost(const ExperimentalInstrumentHost&) = delete;
    ExperimentalInstrumentHost& operator=(const ExperimentalInstrumentHost&) = delete;

    /// [Message thread] Loads from a .vst3 bundle path; replaces any existing instrument.
    /// On failure the previous slot (if any) is cleared.
    [[nodiscard]] juce::Result loadInstrumentFromVst3File(const juce::File& vst3File);

    /// [Message thread] Loads from a description obtained out-of-process (e.g. raw OOP scan or XML cache).
    /// Skips in-process findAllTypesForFile. If `desc.fileOrIdentifier` is empty, uses `originalPath`.
    /// `sourceTag` is copied into experimental-instrument.log (e.g. "oop-description", "cached-oop-description").
    [[nodiscard]] juce::Result loadInstrumentFromDescription(const juce::PluginDescription& desc,
                                                             const juce::File& originalPath,
                                                             const char* sourceTag = "oop-description");

    /// [Message thread] Runs `findAllTypesForFile` only (no `createPluginInstance`, no bus prep).
    /// Writes flushed boundary lines to `%APPDATA%\\MiniDAWLab\\experimental-vst3-scan-diagnostic.log`.
    /// For comparing plugins that crash during scan vs effects/instruments that return.
    [[nodiscard]] juce::Result diagnosticScanVst3FileOnly(const juce::File& vst3File);

    /// [Message thread] Closes editor, releases instance. Brief wait lets the audio callback
    /// finish any in-flight block (feasibility slice — not a production RT unload protocol).
    void unloadInstrument();

    [[nodiscard]] bool hasInstrument() const noexcept;
    [[nodiscard]] juce::String getInstrumentNameForUi() const;

    void openNativeEditor();
    void editorWindowClosing();

    /// I1: one drum-oriented test note (MIDI 36). Note-off queued from the message thread after a
    /// short delay — never touches plugin state off the audio thread except via MidiMessageCollector.
    void triggerTestKick();

    void prepareForDevice(double sampleRate, int blockSize);
    void releaseResources();

    /// [Audio thread] Clears stereo scratch, drains MIDI collector, runs processBlock if loaded
    /// and layout ok, adds first stereo output bus to device buffers (frame 0..numSamples-1).
    void audioThread_processBlockAndAddToOutputs(float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples) noexcept;

private:
    struct InstrumentOwner
    {
        std::unique_ptr<juce::AudioPluginInstance> inst;
        bool layoutOk = false;
    };

    [[nodiscard]] bool tryPrepareInstrumentLayout(juce::AudioPluginInstance& inst,
                                                  double sampleRate,
                                                  int blockSize);

    void closeNativeEditor();

    struct MidiIoState;
    std::unique_ptr<MidiIoState> midiIo_;

    juce::AudioPluginFormatManager formatManager_;

    juce::AudioBuffer<float> scratch_;
    std::vector<float*> scratchPtrs_;

    std::atomic<std::shared_ptr<InstrumentOwner>> activeOwner_;

    std::unique_ptr<juce::DocumentWindow> editorWindow_;

    /// Schedules MIDI 36 note-off on the message thread (see .cpp).
    struct TestKickNoteOffTimer;
    friend struct TestKickNoteOffTimer;
    std::unique_ptr<TestKickNoteOffTimer> testKickNoteOffTimer_;

    /// [Message thread] Forwards into MidiMessageCollector (implementation in .cpp).
    void queueMidiFromMessageThread(const ::juce::MidiMessage& message);

    double sampleRate_ = 0.0;
    int blockSize_ = 0;
};
