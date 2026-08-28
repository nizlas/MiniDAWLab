#include "app/MainAppDialogs.h"

#include "app/AudioMixdownExporter.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "domain/AudioMixdownProjectSettings.h"
#include "domain/MixdownWavProbe.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"
#include "transport/Transport.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdint>
namespace
{

[[nodiscard]] juce::String sanitizeFileBaseName(juce::String s)
{
    s = s.trim();
    if (s.isEmpty())
    {
        return "mixdown";
    }
    const juce::String illegal = "\\/:*?\"<>|";
    for (int i = 0; i < illegal.length(); ++i)
    {
        s = s.replaceCharacter(illegal[i], '_');
    }
    if (s.trim().isEmpty())
    {
        return "mixdown";
    }
    return s;
}

[[nodiscard]] juce::File defaultMixdownFolder(const Session& session)
{
    if (session.hasKnownProjectFile())
    {
        return session.getCurrentProjectFolder().getChildFile("Mixdown");
    }

    const juce::File music = juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    if (music.isDirectory())
    {
        return music.getChildFile("MiniDAWLab Mixdown");
    }

    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("MiniDAWLab Mixdown");
}

/// Same green family as `TrackHeaderView` power-on (active strip).
[[nodiscard]] juce::Colour mp3EncoderReadyLabelColour() noexcept
{
    return juce::Colour(0xff2d9d53);
}

[[nodiscard]] juce::String formatLoopDurationHint(const std::int64_t lengthSamples, const double sampleRate)
{
    if (lengthSamples <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        return {};
    }
    const double sec = static_cast<double>(lengthSamples) / sampleRate;
    return juce::String(sec, 2) + " s";
}

[[nodiscard]] int clampWaveBitsItemId(const int id, const bool floatSupported) noexcept
{
    if (id == 16)
    {
        return 16;
    }
    if (id == 24)
    {
        return 24;
    }
    if (id == 32 && floatSupported)
    {
        return 32;
    }
    if (id == 32 && !floatSupported)
    {
        return 24;
    }
    return 24;
}

[[nodiscard]] int kbpsFromMp3BitrateComboId(const int comboId) noexcept
{
    constexpr int kTable[] {128, 160, 192, 224, 256, 320};
    const int idx = juce::jlimit(0, 5, comboId - 1);
    return kTable[static_cast<unsigned>(idx)];
}

/// Everything needed to run the export after the settings dialog has closed.
struct MixdownStartPlan
{
    bool mp3 = false;
    juce::File outputFile;
    int mp3BitrateKbps = 192;
    mini_daw_audio_mixdown::MixdownWaveBits wavBits = mini_daw_audio_mixdown::MixdownWaveBits::Pcm24;
};

/// Small always-on-top desktop window with a status line and a progress bar. Export is blocking
/// on the message thread (Slice 2 safety: no message dispatch runs concurrently with the offline
/// render), so updates are painted synchronously via `ComponentPeer::performAnyPendingRepaintsNow`
/// instead of pumping the event loop.
class MixdownProgressWindow final : public juce::Component,
                                    public mini_daw_audio_mixdown::MixdownProgressSink
{
public:
    MixdownProgressWindow()
    {
        setOpaque(true);
        setSize(440, 104);
        setAlwaysOnTop(true);
        addToDesktop(juce::ComponentPeer::windowHasDropShadow);
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            setCentrePosition(display->userArea.getCentre());
        }
        setVisible(true);
        toFront(false);
        appendMixdownDiagnosticLine("progress ui shown");
        flushPaintNow();
    }

    ~MixdownProgressWindow() override
    {
        appendMixdownDiagnosticLine("progress ui closed");
    }

