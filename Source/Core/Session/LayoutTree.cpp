// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/LayoutTree.cpp — see LayoutTree.h.

#include "Core/Session/LayoutTree.h"
#include "Core/Session/CustomTitles.h"

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

namespace Ubiquitty::Session::Layout {

void Workspace::clearZoomIfStale(Surface& surface)
{
    if (surface.zoomedPane == invalidPane)
        return;
    if (surface.tabCount <= 0) {
        surface.zoomedPane = invalidPane;
        return;
    }
    const Tab& tab = surface.tabs[surface.activeTab];
    bool live = false;
    Ubiquitty::Panes::forEachLeaf(tab.panes, [&](int paneID) {
        if (static_cast<PaneID>(paneID) == surface.zoomedPane)
            live = true;
    });
    // A surviving lone pane has nothing to zoom, so a collapse to one pane also
    // clears the zoom (matches the macOS collapse-to-single path).
    if (!live || tab.panes.paneCount() <= 1)
        surface.zoomedPane = invalidPane;
}

std::optional<RootIdentity> mintRootIdentity(Identity::Allocator& identities)
{
    const auto surface = identities.mint(Identity::ObjectKind::Window);
    const auto tab = identities.mint(Identity::ObjectKind::Tab);
    const auto pane = identities.mint(Identity::ObjectKind::Pane);
    if (!surface || !tab || !pane)
        return std::nullopt;
    return RootIdentity {
        SurfaceID { static_cast<std::uint32_t>(surface->number) },
        TabID { static_cast<std::uint32_t>(tab->number) },
        PaneID { static_cast<std::uint32_t>(pane->number) },
    };
}

std::uint32_t Workspace::mintNumber(Identity::ObjectKind kind)
{
    // The allocator's ceiling is the wire's 32 bits, so the narrowing is exact.
    const auto id = identities().mint(kind);
    return id ? static_cast<std::uint32_t>(id->number) : 0;
}

void Workspace::reserveNumber(Identity::ObjectKind kind, std::uint32_t number)
{
    identities().reserveThrough(Identity::ObjectID { kind, number });
}

bool Workspace::initSurface(Surface& surface)
{
    const std::uint32_t surfaceNumber = mintNumber(Identity::ObjectKind::Window);
    const std::uint32_t tabNumber = mintNumber(Identity::ObjectKind::Tab);
    const std::uint32_t paneNumber = mintNumber(Identity::ObjectKind::Pane);
    if (!surfaceNumber || !tabNumber || !paneNumber)
        return false;
    surface.id = SurfaceID { surfaceNumber };
    surface.tabCount = 1;
    surface.activeTab = 0;
    surface.zoomedPane = invalidPane;
    surface.tabs[0].id = TabID { tabNumber };
    surface.tabs[0].title.clear();
    surface.tabs[0].panes = Ubiquitty::Panes::Tree(static_cast<int>(paneNumber));
    return true;
}

Workspace::Workspace()
{
    // A fresh allocator has every kind at one, so the root cannot be refused.
    [[maybe_unused]] const bool didInitialize = initSurface(m_surfaces[0]);
    assert(didInitialize);
    m_surfaceCount = 1;
    m_activeSurface = m_surfaces[0].id;
}

Workspace::Workspace(std::shared_ptr<Identity::Allocator> identities)
    : m_sharedIdentities(std::move(identities))
{
    assert(m_sharedIdentities);
    // An exhausted shared allocator leaves the workspace with no surface, which
    // every lookup answers as unknown; the host refuses the session instead.
    if (!initSurface(m_surfaces[0]))
        return;
    m_surfaceCount = 1;
    m_activeSurface = m_surfaces[0].id;
}

Workspace::Workspace(std::shared_ptr<Identity::Allocator> identities, RootIdentity root)
    : m_sharedIdentities(std::move(identities))
{
    assert(m_sharedIdentities);
    Surface& surface = m_surfaces[0];
    surface.id = root.surface;
    surface.tabCount = 1;
    surface.activeTab = 0;
    surface.zoomedPane = invalidPane;
    surface.tabs[0].id = root.tab;
    surface.tabs[0].title.clear();
    surface.tabs[0].panes = Ubiquitty::Panes::Tree(static_cast<int>(std::to_underlying(root.pane)));
    reserveNumber(Identity::ObjectKind::Window, std::to_underlying(root.surface));
    reserveNumber(Identity::ObjectKind::Tab, std::to_underlying(root.tab));
    reserveNumber(Identity::ObjectKind::Pane, std::to_underlying(root.pane));
    m_surfaceCount = 1;
    m_activeSurface = surface.id;
}

Surface* Workspace::findSurface(SurfaceID id)
{
    if (id == invalidSurface)
        return nullptr;
    for (int i = 0; i < m_surfaceCount; ++i)
        if (m_surfaces[i].id == id)
            return &m_surfaces[i];
    return nullptr;
}

const Surface* Workspace::findSurface(SurfaceID id) const { return const_cast<Workspace*>(this)->findSurface(id); }

Tab* Workspace::activeTabOf(SurfaceID surfaceID)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface || surface->tabCount <= 0)
        return nullptr;
    return &surface->tabs[surface->activeTab];
}

