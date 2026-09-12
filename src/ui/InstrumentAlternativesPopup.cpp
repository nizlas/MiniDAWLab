// =============================================================================
// InstrumentAlternativesPopup.cpp — track-header callout (see header)
// =============================================================================

#include "ui/InstrumentAlternativesPopup.h"

#include <memory>
#include <utility>

namespace
{
    constexpr int kPopupWidthPx = 300;
    constexpr int kRowH = 18;
    constexpr int kComboH = 24;
    constexpr int kButtonH = 22;
    constexpr int kCaptionW = 60;

    /// Inner panel holding every control; the outer content wraps it in a viewport so the callout
    /// can always fit on screen and scroll internally when it cannot.
    class AlternativesPanel final : public juce::Component,
                                    private juce::Timer
    {
    public:
        AlternativesPanel(const TrackId tid,
                          InstrumentProxyUiHost proxyHost,
                          InstrumentSecondaryUiHost secondaryHost)
            : tid_(tid)
            , proxyHost_(std::move(proxyHost))
            , secondaryHost_(std::move(secondaryHost))
        {
            const auto initCaption = [this](juce::Label& l, const char* text) {
                l.setText(text, juce::dontSendNotification);
                l.setFont(juce::FontOptions(10.0f));
                l.setColour(juce::Label::textColourId, juce::Colours::grey);
                addAndMakeVisible(l);
            };
            const auto initValue = [this](juce::Label& l) {
                l.setFont(juce::FontOptions(12.0f));
                l.setJustificationType(juce::Justification::centredLeft);
                addAndMakeVisible(l);
            };

            titleLabel_.setText("Instrument alternatives", juce::dontSendNotification);
            titleLabel_.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
            addAndMakeVisible(titleLabel_);

            initCaption(primaryCaptionLabel_, "Primary");
            initValue(primaryValueLabel_);
            initCaption(secondaryCaptionLabel_, "Secondary");
            initValue(secondaryValueLabel_);

            const juce::String altTooltip
                = "Optional Secondary instrument: a locally available working sound used only when "
                  "the Primary cannot load AND no current proxy is playable. It never replaces the "
                  "Primary configuration or the proxy files, and is never presented as sounding "
                  "identical.\n\n"
                  "With the transport stopped and the Primary missing, played notes audition "
                  "through the Secondary. While a current proxy supplies transport playback, live "
                  "audition is unavailable (the Secondary is never layered on top of the proxy).";
            primaryValueLabel_.setTooltip(altTooltip);
            secondaryValueLabel_.setTooltip(altTooltip);

            selectSecondaryButton_.setButtonText("Select...");
            selectSecondaryButton_.setTooltip(
                "Choose a Secondary instrument from the instrument catalogue. The Primary "
                "configuration is kept untouched.");
            selectSecondaryButton_.setWantsKeyboardFocus(false);
            selectSecondaryButton_.onClick = [this] { showSecondaryCatalogPickerMenu(); };
            addAndMakeVisible(selectSecondaryButton_);

            editorButton_.setButtonText("Editor");
            editorButton_.setTooltip("Open the Secondary instrument's editor (loads it if needed).");
            editorButton_.setWantsKeyboardFocus(false);
            editorButton_.onClick = [this] {
                if (!secondaryHost_.openSecondaryEditor)
                {
                    return;
                }
                // Discreet preparing state: paint "Loading..." first, then perform the
                // (potentially slow, synchronous) load+open on the next message-loop turn.
                secondaryValueLabel_.setText("Loading...", juce::dontSendNotification);
                juce::Component::SafePointer<AlternativesPanel> safe(this);
                juce::Timer::callAfterDelay(30, [safe] {
                    if (safe == nullptr)
                    {
                        return;
                    }
                    safe->secondaryHost_.openSecondaryEditor(safe->tid_);
                    safe->syncFromHosts();
                });
            };
            addAndMakeVisible(editorButton_);

            removeButton_.setButtonText("Remove");
            removeButton_.setTooltip(
                "Remove the Secondary assignment. The Primary and any proxy files are untouched.");
            removeButton_.setWantsKeyboardFocus(false);
            removeButton_.onClick = [this] {
                if (secondaryHost_.removeSecondary)
                {
                    secondaryHost_.removeSecondary(tid_);
                    syncFromHosts();
                }
            };
            addAndMakeVisible(removeButton_);

            initCaption(channelCaptionLabel_, "MIDI to Secondary");
            // Item ids: 1 = Preserve channels (default); 2..17 = Force channel 1..16. Applied ONLY
            // to Secondary delivery — stored notes, Primary channels and MIDI export are never
            // rewritten.
            channelComboBox_.addItem("Preserve channels", 1);
            for (int ch = 1; ch <= 16; ++ch)
            {
                channelComboBox_.addItem("Force channel " + juce::String(ch), ch + 1);
            }
            channelComboBox_.setTooltip(
                "How routed MIDI reaches the Secondary. Preserve channels (default) delivers "
                "events on their original channels; Force channel N moves every event to one "
                "channel — useful when a replacement instrument listens on a single channel. Only "
                "Secondary delivery is affected; stored notes and the Primary configuration are "
                "never rewritten.");
            channelComboBox_.onChange = [this] {
                if (channelComboGuard_ || !secondaryHost_.setChannelMapping)
                {
                    return;
                }
                const int pick = channelComboBox_.getSelectedId();
                if (pick > 0)
                {
                    secondaryHost_.setChannelMapping(tid_, pick - 1);
                    syncFromHosts();
                }
            };
            addAndMakeVisible(channelComboBox_);

            failureReasonLabel_.setFont(juce::FontOptions(11.0f));
            failureReasonLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe08a8a));
            failureReasonLabel_.setJustificationType(juce::Justification::topLeft);
            failureReasonLabel_.setMinimumHorizontalScale(1.0f);
            addChildComponent(failureReasonLabel_);

