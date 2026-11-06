// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_layouttree.cpp — host coverage for the Core-owned live presentation
// topology (Source/Core/Session/LayoutTree.{h,cpp}), the session-model.md tree:
// Workspace → Surface → Tab → Panes::Tree → Pane.
//
// Bare libc++, no platform include. Pins the invariants a frontend projection
// relies on: a fresh workspace is 1 surface / 1 tab / 1 pane; add/close keep the
// active selection sane and ids monotonic + never reused; split/close pane
// delegate to Panes::Tree and collapse the tab on its last pane; the fixed caps
// (16 surfaces / 32 tabs / 64 panes) hold; unknown ids are rejected.

#include "Core/Panes/DividerPath.h"
#include "Core/Session/LayoutTree.h"

#include <cstdio>
#include <memory>
#include <span>

using Ubiquitty::Session::Layout::invalidPane;
using Ubiquitty::Session::Layout::invalidSurface;
using Ubiquitty::Session::Layout::invalidTab;
using Ubiquitty::Session::Layout::PaneID;
using Ubiquitty::Session::Layout::SurfaceID;
using Ubiquitty::Session::Layout::TabID;
using Ubiquitty::Session::Layout::Workspace;
using Orientation = Ubiquitty::Panes::Orientation;
using InsertSide = Ubiquitty::Panes::InsertSide;

// The Workspace pre-allocates its caps (≈1.5 MB), so own it on the heap.
static std::unique_ptr<Workspace> freshWorkspace() { return std::make_unique<Workspace>(); }

TEST(dividerPathsNameTopologyRatherThanPoolIndices)
{
    namespace Panes = Ubiquitty::Panes;
    Panes::Tree tree(1);
    CHECK(!Panes::dividerPath(tree, tree.root()));
    REQUIRE(tree.split(Orientation::Vertical, 2) != Panes::invalidNode);
    REQUIRE(tree.split(Orientation::Horizontal, 3) != Panes::invalidNode);
    const auto root = tree.root();
    const auto nested = tree.node(root).second;
    REQUIRE(!tree.isLeaf(nested));
    const auto rootPath = Panes::dividerPath(tree, root);
    const auto nestedPath = Panes::dividerPath(tree, nested);
    REQUIRE(rootPath && nestedPath);
    CHECK(rootPath->length == 0 && nestedPath->length == 1 && nestedPath->children[0] == 1);
    CHECK(Panes::dividerAtPath(tree, {}) == root);
    CHECK(Panes::dividerAtPath(tree, std::span { nestedPath->children }.first(nestedPath->length)) == nested);
    CHECK(!Panes::dividerPath(tree, -1) && !Panes::dividerPath(tree, Panes::maxNodes));
    for (const auto path : { std::array<std::uint8_t, 2> { 0, 0 }, { 1, 1 }, { 1, 2 } })
        CHECK(!Panes::dividerAtPath(tree, path));
    const std::array<std::uint8_t, Panes::maxPanes> tooDeep {};
    CHECK(!Panes::dividerAtPath(tree, tooDeep));
    CHECK(tree.closePane(1));
    REQUIRE(tree.split(Orientation::Vertical, 4) != Panes::invalidNode);
    const auto replacement = tree.node(tree.root()).second;
    const auto replacementPath = Panes::dividerPath(tree, replacement);
    REQUIRE(replacementPath);
    CHECK(replacementPath->length == 1 && replacementPath->children[0] == 1);
    CHECK(Panes::dividerAtPath(tree, std::span { replacementPath->children }.first(1)) == replacement);
}

TEST(explicitTabSizingLeavesActiveSelectionAndNeighborsAlone)
{
    auto workspace = freshWorkspace();
    const auto window = workspace->activeSurface();
    const auto tab = workspace->activeTab(window);
    const auto first = workspace->focusedPane(window);
    REQUIRE(workspace->splitFocusedPane(window, Orientation::Vertical, 0.3f) != invalidPane);
    const auto* tree = workspace->paneTree(window, tab);
    REQUIRE(tree);
    const auto root = tree->root();
    const auto hiddenFocus = tree->focusedPaneID();
    const auto shown = workspace->addTab(window);
    REQUIRE(shown != invalidTab);
    const auto focus = workspace->focusedPane(window);
    REQUIRE(workspace->setTabDividerRatio(window, tab, root, 0.4f));
    CHECK(tree->node(root).ratio == 0.4f);
    REQUIRE(workspace->adjustPaneRatio(window, tab, first, Ubiquitty::Panes::Direction::Right, 0.1f));
    CHECK(tree->node(root).ratio == 0.5f);
    REQUIRE(workspace->balanceTab(window, tab));
    CHECK(tree->node(root).ratio == 0.5f && tree->focusedPaneID() == hiddenFocus);
    CHECK(workspace->activeTab(window) == shown && workspace->focusedPane(window) == focus);
    CHECK(workspace->paneTree(window, shown)->paneCount() == 1);
    CHECK(!workspace->balanceTab(SurfaceID { 999 }, tab));
    CHECK(!workspace->adjustPaneRatio(window, shown, first, Ubiquitty::Panes::Direction::Left, 0.1f));
    CHECK(!workspace->setTabDividerRatio(window, shown, root, 0.2f));
}

TEST(tabExtractionRefusesExhaustedWindowIdentityWithoutMutation)
{
    namespace Identity = Ubiquitty::Session::Identity;
    auto identities = std::make_shared<Identity::Allocator>();
    auto workspace = std::make_unique<Workspace>(identities);
    const auto window = workspace->activeSurface();
    const auto original = workspace->activeTab(window);
    const auto added = workspace->addTab(window);
    REQUIRE(added != invalidTab);
    const auto pane = workspace->focusedPane(window);
    identities->reserveThrough({ Identity::ObjectKind::Window, UINT32_MAX });
    CHECK(workspace->extractTabToSurface(window, original) == invalidSurface);
    CHECK(workspace->surfaceCount() == 1);
    CHECK(workspace->tabCount(window) == 2);
    CHECK(workspace->activeSurface() == window);
    CHECK(workspace->activeTab(window) == added);
    CHECK(workspace->focusedPane(window) == pane);
    CHECK(workspace->paneTree(window, original));
}

TEST(windowExtractionsRefuseEachExhaustedDestinationIdentity)
{
    namespace Identity = Ubiquitty::Session::Identity;
    for (const auto kind : { Identity::ObjectKind::Window, Identity::ObjectKind::Tab, Identity::ObjectKind::Pane }) {
        for (const auto scope : { Identity::ObjectKind::Tab, Identity::ObjectKind::Pane }) {
            auto identities = std::make_shared<Identity::Allocator>();
            auto workspace = std::make_unique<Workspace>(identities);
            const auto window = workspace->activeSurface();
            const auto source = workspace->activeTab(window);
            const auto pane = workspace->focusedPane(window);
            const auto active = workspace->addTab(window);
            REQUIRE(active != invalidTab);
            const auto focused = workspace->focusedPane(window);
            identities->reserveThrough({ kind, UINT32_MAX });
            CHECK((scope == Identity::ObjectKind::Tab ? workspace->extractTabToSurface(window, source)
                                                      : workspace->extractPaneToSurface(window, pane))
                == invalidSurface);
            CHECK(workspace->surfaceCount() == 1 && workspace->tabCount(window) == 2);
            CHECK(workspace->activeSurface() == window && workspace->activeTab(window) == active);
            CHECK(workspace->focusedPane(window) == focused);
            CHECK(workspace->surfaceHolding(pane) == window && workspace->tabHolding(window, pane) == source);
            CHECK(workspace->surfaceHolding(focused) == window && workspace->tabHolding(window, focused) == active);
        }
    }
}