Tab* Workspace::tabOf(SurfaceID surfaceID, TabID tabID)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface)
        return nullptr;
    for (int index = 0; index < surface->tabCount; ++index) {
        if (surface->tabs[index].id == tabID)
            return &surface->tabs[index];
    }
    return nullptr;
}

SurfaceID Workspace::surfaceIDAt(int index) const
{
    if (index < 0 || index >= m_surfaceCount)
        return invalidSurface;
    return m_surfaces[index].id;
}

const Surface* Workspace::surface(SurfaceID id) const { return findSurface(id); }

bool Workspace::setActiveSurface(SurfaceID id)
{
    if (findSurface(id) == nullptr)
        return false;
    m_activeSurface = id;
    return true;
}

SurfaceID Workspace::addSurface()
{
    if (m_surfaceCount >= maxSurfaces || !initSurface(m_surfaces[m_surfaceCount]))
        return invalidSurface;
    const SurfaceID id = m_surfaces[m_surfaceCount].id;
    ++m_surfaceCount;
    m_activeSurface = id;
    return id;
}

bool Workspace::closeSurface(SurfaceID id)
{
    if (m_surfaceCount <= 1) // a Workspace always keeps at least one surface
        return false;
    int index = -1;
    for (int i = 0; i < m_surfaceCount; ++i)
        if (m_surfaces[i].id == id) {
            index = i;
            break;
        }
    if (index < 0)
        return false;
    for (int i = index; i + 1 < m_surfaceCount; ++i)
        m_surfaces[i] = m_surfaces[i + 1];
    --m_surfaceCount;
    if (m_activeSurface == id) {
        const int neighbor = index < m_surfaceCount ? index : m_surfaceCount - 1;
        m_activeSurface = m_surfaces[neighbor].id;
    }
    return true;
}

int Workspace::tabCount(SurfaceID surfaceID) const
{
    const Surface* surface = findSurface(surfaceID);
    return surface != nullptr ? surface->tabCount : 0;
}

TabID Workspace::activeTab(SurfaceID surfaceID) const
{
    const Surface* surface = findSurface(surfaceID);
    if (!surface || surface->tabCount <= 0)
        return invalidTab;
    return surface->tabs[surface->activeTab].id;
}

TabID Workspace::tabAt(SurfaceID surfaceID, int index) const
{
    const Surface* surface = findSurface(surfaceID);
    if (!surface || index < 0 || index >= surface->tabCount)
        return invalidTab;
    return surface->tabs[index].id;
}

std::string_view Workspace::tabTitle(SurfaceID surfaceID, TabID tabID) const
{
    const Surface* surface = findSurface(surfaceID);
    if (!surface) {
        return {};
    }
    for (int index = 0; index < surface->tabCount; ++index) {
        if (surface->tabs[index].id == tabID) {
            return surface->tabs[index].title;
        }
    }
    return {};
}

bool Workspace::renameTab(SurfaceID surfaceID, TabID tabID, std::string_view title)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface) {
        return false;
    }
    for (int index = 0; index < surface->tabCount; ++index) {
        if (surface->tabs[index].id == tabID) {
            // Kept as every surface keeps a typed title, whoever sent it: a
            // client's verb and a restored record arrive here alike.
            surface->tabs[index].title = normalizedCustomTitle(title);
            return true;
        }
    }
    return false;
}

