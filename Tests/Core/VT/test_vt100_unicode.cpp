// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Text, once the bytes have been decoded and before the cell is written.
//
// Grapheme clustering, combining marks and the pool that holds the ones a cell
// cannot, East Asian width, emoji presentation, default-ignorable code points,
// and the bidirectional row projection.
//
// The rule that decides how many cells a code point occupies is a join of
// several tables, and their precedence is the whole subject: ignorability beats
// width, presentation selectors beat the default, and a cluster occupies the
// width of its base rather than the sum of its parts. Each case names which
// table wins.

#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "VT100Fixture.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <string_view>

TEST(testArabicDisplayRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // "بسم" (BEH SEEN MEEM) U+0628 U+0633 U+0645 in columns 0..2.
    feed(t, "\xD8\xA8\xD8\xB3\xD9\x85");

    // Logical store is untouched: original codePoints in logical order.
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0)->codePoint == 0x0628);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 1)->codePoint == 0x0633);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 2)->codePoint == 0x0645);

    // CellGrid row: shaped (initial/medial/final) then reversed RTL, so
    // visual reads final MEEM, medial SEEN, initial BEH.
    Ubiquitty::Cell row[Ubiquitty::maxColumns];
    Ubiquitty::Screen::getDisplayRow(t, 0, 0, row, t.columns);
    CHECK(row[0].codePoint == 0xFEE2); // MEEM final
    CHECK(row[1].codePoint == 0xFEB4); // SEEN medial
    CHECK(row[2].codePoint == 0xFE91); // BEH initial
    // Trailing blanks stay blank.
    CHECK(row[3].codePoint == 0);

    // "لا" lam-alef on the next row: the mandatory ligature (ISOLATED form FEFB —
    // no preceding letter) drawn as one combined glyph spanning two cells. The
    // display row carries the ligature as a wide-lead (column 0) + a
    // wide-continuation half (column 1); the logical store keeps LAM, ALEF so
    // copy/paste/search are unaffected.
    feed(t, "\r\n\xD9\x84\xD8\xA7");
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 1, 0)->codePoint == 0x0644); // logical LAM
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 1, 1)->codePoint == 0x0627); // logical ALEF
    Ubiquitty::Screen::getDisplayRow(t, 0, 1, row, t.columns);
    CHECK(row[0].codePoint == 0xFEFB); // lam-alef ligature (isolated), wide-lead
    CHECK(row[0].pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(row[1].codePoint == 0); // continuation half (blank glyph)
    CHECK(row[1].pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));

    // A pure-ASCII row is returned identically (LTR, no shaping).
    feed(t, "\r\nhi");
    Ubiquitty::Screen::getDisplayRow(t, 0, 2, row, t.columns);
    CHECK(row[0].codePoint == 'h');
    CHECK(row[1].codePoint == 'i');
    CHECK(row[0].codePoint == Ubiquitty::Screen::visibleCell(&t, 0, 2, 0)->codePoint);
    CHECK(row[1].codePoint == Ubiquitty::Screen::visibleCell(&t, 0, 2, 1)->codePoint);
}

TEST(testZeroWidthCodepointsClusterIntoBase)
{
    Ubiquitty::Terminal t;
    unsigned char combining_acute[] = { 'A', 0xCC, 0x81, 'B' };
    unsigned char variation_16[] = { 'A', 0xEF, 0xB8, 0x8F, 'B' };

    /// A combining mark joins the base cell's cluster instead of taking
    /// its own cell: the base + the next letter keep their columns, the
    /// cursor does not advance for the mark, and the mark is recorded in
    /// the base cell's combining buffer.
    Ubiquitty::hardReset(t);
    feed(t, combining_acute);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 0)->combiningLength() == 1);
    CHECK(cell(t, 0, 0)->combining[0] == 0x0301); // combining acute
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 1)->combiningLength() == 0);
    CHECK(t.screens.active->cursorX == 2);

    /// VS-16 (variation selector 16) clusters the same way.
    Ubiquitty::hardReset(t);
    feed(t, variation_16);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 0)->combiningLength() == 1);
    CHECK(cell(t, 0, 0)->combining[0] == 0xFE0F); // VS-16
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(t.screens.active->cursorX == 2);
}

TEST(testDefaultIgnorableCodepointsAreDropped)
{
    // ZWSP (U+200B) between two letters: "a" + ZWSP + "b" → two adjacent
    // cells "ab"; the ZWSP occupies no cell and the cursor advances by 2,
    // not 3.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        unsigned char in[] = { 'a', 0xE2, 0x80, 0x8B, 'b' }; // a U+200B b
        feed(t, in);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 0)->combiningLength() == 0); // ZWSP not clustered onto 'a'
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(t.screens.active->cursorX == 2);
    }

    // ZWNBSP / BOM (U+FEFF) drops identically.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        unsigned char in[] = { 'a', 0xEF, 0xBB, 0xBF, 'b' }; // a U+FEFF b
        feed(t, in);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 0)->combiningLength() == 0);
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(t.screens.active->cursorX == 2);
    }

    // SOFT HYPHEN (U+00AD) — a Cf format the general category lists but
    // which many fonts would otherwise paint as '-'. Dropped.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        unsigned char in[] = { 'a', 0xC2, 0xAD, 'b' }; // a U+00AD b
        feed(t, in);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 0)->combiningLength() == 0);
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(t.screens.active->cursorX == 2);
    }

    // HANGUL CHOSEONG FILLER (U+115F) — Default_Ignorable, not Cf, and
    // East-Asian-Wide in the width tables. Ignorability wins: it collapses to no
    // cell. A width table consulted ahead of ignorability claims a two-cell
    // ghost that nothing can overwrite.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        unsigned char in[] = { 'a', 0xE1, 0x85, 0x9F, 'b' }; // a U+115F b
        feed(t, in);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(t.screens.active->cursorX == 2);
    }

    // HANGUL FILLER (U+3164) — another non-Cf Default_Ignorable.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        unsigned char in[] = { 'a', 0xE3, 0x85, 0xA4, 'b' }; // a U+3164 b
        feed(t, in);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(t.screens.active->cursorX == 2);
    }

    // REGRESSION: ZWJ (U+200D) is a Default_Ignorable, but inside an emoji
    // family it must still be consumed by clustering, not dropped. The
    // family collapses to one wide cell with all codePoints recoverable.
    {
        Ubiquitty::Terminal t;
        unsigned char family[] = { 0xF0, 0x9F, 0x91, 0xA8, // U+1F468 man
            0xE2, 0x80, 0x8D, // ZWJ U+200D
            0xF0, 0x9F, 0x91, 0xA9, // U+1F469 woman
            0xE2, 0x80, 0x8D, // ZWJ U+200D
            0xF0, 0x9F, 0x91, 0xA7, // U+1F467 girl
            'X' };
        Ubiquitty::hardReset(t);
        feed(t, family);
        CHECK(cell(t, 0, 0)->codePoint == 0x1F468);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 2)->codePoint == 'X'); // one wide cell only
        CHECK(t.screens.active->cursorX == 3);
        uint32_t codePoints[16];
        int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
        CHECK(n == 5);
        CHECK(codePoints[0] == 0x1F468);
        CHECK(codePoints[1] == 0x200D); // ZWJ survived inside the cluster
        CHECK(codePoints[2] == 0x1F469);
        CHECK(codePoints[3] == 0x200D);
        CHECK(codePoints[4] == 0x1F467);
    }

    // REGRESSION: a combining-mark stack still composes onto its base. The
    // COMBINING GRAPHEME JOINER (U+034F) is itself Default_Ignorable and
    // grapheme-relevant (Extend) — between two combining marks it must
    // extend the cluster, not be dropped.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        unsigned char stack[] = { 'e', 0xCC, 0x81, // U+0301 combining acute
            0xCD, 0x8F, // U+034F COMBINING GRAPHEME JOINER (DI + Extend)
            0xCC, 0x88, // U+0308 combining diaeresis
            'z' };
        feed(t, stack);
        CHECK(cell(t, 0, 0)->codePoint == 'e');
        CHECK(cell(t, 0, 1)->codePoint == 'z'); // no ghost cell from the CGJ
        CHECK(t.screens.active->cursorX == 2);
        uint32_t codePoints[16];
        int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
        CHECK(n == 4); // e + acute + CGJ + diaeresis, all in one cluster
        CHECK(codePoints[0] == 'e');
        CHECK(codePoints[1] == 0x0301);
        CHECK(codePoints[2] == 0x034F); // CGJ consumed by clustering, not dropped
        CHECK(codePoints[3] == 0x0308);
    }
}

TEST(testDroppedZeroWidthCodePointEndsTheOpenCluster)
{
    // A zero-width code point the rules put a boundary before takes no cell and
    // ends the open cluster, so what follows it never joins an earlier cell. A
    // Prepend is the case that tests it: GB9b joins the code point after it, and
    // that code point must start a cell of its own rather than land in the
    // cluster before the Prepend. A zero-width code point the rules never weigh
    // leaves the cluster open. Each case holds at every chunking.
    struct Case {
        const char* name;
        std::string_view bytes;
        int clusterLength; // code points the first cell keeps
        int clusterColumns; // columns the first cell spans
        uint32_t next; // the code point that must start the next cell, or 0
        int cursor;
    };
    const Case cases[] = {
        { "flag, Prepend U+0605, ideograph", "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xD8\x85\xE8\xBF\xB2", 2, 2, 0x8FF2, 4 },
        { "letter, Prepend U+0605, Greek letter", "a\xD8\x85\xCE\xB1", 1, 1, 0x03B1, 2 },
        { "ideograph, Prepend U+070F, Greek letter", "\xE4\xBD\xA0\xDC\x8F\xCE\xB1", 1, 2, 0x03B1, 3 },
        // Both the letter and SOFT HYPHEN sit below every rule's range, so the
        // soft hyphen is dropped unweighed and the acute still joins the letter.
        { "letter, SOFT HYPHEN, combining acute", "a\xC2\xAD\xCC\x81", 2, 1, 0, 1 },
    };
    for (const Case& test : cases) {
        for (const std::size_t chunk : { test.bytes.size(), std::size_t { 1 } }) {
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            for (std::size_t offset = 0; offset < test.bytes.size(); offset += chunk)
                Ubiquitty::processBytes(&t, test.bytes.substr(offset, chunk));
            uint32_t codePoints[16];
            CHECK_MSG(Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16) == test.clusterLength, test.name);
            if (test.next != 0)
                CHECK_MSG(cell(t, 0, test.clusterColumns)->codePoint == test.next, test.name);
            CHECK_MSG(t.screens.active->cursorX == test.cursor, test.name);
        }
    }
}

