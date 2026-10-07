// =============================================================================
// TrackHeaderView.cpp — model/callback-driven header; optional drag host
// =============================================================================
// Geometry summary (logical px; see the header and `ui/TrackRowHeightPresets.h`):
//   x: [0..8) group margin (active stripe 0..4, group marker 5..7) | colour segment (icon +
//      number, width from the shared digit column) | 3 gap | [Power][Mute][Solo] (22 px cells,
//      present ones collapsed left) | 4 gap | name … | 8 pad
//   y: 2 pad | title row 22 (icon, number, cells, name) | 3 gap | second row 22 (Monitor, Arm,
//      instrument editor, alternatives; only when the height fits) | … | 4 px resize band.
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "ui/ForbiddenCursor.h"
#include "ui/TrackColourPalette.h"

#include <algorithm>
#include <array>
#include <memory>
#include <juce_core/juce_core.h>

namespace
{
    constexpr float kHeaderDragThresholdPx = 3.0f;

    // Face / glyph vocabulary shared with the mixer strips (`ui/TrackStripButtonGlyphs.h`).
    using namespace track_strip_glyphs;

    /// Order number font (fits three 7 px digit advances in the segment's digit column).
    constexpr float kHeaderNumberFontHeight = 11.0f;

    static_assert(TrackHeaderView::kStripControlCellWidthPx == track_row_heights::kHeaderControlCellPx,
                  "the title row IS one control cell tall");
    static_assert(TrackHeaderView::kHeaderGroupMarginPx
                      >= TrackHeaderView::kHeaderGroupMarkerXPx + TrackHeaderView::kHeaderGroupMarkerWidthPx,
                  "the group marker must fit inside the group margin");
    static_assert(TrackHeaderView::kHeaderActiveStripeWidthPx == track_strip_glyphs::kHeaderActiveStripeWidthPx,
                  "header stripe width is shared with the mixer plate");
    // Micro = pad + title row + band: the title row fits with nothing to spare and no overlap
    // between the cells and the resize band.
    static_assert(track_row_heights::kMicroRowHeightPx
                      == TrackHeaderView::kHeaderRowTopPadPx + TrackHeaderView::kStripControlCellWidthPx
                             + TrackHeaderView::kHeaderResizeBandPx,
                  "Micro row height must equal the title row + pad + resize band");
} // namespace

juce::Rectangle<int>
TrackHeaderView::squareStripButtonBodyFromCell(juce::Rectangle<int> const cell) const noexcept
{
    if (cell.isEmpty())
    {
        return {};
    }
    const int cw = cell.getWidth();
    const int ch = cell.getHeight();
    if (cw <= TrackHeaderView::kStripSquareBodyInsetPx * 2
        || ch <= TrackHeaderView::kStripSquareBodyInsetPx * 2)
    {
        return {};
    }
    const int availW = cw - TrackHeaderView::kStripSquareBodyInsetPx * 2;
    const int availH = ch - TrackHeaderView::kStripSquareBodyInsetPx * 2;
    const int side = juce::jmin(availW, availH);
    if (side < 6)
    {
        return {};
    }
    const int cx = cell.getCentreX();
    const int cy = cell.getCentreY();
    const int ox = cx - side / 2;
    const int oy = cy - side / 2;

    auto out = juce::Rectangle<int>(ox, oy, side, side).getIntersection(cell);
    if (out.isEmpty())
    {
        return {};
    }
    return out;
}

const TrackHeaderView::TrackHeaderStripButtonSpec*
TrackHeaderView::findStripControlSpec(std::vector<TrackHeaderStripButtonSpec> const& specs,
                                      TrackHeaderButtonKind const kind) const noexcept
{
    for (auto const& s : specs)
    {
        if (s.kind == kind)
        {
            return &s;
        }
    }
    return nullptr;
}

juce::Rectangle<int>
TrackHeaderView::stripButtonCellBounds(TrackHeaderButtonKind const kind,
                                       std::vector<TrackHeaderStripButtonSpec> const& specs) const noexcept
{
    if (auto const* s = findStripControlSpec(specs, kind))
    {
        return s->cellBounds;
    }
    return {};
}

// -----------------------------------------------------------------------------------------------
// Cell presence (model-driven; a cell that is absent has NO bounds, hit area or tooltip)
// -----------------------------------------------------------------------------------------------
bool TrackHeaderView::hasPowerCell() const noexcept
{
    return modelProvider_().showRecordAndPowerStripCells;
}

bool TrackHeaderView::hasArmCell() const noexcept
{
    return modelProvider_().showRecordAndPowerStripCells;
}

bool TrackHeaderView::hasInstrumentEditorCell() const noexcept
{
    const auto m = modelProvider_();
    return m.showRecordAndPowerStripCells && callbacks_.onOpenInstrumentEditor != nullptr
           && m.instrumentEditorAvailable;
}

bool TrackHeaderView::hasAlternativesCell() const noexcept
{
    const auto m = modelProvider_();
    return m.showRecordAndPowerStripCells && callbacks_.onShowInstrumentAlternatives != nullptr
           && m.instrumentAlternativesAvailable;
}

bool TrackHeaderView::hasSoloCell() const noexcept
{
    // Model-driven like the other optional cells: every row kind except Master/Stereo Out sets
    // `soloAvailable`; the cell also needs the wired command (no greyed placeholder while the
    // owning view has no solo coordinator).
    const auto m = modelProvider_();
    return m.soloAvailable && callbacks_.onToggleSolo != nullptr;
}

bool TrackHeaderView::hasMonitorCell() const noexcept
{
    // Model-driven only (kind-checked by each row's provider on every poll): audio rows set
    // `monitorAvailable` + interactable, Instrument destination rows set available but NOT
    // interactable (disabled placeholder — no toggle callback exists there), and plain MIDI /
    // group / master rows leave it false so no cell, hit target, or tooltip appears.
    const auto m = modelProvider_();
    return m.showRecordAndPowerStripCells && m.monitorAvailable;
}

