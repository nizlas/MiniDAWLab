// =============================================================================
// TrackHeaderView.cpp — model/callback-driven header; optional drag host
// =============================================================================

#include "ui/TrackHeaderView.h"

#include "ui/ForbiddenCursor.h"

#include <array>
#include <juce_core/juce_core.h>

namespace
{
    constexpr float kHeaderDragThresholdPx = 3.0f;

    constexpr float kCubaseCtlCornerRadMax = 2.85f;

    /// Outer padding from header edges (matches prior name insets).
    constexpr int kHeaderOuterPadXPx = 8;
    constexpr int kHeaderOuterPadYPx = 4;
    /// Extra trim after horizontal outer pad (active-state accent eats ~4px blue bar).
    constexpr int kHeaderNameTrimLeftActivePx = 6;
    constexpr int kHeaderNameTrimLeftInactivePx = 4;
    /// Vertical gap between name block and control strip.
    constexpr int kHeaderNameToButtonsGapPx = 3;
    /// Default title-only block height (~one line at 14pt).
    constexpr int kHeaderNameBlockTitleOnlyPx = 22;
    /// Title + subtitle stacked block height.
    constexpr int kHeaderNameBlockWithSubtitlePx = 30;
    [[nodiscard]] float cubaseCornerRadiusForSquare(float side) noexcept
    {
        return juce::jlimit(1.4f, kCubaseCtlCornerRadMax, side * 0.16f);
    }

    [[nodiscard]] float stripStandardGlyphInsetForSquareBody(float squareSidePx) noexcept
    {
        return juce::jlimit(2.0f, 3.5f, squareSidePx * 0.11f);
    }

    /** One shared inset for the power ring/stem in every enabled/disabled/power-off state (geometry only varies with square body side). */
    [[nodiscard]] juce::Rectangle<float> nonLetterGlyphAreaFromSquareBodyPx(juce::Rectangle<int> squareBodyPx) noexcept
    {
        const float side = static_cast<float>(juce::jmin(squareBodyPx.getWidth(), squareBodyPx.getHeight()));
        if (side < 6.5f)
        {
            return squareBodyPx.toFloat();
        }
        const float pad = stripStandardGlyphInsetForSquareBody(side);
        return squareBodyPx.toFloat().reduced(pad);
    }

    void drawStandardStripButtonFace(juce::Graphics& g,
                                     juce::Rectangle<float> body,
                                     juce::Colour fill,
                                     juce::Colour edge,
                                     bool hovered)
    {
        const float rad = cubaseCornerRadiusForSquare(juce::jmin(body.getWidth(), body.getHeight()));
        if (hovered)
        {
            fill = fill.brighter(0.12f);
            edge = edge.brighter(0.28f);
        }
        g.setColour(fill);
        g.fillRoundedRectangle(body, rad);
        g.setColour(edge);
        g.drawRoundedRectangle(body, rad, 1.0f);
    }

    void drawPowerGlyphInSquare(juce::Graphics& g,
                                juce::Rectangle<float> icon,
                                juce::Colour glyphColour)
    {
        float side = juce::jmin(icon.getWidth(), icon.getHeight());
        if (side <= 4.0f)
        {
            return;
        }

        icon =
            juce::Rectangle<float>(icon.getCentreX() - side * 0.5f, icon.getCentreY() - side * 0.5f, side, side);

        const auto x = [&](float nx) { return icon.getX() + nx * side; };
        const auto y = [&](float ny) { return icon.getY() + ny * side; };

        const float stroke = juce::jlimit(1.75f, 2.5f, side * 0.15f);

        g.setColour(glyphColour);

        constexpr float ringCx = 0.5f;
        constexpr float ringCy = 0.54f;
        constexpr float ringR = 0.36f;
        constexpr float arcFromDeg = 35.0f;
        constexpr float arcToDeg = 325.0f;

        juce::Path ring;
        ring.addCentredArc(x(ringCx),
                           y(ringCy),
                           side * ringR,
                           side * ringR,
                           0.0f,
                           juce::degreesToRadians(arcFromDeg),
                           juce::degreesToRadians(arcToDeg),
                           true);

        g.strokePath(
            ring,
            juce::PathStrokeType(stroke,
                                 juce::PathStrokeType::mitered,
                                 juce::PathStrokeType::butt));

        juce::Path stem;
        stem.startNewSubPath(x(0.5f), y(0.1f));
        stem.lineTo(x(0.5f), y(0.38f));

        g.strokePath(
            stem,
            juce::PathStrokeType(stroke,
                                 juce::PathStrokeType::mitered,
                                 juce::PathStrokeType::butt));
    }

