// =============================================================================
// AudioFileLoader.cpp  —  JUCE decoders: open file → read into float buffer → wrap AudioClip
// =============================================================================
//
// READ-ORDER FOR THIS FILE (plain steps; matches the function body in order)
//   1. Reset output and verify the path exists.
//   2. Create a juce::AudioFormatManager, register default codecs (WAV, AIFF, etc.) — that is
//      how JUCE picks a juce::AudioFormatReader for the file.
//   3. openReader: if the format is unknown, we fail early.
//   4. Enforce same sample rate as the current audio device: Phase 1 has no resampler; we
//      compare with juce::approximatelyEqual and bail with a clear error if the file and
//      device differ.
//   5. Sanity: channels and length; cap size so we can hold numSamples in an int for JUCE
//      buffer APIs in Phase 1.
//   6. Allocate a juce::AudioBuffer<float>, read() interleaved file data into it, wrap in
//      AudioClip with source rate and a description string.
//
// JUCE TYPES
//   • AudioFormatManager / registerBasicFormats / createReaderFor — file sniffing and decode.
//   • AudioFormatReader::read — fills channel pointers; we pass buffer.getArrayOfWritePointers().
//   • approximatelyEqual — float sample rate compare (not exact ==).
// =============================================================================

#include "io/AudioFileLoader.h"

#include "domain/AudioClip.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <limits>

juce::Result AudioFileLoader::loadFromFile(const juce::File& file,
                                           double requiredDeviceSampleRate,
                                           std::unique_ptr<AudioClip>& outClip)
{
    // Start clean so a failed run does not leave a stale `outClip` from a previous attempt
    // (Session treats success/failure on this Result only).
    outClip.reset();

    if (!file.existsAsFile())
        return juce::Result::fail("File does not exist.");

    juce::AudioFormatManager formatManager;
    // Register WAV/AIFF/etc. once for this call so `createReaderFor` can sniff the file type.
    formatManager.registerBasicFormats();

    const std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
    {
        // JUCE: null means the path was unreadable, the extension was not recognized, or the
        // data was not a decodable format — we do not distinguish further in Phase 1.
        return juce::Result::fail("Could not read audio file (unsupported format or I/O error).");
    }

    if (!juce::approximatelyEqual(reader->sampleRate, requiredDeviceSampleRate))
    {
        // Phase 1 product rule: playback and this decode path assume one shared sample rate
        // (the device’s). Without a resampler, running at 44.1 and decoding a 48k file would
        // play at the wrong speed — we fail fast with a user-visible message instead.
        return juce::Result::fail(
            "Audio file sample rate ("
            + juce::String(reader->sampleRate, 2)
            + " Hz) does not match the audio device ("
            + juce::String(requiredDeviceSampleRate, 2)
            + " Hz). Phase 1 does not resample; use a matching file or change the device rate.");
    }

    const int numChannels = static_cast<int>(reader->numChannels);
    if (numChannels <= 0)
        return juce::Result::fail("Audio file has no channels.");

    const juce::int64 lengthInSamples = reader->lengthInSamples;
    if (lengthInSamples <= 0)
        return juce::Result::fail("Audio file contains no samples.");

    if (lengthInSamples > static_cast<juce::int64>(std::numeric_limits<int>::max()))
    {
        // juce::AudioBuffer uses int lengths in Phase 1; oversized files are out of scope rather
        // than truncated silently.
        return juce::Result::fail("Audio file is too large for Phase 1 in-memory decode.");
    }

    const int numSamples = static_cast<int>(lengthInSamples);
    juce::AudioBuffer<float> buffer(numChannels, numSamples);

    if (!reader->read(buffer.getArrayOfWritePointers(), numChannels, 0, numSamples))
    {
        // Decode step failed after the header looked valid — report as a read error, not a silent clip.
        return juce::Result::fail("Failed to read sample data from file.");
    }

    // Success: the buffer becomes an immutable AudioClip; filename is kept for on-screen errors.
    outClip = std::make_unique<AudioClip>(
        std::move(buffer),
        reader->sampleRate,
        file.getFullPathName());

    return juce::Result::ok();
}
