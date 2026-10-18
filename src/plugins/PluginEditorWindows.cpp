// =============================================================================
// PluginEditorWindows.cpp — native + generic plugin editor shells (message thread)
// =============================================================================

#include "plugins/PluginEditorWindows.h"

#include "plugins/PluginInsertHost.h"
#include "ui/TrackStripButtonGlyphs.h"

namespace
{
    constexpr int kToolbarH = 28;
    constexpr int kButton = 22;

    [[nodiscard]] int presetButtonWidth(const juce::String& text)
    {
        const juce::Font font(juce::FontOptions(12.0f));
        return font.getStringWidth(text) + 18;
    }

    [[nodiscard]] int presetToolbarMinWidth()
    {
        return 8 + kButton + 6 + kButton + 8 + presetButtonWidth("Load preset...") + 6
               + presetButtonWidth("Save preset...") + 8;
    }

    [[nodiscard]] bool dispatchHostUndoRedoFromPluginWindowKey(const juce::KeyPress& key,
                                                               const PluginEditorWindowHostShortcuts& sh)
    {
        if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        {
            return false;
        }

        if (!key.getModifiers().isCommandDown())
        {
            return false;
        }

        const int kc = key.getKeyCode();
        if (!key.getModifiers().isShiftDown() && (kc == 'z' || kc == 'Z'))
        {
            if (sh.requestUndo)
            {
                sh.requestUndo();
                return true;
            }
            return false;
        }
        if ((kc == 'y' || kc == 'Y')
            || (key.getModifiers().isShiftDown() && (kc == 'z' || kc == 'Z')))
        {
            if (sh.requestRedo)
            {
                sh.requestRedo();
                return true;
            }
            return false;
        }
        return false;
    }

