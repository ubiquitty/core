// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/Drawing.h — what a frontend draws for a block, and where
// (Docs/rich-text.md, "Rendering").
//
// Core places every grapheme from the block's layout; the frontend sets each
// in the face its style resolves to, at the position given, and never shapes
// or measures for itself. So the drawn glyph, the highlight under it and the
// character a click names are one layout. A drawing is built once per block
// and table, and every frame's capture shares it.

#include "Base/SwiftInterop.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Layout.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Ubiquitty::RichText {

/// One style a block's graphemes are drawn in.
struct UBIQUITTY_ESCAPABLE DrawnStyle {
    Alignment alignment = Alignment::Unspecified;
    /// The spacing and indents of the run's paragraph properties.
    Paragraph paragraph {};
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    /// The face: the table's face the run resolved to, or the run's own class
    /// with no table.
    FamilyClass familyClass = FamilyClass::SansSerif;
    /// The requested native family, or the measured table face for an
    /// unshaped layout. Own the bytes so captures outlive their source block.
    std::array<char, maxFamilyNameBytes> name {};
    std::uint8_t nameLength = 0;

    [[nodiscard]] std::string_view nameView() const { return { name.data(), nameLength }; }
    [[nodiscard]] std::string familyName() const { return std::string(nameView()); }
    /// Copies a parsed font entry across the Swift shaping seam.
    /// - Parameter font: The source family identifier and its fallback class.
    void setFont(const FontEntry& font)
    {
        name = font.name;
        nameLength = font.nameLength;
        familyClass = font.familyClass;
    }
    /// ``StyleBit`` values, decorations included.
    std::uint8_t styles = 0;
    /// The size in half-points.
    float halfPoints = defaultHalfPoints;
    /// The colour, when the run names one.
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    /// The run's colour is the automatic one: the placing pen's foreground,
    /// which the capture resolves through the palette per frame.
    std::uint8_t backgroundRed = 0;
    std::uint8_t backgroundGreen = 0;
    std::uint8_t backgroundBlue = 0;
    std::uint8_t hasBackground = 0; // bool-ok: a byte read by the Swift renderer
    std::uint8_t isAutomaticColor = 1; // bool-ok: a wire-shaped byte the Swift renderer reads
};

/// One grapheme of a block, placed.
struct UBIQUITTY_ESCAPABLE DrawnCluster {
    /// The grapheme's code points in the block's text, `[first, end)`; empty
    /// for the ellipsis, which is U+2026.
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    /// Its pen position and baseline from the block's top-left, and its
    /// advance, in layout units.
    std::uint32_t x = 0;
    std::uint32_t baseline = 0;
    std::uint32_t advance = 0;
    std::uint32_t top = 0;
    std::uint32_t height = 0;
    /// Its index in the drawing's styles.
    std::uint16_t style = 0;
    std::uint16_t isEllipsis = 0; // bool-ok: a wire-shaped half-word the Swift renderer reads
};

/// One stretch of a line a style's decorations are drawn under: a rule for
/// each of the style's underline and strikethrough, stroked once across the
/// whole stretch. A stretch is the longest run of touching graphemes on one
/// line that share a style, so a rule is continuous across letter boundaries
/// and breaks only where the style or the line does.
struct UBIQUITTY_ESCAPABLE DrawnRule {
    /// Its left edge from the block's left and its width, in layout units.
    std::uint32_t x = 0;
    std::uint32_t width = 0;
    /// The baseline of its line from the block's top, in layout units.
    std::uint32_t baseline = 0;
    /// Its index in the drawing's styles.
    std::uint16_t style = 0;
};

/// A block as its frontend draws it.
class BlockDrawing {
public:
    /// Places the shown graphemes of a block's layout.
    ///
    /// - Parameters:
    ///   - block: The block.
    ///   - layout: Its layout.
    ///   - facts: The table the layout was built from, or null.
    /// - Returns: The drawing.
    [[nodiscard]] static BlockDrawing draw(const Block& block, const Layout& layout, const TypographyFacts* facts);

