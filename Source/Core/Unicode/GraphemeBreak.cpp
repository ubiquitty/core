// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/GraphemeBreak.h"
#include "Core/Unicode/RangeTable.h"
#include "Core/Unicode/Width.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

// UAX #29 grapheme-break property tables, generated from UCD 17.0.0 by
// Scripts/build-grapheme-tables.py, sorted by .first and binary-searched:
//   Extend                — combining marks, variation selectors, skin tones
//   ZWJ                   — U+200D
//   SpacingMark           — Indic spacing marks (GB9a)
//   Prepend               — leading Indic/Arabic format codePoints (GB9b)
//   Regional_Indicator    — flag-emoji halves (GB12/GB13)
//   Extended_Pictographic — emoji bases (GB11 ZWJ sequences)
//   Hangul L / V / T      — conjoining jamo runs (GB6/GB7/GB8)

namespace {

struct Range {
    std::uint32_t first;
    std::uint32_t last;
};

// Extend: Grapheme_Extend=Yes plus emoji modifiers (U+1F3FB..U+1F3FF) and
// variation selectors. Attach to the preceding cluster with no boundary (GB9).
#include "Core/Unicode/GraphemeBreak.tables.inc"

// Extended_Pictographic (extPict, in the generated tables above) is exactly
// the property, not a wider set around it. A range reaching past the property
// reaches over assigned code points rather than the reserved positions such
// widening is meant for: chess pieces, dice and the music notes sit inside
// emoji blocks and are not pictographic. Each one swallowed joins across a
// ZWJ under GB11, and this predicate also gates the VS16 emoji upgrade, so a
// selector after one widens a cell that should stay narrow.

template<int N> bool member(const Range (&r)[N], std::uint32_t codePoint) { return Ubiquitty::Unicode::rangesContain(r, N, codePoint); }

bool isRI(std::uint32_t codePoint) { return codePoint >= 0x1F1E6 && codePoint <= 0x1F1FF; }

// GB9c, Indic conjunct break. Consonants carry their own GCB class, so the
// only table probe left is the linker, and it runs solely while a conjunct is
// open. incbBlockBitmap marks the 256-codepoint blocks holding a linker: 36
// bytes, one cache line, so any other script rejects in a shift and a mask.
inline bool incbBlockMayHold(std::uint32_t codePoint)
{
    const std::uint32_t block = codePoint >> 8;
    return block <= incbHighestBlock && (incbBlockBitmap[block >> 3] & (1u << (block & 7))) != 0;
}
bool isIncbLinker(std::uint32_t codePoint)
{
    return incbBlockMayHold(codePoint) && member(incbLinker, codePoint);
}
// U+00A9 is the lowest Extended_Pictographic codePoint; anything below it can
// never be pictographic (lets callers skip the probe on the ASCII/Latin-1 path).
bool isHangulLV(std::uint32_t codePoint)
{
    if (codePoint < 0xAC00 || codePoint > 0xD7A3)
        return false;
    return ((codePoint - 0xAC00) % 28) == 0; // LV syllables: T-index 0
}
bool isHangulLVT(std::uint32_t codePoint)
{
    if (codePoint < 0xAC00 || codePoint > 0xD7A3)
        return false;
    return ((codePoint - 0xAC00) % 28) != 0; // LVT syllables: T-index != 0
}

// One sorted {first,last,class} table over the disjoint GCB property ranges, so
// a break class is one binary search instead of a per-property cascade.
// Extended_Pictographic stays a separate overlay bit — orthogonal to GCB (a
// skin-tone modifier is GCB=Extend and ExtPict). Hangul LV/LVT collapse to one
// HangulSyllable class; the GB6/7/8 rules split them via the modulo on the raw
// codePoint.
enum GraphemeBreakClass : std::uint8_t {
    ClassOther = 0,
    ClassExtend,
    ClassZWJ,
    ClassSpacingMark,
    ClassPrepend,
    ClassRegionalIndicator,
    ClassHangulL,
    ClassHangulV,
    ClassHangulT,
    ClassHangulSyllable,
    ClassIncbConsonant
};
// Keep the header's pinned copies in lockstep with this enum.
static_assert(Ubiquitty::Unicode::graphemeClassExtend == ClassExtend);
static_assert(Ubiquitty::Unicode::graphemeClassZWJ == ClassZWJ);
static_assert(Ubiquitty::Unicode::graphemeClassPrepend == ClassPrepend);

struct ClassRange {
    std::uint32_t first, last;
    std::uint8_t cls;
};
ClassRange graphemeClassRanges[1024];
int graphemeClassRangeCount = 0;

template<int N> void addClassRanges(const Range (&r)[N], std::uint8_t cls)
{
    for (int i = 0; i < N; ++i) {
        if (graphemeClassRangeCount < static_cast<int>(sizeof(graphemeClassRanges) / sizeof(graphemeClassRanges[0])))
            graphemeClassRanges[graphemeClassRangeCount++] = { r[i].first, r[i].last, cls };
    }
}
void addClassRange(std::uint32_t lo, std::uint32_t hi, std::uint8_t cls)
{
    if (graphemeClassRangeCount < static_cast<int>(sizeof(graphemeClassRanges) / sizeof(graphemeClassRanges[0])))
        graphemeClassRanges[graphemeClassRangeCount++] = { lo, hi, cls };
}

// Two-stage lookup trie over the GCB class: stage1[codePoint>>8] indexes a
// shared 256-entry stage2 block, content-deduplicated. If the cap is exceeded
// the trie is abandoned and lookups fall back to binary search.
const int trieMaxBlocks = 160;
std::uint8_t graphemeTrieStage2[trieMaxBlocks][256];
std::uint16_t graphemeTrieStage1[0x1100]; // codePoint >> 8 for codePoint in [0, 0x10FFFF]
int graphemeTrieStage2Used = 0;
bool graphemeTrieBuilt = false;

void buildGraphemeBreakTrie()
{
    graphemeClassRangeCount = 0;
    addClassRanges(extend, ClassExtend);
    addClassRanges(spacingMark, ClassSpacingMark);
    addClassRanges(prepend, ClassPrepend);
    addClassRanges(hangulL, ClassHangulL);
    addClassRanges(hangulV, ClassHangulV);
    addClassRanges(hangulT, ClassHangulT);
    addClassRange(0x200D, 0x200D, ClassZWJ);
    addClassRange(0x1F1E6, 0x1F1FF, ClassRegionalIndicator);
    addClassRange(0xAC00, 0xD7A3, ClassHangulSyllable);
    // Every InCB=Consonant is GCB=Other, so this splits Other in two rather
    // than displacing a rule class. It also makes them grapheme-relevant,
    // which they are once GB9c exists: the caller's fast path must not swallow
    // a consonant that may close a conjunct.
    addClassRanges(incbConsonant, ClassIncbConsonant);
    // insertion sort by `first`. The GCB property is a partition, so the ranges
    // are disjoint and need no coalescing.
    for (int i = 1; i < graphemeClassRangeCount; ++i) {
        ClassRange key = graphemeClassRanges[i];
        int j = i - 1;
        while (j >= 0 && graphemeClassRanges[j].first > key.first) {
            graphemeClassRanges[j + 1] = graphemeClassRanges[j];
            --j;
        }
        graphemeClassRanges[j + 1] = key;
    }

    // Build the deduplicated 2-stage trie. Block 0 is all-Other (the default
    // for any untouched stage1 slot).
    const int extPictN = static_cast<int>(sizeof(extPict) / sizeof(extPict[0]));
    int ri = 0; // window cursor into the sorted graphemeClassRanges
    graphemeTrieBuilt = Ubiquitty::Unicode::buildByteTrie(graphemeTrieStage1, graphemeTrieStage2, graphemeTrieStage2Used, ClassOther, [&ri](std::uint8_t* block, std::uint32_t base) {
        bool touched = false;
        while (ri < graphemeClassRangeCount && graphemeClassRanges[ri].last < base)
            ++ri;
        for (int r = ri; r < graphemeClassRangeCount && graphemeClassRanges[r].first <= base + 255; ++r) {
            std::uint32_t lo = graphemeClassRanges[r].first > base ? graphemeClassRanges[r].first : base;
            std::uint32_t hi = graphemeClassRanges[r].last < base + 255 ? graphemeClassRanges[r].last : base + 255;
            for (std::uint32_t codePoint = lo; codePoint <= hi; ++codePoint)
                block[codePoint & 0xFF] = graphemeClassRanges[r].cls;
            touched = true;
        }
        // Overlay Extended_Pictographic as bit 0x80 — orthogonal to GCB, so
        // isExtendedPictographic and isGraphemeRelevant share this one trie.
        for (int e = 0; e < extPictN && extPict[e].first <= base + 255; ++e) {
            if (extPict[e].last < base)
                continue;
            std::uint32_t lo = extPict[e].first > base ? extPict[e].first : base;
            std::uint32_t hi = extPict[e].last < base + 255 ? extPict[e].last : base + 255;
            for (std::uint32_t codePoint = lo; codePoint <= hi; ++codePoint)
                block[codePoint & 0xFF] = static_cast<std::uint8_t>(block[codePoint & 0xFF] | 0x80);
            touched = true;
        }
        return touched;
    });
}

// Raw byte from the 2-stage trie (or range fallback): GCB class in the low
// nibble, Extended_Pictographic in bit 0x80. The full-plane flat table is built
// from this.
std::uint8_t graphemeBreakRawByteTrie(std::uint32_t codePoint)
{
    // call_once publishes the whole trie atomically — concurrent first-touch
    // can't double-build.
    static std::once_flag graphemeTrieOnce;
    std::call_once(graphemeTrieOnce, buildGraphemeBreakTrie);
    if (graphemeTrieBuilt) {
        if (codePoint >= 0x110000)
            return 0;
        return graphemeTrieStage2[graphemeTrieStage1[codePoint >> 8]][codePoint & 0xFF];
    }
    // fallback: trie cap was exceeded — recompute from the range tables.
    const ClassRange* match = Ubiquitty::Unicode::findRange(graphemeClassRanges, graphemeClassRangeCount, codePoint);
    std::uint8_t b = match ? match->cls : static_cast<std::uint8_t>(ClassOther);
    if (codePoint >= 0xA9 && member(extPict, codePoint))
        b = static_cast<std::uint8_t>(b | 0x80);
    return b;
}

} // namespace