// -----------------------------------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------------------------------
TrackHeaderView::HeaderContentLayout TrackHeaderView::computeHeaderContentLayout() const noexcept
{
    HeaderContentLayout L{};
    const TrackHeaderModel m = modelProvider_();
    auto const b = getLocalBounds();
    if (b.isEmpty())
    {
        return L;
    }
    const int cell = kStripControlCellWidthPx;
    const int titleY = b.getY() + kHeaderRowTopPadPx;

    // Colour segment: right of the group margin, the FULL row height — from the row's top edge
    // down to the separator below (the resize band is a hit area only, never its own visible
    // strip, so the segment does not stop above it). Icon + number sit on the title row inside it.
    const int segW = colourSegmentWidthPxForDigits(m.trackNumberDigits);
    const int segX = b.getX() + kHeaderGroupMarginPx;
    L.colourSegmentBounds = { segX, b.getY(), segW, b.getHeight() };
    L.typeIconBounds = { segX + kHeaderSegmentPadPx, titleY + (cell - kHeaderTypeIconPx) / 2, kHeaderTypeIconPx,
                         kHeaderTypeIconPx };
    const int digitsW = segW - (kHeaderSegmentPadPx + kHeaderTypeIconPx + kHeaderSegmentPadPx + kHeaderSegmentPadPx);
    L.numberBounds = { segX + kHeaderSegmentPadPx + kHeaderTypeIconPx + kHeaderSegmentPadPx, titleY,
                       juce::jmax(0, digitsW), cell };

    // Title-row control slots (reserved for three cells on every kind → common name x).
    const int stripX = segX + segW + kHeaderSegmentToStripGapPx;
    L.titleStripBounds = { stripX, titleY, kHeaderTitleCellCount * cell, cell };

    const int nameX = stripX + kHeaderTitleCellCount * cell + kHeaderStripToNameGapPx;
    const int nameRight = b.getRight() - kHeaderOuterPadXPx;
    L.nameTextBounds = { nameX, titleY, juce::jmax(0, nameRight - nameX), cell };

    // Second row: only when it fits entirely above the resize band (Small and up).
    if (b.getHeight() >= kMinimumHeightForSecondRowPx)
    {
        const int secondY = titleY + cell + kHeaderRowGapPx;
        const int count = (hasMonitorCell() ? 1 : 0) + (hasArmCell() ? 1 : 0) + (hasInstrumentEditorCell() ? 1 : 0)
                          + (hasAlternativesCell() ? 1 : 0);
        L.secondStripBounds = { stripX, secondY, count * cell, cell };
    }
    return L;
}

juce::Rectangle<int> TrackHeaderView::visibleChromeBoundsExcludingResizeBand() const noexcept
{
    return getLocalBounds().withTrimmedBottom(kHeaderResizeBandPx);
}

bool TrackHeaderView::stripCellHitIntersectsVisibleChrome(juce::Rectangle<int> const cell,
                                                          juce::Point<int> const pos) const noexcept
{
    auto const hit = cell.getIntersection(visibleChromeBoundsExcludingResizeBand());
    return !hit.isEmpty() && hit.contains(pos);
}

juce::Rectangle<int> TrackHeaderView::titleStripCellAtIndex(int const index) const noexcept
{
    auto s = computeHeaderContentLayout().titleStripBounds;
    if (s.isEmpty() || index < 0 || index >= kHeaderTitleCellCount)
    {
        return {};
    }
    s.removeFromLeft(kStripControlCellWidthPx * index);
    return s.removeFromLeft(kStripControlCellWidthPx);
}

juce::Rectangle<int> TrackHeaderView::secondStripCellAtIndex(int const index) const noexcept
{
    auto s = computeHeaderContentLayout().secondStripBounds;
    if (s.isEmpty() || index < 0 || kStripControlCellWidthPx * (index + 1) > s.getWidth())
    {
        return {};
    }
    s.removeFromLeft(kStripControlCellWidthPx * index);
    return s.removeFromLeft(kStripControlCellWidthPx);
}

// Title row order (present cells collapse left): [Power?][Mute][Solo?]
juce::Rectangle<int> TrackHeaderView::getPowerButtonBounds() const noexcept
{
    return hasPowerCell() ? titleStripCellAtIndex(0) : juce::Rectangle<int>{};
}

juce::Rectangle<int> TrackHeaderView::getMuteButtonBounds() const noexcept
{
    return titleStripCellAtIndex(hasPowerCell() ? 1 : 0);
}

juce::Rectangle<int> TrackHeaderView::getSoloButtonBounds() const noexcept
{
    if (!hasSoloCell())
    {
        return {};
    }
    return titleStripCellAtIndex((hasPowerCell() ? 1 : 0) + 1);
}

// Second row order (present cells collapse left): [Monitor?][Arm?][InstrumentEditor?][Alternatives?]
juce::Rectangle<int> TrackHeaderView::getMonitorButtonBounds() const noexcept
{
    return hasMonitorCell() ? secondStripCellAtIndex(0) : juce::Rectangle<int>{};
}

juce::Rectangle<int> TrackHeaderView::getArmButtonBounds() const noexcept
{
    return hasArmCell() ? secondStripCellAtIndex(hasMonitorCell() ? 1 : 0) : juce::Rectangle<int>{};
}

juce::Rectangle<int> TrackHeaderView::getInstrumentEditorButtonBounds() const noexcept
{
    if (!hasInstrumentEditorCell())
    {
        return {};
    }
    return secondStripCellAtIndex((hasMonitorCell() ? 1 : 0) + (hasArmCell() ? 1 : 0));
}