// toggleZoom records / clears a per-surface zoomed pane (model-only intent the
// frontend realizes), with the same guards as the macOS path.
// Closing the zoomed pane clears the zoom; collapsing back to a single pane
// also clears it.
// A zoom belongs to the active tab: switching tabs clears it; an unknown
// surface reports no zoom and refuses toggles.
// The macOS Chunk-B close path: a per-pane title-bar close can target any
// pane, so the frontend focusPane(closingID) then closeFocusedPane(). This
// exercises that exact sequence through split -> moveFocus -> close-collapse ->
// last-pane escalation, pinning: ids are monotonic + never reused across the
// whole run; focusPane(closingID) only succeeds for a live leaf (the close-the-
// right-pane guard); closing a pane from elsewhere lands focus on the survivor;
// collapsing to one pane and then closing the last pane escalates (fails, keeps
// >=1 tab); and a zoom set on one pane auto-clears when a DIFFERENT pane's close
// collapses the layout.
// Zoom auto-clears when a DIFFERENT pane's close collapses the layout (the macOS
// path relies on Workspace::clearZoomIfStale running inside closeFocusedPane;
// the frontend does not clear zoom by hand).
// Workspace::closePane(surface, paneID) closes a SPECIFIC pane by id, collapsing
// one split level — and (unlike closeFocusedPane) leaves keyboard focus on the
// actually-focused pane when a BACKGROUND pane closes; only when the FOCUSED pane
// itself closes does focus promote to the sibling. This is the macOS background-
// pane-close fix (a title-bar close must not steal focus from the active pane).
// closePane clears a now-stale zoom: closing the zoomed pane, or collapsing back
// to a lone pane, drops the surface zoom (same guard as closeFocusedPane).
// Workspace::adoptTree(surface, tree) replaces the active tab's tree wholesale
// (the tmux / session-restore path). It (a) clears a zoom on a pane the adopted
// tree no longer carries, and (b) advances the id allocator past every adopted id
// so a later splitFocusedPane mints a NON-colliding id.
// adoptTree scans every tab of the surface (a zoom-detached sibling or another
// tab can hold an adopted id too), and an unknown surface is rejected.
// movePane in the active tab reorients panes; a zoom survives (it names
// the pane, not the position). swapPanes trades positions, focus follows.
// movePaneToTab relocates a pane between tabs: the target tree gains it,
// the source collapses (or closes when emptied), the active tab stays.
// A zoomed pane that leaves the active tab drops the zoom.
// extractPaneToTab roots a NEW active tab at the existing pane id and
// collapses the source; the pane keeps its identity, ids stay monotonic.
// extractPaneToSurface seeds extract-to-window: a new ACTIVE surface
// rooted at the pane; the source collapses (closing an emptied tab).
// movePaneToTab's same-tab branch rearranges a NON-active tab in place;
// a same-tab request whose target lives elsewhere refuses untouched.
// The at-cap refusals hold at their exact boundaries, and a refused
// cross-tab move leaves the source tree intact (insert-before-detach).
// The remaining zoom crossings: swap keeps the zoom on the pane id; a
// surface-extraction of the zoomed pane clears it; a move that collapses
// the active tab to a lone pane clears it via the lone-pane rule.
// Round-trip: extract a pane to its own tab, then move it back — the
// emptied tab closes and the pane rejoins its origin.
// The new ops interoperate with adoptTree (the tmux/session-restore
// wholesale swap that injects foreign ids + reconciles the allocator):
// id monotonicity survives extract's mint-discard on an adopted tree, and
// a zoom on an adopted pane clears when that pane leaves the active tab.
// (Spec t6: adoptTree interaction.)
// balanceActiveTab equalizes the tab's splits; adjustFocusedPaneRatio
// nudges the focused pane's divider.
TEST(testFreshIsOneOfEach)
{
    auto ws = freshWorkspace();
    CHECK(ws->surfaceCount() == 1);
    const SurfaceID s = ws->activeSurface();
    CHECK(s != invalidSurface);
    CHECK(ws->surfaceIDAt(0) == s);
    CHECK(ws->surfaceIDAt(1) == invalidSurface);
    CHECK(ws->tabCount(s) == 1);
    CHECK(ws->activeTab(s) != invalidTab);
    CHECK(ws->focusedPane(s) != invalidPane);
    CHECK(ws->activePaneTree(s) != nullptr);
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
}

TEST(testSurfaceAddCloseActive)
{
    auto ws = freshWorkspace();
    const SurfaceID s0 = ws->activeSurface();
    const SurfaceID s1 = ws->addSurface();
    CHECK(s1 != invalidSurface);
    CHECK(s1 != s0);
    CHECK(s1 > s0); // ids monotonic
    CHECK(ws->surfaceCount() == 2);
    CHECK(ws->activeSurface() == s1); // a new surface becomes active

    CHECK(ws->setActiveSurface(s0));
    CHECK(ws->activeSurface() == s0);
    CHECK(!ws->setActiveSurface(SurfaceID { 99999 })); // unknown id rejected

    // Closing the active surface re-homes active onto a neighbor.
    CHECK(ws->setActiveSurface(s1));
    CHECK(ws->closeSurface(s1));
    CHECK(ws->surfaceCount() == 1);
    CHECK(ws->activeSurface() == s0);
    CHECK(!ws->closeSurface(s0)); // can't close the last surface
}

TEST(testSurfaceCapacityAndMonotonic)
{
    auto ws = freshWorkspace();
    SurfaceID last = ws->activeSurface();
    for (int i = 1; i < Ubiquitty::Session::Layout::maxSurfaces; ++i) {
        const SurfaceID s = ws->addSurface();
        CHECK(s != invalidSurface);
        CHECK(s > last); // strictly increasing, never reused
        last = s;
    }
    CHECK(ws->surfaceCount() == Ubiquitty::Session::Layout::maxSurfaces);
    CHECK(ws->addSurface() == invalidSurface); // at the cap

    // Close one, then add: the new id is past every prior id (no reuse).
    const SurfaceID reopened = ws->surfaceIDAt(1);
    CHECK(ws->closeSurface(reopened));
    const SurfaceID s = ws->addSurface();
    CHECK(s != invalidSurface);
    CHECK(s > last);
}

TEST(testTabAddCloseActive)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const TabID t0 = ws->activeTab(s);
    const TabID t1 = ws->addTab(s);
    CHECK(t1 != invalidTab);
    CHECK(t1 != t0);
    CHECK(t1 > t0);
    CHECK(ws->tabCount(s) == 2);
    CHECK(ws->activeTab(s) == t1); // a new tab becomes active

    CHECK(ws->setActiveTab(s, t0));
    CHECK(ws->activeTab(s) == t0);
    CHECK(!ws->setActiveTab(s, TabID { 99999 }));

    CHECK(ws->closeTab(s, t1));
    CHECK(ws->tabCount(s) == 1);
    CHECK(ws->activeTab(s) == t0);
    CHECK(!ws->closeTab(s, t0)); // can't close the last tab
}

TEST(testTabCapacity)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    for (int i = 1; i < Ubiquitty::Session::Layout::maxTabsPerSurface; ++i)
        CHECK(ws->addTab(s) != invalidTab);
    CHECK(ws->tabCount(s) == Ubiquitty::Session::Layout::maxTabsPerSurface);
    CHECK(ws->addTab(s) == invalidTab);
}

TEST(testPaneSplitFocusClose)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);

    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(p1 != invalidPane);
    CHECK(p1 != p0);
    CHECK(ws->focusedPane(s) == p1); // the new leaf is focused
    CHECK(ws->activePaneTree(s)->paneCount() == 2);

    CHECK(ws->focusPane(s, p0));
    CHECK(ws->focusedPane(s) == p0);
    CHECK(!ws->focusPane(s, PaneID { 99999 }));

    // Close the focused pane (p0): the split collapses, p1 survives + focuses.
    CHECK(ws->closeFocusedPane(s));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->focusedPane(s) == p1);

    // Closing the last pane of the surface's only tab fails (keeps ≥1 tab).
    CHECK(!ws->closeFocusedPane(s));
}

TEST(testCloseLastPaneClosesTab)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const TabID t0 = ws->activeTab(s);
    const TabID t1 = ws->addTab(s); // active tab is now t1, a single pane
    CHECK(ws->tabCount(s) == 2);
    // Closing t1's only pane closes the tab, leaving t0.
    CHECK(ws->closeFocusedPane(s));
    CHECK(ws->tabCount(s) == 1);
    CHECK(ws->activeTab(s) == t0);
    (void)t1;
}

TEST(testPaneCapacity)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    for (int i = 1; i < Ubiquitty::Session::Layout::maxPanesPerTab; ++i)
        CHECK(ws->splitFocusedPane(s, Orientation::Horizontal) != invalidPane);
    CHECK(ws->activePaneTree(s)->paneCount() == Ubiquitty::Session::Layout::maxPanesPerTab);
    CHECK(ws->splitFocusedPane(s, Orientation::Horizontal) == invalidPane);
}

TEST(testToggleZoom)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);

    // A single-pane tab has nothing to expand: toggle is refused, no zoom set.
    CHECK(ws->zoomedPane(s) == invalidPane);
    CHECK(!ws->toggleZoom(s, p0));
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Split, then zoom the new pane.
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(p1 != invalidPane);
    CHECK(ws->toggleZoom(s, p1));
    CHECK(ws->zoomedPane(s) == p1);

    // Toggling the same pane again clears the zoom.
    CHECK(ws->toggleZoom(s, p1));
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Zooming a different live pane switches the zoom to it.
    CHECK(ws->toggleZoom(s, p0));
    CHECK(ws->zoomedPane(s) == p0);
    CHECK(ws->toggleZoom(s, p1)); // re-target, not toggle-off
    CHECK(ws->zoomedPane(s) == p1);

    // A pane that isn't a live leaf of the active tab is rejected.
    CHECK(!ws->toggleZoom(s, PaneID { 99999 }));
    CHECK(ws->zoomedPane(s) == p1); // unchanged
    CHECK(!ws->toggleZoom(s, invalidPane));
    CHECK(ws->zoomedPane(s) == p1);
}

