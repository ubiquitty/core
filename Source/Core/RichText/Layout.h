// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/Layout.h — a rich text block laid out from a typography table
// (Docs/rich-text.md, "Layout" and "Performance").
//
// Lengths are layout units of 1/4096 point: an advance in 1/2048 em times a
// size in half-points, so every sum is an exact integer and the layout is the
// same on every arm that holds the same table. A layout is built once — when
// its block is placed or the table moves — into one allocation: the prefix
// summed right edge of every character, relative to its line's origin, and
// the lines after them. The hit test is two binary searches, the highlight of
// a line two reads; nothing here runs per frame.
//
// Without a table Core lays text out in cells, as the sizing protocol answers
// without a measurer: a character takes its cell width and a line one row.

#include "Core/GridBounds.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Shaping.h"
#include "Core/RichText/TypographyFacts.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Ubiquitty::RichText {

class BlockDrawing;

/// The widest open query, bounded by the terminal's column capacity.
inline constexpr std::uint16_t openWidthCells = static_cast<std::uint16_t>(maxColumns);
/// The tallest open query: a placement plus the maximum content viewport offset.
inline constexpr std::uint16_t openHeightCells = static_cast<std::uint16_t>(maxColumns + maxCodePoints);

/// One laid-out line.
struct LineLayout {
    /// The line's characters, `[first, end)`; a line ending at `\par` or
    /// `\line` holds its line feed last.
    std::uint32_t first = 0;
    std::uint32_t end = 0;
    /// From the block's top, in layout units.
    std::uint32_t top = 0;
    std::uint32_t height = 0;
    /// The baseline, from the line's top.
    std::uint32_t baseline = 0;
    /// Where the line's first character starts, from the block's left edge.
    std::uint32_t origin = 0;
    /// The right edge of the line's last character that is not a space or a
    /// line feed, from `origin`, an ellipsis included.
    std::uint32_t width = 0;
    /// The advance of the ellipsis drawn after `end`; zero for none.
    std::uint32_t ellipsisAdvance = 0;
    /// Whether the line's last character is the line feed `\par` or `\line`
    /// ended it with.
    bool endsWithLineFeed = false; // bool-ok: a fact about the line, read by the hit test
};

/// A block's text laid out in its rectangle.
class Layout {
public:
    /// Lays a document out.
    ///
    /// - Parameters:
    ///   - document: The text and its runs.
    ///   - widthCells: The rectangle's width in cells.
    ///   - heightCells: The rectangle's height in rows.
    ///   - overflow: What happens to text that does not fit.
    ///   - alignment: The alignment of a paragraph that names none.
    ///   - facts: The table, or null to lay out in cells.
    /// - Returns: The layout.
    [[nodiscard]] static Layout lay(const Document& document, std::uint16_t widthCells, std::uint16_t heightCells, Overflow overflow, Alignment alignment, const TypographyFacts* facts, LineMode lineMode = LineMode::Natural, const Shaper* shaper = nullptr, Insets padding = {}, std::uint16_t sourceAnchor = 0);

    /// Lays a block out in its own rectangle.
    [[nodiscard]] static Layout lay(const Block& block, const TypographyFacts* facts, const Shaper* shaper = nullptr)
    {
        Layout layout = lay(block.document, block.width, block.height, block.overflow, block.alignment, facts, block.lineMode, shaper, block.padding, block.viewportTop);
        if (!block.viewportTop)
            layout.placeVertically(block.verticalPlacement);
        return layout;
    }

    /// How many lines the text takes, shown or not.
    [[nodiscard]] std::size_t lineCount() const { return m_lineCount; }
    /// How many of them the rectangle shows.
    [[nodiscard]] std::size_t visibleLineCount() const { return m_visibleLineCount; }
    [[nodiscard]] std::size_t firstVisibleLine() const { return m_firstVisibleLine; }
    [[nodiscard]] std::uint32_t viewportTop() const { return m_viewportTop; }
    /// One line.
    [[nodiscard]] LineLayout line(std::size_t index) const;

    /// A character's right edge from its line's origin.
    [[nodiscard]] std::uint32_t rightEdge(std::size_t index) const { return m_storage[index] & edgeMask; }
    /// A character's left edge from its line's origin: the right edge of the
    /// character before it on the line, or zero for its first.
    [[nodiscard]] std::uint32_t leftEdge(const LineLayout& line, std::size_t index) const { return index == line.first ? 0 : rightEdge(index - 1); }
    /// Whether a character continues the grapheme of the one before it, which
    /// no caret stands between.
    [[nodiscard]] bool continuesGrapheme(std::size_t index) const { return (m_storage[index] & continuationBit) != 0; }