juce::Rectangle<int> TrackHeaderView::getAlternativesButtonBounds() const noexcept
{
    if (!hasAlternativesCell())
    {
        return {};
    }
    return secondStripCellAtIndex((hasMonitorCell() ? 1 : 0) + (hasArmCell() ? 1 : 0)
                                  + (hasInstrumentEditorCell() ? 1 : 0));
}

juce::Rectangle<int> TrackHeaderView::getColourSegmentBounds() const noexcept
{
    return computeHeaderContentLayout().colourSegmentBounds;
}

juce::Rectangle<int> TrackHeaderView::getTypeIconBounds() const noexcept
{
    return computeHeaderContentLayout().typeIconBounds;
}

juce::Rectangle<int> TrackHeaderView::getTrackNumberBounds() const noexcept
{
    return computeHeaderContentLayout().numberBounds;
}

juce::Rectangle<int> TrackHeaderView::getNameTextBounds() const noexcept
{
    return computeHeaderContentLayout().nameTextBounds;
}

juce::Rectangle<int> TrackHeaderView::getSecondRowStripBounds() const noexcept
{
    return computeHeaderContentLayout().secondStripBounds;
}

bool TrackHeaderView::isPositionInColourSegment(juce::Point<int> const pos) const noexcept
{
    const juce::Rectangle<int> seg = computeHeaderContentLayout().colourSegmentBounds;
    return !seg.isEmpty() && seg.contains(pos) && !isPositionInRowResizeBand(pos);
}

// -----------------------------------------------------------------------------------------------
// Strip specs
// -----------------------------------------------------------------------------------------------
std::vector<TrackHeaderView::TrackHeaderStripButtonSpec>
TrackHeaderView::buildStripControlSpecs() const noexcept
{
    const TrackHeaderModel m = modelProvider_();
    std::vector<TrackHeaderStripButtonSpec> specs;
    specs.reserve(7);

    if (hasPowerCell())
    {
        TrackHeaderStripButtonSpec p{};
        p.kind = TrackHeaderButtonKind::Power;
        p.enabled = callbacks_.onTogglePower != nullptr && m.powerInteractable;
        p.powerStandby = m.off;
        p.cellBounds = getPowerButtonBounds();
        specs.push_back(std::move(p));
    }
    {
        TrackHeaderStripButtonSpec mu{};
        mu.kind = TrackHeaderButtonKind::Mute;
        // While solo is active the Mute command path is locked: the cell ignores clicks but still
        // shows the EFFECTIVE state (stored mute / solo-silenced tint / audible) plus a small lock.
        mu.enabled = callbacks_.onToggleMute != nullptr && m.muteInteractable && !m.muteLockedBySolo;
        mu.muteActive = m.muted;
        mu.muteSoloSilenced = m.soloSilenced;
        mu.muteLocked = m.muteLockedBySolo;
        mu.cellBounds = getMuteButtonBounds();
        specs.push_back(std::move(mu));
    }
    if (hasSoloCell())
    {
        TrackHeaderStripButtonSpec so{};
        so.kind = TrackHeaderButtonKind::Solo;
        so.enabled = true; // presence already implies callback + model availability
        so.soloActive = m.soloed;
        so.cellBounds = getSoloButtonBounds();
        specs.push_back(std::move(so));
    }
    // Second row (each getter returns empty bounds when the row is hidden → no hit, no paint).
    if (hasMonitorCell())
    {
        TrackHeaderStripButtonSpec mo{};
        mo.kind = TrackHeaderButtonKind::Monitor;
        mo.enabled = callbacks_.onToggleMonitor != nullptr && m.monitorInteractable;
        mo.monitorActive = m.monitorEnabled;
        mo.cellBounds = getMonitorButtonBounds();
        specs.push_back(std::move(mo));
    }
    if (hasArmCell())
    {
        TrackHeaderStripButtonSpec a{};
        a.kind = TrackHeaderButtonKind::Arm;
        a.enabled = callbacks_.onToggleArm != nullptr && m.armInteractable;
        a.armActive = m.armed;
        a.cellBounds = getArmButtonBounds();
        specs.push_back(std::move(a));
    }
    if (hasInstrumentEditorCell())
    {
        TrackHeaderStripButtonSpec s{};
        s.kind = TrackHeaderButtonKind::InstrumentEditor;
        s.enabled = callbacks_.onOpenInstrumentEditor != nullptr && m.instrumentEditorAvailable;
        s.cellBounds = getInstrumentEditorButtonBounds();
        specs.push_back(s);
    }
    if (hasAlternativesCell())
    {
        TrackHeaderStripButtonSpec al{};
        al.kind = TrackHeaderButtonKind::Alternatives;
        al.enabled = callbacks_.onShowInstrumentAlternatives != nullptr && m.instrumentAlternativesAvailable;
        al.cellBounds = getAlternativesButtonBounds();
        specs.push_back(std::move(al));
    }
    // Drop every spec whose cell is hidden (empty bounds): nothing to paint, hit or tooltip.
    specs.erase(std::remove_if(specs.begin(), specs.end(),
                               [](const TrackHeaderStripButtonSpec& s) { return s.cellBounds.isEmpty(); }),
                specs.end());
    return specs;
}

