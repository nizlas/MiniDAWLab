#include "plugins/Vst3ChildProcessScan.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

namespace mini_daw
{

juce::File getExperimentalVst3DescriptionsCacheFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("experimental-vst3-descriptions.xml");
}

namespace
{
    [[nodiscard]] juce::File getVst3OopScanLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("experimental-vst3-oop-scan.log");
    }

    void appendVst3OopScanLineFlushed(const juce::String& message)
    {
        try
        {
            const juce::File f = getVst3OopScanLogFile();
            if (!f.getParentDirectory().isDirectory())
            {
                (void)f.getParentDirectory().createDirectory();
            }
            const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
            const char* const utf8 = line.toRawUTF8();
            const size_t numBytes = (size_t)line.getNumBytesAsUTF8();
            if (utf8 == nullptr || numBytes == 0U)
            {
                return;
            }
            for (int attempt = 0; attempt < 8; ++attempt)
            {
                if (f.appendData(utf8, numBytes))
                {
                    return;
                }
                juce::Thread::sleep(12);
            }
            (void)f.appendText(line);
        }
        catch (...)
        {
        }
    }

    [[nodiscard]] int elapsedMsSince(const std::chrono::steady_clock::time_point t0) noexcept
    {
        return (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    }

    void logParentScanOutcome(const Vst3OopScanResult& out, const int elapsedMs)
    {
        juce::String line = "parent: outcome=";
        switch (out.outcome)
        {
        case Vst3OopScanOutcome::Success:
            line << "completed descriptionCount=" << juce::String(out.descriptionCount);
            break;
        case Vst3OopScanOutcome::Timeout:
            line << "timeout";
            break;
        case Vst3OopScanOutcome::ChildCrashedOrFailed:
            line << "child_crashed_or_failed";
            break;
        case Vst3OopScanOutcome::LaunchFailed:
            line << "launch_failed";
            break;
        case Vst3OopScanOutcome::ParseFailed:
            line << "parse_failed";
            break;
        default:
            line << "launch_failed";
            break;
        }
        line << " elapsedMs=" << juce::String(elapsedMs);
        appendVst3OopScanLineFlushed(line);
    }

    [[nodiscard]] juce::String xmlRootTag() noexcept
    {
        return "MiniDAWVst3ScanResult";
    }

    [[nodiscard]] bool parseScanResultXml(const juce::String& xmlText,
                                         juce::StringArray& linesOut,
                                         std::vector<juce::PluginDescription>& descriptionsOut,
                                         int& countOut,
                                         juce::String& errOut)
    {
        linesOut.clear();
        descriptionsOut.clear();
        countOut = 0;
        errOut.clear();
        if (xmlText.isEmpty())
        {
            errOut = "empty XML";
            return false;
        }
        const std::unique_ptr<juce::XmlElement> root = juce::parseXML(xmlText);
        if (root == nullptr || !root->hasTagName(xmlRootTag()))
        {
            errOut = "not a MiniDAWVst3ScanResult document";
            return false;
        }
        countOut = root->getNumChildElements();
        for (auto* e : root->getChildIterator())
        {
            if (e == nullptr)
            {
                continue;
            }
            juce::PluginDescription d;
            if (!d.loadFromXml(*e))
            {
                linesOut.add("[unparsed child element tag=\"" + e->getTagName() + "\"]");
                continue;
            }
            descriptionsOut.push_back(d);
            juce::String one = "name=\"" + d.name + "\"";
            if (d.manufacturerName.isNotEmpty())
            {
                one << " manufacturer=\"" << d.manufacturerName << "\"";
            }
            if (d.pluginFormatName.isNotEmpty())
            {
                one << " format=\"" << d.pluginFormatName << "\"";
            }
            linesOut.add(one);
        }
        return true;
    }

    [[nodiscard]] static juce::String resultXMLSubstring(const juce::String& s)
    {
        if (s.length() < 800)
        {
            return s;
        }
        return s.substring(0, 800) + "...";
    }

    /// Windows-style argv split (double quotes; no `juce_ArgumentList` include on some JUCE setups).
    [[nodiscard]] juce::StringArray splitCommandLineToArgs(const juce::String& commandLine)
    {
        juce::StringArray out;
        juce::String cur;
        bool inQuotes = false;
        const juce::String s = commandLine.trim();
        for (int i = 0; i < s.length(); ++i)
        {
            const juce::juce_wchar c = s[i];
            if (c == '"')
            {
                inQuotes = !inQuotes;
                continue;
            }
            if (!inQuotes && juce::CharacterFunctions::isWhitespace(c))
            {
                if (cur.isNotEmpty())
                {
                    out.add(cur);
                    cur.clear();
                }
                continue;
            }
            cur += c;
        }
        if (cur.isNotEmpty())
        {
            out.add(cur);
        }
        return out;
    }
} // namespace

namespace vst3_experimental_desc_cache
{
    [[nodiscard]] juce::String experimentalCacheRootTag() noexcept
    {
        return "MiniDAWExperimentalVst3Descriptions";
    }

    [[nodiscard]] juce::String scanOutcomeToAttributeString(const Vst3ExperimentalCacheScanOutcome o) noexcept
    {
        return o == Vst3ExperimentalCacheScanOutcome::Success ? "success" : "failed";
    }

