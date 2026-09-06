#include "app/MainWindow.h"

#include <JuceHeader.h>

#include "app/ShortcutDiagnostics.h"
#include "diagnostics/TransportShortcutDiagLog.h"
#include "app/TransportControlsFactory.h"
#include "app/TransportControlsShortcutTarget.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "ui/TransportShortcutKeys.h"

#include "domain/Session.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include "audio/LatencySettingsStore.h"

#include <typeinfo>

MainWindow::MainWindow(const juce::String& name,
                       Transport& transport,
                       Session& session,
                       PluginInsertHost& pluginInsertHost,
                       juce::AudioDeviceManager& deviceManager,
                       RecorderService& recorderService,
                       CountInClickOutput& countInClicks,
                       LatencySettingsStore& latencyStore,
                       PlaybackEngine& playbackEngine,
                       proxy_render::ProxyRenderScheduler& proxyRenderScheduler)
    : DocumentWindow(name,
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);

    CreatedTransportUiForMainWindow bundle = createTransportUiForMainWindow(transport,
                                                                                    session,
                                                                                    pluginInsertHost,
                                                                                    deviceManager,
                                                                                    recorderService,
                                                                                    countInClicks,
                                                                                    latencyStore,
                                                                                    playbackEngine,
                                                                                    proxyRenderScheduler);
    shortcutTargetFromContent_ = bundle.shortcutTarget;
    setContentOwned(bundle.component.release(), true);

    setResizable(true, true);
    setResizeLimits(320, 240, 10000, 10000);
    centreWithSize(640, 400);
    addKeyListener(this);
    if (juce::Component* c = getContentComponent())
    {
        c->setWantsKeyboardFocus(true);
    }
    juce::MessageManager::callAsync([this] {
        if (juce::Component* c = getContentComponent())
        {
            c->grabKeyboardFocus();
        }
    });
    setVisible(true);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] enabled");
    }
}

MainWindow::~MainWindow()
{
    removeKeyListener(this);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

bool MainWindow::tryInterceptQuitForUnsavedChanges()
{
    if (shortcutTargetFromContent_ != nullptr)
    {
        return shortcutTargetFromContent_->invokeQuitUnsavedGuardFromWindow();
    }
    return false;
}

void MainWindow::offerAutosaveRecoveryOnStartup(const bool commandLineProjectOpenQueued)
{
    if (shortcutTargetFromContent_ != nullptr)
    {
        shortcutTargetFromContent_->invokeAutosaveRecoveryCheckFromStartup(
            commandLineProjectOpenQueued);
    }
}

void MainWindow::openProjectFileFromCommandLine(const juce::File& projectFile)
{
    if (shortcutTargetFromContent_ != nullptr)
    {
        shortcutTargetFromContent_->invokeLoadProjectFileFromStartup(projectFile);
    }
}

void MainWindow::startStabilityScenario(const StabilityScenarioRequest& request)
{
    if (shortcutTargetFromContent_ != nullptr)
    {
        shortcutTargetFromContent_->invokeStartStabilityScenarioFromStartup(request);
    }
}

void MainWindow::startSpike01StateCaptureProbe(const juce::String& autoPlanId)
{
    if (shortcutTargetFromContent_ != nullptr)
    {
        shortcutTargetFromContent_->invokeStartSpike01StateCaptureProbeFromStartup(autoPlanId);
    }
}

void MainWindow::activeWindowStatusChanged()
{
    juce::DocumentWindow::activeWindowStatusChanged();
    if (isActiveWindow())
    {
        juce::MessageManager::callAsync([this] {
            if (juce::Component* c = getContentComponent())
            {
                c->grabKeyboardFocus();
            }
        });
    }
}

bool MainWindow::keyPressed(const juce::KeyPress& key, juce::Component* originating)
{
    juce::ignoreUnused(originating);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] MainWindow::keyPressed desc=\""
                                   + key.getTextDescription() + "\"");
    }
    if (shortcut_diagnostics::kShowKeyDiagnostic)
    {
        if (shortcutTargetFromContent_ != nullptr)
        {
            shortcutTargetFromContent_->setKeyDiagnosticLine(
                juce::String{ "0x" } + juce::String::toHexString((juce::uint32)key.getKeyCode())
                + " ch=0x" + juce::String::toHexString((juce::uint32)key.getTextCharacter()) + " "
                + key.getTextDescription());
        }
    }
    return routeShortcut(key);
}