TEST(testASCIICombiningRunPreservesClusters)
{
    {
        Ubiquitty::Terminal t;
        unsigned char row[] = {
            'a',
            0xCC,
            0x81, // U+0301 combining acute
            'b',
            0xCC,
            0x82, // U+0302 combining circumflex
            'c',
            0xCC,
            0x83, // U+0303 combining tilde
            'd',
            0xCC,
            0x88, // U+0308 combining diaeresis
            'e',
            0xCC,
            0xA7, // U+0327 combining cedilla
        };
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 2, 5);
        feed(t, row);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0x0301);
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(cell(t, 0, 1)->combiningLength() == 1);
        CHECK(cell(t, 0, 1)->combining[0] == 0x0302);
        CHECK(cell(t, 0, 2)->codePoint == 'c');
        CHECK(cell(t, 0, 2)->combiningLength() == 1);
        CHECK(cell(t, 0, 2)->combining[0] == 0x0303);
        CHECK(cell(t, 0, 3)->codePoint == 'd');
        CHECK(cell(t, 0, 3)->combiningLength() == 1);
        CHECK(cell(t, 0, 3)->combining[0] == 0x0308);
        CHECK(cell(t, 0, 4)->codePoint == 'e');
        CHECK(cell(t, 0, 4)->combiningLength() == 1);
        CHECK(cell(t, 0, 4)->combining[0] == 0x0327);
        CHECK(t.screens.active->cursorX == 4);
        CHECK(t.screens.active->wrapPending == true);
    }

    {
        Ubiquitty::Terminal t;
        unsigned char extra_mark[] = { 'a', 0xCC, 0x81, // U+0301 combining acute
            'b', 0xCC, 0x82, // U+0302 combining circumflex
            0xCC, 0x80, // U+0300 combining grave
            'C' };
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 2, 10);
        feed(t, extra_mark);
        CHECK(cell(t, 0, 0)->codePoint == 'a');
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0x0301);
        CHECK(cell(t, 0, 1)->codePoint == 'b');
        CHECK(cell(t, 0, 1)->combiningLength() == 1);
        CHECK(cell(t, 0, 1)->combining[0] == 0x0302);
        CHECK(cell(t, 0, 1)->clusterID() != 0);
        {
            uint32_t codePoints[16];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 1), codePoints, 16);
            CHECK(n == 3);
            CHECK(codePoints[0] == 'b');
            CHECK(codePoints[1] == 0x0302);
            CHECK(codePoints[2] == 0x0300);
        }
        CHECK(cell(t, 0, 2)->codePoint == 'C');
        CHECK(t.screens.active->cursorX == 3);
    }
}

TEST(testBasePlusTwoClusterSpillsToPool)
{
    /// A base + two continuations exceeds the inline buffer
    /// (cellMaxCombining == 1, one slot), so the second mark spills the
    /// WHOLE cluster into the overflow pool: combining[0] keeps the first
    /// mark as the eviction fallback, clusterID is set, and the full
    /// three-codePoint sequence reads back via cellClusterCodePoints. This
    /// is the case that overflows the inline buffer. 'e' U+0301 U+0300 'B'.
    Ubiquitty::Terminal t;
    unsigned char two_marks[] = { 'e', 0xCC, 0x81, 0xCC, 0x80, 'B' };
    Ubiquitty::hardReset(t);
    feed(t, two_marks);
    CHECK(cell(t, 0, 0)->codePoint == 'e');
    CHECK(cell(t, 0, 0)->combiningLength() == 1);
    CHECK(cell(t, 0, 0)->combining[0] == 0x0301); // first mark stays inline
    CHECK(cell(t, 0, 0)->clusterID() != 0); // second spilled to pool
    {
        uint32_t codePoints[16];
        int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
        CHECK(n == 3);
        CHECK(codePoints[0] == 'e');
        CHECK(codePoints[1] == 0x0301); // combining acute
        CHECK(codePoints[2] == 0x0300); // combining grave
    }
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(t.screens.active->cursorX == 2);
}

