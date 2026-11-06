// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/Width.h"
#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/RangeTable.h"

#include <mutex>

// Range tables live in Width.tables.inc, generated from UCD 17.0.0 by
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

// East_Asian_Width=Ambiguous (UCD 17.0.0; hand-maintained) snapshot, sorted
// by .first. Not in wideRanges: these carry ambiguousWidthClass in the trie and
// the shared flat table, and each terminal's choice resolves them at lookup.
// EAW=A codepoints that are also zero-width (combining marks like
// U+0300..U+036F) are excluded: the zero-width contract wins and they must never
// widen.
static const WidthRange ambiguousRanges[] = {
    { 0x00A1, 0x00A1 },
    { 0x00A4, 0x00A4 },
    { 0x00A7, 0x00A8 },
    { 0x00AA, 0x00AA },
    { 0x00AE, 0x00AE },
    { 0x00B0, 0x00B4 },
    { 0x00B6, 0x00BA },
    { 0x00BC, 0x00BF },
    { 0x00C6, 0x00C6 },
    { 0x00D0, 0x00D0 },
    { 0x00D7, 0x00D8 },
    { 0x00DE, 0x00E1 },
    { 0x00E6, 0x00E6 },
    { 0x00E8, 0x00EA },
    { 0x00EC, 0x00ED },
    { 0x00F0, 0x00F0 },
    { 0x00F2, 0x00F3 },
    { 0x00F7, 0x00FA },
    { 0x00FC, 0x00FC },
    { 0x00FE, 0x00FE },
    { 0x0101, 0x0101 },
    { 0x0111, 0x0111 },
    { 0x0113, 0x0113 },
    { 0x011B, 0x011B },
    { 0x0126, 0x0127 },
    { 0x012B, 0x012B },
    { 0x0131, 0x0133 },
    { 0x0138, 0x0138 },
    { 0x013F, 0x0142 },
    { 0x0144, 0x0144 },
    { 0x0148, 0x014B },
    { 0x014D, 0x014D },
    { 0x0152, 0x0153 },
    { 0x0166, 0x0167 },
    { 0x016B, 0x016B },
    { 0x01CE, 0x01CE },
    { 0x01D0, 0x01D0 },
    { 0x01D2, 0x01D2 },
    { 0x01D4, 0x01D4 },
    { 0x01D6, 0x01D6 },
    { 0x01D8, 0x01D8 },
    { 0x01DA, 0x01DA },
    { 0x01DC, 0x01DC },
    { 0x0251, 0x0251 },
    { 0x0261, 0x0261 },
    { 0x02C4, 0x02C4 },
    { 0x02C7, 0x02C7 },
    { 0x02C9, 0x02CB },
    { 0x02CD, 0x02CD },
    { 0x02D0, 0x02D0 },
    { 0x02D8, 0x02DB },
    { 0x02DD, 0x02DD },
    { 0x02DF, 0x02DF },
    // U+0391..U+03A9 / U+03B1..U+03C9 Greek, U+0401/0410.. Cyrillic — the
    // grapheme-relevant combining range U+0300..U+036F is absent: those combine
    // with a base rather than occupying a cell, so ambiguous width never applies.
    { 0x0391, 0x03A1 },
    { 0x03A3, 0x03A9 },
    { 0x03B1, 0x03C1 },
    { 0x03C3, 0x03C9 },
    { 0x0401, 0x0401 },
    { 0x0410, 0x044F },
    { 0x0451, 0x0451 },
    { 0x2010, 0x2010 },
    { 0x2013, 0x2016 },
    { 0x2018, 0x2019 },
    { 0x201C, 0x201D },
    { 0x2020, 0x2022 },
    { 0x2024, 0x2027 },
    { 0x2030, 0x2030 },
    { 0x2032, 0x2033 },
    { 0x2035, 0x2035 },
    { 0x203B, 0x203B },
    { 0x203E, 0x203E },
    { 0x2074, 0x2074 },
    { 0x207F, 0x207F },
    { 0x2081, 0x2084 },
    { 0x20AC, 0x20AC },
    { 0x2103, 0x2103 },
    { 0x2105, 0x2105 },
    { 0x2109, 0x2109 },
    { 0x2113, 0x2113 },
    { 0x2116, 0x2116 },
    { 0x2121, 0x2122 },
    { 0x2126, 0x2126 },
    { 0x212B, 0x212B },
    { 0x2153, 0x2154 },
    { 0x215B, 0x215E },
    { 0x2160, 0x216B },
    { 0x2170, 0x2179 },
    { 0x2189, 0x2189 },
    { 0x2190, 0x2199 },
    { 0x21B8, 0x21B9 },
    { 0x21D2, 0x21D2 },
    { 0x21D4, 0x21D4 },
    { 0x21E7, 0x21E7 },
    { 0x2200, 0x2200 },
    { 0x2202, 0x2203 },
    { 0x2207, 0x2208 },
    { 0x220B, 0x220B },
    { 0x220F, 0x220F },
    { 0x2211, 0x2211 },
    { 0x2215, 0x2215 },
    { 0x221A, 0x221A },
    { 0x221D, 0x2220 },
    { 0x2223, 0x2223 },
    { 0x2225, 0x2225 },
    { 0x2227, 0x222C },
    { 0x222E, 0x222E },
    { 0x2234, 0x2237 },
    { 0x223C, 0x223D },
    { 0x2248, 0x2248 },
    { 0x224C, 0x224C },
    { 0x2252, 0x2252 },
    { 0x2260, 0x2261 },
    { 0x2264, 0x2267 },
    { 0x226A, 0x226B },
    { 0x226E, 0x226F },
    { 0x2282, 0x2283 },
    { 0x2286, 0x2287 },
    { 0x2295, 0x2295 },
    { 0x2299, 0x2299 },
    { 0x22A5, 0x22A5 },
    { 0x22BF, 0x22BF },
    { 0x2312, 0x2312 },
    { 0x2460, 0x24E9 },
    { 0x24EB, 0x24FF },
    // NOTE: U+2500..U+259F (Box Drawing + Block Elements) are EAW=Ambiguous
    // upstream but excluded from this table — they take VT.cpp's
    // isNarrowGraphicFast hot path (hardcoded width 1, procedurally drawn to
    // fill exactly one cell) and must stay width 1 in both modes.
    { 0x25A0, 0x25A1 },
    { 0x25A3, 0x25A9 },
    { 0x25B2, 0x25B3 },
    { 0x25B6, 0x25B7 },
    { 0x25BC, 0x25BD },
    { 0x25C0, 0x25C1 },
    { 0x25C6, 0x25C8 },
    { 0x25CB, 0x25CB },
    { 0x25CE, 0x25D1 },
    { 0x25E2, 0x25E5 },
    { 0x25EF, 0x25EF },
    { 0x2605, 0x2606 },
    { 0x2609, 0x2609 },
    { 0x260E, 0x260F },
    { 0x261C, 0x261C },
    { 0x261E, 0x261E },
    { 0x2640, 0x2640 },
    { 0x2642, 0x2642 },
    { 0x2660, 0x2661 },
    { 0x2663, 0x2665 },
    { 0x2667, 0x266A },
    { 0x266C, 0x266D },
    { 0x266F, 0x266F },
    { 0x269E, 0x269F },
    { 0x26BF, 0x26BF },
    { 0x26C6, 0x26CD },
    { 0x26CF, 0x26D3 },
    { 0x26D5, 0x26E1 },
    { 0x26E3, 0x26E3 },
    { 0x26E8, 0x26E9 },
    { 0x26EB, 0x26F1 },
    { 0x26F4, 0x26F4 },
    { 0x26F6, 0x26F9 },
    { 0x26FB, 0x26FC },
    { 0x26FE, 0x26FF },
    { 0x273D, 0x273D },
    { 0x2776, 0x277F },
    { 0x2B56, 0x2B59 },
    { 0x3248, 0x324F },
    { 0xFFFD, 0xFFFD },
    // NOTE: the BMP Private Use Area (U+E000..U+F8FF) is EAW=Ambiguous upstream
    // but excluded from this table — it carries Powerline / Nerd-Font icons that
    // Ubiquitty renders as single-cell glyphs; widening them would break prompts.
};

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