void TrackHeaderView::drawStripControlButton(juce::Graphics& g,
                                             TrackHeaderStripButtonSpec const& spec,
                                             bool const hoverThis,
                                             juce::Colour const& ctlEdgeNeutral) noexcept
{
    if (spec.cellBounds.isEmpty())
    {
        return;
    }

    juce::Rectangle<int> bodyPx = squareStripButtonBodyFromCell(spec.cellBounds);
    if (bodyPx.isEmpty())
    {
        return;
    }

    auto const rf = bodyPx.toFloat();
    const bool showHoverBrighten = hoverThis && spec.enabled;
    const juce::Colour edgeInactiveStroke(0xc0222222);

    switch (spec.kind)
    {
    case TrackHeaderButtonKind::InstrumentEditor:
        drawStandardStripButtonFace(g,
                                    rf,
                                    juce::Colour(0xff5c5f66),
                                    ctlEdgeNeutral,
                                    showHoverBrighten);
        drawInstrumentPianoGlyphCubaseSimple(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx));
        break;

    case TrackHeaderButtonKind::Power:
        // On/Off appearance follows `spec.powerStandby` only (`TrackHeaderModel::off`). Interactivity is
        // `spec.enabled`; it affects hover brighten via `showHoverBrighten`, not base body/glyph colors.
        drawStandardStripButtonFace(g,
                                    rf,
                                    spec.powerStandby ? juce::Colour(0xff5a5858) : juce::Colour(0xff2d9d53),
                                    ctlEdgeNeutral,
                                    showHoverBrighten);
        drawPowerGlyphInSquare(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx), juce::Colour(0xfff2f6f9));
        break;

    case TrackHeaderButtonKind::Mute:
    {
        // Shared M rendering (arrangement + mixer): effective-state display + lock while solo is
        // active lives in `track_strip_glyphs::drawStripButton` only.
        StripButtonState s{};
        s.kind = StripButtonKind::Mute;
        s.enabled = spec.enabled;
        s.active = spec.muteActive;
        s.hovered = hoverThis;
        s.soloSilenced = spec.muteSoloSilenced;
        s.lockMarked = spec.muteLocked;
        drawStripButton(g, bodyPx, s, ctlEdgeNeutral);
        break;
    }

    case TrackHeaderButtonKind::Solo:
    {
        StripButtonState s{};
        s.kind = StripButtonKind::Solo;
        s.enabled = spec.enabled;
        s.active = spec.soloActive;
        s.hovered = hoverThis;
        drawStripButton(g, bodyPx, s, ctlEdgeNeutral);
        break;
    }

    case TrackHeaderButtonKind::Monitor:
        // Input monitoring: orange body while monitoring is ON; neutral grey (still clickable)
        // while OFF — same face/hover conventions as the Mute/Arm cells.
        if (spec.enabled)
        {
            drawStandardStripButtonFace(g,
                                        rf,
                                        spec.monitorActive ? juce::Colour(0xffe07b18)
                                                           : juce::Colour(0xff5a5858),
                                        ctlEdgeNeutral,
                                        showHoverBrighten);
            drawMonitorSpeakerGlyph(g,
                                    nonLetterGlyphAreaFromSquareBodyPx(bodyPx),
                                    spec.monitorActive ? juce::Colour(0xff141414)
                                                       : juce::Colour(0xffeaeaea));
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(0xff3e3e3e), edgeInactiveStroke, false);
            drawMonitorSpeakerGlyph(g,
                                    nonLetterGlyphAreaFromSquareBodyPx(bodyPx),
                                    juce::Colour(0xff7a7a7a));
        }
        break;

    case TrackHeaderButtonKind::Arm:
        if (spec.enabled)
        {
            drawStandardStripButtonFace(g,
                                        rf,
                                        spec.armActive ? juce::Colour(0xffd01818) : juce::Colour(0xff5a5858),
                                        ctlEdgeNeutral,
                                        showHoverBrighten);
            drawStripLetter(g, bodyPx, "R", spec.armActive ? juce::Colour(0xfff8f8ff) : juce::Colour(0xffeaeaea));
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(0xff3e3e3e), edgeInactiveStroke, false);
            drawStripLetter(g, bodyPx, "R", juce::Colour(0xff888888));
        }
        break;

    case TrackHeaderButtonKind::Alternatives:
        drawStandardStripButtonFace(g,
                                    rf,
                                    juce::Colour(0xff4f545c),
                                    ctlEdgeNeutral,
                                    showHoverBrighten);
        drawAlternativesLayersGlyph(g, nonLetterGlyphAreaFromSquareBodyPx(bodyPx));
        break;
    }
}

void TrackHeaderView::repaintStripHoverCell(std::optional<TrackHeaderButtonKind> kind) noexcept
{
    auto const specs = buildStripControlSpecs();
    auto const bounds = [&](std::optional<TrackHeaderButtonKind> const k) -> juce::Rectangle<int> {
        if (!k.has_value())
        {
            return {};
        }
        return stripButtonCellBounds(*k, specs);
    };
    auto const r = bounds(kind);
    if (!r.isEmpty())
    {
        repaint(r.expanded(3));
    }
}

void TrackHeaderView::updateStripHoverFromPosition(juce::Point<int> const pos) noexcept
{
    auto const specs = buildStripControlSpecs();
    std::optional<TrackHeaderButtonKind> next;

    for (auto const& s : specs)
    {
        if (s.enabled && stripCellHitIntersectsVisibleChrome(s.cellBounds, pos))
        {
            next = s.kind;
            break;
        }
    }

    if (stripHoveredButton_ == next)
    {
        return;
    }

    repaintStripHoverCell(stripHoveredButton_);
    stripHoveredButton_ = next;
    repaintStripHoverCell(stripHoveredButton_);
}

void TrackHeaderView::clearStripHover() noexcept
{
    if (!stripHoveredButton_.has_value())
    {
        return;
    }
    repaintStripHoverCell(stripHoveredButton_);
    stripHoveredButton_.reset();
}