TEST(testGraphemeClustersOccupyOneCell)
{
    // ZWJ family emoji 👨‍👩‍👧 (U+1F468 ZWJ U+1F469 ZWJ U+1F467) lands in a
    // single wide cell: the base in the lead cell, the ZWJ + joined
    // codePoints in its combining buffer, and the next plain char one
    // column past the wide pair.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char family[] = { 0xF0, 0x9F, 0x91, 0xA8, // U+1F468
            0xE2, 0x80, 0x8D, // ZWJ U+200D
            0xF0, 0x9F, 0x91, 0xA9, // U+1F469
            0xE2, 0x80, 0x8D, // ZWJ U+200D
            0xF0, 0x9F, 0x91, 0xA7, // U+1F467
            'X' };
        Ubiquitty::hardReset(t);
        feed(t, family);
        CHECK(cell(t, 0, 0)->codePoint == 0x1F468);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 2)->codePoint == 'X');
        CHECK(t.screens.active->cursorX == 3);
        // Five codePoints overflow the inline buffer (cellMaxCombining
        // continuations): the lead cell carries a pool id and the full
        // cluster reads back complete via cellClusterCodePoints.
        CHECK(cell(t, 0, 0)->clusterID() != 0);
        {
            uint32_t codePoints[16];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
            CHECK(n == 5);
            CHECK(codePoints[0] == 0x1F468);
            CHECK(codePoints[1] == 0x200D);
            CHECK(codePoints[2] == 0x1F469);
            CHECK(codePoints[3] == 0x200D);
            CHECK(codePoints[4] == 0x1F467);
        }
    }

    // Four-person ZWJ family 👨‍👩‍👧‍👦 (7 codePoints) → one wide emoji cell
    // whose cluster reads back all 7 codePoints from the overflow pool.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char family4[] = { 0xF0, 0x9F, 0x91, 0xA8, // U+1F468 man
            0xE2, 0x80, 0x8D, // ZWJ
            0xF0, 0x9F, 0x91, 0xA9, // U+1F469 woman
            0xE2, 0x80, 0x8D, // ZWJ
            0xF0, 0x9F, 0x91, 0xA7, // U+1F467 girl
            0xE2, 0x80, 0x8D, // ZWJ
            0xF0, 0x9F, 0x91, 0xA6, // U+1F466 boy
            'Y' };
        Ubiquitty::hardReset(t);
        feed(t, family4);
        CHECK(cell(t, 0, 0)->codePoint == 0x1F468);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 2)->codePoint == 'Y'); // one wide cell only
        CHECK(t.screens.active->cursorX == 3);
        CHECK(cell(t, 0, 0)->clusterID() != 0);
        {
            uint32_t codePoints[16];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
            CHECK(n == 7);
            CHECK(codePoints[0] == 0x1F468);
            CHECK(codePoints[1] == 0x200D);
            CHECK(codePoints[2] == 0x1F469);
            CHECK(codePoints[3] == 0x200D);
            CHECK(codePoints[4] == 0x1F467);
            CHECK(codePoints[5] == 0x200D);
            CHECK(codePoints[6] == 0x1F466);
        }
    }

    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        const char* sequences[] = {
            "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6",
            "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA6\xE2\x80\x8D\xF0\x9F\x91\xA6",
            "\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBB\xE2\x80\x8D\xF0\x9F\xA4\x9D\xE2\x80\x8D\xF0\x9F\xA7\x91\xF0\x9F\x8F\xBD",
            "\xF0\x9F\x91\xA8\xF0\x9F\x8F\xBF\xE2\x80\x8D\xF0\x9F\x92\xBB",
            "\xF0\x9F\x91\xA9\xF0\x9F\x8F\xBC\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x91\xA8\xF0\x9F\x8F\xBE",
            "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88",
            "\xF0\x9F\x8F\xB4\xE2\x80\x8D\xE2\x98\xA0\xEF\xB8\x8F",
            "\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\xA6\xB2",
        };
        std::string stream;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 24, 80);
        for (int frame = 0; frame < 64; ++frame) {
            stream += "\033[H";
            for (int row = 0; row < 24; ++row) {
                char cursor[24];
                std::snprintf(cursor, sizeof(cursor), "\033[%d;1H", row + 1);
                stream += cursor;
                for (int column = 0; column < 40; ++column)
                    stream += sequences[(column + row + frame) & 7];
            }
        }
        feed(t, stream);
        const uint32_t expected[8][8] = {
            { 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466, 0 },
            { 0x1F469, 0x200D, 0x1F469, 0x200D, 0x1F466, 0x200D, 0x1F466, 0 },
            { 0x1F9D1, 0x1F3FB, 0x200D, 0x1F91D, 0x200D, 0x1F9D1, 0x1F3FD, 0 },
            { 0x1F468, 0x1F3FF, 0x200D, 0x1F4BB, 0, 0, 0, 0 },
            { 0x1F469, 0x1F3FC, 0x200D, 0x2764, 0xFE0F, 0x200D, 0x1F468, 0x1F3FE },
            { 0x1F3F3, 0xFE0F, 0x200D, 0x1F308, 0, 0, 0, 0 },
            { 0x1F3F4, 0x200D, 0x2620, 0xFE0F, 0, 0, 0, 0 },
            { 0x1F9D1, 0x200D, 0x1F9B2, 0, 0, 0, 0, 0 },
        };
        const int expectedLengths[8] = { 7, 7, 7, 4, 8, 4, 4, 3 };
        auto matchesExpectedSequence = [&](const uint32_t* codePoints, int length) {
            for (int sequence = 0; sequence < 8; ++sequence) {
                if (length != expectedLengths[sequence])
                    continue;
                bool same = true;
                for (int i = 0; i < length; ++i) {
                    if (codePoints[i] != expected[sequence][i]) {
                        same = false;
                        break;
                    }
                }
                if (same)
                    return true;
            }
            return false;
        };
        uint32_t codePoints[16];
        for (int row = 0; row < 24; ++row) {
            for (int column = 0; column < 80; column += 2) {
                int sequence = ((column / 2) + row + 63) & 7;
                const Ubiquitty::Cell* lead = cell(t, row, column);
                CHECK(lead->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
                CHECK(lead->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
                CHECK(cell(t, row, column + 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
                int n = Ubiquitty::cellClusterCodePoints(&t, lead, codePoints, 16);
                CHECK(n == expectedLengths[sequence]);
                for (int i = 0; i < n; ++i)
                    CHECK(codePoints[i] == expected[sequence][i]);
            }
        }
        Ubiquitty::resize(t, 54, 205);
        int emojiLeads = 0;
        for (int row = 0; row < t.rows; ++row) {
            for (int column = 0; column < t.columns; ++column) {
                const Ubiquitty::Cell* lead = cell(t, row, column);
                if (!lead || !lead->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji) || lead->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                    continue;
                ++emojiLeads;
                int n = Ubiquitty::cellClusterCodePoints(&t, lead, codePoints, 16);
                CHECK(matchesExpectedSequence(codePoints, n));
            }
        }
        CHECK(emojiLeads > 0);
        Ubiquitty::Terminal scrollingTerm;
        std::string scrollingStream;
        Ubiquitty::hardReset(scrollingTerm);
        Ubiquitty::resize(scrollingTerm, 24, 80);
        for (int frame = 0; frame < 4; ++frame) {
            for (int row = 0; row < 24; ++row) {
                for (int column = 0; column < 39; ++column)
                    scrollingStream += sequences[(column + row + frame) & 7];
                scrollingStream += "\r\n";
            }
        }
        feed(scrollingTerm, scrollingStream);
        int scrolledEmojiLeads = 0;
        const int totalRows = Ubiquitty::Screen::scrollbackCount(&scrollingTerm) + scrollingTerm.rows;
        for (int row = 0; row < totalRows; ++row) {
            for (int column = 0; column < scrollingTerm.columns; ++column) {
                const Ubiquitty::Cell* lead = Ubiquitty::Screen::absoluteCell(&scrollingTerm, row, column);
                if (!lead || !lead->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji) || lead->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation))
                    continue;
                ++scrolledEmojiLeads;
                int n = Ubiquitty::cellClusterCodePoints(&scrollingTerm, lead, codePoints, 16);
                CHECK(matchesExpectedSequence(codePoints, n));
            }
        }
        CHECK(scrolledEmojiLeads > 0);
    }

    // Regional-indicator flag pair 🇺🇸 (U+1F1FA U+1F1F8) → one wide cell.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char flag[] = { 0xF0, 0x9F, 0x87, 0xBA, // U+1F1FA REGIONAL INDICATOR U
            0xF0, 0x9F, 0x87, 0xB8, // U+1F1F8 REGIONAL INDICATOR S
            'Z' };
        Ubiquitty::hardReset(t);
        feed(t, flag);
        CHECK(cell(t, 0, 0)->codePoint == 0x1F1FA);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0x1F1F8);
        CHECK(cell(t, 0, 2)->codePoint == 'Z');
        CHECK(t.screens.active->cursorX == 3);
        // The flag pair fits inline (clusterID 0); the accessor still
        // returns the two regional-indicator codePoints.
        CHECK(cell(t, 0, 0)->clusterID() == 0);
        {
            uint32_t codePoints[8];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 8);
            CHECK(n == 2);
            CHECK(codePoints[0] == 0x1F1FA);
            CHECK(codePoints[1] == 0x1F1F8);
        }
    }

    // Two flags 🇺🇸🇬🇧 stay separate: GB12/GB13 only pairs RI in twos, so
    // the third RI opens a new cluster (a new wide cell).
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char flags[] = {
            0xF0, 0x9F, 0x87, 0xBA, // U+1F1FA U
            0xF0, 0x9F, 0x87, 0xB8, // U+1F1F8 S
            0xF0, 0x9F, 0x87, 0xAC, // U+1F1EC G
            0xF0, 0x9F, 0x87, 0xA7 // U+1F1E7 B
        };
        Ubiquitty::hardReset(t);
        feed(t, flags);
        CHECK(cell(t, 0, 0)->codePoint == 0x1F1FA);
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0x1F1F8);
        CHECK(cell(t, 0, 2)->codePoint == 0x1F1EC); // second flag's lead cell
        CHECK(cell(t, 0, 2)->combiningLength() == 1);
        CHECK(cell(t, 0, 2)->combining[0] == 0x1F1E7);
        CHECK(t.screens.active->cursorX == 4);
    }

    // Skin-tone modifier 👍🏽 (U+1F44D U+1F3FD) → one wide cell.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char thumbs[] = {
            0xF0, 0x9F, 0x91, 0x8D, // U+1F44D thumbs up
            0xF0, 0x9F, 0x8F, 0xBD // U+1F3FD MEDIUM SKIN TONE
        };
        Ubiquitty::hardReset(t);
        feed(t, thumbs);
        CHECK(cell(t, 0, 0)->codePoint == 0x1F44D);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0x1F3FD);
        CHECK(t.screens.active->cursorX == 2);
    }

    // VS-16 on a text-default emoji ❤️ (U+2764 U+FE0F) → emoji
    // presentation: the cell gains Attribute::Emoji, stays a wide pair, and the
    // VS-16 lands in the lead cell's combining buffer. 'Q' follows at
    // column 2.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char heart[] = { 0xE2, 0x9D, 0xA4, // U+2764 HEAVY BLACK HEART
            0xEF, 0xB8, 0x8F, // U+FE0F VS-16
            'Q' };
        Ubiquitty::hardReset(t);
        feed(t, heart);
        CHECK(cell(t, 0, 0)->codePoint == 0x2764);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)); // VS-16 → emoji
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0xFE0F);
        // The continuation half survives the following 'Q': the pair keeps
        // its two-cell span and 'Q' lands past it, not on top of it.
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
        CHECK(cell(t, 0, 2)->codePoint == 'Q');
        CHECK(t.screens.active->cursorX == 3);
    }

    // Emoji-presentation sequences span two cells, and the continuation
    // half must SURVIVE the next emitted glyph. A default-text-presentation
    // pictograph upgraded by VS-16 — whether its curated base width is 2
    // (⚠ U+26A0, ❤ U+2764) or 1 (✉ U+2709) — leaves the lead as Attribute::Emoji|Attribute::Wide, column 1 as
    // Attribute::WideContinuation, and a following 'X' at column 2 with the cursor at column 3:
    // the same column an 'X' after the width-2 reference "MM" reaches. A
    // collapse to width 1 would put the WIDE_CONT under 'X' (overwritten)
    // and the cursor at column 2.
    {
        struct {
            unsigned char bytes[8];
            int length;
            uint32_t base;
        } cases[] = {
            { { 0xE2, 0x9A, 0xA0, 0xEF, 0xB8, 0x8F, 'X' }, 7, 0x26A0 }, // ⚠
            { { 0xE2, 0x9D, 0xA4, 0xEF, 0xB8, 0x8F, 'X' }, 7, 0x2764 }, // ❤
            { { 0xE2, 0x9C, 0x89, 0xEF, 0xB8, 0x8F, 'X' }, 7, 0x2709 }, // ✉
        };
        for (int i = 0; i < 3; i++) {
            // Heap, not frame: under -O0 with coverage every block's Terminal
            // holds its own frame slot at once, and fifteen of them overflow
            // the 8 MiB main-thread stack before the first case prints.
            auto tHold = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::Terminal& t = *tHold;
            Ubiquitty::hardReset(t);
            feed(t, std::span(cases[i].bytes, cases[i].length));
            CHECK(cell(t, 0, 0)->codePoint == cases[i].base);
            CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
            CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
            CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
            CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
            CHECK(cell(t, 0, 2)->codePoint == 'X'); // next glyph past the pair
            CHECK(t.screens.active->cursorX == 3);
        }
        /// The width-2 reference: an 'X' after "MM" also lands at column 2.
        Ubiquitty::Terminal ref;
        unsigned char mmx[] = { 'M', 'M', 'X' };
        Ubiquitty::hardReset(ref);
        feed(ref, mmx);
        CHECK(cell(ref, 0, 2)->codePoint == 'X');
        CHECK(ref.screens.active->cursorX == 3);
    }

    // Keycap 1️⃣ ('1' U+0031 + VS16 + U+20E3): the same widening path as a
    // VS-16 pictograph, so its continuation half must also survive the next
    // glyph. After 'X' the lead stays a wide emoji pair, column 1 is still
    // Attribute::WideContinuation, and 'X' is at column 2 (cursor at column 3).
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char keycap[] = { '1', // U+0031
            0xEF, 0xB8, 0x8F, // U+FE0F VS-16
            0xE2, 0x83, 0xA3, // U+20E3 COMBINING ENCLOSING KEYCAP
            'X' };
        Ubiquitty::hardReset(t);
        feed(t, keycap);
        CHECK(cell(t, 0, 0)->codePoint == 0x31);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
        CHECK(cell(t, 0, 2)->codePoint == 'X');
        CHECK(t.screens.active->cursorX == 3);
    }

    // Bare ⚠ / ❤ (no VS-16) occupy one cell and are not tagged emoji
    // presentation; the same base with VS-16 widens to two. Both codepoints are
    // East_Asian_Width=Neutral with Emoji_Presentation=No, so the generated
    // width tables (UCD 17.0.0) call them narrow — the hand-curated table they
    // replaced said 2, which is what this once asserted. The pair is
    // the point: the upgrade path fires on the variation selector and only on
    // it.
    {
        struct {
            unsigned char bytes[4];
            int length;
            uint32_t base;
        } bare[] = {
            { { 0xE2, 0x9A, 0xA0 }, 3, 0x26A0 },
            { { 0xE2, 0x9D, 0xA4 }, 3, 0x2764 },
        };
        for (int i = 0; i < 2; i++) {
            // Heap, not frame: under -O0 with coverage every block's Terminal
            // holds its own frame slot at once, and fifteen of them overflow
            // the 8 MiB main-thread stack before the first case prints.
            auto tHold = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::Terminal& t = *tHold;
            Ubiquitty::hardReset(t);
            feed(t, std::span(bare[i].bytes, bare[i].length));
            CHECK(cell(t, 0, 0)->codePoint == bare[i].base);
            CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
            CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
            CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
            CHECK(cell(t, 0, 0)->combiningLength() == 0);
            CHECK(t.screens.active->cursorX == 1);
        }
        // Same bases, now with VS-16: the upgrade fires and claims two cells.
        for (int i = 0; i < 2; i++) {
            // Heap, not frame: under -O0 with coverage every block's Terminal
            // holds its own frame slot at once, and fifteen of them overflow
            // the 8 MiB main-thread stack before the first case prints.
            auto tHold = std::make_unique<Ubiquitty::Terminal>();
            Ubiquitty::Terminal& t = *tHold;
            unsigned char upgraded[6];
            int n = 0;
            for (int b = 0; b < bare[i].length; b++)
                upgraded[n++] = bare[i].bytes[b];
            upgraded[n++] = 0xEF; // U+FE0F VS-16
            upgraded[n++] = 0xB8;
            upgraded[n++] = 0x8F;
            Ubiquitty::hardReset(t);
            feed(t, std::span(upgraded, n));
            CHECK(cell(t, 0, 0)->codePoint == bare[i].base);
            CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
            CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
            CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
            CHECK(t.screens.active->cursorX == 2);
        }
    }

    // Right-margin handling. A narrow pictograph (✉ U+2709, curated width 1)
    // upgraded by VS-16 needs two cells; place it where it doesn't fit.
    {
        /// Last column: there is no room for the continuation half, so the
        /// widen falls back gracefully — the cell stays a single emoji cell
        /// (Attribute::Emoji, no Attribute::Wide), no off-screen continuation is stamped,
        /// and the following 'X' wraps to column 0 of the next row.
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        Ubiquitty::hardReset(t);
        char cup[32];
        int lastColumn = t.columns; // 1-based CUP column = columns → column index columns-1
        int n = 0;
        cup[n++] = 0x1B;
        cup[n++] = '[';
        cup[n++] = '1';
        cup[n++] = ';';
        if (lastColumn >= 10)
            cup[n++] = (char)('0' + lastColumn / 10);
        cup[n++] = (char)('0' + lastColumn % 10);
        cup[n++] = 'H';
        feed(t, std::string_view(cup, n));
        unsigned char env[] = { 0xE2, 0x9C, 0x89, 0xEF, 0xB8, 0x8F, 'X' };
        feed(t, env);
        CHECK(cell(t, 0, t.columns - 1)->codePoint == 0x2709);
        CHECK(cell(t, 0, t.columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(!cell(t, 0, t.columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 1, 0)->codePoint == 'X'); // X wrapped to the next row
    }
    {
        /// Second-to-last column: exactly two cells fit, so the pair occupies
        /// the last two columns (lead Attribute::Emoji|Attribute::Wide at columns-2, the
        /// continuation at columns-1) and 'X' wraps to the next row.
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        Ubiquitty::hardReset(t);
        char cup[32];
        int column = t.columns - 1; // 1-based CUP column = columns-1
        int n = 0;
        cup[n++] = 0x1B;
        cup[n++] = '[';
        cup[n++] = '1';
        cup[n++] = ';';
        if (column >= 10)
            cup[n++] = (char)('0' + column / 10);
        cup[n++] = (char)('0' + column % 10);
        cup[n++] = 'H';
        feed(t, std::string_view(cup, n));
        unsigned char env[] = { 0xE2, 0x9C, 0x89, 0xEF, 0xB8, 0x8F, 'X' };
        feed(t, env);
        CHECK(cell(t, 0, t.columns - 2)->codePoint == 0x2709);
        CHECK(cell(t, 0, t.columns - 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, t.columns - 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, t.columns - 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, t.columns - 1)->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
        CHECK(cell(t, 1, 0)->codePoint == 'X'); // X wrapped to the next row
    }

    // VS-15 on the same base ❤︎ (U+2764 U+FE0E) → text presentation: no
    // Attribute::Emoji. The base's curated width is unchanged (the width table
    // treats U+2764 as a wide glyph), but the emoji flag stays off.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char heartText[] = { 0xE2, 0x9D, 0xA4, // U+2764
            0xEF, 0xB8, 0x8E, // U+FE0E VS-15
            'Q' };
        Ubiquitty::hardReset(t);
        feed(t, heartText);
        CHECK(cell(t, 0, 0)->codePoint == 0x2764);
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji)); // VS-15 → text
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0xFE0E);
    }

    // Bare ❤ (U+2764) with no variation selector → text presentation
    // (no Attribute::Emoji), lone codePoint with an empty combining buffer.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char bareHeart[] = { 0xE2, 0x9D, 0xA4, 'Q' };
        Ubiquitty::hardReset(t);
        feed(t, bareHeart);
        CHECK(cell(t, 0, 0)->codePoint == 0x2764);
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 0)->combiningLength() == 0);
    }

    // VS-16 after a non-pictographic base (a letter) must not retag the
    // cell as emoji nor widen it: 'A' U+FE0F leaves 'A' a narrow text
    // cell with the selector clustered in its buffer.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char letterVS[] = { 'A', 0xEF, 0xB8, 0x8F, 'B' };
        Ubiquitty::hardReset(t);
        feed(t, letterVS);
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 1)->codePoint == 'B');
        CHECK(t.screens.active->cursorX == 2);
    }

    // VS-16 after a base the Extended_Pictographic property does not cover --
    // U+2701 UPPER BLADE SCISSORS, one of the assigned symbols a too-wide
    // extPict[] over-covers (chess pieces, dice, music notes) -- must not widen
    // the cell. The generic negative above uses 'A', which no plausible extPict
    // error includes; this pins the symbol-block class a hand-maintained set gets
    // wrong, where isExtendedPictographic gates the width upgrade at VT.cpp. Its
    // GB11 twin is the grapheme-conformance corpus's U+2701 case.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char scissorsVS[] = { 0xE2, 0x9C, 0x81, // U+2701 UPPER BLADE SCISSORS
            0xEF, 0xB8, 0x8F, // U+FE0F VS-16
            'B' };
        Ubiquitty::hardReset(t);
        feed(t, scissorsVS);
        CHECK(cell(t, 0, 0)->codePoint == 0x2701);
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 1)->codePoint == 'B');
        CHECK(t.screens.active->cursorX == 2);
    }

    // VS-16 on a NARROW text-default pictograph ☺ (U+263A, width 1 in the
    // curated table) widens the cell to a two-cell emoji pair: Attribute::Emoji
    // + Attribute::Wide on the lead, Attribute::WideContinuation on its right neighbour, and
    // the following 'R' lands at column 2 (cursor advanced past the pair).
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char smile[] = { 0xE2, 0x98, 0xBA, // U+263A WHITE SMILING FACE
            0xEF, 0xB8, 0x8F, // U+FE0F VS-16
            'R' };
        Ubiquitty::hardReset(t);
        feed(t, smile);
        CHECK(cell(t, 0, 0)->codePoint == 0x263A);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
        CHECK(cell(t, 0, 2)->codePoint == 'R');
        CHECK(t.screens.active->cursorX == 3);
    }

    // Emoji keycap 1️⃣ ('1' U+0031 + VS16 U+FE0F + U+20E3 COMBINING
    // ENCLOSING KEYCAP). The keycap base is not Extended_Pictographic, so
    // VS16 alone does not retag it; the trailing U+20E3 completes the
    // sequence and upgrades the lead cell to a wide emoji pair: Attribute::Emoji
    // + Attribute::Wide on the lead, Attribute::WideContinuation on its neighbour. The full
    // cluster reads back as {0x31, 0xFE0F, 0x20E3} and 'Z' lands at column 2.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char keycap[] = { '1', // U+0031
            0xEF, 0xB8, 0x8F, // U+FE0F VS-16
            0xE2, 0x83, 0xA3, // U+20E3 COMBINING ENCLOSING KEYCAP
            'Z' };
        Ubiquitty::hardReset(t);
        feed(t, keycap);
        CHECK(cell(t, 0, 0)->codePoint == 0x31);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::wideContinuationSentinelCodePoint);
        {
            uint32_t codePoints[16];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
            CHECK(n == 3);
            CHECK(codePoints[0] == 0x31);
            CHECK(codePoints[1] == 0xFE0F);
            CHECK(codePoints[2] == 0x20E3);
        }
        CHECK(cell(t, 0, 2)->codePoint == 'Z');
        CHECK(t.screens.active->cursorX == 3);
    }

    // Hash keycap #️⃣ ('#' U+0023 + VS16 + U+20E3) behaves the same as the
    // digit keycaps: a wide emoji pair with the full sequence readable.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char hashKeycap[] = { '#', // U+0023
            0xEF, 0xB8, 0x8F, // U+FE0F VS-16
            0xE2, 0x83, 0xA3, // U+20E3
            'Z' };
        Ubiquitty::hardReset(t);
        feed(t, hashKeycap);
        CHECK(cell(t, 0, 0)->codePoint == 0x23);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        {
            uint32_t codePoints[16];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
            CHECK(n == 3);
            CHECK(codePoints[0] == 0x23);
            CHECK(codePoints[1] == 0xFE0F);
            CHECK(codePoints[2] == 0x20E3);
        }
        CHECK(cell(t, 0, 2)->codePoint == 'Z');
        CHECK(t.screens.active->cursorX == 3);
    }

    // A bare digit '1' (no keycap mark) stays a plain width-1 text cell:
    // no Attribute::Emoji, no Attribute::Wide, cursor advances by one.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char digit[] = { '1', 'Z' };
        Ubiquitty::hardReset(t);
        feed(t, digit);
        CHECK(cell(t, 0, 0)->codePoint == '1');
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->combiningLength() == 0);
        CHECK(cell(t, 0, 1)->codePoint == 'Z');
        CHECK(t.screens.active->cursorX == 2);
    }

    // A digit followed only by VS16 ('1' U+FE0F, no U+20E3) is not a
    // keycap: the cell stays a narrow text digit (no Attribute::Emoji, no
    // Attribute::Wide) with the selector clustered into its combining buffer.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char digitVS[] = { '1', 0xEF, 0xB8, 0x8F, 'Z' };
        Ubiquitty::hardReset(t);
        feed(t, digitVS);
        CHECK(cell(t, 0, 0)->codePoint == '1');
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0xFE0F);
        CHECK(cell(t, 0, 1)->codePoint == 'Z');
        CHECK(t.screens.active->cursorX == 2);
    }

    // A cursor move between a base and a combining mark severs the
    // cluster: the mark cannot retro-attach across the reposition. After
    // 'A' the cursor sits at column 1; CR homes it to column 0; a combining
    // mark then arrives with no live cluster and is dropped (width 0).
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char seq[] = { 'A', '\r', 0xCC, 0x81 }; // A CR combining-acute
        Ubiquitty::hardReset(t);
        feed(t, seq);
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(cell(t, 0, 0)->combiningLength() == 0);
    }

    // A narrow base with several combining marks stays a single width-1
    // cell. 'e' + four combining accents: the first cellMaxCombining
    // marks fill the inline buffer, the overflow moves the whole sequence
    // into the cluster pool (clusterID != 0), and the readback returns all
    // five codePoints (base + four marks) in order. Cursor advances by one
    // (the base's east-asian width), not by the number of marks.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char eAccents[] = { 'e', 0xCC, 0x81, // U+0301 COMBINING ACUTE ACCENT
            0xCC, 0x80, // U+0300 COMBINING GRAVE ACCENT
            0xCC, 0x82, // U+0302 COMBINING CIRCUMFLEX ACCENT
            0xCC, 0x83, // U+0303 COMBINING TILDE
            'X' };
        Ubiquitty::hardReset(t);
        feed(t, eAccents);
        CHECK(cell(t, 0, 0)->codePoint == 'e');
        CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)); // width 1
        CHECK(cell(t, 0, 0)->clusterID() != 0); // overflowed
        {
            uint32_t codePoints[16];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 16);
            CHECK(n == 5);
            CHECK(codePoints[0] == 'e');
            CHECK(codePoints[1] == 0x0301);
            CHECK(codePoints[2] == 0x0300);
            CHECK(codePoints[3] == 0x0302);
            CHECK(codePoints[4] == 0x0303);
        }
        CHECK(cell(t, 0, 1)->codePoint == 'X'); // base width 1
        CHECK(t.screens.active->cursorX == 2);
    }

    // A wide (east-asian) base with a combining mark stays a single
    // width-2 cell. U+4E00 一 (wide) + U+0301: the lead keeps Attribute::Wide,
    // the right half stays Attribute::WideContinuation, the mark attaches to the lead's
    // cluster, and the following 'Y' lands at column 2 (cursor past the wide
    // pair). Width comes from the base, so the combining sequence does not
    // collapse the cell to width 1.
    {
        // Heap, not frame: under -O0 with coverage every block's Terminal
        // holds its own frame slot at once, and fifteen of them overflow
        // the 8 MiB main-thread stack before the first case prints.
        auto tHold = std::make_unique<Ubiquitty::Terminal>();
        Ubiquitty::Terminal& t = *tHold;
        unsigned char wideMark[] = { 0xE4, 0xB8, 0x80, // U+4E00 — wide CJK base
            0xCC, 0x81, // U+0301 COMBINING ACUTE ACCENT
            'Y' };
        Ubiquitty::hardReset(t);
        feed(t, wideMark);
        CHECK(cell(t, 0, 0)->codePoint == 0x4E00);
        CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide)); // width 2 kept
        CHECK(cell(t, 0, 0)->combiningLength() == 1);
        CHECK(cell(t, 0, 0)->combining[0] == 0x0301);
        CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
        CHECK(cell(t, 0, 2)->codePoint == 'Y');
        CHECK(t.screens.active->cursorX == 3);
        {
            uint32_t codePoints[8];
            int n = Ubiquitty::cellClusterCodePoints(&t, cell(t, 0, 0), codePoints, 8);
            CHECK(n == 2);
            CHECK(codePoints[0] == 0x4E00);
            CHECK(codePoints[1] == 0x0301);
        }
    }
}

