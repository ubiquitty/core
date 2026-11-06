// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// OverlayGeometry.cpp — see OverlayGeometry.h. One copy of the math, so every
// frontend renders the cursor / selection / scroll thumb identically.

#include "Core/Rendering/OverlayGeometry.h"

#include "Core/Rendering/ColorResolve.h"

#include "Core/Screen/Addressing.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

OverlayQuad cellSpanQuad(int column0, int column1, int visibleRow, FloatSize cellSizeInPixels, FloatPoint origin, OverlayColor color)
{
    OverlayQuad q;
    q.minX = static_cast<float>(column0) * cellSizeInPixels.width + origin.x;
    q.minY = static_cast<float>(visibleRow) * cellSizeInPixels.height + origin.y;
    q.maxX = static_cast<float>(column1 + 1) * cellSizeInPixels.width + origin.x;
    q.maxY = static_cast<float>(visibleRow + 1) * cellSizeInPixels.height + origin.y;
    q.red = color.red;
    q.green = color.green;
    q.blue = color.blue;
    q.alpha = color.alpha;
    return q;
}

bool searchMatchQuad(int absoluteRow, int column, int length, int history, int viewOffset, int rows, FloatSize cellSizeInPixels, FloatPoint origin, SearchMatchEmphasis emphasis, OverlayQuad& out)
{
    if (length <= 0)
        return false; // empty span — nothing to highlight
    // The caller has already resolved the offset the way the addressing does;
    // this is pure geometry over the row map it defines.
    const int vrow = Screen::visibleRowForAbsolute(history, viewOffset, absoluteRow);
    if (vrow < 0 || vrow >= rows)
        return false; // scrolled out of the visible viewport

    // Xcode/native find grammar: ordinary matches get a subtle NEUTRAL (gray)
    // rounded highlight so they read as "also here" without competing with the
    // one the user is standing on.
    //
    // The focused match carries no quad at all. Its YELLOW is baked into the
    // cell background, which is what lets the builder compute an ink that
    // contrasts with it; anything this function drew would sit over the glyph
    // and bury the ink the bake exists to make readable. A border drawn around
    // it reads as a second highlight rather than as emphasis, so the focused
    // match is the background and its ink, and nothing else.
    if (emphasis == SearchMatchEmphasis::Focused)
        return false;
    // [column, column+length) exclusive → inclusive col1.
    return otherSearchMatchQuadAtVisibleRow(vrow, column, column + length - 1, cellSizeInPixels, origin, out);
}

SearchPillInsets searchPillInsets(float cellHeightInPixels, double cellHeightPoints, double atlasScale, float fontAscent, float lineGapTopInPixels, float xHeight, float capHeight)
{
    const float bandHeight = capHeight > 0.0f ? capHeight : xHeight;
    if (cellHeightInPixels <= 0.0f || fontAscent <= 0.0f || bandHeight <= 0.0f)
        return {};
    const double points = cellHeightPoints > 0.0 ? cellHeightPoints : static_cast<double>(cellHeightInPixels);
    const float lineHeightAtlas = static_cast<float>(points * (atlasScale > 0.0 ? atlasScale : 1.0));
    if (lineHeightAtlas <= 0.0f)
        return {};
    // The band's midpoint as a fraction of the line, so the answer holds
    // across scales: the baseline less half the band's height.
    const float baselineFraction = (fontAscent + lineGapTopInPixels) / lineHeightAtlas;
    const float centre = (baselineFraction - 0.5f * bandHeight / lineHeightAtlas) * cellHeightInPixels;
    const float breathing = std::round(searchPillBreathingPoints * cellHeightInPixels / static_cast<float>(points));
    const float ceiling = std::floor(cellHeightInPixels / 3.0f);
    const float reach = std::max(std::min(centre, cellHeightInPixels - centre) - breathing, cellHeightInPixels / 3.0f);
    SearchPillInsets insets;
    insets.top = std::clamp(std::round(centre - reach), 0.0f, ceiling);
    insets.bottom = std::clamp(std::round(cellHeightInPixels - centre - reach), 0.0f, ceiling);
    return insets;
}

bool otherSearchMatchQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, SearchPillInsets insets, OverlayQuad& out)
{
    if (!otherSearchMatchQuadAtVisibleRow(visibleRow, column0, column1, cellSizeInPixels, origin, out))
        return false;
    out.minY += insets.top;
    out.maxY -= insets.bottom;
    // The corners follow the pill's own height, so a shorter pill stays as
    // round as a full one.
    out.cornerRadius = (out.maxY - out.minY) * 0.22f;
    return true;
}

