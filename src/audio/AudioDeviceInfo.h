#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace juce
{
    class AudioDeviceManager;
}

namespace mini_daw
{
    /// Read-only, message-thread. Describes the device JUCE is actually using (not Windows defaults).
    /// Non-const ref: JUCE queries may need a non-const `AudioDeviceManager`; we do not change setup.
    [[nodiscard]] juce::String describeActiveAudioDeviceOneLine(juce::AudioDeviceManager& manager);
    /// Includes enumeration of all registered backends and their scanned device names.
    [[nodiscard]] juce::String describeActiveAudioDeviceMultiLine(juce::AudioDeviceManager& manager);

    /// %APPDATA%\\MiniDAWLab\\audio-device.xml
    [[nodiscard]] juce::File getAudioSettingsFile();
    /// %APPDATA%\\MiniDAWLab\\audio-latency.xml (per-device latency / timing overrides)
    [[nodiscard]] juce::File getLatencySettingsFile();
    /// `createStateXml()` save; no-op if JUCE returns null. Write errors are logged only (non-fatal).
    void trySaveAudioDeviceState(juce::AudioDeviceManager& manager, const juce::File& file);
    [[nodiscard]] std::unique_ptr<juce::XmlElement> loadAudioSettingsXmlIfAny(const juce::File& file);
} // namespace mini_daw