TEST(testZoomClearedOnClose)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical); // focus p1
    const PaneID p2 = ws->splitFocusedPane(s, Orientation::Horizontal); // focus p2
    CHECK(ws->activePaneTree(s)->paneCount() == 3);

    // Zoom p2, then close it (it is focused): zoom clears.
    CHECK(ws->toggleZoom(s, p2));
    CHECK(ws->zoomedPane(s) == p2);
    CHECK(ws->closeFocusedPane(s)); // closes p2
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Now 2 panes remain. Zoom p0, then close the other pane until one remains:
    // collapsing to a single pane clears the zoom.
    CHECK(ws->activePaneTree(s)->paneCount() == 2);
    CHECK(ws->toggleZoom(s, p0));
    CHECK(ws->zoomedPane(s) == p0);
    CHECK(ws->focusPane(s, p1));
    CHECK(ws->closeFocusedPane(s)); // closes p1 → only p0 left
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->zoomedPane(s) == invalidPane);
    (void)p1;
}

TEST(testZoomPerActiveTab)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const TabID t0 = ws->activeTab(s);
    ws->splitFocusedPane(s, Orientation::Vertical);
    const PaneID p1 = ws->focusedPane(s);
    CHECK(ws->toggleZoom(s, p1));
    CHECK(ws->zoomedPane(s) == p1);

    // Opening a new tab makes it active → the prior tab's zoom is cleared.
    const TabID t1 = ws->addTab(s);
    CHECK(t1 != invalidTab);
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Switching back to t0 does not resurrect the zoom (cleared on tab change).
    CHECK(ws->setActiveTab(s, t0));
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Unknown surface: no zoom, toggle refused.
    CHECK(ws->zoomedPane(SurfaceID { 99999 }) == invalidPane);
    CHECK(!ws->toggleZoom(SurfaceID { 99999 }, p1));
}

TEST(testSplitMovefocusCloseCollapseSequence)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);

    // Split twice: p0 -> {p0, p1} -> focus p1 -> {p0, p1, p2}. Monotonic ids.
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical);
    const PaneID p2 = ws->splitFocusedPane(s, Orientation::Horizontal);
    CHECK(p1 > p0 && p2 > p1); // strictly increasing
    CHECK(ws->activePaneTree(s)->paneCount() == 3);
    CHECK(ws->focusedPane(s) == p2); // the newest leaf is focused

    // moveFocus walks the tree; whatever it lands on must be a real, distinct leaf.
    const PaneID beforeMove = ws->focusedPane(s);
    if (ws->moveFocus(s, Ubiquitty::Panes::Direction::Left)) {
        const PaneID afterMove = ws->focusedPane(s);
        CHECK(afterMove != beforeMove);
        CHECK(afterMove == p0 || afterMove == p1 || afterMove == p2);
    }

    // Close p1 from elsewhere (focus p0 first, the macOS title-bar-close idiom):
    // focusPane(closingID) must succeed for the live leaf, then closeFocusedPane
    // collapses p1's split level and focus lands on a survivor (p0 or p2).
    CHECK(ws->focusPane(s, p1)); // p1 is a live leaf
    CHECK(ws->focusedPane(s) == p1);
    CHECK(ws->closeFocusedPane(s));
    CHECK(ws->activePaneTree(s)->paneCount() == 2);
    const PaneID afterClose = ws->focusedPane(s);
    CHECK(afterClose == p0 || afterClose == p2);
    CHECK(afterClose != p1); // never the closed pane

    // The close-the-RIGHT-pane guard: focusPane on the now-dead p1 fails, so the
    // frontend would refuse the close rather than collapse the wrong (focused) pane.
    CHECK(!ws->focusPane(s, p1));

    // Collapse to one pane: close the other survivor. paneCount -> 1.
    const PaneID survivor = afterClose;
    const PaneID other = (survivor == p0) ? p2 : p0;
    CHECK(ws->focusPane(s, other));
    CHECK(ws->closeFocusedPane(s));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->focusedPane(s) == survivor);

    // Last-pane escalation: closing the surface's only tab's last pane fails
    // (a surface always keeps >=1 tab).
    CHECK(!ws->closeFocusedPane(s));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);

    // No id was reused anywhere: the next split mints an id past every prior one.
    const PaneID p3 = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(p3 > p2);
    CHECK(p3 != p0 && p3 != p1 && p3 != p2);
}

TEST(testZoomAutoclearsOnCloseFromElsewhere)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical); // {p0, p1}

    // Zoom p0, then close p1 (a DIFFERENT pane) — the collapse to a lone pane
    // clears the zoom even though the zoomed pane itself didn't close.
    CHECK(ws->toggleZoom(s, p0));
    CHECK(ws->zoomedPane(s) == p0);
    CHECK(ws->focusPane(s, p1));
    CHECK(ws->closeFocusedPane(s)); // closes p1 -> only p0 remains
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->zoomedPane(s) == invalidPane); // auto-cleared, no manual clear
}

TEST(testClosePaneByIdKeepsFocusPut)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical); // focus p1
    const PaneID p2 = ws->splitFocusedPane(s, Orientation::Horizontal); // focus p2
    CHECK(ws->activePaneTree(s)->paneCount() == 3);
    CHECK(ws->focusedPane(s) == p2); // newest leaf focused

    // Close a BACKGROUND pane (p0, not focused): focus must not move — it stays
    // on p2, the pane the user was actually working in.
    CHECK(ws->closePane(s, p0));
    CHECK(ws->activePaneTree(s)->paneCount() == 2);
    CHECK(ws->focusedPane(s) == p2); // focus stayed put on the active pane

    // Closing a non-live id is a no-op (false), focus unchanged.
    CHECK(!ws->closePane(s, p0)); // already closed
    CHECK(!ws->closePane(s, PaneID { 99999 })); // never existed
    CHECK(ws->focusedPane(s) == p2);

    // Now close the FOCUSED pane (p2): the sibling (p1) is promoted to focus.
    CHECK(ws->closePane(s, p2));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->focusedPane(s) == p1); // sibling promoted

    // closePane refuses the last pane (a lone pane routes through the tab/window
    // close path, not this) — paneCount stays 1, focus unchanged.
    CHECK(!ws->closePane(s, p1));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->focusedPane(s) == p1);
    (void)p0;
}

TEST(testClosePaneByIdClearsZoom)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical);
    const PaneID p2 = ws->splitFocusedPane(s, Orientation::Horizontal);

    // Zoom a BACKGROUND pane (p0), then close p0 by id: the zoomed pane closed,
    // so the zoom clears even though it wasn't focused.
    CHECK(ws->toggleZoom(s, p0));
    CHECK(ws->zoomedPane(s) == p0);
    CHECK(ws->closePane(s, p0));
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Two panes (p1, p2) remain. Zoom p1, then close p2 by id — the collapse to a
    // lone pane clears the zoom even though the zoomed pane didn't close.
    CHECK(ws->activePaneTree(s)->paneCount() == 2);
    CHECK(ws->toggleZoom(s, p1));
    CHECK(ws->zoomedPane(s) == p1);
    CHECK(ws->closePane(s, p2));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->zoomedPane(s) == invalidPane);
}

TEST(testAdoptTreeClearsStaleZoomAndAdvancesAllocator)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID p0 = ws->focusedPane(s);
    const PaneID p1 = ws->splitFocusedPane(s, Orientation::Vertical); // {p0, p1}, focus p1
    CHECK(ws->activePaneTree(s)->paneCount() == 2);

    // Zoom p1, then adopt a fresh tree that does not contain p1 — the zoom must
    // be dropped (the zoomed pane is no longer a live leaf of the adopted tree).
    CHECK(ws->toggleZoom(s, p1));
    CHECK(ws->zoomedPane(s) == p1);

    // Adopt a tree carrying ids well ABOVE the current allocator (as a tmux %M
    // injection would): a single-leaf tree rooted at a high id.
    const int adoptedHigh = 500;
    CHECK(ws->adoptTree(s, Ubiquitty::Panes::Tree(adoptedHigh)));
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(static_cast<int>(ws->focusedPane(s)) == adoptedHigh); // adopted tree's focus
    CHECK(ws->zoomedPane(s) == invalidPane); // stale zoom on p1 cleared

    // The allocator was reconciled past the adopted id: a later split mints an id
    // STRICTLY GREATER than the injected one, so it can never collide with it.
    const PaneID minted = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(minted != invalidPane);
    CHECK(static_cast<int>(minted) > adoptedHigh);
    CHECK(minted != static_cast<PaneID>(adoptedHigh));
    (void)p0;
    (void)p1;
}

