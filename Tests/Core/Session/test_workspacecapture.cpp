// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// test_workspacecapture.cpp — capture of the live layout tree (Session::Layout::Workspace)
// into the serialized snapshot (Session::Workspace), and that the snapshot
// survives the existing serialize → deserialize round trip.
//
// Builds a 2-surface workspace (surface 0: a V-split tab + a single-pane tab;
// surface 1: a single pane), captures it through a stub Origin/title resolver,
// and pins the resulting window/tab/PaneNode shape + the round trip.

#include "Core/Session/Workspace.h"
#include "Core/Session/WorkspaceCapture.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

using Ubiquitty::Session::Layout::PaneID;
using Ubiquitty::Session::Layout::PaneInfo;
using Ubiquitty::Session::Layout::SurfaceID;
using Ubiquitty::Session::Layout::Workspace;
using Orientation = Ubiquitty::Panes::Orientation;

// Stateless resolver: Local origin, title "p<id>" — deterministic from the id so
// the assertions can predict each leaf's title.
static PaneInfo resolveStub(void*, PaneID pane)
{
    PaneInfo info;
    info.origin.transport = Ubiquitty::Transport::OriginKind::Local;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "p%u", static_cast<unsigned>(pane));
    info.title = buffer;
    return info;
}

// For the restore round trip: restore reports each reconstructed pane's new id +
// info, which we stash by id; capturing the restored tree then resolves from it,
// so the re-captured snapshot should equal the original (ids differ but the
// id-free snapshot form carries only origin/title/shape).
struct RestoreContext {
    std::map<PaneID, PaneInfo> byID;
};
static void restoreCollect(void* context, PaneID pane, const PaneInfo& info) { static_cast<RestoreContext*>(context)->byID[pane] = info; }
static PaneInfo resolveFromMap(void* context, PaneID pane)
{
    const auto& byID = static_cast<RestoreContext*>(context)->byID;
    const auto it = byID.find(pane);
    return it != byID.end() ? it->second : PaneInfo {};
}

