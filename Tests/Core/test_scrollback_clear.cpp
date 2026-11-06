// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Scrollback.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

#include <cstdio>
#include <cstring>

// The two scrollback clears are not the same operation, and their names once
// differed only in word order. clearScrollback carries the full policy —
// including re-basing pendingPromptRow, since the live screen stays put while
// history drops. resetScrollbackStore is the bare ring reset the sync/teleport
// path wants, because it repopulates from a snapshot carrying its own prompt
// state and re-basing against history about to be replaced would be wrong.

// clearScrollback carries the full policy: it re-bases pendingPromptRow, because
// the live screen stays put while history drops, so a prompt awaiting its command
// keeps its row but loses the dropped history from its absolute index.
TEST(test_clearScrollback_rebases_pending_prompt)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);

    // Push rows into history, then open a prompt that never gets its C marker.
    feed(t, "a\r\nb\r\nc\r\nd\r\ne\r\n");
    feed(t, "\033]133;A\007$ ");
    const int historyBefore = Ubiquitty::Screen::scrollbackCount(&t);
    CHECK(historyBefore > 0);
    CHECK(t.pendingPromptRow >= historyBefore);

    Ubiquitty::Screen::dropScrollback(&t);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    // Re-based by exactly the history that was dropped, not reset and not left stale.
    CHECK(t.pendingPromptRow >= 0);
    CHECK(t.pendingPromptRow < t.rows);
}

// resetScrollbackStore is the ring reset without that policy — the sync/teleport
// path clears and immediately repopulates from a snapshot carrying its own prompt
// state, so re-basing against history about to be replaced would be wrong.
TEST(test_resetScrollbackStore_leaves_pending_prompt_alone)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);

    feed(t, "a\r\nb\r\nc\r\nd\r\ne\r\n");
    feed(t, "\033]133;A\007$ ");
    const int promptBefore = t.pendingPromptRow;
    CHECK(Ubiquitty::Screen::scrollbackCount(&t) > 0);

    Ubiquitty::Screen::resetScrollbackStore(&t);

    CHECK(Ubiquitty::Screen::scrollbackCount(&t) == 0);
    CHECK(t.pendingPromptRow == promptBefore); // untouched, unlike clearScrollback
}

UBIQUITTY_TEST_MAIN()
