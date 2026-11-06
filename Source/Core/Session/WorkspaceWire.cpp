// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/WorkspaceWire.h"

#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Protocol/Tmux/LayoutNode.h"
#include "Core/Protocol/Tmux/PaneLock.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Session::Layout {

namespace {

// The layout node a tmux layout would parse to for a node of a structure's
// split: the same cells at the same place, a leaf under its pane's layout id.
// The leaves of a split are the tab's panes in order, so `nextLeaf` walks
// them; false when the split has more leaves than the tab has panes.
bool layoutNodeOf(const Protocol::Sync::TopologyNode& node, std::span<const TreePane> panes, std::size_t& nextLeaf, Protocol::Tmux::LayoutNode& out)
{
    out.x = node.offsetColumns;
    out.y = node.offsetRows;
    out.width = node.columns;
    out.height = node.rows;
    if (node.isLeaf()) {
        if (nextLeaf >= panes.size())
            return false;
        out.split = Protocol::Tmux::Split::Leaf;
        out.paneID = static_cast<Protocol::Tmux::PaneID>(std::to_underlying(panes[nextLeaf++].id));
        return true;
    }
    // The topology's Horizontal is the axis its children are laid along:
    // side by side, which a layout calls columns.
    out.split = node.axis == Protocol::Sync::SplitAxis::Horizontal ? Protocol::Tmux::Split::Columns : Protocol::Tmux::Split::Rows;
    out.children.resize(node.children.size());
    for (std::size_t child = 0; child < node.children.size(); ++child) {
        if (!layoutNodeOf(node.children[child], panes, nextLeaf, out.children[child]))
            return false;
    }
    return true;
}

// Actual pane grids can arrive at different times after a resize. The
// canonical binary topology carries the layout's ratio independently.
bool adoptRatios(const Protocol::Sync::TopologyNode& node, Ubiquitty::Panes::Tree& tree, int index)
{
    if (node.isLeaf())
        return tree.isLeaf(index);
    if (node.children.size() != 2 || tree.isLeaf(index) || !std::isfinite(node.ratio) || node.ratio < 0 || node.ratio > 1)
        return false;
    const auto& split = tree.node(index);
    return tree.setRatio(index, static_cast<float>(node.ratio))
        && adoptRatios(node.children[0], tree, split.first)
        && adoptRatios(node.children[1], tree, split.second);
}

void appendNode(std::string& text, const Protocol::Sync::TopologyNode& node, std::span<const TreePane> panes, std::size_t& nextLeaf)
{
    text += std::to_string(node.columns) + "x" + std::to_string(node.rows) + "," + std::to_string(node.offsetColumns) + "," + std::to_string(node.offsetRows);
    if (node.isLeaf()) {
        if (nextLeaf < panes.size()) {
            text += "," + std::to_string(std::to_underlying(panes[nextLeaf].id)) + ":" + std::to_string(panes[nextLeaf].sessionID);
            ++nextLeaf;
        }
        return;
    }
    text += ",r" + std::to_string(std::bit_cast<std::uint64_t>(node.ratio));
    text += node.axis == Protocol::Sync::SplitAxis::Horizontal ? '{' : '[';
    for (std::size_t child = 0; child < node.children.size(); ++child) {
        if (child != 0)
            text += ',';
        appendNode(text, node.children[child], panes, nextLeaf);
    }
    text += node.axis == Protocol::Sync::SplitAxis::Horizontal ? '}' : ']';
}

} // namespace

