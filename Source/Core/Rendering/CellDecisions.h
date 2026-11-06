// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/ShapedGlyphGroups.h"

#include "Core/CellPen.h"
#include "Core/Rendering/ColorResolve.h"
#include "Core/Rendering/GlyphBackend.h"
#include "Core/TextSizeTable.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "Core/TerminalOperations.h"

#include <cstdint>

namespace Ubiquitty::Rendering {

// Per-cell styling decisions for the portable instance builder. Header-only
// definitions allow callers to inline the decisions without cross-language LTO.
// No palette / GPU / platform types.

/// The columns of `row` a hovered link run covers.
///
/// A run spans `[rowFirst, rowLast]`. Its first row starts at `columnFirst`, its
/// last ends at `columnLast`, and rows between the two are covered edge to edge
/// across the span the run was resolved in -- a pane's columns, or the screen's.
/// A run wrapped inside a tmux pane ends each row at the pane's right edge, and
/// a band drawn to the screen's width instead strokes across the neighbour
/// pane. `spanLast < 0` means the screen's own width.
/// The one-row case takes both bounds, since `rowFirst == rowLast`.
///
/// - Returns: An inclusive `[first, last]` column range, empty (`last < first`)
///   when `row` lies outside the run.
struct HoverColumnBand {
    int first = 0;
    int last = -1;

    /// - Returns: True when the band covers no column.
    [[nodiscard]] constexpr bool empty() const { return last < first; }
};

constexpr HoverColumnBand hoverColumnBand(int row, int rowFirst, int rowLast, int columnFirst, int columnLast, int spanFirst, int spanLast, int columns)
{
    if (row < rowFirst || row > rowLast)
        return {};
    const int left = spanFirst > 0 ? spanFirst : 0;
    const int right = (spanLast >= 0 && spanLast < columns) ? spanLast : columns - 1;
    HoverColumnBand band;
    band.first = row == rowFirst ? columnFirst : left;
    band.last = row == rowLast ? columnLast : right;
    return band;
}

// Link-hover membership after the frontend has resolved the hover span. Regex /
// path links have no id and use the span directly; OSC 8 links additionally
// require the hovered id so same-row wrap gaps and a reused id outside the span
// do not tint as hovered.
inline bool linkHoverContainsCell(bool hoverActive, std::uint16_t hoverLinkID, int column, int hoverColumn0, int hoverColumn1, std::uint16_t cellLinkID)
{
    if (!hoverActive || column < hoverColumn0 || column > hoverColumn1)
        return false;
    if (hoverLinkID == 0)
        return true;
    return cellLinkID == hoverLinkID;
}

/// Digests the code points a hovered run covers, row by row through
/// ``hoverColumnBand``, so the frame that draws the run can tell whether the
/// text under it is still the text the pointer resolved.
///
/// The resolve takes the digest from the model and the frame takes it from its
/// own cells; the two agree exactly when the cells still spell the run.
///
/// - Parameters:
///   - rowFirst: The run's first row, in whichever row space `codePointAt` reads.
///   - rowLast: The run's last row.
///   - columnFirst: The run's first column, on its first row.
///   - columnLast: The run's last column, on its last row.
///   - spanFirst: The pane span's first column.
///   - spanLast: The pane span's last column, or a value below zero for the row's width.
///   - columns: The row width.
///   - codePointAt: Reads the code point at a row and column.
/// - Returns: The digest, never zero: zero is the absence of a digest.
template<typename CodePointAt>
[[nodiscard]] std::uint64_t linkRunTextDigest(int rowFirst, int rowLast, int columnFirst, int columnLast,
    int spanFirst, int spanLast, int columns, CodePointAt&& codePointAt)
{
    std::uint64_t digest = 0xCBF29CE484222325ull;
    for (int row = rowFirst; row <= rowLast; ++row) {
        const HoverColumnBand band = hoverColumnBand(row, rowFirst, rowLast, columnFirst, columnLast, spanFirst, spanLast, columns);
        for (int column = std::max(band.first, 0); column <= band.last && column < columns; ++column) {
            digest ^= static_cast<std::uint64_t>(codePointAt(row, column));
            digest *= 0x100000001B3ull;
        }
    }
    return digest | 1ull;
}

/// The axes that change a glyph's SHAPE, folded into one comparable value.
///
/// Ligature shaping tracks glyph geometry, not paint, so colour is not among
/// the axes. A later colour or reverse change that split a strip would re-anchor
/// the run's phase, and unrelated cells would wobble when their styling changed
/// and their shape did not.
///
/// Every field is shifted by the width its pen storage allows rather than by the
/// range the SGR codes use today, so a value can never overflow its field and
/// alias a neighbour's. Two font styles that fold to one identity would join
/// runs the font shapes differently, which draws a wrong glyph and reports
/// nothing.
///
/// - Parameter pen: The cell's pen.
/// - Returns: An opaque identity; equal values shape in one run.
inline std::uint32_t ligatureShapeIdentity(const Pen& pen)
{
    // Asserted against what the pen field can HOLD, not against the codes
    // assigned so far: a field widened in Pen would otherwise start aliasing its
    // neighbour here on the first cell that used the new range, silently.
    static_assert(Ubiquitty::VT::ExtendedAttributes::WeightField::maxValue <= 0x7);
    static_assert(Ubiquitty::VT::PenScriptField::maxValue <= 0x3);
    static_assert(Ubiquitty::VT::PenFontStyleField::maxValue <= 0x3);
    static_assert(Ubiquitty::VT::PenProportionalField::maxValue <= 0x1);
    static_assert(Ubiquitty::VT::ExtendedAttributes::TextSizeSpecField::maxValue <= 0xF);
    static_assert(Ubiquitty::VT::PenTextBandField::maxValue <= 0x7);
    return (pen.attributes.contains(Ubiquitty::VT::Attribute::Bold) ? 1u : 0u)
        | (static_cast<std::uint32_t>(pen.extendedAttributes.weight()) << 1)
        | (static_cast<std::uint32_t>(penScript(&pen)) << 4)
        | (static_cast<std::uint32_t>(penFontStyle(&pen)) << 6)
        | (penProportional(&pen) ? 1u << 8 : 0u)
        | (static_cast<std::uint32_t>(pen.extendedAttributes.textSizeSpecID()) << 9)
        | (static_cast<std::uint32_t>(penTextBand(&pen)) << 13);
}

// Expressed through the identity rather than beside it: the seam hands a
// backend the identity and nothing else, so a predicate that answered by its own
// route could disagree with what the backend was given, and the disagreement
// would show up as a run split in one arm and not the other.
inline bool ligatureCellsHaveSameShape(const Cell* a, const Cell* b)
{
    return ligatureShapeIdentity(Ubiquitty::cellPen(*a)) == ligatureShapeIdentity(Ubiquitty::cellPen(*b));
}

/// SGR 73/74 super/subscript glyph geometry.
///
/// The glyph shrinks about its own centre and shifts off the baseline within its
/// cell; the background quad, decorations and cursor stay full-cell. The shift is
/// a fraction of the cell height so it tracks a resize or DECCOLM rather than
/// being a fixed pixel count.
struct ScriptGlyphBox {
    Vec2 origin;
    Vec2 size;
};

/// Whether the face behind a glyph can be drawn at a size of its own.
///
/// The answer decides what SGR 73 and 74 can do with it, and it is not a
/// preference: a tier with one raster per glyph has nothing to draw a smaller
/// letter from, and asking each backend to resample the mask it holds gets a
/// different letter out of every one of them.
enum class GlyphSizing : std::uint8_t {
    /// One raster per glyph, at one size — the embedded bitmap tier.
    FixedRaster,
    /// A face the backend rasterises at whatever size is asked of it.
    AnySize,
};

/// Fraction of the glyph box a scripted character occupies.
constexpr float scriptGlyphScale = 0.70f;
/// Vertical shift off centre, as a fraction of the cell height.
constexpr float scriptShiftFraction = 0.22f;

/// - Parameters:
///   - origin: The unscripted glyph origin, device pixels.
///   - size: The unscripted glyph size, device pixels.
///   - script: `scriptNone`, `scriptSuper` or `scriptSub`.
///   - cellHeight: Cell height in device pixels, which the shift scales with.
///   - sizing: Whether the face can be drawn at a size of its own.
///     ``GlyphSizing::FixedRaster`` keeps the letter at the one size it has and
///     says super- or subscript with the shift alone, rounded to whole pixels
///     so the quad lands on the raster it came from: shrinking a mask that
///     carries no intermediate values makes every backend's sampler
///     approximate the letter its own way, and arms held to one golden then
///     draw two pictures of it.
/// - Returns: The box to draw in; unchanged for `scriptNone`.
constexpr ScriptGlyphBox scriptGlyphBox(Vec2 origin, Vec2 size, std::uint8_t script, float cellHeight,
    GlyphSizing sizing = GlyphSizing::AnySize)
{
    if (script == Ubiquitty::VT::scriptNone)
        return { origin, size };
    const float centerX = origin.x + size.x * 0.5f;
    const float centerY = origin.y + size.y * 0.5f;
    const bool canShrink = sizing == GlyphSizing::AnySize;
    const float scale = canShrink ? scriptGlyphScale : 1.0f;
    const float width = size.x * scale;
    const float height = size.y * scale;
    const float unrounded = cellHeight * scriptShiftFraction;
    // `std::round` is not constexpr; the truncation below rounds the positive
    // value this always is.
    const float shift = canShrink ? unrounded : static_cast<float>(static_cast<int>(unrounded + 0.5f));
    const float top = centerY - height * 0.5f + (script == Ubiquitty::VT::scriptSuper ? -shift : shift);
    return { { centerX - width * 0.5f, top }, { width, height } };
}

/// Where an OSC 66 block's fractionally scaled render area stands.
///
/// Kitty's `n`/`d` draw the text at `n/d` of the block's size; `v` and `h` say
/// where that smaller area sits inside the `s*w` by `s` cells the block still
/// claims. The area is the block shrunk by ``scale`` and moved by ``offset``,
/// so every point of the full-size picture maps through one affine map, and a
/// band draws whatever part of the mapped picture falls in its own row.
struct FractionalRenderArea {
    /// `n/d`, or 1 for a block that asks for no fraction.
    float scale = 1.0f;
    /// The area's top-left relative to the block's, device pixels.
    Vec2 offset {};

