// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Unicode/Width.h"
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <string>
#include <wchar.h>

// The boundary sweeps below walk the same generated ranges the
// implementation uses, so a table regeneration cannot leave the test
// asserting a stale Unicode version. WidthRange mirrors the definition in
// Width.cpp; the .inc supplies only static const arrays.
struct WidthRange {
    std::uint32_t first;
    std::uint32_t last;
};
#include "Core/Unicode/Width.tables.inc"

using uint32_t = std::uint32_t;

static int failures = 0;

constexpr auto ambiguousNarrow = Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow;
constexpr auto ambiguousWide = Ubiquitty::Unicode::Width::AmbiguousWidth::Wide;

#define CHECK_WIDTH(codePoint, expected)                                                                  \
    do {                                                                                                  \
        int actual = Ubiquitty::Unicode::Width::forCodePoint((codePoint), ambiguousNarrow);               \
        if (actual != (expected)) {                                                                       \
            printf("FAIL U+%04lX: got %d expected %d\n", (unsigned long)(codePoint), actual, (expected)); \
            failures++;                                                                                   \
        }                                                                                                 \
    } while (0)

// The width the flat table answers under a choice: the width field the emit
// path reads, resolved the way the emit path resolves it.
static int flatTableWidth(uint32_t codePoint, Ubiquitty::Unicode::Width::AmbiguousWidth ambiguous)
{
    const std::uint8_t entry = Ubiquitty::Unicode::Width::classify(codePoint);
    return Ubiquitty::Unicode::Width::cellsForWidthClass(Ubiquitty::Unicode::detail::PropWidthField::get(entry), ambiguous);
}