    [[nodiscard]] std::unique_ptr<juce::XmlElement> pluginCapabilitiesToXmlElement(const PluginCapabilities& caps)
    {
        auto root = std::make_unique<juce::XmlElement>("capabilities");
        if (caps.drumNoteDisplay.has_value())
        {
            root->addChildElement(std::make_unique<juce::XmlElement>("drumNoteDisplay").release());
        }
        if (caps.rawPitchMap.has_value())
        {
            root->addChildElement(std::make_unique<juce::XmlElement>("rawPitchMap").release());
        }
        if (caps.playableRange.has_value())
        {
            root->addChildElement(std::make_unique<juce::XmlElement>("playableRange").release());
        }
        if (caps.programList.has_value())
        {
            root->addChildElement(std::make_unique<juce::XmlElement>("programList").release());
        }
        return root;
    }

    /// Phase 2 wrapper only — must be lowercase `plugin`. JUCE persists descriptions as uppercase `PLUGIN`.
    [[nodiscard]] bool isPhase2PluginWrapperElement(const juce::XmlElement* e) noexcept
    {
        return e != nullptr && e->getTagName() == juce::String("plugin");
    }

    [[nodiscard]] bool isCapabilitiesElement(const juce::XmlElement* e) noexcept
    {
        return e != nullptr && e->getTagName() == juce::String("capabilities");
    }

    void collectPluginDescriptionsFromBundle(juce::XmlElement* bundle,
                                             std::vector<juce::PluginDescription>& descriptionsOut)
    {
        descriptionsOut.clear();
        if (bundle == nullptr || !bundle->hasTagName("bundle"))
        {
            return;
        }
        bool anyPhase2Wrapper = false;
        for (auto* e : bundle->getChildIterator())
        {
            if (isPhase2PluginWrapperElement(e))
            {
                anyPhase2Wrapper = true;
                break;
            }
        }
        if (anyPhase2Wrapper)
        {
            for (auto* e : bundle->getChildIterator())
            {
                if (!isPhase2PluginWrapperElement(e))
                {
                    continue;
                }
                for (auto* inner : e->getChildIterator())
                {
                    if (inner == nullptr || isCapabilitiesElement(inner))
                    {
                        continue;
                    }
                    juce::PluginDescription d;
                    if (d.loadFromXml(*inner))
                    {
                        descriptionsOut.push_back(std::move(d));
                    }
                }
            }
            return;
        }
        for (auto* e : bundle->getChildIterator())
        {
            if (e == nullptr)
            {
                continue;
            }
            juce::PluginDescription d;
            if (d.loadFromXml(*e))
            {
                descriptionsOut.push_back(std::move(d));
            }
        }
    }

    void applyFingerprintAttributesToBundle(juce::XmlElement& bundle,
                                            const Vst3BundleFileFingerprint& fp,
                                            const juce::String& lastScanIso,
                                            const Vst3ExperimentalCacheScanOutcome outcome)
    {
        bundle.setAttribute("schemaVersion", 2);
        bundle.setAttribute("fileSize", juce::String(fp.fileSizeBytes));
        bundle.setAttribute("fileMtimeIso", fp.fileMtimeIso);
        bundle.setAttribute("fileSha256Prefix", fp.fileSha256Prefix16Hex);
        bundle.setAttribute("lastScanIso", lastScanIso);
        bundle.setAttribute("scanOutcome", scanOutcomeToAttributeString(outcome));
    }

    void appendSortedDescriptionsToV2Bundle(juce::XmlElement& bundle,
                                            const std::vector<juce::PluginDescription>& descriptionsSorted,
                                            const PluginCapabilities& defaultCaps)
    {
        for (const auto& d : descriptionsSorted)
        {
            if (auto leg = d.createXml())
            {
                bundle.addChildElement(leg.release());
            }
            auto* wrap = new juce::XmlElement("plugin");
            wrap->setAttribute("identifier", d.createIdentifierString());
            wrap->setAttribute(
                "uid",
                juce::String::formatted("%08x", static_cast<unsigned int>(d.uniqueId) & 0xffffffffu));
            wrap->setAttribute("nameLower", d.name.toLowerCase());
            if (auto innerPd = d.createXml())
            {
                wrap->addChildElement(innerPd.release());
            }
            if (auto capsXml = pluginCapabilitiesToXmlElement(defaultCaps))
            {
                wrap->addChildElement(capsXml.release());
            }
            bundle.addChildElement(wrap);
        }
    }
} // namespace vst3_experimental_desc_cache

Vst3BundleFileFingerprint computeVst3BundleFileFingerprint(const juce::File& vst3Bundle) noexcept
{
    Vst3BundleFileFingerprint fp;
    if (!vst3Bundle.exists())
    {
        return fp;
    }
    fp.fileSizeBytes = vst3Bundle.getSize();
    fp.fileMtimeIso = juce::Time(vst3Bundle.getLastModificationTime()).toISO8601(true);
    if (!vst3Bundle.isDirectory())
    {
        constexpr juce::int64 kMaxHashBytes = 64 * 1024 * 1024;
        const juce::int64 sz = vst3Bundle.getSize();
        if (sz > 0 && sz <= kMaxHashBytes)
        {
            juce::MemoryBlock mb;
            if (vst3Bundle.loadFileAsData(mb) && mb.getSize() > 0)
            {
                try
                {
                    juce::SHA256 sha(mb.getData(), (size_t)mb.getSize());
                    fp.fileSha256Prefix16Hex = sha.toHexString().substring(0, 16);
                }
                catch (...)
                {
                }
            }
        }
    }
    return fp;
}

