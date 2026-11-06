// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Core/Screen/PromptNavigation.h — the Cmd-Shift-Up / Cmd-Shift-Down prompt-navigation
// viewport state machine: one pure, host-testable Core function every frontend
// calls.
//
// The function reads OSC 133 marks through a provider, so these tests drive it
// over a SCRIPTED mark layout (a flat per-absolute-row mark array) with no real
// Terminal: every case pins the {newViewOffset, newAnchor, action} the frontends
// then apply.
#include "Core/Screen/PromptNavigation.h"
#include "Core/VT/Constants.h"
#include <cstdio>
#include <vector>

using Ubiquitty::Screen::PromptStepAction;
using Ubiquitty::Screen::PromptStepResult;
using Ubiquitty::Screen::stepToPrompt;

// A scripted mark layout: mark bits per absolute row. Rows outside the array
// read 0 (the same contract Terminal::rowMark honours for out-of-range rows).
struct MarkLayout {
    std::vector<uint8_t> marks;
};

static uint8_t layoutMarkAt(void* context, int absoluteRow)
{
    const MarkLayout* l = static_cast<const MarkLayout*>(context);
    if (absoluteRow < 0 || absoluteRow >= (int)l->marks.size())
        return 0;
    return l->marks[absoluteRow];
}

// Convenience: build a layout where the given absolute rows carry CMD_EXECUTED.
static MarkLayout withExecutedRows(int total, std::initializer_list<int> rows)
{
    MarkLayout l;
    l.marks.assign(total, 0);
    for (int r : rows)
        if (r >= 0 && r < total)
            l.marks[r] = Ubiquitty::Screen::osc133MarkCmdExecuted;
    return l;
}

static Ubiquitty::Screen::PromptStepResult step(const MarkLayout& l, int history, int rows, int viewOffset, int anchor, int direction)
{
    // const_cast: the provider takes a void* but only reads.
    return Ubiquitty::Screen::stepToPrompt(history, rows, viewOffset, anchor, direction, layoutMarkAt, const_cast<MarkLayout*>(&l), Ubiquitty::Screen::osc133MarkCmdExecuted);
}

// Down at the live tail (viewOffset 0) rings the bell and moves nothing —
// regardless of the anchor or what's above.
TEST(testDownAtTailBells)
{
    // 10-row buffer (history 4, 6 visible), prompts at rows 0,2,5.
    MarkLayout l = withExecutedRows(10, { 0, 2, 5 });
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 4, /*rows*/ 6, /*viewOffset*/ 0,
        /*anchor*/ -1, /*direction*/ +1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Bell);
    CHECK(r.newViewOffset == 0);
    printf("testDownAtTailBells done\n");
}

// From a fresh view (anchor -1) scrolled back, Up lands on the bottom-most
// prompt visible at/above the viewport bottom, placed one row below the top.
TEST(testUpFreshViewBottommost)
{
    // history 8, 6 visible. viewOffset 0 → topAbsoluteRow=8, bottomAbsoluteRow=13. Prompts at
    // 3,7,11. Fresh Up searches at/below bottomAbsoluteRow+1=14 exclusive → row 11.
    MarkLayout l = withExecutedRows(14, { 3, 7, 11 });
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 8, /*rows*/ 6, /*viewOffset*/ 0,
        /*anchor*/ -1, /*direction*/ -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 11);
    // offset = history - target + 1 = 8 - 11 + 1 = -2, clamped to 0.
    CHECK(r.newViewOffset == 0);
    printf("testUpFreshViewBottommost done\n");
}

