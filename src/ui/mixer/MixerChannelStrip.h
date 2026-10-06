#pragma once

// =============================================================================
// MixerChannelStrip — one mixer channel bound to ONE TrackId (never to the active track)
// =============================================================================
//
// ROLE
//   Vertical channel strip for a row of any kind: header (name, kind, base buttons), Routing,
//   Pre-gain, Pre / Post inserts, Sends, fader + pan and the output meter, laid out in the bands
//   `mixer_layout::computeLayout` hands every strip (so all strips stay aligned). Every control
//   reads from the published snapshot and writes through `MixerStripBindings` with this strip's
//   TrackId — the same Session setters, undo wrapping, validation and recording refusal the
//   Inspector uses; the Inspector polls the same snapshot and follows.
//
// PER KIND (the model's rules, not new ones)
//   Audio: Audio Input / Output, pre-gain, inserts, sends, pan, fader, meter, Power / Mute /
//   Monitor / R. Instrument: MIDI Input + channel filter, MIDI Channel, Audio Output, inserts,
//   sends, pan, fader, meter, instrument editor / alternatives, Power / Mute / Monitor / R.
//   Midi: MIDI Input + filter, MIDI To, MIDI Channel, Power / Mute / Monitor / R — no audio
//   controls at all (no fader, pan, meter, inserts, sends, bus). Group: Audio Output, inserts,
//   sends, pan, fader, meter, Mute. Stereo Out: device output (information), inserts, fader,
//   Stereo Out meter, Mute — never sends, never an output selector.
//
// REFRESH DISCIPLINE
//   `refreshFromSession` runs on the owner's 10 Hz poll and after edits; it never rewrites a
//   control the user is dragging or typing in (fader drag, pan drag, focused text fields) and
//   rebuilds selector lists / insert rows only when their model changed.
//
// METERS
//   A `LevelMeterHub::Listener`: reports its row as interest while its meter is shown, pushes
//   the hub's windows into its `LevelMeterComponent`, forwards lamp clicks as acknowledgements.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "domain/Track.h"
#include "plugins/InsertSlotId.h"
#include "ui/ChannelFaderComponent.h"
#include "ui/InspectorPanControl.h"
#include "ui/LevelMeterComponent.h"
#include "ui/LevelMeterHub.h"
#include "ui/TrackStripButtonGlyphs.h"
#include "ui/mixer/MixerSectionLayout.h"
#include "ui/mixer/MixerStripBindings.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