bool TrackHeaderView::isPositionInRowResizeBand(juce::Point<int> const position) const noexcept
{
    if (callbacks_.onRowHeightDrag == nullptr)
    {
        return false;
    }

    auto const b = getLocalBounds();
    const int h = b.getHeight();
    if (h <= 0)
    {
        return false;
    }

    const int bandH = juce::jmin(kHeaderResizeBandPx, h);
    const int bandTop = b.getBottom() - bandH;
    // The band is the bottom 4 px of the header at EVERY height; the title row ends above it even
    // in Micro (28 = 2 + 22 + 4), so the band never overlaps a control cell.
    return position.y >= bandTop;
}

TrackHeaderView::TrackHeaderView(TrackHeaderModelProvider modelProvider,
                                 TrackHeaderCallbacks callbacks,
                                 TrackId dragTrackId,
                                 std::optional<TrackHeaderDragHost> dragHost) noexcept
    : modelProvider_(std::move(modelProvider))
    , callbacks_(std::move(callbacks))
    , dragTrackId_(dragTrackId)
    , dragHost_(std::move(dragHost))
{
    if (dragHost_.has_value())
    {
        jassert(dragTrackId_ != kInvalidTrackId);
        jassert(dragHost_->onHeaderDragBegan != nullptr);
        jassert(dragHost_->onHeaderDragMoved != nullptr);
        jassert(dragHost_->onHeaderDragEnded != nullptr);
    }
}

void TrackHeaderView::setHeaderReorderDrag(
    std::optional<TrackHeaderDragHost> host,
    TrackId const dragTrackIdForwarded) noexcept
{
    dragHost_ = std::move(host);
    dragTrackId_ = dragTrackIdForwarded;
    if (dragHost_.has_value())
    {
        jassert(dragTrackId_ != kInvalidTrackId);
        jassert(dragHost_->onHeaderDragBegan != nullptr);
        jassert(dragHost_->onHeaderDragMoved != nullptr);
        jassert(dragHost_->onHeaderDragEnded != nullptr);
    }
}

void TrackHeaderView::patchRenameCallbacks(std::function<bool()> canBeginRenameTrack,
                                           std::function<bool(juce::String trimmedNewName)>
                                               onCommitRenameTrack) noexcept
{
    callbacks_.canBeginRenameTrack = std::move(canBeginRenameTrack);
    callbacks_.onCommitRenameTrack = std::move(onCommitRenameTrack);
}

juce::String TrackHeaderView::getTooltip()
{
    // Strip-cell tooltips (hover state is kept current by mouseMove via
    // updateStripHoverFromPosition). A hidden cell can never be hovered (no bounds).
    if (stripHoveredButton_.has_value()
        && *stripHoveredButton_ == TrackHeaderButtonKind::Alternatives)
    {
        return "Instrument alternatives";
    }
    // No Monitor tooltip: a speaker toggle next to Mute/Record is self-explanatory, and no other
    // strip button has one — a lone tooltip there reads as an inconsistency, not as help.
    return {};
}

void TrackHeaderView::ensureTrackNameEditor()
{
    if (trackNameEditor_ != nullptr)
    {
        return;
    }
    trackNameEditor_ = std::make_unique<juce::TextEditor>();
    trackNameEditor_->setMultiLine(false);
    trackNameEditor_->setReturnKeyStartsNewLine(false);
    trackNameEditor_->setScrollbarsShown(false);
    trackNameEditor_->setFont(juce::Font(juce::FontOptions(kHeaderNameFontHeight)));
    trackNameEditor_->setIndents(4, 2);
    trackNameEditor_->setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    trackNameEditor_->setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    trackNameEditor_->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::skyblue);
    addChildComponent(*trackNameEditor_);
    trackNameEditor_->setAlwaysOnTop(true);
    trackNameEditor_->setVisible(false);
}

void TrackHeaderView::layoutInlineTrackNameEditor()
{
    if (trackNameEditor_ == nullptr || !trackNameEditor_->isVisible())
    {
        return;
    }
    trackNameEditor_->setBounds(computeHeaderContentLayout().nameTextBounds);
}

void TrackHeaderView::beginInlineTrackRenameIfPossible(juce::Point<int> const clickLocal)
{
    if (callbacks_.onCommitRenameTrack == nullptr)
    {
        return;
    }
    if (!modelProvider_().trackNameRenameEnabled)
    {
        return;
    }
    if (callbacks_.canBeginRenameTrack != nullptr && !callbacks_.canBeginRenameTrack())
    {
        return;
    }
    if (trackNameEditor_ != nullptr && trackNameEditor_->isVisible())
    {
        return;
    }
    if (isPositionInRowResizeBand(clickLocal))
    {
        return;
    }
    auto const layout = computeHeaderContentLayout();
    if (layout.nameTextBounds.isEmpty() || !layout.nameTextBounds.contains(clickLocal))
    {
        return;
    }
    ensureTrackNameEditor();
    trackNameEditor_->setText(modelProvider_().name, juce::dontSendNotification);
    trackNameEditor_->setBounds(layout.nameTextBounds);
    trackNameEditor_->addListener(this);
    trackNameEditor_->setVisible(true);
    trackNameEditor_->grabKeyboardFocus();
    dragBlocker_ = DragBlocker::InlineRename;
    repaint();
}

void TrackHeaderView::cancelInlineTrackRename() noexcept
{
    ignoreTrackNameEditorFocusLoss_ = true;
    if (trackNameEditor_ != nullptr)
    {
        trackNameEditor_->removeListener(this);
        trackNameEditor_->setText(modelProvider_().name, juce::dontSendNotification);
        trackNameEditor_->setVisible(false);
    }
    dragBlocker_ = DragBlocker::None;
    repaint();
    ignoreTrackNameEditorFocusLoss_ = false;
}