    /// Host Active / Bypass bar. The plug-in editor is a child below it and keeps its own size.
    class InsertEditorShell final : public juce::Component,
                                    public juce::SettableTooltipClient,
                                    private juce::ComponentListener,
                                    private juce::Timer
    {
    public:
        InsertEditorShell(PluginInsertHost& host,
                          const TrackId trackId,
                          const InsertSlotId slotId,
                          std::unique_ptr<juce::AudioProcessorEditor> editor)
            : host_(host)
            , trackId_(trackId)
            , slotId_(slotId)
            , editor_(std::move(editor))
        {
            jassert(editor_ != nullptr);
            editorResizable_ = editor_->isResizable();
            addAndMakeVisible(editor_.get());
            editor_->addComponentListener(this);
            // juce::String(const char*) is ASCII (CharPointer_ASCII). U+2026 in a
            // narrow literal is three bytes above 127 and paints as broken glyphs.
            configurePresetButton(loadButton_,
                                  "Load preset...",
                                  "Load a .vstpreset into this insert. The plug-in's own state is replaced. Host Active and Bypass stay as they are.");
            configurePresetButton(saveButton_,
                                  "Save preset...",
                                  "Save this insert's plug-in state as a .vstpreset. The project is not modified. Host Active and Bypass are not stored in the file.");
            loadButton_.onClick = [this] { beginLoadPreset(); };
            saveButton_.onClick = [this] { beginSavePreset(); };
            addAndMakeVisible(loadButton_);
            addAndMakeVisible(saveButton_);
            const int w = juce::jmax(presetToolbarMinWidth(), editor_->getWidth());
            const int h = juce::jmax(40, editor_->getHeight());
            setSize(w, h + kToolbarH);
            refreshPresetButtons();
            startTimerHz(15);
        }

        ~InsertEditorShell() override
        {
            if (editor_ != nullptr)
            {
                editor_->removeComponentListener(this);
            }
        }

        void paint(juce::Graphics& g) override
        {
            g.setColour(juce::Colour(0xff26292d));
            g.fillRect(0, 0, getWidth(), kToolbarH);
            g.setColour(juce::Colour(0xff3a3d42));
            g.drawHorizontalLine(kToolbarH - 1, 0.0f, static_cast<float>(getWidth()));

            const auto audible = host_.insertHostAudible(trackId_, slotId_);
            paintButton(g, activeBounds(), true, audible.active, audible.active, audible.available);
            const bool bypassShown = audible.bypassed;
            const bool bypassEnabled = audible.available && audible.active;
            paintButton(g, bypassBounds(), false, bypassShown, bypassEnabled, audible.available);
        }

        void resized() override
        {
            if (guard_ || editor_ == nullptr)
            {
                return;
            }
            const juce::ScopedValueSetter<bool> guard(guard_, true);
            const int y = (kToolbarH - kButton) / 2;
            const int loadW = presetButtonWidth(loadButton_.getButtonText());
            const int saveW = presetButtonWidth(saveButton_.getButtonText());
            const int x = 8 + kButton + 6 + kButton + 8;
            loadButton_.setBounds(x, y, loadW, kButton);
            saveButton_.setBounds(x + loadW + 6, y, saveW, kButton);
            if (editorResizable_)
            {
                editor_->setBounds(0, kToolbarH, getWidth(), juce::jmax(1, getHeight() - kToolbarH));
            }
            else if (editor_->getX() != 0 || editor_->getY() != kToolbarH)
            {
                editor_->setTopLeftPosition(0, kToolbarH);
            }
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            if (activeBounds().contains(e.getPosition()))
            {
                setTooltip("Active. Off sends this insert's input onward and stops processing it after a short fade. The plug-in stays loaded.");
            }
            else if (bypassBounds().contains(e.getPosition()))
            {
                setTooltip("Bypass. You hear the signal just before this insert while the effect keeps processing. Disabled while Active is off; the Bypass choice is kept.");
            }
            else
            {
                setTooltip({});
            }
        }

        void mouseUp(const juce::MouseEvent& e) override
        {
            const auto audible = host_.insertHostAudible(trackId_, slotId_);
            if (!audible.available || !e.mouseWasClicked())
            {
                return;
            }
            if (activeBounds().contains(e.getPosition()))
            {
                host_.setInsertHostActive(trackId_, slotId_, !audible.active);
                repaint();
                return;
            }
            if (audible.active && bypassBounds().contains(e.getPosition()))
            {
                host_.setInsertHostBypassed(trackId_, slotId_, !audible.bypassed);
                repaint();
            }
        }

    private:
        void timerCallback() override
        {
            refreshPresetButtons();
            repaint();
        }

        void componentMovedOrResized(juce::Component& component, bool, bool wasResized) override
        {
            if (&component != editor_.get() || !wasResized || guard_)
            {
                return;
            }
            const juce::ScopedValueSetter<bool> guard(guard_, true);
            const int w = juce::jmax(presetToolbarMinWidth(), component.getWidth());
            const int h = component.getHeight() + kToolbarH;
            if (getWidth() != w || getHeight() != h)
            {
                setSize(w, h);
            }
            if (component.getX() != 0 || component.getY() != kToolbarH)
            {
                component.setTopLeftPosition(0, kToolbarH);
            }
        }

        [[nodiscard]] juce::Rectangle<int> activeBounds() const noexcept
        {
            return { 8, (kToolbarH - kButton) / 2, kButton, kButton };
        }

        [[nodiscard]] juce::Rectangle<int> bypassBounds() const noexcept
        {
            return { 8 + kButton + 6, (kToolbarH - kButton) / 2, kButton, kButton };
        }

        void paintButton(juce::Graphics& g,
                         juce::Rectangle<int> bounds,
                         const bool power,
                         const bool on,
                         const bool enabled,
                         const bool available) const
        {
            juce::Colour fill = juce::Colour(track_strip_glyphs::kNeutralFaceArgb);
            juce::Colour edge = juce::Colour(track_strip_glyphs::kCtlNeutralEdgeArgb);
            if (!available || !enabled)
            {
                fill = juce::Colour(track_strip_glyphs::kDisabledFaceArgb);
            }
            else if (power && on)
            {
                fill = juce::Colour(track_strip_glyphs::kPowerOnArgb);
            }
            else if (!power && on)
            {
                fill = juce::Colour(track_strip_glyphs::kMuteOnArgb);
            }
            // Active off keeps the Bypass choice visible: the button stays disabled, with an amber edge.
            if (!power && on)
            {
                edge = juce::Colour(track_strip_glyphs::kMuteOnArgb);
            }
            const auto body = bounds.toFloat();
            track_strip_glyphs::drawStandardStripButtonFace(g, body, fill, edge, false);
            const auto glyph = track_strip_glyphs::nonLetterGlyphAreaFromSquareBodyPx(bounds);
            const auto colour = juce::Colour((!available || !enabled) ? track_strip_glyphs::kGlyphDisabledArgb
                                                                      : track_strip_glyphs::kGlyphLightArgb);
            if (power)
            {
                track_strip_glyphs::drawPowerGlyphInSquare(g, glyph, colour);
            }
            else
            {
                track_strip_glyphs::drawBypassGlyphInSquare(g, glyph, colour);
            }
        }

        static void configurePresetButton(juce::TextButton& button, const juce::String& name, const juce::String& tip)
        {
            button.setButtonText(name);
            button.setName(name);
            button.setTitle(name);
            button.setDescription(tip);
            button.setTooltip(tip);
            button.setWantsKeyboardFocus(true);
            button.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff3a3d42));
            button.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff4a4e54));
            button.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe8e8e8));
            button.setColour(juce::TextButton::textColourOnId, juce::Colour(0xffe8e8e8));
        }

        void refreshPresetButtons()
        {
            const auto audible = host_.insertHostAudible(trackId_, slotId_);
            const bool enabled = audible.available && !dialogBusy_;
            loadButton_.setEnabled(enabled);
            saveButton_.setEnabled(enabled);
        }

        void beginLoadPreset()
        {
            launchPresetDialog(false);
        }

        void beginSavePreset()
        {
            launchPresetDialog(true);
        }

        void launchPresetDialog(const bool save)
        {
            if (dialogBusy_)
            {
                return;
            }
            const auto token = host_.insertInstanceToken(trackId_, slotId_);
            if (!token.namesALiveInstance() || !host_.insertHostAudible(trackId_, slotId_).available)
            {
                return;
            }
            dialogBusy_ = true;
            refreshPresetButtons();
            const juce::Component::SafePointer<InsertEditorShell> safe(this);
            launchInsertPresetFileChooser(host_, trackId_, slotId_, token.instanceEpoch, save, [safe] {
                if (safe != nullptr)
                {
                    safe->dialogBusy_ = false;
                    safe->refreshPresetButtons();
                    safe->repaint();
                }
            });
        }

        PluginInsertHost& host_;
        TrackId trackId_ = kInvalidTrackId;
        InsertSlotId slotId_ = kInvalidInsertSlotId;
        std::unique_ptr<juce::AudioProcessorEditor> editor_;
        juce::TextButton loadButton_;
        juce::TextButton saveButton_;
        bool editorResizable_ = false;
        bool guard_ = false;
        bool dialogBusy_ = false;
    };

    void installShell(juce::DocumentWindow& window,
                      PluginInsertHost& host,
                      const TrackId trackId,
                      const InsertSlotId slotId,
                      std::unique_ptr<juce::AudioProcessorEditor> editor)
    {
        const bool resizable = editor != nullptr && editor->isResizable();
        auto* shell = new InsertEditorShell(host, trackId, slotId, std::move(editor));
        window.setContentOwned(shell, true);
        window.setResizable(resizable, false);
        shell->setMouseCursor(juce::MouseCursor::NormalCursor);
    }
} // namespace

