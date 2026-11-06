// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The portable tmux window/pane registry: the maps + policy driving a `tmux -CC`
// client (which pane backs `%M`, which `%M`/controller backs `@N`, atomic spawn
// dedup, a live-pane cap, and @N-ordered tab placement).
//
// Pure registry: it stores opaque handles (the frontend's pane / controller objects
// as `uintptr_t` tokens, never an AppKit/GTK type) and consults a frontend-supplied
// liveness probe for weakly-held entries. Holds no lock — the frontend supplies its
// own around the model lock. Depends only on the C++ standard library.

#include "Core/Protocol/Tmux/PaneID.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Ubiquitty::Protocol::Tmux {

// Opaque frontend object tokens. The registry never dereferences these — it
// stores, compares, and hands them back. The frontend casts its own pane /
// controller objects to/from these (a `__bridge void*` on macOS, a `GObject*`
// on GTK). 0 is the "none / not present" sentinel.
using PaneHandle = std::uintptr_t;
using ControllerHandle = std::uintptr_t;

// Cap on live render-only tmux panes so a malicious/buggy server can't emit
// thousands of pane records and spawn a surface for each, exhausting memory.
constexpr int maxTmuxPanes = 256;

// Liveness probe for a weakly-held handle. The frontend holds pane / controller
// surfaces WEAKLY (a closed tab deallocs its surface), so a stored handle may
// refer to an object that is already gone. The registry consults this before
// counting / returning / dedup-ing an entry; a probe returning false means the
// slot went nil and the registry drops it. `context` is the frontend's opaque
// cookie, passed back unchanged.
using HandleLivenessProbe = bool (*)(void* context, std::uintptr_t handle);

// Where a newly-discovered window's tab grafts relative to its anchor, in tmux
// window-id order. tmux assigns @N incrementally, so anchoring by @N keeps the
// user's window order across a reconnect (a manual swap-window is the documented
// exception).
enum class TabPlacement {
    None, // no other live window yet — the caller places the tab standalone
    AfterAnchor, // graft after the anchor (the nearest live window with a smaller @N)
    BeforeAnchor // graft before the anchor (the nearest live window with a larger @N)
};

// The resolved anchor for a new window's tab: a placement + the controller to
// anchor onto. `placement == None` means `anchor` is unset (0).
struct TabAnchor {
    TabPlacement placement = TabPlacement::None;
    ControllerHandle anchor = 0;
};

class WindowRegistry {
public:
    // Install the liveness probe for weakly-held handles. Must be set before any
    // read that can encounter a dead slot (registeredPaneCount, paneForPaneID,
    // controllerForWindow, the anchor + sibling queries). `context` is passed back
    // to the probe unchanged.
    void setLivenessProbe(HandleLivenessProbe probe, void* context)
    {
        m_probe = probe;
        m_probeContext = context;
    }

    // ---- pane registry (%M → surface) -------------------------------------

    // Register the surface backing pane `%paneID`. tmux's first pane is %0, so 0
    // is a valid id; only negatives mean "none". A null handle is ignored.
    void registerPane(PaneID paneID, PaneHandle pane)
    {
        if (!pane || paneID < 0)
            return;
        m_paneRegistry[paneID] = pane;
    }

    // Drop pane `%paneID` and any `@window → %pane` entries pointing at it, so the
    // maps stay consistent regardless of which the caller reached first.
    void unregisterPane(PaneID paneID)
    {
        m_paneRegistry.erase(paneID);
        m_paneWindow.erase(paneID);
        for (auto it = m_windowToPane.begin(); it != m_windowToPane.end();) {
            if (it->second == paneID)
                it = m_windowToPane.erase(it);
            else
                ++it;
        }
    }

    // The live surface backing `%paneID`, or 0. A weak slot that went nil is
    // dropped and reported as 0.
    [[nodiscard]] PaneHandle paneForPaneID(PaneID paneID)
    {
        auto it = m_paneRegistry.find(paneID);
        if (it == m_paneRegistry.end())
            return 0;
        if (!isLive(it->second)) {
            m_paneRegistry.erase(it);
            return 0;
        }
        return it->second;
    }