bool otherSearchMatchQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out)
{
    if (column1 < column0)
        return false; // empty / inverted span — nothing to highlight
    const OverlayColor color { 0.60f, 0.60f, 0.62f, 0.30f };
    out = cellSpanQuad(column0, column1, visibleRow, cellSizeInPixels, origin, color);
    // Wider than the cells it covers. A highlight ending on the advance box
    // clips the side bearing of the first and last glyph, so the ink touches the
    // edge and the span reads as cut off rather than as a highlight around it.
    // A quarter of a cell each side is enough to clear any bearing this cell
    // width can hold.
    const float sideBearing = cellSizeInPixels.width * 0.25f;
    out.minX -= sideBearing;
    out.maxX += sideBearing;
    // A continuous corner, resolved by the caller's shape: the radius is a
    // little over a fifth of the line so the curve runs most of the corner
    // rather than meeting a straight edge partway, which is what makes it read
    // as continuous rather than as an arc pasted on.
    out.cornerRadius = cellSizeInPixels.height * 0.22f;
    out.cornerStyle = CornerStyle::Continuous;
    return true;
}

OverlayColor focusedSearchMatchColor()
{
    return { 1.00f, 0.82f, 0.18f, 1.0f };
}

float focusedPillScale(float ageSeconds)
{
    if (ageSeconds < 0.0f || ageSeconds >= focusedPillBounceSeconds)
        return 1.0f;
    // Amplitude, decay and period chosen so the peak lands near fifty
    // milliseconds at about 1.19 and the curve is within a hundredth of rest
    // by the bounce's end.
    constexpr float amplitude = 0.35f;
    constexpr float decaySeconds = 0.08f;
    constexpr float periodSeconds = 0.2f;
    constexpr float twoPi = 6.28318530718f;
    return 1.0f + amplitude * std::exp(-ageSeconds / decaySeconds) * std::sin(twoPi * ageSeconds / periodSeconds);
}

bool triggerHighlightQuad(int absoluteRow, int column0, int column1, int history, int viewOffset, int rows, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out)
{
    if (column1 < column0)
        return false; // empty / inverted span — nothing to highlight
    const int vrow = Screen::visibleRowForAbsolute(history, viewOffset, absoluteRow);
    if (vrow < 0 || vrow >= rows)
        return false; // scrolled out of the visible viewport
    // column0/column1 are already INCLUSIVE (Triggers reports an inclusive span).
    return triggerHighlightQuadAtVisibleRow(vrow, column0, column1, cellSizeInPixels, origin, out);
}

int hiliteRegionRowSpans(int startColumn, int startRow, int firstRow, int lastRow, int rows, int columns, int viewOffset, SelectionRowSpan* out, int maxOut)
{
    if (!out || maxOut <= 0 || rows <= 0 || columns <= 0)
        return 0;
    // 1-based from the host to 0-based, then clamped into the live screen: the
    // band is whatever the host claims and the screen may since have resized.
    int first = firstRow - 1;
    int last = lastRow - 1;
    if (first < 0)
        first = 0;
    if (last > rows - 1)
        last = rows - 1;
    if (last < first)
        return 0;
    int start = startRow - 1;
    if (start < first)
        start = first;
    // A start below the band collapses onto its last row rather than emptying
    // the band: the host named a region, so it gets its final row.
    if (start > last)
        start = last;
    int column = startColumn - 1;
    if (column < 0)
        column = 0;
    if (column > columns - 1)
        column = columns - 1;

    int written = 0;
    for (int row = start; row <= last && written < maxOut; ++row) {
        const int visibleRow = row + viewOffset; // screen row -> visible row
        if (visibleRow < 0 || visibleRow >= rows)
            continue;
        // Only the first row starts mid-line. A band spanning three rows covers
        // the tail of the first and the whole of the rest, which is what a
        // linear selection looks like and what the host means by a region.
        const int column0 = (row == start) ? column : 0;
        const int column1 = columns - 1;
        if (column1 < column0)
            continue;
        out[written] = { visibleRow, column0, column1 };
        ++written;
    }
    return written;
}

bool triggerHighlightQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out)
{
    if (column1 < column0)
        return false; // empty / inverted span — nothing to highlight
    // Translucent violet, far enough from find's yellow/orange that a trigger
    // highlight is never mistaken for a search match. Alpha kept light
    // so the underlying glyphs stay legible.
    constexpr OverlayColor color { 0.62f, 0.40f, 0.95f, 0.34f };
    out = cellSpanQuad(column0, column1, visibleRow, cellSizeInPixels, origin, color);
    return true;
}