    /** Three vertical white keys + two black keys; separators/black use near‑black strokes only. */
    void drawInstrumentPianoGlyphCubaseSimple(juce::Graphics& g, juce::Rectangle<float> glyphArea)
    {
        if (glyphArea.getWidth() < 5.0f || glyphArea.getHeight() < 6.0f)
        {
            return;
        }

        const auto kb = glyphArea.reduced(1.2f);
        const float w = kb.getWidth();
        const float h = kb.getHeight();
        if (w < 3.5f || h < 4.5f)
        {
            return;
        }

        const float rad = juce::jlimit(1.0f, 1.35f, juce::jmin(w, h) * 0.11f);
        const auto borderBlack = juce::Colours::black;
        const auto keyBlack = juce::Colours::black;

        g.setColour(juce::Colour(0xfffafafa));
        g.fillRoundedRectangle(kb, rad);
        g.setColour(borderBlack);
        g.drawRoundedRectangle(kb, rad, 1.0f);

        const float bx1 = kb.getX() + w / 3.0f;
        const float bx2 = kb.getX() + 2.0f * w / 3.0f;

        constexpr float sepThick = 1.35f;
        g.setColour(keyBlack);
        g.drawLine(bx1, kb.getY() + 1.0f + rad * 0.15f, bx1, kb.getBottom() - 1.0f, sepThick);
        g.drawLine(bx2, kb.getY() + 1.0f + rad * 0.15f, bx2, kb.getBottom() - 1.0f, sepThick);

        const float bkH = juce::jlimit(h * 0.45f, h * 0.55f, h * 0.52f);
        const float bkW = juce::jmax(2.8f, w * 0.22f);
        const float yb = kb.getY() + 0.85f;

        juce::Rectangle<float> bk1(bx1 - bkW * 0.5f, yb, bkW, bkH);
        juce::Rectangle<float> bk2(bx2 - bkW * 0.5f, yb, bkW, bkH);
        g.setColour(keyBlack);
        g.fillRect(bk1);
        g.fillRect(bk2);
    }

    [[nodiscard]] int headerMinimumRowHeightPxForNameOnly(const bool hasSubtitle) noexcept
    {
        const int nameBlock = hasSubtitle ? kHeaderNameBlockWithSubtitlePx : kHeaderNameBlockTitleOnlyPx;
        return kHeaderOuterPadYPx + nameBlock + TrackHeaderView::kHeaderResizeBandPx;
    }
} // namespace

int TrackHeaderView::minimumRowHeightPxForNameOnlyLayout(const bool hasSubtitle) noexcept
{
    return headerMinimumRowHeightPxForNameOnly(hasSubtitle);
}

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