// Repeated Up steps prompt-by-prompt above the anchor (the previous landed row),
// even though that anchor prompt is still on screen.
TEST(testUpStepsByAnchor)
{
    // history 20, 6 visible. Prompts at 3, 7, 11, 15.
    MarkLayout l = withExecutedRows(26, { 3, 7, 11, 15 });
    // Anchor on row 15, viewOffset positioned so the anchor is visible:
    // offset = history - 15 + 1 = 6 → topAbsoluteRow = 20 - 6 = 14, bottomAbsoluteRow = 19.
    // 15 is in [14,19] and marked → anchorVisible. Up searches strictly above
    // 15 → finds 11.
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 20, /*rows*/ 6, /*viewOffset*/ 6,
        /*anchor*/ 15, /*direction*/ -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 11);
    CHECK(r.newViewOffset == 20 - 11 + 1); // 10
    // Step again from the new anchor 11 (now visible at offset 10) → 7.
    Ubiquitty::Screen::PromptStepResult r2 = step(l, 20, 6, /*viewOffset*/ 10, /*anchor*/ 11, -1);
    CHECK(r2.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r2.newAnchor == 7);
    CHECK(r2.newViewOffset == 20 - 7 + 1); // 14
    printf("testUpStepsByAnchor done\n");
}

// Up with no prompt above the search ceiling rings the bell and leaves the
// viewport put (no SnapToTail going up).
TEST(testUpNoPromptAboveBells)
{
    // history 10, 6 visible. Only prompt is row 12 (below the view). From a
    // fresh view at offset 4: topAbsoluteRow=6, bottomAbsoluteRow=11 → bottomAbsoluteRow+1=12 exclusive,
    // so the Up ceiling is row 11; nothing marked at/below 11 going up.
    MarkLayout l = withExecutedRows(16, { 12 });
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 10, /*rows*/ 6, /*viewOffset*/ 4,
        /*anchor*/ -1, /*direction*/ -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Bell);
    CHECK(r.newViewOffset == 4); // unchanged
    printf("testUpNoPromptAboveBells done\n");
}

// Down steps to the next prompt below the anchor and lands it one row below the
// top.
TEST(testDownStepsToNext)
{
    MarkLayout l = withExecutedRows(26, { 3, 7, 11, 15 });
    // Anchor 7 visible at offset = 20 - 7 + 1 = 14 → topAbsoluteRow=6, bottomAbsoluteRow=11.
    // 7 in [6,11], marked → anchorVisible. Down searches strictly below 7 → 11.
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 20, /*rows*/ 6, /*viewOffset*/ 14,
        /*anchor*/ 7, /*direction*/ +1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 11);
    CHECK(r.newViewOffset == 20 - 11 + 1); // 10
    printf("testDownStepsToNext done\n");
}

// Down with nothing below but history left to unwind snaps to the live tail and
// forgets the anchor.
TEST(testDownNoPromptSnapsToTail)
{
    // history 20, 6 visible. Prompts at 3 and 7 only; both above the view.
    MarkLayout l = withExecutedRows(26, { 3, 7 });
    // viewOffset 14 → topAbsoluteRow=6, bottomAbsoluteRow=11; anchor 7 visible. Down searches
    // strictly below 7 over [8, 26) — none marked → SnapToTail.
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 20, /*rows*/ 6, /*viewOffset*/ 14,
        /*anchor*/ 7, /*direction*/ +1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::SnapToTail);
    CHECK(r.newViewOffset == 0);
    CHECK(r.newAnchor == -1);
    printf("testDownNoPromptSnapsToTail done\n");
}

// A stale anchor (off-screen, or no longer marked) is ignored: the search
// re-derives from the viewport edges. Here the anchor row is below the view, so
// Up falls back to the bottom-most visible prompt.
TEST(testStaleAnchorRedirivesFromEdges)
{
    MarkLayout l = withExecutedRows(20, { 2, 5, 9 });
    // viewOffset 0, history 6 → topAbsoluteRow=6, bottomAbsoluteRow=11. Anchor 99 is off-screen
    // → ignored. Fresh Up ceiling = bottomAbsoluteRow+1 = 12 exclusive → finds row 9.
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 6, /*rows*/ 6, /*viewOffset*/ 0,
        /*anchor*/ 99, /*direction*/ -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 9);
    printf("testStaleAnchorRedirivesFromEdges done\n");
}

// An anchor that's on screen but no longer carries the mark (e.g. the prompt was
// cleared) is treated as stale too.
TEST(testUnmarkedAnchorIsStale)
{
    // Prompts at 5 and 9 only; anchor 7 is on screen but unmarked.
    MarkLayout l = withExecutedRows(20, { 5, 9 });
    // viewOffset 0, history 6 → topAbsoluteRow=6, bottomAbsoluteRow=11. Anchor 7 in range but
    // not marked → not anchorVisible. Fresh Up ceiling 12 exclusive → row 9
    // (an on-screen prompt below the stale anchor still counts).
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 6, /*rows*/ 6, /*viewOffset*/ 0,
        /*anchor*/ 7, /*direction*/ -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 9);
    printf("testUnmarkedAnchorIsStale done\n");
}