TabID Workspace::addTab(SurfaceID surfaceID)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface || surface->tabCount >= maxTabsPerSurface)
        return invalidTab;
    const std::uint32_t tabNumber = mintNumber(Identity::ObjectKind::Tab);
    const std::uint32_t paneNumber = mintNumber(Identity::ObjectKind::Pane);
    if (!tabNumber || !paneNumber)
        return invalidTab;
    Tab& tab = surface->tabs[surface->tabCount];
    tab.id = TabID { tabNumber };
    tab.title.clear();
    tab.panes = Ubiquitty::Panes::Tree(static_cast<int>(paneNumber));
    surface->activeTab = surface->tabCount;
    ++surface->tabCount;
    clearZoomIfStale(*surface); // a fresh single-pane tab is never zoomed
    return tab.id;
}

bool Workspace::closeTab(SurfaceID surfaceID, TabID tabID)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface || surface->tabCount <= 1) // keep at least one tab
        return false;
    int index = -1;
    for (int i = 0; i < surface->tabCount; ++i)
        if (surface->tabs[i].id == tabID) {
            index = i;
            break;
        }
    if (index < 0)
        return false;
    for (int i = index; i + 1 < surface->tabCount; ++i)
        surface->tabs[i] = surface->tabs[i + 1];
    --surface->tabCount;
    // Keep the active tab pointing at the same logical tab where possible.
    if (surface->activeTab > index || surface->activeTab >= surface->tabCount)
        --surface->activeTab;
    if (surface->activeTab < 0)
        surface->activeTab = 0;
    clearZoomIfStale(*surface); // the active tab may have changed
    return true;
}

bool Workspace::setActiveTab(SurfaceID surfaceID, TabID tabID)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface)
        return false;
    for (int i = 0; i < surface->tabCount; ++i)
        if (surface->tabs[i].id == tabID) {
            surface->activeTab = i;
            clearZoomIfStale(*surface); // zoom belongs to the previously active tab
            return true;
        }
    return false;
}

PaneID Workspace::focusedPane(SurfaceID surfaceID) const
{
    const Tab* tab = const_cast<Workspace*>(this)->activeTabOf(surfaceID);
    return tab != nullptr ? static_cast<PaneID>(tab->panes.focusedPaneID()) : invalidPane;
}

PaneID Workspace::splitFocusedPane(SurfaceID surfaceID, Ubiquitty::Panes::Orientation orientation, float ratio)
{
    Tab* tab = activeTabOf(surfaceID);
    if (!tab || tab->panes.paneCount() >= maxPanesPerTab)
        return invalidPane;
    const std::uint32_t paneNumber = mintNumber(Identity::ObjectKind::Pane);
    if (!paneNumber)
        return invalidPane;
    const PaneID newPane = PaneID { paneNumber };
    const int node = tab->panes.split(orientation, static_cast<int>(newPane), ratio);
    if (node == Ubiquitty::Panes::invalidNode)
        return invalidPane; // split failed; the consumed id is skipped
    return newPane;
}

bool Workspace::closeFocusedPane(SurfaceID surfaceID)
{
    Tab* tab = activeTabOf(surfaceID);
    if (!tab)
        return false;
    if (tab->panes.paneCount() > 1) {
        if (!tab->panes.closeFocused())
            return false;
        // The closed pane may have been the zoomed one, or the collapse may
        // have left a single pane — either clears the zoom.
        if (Surface* surface = findSurface(surfaceID))
            clearZoomIfStale(*surface);
        return true;
    }
    // The tab's last pane → close the whole tab (false if it was the last tab).
    return closeTab(surfaceID, tab->id);
}

bool Workspace::closePane(SurfaceID surfaceID, PaneID pane)
{
    Tab* tab = activeTabOf(surfaceID);
    if (!tab)
        return false;
    // closePane is for collapsing one split level by id; it leaves m_focused put
    // unless the CLOSED pane was the focused one (then the sibling is promoted).
    // It refuses the last pane, so a lone-pane window still routes through the
    // tab/window close path (the macOS caller guards that case before us).
    if (!tab->panes.closePane(static_cast<int>(pane)))
        return false;
    // The closed pane may have been the zoomed one, or the collapse may have left
    // a single pane — either clears the zoom.
    if (Surface* surface = findSurface(surfaceID))
        clearZoomIfStale(*surface);
    return true;
}