void TrackHeaderView::submitInlineTrackRenameFromEditor()
{
    if (trackNameRenameSubmitting_ || trackNameEditor_ == nullptr || !trackNameEditor_->isVisible())
    {
        return;
    }
    trackNameRenameSubmitting_ = true;
    const juce::String trimmed = trackNameEditor_->getText().trim();
    trackNameEditor_->removeListener(this);
    trackNameEditor_->setVisible(false);
    dragBlocker_ = DragBlocker::None;
    repaint();

    if (!trimmed.isEmpty() && callbacks_.onCommitRenameTrack != nullptr)
    {
        juce::ignoreUnused(callbacks_.onCommitRenameTrack(trimmed));
    }

    trackNameRenameSubmitting_ = false;
}

void TrackHeaderView::textEditorReturnKeyPressed(juce::TextEditor& e)
{
    juce::ignoreUnused(e);
    submitInlineTrackRenameFromEditor();
}

void TrackHeaderView::textEditorEscapeKeyPressed(juce::TextEditor& e)
{
    juce::ignoreUnused(e);
    cancelInlineTrackRename();
}

void TrackHeaderView::textEditorFocusLost(juce::TextEditor& e)
{
    if (trackNameEditor_.get() != &e)
    {
        return;
    }
    if (ignoreTrackNameEditorFocusLoss_ || trackNameRenameSubmitting_)
    {
        return;
    }
    submitInlineTrackRenameFromEditor();
}

void TrackHeaderView::resized()
{
    layoutInlineTrackNameEditor();
}

void TrackHeaderView::mouseDoubleClick(juce::MouseEvent const& e)
{
    if (e.mods.isPopupMenu())
    {
        return;
    }
    beginInlineTrackRenameIfPossible(e.getPosition());
}

// -----------------------------------------------------------------------------------------------
// Paint
// -----------------------------------------------------------------------------------------------
void TrackHeaderView::paint(juce::Graphics& g)
{
    TrackHeaderModel const m = modelProvider_();

    auto const specs = buildStripControlSpecs();

    auto const active = m.active;

    auto const b = getLocalBounds();
    // Plate colours / stripe shared with the mixer strip headers (`TrackStripButtonGlyphs.h`).
    drawHeaderPlate(g, b, active);

    // Header multi-selection (visual-group creation): ONE flat blue wash over the whole plate,
    // under the name/strip content — no hairline or outline (a 1 px frame read as a second,
    // blue separator above the black row separator). Deliberately distinct from (and additive
    // to) the 4 px active-track stripe: the active row keeps its own look inside a multi-selection.
    if (m.headerMultiSelected)
    {
        g.setColour(juce::Colour(0x2d2e7bd6));
        // The active row's 4 px stripe stays pure (the clear left marker of the active track).
        g.fillRect(active ? b.withTrimmedLeft(kHeaderActiveStripeWidthPx) : b);
    }
    // Visual group membership: a discreet 2 px vertical marker in the group margin, RIGHT of the
    // active stripe (x 0…4) and LEFT of the colour segment (x ≥ 8) — obscures nothing.
    if (m.visualGroupMember)
    {
        g.setColour(juce::Colour(0xff6f8096));
        g.fillRect(kHeaderGroupMarkerXPx, 0, kHeaderGroupMarkerWidthPx, b.getHeight());
    }

    auto const layout = computeHeaderContentLayout();
    auto const chrome = visibleChromeBoundsExcludingResizeBand();

    // Colour segment: the track's palette colour as a full-chrome-height band with the type icon
    // and the order number on the title row. The header plate itself keeps its colour (only the
    // segment and the track's events carry the track colour); the active stripe stays separate.
    if (!layout.colourSegmentBounds.isEmpty())
    {
        g.setColour(track_colour_palette::headerSegmentFill(m.colourKey));
        g.fillRect(layout.colourSegmentBounds);
        const juce::Colour ink = track_colour_palette::segmentInk();
        drawTrackTypeIcon(g, layout.typeIconBounds.toFloat(), m.typeIcon, ink);
        if (m.trackNumber > 0 && !layout.numberBounds.isEmpty())
        {
            juce::Graphics::ScopedSaveState const gs(g);
            g.reduceClipRegion(layout.colourSegmentBounds.getIntersection(chrome));
            g.setColour(ink);
            g.setFont(juce::Font(juce::FontOptions(kHeaderNumberFontHeight)));
            g.drawText(juce::String(m.trackNumber), layout.numberBounds, juce::Justification::centredRight, false);
        }
    }

    auto nameArea = layout.nameTextBounds;
    // Rows that can show the live-MIDI activity dot keep its slot free at all times so the
    // ellipsized title never jumps or collides with the dot when MIDI starts arriving.
    constexpr int kDot = 6;
    if (m.typeIcon == TrackTypeIcon::Instrument || m.typeIcon == TrackTypeIcon::Midi)
    {
        nameArea = nameArea.withTrimmedRight(kDot + 3);
    }
    if (!nameArea.isEmpty()
        && (trackNameEditor_ == nullptr || !trackNameEditor_->isVisible()))
    {
        // Same 14 pt as before; a narrow header truncates with an ellipsis instead of shrinking.
        g.setColour(headerNameColour());
        g.setFont(kHeaderNameFontHeight);
        g.drawText(m.name, nameArea, juce::Justification::centredLeft, true);
    }
    // Live-MIDI activity dot: a 6 px disc at the right end of the name row while MIDI is
    // arriving for this row. Painted only when active so idle headers are byte-identical to before.
    if (m.midiActivity && !layout.nameTextBounds.isEmpty())
    {
        const juce::Rectangle<int> dot(layout.nameTextBounds.getRight() - kDot - 1,
                                       layout.nameTextBounds.getCentreY() - kDot / 2, kDot, kDot);
        g.setColour(juce::Colour(0xff3ddc84));
        g.fillEllipse(dot.toFloat());
    }

    juce::Colour const ctlNeutralEdge(kCtlNeutralEdgeArgb);

    {
        juce::Graphics::ScopedSaveState const gs(g);
        g.reduceClipRegion(chrome);

        // EVERY cell kind in `specs` is painted here (hit testing / hover / tooltips work from the
        // same spec list, so a kind missing from this list would be an invisible-but-clickable
        // button — the original Monitor-button defect).
        constexpr std::array<TrackHeaderButtonKind, 7> paintOrderBottomToTop{{
            TrackHeaderButtonKind::Mute,
            TrackHeaderButtonKind::Solo,
            TrackHeaderButtonKind::Monitor,
            TrackHeaderButtonKind::Arm,
            TrackHeaderButtonKind::Alternatives,
            TrackHeaderButtonKind::InstrumentEditor,
            TrackHeaderButtonKind::Power,
        }};
        auto const hovered = stripHoveredButton_;
        for (auto const kind : paintOrderBottomToTop)
        {
            TrackHeaderStripButtonSpec const* const s = findStripControlSpec(specs, kind);
            if (s == nullptr)
            {
                continue;
            }
            bool const hilite = hovered.has_value() && (*hovered == kind);

            drawStripControlButton(g, *s, hilite, ctlNeutralEdge);
        }
    }
}