bool hiliteRegionQuadAtVisibleRow(int visibleRow, int column0, int column1, FloatSize cellSizeInPixels, FloatPoint origin, OverlayQuad& out)
{
    if (column1 < column0)
        return false; // empty / inverted span — nothing to tint
    out = cellSpanQuad(column0, column1, visibleRow, cellSizeInPixels, origin, hiliteRegionColor());
    return true;
}

namespace {

/// The bar as the toolkit's insertion caret, in GTK's words.
///
/// `gtk_css_style_snapshot_caret` places the caret at the line's logical
/// rectangle mapped to whole logical pixels by `pango_extents_to_pixels` —
/// the origin floored, the far edge ceiled — and `snapshot_insertion_cursor`
/// fills a colour rectangle `stem` wide, `stem / 2` left of the insertion
/// point for left-to-right text, where `stem` truncates `height * aspect + 1`.
/// The logical rectangle then scales to device pixels whole, so the stem is
/// never feathered, and the blink fades it as an opacity over the colour.
/// Only a focused bar is the caret: GTK draws none in a text without focus,
/// and the terminal draws its unfocused cursor as the hollow outline.
///
/// - Parameters:
///   - p: The cursor's parameters; `toolkitCaret` is present.
///   - left: The insertion point, the cell's left edge, before the origin.
///   - lineTop: The top of the font's line box, before the origin.
///   - lineBottom: The bottom of the font's line box, before the origin.
///   - out: Receives the stem.
/// - Returns: True; the stem always draws.
bool buildToolkitCaretQuad(const CursorQuadParameters& p, float left, float lineTop, float lineBottom, OverlayQuad& out)
{
    const double scale = p.toolkitCaret.logicalScale;
    const double logicalLeft = std::floor((static_cast<double>(left) + p.origin.x) / scale);
    const double logicalTop = std::floor((static_cast<double>(lineTop) + p.origin.y) / scale);
    const double logicalBottom = std::ceil((static_cast<double>(lineBottom) + p.origin.y) / scale);
    const double logicalHeight = logicalBottom - logicalTop;
    const int stem = static_cast<int>(logicalHeight * p.toolkitCaret.aspectRatio + 1);
    const double stemLeft = logicalLeft - stem / 2;
    out.minX = static_cast<float>(stemLeft * scale);
    out.maxX = static_cast<float>((stemLeft + stem) * scale);
    out.minY = static_cast<float>(logicalTop * scale);
    out.maxY = static_cast<float>(logicalBottom * scale);
    out.red = p.cursorRed;
    out.green = p.cursorGreen;
    out.blue = p.cursorBlue;
    out.alpha = p.cursorAlpha * p.blinkAlpha;
    out.cornerRadius = 0.0f;
    out.strokeWidth = 0.0f;
    return true;
}

} // namespace

