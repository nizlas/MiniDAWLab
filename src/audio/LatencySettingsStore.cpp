#include "audio/LatencySettingsStore.h"

#include <juce_data_structures/juce_data_structures.h>

namespace
{
    constexpr const char* kRootTag = "LATENCY_SETTINGS";
    constexpr const char* kDeviceTag = "DEVICE";

    [[nodiscard]] juce::String backendTypeName(juce::AudioDeviceManager& manager)
    {
        if (juce::AudioIODeviceType* t = manager.getCurrentDeviceTypeObject())
        {
            return t->getTypeName();
        }
        const juce::String s = manager.getCurrentAudioDeviceType();
        if (s.isNotEmpty())
        {
            return s;
        }
        return {};
    }

    [[nodiscard]] std::int64_t xmlAttrLargeInt(const juce::XmlElement& e,
                                               const char* attribute,
                                               std::int64_t fallback)
    {
        if (!e.hasAttribute(attribute))
        {
            return fallback;
        }
        auto v = static_cast<juce::int64>(e.getStringAttribute(attribute).getLargeIntValue());
        return static_cast<std::int64_t>(v);
    }
} // namespace

LatencySettingsStore::LatencySettingsStore(juce::AudioDeviceManager& deviceManager,
                                         juce::File persistenceFile)
    : deviceManager_(deviceManager)
    , persistenceFile_(std::move(persistenceFile))
{
}

void LatencySettingsStore::loadFromFile()
{
    perDeviceStored_.clear();
    if (!persistenceFile_.existsAsFile())
    {
        return;
    }

    auto xml = juce::parseXML(persistenceFile_);
    if (xml == nullptr || !xml->hasTagName(juce::StringRef(kRootTag)))
    {
        juce::Logger::writeToLog(
            "[Latency] ignoring malformed audio-latency.xml (missing root or parse error)");
        return;
    }

    for (auto* child : xml->getChildIterator())
    {
        if (child == nullptr || !child->hasTagName(juce::StringRef(kDeviceTag)))
        {
            continue;
        }
        const juce::String tn = child->getStringAttribute("typeName");
        const juce::String dn = child->getStringAttribute("name");
        if (tn.isEmpty() && dn.isEmpty())
        {
            continue;
        }
        const juce::String key = makeDeviceKey(tn, dn);
        Offsets off;
        off.recording = xmlAttrLargeInt(*child, "recOffsetSamples", std::int64_t{ 0 });
        off.playback = xmlAttrLargeInt(*child, "playbackOffsetSamples", std::int64_t{ 0 });
        perDeviceStored_[key] = off;
    }
}

juce::String LatencySettingsStore::makeDeviceKey(const juce::String& typeName,
                                                 const juce::String& deviceName) const
{
    return typeName.trim() + "::" + deviceName.trim();
}

std::int64_t LatencySettingsStore::defaultRecordingOffsetFromReportedInput() const noexcept
{
    const int rin = reportedInputLatencySamples_;
    if (rin < 0)
    {
        return 0;
    }
    return static_cast<std::int64_t>(rin) * static_cast<std::int64_t>(-1);
}

void LatencySettingsStore::refreshFromCurrentDevice()
{
    juce::AudioIODevice* const dev = deviceManager_.getCurrentAudioDevice();
    deviceTypeName_ = backendTypeName(deviceManager_);
    deviceIoName_ = dev != nullptr ? dev->getName() : juce::String();

    currentSampleRate_
        = (dev != nullptr && dev->getCurrentSampleRate() > 0.0) ? dev->getCurrentSampleRate() : 0.0;
    currentBufferSamples_ = dev != nullptr ? dev->getCurrentBufferSizeSamples() : 0;
    reportedInputLatencySamples_
        = dev != nullptr ? dev->getInputLatencyInSamples() : -1;
    reportedOutputLatencySamples_
        = dev != nullptr ? dev->getOutputLatencyInSamples() : -1;

    currentKey_
        = (deviceTypeName_.isEmpty() && deviceIoName_.isEmpty()) ? juce::String()
                                                                  : makeDeviceKey(deviceTypeName_, deviceIoName_);

    applyStoredOrDefaultsForCurrentKey();
}

void LatencySettingsStore::applyStoredOrDefaultsForCurrentKey()
{
    if (currentKey_.isEmpty())
    {
        recordingOffsetSamples_ = 0;
        playbackOffsetSamples_ = 0;
        return;
    }

    if (auto it = perDeviceStored_.find(currentKey_); it != perDeviceStored_.end())
    {
        recordingOffsetSamples_ = it->second.recording;
        playbackOffsetSamples_ = it->second.playback;
        return;
    }

    recordingOffsetSamples_ = defaultRecordingOffsetFromReportedInput();
    playbackOffsetSamples_ = 0;
}

void LatencySettingsStore::setCurrentRecordingOffsetSamples(const std::int64_t v)
{
    recordingOffsetSamples_ = v;
    if (currentKey_.isEmpty())
    {
        return;
    }
    Offsets off;
    off.recording = v;
    off.playback = playbackOffsetSamples_;
    perDeviceStored_[currentKey_] = off;
    save();
}

void LatencySettingsStore::setCurrentPlaybackOffsetSamples(const std::int64_t v)
{
    playbackOffsetSamples_ = v;
    if (currentKey_.isEmpty())
    {
        return;
    }
    Offsets off;
    off.recording = recordingOffsetSamples_;
    off.playback = v;
    perDeviceStored_[currentKey_] = off;
    save();
}

void LatencySettingsStore::resetRecordingToMinusReportedInput()
{
    setCurrentRecordingOffsetSamples(defaultRecordingOffsetFromReportedInput());
}

void LatencySettingsStore::resetPlaybackToZero()
{
    setCurrentPlaybackOffsetSamples(0);
}

void LatencySettingsStore::setPlaybackToReportedOutputLatency()
{
    const int lat = reportedOutputLatencySamples_;
    const std::int64_t plus = lat < 0 ? std::int64_t{ 0 } : static_cast<std::int64_t>(lat);
    setCurrentPlaybackOffsetSamples(plus);
}

void LatencySettingsStore::persistAllKnownDevicesToFile()
{
    auto root = std::make_unique<juce::XmlElement>(kRootTag);
    root->setAttribute("version", "1");

    // Stable order helps diffing configs.
    for (const auto& kv : perDeviceStored_)
    {
        auto* elem = root->createNewChildElement(kDeviceTag);

        auto splitDoubleColon = [&](const juce::String& k) -> std::pair<juce::String, juce::String>
        {
            const int pos = k.indexOfIgnoreCase("::");
            if (pos < 0)
            {
                return { {}, k };
            }
            return { k.substring(0, pos), k.substring(pos + 2) };
        };

        const auto [tn, dn] = splitDoubleColon(kv.first);

        elem->setAttribute("typeName", tn);
        elem->setAttribute("name", dn);
        elem->setAttribute("recOffsetSamples", juce::String(kv.second.recording));
        elem->setAttribute("playbackOffsetSamples", juce::String(kv.second.playback));
    }

    const juce::File parent = persistenceFile_.getParentDirectory();
    if (!parent.isDirectory() && !parent.createDirectory())
    {
        juce::Logger::writeToLog(
            juce::String{"[Latency] could not create settings directory: "} + parent.getFullPathName());
        return;
    }
    if (!persistenceFile_.replaceWithText(root->toString(), false, true))
    {
        juce::Logger::writeToLog(
            juce::String{"[Latency] could not write "} + persistenceFile_.getFullPathName());
        return;
    }
}

void LatencySettingsStore::save()
{
    persistAllKnownDevicesToFile();
}