TEST(testAdoptTreeScansAllTabsAndRejectsUnknown)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const TabID t0 = ws->activeTab(s);

    // Adopt a high id into t0, then open t1 and adopt a LOWER (but still high) id.
    CHECK(ws->adoptTree(s, Ubiquitty::Panes::Tree(800)));
    const TabID t1 = ws->addTab(s);
    CHECK(t1 != invalidTab);
    CHECK(ws->adoptTree(s, Ubiquitty::Panes::Tree(300)));

    // The allocator must be past the HIGHEST live id across all tabs (800 in t0),
    // not just the active tab's (300) — a later split must clear 800.
    const PaneID minted = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(minted != invalidPane);
    CHECK(static_cast<int>(minted) > 800);

    // Unknown surface: adopt is rejected.
    CHECK(!ws->adoptTree(SurfaceID { 99999 }, Ubiquitty::Panes::Tree(1)));
    (void)t0;
}

TEST(testUnknownSurfaceRejected)
{
    auto ws = freshWorkspace();
    CHECK(ws->surface(SurfaceID { 99999 }) == nullptr);
    CHECK(ws->tabCount(SurfaceID { 99999 }) == 0);
    CHECK(ws->activeTab(SurfaceID { 99999 }) == invalidTab);
    CHECK(ws->addTab(SurfaceID { 99999 }) == invalidTab);
    CHECK(ws->focusedPane(SurfaceID { 99999 }) == invalidPane);
    CHECK(ws->splitFocusedPane(SurfaceID { 99999 }, Orientation::Vertical) == invalidPane);
    CHECK(!ws->closeFocusedPane(SurfaceID { 99999 }));
    CHECK(!ws->closePane(SurfaceID { 99999 }, PaneID { 1 }));
    CHECK(!ws->adoptTree(SurfaceID { 99999 }, Ubiquitty::Panes::Tree(1)));
    CHECK(ws->activePaneTree(SurfaceID { 99999 }) == nullptr);
}

TEST(testMoveAndSwapInActiveTab)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID first = ws->focusedPane(s);
    const PaneID second = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(second != invalidPane);

    CHECK(ws->toggleZoom(s, first));
    CHECK(ws->movePane(s, first, second, Orientation::Horizontal, InsertSide::First));
    CHECK(ws->zoomedPane(s) == first); // zoom survived the move
    const auto* tree = ws->activePaneTree(s);
    CHECK(tree != nullptr);
    CHECK(tree->node(tree->root()).orientation == Orientation::Horizontal);
    CHECK(static_cast<PaneID>(tree->node(tree->node(tree->root()).first).paneID) == first);

    // Swap: positions trade, focus follows the focused pane's id.
    const PaneID focusedBefore = ws->focusedPane(s);
    CHECK(ws->swapPanes(s, first, second));
    CHECK(ws->focusedPane(s) == focusedBefore);
    CHECK(!ws->swapPanes(s, first, first));
    CHECK(!ws->movePane(s, first, first, Orientation::Vertical, InsertSide::Second));
}

TEST(testMovePaneToTab)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID paneA1 = ws->focusedPane(s);
    const PaneID paneA2 = ws->splitFocusedPane(s, Orientation::Vertical);
    const TabID tabA = ws->activeTab(s);
    const TabID tabB = ws->addTab(s);
    CHECK(tabB != invalidTab);
    const PaneID paneB1 = ws->focusedPane(s); // tab B's root (B is active)
    CHECK(ws->setActiveTab(s, tabA));

    // Move A2 into tab B next to B1. Active tab stays A.
    CHECK(ws->movePaneToTab(s, paneA2, tabB, paneB1, Orientation::Vertical, InsertSide::Second));
    CHECK(ws->activeTab(s) == tabA);
    CHECK(ws->activePaneTree(s)->paneCount() == 1); // A collapsed to A1
    CHECK(ws->setActiveTab(s, tabB));
    CHECK(ws->activePaneTree(s)->paneCount() == 2); // B gained A2
    bool didMove = false;
    Ubiquitty::Panes::forEachLeaf(*ws->activePaneTree(s), [&](int paneID) {
        if (static_cast<PaneID>(paneID) == paneA2)
            didMove = true;
    });
    CHECK(didMove);

    // Moving tab A's LAST pane into B closes tab A.
    CHECK(ws->movePaneToTab(s, paneA1, tabB, paneB1, Orientation::Horizontal, InsertSide::Second));
    CHECK(ws->tabCount(s) == 1);
    CHECK(ws->activePaneTree(s)->paneCount() == 3);

    // Guards: unknown target tab / unknown pane / pane onto itself.
    CHECK(!ws->movePaneToTab(s, paneA1, TabID { 9999 }, paneB1, Orientation::Vertical, InsertSide::Second));
    CHECK(!ws->movePaneToTab(s, PaneID { 9999 }, tabB, paneB1, Orientation::Vertical, InsertSide::Second));
    CHECK(!ws->movePaneToTab(s, paneA1, tabB, paneA1, Orientation::Vertical, InsertSide::Second));
}

TEST(testMovePaneToTabClearsLeftBehindZoom)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID second = ws->splitFocusedPane(s, Orientation::Vertical);
    const TabID tabA = ws->activeTab(s);
    const TabID tabB = ws->addTab(s);
    const PaneID paneB1 = ws->focusedPane(s);
    CHECK(ws->setActiveTab(s, tabA));
    CHECK(ws->toggleZoom(s, second));
    CHECK(ws->movePaneToTab(s, second, tabB, paneB1, Orientation::Vertical, InsertSide::Second));
    CHECK(ws->zoomedPane(s) == invalidPane);
}

TEST(testMovePaneToTabSameTabNonActive)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const TabID tabA = ws->activeTab(s);
    const PaneID paneA1 = ws->focusedPane(s);
    const TabID tabB = ws->addTab(s);
    const PaneID paneB1 = ws->focusedPane(s);
    const PaneID paneB2 = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(ws->setActiveTab(s, tabA)); // operate on B while A is active

    // Rearrange within tab B: B1 moves below B2.
    CHECK(ws->movePaneToTab(s, paneB1, tabB, paneB2, Orientation::Horizontal, InsertSide::Second));
    CHECK(ws->activeTab(s) == tabA); // active tab untouched
    CHECK(ws->activePaneTree(s)->paneCount() == 1); // tab A untouched
    CHECK(ws->setActiveTab(s, tabB));
    const auto* tree = ws->activePaneTree(s);
    CHECK(tree->paneCount() == 2);
    CHECK(tree->node(tree->root()).orientation == Orientation::Horizontal);
    CHECK(static_cast<PaneID>(tree->node(tree->node(tree->root()).first).paneID) == paneB2);

    // Same-tab request naming a target from ANOTHER tab: refused, no change.
    CHECK(!ws->movePaneToTab(s, paneB2, tabB, paneA1, Orientation::Vertical, InsertSide::Second));
    CHECK(ws->activePaneTree(s)->paneCount() == 2);
}