    void setMixdownProgress(const juce::String& statusText, const double fraction01) override
    {
        statusText_ = statusText;
        fraction_ = fraction01;
        ++pulseCounter_;
        repaint();
        flushPaintNow();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff2a2a33));
        g.setColour(juce::Colours::white.withAlpha(0.25f));
        g.drawRect(getLocalBounds(), 1);

        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(15.0f));
        g.drawText(statusText_, 16, 14, getWidth() - 32, 22, juce::Justification::centredLeft);

        const juce::Rectangle<int> barArea(16, 52, getWidth() - 32, 22);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.fillRect(barArea);
        g.setColour(mp3EncoderReadyLabelColour());
        if (fraction_ >= 0.0)
        {
            const int w = juce::roundToInt(barArea.getWidth() * juce::jlimit(0.0, 1.0, fraction_));
            g.fillRect(barArea.withWidth(w));
        }
        else
        {
            // Indeterminate: a segment bouncing left-right, advanced by each progress pulse.
            const int segW = juce::jmax(24, barArea.getWidth() / 4);
            const int span = juce::jmax(1, barArea.getWidth() - segW);
            const int offset = static_cast<int>((pulseCounter_ * 10) % (2 * span));
            const int x = offset <= span ? offset : (2 * span - offset);
            g.fillRect(barArea.getX() + x, barArea.getY(), segW, barArea.getHeight());
        }
        g.setColour(juce::Colours::white.withAlpha(0.4f));
        g.drawRect(barArea, 1);
    }

private:
    void flushPaintNow()
    {
        if (auto* peer = getPeer())
        {
            peer->performAnyPendingRepaintsNow();
        }
    }

    juce::String statusText_ { "Preparing..." };
    double fraction_ = -1.0;
    std::uint64_t pulseCounter_ = 0;
};

/// Runs the confirmed export (dialog already closed). Blocking on the message thread; live
/// feedback comes from `MixdownProgressWindow`. Shows the final success/failure alert.
void runConfirmedMixdownExport(Transport& transport,
                               Session& session,
                               PlaybackEngine& playbackEngine,
                               juce::AudioDeviceManager& deviceManager,
                               const std::function<void()>& syncTransportUiFromDomain,
                               const MixdownStartPlan& plan)
{
    const char* const fmt = plan.mp3 ? "mp3" : "wav";
    juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
    if (dev == nullptr)
    {
        appendMixdownDiagnosticLine(juce::String("FINAL ") + fmt
                                    + " export failed: no audio device at export start");
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Audio Mixdown",
                                               "No audio device is open.");
        return;
    }

    MixdownProgressWindow progress;
    juce::Result result = juce::Result::ok();
    if (plan.mp3)
    {
        result = mini_daw_audio_mixdown::exportStereoMixdownMp3Blocking(transport,
                                                                        session,
                                                                        playbackEngine,
                                                                        deviceManager,
                                                                        syncTransportUiFromDomain,
                                                                        plan.outputFile,
                                                                        plan.mp3BitrateKbps,
                                                                        &progress,
                                                                        true);
    }
    else
    {
        mini_daw_audio_mixdown::MixdownExportRequest request;
        request.outputFile = plan.outputFile;
        request.sampleRate = dev->getCurrentSampleRate();
        request.bits = plan.wavBits;
        request.progressSink = &progress;
        request.overwriteConfirmed = true;
        result = mini_daw_audio_mixdown::exportStereoMixdownWavBlocking(transport,
                                                                        session,
                                                                        playbackEngine,
                                                                        deviceManager,
                                                                        syncTransportUiFromDomain,
                                                                        request);
    }

    if (result.failed())
    {
        const juce::String msg = result.getErrorMessage();
        appendMixdownDiagnosticLine(juce::String("FINAL ") + fmt + " export failed: "
                                    + msg.replaceCharacter('\n', ' '));
        writeLastOperationBreadcrumb(juce::String("mixdown ") + fmt + " failed");
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Audio Mixdown", msg);
        return;
    }

    appendMixdownDiagnosticLine(juce::String("FINAL ") + fmt + " export ok");
    writeLastOperationBreadcrumb(juce::String("mixdown ") + fmt + " end ok");
    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                           "Audio Mixdown",
                                           "Export complete:\n" + plan.outputFile.getFullPathName());
}

[[nodiscard]] int mp3BitrateComboIdFromKbps(const int kbps) noexcept
{
    const int k = clampMp3BitRateKbps(kbps);
    switch (k)
    {
    case 128:
        return 1;
    case 160:
        return 2;
    case 192:
        return 3;
    case 224:
        return 4;
    case 256:
        return 5;
    case 320:
    default:
        return 6;
    }
}