            retryLoadButton_.setButtonText("Retry load");
            retryLoadButton_.setTooltip(
                "Try to load the Secondary instrument again (after fixing the cause).");
            retryLoadButton_.setWantsKeyboardFocus(false);
            retryLoadButton_.onClick = [this] {
                if (!secondaryHost_.retryLoad)
                {
                    return;
                }
                secondaryValueLabel_.setText("Loading...", juce::dontSendNotification);
                juce::Component::SafePointer<AlternativesPanel> safe(this);
                juce::Timer::callAfterDelay(30, [safe] {
                    if (safe == nullptr)
                    {
                        return;
                    }
                    (void)safe->secondaryHost_.retryLoad(safe->tid_);
                    safe->syncFromHosts();
                });
            };
            addChildComponent(retryLoadButton_);

            // ---- proxy rows (moved from the Inspector's "Instrument Proxy" section) ----
            proxyTitleLabel_.setText("Instrument Proxy", juce::dontSendNotification);
            proxyTitleLabel_.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
            addAndMakeVisible(proxyTitleLabel_);

            initCaption(proxySourceCaptionLabel_, "Playing");
            initValue(proxySourceValueLabel_);
            initCaption(proxyCacheCaptionLabel_, "Proxy");
            initValue(proxyCacheValueLabel_);

            proxyProgressLabel_.setFont(juce::Font(juce::FontOptions(10.0f)).italicised());
            proxyProgressLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
            proxyProgressLabel_.setJustificationType(juce::Justification::centredLeft);
            proxyProgressLabel_.setInterceptsMouseClicks(false, false);
            addAndMakeVisible(proxyProgressLabel_);