TEST(testWideCodepointsOccupyTwoCells)
{
    Ubiquitty::Terminal t;
    unsigned char cjk_then_x[] = { 0xE4, 0xB8, 0x80, 'X' }; // U+4E00

    Ubiquitty::hardReset(t);
    feed(t, cjk_then_x);
    CHECK(cell(t, 0, 0)->codePoint == 0x4E00); // 一 — canonical Unicode
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(cell(t, 0, 2)->codePoint == 'X');
    CHECK(t.screens.active->cursorX == 3);
}

TEST(testWideCodepointWrapsAtMargin)
{
    Ubiquitty::Terminal t;
    unsigned char seq[] = { 'A', 'B', 'C', 0xE4, 0xB8, 0x80 }; // U+4E00

    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 'C');
    CHECK(cell(t, 0, 3)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 0x4E00);
    CHECK(cell(t, 1, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 1, 1)->codePoint == 0);
    CHECK(cell(t, 1, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->cursorX == 2);
    CHECK(t.screens.active->cursorY == 1);
}

TEST(testEmojiWidthRoutedThroughWidthFor)
{
    Ubiquitty::Terminal t;
    // U+2B50 ⭐ encoded as UTF-8: E2 AD 90
    unsigned char star_then_x[] = { 0xE2, 0xAD, 0x90, 'X' };
    Ubiquitty::hardReset(t);
    feed(t, star_then_x);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(cell(t, 0, 2)->codePoint == 'X');
}

