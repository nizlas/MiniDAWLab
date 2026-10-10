#include "engine/ReadAheadStartupConfig.h"

namespace readahead
{
namespace
{
ReadAheadProcessConfig gPublished{ 0, false, false, true, false };

struct CliChoice
{
    bool seen = false;
    bool enabled = false;
    int depth = kDefaultReadAheadDepth;
    int flagCount = 0;
};

void noteCli(CliChoice& choice, const bool enabled, const int depth) noexcept
{
    choice.seen = true;
    choice.enabled = enabled;
    choice.depth = depth;
    ++choice.flagCount;
}

[[nodiscard]] int clampExplicitDepth(const int raw) noexcept
{
    return juce::jlimit(kReadAheadDepthMin, kReadAheadDepthMax, raw);
}
} // namespace

static_assert(kDefaultReadAheadDepth >= kReadAheadDepthMin && kDefaultReadAheadDepth <= kReadAheadDepthMax,
              "default read-ahead depth must stay inside the renderer limits");

juce::File defaultReadAheadPreferenceFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("read-ahead.xml");
}

std::optional<bool> loadReadAheadEnabledPreference(const juce::File& file)
{
    if (!file.existsAsFile())
    {
        return std::nullopt;
    }
    const std::unique_ptr<juce::XmlElement> xml(juce::XmlDocument::parse(file));
    if (xml == nullptr || !xml->hasTagName("READAHEAD") || !xml->hasAttribute("enabled"))
    {
        return std::nullopt;
    }
    const juce::String value = xml->getStringAttribute("enabled").trim().toLowerCase();
    if (value == "1" || value == "true")
    {
        return true;
    }
    if (value == "0" || value == "false")
    {
        return false;
    }
    return std::nullopt;
}

bool saveReadAheadEnabledPreference(const juce::File& file, const bool enabled)
{
    const juce::File parent = file.getParentDirectory();
    if (!parent.isDirectory() && !parent.createDirectory())
    {
        juce::Logger::writeToLog("[read-ahead] could not create settings directory: " + parent.getFullPathName());
        return false;
    }
    juce::XmlElement root("READAHEAD");
    root.setAttribute("enabled", enabled ? "1" : "0");
    if (!file.replaceWithText(root.toString(), false, true))
    {
        juce::Logger::writeToLog("[read-ahead] could not write " + file.getFullPathName());
        return false;
    }
    return true;
}

ReadAheadProcessConfig resolveReadAheadProcessConfig(const juce::StringArray& args,
                                                     const juce::File& preferenceFile)
{
    CliChoice cli;
    for (const juce::String& arg : args)
    {
        if (arg == "--no-readahead")
        {
            noteCli(cli, false, 0);
        }
        else if (arg == "--experimental-readahead")
        {
            noteCli(cli, true, kDefaultReadAheadDepth);
        }
        else if (arg.startsWith("--experimental-readahead="))
        {
            const int raw = arg.fromFirstOccurrenceOf("=", false, false).getIntValue();
            noteCli(cli, true, clampExplicitDepth(raw));
        }
    }

    const std::optional<bool> saved = loadReadAheadEnabledPreference(preferenceFile);
    ReadAheadProcessConfig config;
    config.hasSavedChoice = saved.has_value();
    config.nextStartEnabled = saved.value_or(true);
    config.commandLineOverride = cli.seen;
    config.conflictingArguments = cli.flagCount > 1;
    if (cli.seen)
    {
        config.activeDepth = cli.enabled ? cli.depth : 0;
    }
    else if (saved.has_value() && !(*saved))
    {
        config.activeDepth = 0;
    }
    else
    {
        config.activeDepth = kDefaultReadAheadDepth;
    }
    return config;
}

void publishReadAheadProcessConfig(const ReadAheadProcessConfig& config) noexcept
{
    gPublished = config;
}

ReadAheadProcessConfig publishedReadAheadProcessConfig() noexcept
{
    return gPublished;
}

} // namespace readahead