std::vector<TrackHeaderView::TrackHeaderStripButtonSpec>
TrackHeaderView::buildStripControlSpecs() const noexcept
{
    const TrackHeaderModel m = modelProvider_();
    std::vector<TrackHeaderStripButtonSpec> specs;
    specs.reserve(4);

    juce::Rectangle<int> inst = getInstrumentEditorButtonBounds();
    if (!inst.isEmpty())
    {
        TrackHeaderStripButtonSpec s{};
        s.kind = TrackHeaderButtonKind::InstrumentEditor;
        s.enabled =
            callbacks_.onOpenInstrumentEditor != nullptr && m.instrumentEditorAvailable;
        s.cellBounds = inst;
        specs.push_back(s);
    }

    TrackHeaderStripButtonSpec p{};
    p.kind = TrackHeaderButtonKind::Power;
    p.enabled = callbacks_.onTogglePower != nullptr && m.powerInteractable;
    p.powerStandby = m.off;
    p.cellBounds = getPowerButtonBounds();
    specs.push_back(std::move(p));

    TrackHeaderStripButtonSpec mu{};
    mu.kind = TrackHeaderButtonKind::Mute;
    mu.enabled = callbacks_.onToggleMute != nullptr && m.muteInteractable;
    mu.muteActive = m.muted;
    mu.cellBounds = getMuteButtonBounds();
    specs.push_back(std::move(mu));

    TrackHeaderStripButtonSpec a{};
    a.kind = TrackHeaderButtonKind::Arm;
    a.enabled = callbacks_.onToggleArm != nullptr && m.armInteractable;
    a.armActive = m.armed;
    a.cellBounds = getArmButtonBounds();
    specs.push_back(std::move(a));

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
        if (spec.enabled)
        {
            drawStandardStripButtonFace(g,
                                        rf,
                                        spec.muteActive ? juce::Colour(0xffc6a42a)
                                                        : juce::Colour(0xff5a5858),
                                        ctlEdgeNeutral,
                                        showHoverBrighten);
            {
                juce::Graphics::ScopedSaveState gs(g);
                g.reduceClipRegion(bodyPx);
                const float fontH =
                    juce::jlimit(8.5f, 11.5f,
                                 juce::jmin(static_cast<float>(bodyPx.getWidth()),
                                            static_cast<float>(bodyPx.getHeight()))
                                     * 0.52f);
                g.setFont(juce::Font(juce::FontOptions().withHeight(fontH)));
                g.setColour(spec.muteActive ? juce::Colour(0xff0a0a0a) : juce::Colour(0xffeaeaea));
                g.drawFittedText("M", bodyPx, juce::Justification::centred, 1);
            }
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(0xff3e3e3e), edgeInactiveStroke, false);
            juce::Graphics::ScopedSaveState gs(g);
            g.reduceClipRegion(bodyPx);
            const float fontH =
                juce::jlimit(8.5f,
                             11.5f,
                             juce::jmin(static_cast<float>(bodyPx.getWidth()),
                                        static_cast<float>(bodyPx.getHeight()))
                                     * 0.52f);
            g.setFont(juce::Font(juce::FontOptions().withHeight(fontH)));
            g.setColour(juce::Colour(0xff7a7a7a));
            g.drawFittedText("M", bodyPx, juce::Justification::centred, 1);
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
            juce::Graphics::ScopedSaveState gs(g);
            g.reduceClipRegion(bodyPx);
            const float fontH =
                juce::jlimit(8.5f,
                             11.5f,
                             juce::jmin(static_cast<float>(bodyPx.getWidth()),
                                        static_cast<float>(bodyPx.getHeight()))
                                     * 0.52f);
            g.setFont(juce::Font(juce::FontOptions().withHeight(fontH)));
            g.setColour(spec.armActive ? juce::Colour(0xfff8f8ff) : juce::Colour(0xffeaeaea));
            g.drawFittedText("R", bodyPx, juce::Justification::centred, 1);
        }
        else
        {
            drawStandardStripButtonFace(g, rf, juce::Colour(0xff3e3e3e), edgeInactiveStroke, false);
            juce::Graphics::ScopedSaveState gs(g);
            g.reduceClipRegion(bodyPx);
            const float fontH =
                juce::jlimit(8.5f,
                             11.5f,
                             juce::jmin(static_cast<float>(bodyPx.getWidth()),
                                        static_cast<float>(bodyPx.getHeight()))
                                     * 0.52f);
            g.setFont(juce::Font(juce::FontOptions().withHeight(fontH)));
            g.setColour(juce::Colour(0xff888888));
            g.drawFittedText("R", bodyPx, juce::Justification::centred, 1);
        }
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

int TrackHeaderView::computeRightStripCellCount() const noexcept
{
    const auto m = modelProvider_();
    if (callbacks_.onOpenInstrumentEditor == nullptr || !m.instrumentEditorAvailable)
    {
        return 3;
    }
    return 4;
}

TrackHeaderView::HeaderContentLayout TrackHeaderView::computeHeaderContentLayout() const noexcept
{
    HeaderContentLayout L{};
    const TrackHeaderModel m = modelProvider_();
    auto const b = getLocalBounds();
    if (b.isEmpty())
    {
        return L;
    }

    const bool active = m.active;
    const int contentLeft =
        b.getX() + kHeaderOuterPadXPx + (active ? kHeaderNameTrimLeftActivePx : kHeaderNameTrimLeftInactivePx);
    const int contentTop = b.getY() + kHeaderOuterPadYPx;
    const int contentRight = b.getRight() - kHeaderOuterPadXPx;

    const int cell = kStripControlCellWidthPx;
    const int stripW = cell * computeRightStripCellCount();

    const int nameBlockPref =
        m.subtitle.isEmpty() ? kHeaderNameBlockTitleOnlyPx : kHeaderNameBlockWithSubtitlePx;
    const int btnY = contentTop + nameBlockPref + kHeaderNameToButtonsGapPx;
    const int nameW = juce::jmax(0, contentRight - contentLeft);

    L.nameTextBounds = { contentLeft, contentTop, nameW, nameBlockPref };
    L.controlStripBounds = { contentLeft, btnY, stripW, cell };
    return L;
}