            initCaption(proxyModeCaptionLabel_, "Proxy updates");
            // Item ids are modeComboIndex + 1 (0 Auto / 1 On Save / 2 Manual / 3 Off).
            proxyModeComboBox_.addItem("Auto after idle", 1);
            proxyModeComboBox_.addItem("On Save", 2);
            proxyModeComboBox_.addItem("Manual", 3);
            proxyModeComboBox_.addItem("Off", 4);
            proxyModeComboBox_.setTooltip(
                "How this instrument's proxy (portable audio stand-in) is kept up to date.\n\n"
                "Auto after idle: an edit marks the proxy stale immediately; rendering begins "
                "after five minutes without further relevant edits. Saving never waits for "
                "rendering, and playback can continue while it renders.\n"
                "On Save: rendering is queued by an explicit Save (autosave never renders).\n"
                "Manual: rendering starts only from Render now / Retry.\n"
                "Off: no automatic updates; existing proxy files are kept safe.");
            proxyModeComboBox_.onChange = [this] {
                if (proxyModeComboGuard_ || !proxyHost_.setUpdateMode)
                {
                    return;
                }
                const int pick = proxyModeComboBox_.getSelectedId();
                if (pick > 0)
                {
                    proxyHost_.setUpdateMode(tid_, pick - 1);
                    syncFromHosts();
                }
            };
            addAndMakeVisible(proxyModeComboBox_);

            proxyRenderNowButton_.setButtonText("Render now");
            proxyRenderNowButton_.setTooltip("Render this instrument's proxy now (Manual mode).");
            proxyRenderNowButton_.setWantsKeyboardFocus(false);
            proxyRenderNowButton_.onClick = [this] {
                if (proxyHost_.renderNow)
                {
                    proxyHost_.renderNow(tid_);
                    syncFromHosts();
                }
            };
            addAndMakeVisible(proxyRenderNowButton_);

            proxyCancelButton_.setButtonText("Cancel");
            proxyCancelButton_.setTooltip(
                "Cancel the queued or running proxy render. The previous proxy is kept.");
            proxyCancelButton_.setWantsKeyboardFocus(false);
            proxyCancelButton_.onClick = [this] {
                if (proxyHost_.cancelRender)
                {
                    proxyHost_.cancelRender(tid_);
                    syncFromHosts();
                }
            };
            addAndMakeVisible(proxyCancelButton_);

            proxyRetryButton_.setButtonText("Retry");
            proxyRetryButton_.setTooltip("Retry the failed proxy render.");
            proxyRetryButton_.setWantsKeyboardFocus(false);
            proxyRetryButton_.onClick = [this] {
                if (proxyHost_.retryRender)
                {
                    proxyHost_.retryRender(tid_);
                    syncFromHosts();
                }
            };
            addAndMakeVisible(proxyRetryButton_);

