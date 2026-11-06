// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_pane_activity.cpp — a pane's last output and last input are two
// instants, and a node above panes shows its newest child for each
// (Source/Core/Session/PaneActivity.h). The clock is the test's.

#include "Core/Session/PaneActivity.h"

#include <vector>

using Ubiquitty::Session::newestActivity;
using Ubiquitty::Session::PaneActivity;

TEST(outputAndInputAreStampedApart)
{
    PaneActivity pane;
    CHECK_MSG(pane.lastOutputUnixMilliseconds == 0 && pane.lastInputUnixMilliseconds == 0, "never, until something happens");
    pane.noteOutput(1'000);
    CHECK(pane.lastOutputUnixMilliseconds == 1'000);
    CHECK_MSG(pane.lastInputUnixMilliseconds == 0, "output is not input");
    pane.noteInput(2'500);
    CHECK(pane.lastInputUnixMilliseconds == 2'500);
    CHECK_MSG(pane.lastOutputUnixMilliseconds == 1'000, "input is not output");
    pane.noteOutput(4'000);
    CHECK(pane.lastOutputUnixMilliseconds == 4'000 && pane.lastInputUnixMilliseconds == 2'500);
    // The wall clock can be set back; a stamp is when the batch happened by
    // the clock as it stands, not the greatest value ever seen.
    pane.noteOutput(3'000);
    CHECK(pane.lastOutputUnixMilliseconds == 3'000);
}

TEST(aParentShowsItsNewestChildForEachInstant)
{
    CHECK(newestActivity({}) == PaneActivity {});
    const std::vector<PaneActivity> panes { { 100, 900 }, { 700, 0 }, { 300, 400 }, { 0, 0 } };
    const PaneActivity tab = newestActivity(panes);
    CHECK_MSG(tab.lastOutputUnixMilliseconds == 700 && tab.lastInputUnixMilliseconds == 900, "the newest output and the newest input come from different panes");

    // However the panes are grouped, the workspace shows the same.
    for (std::size_t tabSize = 1; tabSize <= panes.size(); ++tabSize) {
        std::vector<PaneActivity> tabs;
        for (std::size_t first = 0; first < panes.size(); first += tabSize) {
            const std::size_t last = std::min(panes.size(), first + tabSize);
            tabs.push_back(newestActivity({ panes.data() + first, last - first }));
        }
        CHECK(newestActivity(tabs) == tab);
    }
}

UBIQUITTY_TEST_MAIN()