bool Workspace::adoptWorkspace(std::span<const AdoptedSurface> surfaces, SurfaceID activeSurface)
{
    if (surfaces.empty() || surfaces.size() > static_cast<std::size_t>(maxSurfaces))
        return false;
    std::vector<std::uint32_t> paneIDs;
    bool activeSurfaceListed = false;
    for (std::size_t index = 0; index < surfaces.size(); ++index) {
        const AdoptedSurface& surface = surfaces[index];
        if (surface.id == invalidSurface || surface.tabs.empty() || surface.tabs.size() > static_cast<std::size_t>(maxTabsPerSurface))
            return false;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (surfaces[earlier].id == surface.id)
                return false;
        bool activeTabListed = false;
        for (const AdoptedTab& tab : surface.tabs) {
            if (tab.id == invalidTab || tab.panes == nullptr || tab.panes->paneCount() < 1)
                return false;
            for (const AdoptedSurface& other : surfaces)
                for (const AdoptedTab& otherTab : other.tabs)
                    if (&otherTab != &tab && otherTab.id == tab.id)
                        return false;
            Ubiquitty::Panes::forEachLeaf(*tab.panes, [&](int paneID) { paneIDs.push_back(static_cast<std::uint32_t>(paneID)); });
            activeTabListed = activeTabListed || tab.id == surface.activeTab;
        }
        if (!activeTabListed)
            return false;
        activeSurfaceListed = activeSurfaceListed || surface.id == activeSurface;
    }
    if (!activeSurfaceListed)
        return false;
    std::ranges::sort(paneIDs);
    if (std::ranges::adjacent_find(paneIDs) != paneIDs.end() || paneIDs.front() == std::to_underlying(invalidPane))
        return false;

    m_surfaceCount = 0;
    std::uint32_t highestSurface = 0;
    std::uint32_t highestTab = 0;
    for (const AdoptedSurface& adopted : surfaces) {
        Surface& surface = m_surfaces[m_surfaceCount++];
        surface.id = adopted.id;
        surface.tabCount = 0;
        surface.activeTab = 0;
        surface.zoomedPane = adopted.zoomedPane;
        for (const AdoptedTab& adoptedTab : adopted.tabs) {
            Tab& tab = surface.tabs[surface.tabCount];
            tab.id = adoptedTab.id;
            tab.title.assign(adoptedTab.title);
            tab.panes = *adoptedTab.panes;
            if (tab.id == adopted.activeTab)
                surface.activeTab = surface.tabCount;
            ++surface.tabCount;
            highestTab = std::max(highestTab, std::to_underlying(tab.id));
        }
        clearZoomIfStale(surface);
        highestSurface = std::max(highestSurface, std::to_underlying(surface.id));
    }
    m_activeSurface = activeSurface;
    reserveNumber(Identity::ObjectKind::Window, highestSurface);
    reserveNumber(Identity::ObjectKind::Tab, highestTab);
    reserveNumber(Identity::ObjectKind::Pane, paneIDs.back());
    return true;
}

SurfaceID Workspace::surfaceHolding(PaneID pane) const
{
    for (int index = 0; index < m_surfaceCount; ++index)
        if (tabHolding(m_surfaces[index].id, pane) != invalidTab)
            return m_surfaces[index].id;
    return invalidSurface;
}

TabID Workspace::tabHolding(SurfaceID surfaceID, PaneID pane) const
{
    const Surface* surface = findSurface(surfaceID);
    if (surface == nullptr)
        return invalidTab;
    for (int index = 0; index < surface->tabCount; ++index)
        if (surface->tabs[index].panes.holdsPane(static_cast<int>(pane)))
            return surface->tabs[index].id;
    return invalidTab;
}

bool Workspace::removePane(PaneID pane)
{
    for (int surfaceIndex = 0; surfaceIndex < m_surfaceCount; ++surfaceIndex) {
        Surface& surface = m_surfaces[surfaceIndex];
        for (int tabIndex = 0; tabIndex < surface.tabCount; ++tabIndex) {
            Tab& tab = surface.tabs[tabIndex];
            if (!tab.panes.holdsPane(static_cast<int>(pane)))
                continue;
            if (tab.panes.paneCount() > 1) {
                if (!tab.panes.closePane(static_cast<int>(pane)))
                    return false;
                clearZoomIfStale(surface);
                return true;
            }
            if (surface.tabCount > 1)
                return closeTab(surface.id, tab.id);
            return closeSurface(surface.id);
        }
    }
    return false;
}