juce::StringArray rawScanWorkerArgsFromCommandLine(const juce::String& commandLine)
{
    juce::StringArray out = splitCommandLineToArgs(commandLine);
    const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    if (out.size() >= 1 && juce::File(out[0]) == exe)
    {
        out.remove(0);
    }
    return out;
}

bool isVst3RawScanWorkerArgv(const juce::StringArray& argv) noexcept
{
    return argv.size() >= 3 && argv[0] == kVst3OopRawScanWorkerArg;
}

int runVst3RawScanWorkerMain(const juce::StringArray& argv)
{
    appendVst3OopScanLineFlushed("child: raw-scan worker mode argc=" + juce::String(argv.size()));
    if (!isVst3RawScanWorkerArgv(argv))
    {
        appendVst3OopScanLineFlushed("child: raw-scan bad argv (expected flag + vst3 path + result path)");
        return 2;
    }

    const juce::String vst3Path = argv[1];
    const juce::File resultFile = juce::File(argv[2]);
    appendVst3OopScanLineFlushed("child: vst3=\"" + vst3Path + "\"");
    appendVst3OopScanLineFlushed("child: resultFile=\"" + resultFile.getFullPathName() + "\"");

    if (!juce::File(vst3Path).exists())
    {
        appendVst3OopScanLineFlushed("child: raw-scan vst3 path does not exist");
        return 3;
    }

    juce::OwnedArray<juce::PluginDescription> list;
    appendVst3OopScanLineFlushed("child: findAllTypesForFile START");
    {
        juce::VST3PluginFormat fmt;
        fmt.findAllTypesForFile(list, vst3Path);
    }
    appendVst3OopScanLineFlushed("child: findAllTypesForFile END count=" + juce::String(list.size()));

    juce::XmlElement root(xmlRootTag());
    for (int i = 0; i < list.size(); ++i)
    {
        const juce::PluginDescription* const d = list[i];
        if (d == nullptr)
        {
            continue;
        }
        if (auto one = d->createXml())
        {
            root.addChildElement(one.release());
        }
    }

    const juce::String xmlStr = root.toString();
    const juce::MemoryBlock mb(xmlStr.toUTF8(), (size_t)xmlStr.getNumBytesAsUTF8());
    if (!resultFile.getParentDirectory().isDirectory())
    {
        (void)resultFile.getParentDirectory().createDirectory();
    }
    if (!resultFile.replaceWithData(mb.getData(), mb.getSize()))
    {
        appendVst3OopScanLineFlushed("child: raw-scan FAILED replaceWithData result file");
        return 4;
    }
    appendVst3OopScanLineFlushed("child: wrote result file bytes=" + juce::String((int)mb.getSize()));
    appendVst3OopScanLineFlushed("child: exiting code=0");
    return 0;
}

void writeVst3OopScanDiagnosticLogLine(const juce::String& message)
{
    appendVst3OopScanLineFlushed(message);
}

bool tryLoadExperimentalVst3DescriptionsFromCache(const juce::File& vst3Bundle,
                                                  std::vector<juce::PluginDescription>& descriptionsOut)
{
    descriptionsOut.clear();
    const juce::File cacheFile = getExperimentalVst3DescriptionsCacheFile();
    if (!cacheFile.existsAsFile())
    {
        return false;
    }

    const std::unique_ptr<juce::XmlElement> root = juce::parseXML(cacheFile);
    if (root == nullptr || !root->hasTagName(vst3_experimental_desc_cache::experimentalCacheRootTag()))
    {
        return false;
    }

    int bundleCount = 0;
    for (juce::XmlElement* b = root->getFirstChildElement(); b != nullptr; b = b->getNextElement())
    {
        if (b->hasTagName("bundle"))
        {
            ++bundleCount;
        }
    }

    const juce::String key = vst3Bundle.getFullPathName();
    writeVst3OopScanDiagnosticLogLine(
        "cache parse: root tag=\"" + root->getTagName() + "\" version=\""
        + root->getStringAttribute("version", "(none)") + "\" bundleCount=" + juce::String(bundleCount)
        + " targetPath=\"" + key + "\"");

    for (juce::XmlElement* bundle = root->getFirstChildElement(); bundle != nullptr;
         bundle = bundle->getNextElement())
    {
        if (!bundle->hasTagName("bundle"))
        {
            continue;
        }
        const juce::String stored = bundle->getStringAttribute("vst3Path");
#if JUCE_WINDOWS
        const bool pathMatched = (stored.compareIgnoreCase(key) == 0);
#else
        const bool pathMatched = (stored == key);
#endif
        writeVst3OopScanDiagnosticLogLine(
            "cache parse: bundle vst3Path=\"" + stored + "\" pathMatched="
            + juce::String(pathMatched ? "yes" : "no"));
        if (!pathMatched)
        {
            continue;
        }

        vst3_experimental_desc_cache::collectPluginDescriptionsFromBundle(bundle, descriptionsOut);
        juce::String firstLoaded;
        if (!descriptionsOut.empty())
        {
            const auto& d0 = descriptionsOut.front();
            firstLoaded = " firstLoaded name=\"" + d0.name + "\" file=\"" + d0.fileOrIdentifier + "\"";
        }
        writeVst3OopScanDiagnosticLogLine("cache parse: matched bundle descriptionsLoaded="
                                          + juce::String((int)descriptionsOut.size()) + firstLoaded);
        return !descriptionsOut.empty();
    }

    writeVst3OopScanDiagnosticLogLine("cache parse: no matching bundle for targetPath=\"" + key + "\"");
    return false;
}