    // Count of live render-only panes (skipping any weak slot that went nil). The
    // spawn path caps against this so a server's pane flood can't exceed
    // maxTmuxPanes surfaces.
    [[nodiscard]] int registeredPaneCount() const
    {
        int count = 0;
        for (const auto& kv : m_paneRegistry)
            if (isLive(kv.second))
                ++count;
        return count;
    }

    // Whether registering another pane would exceed the cap.
    [[nodiscard]] bool atPaneCapacity() const { return registeredPaneCount() >= maxTmuxPanes; }

    // ---- @window → %pane ----------------------------------------------------

    void mapWindowToPane(int windowID, PaneID paneID)
    {
        if (paneID < 0)
            return;
        m_windowToPane[windowID] = paneID;
    }

    // The pane backing `@windowID`, or invalidPaneID when unknown.
    [[nodiscard]] PaneID paneForWindow(int windowID) const
    {
        auto it = m_windowToPane.find(windowID);
        return (it == m_windowToPane.end()) ? invalidPaneID : it->second;
    }

    // ---- @window → owning controller ---------------------------------------
    //
    // A tmux window's N panes share one controller: the first pane spawns it;
    // subsequent panes are built into it by the layout reconciler. Held weakly so
    // a closed window's controller prunes itself.

    void setControllerForWindow(int windowID, ControllerHandle controller) { m_windowToController[windowID] = controller; }

    // The live controller owning `@windowID`, or 0. A weak slot that went nil is
    // dropped and reported as 0.
    [[nodiscard]] ControllerHandle controllerForWindow(int windowID)
    {
        auto it = m_windowToController.find(windowID);
        if (it == m_windowToController.end())
            return 0;
        if (!isLive(it->second)) {
            m_windowToController.erase(it);
            return 0;
        }
        return it->second;
    }

    // Drop a closed window's controller entry (its panes are unregistered
    // separately). The caller pairs this with dropping any cached layout.
    void forgetWindow(int windowID) { m_windowToController.erase(windowID); }

    // ---- spawn dedup (atomic check-and-claim) ------------------------------
    // A pane is discovered twice (attach list-panes + %window-add) and the surface
    // spawn pumps the runloop, so a second discovery could re-enter the spawn before
    // the first registers → a duplicate tab. The reservation makes the claim atomic.

    // Try to claim `%paneID`'s spawn. Returns true if the caller now owns it (it
    // must balance with endPaneSpawn); false if the pane is already live or
    // another spawn is in flight.
    [[nodiscard]] bool reservePaneForSpawn(PaneID paneID)
    {
        auto it = m_paneRegistry.find(paneID);
        const bool alreadyLive = (it != m_paneRegistry.end() && isLive(it->second));
        const bool inFlight = (m_panesSpawning.count(paneID) != 0);
        if (alreadyLive || inFlight)
            return false;
        m_panesSpawning.insert(paneID);
        return true;
    }

    void endPaneSpawn(PaneID paneID) { m_panesSpawning.erase(paneID); }

    // ---- tab-anchor ordering (pure) ----------------------------------------

    // Resolve where a newly-discovered window's tab grafts, in @N order. Graft
    // after the nearest live window with a smaller @N; else before the nearest
    // with a larger @N; else None (no other live window yet). The current window
    // (`windowID`) and an `excluded` controller (its own, already registered) are
    // skipped, as are dead weak slots. Pure over the @window → controller map.
    [[nodiscard]] TabAnchor tabAnchorForWindow(int windowID, ControllerHandle excluded)
    {
        ControllerHandle lower = 0;
        int lowerID = -1; // window ids are >= 0, so this starts below any real id
        ControllerHandle upper = 0;
        int upperID = 0;
        bool haveUpper = false;
        for (auto it = m_windowToController.begin(); it != m_windowToController.end();) {
            const int otherID = it->first;
            const ControllerHandle controller = it->second;
            if (!isLive(controller)) {
                it = m_windowToController.erase(it); // weak slot went nil — drop it
                continue;
            }
            ++it;
            if (controller == excluded || otherID == windowID)
                continue;
            if (otherID < windowID) {
                if (otherID > lowerID) {
                    lowerID = otherID;
                    lower = controller;
                }
            } else if (otherID > windowID) {
                if (!haveUpper || otherID < upperID) {
                    upperID = otherID;
                    upper = controller;
                    haveUpper = true;
                }
            }
        }
        if (lower)
            return { TabPlacement::AfterAnchor, lower };
        if (upper)
            return { TabPlacement::BeforeAnchor, upper };
        return { TabPlacement::None, 0 };
    }