TEST(testRearrangementCapacityBoundaries)
{
    using Ubiquitty::Session::Layout::maxPanesPerTab;
    using Ubiquitty::Session::Layout::maxSurfaces;
    using Ubiquitty::Session::Layout::maxTabsPerSurface;

    // extractPaneToTab: succeeds into the last tab slot, refuses past it.
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    ws->splitFocusedPane(s, Orientation::Vertical); // 2 panes to extract from
    while (ws->tabCount(s) < maxTabsPerSurface - 1)
        CHECK(ws->addTab(s) != invalidTab);
    CHECK(ws->setActiveTab(s, ws->surface(s)->tabs[0].id)); // back to the 2-pane tab
    const PaneID extractable = ws->focusedPane(s);
    CHECK(ws->tabCount(s) == maxTabsPerSurface - 1);
    CHECK(ws->extractPaneToTab(s, extractable) != invalidTab); // fills the last slot
    CHECK(ws->tabCount(s) == maxTabsPerSurface);
    ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(ws->extractPaneToTab(s, ws->focusedPane(s)) == invalidTab); // at cap: refused

    // movePaneToTab: destination at the pane cap refuses and the source
    // still holds the pane.
    auto ws2 = freshWorkspace();
    const SurfaceID s2 = ws2->activeSurface();
    const TabID full = ws2->activeTab(s2);
    while (ws2->activePaneTree(s2)->paneCount() < maxPanesPerTab)
        CHECK(ws2->splitFocusedPane(s2, Orientation::Vertical) != invalidPane);
    const PaneID fullRoot = ws2->focusedPane(s2);
    const TabID other = ws2->addTab(s2);
    CHECK(other != invalidTab);
    const PaneID mover = ws2->focusedPane(s2);
    const PaneID sibling = ws2->splitFocusedPane(s2, Orientation::Vertical);
    CHECK(sibling != invalidPane);
    CHECK(!ws2->movePaneToTab(s2, mover, full, fullRoot, Orientation::Vertical, InsertSide::Second));
    CHECK(ws2->activePaneTree(s2)->paneCount() == 2); // source untouched
    bool isStillPresent = false;
    Ubiquitty::Panes::forEachLeaf(*ws2->activePaneTree(s2), [&](int paneID) {
        if (static_cast<PaneID>(paneID) == mover)
            isStillPresent = true;
    });
    CHECK(isStillPresent);

    // extractPaneToSurface at the surface cap: refused, source intact.
    auto ws3 = freshWorkspace();
    const SurfaceID s3 = ws3->activeSurface();
    while (ws3->surfaceCount() < maxSurfaces)
        CHECK(ws3->addSurface() != invalidSurface);
    CHECK(ws3->setActiveSurface(s3));
    ws3->splitFocusedPane(s3, Orientation::Vertical);
    CHECK(ws3->extractPaneToSurface(s3, ws3->focusedPane(s3)) == invalidSurface);
    CHECK(ws3->activePaneTree(s3)->paneCount() == 2);
}

TEST(testZoomCrossings)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID zoomed = ws->focusedPane(s);
    const PaneID other = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(ws->toggleZoom(s, zoomed));
    CHECK(ws->swapPanes(s, zoomed, other));
    CHECK(ws->zoomedPane(s) == zoomed); // zoom names the pane, not the slot

    // Extract the ZOOMED pane to a surface: the source's zoom clears.
    const SurfaceID created = ws->extractPaneToSurface(s, zoomed);
    CHECK(created != invalidSurface);
    CHECK(ws->zoomedPane(s) == invalidPane);

    // Move the NON-zoomed pane away, collapsing the active tab to one:
    // the lone-pane rule clears the zoom.
    auto ws2 = freshWorkspace();
    const SurfaceID sz = ws2->activeSurface();
    const PaneID keep = ws2->focusedPane(sz);
    const PaneID leave = ws2->splitFocusedPane(sz, Orientation::Vertical);
    const TabID tabA = ws2->activeTab(sz);
    const TabID tabB = ws2->addTab(sz);
    const PaneID anchor = ws2->focusedPane(sz);
    CHECK(ws2->setActiveTab(sz, tabA));
    CHECK(ws2->toggleZoom(sz, keep));
    CHECK(ws2->movePaneToTab(sz, leave, tabB, anchor, Orientation::Vertical, InsertSide::Second));
    CHECK(ws2->zoomedPane(sz) == invalidPane); // lone survivor: nothing to zoom
}

TEST(testExtractThenMoveBackRoundTrip)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID anchor = ws->focusedPane(s);
    const PaneID wanderer = ws->splitFocusedPane(s, Orientation::Vertical);
    const TabID origin = ws->activeTab(s);

    const TabID created = ws->extractPaneToTab(s, wanderer);
    CHECK(created != invalidTab);
    CHECK(ws->tabCount(s) == 2);
    CHECK(ws->movePaneToTab(s, wanderer, origin, anchor, Orientation::Vertical, InsertSide::Second));
    CHECK(ws->tabCount(s) == 1); // the emptied extraction tab closed
    CHECK(ws->activeTab(s) == origin);
    CHECK(ws->activePaneTree(s)->paneCount() == 2);
    // A later split still mints a globally fresh id.
    const PaneID minted = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(minted != invalidPane && minted != anchor && minted != wanderer);
}

TEST(testExtractPaneToTab)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    const PaneID first = ws->focusedPane(s);
    const PaneID second = ws->splitFocusedPane(s, Orientation::Vertical);
    const TabID sourceTab = ws->activeTab(s);

    const TabID created = ws->extractPaneToTab(s, second);
    CHECK(created != invalidTab);
    CHECK(ws->activeTab(s) == created); // the new tab is active
    CHECK(ws->focusedPane(s) == second); // rooted at the same pane id
    CHECK(ws->activePaneTree(s)->paneCount() == 1);
    CHECK(ws->setActiveTab(s, sourceTab));
    CHECK(ws->activePaneTree(s)->paneCount() == 1); // source collapsed
    CHECK(ws->focusedPane(s) == first);

    // A lone pane is already tab-shaped.
    CHECK(ws->extractPaneToTab(s, first) == invalidTab);
    // A fresh split after the extraction mints a fresh, unique id.
    const PaneID third = ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(third != invalidPane && third != first && third != second);
    // Zoomed pane extraction drops the zoom.
    CHECK(ws->toggleZoom(s, third));
    CHECK(ws->extractPaneToTab(s, third) != invalidTab);
    CHECK(ws->zoomedPane(s) == invalidPane);
}

TEST(testExtractPaneToSurface)
{
    auto ws = freshWorkspace();
    const SurfaceID source = ws->activeSurface();
    const PaneID first = ws->focusedPane(source);
    // Already surface-shaped: the only pane of the only tab.
    CHECK(ws->extractPaneToSurface(source, first) == invalidSurface);

    const PaneID second = ws->splitFocusedPane(source, Orientation::Vertical);
    const SurfaceID created = ws->extractPaneToSurface(source, second);
    CHECK(created != invalidSurface);
    CHECK(ws->activeSurface() == created);
    CHECK(ws->focusedPane(created) == second);
    CHECK(ws->activePaneTree(created)->paneCount() == 1);
    CHECK(ws->activePaneTree(source)->paneCount() == 1);

    // Lone pane in a NON-lone tab extracts, closing its emptied source tab.
    const TabID extraTab = ws->addTab(source);
    CHECK(extraTab != invalidTab);
    CHECK(ws->setActiveSurface(source));
    const PaneID lone = ws->focusedPane(source); // extraTab's root (active)
    CHECK(ws->tabCount(source) == 2);
    const SurfaceID third = ws->extractPaneToSurface(source, lone);
    CHECK(third != invalidSurface);
    CHECK(ws->tabCount(source) == 1); // emptied tab closed
    CHECK(ws->focusedPane(third) == lone);

    // A later split on the new surface mints a globally fresh id.
    const PaneID minted = ws->splitFocusedPane(third, Orientation::Vertical);
    CHECK(minted != invalidPane && minted != first && minted != second && minted != lone);
}

TEST(testNewOpsAfterAdoptTree)
{
    // A 3-pane tree carrying HIGH ids, as a tmux %M injection would.
    auto buildAdopted = [] {
        Ubiquitty::Panes::Tree adopted(500);
        adopted.split(Orientation::Vertical, 501);
        adopted.split(Orientation::Horizontal, 502);
        return adopted;
    };

    // extractPaneToSurface after adopt: the addSurface mint-discard + the
    // preserved adopted id keep the allocator strictly monotone past every
    // injected id (500/501/502).
    {
        auto ws = freshWorkspace();
        const SurfaceID s = ws->activeSurface();
        CHECK(ws->adoptTree(s, buildAdopted()));
        CHECK(ws->activePaneTree(s)->paneCount() == 3);
        const SurfaceID created = ws->extractPaneToSurface(s, PaneID { 501 });
        CHECK(created != invalidSurface);
        CHECK(ws->focusedPane(created) == PaneID { 501 }); // the pane keeps its adopted id
        CHECK(ws->activePaneTree(s)->paneCount() == 2); // source collapsed
        const PaneID mintedNew = ws->splitFocusedPane(created, Orientation::Vertical);
        CHECK(mintedNew != invalidPane && static_cast<int>(mintedNew) > 502);
        const PaneID mintedSource = ws->splitFocusedPane(s, Orientation::Vertical);
        CHECK(mintedSource != invalidPane && static_cast<int>(mintedSource) > 502 && mintedSource != mintedNew);
    }

    // Zoom an ADOPTED pane, then move it out of the active tab: the zoom on
    // the departed pane clears via clearZoomIfStale.
    {
        auto ws = freshWorkspace();
        const SurfaceID s = ws->activeSurface();
        CHECK(ws->adoptTree(s, buildAdopted()));
        const TabID adoptedTab = ws->activeTab(s);
        const TabID other = ws->addTab(s);
        const PaneID anchor = ws->focusedPane(s); // the new tab's root
        CHECK(ws->setActiveTab(s, adoptedTab));
        CHECK(ws->toggleZoom(s, PaneID { 502 }));
        CHECK(ws->zoomedPane(s) == PaneID { 502 });
        CHECK(ws->movePaneToTab(s, PaneID { 502 }, other, anchor, Orientation::Vertical, InsertSide::Second));
        CHECK(ws->zoomedPane(s) == invalidPane);
    }
}