bool buildCursorQuad(const CursorQuadParameters& p, OverlayQuad& out)
{
    if (p.blinkAlpha <= 0.001f)
        return false; // fully faded out this frame
    // vrow is already shifted into the scrolled view; a scrolled-back
    // cursor that falls off-screen is hidden by the bounds check. While sliding
    // the integer cell is still the target, so the same bounds gate applies.
    if (p.vrow < 0 || p.vrow >= p.rows || p.column < 0 || p.column >= p.columns)
        return false;

    const float cr = p.cursorRed;
    const float cg = p.cursorGreen;
    const float cb = p.cursorBlue;
    const float baseAlpha = p.cursorAlpha; // per-color cursor opacity (1 = opaque)

    // DECDWL/DECDHL rows render every cell at 2× width (the grid walk doubles
    // the cell advance), so the cursor must too — else bar/underline/outline
    // land at half-column and half-width. All three non-normal attrs are
    // double-width; the settled focused block is the tier over its cell and isn't here.
    const float xStep = (p.lineAttribute == lineAttributeNormal) ? 1.0f : 2.0f;
    // Motion smoothing: position at the fractional cell while a slide is in
    // flight; otherwise the exact integer cell.
    const float posColumn = p.sliding ? p.columnFractional : static_cast<float>(p.column);
    const float posVRow = p.sliding ? p.vrowFractional : static_cast<float>(p.vrow);
    const float left = posColumn * p.cellWidthInPixels * xStep;
    const float top = posVRow * p.cellHeightInPixels;
    // A glyph claiming several columns — a wide pair, a sized block, a
    // proportional run — is one glyph, and the cursor standing on it spans
    // the claim; a cursor one column wide inside a label reads as a bar in
    // the middle of a word.
    const float claimedCells = static_cast<float>(std::max(1, p.widthCells));
    const float right = left + p.cellWidthInPixels * xStep * claimedCells;
    const float bottom = top + p.cellHeightInPixels;

    // Base alpha is the cursor color's own opacity; blink fade / focus
    // logic below scales it. The thin bar/underline shapes reset to it too.
    float xMin = left, yMin = top, xMax = right, yMax = bottom;
    float alpha = baseAlpha;
    float cornerRadius = 0.0f;
    float strokeWidth = 0.0f;
    // The block fills the cell, so its soft sub-pixel edges are invisible and
    // it rides the fractional cell grid. The thin bar/underline are all edge,
    // so they snap to the device-pixel grid (crisp, like Xcode — the gap to
    // the prior glyph then varies by column, which is fine for a code editor).
    bool snap = false;
    bool ceilX = false; // bar: bias the horizontal snap rightward (see below)
    // points → device px via the cell scale.
    const float pixelsPerPoint = (p.cellHeightPoints > 0.0) ? p.cellHeightInPixels / static_cast<float>(p.cellHeightPoints) : 1.0f;
    // Bar + underline span the font's natural line height (Xcode's "default
    // font height"); the user's line spacing is the padding, split evenly
    // above and below — inset each edge by half the line spacing. (The block
    // instead matches its focused fill, which is the full cell background —
    // see its case below.)
    const float vpad = static_cast<float>(p.lineSpacingPoints * 0.5) * pixelsPerPoint;
    // An unfocused cursor is steady and drawn as the hollow outline
    // (Docs/cursor-blink-design.md), so every unfocused shape is the
    // unfocused block's outline — on every arm, toolkit caret or capsule.
    const Ubiquitty::Cursor::Shape shape = p.focused ? p.shape : Ubiquitty::Cursor::Shape::Block;
    switch (shape) {
    case Ubiquitty::Cursor::Shape::Underline: {
        // The bar's shape rotated 90°: a horizontal capsule along the cell
        // bottom, 2 pt thick (matching the bar's 2 pt width), with the same
        // half-thickness radius so the left/right ends are semicircles too.
        float th = roundf(2.0f * pixelsPerPoint);
        if (th < 2.0f)
            th = 2.0f;
        if (fmodf(th, 2.0f) != 0.0f)
            th += 1.0f;
        xMin = left;
        xMax = right;
        // Sit on the font box bottom (= block's bottom, inset by half the
        // line spacing), not the padded cell bottom — else it's vpad too low.
        yMax = bottom - vpad;
        yMin = yMax - th;
        alpha = baseAlpha;
        cornerRadius = th * 0.52f; // just past half-thickness
        const float halfLength = (xMax - xMin) * 0.5f;
        if (cornerRadius > halfLength)
            cornerRadius = halfLength;
        snap = true;
        break;
    }
    case Ubiquitty::Cursor::Shape::Bar: {
        if (p.toolkitCaret.isPresent())
            return buildToolkitCaretQuad(p, left, top + vpad, bottom - vpad, out);
        // Same height as the block; 2 pt wide, centred on the cell's left
        // edge (the insertion point); capsule (semicircle) ends. Even
        // integer width so the snapped, centred edges stay symmetric.
        float barWidth = roundf(2.0f * pixelsPerPoint);
        if (barWidth < 2.0f)
            barWidth = 2.0f;
        if (fmodf(barWidth, 2.0f) != 0.0f)
            barWidth += 1.0f;
        xMin = left - barWidth * 0.5f;
        xMax = left + barWidth * 0.5f;
        yMin = top + vpad;
        yMax = bottom - vpad;
        alpha = baseAlpha;
        cornerRadius = barWidth * 0.52f; // just past half-width
        const float halfHeight = (yMax - yMin) * 0.5f;
        if (cornerRadius > halfHeight)
            cornerRadius = halfHeight;
        snap = true;
        ceilX = true;
        break;
    }
    // The one `default:` in Core that covers its enumeration and stays. The
    // three siblings shared a body that only returned, so the fallback moved
    // below the switch and -Wswitch took over. This body sets the vertical
    // bounds and returns early, so deleting the label would leave a shape
    // outside the enumeration with neither — a different drawing, not a
    // different diagnostic.
    case Ubiquitty::Cursor::Shape::Block:
    default: {
        // A settled focused block is the cursor tier (see the grid walk): the
        // cell re-emitted with the cursor's colours, glyph inverted on top —
        // don't paint the opaque overlay over it. While SLIDING the walk builds
        // no tier, so draw the block as a filled moving quad here (it briefly
        // occludes the glyph during the slide). Unfocused always draws the
        // hollow outline below.
        if (p.focused && !p.sliding)
            return false;
        // Span the full cell (no vpad inset): the focused block fills the
        // whole cell background, and the shader strokes this outline just
        // inside its bounds, so the outline's outer edge lands exactly on
        // the filled block's edge. Insetting here made the outline read
        // shorter than the filled cursor whenever line spacing was set.
        yMin = top;
        yMax = bottom;
        break;
    }
    }

    // A flying caret rides the fractional grid (no device-pixel snap) so the
    // glide is smooth and lands true — otherwise the bar's rightward ceil bias
    // shifts the settle a few px. It snaps crisp again the moment it settles
    // (p.sliding clears).
    if (p.sliding)
        snap = false;

    // A flying caret stays fully lit so the motion reads: the blink fade — and
    // any mid-blink / post-⌘K / idle dim — is suppressed while sliding. Unfocused
    // stays solid (no blink) and the block becomes a hollow outline.
    if (p.focused) {
        if (!p.sliding)
            alpha *= p.blinkAlpha;
    } else if (shape == Ubiquitty::Cursor::Shape::Block) {
        strokeWidth = fmaxf(1.0f, roundf(pixelsPerPoint));
    }

    float minX = xMin + p.origin.x, minY = yMin + p.origin.y;
    float maxX = xMax + p.origin.x, maxY = yMax + p.origin.y;
    if (snap) { // snap to the device-pixel grid; offsets are integers, so the
        minY = roundf(minY);
        maxY = roundf(maxY); // bar width / underline
        // The bar straddles the cell boundary, so nearest-rounding can pull it
        // left onto the prior glyph (no gap). Ceil biases it right into the
        // current cell, keeping a consistent gap from the glyph (like Xcode).
        minX = ceilX ? ceilf(minX) : roundf(minX);
        maxX = ceilX ? ceilf(maxX) : roundf(maxX);
    }
    out.minX = minX;
    out.minY = minY;
    out.maxX = maxX;
    out.maxY = maxY;
    out.red = cr;
    out.green = cg;
    out.blue = cb;
    out.alpha = alpha;
    out.cornerRadius = cornerRadius;
    out.strokeWidth = strokeWidth;
    return true;
}