bool MainWindow::routeShortcut(const juce::KeyPress& key)
{
    shortcut_diagnostics::logShortcutRouterKey(key);
    if constexpr (transport_shortcut_diag::kEnabled)
    {
        const juce::Component* focused = juce::Component::getCurrentlyFocusedComponent();
        transport_shortcut_diag::appendLine(
            "router key=0x" + juce::String::toHexString((juce::uint32)key.getKeyCode())
            + " ch=0x" + juce::String::toHexString((juce::uint32)key.getTextCharacter())
            + " desc=\"" + key.getTextDescription() + "\" focus="
            + (focused != nullptr ? juce::String(typeid(*focused).name()) : juce::String("none"))
            + " matchJumpL="
            + (midi_transport_shortcuts::isJumpToLeftLocatorShortcut(key) ? "Y" : "n"));
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        const bool cmd = key.getModifiers().isCommandDown();
        const bool z = (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
        const bool y = (key.getKeyCode() == 'y' || key.getKeyCode() == 'Y');
        const bool undoCombo = cmd && !key.getModifiers().isShiftDown() && z;
        const bool redoCombo = cmd && (y || (key.getModifiers().isShiftDown() && z));
        if (undoCombo || redoCombo)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] routeShortcut entered undoRelated desc=\"" + key.getTextDescription()
                + "\" undoCombo=" + juce::String(undoCombo ? "Y" : "n") + " redoCombo="
                + juce::String(redoCombo ? "Y" : "n") + ")");
        }
    }
    if constexpr (shortcut_diagnostics::kShowShortcutDiagnostics)
    {
        if (shortcutTargetFromContent_ != nullptr)
        {
            shortcutTargetFromContent_->setShortcutDiagVisibleCaption(
                shortcut_diagnostics::makeShortcutDiagVisibleCaption(key));
        }
    }

    const bool editorHasFocus = (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())
                                != nullptr);
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        const bool undoShortcut = key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
                                  && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
        const bool redoShortcut
            = key.getModifiers().isCommandDown()
              && ((key.getKeyCode() == 'y' || key.getKeyCode() == 'Y')
                  || ((key.getKeyCode() == 'z' || key.getKeyCode() == 'Z')
                      && key.getModifiers().isShiftDown()));
        if (editorHasFocus && (undoShortcut || redoShortcut))
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] routeShortcut blocked: TextEditor focus undoShortcut="
                + juce::String(undoShortcut ? "Y" : "n") + " redoShortcut="
                + juce::String(redoShortcut ? "Y" : "n") + " desc=\"" + key.getTextDescription()
                + "\"");
        }
    }
    if (!editorHasFocus)
    {
        if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
            && (key.getKeyCode() == 's' || key.getKeyCode() == 'S'))
        {
            if (shortcutTargetFromContent_ != nullptr)
            {
                shortcutTargetFromContent_->invokeSaveProjectFromWindowShortcut();
                juce::Logger::writeToLog("[Shortcut] Ctrl+S save project");
                return true;
            }
        }
        if (key.isKeyCode(juce::KeyPress::deleteKey))
        {
            if (shortcutTargetFromContent_ != nullptr)
            {
                shortcutTargetFromContent_->invokeDeleteSelectedPlacedClipFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'c' || key.getKeyCode() == 'C'))
        {
            if (shortcutTargetFromContent_ != nullptr)
            {
                shortcutTargetFromContent_->invokeCopySelectedClipFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'v' || key.getKeyCode() == 'V'))
        {
            if (shortcutTargetFromContent_ != nullptr)
            {
                shortcutTargetFromContent_->invokePasteClipFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
            && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z'))
        {
            if (shortcutTargetFromContent_ != nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] routeShortcut matched=undo desc=\""
                                               + key.getTextDescription() + "\"");
                }
                shortcutTargetFromContent_->invokeUndoFromWindowShortcut();
                return true;
            }
        }
        if (key.getModifiers().isCommandDown()
            && ((key.getKeyCode() == 'y' || key.getKeyCode() == 'Y')
                || ((key.getKeyCode() == 'z' || key.getKeyCode() == 'Z')
                    && key.getModifiers().isShiftDown())))
        {
            if (shortcutTargetFromContent_ != nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] routeShortcut matched=redo desc=\""
                                               + key.getTextDescription() + "\"");
                }
                shortcutTargetFromContent_->invokeRedoFromWindowShortcut();
                return true;
            }
        }
    }

    if (midi_transport_shortcuts::isRecordToggleShortcut(key))
    {
        if (shortcutTargetFromContent_ != nullptr)
        {
            shortcutTargetFromContent_->invokeRecordToggleFromWindowShortcut();
            juce::Logger::writeToLog(juce::String{ "[Shortcut] record toggle: " } + key.getTextDescription());
            return true;
        }
        return false;
    }
    if (midi_transport_shortcuts::isJumpToLeftLocatorShortcut(key))
    {
        if (shortcutTargetFromContent_ != nullptr)
        {
            shortcutTargetFromContent_->invokeJumpToLeftLocatorFromWindowShortcut();
            juce::Logger::writeToLog(juce::String{
                "[Shortcut] jump to left locator (numpad1 / top-row "
                "1 / VK): " }
                                     + key.getTextDescription());
            return true;
        }
        return false;
    }
    if (midi_transport_shortcuts::isSpacePlayPauseShortcut(key))
    {
        if (shortcutTargetFromContent_ != nullptr)
        {
            shortcutTargetFromContent_->invokePlayPauseToggleFromWindowShortcut();
            juce::Logger::writeToLog(juce::String{ "[Shortcut] play/pause: " } + key.getTextDescription());
            return true;
        }
        return false;
    }
    return false;
}

[[nodiscard]] std::unique_ptr<MainWindow> createMainWindow(const juce::String& name,
                                             Transport& transport,
                                             Session& session,
                                             PluginInsertHost& pluginInsertHost,
                                             juce::AudioDeviceManager& deviceManager,
                                             RecorderService& recorderService,
                                             CountInClickOutput& countInClicks,
                                             LatencySettingsStore& latencyStore,
                                             PlaybackEngine& playbackEngine,
                                             proxy_render::ProxyRenderScheduler& proxyRenderScheduler)
{
    return std::make_unique<MainWindow>(
        name, transport, session, pluginInsertHost, deviceManager, recorderService, countInClicks,
        latencyStore, playbackEngine, proxyRenderScheduler);
}