TEST(workspacecapture)
{

    auto ws = std::make_unique<Workspace>();
    const SurfaceID s0 = ws->activeSurface();
    // surface 0, tab 0: split vertically into two panes.
    CHECK(ws->splitFocusedPane(s0, Orientation::Vertical) != Ubiquitty::Session::Layout::invalidPane);
    // surface 0, tab 1: a fresh single-pane tab (becomes active).
    CHECK(ws->addTab(s0) != Ubiquitty::Session::Layout::invalidTab);
    // surface 1: single tab / single pane.
    const SurfaceID s1 = ws->addSurface();
    CHECK(s1 != Ubiquitty::Session::Layout::invalidSurface);

    const Ubiquitty::Session::Workspace snap = Ubiquitty::Session::Layout::captureToSnapshot(*ws, resolveStub, nullptr);

    // Two surfaces → two windows, in creation order.
    CHECK(snap.windows.size() == 2);
    if (snap.windows.size() == 2) {
        const Ubiquitty::Session::WorkspaceWindow& w0 = snap.windows[0];
        CHECK(w0.tabs.size() == 2);
        CHECK(w0.activeTab == 1); // addTab made tab 1 active
        if (w0.tabs.size() == 2) {
            // tab 0 is the vertical split: two leaf children, ratio 0.5, side-by-side.
            const Ubiquitty::Session::PaneNode& split = w0.tabs[0].root;
            CHECK(!split.leaf);
            CHECK(!split.horizontalSplit); // Vertical = columns side by side
            CHECK(split.children.size() == 2);
            if (split.children.size() == 2) {
                CHECK(split.children[0].leaf);
                CHECK(split.children[1].leaf);
                CHECK(std::strncmp(split.children[0].title.c_str(), "p", 1) == 0);
                CHECK(split.children[0].origin.transport == Ubiquitty::Transport::OriginKind::Local);
            }
            // tab 1 is a single pane.
            CHECK(w0.tabs[1].root.leaf);
        }
        // surface 1 → a single-pane window.
        CHECK(snap.windows[1].tabs.size() == 1);
        CHECK(snap.windows[1].tabs[0].root.leaf);
    }

    // Chunk B: a tree built and then collapsed (split -> moveFocus -> close
    // -> last-pane escalation) captures the SURVIVING shape, with never-reused
    // ids. Build a 3-pane tab on surface 1, close one pane back to a 2-pane split,
    // then capture: the snapshot must reflect the collapsed (split-of-two) shape.
    {
        auto live = std::make_unique<Workspace>();
        const SurfaceID c = live->activeSurface();
        const PaneID c0 = live->focusedPane(c);
        const PaneID c1 = live->splitFocusedPane(c, Orientation::Vertical);
        const PaneID c2 = live->splitFocusedPane(c, Orientation::Horizontal);
        CHECK(c1 > c0 && c2 > c1); // monotonic
        CHECK(live->activePaneTree(c)->paneCount() == 3);
        // Move focus, then close a pane from elsewhere (the macOS focus-then-close
        // idiom): collapse back to a 2-pane split.
        live->moveFocus(c, Ubiquitty::Panes::Direction::Up);
        CHECK(live->focusPane(c, c2));
        CHECK(live->closeFocusedPane(c));
        CHECK(live->activePaneTree(c)->paneCount() == 2);

        const Ubiquitty::Session::Workspace collapsed = Ubiquitty::Session::Layout::captureToSnapshot(*live, resolveStub, nullptr);
        CHECK(collapsed.windows.size() == 1);
        if (collapsed.windows.size() == 1) {
            CHECK(collapsed.windows[0].tabs.size() == 1);
            const Ubiquitty::Session::PaneNode& root = collapsed.windows[0].tabs[0].root;
            CHECK(!root.leaf); // a 2-pane split, not a lone leaf
            CHECK(root.children.size() == 2);
        }
        // The collapsed snapshot survives the serialize round trip too.
        std::vector<std::uint8_t> collapsedBytes;
        Ubiquitty::Session::serializeWorkspace(collapsed, collapsedBytes);
        Ubiquitty::Session::Workspace collapsedBack;
        CHECK(Ubiquitty::Session::deserializeWorkspace(collapsedBytes, collapsedBack));
        CHECK(Ubiquitty::Session::equalWorkspaces(collapsed, collapsedBack));
    }

    // The captured snapshot survives serialize → deserialize unchanged.
    std::vector<std::uint8_t> bytes;
    Ubiquitty::Session::serializeWorkspace(snap, bytes);
    Ubiquitty::Session::Workspace restored;
    CHECK(Ubiquitty::Session::deserializeWorkspace(bytes, restored));
    CHECK(Ubiquitty::Session::equalWorkspaces(snap, restored));

    // Restore round trip: deserialized snapshot → a fresh live tree → captured
    // again must reproduce the same snapshot (shape + origins + titles), proving
    // the split-replay reconstruction is the inverse of capture.
    RestoreContext restoreContext;
    auto live2 = std::make_unique<Workspace>();
    CHECK(Ubiquitty::Session::Layout::restoreFromSnapshot(restored, *live2, restoreCollect, &restoreContext));
    const Ubiquitty::Session::Workspace reCaptured = Ubiquitty::Session::Layout::captureToSnapshot(*live2, resolveFromMap, &restoreContext);
    CHECK(Ubiquitty::Session::equalWorkspaces(snap, reCaptured));
}

// The resolver is asked in leaf order, first child before second: a caller
// that records each pane as it is asked (the host's layout record) keeps its
// records beside the leaves they describe.
static PaneInfo recordAskedOrder(void* context, PaneID pane)
{
    static_cast<std::vector<PaneID>*>(context)->push_back(pane);
    return resolveStub(nullptr, pane);
}