int main()
{
    CHECK_WIDTH(0x0041, 1);
    CHECK_WIDTH(0x0020, 1);
    CHECK_WIDTH(0x007E, 1);
    CHECK_WIDTH(0x00A1, 1);

    CHECK_WIDTH(0x0300, 0);
    CHECK_WIDTH(0x036F, 0);
    CHECK_WIDTH(0x200B, 0);
    CHECK_WIDTH(0x200D, 0);
    CHECK_WIDTH(0x200F, 0);
    CHECK_WIDTH(0x2028, 0);
    CHECK_WIDTH(0x202E, 0);
    CHECK_WIDTH(0x2060, 0);
    CHECK_WIDTH(0x2064, 0);
    CHECK_WIDTH(0x206A, 0);
    CHECK_WIDTH(0x206F, 0);
    CHECK_WIDTH(0xFE00, 0);
    CHECK_WIDTH(0xFE0F, 0);
    CHECK_WIDTH(0xFE20, 0);
    CHECK_WIDTH(0xFE2F, 0);
    CHECK_WIDTH(0xFEFF, 0);
    CHECK_WIDTH(0x1F3FB, 0);
    CHECK_WIDTH(0x1F3FF, 0);
    CHECK_WIDTH(0xE0100, 0);
    CHECK_WIDTH(0xE01EF, 0);

    // Default_Ignorable_Code_Point=Yes — a lone DI codePoint occupies no
    // cell. The non-Cf members the general category misses (Hangul fillers,
    // the E0000 tag block) are the parity-sensitive ones.
    CHECK_WIDTH(0x00AD, 0); // SOFT HYPHEN
    CHECK_WIDTH(0x034F, 0); // COMBINING GRAPHEME JOINER
    CHECK_WIDTH(0x061C, 0); // ARABIC LETTER MARK
    CHECK_WIDTH(0x115F, 0); // HANGUL CHOSEONG FILLER (was wide)
    CHECK_WIDTH(0x1160, 0); // HANGUL JUNGSEONG FILLER
    CHECK_WIDTH(0x17B4, 0); // KHMER VOWEL INHERENT AQ
    CHECK_WIDTH(0x180B, 0); // MONGOLIAN FVS1
    CHECK_WIDTH(0x180E, 0); // MONGOLIAN VOWEL SEPARATOR
    CHECK_WIDTH(0x180F, 0); // MONGOLIAN FVS4
    CHECK_WIDTH(0x2065, 0); // reserved DI (gap inside 2060..206F)
    CHECK_WIDTH(0x3164, 0); // HANGUL FILLER
    CHECK_WIDTH(0xFFA0, 0); // HALFWIDTH HANGUL FILLER
    CHECK_WIDTH(0xFFF0, 0); // reserved DI
    CHECK_WIDTH(0xFFF8, 0); // reserved DI
    CHECK_WIDTH(0x1D173, 0); // MUSICAL SYMBOL BEGIN BEAM
    CHECK_WIDTH(0xE0000, 0); // tag block (deprecated)
    CHECK_WIDTH(0xE0001, 0); // LANGUAGE TAG
    CHECK_WIDTH(0xE007F, 0); // CANCEL TAG
    CHECK_WIDTH(0xE0FFF, 0); // top of the E0000 DI block

    CHECK_WIDTH(0x1100, 2);
    CHECK_WIDTH(0x115E, 2); // last wide Hangul Jamo before the filler
    CHECK_WIDTH(0x2E80, 2);
    CHECK_WIDTH(0x303E, 2);
    CHECK_WIDTH(0x303E, 2); // IDEOGRAPHIC VARIATION INDICATOR; 303F is EAW=N
    CHECK_WIDTH(0x3041, 2);
    CHECK_WIDTH(0x33FF, 2);
    CHECK_WIDTH(0x3400, 2);
    CHECK_WIDTH(0x4DBF, 2);
    CHECK_WIDTH(0x4E00, 2);
    CHECK_WIDTH(0x4E2D, 2);
    CHECK_WIDTH(0x9FFF, 2);
    CHECK_WIDTH(0xA000, 2);
    CHECK_WIDTH(0xA4C6, 2); // last assigned Yi radical; A4CF is unassigned
    CHECK_WIDTH(0xAC00, 2);
    CHECK_WIDTH(0xD7A3, 2);
    CHECK_WIDTH(0xF900, 2);
    CHECK_WIDTH(0xFAFF, 2);
    CHECK_WIDTH(0xFE30, 2);
    CHECK_WIDTH(0xFE4F, 2);
    CHECK_WIDTH(0xFF01, 2);
    CHECK_WIDTH(0xFF60, 2);
    CHECK_WIDTH(0xFFE0, 2);
    CHECK_WIDTH(0xFFE6, 2);
    CHECK_WIDTH(0x1F300, 2);
    CHECK_WIDTH(0x1F600, 2);
    CHECK_WIDTH(0x1F64F, 2);
    CHECK_WIDTH(0x1F90C, 2);
    CHECK_WIDTH(0x1F9FF, 2);
    CHECK_WIDTH(0x20000, 2);
    CHECK_WIDTH(0x2FFFD, 2);
    CHECK_WIDTH(0x30000, 2);
    CHECK_WIDTH(0x3FFFD, 2);

    // BMP emoji presentation codePoints. These sit outside the East
    // Asian Wide block but UAX #51 still requires a two-cell slot so the
    // emoji bitmap has room. Ubiquitty::Unicode::Width::forCodePoint must report 2 for all of them.
    CHECK_WIDTH(0x231A, 2); // ⌚
    CHECK_WIDTH(0x231B, 2); // ⌛
    CHECK_WIDTH(0x23F0, 2); // ⏰
    CHECK_WIDTH(0x2614, 2); // ☔
    CHECK_WIDTH(0x2705, 2); // ✅
    CHECK_WIDTH(0x2728, 2); // ✨
    CHECK_WIDTH(0x274C, 2); // ❌
    CHECK_WIDTH(0x2753, 2); // ❓
    CHECK_WIDTH(0x2B1B, 2); // ⬛
    CHECK_WIDTH(0x2B50, 2); // ⭐
    CHECK_WIDTH(0x2B55, 2); // ⭕

    // Sanity: adjacent narrow codePoints should stay 1-cell.
    CHECK_WIDTH(0x2718, 1); // ✘ (heavy ballot X — not default emoji)
    CHECK_WIDTH(0x2B51, 1); // ★ (between 2B50 and 2B55, narrow)

    // Regional Indicator Symbols (flag emoji halves).
    CHECK_WIDTH(0x1F1E6, 2); // REGIONAL INDICATOR SYMBOL LETTER A
    CHECK_WIDTH(0x1F1F8, 2); // ...LETTER S (used by 🇺🇸)
    CHECK_WIDTH(0x1F1FA, 2); // ...LETTER U
    CHECK_WIDTH(0x1F1FF, 2); // ...LETTER Z

    // Mahjong / Dominoes / Playing Cards. Only those with
    // Emoji_Presentation=Yes are wide; the rest are EAW=N and stay one
    // cell, and U+1F0FF is unassigned.
    CHECK_WIDTH(0x1F004, 2); // MAHJONG TILE RED DRAGON
    CHECK_WIDTH(0x1F0CF, 2); // PLAYING CARD BLACK JOKER
    CHECK_WIDTH(0x1F02B, 1); // MAHJONG TILE BACK
    CHECK_WIDTH(0x1F030, 1); // DOMINO TILE HORIZONTAL BACK
    CHECK_WIDTH(0x1F093, 1); // DOMINO TILE VERTICAL-06-06
    CHECK_WIDTH(0x1F0A0, 1); // PLAYING CARD BACK

    // Enclosed Alphanumeric Supplement entries.
    CHECK_WIDTH(0x1F18E, 2); // NEGATIVE SQUARED AB

    // ASCII fast-path sentinel. If a future Width table edit adds a
    // sub-0x300 zero-width entry, this boundary check fires and
    // forceCodePoint's fast path must be tightened.
    CHECK_WIDTH(0x02FF, 1);

    /// Boundary sweep — for every wide range, the codePoint just
    /// below `first` and just above `last` must not report width=2.
    /// Catches future range-merge typos (off-by-one upper bounds, two
    /// ranges accidentally collapsing). For zero-width ranges, neighbours
    /// must not report width=0. The ranges come from the generated table
    /// itself, so this stays correct across regenerations.
    const WidthRange* wideBounds = wideRanges;
    int wideCount = (int)(sizeof(wideRanges) / sizeof(wideRanges[0]));
    for (int i = 0; i < wideCount; i++) {
        uint32_t before = wideBounds[i].first - 1;
        uint32_t after = wideBounds[i].last + 1;
        /// Cells just outside the range must not report wide unless an
        /// adjacent wide range also claims them (the table has
        /// contiguous SMP runs). Treat "the next entry claims it" as
        /// OK by skipping the check when the next bound's first ==
        /// after, or when the previous bound's last == before.
        bool boundedByNeighbour = false;
        for (int j = 0; j < wideCount; j++) {
            if (j == i)
                continue;
            if (wideBounds[j].first <= before && wideBounds[j].last >= before) {
                boundedByNeighbour = true;
                break;
            }
        }
        if (!boundedByNeighbour && before >= 0x300) {
            int w = Ubiquitty::Unicode::Width::forCodePoint(before, ambiguousNarrow);
            if (w == 2) {
                printf("FAIL boundary U+%04lX (just below range [%lX..%lX]) "
                       "reports wide\n",
                    (unsigned long)before, (unsigned long)wideBounds[i].first, (unsigned long)wideBounds[i].last);
                failures++;
            }
        }
        bool afterBoundedByNeighbour = false;
        for (int j = 0; j < wideCount; j++) {
            if (j == i)
                continue;
            if (wideBounds[j].first <= after && wideBounds[j].last >= after) {
                afterBoundedByNeighbour = true;
                break;
            }
        }
        if (!afterBoundedByNeighbour) {
            int w = Ubiquitty::Unicode::Width::forCodePoint(after, ambiguousNarrow);
            if (w == 2) {
                printf("FAIL boundary U+%04lX (just above range [%lX..%lX]) "
                       "reports wide\n",
                    (unsigned long)after, (unsigned long)wideBounds[i].first, (unsigned long)wideBounds[i].last);
                failures++;
            }
        }
    }
    /// Symmetric sweep for zero-width ranges.
    const WidthRange* zeroBounds = zeroWidthRanges;
    int zeroCount = (int)(sizeof(zeroWidthRanges) / sizeof(zeroWidthRanges[0]));
    for (int i = 0; i < zeroCount; i++) {
        uint32_t after = zeroBounds[i].last + 1;
        bool neighbour = false;
        for (int j = 0; j < zeroCount; j++) {
            if (j == i)
                continue;
            if (zeroBounds[j].first <= after && zeroBounds[j].last >= after) {
                neighbour = true;
                break;
            }
        }
        if (!neighbour) {
            int w = Ubiquitty::Unicode::Width::forCodePoint(after, ambiguousNarrow);
            if (w == 0) {
                printf("FAIL boundary U+%04lX (above zero range [%lX..%lX]) "
                       "reports zero-width\n",
                    (unsigned long)after, (unsigned long)zeroBounds[i].first, (unsigned long)zeroBounds[i].last);
                failures++;
            }
        }
    }

    // [terminal] ambiguous_width: East_Asian_Width=Ambiguous codePoints
    // measure 1 cell under the narrow choice (modern) and 2 under the wide one
    // (legacy CJK). Non-ambiguous codePoints stay put either way.
    //
    // Both choices are asked of the same tables in one run, with nothing set in
    // between and the narrow answers asked again after the wide ones: the tables
    // carry no choice of their own, so no answer may depend on which choice
    // was asked before it.
    //
    // Checked through each accessor. classify() reads the flat table the emit
    // path consults and forCodePoint() reads the trie the render path consults,
    // and they are two routes to one answer: a cell the parser advances two
    // columns for has to be drawn two columns wide, or the row loses its
    // alignment for the rest of its length.
    {
        /// a representative spread of EAW=Ambiguous codePoints
        const uint32_t ambiguous[] = {
            0x00A1, // ¡ INVERTED EXCLAMATION MARK
            0x00A7, // § SECTION SIGN
            0x2026, // … HORIZONTAL ELLIPSIS
            0x2018, // ' LEFT SINGLE QUOTATION MARK
            0x2192, // → RIGHTWARDS ARROW
            0x25CB, // ○ WHITE CIRCLE
            0x2605, // ★ BLACK STAR
            0x2460, // ① circled digit one
            0x0391, // Α GREEK CAPITAL ALPHA
            0x0410, // А CYRILLIC CAPITAL A
        };
        /// never-ambiguous controls: ASCII narrow, CJK wide, a combining mark
        /// that is EAW=A upstream but must stay zero-width, and a box-drawing
        /// glyph that stays narrow at either choice, so a framed TUI keeps its
        /// column count under the wide one.
        const uint32_t narrowAlways[] = { 0x0061 /* a */, 0x007E /* ~ */, 0x2500 /* ─ box drawing */ };
        const uint32_t wideAlways[] = { 0x4E2D /* 中 */, 0xFF21 /* Ａ fullwidth */ };
        const uint32_t zeroAlways[] = { 0x0300 /* combining grave (EAW=A) */ };

        auto expectWidth = [](uint32_t codePoint, Ubiquitty::Unicode::Width::AmbiguousWidth choice, int expected, const char* what) {
            const char* choiceLabel = choice == ambiguousWide ? "wide" : "narrow";
            const int viaFlatTable = flatTableWidth(codePoint, choice);
            const int viaTrie = Ubiquitty::Unicode::Width::forCodePoint(codePoint, choice);
            if (viaFlatTable != expected || viaTrie != expected) {
                printf("FAIL U+%04lX (%s, %s choice): expected %d, classify()=%d forCodePoint()=%d\n",
                    (unsigned long)codePoint, what, choiceLabel, expected, viaFlatTable, viaTrie);
                failures++;
            }
        };

        const Ubiquitty::Unicode::Width::AmbiguousWidth choices[] = { ambiguousNarrow, ambiguousWide, ambiguousNarrow };
        for (const auto choice : choices) {
            for (const uint32_t codePoint : ambiguous)
                expectWidth(codePoint, choice, choice == ambiguousWide ? 2 : 1, "ambiguous");
            for (const uint32_t codePoint : narrowAlways)
                expectWidth(codePoint, choice, 1, "narrow control");
            for (const uint32_t codePoint : wideAlways)
                expectWidth(codePoint, choice, 2, "wide control");
            for (const uint32_t codePoint : zeroAlways)
                expectWidth(codePoint, choice, 0, "zero-width must not widen");
        }

        // The flat table stores the class rather than an answer resolved for
        // one choice, so one immutable table serves every terminal.
        for (const uint32_t codePoint : ambiguous) {
            const std::uint8_t widthClass = Ubiquitty::Unicode::detail::PropWidthField::get(Ubiquitty::Unicode::Width::classify(codePoint));
            if (widthClass != Ubiquitty::Unicode::Width::ambiguousWidthClass) {
                printf("FAIL U+%04lX: the flat table holds width class %u, not the ambiguous class\n",
                    (unsigned long)codePoint, static_cast<unsigned>(widthClass));
                failures++;
            }
        }

        // The trie answers from its own storage rather than from the range-scan
        // fallback, so the agreement above is the built table's, not a cap
        // overflow's.
        if (Ubiquitty::Unicode::Width::propertyBlockIndex(0x25CB) < 0) {
            printf("FAIL the width trie is unbuilt with the ambiguous class in it\n");
            failures++;
        }
    }

    // The dentistry notation symbols measure ONE cell, like every other
    // EAW=Neutral character.
    //
    // A terminal that disagrees with wcwidth(3) about a character does not
    // merely look different from other terminals: it corrupts the input of
    // every application inside it that lays out with wcwidth. Widened, zsh
    // gets 1 where this table says 2, and its redraws overwrite live text.
    //
    // The two neighbours are still the point of the case: U+23BD and U+23CD
    // sit either side of the range, so a change that reached one code point
    // too far in either direction fails here rather than shipping.
    //
    // The range is EAW=N, not Ambiguous, so the ambiguous-width choice must
    // not move it in either direction — the widening is the product's, and
    // a legacy-CJK user choosing wide ambiguous width does not own it.
    {
        int dentistryFailures = 0;
        const std::uint32_t neighbours[] = { 0x23BD, 0x23CD };
        for (const auto choice : { ambiguousNarrow, ambiguousWide }) {
            const char* choiceLabel = choice == ambiguousWide ? "wide" : "narrow";
            for (std::uint32_t codePoint = 0x23BE; codePoint <= 0x23CC; ++codePoint) {
                if (Ubiquitty::Unicode::Width::forCodePoint(codePoint, choice) != 1) {
                    printf("FAIL U+%04lX: dentistry expected width 1 under the %s choice, got %d\n", (unsigned long)codePoint,
                        choiceLabel, Ubiquitty::Unicode::Width::forCodePoint(codePoint, choice));
                    dentistryFailures++;
                }
            }
            for (const std::uint32_t codePoint : neighbours) {
                if (Ubiquitty::Unicode::Width::forCodePoint(codePoint, choice) != 1) {
                    printf("FAIL U+%04lX: the neighbour outside the range expected width 1 under the %s choice, got %d\n",
                        (unsigned long)codePoint, choiceLabel, Ubiquitty::Unicode::Width::forCodePoint(codePoint, choice));
                    dentistryFailures++;
                }
            }
        }
        failures += dentistryFailures;
    }

    // Our width and the system's wcwidth(3) agree across Miscellaneous
    // Technical.
    //
    // This is the dentistry case above, generalised. Pinning
    // "the table says 1" catches that one instance; the FAILURE is a
    // DISAGREEMENT with wcwidth, because every application inside the terminal
    // — zsh above all — lays its own line out with wcwidth and addresses
    // columns by the answer it gets. When the two disagree the application
    // writes over live text, which is data loss rather than a rendering
    // blemish. Comparing the two directly catches the whole class, a widening
    // of the dentistry symbols included.
    //
    // Scoped to U+2300..U+23FF because that is the block the dentistry symbols
    // are in and the one this file's other cases are about. A whole-plane comparison
    // is a different and larger claim: this table parts company
    // with some platforms' wcwidth on emoji presentation, and a test asserting
    // otherwise would be asserting something untrue.
    //
    // wcwidth returns -1 for everything outside the portable character set
    // under LC_ALL=C, which is what a CI image starts a test in, so the check
    // does not take the locale it inherits on trust: it tries the
    // environment's, then the UTF-8 locales C libraries ship under fixed
    // names. The probe is a known-wide CJK ideograph: a locale that cannot
    // call that two cells cannot be asked about this block either. Only a
    // system with no such locale at all skips.
    {
        const char* previousLocale = setlocale(LC_ALL, nullptr);
        std::string savedLocale = previousLocale ? previousLocale : "C";
        bool canMeasureWideCharacters = false;
        for (const char* candidate : { "", "C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8" }) {
            if (setlocale(LC_ALL, candidate) && wcwidth(static_cast<wchar_t>(0x4E00)) == 2) {
                canMeasureWideCharacters = true;
                break;
            }
        }
        if (!canMeasureWideCharacters) {
            printf("SKIP wcwidth agreement: no locale on this system can measure wide characters\n");
        } else {
            int disagreements = 0;
            for (std::uint32_t codePoint = 0x2300; codePoint <= 0x23FF; ++codePoint) {
                const int ours = Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousNarrow);
                const int theirs = wcwidth(static_cast<wchar_t>(codePoint));
                if (theirs < 0)
                    continue; // the C library has no opinion; nothing to disagree with
                if (ours != theirs) {
                    if (disagreements < 8) {
                        printf("FAIL U+%04lX: width %d but wcwidth() says %d — an application laying out "
                               "with wcwidth will address the wrong column\n",
                            (unsigned long)codePoint, ours, theirs);
                    }
                    disagreements++;
                }
            }
            if (disagreements)
                printf("%d code point(s) in U+2300..U+23FF disagree with wcwidth\n", disagreements);
            failures += disagreements;
        }
        setlocale(LC_ALL, savedLocale.c_str());
    }

    // Emoji-presentation predicate: emoji codePoints true, ASCII / CJK /
    // text-presentation symbols false.
    {
        const uint32_t emoji[] = {
            0x1F389,
            0x1F525,
            0x1F600,
            0x1F680,
            0x1F9D1,
            0x231A,
            0x1F44D,
            0x1F1FA,
            0x2B50,
            // Added by Unicode after the hand snapshot this table replaced.
            // Each was already wide, because the width comes from the generated
            // ranges — and not emoji-presenting, because that answer came from
            // the snapshot. Two cells and a text-presentation font.
            0x1F6D8,
            0x1FA89,
            0x1FA8E,
            0x1FABE,
            0x1FAC6,
            0x1FACD,
            0x1FADC,
            0x1FAE9,
            0x1FAEF,
        };
        for (unsigned i = 0; i < sizeof(emoji) / sizeof(emoji[0]); ++i) {
            if (!Ubiquitty::Unicode::Width::isEmojiPresentation(emoji[i])) {
                printf("FAIL U+%04lX: expected emoji-presentation\n", (unsigned long)emoji[i]);
                failures++;
            }
        }
        const uint32_t notEmoji[] = {
            0x0041,
            0x0020,
            0x4E00 /* CJK */,
            0x00A9 /* © text-pres */,
            0x2603 /* snowman, text-pres */,
            0x1F3FB /* skin-tone mod */,
            0x2764 /* ❤ defaults to text presentation without VS16 */,
        };
        for (unsigned i = 0; i < sizeof(notEmoji) / sizeof(notEmoji[0]); ++i) {
            if (Ubiquitty::Unicode::Width::isEmojiPresentation(notEmoji[i])) {
                printf("FAIL U+%04lX: expected NOT emoji-presentation\n", (unsigned long)notEmoji[i]);
                failures++;
            }
        }
    }

    // An Emoji_Presentation=Yes codePoint occupies two cells: width is derived
    // from the emoji bit, so the two properties are consistent for every
    // codePoint in the plane, under either ambiguous-width choice.
    {
        int drift = 0;
        for (uint32_t codePoint = 0; codePoint < 0x110000; ++codePoint) {
            if (!Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint))
                continue;
            const int narrowWidth = Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousNarrow);
            const int wideWidth = Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousWide);
            if (narrowWidth != 2 || wideWidth != 2) {
                if (drift < 12)
                    printf("FAIL U+%05lX: emoji-presentation but width=%d/%d (must be 2)\n",
                        (unsigned long)codePoint, narrowWidth, wideWidth);
                ++drift;
                failures++;
            }
        }
        if (drift)
            printf("  %d emoji-presentation codePoint(s) have width != 2 (table drift)\n", drift);
        /// The colored circles/squares, the heavy equals sign, and the
        /// enclosed-CJK squared emoji occupy two cells, like every
        /// Emoji_Presentation=Yes codePoint.
        const uint32_t sameSizeEmoji[] = { 0x1F534, 0x1F7E0, 0x1F7E1, 0x1F7E2, 0x1F7EB, 0x1F7F0, 0x1F232, 0x1F201 };
        for (unsigned i = 0; i < sizeof(sameSizeEmoji) / sizeof(sameSizeEmoji[0]); ++i) {
            if (Ubiquitty::Unicode::Width::forCodePoint(sameSizeEmoji[i], ambiguousNarrow) != 2) {
                printf("FAIL U+%05lX: expected width 2 (color emoji occupies a cell pair)\n", (unsigned long)sameSizeEmoji[i]);
                failures++;
            }
        }
    }

    // One probe must report what two report, for every code point in the plane.
    // The shared property table's build reads both fields from propertyByte, so
    // a disagreement anywhere would put a wrong width or a wrong emoji flag into
    // the table every frame reads, for exactly the code points that disagree.
    // The shared table is filled a BLOCK at a time: blocks resolving to the same
    // pair of trie blocks are copied rather than recomputed, so a mis-keyed copy
    // hands one block another's 256 entries. All four fields ride in one byte, so
    // checking the table's width against the trie's own answer catches a wrong
    // block whichever field the damage would have shown in. Both choices are
    // asked, because an ambiguous entry resolves differently under each.
    int tableFailures = 0;
    for (std::uint32_t codePoint = 0; codePoint < 0x110000; ++codePoint) {
        const std::uint8_t entry = Ubiquitty::Unicode::Width::classify(codePoint);
        const bool isEmoji = Ubiquitty::Unicode::detail::PropEmojiPresentationField::get(entry) != 0;
        const int narrowWidth = flatTableWidth(codePoint, ambiguousNarrow);
        const int wideWidth = flatTableWidth(codePoint, ambiguousWide);
        if (narrowWidth != Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousNarrow)
            || wideWidth != Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousWide)
            || isEmoji != Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint)) {
            if (tableFailures < 8)
                printf("shared table wrong at U+%04X (block %u): width %d/%d vs %d/%d\n", codePoint,
                    codePoint >> 8, narrowWidth, wideWidth, Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousNarrow),
                    Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousWide));
            tableFailures++;
        }
    }
    if (tableFailures) {
        printf("the shared property table disagrees with the width trie at %d code point(s)\n", tableFailures);
        failures += tableFailures;
    }

    int combinedFailures = 0;
    for (std::uint32_t codePoint = 0; codePoint < 0x110000; ++codePoint) {
        const std::uint8_t properties = Ubiquitty::Unicode::Width::propertyByte(codePoint);
        const std::uint8_t widthClass = Ubiquitty::Unicode::detail::PropWidthField::get(properties);
        const int narrowWidth = Ubiquitty::Unicode::Width::cellsForWidthClass(widthClass, ambiguousNarrow);
        const int wideWidth = Ubiquitty::Unicode::Width::cellsForWidthClass(widthClass, ambiguousWide);
        const bool isEmoji = Ubiquitty::Unicode::detail::PropEmojiPresentationField::get(properties) != 0;
        if (narrowWidth != Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousNarrow)
            || wideWidth != Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousWide)
            || isEmoji != Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint)) {
            if (combinedFailures < 8) {
                printf("propertyByte disagrees at U+%04X: width %d/%d vs %d/%d, emoji %d vs %d\n", codePoint, narrowWidth, wideWidth,
                    Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousNarrow),
                    Ubiquitty::Unicode::Width::forCodePoint(codePoint, ambiguousWide), isEmoji ? 1 : 0,
                    Ubiquitty::Unicode::Width::isEmojiPresentation(codePoint) ? 1 : 0);
            }
            combinedFailures++;
        }
    }
    if (combinedFailures) {
        printf("propertyByte disagrees with the two accessors at %d code point(s)\n", combinedFailures);
        failures += combinedFailures;
    }

    if (failures) {
        printf("%d width test failure(s)\n", failures);
        return 1;
    }
    printf("width tests passed\n");
    return 0;
}
