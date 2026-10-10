// Read-ahead on/off choice for 1.3.1. Isolated files only — never the user's APPDATA.
// Also opens the real checkbox section in a native window and clicks it.

#include <JuceHeader.h>

#include <cstdio>

#include "engine/ReadAheadStartupConfig.h"
#include "ui/ReadAheadPreferenceSection.h"

namespace
{
int checks = 0;
int failures = 0;

void expect(const bool ok, const char* what)
{
    ++checks;
    if (ok)
    {
        std::printf("[ ok ] %s\n", what);
    }
    else
    {
        ++failures;
        std::printf("[FAIL] %s\n", what);
    }
}

juce::File tempXml(const char* name)
{
    return juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("MiniDAWLab-readahead-tests")
        .getChildFile(name);
}

void testMissingFileIsOn()
{
    const juce::File file = tempXml("missing.xml");
    file.deleteFile();
    const auto cfg = readahead::resolveReadAheadProcessConfig({}, file);
    expect(cfg.activeDepth == readahead::kDefaultReadAheadDepth, "missing file: depth 3");
    expect(!cfg.commandLineOverride, "missing file: not a start argument");
    expect(cfg.nextStartEnabled, "missing file: next start on");
    expect(!cfg.hasSavedChoice, "missing file: no saved choice");
    expect(!file.existsAsFile(), "missing file: resolve does not create a preference");
}

void testSavedOffStaysOff()
{
    const juce::File file = tempXml("saved-off.xml");
    expect(readahead::saveReadAheadEnabledPreference(file, false), "save off");
    const auto cfg = readahead::resolveReadAheadProcessConfig({}, file);
    expect(cfg.activeDepth == 0, "saved off: depth 0");
    expect(cfg.hasSavedChoice && !cfg.nextStartEnabled, "saved off: checkbox off");
    const auto again = readahead::loadReadAheadEnabledPreference(file);
    expect(again.has_value() && !(*again), "saved off: reload still off");
}

void testSavedOnIsOn()
{
    const juce::File file = tempXml("saved-on.xml");
    expect(readahead::saveReadAheadEnabledPreference(file, true), "save on");
    const auto cfg = readahead::resolveReadAheadProcessConfig({}, file);
    expect(cfg.activeDepth == readahead::kDefaultReadAheadDepth, "saved on: depth 3");
    expect(cfg.hasSavedChoice && cfg.nextStartEnabled, "saved on: checkbox on");
}

void testUnrecognisedFileIsDefaultOn()
{
    const juce::File file = tempXml("garbage.xml");
    file.getParentDirectory().createDirectory();
    file.replaceWithText("<READAHEAD enabled=\"maybe\"/>");
    const auto cfg = readahead::resolveReadAheadProcessConfig({}, file);
    expect(cfg.activeDepth == readahead::kDefaultReadAheadDepth, "unrecognised value: depth 3");
    expect(!cfg.hasSavedChoice, "unrecognised value: not a saved choice");
}

void testCliDoesNotRewriteTheFile()
{
    const juce::File file = tempXml("cli-keeps.xml");
    expect(readahead::saveReadAheadEnabledPreference(file, true), "cli fixture save on");
    const juce::String before = file.loadFileAsString();
    juce::StringArray off;
    off.add("--no-readahead");
    const auto forcedOff = readahead::resolveReadAheadProcessConfig(off, file);
    expect(forcedOff.activeDepth == 0 && forcedOff.commandLineOverride, "--no-readahead forces off");
    expect(forcedOff.nextStartEnabled, "--no-readahead leaves the saved On choice");
    expect(file.loadFileAsString() == before, "--no-readahead does not rewrite the file");

    expect(readahead::saveReadAheadEnabledPreference(file, false), "cli fixture save off");
    const juce::String beforeOff = file.loadFileAsString();
    juce::StringArray on;
    on.add("--experimental-readahead");
    const auto forcedOn = readahead::resolveReadAheadProcessConfig(on, file);
    expect(forcedOn.activeDepth == readahead::kDefaultReadAheadDepth && forcedOn.commandLineOverride,
           "--experimental-readahead forces on at depth 3");
    expect(!forcedOn.nextStartEnabled, "CLI on leaves the saved Off choice");
    expect(file.loadFileAsString() == beforeOff, "CLI on does not rewrite the file");

    juce::StringArray depth;
    depth.add("--experimental-readahead=5");
    const auto five = readahead::resolveReadAheadProcessConfig(depth, file);
    expect(five.activeDepth == 5, "--experimental-readahead=5 keeps the explicit depth");
}

void testLastArgumentWins()
{
    const juce::File file = tempXml("unused-for-cli.xml");
    file.deleteFile();
    juce::StringArray offThenOn;
    offThenOn.add("--no-readahead");
    offThenOn.add("--experimental-readahead=4");
    const auto on = readahead::resolveReadAheadProcessConfig(offThenOn, file);
    expect(on.conflictingArguments && on.commandLineOverride && on.activeDepth == 4,
           "last argument wins: depth 4");

    juce::StringArray onThenOff;
    onThenOff.add("--experimental-readahead=6");
    onThenOff.add("--no-readahead");
    const auto off = readahead::resolveReadAheadProcessConfig(onThenOff, file);
    expect(off.conflictingArguments && off.activeDepth == 0, "last argument wins: --no-readahead");
}

void testCheckboxInANativeWindow()
{
    const juce::File file = tempXml("dialog-section.xml");
    file.deleteFile();
    auto* section = new ReadAheadPreferenceSection(file, /*activeThisSession*/ true, /*commandLineOverride*/ false);
    juce::DocumentWindow window("Audio Settings", juce::Colour(0xff2a2a2a), juce::DocumentWindow::closeButton);
    window.setUsingNativeTitleBar(true);
    window.setContentOwned(section, true);
    window.centreWithSize(640, ReadAheadPreferenceSection::kPreferredHeightPx + 32);
    window.setVisible(true);
    section->resized();

    expect(section->isShowing(), "section is showing in a native window");
    expect(section->enableToggle().isShowing(), "checkbox is showing");
    expect(section->enableToggle().getButtonText() == "Enable read-ahead processing", "checkbox text");
    expect(section->enableToggle().getToggleState(), "no saved file: checkbox starts on");
    expect(section->activeSessionText() == "Active this session: On", "active label matches the session");
    expect(section->noticeText().isEmpty(), "matching on/on: no restart notice");

    section->enableToggle().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    expect(!section->enableToggle().getToggleState(), "click turns the checkbox off");
    expect(section->activeSessionText() == "Active this session: On", "click does not change the active session");
    expect(section->noticeText() == "Restart required", "saved off while session on: restart required");
    const auto saved = readahead::loadReadAheadEnabledPreference(file);
    expect(saved.has_value() && !(*saved), "click wrote enabled=0");

    auto* cliSection = new ReadAheadPreferenceSection(file, /*activeThisSession*/ false, /*commandLineOverride*/ true);
    juce::DocumentWindow cliWindow("Audio Settings", juce::Colour(0xff2a2a2a), juce::DocumentWindow::closeButton);
    cliWindow.setUsingNativeTitleBar(true);
    cliWindow.setContentOwned(cliSection, true);
    cliWindow.centreWithSize(640, ReadAheadPreferenceSection::kPreferredHeightPx + 32);
    cliWindow.setVisible(true);
    expect(cliSection->activeSessionText() == "Active this session: Off", "CLI off: active label is Off");
    expect(!cliSection->enableToggle().getToggleState(), "CLI off still shows the saved Off checkbox");
    expect(cliSection->noticeText() == "Controlled by start argument.",
           "CLI matching off/off: start argument, no restart");

    cliSection->enableToggle().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    expect(cliSection->activeSessionText() == "Active this session: Off", "CLI click does not turn the session on");
    expect(cliSection->noticeText().contains("Restart required"), "CLI mismatch: restart required");
    expect(cliSection->noticeText().contains("Controlled by start argument."), "CLI mismatch: still names the argument");
    const auto after = readahead::loadReadAheadEnabledPreference(file);
    expect(after.has_value() && *after, "CLI click saved On without being the active session");

    window.setVisible(false);
    cliWindow.setVisible(false);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("ReadAheadStartupFocusedTests\n");
    testMissingFileIsOn();
    testSavedOffStaysOff();
    testSavedOnIsOn();
    testUnrecognisedFileIsDefaultOn();
    testCliDoesNotRewriteTheFile();
    testLastArgumentWins();
    testCheckboxInANativeWindow();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