bool adoptWorkspace(const TreeWorkspace& structure, Workspace& out, PaneBoundFn onPane, void* context)
{
    if (structure.windows.empty() || structure.hostActiveWindow >= structure.windows.size())
        return false;
    std::size_t tabTotal = 0;
    for (const TreeWindow& window : structure.windows)
        tabTotal += window.tabs.size();
    // Reserved once: the adopted spans point into these.
    std::vector<Ubiquitty::Panes::Tree> trees;
    std::vector<AdoptedTab> tabs;
    std::vector<AdoptedSurface> surfaces;
    trees.reserve(tabTotal);
    tabs.reserve(tabTotal);
    surfaces.reserve(structure.windows.size());
    for (const TreeWindow& window : structure.windows) {
        if (window.tabs.empty() || window.hostActiveTab >= window.tabs.size())
            return false;
        const std::size_t firstTab = tabs.size();
        for (const TreeTab& tab : window.tabs) {
            if (tab.panes.empty() || tab.hostFocusedPane >= tab.panes.size())
                return false;
            Protocol::Tmux::LayoutNode root;
            std::size_t leaves = 0;
            if (!layoutNodeOf(tab.split, tab.panes, leaves, root) || leaves != tab.panes.size())
                return false;
            // Seeded with the split's own leftmost pane, which is the leaf
            // buildPaneTreeInto nests against.
            Ubiquitty::Panes::Tree& tree = trees.emplace_back(Protocol::Tmux::leftmostPaneID(root));
            Protocol::Tmux::buildPaneTreeInto(tree, root);
            if (!adoptRatios(tab.split, tree, tree.root()))
                return false;
            // The pane list and the split name the same leaves, each once.
            if (static_cast<std::size_t>(tree.paneCount()) != tab.panes.size())
                return false;
            for (const TreePane& pane : tab.panes) {
                if (!tree.holdsPane(static_cast<int>(std::to_underlying(pane.id))))
                    return false;
            }
            tree.focusPane(static_cast<int>(std::to_underlying(tab.panes[tab.hostFocusedPane].id)));
            tabs.push_back({ tab.id, &tree, tab.name });
        }
        PaneID zoomed = invalidPane;
        if (window.hostZoomedPane) {
            if (window.hostZoomedPane->tab >= window.tabs.size() || window.hostZoomedPane->pane >= window.tabs[window.hostZoomedPane->tab].panes.size())
                return false;
            zoomed = window.tabs[window.hostZoomedPane->tab].panes[window.hostZoomedPane->pane].id;
        }
        surfaces.push_back({ window.id, window.tabs[window.hostActiveTab].id, zoomed, std::span<const AdoptedTab>(tabs).subspan(firstTab, window.tabs.size()) });
    }
    if (!out.adoptWorkspace(surfaces, structure.windows[structure.hostActiveWindow].id))
        return false;
    for (const TreeWindow& window : structure.windows) {
        for (const TreeTab& tab : window.tabs) {
            for (const TreePane& pane : tab.panes)
                onPane(context, pane.id, pane.sessionID);
        }
    }
    return true;
}

namespace {

void appendArrangement(std::string& text, const Protocol::Sync::TopologyNode& node, std::span<const TreePane> panes, std::size_t& nextLeaf)
{
    if (node.isLeaf()) {
        if (nextLeaf < panes.size()) {
            text += std::to_string(std::to_underlying(panes[nextLeaf].id)) + ":" + std::to_string(panes[nextLeaf].sessionID);
            ++nextLeaf;
        }
        return;
    }
    text += "r" + std::to_string(std::bit_cast<std::uint64_t>(node.ratio));
    text += node.axis == Protocol::Sync::SplitAxis::Horizontal ? '{' : '[';
    for (std::size_t child = 0; child < node.children.size(); ++child) {
        if (child != 0)
            text += ',';
        appendArrangement(text, node.children[child], panes, nextLeaf);
    }
    text += node.axis == Protocol::Sync::SplitAxis::Horizontal ? '}' : ']';
}

} // namespace

std::string arrangementSignature(const TreeWorkspace& structure)
{
    std::string text = std::to_string(structure.id) + "\n";
    const auto appendText = [&](std::string_view value) {
        text += std::to_string(value.size()) + ":";
        text.append(value);
    };
    appendText(structure.name);
    appendText(structure.label);
    text += "\n" + std::to_string(std::to_underlying(structure.state)) + (structure.endsWhenDetached ? "e" : "") + "\n"
        + std::to_string(structure.hostActiveWindow) + "\n";
    for (const TreeWindow& window : structure.windows) {
        text += "w" + std::to_string(std::to_underlying(window.id)) + " " + std::to_string(window.hostActiveTab);
        if (window.hostZoomedPane)
            text += " z" + std::to_string(window.hostZoomedPane->tab) + "." + std::to_string(window.hostZoomedPane->pane);
        text += " ";
        appendText(window.label);
        text += "\n";
        for (const TreeTab& tab : window.tabs) {
            text += "t" + std::to_string(std::to_underlying(tab.id)) + " " + std::to_string(tab.hostFocusedPane) + " ";
            appendText(tab.name);
            appendText(tab.label);
            std::size_t leaves = 0;
            appendArrangement(text, tab.split, tab.panes, leaves);
            text += "\n";
        }
    }
    return text;
}