TEST(testGraphemeClusteringMode2027)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.graphemeClustering == false);
    /// Clustering is unconditional in Ubiquitty — it is the only path that
    /// attaches combining marks / VS / ZWJ to a cell, so it cannot be gated
    /// off without regressing the default render. DECRQM therefore reports
    /// mode 2027 as PERMANENTLY SET (Pm=3) regardless of the cosmetic
    /// graphemeClustering flag, so a probing app sees that grapheme-cluster
    /// processing is always active rather than the flag's value.
    auto checkReport = [&]() {
        char outputBuffer[64];
        feed(t, "\033[?2027$p");
        int n = drain(t, outputBuffer);
        CHECK(n > 0);
        CHECK(strcmp(outputBuffer, "\033[?2027;3$y") == 0); // permanently set
    };
    checkReport(); // default (flag off) still reports permanently set
    // The flag still round-trips on the field (apps may set/reset it), but
    // the DECRQM report stays permanently-set either way.
    feed(t, "\033[?2027h");
    CHECK(t.modes.graphemeClustering == true);
    checkReport();
    feed(t, "\033[?2027l");
    CHECK(t.modes.graphemeClustering == false);
    checkReport();
}

// An open cluster is bound to the screen it started on: the alternate-screen
// switch carries the cursor to a different buffer, so a combining mark arriving
// after the switch cannot retro-attach to the primary screen's cluster.
TEST(testClusterDoesNotAttachAcrossScreenSwitch)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "a"); // opens a cluster on the primary screen
    feed(t, "\033[?1049h"); // alternate screen: erased
    feed(t, "\xcc\x81"); // U+0301 combining acute — must not attach
    feed(t, "\033[?1049l"); // back to the primary screen
    CHECK(cell(t, 0, 0)->codePoint == 'a');
    CHECK(cell(t, 0, 0)->combiningLength() == 0); // no retro-attach
}

// VS16 after a narrow pictographic base upgrades it to the two-cell emoji span:
// the base gains Emoji+Wide, the cell to the right becomes the continuation
// half, and the cursor steps past the claim.
TEST(testVS16UpgradeClaimsWideCell)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);

    feed(t, "\xE2\x98\xBA\xEF\xB8\x8F"); // U+263A ☺ + VS16
    CHECK(cell(t, 0, 0)->codePoint == 0x263A);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->cursorX == 2);
    CHECK(t.screens.active->wrapPending == false);
}

// The same upgrade with the base on the last column cannot claim a cell to the
// right — the deferred wrap arms instead, exactly like a wide char at the margin.
TEST(testVS16UpgradeAtMarginArmsWrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);

    feed(t, "\033[1;3H\xE2\x98\xBA\xEF\xB8\x8F"); // base at col 2 (last of 4)
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 3)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->wrapPending == true);
    feed(t, "x"); // the armed wrap commits on the next printable
    CHECK(cell(t, 1, 0)->codePoint == 'x');
}

// VS15 after the upgrade downgrades presentation: the Emoji bit clears (text
// presentation) while the claimed wide span stays.
TEST(testVS15DowngradesEmojiPresentation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);

    feed(t, "\xE2\x98\xBA\xEF\xB8\x8F\xEF\xB8\x8E"); // ☺ + VS16 + VS15
    CHECK(cell(t, 0, 0)->codePoint == 0x263A);
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
}

// The same upgrade reached through the batched cluster-extension scan: when a
// VS16 arrives as one of several grapheme-extension code points in a decoded
// run, clusterAppendRun applies the presentation effects in wire order — the
// base widens and the right-hand cell becomes the continuation half.
TEST(testBatchedVS16ClusterUpgradeWidensCell)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);

    feed(t, "\xE2\x98\xBA\xCC\x81\xEF\xB8\x8F"); // ☺ + combining acute + VS16
    CHECK(cell(t, 0, 0)->codePoint == 0x263A);
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->cursorX == 2);
}

// The batched upgrade on the right margin cannot claim a continuation cell
// beyond it — the deferred wrap arms instead.
TEST(testBatchedVS16ClusterUpgradeAtMarginArmsWrap)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[?69h\033[1;2s"); // DECSLRM right margin at col 2 (index 1)
    feed(t, "\033[1;2H"); // cursor at the right margin

    feed(t, "\xE2\x98\xBA\xCC\x81\xEF\xB8\x8F"); // ☺ + combining acute + VS16
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->wrapPending == true);
    feed(t, "x"); // the armed wrap commits on the next printable
    CHECK(cell(t, 1, 0)->codePoint == 'x');
}