    /// One style per run of the document, in run order.
    [[nodiscard]] std::span<const DrawnStyle> styles() const { return m_styles; }
    /// The graphemes to draw, line by line.
    [[nodiscard]] std::span<const DrawnCluster> clusters() const { return m_clusters; }
    /// The decorated stretches, in line order. A frontend draws a style's
    /// rules from these, never one per grapheme: abutting antialiased rules
    /// leave a seam at every boundary that falls inside a device pixel.
    [[nodiscard]] std::span<const DrawnRule> rules() const { return m_rules; }
    /// Whole shaped lines. A frontend draws these as runs, preserving
    /// substitutions and contextual positioning across graphemes.
    [[nodiscard]] std::span<const ShapedLine> shapedLines() const { return m_shapedLines; }
    /// The block's text, which the clusters index.
    [[nodiscard]] std::span<const std::uint32_t> text() const { return m_text; }
    /// One cell, in layout units: what the frontend's pixel cell maps to.
    [[nodiscard]] std::uint32_t cellWidth() const { return m_cellWidth; }
    [[nodiscard]] std::uint32_t cellHeight() const { return m_cellHeight; }
    /// Visible cell rectangles; empty means the full placement.
    [[nodiscard]] std::span<const VisibleRect> visibleRects() const { return m_visibleRects; }
    [[nodiscard]] Overflow overflow() const { return m_overflow; }
    /// The padding the layout's content box is inset by, in points; a native
    /// rasterizer that lays the text out again wraps it in that box.
    [[nodiscard]] Insets padding() const { return m_padding; }

private:
    std::vector<DrawnStyle> m_styles;
    std::vector<DrawnCluster> m_clusters;
    std::vector<DrawnRule> m_rules;
    std::vector<ShapedLine> m_shapedLines;
    std::vector<std::uint32_t> m_text;
    std::vector<VisibleRect> m_visibleRects;
    Overflow m_overflow = Overflow::Wrap;
    Insets m_padding {};
    std::uint32_t m_cellWidth = 0;
    std::uint32_t m_cellHeight = 0;
};

/// A block the frame shows, in drawable pixels.
struct UBIQUITTY_ESCAPABLE BlockQuad {
    /// The block's serial, and a key that moves whenever what the block's
    /// raster holds would: its table, its automatic colour or the cell's
    /// device size. A frontend rasterises again only when the key moves.
    std::uint32_t serial = 0;
    std::uint64_t contentKey = 0;
    /// The block's rectangle; it may reach outside the viewport.
    float left = 0;
    float top = 0;
    float width = 0;
    float height = 0;
    /// Device pixels per layout unit, across and down.
    float pixelsPerUnitX = 0;
    float pixelsPerUnitY = 0;
    /// The frame's rule settings, as grid text takes them: device pixels per
    /// point, the underline weight's multiplier, the rule line's offset in
    /// device pixels, and whether and by how many device pixels a rule
    /// clears the ink that crosses it.
    float deviceScale = 1;
    float underlineThicknessScale = 1;
    float underlineOffset = 0;
    float skipInkPadding = 0;
    std::uint8_t isSkippingInk = 0; // bool-ok: a wire-shaped byte the Swift renderer reads
    /// The colour a selection highlights the block in, RGBA 0..1 as a cell's
    /// colours are: behind the text, the opaque wash a selected cell's
    /// background takes over the block's own; over it, the selection's tint.
    float highlightRed = 0;
    float highlightGreen = 0;
    float highlightBlue = 0;
    float highlightAlpha = 0;
    /// The automatic colour, resolved through the palette.
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
};

/// One selection highlight inside a block: a band of a line, in drawable
/// pixels, and its block's highlight colour.
struct UBIQUITTY_ESCAPABLE HighlightQuad {
    float left = 0;
    float top = 0;
    float width = 0;
    float height = 0;
    float red = 0;
    float green = 0;
    float blue = 0;
    float alpha = 0;
};

/// A run's face as the raster's font engine measures it at the run's size,
/// in device pixels: what its underline and strike are placed from.
struct UBIQUITTY_ESCAPABLE DecorationFace {
    /// The em size.
    float em = 0;
    /// The face's underline weight; zero when the engine has none.
    float underlineThickness = 0;
    float ascent = 0;
    float descent = 0;
    /// The face's line gap.
    float leading = 0;
    /// The face's x-height; under a pixel takes half the ascent instead.
    float xHeight = 0;
};

/// The rows a run's underline and strike take, in device pixels down the
/// raster, each `[top, bottom)` on whole pixels.
struct UBIQUITTY_ESCAPABLE DecorationRows {
    float underlineTop = 0;
    float underlineBottom = 0;
    float strikeTop = 0;
    float strikeBottom = 0;
};