    // ---- sibling render-only resize selection (pure) -----------------------

    // The live pane surfaces to resize when `activeController`'s grid changes:
    // every registered pane not owned by the active controller (the active tab
    // resizes itself). Pure selection — the frontend does the actual geometry-only
    // Terminal::resize on each under its own model lock. `ownerOf` maps a pane handle
    // to its owning controller handle (the frontend's accessor); a pane whose
    // weak slot went nil is skipped (and dropped).
    using OwnerOfPane = ControllerHandle (*)(void* context, PaneHandle pane);
    [[nodiscard]] std::vector<PaneHandle> siblingPanesExcept(ControllerHandle activeController, OwnerOfPane ownerOf, void* ownerContext)
    {
        std::vector<PaneHandle> siblings;
        for (auto it = m_paneRegistry.begin(); it != m_paneRegistry.end();) {
            const PaneHandle pane = it->second;
            if (!isLive(pane)) {
                it = m_paneRegistry.erase(it);
                continue;
            }
            ++it;
            if (ownerOf && ownerOf(ownerContext, pane) != activeController)
                siblings.push_back(pane);
        }
        return siblings;
    }

    // ---- bulk teardown ------------------------------------------------------

    // Snapshot the distinct live owning controllers of every registered pane
    // (deduped), then clear the pane + window→pane maps. The caller closes each
    // returned controller OUTSIDE its own lock (closing a surface re-enters
    // unregisterPane). `ownerOf` maps a pane handle to its owning controller.
    [[nodiscard]] std::vector<ControllerHandle> takeOwningControllersForExit(OwnerOfPane ownerOf, void* ownerContext)
    {
        std::vector<ControllerHandle> owners;
        for (const auto& kv : m_paneRegistry) {
            if (!ownerOf)
                break;
            const ControllerHandle owner = ownerOf(ownerContext, kv.second);
            if (owner == 0)
                continue;
            bool seen = false;
            for (ControllerHandle existing : owners)
                if (existing == owner) {
                    seen = true;
                    break;
                }
            if (!seen)
                owners.push_back(owner);
        }
        m_paneRegistry.clear();
        m_windowToPane.clear();
        return owners;
    }

    // Live controllers across all windows (deduped), for the badge re-label /
    // session-window enumeration. Weak slots that went nil are dropped.
    [[nodiscard]] std::vector<ControllerHandle> liveControllers()
    {
        std::vector<ControllerHandle> controllers;
        for (auto it = m_windowToController.begin(); it != m_windowToController.end();) {
            const ControllerHandle controller = it->second;
            if (!isLive(controller)) {
                it = m_windowToController.erase(it);
                continue;
            }
            ++it;
            bool seen = false;
            for (ControllerHandle existing : controllers)
                if (existing == controller) {
                    seen = true;
                    break;
                }
            if (!seen)
                controllers.push_back(controller);
        }
        return controllers;
    }

    // ---- control-client grid (the size the whole session tracks) ------------
    // Written by the resize path immediately before it sends refresh-client -C;
    // read by sibling sync so a split pane's tile grid is never the sibling target
    // (the session is window-size=latest, so one tile would resize every window).
    struct ClientGrid {
        int columns = 0;
        int rows = 0;
    };
    void setClientGrid(int columns, int rows) { m_clientGrid = { columns, rows }; }
    [[nodiscard]] ClientGrid clientGrid() const { return m_clientGrid; }

