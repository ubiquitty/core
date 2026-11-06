// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Every editing control, swept against a wide pair in every intersecting
// position.
//
// A wide character occupies two columns: Attribute::Wide at the lead and
// Attribute::WideContinuation at the cell after it. CellGrid::writeWideCell
// asserts that shape when the pair is written, and nothing asserts it again
// afterwards — so an editing control that shifts, erases or scrolls only one
// half leaves a grid the write path would have rejected.
//
// The invariant — one property, not an expected grid per case. After any
// control, in every row:
//
//     a WideContinuation cell has a Wide cell immediately to its left, and
//     a Wide cell has a WideContinuation cell immediately to its right.
//
// Either half alone is an orphan. Stating it this way means the sweep does not
// have to know the correct outcome of each intersection — only that whatever
// the control did, it did not saw a pair in half. That catches the whole
// partial-intersection class without encoding one control's semantics, and it
// keeps holding when a pair becomes a block: "every continuation resolves to a
// live anchor" is the same rule in two dimensions.

#include "Core/Screen/Addressing.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/TerminalOperations.h"

#include <cstdio>
#include <string>

using UbiquittyTest::cell;
using UbiquittyTest::feed;
using UbiquittyTest::firstOrphanedWideHalf;

namespace {

// A CJK code point is width 2 on every width table the Core ships, so it lands
// as a lead plus a continuation without depending on emoji presentation rules.
constexpr const char* wideCharacter = "\xE6\x9D\xB1"; // U+6771

/// One editing control, named for the failure message and parameterised by the
/// column it acts from.
struct Control {
    const char* name;
    const char* format; // one %d: the column the cursor sits at, 1-based
};

// The seven the spec calls out, plus the two scroll controls that move whole
// rows and can therefore separate a pair from its row-mates.
//
// The DEC rectangle family and the column controls are here for the same reason
// and reach the pair differently. ICH and DCH shift a whole row from the cursor;
// DECIC and DECDC shift a COLUMN through every row at once, so a pair three rows
// down is separated by a control aimed nowhere near it. The rectangle ops act on
// a region with its own left and right edge, which is the only way an erase can
// land on a pair's continuation while leaving its lead and the columns beyond it
// untouched — the linear erases always run to a margin.
//
// DECCRA is the one with no analogue: it COPIES a region, so a half can arrive
// somewhere its partner never was. An orphan it creates is written rather than
// left behind, which no other control here can do.
//
// Each rectangle sequence spans two columns from the cursor and two rows, so the
// same offset sweep straddles the pair's lead and its continuation.
constexpr Control controls[] = {
    { "ICH", "\x1b[%d;%dH\x1b[2@" },
    { "DCH", "\x1b[%d;%dH\x1b[2P" },
    { "ECH", "\x1b[%d;%dH\x1b[2X" },
    { "EL-right", "\x1b[%d;%dH\x1b[0K" },
    { "EL-left", "\x1b[%d;%dH\x1b[1K" },
    { "EL-all", "\x1b[%d;%dH\x1b[2K" },
    { "ED-below", "\x1b[%d;%dH\x1b[0J" },
    { "ED-above", "\x1b[%d;%dH\x1b[1J" },
    { "IL", "\x1b[%d;%dH\x1b[1L" },
    { "DL", "\x1b[%d;%dH\x1b[1M" },
    { "SU", "\x1b[%d;%dH\x1b[1S" },
    { "SD", "\x1b[%d;%dH\x1b[1T" },
    { "DECIC", "\x1b[%d;%dH\x1b[2'}" },
    { "DECDC", "\x1b[%d;%dH\x1b[2'~" },
    { "DECERA", "\x1b[%d;%dH\x1b[1;%d;2;%d$z" },
    { "DECSERA", "\x1b[%d;%dH\x1b[1;%d;2;%d${" },
    { "DECFRA", "\x1b[%d;%dH\x1b[65;1;%d;2;%d$x" },
    { "DECCRA", "\x1b[%d;%dH\x1b[1;%d;2;%d;1;3;1;1$v" },
    { "DECCARA", "\x1b[%d;%dH\x1b[1;%d;2;%d;7$r" },
    { "DECRARA", "\x1b[%d;%dH\x1b[1;%d;2;%d;7$t" },
};

/// Places a wide pair with its lead at `leadColumn`, then runs `control` from
/// `cursorColumn`, and reports any orphan left behind.
///
/// The cursor sweeps every column from before the pair to past it, so each
/// control meets the pair head-on, straddling either edge, and clear of it.
void sweepControl(const Control& control, int leadColumn, int cursorColumn, int& checked)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);

    // Fill the row so an erase has something to remove and a shift has
    // something to move, then overwrite the pair's columns.
    feed(terminal, "\x1b[1;1H");
    for (int column = 0; column < terminal.columns; ++column)
        feed(terminal, "-");
    char place[32];
    std::snprintf(place, sizeof(place), "\x1b[1;%dH", leadColumn + 1);
    feed(terminal, place);
    feed(terminal, wideCharacter);

    // A second row, so the row-moving controls have a neighbour to pull in.
    feed(terminal, "\x1b[2;1H");
    feed(terminal, wideCharacter);

    // Four arguments for every control, because a rectangle names its own left
    // and right edge where a linear one runs to the margin. Passing fewer than a
    // format consumes reads past the argument list, so the count is uniform and
    // the linear formats ignore what they do not need.
    char sequence[64];
    std::snprintf(sequence, sizeof(sequence), control.format,
        1, cursorColumn + 1, cursorColumn + 1, cursorColumn + 2);
    feed(terminal, sequence);

    const std::string orphan = firstOrphanedWideHalf(terminal);
    ++checked;
    if (!orphan.empty()) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s at lead=%d cursor=%d: %s",
            control.name, leadColumn, cursorColumn, orphan.c_str());
        CHECK_MSG(false, message);
    }
}

} // namespace