void launchInsertPresetFileChooser(PluginInsertHost& host,
                                   const TrackId trackId,
                                   const InsertSlotId slotId,
                                   const std::uint64_t instanceEpoch,
                                   const bool save,
                                   std::function<void()> onFinished)
{
    PluginInsertHost::InsertInstanceToken token;
    token.trackId = trackId;
    token.slotId = slotId;
    token.instanceEpoch = instanceEpoch;
    if (!token.namesALiveInstance())
    {
        if (onFinished)
        {
            onFinished();
        }
        return;
    }
    const int flags = save ? (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting
                              | juce::FileBrowserComponent::canSelectFiles)
                           : (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles);
    const juce::File start = save ? juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                       .getChildFile("Preset.vstpreset")
                                 : juce::File{};
    const auto chooser = std::make_shared<juce::FileChooser>(save ? "Save preset" : "Load preset", start, "*.vstpreset");
    chooser->launchAsync(flags, [token, save, chooser, host = &host, onFinished](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
        juce::File file = fc.getResult();
        if (file != juce::File{})
        {
            if (save && !file.hasFileExtension("vstpreset"))
            {
                const juce::File withExt = file.withFileExtension("vstpreset");
                if (withExt.existsAsFile())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Save preset",
                        "A preset with that name already exists. Choose it in the dialog to replace it.");
                    file = juce::File{};
                }
                else
                {
                    file = withExt;
                }
            }
            if (file != juce::File{})
            {
                const juce::Result result = save ? host->saveInsertPresetToFile(token, file)
                                                 : host->loadInsertPresetFromFile(token, file);
                if (!result.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                          save ? "Save preset" : "Load preset",
                                                          result.getErrorMessage());
                }
            }
        }
        if (onFinished)
        {
            onFinished();
        }
    });
}