std::string mirrorSignature(const TreeWorkspace& structure)
{
    std::string text = std::to_string(structure.id) + "\n";
    const auto appendText = [&](std::string_view value) {
        text += std::to_string(value.size()) + ":";
        text.append(value);
    };
    appendText(structure.name);
    appendText(structure.label);
    text += "\n" + std::to_string(std::to_underlying(structure.state)) + (structure.endsWhenDetached ? "e" : "") + "\n"
        + std::to_string(structure.hostActiveWindow) + "\n";
    for (const TreeWindow& window : structure.windows) {
        text += "w" + std::to_string(std::to_underlying(window.id)) + " " + std::to_string(window.hostActiveTab);
        if (window.hostZoomedPane)
            text += " z" + std::to_string(window.hostZoomedPane->tab) + "." + std::to_string(window.hostZoomedPane->pane);
        text += " ";
        appendText(window.label);
        text += "\n";
        for (const TreeTab& tab : window.tabs) {
            text += "t" + std::to_string(std::to_underlying(tab.id)) + " " + std::to_string(tab.hostFocusedPane) + " ";
            appendText(tab.name);
            appendText(tab.label);
            text += "\n";
            std::size_t leaves = 0;
            appendNode(text, tab.split, tab.panes, leaves);
            text += "\n";
        }
    }
    return text;
}

namespace {

// Whether two split trees are cut the same way: leaves where leaves are and
// splits along the same axes, whatever their ids and ratios.
bool isSplitAlike(const Ubiquitty::Panes::Tree& former, int formerIndex, const Ubiquitty::Panes::Tree& restored, int restoredIndex, int depth = 0)
{
    if (depth > Ubiquitty::Panes::maxPanes || formerIndex < 0 || formerIndex >= Ubiquitty::Panes::maxNodes || restoredIndex < 0
        || restoredIndex >= Ubiquitty::Panes::maxNodes)
        return false;
    const bool isFormerLeaf = former.isLeaf(formerIndex);
    if (isFormerLeaf != restored.isLeaf(restoredIndex))
        return false;
    if (isFormerLeaf)
        return true;
    const Ubiquitty::Panes::Node formerNode = former.node(formerIndex);
    const Ubiquitty::Panes::Node restoredNode = restored.node(restoredIndex);
    return formerNode.orientation == restoredNode.orientation && isSplitAlike(former, formerNode.first, restored, restoredNode.first, depth + 1)
        && isSplitAlike(former, formerNode.second, restored, restoredNode.second, depth + 1);
}

std::uint32_t sessionBoundTo(std::span<const std::pair<std::uint32_t, std::uint32_t>> bindings, int pane)
{
    for (const auto& [bound, sessionID] : bindings) {
        if (static_cast<int>(bound) == pane)
            return sessionID;
    }
    return 0;
}

/// A pane and where an arrangement holds it.
struct PlacedPane {
    SurfaceID surface = invalidSurface;
    TabID tab = invalidTab;
    PaneID pane = invalidPane;
};

/// What an arrangement's shape is written in, ids and ratios aside.
enum class ShapePart : std::uint8_t {
    Window,
    Tab,
    SideBySide,
    Stacked,
    Pane,
};

/// The most panes passed over while trying the ways a restoration could have
/// lost panes, all ways together: one way is one pass over the former panes.
constexpr std::size_t maxLossWork = std::size_t { 1 } << 18;

/// An arrangement read once for the shapes it leaves when it loses panes:
/// its panes in the record's order, windows in order, each window's tabs in
/// order and each tab's leaves first child first, and how many panes lie
/// under each node of each tab's split tree.
class ArrangementShape {
public:
    explicit ArrangementShape(const Workspace& workspace)
        : m_workspace(workspace)
    {
        for (int surfaceIndex = 0; surfaceIndex < workspace.surfaceCount(); ++surfaceIndex) {
            const Surface* surface = workspace.surface(workspace.surfaceIDAt(surfaceIndex));
            if (!surface)
                continue;
            for (int tabIndex = 0; tabIndex < surface->tabCount && tabIndex < maxTabsPerSurface; ++tabIndex) {
                const Tab& tab = surface->tabs[tabIndex];
                m_leafCounts.emplace_back();
                countLeaves(tab.panes, tab.panes.root(), m_leafCounts.back(), 0);
                Ubiquitty::Panes::forEachLeaf(tab.panes, [&](int pane) {
                    m_panes.push_back({ .surface = surface->id, .tab = tab.id, .pane = PaneID { static_cast<std::uint32_t>(pane) } });
                });
            }
        }
    }

