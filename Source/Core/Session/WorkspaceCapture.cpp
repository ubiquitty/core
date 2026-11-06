// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/WorkspaceCapture.cpp — see WorkspaceCapture.h.

#include "Core/Session/WorkspaceCapture.h"

#include "Core/Panes/PaneTree.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace Ubiquitty::Session::Layout {

namespace {

int capturedLeafIndex(const Ubiquitty::Panes::Tree& tree, PaneID pane)
{
    int index = 0;
    int found = -1;
    Ubiquitty::Panes::forEachLeaf(tree, [&](int leaf) {
        if (static_cast<PaneID>(leaf) == pane) {
            found = index;
        }
        ++index;
    });
    return found;
}

int restorablePaneCount(const Ubiquitty::Session::PaneNode& node, int depth = 0)
{
    if (depth >= maxPanesPerTab) {
        return 0;
    }
    if (node.leaf) {
        return node.children.empty() ? 1 : 0;
    }
    if (node.children.size() != 2 || !std::isfinite(node.ratio) || node.ratio < 0 || node.ratio > 1) {
        return 0;
    }
    const int first = restorablePaneCount(node.children[0], depth + 1);
    const int second = restorablePaneCount(node.children[1], depth + 1);
    return first && second && first + second <= maxPanesPerTab ? first + second : 0;
}

// Convert the subtree rooted at `nodeIndex` of a Panes::Tree into the
// serialized PaneNode form. A leaf resolves its Origin + title; a split
// carries orientation (Horizontal → stacked) + ratio and recurses.
Ubiquitty::Session::PaneNode convertNode(const Ubiquitty::Panes::Tree& tree, int nodeIndex, PaneInfoResolver resolve, void* context)
{
    if (tree.isLeaf(nodeIndex)) {
        const int paneID = tree.node(nodeIndex).paneID;
        const PaneInfo info = resolve(context, static_cast<PaneID>(paneID));
        auto leaf = Ubiquitty::Session::PaneNode::makeLeaf(info.origin, info.title);
        leaf.directory = info.directory;
        return leaf;
    }
    const Ubiquitty::Panes::Node& node = tree.node(nodeIndex);
    const bool isHorizontal = node.orientation == Ubiquitty::Panes::Orientation::Horizontal;
    // Each in its own statement: the order a call's arguments are evaluated
    // in is unspecified, and a resolver that records each pane as it is asked
    // (the host's layout record does) must be asked in first-child order.
    auto first = convertNode(tree, node.first, resolve, context);
    auto second = convertNode(tree, node.second, resolve, context);
    return Ubiquitty::Session::PaneNode::makeSplit(isHorizontal, node.ratio, std::move(first), std::move(second));
}

} // namespace

namespace {

// The focused leaf stands in for the subtree root. Leaves are recorded in
// first-child order so persisted focus and zoom never depend on allocated ids.
bool buildPanes(Workspace& out, SurfaceID surface, const Ubiquitty::Session::PaneNode& node, PaneRestoredFn onPane, void* context,
    std::array<PaneID, maxPanesPerTab>& paneIDs, int& paneCount)
{
    if (node.leaf) {
        const PaneID pane = out.focusedPane(surface);
        paneIDs[paneCount++] = pane;
        const PaneInfo info { node.origin, node.title, node.directory };
        onPane(context, pane, info);
        return true;
    }
    if (node.children.size() != 2)
        return false;
    const PaneID firstLeaf = out.focusedPane(surface);
    const Ubiquitty::Panes::Orientation orientation = node.horizontalSplit ? Ubiquitty::Panes::Orientation::Horizontal : Ubiquitty::Panes::Orientation::Vertical;
    const PaneID secondLeaf = out.splitFocusedPane(surface, orientation, static_cast<float>(node.ratio));
    if (secondLeaf == invalidPane)
        return false;
    if (!out.focusPane(surface, firstLeaf))
        return false;
    if (!buildPanes(out, surface, node.children[0], onPane, context, paneIDs, paneCount))
        return false;
    if (!out.focusPane(surface, secondLeaf))
        return false;
    return buildPanes(out, surface, node.children[1], onPane, context, paneIDs, paneCount);
}

} // namespace

