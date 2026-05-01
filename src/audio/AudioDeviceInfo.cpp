#include "audio/AudioDeviceInfo.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>

namespace
{
    [[nodiscard]] juce::String backendNameFromManager(juce::AudioDeviceManager& manager)
    {
        if (juce::AudioIODeviceType* t = manager.getCurrentDeviceTypeObject())
        {
            return t->getTypeName();
        }
        {
            const juce::String s = manager.getCurrentAudioDeviceType();
            if (s.isNotEmpty())
            {
                return s;
            }
        }
        return "unknown backend";
    }

    [[nodiscard]] juce::String
        formatActiveChannelsWithNames(const juce::BigInteger& active, const juce::StringArray& names)
    {
        juce::String out;
        const int hi = juce::jmax(0, active.getHighestBit());
        for (int i = 0; i <= hi; ++i)
        {
            if (!active[i])
            {
                continue;
            }
            if (out.isNotEmpty())
            {
                out << ", ";
            }
            if (i < names.size() && names[i].isNotEmpty())
            {
                out << names[i];
            }
            else
            {
                out << "ch" << (i + 1);
            }
        }
        return out.isEmpty() ? juce::String("none") : out;
    }

    [[nodiscard]] juce::String
        latencyString(int samples, double sampleRate, const char* ifInvalid)
    {
        if (samples < 0)
        {
            return ifInvalid;
        }
        if (sampleRate > 0.0)
        {
            const double ms = 1000.0 * (double)samples / sampleRate;
            return juce::String(samples) + " (~" + juce::String(ms, 2) + " ms)";
        }
        return juce::String(samples) + " samples";
    }
} // namespace

juce::String mini_daw::describeActiveAudioDeviceOneLine(juce::AudioDeviceManager& manager)
{
    juce::String line;
    line << "[JUCE active] ";
    juce::AudioIODevice* const dev = manager.getCurrentAudioDevice();
    if (dev == nullptr)
    {
        line << "(no active device) · backend: " << backendNameFromManager(manager);
        return line;
    }
    const juce::String back = backendNameFromManager(manager);
    const juce::String name = dev->getName();
    const double sr = dev->getCurrentSampleRate();
    const int bl = dev->getCurrentBufferSizeSamples();
    const juce::BigInteger inAct = dev->getActiveInputChannels();
    const juce::BigInteger outAct = dev->getActiveOutputChannels();
    const juce::String inNames = formatActiveChannelsWithNames(inAct, dev->getInputChannelNames());
    const juce::String outNames
        = formatActiveChannelsWithNames(outAct, dev->getOutputChannelNames());
    const int inLat = dev->getInputLatencyInSamples();
    const int outLat = dev->getOutputLatencyInSamples();

    line << back << " | \"" << name << "\" | " << juce::String(juce::roundToInt(sr)) << " Hz | buf "
         << juce::String(bl);
    line << " | in: " << inNames;
    if (inLat >= 0)
    {
        line << " (lat " << latencyString(inLat, sr, "?") << ")";
    }
    line << " | out: " << outNames;
    if (outLat >= 0)
    {
        line << " (lat " << latencyString(outLat, sr, "?") << ")";
    }
    return line;
}

juce::String mini_daw::describeActiveAudioDeviceMultiLine(juce::AudioDeviceManager& manager)
{
    juce::String s;
    s << "JUCE-reported device (this is not necessarily the same as the current Windows default).\n\n";

    juce::AudioIODevice* const dev = manager.getCurrentAudioDevice();
    s << "Active backend: " << backendNameFromManager(manager) << "\n";

    if (dev == nullptr)
    {
        s << "Active device: (no active device)\n";
    }
    else
    {
        const double sr = dev->getCurrentSampleRate();
        const int bl = dev->getCurrentBufferSizeSamples();
        s << "Active device: \"" << dev->getName() << "\"\n";
        s << "Current sample rate: " << (sr > 0.0 ? juce::String(sr, 2) : juce::String("?"))
          << " Hz\n";
        s << "Current buffer: " << juce::String(bl) << " samples\n";

        const juce::BigInteger inAct = dev->getActiveInputChannels();
        const juce::BigInteger outAct = dev->getActiveOutputChannels();

        s << "Active input channel mask: " << inAct.toString(2) << "\n";
        s << "Active output channel mask: " << outAct.toString(2) << "\n";
        s << "Active input channels: "
          << formatActiveChannelsWithNames(inAct, dev->getInputChannelNames()) << "\n";
        s << "Active output channels: "
          << formatActiveChannelsWithNames(outAct, dev->getOutputChannelNames()) << "\n";

        const int inLat = dev->getInputLatencyInSamples();
        const int outLat = dev->getOutputLatencyInSamples();
        s << "Input latency: " << latencyString(inLat, sr, "n/a")
          << (inLat < 0 ? " (n/a)\n" : "\n");
        s << "Output latency: " << latencyString(outLat, sr, "n/a")
          << (outLat < 0 ? " (n/a)\n" : "\n");
    }

    s << "\nAll registered device types (read-only; scan for names):\n";
    const juce::OwnedArray<juce::AudioIODeviceType>& types = manager.getAvailableDeviceTypes();
    for (int ti = 0; ti < types.size(); ++ti)
    {
        juce::AudioIODeviceType* t = types[ti];
        if (t == nullptr)
        {
            continue;
        }
        t->scanForDevices();
        s << "— " << t->getTypeName() << " —\n";
        const juce::StringArray in = t->getDeviceNames(true);
        const juce::StringArray out = t->getDeviceNames(false);
        s << "  Input device names (" << in.size() << "):\n";
        if (in.isEmpty())
        {
            s << "    (none)\n";
        }
        else
        {
            for (int i = 0; i < in.size(); ++i)
            {
                s << "    [" << (i + 1) << "] " << in[i] << "\n";
            }
        }
        s << "  Output device names (" << out.size() << "):\n";
        if (out.isEmpty())
        {
            s << "    (none)\n";
        }
        else
        {
            for (int i = 0; i < out.size(); ++i)
            {
                s << "    [" << (i + 1) << "] " << out[i] << "\n";
            }
        }
    }
    if (types.isEmpty())
    {
        s << "(no device types registered — unexpected)\n";
    }
    return s;
}

juce::File mini_daw::getAudioSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("audio-device.xml");
}

juce::File mini_daw::getLatencySettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("audio-latency.xml");
}

void mini_daw::trySaveAudioDeviceState(juce::AudioDeviceManager& manager, const juce::File& file)
{
    std::unique_ptr<juce::XmlElement> state = manager.createStateXml();
    if (state == nullptr)
    {
        return;
    }
    const juce::File parent = file.getParentDirectory();
    if (!parent.isDirectory() && !parent.createDirectory())
    {
        juce::Logger::writeToLog(
            juce::String{"[Audio] could not create settings directory: "} + parent.getFullPathName());
        return;
    }
    if (!file.replaceWithText(state->toString(), false, true))
    {
        juce::Logger::writeToLog(
            juce::String{"[Audio] could not write device settings file: "} + file.getFullPathName());
        return;
    }
    juce::Logger::writeToLog(
        juce::String{"[Audio] saved device state to "} + file.getFullPathName());
}

std::unique_ptr<juce::XmlElement> mini_daw::loadAudioSettingsXmlIfAny(const juce::File& file)
{
    if (!file.existsAsFile())
    {
        return nullptr;
    }
    return juce::parseXML(file);
}