TEST(testBalanceAndAdjust)
{
    auto ws = freshWorkspace();
    const SurfaceID s = ws->activeSurface();
    ws->splitFocusedPane(s, Orientation::Vertical);
    ws->splitFocusedPane(s, Orientation::Vertical);
    CHECK(ws->balanceActiveTab(s));
    const auto* tree = ws->activePaneTree(s);
    CHECK(tree->node(tree->root()).ratio > 0.332f && tree->node(tree->root()).ratio < 0.334f);

    CHECK(ws->adjustFocusedPaneRatio(s, Ubiquitty::Panes::Direction::Right, 0.1f));
    CHECK(!ws->balanceActiveTab(SurfaceID { 9999 }));
    CHECK(!ws->adjustFocusedPaneRatio(SurfaceID { 9999 }, Ubiquitty::Panes::Direction::Right, 0.1f));
}

TEST(testRemovePane)
{
    auto ws = freshWorkspace();
    const SurfaceID first = ws->activeSurface();
    const PaneID rootPane = ws->focusedPane(first);

    // A split collapses wherever the pane is: here in a tab that is not active,
    // on a surface that is not active, and neither active choice moves.
    const TabID secondTab = ws->addTab(first);
    const PaneID inSecondTab = ws->splitFocusedPane(first, Orientation::Vertical);
    CHECK(inSecondTab != invalidPane);
    const SurfaceID second = ws->addSurface();
    CHECK(ws->setActiveSurface(second));
    CHECK(ws->setActiveTab(first, ws->tabAt(first, 0)));
    CHECK(ws->removePane(inSecondTab));
    CHECK(ws->paneTree(first, secondTab)->paneCount() == 1);
    CHECK(ws->activeSurface() == second);
    CHECK(ws->activeTab(first) == ws->tabAt(first, 0));

    // A tab's last pane closes the tab; a surface's last tab closes the surface.
    CHECK(ws->removePane(static_cast<PaneID>(ws->paneTree(first, secondTab)->focusedPaneID())));
    CHECK(ws->tabCount(first) == 1);
    CHECK(ws->removePane(ws->focusedPane(second)));
    CHECK(ws->surfaceCount() == 1);
    CHECK(ws->activeSurface() == first);

    // The workspace's last pane stays, and a pane it does not hold is refused.
    CHECK(!ws->removePane(rootPane));
    CHECK(!ws->removePane(PaneID { 9999 }));
    CHECK(ws->surfaceCount() == 1 && ws->tabCount(first) == 1);
}

TEST(testWhichSurfaceAndTabHoldAPane)
{
    auto ws = freshWorkspace();
    const SurfaceID first = ws->activeSurface();
    const PaneID rootPane = ws->focusedPane(first);
    const TabID secondTab = ws->addTab(first);
    const PaneID inSecondTab = ws->focusedPane(first);
    const SurfaceID second = ws->addSurface();
    const PaneID onSecond = ws->focusedPane(second);

    CHECK(ws->surfaceHolding(rootPane) == first && ws->tabHolding(first, rootPane) == ws->tabAt(first, 0));
    CHECK(ws->surfaceHolding(inSecondTab) == first && ws->tabHolding(first, inSecondTab) == secondTab);
    CHECK(ws->surfaceHolding(onSecond) == second);
    // A pane of another surface, an unknown pane, an unknown surface.
    CHECK(ws->tabHolding(second, rootPane) == invalidTab);
    CHECK(ws->surfaceHolding(PaneID { 9999 }) == invalidSurface);
    CHECK(ws->tabHolding(SurfaceID { 9999 }, rootPane) == invalidTab);
}

TEST(testAdoptWorkspace)
{
    auto ws = freshWorkspace();
    Ubiquitty::Panes::Tree split(100);
    CHECK(split.split(Orientation::Vertical, 101) != Ubiquitty::Panes::invalidNode);
    Ubiquitty::Panes::Tree lone(200);
    const Ubiquitty::Session::Layout::AdoptedTab tabs[] = { { TabID { 5 }, &split }, { TabID { 6 }, &lone } };
    // A zoom naming a pane the active tab does not hold is dropped on the way in.
    const Ubiquitty::Session::Layout::AdoptedSurface surfaces[] = { { SurfaceID { 3 }, TabID { 6 }, PaneID { 101 }, tabs } };
    CHECK(ws->adoptWorkspace(surfaces, SurfaceID { 3 }));
    const SurfaceID adopted { 3 };
    CHECK(ws->surfaceCount() == 1 && ws->surfaceIDAt(0) == adopted && ws->activeSurface() == adopted);
    CHECK(ws->tabCount(adopted) == 2 && ws->tabAt(adopted, 0) == TabID { 5 } && ws->activeTab(adopted) == TabID { 6 });
    CHECK(ws->zoomedPane(adopted) == invalidPane);
    CHECK(ws->paneTree(adopted, TabID { 5 })->paneCount() == 2);
    CHECK(ws->focusedPane(adopted) == PaneID { 200 });

    // Every allocator is past the adopted ids.
    CHECK(std::to_underlying(ws->splitFocusedPane(adopted, Orientation::Vertical)) > 200u);
    CHECK(std::to_underlying(ws->addTab(adopted)) > 6u);
    CHECK(std::to_underlying(ws->addSurface()) > 3u);

    // Refusals leave the topology as it was.
    const auto unchanged = [&] { return ws->surfaceCount() == 2 && ws->surfaceIDAt(0) == adopted && ws->tabCount(adopted) == 3; };
    const Ubiquitty::Session::Layout::AdoptedSurface noTabs[] = { { SurfaceID { 9 }, invalidTab, invalidPane, {} } };
    CHECK(!ws->adoptWorkspace(noTabs, SurfaceID { 9 }) && unchanged());
    const Ubiquitty::Session::Layout::AdoptedSurface activeSurfaceUnlisted[] = { { SurfaceID { 9 }, TabID { 5 }, invalidPane, tabs } };
    CHECK(!ws->adoptWorkspace(activeSurfaceUnlisted, SurfaceID { 8 }) && unchanged());
    const Ubiquitty::Session::Layout::AdoptedSurface activeTabUnlisted[] = { { SurfaceID { 9 }, TabID { 7 }, invalidPane, tabs } };
    CHECK(!ws->adoptWorkspace(activeTabUnlisted, SurfaceID { 9 }) && unchanged());
    const Ubiquitty::Session::Layout::AdoptedTab noTree[] = { { TabID { 7 }, nullptr } };
    const Ubiquitty::Session::Layout::AdoptedSurface treeless[] = { { SurfaceID { 9 }, TabID { 7 }, invalidPane, noTree } };
    CHECK(!ws->adoptWorkspace(treeless, SurfaceID { 9 }) && unchanged());
    const Ubiquitty::Session::Layout::AdoptedSurface repeatedSurface[] = { { SurfaceID { 3 }, TabID { 5 }, invalidPane, std::span(tabs).first(1) }, { SurfaceID { 3 }, TabID { 6 }, invalidPane, std::span(tabs).last(1) } };
    CHECK(!ws->adoptWorkspace(repeatedSurface, SurfaceID { 3 }) && unchanged());
    const Ubiquitty::Session::Layout::AdoptedTab repeatedTab[] = { { TabID { 5 }, &split }, { TabID { 5 }, &lone } };
    const Ubiquitty::Session::Layout::AdoptedSurface repeatedTabs[] = { { SurfaceID { 3 }, TabID { 5 }, invalidPane, repeatedTab } };
    CHECK(!ws->adoptWorkspace(repeatedTabs, SurfaceID { 3 }) && unchanged());
    Ubiquitty::Panes::Tree samePane(100);
    const Ubiquitty::Session::Layout::AdoptedTab repeatedPane[] = { { TabID { 5 }, &split }, { TabID { 6 }, &samePane } };
    const Ubiquitty::Session::Layout::AdoptedSurface repeatedPanes[] = { { SurfaceID { 3 }, TabID { 5 }, invalidPane, repeatedPane } };
    CHECK(!ws->adoptWorkspace(repeatedPanes, SurfaceID { 3 }) && unchanged());
}