class AudioMixdownDialogContent final : public juce::Component
{
public:
    AudioMixdownDialogContent(Transport& transport,
                              Session& session,
                              PlaybackEngine& playbackEngine,
                              juce::AudioDeviceManager& deviceManager,
                              std::function<void()> syncTransportUiFromDomain,
                              std::function<void(std::function<void()>)> confirmSaveBeforeExport)
        : transport_(transport)
        , session_(session)
        , playbackEngine_(playbackEngine)
        , deviceManager_(deviceManager)
        , syncTransportUiFromDomain_(std::move(syncTransportUiFromDomain))
        , confirmSaveBeforeExport_(std::move(confirmSaveBeforeExport))
    {
        addAndMakeVisible(nameLabel_);
        nameLabel_.setText("Name", juce::dontSendNotification);
        nameLabel_.attachToComponent(&nameEditor_, true);

        addAndMakeVisible(nameEditor_);

        floatWaveSupportedAtDialogOpen_ = probeStereoFloatWavSupportedMixdown(
            deviceManager_.getCurrentAudioDevice() != nullptr
                ? deviceManager_.getCurrentAudioDevice()->getCurrentSampleRate()
                : 44100.0);

        const AudioMixdownProjectSettings proj = session_.getAudioMixdownSettings();

        juce::String initialBase = "mixdown";
        if (session_.hasKnownProjectFile())
        {
            initialBase = session_.getCurrentProjectFile().getFileNameWithoutExtension();
        }
        if (initialBase.trim().isEmpty())
        {
            initialBase = "mixdown";
        }

        {
            const juce::String storedName = proj.fileNameWithoutExtension.trim();
            nameEditor_.setText(
                sanitizeFileBaseName(storedName.isEmpty() ? initialBase : storedName),
                false);
        }
        nameEditor_.onTextChange = [this] {
            pushMixdownUiToSession();
            refreshPreviewAndExportState();
        };

        addAndMakeVisible(pathLabel_);
        pathLabel_.setText("Path", juce::dontSendNotification);
        pathLabel_.attachToComponent(&pathEditor_, true);

        addAndMakeVisible(pathEditor_);
        {
            juce::File pathFile;
            if (session_.hasKnownProjectFile())
            {
                pathFile = decodeMixdownOutputDirectorySpec(session_.getCurrentProjectFolder(),
                                                           proj.outputDirectorySpec);
            }
            if (pathFile.getFullPathName().isEmpty())
            {
                pathFile = defaultMixdownFolder(session_);
            }
            pathEditor_.setText(pathFile.getFullPathName(), false);
        }
        pathEditor_.onTextChange = [this] {
            pushMixdownUiToSession();
            refreshPreviewAndExportState();
        };

        addAndMakeVisible(previewLabel_);
        previewLabel_.setText("Preview", juce::dontSendNotification);
        previewLabel_.attachToComponent(&previewPathLabel_, true);

        addAndMakeVisible(previewPathLabel_);
        previewPathLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);

        addAndMakeVisible(fileTypeLabel_);
        fileTypeLabel_.setText("File Type", juce::dontSendNotification);
        fileTypeLabel_.attachToComponent(&fileTypeCombo_, true);

        addAndMakeVisible(fileTypeCombo_);
        fileTypeCombo_.addItem("Wave", 1);
        fileTypeCombo_.addItem("MPEG 1 Layer 3", 2);
        fileTypeCombo_.setSelectedId(
            proj.fileType == AudioMixdownProjectSettings::FileType::MpegLayer3 ? 2 : 1,
            juce::dontSendNotification);
        fileTypeCombo_.onChange = [this] {
            refreshPreviewAndExportState();
            pushMixdownUiToSession();
        };

        addAndMakeVisible(sampleRateLabel_);
        sampleRateLabel_.setText("Sample Rate", juce::dontSendNotification);
        sampleRateLabel_.attachToComponent(&sampleRateValueLabel_, true);

        addAndMakeVisible(sampleRateValueLabel_);
        sampleRateValueLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
        if (juce::AudioIODevice* dev = deviceManager_.getCurrentAudioDevice())
        {
            const double sr = dev->getCurrentSampleRate();
            sampleRateValueLabel_.setText(
                juce::String(sr, 2) + " Hz (current device rate)",
                juce::dontSendNotification);
        }
        else
        {
            sampleRateValueLabel_.setText("(no audio device)", juce::dontSendNotification);
        }

        addAndMakeVisible(rangeLabel_);
        rangeLabel_.setText("Range", juce::dontSendNotification);
        rangeLabel_.attachToComponent(&rangeValueLabel_, true);