    [[nodiscard]] std::span<const PlacedPane> panes() const { return m_panes; }

    /// Writes the shape the arrangement is left with when it keeps only some
    /// of its panes: a split that keeps one side is that side, a tab that
    /// keeps no pane goes, and so does a window that keeps no tab, as a host
    /// lays out a record whose panes it could not all start.
    ///
    /// - Parameters:
    ///   - keptBefore: For each index of `panes()` and one past the last, how
    ///     many panes before it are kept.
    ///   - out: Receives the shape, replacing what it held.
    void write(std::span<const std::size_t> keptBefore, std::vector<ShapePart>& out) const
    {
        out.clear();
        std::size_t first = 0;
        std::size_t tabOrdinal = 0;
        for (int surfaceIndex = 0; surfaceIndex < m_workspace.surfaceCount(); ++surfaceIndex) {
            const Surface* surface = m_workspace.surface(m_workspace.surfaceIDAt(surfaceIndex));
            if (!surface)
                continue;
            bool isWindowWritten = false;
            for (int tabIndex = 0; tabIndex < surface->tabCount && tabIndex < maxTabsPerSurface; ++tabIndex, ++tabOrdinal) {
                const Tab& tab = surface->tabs[tabIndex];
                const LeafCounts& counts = m_leafCounts[tabOrdinal];
                const int root = tab.panes.root();
                const std::size_t leaves = root >= 0 && root < Ubiquitty::Panes::maxNodes ? counts[static_cast<std::size_t>(root)] : 0;
                if (keptBefore[first + leaves] > keptBefore[first]) {
                    if (!std::exchange(isWindowWritten, true))
                        out.push_back(ShapePart::Window);
                    out.push_back(ShapePart::Tab);
                    writeNode(tab.panes, counts, root, first, keptBefore, out, 0);
                }
                first += leaves;
            }
        }
    }

private:
    using LeafCounts = std::array<std::size_t, Ubiquitty::Panes::maxNodes>;

    static std::size_t countLeaves(const Ubiquitty::Panes::Tree& tree, int node, LeafCounts& counts, int depth)
    {
        if (depth > Ubiquitty::Panes::maxPanes || node < 0 || node >= Ubiquitty::Panes::maxNodes)
            return 0;
        const auto index = static_cast<std::size_t>(node);
        if (tree.isLeaf(node)) {
            counts[index] = 1;
        } else {
            const Ubiquitty::Panes::Node split = tree.node(node);
            counts[index] = countLeaves(tree, split.first, counts, depth + 1) + countLeaves(tree, split.second, counts, depth + 1);
        }
        return counts[index];
    }

    static void writeNode(const Ubiquitty::Panes::Tree& tree, const LeafCounts& counts, int node, std::size_t first, std::span<const std::size_t> keptBefore,
        std::vector<ShapePart>& out, int depth)
    {
        if (depth > Ubiquitty::Panes::maxPanes || node < 0 || node >= Ubiquitty::Panes::maxNodes)
            return;
        if (tree.isLeaf(node)) {
            out.push_back(ShapePart::Pane);
            return;
        }
        const Ubiquitty::Panes::Node split = tree.node(node);
        if (split.first < 0 || split.first >= Ubiquitty::Panes::maxNodes)
            return;
        const std::size_t middle = first + counts[static_cast<std::size_t>(split.first)];
        const std::size_t end = first + counts[static_cast<std::size_t>(node)];
        const bool isFirstKept = keptBefore[middle] > keptBefore[first];
        const bool isSecondKept = keptBefore[end] > keptBefore[middle];
        if (isFirstKept && isSecondKept)
            out.push_back(split.orientation == Ubiquitty::Panes::Orientation::Vertical ? ShapePart::SideBySide : ShapePart::Stacked);
        if (isFirstKept)
            writeNode(tree, counts, split.first, first, keptBefore, out, depth + 1);
        if (isSecondKept)
            writeNode(tree, counts, split.second, middle, keptBefore, out, depth + 1);
    }