TEST(theResolverIsAskedInLeafOrder)
{
    auto ws = std::make_unique<Workspace>();
    const SurfaceID surface = ws->activeSurface();
    const PaneID first = ws->focusedPane(surface);
    const PaneID second = ws->splitFocusedPane(surface, Orientation::Vertical);
    REQUIRE(second != Ubiquitty::Session::Layout::invalidPane);
    ws->focusPane(surface, second);
    const PaneID third = ws->splitFocusedPane(surface, Orientation::Horizontal);
    REQUIRE(third != Ubiquitty::Session::Layout::invalidPane);
    std::vector<PaneID> asked;
    (void)Ubiquitty::Session::Layout::captureToSnapshot(*ws, recordAskedOrder, &asked);
    CHECK((asked == std::vector<PaneID> { first, second, third }));
}

TEST(presentationStateSurvivesPersistenceAndFreshPaneIDs)
{
    auto live = std::make_unique<Workspace>();
    const SurfaceID surface = live->activeSurface();
    const auto firstTab = live->activeTab(surface);
    const PaneID first = live->focusedPane(surface);
    const PaneID second = live->splitFocusedPane(surface, Orientation::Vertical, 0.31f);
    const PaneID third = live->splitFocusedPane(surface, Orientation::Horizontal, 0.73f);
    REQUIRE(third != Ubiquitty::Session::Layout::invalidPane);
    REQUIRE(live->focusPane(surface, second));
    REQUIRE(live->renameTab(surface, firstTab, "Build and logs"));
    REQUIRE(live->addTab(surface) != Ubiquitty::Session::Layout::invalidTab);
    REQUIRE(live->setActiveTab(surface, firstTab));
    REQUIRE(live->toggleZoom(surface, third));
    REQUIRE(live->addSurface() != Ubiquitty::Session::Layout::invalidSurface);
    REQUIRE(live->setActiveSurface(surface));

    auto snapshot = Ubiquitty::Session::Layout::captureToSnapshot(*live, resolveStub, nullptr);
    CHECK_EQ(snapshot.activeWindow, 0);
    CHECK_EQ(snapshot.windows[0].activeTab, 0);
    CHECK_EQ(snapshot.windows[0].zoomedPaneIndex, 2);
    CHECK_EQ(snapshot.windows[0].tabs[0].focusedPaneIndex, 1);
    CHECK_STR_EQ(snapshot.windows[0].tabs[0].title, "Build and logs");
    CHECK(snapshot.windows[0].tabs[0].root.ratio == static_cast<double>(0.31f));
    CHECK(first != second);
    std::vector<std::uint8_t> bytes;
    Ubiquitty::Session::serializeWorkspace(snapshot, bytes);
    Ubiquitty::Session::Workspace decoded;
    REQUIRE(Ubiquitty::Session::deserializeWorkspace(bytes, decoded));

    auto restored = std::make_unique<Workspace>();
    RestoreContext context;
    REQUIRE(Ubiquitty::Session::Layout::restoreFromSnapshot(decoded, *restored, restoreCollect, &context));
    const auto captured = Ubiquitty::Session::Layout::captureToSnapshot(*restored, resolveFromMap, &context);
    CHECK(Ubiquitty::Session::equalWorkspaces(snapshot, captured));
}

TEST(invalidArrangementDoesNotLaunchAnEarlierValidPane)
{
    Ubiquitty::Session::Workspace snapshot;
    Ubiquitty::Session::WorkspaceWindow window;
    window.tabs.emplace_back();
    window.tabs.emplace_back();
    window.tabs.back().root.leaf = false;
    snapshot.windows.push_back(window);
    auto restored = std::make_unique<Workspace>();
    const auto originalSurface = restored->activeSurface();
    RestoreContext context;
    CHECK(!Ubiquitty::Session::Layout::restoreFromSnapshot(snapshot, *restored, restoreCollect, &context));
    CHECK(context.byID.empty());
    CHECK_EQ(restored->surfaceCount(), 1);
    CHECK_EQ(restored->activeSurface(), originalSurface);
    CHECK_EQ(restored->surface(originalSurface)->tabCount, 1);
}

UBIQUITTY_TEST_MAIN()
