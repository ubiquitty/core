// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Text written inside DECLRMM margins lands where the per-character path puts
// it, however many bytes arrive at once.
//
// The batched writers take a run only when the whole run fits before the right
// margin, and they arm the deferred wrap at that margin rather than at the
// screen edge. Both are decisions the per-character path also makes, one byte
// at a time, so the two have to agree cell for cell and on where the cursor
// stops — and the run length is what picks between them, since the batch has a
// minimum of two.
//
// This is the shape tmux drives: it turns DECLRMM on for side-by-side panes and
// cursor-addresses every row, so a margined row carrying one long run is the
// ordinary case rather than an exotic one.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/CellGrid.h"
#include "Core/Screen/Margins.h"
#include "Core/TerminalOperations.h"

#include <cstring>
#include <string>

using UbiquittyTest::feed;

namespace {

const Ubiquitty::Cell* cellAt(Ubiquitty::Terminal& t, int row, int column)
{
    return t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
}

// Feeds `text` twice under the same margins — once byte by byte, once whole —
// and reports whether the two rows agree, cursor and wrap flag included.
void checkBatchMatchesPerCharacter(const char* setup, int startColumn, const char* text)
{
    const int count = static_cast<int>(std::strlen(text));

    Ubiquitty::Terminal perCharacter;
    Ubiquitty::hardReset(perCharacter);
    feed(perCharacter, setup);
    feed(perCharacter, ("\033[1;" + std::to_string(startColumn + 1) + "H").c_str());
    for (int index = 0; index < count; ++index) {
        const char single[2] = { text[index], '\0' };
        feed(perCharacter, single);
    }

    Ubiquitty::Terminal batched;
    Ubiquitty::hardReset(batched);
    feed(batched, setup);
    feed(batched, ("\033[1;" + std::to_string(startColumn + 1) + "H").c_str());
    feed(batched, text);

    for (int column = 0; column < perCharacter.columns; ++column) {
        const Ubiquitty::Cell* expected = cellAt(perCharacter, 0, column);
        const Ubiquitty::Cell* actual = cellAt(batched, 0, column);
        CHECK(expected != nullptr && actual != nullptr);
        CHECK(expected->codePoint == actual->codePoint);
    }
    CHECK(perCharacter.screens.active->cursorX == batched.screens.active->cursorX);
    CHECK(perCharacter.screens.active->cursorY == batched.screens.active->cursorY);
    CHECK(perCharacter.screens.active->wrapPending == batched.screens.active->wrapPending);
}

// DECLRMM on with a genuinely narrower region, as a split pane leaves it.
const char* const narrowedMargins = "\033[?69h\033[11;40s";

} // namespace

TEST(marginedTextBatchMatchesPerCharacter)
{
    // Well inside the region.
    checkBatchMatchesPerCharacter(narrowedMargins, 10, "hello margins");
    // Ending exactly on the right margin, which arms the deferred wrap.
    checkBatchMatchesPerCharacter(narrowedMargins, 10, "0123456789012345678901234567890");
    // Longer than the region, so the run has to stop at the margin and the
    // remainder wraps.
    checkBatchMatchesPerCharacter(narrowedMargins, 10, "0123456789012345678901234567890abcdefghij");
    // Starting on the last column of the region.
    checkBatchMatchesPerCharacter(narrowedMargins, 38, "xyz");
    // A two-byte run is the batch's minimum; one byte below it is not a batch.
    checkBatchMatchesPerCharacter(narrowedMargins, 10, "ab");
    checkBatchMatchesPerCharacter(narrowedMargins, 10, "a");
    // The cursor parked OUTSIDE the region, both sides.
    checkBatchMatchesPerCharacter(narrowedMargins, 2, "outside left");
    checkBatchMatchesPerCharacter(narrowedMargins, 60, "outside right");
    // A region pinned to the left edge, and one pinned to the right.
    checkBatchMatchesPerCharacter("\033[?69h\033[1;20s", 0, "left edge region");
    checkBatchMatchesPerCharacter("\033[?69h\033[60;80s", 62, "right edge region");
    // The mode on but the margins spanning the whole width: the region is not
    // narrowed, and the answer must not change either.
    checkBatchMatchesPerCharacter("\033[?69h\033[1;80s", 10, "full width region");
    // No margins at all, the baseline the rest is measured against.
    checkBatchMatchesPerCharacter("\033[?69l", 10, "no margins here");
}

TEST(marginedTextBatchStopsAtTheRightMargin)
{
    // The clamp itself, stated directly rather than through the comparison: a
    // run longer than the region leaves the columns past the margin untouched.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, narrowedMargins);
    feed(t, "\033[1;11H");
    feed(t, "0123456789012345678901234567890123456789");

    CHECK(Ubiquitty::Screen::effectiveRight(t) == 39);
    CHECK(cellAt(t, 0, 10)->codePoint == '0');
    CHECK(cellAt(t, 0, 39)->codePoint != 0); // the margin column carries the run
    CHECK(cellAt(t, 0, 40)->codePoint == 0); // one past it does not
    CHECK(cellAt(t, 0, 79)->codePoint == 0);
}

UBIQUITTY_TEST_MAIN()
