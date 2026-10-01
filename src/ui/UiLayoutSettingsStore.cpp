#include "ui/UiLayoutSettingsStore.h"

namespace
{
    constexpr const char* kRootTag = "UI_LAYOUT";
    constexpr const char* kTrackHeaderColumnTag = "TRACK_HEADER_COLUMN";
    constexpr const char* kWidthAttribute = "widthPx";
} // namespace

UiLayoutSettingsStore::UiLayoutSettingsStore(juce::File persistenceFile)
    : persistenceFile_(std::move(persistenceFile))
{
}

juce::File UiLayoutSettingsStore::defaultFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("ui-layout.xml");
}

void UiLayoutSettingsStore::loadFromFile()
{
    trackHeaderColumnWidthPx_.reset();
    if (!persistenceFile_.existsAsFile())
    {
        return;
    }
    const auto xml = juce::parseXML(persistenceFile_);
    if (xml == nullptr || !xml->hasTagName(juce::StringRef(kRootTag)))
    {
        juce::Logger::writeToLog("[UiLayout] ignoring malformed " + persistenceFile_.getFileName()
                                 + " (missing root or parse error); defaults apply");
        return;
    }
    if (const juce::XmlElement* col = xml->getChildByName(juce::StringRef(kTrackHeaderColumnTag)))
    {
        const juce::String raw = col->getStringAttribute(kWidthAttribute).trim();
        // Strict: digits only (no "144px", no floats) — anything else is treated as absent.
        if (raw.isNotEmpty() && raw.containsOnly("0123456789"))
        {
            const int v = raw.getIntValue();
            if (v > 0)
            {
                trackHeaderColumnWidthPx_ = v;
            }
        }
        else if (raw.isNotEmpty())
        {
            juce::Logger::writeToLog("[UiLayout] ignoring invalid track header column width \"" + raw + "\"");
        }
    }
}

void UiLayoutSettingsStore::setTrackHeaderColumnWidthPx(const int widthPx) noexcept
{
    if (widthPx > 0)
    {
        trackHeaderColumnWidthPx_ = widthPx;
    }
}

void UiLayoutSettingsStore::save()
{
    juce::XmlElement root(kRootTag);
    root.setAttribute("version", "1");
    if (trackHeaderColumnWidthPx_.has_value())
    {
        auto* col = root.createNewChildElement(kTrackHeaderColumnTag);
        col->setAttribute(kWidthAttribute, juce::String(*trackHeaderColumnWidthPx_));
    }
    const juce::File parent = persistenceFile_.getParentDirectory();
    if (!parent.isDirectory() && !parent.createDirectory())
    {
        juce::Logger::writeToLog("[UiLayout] could not create settings directory: " + parent.getFullPathName());
        return;
    }
    if (!persistenceFile_.replaceWithText(root.toString(), false, true))
    {
        juce::Logger::writeToLog("[UiLayout] could not write " + persistenceFile_.getFullPathName());
    }
}
