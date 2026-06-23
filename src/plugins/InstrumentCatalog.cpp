#include "plugins/InstrumentCatalog.h"

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "plugins/PluginDiscovery.h"
#include "plugins/Vst3ChildProcessScan.h"

#include <set>

namespace mini_daw
{

namespace
{
    [[nodiscard]] juce::File instrumentCatalogRescanLogFileInternal()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("instrument-catalog-rescan.log");
    }

    void appendInstrumentCatalogRescanLineFlushed(const juce::String& message)
    {
        try
        {
            const juce::File f = instrumentCatalogRescanLogFileInternal();
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

    [[nodiscard]] const char* oopOutcomeTag(const Vst3OopScanOutcome o) noexcept
    {
        switch (o)
        {
        case Vst3OopScanOutcome::Success:
            return "Success";
        case Vst3OopScanOutcome::ChildCrashedOrFailed:
            return "ChildCrashedOrFailed";
        case Vst3OopScanOutcome::Timeout:
            return "Timeout";
        case Vst3OopScanOutcome::LaunchFailed:
            return "LaunchFailed";
        case Vst3OopScanOutcome::ParseFailed:
            return "ParseFailed";
        default:
            return "Unknown";
        }
    }

    [[nodiscard]] bool pathLooksLikeBbcSymphonyOrchestra(const juce::String& path) noexcept
    {
        return path.containsIgnoreCase("BBC Symphony Orchestra");
    }

    void writeInstrumentCatalogV1Atomic(const std::vector<InstrumentCatalogEntry>& accepted,
                                        const InstrumentCatalogRescanSummary& summary)
    {
        juce::XmlElement root("InstrumentCatalog");
        root.setAttribute("version", 1);

        auto* scanEl = root.createNewChildElement("scan");
        scanEl->setAttribute("completedAt", juce::Time::getCurrentTime().toISO8601(true));
        scanEl->setAttribute("candidateBundleCount", summary.candidateBundleCount);
        scanEl->setAttribute("acceptedInstrumentCount", summary.acceptedInstrumentCount);
        scanEl->setAttribute("rejectedEffectCount", summary.rejectedEffectCount);
        scanEl->setAttribute("rejectedValidationFailedCount", summary.rejectedValidationFailedCount);
        scanEl->setAttribute("rejectedDuplicateCount", summary.rejectedDuplicateCount);

        for (const auto& e : accepted)
        {
            auto* inst = root.createNewChildElement("instrument");
            inst->setAttribute("bundle", e.bundlePath);
            inst->setAttribute("name", e.description.name);
            inst->setAttribute("manufacturer", e.description.manufacturerName);
            inst->setAttribute("category", e.description.category);
            inst->setAttribute("isInstrument", e.description.isInstrument ? 1 : 0);
            inst->setAttribute("classificationReason", e.classificationReason);
            inst->setAttribute("uid", e.description.uniqueId);
            inst->setAttribute("fileOrIdentifier", e.description.fileOrIdentifier);
            if (auto xml = e.description.createXml())
            {
                inst->addChildElement(xml.release());
            }
        }

        const juce::File cacheFile = getInstrumentCatalogV1CacheFile();
        if (!cacheFile.getParentDirectory().isDirectory())
        {
            (void)cacheFile.getParentDirectory().createDirectory();
        }

        const juce::File temp = cacheFile.getSiblingFile(cacheFile.getFileName() + ".tmp");
        if (!temp.replaceWithText(root.toString(), false, true))
        {
            writeInstrumentCatalogRescanLogLine("catalog write failed temp=\"" + temp.getFullPathName() + "\"");
            return;
        }
        if (!temp.moveFileTo(cacheFile))
        {
            writeInstrumentCatalogRescanLogLine("catalog replace failed path=\"" + cacheFile.getFullPathName() + "\"");
            return;
        }
        writeInstrumentCatalogRescanLogLine("catalog written path=\"" + cacheFile.getFullPathName() + "\" accepted="
                                            + juce::String((int)accepted.size()));
    }

} // namespace

juce::File getInstrumentCatalogV1CacheFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("instrument-catalog-v1.xml");
}

juce::File getInstrumentCatalogRescanLogFile()
{
    return instrumentCatalogRescanLogFileInternal();
}

void writeInstrumentCatalogRescanLogLine(const juce::String& message)
{
    appendInstrumentCatalogRescanLineFlushed(message);
    juce::Logger::writeToLog("[instrument-catalog] " + message);
}

bool classifyPluginDescriptionAsInstrument(const juce::PluginDescription& d,
                                           juce::String& reasonOut) noexcept
{
    if (d.isInstrument)
    {
        reasonOut = "isInstrument=1";
        return true;
    }
    const juce::String cat = d.category.toLowerCase();
    if (cat.contains("instrument") || cat.contains("synth"))
    {
        reasonOut = "categoryFallback=\"" + d.category + "\"";
        return true;
    }
    reasonOut = "rejectedEffect isInstrument=0 category=\"" + d.category + "\"";
    return false;
}

InstrumentCatalogRescanSummary rescanInstrumentCatalogBlocking()
{
    InstrumentCatalogRescanSummary summary;
    writeInstrumentCatalogRescanLogLine("rescan started log=\"" + getInstrumentCatalogRescanLogFile().getFullPathName()
                                        + "\" catalog=\"" + getInstrumentCatalogV1CacheFile().getFullPathName() + "\"");

    juce::FileSearchPath combined = getStandardVst3SearchPaths();
    const juce::FileSearchPath userPaths = loadUserVst3SearchPaths();
    for (int i = 0; i < userPaths.getNumPaths(); ++i)
    {
        combined.add(userPaths[i], -1);
    }

    for (int i = 0; i < combined.getNumPaths(); ++i)
    {
        const juce::String folder = combined[i].getFullPathName();
        summary.scannedFolderPaths.add(folder);
        writeInstrumentCatalogRescanLogLine("scanned folder: \"" + folder + "\"");
    }

    const PluginDiscoveryResult discovery = scanForVst3Plugins(combined);
    summary.candidateBundleCount = static_cast<int>(discovery.entries.size());

    writeInstrumentCatalogRescanLogLine("candidate bundles: " + juce::String(summary.candidateBundleCount));

    std::vector<InstrumentCatalogEntry> accepted;
    std::set<juce::String> seenInstrumentKeys;

    for (const auto& entry : discovery.entries)
    {
        const juce::File bundle = entry.file;
        const juce::String bundlePath = bundle.getFullPathName();
        const bool isBbc = pathLooksLikeBbcSymphonyOrchestra(bundlePath);

        writeInstrumentCatalogRescanLogLine("candidate: \"" + bundlePath + "\"");

        const Vst3OopScanResult scanResult = runVst3OopScanBlocking(bundle, kVst3OopScanReplyTimeoutMs);
        const bool scanOk = scanResult.outcome == Vst3OopScanOutcome::Success && !scanResult.descriptions.empty();

        if (!scanOk)
        {
            ++summary.rejectedValidationFailedCount;
            const juce::String failLine = "  validation failed outcome=" + juce::String(oopOutcomeTag(scanResult.outcome))
                                          + " descriptionCount=" + juce::String((int)scanResult.descriptions.size());
            writeInstrumentCatalogRescanLogLine(failLine);
            if (isBbc)
            {
                summary.bbcSymphonyOrchestraFinding
                    = "found at \"" + bundlePath + "\" but OOP scan failed (" + oopOutcomeTag(scanResult.outcome) + ")";
            }
            continue;
        }

        for (const auto& desc : scanResult.descriptions)
        {
            juce::String classReason;
            const bool isInst = classifyPluginDescriptionAsInstrument(desc, classReason);
            const juce::String descLine = "  description: name=\"" + desc.name + "\" manufacturer=\""
                                          + desc.manufacturerName + "\" isInstrument="
                                          + juce::String(desc.isInstrument ? 1 : 0) + " category=\"" + desc.category
                                          + "\" uid=" + juce::String(desc.uniqueId) + " -> " + classReason;
            writeInstrumentCatalogRescanLogLine(descLine);

            if (isBbc)
            {
                summary.bbcSymphonyOrchestraFinding = isInst
                    ? ("accepted as instrument (" + classReason + ") at \"" + bundlePath + "\" name=\"" + desc.name
                       + "\"")
                    : ("rejected at \"" + bundlePath + "\" name=\"" + desc.name + "\" reason=" + classReason);
            }

            if (!isInst)
            {
                ++summary.rejectedEffectCount;
                writeInstrumentCatalogRescanLogLine("  rejected: effect/non-instrument");
                continue;
            }

            const juce::String dedupeKey = bundlePath.toLowerCase() + "|" + desc.createIdentifierString();
            if (seenInstrumentKeys.count(dedupeKey) != 0)
            {
                ++summary.rejectedDuplicateCount;
                writeInstrumentCatalogRescanLogLine("  rejected: duplicate key=\"" + dedupeKey + "\"");
                continue;
            }
            seenInstrumentKeys.insert(dedupeKey);

            InstrumentCatalogEntry catalogEntry;
            catalogEntry.bundlePath = bundlePath;
            catalogEntry.description = desc;
            catalogEntry.classificationReason = classReason;
            accepted.push_back(std::move(catalogEntry));
            ++summary.acceptedInstrumentCount;
            writeInstrumentCatalogRescanLogLine("  accepted: instrument name=\"" + desc.name + "\"");
        }
    }

    for (const auto& inaccessible : discovery.inaccessibleFolders)
    {
        writeInstrumentCatalogRescanLogLine("inaccessible folder: \"" + inaccessible.getFullPathName() + "\"");
    }

    writeInstrumentCatalogV1Atomic(accepted, summary);

    summary.completed = true;
    writeInstrumentCatalogRescanLogLine(
        "summary: candidates=" + juce::String(summary.candidateBundleCount) + " accepted="
        + juce::String(summary.acceptedInstrumentCount) + " rejected_effect="
        + juce::String(summary.rejectedEffectCount) + " validation_failed="
        + juce::String(summary.rejectedValidationFailedCount) + " duplicates="
        + juce::String(summary.rejectedDuplicateCount));

    if (summary.bbcSymphonyOrchestraFinding.isEmpty())
    {
        writeInstrumentCatalogRescanLogLine("BBC Symphony Orchestra: not found in scanned candidate paths");
    }
    else
    {
        writeInstrumentCatalogRescanLogLine("BBC Symphony Orchestra: " + summary.bbcSymphonyOrchestraFinding);
    }

    return summary;
}

bool loadInstrumentCatalogFromCache(std::vector<InstrumentCatalogEntry>& out)
{
    out.clear();
    const juce::File cacheFile = getInstrumentCatalogV1CacheFile();
    if (!cacheFile.existsAsFile())
    {
        return false;
    }

    const std::unique_ptr<juce::XmlElement> root = juce::parseXML(cacheFile);
    if (root == nullptr || !root->hasTagName("InstrumentCatalog"))
    {
        return false;
    }

    for (juce::XmlElement* inst = root->getFirstChildElement(); inst != nullptr;
         inst = inst->getNextElement())
    {
        if (!inst->hasTagName("instrument"))
        {
            continue;
        }

        InstrumentCatalogEntry e;
        e.bundlePath = inst->getStringAttribute("bundle");
        e.classificationReason = inst->getStringAttribute("classificationReason");

        if (juce::XmlElement* const pluginXml = inst->getChildByName("PLUGIN"))
        {
            e.description.loadFromXml(*pluginXml);
        }
        else
        {
            e.description.name = inst->getStringAttribute("name");
            e.description.manufacturerName = inst->getStringAttribute("manufacturer");
            e.description.category = inst->getStringAttribute("category");
            e.description.isInstrument = inst->getIntAttribute("isInstrument", 0) != 0;
            e.description.uniqueId = inst->getIntAttribute("uid", 0);
            e.description.fileOrIdentifier = inst->getStringAttribute("fileOrIdentifier", e.bundlePath);
        }

        if (e.bundlePath.isEmpty())
        {
            continue;
        }
        if (e.description.fileOrIdentifier.isEmpty())
        {
            e.description.fileOrIdentifier = e.bundlePath;
        }

        juce::String classReason;
        if (!classifyPluginDescriptionAsInstrument(e.description, classReason))
        {
            continue;
        }

        out.push_back(std::move(e));
    }

    std::sort(
        out.begin(),
        out.end(),
        [](const InstrumentCatalogEntry& a, const InstrumentCatalogEntry& b) {
            const int c = a.description.name.compareNatural(b.description.name);
            if (c != 0)
            {
                return c < 0;
            }
            return a.bundlePath.compareIgnoreCase(b.bundlePath) < 0;
        });

    return !out.empty();
}

void fillProjectGenericVst3DescriptorFromPluginDescription(
    ProjectFileGenericVst3DescriptorV1& out,
    const juce::PluginDescription& d) noexcept
{
    out.name = d.name.replaceCharacter('\0', ' ');
    out.descriptiveName = d.descriptiveName.replaceCharacter('\0', ' ');
    out.manufacturerName = d.manufacturerName.replaceCharacter('\0', ' ');
    out.pluginFormatName = d.pluginFormatName.replaceCharacter('\0', ' ');
    out.category = d.category.replaceCharacter('\0', ' ');
    out.fileOrIdentifier = d.fileOrIdentifier.replaceCharacter('\0', ' ');
    out.uniqueId = d.uniqueId;
    out.deprecatedUid = d.deprecatedUid;
    out.isInstrument = d.isInstrument;
}

juce::PluginDescription pluginDescriptionFromProjectGenericVst3Descriptor(
    const ProjectFileGenericVst3DescriptorV1& saved) noexcept
{
    juce::PluginDescription d;
    d.name = saved.name;
    d.descriptiveName = saved.descriptiveName;
    d.manufacturerName = saved.manufacturerName;
    d.pluginFormatName = saved.pluginFormatName;
    d.category = saved.category;
    d.fileOrIdentifier = saved.fileOrIdentifier;
    d.uniqueId = saved.uniqueId;
    d.deprecatedUid = saved.deprecatedUid;
    d.isInstrument = saved.isInstrument;
    return d;
}

GenericVst3ProjectLoadResolution tryResolveGenericVst3ForProjectLoad(
    const bool hasSavedDescriptor,
    const ProjectFileGenericVst3DescriptorV1& savedDescriptor,
    const juce::String& savedBundlePath,
    const juce::String& fallbackDisplayName) noexcept
{
    GenericVst3ProjectLoadResolution out;
    juce::PluginDescription candidateDesc;
    if (hasSavedDescriptor)
    {
        candidateDesc = pluginDescriptionFromProjectGenericVst3Descriptor(savedDescriptor);
    }
    else if (fallbackDisplayName.isNotEmpty())
    {
        candidateDesc.name = fallbackDisplayName;
    }

    const auto tryAccept = [&](const juce::File& bundle, juce::PluginDescription desc, const char* sourceTag) noexcept
        -> bool
    {
        if (!bundle.exists())
        {
            appendProjectLoadDiagnosticLine(juce::String("load: GenericVst3 resolve reject source=") + sourceTag
                                            + " reason=bundle-missing path=\"" + bundle.getFullPathName() + "\"");
            return false;
        }
        desc.fileOrIdentifier = bundle.getFullPathName();
        if (desc.pluginFormatName.isEmpty())
        {
            desc.pluginFormatName = "VST3";
        }
        juce::String reason;
        if (!classifyPluginDescriptionAsInstrument(desc, reason))
        {
            appendProjectLoadDiagnosticLine(juce::String("load: GenericVst3 resolve reject source=") + sourceTag
                                            + " reason=not-instrument " + reason + " path=\""
                                            + bundle.getFullPathName() + "\"");
            return false;
        }
        out.resolved = true;
        out.bundle = bundle;
        out.description = desc;
        appendProjectLoadDiagnosticLine(juce::String("load: GenericVst3 resolve ok source=") + sourceTag
                                        + " path=\"" + bundle.getFullPathName() + "\" name=\"" + desc.name + "\"");
        return true;
    };

    std::vector<InstrumentCatalogEntry> catalog;
    const bool catalogLoaded = loadInstrumentCatalogFromCache(catalog);
    appendProjectLoadDiagnosticLine("load: GenericVst3 resolve begin hasDescriptor="
                                    + juce::String(hasSavedDescriptor ? "yes" : "no") + " bundlePath=\""
                                    + savedBundlePath + "\" catalogLoaded="
                                    + juce::String(catalogLoaded ? "yes" : "no"));

    if (savedBundlePath.isNotEmpty() && catalogLoaded)
    {
        for (const InstrumentCatalogEntry& e : catalog)
        {
            if (e.bundlePath.equalsIgnoreCase(savedBundlePath))
            {
                if (tryAccept(juce::File(e.bundlePath), e.description, "catalog-bundle-path"))
                {
                    return out;
                }
            }
        }
    }

    if (savedBundlePath.isNotEmpty())
    {
        const juce::File savedBundle(savedBundlePath);
        if (tryAccept(savedBundle, candidateDesc, "saved-bundle-path"))
        {
            return out;
        }
    }

    if (!catalogLoaded)
    {
        appendProjectLoadDiagnosticLine("load: GenericVst3 resolve catalog cache unavailable");
        return out;
    }

    if (hasSavedDescriptor && savedDescriptor.uniqueId != 0)
    {
        for (const InstrumentCatalogEntry& e : catalog)
        {
            if (e.description.uniqueId == savedDescriptor.uniqueId)
            {
                if (tryAccept(juce::File(e.bundlePath), e.description, "catalog-uniqueId"))
                {
                    return out;
                }
            }
        }
    }

    const juce::String matchName
        = (hasSavedDescriptor && savedDescriptor.name.isNotEmpty()) ? savedDescriptor.name : fallbackDisplayName;
    const juce::String matchMfg = hasSavedDescriptor ? savedDescriptor.manufacturerName : juce::String{};
    if (matchName.isNotEmpty())
    {
        for (const InstrumentCatalogEntry& e : catalog)
        {
            if (e.description.name != matchName)
            {
                continue;
            }
            if (matchMfg.isNotEmpty() && e.description.manufacturerName != matchMfg)
            {
                continue;
            }
            if (tryAccept(juce::File(e.bundlePath), e.description, "catalog-name"))
            {
                return out;
            }
        }
    }

    appendProjectLoadDiagnosticLine("load: GenericVst3 resolve failed all strategies");
    return out;
}

} // namespace mini_daw
