// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_tabshortcuts.cpp — the ⌘1..⌘9 tab keyboard-shortcut policy
// (Session/TabShortcuts.h), read by the menu builder (producer) and by tab
// selection (consumer). Pins:
//
//   - tabPositionHasShortcut: only 1-based positions 1..9 get a numbered shortcut.
//   - tabIndexForShortcut: position->index, including the ⌘9 wrap-to-last rule
//     when a window has more than nine tabs, and -1 for any non-existent tab.
//
// Header-only policy, links standalone.

#include "Core/Session/TabShortcuts.h"

#include <cstdio>

using Ubiquitty::Session::maxTabShortcutPosition;
using Ubiquitty::Session::tabIndexForShortcut;
using Ubiquitty::Session::tabPositionHasShortcut;

TEST(tabshortcuts)
{

    // ---- PRODUCER: which positions get a numbered shortcut ----
    CHECK(maxTabShortcutPosition == 9);
    CHECK(tabPositionHasShortcut(0) == false); // 0 is not a 1-based position
    CHECK(tabPositionHasShortcut(-1) == false);
    for (int p = 1; p <= 9; ++p)
        CHECK(tabPositionHasShortcut(p) == true);
    CHECK(tabPositionHasShortcut(10) == false);
    CHECK(tabPositionHasShortcut(100) == false);

    // ---- CONSUMER: position -> 0-based tab index ----

    // Eight tabs: ⌘1..⌘8 map straight through; ⌘9 is past the last tab, so it
    // clamps to the last (8th) tab rather than no-op'ing.
    CHECK(tabIndexForShortcut(1, 8) == 0);
    CHECK(tabIndexForShortcut(8, 8) == 7);
    CHECK(tabIndexForShortcut(9, 8) == 7); // past the last tab -> last tab

    // Exactly nine tabs: ⌘1..⌘9 map straight through 0..8 (count == 9).
    CHECK(tabIndexForShortcut(1, 9) == 0);
    CHECK(tabIndexForShortcut(9, 9) == 8); // the literal ninth tab

    // More than nine tabs: ⌘1..⌘8 still straight through, and ⌘9 stays the
    // dedicated "last tab" shortcut (index count-1) — the documented behaviour.
    CHECK(tabIndexForShortcut(1, 12) == 0);
    CHECK(tabIndexForShortcut(8, 12) == 7);
    CHECK(tabIndexForShortcut(9, 12) == 11); // ⌘9 = last (12th) tab
    CHECK(tabIndexForShortcut(9, 10) == 9); // ⌘9 = last (10th) tab
    CHECK(tabIndexForShortcut(9, 100) == 99); // ⌘9 = last (100th) tab

    // A position PAST the last tab clamps to the last tab (the reported fix):
    // three tabs, ⌘4/⌘5/…/⌘9 all land on the third (last) tab.
    CHECK(tabIndexForShortcut(4, 3) == 2);
    CHECK(tabIndexForShortcut(5, 3) == 2);
    CHECK(tabIndexForShortcut(9, 3) == 2);
    CHECK(tabIndexForShortcut(3, 2) == 1); // ⌘3 on two tabs -> last
    CHECK(tabIndexForShortcut(5, 1) == 0); // ⌘5 on one tab -> the only tab

    // Degenerate inputs: no tabs, or a non-positive position.
    CHECK(tabIndexForShortcut(1, 0) == -1);
    CHECK(tabIndexForShortcut(9, 0) == -1);
    CHECK(tabIndexForShortcut(0, 5) == -1);
    CHECK(tabIndexForShortcut(-1, 5) == -1);
}

// The label and the key are one pair: a tab that shows ⌘N is the tab ⌘N selects.
TEST(tabShortcutLabelAndKeySelectTheSameTab)
{
    using Ubiquitty::Session::tabIndexForShortcut;
    using Ubiquitty::Session::tabShortcutPositionForIndex;

    // Round trip: every labelled tab, at every count either side of nine, is
    // selected by the very shortcut it advertises.
    for (int tabCount = 1; tabCount <= 14; ++tabCount) {
        for (int index = 0; index < tabCount; ++index) {
            const int position = tabShortcutPositionForIndex(index, tabCount);
            if (position == 0)
                continue;
            CHECK_MSG(tabIndexForShortcut(position, tabCount) == index,
                "a labelled tab is the tab its own label selects");
        }
    }

    // Under ten tabs every position is labelled.
    CHECK(tabShortcutPositionForIndex(0, 5) == 1);
    CHECK(tabShortcutPositionForIndex(4, 5) == 5);
    CHECK(tabShortcutPositionForIndex(8, 9) == 9);

    // Past nine, the ninth through second-to-last are reachable by no shortcut,
    // so they carry no label; the last carries ⌘9.
    CHECK(tabShortcutPositionForIndex(7, 12) == 8);
    CHECK(tabShortcutPositionForIndex(8, 12) == 0);
    CHECK(tabShortcutPositionForIndex(10, 12) == 0);
    CHECK(tabShortcutPositionForIndex(11, 12) == 9);

    // Out of range names nothing.
    CHECK(tabShortcutPositionForIndex(-1, 5) == 0);
    CHECK(tabShortcutPositionForIndex(5, 5) == 0);
    CHECK(tabShortcutPositionForIndex(0, 0) == 0);
}

UBIQUITTY_TEST_MAIN()