// -----------------------------------------------------------------------------------------------
// Mouse
// -----------------------------------------------------------------------------------------------
bool TrackHeaderView::dispatchStripClick(juce::Point<int> const position,
                                         std::vector<TrackHeaderStripButtonSpec>&& specs) noexcept
{
    for (auto const& spec : specs)
    {
        if (!spec.enabled || spec.cellBounds.isEmpty()
            || !stripCellHitIntersectsVisibleChrome(spec.cellBounds, position))
        {
            continue;
        }

        switch (spec.kind)
        {
        case TrackHeaderButtonKind::InstrumentEditor:
            if (callbacks_.onOpenInstrumentEditor != nullptr)
            {
                callbacks_.onOpenInstrumentEditor();
            }
            return true;

        case TrackHeaderButtonKind::Power:
            if (callbacks_.onTogglePower != nullptr && callbacks_.onTogglePower())
            {
                dragBlocker_ = DragBlocker::Power;
            }
            return true;

        case TrackHeaderButtonKind::Mute:
            if (callbacks_.onToggleMute != nullptr)
            {
                dragBlocker_ = DragBlocker::Mute;
                callbacks_.onToggleMute();
            }
            return true;

        case TrackHeaderButtonKind::Solo:
            if (callbacks_.onToggleSolo != nullptr)
            {
                dragBlocker_ = DragBlocker::Solo;
                callbacks_.onToggleSolo();
            }
            return true;

        case TrackHeaderButtonKind::Monitor:
            if (callbacks_.onToggleMonitor != nullptr)
            {
                dragBlocker_ = DragBlocker::Monitor;
                callbacks_.onToggleMonitor();
            }
            return true;

        case TrackHeaderButtonKind::Arm:
            if (callbacks_.onToggleArm != nullptr)
            {
                dragBlocker_ = DragBlocker::Arm;
                callbacks_.onToggleArm();
            }
            return true;

        case TrackHeaderButtonKind::Alternatives:
            if (callbacks_.onShowInstrumentAlternatives != nullptr)
            {
                // Anchor the popup to the clicked cell (screen coordinates).
                callbacks_.onShowInstrumentAlternatives(localAreaToGlobal(spec.cellBounds));
            }
            return true;

        default:
            jassertfalse;
            return false;
        }
    }

    return false;
}

bool TrackHeaderView::clickStripCellLikeMouse(const TrackHeaderButtonKind kind)
{
    auto specs = buildStripControlSpecs();
    TrackHeaderStripButtonSpec const* const spec = findStripControlSpec(specs, kind);
    if (spec == nullptr || !spec->enabled || spec->cellBounds.isEmpty())
    {
        return false;
    }
    const juce::Point<int> centre = spec->cellBounds.getCentre();
    return dispatchStripClick(centre, std::move(specs));
}

bool TrackHeaderView::clickMonitorCellLikeMouseForStabilityTest() { return clickStripCellLikeMouse(TrackHeaderButtonKind::Monitor); }
bool TrackHeaderView::clickArmCellLikeMouseForStabilityTest() { return clickStripCellLikeMouse(TrackHeaderButtonKind::Arm); }
bool TrackHeaderView::clickMuteCellLikeMouseForStabilityTest() { return clickStripCellLikeMouse(TrackHeaderButtonKind::Mute); }
bool TrackHeaderView::clickSoloCellLikeMouseForStabilityTest() { return clickStripCellLikeMouse(TrackHeaderButtonKind::Solo); }
bool TrackHeaderView::clickPowerCellLikeMouseForStabilityTest() { return clickStripCellLikeMouse(TrackHeaderButtonKind::Power); }

namespace
{
    [[nodiscard]] juce::MouseEvent makeRightButtonPressAt(juce::Component& target, const juce::Point<float> local)
    {
        const juce::Time now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                                local,
                                juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier
                                                   | juce::ModifierKeys::popupMenuClickModifier),
                                juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation,
                                juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX,
                                juce::MouseInputSource::defaultTiltY,
                                &target,
                                &target,
                                now,
                                local,
                                now,
                                1,
                                false);
    }
} // namespace

bool TrackHeaderView::showContextMenuLikeRightClickForStabilityTest()
{
    if (callbacks_.onShowContextMenu == nullptr)
    {
        return false;
    }
    // The name area (never the colour segment, whose right-click is the palette).
    const juce::Rectangle<int> name = computeHeaderContentLayout().nameTextBounds;
    const juce::Point<float> local = (name.isEmpty() ? getLocalBounds().getCentre() : name.getCentre()).toFloat();
    mouseDown(makeRightButtonPressAt(*this, local));
    return true;
}