PromptMarkGeometry resolvePromptMarkGeometry(const PromptMarkParameters& p)
{
    PromptMarkGeometry g;
    if (p.grid.rows <= 0 || p.grid.columns <= 0)
        return g; // perRow stays 0

    const float pixelsPerPoint = (p.cellHeightPoints > 0.0) ? p.cellSizeInPixels.height / static_cast<float>(p.cellHeightPoints) : 1.0f;
    g.pixelsPerPoint = pixelsPerPoint;
    // Bracket: a vertical spine plus a top and bottom stub, 2pt wide. The stroke
    // is ~1pt; the stubs reach across the 2pt width toward the text. The spine is
    // 20% shorter than the point size (12pt → ~10pt) so it reads as a compact
    // marker, scaling with the font.
    float thick = roundf(1.0f * pixelsPerPoint);
    if (thick < 1.0f)
        thick = 1.0f;
    if (thick > 2.0f)
        thick = 2.0f;
    float barWidth = roundf(2.0f * pixelsPerPoint);
    if (barWidth < thick + 1.0f)
        barWidth = thick + 1.0f; // keep a visible stub past the spine
    float barHeight = roundf(0.8f * static_cast<float>(p.fontSize) * pixelsPerPoint);
    if (barHeight < 2.0f * thick + 1.0f)
        barHeight = 2.0f * thick + 1.0f; // top + bottom stub must still fit
    g.thick = thick;
    g.barHeight = barHeight;

    // Brackets sit 2 px clear of the grid in both gutters; a side is dropped
    // when its padding is too narrow.
    const float padLeft = p.origin.x;
    const float gridRight = p.origin.x + static_cast<float>(p.grid.columns) * p.cellSizeInPixels.width;
    const float padRight = p.viewportWidthInPixels - gridRight;
    g.drawLeft = (padLeft >= barWidth + 2.0f);
    g.drawRight = (padRight >= barWidth + 2.0f);
    g.perRow = (g.drawLeft ? 3 : 0) + (g.drawRight ? 3 : 0);
    // Left bracket: spine on the box's left, stubs reach right toward the text → [
    g.maxLeftX = padLeft - 2.0f;
    g.minLeftX = g.maxLeftX - barWidth;
    // Right bracket: spine on the box's right, stubs reach left toward the text → ]
    g.minRightX = gridRight + 2.0f;
    g.maxRightX = g.minRightX + barWidth;

    // Baseline distance from the cell top is (ascent + top half-gap), taken as a
    // fraction of cell height so it holds across font sizes / backing scales.
    // ascent/descent fractions bound the glyph ink box; the bracket is centered on
    // that box's midpoint so it sits on the text line rather than above the caps.
    const float lineHeightAtlas = static_cast<float>(p.cellHeightPoints * p.atlasScale);
    g.baselineFrac = (lineHeightAtlas > 0.0f) ? (p.fontAscent + p.lineGapTopInPixels) / lineHeightAtlas : 0.8f;
    g.ascentFrac = (lineHeightAtlas > 0.0f) ? p.fontAscent / lineHeightAtlas : 0.78f;
    g.descentFrac = (lineHeightAtlas > 0.0f) ? p.fontDescent / lineHeightAtlas : 0.2f;
    g.hasBracketInk = lineHeightAtlas > 0.0f && (p.bracketInkTopInPixels > 0.0f || p.bracketInkBottomInPixels > 0.0f);
    if (g.hasBracketInk)
        g.bracketMidAboveBaselineFrac = 0.5f * (p.bracketInkTopInPixels - p.bracketInkBottomInPixels) / lineHeightAtlas;
    return g;
}

