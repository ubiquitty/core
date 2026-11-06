// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_session_tree.cpp — the tree of everything a host holds, built from
// hand-made layouts: one pane, a tab of splits, several tabs, several windows
// and workspaces, a zoomed pane, and a workspace of nothing. Each case
// asserts the structure and every roll-up above the panes.
//

#include "Harness.h"

#include "Core/App/StatusRollup.h"
#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Session/Identity.h"
#include "Core/Session/IllFormedTree.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/PaneActivity.h"
#include "Core/Session/SessionRows.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/StatedPane.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Ubiquitty::App::RolledStatus;
using Ubiquitty::App::StatusKind;
using Ubiquitty::Panes::Orientation;
using Ubiquitty::Protocol::Sync::SplitAxis;
using Ubiquitty::Protocol::Sync::TopologyNode;
using Ubiquitty::Session::buildSessionTree;
using Ubiquitty::Session::PaneActivity;
using Ubiquitty::Session::SessionTree;
using Ubiquitty::Session::TreePanePath;
using Ubiquitty::Session::TreeTab;
using Ubiquitty::Session::TreeWindow;
using Ubiquitty::Session::TreeWorkspace;
using Ubiquitty::Session::TreeWorkspaceSource;
using Ubiquitty::Session::Identity::Allocator;
using Ubiquitty::Session::Identity::LabelBook;
using Ubiquitty::Session::Identity::ObjectID;
using Ubiquitty::Session::Identity::ObjectKind;
namespace Layout = Ubiquitty::Session::Layout;
namespace OSC = Ubiquitty::OSC;

namespace {

/// The zoom a window holds on one pane of one of its tabs.
std::optional<Ubiquitty::Session::TreeZoom> zoomAt(std::size_t tab, std::size_t pane)
{
    return Ubiquitty::Session::TreeZoom { .tab = tab, .pane = pane };
}

/// What a test says about a pane; the tree is built from the records a host
/// would send for it (StatedPane.h).
using TreePaneFacts = UbiquittyTest::StatedPane;
using UbiquittyTest::sameColor;
using UbiquittyTest::sameStatus;

/// A host's panes as a test states them: facts by pane id, and what was asked.
struct Panes {
    std::map<std::uint32_t, TreePaneFacts> facts;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> asked;

    TreePaneFacts& operator[](Layout::PaneID pane) { return facts[std::to_underlying(pane)]; }

    auto provider()
    {
        return [this](std::uint32_t workspaceID, Layout::PaneID pane) {
            asked.emplace_back(workspaceID, std::to_underlying(pane));
            return facts[std::to_underlying(pane)].treeFacts();
        };
    }
};

/// A shell at its prompt.
TreePaneFacts idle()
{
    TreePaneFacts facts;
    facts.status.isAtPrompt = true;
    return facts;
}

/// A program whose status record says `state`.
TreePaneFacts reporting(OSC::ProgramState state)
{
    TreePaneFacts facts;
    facts.status.programState = state;
    return facts;
}

TreePaneFacts at(TreePaneFacts facts, std::uint64_t output, std::uint64_t input)
{
    facts.activity = { output, input };
    return facts;
}

std::vector<std::uint32_t> paneIDs(const TreeTab& tab)
{
    std::vector<std::uint32_t> ids;
    for (const auto& pane : tab.panes)
        ids.push_back(std::to_underlying(pane.id));
    return ids;
}

/// Every pane under a workspace folded in one pass, with no levels between.
RolledStatus flatStatus(const TreeWorkspace& workspace)
{
    std::vector<RolledStatus> statuses;
    for (const auto& window : workspace.windows) {
        for (const auto& tab : window.tabs) {
            for (const auto& pane : tab.panes)
                statuses.push_back(pane.status);
        }
    }
    return Ubiquitty::App::rollUpStatus(statuses);
}

constexpr Ubiquitty::ColorRGBA red { 200, 30, 30, 255 };
constexpr Ubiquitty::ColorRGBA blue { 30, 30, 200, 255 };

} // namespace

TEST(anUnnamedWorkspaceIsCalledByTheFolderOfThePaneInFront)
{
    using Ubiquitty::Session::TreePane;
    using Ubiquitty::Session::TreeTab;
    using Ubiquitty::Session::TreeWindow;
    using Ubiquitty::Session::TreeWorkspace;
    const auto at = [](std::string directory, std::string home = {}) {
        TreePane pane;
        pane.title.currentDirectory = std::move(directory);
        pane.title.homeDirectory = std::move(home);
        return pane;
    };
    TreeWorkspace workspace;
    CHECK_MSG(workspace.defaultName().empty(), "a workspace with no windows has no folder to be called by");

    TreeTab tab;
    tab.panes = { at("/srv/api"), at("/srv/deploy/assets/") };
    TreeWindow window;
    window.tabs = { tab };
    workspace.windows = { window };
    CHECK_STR_EQ(workspace.defaultName(), "api");
    // The pane the tab types into, with a trailing slash dropped.
    workspace.windows[0].tabs[0].hostFocusedPane = 1;
    CHECK_STR_EQ(workspace.defaultName(), "assets");
    // At home, "~", as a tab says; below it, the folder; the root is "/".
    workspace.windows[0].tabs[0].panes = { at("/home/user", "/home/user") };
    workspace.windows[0].tabs[0].hostFocusedPane = 0;
    CHECK_STR_EQ(workspace.defaultName(), "~");
    workspace.windows[0].tabs[0].panes = { at("/home/user/notes", "/home/user") };
    CHECK_STR_EQ(workspace.defaultName(), "notes");
    workspace.windows[0].tabs[0].panes = { at("/") };
    CHECK_STR_EQ(workspace.defaultName(), "/");
    // The window in front and its tab in front decide, not the first.
    TreeTab other;
    other.panes = { at("/var/log/release") };
    TreeWindow second;
    second.tabs = { tab, other };
    second.hostActiveTab = 1;
    workspace.windows.push_back(second);
    workspace.hostActiveWindow = 1;
    CHECK_STR_EQ(workspace.defaultName(), "release");
    // A host's out-of-range index is read as the first, never past the end.
    workspace.hostActiveWindow = 7;
    workspace.windows[0].tabs[0].hostFocusedPane = 9;
    CHECK_STR_EQ(workspace.defaultName(), "/");
    // A pane that reports no directory leaves it unnamed.
    workspace.windows[0].tabs[0].panes = { at("") };
    CHECK(workspace.defaultName().empty());

    // A word a person put on what is in front comes before the folder: the
    // label of a tab's one pane, the tab's name, the window's label.
    workspace.windows[0].tabs[0].panes = { at("/srv/api") };
    workspace.windows[0].tabs[0].panes[0].label = "server";
    CHECK_STR_EQ(workspace.defaultName(), "server");
    workspace.windows[0].tabs[0].name = "build";
    CHECK_STR_EQ(workspace.defaultName(), "build");
    workspace.windows[0].label = "ops";
    CHECK_STR_EQ(workspace.defaultName(), "ops");
    // A pane's label names a tab only when it is the tab's one pane.
    TreeTab split;
    split.panes = { at("/srv/api"), at("/srv/web") };
    split.panes[0].label = "left";
    workspace.windows[0] = TreeWindow {};
    workspace.windows[0].tabs = { split };
    CHECK_STR_EQ(workspace.defaultName(), "api");
}

