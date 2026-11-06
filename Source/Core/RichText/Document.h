// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/Document.h — a rich text block's content as the RTF reader leaves
// it: the text once, in code points, and the styled runs that cover it
// (Docs/rich-text.md).
//
// The document is what the model stores and the wire carries; the RTF body it
// came from is parsed once, on the host, and dropped. Layout is derived from a
// document and a typography table, never stored beside it.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "Base/FallibleVector.h"

namespace Ubiquitty::RichText {

/// The largest RTF body one code may carry, in bytes.
inline constexpr std::size_t maxBodyBytes = 16384;
/// The most code points one block's text may hold.
inline constexpr int maxCodePoints = 4096;
/// The most styled runs one block may hold.
inline constexpr int maxRuns = 512;
/// The most font table entries one block may hold.
inline constexpr int maxFonts = 16;
/// The most colour table entries one block may hold.
inline constexpr int maxColors = 16;
/// The deepest group nesting a body may reach.
inline constexpr int maxGroupDepth = 32;
/// The longest accepted family identifier, in bytes; a longer name refuses the body.
inline constexpr int maxFamilyNameBytes = std::numeric_limits<std::uint8_t>::max();
/// The smallest and largest sizes a run may take, in half-points.
inline constexpr std::uint16_t minHalfPoints = 2;
inline constexpr std::uint16_t maxHalfPoints = 1000;
/// The size of text that names none: 12 points.
inline constexpr std::uint16_t defaultHalfPoints = 24;

/// The generic family a font entry belongs to, which decides the face it
/// falls back to when its named family is not published.
enum class FamilyClass : std::uint8_t {
    SansSerif,
    Serif,
    Monospace,
};

/// One entry of a body's font table.
struct FontEntry {
    /// The complete UTF-8 family name, at most ``maxFamilyNameBytes`` bytes.
    std::array<char, maxFamilyNameBytes> name {};
    std::uint8_t nameLength = 0;
    FamilyClass familyClass = FamilyClass::SansSerif;

    [[nodiscard]] std::string_view familyName() const { return { name.data(), nameLength }; }

    friend bool operator==(const FontEntry&, const FontEntry&) = default;
};

/// One entry of a body's colour table.
struct ColorEntry {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    /// The entry named no component: the automatic colour, the pen's
    /// foreground.
    bool isAutomatic = true; // bool-ok: one property of a table entry, compared whole

    friend bool operator==(const ColorEntry&, const ColorEntry&) = default;
};

/// The character styles a run carries, as bits.
enum class StyleBit : std::uint8_t {
    Bold = 1 << 0,
    Italic = 1 << 1,
    Underline = 1 << 2,
    Strikethrough = 1 << 3,
};

/// Every bit a run's `styles` may carry.
inline constexpr std::uint8_t allStyleBits = 0x0F;

/// A paragraph's alignment as the body states it.
enum class Alignment : std::uint8_t {
    /// The body named none; the block's own default applies.
    Unspecified,
    Left,
    Center,
    Right,
};

/// The font that text naming no table entry takes.
inline constexpr std::uint8_t noFontEntry = 0xFF;

/// The largest paragraph length the subset reads, in twips: line spacing,
/// the space before and after, and the indents.
inline constexpr int maxParagraphTwips = 32767;
/// The largest line spacing multiple, in 240ths of a single line: ten lines.
inline constexpr int maxLineSpacingMultiple = 2400;

/// How a paragraph's lines are spaced (Docs/rich-text.md, "Height").
enum class LineSpacing : std::uint8_t {
    /// Each line is its tallest face's natural line height.
    Natural,
    /// At least ``Paragraph/lineSpacingValue`` twips.
    AtLeast,
    /// Exactly ``Paragraph/lineSpacingValue`` twips.
    Exactly,
    /// The natural height times ``Paragraph/lineSpacingValue`` 240ths.
    Multiple,
};

/// A paragraph's spacing and indents, as RTF's paragraph properties set them.
/// A paragraph takes the properties in force at its first character, as it
/// takes its alignment.
struct Paragraph {
    LineSpacing lineSpacing = LineSpacing::Natural;
    /// Twips for ``LineSpacing/AtLeast`` and ``LineSpacing/Exactly``; 240ths
    /// of a line for ``LineSpacing/Multiple``; zero otherwise.
    std::uint16_t lineSpacingValue = 0;
    /// The space above the paragraph's first line, in twips.
    std::uint16_t spaceBefore = 0;
    /// The space below the paragraph's last line, in twips.
    std::uint16_t spaceAfter = 0;
    /// Every line's indent from the content box's left edge, in twips.
    std::uint16_t leftIndent = 0;
    /// Every line's indent from the content box's right edge, in twips.
    std::uint16_t rightIndent = 0;
    /// The first line's indent from ``leftIndent``, in twips; negative hangs
    /// the first line to the left of the others.
    std::int16_t firstIndent = 0;