    const Workspace& m_workspace;
    std::vector<PlacedPane> m_panes;
    std::vector<LeafCounts> m_leafCounts;
};

/// Whether choosing `chosen` of `count` can be done at most `limit` ways.
bool isChoiceCountAtMost(std::size_t count, std::size_t chosen, std::size_t limit)
{
    std::size_t ways = 1;
    for (std::size_t step = 1; step <= chosen; ++step) {
        ways = ways * (count - chosen + step) / step;
        if (ways > limit)
            return false;
    }
    return true;
}

/// Steps a choice of indices, ascending, to the next one of `count`.
///
/// - Returns: False once every choice has been made.
bool advanceChoice(std::vector<std::size_t>& choice, std::size_t count)
{
    for (std::size_t slot = choice.size(); slot-- > 0;) {
        if (choice[slot] < count - choice.size() + slot) {
            ++choice[slot];
            for (std::size_t next = slot + 1; next < choice.size(); ++next)
                choice[next] = choice[next - 1] + 1;
            return true;
        }
    }
    return false;
}

/// Pairs the panes of a restoration that holds fewer than the mirror did,
/// where every way the mirror's arrangement could have lost the missing
/// panes and been left with the restored one puts the same former pane in a
/// restored pane's place.
///
/// - Returns: Each restored pane's index in `restored.panes()` with the
///   former pane's in `former.panes()`, in the record's order; nothing when
///   no way of losing panes leaves the restored arrangement, or there are
///   more ways than the bound lets this try.
std::optional<std::vector<std::pair<std::size_t, std::size_t>>> pairAcrossLostPanes(const ArrangementShape& former, const ArrangementShape& restored)
{
    const std::size_t formerCount = former.panes().size();
    const std::size_t restoredCount = restored.panes().size();
    if (!restoredCount || restoredCount >= formerCount)
        return std::nullopt;
    // The smaller of the panes kept and the panes lost is the one chosen.
    const bool isChoiceKept = restoredCount <= formerCount - restoredCount;
    const std::size_t chosen = isChoiceKept ? restoredCount : formerCount - restoredCount;
    if (!isChoiceCountAtMost(formerCount, chosen, maxLossWork / formerCount))
        return std::nullopt;

    std::vector<std::size_t> keptBefore(restoredCount + 1);
    for (std::size_t index = 0; index <= restoredCount; ++index)
        keptBefore[index] = index;
    std::vector<ShapePart> target;
    restored.write(keptBefore, target);

    constexpr std::size_t unset = SIZE_MAX;
    constexpr std::size_t contested = SIZE_MAX - 1;
    std::vector<std::size_t> placed(restoredCount, unset);
    bool isLeftByAnyLoss = false;
    std::vector<std::size_t> choice(chosen);
    for (std::size_t slot = 0; slot < chosen; ++slot)
        choice[slot] = slot;
    std::vector<bool> isKept;
    std::vector<ShapePart> shape;
    keptBefore.assign(formerCount + 1, 0);
    do {
        isKept.assign(formerCount, !isChoiceKept);
        for (const std::size_t index : choice)
            isKept[index] = isChoiceKept;
        for (std::size_t index = 0; index < formerCount; ++index)
            keptBefore[index + 1] = keptBefore[index] + (isKept[index] ? 1 : 0);
        former.write(keptBefore, shape);
        if (shape != target)
            continue;
        isLeftByAnyLoss = true;
        std::size_t next = 0;
        for (std::size_t index = 0; index < formerCount && next < restoredCount; ++index) {
            if (!isKept[index])
                continue;
            placed[next] = placed[next] == unset || placed[next] == index ? index : contested;
            ++next;
        }
    } while (advanceChoice(choice, formerCount));
    if (!isLeftByAnyLoss)
        return std::nullopt;
    std::vector<std::pair<std::size_t, std::size_t>> pairs;
    for (std::size_t index = 0; index < restoredCount; ++index) {
        if (placed[index] != unset && placed[index] != contested)
            pairs.emplace_back(index, placed[index]);
    }
    return pairs;
}

} // namespace