int buildPromptMarkBrackets(const PromptMarkParameters& p, const PromptMarkGeometry& g, int rowIndex, bool failed, OverlayQuad* out, int maxOut)
{
    if (g.perRow <= 0 || g.perRow > maxOut)
        return 0;
    if (rowIndex < 0 || rowIndex >= p.grid.rows)
        return 0;

    // The caller's "ok" ink for an ordinary prompt, its "fail" ink for a
    // command that exited non-zero (osc133MarkCmdFailed), picked per row,
    // mixed with the ground in linear light and drawn solid: the softening is
    // in the colour, not in an alpha that would thicken a translucent
    // surface where the bracket stands.
    const double weight = failed ? promptMarkFailWeight : promptMarkOkWeight;
    const auto mixed = [&](float ink, float ground) {
        return static_cast<float>(SRGBGamma::encode(SRGBGamma::linearize(ink) * weight + SRGBGamma::linearize(ground) * (1.0 - weight)));
    };
    const float cr = mixed(failed ? p.failRed : p.okRed, p.groundRed);
    const float cg = mixed(failed ? p.failGreen : p.okGreen, p.groundGreen);
    const float cb = mixed(failed ? p.failBlue : p.okBlue, p.groundBlue);
    const float ca = 1.0f;

    const float cellHeight = p.cellSizeInPixels.height;
    const float thick = g.thick;
    const float rowTop = static_cast<float>(rowIndex) * cellHeight + p.origin.y;
    const float baselineY = rowTop + g.baselineFrac * cellHeight;
    float yTop;
    float yBottom;
    if (g.hasBracketInk) {
        // Centred where the face centres its own `[`: the mark frames the
        // line the way the font's bracket would, whatever the metrics say.
        // y grows downward, so a centre above the baseline subtracts.
        const float glyphMid = baselineY - g.bracketMidAboveBaselineFrac * cellHeight;
        yTop = glyphMid - 0.5f * g.barHeight;
        yBottom = glyphMid + 0.5f * g.barHeight;
    } else {
        // Unmeasured face: centre on the ascent/descent midpoint with the
        // optical nudges that made that midpoint look right on the faces it
        // was tuned on — 0.5pt down, and 0.5pt taller at the top.
        const float glyphMid = baselineY + 0.5f * (g.descentFrac - g.ascentFrac) * cellHeight + 0.5f * g.pixelsPerPoint;
        yTop = glyphMid - 0.5f * g.barHeight - 0.5f * g.pixelsPerPoint;
        yBottom = glyphMid + 0.5f * g.barHeight;
    }
    // Keep the bracket inside its own row even when the cell is short.
    const float rowBottom = rowTop + cellHeight;
    if (yTop < rowTop)
        yTop = rowTop;
    if (yBottom > rowBottom)
        yBottom = rowBottom;

    int n = 0;
    auto emit = [&](float xMin, float yMin, float xMax, float yMax) {
        OverlayQuad& q = out[n++];
        q.minX = xMin;
        q.minY = yMin;
        q.maxX = xMax;
        q.maxY = yMax;
        q.red = cr;
        q.green = cg;
        q.blue = cb;
        q.alpha = ca;
        q.cornerRadius = 0.0f;
        q.strokeWidth = 0.0f;
    };

    // Stubs start at the spine's inner edge (not inside it) so the corner pixel
    // isn't filled twice — overlapping quads double the alpha → dark.
    if (g.drawLeft) {
        emit(g.minLeftX, yTop, g.minLeftX + thick, yBottom); // spine
        emit(g.minLeftX + thick, yTop, g.maxLeftX, yTop + thick); // top stub
        emit(g.minLeftX + thick, yBottom - thick, g.maxLeftX, yBottom); // bottom stub
    }
    if (g.drawRight) {
        emit(g.maxRightX - thick, yTop, g.maxRightX, yBottom); // spine
        emit(g.minRightX, yTop, g.maxRightX - thick, yTop + thick); // top stub
        emit(g.minRightX, yBottom - thick, g.maxRightX - thick, yBottom); // bottom stub
    }
    return n;
}