        addAndMakeVisible(rangeValueLabel_);
        rangeValueLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);

        addAndMakeVisible(waveBitsLabel_);
        waveBitsLabel_.setText("Bit Depth", juce::dontSendNotification);
        waveBitsLabel_.attachToComponent(&waveBitsCombo_, true);

        addAndMakeVisible(waveBitsCombo_);
        waveBitsCombo_.addItem("16 bit PCM", 16);
        waveBitsCombo_.addItem("24 bit PCM", 24);
        if (floatWaveSupportedAtDialogOpen_)
        {
            waveBitsCombo_.addItem("32 bit float", 32);
        }
        {
            int waveChoice = proj.wavBitDepth;
            if (waveChoice != 16 && waveChoice != 24 && waveChoice != 32)
            {
                waveChoice = floatWaveSupportedAtDialogOpen_ ? 32 : 24;
            }
            waveChoice = clampWaveBitsItemId(waveChoice, floatWaveSupportedAtDialogOpen_);
            waveBitsCombo_.setSelectedId(waveChoice, juce::dontSendNotification);
        }
        waveBitsCombo_.onChange = [this] {
            pushMixdownUiToSession();
            refreshPreviewAndExportState();
        };

        addAndMakeVisible(mp3BitrateLabel_);
        mp3BitrateLabel_.setText("Bit Rate", juce::dontSendNotification);
        mp3BitrateLabel_.attachToComponent(&mp3BitrateCombo_, true);

        addAndMakeVisible(mp3BitrateCombo_);
        mp3BitrateCombo_.addItem("128 kbps", 1);
        mp3BitrateCombo_.addItem("160 kbps", 2);
        mp3BitrateCombo_.addItem("192 kbps", 3);
        mp3BitrateCombo_.addItem("224 kbps", 4);
        mp3BitrateCombo_.addItem("256 kbps", 5);
        mp3BitrateCombo_.addItem("320 kbps", 6);
        mp3BitrateCombo_.setSelectedId(mp3BitrateComboIdFromKbps(proj.mp3BitRateKbps),
                                       juce::dontSendNotification);
        mp3BitrateCombo_.onChange = [this] {
            pushMixdownUiToSession();
            refreshPreviewAndExportState();
        };

        addAndMakeVisible(mp3StatusLabel_);
        mp3StatusLabel_.setJustificationType(juce::Justification::topLeft);

        addAndMakeVisible(exportButton_);
        exportButton_.setButtonText("Export Audio");
        exportButton_.onClick = [this] { handleExportClicked(); };

        addAndMakeVisible(cancelButton_);
        cancelButton_.setButtonText("Cancel");
        cancelButton_.onClick = [this] {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            {
                dw->exitModalState(0);
            }
        };

        setSize(560, 416);
        pushMixdownUiToSession();
        refreshPreviewAndExportState();
    }

    void resized() override
    {
        constexpr int kLeftCol = 140;
        constexpr int kRowH = 28;
        constexpr int kGap = 8;
        constexpr int kBottomBtnW = 120;
        constexpr int kBottomBtnH = 32;

        auto r = getLocalBounds().reduced(14, 14);
        int y = r.getY();

        nameEditor_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        y += kRowH + kGap;

        pathEditor_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        y += kRowH + kGap;

        previewPathLabel_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        y += kRowH + kGap;

        fileTypeCombo_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        y += kRowH + kGap;

        sampleRateValueLabel_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        y += kRowH + kGap;

        rangeValueLabel_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        y += kRowH + kGap;

        waveBitsCombo_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);
        mp3BitrateCombo_.setBounds(r.getX() + kLeftCol, y, r.getWidth() - kLeftCol, kRowH);

        mp3StatusLabel_.setBounds(r.getX() + kLeftCol,
                                  y + kRowH + 6,
                                  r.getWidth() - kLeftCol,
                                  44);
        y += kRowH + kGap;

        const int btnY = getHeight() - 14 - kBottomBtnH;
        cancelButton_.setBounds(getWidth() - 14 - kBottomBtnW, btnY, kBottomBtnW, kBottomBtnH);
        exportButton_.setBounds(cancelButton_.getX() - 10 - kBottomBtnW, btnY, kBottomBtnW, kBottomBtnH);
    }