bool Workspace::focusPane(SurfaceID surfaceID, PaneID pane)
{
    Tab* tab = activeTabOf(surfaceID);
    return tab != nullptr && tab->panes.focusPane(static_cast<int>(pane));
}

bool Workspace::moveFocus(SurfaceID surfaceID, Ubiquitty::Panes::Direction direction)
{
    Tab* tab = activeTabOf(surfaceID);
    return tab != nullptr && tab->panes.moveFocus(direction);
}

int Workspace::tabIndexHolding(const Surface& surface, PaneID pane)
{
    for (int i = 0; i < surface.tabCount; ++i) {
        bool found = false;
        Ubiquitty::Panes::forEachLeaf(surface.tabs[i].panes, [&](int paneID) {
            if (static_cast<PaneID>(paneID) == pane)
                found = true;
        });
        if (found)
            return i;
    }
    return -1;
}

bool Workspace::movePane(SurfaceID surfaceID, PaneID pane, PaneID target, Ubiquitty::Panes::Orientation orientation, Ubiquitty::Panes::InsertSide side)
{
    Tab* tab = activeTabOf(surfaceID);
    if (!tab || !tab->panes.movePane(static_cast<int>(pane), static_cast<int>(target), orientation, side))
        return false;
    // A move never changes pane liveness or count, so a zoom (which names
    // the pane, not the position) survives; the call is the standard
    // post-mutation coherence sweep.
    if (Surface* surface = findSurface(surfaceID))
        clearZoomIfStale(*surface);
    return true;
}

bool Workspace::swapPanes(SurfaceID surfaceID, PaneID firstPane, PaneID secondPane)
{
    Tab* tab = activeTabOf(surfaceID);
    return tab != nullptr && tab->panes.swapPanes(static_cast<int>(firstPane), static_cast<int>(secondPane));
}

bool Workspace::movePaneToTab(SurfaceID surfaceID, PaneID pane, TabID targetTab, PaneID targetPane, Ubiquitty::Panes::Orientation orientation, Ubiquitty::Panes::InsertSide side, Ubiquitty::Panes::InsertTarget insertTarget)
{
    Surface* source = findSurface(surfaceID);
    const SurfaceID destinationID = surfaceHolding(targetPane);
    Surface* destination = findSurface(destinationID);
    if (!source || !destination || pane == invalidPane || pane == targetPane || tabHolding(destinationID, targetPane) != targetTab) {
        return false;
    }
    const int sourceIndex = tabIndexHolding(*source, pane);
    if (sourceIndex < 0) {
        return false;
    }
    const int destinationIndex = tabIndexHolding(*destination, targetPane);
    Tab& sourceTab = source->tabs[sourceIndex];
    Tab& destinationTab = destination->tabs[destinationIndex];
    if (sourceTab.id == targetTab) {
        if (!sourceTab.panes.movePane(static_cast<int>(pane), static_cast<int>(targetPane), orientation, side, insertTarget)) {
            return false;
        }
        if (destination->activeTab == destinationIndex) {
            destination->zoomedPane = invalidPane;
        }
        return true;
    }
    // The destination validates capacity before the source loses its leaf.
    if (destinationTab.panes.insertPane(static_cast<int>(pane), static_cast<int>(targetPane), orientation,
            Ubiquitty::Panes::defaultRatio, side, insertTarget)
        == Ubiquitty::Panes::invalidNode) {
        return false;
    }
    if (destination->activeTab == destinationIndex) {
        destination->zoomedPane = invalidPane;
    }
    if (sourceTab.panes.paneCount() > 1) {
        sourceTab.panes.closePane(static_cast<int>(pane));
        clearZoomIfStale(*source);
    } else if (source->tabCount > 1) {
        closeTab(surfaceID, sourceTab.id);
    } else {
        closeSurface(surfaceID);
    }
    return true;
}

