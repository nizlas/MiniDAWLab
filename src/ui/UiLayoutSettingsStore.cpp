#include "ui/UiLayoutSettingsStore.h"

#include <cstdlib>

namespace
{
    constexpr const char* kRootTag = "UI_LAYOUT";
    constexpr const char* kTrackHeaderColumnTag = "TRACK_HEADER_COLUMN";
    constexpr const char* kWidthAttribute = "widthPx";
    constexpr const char* kMixerWindowTag = "MIXER_WINDOW";
    constexpr const char* kMixerSectionsTag = "MIXER_SECTIONS";
    constexpr const char* kMixerSectionHeightsTag = "MIXER_SECTION_HEIGHTS";
    constexpr int kMixerBoundsLimitPx = 20000;
    constexpr int kMixerSectionHeightLimitPx = 5000;

    /// Strict integer attribute (optional leading '-', digits only) — anything else is absent.
    [[nodiscard]] std::optional<int> strictIntAttribute(const juce::XmlElement& e, const char* name)
    {
        const juce::String raw = e.getStringAttribute(name).trim();
        if (raw.isEmpty())
        {
            return std::nullopt;
        }
        const juce::String digits = raw.startsWithChar('-') ? raw.substring(1) : raw;
        if (digits.isEmpty() || !digits.containsOnly("0123456789"))
        {
            return std::nullopt;
        }
        return raw.getIntValue();
    }
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
    mixerWindowBounds_.reset();
    mixerSectionShown_.clear();
    mixerSectionHeightPx_.clear();
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
    if (const juce::XmlElement* mw = xml->getChildByName(juce::StringRef(kMixerWindowTag)))
    {
        const auto x = strictIntAttribute(*mw, "x");
        const auto y = strictIntAttribute(*mw, "y");
        const auto w = strictIntAttribute(*mw, "width");
        const auto h = strictIntAttribute(*mw, "height");
        if (x && y && w && h && *w > 0 && *h > 0 && *w <= kMixerBoundsLimitPx && *h <= kMixerBoundsLimitPx
            && std::abs(*x) <= kMixerBoundsLimitPx && std::abs(*y) <= kMixerBoundsLimitPx)
        {
            mixerWindowBounds_ = juce::Rectangle<int>(*x, *y, *w, *h);
        }
        else
        {
            juce::Logger::writeToLog("[UiLayout] ignoring invalid mixer window bounds; defaults apply");
        }
    }
    if (const juce::XmlElement* ms = xml->getChildByName(juce::StringRef(kMixerSectionsTag)))
    {
        for (int i = 0; i < ms->getNumAttributes(); ++i)
        {
            const juce::String value = ms->getAttributeValue(i).trim();
            if (value == "1" || value == "0")
            {
                mixerSectionShown_[ms->getAttributeName(i)] = (value == "1");
            }
        }
    }
    if (const juce::XmlElement* mh = xml->getChildByName(juce::StringRef(kMixerSectionHeightsTag)))
    {
        for (int i = 0; i < mh->getNumAttributes(); ++i)
        {
            const auto v = strictIntAttribute(*mh, mh->getAttributeName(i).toRawUTF8());
            // Strictly positive and sane; anything else is absent (the mixer applies its default).
            if (v && *v > 0 && *v <= kMixerSectionHeightLimitPx)
            {
                mixerSectionHeightPx_[mh->getAttributeName(i)] = *v;
            }
            else
            {
                juce::Logger::writeToLog("[UiLayout] ignoring invalid mixer section height \"" + mh->getAttributeValue(i) + "\" for " + mh->getAttributeName(i));
            }
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

void UiLayoutSettingsStore::setMixerWindowBounds(const juce::Rectangle<int> bounds) noexcept
{
    if (bounds.getWidth() > 0 && bounds.getHeight() > 0)
    {
        mixerWindowBounds_ = bounds;
    }
}

std::optional<bool> UiLayoutSettingsStore::getMixerSectionShown(const juce::String& key) const
{
    const auto it = mixerSectionShown_.find(key);
    if (it == mixerSectionShown_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void UiLayoutSettingsStore::setMixerSectionShown(const juce::String& key, const bool shown)
{
    if (key.isNotEmpty())
    {
        mixerSectionShown_[key] = shown;
    }
}

std::optional<int> UiLayoutSettingsStore::getMixerSectionHeightPx(const juce::String& key) const
{
    const auto it = mixerSectionHeightPx_.find(key);
    if (it == mixerSectionHeightPx_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void UiLayoutSettingsStore::setMixerSectionHeightPx(const juce::String& key, const int heightPx)
{
    if (key.isNotEmpty() && heightPx > 0 && heightPx <= kMixerSectionHeightLimitPx)
    {
        mixerSectionHeightPx_[key] = heightPx;
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
    if (mixerWindowBounds_.has_value())
    {
        auto* mw = root.createNewChildElement(kMixerWindowTag);
        mw->setAttribute("x", mixerWindowBounds_->getX());
        mw->setAttribute("y", mixerWindowBounds_->getY());
        mw->setAttribute("width", mixerWindowBounds_->getWidth());
        mw->setAttribute("height", mixerWindowBounds_->getHeight());
    }
    if (!mixerSectionShown_.empty())
    {
        auto* ms = root.createNewChildElement(kMixerSectionsTag);
        for (const auto& [key, shown] : mixerSectionShown_)
        {
            ms->setAttribute(key, shown ? "1" : "0");
        }
    }
    if (!mixerSectionHeightPx_.empty())
    {
        auto* mh = root.createNewChildElement(kMixerSectionHeightsTag);
        for (const auto& [key, px] : mixerSectionHeightPx_)
        {
            mh->setAttribute(key, px);
        }
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