[[nodiscard]] static bool experimentalBundleXmlKeyMatches(const juce::String& storedPath,
                                                          const juce::File& vst3Bundle) noexcept
{
    const juce::String key = vst3Bundle.getFullPathName();
#if JUCE_WINDOWS
    return storedPath.compareIgnoreCase(key) == 0;
#else
    return storedPath == key;
#endif
}

#if JUCE_WINDOWS
[[nodiscard]] static juce::File findGrooveAgentSeVst3BundleOnDiskWindows()
{
    const juce::File preferredSteinberg(
        "C:\\Program Files\\Common Files\\VST3\\Steinberg\\Groove Agent SE.vst3");
    if (preferredSteinberg.isDirectory())
    {
        return preferredSteinberg;
    }
    const juce::File preferredRoot("C:\\Program Files\\Common Files\\VST3\\Groove Agent SE.vst3");
    if (preferredRoot.isDirectory())
    {
        return preferredRoot;
    }
    const juce::File vst3Root("C:\\Program Files\\Common Files\\VST3");
    if (!vst3Root.isDirectory())
    {
        return {};
    }
    for (const auto& entry : juce::RangedDirectoryIterator(
             vst3Root, true, "*", juce::File::findFilesAndDirectories))
    {
        const juce::File f = entry.getFile();
        if (f.isDirectory() && f.getFileName() == "Groove Agent SE.vst3")
        {
            return f;
        }
    }
    return {};
}
#endif

[[nodiscard]] static juce::File grooveAgentPreferredInnerModuleFile(const juce::File& bundleDir)
{
    const juce::File inner = bundleDir.getChildFile("Contents")
                                 .getChildFile("x86_64-win")
                                 .getChildFile("Groove Agent SE.vst3");
    if (inner.exists())
    {
        return inner;
    }
    return bundleDir;
}

static void applyGrooveAgentDescriptionPathRepair(juce::PluginDescription& d,
                                                const juce::String& oldBundleKeyPath,
                                                const juce::File& newBundleDir)
{
    const juce::String oldRoot = juce::File(oldBundleKeyPath).getFullPathName();
    const juce::String newRoot = newBundleDir.getFullPathName();
    juce::String fid = d.fileOrIdentifier;
    if (fid.isNotEmpty() && oldRoot.isNotEmpty())
    {
#if JUCE_WINDOWS
        if (fid.startsWithIgnoreCase(oldRoot))
        {
            fid = newRoot + fid.substring(oldRoot.length());
        }
#else
        if (fid.startsWith(oldRoot))
        {
            fid = newRoot + fid.substring(oldRoot.length());
        }
#endif
    }
    const juce::File preferred = grooveAgentPreferredInnerModuleFile(newBundleDir);
    if (preferred.exists())
        d.fileOrIdentifier = preferred.getFullPathName();
    else
        d.fileOrIdentifier = fid.isNotEmpty() ? fid : newRoot;
}

[[nodiscard]] static bool grooveAgentCachedPathsStillValid(const std::vector<juce::PluginDescription>& descs,
                                                          const juce::File& cacheKeyBundle)
{
    if (!cacheKeyBundle.exists())
    {
        return false;
    }
    if (descs.empty())
    {
        return false;
    }
    const auto& d = descs.front();
    if (d.fileOrIdentifier.isEmpty())
    {
        return true;
    }
    return juce::File{ d.fileOrIdentifier }.exists();
}

[[nodiscard]] static bool tryScanEntireExperimentalCacheForGrooveAgentSE(
    std::vector<juce::PluginDescription>& descriptionsOut,
    juce::String& bundleKeyAttributeOut)
{
    descriptionsOut.clear();
    bundleKeyAttributeOut.clear();
    const juce::File cacheFile = getExperimentalVst3DescriptionsCacheFile();
    if (!cacheFile.existsAsFile())
    {
        return false;
    }
    const std::unique_ptr<juce::XmlElement> xmlRoot = juce::parseXML(cacheFile);
    if (xmlRoot == nullptr || !xmlRoot->hasTagName(vst3_experimental_desc_cache::experimentalCacheRootTag()))
    {
        return false;
    }

    int bundleCount = 0;
    for (juce::XmlElement* b = xmlRoot->getFirstChildElement(); b != nullptr; b = b->getNextElement())
    {
        if (b->hasTagName("bundle"))
        {
            ++bundleCount;
        }
    }
    writeVst3OopScanDiagnosticLogLine(
        "groove full-cache scan: root tag=\"" + xmlRoot->getTagName() + "\" version=\""
        + xmlRoot->getStringAttribute("version", "(none)") + "\" bundleCount=" + juce::String(bundleCount));

    for (juce::XmlElement* bundle = xmlRoot->getFirstChildElement(); bundle != nullptr;
         bundle = bundle->getNextElement())
    {
        if (!bundle->hasTagName("bundle"))
        {
            continue;
        }
        const juce::String stored = bundle->getStringAttribute("vst3Path");
        std::vector<juce::PluginDescription> oneBundle;
        vst3_experimental_desc_cache::collectPluginDescriptionsFromBundle(bundle, oneBundle);
        writeVst3OopScanDiagnosticLogLine(
            "groove full-cache scan: bundle vst3Path=\"" + stored + "\" descriptionsLoaded="
            + juce::String((int)oneBundle.size()));
        for (const auto& d : oneBundle)
        {
            if (d.name.containsIgnoreCase("Groove Agent SE"))
            {
                descriptionsOut = std::move(oneBundle);
                bundleKeyAttributeOut = stored;
                juce::String firstLoaded;
                if (!descriptionsOut.empty())
                {
                    const auto& d0 = descriptionsOut.front();
                    firstLoaded = " firstLoaded name=\"" + d0.name + "\" file=\"" + d0.fileOrIdentifier + "\"";
                }
                writeVst3OopScanDiagnosticLogLine("groove full-cache scan: HIT name match bundleKey=\""
                                                  + bundleKeyAttributeOut + "\" descriptionsLoaded="
                                                  + juce::String((int)descriptionsOut.size()) + firstLoaded);
                return !descriptionsOut.empty();
            }
        }
    }

    writeVst3OopScanDiagnosticLogLine("groove full-cache scan: no Groove Agent SE entry found");
    return false;
}