    friend bool operator==(const Paragraph&, const Paragraph&) = default;
};

/// Whether a paragraph's properties are ones the reader could have produced.
[[nodiscard]] constexpr bool isWellFormed(const Paragraph& paragraph)
{
    const auto spacing = static_cast<std::uint8_t>(paragraph.lineSpacing);
    if (spacing > static_cast<std::uint8_t>(LineSpacing::Multiple))
        return false;
    if (paragraph.lineSpacing == LineSpacing::Natural        ? paragraph.lineSpacingValue != 0
            : paragraph.lineSpacing == LineSpacing::Multiple ? paragraph.lineSpacingValue > maxLineSpacingMultiple
                                                             : paragraph.lineSpacingValue == 0 || paragraph.lineSpacingValue > maxParagraphTwips)
        return false;
    return paragraph.spaceBefore <= maxParagraphTwips && paragraph.spaceAfter <= maxParagraphTwips
        && paragraph.leftIndent <= maxParagraphTwips && paragraph.rightIndent <= maxParagraphTwips
        && paragraph.firstIndent >= -maxParagraphTwips;
}

/// U+2028 LINE SEPARATOR, which `\line` writes: it breaks the line and leaves
/// the paragraph whole. Only U+000A, which `\par` writes, ends a paragraph.
inline constexpr std::uint32_t lineSeparator = 0x2028;

/// Whether a code point ends a line: a paragraph break or a line break.
[[nodiscard]] constexpr bool isLineBreak(std::uint32_t codePoint) { return codePoint == '\n' || codePoint == lineSeparator; }

/// The code point a copy or a text reading yields for a document's code point:
/// a line break reads as a line feed, as a paragraph break does.
[[nodiscard]] constexpr std::uint32_t copiedCodePoint(std::uint32_t codePoint) { return codePoint == lineSeparator ? '\n' : codePoint; }

/// One stretch of text in one style.
///
/// Runs tile the text in order with no gaps, and two neighbours never carry
/// the same style: the reader starts a run only when the style changes.
struct Run {
    /// The run's first code point in the document's text.
    std::uint32_t start = 0;
    /// How many code points the run covers; at least one.
    std::uint32_t length = 0;
    /// The size in half-points, ``minHalfPoints`` through ``maxHalfPoints``.
    std::uint16_t halfPoints = defaultHalfPoints;
    /// The font table index, or ``noFontEntry``.
    std::uint8_t font = noFontEntry;
    /// The colour table index; zero is the automatic colour.
    std::uint8_t color = 0;
    /// RTF highlight colour; zero leaves the placing background visible.
    std::uint8_t background = 0;
    /// ``StyleBit`` values, or'd.
    std::uint8_t styles = 0;
    /// The alignment of the paragraph the run is in.
    Alignment alignment = Alignment::Unspecified;
    /// The spacing and indents in force for the run's characters.
    Paragraph paragraph {};

    [[nodiscard]] bool has(StyleBit bit) const { return (styles & static_cast<std::uint8_t>(bit)) != 0; }