TEST(everyEditingControlLeavesNoOrphanedWideHalf)
{
    int checked = 0;
    // Lead positions cover the left edge, the interior and the right edge; the
    // cursor sweeps a window around each so every intersection shape occurs —
    // entirely before the pair, straddling the lead, straddling the
    // continuation, exactly covering it, and entirely after.
    for (const Control& control : controls) {
        for (int leadColumn : { 0, 1, 5, 38, 78 }) {
            for (int offset = -3; offset <= 3; ++offset) {
                const int cursorColumn = leadColumn + offset;
                if (cursorColumn < 0 || cursorColumn >= 80)
                    continue;
                sweepControl(control, leadColumn, cursorColumn, checked);
            }
        }
    }
    CHECK(checked > 300);
}

TEST(writingOverOneHalfOfAPairLeavesNoOrphan)
{
    // The direct case the editing controls reach indirectly: a plain glyph
    // written onto either half must take the whole pair with it.
    for (int overwriteOffset : { 0, 1 }) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        feed(terminal, "\x1b[1;5H");
        feed(terminal, wideCharacter);
        char place[32];
        std::snprintf(place, sizeof(place), "\x1b[1;%dH", 5 + overwriteOffset);
        feed(terminal, place);
        feed(terminal, "x");
        const std::string orphan = firstOrphanedWideHalf(terminal);
        char message[160];
        std::snprintf(message, sizeof(message),
            "overwriting half %d of a wide pair: %s", overwriteOffset, orphan.c_str());
        CHECK_MSG(orphan.empty(), message);
    }
}