void mergeExperimentalVst3DescriptionsCacheBundle(const juce::File& vst3Bundle,
                                                    const std::vector<juce::PluginDescription>& descriptions,
                                                    const Vst3ExperimentalCacheScanOutcome scanOutcome)
{
    if (descriptions.empty())
    {
        return;
    }
    std::vector<juce::PluginDescription> sorted = descriptions;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
            const int c = a.createIdentifierString().compare(b.createIdentifierString());
            if (c != 0)
            {
                return c < 0;
            }
            return a.name.compare(b.name) < 0;
        });

    int xmlCount = 0;
    for (const auto& d : sorted)
    {
        if (d.createXml() != nullptr)
        {
            ++xmlCount;
        }
    }
    if (xmlCount == 0)
    {
        writeVst3OopScanDiagnosticLogLine(
            "parent: experimental-vst3-descriptions.xml not updated (PluginDescription::createXml returned "
            "nothing for all descriptions)");
        return;
    }

    const Vst3BundleFileFingerprint fp = computeVst3BundleFileFingerprint(vst3Bundle);
    const juce::String lastScanIso = juce::Time::getCurrentTime().toISO8601(true);
    const PluginCapabilities emptyCaps;

    auto* newBundle = new juce::XmlElement("bundle");
    newBundle->setAttribute("vst3Path", vst3Bundle.getFullPathName());
    vst3_experimental_desc_cache::applyFingerprintAttributesToBundle(
        *newBundle, fp, lastScanIso, scanOutcome);
    vst3_experimental_desc_cache::appendSortedDescriptionsToV2Bundle(*newBundle, sorted, emptyCaps);

    const juce::File cacheFile = getExperimentalVst3DescriptionsCacheFile();
    try
    {
        if (!cacheFile.getParentDirectory().isDirectory())
        {
            (void)cacheFile.getParentDirectory().createDirectory();
        }

        std::unique_ptr<juce::XmlElement> root;
        if (cacheFile.existsAsFile())
        {
            root = juce::parseXML(cacheFile);
        }

        if (root == nullptr || !root->hasTagName(vst3_experimental_desc_cache::experimentalCacheRootTag()))
        {
            root = std::make_unique<juce::XmlElement>(vst3_experimental_desc_cache::experimentalCacheRootTag());
        }
        root->setAttribute("version", 2);

        juce::XmlElement* walk = root->getFirstChildElement();
        while (walk != nullptr)
        {
            juce::XmlElement* const next = walk->getNextElement();
            if (walk->hasTagName("bundle")
                && experimentalBundleXmlKeyMatches(walk->getStringAttribute("vst3Path"), vst3Bundle))
            {
                root->removeChildElement(walk, true);
            }
            walk = next;
        }

        root->addChildElement(newBundle);

        if (!root->writeTo(cacheFile))
        {
            writeVst3OopScanDiagnosticLogLine(
                "parent: experimental-vst3-descriptions.xml write FAILED path=\"" + cacheFile.getFullPathName()
                + "\"");
            return;
        }

        writeVst3OopScanDiagnosticLogLine(
            "parent: experimental-vst3-descriptions.xml updated vst3Path=\"" + vst3Bundle.getFullPathName()
            + "\" pluginXmlCount=" + juce::String(xmlCount) + " cacheSchema=v2");
    }
    catch (...)
    {
        writeVst3OopScanDiagnosticLogLine(
            "parent: experimental-vst3-descriptions.xml update threw (path=\"" + cacheFile.getFullPathName()
            + "\")");
    }
}

