#include "app/MainMenuModel.h"

namespace mini_daw_app_menu
{

MainMenuModel::MainMenuModel(MainMenuActions actions)
    : actions_(std::move(actions))
{
}

juce::StringArray MainMenuModel::getMenuBarNames()
{
    return { "File", "Audio", "Help" };
}

juce::PopupMenu MainMenuModel::getMenuForIndex(const int topLevelMenuIndex,
                                               const juce::String& menuName)
{
    juce::ignoreUnused(menuName);

    switch (topLevelMenuIndex)
    {
    case 0: {
        juce::PopupMenu m;
        m.addItem(static_cast<int>(MainMenuCommandId::FileSaveProject), "Save Project...");
        m.addItem(static_cast<int>(MainMenuCommandId::FileLoadProject), "Load Project...");
        m.addSeparator();
        m.addItem(static_cast<int>(MainMenuCommandId::FileAudioMixdown), "Audio Mixdown...");
        return m;
    }
    case 1: {
        juce::PopupMenu m;
        m.addItem(static_cast<int>(MainMenuCommandId::AudioSettings), "Audio Settings...");
        return m;
    }
    case 2: {
        juce::PopupMenu m;
        m.addItem(static_cast<int>(MainMenuCommandId::HelpRoot), "Help...");
        return m;
    }
    default:
        return {};
    }
}

void MainMenuModel::menuItemSelected(const int menuItemID, const int topLevelMenuIndex)
{
    juce::ignoreUnused(topLevelMenuIndex);

    switch (menuItemID)
    {
    case static_cast<int>(MainMenuCommandId::FileSaveProject):
        if (actions_.saveProject != nullptr)
        {
            actions_.saveProject();
        }
        return;
    case static_cast<int>(MainMenuCommandId::FileLoadProject):
        if (actions_.loadProject != nullptr)
        {
            actions_.loadProject();
        }
        return;
    case static_cast<int>(MainMenuCommandId::FileAudioMixdown):
        if (actions_.openAudioMixdown != nullptr)
        {
            actions_.openAudioMixdown();
        }
        return;
    case static_cast<int>(MainMenuCommandId::AudioSettings):
        if (actions_.openAudioSettings != nullptr)
        {
            actions_.openAudioSettings();
        }
        return;
    case static_cast<int>(MainMenuCommandId::HelpRoot):
        if (actions_.openHelp != nullptr)
        {
            actions_.openHelp();
        }
        return;
    default:
        return;
    }
}

} // namespace mini_daw_app_menu
