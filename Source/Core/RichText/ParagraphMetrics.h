// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// RichText/ParagraphMetrics.h — how a paragraph's spacing and indents place
// its lines (Docs/rich-text.md, "Height" and "Indents").
//
// One set of rules for every layout: Core's table layout and each frontend's
// native shaper call these, so a block measures and draws its paragraphs the
// same way on every arm. Lengths are layout units of 1/4096 point.

#include "Base/SwiftInterop.h"
#include "Core/RichText/BlockStore.h"
#include "Core/RichText/Document.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace Ubiquitty::RichText {

/// Layout units in one twip, a twentieth of a point: 4096 / 20.
[[nodiscard]] constexpr std::int64_t unitsOfTwips(std::int64_t twips) { return (twips * 1024 + (twips < 0 ? -2 : 2)) / 5; }

/// A line's box: its height and its baseline below its top, in layout units.
struct UBIQUITTY_ESCAPABLE LineBox {
    std::uint32_t height = 0;
    std::uint32_t baseline = 0;
};

/// Where a line falls in its paragraph.
struct UBIQUITTY_ESCAPABLE LinePlace {
    /// The line starts the paragraph: it takes the space before and the first
    /// line's indent.
    bool startsParagraph = true; // bool-ok: one of two facts about a line, read together
    /// The line ends the paragraph: it takes the space after.
    bool endsParagraph = true; // bool-ok: one of two facts about a line, read together
};

/// The box a line takes from its natural height and baseline.
///
/// The paragraph's line spacing sets the box: at least or exactly its value,
/// or the natural height times its multiple, with the baseline keeping the
/// share of the box it has at natural height. The space before the paragraph
/// sits above its first line inside that line's box and the space after below
/// its last, so the boxes tile the block with no gap. Terminal lines (`g=1`)
/// keep one row each and take no spacing.
///
/// - Parameters:
///   - paragraph: The line's paragraph.
///   - naturalHeight: The largest ascent plus the largest descent and leading.
///   - naturalBaseline: The largest ascent of the line's faces.
///   - place: Where the line falls in its paragraph.
///   - lineMode: The block's line mode.
/// - Returns: The line's box.
[[nodiscard]] constexpr LineBox lineBox(const Paragraph& paragraph, std::uint32_t naturalHeight, std::uint32_t naturalBaseline, LinePlace place, LineMode lineMode)
{
    if (lineMode == LineMode::Terminal)
        return { naturalHeight, std::min(naturalBaseline, naturalHeight) };
    std::int64_t height = naturalHeight;
    switch (paragraph.lineSpacing) {
    case LineSpacing::Natural:
        break;
    case LineSpacing::AtLeast:
        height = std::max<std::int64_t>(naturalHeight, unitsOfTwips(paragraph.lineSpacingValue));
        break;
    case LineSpacing::Exactly:
        height = unitsOfTwips(paragraph.lineSpacingValue);
        break;
    case LineSpacing::Multiple:
        height = (static_cast<std::int64_t>(naturalHeight) * paragraph.lineSpacingValue + 120) / 240;
        break;
    }
    height = std::max<std::int64_t>(height, 1);
    std::int64_t baseline = naturalHeight ? (static_cast<std::int64_t>(std::min(naturalBaseline, naturalHeight)) * height + naturalHeight / 2) / naturalHeight : 0;
    if (place.startsParagraph) {
        const std::int64_t before = unitsOfTwips(paragraph.spaceBefore);
        height += before;
        baseline += before;
    }
    if (place.endsParagraph)
        height += unitsOfTwips(paragraph.spaceAfter);
    return { static_cast<std::uint32_t>(height), static_cast<std::uint32_t>(std::min(baseline, height)) };
}

/// A line's start from the content box's left edge: the left indent, and the
/// first line's indent from it; no line starts left of the box.
[[nodiscard]] constexpr std::uint32_t lineIndent(const Paragraph& paragraph, LinePlace place)
{
    const std::int64_t indent = unitsOfTwips(paragraph.leftIndent) + (place.startsParagraph ? unitsOfTwips(paragraph.firstIndent) : 0);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(indent, 0));
}

/// The width a line wraps at: the block's width less the line's indent and
/// the right indent, never negative.
[[nodiscard]] constexpr std::uint32_t lineWidth(const Paragraph& paragraph, LinePlace place, std::uint32_t blockWidth)
{
    const std::int64_t width = static_cast<std::int64_t>(blockWidth) - lineIndent(paragraph, place) - unitsOfTwips(paragraph.rightIndent);
    return static_cast<std::uint32_t>(std::max<std::int64_t>(width, 0));
}

/// The stop of a hanging first line's first tab, from the content box's left
/// edge, or zero when the line does not hang: as RTF writers set a bullet, the
/// text after it starts at the indent the paragraph's other lines start at.
[[nodiscard]] constexpr std::uint32_t hangingTabStop(const Paragraph& paragraph, LinePlace place)
{
    if (!place.startsParagraph || paragraph.firstIndent >= 0)
        return 0;
    return static_cast<std::uint32_t>(unitsOfTwips(paragraph.leftIndent));
}

/// The end of the paragraph holding a character: one past its U+000A, or the
/// text's end.
[[nodiscard]] inline std::uint32_t paragraphEnd(std::span<const std::uint32_t> text, std::uint32_t character)
{
    while (character < text.size() && text[character] != '\n')
        ++character;
    return static_cast<std::uint32_t>(std::min<std::size_t>(character + 1, text.size()));
}

/// The start of the paragraph holding a character: one past the U+000A before
/// it, or the text's start.
[[nodiscard]] inline std::uint32_t paragraphStart(std::span<const std::uint32_t> text, std::uint32_t character)
{
    character = static_cast<std::uint32_t>(std::min<std::size_t>(character, text.size()));
    while (character > 0 && text[character - 1] != '\n')
        --character;
    return character;
}

/// Where a line `[first, end)` falls in its paragraph.
[[nodiscard]] inline LinePlace linePlace(std::span<const std::uint32_t> text, std::uint32_t first, std::uint32_t end)
{
    return { .startsParagraph = first == 0 || text[first - 1] == '\n', .endsParagraph = end >= text.size() || text[end - 1] == '\n' };
}

} // namespace Ubiquitty::RichText