void mergeCapabilitiesIntoBundle(const juce::File& vst3Bundle,
                                 const juce::PluginDescription& forPlugin,
                                 const PluginCapabilities& caps)
{
    const juce::File cacheFile = getExperimentalVst3DescriptionsCacheFile();
    if (!cacheFile.existsAsFile())
    {
        return;
    }
    try
    {
        std::unique_ptr<juce::XmlElement> root = juce::parseXML(cacheFile);
        if (root == nullptr || !root->hasTagName(vst3_experimental_desc_cache::experimentalCacheRootTag()))
        {
            return;
        }
        juce::XmlElement* bundleEl = nullptr;
        for (auto* b = root->getFirstChildElement(); b != nullptr; b = b->getNextElement())
        {
            if (b->hasTagName("bundle") && experimentalBundleXmlKeyMatches(b->getStringAttribute("vst3Path"), vst3Bundle))
            {
                bundleEl = b;
                break;
            }
        }
        if (bundleEl == nullptr)
        {
            return;
        }
        bool anyPluginWrapper = false;
        for (auto* e : bundleEl->getChildIterator())
        {
            if (vst3_experimental_desc_cache::isPhase2PluginWrapperElement(e))
            {
                anyPluginWrapper = true;
                break;
            }
        }
        if (!anyPluginWrapper)
        {
            return;
        }
        const juce::String wantId = forPlugin.createIdentifierString();
        juce::XmlElement* targetPlugin = nullptr;
        for (auto* e : bundleEl->getChildIterator())
        {
            if (!vst3_experimental_desc_cache::isPhase2PluginWrapperElement(e))
            {
                continue;
            }
            if (e->getStringAttribute("identifier") == wantId)
            {
                targetPlugin = e;
                break;
            }
        }
        if (targetPlugin == nullptr)
        {
            return;
        }
        juce::XmlElement* capToRemove = nullptr;
        for (auto* ch : targetPlugin->getChildIterator())
        {
            if (ch != nullptr && vst3_experimental_desc_cache::isCapabilitiesElement(ch))
            {
                capToRemove = ch;
                break;
            }
        }
        if (capToRemove != nullptr)
        {
            targetPlugin->removeChildElement(capToRemove, true);
        }
        if (auto capXml = vst3_experimental_desc_cache::pluginCapabilitiesToXmlElement(caps))
        {
            targetPlugin->addChildElement(capXml.release());
        }
        if (!root->writeTo(cacheFile))
        {
            return;
        }
    }
    catch (...)
    {
    }
}

bool verifyExperimentalVst3DescriptionsCachePhase2() noexcept
{
    try
    {
        juce::XmlElement rootV1(vst3_experimental_desc_cache::experimentalCacheRootTag());
        rootV1.setAttribute("version", 1);
        auto* b1 = rootV1.createNewChildElement("bundle");
        b1->setAttribute("vst3Path", "C:/unittest/LegacyPlugin.vst3");
        juce::PluginDescription dLegacy;
        dLegacy.name = "LegacyPlugin";
        dLegacy.pluginFormatName = "VST3";
        dLegacy.manufacturerName = "Mfgr";
        if (auto dx = dLegacy.createXml())
        {
            b1->addChildElement(dx.release());
        }
        std::vector<juce::PluginDescription> v1Out;
        vst3_experimental_desc_cache::collectPluginDescriptionsFromBundle(b1, v1Out);
        if (v1Out.size() != 1 || v1Out[0].name != "LegacyPlugin")
        {
            return false;
        }

        juce::XmlElement rootV2(vst3_experimental_desc_cache::experimentalCacheRootTag());
        rootV2.setAttribute("version", 2);
        auto* b2 = rootV2.createNewChildElement("bundle");
        b2->setAttribute("vst3Path", "C:/unittest/WrapPlugin.vst3");
        b2->setAttribute("schemaVersion", 2);
        b2->setAttribute("fileSize", "4096");
        b2->setAttribute("fileMtimeIso", "2026-05-10T12:00:00+0000");
        b2->setAttribute("fileSha256Prefix", "deadbeefcafebabe");
        b2->setAttribute("lastScanIso", "2026-05-10T12:01:00+0000");
        b2->setAttribute("scanOutcome", "success");
        juce::PluginDescription dWrap;
        dWrap.name = "WrapPlugin";
        dWrap.pluginFormatName = "VST3";
        dWrap.manufacturerName = "Mfgr";
        dWrap.uniqueId = 0x12345678;
        if (auto leg = dWrap.createXml())
        {
            b2->addChildElement(leg.release());
        }
        auto* plug = b2->createNewChildElement("plugin");
        plug->setAttribute("identifier", dWrap.createIdentifierString());
        plug->setAttribute("uid", juce::String::formatted("%08x", static_cast<unsigned int>(dWrap.uniqueId) & 0xffffffffu));
        plug->setAttribute("nameLower", dWrap.name.toLowerCase());
        if (auto inner = dWrap.createXml())
        {
            plug->addChildElement(inner.release());
        }
        if (auto cap = vst3_experimental_desc_cache::pluginCapabilitiesToXmlElement({}))
        {
            plug->addChildElement(cap.release());
        }
        if (rootV2.getStringAttribute("version") != "2")
        {
            return false;
        }
        if (b2->getStringAttribute("fileSha256Prefix") != "deadbeefcafebabe")
        {
            return false;
        }
        int bareLegacyPd = 0;
        int pluginWrappers = 0;
        for (auto* ch : b2->getChildIterator())
        {
            if (ch == nullptr)
            {
                continue;
            }
            if (vst3_experimental_desc_cache::isPhase2PluginWrapperElement(ch))
            {
                ++pluginWrappers;
                continue;
            }
            juce::PluginDescription probe;
            if (probe.loadFromXml(*ch))
            {
                ++bareLegacyPd;
            }
        }
        if (bareLegacyPd != 1 || pluginWrappers != 1)
        {
            return false;
        }
        std::vector<juce::PluginDescription> v2Out;
        vst3_experimental_desc_cache::collectPluginDescriptionsFromBundle(b2, v2Out);
        if (v2Out.size() != 1 || v2Out[0].name != "WrapPlugin")
        {
            return false;
        }

        auto* bDup = rootV2.createNewChildElement("bundle");
        bDup->setAttribute("vst3Path", "C:/unittest/DupPlugin.vst3");
        if (auto leg2 = dWrap.createXml())
        {
            bDup->addChildElement(leg2.release());
        }
        auto* plug2 = bDup->createNewChildElement("plugin");
        plug2->setAttribute("identifier", dWrap.createIdentifierString());
        if (auto inner2 = dWrap.createXml())
        {
            plug2->addChildElement(inner2.release());
        }
        std::vector<juce::PluginDescription> dupOut;
        vst3_experimental_desc_cache::collectPluginDescriptionsFromBundle(bDup, dupOut);
        if (dupOut.size() != 1)
        {
            return false;
        }

        const juce::String xml = rootV2.toString();
        juce::TemporaryFile tmpFile(".xml");
        if (!tmpFile.getFile().replaceWithText(xml))
        {
            return false;
        }
        const std::unique_ptr<juce::XmlElement> parsed = juce::parseXML(tmpFile.getFile());
        if (parsed == nullptr || parsed->getStringAttribute("version") != "2")
        {
            return false;
        }
        juce::XmlElement* bundleWalk = nullptr;
        for (auto* c = parsed->getFirstChildElement(); c != nullptr; c = c->getNextElement())
        {
            if (c->hasTagName("bundle") && c->getStringAttribute("vst3Path") == "C:/unittest/WrapPlugin.vst3")
            {
                bundleWalk = c;
                break;
            }
        }
        if (bundleWalk == nullptr)
        {
            return false;
        }
        std::vector<juce::PluginDescription> roundTrip;
        vst3_experimental_desc_cache::collectPluginDescriptionsFromBundle(bundleWalk, roundTrip);
        if (roundTrip.size() != 1 || roundTrip[0].name != "WrapPlugin")
        {
            return false;
        }

        PluginCapabilities onlyDrum;
        onlyDrum.drumNoteDisplay = DrumNoteDisplayCapability{};
        auto capXml = vst3_experimental_desc_cache::pluginCapabilitiesToXmlElement(onlyDrum);
        if (capXml == nullptr || capXml->getNumChildElements() != 1
            || !capXml->getChildElement(0)->hasTagName("drumNoteDisplay"))
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }
    return true;
}

