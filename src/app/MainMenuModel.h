#pragma once

#include <JuceHeader.h>

#include <functional>

namespace mini_daw_app_menu
{

struct MainMenuActions
{
    std::function<void()> saveProject;
    std::function<void()> loadProject;
    std::function<void()> openAudioMixdown;
    /// P1J (steering §16.6): explicit portable packaging operation.
    std::function<void()> preparePortableProject;
    std::function<void()> openAudioSettings;
    std::function<void()> openHelp;
};

enum class MainMenuCommandId : int
{
    FileSaveProject = 1,
    FileLoadProject = 2,
    FileAudioMixdown = 3,
    FilePreparePortableProject = 4,
    AudioSettings = 10,
    HelpRoot = 20,
};

/// Standard top-level menu bar (File / Audio / Help) for the main transport shell.
class MainMenuModel final : public juce::MenuBarModel
{
public:
    explicit MainMenuModel(MainMenuActions actions);

    juce::StringArray getMenuBarNames() override;

    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex,
                                    const juce::String& menuName) override;

    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    MainMenuActions actions_;
};

} // namespace mini_daw_app_menu