bool TrackHeaderView::showColourMenuLikeRightClickForStabilityTest()
{
    if (callbacks_.onShowColourMenu == nullptr)
    {
        return false;
    }
    const juce::Rectangle<int> seg = computeHeaderContentLayout().colourSegmentBounds;
    if (seg.isEmpty())
    {
        return false;
    }
    mouseDown(makeRightButtonPressAt(*this, seg.getCentre().toFloat()));
    return true;
}

void TrackHeaderView::mouseDown(juce::MouseEvent const& e)
{
    if (e.mods.isPopupMenu())
    {
        dragBlocker_ = DragBlocker::None;
        headerDragInProgress_ = false;
        // Right-click on the type-icon / number segment: the track colour palette (for THIS
        // header's track, whichever row is active). Elsewhere: the generic context menu.
        if (callbacks_.onShowColourMenu != nullptr && isPositionInColourSegment(e.getPosition()))
        {
            callbacks_.onShowColourMenu(*this, localAreaToGlobal(computeHeaderContentLayout().colourSegmentBounds));
            return;
        }
        if (callbacks_.onShowContextMenu != nullptr)
        {
            callbacks_.onShowContextMenu(*this, e);
        }
        return;
    }

    if (isPositionInRowResizeBand(e.getPosition()))
    {
        dragBlocker_ = DragBlocker::RowResize;
        headerDragInProgress_ = false;
        rowResizeStartHeightPx_ = getHeight();
        rowResizeStartScreenY_ = e.getScreenY();
        return;
    }

    if (dispatchStripClick(e.getPosition(), buildStripControlSpecs()))
    {
        return;
    }

    dragBlocker_ = DragBlocker::None;
    headerDragInProgress_ = false;
    // Header multi-selection rides on the same press that activates the row. Plain click:
    // activate this row (Inspector target) AND select exactly this header (anchor moves here).
    // Shift-click: extend the contiguous selection from the anchor WITHOUT moving the active
    // track — the Inspector keeps editing the single active track while a range is built.
    const bool shiftRange
        = e.mods.isShiftDown() && callbacks_.onHeaderSelectionClick != nullptr;
    if (!shiftRange && callbacks_.onActivateName != nullptr)
    {
        callbacks_.onActivateName();
    }
    if (callbacks_.onHeaderSelectionClick != nullptr)
    {
        callbacks_.onHeaderSelectionClick(shiftRange);
    }
}

void TrackHeaderView::mouseDrag(juce::MouseEvent const& e)
{
    if (!e.mods.isLeftButtonDown())
    {
        return;
    }

    if (dragBlocker_ == DragBlocker::InlineRename)
    {
        return;
    }

    if (dragBlocker_ == DragBlocker::RowResize)
    {
        if (callbacks_.onRowHeightDrag != nullptr)
        {
            callbacks_.onRowHeightDrag(rowResizeStartHeightPx_, e.getScreenY() - rowResizeStartScreenY_);
        }
        return;
    }

    if (!dragHost_.has_value())
    {
        return;
    }
    if (dragBlocker_ != DragBlocker::None)
    {
        return;
    }
    if (e.getDistanceFromDragStart() > kHeaderDragThresholdPx)
    {
        if (!headerDragInProgress_)
        {
            headerDragInProgress_ = true;
            dragHost_->onHeaderDragBegan(dragTrackId_, this);
        }
        juce::Point<int> const screen(e.getScreenX(), e.getScreenY());
        dragHost_->onHeaderDragMoved(dragTrackId_, screen);
    }
}

void TrackHeaderView::mouseMove(juce::MouseEvent const& e)
{
    if (dragBlocker_ == DragBlocker::RowResize)
    {
        setMouseCursor(juce::MouseCursor(juce::MouseCursor::UpDownResizeCursor));
        return;
    }

    if (dragBlocker_ == DragBlocker::InlineRename)
    {
        setMouseCursor(juce::MouseCursor(juce::MouseCursor::NormalCursor));
        return;
    }

    if (isPositionInRowResizeBand(e.getPosition()))
    {
        setMouseCursor(juce::MouseCursor(juce::MouseCursor::UpDownResizeCursor));
        clearStripHover();
        return;
    }

    setMouseCursor(juce::MouseCursor(juce::MouseCursor::NormalCursor));

    updateStripHoverFromPosition(e.getPosition());
}

void TrackHeaderView::mouseExit(juce::MouseEvent const& e)
{
    juce::ignoreUnused(e);
    if (dragBlocker_ != DragBlocker::RowResize)
    {
        setMouseCursor(juce::MouseCursor(juce::MouseCursor::NormalCursor));
    }
    clearStripHover();
}

void TrackHeaderView::mouseUp(juce::MouseEvent const& e)
{
    if (dragBlocker_ == DragBlocker::RowResize)
    {
        dragBlocker_ = DragBlocker::None;
        if (callbacks_.onRowHeightDragEnd != nullptr)
        {
            callbacks_.onRowHeightDragEnd();
        }
        setMouseCursor(juce::MouseCursor(juce::MouseCursor::NormalCursor));
        juce::ignoreUnused(e);
        return;
    }

    if (headerDragInProgress_ && dragHost_.has_value())
    {
        dragHost_->onHeaderDragEnded(dragTrackId_);
        headerDragInProgress_ = false;
        dragBlocker_ = DragBlocker::None;
        return;
    }
    dragBlocker_ = DragBlocker::None;
    juce::ignoreUnused(e);
}

void TrackHeaderView::setSourceForbiddenForHeaderDrag() noexcept
{
    setMouseCursor(getForbiddenNoDropMouseCursor());
}

void TrackHeaderView::restoreSourceCursorAfterHeaderDrag() noexcept
{
    setMouseCursor(juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
}