TEST(aLonePaneIsAWorkspaceOfOneWindowOneTabOneLeaf)
{
    auto layout = std::make_unique<Layout::Workspace>();
    const Layout::SurfaceID surface = layout->surfaceIDAt(0);
    const Layout::TabID tabID = layout->tabAt(surface, 0);
    const Layout::PaneID paneID = layout->focusedPane(surface);
    CHECK(layout->renameTab(surface, tabID, "notes"));

    LabelBook labels;
    CHECK(labels.set({ ObjectKind::Workspace, 7 }, "client") == Ubiquitty::Session::Identity::LabelVerdict::Accepted);
    (void)labels.set({ ObjectKind::Window, std::to_underlying(surface) }, "main");
    (void)labels.set({ ObjectKind::Tab, std::to_underlying(tabID) }, "scratch");
    (void)labels.set({ ObjectKind::Pane, std::to_underlying(paneID) }, "editor");
    (void)labels.set({ ObjectKind::Session, 41 }, "long-lived");

    Panes panes;
    TreePaneFacts facts = at(reporting(OSC::ProgramState::Working), 5'000, 4'000);
    facts.sessionID = 41;
    facts.title.programTitle = "vim notes.md";
    facts.title.currentDirectory = "/home/user/notes";
    facts.title.hostName = "devbox";
    facts.title.columns = 132;
    facts.title.rows = 43;
    facts.title.paneLabel = "not the host's to say";
    facts.program = "vim";
    facts.badgeText = "prod";
    facts.status.tabColor = red;
    panes[paneID] = facts;

    const std::array sources { TreeWorkspaceSource { 7, "work", layout.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 12, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels, panes.provider());

    CHECK_EQ(tree.revision, 12u);
    CHECK_MSG(tree.hostNowUnixMilliseconds == 100'000, "the tree says what time it was on the host");
    CHECK_EQ(tree.workspaces.size(), 1u);
    const TreeWorkspace& workspace = tree.workspaces[0];
    CHECK_EQ(workspace.id, 7u);
    CHECK_STR_EQ(workspace.name, "work");
    CHECK_STR_EQ(workspace.label, "client");
    CHECK_EQ(workspace.windows.size(), 1u);
    CHECK_EQ(workspace.hostActiveWindow, 0u);
    const TreeWindow& window = workspace.windows[0];
    CHECK(window.id == surface && window.isHostActive);
    CHECK_STR_EQ(window.label, "main");
    CHECK_EQ(window.tabs.size(), 1u);
    CHECK_MSG(window.soleTab() == &window.tabs[0] && !window.hasTabRows(), "a window of one tab is listed as that tab");
    const TreeTab& tab = window.tabs[0];
    CHECK(tab.id == tabID && tab.isHostActive);
    CHECK_STR_EQ(tab.label, "scratch");
    CHECK_STR_EQ(tab.name, "notes");
    CHECK_MSG(tab.split.isLeaf(), "a tab of one pane is split nowhere");
    CHECK(tab.split.columns == 132 && tab.split.rows == 43);
    CHECK_EQ(tab.panes.size(), 1u);
    CHECK(tab.hostFocusedPane == 0 && !window.hostZoomedPane);
    CHECK(&tab.titlePane() == &tab.panes[0]);

    const auto& pane = tab.panes[0];
    CHECK(pane.id == paneID && pane.isHostFocused && !pane.isHostZoomed);
    CHECK_EQ(pane.sessionID, 41u);
    CHECK_STR_EQ(pane.label, "editor");
    CHECK_STR_EQ(pane.sessionLabel, "long-lived");
    CHECK_STR_EQ(pane.program, "vim");
    CHECK_STR_EQ(pane.badge, "prod");
    CHECK(pane.columns() == 132 && pane.rows() == 43);
    CHECK_STR_EQ(pane.directory(), "/home/user/notes");
    // The title inputs: the host's own, and the three the tree decides.
    CHECK_STR_EQ(pane.title.programTitle, "vim notes.md");
    CHECK_STR_EQ(pane.title.hostName, "devbox");
    CHECK_STR_EQ(pane.title.paneLabel, "editor");
    CHECK_STR_EQ(pane.title.tabTitle, "notes");
    CHECK_STR_EQ(pane.title.windowTitle, "main");

    // One pane: every level shows that pane.
    CHECK(pane.status.kind == StatusKind::Working && pane.status.count == 1 && pane.status.paneCount == 1);
    CHECK_MSG(pane.status.badgeCount == 1, "a pane with badge text carries a badge whatever its facts said");
    CHECK(sameColor(pane.status.tabColor, red));
    for (const RolledStatus* status : { &tab.panes[0].status, &tab.status, &window.status, &workspace.status, &tree.status })
        CHECK(sameStatus(*status, pane.status));
    for (const PaneActivity* activity : { &tab.panes[0].activity, &tab.activity, &window.activity, &workspace.activity, &tree.activity })
        CHECK(*activity == (PaneActivity { 5'000, 4'000 }));

    CHECK_MSG(panes.asked == (std::vector<std::pair<std::uint32_t, std::uint32_t>> { { 7, std::to_underlying(paneID) } }), "the pane was asked about once, under its workspace");
    CHECK(tree.locate(paneID) == (TreePanePath { 0, 0, 0, 0 }));
    CHECK(&tree.pane(*tree.locate(paneID)) == &pane);
    CHECK(!tree.locate(Layout::PaneID { 999 }));
}

TEST(aTabOfSplitsIsStructuredNodesWithARollUpAtEachSplit)
{
    auto layout = std::make_unique<Layout::Workspace>();
    const Layout::SurfaceID surface = layout->surfaceIDAt(0);
    const Layout::PaneID a = layout->focusedPane(surface);
    // a | (b over c), a taking a quarter.
    const Layout::PaneID b = layout->splitFocusedPane(surface, Orientation::Vertical, 0.25f);
    const Layout::PaneID c = layout->splitFocusedPane(surface, Orientation::Horizontal, 0.5f);
    CHECK(layout->focusPane(surface, b));

    Panes panes;
    panes[a] = at(idle(), 100, 900);
    panes[b] = at(reporting(OSC::ProgramState::Error), 300, 200);
    panes[b].status.indicatorColor = blue;
    panes[c] = at(reporting(OSC::ProgramState::Working), 700, 0);
    panes[c].status.hasUnseenBell = true;
    panes[c].badgeText = "c";
    panes[c].status.progress = { OSC::ProgressNormal, 40 };
    // Each pane as the screen plane names it; c at its own grid and frame.
    panes[a].paneID = { .low = std::to_underlying(a), .high = 7 };
    panes[b].paneID = { .low = std::to_underlying(b), .high = 7 };
    panes[c].paneID = { .low = std::to_underlying(c), .high = 7 };
    panes[c].structuralGeneration = 31;
    panes[c].title.rows = 20;
    panes[c].title.columns = 60;

    const std::array sources { TreeWorkspaceSource { 1, "", layout.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, LabelBook {}, panes.provider());
    const TreeTab& tab = tree.workspaces[0].windows[0].tabs[0];

    CHECK(paneIDs(tab) == (std::vector<std::uint32_t> { std::to_underlying(a), std::to_underlying(b), std::to_underlying(c) }));
    CHECK_MSG(tab.hostFocusedPane == 1 && tab.panes[1].isHostFocused && !tab.panes[0].isHostFocused && !tab.panes[2].isHostFocused, "the tab types into b");
    CHECK(&tab.titlePane() == &tab.panes[1]);

    // The split as the wire's topology node: first child first, and its
    // leaves are the tab's panes in order.
    const TopologyNode& root = tab.split;
    REQUIRE(root.children.size() == 2);
    // A vertical divider puts its halves side by side: laid along the
    // horizontal axis.
    CHECK(root.axis == SplitAxis::Horizontal);
    CHECK(root.ratio > 0.24 && root.ratio < 0.26);
    CHECK(root.children[0].isLeaf());
    const TopologyNode& innerSplit = root.children[1];
    REQUIRE(innerSplit.children.size() == 2);
    CHECK(innerSplit.axis == SplitAxis::Vertical);
    CHECK(innerSplit.ratio > 0.49 && innerSplit.ratio < 0.51);
    CHECK(innerSplit.children[0].isLeaf() && innerSplit.children[1].isLeaf());
    // A leaf names the pane as the screen plane does, at its grid and the
    // generation of the frame that gave it that grid.
    CHECK(root.children[0].paneID == (Ubiquitty::Protocol::Sync::ID128 { .low = std::to_underlying(a), .high = 7 }));
    CHECK(innerSplit.children[0].paneID == (Ubiquitty::Protocol::Sync::ID128 { .low = std::to_underlying(b), .high = 7 }));
    CHECK(innerSplit.children[1].paneID == (Ubiquitty::Protocol::Sync::ID128 { .low = std::to_underlying(c), .high = 7 }));
    CHECK_EQ(innerSplit.children[1].structuralGeneration, 31u);
    CHECK(innerSplit.children[1].rows == 20 && innerSplit.children[1].columns == 60);

    // The leaves.
    CHECK(tab.panes[0].status.kind == StatusKind::Idle);
    CHECK(tab.panes[1].status.kind == StatusKind::Failed);
    CHECK_MSG(tab.panes[2].status.kind == StatusKind::Attention, "an unseen bell outranks work in progress");
    // b over c, folded as a level of their own would be.
    struct Level {
        RolledStatus status;
        PaneActivity activity;
    };
    const std::array<RolledStatus, 2> lowerStatuses { tab.panes[1].status, tab.panes[2].status };
    const std::array<PaneActivity, 2> lowerActivities { tab.panes[1].activity, tab.panes[2].activity };
    const Level inner { Ubiquitty::App::rollUpStatus(lowerStatuses), Ubiquitty::Session::newestActivity(lowerActivities) };
    // b over c: the failure, one pane of two, with c's bell, badge and progress.
    CHECK(inner.status.kind == StatusKind::Failed && inner.status.count == 1 && inner.status.paneCount == 2);
    CHECK(inner.status.hasUnseenBell && !inner.status.hasUnseenNotification);
    CHECK(inner.status.badgeCount == 1);
    CHECK(sameColor(inner.status.indicatorColor, blue));
    CHECK(inner.status.progress.state == OSC::ProgressNormal && inner.status.progress.value == 40);
    CHECK(inner.activity == (PaneActivity { 700, 200 }));
    // The whole tab, which folding a over that level also gives.
    CHECK(tab.status.kind == StatusKind::Failed && tab.status.count == 1 && tab.status.paneCount == 3);
    CHECK_MSG(tab.activity == (PaneActivity { 700, 900 }), "the newest output is c's and the newest input a's");
    const std::array<RolledStatus, 2> nested { tab.panes[0].status, inner.status };
    CHECK(sameStatus(tab.status, Ubiquitty::App::rollUpStatus(nested)));
    CHECK_EQ(panes.asked.size(), 3u);
    CHECK(tree.locate(c) == (TreePanePath { 0, 0, 0, 2 }));
    CHECK(tree.pane(*tree.locate(b)).id == b);
}

TEST(aWindowOfSeveralTabsRollsThemUpAndListsTabRows)
{
    auto layout = std::make_unique<Layout::Workspace>();
    const Layout::SurfaceID surface = layout->surfaceIDAt(0);
    const Layout::TabID first = layout->tabAt(surface, 0);
    const Layout::PaneID firstPane = layout->focusedPane(surface);
    const Layout::TabID second = layout->addTab(surface);
    const Layout::PaneID secondPane = layout->focusedPane(surface);
    const Layout::PaneID secondSplit = layout->splitFocusedPane(surface, Orientation::Vertical);
    const Layout::TabID third = layout->addTab(surface);
    const Layout::PaneID thirdPane = layout->focusedPane(surface);
    CHECK(layout->renameTab(surface, second, "build"));
    CHECK(layout->setActiveTab(surface, second));

    LabelBook labels;
    // A tab and a pane may share a number; their labels are their own.
    (void)labels.set({ ObjectKind::Tab, std::to_underlying(second) }, "the tab");
    (void)labels.set({ ObjectKind::Pane, std::to_underlying(second) }, "a pane of that number");

    Panes panes;
    panes[firstPane] = at(reporting(OSC::ProgramState::Blocked), 10, 10);
    panes[firstPane].status.blockKind = OSC::ProgramBlockKind::Permission;
    panes[firstPane].status.tabColor = red;
    panes[secondPane] = at(reporting(OSC::ProgramState::Blocked), 50, 20);
    panes[secondPane].status.tabColor = blue;
    panes[secondPane].badgeText = "x";
    panes[secondSplit] = at(reporting(OSC::ProgramState::Done), 20, 60);
    panes[secondSplit].status.hasUnseenNotification = true;
    panes[thirdPane] = at(idle(), 30, 30);
    panes[thirdPane].badgeText = "y";

    const std::array sources { TreeWorkspaceSource { 3, "", layout.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels, panes.provider());
    const TreeWindow& window = tree.workspaces[0].windows[0];

    CHECK_EQ(window.tabs.size(), 3u);
    CHECK(window.tabs[0].id == first && window.tabs[1].id == second && window.tabs[2].id == third);
    CHECK_MSG(window.soleTab() == nullptr && window.hasTabRows(), "a window of several tabs lists them");
    CHECK(window.hostActiveTab == 1 && window.tabs[1].isHostActive && !window.tabs[0].isHostActive && !window.tabs[2].isHostActive);
    CHECK_STR_EQ(window.tabs[1].label, "the tab");
    CHECK_STR_EQ(window.tabs[1].name, "build");
    CHECK_STR_EQ(window.tabs[0].name, "");
    CHECK_STR_EQ(window.tabs[1].panes[0].title.tabTitle, "build");
    CHECK_STR_EQ(window.tabs[1].panes[1].title.tabTitle, "build");
    CHECK_STR_EQ(window.tabs[0].panes[0].title.tabTitle, "");
    CHECK_STR_EQ(window.tabs[2].panes[0].title.tabTitle, "");

    CHECK(window.tabs[0].status.kind == StatusKind::Blocked && window.tabs[0].status.count == 1 && window.tabs[0].status.paneCount == 1);
    CHECK(window.tabs[1].status.kind == StatusKind::Blocked && window.tabs[1].status.count == 1 && window.tabs[1].status.paneCount == 2);
    CHECK(window.tabs[1].status.hasUnseenNotification && window.tabs[1].status.badgeCount == 1 && sameColor(window.tabs[1].status.tabColor, blue));
    CHECK(window.tabs[1].activity == (PaneActivity { 50, 60 }));
    CHECK(window.tabs[2].status.kind == StatusKind::Idle && window.tabs[2].status.badgeCount == 1);

    // The window: two blocked panes of four, in two tabs.
    CHECK(window.status.kind == StatusKind::Blocked && window.status.count == 2 && window.status.paneCount == 4);
    CHECK_MSG(window.status.blockKind == OSC::ProgramBlockKind::Permission && sameColor(window.status.tabColor, red), "the first most-urgent pane in display order says what and in which colour");
    CHECK(window.status.hasUnseenNotification && !window.status.hasUnseenBell);
    CHECK_EQ(window.status.badgeCount, 2u);
    CHECK(window.activity == (PaneActivity { 50, 60 }));
    CHECK(sameStatus(tree.workspaces[0].status, window.status));
    CHECK(sameStatus(flatStatus(tree.workspaces[0]), window.status));
}

TEST(workspacesAndWindowsAreListedInAStableOrder)
{
    const auto identities = std::make_shared<Allocator>();
    auto early = std::make_unique<Layout::Workspace>(identities);
    auto late = std::make_unique<Layout::Workspace>(identities);
    const Layout::SurfaceID earlyWindow = early->surfaceIDAt(0);
    const Layout::PaneID earlyPane = early->focusedPane(earlyWindow);
    const Layout::SurfaceID lateFirst = late->surfaceIDAt(0);
    const Layout::PaneID lateFirstPane = late->focusedPane(lateFirst);
    const Layout::SurfaceID lateSecond = late->addSurface();
    const Layout::PaneID lateSecondPane = late->focusedPane(lateSecond);
    const Layout::SurfaceID lateThird = late->addSurface();
    const Layout::PaneID lateThirdPane = late->focusedPane(lateThird);
    CHECK(late->setActiveSurface(lateSecond));

    LabelBook labels;
    (void)labels.set({ ObjectKind::Workspace, 9 }, "nine");
    (void)labels.set({ ObjectKind::Window, std::to_underlying(lateSecond) }, "middle");
    (void)labels.set({ ObjectKind::Pane, std::to_underlying(lateSecondPane) }, "the pane");

    Panes panes;
    panes[earlyPane] = at(reporting(OSC::ProgramState::Done), 1'000, 10);
    panes[lateFirstPane] = at(idle(), 20, 2'000);
    panes[lateSecondPane] = at(reporting(OSC::ProgramState::Working), 30, 30);
    panes[lateThirdPane] = at(reporting(OSC::ProgramState::Working), 40, 40);

    // Given newest first; listed oldest first.
    const std::array sources { TreeWorkspaceSource { 9, "late", late.get() }, TreeWorkspaceSource { 2, "early", early.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 77, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels, panes.provider());
    CHECK_EQ(tree.workspaces.size(), 2u);
    CHECK(tree.workspaces[0].id == 2 && tree.workspaces[1].id == 9);
    CHECK_STR_EQ(tree.workspaces[0].name, "early");
    CHECK_STR_EQ(tree.workspaces[0].label, "");
    CHECK_STR_EQ(tree.workspaces[1].label, "nine");

    const std::array reversed { sources[1], sources[0] };
    Panes again = panes;
    const SessionTree other = buildSessionTree({ .revision = 77, .nowUnixMilliseconds = 100'000, .workspaces = reversed }, labels, again.provider());
    CHECK_MSG(other.workspaces[0].id == 2 && other.workspaces[1].id == 9, "the order given does not change the order listed");

    const TreeWorkspace& workspace = tree.workspaces[1];
    CHECK_EQ(workspace.windows.size(), 3u);
    CHECK(workspace.windows[0].id == lateFirst && workspace.windows[1].id == lateSecond && workspace.windows[2].id == lateThird);
    CHECK(workspace.hostActiveWindow == 1 && workspace.windows[1].isHostActive && !workspace.windows[0].isHostActive && !workspace.windows[2].isHostActive);
    CHECK_STR_EQ(workspace.windows[1].label, "middle");
    CHECK_STR_EQ(workspace.windows[1].tabs[0].panes[0].title.windowTitle, "middle");
    CHECK_STR_EQ(workspace.windows[1].tabs[0].panes[0].title.paneLabel, "the pane");
    CHECK_STR_EQ(workspace.windows[0].tabs[0].panes[0].title.windowTitle, "");
    for (const TreeWindow& window : workspace.windows)
        CHECK(window.soleTab() != nullptr);

    // Each workspace from its own windows, and the host from its workspaces.
    CHECK(tree.workspaces[0].status.kind == StatusKind::Done && tree.workspaces[0].status.count == 1 && tree.workspaces[0].status.paneCount == 1);
    CHECK(workspace.status.kind == StatusKind::Working && workspace.status.count == 2 && workspace.status.paneCount == 3);
    CHECK(workspace.activity == (PaneActivity { 40, 2'000 }));
    CHECK(sameStatus(flatStatus(workspace), workspace.status));
    CHECK(tree.status.kind == StatusKind::Done && tree.status.count == 1 && tree.status.paneCount == 4);
    CHECK(tree.activity == (PaneActivity { 1'000, 2'000 }));

    CHECK(tree.locate(lateThirdPane) == (TreePanePath { 1, 2, 0, 0 }));
    CHECK(tree.locate(earlyPane) == (TreePanePath { 0, 0, 0, 0 }));
    // A pane is asked about under the workspace that holds it.
    for (const auto& [workspaceID, pane] : panes.asked)
        CHECK(workspaceID == (pane == std::to_underlying(earlyPane) ? 2u : 9u));
    CHECK_EQ(panes.asked.size(), 4u);
}

TEST(whoIsAttachedAndWhenAPaneWasMadeRollUpWithoutCountingAnyoneTwice)
{
    using Ubiquitty::Session::TreeAttendance;
    using Ubiquitty::Session::TreeAttendee;
    using Ubiquitty::Session::TreeAttendeeKind;
    const auto identities = std::make_shared<Allocator>();
    auto first = std::make_unique<Layout::Workspace>(identities);
    auto second = std::make_unique<Layout::Workspace>(identities);
    const Layout::SurfaceID window = first->surfaceIDAt(0);
    const Layout::PaneID a = first->focusedPane(window);
    const Layout::PaneID b = first->splitFocusedPane(window, Orientation::Vertical);
    const Layout::SurfaceID otherWindow = first->addSurface();
    const Layout::PaneID c = first->focusedPane(otherWindow);
    const Layout::PaneID d = second->focusedPane(second->surfaceIDAt(0));

    const TreeAttendee person { 1, TreeAttendeeKind::Person };
    const TreeAttendee agent { 2, TreeAttendeeKind::Agent };
    const TreeAttendee visitor { 3, TreeAttendeeKind::Person };
    Panes panes;
    panes[a] = idle();
    panes[a].attendees = { person };
    panes[a].createdUnixSeconds = 5'000;
    panes[b] = idle();
    // The same person on both panes of the split, listed twice on this one.
    panes[b].attendees = { person, agent, person };
    panes[b].createdUnixSeconds = 3'000;
    panes[b].isAdopted = true;
    panes[c] = idle();
    panes[c].attendees = { visitor, agent };
    panes[c].createdUnixSeconds = 0;
    panes[d] = idle();
    panes[d].createdUnixSeconds = 1'000;

    const std::array sources { TreeWorkspaceSource { 1, "", first.get() }, TreeWorkspaceSource { 2, "", second.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, LabelBook {}, panes.provider());
    const TreeWorkspace& workspace = tree.workspaces[0];
    const TreeTab& tab = workspace.windows[0].tabs[0];

    CHECK(tab.panes[0].attendance == (TreeAttendance { 1, 0 }) && !tab.panes[0].isShared());
    CHECK_MSG(tab.panes[1].attendance == (TreeAttendance { 1, 1 }) && tab.panes[1].isShared(), "someone listed twice on a pane is there once");
    CHECK(tab.panes[1].attendees == (std::vector<TreeAttendee> { person, agent }));
    CHECK(tab.panes[1].isAdopted && !tab.panes[0].isAdopted);
    CHECK(tab.panes[0].createdUnixSeconds == 5'000 && tab.panes[1].createdUnixSeconds == 3'000);

    CHECK_MSG(tab.attendance == (TreeAttendance { 1, 1 }), "a person on two panes of a tab is one person on the tab");
    CHECK(tab.createdUnixSeconds == 3'000);
    CHECK(workspace.windows[0].attendance == (TreeAttendance { 1, 1 }));
    CHECK(workspace.windows[1].attendance == (TreeAttendance { 1, 1 }));
    CHECK_MSG(workspace.windows[1].createdUnixSeconds == 0, "a time nobody recorded is not the earliest");
    CHECK_MSG(workspace.attendance == (TreeAttendance { 2, 1 }), "the agent on two windows is one agent on the workspace");
    CHECK(workspace.createdUnixSeconds == 3'000);

    CHECK(tree.workspaces[1].attendance == (TreeAttendance { 0, 0 }));
    CHECK(tree.workspaces[1].createdUnixSeconds == 1'000);
    CHECK(!tree.workspaces[1].windows[0].tabs[0].panes[0].isShared());
    CHECK(tree.attendance == (TreeAttendance { 2, 1 }) && tree.attendance.total() == 3);
    CHECK(tree.createdUnixSeconds == 1'000);
}

TEST(theTagsAPersonPutOnSomethingAreOnItsNode)
{
    using Ubiquitty::Session::Identity::TagBook;
    auto layout = std::make_unique<Layout::Workspace>();
    const Layout::SurfaceID surface = layout->surfaceIDAt(0);
    const Layout::TabID tabID = layout->tabAt(surface, 0);
    const Layout::PaneID paneID = layout->focusedPane(surface);
    const Layout::SurfaceID other = layout->addSurface();

    TagBook tags;
    const auto put = [&](ObjectID id, std::vector<std::string> names) { CHECK(tags.set(id, names) == Ubiquitty::Session::Identity::TagVerdict::Accepted); };
    put({ ObjectKind::Workspace, 4 }, { "client" });
    put({ ObjectKind::Window, std::to_underlying(surface) }, { "production", "billing" });
    put({ ObjectKind::Tab, std::to_underlying(tabID) }, { "tab tag" });
    put({ ObjectKind::Pane, std::to_underlying(paneID) }, { "pane tag" });
    put({ ObjectKind::Session, 41 }, { "session tag", "kept" });
    put({ ObjectKind::Session, 42 }, { "another session's" });

    Panes panes;
    TreePaneFacts behind;
    behind.sessionID = 41;
    panes[paneID] = behind;
    const std::array sources { TreeWorkspaceSource { 4, "", layout.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 0, .workspaces = sources, .tags = &tags }, LabelBook {}, panes.provider());
    const TreeWorkspace& workspace = tree.workspaces[0];
    CHECK(workspace.tags == std::vector<std::string> { "client" });
    CHECK(workspace.windows[0].tags == (std::vector<std::string> { "production", "billing" }));
    CHECK_MSG(workspace.windows[1].tags.empty() && workspace.windows[1].id == other, "one window's tags are not its neighbour's");
    CHECK(workspace.windows[0].tabs[0].tags == std::vector<std::string> { "tab tag" });
    CHECK(workspace.windows[0].tabs[0].panes[0].tags == std::vector<std::string> { "pane tag" });
    CHECK_MSG(workspace.windows[0].tabs[0].panes[0].sessionTags == (std::vector<std::string> { "session tag", "kept" }), "a session's tags are on the pane it draws into, apart from the pane's own");
    CHECK_MSG(workspace.windows[1].tabs[0].panes[0].sessionTags.empty(), "a pane no session draws into has no session's tags");
    CHECK(workspace.windows[1].tabs[0].tags.empty() && workspace.windows[1].tabs[0].panes[0].tags.empty());

    // A host that keeps no tags lists none.
    const SessionTree bare = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 0, .workspaces = sources }, LabelBook {}, panes.provider());
    CHECK(bare.workspaces[0].tags.empty() && bare.workspaces[0].windows[0].tags.empty());
}

TEST(aWorkspaceTheHostCouldRestoreIsListedFromItsRecord)
{
    using Ubiquitty::Session::HostLayoutRecord;
    using Ubiquitty::Session::PaneNode;
    using Ubiquitty::Session::TreeRestorableSource;
    using Ubiquitty::Session::TreeWorkspaceState;
    using Ubiquitty::Session::WorkspaceTab;
    using Ubiquitty::Session::WorkspaceWindow;
    using Ubiquitty::Transport::Origin;
    using Ubiquitty::Transport::OriginKind;
    const auto leaf = [](std::string directory, std::string title, Origin origin = {}) {
        PaneNode node = PaneNode::makeLeaf(std::move(origin), std::move(title));
        node.directory = std::move(directory);
        return node;
    };

    // Two windows: a tab of one pane and a named tab of three, the first
    // pane zoomed and the last typed into; and a window of one remote pane.
    HostLayoutRecord record;
    record.arrangement.name = "kept";
    record.workspaceLabel = "k";
    WorkspaceWindow first;
    WorkspaceTab single;
    single.root = leaf("/home/user", "shell");
    first.tabs.push_back(single);
    WorkspaceTab split;
    split.title = "logs";
    split.root = PaneNode::makeSplit(false, 0.25, leaf("/var/log", "tail"), PaneNode::makeSplit(true, 0.5, leaf("/tmp", "less", Origin { OriginKind::Local, "not a host" }), leaf("/srv", "vim")));
    split.focusedPaneIndex = 2;
    first.tabs.push_back(split);
    first.activeTab = 1;
    first.zoomedPaneIndex = 0;
    record.arrangement.windows.push_back(first);
    WorkspaceWindow second;
    WorkspaceTab remote;
    remote.root = leaf("/home/user/project", "remote", Origin { OriginKind::SSH, "user@box" });
    second.tabs.push_back(remote);
    record.arrangement.windows.push_back(second);
    record.arrangement.activeWindow = 1;
    record.windowLabels = { "main", "" };
    record.tabLabels = { "", "tailing", "far" };
    record.windowTags = { { "production" }, {} };
    record.workspaceTags = { "client" };
    record.tabTags = { {}, { "noisy" } };
    record.panes = { { 80, 24, "", "", {}, {} }, { 100, 30, "tailer", "", { "watch" }, {} }, { 50, 15, "", "scratch", {}, { "throwaway" } }, { 50, 14, "", "", {}, {} }, { 132, 43, "", "", {}, {} } };

    const auto identities = std::make_shared<Allocator>();
    auto live = std::make_unique<Layout::Workspace>(identities);
    const Layout::PaneID livePane = live->focusedPane(live->surfaceIDAt(0));
    Panes panes;
    panes[livePane] = at(reporting(OSC::ProgramState::Working), 500, 400);
    auto restored = std::make_unique<Layout::Workspace>(identities);
    const std::array sources {
        TreeWorkspaceSource { .id = 8, .name = "live", .layout = live.get(), .endsWhenDetached = true },
        TreeWorkspaceSource { .id = 3, .name = "back", .layout = restored.get(), .wasRestoredAfterHostRestart = true },
    };
    const std::array restorable { TreeRestorableSource { 5, &record }, TreeRestorableSource { 6, nullptr } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 1'000, .workspaces = sources, .restorable = restorable }, LabelBook {}, panes.provider());

    // Listed among the live ones, by id; a source with no record lists nothing.
    CHECK_EQ(tree.workspaces.size(), 3u);
    if (tree.workspaces.size() != 3)
        return;
    CHECK(tree.workspaces[0].id == 3 && tree.workspaces[1].id == 5 && tree.workspaces[2].id == 8);
    CHECK(tree.workspaces[0].state == TreeWorkspaceState::RestoredAfterHostRestart && !tree.workspaces[0].isRestorable() && !tree.workspaces[0].endsWhenDetached);
    CHECK(tree.workspaces[2].state == TreeWorkspaceState::Live && tree.workspaces[2].endsWhenDetached && !tree.workspaces[2].isRestorable());
    const TreeWorkspace& kept = tree.workspaces[1];
    CHECK(kept.state == TreeWorkspaceState::Restorable && kept.isRestorable() && !kept.endsWhenDetached);
    CHECK_STR_EQ(kept.name, "kept");
    CHECK_STR_EQ(kept.label, "k");

    // The recorded arrangement.
    CHECK_EQ(kept.windows.size(), 2u);
    CHECK(kept.hostActiveWindow == 1 && kept.windows[1].isHostActive && !kept.windows[0].isHostActive);
    const TreeWindow& main = kept.windows[0];
    CHECK_STR_EQ(main.label, "main");
    CHECK(main.tags == std::vector<std::string> { "production" } && kept.windows[1].tags.empty());
    CHECK(main.id == Layout::invalidSurface);
    CHECK(main.tabs.size() == 2 && main.hostActiveTab == 1 && main.tabs[1].isHostActive && !main.tabs[0].isHostActive);
    CHECK(main.soleTab() == nullptr && kept.windows[1].soleTab() != nullptr);
    const TreeTab& logs = main.tabs[1];
    CHECK_STR_EQ(logs.name, "logs");
    CHECK_STR_EQ(logs.label, "tailing");
    CHECK_STR_EQ(main.tabs[0].label, "");
    CHECK_STR_EQ(kept.windows[1].tabs[0].label, "far");
    CHECK(logs.id == Layout::invalidTab);

    // The split, as structured nodes: recorded panes have no name on the
    // screen plane, so a leaf is its grid alone.
    REQUIRE(logs.split.children.size() == 2);
    CHECK(logs.split.axis == SplitAxis::Horizontal && logs.split.ratio > 0.24 && logs.split.ratio < 0.26);
    CHECK(logs.split.children[0].isLeaf());
    REQUIRE(logs.split.children[1].children.size() == 2);
    CHECK(logs.split.children[1].axis == SplitAxis::Vertical);
    CHECK(logs.split.children[1].children[0].isLeaf() && logs.split.children[1].children[1].isLeaf());
    CHECK(logs.split.children[0].paneID == Ubiquitty::Protocol::Sync::ID128 {} && logs.split.children[0].structuralGeneration == 0);

    // Each pane's recorded facts, matched to it in leaf order.
    CHECK_EQ(logs.panes.size(), 3u);
    CHECK_STR_EQ(logs.panes[0].label, "tailer");
    CHECK_STR_EQ(logs.panes[0].title.programTitle, "tail");
    CHECK_STR_EQ(logs.panes[0].directory(), "/var/log");
    CHECK(logs.panes[0].columns() == 100 && logs.panes[0].rows() == 30);
    CHECK_STR_EQ(logs.panes[1].sessionLabel, "scratch");
    // The tags a person put on each level, as the record kept them.
    CHECK(kept.tags == std::vector<std::string> { "client" });
    CHECK(logs.tags == std::vector<std::string> { "noisy" } && main.tabs[0].tags.empty() && kept.windows[1].tabs[0].tags.empty());
    CHECK(logs.panes[0].tags == std::vector<std::string> { "watch" } && logs.panes[0].sessionTags.empty());
    CHECK(logs.panes[1].tags.empty() && logs.panes[1].sessionTags == std::vector<std::string> { "throwaway" });
    CHECK(logs.panes[1].columns() == 50 && logs.panes[1].rows() == 15);
    CHECK_STR_EQ(logs.panes[2].title.programTitle, "vim");
    CHECK(logs.panes[2].columns() == 50 && logs.panes[2].rows() == 14);
    CHECK(main.tabs[0].panes[0].columns() == 80 && main.tabs[0].panes[0].directory() == "/home/user");
    const auto& far = kept.windows[1].tabs[0].panes[0];
    CHECK(far.columns() == 132 && far.rows() == 43);
    CHECK_MSG(far.title.hostName == "user@box" && logs.panes[0].title.hostName.empty(), "a pane recorded as a remote session names its host");
    CHECK_MSG(logs.panes[1].title.hostName.empty(), "and a local pane names none, whatever its record's origin holds");
    // The title inputs the tree decides.
    CHECK_STR_EQ(logs.panes[0].title.paneLabel, "tailer");
    CHECK_STR_EQ(logs.panes[0].title.tabTitle, "logs");
    CHECK_STR_EQ(logs.panes[0].title.windowTitle, "main");
    CHECK_STR_EQ(far.title.windowTitle, "");
    // Focus and zoom, as recorded.
    CHECK(logs.hostFocusedPane == 2 && logs.panes[2].isHostFocused && !logs.panes[0].isHostFocused);
    CHECK(main.hostZoomedPane == zoomAt(1, 0) && logs.panes[0].isHostZoomed && !logs.panes[2].isHostZoomed);
    CHECK_MSG(!main.tabs[0].panes[0].isHostZoomed, "the zoom is the shown tab's");

    // Nothing runs: no identity below the workspace, no status, no activity.
    for (const TreeWindow& window : kept.windows) {
        for (const TreeTab& tab : window.tabs) {
            for (const auto& pane : tab.panes) {
                CHECK(pane.id == Layout::invalidPane && pane.sessionID == 0);
                CHECK(pane.status.kind == StatusKind::None && pane.status.count == 0 && pane.status.paneCount == 1);
                CHECK(pane.activity == PaneActivity {} && pane.attendees.empty());
            }
        }
    }
    CHECK(logs.status.kind == StatusKind::None && logs.status.paneCount == 3);
    CHECK(kept.status.kind == StatusKind::None && kept.status.count == 0 && kept.status.paneCount == 5);
    CHECK(kept.activity == PaneActivity {} && kept.attendance.total() == 0 && kept.createdUnixSeconds == 0);
    CHECK_MSG(tree.status.kind == StatusKind::Working && tree.status.count == 1 && tree.status.paneCount == 7, "the host's status is its live panes'; the recorded ones are only counted");
    CHECK_MSG(panes.asked.size() == 2, "no recorded pane is asked about");
    CHECK_MSG(!tree.locate(Layout::invalidPane).has_value(), "a pane with no identity cannot be found by one");
    CHECK(tree.locate(livePane) == (TreePanePath { 2, 0, 0, 0 }));
}

TEST(aZoomedPaneIsMarkedAndTheSplitStaysAsItWas)
{
    auto layout = std::make_unique<Layout::Workspace>();
    const Layout::SurfaceID surface = layout->surfaceIDAt(0);
    const Layout::PaneID a = layout->focusedPane(surface);
    const Layout::PaneID b = layout->splitFocusedPane(surface, Orientation::Vertical);
    CHECK(layout->toggleZoom(surface, a));

    Panes panes;
    panes[a] = idle();
    panes[b] = idle();
    const std::array sources { TreeWorkspaceSource { 1, "", layout.get() } };
    SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, LabelBook {}, panes.provider());
    const TreeTab* tab = &tree.workspaces[0].windows[0].tabs[0];
    CHECK(tree.workspaces[0].windows[0].hostZoomedPane == zoomAt(0, 0));
    CHECK(tab->panes[0].isHostZoomed && !tab->panes[1].isHostZoomed);
    CHECK_MSG(tab->split.children.size() == 2 && tab->panes.size() == 2, "the split is still there under the zoom");
    CHECK_MSG(tab->hostFocusedPane == 1 && tab->panes[1].isHostFocused, "zoom and focus are told apart");

    CHECK(layout->toggleZoom(surface, a));
    tree = buildSessionTree({ .revision = 2, .nowUnixMilliseconds = 100'000, .workspaces = sources }, LabelBook {}, panes.provider());
    tab = &tree.workspaces[0].windows[0].tabs[0];
    CHECK(!tree.workspaces[0].windows[0].hostZoomedPane && !tab->panes[0].isHostZoomed && !tab->panes[1].isHostZoomed);
}

TEST(aWorkspaceOfNothingAndAHostOfNothingAreStillTrees)
{
    const SessionTree none = buildSessionTree({ .revision = 5, .nowUnixMilliseconds = 100'000, .workspaces = std::span<const TreeWorkspaceSource> {} }, LabelBook {}, [](std::uint32_t, Layout::PaneID) { return Ubiquitty::Session::TreePaneFacts {}; });
    CHECK(none.revision == 5 && none.workspaces.empty());
    CHECK(none.status.kind == StatusKind::None && none.status.count == 0 && none.status.paneCount == 0);
    CHECK(none.activity == PaneActivity {});
    CHECK(!none.locate(Layout::PaneID { 1 }));

    // An allocator with nothing left gives a workspace no window at all.
    auto unbuilt = std::make_unique<Layout::Workspace>(std::make_shared<Allocator>(0));
    CHECK_MSG(unbuilt->surfaceCount() == 0, "premise: a workspace with no window");
    auto real = std::make_unique<Layout::Workspace>();
    Panes panes;
    panes[real->focusedPane(real->surfaceIDAt(0))] = reporting(OSC::ProgramState::Working);
    LabelBook labels;
    (void)labels.set({ ObjectKind::Workspace, 4 }, "hollow");
    const std::array sources { TreeWorkspaceSource { 4, "empty", unbuilt.get() }, TreeWorkspaceSource { 5, "absent", nullptr }, TreeWorkspaceSource { 6, "real", real.get() } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, labels, panes.provider());
    CHECK_EQ(tree.workspaces.size(), 3u);
    for (std::size_t index = 0; index < 2; ++index) {
        const TreeWorkspace& workspace = tree.workspaces[index];
        CHECK(workspace.windows.empty() && workspace.hostActiveWindow == 0);
        CHECK(workspace.status.kind == StatusKind::None && workspace.status.count == 0 && workspace.status.paneCount == 0);
        CHECK(workspace.activity == PaneActivity {});
    }
    CHECK_STR_EQ(tree.workspaces[0].name, "empty");
    CHECK_STR_EQ(tree.workspaces[0].label, "hollow");
    CHECK_MSG(tree.status.kind == StatusKind::Working && tree.status.count == 1 && tree.status.paneCount == 1, "an empty workspace adds nothing to the host's status");
    CHECK_EQ(panes.asked.size(), 1u);
}

TEST(everyTextATreeHoldsIsUTF8WhateverBytesItWasBuiltFrom)
{
    // Records and names stay bytes on the wire. A tree is what a listing
    // reads, so each of its texts is text: an ill-formed byte is U+FFFD in
    // it, and everything around the byte is kept.
    const SessionTree tree = UbiquittyTest::illFormedTree();
    std::size_t texts = 0;
    std::size_t repaired = 0;
    UbiquittyTest::forEachText(tree, [&](const std::string& text) {
        ++texts;
        CHECK_MSG(Ubiquitty::Protocol::Sync::isValidUTF8({ reinterpret_cast<const std::uint8_t*>(text.data()), text.size() }), text.c_str());
        if (text.empty())
            return;
        CHECK_MSG(text.starts_with("bad\xEF\xBF\xBD"), text.c_str());
        ++repaired;
    });
    CHECK(texts > 80);
    CHECK(repaired > 70);
    const Ubiquitty::Session::TreePane& pane = tree.workspaces.at(0).windows.at(0).tabs.at(1).panes.at(0);
    CHECK_STR_EQ(tree.workspaces[0].name, "bad\xEF\xBF\xBDworkspace");
    CHECK_STR_EQ(tree.workspaces[0].windows[0].tabs[1].name, "bad\xEF\xBF\xBDtab");
    CHECK_STR_EQ(pane.facts.attention.notificationTitle, "bad\xEF\xBF\xBDnotification");
    CHECK_STR_EQ(pane.facts.attention.notificationBody, "bad\xEF\xBF\xBD"
                                                        "body");
    CHECK_STR_EQ(pane.facts.currentDirectory, "bad\xEF\xBF\xBD"
                                              "directory");
    CHECK_STR_EQ(pane.title.programTitle, "bad\xEF\xBF\xBDtitle");
    CHECK_STR_EQ(pane.program, "bad\xEF\xBF\xBDprogram");
    CHECK_STR_EQ(pane.badge, "bad\xEF\xBF\xBD"
                             "badge");
    CHECK_STR_EQ(pane.sessionLabel, "bad\xEF\xBF\xBDsession label");
    CHECK_STR_EQ(pane.records.store.records[0].message, "bad\xEF\xBF\xBDmessage");
    // Text that is already text is the tree's byte for byte.
    Ubiquitty::Session::TreeWorkspace plain = UbiquittyTest::illFormedStructure();
    plain.name = "caf\xC3\xA9 \xF0\x9F\x98\x80";
    const SessionTree kept = assembleSessionTree(std::span { &plain, 1 }, 1, 2, [](Ubiquitty::Protocol::Sync::ID128) { return Ubiquitty::Session::HeldPaneRecords {}; });
    CHECK_STR_EQ(kept.workspaces.at(0).name, "caf\xC3\xA9 \xF0\x9F\x98\x80");
}

// ---- The tree read as one row a session -------------------------------------

namespace {

using Ubiquitty::Session::SessionRow;
using Ubiquitty::Session::sessionRows;
using Ubiquitty::Session::TreePane;
using Ubiquitty::Session::TreeWorkspaceState;

/// A pane a session draws into, with what a row reads of it.
TreePane rowPane(std::uint32_t pane, std::uint32_t session, std::string title)
{
    TreePane made;
    made.id = Layout::PaneID { pane };
    made.sessionID = session;
    made.title.programTitle = std::move(title);
    return made;
}

} // namespace

TEST(aRowSaysWhereItsSessionStandsByIdentityAndByWhatEachLevelIsCalled)
{
    SessionTree tree;
    TreeWorkspace work;
    work.id = 80;
    work.name = "work";
    work.label = "client";
    work.endsWhenDetached = true;
    work.state = TreeWorkspaceState::RestoredAfterHostRestart;
    TreeWindow window;
    window.id = Layout::SurfaceID { 11 };
    window.label = "main";
    TreeTab build;
    build.id = Layout::TabID { 21 };
    build.name = "build";
    build.label = "tb";
    TreePane make = rowPane(31, 9, "make all");
    make.label = "runner";
    make.sessionLabel = "ci";
    make.title.currentDirectory = "/srv/app";
    make.title.columns = 132;
    make.title.rows = 43;
    make.program = "make";
    make.facts.isAtPrompt = true;
    make.attendance = { .people = 2, .agents = 1 };
    make.createdUnixSeconds = 1'700'000'000;
    make.isAdopted = true;
    // A pane no session draws into, and one of a session numbered below.
    build.panes = { make, rowPane(32, 0, "nothing"), rowPane(33, 4, "vim") };
    TreeTab logs;
    logs.id = Layout::TabID { 22 };
    logs.panes = { rowPane(34, 6, "tail") };
    window.tabs = { build, logs };
    work.windows = { window };
    TreeWorkspace alone;
    alone.id = 5;
    TreeWindow aloneWindow;
    aloneWindow.id = Layout::SurfaceID { 12 };
    TreeTab aloneTab;
    aloneTab.id = Layout::TabID { 23 };
    aloneTab.panes = { rowPane(35, 5, "zsh") };
    aloneWindow.tabs = { aloneTab };
    alone.windows = { aloneWindow };
    // The tree lists workspaces in its own order; the rows are by session.
    tree.workspaces = { work, alone };

    const std::vector<SessionRow> rows = sessionRows(tree);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].sessionID == 4 && rows[1].sessionID == 5 && rows[2].sessionID == 6 && rows[3].sessionID == 9);
    const SessionRow& row = rows[3];
    CHECK(row.id == 9);
    CHECK_STR_EQ(row.label, "ci");
    CHECK(row.workspaceID == 80 && row.workspaceName == "work" && row.workspaceLabel == "client");
    CHECK(row.workspaceState == TreeWorkspaceState::RestoredAfterHostRestart && row.wasRestoredAfterHostRestart() && !row.isRestorable());
    CHECK(row.endsWhenDetached());
    CHECK(row.windowID == 11 && row.windowLabel == "main");
    CHECK(row.tabID == 21 && row.tabTitle == "build" && row.tabLabel == "tb");
    CHECK(row.paneID == 31 && row.paneLabel == "runner");
    CHECK_STR_EQ(row.title, "make all");
    CHECK_STR_EQ(row.currentDirectory, "/srv/app");
    CHECK_STR_EQ(row.foregroundProcess, "make");
    CHECK(row.columns == 132 && row.rows == 43);
    CHECK(row.isAtPrompt() && row.isAdopted && row.createdUnixSeconds == 1'700'000'000);
    CHECK(row.attendeeCount == 3 && row.humanAttendeeCount == 2 && row.isShared());
    // Its neighbours are their own: another tab, another workspace.
    CHECK(rows[2].tabID == 22 && rows[2].windowID == 11 && rows[2].workspaceID == 80 && rows[2].tabTitle.empty() && rows[2].title == "tail");
    CHECK(rows[0].tabID == 21 && rows[0].paneID == 33 && rows[0].label.empty() && rows[0].paneLabel.empty() && !rows[0].isAtPrompt() && !rows[0].isShared() && !rows[0].isAdopted);
    CHECK(rows[1].workspaceID == 5 && rows[1].workspaceName.empty() && rows[1].windowID == 12 && rows[1].tabID == 23 && rows[1].paneID == 35);
    CHECK(!rows[1].endsWhenDetached() && !rows[1].wasRestoredAfterHostRestart() && rows[1].attendeeCount == 0);
    // No row for the pane no session draws into.
    CHECK(std::ranges::none_of(rows, [](const SessionRow& each) { return each.paneID == 32; }));

    // More attached than a count holds stops at what it holds.
    tree.workspaces[1].windows[0].tabs[0].panes[0].attendance = { .people = 300, .agents = 5 };
    const std::vector<SessionRow> crowded = sessionRows(tree);
    CHECK(crowded[1].attendeeCount == 255 && crowded[1].humanAttendeeCount == 255);
    CHECK(sessionRows(SessionTree {}).empty());
}

TEST(aWorkspaceTheHostCouldRestoreIsOneRowAfterTheSessions)
{
    SessionTree tree;
    const auto recorded = [](std::uint32_t id, std::string name) {
        TreeWorkspace workspace;
        workspace.id = id;
        workspace.name = std::move(name);
        workspace.label = "kept label";
        workspace.state = TreeWorkspaceState::Restorable;
        TreeWindow window;
        TreeTab first;
        TreePane pane = rowPane(0, 0, "tail -f");
        pane.sessionLabel = "tailer";
        pane.label = "left";
        pane.title.currentDirectory = "/var/log";
        pane.title.columns = 100;
        pane.title.rows = 30;
        // Whatever a record's pane carries, nothing runs in it.
        pane.attendance = { .people = 1, .agents = 0 };
        pane.facts.isAtPrompt = true;
        first.panes = { pane, rowPane(0, 0, "second pane") };
        TreeTab second;
        second.panes = { rowPane(0, 0, "second tab") };
        window.tabs = { first, second };
        workspace.windows = { window };
        return workspace;
    };
    TreeWorkspace live;
    live.id = 70;
    TreeWindow window;
    TreeTab tab;
    tab.panes = { rowPane(41, 70, "zsh") };
    window.tabs = { tab };
    live.windows = { window };
    TreeWorkspace hollow;
    hollow.id = 40;
    hollow.name = "hollow";
    hollow.state = TreeWorkspaceState::Restorable;
    tree.workspaces = { recorded(900, "later"), live, recorded(60, "kept"), hollow };
    // A record cut off at any level reads as a row with nothing under it.
    TreeWorkspace noTabs = hollow;
    noTabs.windows.emplace_back();
    TreeWorkspace noPanes = noTabs;
    noPanes.windows[0].tabs.emplace_back();
    for (const TreeWorkspace& cut : { noTabs, noPanes }) {
        SessionTree alone;
        alone.workspaces = { cut };
        const std::vector<SessionRow> one = sessionRows(alone);
        CHECK(one.size() == 1 && one[0].id == 40 && one[0].isRestorable() && one[0].title.empty());
    }

    const std::vector<SessionRow> rows = sessionRows(tree);
    REQUIRE(rows.size() == 4);
    CHECK_MSG(rows[0].sessionID == 70 && !rows[0].isRestorable(), "what runs is listed first");
    CHECK(rows[1].id == 40 && rows[2].id == 60 && rows[3].id == 900);
    const SessionRow& kept = rows[2];
    CHECK(kept.isRestorable() && kept.sessionID == 0 && kept.workspaceID == 60);
    CHECK(kept.workspaceName == "kept" && kept.workspaceLabel == "kept label");
    CHECK_MSG(kept.title == "tail -f" && kept.currentDirectory == "/var/log" && kept.columns == 100 && kept.rows == 30 && kept.label == "tailer" && kept.paneLabel == "left", "the row is where its first pane was");
    CHECK_MSG(kept.windowID == 0 && kept.tabID == 0 && kept.paneID == 0 && kept.attendeeCount == 0 && !kept.isAtPrompt() && kept.foregroundProcess.empty(), "nothing runs in it: no identity below the workspace, nobody attached, no program");
    CHECK_MSG(rows[1].isRestorable() && rows[1].title.empty() && rows[1].columns == 0, "a record that holds no pane is still a row");
}

UBIQUITTY_TEST_MAIN()