private:
    void applyWaveBitsComboFromProjectSettings()
    {
        AudioMixdownProjectSettings ps = session_.getAudioMixdownSettings();
        int choice = ps.wavBitDepth;
        if (choice != 16 && choice != 24 && choice != 32)
        {
            choice = floatWaveSupportedAtDialogOpen_ ? 32 : 24;
        }
        choice = clampWaveBitsItemId(choice, floatWaveSupportedAtDialogOpen_);
        waveBitsCombo_.setSelectedId(choice, juce::dontSendNotification);
    }

    void pushMixdownUiToSession()
    {
        AudioMixdownProjectSettings s = session_.getAudioMixdownSettings();
        s.fileNameWithoutExtension = sanitizeFileBaseName(nameEditor_.getText());

        const juce::File folder(pathEditor_.getText().trim());
        if (session_.hasKnownProjectFile())
        {
            s.outputDirectorySpec
                = encodeMixdownOutputDirectorySpec(session_.getCurrentProjectFolder(), folder);
        }
        else
        {
            s.outputDirectorySpec = folder.getFullPathName();
        }

        s.fileType = (fileTypeCombo_.getSelectedId() == 2) ? AudioMixdownProjectSettings::FileType::MpegLayer3
                                                            : AudioMixdownProjectSettings::FileType::Wave;

        if (fileTypeCombo_.getSelectedId() != 2)
        {
            const int wid = waveBitsCombo_.getSelectedId();
            if (wid == 16 || wid == 24 || wid == 32)
            {
                s.wavBitDepth = wid;
            }
        }

        s.mp3BitRateKbps = kbpsFromMp3BitrateComboId(mp3BitrateCombo_.getSelectedId());
        session_.setAudioMixdownSettings(std::move(s));
    }

    void refreshPreviewAndExportState()
    {
        const bool mp3 = fileTypeCombo_.getSelectedId() == 2;
        waveBitsLabel_.setVisible(!mp3);
        waveBitsCombo_.setVisible(!mp3);
        mp3BitrateLabel_.setVisible(mp3);
        mp3BitrateCombo_.setVisible(mp3);
        mp3StatusLabel_.setVisible(mp3);

        if (!mp3)
        {
            applyWaveBitsComboFromProjectSettings();
        }

        const juce::String ext = mp3 ? ".mp3" : ".wav";
        const juce::File folder(pathEditor_.getText().trim());
        const juce::String base = sanitizeFileBaseName(nameEditor_.getText());
        previewPathLabel_.setText(folder.getChildFile(base + ext).getFullPathName(), juce::dontSendNotification);

        std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
        juce::AudioIODevice* const dev = deviceManager_.getCurrentAudioDevice();
        const double sampleRate = dev != nullptr ? dev->getCurrentSampleRate() : 0.0;

        mini_daw_audio_mixdown::ActiveLoopMixdownSpan loopSpan {};
        bool loopOk = false;
        if (snap == nullptr)
        {
            rangeValueLabel_.setText("Session snapshot unavailable.", juce::dontSendNotification);
            rangeValueLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
        }
        else
        {
            const juce::Result spanRes = mini_daw_audio_mixdown::resolveActiveLoopMixdownSpan(
                transport_.readCycleEnabledForUi(),
                snap->getLeftLocatorSamples(),
                snap->getRightLocatorSamples(),
                loopSpan);
            if (spanRes.failed())
            {
                rangeValueLabel_.setText(spanRes.getErrorMessage(), juce::dontSendNotification);
                rangeValueLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
            }
            else
            {
                loopOk = true;
                const juce::String dur = formatLoopDurationHint(loopSpan.lengthSamples, sampleRate);
                rangeValueLabel_.setText("Active Loop — " + dur + " (" + juce::String(loopSpan.lengthSamples)
                                             + " samples, " + juce::String(loopSpan.startSample) + " … "
                                             + juce::String(loopSpan.startSample + loopSpan.lengthSamples) + ")",
                                         juce::dontSendNotification);
                rangeValueLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
            }
        }

        if (mp3)
        {
            const bool lameOk = mini_daw_audio_mixdown::isBundledLameEncoderAvailable();
            if (!lameOk)
            {
                mp3StatusLabel_.setText(
                    "MP3 encoder not found. Expected Tools/lame/lame.exe beside the application.",
                    juce::dontSendNotification);
                mp3StatusLabel_.setColour(juce::Label::textColourId, juce::Colours::orange);
                exportButton_.setEnabled(false);
            }
            else
            {
                mp3StatusLabel_.setText("MP3 encoder ready.", juce::dontSendNotification);
                mp3StatusLabel_.setColour(juce::Label::textColourId, mp3EncoderReadyLabelColour());
                exportButton_.setEnabled(loopOk && deviceManager_.getCurrentAudioDevice() != nullptr
                                         && pathEditor_.getText().trim().isNotEmpty()
                                         && nameEditor_.getText().trim().isNotEmpty());
            }
        }
        else
        {
            mp3StatusLabel_.setText({}, juce::dontSendNotification);
            exportButton_.setEnabled(loopOk && deviceManager_.getCurrentAudioDevice() != nullptr
                                     && pathEditor_.getText().trim().isNotEmpty()
                                     && nameEditor_.getText().trim().isNotEmpty());
        }
    }

    void handleExportClicked()
    {
        const bool mp3 = fileTypeCombo_.getSelectedId() == 2;
        appendMixdownDiagnosticLine(juce::String("export clicked format=") + (mp3 ? "mp3" : "wav"));
        writeLastOperationBreadcrumb("mixdown export requested");

        pushMixdownUiToSession();

        juce::AudioIODevice* const dev = deviceManager_.getCurrentAudioDevice();
        if (dev == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Mixdown",
                                                   "No audio device is open.");
            return;
        }

        if (mp3 && !mini_daw_audio_mixdown::isBundledLameEncoderAvailable())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Audio Mixdown",
                "MP3 encoder not found. Expected Tools/lame/lame.exe beside the application.");
            return;
        }

        std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Mixdown",
                                                   "Session snapshot is not available.");
            return;
        }

        mini_daw_audio_mixdown::ActiveLoopMixdownSpan loopProbe {};
        const juce::Result spanProbe = mini_daw_audio_mixdown::resolveActiveLoopMixdownSpan(
            transport_.readCycleEnabledForUi(),
            snap->getLeftLocatorSamples(),
            snap->getRightLocatorSamples(),
            loopProbe);
        if (spanProbe.failed())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Audio Mixdown",
                                                   spanProbe.getErrorMessage());
            return;
        }

        MixdownStartPlan plan;
        plan.mp3 = mp3;
        const juce::File folder(pathEditor_.getText().trim());
        const juce::String base = sanitizeFileBaseName(nameEditor_.getText());
        plan.outputFile = folder.getChildFile(base + (mp3 ? ".mp3" : ".wav"));
        plan.mp3BitrateKbps = kbpsFromMp3BitrateComboId(mp3BitrateCombo_.getSelectedId());
        switch (waveBitsCombo_.getSelectedId())
        {
        case 16:
            plan.wavBits = mini_daw_audio_mixdown::MixdownWaveBits::Pcm16;
            break;
        case 32:
            plan.wavBits = mini_daw_audio_mixdown::MixdownWaveBits::IeeeFloat32;
            break;
        case 24:
        default:
            plan.wavBits = mini_daw_audio_mixdown::MixdownWaveBits::Pcm24;
            break;
        }

        // Save-before-export prompt (Slice 5) resolves first; the dialog stays open behind the
        // prompt so Cancel / a failed save leaves the user exactly where they were.
        const auto proceed = [safeThis = juce::Component::SafePointer<AudioMixdownDialogContent>(this),
                              plan] {
            if (safeThis == nullptr)
            {
                appendMixdownDiagnosticLine("export continuation skipped: dialog already destroyed");
                return;
            }
            safeThis->beginConfirmedExport(plan);
        };
        if (confirmSaveBeforeExport_ != nullptr)
        {
            confirmSaveBeforeExport_(proceed);
        }
        else
        {
            proceed();
        }
    }

    /// Export is confirmed (save prompt resolved). Ask about overwrite, then close the settings
    /// dialog and start the export from a queued continuation that no longer touches this content.
    void beginConfirmedExport(const MixdownStartPlan& plan)
    {
        if (!plan.outputFile.existsAsFile())
        {
            closeDialogAndStartExport(plan);
            return;
        }
        // Async prompt with callback (same pattern as the Slice 5 save prompts). The synchronous
        // NativeMessageBox::showYesNoBox returned instantly with an arbitrary answer here (see
        // mixdown-diag.log "export cancelled at overwrite prompt" ~10 ms after the click), so the
        // user never saw the question.
        appendMixdownDiagnosticLine("overwrite prompt shown path=\""
                                    + plan.outputFile.getFullPathName() + "\"");
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::WarningIcon,
            "Audio Mixdown",
            "\"" + plan.outputFile.getFileName() + "\" already exists.\n\n"
                + plan.outputFile.getFullPathName() + "\n\nDo you want to replace it?",
            "Replace",
            "Cancel",
            this,
            juce::ModalCallbackFunction::create(
                [safeThis = juce::Component::SafePointer<AudioMixdownDialogContent>(this),
                 plan](const int result) {
                    if (result != 1) // Cancel / dismissed
                    {
                        appendMixdownDiagnosticLine(
                            "export cancelled at overwrite prompt; dialog stays open");
                        writeLastOperationBreadcrumb(juce::String("mixdown ")
                                                     + (plan.mp3 ? "mp3" : "wav") + " cancelled");
                        return;
                    }
                    if (safeThis == nullptr)
                    {
                        appendMixdownDiagnosticLine(
                            "overwrite confirmed but dialog already destroyed; export skipped");
                        return;
                    }
                    appendMixdownDiagnosticLine("overwrite confirmed by user");
                    safeThis->closeDialogAndStartExport(plan);
                }));
    }

    /// Final step: close the settings dialog, then run the export from a queued continuation that
    /// no longer touches this (destroyed) content.
    void closeDialogAndStartExport(const MixdownStartPlan& plan)
    {
        // App-lifetime references (owned by the application object): safe to use from the queued
        // continuation after this dialog content is destroyed.
        Transport& transport = transport_;
        Session& session = session_;
        PlaybackEngine& playbackEngine = playbackEngine_;
        juce::AudioDeviceManager& deviceManager = deviceManager_;
        const std::function<void()> syncTransportUi = syncTransportUiFromDomain_;

        appendMixdownDiagnosticLine(juce::String("dialog closed before export start format=")
                                    + (plan.mp3 ? "mp3" : "wav"));
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        {
            dw->exitModalState(1);
        }
        juce::MessageManager::callAsync(
            [&transport, &session, &playbackEngine, &deviceManager, syncTransportUi, plan] {
                runConfirmedMixdownExport(
                    transport, session, playbackEngine, deviceManager, syncTransportUi, plan);
            });
    }

    Transport& transport_;
    Session& session_;
    PlaybackEngine& playbackEngine_;
    juce::AudioDeviceManager& deviceManager_;
    std::function<void()> syncTransportUiFromDomain_;
    /// Slice 5 unsaved-changes guard: invoked with the continuation that starts the export.
    std::function<void(std::function<void()>)> confirmSaveBeforeExport_;

    bool floatWaveSupportedAtDialogOpen_ = false;

    juce::Label nameLabel_;
    juce::TextEditor nameEditor_;

    juce::Label pathLabel_;
    juce::TextEditor pathEditor_;

    juce::Label previewLabel_;
    juce::Label previewPathLabel_;

    juce::Label fileTypeLabel_;
    juce::ComboBox fileTypeCombo_;

    juce::Label sampleRateLabel_;
    juce::Label sampleRateValueLabel_;

    juce::Label rangeLabel_;
    juce::Label rangeValueLabel_;

    juce::Label waveBitsLabel_;
    juce::ComboBox waveBitsCombo_;

    juce::Label mp3BitrateLabel_;
    juce::ComboBox mp3BitrateCombo_;

    juce::Label mp3StatusLabel_;

    juce::TextButton exportButton_;
    juce::TextButton cancelButton_;
};

} // namespace

namespace mini_daw_app_dialogs
{

void showAudioMixdownDialog(juce::Component& parent,
                            Transport& transport,
                            Session& session,
                            PlaybackEngine& playbackEngine,
                            juce::AudioDeviceManager& deviceManager,
                            std::function<void()> updatePlayPauseButtonFromTransport,
                            std::function<void(std::function<void()>)> confirmSaveBeforeExport)
{
    auto* body = new AudioMixdownDialogContent(transport,
                                               session,
                                               playbackEngine,
                                               deviceManager,
                                               std::move(updatePlayPauseButtonFromTransport),
                                               std::move(confirmSaveBeforeExport));

    juce::DialogWindow::LaunchOptions opt;
    opt.content.setOwned(body);
    opt.dialogTitle = "Audio Mixdown";
    opt.dialogBackgroundColour
        = parent.getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    opt.componentToCentreAround = &parent;
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = false;
    opt.launchAsync();
}

} // namespace mini_daw_app_dialogs