// The per-byte path (batch ineligible — cursor outside the margins) arms the
// same deferred wrap from the single clusterAppend upgrade.
TEST(testVS16ClusterUpgradeArmsWrapOutsideMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[?69h\033[1;2s"); // DECSLRM right margin at col 2 (index 1)
    feed(t, "\033[1;3H"); // cursor at index 2 — beyond the right margin

    feed(t, "\xE2\x98\xBA\xCC\x81\xEF\xB8\x8F");
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(t.screens.active->wrapPending == true);
    feed(t, "x");
    CHECK(cell(t, 1, 0)->codePoint == 'x');
}

// With the margin bell armed every batch path bails to the per-byte driver, so
// the single clusterAppend upgrade claims the wide span and arms the deferred
// wrap at the full-width margin.
TEST(testVS16ClusterUpgradePerByteAtFullWidthMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[?44h"); // margin bell — batches ineligible
    feed(t, "\033[1;3H"); // cursor at index 2

    feed(t, "\xE2\x98\xBA\xCC\x81\xEF\xB8\x8F");
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
    CHECK(cell(t, 0, 3)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->wrapPending == true);
}

// The single-append VS15 downgrade clears the Emoji bit on a pictographic
// base that reached the per-byte cluster path.
TEST(testVS15ClusterDowngradePerByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[?44h"); // margin bell — batches ineligible
    feed(t, "\033[1;3H");

    feed(t, "\xE2\x98\xBA\xCC\x81\xEF\xB8\x8E"); // ☺ + acute + VS15
    CHECK(cell(t, 0, 2)->codePoint == 0x263A);
    CHECK(!cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji));
}

// The single-append upgrade clear of the margin steps the cursor past the
// claimed wide span instead of arming the wrap.
TEST(testVS16ClusterUpgradePerByteStepsCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[?44h"); // margin bell — batches ineligible

    feed(t, "\xE2\x98\xBA\xCC\x81\xEF\xB8\x8F"); // cluster at col 0
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(t.screens.active->cursorX == 2);
}

// The ASCII+combining batch covers the U+0340..U+036F mark range too (0xCD
// lead bytes), not just U+0300..U+033F.
TEST(testASCIICCombiningMarkRange)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);

    feed(t, "a\xCD\x95"); // a + U+0355 COMBINING RIGHT ARROWHEAD BELOW
    CHECK(cell(t, 0, 0)->codePoint == 'a');
    CHECK(cell(t, 0, 0)->combiningLength() == 1);
    CHECK(cell(t, 0, 0)->combining[0] == 0x0355);
}

// A 0xCC lead whose trail isn't a mark byte (0x80..0xBF) makes the whole pair
// invalid: the combining batch bails to the per-byte path, which renders the
// lead as U+FFFD.
TEST(testCombiningBatchBailsOnInvalidTrail)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 4);

    feed(t, "a\xCC z"); // 0xCC followed by a space: not a mark pair
    CHECK(cell(t, 0, 0)->codePoint == 'a');
    CHECK(cell(t, 0, 1)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 2)->codePoint == ' ');
    CHECK(cell(t, 0, 3)->codePoint == 'z');
}

// Malformed UTF-8 reaches the cluster-route probe and is rejected there: a
// lead with too few bytes left, a non-trail continuation byte, and a value
// past the code point ceiling each fall back to the per-byte decoder.
TEST(testMalformedUTF8RejectedByClusterRouteProbe)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);

    // A 3-byte lead with nothing left in the buffer: the probe refuses it and
    // the per-byte decoder drops the truncated sequence without a cell.
    feed(t, "a\xEF");
    CHECK(cell(t, 0, 0)->codePoint == 'a');
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(t.screens.active->cursorX == 1);

    Ubiquitty::hardReset(t);
    // A 3-byte lead with a non-trail second byte: one U+FFFD replaces the pair.
    feed(t, "a\xEFx");
    CHECK(cell(t, 0, 1)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 2)->codePoint == 'x');

    Ubiquitty::hardReset(t);
    // A 4-byte lead decoding past U+10FFFF: rejected by the probe, and ill-formed
    // from its second byte, so the decoder replaces each of the four bytes.
    feed(t, "a\xF4\x90\x80\x80");
    CHECK(cell(t, 0, 1)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 4)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 5)->codePoint == 0);
    CHECK(t.screens.active->cursorX == 5);
}

// emitClusterRun is gated off the status line: a grapheme-relevant lead with
// DECSASD 1 active goes straight to the status row via the per-byte path.
TEST(testClusterRunBailsOnStatusLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~"); // configure the host-writable status display
    Ubiquitty::resize(t, 2, 4);
    feed(t, "\033[2$~\033[1$}"); // DECSASD: status line receives writes
    feed(t, "\xE2\x98\xBA"); // ☺
    CHECK(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint == 0x263A);
    CHECK(cell(t, 0, 0)->codePoint == 0); // nothing on the page row
}

// A second emoji in a row leaves the first one still tagged as an emoji.
//
// Every other case in this file writes ONE emoji into a row, so none of them can
// see a write that reaches back: the bit is asserted on a cell nothing later
// touches. That blindness is why a colour glyph going undrawn on the Linux arm
// read as a font problem for four days — the presentation bit is what
// `isColorOnOwnSheet` gates the colour branch on, so a lead that loses it emits
// no colour quad at all and the row renders one glyph short.
//
// The code point survives, the width survives, the continuation survives. Only
// the presentation bit goes, which is why nothing that reads text or geometry
// could notice.
TEST(testASecondEmojiLeavesTheFirstTagged)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // U+1F600 twice, adjacent. Each is a lone code point with default emoji
    // presentation, so each is tagged on its own write.
    feed(t, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80");

    const Ubiquitty::Cell* second = cell(t, 0, 2);
    CHECK_MSG(second && second->codePoint == 0x1F600, "the second emoji is where it belongs");
    CHECK_MSG(second->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji),
        "and carries the presentation bit");

    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK_MSG(first && first->codePoint == 0x1F600, "the first emoji is still there");
    CHECK_MSG(first->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide),
        "still two cells wide");
    CHECK_MSG(first->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji),
        "and writing the second did not take its presentation bit away");
}

// The same, spaced rather than adjacent, so the answer cannot be about the wide
// pair's flank blanking reaching one column too far.
TEST(testASpacedSecondEmojiLeavesTheFirstTagged)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\xF0\x9F\x98\x80 \xF0\x9F\x98\x80");

    const Ubiquitty::Cell* first = cell(t, 0, 0);
    CHECK_MSG(first && first->codePoint == 0x1F600, "the first emoji is still there");
    CHECK_MSG(first->pen.attributes.contains(Ubiquitty::VT::Attribute::Emoji),
        "and keeps its presentation bit across a gap");
}

// The same bytes, arriving whole and one byte at a time, land the same grid.
//
// `processBytes` takes "one read's worth of input" (Tests/TerminalFixture.h), so
// how a stream is cut into calls is the host's business and never the content's.
// A path that carries state across a batch can break that without breaking
// anything a single-construct test asks: a run of glyphs that shares one
// attribute set, where the last glyph of a batch takes a different path from its
// predecessors, is such a path.
//
// Every arm in this file feeds one construct per call, which is the shape that
// cannot see it. This varies the axis instead of the content: each probe is fed
// twice and the two grids are compared cell by cell, code point and attributes
// both, so a difference is a defect by construction rather than by an assertion
// somebody thought to write.
//
// It fires. With the presentation fix reverted it reports two mismatches — "two lone
// emoji" and "emoji then cjk", both `attr=0x0100` whole against `0x0900` split,
// which is the presentation bit — so a green here is a measurement and not a
// harness that cannot fail.
//
// The probes are the constructs that carry state between code points: clusters,
// joiners, regional-indicator pairs, variation selectors, keycaps, the wide/
// narrow boundary, and an SGR run whose pen spans the batch.
TEST(testTheSameBytesLandTheSameGridWhateverTheChunking)
{
    struct Probe {
        const char* name;
        std::vector<std::uint8_t> bytes;
    };
    const std::vector<Probe> probes = {
        { "two lone emoji", { 0xF0, 0x9F, 0x98, 0x80, ' ', 0xF0, 0x9F, 0x98, 0x80 } },
        { "emoji then cjk", { 0xF0, 0x9F, 0x98, 0x80, 0xE6, 0xBC, 0xA2, 0xE5, 0xAD, 0x97 } },
        { "cjk run", { 0xE6, 0xBC, 0xA2, 0xE5, 0xAD, 0x97, 0xE6, 0xBC, 0xA2, 0xE5, 0xAD, 0x97 } },
        { "zwj family", { 0xF0, 0x9F, 0x91, 0xA8, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x91, 0xA9, 0xE2, 0x80, 0x8D, 0xF0, 0x9F, 0x91, 0xA7, 'X' } },
        { "skin tone then emoji", { 0xF0, 0x9F, 0x91, 0x8D, 0xF0, 0x9F, 0x8F, 0xBD, ' ', 0xF0, 0x9F, 0x98, 0x80 } },
        { "flag pair", { 0xF0, 0x9F, 0x87, 0xAF, 0xF0, 0x9F, 0x87, 0xB5, ' ', 0xF0, 0x9F, 0x87, 0xAF, 0xF0, 0x9F, 0x87, 0xB5 } },
        { "vs16 heart twice", { 0xE2, 0x9D, 0xA4, 0xEF, 0xB8, 0x8F, ' ', 0xE2, 0x9D, 0xA4, 0xEF, 0xB8, 0x8F } },
        { "keycap then emoji", { '1', 0xEF, 0xB8, 0x8F, 0xE2, 0x83, 0xA3, ' ', 0xF0, 0x9F, 0x98, 0x80 } },
        { "combining then emoji", { 'A', 0xCC, 0x81, ' ', 0xF0, 0x9F, 0x98, 0x80 } },
        { "sgr bold text", { 0x1B, '[', '1', 'm', 'a', 'b', 'c', 0x1B, '[', '0', 'm', 'd', 'e', 'f' } },
        { "wide narrow mix", { 0xE6, 0xBC, 0xA2, 'a', 0xE6, 0xBC, 0xA2, 'b' } },
        { "emoji at row end", { 'a', 'b', 0xF0, 0x9F, 0x98, 0x80 } },
    };

    int mismatches = 0;
    for (const Probe& probe : probes) {
        Ubiquitty::Terminal whole;
        Ubiquitty::hardReset(whole);
        feed(whole, probe.bytes);

        Ubiquitty::Terminal split;
        Ubiquitty::hardReset(split);
        for (std::uint8_t byte : probe.bytes)
            feed(split, std::vector<std::uint8_t> { byte });

        for (int column = 0; column < 24; ++column) {
            const Ubiquitty::Cell* a = cell(whole, 0, column);
            const Ubiquitty::Cell* b = cell(split, 0, column);
            if (!a || !b)
                continue;
            const bool sameCodePoint = a->codePoint == b->codePoint;
            const bool sameAttributes = a->pen.attributes.toRaw() == b->pen.attributes.toRaw();
            if (!sameCodePoint || !sameAttributes) {
                std::printf("  MISMATCH %-22s col %2d: whole cp=%u attr=0x%04X | split cp=%u attr=0x%04X\n",
                    probe.name, column,
                    static_cast<unsigned>(a->codePoint), static_cast<unsigned>(a->pen.attributes.toRaw()),
                    static_cast<unsigned>(b->codePoint), static_cast<unsigned>(b->pen.attributes.toRaw()));
                ++mismatches;
            }
        }
    }
    CHECK_EQ(mismatches, 0);
}