TEST(aPairAtTheRightMarginIsNeverHalfWritten)
{
    // writeWideCell rejects a pair that will not fit rather than writing one
    // half. Autowrap is what normally prevents the case; with it off the reject
    // path is what keeps the grid consistent.
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\x1b[?7l"); // DECAWM off
    feed(terminal, "\x1b[1;80H"); // last column: a wide pair cannot fit
    feed(terminal, wideCharacter);
    const std::string orphan = firstOrphanedWideHalf(terminal);
    char message[160];
    std::snprintf(message, sizeof(message), "wide char at the right margin: %s", orphan.c_str());
    CHECK_MSG(orphan.empty(), message);
}

/// ---- The multicell hint may lag reality upward, never downward ----
//
// RowFlag::HasMulticell tells a reader "resolve this row carefully". The two
// directions are not equally bad, so they are asserted separately.
//
// A row that holds a wide half without the flag is a correctness fault: the
// cheap loop resolves a continuation cell against the wrong anchor and paints
// the wrong glyph. A row that has the flag without holding a wide half is only
// slow. The flag latches on write and clears when the row is recycled, so it is
// allowed to lag in the safe direction and never in the other.
TEST(eachRectAttributeControlAgreesWithItsOwnEnumeration)
{
    // `allAttributes` is written twice in Prefixed.cpp — once for DECRARA at the
    // toggle default, once for DECCARA at the clear default — as the same
    // four-element list under one name in two scopes. Two literals sharing a
    // name drift silently, and every other case in this file passes a single
    // selector, so the no-selector form that reads the list whole is exercised
    // nowhere.
    //
    // Each control is compared with its own explicit enumeration rather than
    // with the other. The two lists hold the same value in different roles —
    // DECRARA's default reverses all four, DECCARA's clears all four — so an
    // equality between the controls would be asserting a coincidence of purpose
    // rather than a shared set, and it fails today for that reason.
    auto attributesAt = [](const Ubiquitty::Terminal& terminal) {
        const Ubiquitty::Cell* here = cell(terminal, 0, 0);
        return here ? Ubiquitty::cellPen(*here).attributes : Ubiquitty::VT::AttributeSet {};
    };
    auto after = [&](const char* sequence) {
        Ubiquitty::Terminal terminal;
        Ubiquitty::hardReset(terminal);
        feed(terminal, "\x1b[1;1HX");
        feed(terminal, sequence);
        return attributesAt(terminal);
    };

    // DECRARA: no selector reverses every attribute in the list, and naming all
    // four reverses the same set.
    const Ubiquitty::VT::AttributeSet reversedByDefault = after("\x1b[1;1;1;1$t");
    const Ubiquitty::VT::AttributeSet reversedByName = after("\x1b[1;1;1;1;1;4;5;7$t");
    CHECK(reversedByDefault == reversedByName);
    CHECK(reversedByDefault != Ubiquitty::VT::AttributeSet {});

    // DECCARA: no selector clears every attribute in the list. Reversing all
    // four first gives it something to clear, so a list that lost a member
    // leaves that member standing.
    const Ubiquitty::VT::AttributeSet clearedByDefault = after("\x1b[1;1;1;1$t\x1b[1;1;1;1$r");
    CHECK(clearedByDefault == Ubiquitty::VT::AttributeSet {});
}

TEST(everyRowHoldingAWideHalfCarriesTheMulticellFlag)
{
    int checked = 0;
    for (const Control& control : controls) {
        for (int leadColumn : { 0, 1, 10, 78 }) {
            for (int cursorColumn : { 0, 1, 10, 11, 78 }) {
                Ubiquitty::Terminal terminal;
                Ubiquitty::hardReset(terminal);
                feed(terminal, "\x1b[1;1H");
                char place[32];
                std::snprintf(place, sizeof(place), "\x1b[1;%dH", leadColumn + 1);
                feed(terminal, place);
                feed(terminal, wideCharacter);
                feed(terminal, "\x1b[2;1H");
                feed(terminal, wideCharacter);

                char sequence[64];
                std::snprintf(sequence, sizeof(sequence), control.format,
                    1, cursorColumn + 1, cursorColumn + 1, cursorColumn + 2);
                feed(terminal, sequence);

                for (int row = 0; row < terminal.rows; ++row) {
                    bool hasWideHalf = false;
                    for (int column = 0; column < terminal.columns; ++column) {
                        const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&terminal, 0, row, column);
                        if (!c)
                            continue;
                        if (Ubiquitty::cellPen(*c).attributes.containsAny(
                                { Ubiquitty::VT::Attribute::Wide, Ubiquitty::VT::Attribute::WideContinuation }))
                            hasWideHalf = true;
                    }
                    if (!hasWideHalf)
                        continue;
                    ++checked;
                    if (!terminal.grid->rowMeta(row).has(Ubiquitty::RowFlag::HasMulticell)) {
                        char message[256];
                        std::snprintf(message, sizeof(message),
                            "%s at lead=%d cursor=%d: row %d holds a wide half with no HasMulticell",
                            control.name, leadColumn, cursorColumn, row);
                        CHECK_MSG(false, message);
                    }
                }
            }
        }
    }
    CHECK(checked > 0); // a sweep that examined nothing proves nothing
}

/// A row that never held a wide cell must not claim it does — the hint is
/// allowed to be stale, but not to start out wrong.
TEST(aPlainRowCarriesNoMulticellFlag)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\x1b[1;1Hplain ascii only");
    CHECK(!terminal.grid->rowMeta(0).has(Ubiquitty::RowFlag::HasMulticell));

    feed(terminal, "\x1b[2;1H");
    feed(terminal, wideCharacter);
    CHECK(terminal.grid->rowMeta(1).has(Ubiquitty::RowFlag::HasMulticell));
    CHECK(!terminal.grid->rowMeta(0).has(Ubiquitty::RowFlag::HasMulticell));
}

UBIQUITTY_TEST_MAIN()
