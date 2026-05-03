#include "plugins/PluginDiscovery.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <set>

namespace
{
    void logInaccessible(const juce::File& folder, std::vector<juce::File>& inaccessible)
    {
        inaccessible.push_back(folder);
        juce::Logger::writeToLog(
            juce::String("[VST3 picker] folder not accessible: ") + folder.getFullPathName());
    }

    void addVst3IfNew(const juce::File& f,
                      std::vector<mini_daw::PluginDiscoveryEntry>& out,
                      std::set<juce::String>& seenKeys)
    {
        if (!f.exists())
        {
            return;
        }
        const juce::String key = f.getFullPathName().toLowerCase();
        if (seenKeys.count(key) != 0)
        {
            return;
        }
        seenKeys.insert(key);
        mini_daw::PluginDiscoveryEntry e;
        e.displayName = f.getFileNameWithoutExtension();
        e.file = f;
        e.support = mini_daw::classifyVst3Candidate(e.displayName);
        if (e.support == mini_daw::PluginPickerSupport::UnsupportedInstrument)
        {
            e.unsupportedReason = "Instrument - not supported yet";
        }
        out.push_back(std::move(e));
    }

    void scanTree(const juce::File& root,
                  std::vector<mini_daw::PluginDiscoveryEntry>& out,
                  std::set<juce::String>& seenKeys,
                  std::vector<juce::File>& inaccessible)
    {
        if (!root.exists())
        {
            logInaccessible(root, inaccessible);
            return;
        }
        if (root.getFileExtension().equalsIgnoreCase(".vst3"))
        {
            addVst3IfNew(root, out, seenKeys);
            return;
        }
        if (!root.isDirectory())
        {
            return;
        }

        for (const auto& entry : juce::RangedDirectoryIterator(
                 root, false, "*", juce::File::findFilesAndDirectories))
        {
            const juce::File f = entry.getFile();
            if (f.getFileExtension().equalsIgnoreCase(".vst3"))
            {
                addVst3IfNew(f, out, seenKeys);
            }
            else if (f.isDirectory())
            {
                scanTree(f, out, seenKeys, inaccessible);
            }
        }
    }
} // namespace

mini_daw::PluginPickerSupport mini_daw::classifyVst3Candidate(const juce::String& displayName) noexcept
{
    static const char* const needles[] = {
        "Groove Agent",
        "HALion Sonic",
    };
    for (const char* const needle : needles)
    {
        if (displayName.containsIgnoreCase(needle))
        {
            return PluginPickerSupport::UnsupportedInstrument;
        }
    }
    return PluginPickerSupport::SupportedCandidate;
}

juce::File mini_daw::getVst3SearchPathsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("vst3-search-paths.xml");
}

juce::FileSearchPath mini_daw::getStandardVst3SearchPaths()
{
    juce::VST3PluginFormat fmt;
    return fmt.getDefaultLocationsToSearch();
}

juce::FileSearchPath mini_daw::loadUserVst3SearchPaths()
{
    juce::FileSearchPath paths;
    const juce::File file = getVst3SearchPathsFile();
    const std::unique_ptr<juce::XmlElement> xml = juce::parseXML(file);
    if (xml == nullptr || !xml->hasTagName("Vst3SearchPaths"))
    {
        return paths;
    }
    for (auto* e : xml->getChildWithTagNameIterator("Path"))
    {
        const juce::String s = e->getAllSubText().trim();
        if (s.isNotEmpty())
        {
            paths.add(juce::File(s), -1);
        }
    }
    return paths;
}

void mini_daw::saveUserVst3SearchPaths(const juce::FileSearchPath& userPaths)
{
    const juce::File file = getVst3SearchPathsFile();
    const juce::File parent = file.getParentDirectory();
    if (!parent.isDirectory() && !parent.createDirectory())
    {
        juce::Logger::writeToLog(
            juce::String("[VST3 picker] could not create settings directory: ")
            + parent.getFullPathName());
        return;
    }

    juce::XmlElement root("Vst3SearchPaths");
    for (int i = 0; i < userPaths.getNumPaths(); ++i)
    {
        juce::XmlElement* const p = root.createNewChildElement("Path");
        p->addTextElement(userPaths[i].getFullPathName());
    }
    if (!file.replaceWithText(root.toString(), false, true))
    {
        juce::Logger::writeToLog(
            juce::String("[VST3 picker] could not write search paths file: ")
            + file.getFullPathName());
    }
}

mini_daw::PluginDiscoveryResult mini_daw::scanForVst3Plugins(const juce::FileSearchPath& combinedRoots)
{
    PluginDiscoveryResult result;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    std::set<juce::String> seenKeys;

    for (int i = 0; i < combinedRoots.getNumPaths(); ++i)
    {
        scanTree(combinedRoots[i], result.entries, seenKeys, result.inaccessibleFolders);
    }

    std::sort(
        result.entries.begin(),
        result.entries.end(),
        [](const PluginDiscoveryEntry& a, const PluginDiscoveryEntry& b) {
            const int c = a.displayName.compareNatural(b.displayName);
            if (c != 0)
            {
                return c < 0;
            }
            return a.file.getFullPathName().compareIgnoreCase(b.file.getFullPathName()) < 0;
        });

    result.millisElapsed
        = juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - t0);
    juce::Logger::writeToLog(
        juce::String("[VST3 picker] scanned in ") + juce::String(result.millisElapsed)
        + " ms; entries=" + juce::String(static_cast<int>(result.entries.size())));
    return result;
}