bool restoreFromSnapshot(const Ubiquitty::Session::Workspace& snapshot, Workspace& out, PaneRestoredFn onPane, void* context, RestorePlacement placement)
{
    // Validate the complete recipe before a callback can launch any processes.
    const auto existing = placement == RestorePlacement::Append ? out.surfaceCount() : 0;
    if (!onPane || snapshot.windows.size() > static_cast<std::size_t>(maxSurfaces - existing)) {
        return false;
    }
    for (const auto& window : snapshot.windows) {
        if (window.tabs.empty() || window.tabs.size() > maxTabsPerSurface) {
            return false;
        }
        for (const auto& tab : window.tabs) {
            if (!restorablePaneCount(tab.root)) {
                return false;
            }
        }
    }
    std::array<SurfaceID, maxSurfaces> surfaceIDs {};
    for (std::size_t windowIndex = 0; windowIndex < snapshot.windows.size(); ++windowIndex) {
        const Ubiquitty::Session::WorkspaceWindow& window = snapshot.windows[windowIndex];
        // Window 0 reuses the default surface; later windows are added.
        const SurfaceID surface = windowIndex == 0 && placement == RestorePlacement::FreshWorkspace ? out.activeSurface() : out.addSurface();
        if (surface == invalidSurface)
            return false; // surface cap
        surfaceIDs[windowIndex] = surface;
        if (window.tabs.empty())
            continue;
        TabID tabIDs[maxTabsPerSurface];
        int tabIDCount = 0;
        PaneID zoomedPane = invalidPane;
        for (std::size_t tabIndex = 0; tabIndex < window.tabs.size(); ++tabIndex) {
            // Tab 0 reuses the surface's default tab; later tabs are added.
            const TabID tab = tabIndex == 0 ? out.activeTab(surface) : out.addTab(surface);
            if (tab == invalidTab)
                return false; // tab cap
            out.setActiveTab(surface, tab);
            if (tabIDCount < maxTabsPerSurface)
                tabIDs[tabIDCount++] = tab;
            const auto& capturedTab = window.tabs[tabIndex];
            out.renameTab(surface, tab, capturedTab.title);
            std::array<PaneID, maxPanesPerTab> paneIDs {};
            int paneCount = 0;
            if (!buildPanes(out, surface, capturedTab.root, onPane, context, paneIDs, paneCount))
                return false;
            const int focusIndex = std::clamp(capturedTab.focusedPaneIndex, 0, paneCount - 1);
            out.focusPane(surface, paneIDs[focusIndex]);
            if (static_cast<int>(tabIndex) == std::clamp(window.activeTab, 0, static_cast<int>(window.tabs.size()) - 1)
                && window.zoomedPaneIndex >= 0 && window.zoomedPaneIndex < paneCount) {
                zoomedPane = paneIDs[window.zoomedPaneIndex];
            }
        }
        // Restore the active tab (the snapshot's activeTab is an index; clamp).
        int activeIndex = window.activeTab;
        if (activeIndex < 0)
            activeIndex = 0;
        if (activeIndex >= tabIDCount)
            activeIndex = tabIDCount - 1;
        if (tabIDCount > 0)
            out.setActiveTab(surface, tabIDs[activeIndex]);
        if (zoomedPane != invalidPane) {
            out.toggleZoom(surface, zoomedPane);
        }
    }
    if (!snapshot.windows.empty()) {
        const int selected = std::clamp(snapshot.activeWindow, 0, static_cast<int>(snapshot.windows.size()) - 1);
        out.setActiveSurface(surfaceIDs[selected]);
    }
    return true;
}

Ubiquitty::Session::Workspace captureToSnapshot(const Workspace& live, PaneInfoResolver resolve, void* context, SurfaceID selectedSurface)
{
    Ubiquitty::Session::Workspace snapshot;
    for (int surfaceIndex = 0; surfaceIndex < live.surfaceCount(); ++surfaceIndex) {
        const SurfaceID surfaceID = live.surfaceIDAt(surfaceIndex);
        if (selectedSurface != invalidSurface && selectedSurface != surfaceID) {
            continue;
        }
        const Surface* surface = live.surface(surfaceID);
        if (!surface)
            continue;
        if (surfaceID == live.activeSurface()) {
            snapshot.activeWindow = static_cast<int>(snapshot.windows.size());
        }
        Ubiquitty::Session::WorkspaceWindow window;
        window.activeTab = surface->activeTab;
        for (int tabIndex = 0; tabIndex < surface->tabCount; ++tabIndex) {
            const Tab& tab = surface->tabs[tabIndex];
            Ubiquitty::Session::WorkspaceTab outTab;
            outTab.title = tab.title;
            outTab.focusedPaneIndex = capturedLeafIndex(tab.panes, static_cast<PaneID>(tab.panes.focusedPaneID()));
            if (tabIndex == surface->activeTab) {
                window.zoomedPaneIndex = capturedLeafIndex(tab.panes, surface->zoomedPane);
            }
            outTab.root = convertNode(tab.panes, tab.panes.root(), resolve, context);
            window.tabs.push_back(std::move(outTab));
        }
        snapshot.windows.push_back(std::move(window));
    }
    return snapshot;
}

} // namespace Ubiquitty::Session::Layout