// The landing offset is clamped into [0, history]: a prompt deep in scrollback
// can't push the offset past the available history.
TEST(testOffsetClampedToHistory)
{
    // history 3, 6 visible. Prompt at absolute row 0 (oldest scrollback row).
    MarkLayout l = withExecutedRows(9, { 0 });
    // viewOffset 3 (fully scrolled back) → topAbsoluteRow=0, bottomAbsoluteRow=5; row 0 visible
    // but we pass a fresh anchor. Fresh Up ceiling = 6 exclusive → finds row 0.
    // offset = 3 - 0 + 1 = 4, clamped to history 3.
    Ubiquitty::Screen::PromptStepResult r = step(l, /*history*/ 3, /*rows*/ 6, /*viewOffset*/ 3,
        /*anchor*/ -1, /*direction*/ -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 0);
    CHECK(r.newViewOffset == 3); // clamped to history, not 4
    printf("testOffsetClampedToHistory done\n");
}

// End-to-end walk: from the live tail, Up steps up through every prompt, then
// Down steps back down and finally snaps to the tail — the canonical
// Cmd-Shift-Up×N then Cmd-Shift-Down×N gesture.
TEST(testFullUpThenDownWalk)
{
    // history 12, 6 visible. Prompts at 1, 4, 8.
    const int history = 12, rows = 6;
    MarkLayout l = withExecutedRows(18, { 1, 4, 8 });
    int viewOffset = 0, anchor = -1;

    // Up #1: fresh view topAbsoluteRow=12,bottomAbsoluteRow=17 → ceiling 18 → row 8.
    Ubiquitty::Screen::PromptStepResult r = step(l, history, rows, viewOffset, anchor, -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 8);
    viewOffset = r.newViewOffset;
    anchor = r.newAnchor; // offset 12-8+1=5
    CHECK(viewOffset == 5);

    // Up #2: anchor 8 visible (topAbsoluteRow=7) → above 8 → row 4.
    r = step(l, history, rows, viewOffset, anchor, -1);
    CHECK(r.newAnchor == 4);
    viewOffset = r.newViewOffset;
    anchor = r.newAnchor; // 12-4+1=9
    CHECK(viewOffset == 9);

    // Up #3: → row 1.
    r = step(l, history, rows, viewOffset, anchor, -1);
    CHECK(r.newAnchor == 1);
    viewOffset = r.newViewOffset;
    anchor = r.newAnchor; // 12-1+1=12
    CHECK(viewOffset == 12);

    // Up #4: nothing above row 1 → bell, viewport unchanged.
    r = step(l, history, rows, viewOffset, anchor, -1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Bell);
    CHECK(r.newViewOffset == 12);

    // Down #1: anchor 1 visible → below 1 → row 4.
    r = step(l, history, rows, viewOffset, anchor, +1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Scroll);
    CHECK(r.newAnchor == 4);
    viewOffset = r.newViewOffset;
    anchor = r.newAnchor;

    // Down #2: → row 8.
    r = step(l, history, rows, viewOffset, anchor, +1);
    CHECK(r.newAnchor == 8);
    viewOffset = r.newViewOffset;
    anchor = r.newAnchor;

    // Down #3: nothing below row 8 → snap to tail, forget anchor.
    r = step(l, history, rows, viewOffset, anchor, +1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::SnapToTail);
    CHECK(r.newViewOffset == 0);
    CHECK(r.newAnchor == -1);
    viewOffset = r.newViewOffset;
    anchor = r.newAnchor;

    // Down #4: now at the tail → bell.
    r = step(l, history, rows, viewOffset, anchor, +1);
    CHECK(r.action == Ubiquitty::Screen::PromptStepAction::Bell);
    printf("testFullUpThenDownWalk done\n");
}

UBIQUITTY_TEST_MAIN()