    /// The character a point falls on: the first on the point's line whose
    /// right edge is past it, at the start of its grapheme.
    ///
    /// The line is the last shown line whose top is at or above the point; a
    /// point above the first is on the first, below the last shown on the
    /// last. A point left of a line's first character is on it; one right of
    /// its last is past it, which answers the line's end — before its line
    /// feed when it has one.
    ///
    /// - Parameters:
    ///   - x: From the block's left edge, in layout units.
    ///   - y: From the block's top, in layout units.
    /// - Returns: The character's index, or the line's end.
    [[nodiscard]] std::uint32_t characterAt(std::int64_t x, std::int64_t y) const;

    /// The nearest insertion boundary, including right-to-left visual order.
    [[nodiscard]] std::uint32_t caretAt(std::int64_t x, std::int64_t y) const;
    [[nodiscard]] std::int32_t caretX(const LineLayout&, std::uint32_t character) const;
    [[nodiscard]] std::span<const ShapedSpan> shapedSpans() const { return m_shapedSpans; }
    [[nodiscard]] bool isShaped() const { return m_isShaped; }
    [[nodiscard]] const ShapedText* sourceGeometry() const { return m_sourceGeometry.get(); }
    [[nodiscard]] bool isElided() const { return m_sourceGeometry && m_sourceGeometry->elision; }

    /// The index after the grapheme that starts at `index`.
    [[nodiscard]] std::uint32_t graphemeEnd(std::uint32_t index) const;

    /// The characters a copy yields: the text through the last shown line,
    /// an ellipsis's cut applied.
    [[nodiscard]] std::uint32_t copyableEnd() const { return m_copyableEnd; }

    /// The characters the rectangle shows whole.
    [[nodiscard]] std::uint32_t shownCount() const { return m_shownCount; }
    /// Whether the rectangle shows every character whole.
    [[nodiscard]] bool isFitting() const { return m_isFitting; }
    /// Whether every character was measured from a published table.
    [[nodiscard]] bool isExact() const { return m_isExact; }
    /// The widest line, and the height of every line together.
    [[nodiscard]] std::uint32_t widestLine() const { return m_widestLine; }
    [[nodiscard]] std::uint32_t totalHeight() const { return m_totalHeight; }
    /// One cell, in layout units.
    [[nodiscard]] std::uint32_t cellWidth() const { return m_cellWidth; }
    [[nodiscard]] std::uint32_t cellHeight() const { return m_cellHeight; }
    /// The rectangle, in layout units.
    [[nodiscard]] std::uint32_t blockWidth() const { return m_blockWidth; }
    [[nodiscard]] std::uint32_t blockHeight() const { return m_blockHeight; }
    /// The padding the content box is inset by; none without a table, where
    /// the layout counts in cells.
    [[nodiscard]] Insets padding() const { return m_padding; }

    /// The characters per line and the lines a rectangle fits in the style of
    /// the document's first character: what a query's `c` and `l` answer.
    [[nodiscard]] std::uint32_t charactersPerLine() const { return m_charactersPerLine; }
    [[nodiscard]] std::uint32_t linesThatFit() const { return m_linesThatFit; }

    /// The bytes the layout holds, for the performance contract's test.
    [[nodiscard]] std::size_t storageCapacity() const { return m_storage.capacity(); }

private:
    /// Lays the body out in its content box, every position relative to the
    /// box's top-left; ``lay`` then moves it inside the padding.
    [[nodiscard]] static Layout layContent(const Document& document, std::uint16_t widthCells, std::uint16_t heightCells, Overflow overflow, Alignment alignment, const TypographyFacts* facts, LineMode lineMode, const Shaper* shaper, Insets padding, std::uint16_t sourceAnchor = 0);
    /// Moves every line, caret and edge across and down.
    ///
    /// - Parameters:
    ///   - deltaX: Layout units to the right.
    ///   - deltaY: Layout units down.
    void shift(std::uint32_t deltaX, std::uint32_t deltaY);
    /// Sets the content in the slack its box's height leaves, when it needs
    /// less than the box.
    ///
    /// - Parameter placement: Top, centre or bottom of the slack.
    void placeVertically(VerticalPlacement placement);

    std::shared_ptr<ShapedText> m_sourceGeometry;
    Insets m_padding {};
    /// The content box's top edge: the top padding, in layout units.
    std::uint32_t m_contentTop = 0;
    static constexpr std::uint32_t continuationBit = 1u << 31;
    static constexpr std::uint32_t edgeMask = continuationBit - 1;
    static constexpr std::size_t wordsPerLine = 9;

    /// Where a line's words start in the storage.
    ///
    /// - Parameter line: The line's index.
    /// - Returns: The index of its first word.
    [[nodiscard]] std::size_t lineWord(std::size_t line) const { return m_characterCount + line * wordsPerLine; }
    /// Writes a line's words.
    ///
    /// - Parameters:
    ///   - index: The line's index.
    ///   - line: The line.
    void setLine(std::size_t index, const LineLayout& line);

