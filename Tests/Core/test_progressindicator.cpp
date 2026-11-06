// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_progressindicator.cpp — host coverage for the portable OSC 9;4
// progress-indicator presentation policy (Source/Core/OSC/ProgressIndicator.h).
//
// Compiles against bare libc++ — no platform include. Pins the decisions both
// frontends share: which slot lights up (content bar vs. tab button bar vs.
// per-pane bars), how a bar is tinted (accent / inactive-gray / error-red by
// focus + window activity, error never dimmed), and how per-pane progress folds
// into one value for a whole-window slot (error > indeterminate > normal >
// paused; the least-complete determinate value).

#include "Core/OSC/ProgressIndicator.h"

#include <cstdio>
#include <vector>

using namespace Ubiquitty::OSC; // ProgressState — the wire enum the policy ranks

static ProgressSample aggregate(const std::vector<ProgressSample>& samples) { return aggregateProgress(samples); }

TEST(progressindicator)
{

    // --- progressRank: the aggregation precedence ---------------------------
    CHECK(progressRank(ProgressError) > progressRank(ProgressIndeterminate));
    CHECK(progressRank(ProgressIndeterminate) > progressRank(ProgressNormal));
    CHECK(progressRank(ProgressNormal) > progressRank(ProgressPaused));
    CHECK(progressRank(ProgressPaused) > progressRank(ProgressNone));
    CHECK(progressRank(ProgressNone) == 0);

    // --- progressTint: error is never dimmed --------------------------------
    // Error reads red regardless of focus / activity / split.
    CHECK(progressTint(ProgressError, PaneFocusState::Focused, PaneLayout::SinglePane, WindowActivityState::Active) == ProgressTint::Error);
    CHECK(progressTint(ProgressError, PaneFocusState::Unfocused, PaneLayout::MultiPane, WindowActivityState::Background) == ProgressTint::Error);

    // Lone window / single pane (multiPane=false, paneFocused=true) tracks window
    // activity alone.
    CHECK(progressTint(ProgressNormal, PaneFocusState::Focused, PaneLayout::SinglePane, WindowActivityState::Active) == ProgressTint::Accent);
    CHECK(progressTint(ProgressNormal, PaneFocusState::Focused, PaneLayout::SinglePane, WindowActivityState::Background) == ProgressTint::Inactive);
    CHECK(progressTint(ProgressIndeterminate, PaneFocusState::Focused, PaneLayout::SinglePane, WindowActivityState::Active) == ProgressTint::Accent);

    // Split: the focused pane of the active window is accent; an unfocused pane
    // dims to gray even though the window is active.
    CHECK(progressTint(ProgressNormal, PaneFocusState::Focused, PaneLayout::MultiPane, WindowActivityState::Active) == ProgressTint::Accent);
    CHECK(progressTint(ProgressNormal, PaneFocusState::Unfocused, PaneLayout::MultiPane, WindowActivityState::Active) == ProgressTint::Inactive);
    // Backgrounded window dims every pane, focused or not.
    CHECK(progressTint(ProgressNormal, PaneFocusState::Focused, PaneLayout::MultiPane, WindowActivityState::Background) == ProgressTint::Inactive);
    CHECK(progressTint(ProgressNormal, PaneFocusState::Unfocused, PaneLayout::MultiPane, WindowActivityState::Background) == ProgressTint::Inactive);

    // --- contentBarVisible: lone unsplit untabbed window only ---------------
    CHECK(contentBarVisible(ProgressNormal, TabLayout::SingleTab, SplitLayout::Unsplit));
    CHECK(!contentBarVisible(ProgressNone, TabLayout::SingleTab, SplitLayout::Unsplit)); // no progress
    CHECK(!contentBarVisible(ProgressNormal, TabLayout::Tabbed, SplitLayout::Unsplit)); // tabbed → tab chrome
    CHECK(!contentBarVisible(ProgressNormal, TabLayout::SingleTab, SplitLayout::Split)); // split → pane bars
    CHECK(!contentBarVisible(ProgressNormal, TabLayout::Tabbed, SplitLayout::Split));

    // --- tabButtonBarVisible: selected tab of a multi-tab group, unsplit -----
    CHECK(tabButtonBarVisible(ProgressNormal, TabSelectionState::Active, TabLayout::Tabbed, SplitLayout::Unsplit));
    CHECK(!tabButtonBarVisible(ProgressNormal, TabSelectionState::Inactive, TabLayout::Tabbed, SplitLayout::Unsplit)); // inactive tab → ring
    CHECK(!tabButtonBarVisible(ProgressNormal, TabSelectionState::Active, TabLayout::SingleTab, SplitLayout::Unsplit)); // lone window → content bar
    CHECK(!tabButtonBarVisible(ProgressNormal, TabSelectionState::Active, TabLayout::Tabbed, SplitLayout::Split)); // split → pane bars
    CHECK(!tabButtonBarVisible(ProgressNone, TabSelectionState::Active, TabLayout::Tabbed, SplitLayout::Unsplit)); // no progress

    // --- aggregateProgress: rank wins, determinate takes least-complete ------
    CHECK(aggregate({}).state == ProgressNone);
    CHECK(aggregate({ { ProgressNone, 0 }, { ProgressNone, 0 } }).state == ProgressNone);

    // Error beats everything.
    {
        ProgressSample r = aggregate({ { ProgressNormal, 50 }, { ProgressError, 0 }, { ProgressIndeterminate, 0 } });
        CHECK(r.state == ProgressError);
        CHECK(r.value == 0); // non-determinate result carries no value
    }
    // Indeterminate beats determinate/paused.
    CHECK(aggregate({ { ProgressNormal, 90 }, { ProgressIndeterminate, 0 } }).state == ProgressIndeterminate);

    // All determinate → least-complete value (ring is "done" only when all are).
    {
        ProgressSample r = aggregate({ { ProgressNormal, 80 }, { ProgressNormal, 30 }, { ProgressNormal, 60 } });
        CHECK(r.state == ProgressNormal);
        CHECK(r.value == 30);
    }
    // Paused panes don't contribute a determinate value.
    {
        ProgressSample r = aggregate({ { ProgressPaused, 10 }, { ProgressNormal, 70 } });
        CHECK(r.state == ProgressNormal);
        CHECK(r.value == 70);
    }
    // Only paused → paused, value 0.
    {
        ProgressSample r = aggregate({ { ProgressPaused, 40 }, { ProgressNone, 0 } });
        CHECK(r.state == ProgressPaused);
        CHECK(r.value == 0);
    }
}

UBIQUITTY_TEST_MAIN()