bool buildScrollIndicatorQuad(const ScrollIndicatorParameters& p, OverlayQuad& out)
{
    if (p.alpha <= 0.0f)
        return false;
    const int sb = p.scrollbackCount;
    if (sb <= 0)
        return false; // no history to position within

    const int rows = p.rows > 0 ? p.rows : 1;
    // Total extent = scrollback + the visible page; the thumb covers the
    // page fraction and slides from bottom (live tail) to top (oldest).
    const float total = static_cast<float>(sb + rows);
    const float vh = p.viewportSizeInPixels.height;
    const float vw = p.viewportSizeInPixels.width;
    if (vh <= 0.0f || vw <= 0.0f)
        return false;

    // Inset more at the bottom so the thumb clears the window's rounded
    // corner; a little at the top keeps it off the titlebar edge. In
    // fullscreen there's no rounded corner, so match the top inset.
    const float topInset = 6.0f;
    const float bottomInset = p.isFullScreen ? topInset : 16.0f;
    const float trackTop = topInset;
    const float trackHeight = vh - topInset - bottomInset;
    if (trackHeight <= 0.0f)
        return false;

    float thumbFrac = static_cast<float>(rows) / total;
    if (thumbFrac > 1.0f)
        thumbFrac = 1.0f;
    float thumbHeight = thumbFrac * trackHeight;
    if (thumbHeight < 24.0f)
        thumbHeight = 24.0f; // keep it grabbable-looking
    if (thumbHeight > trackHeight)
        thumbHeight = trackHeight;

    // viewOffset 0 = live tail (thumb at bottom); sb = oldest (thumb at top).
    float off = static_cast<float>(p.viewOffset);
    if (off > static_cast<float>(sb))
        off = static_cast<float>(sb);
    const float scrolledFrac = (sb > 0) ? (off / static_cast<float>(sb)) : 0.0f;
    const float thumbTop = trackTop + (1.0f - scrolledFrac) * (trackHeight - thumbHeight);

    // 6 pt thumb width matches NSScroller's overlay-style thumb at
    // NSControlSizeRegular (system scroller draws a 15 pt-wide track with a
    // ~6 pt visible thumb centered). Right-edge inset ~3 pt as macOS.
    const float pixelsPerPoint = (p.scale > 0.0f) ? p.scale : 1.0f;
    const float barWidth = 6.0f * pixelsPerPoint;
    const float x1 = vw - 3.0f * pixelsPerPoint;
    const float x0 = x1 - barWidth;

    out.minX = x0;
    out.minY = thumbTop;
    out.maxX = x1;
    out.maxY = thumbTop + thumbHeight;
    // Subdued tone matching the system overlay scroller (tertiary-label-ish
    // gray), composited over whatever is behind: low alpha, neutral RGB.
    out.red = 0.55f;
    out.green = 0.55f;
    out.blue = 0.57f;
    out.alpha = 0.45f * p.alpha;
    out.cornerRadius = barWidth * 0.5f;
    out.strokeWidth = 0.0f;
    return true;
}