int TrackHeaderView::snapTrackHeaderRowHeightAfterResize(const int heightPx,
                                                         const bool hasSubtitle,
                                                         const int globalMinRowPx,
                                                         const int globalMaxRowPx) noexcept
{
    const int contentTop = kHeaderOuterPadYPx;
    const int nameBlock = hasSubtitle ? kHeaderNameBlockWithSubtitlePx : kHeaderNameBlockTitleOnlyPx;
    const int btnY = contentTop + nameBlock + kHeaderNameToButtonsGapPx;
    const int cell = kStripControlCellWidthPx;
    const int band = kHeaderResizeBandPx;

    const int minFullIdeal = btnY + cell + band;
    // Match `minimumRowHeightPxForNameOnlyLayout` / drag clamp — not `btnY + band`, which keeps the
    // name-to-buttons gap inside chrome and reads slightly taller than the allowed drag minimum.
    const int minNameIdeal = minimumRowHeightPxForNameOnlyLayout(hasSubtitle);

    const int hClamped = juce::jlimit(globalMinRowPx, globalMaxRowPx, heightPx);

    if (hClamped >= minFullIdeal)
    {
        return hClamped;
    }

    const int chromeBottom = hClamped - band;
    const int visiblePx = juce::jmax(0, juce::jmin(btnY + cell, chromeBottom) - btnY);
    const double frac = (cell > 0) ? static_cast<double>(visiblePx) / static_cast<double>(cell) : 0.0;

    int snappedIdeal = (frac > 0.5) ? minFullIdeal : minNameIdeal;
    return juce::jlimit(globalMinRowPx, globalMaxRowPx, snappedIdeal);
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

juce::Rectangle<int> TrackHeaderView::getRightControlsStripBounds() const noexcept
{
    return computeHeaderContentLayout().controlStripBounds;
}

juce::Rectangle<int> TrackHeaderView::getArmButtonBounds() const noexcept
{
    auto s = computeHeaderContentLayout().controlStripBounds;
    if (s.isEmpty())
    {
        return {};
    }
    const int cell = kStripControlCellWidthPx;
    if (computeRightStripCellCount() == 4)
    {
        s.removeFromLeft(cell);
    }
    s.removeFromLeft(cell);
    s.removeFromLeft(cell);
    return s.removeFromLeft(cell);
}

juce::Rectangle<int> TrackHeaderView::getMuteButtonBounds() const noexcept
{
    auto s = computeHeaderContentLayout().controlStripBounds;
    if (s.isEmpty())
    {
        return {};
    }
    const int cell = kStripControlCellWidthPx;
    if (computeRightStripCellCount() == 4)
    {
        s.removeFromLeft(cell);
    }
    s.removeFromLeft(cell);
    return s.removeFromLeft(cell);
}

juce::Rectangle<int> TrackHeaderView::getPowerButtonBounds() const noexcept
{
    auto s = computeHeaderContentLayout().controlStripBounds;
    if (s.isEmpty())
    {
        return {};
    }
    const int cell = kStripControlCellWidthPx;
    if (computeRightStripCellCount() == 4)
    {
        s.removeFromLeft(cell);
    }
    return s.removeFromLeft(cell);
}

juce::Rectangle<int> TrackHeaderView::getInstrumentEditorButtonBounds() const noexcept
{
    if (computeRightStripCellCount() < 4)
    {
        return {};
    }
    auto s = computeHeaderContentLayout().controlStripBounds;
    if (s.isEmpty())
    {
        return {};
    }
    return s.removeFromLeft(kStripControlCellWidthPx);
}

void TrackHeaderView::updateStripHoverFromPosition(juce::Point<int> const pos) noexcept
{
    auto const specs = buildStripControlSpecs();
    std::optional<TrackHeaderButtonKind> next;

    constexpr std::array<TrackHeaderButtonKind, 4> hitPrioritiesRightToLeft{{
        TrackHeaderButtonKind::Arm,
        TrackHeaderButtonKind::Mute,
        TrackHeaderButtonKind::Power,
        TrackHeaderButtonKind::InstrumentEditor,
    }};
    for (auto const pri : hitPrioritiesRightToLeft)
    {
        if (auto const* s = findStripControlSpec(specs, pri);
            s != nullptr && s->enabled && stripCellHitIntersectsVisibleChrome(s->cellBounds, pos))
        {
            next = pri;
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
    if (position.y < bandTop)
    {
        return false;
    }

    // Do not exclude using logical `controlStripBounds`: when the row is name-only, those bounds still
    // overlap the bottom band in Y even though paint/hit-test clip the strip to chrome above the band,
    // which incorrectly disabled the entire resize zone (buttons are not hit-tested here anyway).

    return true;
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

void TrackHeaderView::paint(juce::Graphics& g)
{
    TrackHeaderModel const m = modelProvider_();

    auto const specs = buildStripControlSpecs();

    auto const active = m.active;

    auto const b = getLocalBounds();
    g.setColour(active ? juce::Colour(0xff2a4a5a) : juce::Colour(0xff333333));
    g.fillRect(b);
    if (active)
    {
        g.setColour(juce::Colours::deepskyblue);
        g.fillRect(b.getX(), b.getY(), 4, b.getHeight());
    }

    auto const layout = computeHeaderContentLayout();
    auto nameArea = layout.nameTextBounds;
    if (!nameArea.isEmpty())
    {
        g.setColour(juce::Colours::whitesmoke);
        g.setFont(14.0f);
        if (m.subtitle.isEmpty())
        {
            g.drawFittedText(m.name, nameArea, juce::Justification::centredLeft, 1);
        }
        else
        {
            auto r = nameArea;
            const int titleH = juce::jlimit(12, 17, juce::jmax(12, r.getHeight() / 2));
            g.drawText(m.name, r.removeFromTop(titleH), juce::Justification::centredLeft, true);
            g.setColour(juce::Colour(0xffcccccc));
            g.setFont(11.0f);
            g.drawFittedText(m.subtitle, r, juce::Justification::topLeft, 2);
        }
    }

    juce::Colour const ctlNeutralEdge(0xd0161616);

    auto const chrome = visibleChromeBoundsExcludingResizeBand();
    if (!layout.controlStripBounds.getIntersection(chrome).isEmpty())
    {
        juce::Graphics::ScopedSaveState const gs(g);
        g.reduceClipRegion(chrome);

        constexpr std::array<TrackHeaderButtonKind, 4> paintOrderBottomToTop{{
            TrackHeaderButtonKind::Mute,
            TrackHeaderButtonKind::Arm,
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

bool TrackHeaderView::dispatchStripClick(juce::Point<int> const position,
                                         std::vector<TrackHeaderStripButtonSpec>&& specs) noexcept
{
    constexpr std::array<TrackHeaderButtonKind, 4> hitPrioritiesRightToLeft{{
        TrackHeaderButtonKind::Arm,
        TrackHeaderButtonKind::Mute,
        TrackHeaderButtonKind::Power,
        TrackHeaderButtonKind::InstrumentEditor,
    }};

    for (auto const pri : hitPrioritiesRightToLeft)
    {
        TrackHeaderStripButtonSpec const* const spec = findStripControlSpec(specs, pri);
        if (spec == nullptr || !spec->enabled || spec->cellBounds.isEmpty()
            || !stripCellHitIntersectsVisibleChrome(spec->cellBounds, position))
        {
            continue;
        }

        switch (spec->kind)
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

        case TrackHeaderButtonKind::Arm:
            if (callbacks_.onToggleArm != nullptr)
            {
                dragBlocker_ = DragBlocker::Arm;
                callbacks_.onToggleArm();
            }
            return true;

        default:
            jassertfalse;
            return false;
        }
    }

    return false;
}

void TrackHeaderView::mouseDown(juce::MouseEvent const& e)
{
    if (e.mods.isPopupMenu())
    {
        if (callbacks_.onShowContextMenu != nullptr)
        {
            dragBlocker_ = DragBlocker::None;
            headerDragInProgress_ = false;
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
    if (callbacks_.onActivateName != nullptr)
    {
        callbacks_.onActivateName();
    }
}

void TrackHeaderView::mouseDrag(juce::MouseEvent const& e)
{
    if (!e.mods.isLeftButtonDown())
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
