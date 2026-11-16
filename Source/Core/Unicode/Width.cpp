// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/Width.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/RangeTable.h"

#include <mutex>

// Range tables live in Width.tables.inc, generated from UCD 18.0.0 by
// Scripts/build-unicode-tables.py.

struct WidthRange {
    std::uint32_t first;
    std::uint32_t last;
};

#include "Core/Unicode/Width.tables.inc"

using Ubiquitty::Unicode::rangesContain;

// The width trie and the shared property table share their low bits (width 0-1,
// Emoji_Presentation 2), so these read/write both. Layout owned by GraphemeBreak.h.
using Ubiquitty::Unicode::detail::PropEmojiPresentationField;
using Ubiquitty::Unicode::detail::PropWidthField;
using Ubiquitty::Unicode::Width::ambiguousWidthClass;

// Two-stage lookup trie packing width + emoji-presentation into one byte per
// codePoint (bits 0-1 = width 0/1/2 or ambiguousWidthClass, bit 2 =
// Emoji_Presentation), built once
// from the range tables above. Content-deduplicated; on cap overflow the
// lookups fall back to the binary searches.
namespace {
const int widthTrieMaxBlocks = 192;
std::uint8_t widthTrieStage2[widthTrieMaxBlocks][256];
std::uint16_t widthTrieStage1[0x1100]; // codePoint >> 8 for codePoint in [0, 0x10FFFF]
int widthTrieStage2Used = 0;
bool widthTrieBuilt = false;

constexpr int zeroWidthCount = static_cast<int>(sizeof(zeroWidthRanges) / sizeof(zeroWidthRanges[0]));
constexpr int wideCount = static_cast<int>(sizeof(wideRanges) / sizeof(wideRanges[0]));
constexpr int emojiPresentationCount = static_cast<int>(sizeof(emojiPresentationRanges) / sizeof(emojiPresentationRanges[0]));
constexpr int ambiguousCount = static_cast<int>(sizeof(ambiguousRanges) / sizeof(ambiguousRanges[0]));

bool widthFill(std::uint8_t* tmp, std::uint32_t base, const WidthRange* r, int n, std::uint8_t setWidth, bool orEmoji)
{
    bool touched = false;
    for (int i = 0; i < n; ++i) {
        if (r[i].last < base || r[i].first > base + 255)
            continue;
        std::uint32_t lo = r[i].first > base ? r[i].first : base;
        std::uint32_t hi = r[i].last < base + 255 ? r[i].last : base + 255;
        for (std::uint32_t codePoint = lo; codePoint <= hi; ++codePoint) {
            std::uint8_t& b = tmp[codePoint & 0xFF];
            if (orEmoji)
                b = PropEmojiPresentationField::set(b, 1);
            else
                b = PropWidthField::set(b, setWidth);
        }
        touched = true;
    }
    return touched;
}

void buildWidthTrie()
{
    widthTrieBuilt = Ubiquitty::Unicode::buildByteTrie(widthTrieStage1, widthTrieStage2, widthTrieStage2Used,
        /* defaultByte (width 1) */ 0x01, [](std::uint8_t* block, std::uint32_t base) {
            // Ambiguous first, so the two passes after it win where they overlap:
            // the zero-width contract outranks widening, and a codepoint that is
            // both ambiguous and wide is wide under either choice.
            bool didTouchBlock = widthFill(block, base, ambiguousRanges, ambiguousCount, ambiguousWidthClass, false);
            didTouchBlock |= widthFill(block, base, zeroWidthRanges, zeroWidthCount, 0x00, false);
            didTouchBlock |= widthFill(block, base, wideRanges, wideCount, 0x02, false);
            didTouchBlock |= widthFill(block, base, emojiPresentationRanges, emojiPresentationCount, 0x00, true);
            return didTouchBlock;
        });
}

// Emoji_Presentation=Yes implies width 2: derive width from the emoji bit so
// the width bits and emoji bit stay consistent.
inline std::uint8_t foldEmojiPresentationWide(std::uint8_t b)
{
    return PropEmojiPresentationField::get(b) ? PropWidthField::set(b, 2) : b;
}

inline std::uint8_t widthByte(std::uint32_t codePoint)
{
    // call_once publishes the whole trie atomically — concurrent first-touch
    // can't double-build or read mid-build.
    static std::once_flag widthTrieOnce;
    std::call_once(widthTrieOnce, buildWidthTrie);
    if (widthTrieBuilt && codePoint < 0x110000)
        return foldEmojiPresentationWide(widthTrieStage2[widthTrieStage1[codePoint >> 8]][codePoint & 0xFF]);
    // fallback: trie cap exceeded — recompute from the range tables, in the
    // order the trie fills them so both report one width.
    std::uint8_t b = 0x01;
    if (rangesContain(zeroWidthRanges, zeroWidthCount, codePoint))
        b = 0x00;
    else if (rangesContain(wideRanges, wideCount, codePoint))
        b = 0x02;
    else if (rangesContain(ambiguousRanges, ambiguousCount, codePoint))
        b = ambiguousWidthClass;
    if (codePoint >= 0x231A && rangesContain(emojiPresentationRanges, emojiPresentationCount, codePoint))
        b = PropEmojiPresentationField::set(b, 1);
    return foldEmojiPresentationWide(b);
}
} // namespace

int Ubiquitty::Unicode::Width::propertyBlockIndex(std::uint32_t codePoint)
{
    if (!widthTrieBuilt || codePoint >= 0x110000)
        return -1;
    return static_cast<int>(widthTrieStage1[codePoint >> 8]);
}

std::uint8_t Ubiquitty::Unicode::Width::propertyByte(std::uint32_t codePoint)
{
    // The low-code-point shortcuts in the two accessors below are shortcuts, not
    // a different answer: the trie carries U+00AD as zero width and holds no
    // emoji presentation under U+231A, so one probe reports what both report.
    return widthByte(codePoint);
}

bool Ubiquitty::Unicode::Width::isEmojiPresentation(std::uint32_t codePoint)
{
    if (codePoint < 0x231A)
        return false;
    return PropEmojiPresentationField::get(widthByte(codePoint)) != 0;
}

int Ubiquitty::Unicode::Width::forCodePoint(std::uint32_t codePoint, AmbiguousWidth ambiguous)
{
    // Fast path: nothing below U+00A1 appears in any of the four range tables,
    // so every codepoint under it is narrow whatever the ambiguous-width choice
    // is. The cut cannot be raised past the lowest entry any table holds —
    // ambiguous opens at U+00A1, zero width at U+00AD — and a shortcut that
    // reached further would answer for the legacy CJK choice as if it were the
    // default. test_width.cpp holds both accessors to one answer at both.
    constexpr std::uint32_t lowestTabledCodePoint = 0xA1;
    if (codePoint < lowestTabledCodePoint)
        return 1;
    return cellsForWidthClass(PropWidthField::get(widthByte(codePoint)), ambiguous);
}