// The batched text run carries ASCII between and after its glyphs and
// substitutes U+FFFD for malformed sequences itself; bytewise input, which
// never batches, is the reference for what lands on the screen.
TEST(mixedScriptAndMalformedTextMatchesBytewiseInput)
{
    static constexpr std::string_view pieces[] = { "ab", "x", "The", "%T", "   ", "\xC3\xA9", "\xC3\xBF", "\xE4\xB8\xAD", "\xE7\x95\x8C",
        "\x80", "\xBF", "\xC3", "\xE2\x82", "\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFE", "\xFF",
        "\xC2\x94", "\xF0\x9F\x98\x80", "e\xCC\x81", "\r\n", "\n", "\033[31m", "\033[m", "\t", "\xC2\xA0",
        "\xCC\x81", "\xE2\x80\x8D", "\xF0\x9F\x87\xA6", "\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA4", "\xE1\x84\x80\xE1\x85\xA1", "\xD0\xB0\xCC\x88\xCC\x81" };
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    const auto next = [&state] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (int round = 0; round < 24; ++round) {
        std::string stream;
        for (int piece = 0; piece < 400; ++piece)
            stream += pieces[next() % std::size(pieces)];
        for (const int columns : { 80, 160 }) {
            Ubiquitty::Terminal whole;
            Ubiquitty::Terminal bytewise;
            for (auto* terminal : { &whole, &bytewise }) {
                Ubiquitty::hardReset(*terminal);
                Ubiquitty::resize(*terminal, 6, columns, Ubiquitty::Modes::ReflowMode::Fixed);
            }
            Ubiquitty::processBytes(&whole, stream);
            for (const char& byte : stream)
                Ubiquitty::processBytes(&bytewise, std::string_view(&byte, 1));
            CHECK_EQ(whole.screens.active->cursorX, bytewise.screens.active->cursorX);
            CHECK_EQ(whole.screens.active->cursorY, bytewise.screens.active->cursorY);
            CHECK_EQ(whole.screens.active->wrapPending, bytewise.screens.active->wrapPending);
            // A cluster that overflows the cell is a pool handle, and the two
            // histories hand out handles in their own order: compare what
            // the handle resolves to, with everything else in the cell.
            const auto sameCell = [](const Ubiquitty::Terminal& a, const Ubiquitty::Cell& x, const Ubiquitty::Terminal& b, const Ubiquitty::Cell& y) {
                if (x.codePoint != y.codePoint || x.linkID() != y.linkID() || std::memcmp(&x.pen, &y.pen, sizeof x.pen) != 0)
                    return false;
                std::uint32_t xs[16];
                std::uint32_t ys[16];
                const int nx = Ubiquitty::cellClusterCodePoints(&a, &x, xs, 16);
                const int ny = Ubiquitty::cellClusterCodePoints(&b, &y, ys, 16);
                return nx == ny && std::equal(xs, xs + nx, ys);
            };
            int mismatches = 0;
            for (int row = 0; row < 6; ++row) {
                for (int column = 0; column < columns; ++column) {
                    const auto actual = whole.grid->logicalCellValueAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
                    const auto expected = bytewise.grid->logicalCellValueAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
                    if (!actual || !expected || !sameCell(whole, *actual, bytewise, *expected))
                        ++mismatches;
                }
            }
            CHECK_MSG(mismatches == 0, "a batched mixed-script stream left cells that bytewise input did not");
        }
    }
}

UBIQUITTY_TEST_MAIN()
TEST(combiningBatchesPreserveContentAcrossEveryByteSplit)
{
    const std::string input = "a\xCC\x81\xCC\x82\xCC\x83"
                              "b\xCD\x8F\xCC\x88\xCC\xA7";
    for (std::size_t split = 0; split <= input.size(); ++split) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 2);
        feed(terminal, input.substr(0, split).c_str());
        feed(terminal, input.substr(split).c_str());
        const std::uint32_t expected[2][4] = { { 'a', 0x301, 0x302, 0x303 }, { 'b', 0x34F, 0x308, 0x327 } };
        for (int column = 0; column < 2; ++column) {
            std::uint32_t actual[16] {};
            CHECK(Ubiquitty::cellClusterCodePoints(&terminal, cell(terminal, 0, column), actual, 16) == 4);
            for (int index = 0; index < 4; ++index) {
                CHECK(actual[index] == expected[column][index]);
            }
        }
        CHECK(terminal.screens.active->wrapPending);
        feed(terminal, "\xCC\x80");
        std::uint32_t continued[16] {};
        CHECK(Ubiquitty::cellClusterCodePoints(&terminal, cell(terminal, 0, 1), continued, 16) == 5);
        CHECK(continued[4] == 0x300);
    }
}

TEST(combiningBatchBoundsStorageWithoutLeavingStrayCells)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    std::string input = "a";
    for (int index = 0; index < 30; ++index) {
        input += "\xCC\x81";
    }
    feed(terminal, input.c_str());
    CHECK(terminal.screens.active->cursorX == 1);
    std::uint32_t codePoints[16] {};
    CHECK(Ubiquitty::cellClusterCodePoints(&terminal, cell(terminal, 0, 0), codePoints, 16) == 16);
    CHECK(codePoints[0] == 'a');
    for (int index = 1; index < 16; ++index) {
        CHECK(codePoints[index] == 0x301);
    }
    feed(terminal, "b");
    CHECK(cell(terminal, 0, 1)->codePoint == 'b');
    CHECK(terminal.screens.active->cursorX == 2);
}

TEST(wideClusterSpansPreserveCellsAndCursorAcrossInputSplits)
{
    const std::string clusters = "漢，字。가각𠀀漢字́👨‍👩‍👧‍👦👍🏽🇯🇵❤️❤︎❤️︎😀︎😀️각👩‍💻漢字𠀀";
    const char* setups[] = {
        "\033[1;38;2;17;83;129m\033]8;;https://example.test\033\\",
        "\033[?69h\033[2;8s\033[1;2H",
        "\033[4h",
        "\033[?7l",
        "\033[76:1m",
    };
    for (const char* setup : setups) {
        for (int columns : { 5, 8, 17 }) {
            const std::string input = clusters + "\r\n" + clusters + "\033[1;2H" + clusters;
            Ubiquitty::Terminal reference;
            Ubiquitty::hardReset(reference);
            Ubiquitty::resize(reference, 4, columns);
            feed(reference, setup);
            for (char byte : input)
                Ubiquitty::processBytes(&reference, { &byte, 1 });
            // A continuation in a later read must still attach to the last
            // cluster, including when the span ended at the right margin.
            feed(reference, "\xCC\x81");
            for (std::size_t split = 0; split <= input.size(); ++split) {
                Ubiquitty::Terminal candidate;
                Ubiquitty::hardReset(candidate);
                Ubiquitty::resize(candidate, 4, columns);
                feed(candidate, setup);
                Ubiquitty::processBytes(&candidate, { input.data(), split });
                Ubiquitty::processBytes(&candidate, { input.data() + split, input.size() - split });
                feed(candidate, "\xCC\x81");
                CHECK(candidate.screens.active->cursorX == reference.screens.active->cursorX);
                CHECK(candidate.screens.active->cursorY == reference.screens.active->cursorY);
                CHECK(candidate.screens.active->wrapPending == reference.screens.active->wrapPending);
                for (int row = 0; row < 4; ++row) {
                    CHECK(candidate.screens.active->lineWrapped[row] == reference.screens.active->lineWrapped[row]);
                    for (int column = 0; column < columns; ++column) {
                        const auto* actual = cell(candidate, row, column);
                        const auto* expected = cell(reference, row, column);
                        CHECK(actual->codePoint == expected->codePoint);
                        CHECK(std::memcmp(&actual->pen, &expected->pen, sizeof(actual->pen)) == 0);
                        CHECK(actual->linkID() == expected->linkID());
                        std::uint32_t actualCluster[16] {};
                        std::uint32_t expectedCluster[16] {};
                        const int actualLength = Ubiquitty::cellClusterCodePoints(&candidate, actual, actualCluster, 16);
                        const int expectedLength = Ubiquitty::cellClusterCodePoints(&reference, expected, expectedCluster, 16);
                        CHECK(actualLength == expectedLength);
                        if (!std::equal(actualCluster, actualCluster + actualLength, expectedCluster)) {
                            std::printf("split=%zu columns=%d row=%d column=%d setup=%s actual=", split, columns, row, column, setup);
                            for (int index = 0; index < actualLength; ++index)
                                std::printf("%x,", actualCluster[index]);
                            std::printf(" expected=");
                            for (int index = 0; index < expectedLength; ++index)
                                std::printf("%x,", expectedCluster[index]);
                            std::printf("\n");
                        }
                        CHECK(std::equal(actualCluster, actualCluster + actualLength, expectedCluster));
                    }
                }
                TEST_ASSERT_DISPLAY_INVARIANTS(candidate.grid.get());
            }
        }
    }
}

