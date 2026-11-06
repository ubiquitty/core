// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "Core/Unicode/GraphemeBreak.h"

namespace Ubiquitty::Unicode::Width {

/// How many cells an East_Asian_Width=Ambiguous code point occupies.
///
/// Each terminal holds its own choice, so two terminals in one process can
/// measure the same text differently and neither moves the other. Narrow is
/// the Western convention and the default; wide is the legacy CJK one. The
/// underlying value is the cell count.
enum class AmbiguousWidth : std::uint8_t {
    Narrow = 1,
    Wide = 2,
};

/// The choice a configured cell count names: 2 is wide, anything else narrow.
[[nodiscard]] constexpr AmbiguousWidth ambiguousWidthFromCells(int cells) noexcept
{
    return cells == 2 ? AmbiguousWidth::Wide : AmbiguousWidth::Narrow;
}

/// The width field an East_Asian_Width=Ambiguous code point carries.
///
/// The tables are built once and never rewritten, so they hold no choice: an
/// ambiguous code point stores this class and the reader resolves it with
/// ``cellsForWidthClass``. A code point that is also zero width, wide or
/// emoji-presenting carries that answer instead, because each outranks the
/// ambiguity under either choice.
inline constexpr std::uint8_t ambiguousWidthClass = 3;

/// The cells a width field occupies under an ambiguous-width choice.
///
/// - Parameters:
///   - widthClass: A `detail::PropWidthField` value: 0, 1, 2 or ``ambiguousWidthClass``.
///   - ambiguous: The choice of the terminal measuring the code point.
/// - Returns: 0, 1 or 2.
[[nodiscard]] constexpr int cellsForWidthClass(std::uint8_t widthClass, AmbiguousWidth ambiguous) noexcept
{
    return widthClass == ambiguousWidthClass ? static_cast<int>(ambiguous) : static_cast<int>(widthClass);
}

/// The cells a Unicode scalar occupies in the grid: 0, 1 or 2.
///
/// - Parameters:
///   - codePoint: The scalar to measure.
///   - ambiguous: The choice of the terminal whose grid it lands in.
/// - Returns: 0, 1 or 2.
[[nodiscard]] int forCodePoint(std::uint32_t codePoint, AmbiguousWidth ambiguous);

// The shared full-plane property byte (detail::Prop*Field layout); callers read
// width / emoji via those fields. The width field may be `ambiguousWidthClass`,
// which only `cellsForWidthClass` turns into cells.
[[nodiscard]] inline std::uint8_t classify(std::uint32_t codePoint) noexcept { return codePoint < 0x110000 ? Ubiquitty::Unicode::detail::unicodeProps[codePoint] : std::uint8_t(0); }

// Emoji_Presentation=Yes: default presentation is a color emoji.
[[nodiscard]] bool isEmojiPresentation(std::uint32_t codePoint);

/// Both width and emoji presentation from one table probe.
///
/// `forCodePoint` and `isEmojiPresentation` read two fields of a single byte, so
/// asking for both separately walks the trie twice. A caller that wants both for
/// every code point in the plane — the shared property table's own build does —
/// pays that twice over a million times.
///
/// - Parameter codePoint: The code point to classify.
/// - Returns: The byte the `detail::Prop*Field` accessors read, carrying the
///   same width and emoji presentation the two accessors report.
[[nodiscard]] std::uint8_t propertyByte(std::uint32_t codePoint);

/// Which trie block a code point's high byte resolves to, or -1 when no trie is in use.
///
/// The table is two-stage: 4,352 blocks of 256 code points map onto a few dozen
/// distinct blocks, because unassigned ranges all share one. A caller filling
/// something per code point can therefore fill one block per distinct index and
/// copy the rest — the index is the identity of the 256 answers behind it.
/// A negative result means the range tables outgrew the trie and the per-code-point
/// path is the only correct one.
///
/// - Parameter codePoint: Any code point in the block.
/// - Returns: The block index, or -1 when the trie is not in use.
[[nodiscard]] int propertyBlockIndex(std::uint32_t codePoint);

// The two emoji variation selectors (UTS #51). VS16 (U+FE0F) forces emoji
// (color, two-cell) presentation on the preceding base; VS15 (U+FE0E) forces
// text (monochrome, single-cell) presentation.
constexpr std::uint32_t variationSelector16 = 0xFE0F; // emoji
constexpr std::uint32_t variationSelector15 = 0xFE0E; // text
[[nodiscard]] inline bool isEmojiVariationSelector(std::uint32_t codePoint) { return codePoint == variationSelector16 || codePoint == variationSelector15; }

// Format characters that select a form or join a run and carry no glyph of
// their own: the variation selectors and the two joiners. A shaper consumes
// them; a fallback that composites a cluster one code point at a time must skip
// them, because a font asked for a glyph it does not have answers with its
// last-resort box, and that box lands on top of the base.
[[nodiscard]] inline bool isInvisibleFormat(std::uint32_t codePoint)
{
    return (codePoint >= 0xFE00 && codePoint <= variationSelector16)
        || (codePoint >= 0xE0100 && codePoint <= 0xE01EF)
        || codePoint == 0x200C || codePoint == 0x200D;
}

} // namespace Ubiquitty::Unicode::Width