    /// Every character's edge word, then every line's nine words.
    std::vector<std::uint32_t> m_storage;
    std::vector<ShapedSpan> m_shapedSpans;
    bool m_isShaped = false;
    std::size_t m_characterCount = 0;
    std::size_t m_lineCount = 0;
    std::size_t m_visibleLineCount = 0;
    std::size_t m_firstVisibleLine = 0;
    std::uint32_t m_viewportTop = 0;
    std::uint32_t m_copyableEnd = 0;
    std::uint32_t m_shownCount = 0;
    std::uint32_t m_widestLine = 0;
    std::uint32_t m_totalHeight = 0;
    std::uint32_t m_cellWidth = 0;
    std::uint32_t m_cellHeight = 0;
    std::uint32_t m_blockWidth = 0;
    std::uint32_t m_blockHeight = 0;
    std::uint32_t m_charactersPerLine = 0;
    std::uint32_t m_linesThatFit = 0;
    bool m_isFitting = true;
    bool m_isExact = true;

    friend class LayoutBuilder;
};

/// The table a terminal lays its blocks out by, as its frontend last
/// published it.
struct PublishedTypography {
    /// Null until a frontend publishes; then shared, immutable, with every
    /// pane the frontend draws.
    std::shared_ptr<const TypographyFacts> facts;
    /// Moves on every publication, so a layout built from an older table is
    /// rebuilt.
    std::uint32_t epoch = 0;
    /// Zero disables application reports; screen changes end the subscription.
    std::uint16_t subscriptionID = 0;
    /// Coalesces a font-change report while the program's output is backpressured.
    bool needsReport = false;
    Shaper shaper;
    FontCatalogProvider fontCatalog;
};

/// Measures through the same layout used by drawing and pointer hit tests.
[[nodiscard]] Measurement measure(const Document&, const ShapeRequest&, const PublishedTypography&);

/// Queries the published renderer snapshot and validates its bounded typed result.
[[nodiscard]] FontCatalogPage queryFontCatalog(const FontCatalogQuery&, const PublishedTypography&);

/// Each block's layout, built when first asked for and again when the table
/// moves; a block's serial names one placement, so its document and
/// rectangle never change under its layout.
class LayoutCache {
public:
    /// Installs native geometry received for an unchanged block and font epoch.
    ///
    /// - Parameters:
    ///   - block: The exact placement that was measured.
    ///   - typography: The font facts under which it was measured.
    ///   - geometry: Complete logical lines and grapheme carets.
    /// - Returns: Whether the geometry passed the layout boundary's validation.
    [[nodiscard]] bool adopt(const Block& block, const PublishedTypography& typography, ShapedText geometry);
    /// A block's layout under a table.
    ///
    /// - Parameters:
    ///   - block: The block.
    ///   - typography: The table and its epoch.
    /// - Returns: The layout, valid until the next call that builds one.
    [[nodiscard]] const Layout& layoutOf(const Block& block, const PublishedTypography& typography);

    /// A block's drawing under a table, placed from its layout; defined in
    /// `RichText/Drawing.cpp`.
    ///
    /// - Parameters:
    ///   - block: The block.
    ///   - typography: The table and its epoch.
    /// - Returns: The drawing, shared with every capture that shows it.
    [[nodiscard]] std::shared_ptr<const BlockDrawing> drawingOf(const Block& block, const PublishedTypography& typography);

    /// How many drawings were built, for the performance contract's tests.
    [[nodiscard]] std::uint64_t drawingCount() const { return m_drawingCount; }

    /// Drops the layouts of blocks a store no longer holds.
    ///
    /// - Parameter blocks: The terminal's blocks.
    void retainOnly(const BlockStore& blocks);

    /// How many layouts were built, for the performance contract's tests.
    [[nodiscard]] std::uint64_t buildCount() const { return m_buildCount; }

private:
    /// One block's layout and the table epoch it was built under.
    struct Entry {
        std::uint32_t serial = 0;
        std::uint32_t epoch = 0;
        Layout layout;
        /// Built on first draw from `layout`; dropped with it.
        std::shared_ptr<const BlockDrawing> drawing;
    };
    /// The entry for a block, with its layout current for the table.
    ///
    /// - Parameters:
    ///   - block: The block.
    ///   - typography: The table and its epoch.
    /// - Returns: The entry, stable while the cache holds no more blocks than
    ///   both screens may.
    Entry& entryOf(const Block& block, const PublishedTypography& typography);

    std::vector<Entry> m_entries;
    std::uint64_t m_buildCount = 0;
    std::uint64_t m_drawingCount = 0;
};

} // namespace Ubiquitty::RichText
