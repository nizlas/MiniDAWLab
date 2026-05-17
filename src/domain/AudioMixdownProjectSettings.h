#pragma once

// Session-held Audio Mixdown UI/export preferences (saved under optional root `audioMixdown` in .dalproj).

#include <juce_core/juce_core.h>

#include <cstdint>

struct AudioMixdownProjectSettings final
{
    juce::String fileNameWithoutExtension;
    /// Stored JSON form: project-relative path with forward slashes (e.g. "Mixdown"), or an absolute
    /// OS path when the folder lies outside the project directory.
    juce::String outputDirectorySpec;

    enum class FileType : std::uint8_t
    {
        Wave,
        MpegLayer3,
    };

    FileType fileType = FileType::Wave;
    /// 16 / 24 PCM or 32 (IEEE float). **0** = not yet established → dialog applies float probe on first open.
    int wavBitDepth = 0;
    int mp3BitRateKbps = 320;
};

[[nodiscard]] AudioMixdownProjectSettings defaultAudioMixdownProjectSettings() noexcept;

[[nodiscard]] int clampMp3BitRateKbps(int kbps) noexcept;

/// Encode an absolute mixdown folder for JSON (`projectFolder` = parent of `.dalproj`).
[[nodiscard]] juce::String encodeMixdownOutputDirectorySpec(const juce::File& projectFolder,
                                                            const juce::File& absoluteMixdownFolder);

/// Resolve stored spec to an absolute directory for UI path editors / export.
[[nodiscard]] juce::File decodeMixdownOutputDirectorySpec(const juce::File& projectFolder,
                                                          const juce::String& spec);