            syncFromHosts();
            setSize(kPopupWidthPx, preferredHeight());
            startTimer(250); // live refresh + self-dismiss when the track disappears
        }

        [[nodiscard]] int preferredHeight() const noexcept
        {
            int h = 8;                                       // top pad
            h += kRowH + 4;                                  // title
            h += kRowH * 2;                                  // primary + secondary rows
            h += 4 + kButtonH;                               // select/editor/remove
            h += 4 + 14 + kComboH;                           // channel caption + combo
            if (failureReasonVisible_)
            {
                h += 4 + failureReasonHeightPx_ + 4 + kButtonH; // reason + retry
            }
            h += 10 + kRowH + 4;                             // proxy title
            h += kRowH * 2 + 14;                             // playing/proxy rows + progress
            h += 4 + 14 + kComboH;                           // mode caption + combo
            h += 4 + kButtonH;                               // render/cancel/retry
            h += 8;                                          // bottom pad
            return h;
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(10, 8);
            titleLabel_.setBounds(area.removeFromTop(kRowH));
            area.removeFromTop(4);
            {
                auto row = area.removeFromTop(kRowH);
                primaryCaptionLabel_.setBounds(row.removeFromLeft(kCaptionW));
                primaryValueLabel_.setBounds(row);
            }
            {
                auto row = area.removeFromTop(kRowH);
                secondaryCaptionLabel_.setBounds(row.removeFromLeft(kCaptionW));
                secondaryValueLabel_.setBounds(row);
            }
            area.removeFromTop(4);
            {
                auto row = area.removeFromTop(kButtonH);
                const int w = juce::jmax(56, row.getWidth() / 3 - 2);
                selectSecondaryButton_.setBounds(row.removeFromLeft(w));
                row.removeFromLeft(3);
                editorButton_.setBounds(row.removeFromLeft(w));
                row.removeFromLeft(3);
                removeButton_.setBounds(row);
            }
            area.removeFromTop(4);
            channelCaptionLabel_.setBounds(area.removeFromTop(14));
            channelComboBox_.setBounds(area.removeFromTop(kComboH));
            if (failureReasonVisible_)
            {
                area.removeFromTop(4);
                failureReasonLabel_.setBounds(area.removeFromTop(failureReasonHeightPx_));
                area.removeFromTop(4);
                retryLoadButton_.setBounds(area.removeFromTop(kButtonH).removeFromLeft(110));
            }
            area.removeFromTop(10);
            proxyTitleLabel_.setBounds(area.removeFromTop(kRowH));
            area.removeFromTop(4);
            {
                auto row = area.removeFromTop(kRowH);
                proxySourceCaptionLabel_.setBounds(row.removeFromLeft(kCaptionW));
                proxySourceValueLabel_.setBounds(row);
            }
            {
                auto row = area.removeFromTop(kRowH);
                proxyCacheCaptionLabel_.setBounds(row.removeFromLeft(kCaptionW));
                proxyCacheValueLabel_.setBounds(row);
            }
            proxyProgressLabel_.setBounds(area.removeFromTop(14));
            area.removeFromTop(4);
            proxyModeCaptionLabel_.setBounds(area.removeFromTop(14));
            proxyModeComboBox_.setBounds(area.removeFromTop(kComboH));
            area.removeFromTop(4);
            {
                auto row = area.removeFromTop(kButtonH);
                const int w = juce::jmax(60, row.getWidth() / 3 - 2);
                proxyRenderNowButton_.setBounds(row.removeFromLeft(w + 20));
                row.removeFromLeft(3);
                proxyCancelButton_.setBounds(row.removeFromLeft(w - 10));
                row.removeFromLeft(3);
                proxyRetryButton_.setBounds(row);
            }
        }

        void syncFromHosts()
        {
            // Track deletion / project replacement safety: the destination can disappear at any
            // time while the callout is open — dismiss instead of showing stale controls.
            if (!secondaryHost_.isInstrumentDestination || !secondaryHost_.getView
                || !secondaryHost_.isInstrumentDestination(tid_))
            {
                dismissOwningCallOut();
                return;
            }

            const InstrumentSecondaryUiHost::View view = secondaryHost_.getView(tid_);
            primaryValueLabel_.setText(view.primaryText, juce::dontSendNotification);
            secondaryValueLabel_.setText(view.hasSecondary ? view.secondaryText
                                                           : juce::String("None"),
                                         juce::dontSendNotification);
            selectSecondaryButton_.setEnabled(secondaryHost_.listCatalogInstrumentNames != nullptr
                                              && secondaryHost_.selectSecondaryFromCatalog != nullptr);
            editorButton_.setEnabled(view.hasSecondary
                                     && secondaryHost_.openSecondaryEditor != nullptr);
            removeButton_.setEnabled(view.hasSecondary && secondaryHost_.removeSecondary != nullptr);
            channelComboBox_.setEnabled(view.hasSecondary
                                        && secondaryHost_.setChannelMapping != nullptr);
            channelComboGuard_ = true;
            channelComboBox_.setSelectedId(juce::jlimit(0, 16, view.forcedMidiChannel) + 1,
                                           juce::dontSendNotification);
            channelComboGuard_ = false;

            const juce::String reason = (view.hasSecondary && secondaryHost_.getLoadFailureReason)
                                            ? secondaryHost_.getLoadFailureReason(tid_)
                                            : juce::String{};
            const bool showReason = reason.isNotEmpty();
            if (showReason != failureReasonVisible_ || reason != lastFailureReason_)
            {
                failureReasonVisible_ = showReason;
                lastFailureReason_ = reason;
                failureReasonLabel_.setText(showReason ? ("Load failed: " + reason) : juce::String{},
                                            juce::dontSendNotification);
                failureReasonLabel_.setVisible(showReason);
                retryLoadButton_.setVisible(showReason && secondaryHost_.retryLoad != nullptr);
                setSize(kPopupWidthPx, preferredHeight());
            }

            const bool proxyRows = proxyHost_.isProxyDestination && proxyHost_.getStatusView
                                   && proxyHost_.isProxyDestination(tid_);
            proxyTitleLabel_.setVisible(proxyRows);
            proxySourceCaptionLabel_.setVisible(proxyRows);
            proxySourceValueLabel_.setVisible(proxyRows);
            proxyCacheCaptionLabel_.setVisible(proxyRows);
            proxyCacheValueLabel_.setVisible(proxyRows);
            proxyProgressLabel_.setVisible(proxyRows);
            proxyModeCaptionLabel_.setVisible(proxyRows);
            proxyModeComboBox_.setVisible(proxyRows);
            proxyRenderNowButton_.setVisible(proxyRows);
            proxyCancelButton_.setVisible(proxyRows);
            proxyRetryButton_.setVisible(proxyRows);
            if (proxyRows)
            {
                const proxy_status::ProxyStatusView pv = proxyHost_.getStatusView(tid_);
                proxySourceValueLabel_.setText(pv.sourceLabel, juce::dontSendNotification);
                proxyCacheValueLabel_.setText(pv.cacheLabel, juce::dontSendNotification);
                proxyProgressLabel_.setText(pv.showProgress ? pv.progressText : juce::String(),
                                            juce::dontSendNotification);
                proxySourceValueLabel_.setTooltip(pv.tooltip);
                proxyCacheValueLabel_.setTooltip(pv.tooltip);
                proxySourceCaptionLabel_.setTooltip(pv.tooltip);
                proxyCacheCaptionLabel_.setTooltip(pv.tooltip);
                proxyModeComboGuard_ = true;
                proxyModeComboBox_.setSelectedId(pv.modeComboIndex + 1, juce::dontSendNotification);
                proxyModeComboGuard_ = false;
                proxyRenderNowButton_.setEnabled(pv.canRenderNow);
                proxyCancelButton_.setEnabled(pv.canCancel);
                proxyRetryButton_.setEnabled(pv.canRetry);
            }
        }

    private:
        void timerCallback() override { syncFromHosts(); }

        void dismissOwningCallOut()
        {
            stopTimer();
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            {
                box->dismiss();
            }
        }

        void showSecondaryCatalogPickerMenu()
        {
            if (!secondaryHost_.listCatalogInstrumentNames || !secondaryHost_.selectSecondaryFromCatalog)
            {
                return;
            }
            const juce::StringArray names = secondaryHost_.listCatalogInstrumentNames();
            juce::PopupMenu menu;
            if (names.isEmpty())
            {
                menu.addItem(1, "(no instruments in catalogue)", false);
            }
            else
            {
                for (int i = 0; i < names.size(); ++i)
                {
                    menu.addItem(i + 1, names[i]); // itemId = catalogIndex + 1
                }
            }
            juce::Component::SafePointer<AlternativesPanel> safe(this);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&selectSecondaryButton_),
                [safe, hasAny = !names.isEmpty()](const int result) {
                    if (safe == nullptr || result <= 0 || !hasAny)
                    {
                        return;
                    }
                    safe->secondaryValueLabel_.setText("Loading...", juce::dontSendNotification);
                    juce::Component::SafePointer<AlternativesPanel> inner(safe.getComponent());
                    juce::Timer::callAfterDelay(30, [inner, result] {
                        if (inner == nullptr)
                        {
                            return;
                        }
                        inner->secondaryHost_.selectSecondaryFromCatalog(inner->tid_, result - 1);
                        inner->syncFromHosts();
                    });
                });
        }

        const TrackId tid_;
        InstrumentProxyUiHost proxyHost_;
        InstrumentSecondaryUiHost secondaryHost_;

        juce::Label titleLabel_;
        juce::Label primaryCaptionLabel_, primaryValueLabel_;
        juce::Label secondaryCaptionLabel_, secondaryValueLabel_;
        juce::TextButton selectSecondaryButton_, editorButton_, removeButton_;
        juce::Label channelCaptionLabel_;
        juce::ComboBox channelComboBox_;
        bool channelComboGuard_ = false;
        juce::Label failureReasonLabel_;
        juce::TextButton retryLoadButton_;
        bool failureReasonVisible_ = false;
        juce::String lastFailureReason_;
        static constexpr int failureReasonHeightPx_ = 42;

        juce::Label proxyTitleLabel_;
        juce::Label proxySourceCaptionLabel_, proxySourceValueLabel_;
        juce::Label proxyCacheCaptionLabel_, proxyCacheValueLabel_;
        juce::Label proxyProgressLabel_;
        juce::Label proxyModeCaptionLabel_;
        juce::ComboBox proxyModeComboBox_;
        bool proxyModeComboGuard_ = false;
        juce::TextButton proxyRenderNowButton_, proxyCancelButton_, proxyRetryButton_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlternativesPanel)
    };

    /// Outer callout content: hosts the panel in a viewport capped to the display's usable area
    /// (the popup always fits on screen; the panel scrolls internally when it cannot).
    class AlternativesCallOutContent final : public juce::Component
    {
    public:
        AlternativesCallOutContent(const TrackId tid,
                                   const juce::Rectangle<int> screenAnchor,
                                   InstrumentProxyUiHost proxyHost,
                                   InstrumentSecondaryUiHost secondaryHost)
        {
            panel_ = std::make_unique<AlternativesPanel>(tid, std::move(proxyHost),
                                                         std::move(secondaryHost));
            viewport_.setViewedComponent(panel_.get(), false);
            viewport_.setScrollBarsShown(true, false);
            addAndMakeVisible(viewport_);

            const juce::Rectangle<int> usable
                = juce::Desktop::getInstance().getDisplays().getDisplayForRect(screenAnchor)
                      ->userArea;
            // Leave margin for the callout chrome/arrow.
            const int maxH = juce::jmax(160, usable.getHeight() - 80);
            const bool needsScroll = panel_->getHeight() > maxH;
            const int w = panel_->getWidth() + (needsScroll ? 14 : 0);
            setSize(w, juce::jmin(panel_->getHeight(), maxH));
        }

        void resized() override { viewport_.setBounds(getLocalBounds()); }

    private:
        std::unique_ptr<AlternativesPanel> panel_;
        juce::Viewport viewport_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlternativesCallOutContent)
    };
} // namespace

namespace instrument_alternatives_popup
{
    void show(const TrackId tid,
              const juce::Rectangle<int> screenAnchor,
              InstrumentProxyUiHost proxyHost,
              InstrumentSecondaryUiHost secondaryHost)
    {
        auto content = std::make_unique<AlternativesCallOutContent>(
            tid, screenAnchor, std::move(proxyHost), std::move(secondaryHost));
        // nullptr parent => desktop callout; CallOutBox owns the content and keeps itself on
        // screen. Dismissal (click-away/Esc) only closes the UI — no playback/instrument/render
        // side effects (the content merely observes the shared seams).
        (void)juce::CallOutBox::launchAsynchronously(std::move(content), screenAnchor, nullptr);
    }
} // namespace instrument_alternatives_popup