bool Workspace::moveTabToPane(SurfaceID surfaceID, TabID tabID, PaneID targetPane, Ubiquitty::Panes::Orientation orientation, Ubiquitty::Panes::InsertSide side, Ubiquitty::Panes::InsertTarget insertTarget)
{
    Surface* source = findSurface(surfaceID);
    const SurfaceID destinationID = surfaceHolding(targetPane);
    Surface* destination = findSurface(destinationID);
    if (!source || !destination) {
        return false;
    }
    Tab* sourceTab = nullptr;
    for (int index = 0; index < source->tabCount; ++index) {
        if (source->tabs[index].id == tabID) {
            sourceTab = &source->tabs[index];
            break;
        }
    }
    const int destinationIndex = tabIndexHolding(*destination, targetPane);
    if (!sourceTab || sourceTab->panes.holdsPane(static_cast<int>(targetPane))
        || !destination->tabs[destinationIndex].panes.insertTree(sourceTab->panes, static_cast<int>(targetPane), orientation, side, insertTarget)) {
        return false;
    }
    destination->activeTab = destinationIndex;
    destination->zoomedPane = invalidPane;
    m_activeSurface = destinationID;
    if (source->tabCount > 1) {
        closeTab(surfaceID, tabID);
    } else {
        closeSurface(surfaceID);
    }
    return true;
}

bool Workspace::moveTabToSurface(SurfaceID surfaceID, TabID tabID, SurfaceID destinationID)
{
    Surface* source = findSurface(surfaceID);
    Surface* destination = findSurface(destinationID);
    if (!source || !destination || source == destination || destination->tabCount >= maxTabsPerSurface) {
        return false;
    }
    for (int index = 0; index < source->tabCount; ++index) {
        if (source->tabs[index].id != tabID) {
            continue;
        }
        destination->tabs[destination->tabCount] = source->tabs[index];
        destination->activeTab = destination->tabCount++;
        destination->zoomedPane = invalidPane;
        if (source->tabCount > 1) {
            closeTab(surfaceID, tabID);
        } else {
            closeSurface(surfaceID);
        }
        m_activeSurface = destinationID;
        return true;
    }
    return false;
}

bool Workspace::moveTab(TabID tabID, SurfaceID destinationID, int index)
{
    Surface* destination = findSurface(destinationID);
    if (!destination || index < 0)
        return false;
    Surface* source = nullptr;
    int from = -1;
    for (int surfaceIndex = 0; surfaceIndex < m_surfaceCount && !source; ++surfaceIndex) {
        Surface& candidate = m_surfaces[surfaceIndex];
        for (int tabIndex = 0; tabIndex < candidate.tabCount; ++tabIndex) {
            if (candidate.tabs[tabIndex].id == tabID) {
                source = &candidate;
                from = tabIndex;
                break;
            }
        }
    }
    if (!source)
        return false;
    if (source == destination) {
        if (index >= source->tabCount)
            return false;
        const TabID shown = source->tabs[source->activeTab].id;
        Tab* const tabs = source->tabs;
        if (from < index)
            std::rotate(tabs + from, tabs + from + 1, tabs + index + 1);
        else
            std::rotate(tabs + index, tabs + from, tabs + from + 1);
        for (int tabIndex = 0; tabIndex < source->tabCount; ++tabIndex) {
            if (tabs[tabIndex].id == shown)
                source->activeTab = tabIndex;
        }
        return true;
    }
    if (index > destination->tabCount || destination->tabCount >= maxTabsPerSurface)
        return false;
    for (int tabIndex = destination->tabCount; tabIndex > index; --tabIndex)
        destination->tabs[tabIndex] = destination->tabs[tabIndex - 1];
    destination->tabs[index] = source->tabs[from];
    ++destination->tabCount;
    destination->activeTab = index;
    destination->zoomedPane = invalidPane;
    // Closing the source may move the surfaces; nothing below reads either.
    const SurfaceID sourceID = source->id;
    if (source->tabCount > 1)
        closeTab(sourceID, tabID);
    else
        closeSurface(sourceID);
    m_activeSurface = destinationID;
    return true;
}

SurfaceID Workspace::extractTabToSurface(SurfaceID surfaceID, TabID tabID)
{
    Surface* source = findSurface(surfaceID);
    if (!source || source->tabCount <= 1 || !paneTree(surfaceID, tabID) || m_surfaceCount >= maxSurfaces) {
        return invalidSurface;
    }
    const SurfaceID created = addSurface();
    if (created == invalidSurface)
        return invalidSurface;
    Surface* destination = findSurface(created);
    for (int index = 0; index < source->tabCount; ++index) {
        if (source->tabs[index].id == tabID) {
            destination->tabs[0] = source->tabs[index];
            closeTab(surfaceID, tabID);
            return created;
        }
    }
    return invalidSurface;
}