PasswordLockGeometry resolvePasswordLockGeometry(const PasswordLockParameters& p)
{
    PasswordLockGeometry g;
    if (p.cursorRow < 0 || p.cursorRow >= p.grid.rows)
        return g;
    // The cell following the caret. A caret in the last column has none, and the
    // badge is dropped rather than drawn over the caret or outside the grid:
    // the frontends this serves have no chrome to fall back to, so a badge
    // half off the surface is worse than the shell's own `Password:` word.
    const int column = p.cursorColumn + 1;
    if (p.cursorColumn < 0 || column >= p.grid.columns)
        return g;

    const float cellWidth = p.cellSizeInPixels.width;
    const float cellHeight = p.cellSizeInPixels.height;
    // Below this a lock is three or four pixels of shackle over four of body,
    // which reads as a dash rather than as a padlock. The badge is dropped
    // instead, because a shape nobody recognises is not an indicator.
    constexpr float minLockHeight = 8.0f;
    const float lockHeight = std::round(0.60f * cellHeight);
    if (lockHeight < minLockHeight || cellWidth <= 2.0f)
        return g;

    // Proportions of a padlock, taken from the shape rather than from any one
    // platform's icon: a body a little wider than tall, and a shackle about
    // half the body's width standing on it.
    // A pixel of air on each side. Without it the body's edge abuts the caret
    // block it stands next to, and two adjacent filled rects read as one wide
    // one rather than as a caret and a badge.
    float bodyWidth = std::round(0.78f * lockHeight);
    if (bodyWidth > cellWidth - 2.0f)
        bodyWidth = std::floor(cellWidth - 2.0f);
    if (bodyWidth < 4.0f)
        return g;
    const float bodyHeight = std::round(0.56f * lockHeight);
    const float shackleWidth = std::max(3.0f, std::round(0.58f * bodyWidth));
    const float stroke = std::max(1.0f, std::round(0.16f * bodyWidth));

    const float cellLeft = p.origin.x + static_cast<float>(column) * cellWidth;
    const float cellTop = p.origin.y + static_cast<float>(p.cursorRow) * cellHeight;
    const float centreX = std::round(cellLeft + 0.5f * cellWidth);
    const float top = std::round(cellTop + 0.5f * (cellHeight - lockHeight));

    g.doesFit = true;
    g.minX = centreX - 0.5f * bodyWidth;
    g.maxX = g.minX + bodyWidth;
    g.minY = top;
    g.maxY = top + lockHeight;
    g.bodyTop = g.maxY - bodyHeight;
    g.bodyMinX = g.minX;
    g.bodyMaxX = g.maxX;
    g.shackleMinX = centreX - 0.5f * shackleWidth;
    g.shackleMaxX = g.shackleMinX + shackleWidth;
    g.shackleStroke = stroke;
    return g;
}

int buildPasswordLockQuads(const PasswordLockParameters& p, const PasswordLockGeometry& g, OverlayQuad* out, int maxOut)
{
    if (!g.doesFit || maxOut < maxPasswordLockQuads || out == nullptr)
        return 0;

    const auto mixed = [&](float ink, float ground) {
        return static_cast<float>(SRGBGamma::encode(SRGBGamma::linearize(ink) * passwordLockInkWeight
            + SRGBGamma::linearize(ground) * (1.0 - passwordLockInkWeight)));
    };
    const float red = mixed(p.inkRed, p.groundRed);
    const float green = mixed(p.inkGreen, p.groundGreen);
    const float blue = mixed(p.inkBlue, p.groundBlue);

    // The shackle is a hollow rect whose bottom edge is placed one stroke into
    // the body, where the body's fill covers it — what is left showing is a
    // squared arch on two legs, which over a body is a padlock. Square because
    // the software compositor implements no corner radius, and a badge whose
    // shape depends on which arm drew it is not one indicator.
    OverlayQuad& shackle = out[0];
    shackle.minX = g.shackleMinX;
    shackle.maxX = g.shackleMaxX;
    shackle.minY = g.minY;
    shackle.maxY = g.bodyTop + g.shackleStroke;
    shackle.red = red;
    shackle.green = green;
    shackle.blue = blue;
    shackle.alpha = 1.0f;
    shackle.cornerRadius = 0.0f;
    shackle.strokeWidth = g.shackleStroke;
    shackle.cornerStyle = CornerStyle::Circular;

    OverlayQuad& body = out[1];
    body.minX = g.bodyMinX;
    body.maxX = g.bodyMaxX;
    body.minY = g.bodyTop;
    body.maxY = g.maxY;
    body.red = red;
    body.green = green;
    body.blue = blue;
    body.alpha = 1.0f;
    body.cornerRadius = 0.0f;
    body.strokeWidth = 0.0f;
    body.cornerStyle = CornerStyle::Circular;

    return maxPasswordLockQuads;
}

} // namespace Ubiquitty::Rendering