TEST(wideTextRunsPreserveTwoByteCharactersAndPunctuationAcrossInputSplits)
{
    const std::string input = "¡§漢，字。¡§漢，字。\r\n¡§漢，字。\033T";
    for (const auto ambiguousWidth : { Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow, Ubiquitty::Unicode::Width::AmbiguousWidth::Wide }) {
        Ubiquitty::Terminal reference;
        Ubiquitty::hardReset(reference);
        Ubiquitty::resize(reference, 3, 9);
        reference.ambiguousWidth = ambiguousWidth;
        for (const char& byte : input)
            feed(reference, { &byte, 1 });
        for (std::size_t split = 0; split <= input.size(); ++split) {
            Ubiquitty::Terminal candidate;
            Ubiquitty::hardReset(candidate);
            Ubiquitty::resize(candidate, 3, 9);
            candidate.ambiguousWidth = ambiguousWidth;
            feed(candidate, std::string_view(input).substr(0, split));
            feed(candidate, std::string_view(input).substr(split));
            CHECK_EQ(candidate.screens.active->cursorX, reference.screens.active->cursorX);
            CHECK_EQ(candidate.screens.active->cursorY, reference.screens.active->cursorY);
            CHECK_EQ(candidate.screens.active->wrapPending, reference.screens.active->wrapPending);
            for (int row = 0; row < 3; ++row) {
                CHECK(candidate.screens.active->lineWrapped[row] == reference.screens.active->lineWrapped[row]);
                for (int column = 0; column < 9; ++column)
                    CHECK(std::memcmp(cell(candidate, row, column), cell(reference, row, column), sizeof(Ubiquitty::Cell)) == 0);
            }
            TEST_ASSERT_DISPLAY_INVARIANTS(candidate.grid.get());
        }
    }
}

TEST(narrowTextRunsPreserveCellsAndCancellationAcrossInputSplits)
{
    const std::string text = "àéîõü ñçøåæ þðßµ¿¡ €\xF0\x9F\xAC\x80é";
    const std::string input = text + "\r\n" + text + "́\r\n" + text + "\033[1;2H" + text + "\033T";
    const std::array<const char*, 4> setups {
        "\033[38;2;17;83;129m\033]8;;https://example.test\033\\漢字漢字\r",
        "\033[4h",
        "\033[?7l",
        "\033[?1049h",
    };
    for (const auto ambiguousWidth : { Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow, Ubiquitty::Unicode::Width::AmbiguousWidth::Wide }) {
        for (const char* setup : setups) {
            for (const int columns : { 5, 8, 17 }) {
                Ubiquitty::Terminal reference;
                Ubiquitty::hardReset(reference);
                Ubiquitty::resize(reference, 3, columns);
                reference.ambiguousWidth = ambiguousWidth;
                feed(reference, setup);
                for (const char& byte : input)
                    feed(reference, { &byte, 1 });
                feed(reference, "\xCC\x81");
                for (std::size_t split = 0; split <= input.size(); ++split) {
                    Ubiquitty::Terminal candidate;
                    Ubiquitty::hardReset(candidate);
                    Ubiquitty::resize(candidate, 3, columns);
                    candidate.ambiguousWidth = ambiguousWidth;
                    feed(candidate, setup);
                    feed(candidate, std::string_view(input).substr(0, split));
                    feed(candidate, std::string_view(input).substr(split));
                    feed(candidate, "\xCC\x81");
                    CHECK_EQ(candidate.screens.active->cursorX, reference.screens.active->cursorX);
                    CHECK_EQ(candidate.screens.active->cursorY, reference.screens.active->cursorY);
                    CHECK_EQ(candidate.screens.active->wrapPending, reference.screens.active->wrapPending);
                    CHECK_EQ(candidate.scrollbackCount, reference.scrollbackCount);
                    CHECK(static_cast<bool>(candidate.lineStore) == static_cast<bool>(reference.lineStore));
                    if (candidate.lineStore && reference.lineStore) {
                        CHECK_EQ(candidate.lineStore->headLineNumber(), reference.lineStore->headLineNumber());
                        CHECK_EQ(candidate.lineStore->count(), reference.lineStore->count());
                        for (int row = 0; row < candidate.lineStore->count(); ++row) {
                            const auto* actual = candidate.lineStore->at(row);
                            const auto* expected = reference.lineStore->at(row);
                            CHECK_EQ(actual->length, expected->length);
                            if (actual->length)
                                CHECK(std::memcmp(actual->cells, expected->cells, static_cast<std::size_t>(actual->length) * sizeof(Ubiquitty::Cell)) == 0);
                        }
                    }
                    for (int row = 0; row < 3; ++row) {
                        CHECK(candidate.screens.active->lineWrapped[row] == reference.screens.active->lineWrapped[row]);
                        for (int column = 0; column < columns; ++column)
                            CHECK(std::memcmp(cell(candidate, row, column), cell(reference, row, column), sizeof(Ubiquitty::Cell)) == 0);
                    }
                    TEST_ASSERT_DISPLAY_INVARIANTS(candidate.grid.get());
                }
            }
        }
    }
}

TEST(narrowTextRunsLeaveTheirSuccessorsToTheCharacterPath)
{
    for (const std::string_view successor : { "\033T", "́", "😀", "ب", "漢", "\xC2\x94", "\xFF" }) {
        const std::string input = "éè" + std::string(successor);
        Ubiquitty::Terminal reference;
        Ubiquitty::hardReset(reference);
        Ubiquitty::resize(reference, 2, 8);
        for (const char& byte : input)
            feed(reference, { &byte, 1 });
        for (std::size_t split = 0; split <= input.size(); ++split) {
            Ubiquitty::Terminal candidate;
            Ubiquitty::hardReset(candidate);
            Ubiquitty::resize(candidate, 2, 8);
            feed(candidate, std::string_view(input).substr(0, split));
            feed(candidate, std::string_view(input).substr(split));
            CHECK_EQ(candidate.screens.active->cursorX, reference.screens.active->cursorX);
            CHECK_EQ(candidate.screens.active->cursorY, reference.screens.active->cursorY);
            CHECK_EQ(candidate.screens.active->wrapPending, reference.screens.active->wrapPending);
            for (int row = 0; row < 2; ++row) {
                CHECK(candidate.grid->rowMeta(row).has(Ubiquitty::RowFlag::NeedsShaping) == reference.grid->rowMeta(row).has(Ubiquitty::RowFlag::NeedsShaping));
                for (int column = 0; column < 8; ++column)
                    CHECK(std::memcmp(cell(candidate, row, column), cell(reference, row, column), sizeof(Ubiquitty::Cell)) == 0);
            }
            TEST_ASSERT_DISPLAY_INVARIANTS(candidate.grid.get());
        }
    }
}

TEST(wideClusterSpansFuseSelectorsAndBoundContinuationStorage)
{
    const std::string input = "❤️👩‍💻❤️︎" + std::string("😀");
    std::string extended = input;
    for (int index = 0; index < 200; ++index)
        extended += "\xCC\x81";
    extended += "🇺🇸👍🏽";
    Ubiquitty::Terminal reference;
    Ubiquitty::hardReset(reference);
    for (char byte : extended)
        Ubiquitty::processBytes(&reference, { &byte, 1 });
    for (std::size_t split = 0; split <= extended.size(); ++split) {
        Ubiquitty::Terminal candidate;
        Ubiquitty::hardReset(candidate);
        Ubiquitty::processBytes(&candidate, { extended.data(), split });
        Ubiquitty::processBytes(&candidate, { extended.data() + split, extended.size() - split });
        CHECK(candidate.screens.active->cursorX == reference.screens.active->cursorX);
        for (int column = 0; column < 14; ++column) {
            const auto* actual = cell(candidate, 0, column);
            const auto* expected = cell(reference, 0, column);
            CHECK(actual->codePoint == expected->codePoint);
            CHECK(std::memcmp(&actual->pen, &expected->pen, sizeof(actual->pen)) == 0);
            std::uint32_t actualCluster[16] {};
            std::uint32_t expectedCluster[16] {};
            const int actualLength = Ubiquitty::cellClusterCodePoints(&candidate, actual, actualCluster, 16);
            const int expectedLength = Ubiquitty::cellClusterCodePoints(&reference, expected, expectedCluster, 16);
            CHECK(actualLength == expectedLength);
            CHECK(std::equal(actualCluster, actualCluster + actualLength, expectedCluster));
        }
        TEST_ASSERT_DISPLAY_INVARIANTS(candidate.grid.get());
    }
}

TEST(selectorUpgradeDoesNotWrapBeforeItsContinuationReachesTheMargin)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, 4);
    feed(terminal, "\033[1;2H❤");
    feed(terminal, "\xEF\xB8\x8F");
    CHECK(terminal.screens.active->cursorX == 3);
    CHECK(!terminal.screens.active->wrapPending);
    feed(terminal, "x");
    CHECK(cell(terminal, 0, 3)->codePoint == 'x');
    CHECK(terminal.screens.active->wrapPending);
}

TEST(reprintingAWideBaseDropsItsPreviousCluster)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "👩‍💻\r👩");
    CHECK(cell(terminal, 0, 0)->codePoint == 0x1F469);
    CHECK(cell(terminal, 0, 0)->clusterID() == 0);
    CHECK(cell(terminal, 0, 0)->combiningLength() == 0);
    feed(terminal, "\r\033[76:1m😀︎\r😀️");
    CHECK(cell(terminal, 0, 0)->combiningLength() == 1);
    CHECK(cell(terminal, 0, 0)->combining[0] == 0xFE0F);
}