PluginEditorWindow::PluginEditorWindow(PluginInsertHost& host,
                                       const TrackId trackId,
                                       const InsertSlotId insertSlotId,
                                       juce::AudioProcessor& proc,
                                       std::unique_ptr<juce::AudioProcessorEditor> editor,
                                       PluginEditorWindowHostShortcuts hostShortcuts)
    : DocumentWindow(proc.getName().isNotEmpty() ? proc.getName() : "Plugin",
                     juce::Colour(0xff26292d),
                     DocumentWindow::closeButton)
    , host_(host)
    , trackId_(trackId)
    , insertSlotId_(insertSlotId)
    , hostShortcuts_(std::move(hostShortcuts))
{
    juce::ignoreUnused(proc);
    setUsingNativeTitleBar(true);
    installShell(*this, host, trackId, insertSlotId, std::move(editor));
    setVisible(true);
}

bool PluginEditorWindow::keyPressed(const juce::KeyPress& key)
{
    if (dispatchHostUndoRedoFromPluginWindowKey(key, hostShortcuts_))
    {
        return true;
    }
    return DocumentWindow::keyPressed(key);
}

void PluginEditorWindow::closeButtonPressed()
{
    setVisible(false);
    PluginInsertHost* const host = &host_;
    const TrackId trackId = trackId_;
    const InsertSlotId slotId = insertSlotId_;
    juce::MessageManager::callAsync([host, trackId, slotId] {
        if (host != nullptr)
        {
            host->editorWindowClosing(trackId, slotId, false);
        }
    });
}

PluginParamsWindow::PluginParamsWindow(PluginInsertHost& host,
                                       const TrackId trackId,
                                       const InsertSlotId insertSlotId,
                                       juce::AudioProcessor& proc,
                                       std::unique_ptr<juce::AudioProcessorEditor> editor,
                                       PluginEditorWindowHostShortcuts hostShortcuts)
    : DocumentWindow((proc.getName().isNotEmpty() ? proc.getName() : "Plugin") + " — parameters",
                     juce::Colour(0xff26292d),
                     DocumentWindow::closeButton)
    , host_(host)
    , trackId_(trackId)
    , insertSlotId_(insertSlotId)
    , hostShortcuts_(std::move(hostShortcuts))
{
    juce::ignoreUnused(proc);
    setUsingNativeTitleBar(true);
    installShell(*this, host, trackId, insertSlotId, std::move(editor));
    setVisible(true);
}

bool PluginParamsWindow::keyPressed(const juce::KeyPress& key)
{
    if (dispatchHostUndoRedoFromPluginWindowKey(key, hostShortcuts_))
    {
        return true;
    }
    return DocumentWindow::keyPressed(key);
}

void PluginParamsWindow::closeButtonPressed()
{
    setVisible(false);
    PluginInsertHost* const host = &host_;
    const TrackId trackId = trackId_;
    const InsertSlotId slotId = insertSlotId_;
    juce::MessageManager::callAsync([host, trackId, slotId] {
        if (host != nullptr)
        {
            host->editorWindowClosing(trackId, slotId, true);
        }
    });
}
