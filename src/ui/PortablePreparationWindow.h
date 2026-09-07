#pragma once

// =============================================================================
// PortablePreparationWindow — P1J progress surface for Prepare Portable Project
// (steering §16.6): responsive, never blocks the message thread, shows the
// current phase / current item / per-destination progress, offers Cancel while
// running, and an actionable blocker list plus Restart on failure. The window
// is a VIEW ONLY: the operation itself is owned by the app-runtime
// PortablePreparationService (never by this transient window) and survives or
// dies independently of it. No internal fingerprints/generation ids are shown.
// =============================================================================

#include "app/PortableProjectService.h"

#include <JuceHeader.h>

#include <functional>
#include <utility>

/// Non-modal progress window. All callbacks run on the message thread.
class PortablePreparationWindow final : public juce::DocumentWindow
{
public:
    struct Callbacks
    {
        std::function<portable_project::PreparationStatus()> getStatus;
        std::function<void()> cancelOperation;
        /// Restart the whole flow (destination chooser) after a failure.
        std::function<void()> restartOperation;
        std::function<void()> onWindowClosed;
    };

    explicit PortablePreparationWindow(Callbacks callbacks)
        : juce::DocumentWindow("Prepare Portable Project",
                               juce::Colours::darkgrey.darker(0.7f),
                               juce::DocumentWindow::closeButton)
        , callbacks_(std::move(callbacks))
    {
        setUsingNativeTitleBar(true);
        content_ = new Content(callbacks_);
        setContentOwned(content_, false);
        setResizable(false, false);
        centreWithSize(560, 380);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        // Closing the window never silently kills a running operation: the
        // service keeps running (stable ownership); the user can cancel first.
        if (callbacks_.onWindowClosed != nullptr)
        {
            callbacks_.onWindowClosed();
        }
    }

    void refreshNow() { content_->refresh(); }

private:
    class Content final : public juce::Component, private juce::Timer
    {
    public:
        explicit Content(const Callbacks& callbacks) : callbacks_(callbacks)
        {
            const auto caption = [this](juce::Label& l, const float size, const bool bold) {
                l.setFont(juce::Font(juce::FontOptions{}.withHeight(size))
                              .withStyle(bold ? juce::Font::bold : juce::Font::plain));
                l.setColour(juce::Label::textColourId, juce::Colours::white);
                addAndMakeVisible(l);
            };
            caption(phaseLabel_, 19.0f, true);
            caption(itemLabel_, 14.0f, false);
            itemLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            caption(countLabel_, 14.0f, false);
            countLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

            details_.setMultiLine(true);
            details_.setReadOnly(true);
            details_.setCaretVisible(false);
            details_.setScrollbarsShown(true);
            details_.setColour(juce::TextEditor::backgroundColourId,
                               juce::Colours::black.withAlpha(0.35f));
            details_.setColour(juce::TextEditor::textColourId, juce::Colours::white);
            details_.setColour(juce::TextEditor::outlineColourId,
                               juce::Colours::grey.withAlpha(0.4f));
            addAndMakeVisible(details_);

            cancelButton_.setButtonText("Cancel");
            cancelButton_.onClick = [this] {
                if (callbacks_.cancelOperation != nullptr)
                {
                    callbacks_.cancelOperation();
                }
            };
            addAndMakeVisible(cancelButton_);

            openFolderButton_.setButtonText("Open Folder");
            openFolderButton_.onClick = [this] { finalFolder_.revealToUser(); };
            addChildComponent(openFolderButton_);

            restartButton_.setButtonText("Start Again...");
            restartButton_.onClick = [this] {
                if (callbacks_.restartOperation != nullptr)
                {
                    callbacks_.restartOperation();
                }
            };
            addChildComponent(restartButton_);

            setSize(560, 380);
            refresh();
            startTimer(250); // poll the service status; never blocks anything
        }

        void refresh()
        {
            if (callbacks_.getStatus == nullptr)
            {
                return;
            }
            const portable_project::PreparationStatus st = callbacks_.getStatus();
            finalFolder_ = st.finalFolder;
            phaseLabel_.setText(portable_project::preparationPhaseName(st.phase),
                                juce::dontSendNotification);
            itemLabel_.setText(st.currentItem, juce::dontSendNotification);
            countLabel_.setText(st.totalFiles > 0
                                    ? juce::String(st.completedFiles) + " of "
                                          + juce::String(st.totalFiles) + " files"
                                    : juce::String(),
                                juce::dontSendNotification);

            juce::String text;
            if (st.phase == portable_project::PreparationPhase::Complete)
            {
                text << "Portable project ready:\n" << st.finalFolder.getFullPathName()
                     << "\n\nThe folder contains the project, all referenced audio and "
                        "the current instrument proxies. Plugin binaries and licences "
                        "are never included.\n";
            }
            else if (st.phase == portable_project::PreparationPhase::Failed)
            {
                if (st.failureReason.isNotEmpty())
                {
                    text << st.failureReason << "\n";
                }
                if (!st.blockers.isEmpty())
                {
                    text << "\nBlocking tracks:\n";
                    for (const auto& b : st.blockers)
                    {
                        text << "  - " << b << "\n";
                    }
                }
            }
            else
            {
                for (const auto& d : st.destinations)
                {
                    text << d.name << ": " << d.detail << "\n";
                }
            }
            if (text != details_.getText())
            {
                details_.setText(text, juce::dontSendNotification);
            }

            const bool running = st.phase != portable_project::PreparationPhase::Idle
                                 && st.phase != portable_project::PreparationPhase::Complete
                                 && st.phase != portable_project::PreparationPhase::Cancelled
                                 && st.phase != portable_project::PreparationPhase::Failed;
            cancelButton_.setEnabled(running);
            cancelButton_.setVisible(running
                                     || st.phase
                                            == portable_project::PreparationPhase::Cancelled);
            openFolderButton_.setVisible(st.phase
                                         == portable_project::PreparationPhase::Complete);
            restartButton_.setVisible(st.phase == portable_project::PreparationPhase::Failed
                                      || st.phase
                                             == portable_project::PreparationPhase::Cancelled);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(14);
            phaseLabel_.setBounds(r.removeFromTop(28));
            itemLabel_.setBounds(r.removeFromTop(22));
            countLabel_.setBounds(r.removeFromTop(20));
            r.removeFromTop(6);
            auto buttons = r.removeFromBottom(30);
            cancelButton_.setBounds(buttons.removeFromLeft(110));
            buttons.removeFromLeft(8);
            openFolderButton_.setBounds(buttons.removeFromLeft(110));
            restartButton_.setBounds(buttons.removeFromLeft(120));
            r.removeFromBottom(8);
            details_.setBounds(r);
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colours::darkgrey.darker(0.7f));
        }

    private:
        void timerCallback() override { refresh(); }

        const Callbacks& callbacks_;
        juce::Label phaseLabel_, itemLabel_, countLabel_;
        juce::TextEditor details_;
        juce::TextButton cancelButton_, openFolderButton_, restartButton_;
        juce::File finalFolder_;
    };

    Callbacks callbacks_;
    Content* content_ = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PortablePreparationWindow)
};
