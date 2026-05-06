#include "plugins/Vst3ChildProcessScan.h"

#include <juce_audio_processors/juce_audio_processors.h>

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

    [[nodiscard]] static juce::String experimentalDescriptionCacheRootTag() noexcept
    {
        return "MiniDAWExperimentalVst3Descriptions";
    }

    [[nodiscard]] static bool bundlePathKeyMatchesAttribute(const juce::String& storedPath,
                                                            const juce::File& vst3Bundle) noexcept
    {
        const juce::String key = vst3Bundle.getFullPathName();
#if JUCE_WINDOWS
        return storedPath.compareIgnoreCase(key) == 0;
#else
        return storedPath == key;
#endif
    }

    void mergeWriteExperimentalVst3DescriptionCache(const juce::File& vst3Bundle,
                                                    const std::vector<juce::PluginDescription>& descriptions)
    {
        if (descriptions.empty())
        {
            return;
        }

        auto* newBundle = new juce::XmlElement{ "bundle" };
        newBundle->setAttribute("vst3Path", vst3Bundle.getFullPathName());

        int xmlCount = 0;
        for (const auto& d : descriptions)
        {
            auto one = d.createXml();
            if (one != nullptr)
            {
                newBundle->addChildElement(one.release());
                ++xmlCount;
            }
        }

        if (xmlCount == 0)
        {
            delete newBundle;
            appendVst3OopScanLineFlushed(
                "parent: experimental-vst3-descriptions.xml not updated (PluginDescription::createXml returned "
                "nothing for all descriptions)");
            return;
        }

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

            if (root == nullptr || !root->hasTagName(experimentalDescriptionCacheRootTag()))
            {
                root = std::make_unique<juce::XmlElement>(experimentalDescriptionCacheRootTag());
                root->setAttribute("version", 1);
            }

            juce::XmlElement* walk = root->getFirstChildElement();
            while (walk != nullptr)
            {
                juce::XmlElement* const next = walk->getNextElement();
                if (walk->hasTagName("bundle")
                    && bundlePathKeyMatchesAttribute(walk->getStringAttribute("vst3Path"), vst3Bundle))
                {
                    root->removeChildElement(walk, true);
                }
                walk = next;
            }

            root->addChildElement(newBundle);

            if (!root->writeTo(cacheFile))
            {
                appendVst3OopScanLineFlushed(
                    "parent: experimental-vst3-descriptions.xml write FAILED path=\"" + cacheFile.getFullPathName()
                    + "\"");
                return;
            }

            appendVst3OopScanLineFlushed(
                "parent: experimental-vst3-descriptions.xml updated vst3Path=\"" + vst3Bundle.getFullPathName()
                + "\" pluginXmlCount=" + juce::String(xmlCount));
        }
        catch (...)
        {
            appendVst3OopScanLineFlushed(
                "parent: experimental-vst3-descriptions.xml update threw (path=\""
                + cacheFile.getFullPathName() + "\")");
        }
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
    if (root == nullptr || !root->hasTagName("MiniDAWExperimentalVst3Descriptions"))
    {
        return false;
    }

    const juce::String key = vst3Bundle.getFullPathName();
    for (juce::XmlElement* bundle = root->getFirstChildElement(); bundle != nullptr;
         bundle = bundle->getNextElement())
    {
        if (!bundle->hasTagName("bundle"))
        {
            continue;
        }
        const juce::String stored = bundle->getStringAttribute("vst3Path");
#if JUCE_WINDOWS
        if (stored.compareIgnoreCase(key) != 0)
#else
        if (stored != key)
#endif
        {
            continue;
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
        return !descriptionsOut.empty();
    }

    return false;
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

    mergeWriteExperimentalVst3DescriptionCache(vst3File, out.descriptions);

    const int runReturningMs = elapsedMsSince(scanWallT0);
    writeVst3OopScanDiagnosticLogLine("parent: run returning elapsedMs=" + juce::String(runReturningMs));
    return out;
}

} // namespace mini_daw
