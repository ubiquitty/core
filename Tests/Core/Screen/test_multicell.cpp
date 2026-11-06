// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The position-first run query: what covers this cell, and does it claim it.
//
// The interesting cases are the ones where a run is not well formed, because
// that is what the query exists to let a caller assert against. An orphaned
// continuation must not resolve to a run that covers it, and an anchor whose
// continuation was overwritten must claim only what it still has.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Screen/Multicell.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

using Ubiquitty::Screen::MulticellRun;
using Ubiquitty::Screen::multicellRunAt;
using UbiquittyTest::feed;

namespace {

// A wide character, so the grid stores a lead plus one continuation.
const char* wideCharacter = "\xe6\x9d\xb1"; // U+6771

} // namespace

TEST(aWidePairResolvesToOneRunFromEitherHalf)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\x1b[1;3H");
    feed(t, wideCharacter);

    const MulticellRun fromLead = multicellRunAt(&t, 0, 2);
    CHECK(fromLead.isValid());
    CHECK(fromLead.isAnchor(0, 2));
    CHECK(fromLead.span.columns == 2);
    // A TRIPWIRE, not coverage. Multicell.cpp assigns span.rows = 1
    // unconditionally and nothing else writes the field, so this cannot fail
    // today and asserts nothing about a row dimension — the struct declares one
    // the storage does not have. It is here so that whoever makes a run span
    // rows finds a failing test naming the cases that need writing, rather than
    // a green suite that has never seen a multi-row run.
    CHECK(fromLead.span.rows == 1);
    CHECK(fromLead.covers(0, 2));
    CHECK(fromLead.covers(0, 3));
    CHECK(!fromLead.covers(0, 4));

    // Asked from the continuation, the same run — the position resolves to the
    // anchor rather than to the cell it landed on.
    const MulticellRun fromContinuation = multicellRunAt(&t, 0, 3);
    CHECK(fromContinuation.isValid());
    CHECK(fromContinuation.anchorColumn == 2);
    CHECK(!fromContinuation.isAnchor(0, 3));
    CHECK(fromContinuation.covers(0, 3));
}

TEST(aSingleWidthCellBelongsToNoRun)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\x1b[1;1Hplain");

    for (int column = 0; column < 5; ++column) {
        const MulticellRun run = multicellRunAt(&t, 0, column);
        CHECK(!run.isValid());
        CHECK(!run.covers(0, column));
    }
}

TEST(anOrphanedContinuationIsCoveredByNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\x1b[1;3H");
    feed(t, wideCharacter);
    CHECK(multicellRunAt(&t, 0, 3).covers(0, 3));

    // Overwrite the lead alone, leaving its continuation behind. The grid now
    // holds a half-pair, and the query must refuse to invent an anchor for it —
    // that refusal is what asking position-first is for.
    feed(t, "\x1b[1;3Hx");
    const MulticellRun orphan = multicellRunAt(&t, 0, 3);
    CHECK(!orphan.covers(0, 3));
}

TEST(theQueryRefusesPositionsOutsideTheScreen)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(!multicellRunAt(&t, -1, 0).isValid());
    CHECK(!multicellRunAt(&t, 0, -1).isValid());
    CHECK(!multicellRunAt(&t, t.rows, 0).isValid());
    CHECK(!multicellRunAt(&t, 0, t.columns).isValid());
    CHECK(!multicellRunAt(nullptr, 0, 0).isValid());
}

TEST(aPairAtColumnZeroResolvesWithoutWalkingOffTheRow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\x1b[1;1H");
    feed(t, wideCharacter);

    const MulticellRun run = multicellRunAt(&t, 0, 1);
    CHECK(run.isValid());
    CHECK(run.anchorColumn == 0);
    CHECK(run.span.columns == 2);
}

UBIQUITTY_TEST_MAIN()