    friend bool operator==(const Run&, const Run&) = default;
};

/// A block's content: its text and the runs that style it.
///
/// Paragraph breaks are in the text as U+000A, one per `\par`, line breaks as
/// ``lineSeparator``, one per `\line`, and tabs as U+0009. A copy yields a line
/// feed for either break (``copiedCodePoint``).
struct Document {
    FallibleVector<std::uint32_t> text;
    FallibleVector<Run> runs;
    FallibleVector<FontEntry> fonts;
    std::array<ColorEntry, maxColors> colors {};
    std::uint8_t colorCount = 0;

    [[nodiscard]] std::span<const FontEntry> fontTable() const { return fonts; }
    [[nodiscard]] std::span<const ColorEntry> colorTable() const { return { colors.data(), colorCount }; }

    /// The number of entries retained by the bounded font table.
    [[nodiscard]] std::uint8_t fontCount() const
    {
        assert(fonts.size() <= maxFonts);
        return static_cast<std::uint8_t>(fonts.size());
    }
    /// One font entry's value, independent of the document owner's lifetime.
    /// - Parameter index: An index less than `fontCount`.
    [[nodiscard]] FontEntry fontAt(unsigned index) const { return fonts[index]; }

    /// The number of Unicode scalars retained by the document.
    [[nodiscard]] std::size_t codePointCount() const { return text.size(); }
    /// One Unicode scalar, without exporting the text owner's storage type.
    /// - Parameter index: An index less than `codePointCount`.
    [[nodiscard]] std::uint32_t codePointAt(std::size_t index) const { return text[index]; }
    /// The number of styled runs retained by the document.
    [[nodiscard]] std::size_t runCount() const { return runs.size(); }
    /// One run's value, independent of the document owner's lifetime.
    /// - Parameter index: An index less than `runCount`.
    [[nodiscard]] Run runAt(std::size_t index) const { return runs[index]; }

    /// Clones bounded document arrays without publishing a partial copy.
    /// - Parameter other: The document whose independent ownership is needed.
    /// - Returns: Admission or an allocation refusal; this document stays intact.
    [[nodiscard]] std::expected<void, StorageError> tryAssign(const Document& other)
    {
        if (other.text.size() > static_cast<std::size_t>(maxCodePoints) || other.runs.size() > static_cast<std::size_t>(maxRuns)
            || other.fonts.size() > maxFonts)
            return std::unexpected(StorageError::OverCapacity);
        Document staged;
        if (!staged.text.tryAssign(other.text.begin(), other.text.end()) || !staged.runs.tryAssign(other.runs.begin(), other.runs.end())
            || !staged.fonts.tryAssign(other.fonts.begin(), other.fonts.end()))
            return std::unexpected(StorageError::AllocationRefused);
        staged.colors = other.colors;
        staged.colorCount = other.colorCount;
        *this = std::move(staged);
        return {};
    }

    /// The exact capacities owned by the document's variable-sized arrays.
    [[nodiscard]] std::size_t storageBytes() const noexcept
    {
        return text.capacity() * sizeof(std::uint32_t) + runs.capacity() * sizeof(Run) + fonts.capacity() * sizeof(FontEntry);
    }

    friend bool operator==(const Document&, const Document&) = default;
};

/// Borrows every semantic field needed to validate a document.
struct DocumentView {
    std::span<const std::uint32_t> text;
    std::span<const Run> runs;
    std::span<const FontEntry> fonts;
    std::span<const ColorEntry> colors;
};

/// Whether a document keeps every invariant the reader gives one.
///
/// The caps hold, every code point is a nonzero scalar value, the runs tile
/// the text in order with no empty run and no two neighbours alike, and every
/// run's size, font, colour and styles are in range. A document from the
/// wire is held to this before a terminal takes it.
///
/// - Parameter document: The document to check.
/// - Returns: Whether the document is one the reader could have produced.
[[nodiscard]] bool isWellFormed(const Document& document);
/// Validates borrowed storage with the same invariants as an owned document.
[[nodiscard]] bool isWellFormed(const DocumentView& document);

} // namespace Ubiquitty::RichText