std::vector<RestoredPane> pairRestoredPanes(const Workspace& former, std::span<const std::pair<std::uint32_t, std::uint32_t>> formerSessions,
    const Workspace& restored, std::span<const std::pair<std::uint32_t, std::uint32_t>> restoredSessions)
{
    std::vector<RestoredPane> pairs;
    // A host that could not start some of a record's panes lays out the
    // rest: a restoration with fewer panes pairs what every way of losing
    // them agrees on, and leaves the rest unpaired.
    const ArrangementShape formerShape(former);
    const ArrangementShape restoredShape(restored);
    if (const auto acrossLoss = pairAcrossLostPanes(formerShape, restoredShape)) {
        for (const auto& [restoredIndex, formerIndex] : *acrossLoss) {
            const PlacedPane& was = formerShape.panes()[formerIndex];
            const PlacedPane& now = restoredShape.panes()[restoredIndex];
            const std::uint32_t formerSessionID = sessionBoundTo(formerSessions, static_cast<int>(std::to_underlying(was.pane)));
            const std::uint32_t sessionID = sessionBoundTo(restoredSessions, static_cast<int>(std::to_underlying(now.pane)));
            if (!formerSessionID || !sessionID)
                continue;
            pairs.push_back({ .formerSurface = was.surface,
                .formerTab = was.tab,
                .formerPane = was.pane,
                .formerSessionID = formerSessionID,
                .surface = now.surface,
                .tab = now.tab,
                .pane = now.pane,
                .sessionID = sessionID });
        }
        return pairs;
    }
    if (former.surfaceCount() != restored.surfaceCount())
        return pairs;
    for (int surfaceIndex = 0; surfaceIndex < former.surfaceCount(); ++surfaceIndex) {
        const Surface* formerSurface = former.surface(former.surfaceIDAt(surfaceIndex));
        const Surface* restoredSurface = restored.surface(restored.surfaceIDAt(surfaceIndex));
        if (!formerSurface || !restoredSurface || formerSurface->tabCount != restoredSurface->tabCount)
            continue;
        for (int tabIndex = 0; tabIndex < formerSurface->tabCount && tabIndex < maxTabsPerSurface; ++tabIndex) {
            const Tab& formerTab = formerSurface->tabs[tabIndex];
            const Tab& restoredTab = restoredSurface->tabs[tabIndex];
            if (formerTab.panes.paneCount() != restoredTab.panes.paneCount()
                || !isSplitAlike(formerTab.panes, formerTab.panes.root(), restoredTab.panes, restoredTab.panes.root()))
                continue;
            std::vector<int> formerLeaves;
            std::vector<int> restoredLeaves;
            Ubiquitty::Panes::forEachLeaf(formerTab.panes, [&](int pane) { formerLeaves.push_back(pane); });
            Ubiquitty::Panes::forEachLeaf(restoredTab.panes, [&](int pane) { restoredLeaves.push_back(pane); });
            for (std::size_t leaf = 0; leaf < formerLeaves.size() && leaf < restoredLeaves.size(); ++leaf) {
                const std::uint32_t formerSessionID = sessionBoundTo(formerSessions, formerLeaves[leaf]);
                const std::uint32_t sessionID = sessionBoundTo(restoredSessions, restoredLeaves[leaf]);
                if (!formerSessionID || !sessionID)
                    continue;
                pairs.push_back({ .formerSurface = formerSurface->id,
                    .formerTab = formerTab.id,
                    .formerPane = PaneID { static_cast<std::uint32_t>(formerLeaves[leaf]) },
                    .formerSessionID = formerSessionID,
                    .surface = restoredSurface->id,
                    .tab = restoredTab.id,
                    .pane = PaneID { static_cast<std::uint32_t>(restoredLeaves[leaf]) },
                    .sessionID = sessionID });
            }
        }
    }
    return pairs;
}

Restoration followRestoration(const Restoration& unread, std::span<const RestoredPane> later)
{
    if (!unread.isRestored)
        return { .isRestored = true, .panes = { later.begin(), later.end() } };
    Restoration followed { .isRestored = true, .panes = {} };
    for (const RestoredPane& earlier : unread.panes) {
        const auto next = std::ranges::find(later, earlier.pane, &RestoredPane::formerPane);
        if (next == later.end())
            continue;
        RestoredPane through = *next;
        through.formerSurface = earlier.formerSurface;
        through.formerTab = earlier.formerTab;
        through.formerPane = earlier.formerPane;
        through.formerSessionID = earlier.formerSessionID;
        followed.panes.push_back(through);
    }
    return followed;
}

} // namespace Ubiquitty::Session::Layout