namespace Ubiquitty::Unicode {

// The header-declared shared property table. Each byte merges four properties:
//   width 0x03 (Unicode::Width::cellsForWidthClass)   emoji 0x04 (Unicode::Width::isEmojiPresentation)
//   GCB class 0x78 (trie class <<3)    ExtPict 0x80 (trie bit 0x80)
namespace detail {
std::uint8_t unicodeProps[0x110000];
} // namespace detail

namespace {
// Which grapheme-trie block a code point's high byte resolves to, or -1 when the
// trie is not in use. The probe fires the build gate the index depends on.
int graphemeBlockIndex(std::uint32_t codePoint)
{
    (void)graphemeBreakRawByteTrie(codePoint);
    if (!graphemeTrieBuilt || codePoint >= 0x110000)
        return -1;
    return static_cast<int>(graphemeTrieStage1[codePoint >> 8]);
}

// One code point's entry, from the two tries' bytes for it.
inline std::uint8_t unicodePropsByte(std::uint32_t codePoint)
{
    const std::uint8_t trie = graphemeBreakRawByteTrie(codePoint); // class 0x0F | ExtPict 0x80
    // One width probe, not two: the byte carries both fields.
    const std::uint8_t widthProperties = Ubiquitty::Unicode::Width::propertyByte(codePoint);
    std::uint8_t b = detail::PropWidthField::set(std::uint8_t { 0 }, detail::PropWidthField::get(widthProperties));
    if (detail::PropEmojiPresentationField::get(widthProperties) != 0)
        b = detail::PropEmojiPresentationField::set(b, 1);
    b = detail::PropGCBClassField::set(b, static_cast<std::uint8_t>(trie & 0x0F)); // trie class nibble -> bits 3-6
    b = detail::PropExtPictField::set(b, detail::PropExtPictField::get(trie)); // ExtPict is bit 7 in both layouts
    return b;
}

// Fills the shared property table for every code point in the plane.
//
// A block of 256 code points draws its answers from one block of each trie, so
// two blocks resolving to the same pair of trie blocks hold the same 256 bytes —
// and the plane's 4,352 blocks resolve onto a few dozen pairs, because every
// unassigned range shares the default block of both. So each distinct pair is
// computed once and copied wherever it repeats.
//
// A trie that outgrew its block budget reports no index, and then the only
// correct answer is the code point at a time.
void fillUnicodeProps()
{
    constexpr std::uint32_t blocks = 0x110000 >> 8;
    struct SeenBlock {
        int grapheme;
        int width;
        std::uint32_t source; // the first block filled with this pair, to copy from
    };
    std::vector<SeenBlock> seen;
    seen.reserve(64);
    for (std::uint32_t block = 0; block < blocks; ++block) {
        const std::uint32_t base = block << 8;
        const int widthBlock = Ubiquitty::Unicode::Width::propertyBlockIndex(base);
        const int graphemeBlock = graphemeBlockIndex(base);
        if (widthBlock >= 0 && graphemeBlock >= 0) {
            const auto match = std::ranges::find_if(seen, [&](const SeenBlock& s) {
                return s.grapheme == graphemeBlock && s.width == widthBlock;
            });
            if (match != seen.end()) {
                std::memcpy(detail::unicodeProps + base, detail::unicodeProps + (match->source << 8), 256);
                continue;
            }
            seen.push_back(SeenBlock { graphemeBlock, widthBlock, block });
        }
        for (std::uint32_t offset = 0; offset < 256; ++offset)
            detail::unicodeProps[base + offset] = unicodePropsByte(base + offset);
    }
}

struct UnicodePropsInit {
    UnicodePropsInit() { fillUnicodeProps(); }
};
// Eager startup build pre-fires both trie call_once gates single-threaded before
// main, so the hot path is guard-free. The TSAN trie-race negative defines this
// macro to omit the eager instance and exercise the concurrent first-touch.
#ifndef UBIQUITTY_UNICODE_NO_EAGER_PROPS_INIT
UnicodePropsInit unicodePropsInit;
#endif
} // namespace

bool isKeycapBase(std::uint32_t codePoint) { return (codePoint >= 0x0030 && codePoint <= 0x0039) || codePoint == 0x0023 || codePoint == 0x002A; }

void graphemeStateReset(GraphemeState* st, std::uint32_t base)
{
    // Below U+00A9 a codePoint is neither Extended_Pictographic (lowest is
    // U+00A9) nor a Regional Indicator, so skip the trie probe on the ASCII /
    // Latin-1 stream.
    if (base < 0xA9) {
        st->riCount = 0;
        st->pictExtend = false;
        st->zwjAfterPict = false;
        st->prevRaw = 0; // ASCII / Latin-1: GCB class 0
        st->incbChain = 0; // ASCII / Latin-1 holds no InCB=Consonant
        return;
    }
    const std::uint8_t raw = graphemeBreakRawByte(base);
    st->riCount = isRI(base) ? 1u : 0u;
    st->pictExtend = detail::PropExtPictField::get(raw) != 0;
    st->zwjAfterPict = false;
    st->prevRaw = raw;
    st->incbChain = detail::PropGCBClassField::get(raw) == ClassIncbConsonant ? 1u : 0u;
}

// Reset state for a new cluster base whose property byte is already loaded —
// identical to graphemeStateReset(base) but without the table re-read. Leaves
// prevRaw to graphemeExtends, which advances it directly.
static inline void resetStateFromRaw(GraphemeState* st, std::uint32_t base, std::uint8_t raw)
{
    if (base < 0xA9) {
        st->riCount = 0;
        st->pictExtend = false;
    } else {
        st->riCount = isRI(base) ? 1u : 0u;
        st->pictExtend = detail::PropExtPictField::get(raw) != 0;
    }
    st->zwjAfterPict = false;
    st->incbChain = detail::PropGCBClassField::get(raw) == ClassIncbConsonant ? 1u : 0u;
}

// Every joiner reaching this point is InCB=Extend and leaves the conjunct
// chain intact, bar ZWNJ, which is the one GCB=Extend codepoint that is not.
// A Linker arms the chain for the consonant that may follow.
void graphemeIncbAdvance(GraphemeState* st, std::uint32_t next)
{
    if (next == 0x200C) {
        st->incbChain = 0; // ZWNJ is the one joiner that is not InCB=Extend
        return;
    }
    if (isIncbLinker(next))
        st->incbChain = 2;
}

bool graphemeExtends(GraphemeState* st, std::uint32_t prev, std::uint32_t next)
{
    // One merged byte for `next`: GCB class (bits 3-6) drives the rule switch,
    // ExtPict (bit 0x80) is read only at GB11.
    bool isControlPair = false;
    const bool shouldJoinControlPair = graphemeControlDecision(prev, next, isControlPair);
    if (isControlPair) {
        graphemeStateReset(st, next);
        return shouldJoinControlPair;
    }

    const std::uint8_t rawNext = graphemeBreakRawByte(next);
    const std::uint8_t nc = detail::PropGCBClassField::get(rawNext);
    // `prev`'s property byte is carried in the state, so its GCB class is free.
    // Advance the carry now: the cluster's last codePoint is `next` on every path.
    const std::uint8_t pc = detail::PropGCBClassField::get(st->prevRaw);
    st->prevRaw = rawNext;

    // GB9 — Extend / ZWJ never start a new cluster (combining marks,
    // VS-15/16, skin-tone modifiers, ZWJ).
    if (nc == ClassExtend || nc == ClassZWJ) {
        if (nc == ClassZWJ) {
            // GB11's Extend run precedes the ZWJ, so the arming must not
            // survive one after it: a diaeresis between the joiner and the next
            // pictograph means the two are separate clusters. And the pictograph
            // that armed this is consumed by it, so a second ZWJ cannot re-arm
            // from what the first already used.
            st->zwjAfterPict = st->pictExtend; // arm GB11
            st->pictExtend = false;
        } else {
            st->zwjAfterPict = false;
            if (st->incbChain != 0)
                graphemeIncbAdvance(st, next);
        }
        st->riCount = 0;
        return true;
    }

    // GB9a — SpacingMark joins the preceding cluster.
    if (nc == ClassSpacingMark) {
        st->riCount = 0;
        st->pictExtend = false;
        st->zwjAfterPict = false;
        st->incbChain = 0; // SpacingMark is not InCB=Extend
        return true;
    }

    // GB11 — ExtPict Extend* ZWJ × ExtPict: a pending "...ZWJ ×" joins the
    // next pictograph.
    if (st->zwjAfterPict && detail::PropExtPictField::get(rawNext) != 0) {
        st->pictExtend = true;
        st->zwjAfterPict = false;
        st->riCount = 0;
        return true;
    }

    // GB9b — a Prepend attaches to the following base.
    if (pc == ClassPrepend) {
        resetStateFromRaw(st, next, rawNext);
        return true;
    }

    // GB12 / GB13 — RI × RI, pairing an in-progress odd run.
    if (pc == ClassRegionalIndicator && nc == ClassRegionalIndicator && (st->riCount % 2u) == 1u) {
        st->riCount++;
        st->pictExtend = false;
        st->zwjAfterPict = false;
        return true;
    }

    // GB6 / GB7 / GB8 — Hangul jamo runs. A HangulSyllable resolves to LV or
    // LVT via the cheap modulo on the raw codePoint.
    if (pc == ClassHangulL && (nc == ClassHangulL || nc == ClassHangulV || nc == ClassHangulSyllable)) {
        resetStateFromRaw(st, next, rawNext); // GB6: L × (L|V|LV|LVT)
        return true;
    }
    if ((pc == ClassHangulV || (pc == ClassHangulSyllable && isHangulLV(prev))) && (nc == ClassHangulV || nc == ClassHangulT)) {
        resetStateFromRaw(st, next, rawNext); // GB7: (V|LV) × (V|T)
        return true;
    }
    if ((pc == ClassHangulT || (pc == ClassHangulSyllable && isHangulLVT(prev))) && nc == ClassHangulT) {
        resetStateFromRaw(st, next, rawNext); // GB8: (T|LVT) × T
        return true;
    }

    // GB9c — Consonant [Extend|Linker]* Linker [Extend|Linker]* × Consonant.
    // The chain is armed by the Linker above; `next` closes the conjunct and
    // becomes the base a following conjunct would extend.
    if (st->incbChain == 2 && nc == ClassIncbConsonant) {
        resetStateFromRaw(st, next, rawNext);
        return true;
    }

    // GB999 — default boundary. `next` starts a fresh cluster.
    resetStateFromRaw(st, next, rawNext);
    return false;
}

} // namespace Ubiquitty::Unicode