bool tryLoadExperimentalVst3DescriptionsFromCacheWithPathRepair(
    const juce::File& savedOrAdvisoryBundle,
    const juce::String& instrumentKind,
    std::vector<juce::PluginDescription>& descriptionsOut,
    juce::File& resolvedBundleOut,
    juce::String& infoOrWarningOut,
    bool* pathRepairWasUsedOut)
{
    descriptionsOut.clear();
    resolvedBundleOut = juce::File();
    infoOrWarningOut.clear();
    if (pathRepairWasUsedOut != nullptr)
    {
        *pathRepairWasUsedOut = false;
    }

    if (instrumentKind != "GrooveAgentSE")
    {
        infoOrWarningOut = "Path repair is only implemented for instrumentKind=GrooveAgentSE.";
        return false;
    }

    writeVst3OopScanDiagnosticLogLine("project-autoload: Groove Agent requested pluginWasLoadedOnSave=true");

    std::vector<juce::PluginDescription> descs;
    bool hit = false;
    juce::String cacheKeyForPrefix;
    if (savedOrAdvisoryBundle.getFullPathName().isNotEmpty())
    {
        hit = tryLoadExperimentalVst3DescriptionsFromCache(savedOrAdvisoryBundle, descs);
        writeVst3OopScanDiagnosticLogLine("project-autoload: cache lookup path=\""
                                          + savedOrAdvisoryBundle.getFullPathName() + "\" hit="
                                          + juce::String(hit ? "true" : "false"));
        if (hit && !descs.empty())
        {
            cacheKeyForPrefix = savedOrAdvisoryBundle.getFullPathName();
        }
    }
    else
    {
        writeVst3OopScanDiagnosticLogLine("project-autoload: cache lookup path=\"\" hit=false");
    }

    if (!hit || descs.empty())
    {
        juce::String scanKey;
        if (tryScanEntireExperimentalCacheForGrooveAgentSE(descs, scanKey))
        {
            hit = true;
            cacheKeyForPrefix = scanKey;
            writeVst3OopScanDiagnosticLogLine(
                "project-autoload: cache fallback scan hit bundleKey=\"" + scanKey + "\" count="
                + juce::String((int)descs.size()));
        }
    }

    if (!hit || descs.empty())
    {
        infoOrWarningOut
            = "No cached PluginDescription for Groove Agent SE; run OOP scan once or copy the cache from "
              "another machine.";
        writeVst3OopScanDiagnosticLogLine("project-autoload: failed (no cache entry), project remains editable");
        return false;
    }

    const juce::File cacheKeyAsFile(cacheKeyForPrefix);
    if (grooveAgentCachedPathsStillValid(descs, cacheKeyAsFile))
    {
        descriptionsOut = std::move(descs);
        resolvedBundleOut = cacheKeyAsFile;
        return true;
    }

    writeVst3OopScanDiagnosticLogLine("project-autoload: cached path missing, attempting path repair");

#if JUCE_WINDOWS
    const juce::File found = findGrooveAgentSeVst3BundleOnDiskWindows();
#else
    const juce::File found;
#endif

    if (!found.exists())
    {
        infoOrWarningOut = "Groove Agent SE was not found under standard VST3 folders (path repair failed).";
        writeVst3OopScanDiagnosticLogLine("project-autoload: failed, project remains editable");
        return false;
    }

    writeVst3OopScanDiagnosticLogLine("project-autoload: found Groove Agent bundle path=\""
                                      + found.getFullPathName() + "\"");

    for (auto& d : descs)
    {
        applyGrooveAgentDescriptionPathRepair(d, cacheKeyForPrefix, found);
    }

    if (!descs.empty())
    {
        writeVst3OopScanDiagnosticLogLine("project-autoload: repaired desc fileOrIdentifier=\""
                                            + descs.front().fileOrIdentifier + "\"");
    }

    descriptionsOut = std::move(descs);
    resolvedBundleOut = found;
    if (pathRepairWasUsedOut != nullptr)
    {
        *pathRepairWasUsedOut = true;
    }
    return true;
}