/// Places a run's underline and strike by the rule grid text's are placed by
/// (Core/Rendering/RuleGeometry.h): the weight and drop from the face's
/// metrics at the run's size, the frame's thickness multiplier and offset,
/// each weight a whole number of device rows and each top snapped the way
/// the cell builder snaps a single underline and a strike.
///
/// - Parameters:
///   - face: The run's face, as the raster measures it.
///   - baseline: The run's baseline, in device pixels down the raster.
///   - quad: The block's place and the frame's rule settings.
/// - Returns: The rows.
[[nodiscard]] DecorationRows decorationRows(const DecorationFace& face, float baseline, const BlockQuad& quad);

/// A raster's ink coverage, one byte a pixel, rows from the top, named for
/// the language seam: Swift imports a span only through an alias.
using InkCoverage = std::span<const std::uint8_t>;
/// A rule's alpha, one byte a pixel, rows from the rule's top.
using RuleAlpha = std::span<std::uint8_t>;

/// A rectangle of whole device pixels, rows from the raster's top.
struct UBIQUITTY_ESCAPABLE PixelRect {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

/// Intersects a decoration's edges with a raster before narrowing to pixel
/// coordinates. Nonfinite, reversed or empty bounds return an empty rectangle.
/// Fractional edges cover the pixels they touch. The returned area cannot
/// exceed the raster, even when a configured stroke extends far beyond it.
///
/// - Parameters:
///   - left: The decoration's left edge in device pixels.
///   - top: Its top edge, down from the raster's top.
///   - right: Its right edge.
///   - bottom: Its bottom edge.
///   - width: The raster's width in pixels.
///   - height: The raster's height in pixels.
/// - Returns: The visible pixel rectangle, or an empty rectangle.
[[nodiscard]] PixelRect clippedRuleRect(double left, double top, double right, double bottom, int width, int height);

/// Writes the alpha skip-ink leaves each pixel of an underline: one less the
/// strongest ink within ``Rendering::skipInkReach`` of it, sampled between
/// pixels as the grid's shader samples its sheet, and no higher than the
/// rule's top row. A descender crossing the rule opens a gap the padding wide
/// each side of it; a letter that sits on the baseline leaves it whole,
/// however close above the rule its overshoot comes.
///
/// Coverage that does not hold `inkWidth` by `inkHeight` bytes leaves the
/// rule whole, and an alpha too short for the rule is left untouched.
///
/// - Parameters:
///   - ink: The coverage of the glyphs the rule runs under.
///   - inkWidth: The coverage's width, in pixels.
///   - inkHeight: The coverage's height, in pixels.
///   - rule: The visible rule pixels in the coverage.
///   - ruleTop: The original, unclipped top edge. Clipping a rule at the
///     raster's top must not move the floor of its ink test.
///   - padding: The skip-ink padding, in device pixels.
///   - alpha: Receives `rule.width * rule.height` bytes, rows from the top.
void skipInkRuleAlpha(InkCoverage ink, int inkWidth, int inkHeight, PixelRect rule, double ruleTop, float padding, RuleAlpha alpha);

/// The characters a selection covers in one block, by serial, and whether it
/// goes on past the block either way in reading order.
struct UBIQUITTY_ESCAPABLE BlockSelection {
    std::uint32_t serial = 0;
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    /// The selection starts before the block's text, in the grid.
    bool isContinuedBefore = false; // bool-ok: a fact the reading order decides, read by the highlight
    /// The selection ends after the block's text, in the grid.
    bool isContinuedAfter = false; // bool-ok: a fact the reading order decides, read by the highlight
};

/// Where a selection's characters fall in each block, named for the language
/// seam: Swift imports a span only through an alias.
using BlockSelections = std::span<BlockSelection>;

/// The highlights a selection draws in a laid-out block, shaped as a text
/// view shapes one (Docs/rich-text.md, "Selection and copy").
///
/// One band per shown line the selection reaches. A line the selection goes
/// on past fills to the block's trailing edge, and one it came into from
/// before fills from the leading edge; a line crossed whole fills the block's
/// width. Otherwise the selected glyphs are covered from the press to the
/// release: one rectangle per natively shaped span, so a bidirectional line
/// keeps its visual pieces, or one from the table's prefix sums. The bands
/// tile the block down its shown height: each runs from its line's top to the
/// next line's, the first from the block's shown top and the last to its
/// bottom where the selection goes on past them, so the block's bands meet
/// the grid rows' fill around it.
///
/// - Parameters:
///   - layout: The block's layout.
///   - selection: The selected characters and whether the selection goes on
///     past the block.
///   - quad: The block's place in the drawable and its highlight colour.
///   - out: Receives the rectangles.
void appendHighlights(const Layout& layout, const BlockSelection& selection, const BlockQuad& quad, std::vector<HighlightQuad>& out);

} // namespace Ubiquitty::RichText