bool Workspace::movePaneToNewTab(SurfaceID surfaceID, PaneID pane, SurfaceID destinationID)
{
    Surface* source = findSurface(surfaceID);
    Surface* destination = findSurface(destinationID);
    if (!source || !destination || destination->tabCount >= maxTabsPerSurface) {
        return false;
    }
    const int sourceIndex = tabIndexHolding(*source, pane);
    if (sourceIndex < 0) {
        return false;
    }
    Tab& sourceTab = source->tabs[sourceIndex];
    if (sourceTab.panes.paneCount() == 1) {
        return source == destination ? setActiveTab(surfaceID, sourceTab.id) : moveTabToSurface(surfaceID, sourceTab.id, destinationID);
    }
    const std::uint32_t tabNumber = mintNumber(Identity::ObjectKind::Tab);
    if (!tabNumber) {
        return false;
    }
    Tab& created = destination->tabs[destination->tabCount];
    created.id = TabID { tabNumber };
    created.title.clear();
    created.panes = Ubiquitty::Panes::Tree(static_cast<int>(pane));
    destination->activeTab = destination->tabCount++;
    destination->zoomedPane = invalidPane;
    sourceTab.panes.closePane(static_cast<int>(pane));
    clearZoomIfStale(*source);
    m_activeSurface = destinationID;
    return true;
}

TabID Workspace::extractPaneToTab(SurfaceID surfaceID, PaneID pane)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface || pane == invalidPane || surface->tabCount >= maxTabsPerSurface)
        return invalidTab;
    const int sourceIndex = tabIndexHolding(*surface, pane);
    if (sourceIndex < 0)
        return invalidTab;
    Tab& source = surface->tabs[sourceIndex];
    // A tab's only pane is already tab-shaped — nothing to extract.
    if (source.panes.paneCount() <= 1)
        return invalidTab;

    // Root the new tab at the EXISTING pane id (the pane keeps its identity
    // across the extraction), make it active, then collapse the source.
    const std::uint32_t tabNumber = mintNumber(Identity::ObjectKind::Tab);
    if (!tabNumber)
        return invalidTab;
    Tab& created = surface->tabs[surface->tabCount];
    created.id = TabID { tabNumber };
    created.title.clear();
    created.panes = Ubiquitty::Panes::Tree(static_cast<int>(pane));
    surface->activeTab = surface->tabCount;
    ++surface->tabCount;
    source.panes.closePane(static_cast<int>(pane));
    clearZoomIfStale(*surface);
    return created.id;
}

SurfaceID Workspace::extractPaneToSurface(SurfaceID surfaceID, PaneID pane)
{
    Surface* source = findSurface(surfaceID);
    if (!source || pane == invalidPane || m_surfaceCount >= maxSurfaces)
        return invalidSurface;
    const int sourceIndex = tabIndexHolding(*source, pane);
    if (sourceIndex < 0)
        return invalidSurface;
    Tab& sourceTab = source->tabs[sourceIndex];
    // A surface's only pane in its only tab is already surface-shaped.
    if (source->tabCount <= 1 && sourceTab.panes.paneCount() <= 1)
        return invalidSurface;

    // addSurface mints a fresh surface (active) whose root pane id we
    // discard by re-rooting at the EXISTING pane — the skipped id is
    // harmless, exactly like a failed splitFocusedPane's. The surfaces
    // array never moves on add, so `source` stays valid.
    const SurfaceID createdID = addSurface();
    if (createdID == invalidSurface)
        return invalidSurface;
    Surface* created = findSurface(createdID);
    created->tabs[0].panes = Ubiquitty::Panes::Tree(static_cast<int>(pane));

    if (sourceTab.panes.paneCount() > 1)
        sourceTab.panes.closePane(static_cast<int>(pane));
    else
        closeTab(surfaceID, sourceTab.id); // >=2 tabs by the surface-shaped guard
    clearZoomIfStale(*source);
    return createdID;
}

bool Workspace::balanceActiveTab(SurfaceID surfaceID)
{
    Tab* tab = activeTabOf(surfaceID);
    return tab != nullptr && tab->panes.balance();
}