// Diagnostic-only OOP scan using raw juce::ChildProcess (no ChildProcessCoordinator). If worker
// processes are ever left behind or shutdown hangs, revisit ArgumentList parsing or Windows spawn flags.

Vst3OopScanResult runVst3OopScanBlocking(const juce::File& vst3File, const int replyTimeoutMs)
{
    const auto scanWallT0 = std::chrono::steady_clock::now();
    Vst3OopScanResult out;

    const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::File tmpXml
        = juce::File::getSpecialLocation(juce::File::tempDirectory)
              .getChildFile("minidaw-vst3-rawscan-" + juce::Uuid().toString() + ".xml");

    appendVst3OopScanLineFlushed("parent: raw-scan launch exe=\"" + exe.getFullPathName() + "\" vst3=\""
                                 + vst3File.getFullPathName() + "\" resultFile=\""
                                 + tmpXml.getFullPathName() + "\" timeoutMs=" + juce::String(replyTimeoutMs));
    const bool exeOk = exe.existsAsFile();
    const bool vst3Exists = vst3File.exists();
    const bool vst3IsDirectory = vst3Exists && vst3File.isDirectory();
    if (!exeOk || !vst3Exists)
    {
        appendVst3OopScanLineFlushed(
            "parent: raw-scan launch_failed_detail exeExistsAsFile=" + juce::String(exeOk ? "true" : "false")
            + " vst3Exists=" + juce::String(vst3Exists ? "true" : "false") + " vst3IsDirectory="
            + juce::String(vst3IsDirectory ? "true" : "false") + " procStartAttempted=false procStartOk=n/a");
        out.outcome = Vst3OopScanOutcome::LaunchFailed;
        logParentScanOutcome(out, elapsedMsSince(scanWallT0));
        (void)tmpXml.deleteFile();
        const int runReturningMs = elapsedMsSince(scanWallT0);
        writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
        return out;
    }

    juce::ChildProcess proc;
    const juce::StringArray args{ exe.getFullPathName(),
                                  kVst3OopRawScanWorkerArg,
                                  vst3File.getFullPathName(),
                                  tmpXml.getFullPathName() };
    if (!proc.start(args, 0))
    {
        appendVst3OopScanLineFlushed(
            "parent: raw-scan launch_failed_detail exeExistsAsFile=" + juce::String(exeOk ? "true" : "false")
            + " vst3Exists=true vst3IsDirectory=" + juce::String(vst3IsDirectory ? "true" : "false")
            + " procStartAttempted=true procStartOk=false");
        out.outcome = Vst3OopScanOutcome::LaunchFailed;
        logParentScanOutcome(out, elapsedMsSince(scanWallT0));
        (void)tmpXml.deleteFile();
        const int runReturningMs = elapsedMsSince(scanWallT0);
        writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
        return out;
    }

    appendVst3OopScanLineFlushed("parent: raw-scan child started OK");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(replyTimeoutMs);
    while (proc.isRunning() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (proc.isRunning())
    {
        (void)proc.kill();
        out.outcome = Vst3OopScanOutcome::Timeout;
        logParentScanOutcome(out, elapsedMsSince(scanWallT0));
        (void)tmpXml.deleteFile();
        const int runReturningMs = elapsedMsSince(scanWallT0);
        writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
        return out;
    }

    const juce::uint32 exitCode = proc.getExitCode();
    appendVst3OopScanLineFlushed(
        "parent: raw-scan child exited exitCode=" + juce::String((int)exitCode) + " elapsedMs="
        + juce::String(elapsedMsSince(scanWallT0)));

    if (exitCode != 0)
    {
        out.outcome = Vst3OopScanOutcome::ChildCrashedOrFailed;
        logParentScanOutcome(out, elapsedMsSince(scanWallT0));
        (void)tmpXml.deleteFile();
        const int runReturningMs = elapsedMsSince(scanWallT0);
        writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
        return out;
    }

    const juce::String xml = tmpXml.loadFileAsString();
    (void)tmpXml.deleteFile();

    juce::String parseErr;
    if (!parseScanResultXml(xml, out.descriptionLines, out.descriptions, out.descriptionCount, parseErr))
    {
        out.outcome = Vst3OopScanOutcome::ParseFailed;
        out.rawXmlHint = resultXMLSubstring(xml);
        logParentScanOutcome(out, elapsedMsSince(scanWallT0));
        const int runReturningMs = elapsedMsSince(scanWallT0);
        writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
        return out;
    }

    out.outcome = Vst3OopScanOutcome::Success;
    for (const auto& line : out.descriptionLines)
    {
        appendVst3OopScanLineFlushed("parent: description " + line);
    }
    logParentScanOutcome(out, elapsedMsSince(scanWallT0));

    mergeExperimentalVst3DescriptionsCacheBundle(vst3File, out.descriptions);

    const int runReturningMs = elapsedMsSince(scanWallT0);
    writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
    return out;
}

} // namespace mini_daw