    // ---- window <-> pane count (sibling-sync gate) --------------------------
    // Associate a live pane with its tmux window (@N). paneCountForWindow gates the
    // sibling pre-resize to SINGLE-pane windows; a split window waits for tmux's
    // %layout-change echo to size its panes, so it must not be pre-sized to a tile.
    void notePaneWindow(PaneID paneID, int windowID) { m_paneWindow[paneID] = windowID; }
    [[nodiscard]] int paneCountForWindow(int windowID)
    {
        int count = 0;
        for (auto it = m_paneWindow.begin(); it != m_paneWindow.end();) {
            auto reg = m_paneRegistry.find(it->first);
            if (reg == m_paneRegistry.end() || !isLive(reg->second)) {
                it = m_paneWindow.erase(it);
                continue;
            }
            if (it->second == windowID)
                ++count;
            ++it;
        }
        return count;
    }

    // ---- window-level spawn dedup -------------------------------------------
    // Mirror of reservePaneForSpawn at the WINDOW grain: a run-loop pump inside
    // spawnWindowController must not let two pane-discovery blocks each spawn a
    // native window for the same @N.
    [[nodiscard]] bool reserveWindowForSpawn(int windowID)
    {
        auto it = m_windowToController.find(windowID);
        const bool live = (it != m_windowToController.end() && isLive(it->second));
        const bool inFlight = (m_windowsSpawning.count(windowID) != 0);
        if (live || inFlight)
            return false;
        m_windowsSpawning.insert(windowID);
        return true;
    }
    void endWindowSpawn(int windowID) { m_windowsSpawning.erase(windowID); }

    // ---- representative-pane remap ------------------------------------------
    // Repoint @N → a LIVE pane after a reconcile drops the old representative, so
    // a later %window-renamed still resolves. Distinct from erasing the window.
    void updateWindowRepresentativePane(int windowID, PaneID newPaneID)
    {
        if (newPaneID >= 0)
            m_windowToPane[windowID] = newPaneID;
    }

    // ---- cached layout -----------------------------------------------------
    void cacheLayout(int windowID, std::string layout) { m_windowLayouts[windowID] = std::move(layout); }
    [[nodiscard]] const std::string* cachedLayout(int windowID) const
    {
        auto it = m_windowLayouts.find(windowID);
        return (it == m_windowLayouts.end()) ? nullptr : &it->second;
    }
    void dropCachedLayout(int windowID) { m_windowLayouts.erase(windowID); }

    // ---- attach-time active pane (list-panes' `active` column) --------------
    // Recorded per window as pane rows arrive; consumed once by the window's
    // first pane spawn to pick tmux's real active pane for the initial layout
    // mount (a %layout-change echo carries no active-pane field, so this is the
    // only channel for it). invalidPaneID when unknown.
    void noteActivePane(int windowID, PaneID paneID) { m_windowActivePane[windowID] = paneID; }
    [[nodiscard]] PaneID activePaneForWindow(int windowID) const
    {
        auto it = m_windowActivePane.find(windowID);
        return (it == m_windowActivePane.end()) ? invalidPaneID : it->second;
    }
    void dropActivePane(int windowID) { m_windowActivePane.erase(windowID); }

private:
    [[nodiscard]] bool isLive(std::uintptr_t handle) const
    {
        if (handle == 0)
            return false;
        return m_probe ? m_probe(m_probeContext, handle) : true;
    }

    std::unordered_map<PaneID, PaneHandle> m_paneRegistry; // %M → surface (weak)
    std::unordered_map<int, PaneID> m_windowToPane; // @N → %M
    std::unordered_map<int, ControllerHandle> m_windowToController; // @N → controller (weak)
    std::unordered_set<PaneID> m_panesSpawning; // %M mid-spawn (reserve/endPaneSpawn)
    ClientGrid m_clientGrid; // control-client grid (last refresh-client -C)
    std::unordered_map<PaneID, int> m_paneWindow; // %M → @N (for paneCountForWindow)
    std::unordered_set<int> m_windowsSpawning; // @N mid-spawn (window-level dedup)
    std::unordered_map<int, std::string> m_windowLayouts; // @N → cached layout string
    std::unordered_map<int, PaneID> m_windowActivePane; // @N → attach-time active %M

    HandleLivenessProbe m_probe = nullptr;
    void* m_probeContext = nullptr;
};

} // namespace Ubiquitty::Protocol::Tmux