bool Workspace::adjustFocusedPaneRatio(SurfaceID surfaceID, Ubiquitty::Panes::Direction direction, float step)
{
    Tab* tab = activeTabOf(surfaceID);
    return tab != nullptr && tab->panes.adjustRatio(tab->panes.focusedPaneID(), direction, step);
}

bool Workspace::setDividerRatio(SurfaceID surfaceID, int splitIndex, float ratio)
{
    Tab* tab = activeTabOf(surfaceID);
    return tab != nullptr && tab->panes.setRatio(splitIndex, ratio);
}

bool Workspace::balanceTab(SurfaceID surface, TabID tabID)
{
    Tab* tab = tabOf(surface, tabID);
    return tab && tab->panes.balance();
}

bool Workspace::adjustPaneRatio(SurfaceID surface, TabID tabID, PaneID pane, Ubiquitty::Panes::Direction direction, float step)
{
    Tab* tab = tabOf(surface, tabID);
    return tab && tab->panes.adjustRatio(static_cast<int>(std::to_underlying(pane)), direction, step);
}

bool Workspace::setTabDividerRatio(SurfaceID surface, TabID tabID, int splitIndex, float ratio)
{
    Tab* tab = tabOf(surface, tabID);
    return tab && tab->panes.setRatio(splitIndex, ratio);
}

PaneID Workspace::zoomedPane(SurfaceID surfaceID) const
{
    const Surface* surface = findSurface(surfaceID);
    return surface != nullptr ? surface->zoomedPane : invalidPane;
}

bool Workspace::toggleZoom(SurfaceID surfaceID, PaneID pane)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface || pane == invalidPane)
        return false;
    Tab& tab = surface->tabs[surface->activeTab];
    // Nothing to expand in a single-pane tab (mirrors the macOS guard).
    if (tab.panes.paneCount() <= 1)
        return false;
    // `pane` must be a live leaf of the active tab.
    bool live = false;
    Ubiquitty::Panes::forEachLeaf(tab.panes, [&](int paneID) {
        if (static_cast<PaneID>(paneID) == pane)
            live = true;
    });
    if (!live)
        return false;
    // Toggle: zooming the already-zoomed pane clears the zoom.
    surface->zoomedPane = (surface->zoomedPane == pane) ? invalidPane : pane;
    return true;
}

const Ubiquitty::Panes::Tree* Workspace::activePaneTree(SurfaceID surfaceID) const
{
    const Tab* tab = const_cast<Workspace*>(this)->activeTabOf(surfaceID);
    return tab != nullptr ? &tab->panes : nullptr;
}

const Ubiquitty::Panes::Tree* Workspace::paneTree(SurfaceID surfaceID, TabID tabID) const
{
    const Surface* surface = findSurface(surfaceID);
    if (!surface)
        return nullptr;
    for (int index = 0; index < surface->tabCount; ++index) {
        if (surface->tabs[index].id == tabID)
            return &surface->tabs[index].panes;
    }
    return nullptr;
}

bool Workspace::adoptTree(SurfaceID surfaceID, const Ubiquitty::Panes::Tree& tree)
{
    Surface* surface = findSurface(surfaceID);
    if (!surface || surface->tabCount <= 0)
        return false;
    surface->tabs[surface->activeTab].panes = tree;
    // A zoom on a pane the adopted tree no longer carries (or a collapse back to
    // one pane) must be dropped — the topology was replaced wholesale.
    clearZoomIfStale(*surface);
    // Reconcile the allocator: any pane id injected by the adopted tree (a tmux
    // %M, a restored layout id) must be BELOW the next minted id, so a later
    // splitFocusedPane can't re-mint a live id. Scan every tab of this surface
    // (a zoom-detached sibling or another tab can hold an adopted id too).
    PaneID highest = invalidPane;
    for (int i = 0; i < surface->tabCount; ++i) {
        Ubiquitty::Panes::forEachLeaf(surface->tabs[i].panes, [&](int paneID) {
            if (paneID > 0 && static_cast<PaneID>(paneID) > highest)
                highest = static_cast<PaneID>(paneID);
        });
    }
    reserveNumber(Identity::ObjectKind::Pane, std::to_underlying(highest));
    return true;
}

} // namespace Ubiquitty::Session::Layout