class MixerChannelStrip final : public juce::Component,
                                public LevelMeterHub::Listener,
                                private juce::TextEditor::Listener
{
public:
    MixerChannelStrip(TrackId trackId, const MixerStripBindings& bindings, LevelMeterHub* hub);
    ~MixerChannelStrip() override;

    [[nodiscard]] TrackId trackId() const noexcept { return trackId_; }

    /// Apply the shared bands (owner computes them once per layout pass for every strip).
    void applyLayout(const mixer_layout::ComputedLayout& layout);

    /// Re-read this row from the snapshot; `isActive` paints the active-row highlight.
    void refreshFromSession(const SessionSnapshot& snap, bool isActive);

    /// True when the row left the snapshot (owner removes the strip).
    [[nodiscard]] bool isOrphaned() const noexcept { return orphaned_; }

    // --- LevelMeterHub::Listener -------------------------------------------------------------------
    void collectMeterInterest(std::vector<TrackId>& out) override;
    void meterWindowArrived(TrackId trackId, const level_meter::Reading& reading, double nowSeconds) override;
    void meterOverloadAcknowledged(TrackId trackId) override;
    void meterTick(double nowSeconds) override;

    // --- test / scenario surfaces -----------------------------------------------------------------
    [[nodiscard]] ChannelFaderComponent& fader() noexcept { return fader_; }
    [[nodiscard]] InspectorPanControl& pan() noexcept { return pan_; }
    [[nodiscard]] LevelMeterComponent& meter() noexcept { return meter_; }
    [[nodiscard]] juce::String nameText() const { return nameLabel_.getText(); }
    [[nodiscard]] juce::String kindText() const { return kindLabel_.getText(); }
    [[nodiscard]] TrackKind shownKind() const noexcept { return kind_; }
    /// Routing selector `row` (0 … 3): caption + current text ("" when the row is unused).
    [[nodiscard]] juce::String routingCaption(int row) const;
    [[nodiscard]] juce::String routingText(int row) const;
    [[nodiscard]] bool routingRowVisible(int row) const;
    /// Pick a routing item by its visible text through the combo's own `onChange` (false = absent).
    bool chooseRoutingByText(int row, const juce::String& itemText);
    [[nodiscard]] juce::String preGainText() const { return preGainEditor_.getText(); }
    bool isPreGainVisible() const { return preGainEditor_.isVisible(); }
    void commitPreGainText(const juce::String& text);
    [[nodiscard]] int visibleInsertRowCount(InsertStage stage) const;
    [[nodiscard]] juce::String insertRowText(InsertStage stage, int row) const;
    /// The insert row context-menu actions (1 open editor, 2 move up, 3 move down, 4 move to the
    /// other stage, 5 remove) — the menu calls this; tests call it directly. False = not applicable.
    bool performInsertRowAction(InsertStage stage, int row, int actionId);
    [[nodiscard]] bool sendRowVisible(int row) const;
    [[nodiscard]] juce::String sendDestinationText(int row) const;
    [[nodiscard]] juce::String sendAmountText(int row) const;
    [[nodiscard]] bool sendEnabled(int row) const;
    bool chooseSendDestinationByText(int row, const juce::String& itemText);
    void commitSendAmountText(int row, const juce::String& text);
    void clickBaseButtonForTest(track_strip_glyphs::StripButtonKind kind);
    [[nodiscard]] bool baseButtonVisible(track_strip_glyphs::StripButtonKind kind) const;
    [[nodiscard]] bool baseButtonActive(track_strip_glyphs::StripButtonKind kind) const;
    [[nodiscard]] juce::Rectangle<int> headerBounds() const noexcept { return layout_.header; }
    [[nodiscard]] juce::Rectangle<int> lowerBandBounds() const noexcept { return layout_.lowerBand; }
    /// Geometry check: every visible child lies inside the strip and inside its band; appends
    /// human-readable lines, returns false on the first violation.
    [[nodiscard]] bool verifyChildrenInsideBands(juce::String& report) const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    /// One square base-button cell drawn with the shared header glyphs.
    class CellButton final : public juce::Component,
                             public juce::SettableTooltipClient
    {
    public:
        explicit CellButton(track_strip_glyphs::StripButtonKind kind) { state_.kind = kind; }
        track_strip_glyphs::StripButtonState state_;
        std::function<void()> onClick;
        void paint(juce::Graphics& g) override;
        void mouseEnter(const juce::MouseEvent&) override { hover_ = true; repaint(); }
        void mouseExit(const juce::MouseEvent&) override { hover_ = false; repaint(); }
        void mouseUp(const juce::MouseEvent& e) override;

    private:
        bool hover_ = false;
    };

    /// One insert row: click opens the editor, right-click offers move / stage / remove.
    class InsertRowButton final : public juce::TextButton
    {
    public:
        InsertRowButton() = default;
        std::function<void(const juce::MouseEvent&)> onRightClick;
        void mouseUp(const juce::MouseEvent& e) override;
    };

    enum class RoutingRole
    {
        None,
        AudioInput,
        AudioOutput,
        MidiInput,
        MidiInputChannel,
        MidiOutputChannel,
        MidiDestination,
        DeviceOutputInfo,
    };

    struct RoutingRow
    {
        juce::Label caption;
        juce::ComboBox combo;
        juce::Label infoLabel; // Stereo Out: device output text
        RoutingRole role = RoutingRole::None;
        std::vector<TrackInputAssignment> inputValues;
        std::vector<TrackMidiInputAssignment> midiInputValues;
        std::vector<TrackId> trackIdValues;
        std::vector<int> intValues;
        bool guard = false;
    };

    struct SendRow
    {
        juce::ToggleButton enable;
        juce::ComboBox dest;
        juce::TextEditor amount;
        std::vector<TrackId> destIds;
        bool guard = false;
    };

    struct InsertStageUi
    {
        juce::Label caption;
        std::array<std::unique_ptr<InsertRowButton>, mixer_layout::kInsertRowsPerStage> rows;
        std::array<InsertSlotId, mixer_layout::kInsertRowsPerStage> rowSlots{};
        juce::Label moreLabel;
        juce::TextButton addButton;
        std::vector<InspectorInsertRow> lastRows;
        bool populated = false;
    };

    void configureRoutingRow(int row, RoutingRole role, const juce::String& caption);
    void populateRoutingRow(int row, const Track& track, const SessionSnapshot& snap);
    void routingRowChanged(int row);
    void applyKindLayout(TrackKind kind);
    void refreshBaseButtons(const Track& track);
    void refreshInserts(InsertStage stage, const std::vector<InspectorInsertRow>& rows);
    void showInsertRowMenu(InsertStage stage, int row, const juce::MouseEvent& e);
    void refreshSends(const Track& track, const SessionSnapshot& snap);
    void sendDestinationChanged(int row);
    void commitSendAmount(int row);
    void commitPreGain();
    void setPreGainEditorText(float db);
    [[nodiscard]] juce::String kindTag(TrackKind kind) const;

    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor& editor) override;

    const TrackId trackId_;
    const MixerStripBindings& bindings_;
    LevelMeterHub* meterHub_ = nullptr;
    mixer_layout::ComputedLayout layout_;
    TrackKind kind_ = TrackKind::Audio;
    bool kindApplied_ = false;
    bool isMaster_ = false;
    bool isActive_ = false;
    bool orphaned_ = false;
    float lastPreGainDb_ = 0.0f;

    // header
    juce::Label nameLabel_;
    juce::Label kindLabel_;
    CellButton instrumentEditorCell_{ track_strip_glyphs::StripButtonKind::InstrumentEditor };
    CellButton powerCell_{ track_strip_glyphs::StripButtonKind::Power };
    CellButton muteCell_{ track_strip_glyphs::StripButtonKind::Mute };
    CellButton monitorCell_{ track_strip_glyphs::StripButtonKind::Monitor };
    CellButton armCell_{ track_strip_glyphs::StripButtonKind::Arm };
    CellButton alternativesCell_{ track_strip_glyphs::StripButtonKind::Alternatives };

    // sections
    juce::Label routingCaption_;
    std::array<RoutingRow, mixer_layout::kRoutingRowsPerStrip> routing_;
    juce::Label preGainCaption_;
    juce::TextEditor preGainEditor_;
    juce::Label preGainUnitLabel_;
    InsertStageUi preInserts_;
    InsertStageUi postInserts_;
    juce::Label sendsCaption_;
    std::array<SendRow, mixer_layout::kSendRows> sends_;
    juce::Label sendsExtraLabel_;
    InspectorPanControl pan_;
    ChannelFaderComponent fader_;
    juce::Label meterCaption_;
    LevelMeterComponent meter_;
    bool faderGuard_ = false;
    bool panGuard_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerChannelStrip)
};
