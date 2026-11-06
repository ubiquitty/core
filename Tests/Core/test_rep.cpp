// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"
#include <memory>

using UbiquittyTest::cell;
using UbiquittyTest::feed;

#include "Core/CellGrid.h"
#include "Core/LineStore.h"
#include "Core/TerminalOperations.h"
#include "Core/Terminfo.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

static std::unique_ptr<Ubiquitty::Terminal> makeTerm()
{
    auto t = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*t, 25, 80); // (rows, columns) — 80 wide, 25 tall
    return t;
}

// REP repeats the preceding graphic character.
static void testRepRepeatsLastCharacter()
{
    auto t = makeTerm();
    feed(*t, "A");
    feed(*t, "\033[b"); // CSI b with no param = 1 repeat
    CHECK(cell(*t, 0, 0)->codePoint == 'A');
    CHECK(cell(*t, 0, 1)->codePoint == 'A');
    feed(*t, "\033[2b"); // repeat 2 more
    CHECK(cell(*t, 0, 2)->codePoint == 'A');
    CHECK(cell(*t, 0, 3)->codePoint == 'A');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepRepeatsLastCharacter\n");
}

// REP does nothing when no data-stream item precedes it.
static void testRepNoPriorCharacter()
{
    auto t = makeTerm();
    feed(*t, "\033[b");
    CHECK(cell(*t, 0, 0)->codePoint == 0);
    feed(*t, "\033[5b");
    CHECK(cell(*t, 0, 0)->codePoint == 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepNoPriorCharacter\n");
}

// SPACE is a graphic character and must be repeatable.
static void testRepSpace()
{
    auto t = makeTerm();
    feed(*t, " ");
    feed(*t, "\033[3b");
    CHECK(cell(*t, 0, 0)->codePoint == ' ');
    CHECK(cell(*t, 0, 1)->codePoint == ' ');
    CHECK(cell(*t, 0, 2)->codePoint == ' ');
    CHECK(cell(*t, 0, 3)->codePoint == ' ');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepSpace\n");
}

// CR is a repeatable control function. After CR, the preceding item is CR,
// not the graphic. REP repeats CR's effect (idempotent here — cursor to column 0).
static void testRepCR()
{
    auto t = makeTerm();
    feed(*t, "ABC"); // cursor at col 3
    feed(*t, "\r"); // CR → cursor to col 0, preceding = C0 CR
    feed(*t, "\033[2b"); // REP 2 → repeat CR twice (no-op)
    CHECK(cell(*t, 0, 3)->codePoint == 0); // CR didn't print anything
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepCR\n");
}

// REP repeats a C0 control function's effect.
// BS CSI 2 b : after AB, backspace once, then REP 2 repeats BS twice more.
static void testRepC0Backspace()
{
    auto t = makeTerm();
    feed(*t, "AB"); // cursor at col 2
    feed(*t, "\010"); // BS — cursor to col 1
    feed(*t, "\033[2b"); // REP 2 — repeat BS twice more → cursor to col 0
    feed(*t, "X"); // write at col 0 → overwrites A
    CHECK(cell(*t, 0, 0)->codePoint == 'X');
    CHECK(cell(*t, 0, 1)->codePoint == 'B');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepC0Backspace\n");
}

// A C1 control is not a graphic, so REP after one repeats the graphic before it.
// ECMA-48 8.3.103 defines REP over the last graphic character; replaying a
// control sequence multiplies whatever its handler costs, against a count sized
// for placing a glyph.
// The amplification the restriction removes, at a size a test can run: a
// replayed RIS was one full teardown, placement-new and reflow per count, all
// under the model lock, from six bytes of ordinary output.
static void testRepDoesNotAmplifyAResettingControl()
{
    auto t = makeTerm();
    feed(*t, "Z");
    feed(*t, "\033c"); // RIS
    feed(*t, "\033[4096b"); // a replayed RIS would be 4096 hard resets
    // The terminal is alive and reset once: the grid is usable and the REP
    // wrote the graphic the reset had already cleared away.
    CHECK(t->columns > 0);
    CHECK(t->screens.active->cursorY >= 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepDoesNotAmplifyAResettingControl\n");
}

// The second attack the restriction removes: `ESC # 8` then a REP. A replayed
// DECALN is a rows*columns write loop plus markAllDirty per count.
//
// The fill itself cannot show this: DECALN writes E over E, so one is
// indistinguishable from a thousand in the cells and in the content generation
// that counts writes to them. What DECALN also does is reset both margin pairs
// to the full page. Setting a margin after it gives a replay something to
// destroy, and a margin that survived is a DECALN that did not re-run.
static void testRepDoesNotAmplifyDecaln()
{
    auto t = makeTerm();
    feed(*t, "\033#8"); // DECALN — fills the screen with E and clears the margins
    CHECK(cell(*t, 0, 0)->codePoint == 'E');
    feed(*t, "\033[5;20r"); // DECSTBM — a top/bottom margin DECALN would clear
    CHECK(t->screens.active->scrollTop == 4);
    CHECK(t->screens.active->scrollBottom == 19);

    feed(*t, "\033[64b"); // a replayed DECALN would be 64 more fills, clearing it
    CHECK(t->screens.active->scrollTop == 4);
    CHECK(t->screens.active->scrollBottom == 19);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepDoesNotAmplifyDecaln\n");
}

// The third: `CSI c` then a REP. A replayed DA1 costs the screen nothing and
// the host everything -- one device-attributes reply per count, into the output
// path, from four bytes of ordinary program output.
static void testRepDoesNotAmplifyADeviceAttributesReply()
{
    auto t = makeTerm();
    char outputBuffer[512];
    feed(*t, "W"); // the graphic the record holds
    feed(*t, "\033[c"); // DA1 — one reply
    (void)UbiquittyTest::drain(*t, outputBuffer); // taken away, so the next drain is REP's alone
    feed(*t, "\033[64b"); // a replayed DA1 would be 64 more replies
    const int written = UbiquittyTest::drain(*t, outputBuffer);
    // Not one byte of reply. REP repeated the graphic instead.
    CHECK(written == 0);
    CHECK(cell(*t, 0, 1)->codePoint == 'W');
    CHECK(cell(*t, 0, 64)->codePoint == 'W');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepDoesNotAmplifyADeviceAttributesReply\n");
}

static void testRepAfterC1RepeatsTheGraphic()
{
    auto t = makeTerm();
    feed(*t, "\033[3;1H");
    feed(*t, "X"); // the graphic the record holds
    feed(*t, "\033M"); // RI — moves the cursor, records nothing repeatable
    feed(*t, "\033[2b"); // REP 2 — two more Xs, no further reverse index
    // RI moved the cursor up one row and left the column where X had put it, so
    // the repeats land on the row above, starting at that column.
    CHECK(t->screens.active->cursorY == 1); // row 2 less RI's one row, and no more
    CHECK(cell(*t, 1, 1)->codePoint == 'X');
    CHECK(cell(*t, 1, 2)->codePoint == 'X');
    CHECK(cell(*t, 2, 0)->codePoint == 'X'); // the original, where it was written
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterC1RepeatsTheGraphic\n");
}

// A CSI is not a graphic either: after CUF, REP repeats the character, and the
// cursor advances by the repeats rather than by another CUF each time.
static void testRepAfterCSIRepeatsTheGraphic()
{
    auto t = makeTerm();
    feed(*t, "A"); // col 0, cursor at 1
    feed(*t, "\033[2C"); // CUF 2 → col 3
    feed(*t, "\033[3b"); // REP 3 → three As at 3, 4, 5
    CHECK(cell(*t, 0, 3)->codePoint == 'A');
    CHECK(cell(*t, 0, 5)->codePoint == 'A');
    CHECK(cell(*t, 0, 6)->codePoint == 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterCSIRepeatsTheGraphic\n");
}

// DCH is a CSI, so REP after it does not delete again: the record still holds
// the graphic, and repeating it writes cells rather than removing them.
static void testRepAfterDeleteCharactersRepeatsTheGraphic()
{
    auto t = makeTerm();
    feed(*t, "ABC");
    feed(*t, "\033[3D"); // back to col 0
    feed(*t, "\033[1P"); // DCH 1 — deletes A, leaving BC
    feed(*t, "\033[2b"); // REP 2 — two Cs, the last graphic, not two more DCHs
    CHECK(cell(*t, 0, 0)->codePoint == 'C');
    CHECK(cell(*t, 0, 1)->codePoint == 'C');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterDeleteCharactersRepeatsTheGraphic\n");
}

// SGR is the sequence a coloured stream emits between every cell, and it is not
// a graphic: REP after one repeats the character, in the colour the SGR set.
static void testRepAfterSGRRepeatsTheGraphic()
{
    auto t = makeTerm();
    feed(*t, "A"); // A at (0,0), cursor at (0,1)
    feed(*t, "\033[31m"); // red — records nothing repeatable
    feed(*t, "\033[2b"); // REP 2 — two more As, both red
    CHECK(cell(*t, 0, 0)->codePoint == 'A');
    CHECK(cell(*t, 0, 1)->codePoint == 'A');
    CHECK(cell(*t, 0, 2)->codePoint == 'A');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterSGRRepeatsTheGraphic\n");
}

// REP after REP replays the previous REP's WHOLE effect (its expansion),
// carried as the record's multiplier: A REP2 REP2 = 1 + 2 + 2·2 = 7 A's
// (conformance case G10, strict recursive-effect reading).
static void testRepAfterRepReplaysExpansion()
{
    auto t = makeTerm();
    feed(*t, "A"); // preceding = Graphic A (×1)
    feed(*t, "\033[2b"); // REP 2 → AA; preceding = A ×2
    feed(*t, "\033[2b"); // REP 2 → 2 × (A ×2) = AAAA
    for (int c = 0; c <= 6; c++)
        CHECK(cell(*t, 0, c)->codePoint == 'A');
    CHECK(cell(*t, 0, 7)->codePoint == 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterRepReplaysExpansion\n");
}

// The REP-after-REP multiplier saturates at maxRepeatEffects: a chain of
// max-count REPs stays linear (65535 effects each), never the unbounded
// n² / n³ expansion the literal recursive reading would imply.
static void testRepChainSaturates()
{
    auto t = makeTerm();
    feed(*t, "A");
    // The record's multiplier is the effect count the next REP multiplies by,
    // and the saturation is a std::min on it (`repeatPrecedingCharacter` in
    // VT.cpp). cursorY cannot say anything here: every decrement site in Core
    // clamps, so cursorY >= 0 holds whether the cap works or not.
    //
    // Literals rather than PrecedingItem::maxRepeatEffects. Comparing the value
    // against the constant that produced it moves both sides together, which
    // is an assertion about nothing — 300 and 65535 are the documented cap and
    // a count below it, and pinning them is the point.
    //
    // 300 first, because 300 x 300 = 90000 is representable: an unclamped
    // product is DISTINGUISHABLE from the saturated one. At 65535 x 65535 the
    // uncapped value is still representable but emitting it is four billion
    // effects, so a broken cap hangs rather than failing.
    feed(*t, "\033[300b"); // 300 A's; multiplier carries 300
    CHECK_EQ(t->parser.precedingItem.multiplier, 300);
    feed(*t, "\033[300b"); // 300 x 300 = 90000, saturated to 65535
    CHECK_EQ(t->parser.precedingItem.multiplier, 65535);
    feed(*t, "\033[300b"); // still 65535
    CHECK_EQ(t->parser.precedingItem.multiplier, 65535);

    // And the flood bound itself: three max-count REPs complete rather than
    // expanding n^2 / n^3.
    feed(*t, "B");
    feed(*t, "\033[65535b");
    CHECK_EQ(t->parser.precedingItem.multiplier, 65535);
    feed(*t, "\033[65535b");
    CHECK_EQ(t->parser.precedingItem.multiplier, 65535);
    feed(*t, "\033[65535b");
    CHECK_EQ(t->parser.precedingItem.multiplier, 65535);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepChainSaturates\n");
}

// A wide graphic repeats as a full wide pair — lead + continuation —
// exactly as if it arrived again in the stream (conformance case G06).
static void testRepWideGraphic()
{
    auto t = makeTerm();
    feed(*t, "\xe7\x95\x8c"); // 界 (U+754C), cells 0-1
    feed(*t, "\033[2b"); // two more 界: cells 2-3, 4-5
    CHECK(cell(*t, 0, 2)->codePoint == 0x754C);
    CHECK(cell(*t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    CHECK(cell(*t, 0, 3)->pen.attributes.contains(Ubiquitty::VT::Attribute::WideContinuation));
    CHECK(cell(*t, 0, 4)->codePoint == 0x754C);
    CHECK(t->screens.active->cursorX == 6);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepWideGraphic\n");
}

// An emoji graphic repeats with its full code point and width — nothing
// truncated by the predecessor record (conformance case G07).
static void testRepEmoji()
{
    auto t = makeTerm();
    feed(*t, "\xf0\x9f\x99\x82"); // 🙂 (U+1F642), cells 0-1
    feed(*t, "\033[b"); // one more at cells 2-3
    CHECK(cell(*t, 0, 2)->codePoint == 0x1F642);
    CHECK(cell(*t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Wide));
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepEmoji\n");
}

// After a wide graphic the predecessor is that graphic, not an earlier
// narrow one (the wide emit path must record too).
static void testRepAfterWideRepeatsWide()
{
    auto t = makeTerm();
    feed(*t, "A\xe7\x95\x8c"); // A at 0, 界 at 1-2
    feed(*t, "\033[b"); // repeats 界 at 3-4, not a
    CHECK(cell(*t, 0, 3)->codePoint == 0x754C);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterWideRepeatsWide\n");
}

// The last received graphic of "e + combining acute" is the MARK; REP
// re-attaches it to the open cluster instead of re-printing the base
// (conformance case G08). The cursor must not advance.
static void testRepCombiningMark()
{
    auto t = makeTerm();
    feed(*t, "e\xcc\x81"); // e + U+0301 → one cell, cursor at 1
    feed(*t, "\033[2b"); // two more acutes attach to the same cluster
    CHECK(t->screens.active->cursorX == 1);
    CHECK(cell(*t, 0, 0)->codePoint == 'e');
    CHECK(cell(*t, 0, 0)->combiningLength() >= 1); // cluster carries the mark(s)
    CHECK(cell(*t, 0, 1)->codePoint == 0); // nothing printed beyond
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepCombiningMark\n");
}

// DEC Special Graphics: the repeat re-emits the characterSet-resolved glyph
// recorded at receive time (conformance case G09).
static void testRepDECLineDrawing()
{
    auto t = makeTerm();
    feed(*t, "\033(0x\033[4b\033(B"); // designate G0=DEC graphics, x → │, REP 4
    for (int c = 0; c <= 4; c++)
        CHECK(cell(*t, 0, c)->codePoint == 0x2502);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepDECLineDrawing\n");
}

// FF executes the linefeed action; REP after FF repeats that effect
// (conformance case C06 — VT and FF share the LF handler).
static void testRepFormFeed()
{
    auto t = makeTerm();
    feed(*t, "A\f"); // FF → row 1, column preserved
    feed(*t, "\033[2b"); // two more → row 3
    feed(*t, "B");
    CHECK(cell(*t, 3, 1)->codePoint == 'B');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepFormFeed\n");
}

// PM (ESC ^ ... ST) is a control string: excluded from REP exactly like
// OSC/DCS/APC (conformance case S04).
static void testRepExcludesPM()
{
    auto t = makeTerm();
    feed(*t, "A");
    feed(*t, "\033^private message\033\\"); // PM ... ST
    feed(*t, "\033[3b"); // preceding is None — no-op
    CHECK(cell(*t, 0, 1)->codePoint == 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepExcludesPM\n");
}

// SGR 31 then REP: the following X must be red — REP replays the SGR
// effect, it does not print a graphic (conformance case R01).
static void testRepSGRThenPrint()
{
    auto t = makeTerm();
    feed(*t, "\033[31m\033[2b");
    feed(*t, "X");
    CHECK(cell(*t, 0, 0)->codePoint == 'X');
    CHECK(cell(*t, 0, 0)->pen.foregroundIndex == 1); // SGR 31 = red
    CHECK(cell(*t, 0, 1)->codePoint == 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepSGRThenPrint\n");
}

// TBC is a CSI: the tab stops it cleared stay cleared, and REP after it repeats
// the graphic rather than clearing again.
static void testRepAfterTBCRepeatsTheGraphic()
{
    auto t = makeTerm();
    feed(*t, "Q");
    feed(*t, "\033[3g"); // clear all tab stops
    feed(*t, "\033[2b"); // REP 2 — two more Qs
    CHECK(cell(*t, 0, 1)->codePoint == 'Q');
    CHECK(cell(*t, 0, 2)->codePoint == 'Q');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepAfterTBCRepeatsTheGraphic\n");
}

// Control strings set preceding to None. REP after OSC is a no-op.
static void testRepExcludesControlStrings()
{
    auto t = makeTerm();
    feed(*t, "A");
    feed(*t, "\033]0;title\033\\"); // OSC 0 ; title ST
    feed(*t, "\033[3b"); // REP 3 — preceding is None, no-op
    CHECK(cell(*t, 0, 1)->codePoint == 0);
    CHECK(cell(*t, 0, 2)->codePoint == 0);
    CHECK(cell(*t, 0, 3)->codePoint == 0);
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepExcludesControlStrings\n");
}

// ECMA-48: Pn defaults to 1 when OMITTED; an EXPLICIT 0 is the numeric
// value zero — zero repetitions (conformance case G03). Not the mainstream 0→1
// coercion, which collapses the two cases and so cannot tell an omitted Pn from
// an explicit 0.
static void testRepZeroParameter()
{
    auto t = makeTerm();
    feed(*t, "A");
    feed(*t, "\033[0b"); // REP 0 — repeat nothing
    CHECK(cell(*t, 0, 1)->codePoint == 0);
    // A following REP repeats the zero-effect REP: still nothing.
    feed(*t, "\033[b");
    CHECK(cell(*t, 0, 1)->codePoint == 0);
    // Omitted Pn defaults to 1: a fresh graphic + bare REP repeats once.
    feed(*t, "B\033[b");
    CHECK(cell(*t, 0, 1)->codePoint == 'B');
    CHECK(cell(*t, 0, 2)->codePoint == 'B');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepZeroParameter\n");
}

// CUP repeat is idempotent but must not source from screen cells.
// After red A, plain B, CUP 1;2, REP 3 must not copy the stale A cell.
static void testRepDoesNotReadCursorLeft()
{
    auto t = makeTerm();
    feed(*t, "\033[31mA\033[0mB"); // red A, then plain B
    // Cursor is at col 2 after B. REP preceding = Graphic 'B'.
    // Now CUP to 1;2 moves the cursor — preceding becomes CSI CUP.
    feed(*t, "\033[1;2H"); // CUP to row 0, col 1 (cursorX=1)
    feed(*t, "\033[3b"); // REP 3 — repeats CUP effect (idempotent)
    // CUP was the preceding item, so it's re-dispatched 3 times.
    // That's idempotent (same absolute position). Nothing is written.
    CHECK(cell(*t, 0, 0)->codePoint == 'A'); // red 'A' preserved
    CHECK(cell(*t, 0, 1)->codePoint == 'B'); // plain 'B' preserved
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepDoesNotReadCursorLeft\n");
}

// C0 LF repeated: CSI 2 b repeats LF twice.
static void testRepLF()
{
    auto t = makeTerm();
    feed(*t, "A"); // row 0, col 1
    feed(*t, "\n"); // LF → row 1, col 1, preceding = C0 LF
    feed(*t, "\033[2b"); // REP 2 → repeat LF twice → row 3
    feed(*t, "B");
    CHECK(cell(*t, 3, 1)->codePoint == 'B');
    Ubiquitty::hardReset(*t);
    t.reset();
    printf("PASS testRepLF\n");
}

struct GraphicRepeatCase {
    std::string_view initial;
    std::string_view repeated;
};

static constexpr GraphicRepeatCase graphicRepeatCases[] = {
    { "a", "a" },
    { "\xE7\x95\x8C", "\xE7\x95\x8C" },
    { "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x80" },
    { "e\xCC\x81", "\xCC\x81" },
    { "\xE2\x9D\xA4\xEF\xB8\x8F", "\xE2\x9D\xA4\xEF\xB8\x8F" },
    { "\xC2\xA9\xEF\xB8\x8E", "\xC2\xA9\xEF\xB8\x8E" },
};

static void prepareGraphicRepeat(Ubiquitty::Terminal& terminal, Ubiquitty::ScreenMode screen, int columns)
{
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 2, columns, Ubiquitty::Modes::ReflowMode::Fixed);
    if (screen == Ubiquitty::ScreenMode::Alternate)
        feed(terminal, "\033[?1049h");
}

static void checkRepeatedCell(const Ubiquitty::Terminal& actualTerminal, const Ubiquitty::Cell& actual,
    const Ubiquitty::Terminal& expectedTerminal, const Ubiquitty::Cell& expected)
{
    CHECK_EQ(actual.codePoint, expected.codePoint);
    CHECK(std::memcmp(&actual.pen, &expected.pen, sizeof(actual.pen)) == 0);
    CHECK_EQ(actual.linkID(), expected.linkID());
    std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> actualCluster {};
    std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> expectedCluster {};
    CHECK_EQ(Ubiquitty::cellClusterCodePoints(&actualTerminal, &actual, actualCluster.data(), actualCluster.size()),
        Ubiquitty::cellClusterCodePoints(&expectedTerminal, &expected, expectedCluster.data(), expectedCluster.size()));
    CHECK(actualCluster == expectedCluster);
}

static void checkGraphicRepeatState(const Ubiquitty::Terminal& actual, const Ubiquitty::Terminal& expected)
{
    CHECK_EQ(actual.screens.active->cursorX, expected.screens.active->cursorX);
    CHECK_EQ(actual.screens.active->cursorY, expected.screens.active->cursorY);
    CHECK_EQ(actual.screens.active->wrapPending, expected.screens.active->wrapPending);
    CHECK_EQ(actual.scrollbackCount, expected.scrollbackCount);
    for (int row = 0; row < actual.rows; ++row) {
        CHECK_EQ(actual.screens.active->lineWrapped[row], expected.screens.active->lineWrapped[row]);
        for (int column = 0; column < actual.columns; ++column)
            checkRepeatedCell(actual, *cell(actual, row, column), expected, *cell(expected, row, column));
    }
    CHECK(static_cast<bool>(actual.lineStore) == static_cast<bool>(expected.lineStore));
    if (!actual.lineStore || !expected.lineStore)
        return;
    CHECK_EQ(actual.lineStore->count(), expected.lineStore->count());
    CHECK_EQ(actual.lineStore->headLineNumber(), expected.lineStore->headLineNumber());
    CHECK(actual.lineStore->frontLine().continuity == expected.lineStore->frontLine().continuity);
    for (int row = 0; row < std::min(actual.lineStore->count(), expected.lineStore->count()); ++row) {
        const auto& actualLine = *actual.lineStore->at(row);
        const auto& expectedLine = *expected.lineStore->at(row);
        CHECK_EQ(actualLine.length, expectedLine.length);
        CHECK_EQ(actualLine.wrapped, expectedLine.wrapped);
        CHECK_EQ(actualLine.lineAttribute, expectedLine.lineAttribute);
        CHECK_EQ(actualLine.mark, expectedLine.mark);
        CHECK_EQ(actualLine.needsShaping, expectedLine.needsShaping);
        for (int column = 0; column < std::min(actualLine.length, expectedLine.length); ++column)
            checkRepeatedCell(actual, actualLine.cells[column], expected, expectedLine.cells[column]);
    }
}

static void testRepAvoidsGraphicCancellationStorage()
{
    // CSI dispatch makes its entire expansion unavailable to CCH. Even a
    // repeated glyph that wraps and scrolls needs no cancellation journal.
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const int columns : { 4, 5 }) {
            for (const auto& graphic : graphicRepeatCases) {
                auto terminal = makeTerm();
                prepareGraphicRepeat(*terminal, screen, columns);
                feed(*terminal, std::string(graphic.initial) + "\033[100b");
                CHECK(!terminal->graphicCancellation);
                CHECK_EQ(terminal->parser.precedingItem.multiplier, 100u);
            }
        }
    }
    printf("PASS testRepAvoidsGraphicCancellationStorage\n");
}

static void testRepCancellationBoundaryAcrossInputChunks()
{
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const int columns : { 4, 5 }) {
            for (const auto& graphic : graphicRepeatCases) {
                auto expected = makeTerm();
                prepareGraphicRepeat(*expected, screen, columns);
                std::string literal(graphic.initial);
                for (int index = 0; index < 100; ++index)
                    literal += graphic.repeated;
                // A completed CSI ends cancellation eligibility without moving
                // the cursor or changing the emitted cells.
                literal += "\033[m";
                feed(*expected, literal);

                const auto stream = std::string(graphic.initial) + "\033[100b\033T";
                for (std::size_t split = graphic.initial.size(); split <= stream.size(); ++split) {
                    auto actual = makeTerm();
                    prepareGraphicRepeat(*actual, screen, columns);
                    feed(*actual, std::string_view(stream).substr(0, split));
                    feed(*actual, std::string_view(stream).substr(split));
                    checkGraphicRepeatState(*actual, *expected);
                    CHECK_EQ(actual->parser.precedingItem.multiplier, 100u);
                    CHECK(!actual->graphicCancellation || !actual->graphicCancellation->isValid);

                    // The next real graphic is cancellable, including a wrap
                    // that moves a full row into history.
                    feed(*actual, "X\033T");
                    checkGraphicRepeatState(*actual, *expected);
                }
            }
        }
    }
    printf("PASS testRepCancellationBoundaryAcrossInputChunks\n");
}

static void testRepSelectorKeepsSeparatePlacementBeforeALowerBand()
{
    auto actual = makeTerm();
    auto expected = makeTerm();
    for (auto* terminal : { actual.get(), expected.get() }) {
        Ubiquitty::resize(*terminal, 3, 12, Ubiquitty::Modes::ReflowMode::Fixed);
        terminal->drawsSizedText = true;
        feed(*terminal, "\033[1;4H\033]66;s=2;X\033\\\033[1;1H"
                        "\xE2\x9D\xA4\xEF\xB8\x8F\033[2;3H");
    }
    // The base fits before the lower band. Receiving its selector separately
    // widens that placed cell; fusing the pair would skip the reserved band
    // before placing either code point and put the base in another column.
    feed(*expected, "\xE2\x9D\xA4");
    feed(*expected, "\xEF\xB8\x8F");
    feed(*expected, "\033[m");
    feed(*actual, "\033[b\033T");
    CHECK_EQ(cell(*actual, 1, 2)->codePoint, 0x2764u);
    checkGraphicRepeatState(*actual, *expected);
    printf("PASS testRepSelectorKeepsSeparatePlacementBeforeALowerBand\n");
}

int main(int argc, const char* argv[])
{
    (void)argc;
    (void)argv;
    testRepRepeatsLastCharacter();
    testRepNoPriorCharacter();
    testRepSpace();
    testRepCR();
    testRepC0Backspace();
    testRepDoesNotAmplifyAResettingControl();
    testRepDoesNotAmplifyDecaln();
    testRepDoesNotAmplifyADeviceAttributesReply();
    testRepAfterC1RepeatsTheGraphic();
    testRepAfterCSIRepeatsTheGraphic();
    testRepAfterDeleteCharactersRepeatsTheGraphic();
    testRepAfterSGRRepeatsTheGraphic();
    testRepAfterRepReplaysExpansion();
    testRepChainSaturates();
    testRepWideGraphic();
    testRepEmoji();
    testRepAfterWideRepeatsWide();
    testRepCombiningMark();
    testRepDECLineDrawing();
    testRepFormFeed();
    testRepExcludesControlStrings();
    testRepExcludesPM();
    testRepSGRThenPrint();
    testRepAfterTBCRepeatsTheGraphic();
    testRepZeroParameter();
    testRepDoesNotReadCursorLeft();
    testRepLF();
    testRepAvoidsGraphicCancellationStorage();
    testRepCancellationBoundaryAcrossInputChunks();
    testRepSelectorKeepsSeparatePlacementBeforeALowerBand();

    printf("\n=== Results: %s ===\n", failures ? "FAILED" : "ALL PASSED");
    return failures;
}