TEST(testMovePaneBetweenSurfacesPreservesIdentity)
{
    auto workspace = freshWorkspace();
    const auto source = workspace->activeSurface();
    const auto pane = workspace->focusedPane(source);
    const auto destination = workspace->addSurface();
    const auto target = workspace->focusedPane(destination);
    const auto tab = workspace->activeTab(destination);
    CHECK(!workspace->movePaneToTab(source, pane, tab, PaneID { 9999 }, Orientation::Horizontal, InsertSide::First));
    CHECK(workspace->surfaceHolding(pane) == source);
    CHECK(workspace->movePaneToTab(source, pane, tab, target, Orientation::Horizontal, InsertSide::First));
    CHECK(workspace->surfaceCount() == 1);
    CHECK(workspace->surfaceHolding(pane) == destination);
    CHECK(workspace->paneTree(destination, tab)->paneCount() == 2);
    CHECK(workspace->focusedPane(destination) == pane);
    CHECK(workspace->extractPaneToSurface(destination, pane) != invalidSurface);
    CHECK(workspace->surfaceCount() == 2);
}

TEST(testMoveWholeTabPreservesNestedSplitsAndClosesEmptySurface)
{
    auto workspace = freshWorkspace();
    const auto destination = workspace->activeSurface();
    const auto target = workspace->focusedPane(destination);
    const auto source = workspace->addSurface();
    const auto tab = workspace->activeTab(source);
    const auto root = workspace->focusedPane(source);
    workspace->splitFocusedPane(source, Orientation::Horizontal, 0.3f);
    const auto focused = workspace->splitFocusedPane(source, Orientation::Vertical, 0.7f);
    const auto original = *workspace->paneTree(source, tab);
    CHECK(!workspace->moveTabToPane(source, tab, root, Orientation::Vertical, InsertSide::First));
    CHECK(workspace->moveTabToPane(source, tab, target, Orientation::Vertical, InsertSide::First));
    CHECK(workspace->surfaceCount() == 1);
    const auto& tree = *workspace->activePaneTree(destination);
    CHECK(tree.paneCount() == 4);
    const auto inserted = tree.node(tree.root()).first;
    CHECK(Ubiquitty::Panes::sameShape(original, original.root(), tree, inserted));
    CHECK(tree.node(inserted).ratio == 0.3f);
    CHECK(tree.node(tree.node(inserted).second).ratio == 0.7f);
    CHECK(tree.focusedPaneID() == static_cast<int>(focused));
}

// A tab's custom title is kept as every surface keeps a typed one, whoever
// renames it: a client's verb and a restored record land here alike.
TEST(testRenameTabKeepsTheTitleAsTypedTitlesAreKept)
{
    auto workspace = freshWorkspace();
    const auto surface = workspace->activeSurface();
    const auto tab = workspace->activeTab(surface);
    CHECK(workspace->renameTab(surface, tab, "  build\x1b[31m  "));
    CHECK(workspace->tabTitle(surface, tab) == "build[31m");
    CHECK(workspace->renameTab(surface, tab, "   "));
    CHECK_MSG(workspace->tabTitle(surface, tab).empty(), "a name of spaces restores the automatic title");
}

TEST(testTransferTabAndPaneBetweenSingleTabWindows)
{
    auto workspace = freshWorkspace();
    const auto first = workspace->activeSurface();
    const auto tab = workspace->activeTab(first);
    const auto pane = workspace->focusedPane(first);
    CHECK(workspace->renameTab(first, tab, "work"));
    const auto second = workspace->addSurface();
    CHECK(workspace->moveTabToSurface(first, tab, second));
    CHECK(workspace->surfaceCount() == 1);
    CHECK(workspace->tabTitle(second, tab) == "work");
    const auto detached = workspace->extractTabToSurface(second, tab);
    CHECK(detached != invalidSurface && workspace->activeTab(detached) == tab);
    const auto sibling = workspace->splitFocusedPane(detached, Orientation::Horizontal);
    CHECK(workspace->movePaneToNewTab(detached, pane, second));
    CHECK(workspace->surfaceHolding(pane) == second);
    CHECK(workspace->surfaceHolding(sibling) == detached);
    CHECK(workspace->movePaneToNewTab(detached, sibling, second));
    CHECK(workspace->surfaceCount() == 1 && workspace->tabCount(second) == 3);
}

TEST(testFullDestinationRefusesTransferWithoutLosingSource)
{
    auto workspace = freshWorkspace();
    const auto source = workspace->activeSurface();
    const auto tab = workspace->activeTab(source);
    const auto pane = workspace->focusedPane(source);
    const auto destination = workspace->addSurface();
    for (int index = 1; index < Ubiquitty::Panes::maxPanes; ++index) {
        CHECK(workspace->splitFocusedPane(destination, Orientation::Vertical) != invalidPane);
    }
    const auto target = workspace->focusedPane(destination);
    CHECK(!workspace->moveTabToPane(source, tab, target, Orientation::Vertical, InsertSide::Second));
    CHECK(!workspace->movePaneToTab(source, pane, workspace->activeTab(destination), target, Orientation::Horizontal, InsertSide::First));
    CHECK(workspace->surfaceHolding(pane) == source);
    CHECK(workspace->paneTree(source, tab)->paneCount() == 1);
    CHECK(workspace->activePaneTree(destination)->paneCount() == Ubiquitty::Panes::maxPanes);
}

TEST(testIncomingSplitsRevealTheirDestination)
{
    auto workspace = freshWorkspace();
    const auto destination = workspace->activeSurface();
    const auto targetTab = workspace->activeTab(destination);
    const auto target = workspace->splitFocusedPane(destination, Orientation::Vertical);
    CHECK(workspace->toggleZoom(destination, target));
    const auto source = workspace->addSurface();
    const auto sourcePane = workspace->focusedPane(source);
    workspace->splitFocusedPane(source, Orientation::Vertical);
    CHECK(workspace->movePaneToTab(source, sourcePane, targetTab, target, Orientation::Horizontal, InsertSide::Second));
    CHECK(workspace->zoomedPane(destination) == invalidPane);

    CHECK(workspace->addTab(destination) != invalidTab);
    const auto remainingPane = workspace->focusedPane(source);
    CHECK(workspace->moveTabToPane(source, workspace->activeTab(source), target, Orientation::Vertical, InsertSide::First));
    CHECK(workspace->activeSurface() == destination);
    CHECK(workspace->activeTab(destination) == targetTab);
    CHECK(workspace->focusedPane(destination) == remainingPane);
}

TEST(testOuterTabDropsPreserveBothLayouts)
{
    using Ubiquitty::Panes::InsertTarget;
    for (const auto orientation : { Orientation::Horizontal, Orientation::Vertical }) {
        for (const auto side : { InsertSide::First, InsertSide::Second }) {
            auto workspace = freshWorkspace();
            const auto destination = workspace->activeSurface();
            const auto target = workspace->focusedPane(destination);
            workspace->splitFocusedPane(destination, Orientation::Vertical, 0.3f);
            const auto original = *workspace->activePaneTree(destination);
            const auto source = workspace->addSurface();
            const auto sourceTab = workspace->activeTab(source);
            workspace->splitFocusedPane(source, Orientation::Horizontal, 0.7f);
            const auto incoming = *workspace->activePaneTree(source);
            CHECK(workspace->moveTabToPane(source, sourceTab, target, orientation, side, InsertTarget::Root));
            const auto& tree = *workspace->activePaneTree(destination);
            const auto root = tree.node(tree.root());
            CHECK(tree.paneCount() == 4);
            CHECK(root.orientation == orientation);
            const auto existing = side == InsertSide::First ? root.second : root.first;
            const auto inserted = side == InsertSide::First ? root.first : root.second;
            CHECK(Ubiquitty::Panes::sameShape(original, original.root(), tree, existing));
            CHECK(Ubiquitty::Panes::sameShape(incoming, incoming.root(), tree, inserted));
            CHECK(tree.node(existing).ratio == 0.3f);
            CHECK(tree.node(inserted).ratio == 0.7f);
            CHECK(tree.focusedPaneID() == incoming.focusedPaneID());
        }
    }
}

TEST(testPaneMovesToOuterTabEdgeWithinAndBetweenTabs)
{
    using Ubiquitty::Panes::InsertTarget;
    auto workspace = freshWorkspace();
    const auto destination = workspace->activeSurface();
    const auto target = workspace->focusedPane(destination);
    const auto targetTab = workspace->activeTab(destination);
    workspace->splitFocusedPane(destination, Orientation::Vertical, 0.4f);
    const auto original = *workspace->activePaneTree(destination);
    const auto source = workspace->addSurface();
    const auto pane = workspace->focusedPane(source);
    CHECK(workspace->movePaneToTab(source, pane, targetTab, target, Orientation::Horizontal, InsertSide::Second, InsertTarget::Root));
    auto tree = *workspace->activePaneTree(destination);
    CHECK(tree.paneCount() == 3);
    CHECK(Ubiquitty::Panes::sameShape(original, original.root(), tree, tree.node(tree.root()).first));
    CHECK(tree.node(tree.node(tree.root()).second).paneID == static_cast<int>(pane));
    CHECK(workspace->toggleZoom(destination, target));
    CHECK(workspace->movePaneToTab(destination, pane, targetTab, target, Orientation::Vertical, InsertSide::First, InsertTarget::Root));
    tree = *workspace->activePaneTree(destination);
    CHECK(tree.paneCount() == 3);
    CHECK(tree.node(tree.root()).orientation == Orientation::Vertical);
    CHECK(tree.node(tree.node(tree.root()).first).paneID == static_cast<int>(pane));
    CHECK(Ubiquitty::Panes::sameShape(original, original.root(), tree, tree.node(tree.root()).second));
    CHECK(workspace->zoomedPane(destination) == invalidPane);
    CHECK(!workspace->movePaneToTab(destination, pane, targetTab, PaneID { 9999 }, Orientation::Vertical, InsertSide::First, InsertTarget::Root));
    CHECK(workspace->activePaneTree(destination)->paneCount() == 3);
}