    /// - Returns: True when the area is the whole block.
    [[nodiscard]] constexpr bool isWholeBlock() const { return scale == 1.0f; }

    /// Maps a point of the full-size picture into the area.
    ///
    /// - Parameters:
    ///   - point: A point of the unscaled picture, device pixels.
    ///   - blockOrigin: The block's top-left, device pixels.
    /// - Returns: Where the point is drawn.
    [[nodiscard]] constexpr Vec2 map(Vec2 point, Vec2 blockOrigin) const
    {
        return { blockOrigin.x + offset.x + scale * (point.x - blockOrigin.x),
            blockOrigin.y + offset.y + scale * (point.y - blockOrigin.y) };
    }
};

/// The part of a block's slack that stands before its fractional area.
///
/// - Parameters:
///   - align: The `v` or `h` key: 0 the near edge, 1 the far edge, 2 centred.
///   - slack: The block's extent the area does not cover, device pixels.
/// - Returns: The area's offset from the block's near edge.
constexpr float fractionalAreaOffset(std::uint8_t align, float slack)
{
    if (align == 1)
        return slack;
    if (align == 2)
        return slack * 0.5f;
    return 0.0f;
}

/// Places a block's fractionally scaled render area.
///
/// A numerator of zero asks for no fraction: the parser keeps `d > n`, so the
/// one fraction with a zero numerator would draw nothing at all.
///
/// - Parameters:
///   - spec: The block's specification.
///   - blockSize: The block's full size, `s*w` cells by `s` rows, device pixels.
/// - Returns: The area; the whole block when the spec has no fraction.
constexpr FractionalRenderArea fractionalRenderArea(const Ubiquitty::TextSizeSpec& spec, Vec2 blockSize)
{
    if (spec.fractionNumerator == 0 || spec.fractionDenominator <= spec.fractionNumerator)
        return {};
    FractionalRenderArea area;
    area.scale = static_cast<float>(spec.fractionNumerator) / static_cast<float>(spec.fractionDenominator);
    area.offset.x = fractionalAreaOffset(spec.horizontalAlign, blockSize.x * (1.0f - area.scale));
    area.offset.y = fractionalAreaOffset(spec.verticalAlign, blockSize.y * (1.0f - area.scale));
    return area;
}

/// One band's part of a fractionally scaled glyph.
struct FractionalBandSlice {
    bool isVisible = false;
    Vec2 origin {};
    Vec2 size {};
    Vec2 atlasTopLeft {};
    Vec2 atlasBottomRight {};
};

/// Cuts the part of a quad that falls in one row, its atlas rect cut to match.
///
/// - Parameters:
///   - origin: The quad's top-left, device pixels.
///   - size: The quad's size, device pixels.
///   - atlasTopLeft: The atlas rect the whole quad samples, top-left.
///   - atlasBottomRight: The atlas rect the whole quad samples, bottom-right.
///   - rowTop: The row's top, device pixels.
///   - rowBottom: The row's bottom, device pixels.
/// - Returns: The quad's part in the row, invisible when it misses the row.
inline FractionalBandSlice quadRowSlice(Vec2 origin, Vec2 size, Vec2 atlasTopLeft, Vec2 atlasBottomRight, float rowTop, float rowBottom)
{
    FractionalBandSlice slice;
    if (size.x <= 0.0f || size.y <= 0.0f)
        return slice;
    const float top = std::max(origin.y, rowTop);
    const float bottom = std::min(origin.y + size.y, rowBottom);
    if (bottom <= top)
        return slice;
    const float spanV = atlasBottomRight.y - atlasTopLeft.y;
    slice.isVisible = true;
    slice.origin = { origin.x, top };
    slice.size = { size.x, bottom - top };
    slice.atlasTopLeft = { atlasTopLeft.x, atlasTopLeft.y + spanV * (top - origin.y) / size.y };
    slice.atlasBottomRight = { atlasBottomRight.x, atlasTopLeft.y + spanV * (bottom - origin.y) / size.y };
    return slice;
}

/// Cuts the part of a fractionally scaled glyph that falls in one band's row.
///
/// The glyph's full-size quad is mapped into the area, its origin landed on a
/// whole pixel so a fraction that undoes the scale draws the raster 1:1, and the
/// result clipped to the row. A band draws only into its own row, which is the
/// invariant every sized quad keeps: the smaller picture crosses row boundaries
/// at different places than the bands do, so each band takes the rows of the
/// master that land in it rather than its own band of the master.
///
/// - Parameters:
///   - area: The block's render area.
///   - blockOrigin: The block's top-left, device pixels.
///   - glyphOrigin: The full-size glyph quad's top-left, device pixels.
///   - glyphSize: The full-size glyph quad's size, device pixels.
///   - atlasTopLeft: The atlas rect the full-size quad samples, top-left.
///   - atlasBottomRight: The atlas rect the full-size quad samples, bottom-right.
///   - rowTop: The band's row top, device pixels.
///   - rowBottom: The band's row bottom, device pixels.
/// - Returns: The quad to draw, invisible when the glyph misses the row.
inline FractionalBandSlice fractionalBandSlice(const FractionalRenderArea& area, Vec2 blockOrigin, Vec2 glyphOrigin,
    Vec2 glyphSize, Vec2 atlasTopLeft, Vec2 atlasBottomRight, float rowTop, float rowBottom)
{
    const Vec2 mapped = area.map(glyphOrigin, blockOrigin);
    return quadRowSlice({ std::round(mapped.x), std::round(mapped.y) }, { glyphSize.x * area.scale, glyphSize.y * area.scale },
        atlasTopLeft, atlasBottomRight, rowTop, rowBottom);
}

/// Whether `codePoint` is one of the operator characters programming fonts
/// ligate.
///
/// A character-set rule, distinct from ``ligatureCellEligible``, which asks
/// whether a *cell* may shape at all. A run is worth shaping only if it holds at
/// least one of these; letters and digits carry no ligatures in the faces this
/// targets, so a run of them shapes to itself.
/// Every member is ASCII, so membership is two 64-bit words rather than a
/// branch per candidate. Spelled as a switch it compiles to a chain of compares
/// on a value that is different almost every time it is asked — the answer is
/// false for letters, digits and spaces, which is nearly all text — and the
/// mispredictions land on whichever loop asks. The masks are checked against
/// the character list by test_cell_decisions over the whole code-point domain,
/// because a hand-written bitmap is exactly the kind of thing that is wrong in
/// one bit and looks right.
inline bool ligatureOperatorCharacter(std::uint32_t codePoint)
{
    // ! # % & * + - . /   : ; < = > ? @   \ ^ _ |  ~
    constexpr std::uint64_t lowMask = 0xfc00ec6a00000000ull; // 0x00..0x3f
    constexpr std::uint64_t highMask = 0x50000000d0000001ull; // 0x40..0x7f
    if (codePoint >= 128)
        return false;
    const std::uint64_t mask = codePoint < 64 ? lowMask : highMask;
    return ((mask >> (codePoint & 63)) & 1u) != 0;
}

// A cell that may participate in ligature shaping: printable base-font ASCII,
// none of the attributes that force the per-glyph path (wide/emoji forms,
// decorations that must track per-cell, hidden text, the italic face the
// ligature fonts don't cover).
//
// A space is one of them, and that is what makes a run span a stretch rather
// than a word. Excluding it gives each word its own run and its own subpixel
// phase, so the same word draws the same way at every indent; it also sends
// every word through the per-cell owned-ink slice path, which emits no
// instance for a cell whose measured ink is empty. A code screen sends three
// or four runs down that path and a screen of prose sends hundreds, so cells
// draw blank in proportion to the runs a row holds. This predicate decides
// which of the two populations the path receives.
inline bool ligatureCellEligible(const Cell* c)
{
    if (!c)
        return false;
    if (c->codePoint < 0x20 || c->codePoint > 0x7E)
        return false;
    // A shaped run is drawn as one strip and the cell takes a slice of it
    // instead of a glyph of its own, so a column joins a run only if the strip
    // can contain the whole cell. `LigatureCell` carries a base code point and a
    // shape identity; marks are in neither, so a marked column inside a run
    // would be drawn without them by a shaper that was never told they exist.
    // `clusterPacked` shares a cache line with the code point and pen above.
    if (c->combiningLength() != 0 || c->clusterID() != 0)
        return false;
    // Underline, strike and overline are absent from this list on purpose: a
    // decoration is drawn over the cell by its own pass, which reads the pen
    // and not the glyph route (`CellInstanceBuilder.cpp`'s `wantDecoration` is
    // not gated on ligature coverage), so a decorated cell can shape with its
    // word and still be decorated. Ejecting it instead strands the fragments
    // on either side: an underlined `m` in `tmux` left `t` alone — below the
    // two-cell floor, so unshaped — and `ux` shaping as a run of its own, and
    // the healed forms of all four letters changed.
    //
    // What remains is what changes the GLYPH rather than the paint over it: a
    // wide cell and its continuation, an emoji, hidden text, and the italic
    // face the ligature fonts do not cover.
    return !Ubiquitty::cellPen(*c).attributes.containsAny({ Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation, Ubiquitty::VT::Attribute::Emoji, Ubiquitty::VT::Attribute::ForceEmojiPresentation, Ubiquitty::VT::Attribute::Hidden, Ubiquitty::VT::Attribute::Italic });
}

// How many identical blank cells a run needs before it is worth filling as one.
//
// The scan and the run setup are paid once per run, so a two-cell run — the gap
// between two words — would pay them to skip one column body. Four is the
// shortest run where the scan's own vector step (two cells) has a second
// iteration to amortise over.
inline constexpr int blankRunFillFloor = 4;

// A cell whose ink is nothing, so a run of cells identical to it draws one
// background colour and no glyph, rule or overlay contribution.
//
// Blankness is about what the cell DRAWS, not about which pen it carries: a run
// under a themed background is as uniform as one under the default. Every
// attribute tested here is one that would put ink in a cell holding no code
// point — an underline, a strike, a frame — or one that changes the cell's
// geometry rather than its colour.
//
// A wide continuation is excluded rather than treated as blank: its lead draws
// across it, so filling it as background would paint over the lead's glyph.
inline bool isBlankFillable(const Cell& c)
{
    if (c.codePoint != 0 && c.codePoint != ' ')
        return false;
    if (c.combiningLength() != 0 || c.clusterID() != 0)
        return false;
    const Pen& pen = Ubiquitty::cellPen(c);
    if (pen.attributes.containsAny({ Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation,
            Ubiquitty::VT::Attribute::Underline, Ubiquitty::VT::Attribute::Strike, Ubiquitty::VT::Attribute::Overline,
            Ubiquitty::VT::Attribute::Framed, Ubiquitty::VT::Attribute::Encircled, Ubiquitty::VT::Attribute::Emoji }))
        return false;
    // SGR 58 shapes and the ideogram marks live outside `attributes`, and both
    // draw a rule under a cell that carries no character.
    if (penUnderlineShape(&pen) != Ubiquitty::VT::underlineNone || penIdeogram(&pen) != Ubiquitty::VT::ideogramNone)
        return false;
    return true;
}

/// Whether a cell holds nothing and reads as whitespace.
///
/// An erased cell and a typed space are the same picture, and a healing face
/// substitutes on flanking whitespace — Monaspace draws four variants of `m`
/// by its neighbours alone (default, space-left, space-right, space-both:
/// measured glyphs 504, 789, 869, 709). A distillation that turns blanks into
/// hard boundaries makes the two pictures shape apart: the word being typed
/// sits against erased cells and draws default forms, the same word in
/// scrollback sits against spaces and draws healed ones, and the flip happens
/// at the moment of echo.
///
/// A wide glyph's continuation cell also carries no code point, but it is the
/// tail of INK, not whitespace, so it stays a boundary.
///
/// - Parameter c: The cell to test.
/// - Returns: Whether the shaper should read it as a space.
inline bool ligatureCellShapesAsSpace(const Cell* c)
{
    if (!c || c->codePoint != 0)
        return false;
    if (c->combiningLength() != 0 || c->clusterID() != 0)
        return false;
    return !Ubiquitty::cellPen(*c).attributes.containsAny(
        { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation, Ubiquitty::VT::Attribute::Emoji });
}

/// Distils a row into the form the glyph-supply seam shapes from.
///
/// Both arms call this — the portable builder before it reaches
/// ``GlyphBackend::shapeLigatureRuns``, and the macOS inline walk before its own
/// shaper — so eligibility and shape identity are decided once. A second
/// distillation beside this one would be a second answer to "may these two
/// columns join a run", and the arms would split runs differently on exactly the
/// rows a parity comparison is least likely to hold.
///
/// - Parameters:
///   - rowCells: The row's cells, in display order.
///   - out: One entry per cell to fill; the shorter of the two bounds the walk,
///     so a caller whose scratch is stale writes fewer entries rather than past
///     the end of one of them.
inline void distilLigatureRow(std::span<const Cell> rowCells, std::span<LigatureCell> out)
{
    const std::size_t columns = std::min(rowCells.size(), out.size());
    for (std::size_t column = 0; column < columns; ++column) {
        const Cell& cell = rowCells[column];
        const bool isEligible = ligatureCellEligible(&cell);
        if (isEligible) {
            out[column].codePoint = cell.codePoint;
            out[column].shape = ligatureShapeIdentity(Ubiquitty::cellPen(cell));
            continue;
        }
        // A blank distils as the space it looks like, carrying its own pen's
        // shape so it joins the runs a typed space would join.
        if (ligatureCellShapesAsSpace(&cell)) {
            out[column].codePoint = 0x20u;
            out[column].shape = ligatureShapeIdentity(Ubiquitty::cellPen(cell));
            continue;
        }
        out[column].codePoint = 0u;
        out[column].shape = 0u;
    }
}

// How many cells of SHAPING CONTEXT extend from `from` (inclusive) in
// `direction` (±1), capped at `cap`: eligible cells shape-identical to
// `shapeReference`. A shaped segment is fed to CoreText with this context on
// both sides so the font's contextual rules see across the segment boundary;
// without it a segment edge inside a long operator run looks to the font like
// the run's true end and calt rules mis-fire (e.g. the first three pluses of a
// long "+++++++…" ligate while full-context shaping draws them all as singles).
///
/// - Parameters:
///   - row: The distilled row, from ``distilLigatureRow``.
///   - columns: How many columns `row` holds.
///   - from: The first column to test, inclusive.
///   - direction: `+1` to walk right, `-1` to walk left.
///   - cap: The most columns to report.
///   - shapeReference: The run's shape; a column differing from it ends the walk.
/// - Returns: How many columns the context covers, at most `cap`.
inline int ligatureContextLength(const LigatureCell* row, int columns, int from, int direction, int cap, LigatureCell shapeReference, std::uint32_t shapeMask = ~0u)
{
    int n = 0;
    for (int i = from; n < cap && i >= 0 && i < columns; i += direction) {
        if (row[i].codePoint == 0 || (row[i].shape & shapeMask) != (shapeReference.shape & shapeMask))
            break;
        ++n;
    }
    return n;
}

/// One cell's horizontal span of a shaped ligature strip's atlas slot.
struct LigatureStripSlice {
    float atlasLeft;
    float atlasRight;
};

/// Cuts one covered cell's x-slice out of a shaped ligature strip's slot.
///
/// Every cell a strip covers emits its own quad, sliced to that cell's atlas
/// segment and coloured with that cell's resolved colours, so per-cell colour
/// decisions — link hover, selection recolour — survive ligature shaping. A
/// lead-cell-only emit would collapse them to the run's first colour.
///
/// The run's first and last cell absorb the slot's rasterisation overhang, so
/// ink that leaves its cell box still draws; interior boundaries land on cell
/// segment edges, so adjacent slices tile with no gap and no overlap. The
/// insets hold the outer cells back where the overhang beside them belongs to
/// a neighbouring chunk instead — see ``ligatureStripContextInsets``.
///
/// - Parameters:
///   - column: The cell being sliced.
///   - runStart: The run's first column.
///   - runEnd: One past the run's last column.
///   - segmentLeft: The cell's left edge, in atlas coordinates.
///   - segmentRight: The cell's right edge, in atlas coordinates.
///   - usedWidth: The strip's inked extent, which clamps the slice.
///   - contextInsetLeft: The slot's left share belonging to a neighbour.
///   - contextInsetRight: The slot's right share belonging to a neighbour.
/// - Returns: The slice, ordered and inside the inked extent.
inline LigatureStripSlice ligatureStripCellSlice(int column, int runStart, int runEnd, float segmentLeft, float segmentRight, float usedWidth, float contextInsetLeft, float contextInsetRight)
{
    float atlasLeft = (column == runStart) ? contextInsetLeft : segmentLeft;
    float atlasRight = (column + 1 == runEnd) ? usedWidth - contextInsetRight : segmentRight;
    if (atlasLeft < 0.0f)
        atlasLeft = 0.0f;
    if (atlasRight > usedWidth)
        atlasRight = usedWidth;
    if (atlasRight < atlasLeft)
        atlasRight = atlasLeft;
    return { atlasLeft, atlasRight };
}

/// Cuts one covered cell's slice by ink ownership instead of cell geometry.
///
/// The measured alternative to ``ligatureStripCellSlice``, for a strip whose
/// producer reported where each cluster's coverage landed: the cell's quad
/// spans its own cluster's ink, overhang included, so a healed alternate's
/// tips take their own cell's colours rather than the neighbour's. Two
/// neighbouring quads may overlap where their glyphs' ink does — coverage is
/// additive there, which is what overlapping strokes are.
///
/// The slot's bounds still clamp the answer: an extent is measured against
/// the strip the producer drew, and a consumer must not sample outside the
/// slot however the measurement arrived. An empty extent stays empty through
/// the clamp, so a covered column that owns no ink emits a zero-width quad's
/// worth of nothing.
///
/// - Parameters:
///   - ownInkLeft: The column's ink left edge, in slot atlas pixels.
///   - ownInkRight: One past the column's ink right edge, in the same pixels.
///   - slotWidth: The slot's width, the sampling bound.
/// - Returns: The slice, ordered and inside the slot.
inline LigatureStripSlice ligatureOwnedInkSlice(float ownInkLeft, float ownInkRight, float slotWidth)
{
    float atlasLeft = ownInkLeft;
    float atlasRight = ownInkRight;
    if (atlasLeft < 0.0f)
        atlasLeft = 0.0f;
    if (atlasRight > slotWidth)
        atlasRight = slotWidth;
    if (atlasRight < atlasLeft)
        atlasRight = atlasLeft;
    return { atlasLeft, atlasRight };
}

/// A ligature strip slot's width, in atlas pixels.
///
/// ``stripSlotCells`` cell boxes, the margin on each side, and one pixel of
/// subpixel headroom — the box a whole shaped run rasterises into. Derived
/// here so a frontend building a strip sheet and the arm that already has one
/// cut the same slot from the same inputs; a second spelling of this sum is
/// how two sheets end up a pixel apart and every strip a resample.
///
/// - Parameters:
///   - glyphWidthInPixels: The cell box width inside the margins.
///   - glyphMarginXInPixels: The horizontal margin around the glyph box.
/// - Returns: The strip slot's width in atlas pixels.
inline int ligatureStripSlotWidthInPixels(int glyphWidthInPixels, int glyphMarginXInPixels)
{
    return stripSlotCells * glyphWidthInPixels + 2 * glyphMarginXInPixels + 1;
}

/// The pixel rows a range of atlas slots occupies.
struct AtlasRowBand {
    /// First row to copy.
    int firstRow = 0;
    /// How many rows to copy. Zero when the range holds no slot.
    int rowCount = 0;
};

/// The rows an inclusive slot range sits in, clamped to the sheet.
///
/// An atlas upload copies rows, because that is what a buffer-to-image copy
/// addresses: a slot is a rectangle inside a row band, and copying its columns
/// alone would need a region per row for no saving that matters. So a caller
/// holding dirty slots converts them to the band containing them, and a glyph
/// faulting moves one band rather than the whole sheet.
///
/// Two slots far apart carry the band between them. That is the trade a range
/// makes against a set, and it is still the difference between kilobytes and a
/// megabyte sheet, with no per-slot bookkeeping.
///
/// - Parameters:
///   - firstSlot: First dirty slot, inclusive.
///   - lastSlot: Last dirty slot, inclusive. Below `firstSlot` means none.
///   - columns: Slots per row of the atlas grid.
///   - slotHeightInPixels: One slot's height.
///   - atlasHeightInPixels: The sheet's height, which the band is clamped to.
/// - Returns: The rows to copy, empty when the range holds no slot.
[[nodiscard]] inline AtlasRowBand atlasDirtyRowBand(int firstSlot, int lastSlot, int columns, int slotHeightInPixels, int atlasHeightInPixels)
{
    if (lastSlot < firstSlot || firstSlot < 0 || columns <= 0 || slotHeightInPixels <= 0 || atlasHeightInPixels <= 0)
        return {};
    const int firstRow = (firstSlot / columns) * slotHeightInPixels;
    if (firstRow >= atlasHeightInPixels)
        return {}; // a slot past the sheet names no rows in it
    int rowsPastLast = ((lastSlot / columns) + 1) * slotHeightInPixels;
    if (rowsPastLast > atlasHeightInPixels)
        rowsPastLast = atlasHeightInPixels;
    return { firstRow, rowsPastLast - firstRow };
}

/// Slots a glyph atlas asks for before the device clamps it.
///
/// Bounded by what one frame can reference, not by a census: a screen cannot
/// show more distinct glyphs than it has cells, and an atlas that cannot hold
/// one screenful has no slot left for the next glyph the frame asks for. That
/// glyph draws as bare background, so the failure is a missing character rather
/// than a slow one -- a screenful of CJK prose or a mixed-script editor passes
/// a fixed thousand without trying.
///
/// Twice the screen, because the slots a frame is drawing from are pinned until
/// the next re-baseline: an atlas cut for exactly one screen has nothing
/// evictable left the moment the pins are a screen old, and the frames until
/// the re-baseline draw the shortfall as blank cells. At twice, a frame's
/// misses always fit beside the pins.
///
/// The `columns * 128` floor covers a caller whose screen measurement is small
/// or absent -- a window a few rows tall still cycles through more glyphs than
/// it can show at once as the user scrolls.
///
/// What comes back is a request. `clampAtlasGridToTextureLimit` cuts it to what
/// the device will allocate, which is why asking generously here is safe.
///
/// OSC 66 sized text needs no term of its own. A sized window is cut at the
/// destination its quad fills — a cell, or two on the wide sheet — so a block
/// shows at most as many windows as it covers cells, whatever its scale, and a
/// screen of the largest sized text references no more slots than a screen of
/// plain text. A frame holding more distinct windows than its sheet is one no
/// allowed sheet draws sharp; the builder then draws each block it cannot fit
/// soft as a whole, never band by band (CellInstanceBuilder.cpp,
/// `SizedBlockWindows`).
///
/// Never past ``maxGlyphSheetSlots``, where the cache's slot indices alias.
///
/// - Parameters:
///   - cellsInWidestScreen: Cells in the largest grid this atlas may serve.
///   - columns: The atlas grid's column count, which sets the floor's step.
/// - Returns: Slots to ask for, before the device clamp.
[[nodiscard]] inline int glyphAtlasNominalCapacity(int cellsInWidestScreen, int columns)
{
    const int floorCapacity = (columns > 0 ? columns : 1) * 128;
    const int screenCapacity = cellsInWidestScreen > 0 ? 2 * std::min(cellsInWidestScreen, maxGlyphSheetSlots) : 0;
    return std::min(std::max(floorCapacity, screenCapacity), maxGlyphSheetSlots);
}

/// The grid a coverage sheet is cut to for the screen it serves.
///
/// One rule for every coverage sheet: ``glyphAtlasNominalCapacity`` over the
/// slots one screen can reference, `columns` slots a row, clamped to the
/// texture limit and to ``maxGlyphSheetSlots``. A slot whose glyph spans two
/// cells serves half as many glyphs from the same screen.
///
/// - Parameters:
///   - cellsOnScreen: Cells in the largest grid the sheet serves; zero or less
///     when the caller has not measured one.
///   - cellsPerSlot: Grid cells one slot's glyph covers: 1, or 2 for a wide sheet.
///   - columns: Slots per row before the clamp reshapes the grid.
///   - slot: One slot's extent, in pixels.
///   - maxTextureDimension: The texture limit the sheet is held to.
/// - Returns: The fitted grid; invalid when one slot alone exceeds the limit.
[[nodiscard]] inline AtlasGridFit glyphSheetGridForScreen(int cellsOnScreen, int cellsPerSlot, int columns, IntSize slot, int maxTextureDimension)
{
    const int span = cellsPerSlot > 1 ? cellsPerSlot : 1;
    const int screenSlots = cellsOnScreen > 0 ? cellsOnScreen / span + (cellsOnScreen % span != 0 ? 1 : 0) : 0;
    const int stride = columns > 0 ? columns : 1;
    const int wanted = glyphAtlasNominalCapacity(screenSlots, stride);
    AtlasGridFit fit = clampAtlasGridToTextureLimit({ stride, (wanted + stride - 1) / stride }, slot, maxTextureDimension);
    // The clamp reshapes a grid wider rather than taller, which can carry the
    // product past the index range; cut its rows back inside it.
    if (fit.valid && fit.columns > 0 && static_cast<std::int64_t>(fit.columns) * fit.rows > maxGlyphSheetSlots)
        fit.rows = maxGlyphSheetSlots / fit.columns;
    return fit;
}

/// The colour sheet's grid, from the slot it holds.
///
/// Screen-independent, where ``glyphAtlasNominalCapacity`` reads the screen:
/// the software renderer builds its sheets from a cell size alone and the
/// Vulkan arm builds its from a measured screen, so a capacity derived from the
/// screen would have the two arms holding different numbers of emoji. They are
/// compared against each other frame for frame, and a cluster one arm evicted
/// and the other did not is a difference in the picture that is about neither
/// renderer.
///
/// The count is the floor the Metal sheet already asks for — a hundred and
/// twenty-eight per column at its sixteen — and the reason to keep it is
/// measured: `zwj_all_emoji` puts 1614 distinct colour clusters on one screen,
/// and a sheet that cannot hold a screenful evicts a slot the frame is still
/// drawing from. Those cells then sample whatever lands there next.
///
/// The grid is square rather than the sheet's sixteen columns wide, because at
/// a hundred and twenty-eight rows a full-size slot puts the sheet past the
/// 4096 texels every target guarantees; square, the same count fits inside a
/// third of that on each axis.
///
/// - Parameter slotEdgeInPixels: The square slot's side, from
///   ``colourGlyphSquareInPixels``, which both arms measure the same way.
/// - Parameter maxTextureDimension: The largest 2D texture edge the target
///   allocates. A CPU sheet has no such limit and passes
///   ``sharedColourAtlasTextureLimit`` so it lands on the grid a GPU beside it
///   would.
/// - Returns: The grid, clamped; invalid when one slot alone exceeds the limit.
[[nodiscard]] inline AtlasGridFit colourGlyphAtlasGrid(int slotEdgeInPixels, int maxTextureDimension)
{
    constexpr int wantedSlots = 2048;
    int columns = 1;
    while (columns * columns < wantedSlots)
        ++columns;
    const int rows = (wantedSlots + columns - 1) / columns;
    return clampAtlasGridToTextureLimit({ columns, rows },
        { slotEdgeInPixels, slotEdgeInPixels }, maxTextureDimension);
}

/// The texture limit a sheet with no device behind it assumes.
///
/// The floor every target this ships to guarantees. A CPU sheet is a heap
/// allocation and has no limit of its own, but it is compared against a GPU
/// one: sizing it by a limit no GPU is held to would put the two on different
/// grids on exactly the devices that matter.
inline constexpr int sharedColourAtlasTextureLimit = 4096;

/// The strip grid an arm asks for before the device clamps it.
///
/// Per mode because the working sets differ by an order of magnitude:
/// operator runs are sparse — code screens hold dozens — while a healing
/// font shapes prose, and a dense screen of it runs to hundreds of chunks
/// times the styling variants. The numbers carry headroom over those
/// censuses, and the eviction pathology they guard against is a measured
/// one: an undersized strip cache once cost 704ms of rasterisation thrash
/// in a single frame.
///
/// - Parameter participation: The sheet's probe verdict.
/// - Returns: Columns and rows before ``clampAtlasGridToTextureLimit``.
inline GridSize ligatureStripNominalGrid(LigatureParticipation participation, int worstCaseRuns, IntSize slotInPixels)
{
    // A sheet that cannot hold one frame's strips evicts a slot a live row is
    // still drawing from, and that row samples whatever lands there next —
    // characters that blink and change width between frames. So the capacity
    // starts at the frame's worst case rather than at a census.
    const int floorCapacity = participation == LigatureParticipation::AllEligibleRuns ? 1024 : 256;
    // A caller that cannot measure a screen — the portable sheet builds
    // without one — asks for everything the budget affords rather than for a
    // floor. The budget binds first on any screen worth the name, so both
    // arms land on one capacity instead of the Metal sheet carrying four
    // thousand strips against the portable sheet's thousand.
    int capacity = worstCaseRuns > 0 ? worstCaseRuns : std::numeric_limits<int>::max();

    // Every slot is cut for the longest run the sheet accepts, so the worst
    // case pairs the largest strip count with the widest slot — two extremes
    // that cannot happen together, since the strips of one frame cover the
    // screen's cells between them. Honouring both at once costs a hundred
    // megabytes of texture for six of ink, so the byte budget caps it, and
    // the census reports the runs a capped sheet had to refuse.
    if (slotInPixels.width > 0 && slotInPixels.height > 0) {
        const int slotBytes = slotInPixels.width * slotInPixels.height;
        const int affordable = ligatureStripSheetByteBudget(participation) / slotBytes;
        if (affordable > 0 && capacity > affordable)
            capacity = affordable;
    }
    if (capacity < floorCapacity)
        capacity = floorCapacity;

    // Square-ish, because the grid is clamped per axis: a fixed narrow column
    // count spends the whole budget on height and is cut back to a fraction of
    // its capacity at the texture limit.
    int columns = 1;
    while (columns * slotInPixels.width < (capacity / columns) * slotInPixels.height && columns < capacity)
        ++columns;
    if (columns < 4)
        columns = 4;
    const int rows = (capacity + columns - 1) / columns;
    return { columns, rows };
}
/// A strip sheet's whole layout, decided by Core for every arm.
///
/// Slot dimensions from the cell box, the grid from the participation mode,
/// both clamped to the device the frontend reports. One derivation because a
/// sheet an arm sized itself is a policy fork: the Metal arm carried 4096
/// strips while the portable one carried 128, so the same screen thrashed one
/// cache and rattled inside the other, and no comparison names capacity as
/// the cause.
///
struct LigatureStripPlan {
    int columns = 0;
    int rows = 0;
    int capacity = 0;
    int slotWidthInPixels = 0;
    int slotHeightInPixels = 0;
    bool valid = false;
};

/// Plans the strip sheet for one font and device.
///
/// - Parameters:
///   - glyphWidthInPixels: The cell box width inside the margins.
///   - glyphHeightInPixels: The cell box height inside the margins.
///   - glyphMarginXInPixels: The horizontal margin around the glyph box.
///   - glyphMarginYInPixels: The vertical margin around the glyph box.
///   - participation: The sheet's probe verdict, which sizes the grid.
///   - maxTextureDimension: The device's largest sheet edge, from the arm.
///   - worstCaseRuns: The strips one frame can demand, from
///     ``worstCaseLigatureStripsForCells``; zero keeps the mode's floor.
/// - Returns: The plan, invalid when one slot alone exceeds the device.
inline LigatureStripPlan planLigatureStripSheet(int glyphWidthInPixels, int glyphHeightInPixels,
    int glyphMarginXInPixels, int glyphMarginYInPixels, LigatureParticipation participation,
    int maxTextureDimension, int worstCaseRuns = 0)
{
    LigatureStripPlan plan;
    if (glyphWidthInPixels <= 0 || glyphHeightInPixels <= 0)
        return plan;
    plan.slotWidthInPixels = ligatureStripSlotWidthInPixels(glyphWidthInPixels, glyphMarginXInPixels);
    plan.slotHeightInPixels = glyphHeightInPixels + 2 * glyphMarginYInPixels;
    const GridSize nominal = ligatureStripNominalGrid(participation, worstCaseRuns,
        { plan.slotWidthInPixels, plan.slotHeightInPixels });
    const AtlasGridFit fit = clampAtlasGridToTextureLimit(nominal,
        { plan.slotWidthInPixels, plan.slotHeightInPixels }, maxTextureDimension);
    if (!fit.valid || fit.columns <= 0 || fit.rows <= 0)
        return plan;
    plan.columns = fit.columns;
    plan.rows = fit.rows;
    plan.capacity = fit.columns * fit.rows;
    plan.valid = true;
    return plan;
}

/// A shaped run's inked extent inside its strip slot, in atlas pixels.
///
/// The run's advances plus the margin on each side, and one pixel so a stroke
/// landing exactly on the right edge is not shaved by the truncation; clamped
/// to the slot, which a long run reaches. The builder maps cell slices
/// against this extent, so the arm that fills ``LigatureRun`` and the builder
/// consuming it must agree on it to the pixel.
///
/// - Parameters:
///   - runLength: The run's drawn cells.
///   - cellAdvanceInPixels: The font's advance, in the slot's atlas pixels.
///   - glyphMarginXInPixels: The horizontal margin around the glyph box.
///   - slotWidthInPixels: The strip slot's width, the clamp's ceiling.
/// - Returns: The inked width the run's slices divide.
inline float ligatureStripUsedWidthInAtlasPixels(int runLength, double cellAdvanceInPixels, double glyphMarginXInPixels, float slotWidthInPixels)
{
    const float usedWidth = static_cast<float>(std::ceil(2.0 * glyphMarginXInPixels + runLength * cellAdvanceInPixels + 1.0));
    return usedWidth > slotWidthInPixels ? slotWidthInPixels : usedWidth;
}

/// The optical step the automatic proportional size takes over the grid's.
inline constexpr double proportionalOpticalStep = 1.08;

/// The em size a proportional run is set at when the configuration names
/// none (`font.proportional_size = 0`, Docs/proportional-text.md): one
/// optical step over the grid face's em, to a quarter point, never under it.
///
/// A step rather than a metric match, because two faces can share their
/// x-height and their vertical extent and still not read as the same size:
/// what differs is set width and colour. Every glyph of a monospace carries
/// the same wide advance and its stems are drawn heavy enough to hold that
/// cell; a proportional face condenses `i` and `l`, lightens its stems, and
/// so lays down less ink per line. The eye reads that as a smaller size. SF
/// Mono and SF Pro Text make the point — x-height 6.346 against 6.316 at
/// 12pt, ascent 11.43 and descent 2.89 on both — so a metric match hands
/// back the grid's own size and moves nothing, and against Lilex it hands
/// back less. One optical step, about eight percent at terminal sizes, is
/// what restores the balance, and a quarter point is as fine as the size
/// axis answers. A reader who wants another size names it in
/// `font.proportional_size`, which is what that preference is for.
///
/// - Parameter gridPointSize: The grid face's em size in points.
/// - Returns: The proportional face's em size in points.
inline double automaticProportionalPointSize(double gridPointSize)
{
    if (!std::isfinite(gridPointSize) || gridPointSize <= 0.0) {
        return gridPointSize;
    }
    const double stepped = std::round(gridPointSize * proportionalOpticalStep * 4.0) / 4.0;
    return std::max(stepped, gridPointSize);
}

/// The em size a proportional run is set at, configuration and grid together.
///
/// A configured size wins outright: `font.proportional_size` is the reader's
/// own answer to the balance, and zero is the only value that asks for one.
///
/// - Parameters:
///   - gridPointSize: The grid face's em size in points.
///   - configuredPointSize: `font.proportional_size`, or zero for automatic.
/// - Returns: The proportional face's em size in points.
inline double proportionalFacePointSize(double gridPointSize, double configuredPointSize)
{
    if (configuredPointSize > 0.0) {
        return configuredPointSize;
    }
    return automaticProportionalPointSize(gridPointSize);
}

/// Where a proportional run's text sits in its strip slot and where the
/// strip's quad goes (Docs/proportional-text.md): the one arithmetic every
/// arm's strip sheet places by, so a label aligned on one arm is aligned the
/// same on another.
struct ProportionalStripLayout {
    /// The pen the text is drawn at, in slot pixels from the slot's left edge:
    /// the margin plus the aligned pen's residual.
    double penXInSlotPixels;
    /// The clip's left edge in slot pixels — the run's cells less one margin.
    double clipLeftInSlotPixels;
    /// The clip's right edge in slot pixels: the run's cells, plus the margin
    /// when the text fits and nothing when it overflows, which is the
    /// protocol's promise to the neighbour.
    double clipRightInSlotPixels;
    /// The strip rect's width in slot pixels: the ink to its right edge and a
    /// margin, clamped to the slot.
    float usedWidthInSlotPixels;
    /// The quad's left edge from the pen's whole pixel — the alignment's
    /// whole pixels less the margin.
    float quadLeftInPixels;
    /// The aligned pen's residual, what the strip is keyed on.
    double penResidualInPixels;
    /// The atlas inset keeping linear samples inside the tile's raster guard.
    float rasterInsetInPixels;
    /// The number of bounded tiles covering the full run image.
    int tileCount;
    /// The complete run image's width before bounded tiling.
    float fullWidthInPixels;
};

/// Selects a tile from a full-run layout whose tile index is zero.
///
/// - Parameters:
///   - firstTile: The complete run's layout at tile index zero.
///   - slotWidthInPixels: The raster slot's width, including its guard.
///   - tileIndex: The zero-based tile to select.
/// - Returns: The translated layout, or an empty layout for an invalid index.
inline ProportionalStripLayout proportionalStripTile(ProportionalStripLayout firstTile, int slotWidthInPixels, int tileIndex)
{
    if (tileIndex < 0 || tileIndex >= firstTile.tileCount) {
        return {};
    }
    const float tileWidth = static_cast<float>(slotWidthInPixels) - 2.0f * firstTile.rasterInsetInPixels;
    const float offset = static_cast<float>(tileIndex) * tileWidth;
    firstTile.penXInSlotPixels -= offset;
    firstTile.clipLeftInSlotPixels -= offset;
    firstTile.clipRightInSlotPixels -= offset;
    firstTile.quadLeftInPixels += offset;
    firstTile.usedWidthInSlotPixels = std::min(tileWidth, firstTile.fullWidthInPixels - offset);
    return firstTile;
}

/// Lays a proportional run out in its strip (Docs/proportional-text.md).
///
/// The run's pen moves right by the slack its alignment leaves inside the
/// cells, then floors to the pixel as CoreText floors every pen of a line;
/// the whole pixels place the quad and the residual is drawn and keyed, so a
/// label reads the same at every column. The strip holds the text's ink and
/// a margin each side rather than the run's cells — a run may be wider than
/// the slot, and the quad wants the ink. A run whose text fits keeps the
/// margin's overhang each side, as every glyph does; one that overflows is
/// cut at its last cell.
///
/// - Parameters:
///   - textWidthInPixels: The shaped text's advance, in slot pixels.
///   - widthCells: The cells the run occupies.
///   - horizontalAlign: 0 left, 1 right, 2 centred, 3 justified — a
///     justified run's text already spans its cells (the caller widened it
///     by ``proportionalJustifyGapInPixels`` per word gap), so it places
///     as a left-aligned run with no slack.
///   - penResidualInPixels: The run's pen past its whole pixel, as the
///     builder passes it.
///   - cellAdvanceInPixels: One cell's advance, in slot pixels.
///   - marginXInPixels: The slot's horizontal margin, where the pen starts.
///   - slotWidthInPixels: The slot's width, the clamp's ceiling.
///   - tileIndex: The zero-based horizontal tile of the complete shaped run.
/// - Returns: The layout.
inline ProportionalStripLayout proportionalStripLayout(double textWidthInPixels, int widthCells, std::uint8_t horizontalAlign,
    double penResidualInPixels, double cellAdvanceInPixels, double marginXInPixels, double slotWidthInPixels, int tileIndex = 0)
{
    const double available = static_cast<double>(widthCells) * cellAdvanceInPixels;
    const double slack = std::max(0.0, available - textWidthInPixels);
    const double offset = horizontalAlign == 1 ? slack : (horizontalAlign == 2 ? slack / 2.0 : 0.0);
    const double alignedPen = penResidualInPixels + offset;
    const double alignedWhole = std::floor(alignedPen);
    const double residual = alignedPen - alignedWhole;
    const bool doesFit = textWidthInPixels <= available;
    const double cellsLeft = marginXInPixels + residual - offset;
    ProportionalStripLayout layout {};
    layout.penXInSlotPixels = marginXInPixels + residual;
    layout.clipLeftInSlotPixels = cellsLeft - marginXInPixels;
    layout.clipRightInSlotPixels = cellsLeft + available + (doesFit ? marginXInPixels : 0.0);
    const double inkRight = std::min(marginXInPixels + residual + textWidthInPixels, layout.clipRightInSlotPixels);
    const double fullWidth = std::max(0.0, std::ceil(inkRight) + marginXInPixels);
    // Each visible tile owns disjoint integer pixels. The guard rasterises
    // crossing ink on both sides, so a linear sampler sees the same coverage
    // as one unlimited strip without double-compositing the boundary.
    const double guard = slotWidthInPixels > 2.0 ? 1.0 : 0.0;
    const double tileWidth = std::floor(slotWidthInPixels - 2.0 * guard);
    if (tileWidth <= 0.0 || tileIndex < 0 || !std::isfinite(fullWidth)) {
        return {};
    }
    const double tileCount = std::ceil(fullWidth / tileWidth);
    if (tileCount > static_cast<double>(std::numeric_limits<int>::max())) {
        return {};
    }
    layout.tileCount = static_cast<int>(tileCount);
    layout.fullWidthInPixels = static_cast<float>(fullWidth);
    if (tileIndex >= layout.tileCount) {
        return {};
    }
    const double offsetInPixels = static_cast<double>(tileIndex) * tileWidth;
    layout.usedWidthInSlotPixels = static_cast<float>(std::min(tileWidth, fullWidth - offsetInPixels));
    layout.quadLeftInPixels = static_cast<float>(alignedWhole - marginXInPixels);
    layout.penResidualInPixels = residual;
    layout.rasterInsetInPixels = static_cast<float>(guard);
    layout.penXInSlotPixels += guard - offsetInPixels;
    layout.clipLeftInSlotPixels += guard - offsetInPixels;
    layout.clipRightInSlotPixels += guard - offsetInPixels;
    layout.quadLeftInPixels += static_cast<float>(offsetInPixels);
    return layout;
}

/// The extra advance each word gap of a justified proportional run takes.
///
/// Justification spreads the run's slack — the cells it occupies minus its
/// shaped width — evenly over its word gaps, the way a justified line of
/// print opens its word spaces rather than its letters. One arithmetic for
/// every arm that draws the run, so the two arms open the same gaps.
///
/// - Parameters:
///   - textWidthInPixels: The shaped text's natural advance.
///   - widthCells: The cells the run occupies.
///   - cellAdvanceInPixels: One cell's advance, in the same pixels.
///   - gapCount: The word gaps in the run — the spaces strictly inside it.
/// - Returns: The extra advance per gap, zero when the text overflows its
///   cells or has no gap to open.
inline double proportionalJustifyGapInPixels(double textWidthInPixels, int widthCells, double cellAdvanceInPixels, int gapCount)
{
    if (gapCount <= 0)
        return 0.0;
    const double slack = static_cast<double>(widthCells) * cellAdvanceInPixels - textWidthInPixels;
    return slack > 0.0 ? slack / static_cast<double>(gapCount) : 0.0;
}

/// The word gaps a run's text offers justification: its spaces, less any
/// that lead or trail the text — opening those would move the words rather
/// than space them.
///
/// - Parameter codePoints: The run's text.
/// - Returns: The interior space count.
inline int proportionalJustifyGapCount(std::span<const std::uint32_t> codePoints)
{
    std::size_t first = 0;
    std::size_t last = codePoints.size();
    while (first < last && codePoints[first] == 0x20u)
        ++first;
    while (last > first && codePoints[last - 1] == 0x20u)
        --last;
    int gaps = 0;
    for (std::size_t i = first; i < last; ++i)
        gaps += codePoints[i] == 0x20u ? 1 : 0;
    return gaps;
}

/// The share of a strip slot at each edge that belongs to a neighbouring chunk.
struct LigatureStripContextInsets {
    float leftInAtlasPixels;
    float rightInAtlasPixels;
};

/// How much of a strip slot the run's outer cells must leave alone.
///
/// The slot carries a margin on each side for ink that overhangs its cell,
/// and the run's first and last cell reach into it so that overhang draws. A
/// run longer than ``ligatureMaxCells`` is shaped in chunks, and each chunk is
/// shaped with its neighbours as context so the font's rules see across the
/// split — which puts the neighbouring chunk's glyphs in that same margin.
/// Reaching there paints them a second time, on top of the cells the
/// neighbouring chunk already drew, and the seam reads bolder than the text
/// around it. So an edge with context is held back to the run's own ink, and
/// an edge without context keeps the whole slot.
///
/// - Parameters:
///   - leadingContextCells: Cells shaped before the run, from the candidate.
///   - trailingContextCells: Cells shaped after it, from the candidate.
///   - runLength: The run's drawn cells.
///   - cellAdvanceInPixels: The font's advance, in the slot's atlas pixels.
///   - glyphMarginXInPixels: The horizontal margin around the glyph box.
///   - usedWidthInAtlasPixels: The run's inked extent, the slot's own clamp.
/// - Returns: The insets, zero at an edge the run owns.
inline LigatureStripContextInsets ligatureStripContextInsets(int leadingContextCells, int trailingContextCells,
    int runLength, double cellAdvanceInPixels, double glyphMarginXInPixels, float usedWidthInAtlasPixels)
{
    const float left = leadingContextCells > 0 ? static_cast<float>(glyphMarginXInPixels) : 0.0f;
    float right = 0.0f;
    if (trailingContextCells > 0) {
        const float ownInkRight = static_cast<float>(glyphMarginXInPixels + runLength * cellAdvanceInPixels);
        if (ownInkRight < usedWidthInAtlasPixels)
            right = usedWidthInAtlasPixels - ownInkRight;
    }
    return { left, right };
}

/// One shapeable run ``identifyLigatureRuns`` found.
///
/// `length` drawn cells starting at `startColumn`, with `leftContext` and
/// `rightContext` same-shape cells beside them that are shaped but never
/// drawn.
struct LigatureRunCandidate {
    int startColumn = 0;
    int length = 0;
    int leftContext = 0;
    int rightContext = 0;
};

/// Walks a distilled row and reports each run worth shaping.
///
/// A run is a maximal stretch of same-shape eligible cells, chunked at
/// ``ligatureMaxCells``. Under ``LigatureParticipation/OperatorRuns`` a chunk
/// is reported only when it holds at least two cells and one
/// ``ligatureOperatorCharacter`` — an economy that is correct exactly while
/// the font shapes letters and digits to themselves. A texture-healing font
/// does not, so ``LigatureParticipation/AllEligibleRuns`` reports every
/// chunk of two or more cells: the probe that discovers the font's behaviour
/// and the criterion that maps it to a mode are Core's
/// (``ligatureHealingProbe``, ``decideLigatureParticipation``). Context is
/// measured on both sides with ``ligatureContextLength``, so a chunk split
/// inside one long stretch still shapes as its neighbours' neighbour.
///
/// One walk, called by every backend: which cells join a run, where a long
/// stretch splits, and how much context rides along are seam contract, and a
/// backend re-deriving any of them would split rows differently from the arm
/// it is compared against. The backend's own concerns — keying a strip,
/// caching it, declining when its sheet is full — stay behind `emit`.
///
/// Where the cap cuts is not free: a seam inside a multi-cell cluster loses
/// the cells whose ink the cluster's carrier glyph draws from the far side
/// (``ligatureSeamWindowCells`` holds the measurement). So a cap cut whose
/// flanks could cluster retreats to the rightmost boundary in the window that
/// no cluster spans — a space flank, or a change of
/// ``ligatureOperatorCharacter`` class, since a coding face composes its
/// clusters from one class at a time. A stretch that offers neither inside
/// the window still cuts hard, and a cluster astride that seam still loses
/// ink: the residual is pinned by the seam goldens' unbroken-token row.
///
/// - Parameters:
///   - row: The distilled row, from ``distilLigatureRow``.
///   - columns: How many columns `row` holds.
///   - participation: Which chunks are worth a strip for this font.
///   - emit: Called once per candidate, in column order.
template<typename Emit>
inline void identifyLigatureRuns(const LigatureCell* row, int columns, LigatureParticipation participation, LigatureFaceMixing mixing, Emit&& emit)
{
    // What a shape change has to differ in before it ends a run. A backend
    // that can shape one run through several faces keeps a bold boundary
    // inside the run, because the healed form of a letter depends on the
    // letter beside it and not on the weight it is drawn at: this font shapes
    // `tmux` to a healed `m`, `mux` alone to the default one, and a line whose
    // `t` is bold and whose `mux` is not to the healed one again. Splitting
    // there loses a form CoreText keeps.
    const std::uint32_t shapeMask = mixing == LigatureFaceMixing::ShapesMixedFaces
        ? ~ligatureShapeFaceMask
        : ~0u;
    int column = 0;
    while (column < columns) {
        const LigatureCell first = row[column];
        if (first.codePoint == 0) {
            ++column;
            continue;
        }
        const int start = column;
        const int leftContext = ligatureContextLength(row, columns, start - 1, -1, ligatureContextCells, first, shapeMask);
        int length = 0;
        bool hasCandidate = participation == LigatureParticipation::AllEligibleRuns;
        while (column < columns && length < ligatureMaxCells) {
            const LigatureCell cell = row[column];
            if (cell.codePoint == 0 || (cell.shape & shapeMask) != (first.shape & shapeMask))
                break;
            if (ligatureOperatorCharacter(cell.codePoint))
                hasCandidate = true;
            ++length;
            ++column;
        }
        const bool didChunkLimitCutTheStretch = length == ligatureMaxCells && column < columns
            && row[column].codePoint != 0
            && (row[column].shape & shapeMask) == (first.shape & shapeMask);
        if (didChunkLimitCutTheStretch) {
            const auto seamIsSafe = [&](int boundary) {
                const LigatureCell left = row[boundary - 1];
                const LigatureCell right = row[boundary];
                return left.codePoint == 0x20 || right.codePoint == 0x20
                    || ligatureOperatorCharacter(left.codePoint) != ligatureOperatorCharacter(right.codePoint);
            };
            for (int boundary = start + length; boundary >= start + length - ligatureSeamWindowCells && boundary >= start + 2; --boundary) {
                if (!seamIsSafe(boundary))
                    continue;
                if (boundary < start + length) {
                    length = boundary - start;
                    column = boundary;
                    if (participation == LigatureParticipation::OperatorRuns) {
                        hasCandidate = false;
                        for (int cell = start; cell < start + length && !hasCandidate; ++cell)
                            hasCandidate = ligatureOperatorCharacter(row[cell].codePoint);
                    }
                }
                break;
            }
        }
        if (length >= 2 && hasCandidate) {
            const int rightContext = ligatureContextLength(row, columns, column, +1, ligatureContextCells, first, shapeMask);
            emit(LigatureRunCandidate { start, length, leftContext, rightContext });
        }
    }
}

/// Whether a run's left flank rests on the row edge rather than on a glyph.
///
/// The distinction the shapers need and the context counts alone cannot give:
/// a run at the row's first column and a run beside an ineligible glyph both
/// report no left context, but the two flanks mean different things. A glyph
/// neighbour is a fact the font can see — its healing rules key on the
/// adjacent glyph and answer for themselves — while the row edge is invisible
/// to any OpenType rule, because a contextual lookup's backtrack cannot match
/// past the run's first glyph. The window's padding reads as whitespace to
/// the person looking at it, so the shapers stand a space there: a word heals
/// the same at the margin as it does after a real space, instead of changing
/// letterforms with the column it starts in.
///
/// - Parameter candidate: The run, from ``identifyLigatureRuns``.
/// - Returns: Whether the shaper should stand a virtual space to its left.
inline bool ligatureRunFlanksRowStart(const LigatureRunCandidate& candidate)
{
    return candidate.startColumn == 0;
}

/// ``ligatureRunFlanksRowStart``, for the right flank.
///
/// - Parameters:
///   - candidate: The run, from ``identifyLigatureRuns``.
///   - columns: The row's width.
/// - Returns: Whether the shaper should stand a virtual space to its right.
inline bool ligatureRunFlanksRowEnd(const LigatureRunCandidate& candidate, int columns)
{
    return candidate.startColumn + candidate.length == columns && candidate.rightContext == 0;
}

/// The walk above, filling a caller's buffer instead of calling back.
///
/// For callers a function template cannot reach — Swift sees no
/// instantiation of the form above, so this is the spelling the interop
/// importer hands it. One walk either way: this overload is implemented on
/// the template, not beside it.
///
/// - Parameters:
///   - row: The distilled row, from ``distilLigatureRow``.
///   - columns: How many columns `row` holds.
///   - participation: Which chunks are worth a strip for this font.
///   - outCandidates: Where the candidates land, in column order.
///   - capacity: The most candidates to write.
/// - Returns: How many candidates were written.
inline int identifyLigatureRuns(const LigatureCell* row, int columns, LigatureParticipation participation, LigatureFaceMixing mixing, LigatureRunCandidate* outCandidates, int capacity)
{
    int count = 0;
    identifyLigatureRuns(row, columns, participation, mixing, [&](const LigatureRunCandidate& candidate) {
        if (count < capacity)
            outCandidates[count++] = candidate;
    });
    return count;
}

} // namespace Ubiquitty::Rendering
