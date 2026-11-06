// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspace_normalize.cpp — normalizeWorkspace turns a decoded-but-possibly-
// nonsense workspace (the bounds-safe decoder accepts NaN / out-of-range ratios
// and activeTab) into a GUI-safe one: finite ratios in [0,1], activeTab in range.

#include "Core/Session/Workspace.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace Ubiquitty::Session;

namespace {
void check(bool ok, const char* what)
{
    CHECK_MSG(ok, what);
}

Ubiquitty::Transport::Origin local() { return { Ubiquitty::Transport::OriginKind::Local, "" }; }

PaneNode split(double ratio, PaneNode a, PaneNode b) { return PaneNode::makeSplit(false, ratio, std::move(a), std::move(b)); }
PaneNode leaf() { return PaneNode::makeLeaf(local(), "x"); }

Workspace oneTab(PaneNode root, int activeTab, int tabCount)
{
    Workspace ws;
    WorkspaceWindow window;
    for (int i = 0; i < tabCount; ++i) {
        WorkspaceTab tab;
        tab.root = (i == 0) ? root : leaf();
        window.tabs.push_back(tab);
    }
    window.activeTab = activeTab;
    ws.windows = { window };
    return ws;
}

TEST(test_nan_ratio_becomes_half)
{
    Workspace ws = oneTab(split(std::numeric_limits<double>::quiet_NaN(), leaf(), leaf()), 0, 1);
    normalizeWorkspace(ws);
    const double r = ws.windows[0].tabs[0].root.ratio;
    check(std::isfinite(r) && r == 0.5, "NaN ratio normalized to 0.5");
}

TEST(test_infinite_ratio_becomes_half)
{
    Workspace ws = oneTab(split(std::numeric_limits<double>::infinity(), leaf(), leaf()), 0, 1);
    normalizeWorkspace(ws);
    check(ws.windows[0].tabs[0].root.ratio == 0.5, "infinite ratio normalized to 0.5");
}

TEST(test_out_of_range_ratio_is_clamped)
{
    Workspace high = oneTab(split(1.8, leaf(), leaf()), 0, 1);
    normalizeWorkspace(high);
    check(high.windows[0].tabs[0].root.ratio == 1.0, "ratio > 1 clamped to 1");

    Workspace low = oneTab(split(-0.3, leaf(), leaf()), 0, 1);
    normalizeWorkspace(low);
    check(low.windows[0].tabs[0].root.ratio == 0.0, "ratio < 0 clamped to 0");

    Workspace ok = oneTab(split(0.42, leaf(), leaf()), 0, 1);
    normalizeWorkspace(ok);
    check(ok.windows[0].tabs[0].root.ratio == 0.42, "in-range ratio is untouched");
}

TEST(test_nested_ratios_are_normalized)
{
    Workspace ws = oneTab(split(2.0, leaf(), split(std::numeric_limits<double>::quiet_NaN(), leaf(), leaf())), 0, 1);
    normalizeWorkspace(ws);
    const PaneNode& root = ws.windows[0].tabs[0].root;
    check(root.ratio == 1.0, "outer ratio clamped");
    check(root.children[1].ratio == 0.5, "nested NaN ratio normalized");
}

TEST(test_active_tab_is_clamped)
{
    Workspace tooHigh = oneTab(leaf(), /*activeTab*/ 9, /*tabCount*/ 3);
    normalizeWorkspace(tooHigh);
    check(tooHigh.windows[0].activeTab == 2, "activeTab past the end clamps to last");

    Workspace negative = oneTab(leaf(), -5, 3);
    normalizeWorkspace(negative);
    check(negative.windows[0].activeTab == 0, "negative activeTab clamps to 0");

    Workspace noTabs = oneTab(leaf(), 4, 0);
    normalizeWorkspace(noTabs);
    check(noTabs.windows[0].activeTab == 0, "activeTab is 0 for a window with no tabs");
}

TEST(presentationSelectionsAreBounded)
{
    Workspace workspace = oneTab(split(0.4, leaf(), leaf()), 0, 1);
    workspace.activeWindow = 999;
    workspace.windows[0].zoomedPaneIndex = 999;
    workspace.windows[0].tabs[0].focusedPaneIndex = -9;
    normalizeWorkspace(workspace);
    CHECK_EQ(workspace.activeWindow, 0);
    CHECK_EQ(workspace.windows[0].zoomedPaneIndex, -1);
    CHECK_EQ(workspace.windows[0].tabs[0].focusedPaneIndex, 0);
    workspace.windows[0].zoomedPaneIndex = 1;
    workspace.windows[0].tabs[0].focusedPaneIndex = 99;
    normalizeWorkspace(workspace);
    CHECK_EQ(workspace.windows[0].zoomedPaneIndex, 1);
    CHECK_EQ(workspace.windows[0].tabs[0].focusedPaneIndex, 1);
}

} // namespace

UBIQUITTY_TEST_MAIN()