// The tabs of a surface, in order, as their ids.
static std::vector<TabID> tabsOf(const Workspace& workspace, SurfaceID surface)
{
    std::vector<TabID> tabs;
    for (int index = 0; index < workspace.tabCount(surface); ++index)
        tabs.push_back(workspace.tabAt(surface, index));
    return tabs;
}

TEST(aTabMovedWithinItsSurfaceTakesThePlaceNamedAndKeepsWhatItIs)
{
    auto workspace = freshWorkspace();
    const auto surface = workspace->activeSurface();
    const TabID first = workspace->activeTab(surface);
    const TabID second = workspace->addTab(surface);
    const TabID third = workspace->addTab(surface);
    CHECK(workspace->renameTab(surface, first, "work"));
    const PaneID split = workspace->setActiveTab(surface, first) ? workspace->splitFocusedPane(surface, Orientation::Vertical, 0.3f) : invalidPane;
    CHECK(split != invalidPane);
    CHECK(workspace->setActiveTab(surface, second));

    CHECK(workspace->moveTab(first, surface, 2));
    CHECK(tabsOf(*workspace, surface) == (std::vector<TabID> { second, third, first }));
    // The tab is the one it was: its id, its title and its panes.
    CHECK(workspace->tabTitle(surface, first) == "work");
    CHECK(workspace->paneTree(surface, first) && workspace->paneTree(surface, first)->paneCount() == 2);
    CHECK(workspace->tabHolding(surface, split) == first);
    // The tab a person was on is still the one they are on.
    CHECK(workspace->activeTab(surface) == second);

    CHECK(workspace->moveTab(first, surface, 0));
    CHECK(tabsOf(*workspace, surface) == (std::vector<TabID> { first, second, third }));
    CHECK(workspace->activeTab(surface) == second);
    CHECK(workspace->moveTab(second, surface, 2));
    CHECK(tabsOf(*workspace, surface) == (std::vector<TabID> { first, third, second }));
    CHECK(workspace->activeTab(surface) == second);
    // A tab moved to where it stands is moved.
    CHECK(workspace->moveTab(third, surface, 1));
    CHECK(tabsOf(*workspace, surface) == (std::vector<TabID> { first, third, second }));
}

TEST(aTabMoveToAPlaceItsSurfaceDoesNotHaveChangesNothing)
{
    auto workspace = freshWorkspace();
    const auto surface = workspace->activeSurface();
    const TabID first = workspace->activeTab(surface);
    const TabID second = workspace->addTab(surface);
    const std::vector<TabID> before = tabsOf(*workspace, surface);
    CHECK(!workspace->moveTab(first, surface, 2));
    CHECK(!workspace->moveTab(first, surface, -1));
    CHECK(!workspace->moveTab(TabID { 9999 }, surface, 0));
    CHECK(!workspace->moveTab(first, SurfaceID { 9999 }, 0));
    CHECK(!workspace->moveTab(invalidTab, surface, 0));
    CHECK(tabsOf(*workspace, surface) == before);
    CHECK(workspace->activeTab(surface) == second);
}

TEST(aTabMovedToAnotherSurfaceLandsAtThePlaceNamedAndIsShownThere)
{
    auto workspace = freshWorkspace();
    const auto source = workspace->activeSurface();
    const TabID first = workspace->activeTab(source);
    const TabID moving = workspace->addTab(source);
    const TabID last = workspace->addTab(source);
    CHECK(workspace->renameTab(source, moving, "logs"));
    const PaneID movingPane = workspace->paneTree(source, moving) ? static_cast<PaneID>(static_cast<uint32_t>(workspace->paneTree(source, moving)->focusedPaneID())) : invalidPane;
    const auto destination = workspace->addSurface();
    const TabID there = workspace->activeTab(destination);
    const TabID alsoThere = workspace->addTab(destination);
    CHECK(workspace->splitFocusedPane(destination, Orientation::Vertical, 0.5f) != invalidPane);
    CHECK(workspace->toggleZoom(destination, workspace->focusedPane(destination)));
    CHECK(workspace->zoomedPane(destination) != invalidPane);
    CHECK(workspace->setActiveSurface(source));
    CHECK(workspace->setActiveTab(source, last));

    // One past the last place is the end; one further is nowhere.
    CHECK(!workspace->moveTab(moving, destination, 3));
    CHECK(workspace->moveTab(moving, destination, 1));
    CHECK(tabsOf(*workspace, source) == (std::vector<TabID> { first, last }));
    CHECK(tabsOf(*workspace, destination) == (std::vector<TabID> { there, moving, alsoThere }));
    CHECK(workspace->tabTitle(destination, moving) == "logs");
    CHECK(workspace->surfaceHolding(movingPane) == destination);
    CHECK(workspace->activeTab(destination) == moving);
    CHECK(workspace->zoomedPane(destination) == invalidPane);
    CHECK(workspace->activeSurface() == destination);
    // What the source showed, it still shows.
    CHECK(workspace->activeTab(source) == last);

    CHECK(workspace->moveTab(first, destination, 3));
    CHECK(tabsOf(*workspace, destination) == (std::vector<TabID> { there, moving, alsoThere, first }));
    CHECK(workspace->moveTab(last, destination, 0));
    // A surface that gave away its last tab is gone.
    CHECK(workspace->surfaceCount() == 1);
    CHECK(workspace->surface(source) == nullptr);
    CHECK(tabsOf(*workspace, destination) == (std::vector<TabID> { last, there, moving, alsoThere, first }));
    CHECK(workspace->activeTab(destination) == last);
}

TEST(aSurfaceWithEveryTabItMayHoldTakesNoTabFromAnother)
{
    auto workspace = freshWorkspace();
    const auto full = workspace->activeSurface();
    while (workspace->tabCount(full) < Ubiquitty::Session::Layout::maxTabsPerSurface)
        CHECK(workspace->addTab(full) != invalidTab);
    const auto source = workspace->addSurface();
    const TabID tab = workspace->activeTab(source);
    CHECK(!workspace->moveTab(tab, full, 0));
    CHECK(workspace->surfaceCount() == 2);
    CHECK(workspace->activeTab(source) == tab);
    // It still reorders its own.
    CHECK(workspace->moveTab(workspace->tabAt(full, 0), full, Ubiquitty::Session::Layout::maxTabsPerSurface - 1));
}

TEST(workspacesSharingAnAllocatorNeverHoldTheSameID)
{
    using Ubiquitty::Session::Identity::Allocator;
    using Ubiquitty::Session::Layout::mintRootIdentity;
    auto identities = std::make_shared<Allocator>();
    auto first = std::make_unique<Workspace>(identities);
    auto second = std::make_unique<Workspace>(identities);
    CHECK(first->activeSurface() != second->activeSurface());
    CHECK(first->focusedPane(first->activeSurface()) != second->focusedPane(second->activeSurface()));
    const PaneID split = first->splitFocusedPane(first->activeSurface(), Orientation::Vertical, 0.5f);
    const TabID tab = second->addTab(second->activeSurface());
    CHECK(split != second->focusedPane(second->activeSurface()));
    CHECK(tab != first->activeTab(first->activeSurface()));

    // A root minted before its workspace exists is the root the workspace holds,
    // and no later mint from the shared allocator repeats it.
    const auto root = mintRootIdentity(*identities);
    CHECK(root.has_value());
    if (!root)
        return;
    auto third = std::make_unique<Workspace>(identities, *root);
    CHECK(third->activeSurface() == root->surface);
    CHECK(third->activeTab(root->surface) == root->tab);
    CHECK(third->focusedPane(root->surface) == root->pane);
    CHECK(first->splitFocusedPane(first->activeSurface(), Orientation::Vertical, 0.5f) != root->pane);
}

UBIQUITTY_TEST_MAIN()
