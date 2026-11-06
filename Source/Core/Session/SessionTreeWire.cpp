// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SessionTreeWire.cpp — see SessionTreeWire.h.

#include "Core/Session/SessionTreeWire.h"

#include "Core/Protocol/Sync/Wire.h"
#include "Core/Terminal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace Ubiquitty::Session {

namespace {

using Protocol::Sync::ID128;
using Protocol::Sync::TopologyNode;
using Protocol::Sync::WireReader;
using Protocol::Sync::WireWriter;

// A split node's kind on the wire.
constexpr std::uint8_t nodeIsLeaf = 0;
constexpr std::uint8_t nodeIsSplit = 1;

// The flags. A bit no flag names is a malformed body.
constexpr std::uint8_t workspaceEndsWhenDetached = 1 << 0;
constexpr std::uint8_t windowHasZoomedPane = 1 << 0;

// The one nesting bound of a split tree.
constexpr std::size_t maxSplitDepth = Protocol::Sync::maxTopologyDepth;

// What a leaf's pane is, beside its place in the split.
struct LeafContext {
    // Whether the workspace is one nothing runs in, whose leaves carry what
    // its record holds of each pane.
    bool isRestorable = false; // bool-ok: names the one state it reports
};

void putText(WireWriter& writer, std::string_view text, std::size_t maximum)
{
    const std::string_view cut = boundedUTF8View(text, maximum);
    writer.bytes({ reinterpret_cast<const std::uint8_t*>(cut.data()), cut.size() });
}

std::string takeText(WireReader& reader, std::size_t maximum)
{
    const std::span<const std::uint8_t> bytes = reader.bytes(maximum);
    return { bytes.begin(), bytes.end() };
}

std::uint8_t takeFlags(WireReader& reader, std::uint8_t known)
{
    const std::uint8_t flags = reader.u8();
    if ((flags & ~known) != 0)
        reader.fail();
    return flags;
}

// Reads an index that must name one of `count` things.
std::size_t takeIndex(WireReader& reader, std::size_t count)
{
    const std::uint64_t index = reader.varint();
    if (index >= count) {
        reader.fail();
        return 0;
    }
    return static_cast<std::size_t>(index);
}

bool putTags(WireWriter& writer, const std::vector<std::string>& tags)
{
    if (tags.size() > Identity::maxTagsPerObject)
        return false;
    writer.varint(tags.size());
    for (const std::string& tag : tags)
        putText(writer, tag, Identity::maxLabelLength);
    return true;
}

std::vector<std::string> takeTags(WireReader& reader)
{
    std::vector<std::string> tags;
    const std::size_t count = reader.count(Identity::maxTagsPerObject);
    for (std::size_t index = 0; index < count && reader.ok(); ++index)
        tags.push_back(takeText(reader, Identity::maxLabelLength));
    return tags;
}

std::size_t leafCount(const TopologyNode& node)
{
    if (node.isLeaf())
        return 1;
    std::size_t leaves = 0;
    for (const TopologyNode& child : node.children)
        leaves += leafCount(child);
    return leaves;
}

void putGeometry(WireWriter& writer, const TopologyNode& node)
{
    writer.u16(node.rows);
    writer.u16(node.columns);
    writer.u16(node.offsetRows);
    writer.u16(node.offsetColumns);
}

void takeGeometry(WireReader& reader, TopologyNode& node)
{
    node.rows = reader.u16();
    node.columns = reader.u16();
    node.offsetRows = reader.u16();
    node.offsetColumns = reader.u16();
}

bool putLeaf(WireWriter& writer, const TopologyNode& node, const TreePane& pane, LeafContext context)
{
    if (pane.attendees.size() > maxTreeAttendeesPerPane)
        return false;
    writer.u8(nodeIsLeaf);
    writer.u64(node.paneID.low);
    writer.u64(node.paneID.high);
    writer.u32(std::to_underlying(pane.id));
    writer.u32(pane.sessionID);
    putGeometry(writer, node);
    writer.u64(node.structuralGeneration);
    putText(writer, pane.label, Identity::maxLabelLength);
    if (!putTags(writer, pane.tags))
        return false;
    putText(writer, pane.sessionLabel, Identity::maxLabelLength);
    if (!putTags(writer, pane.sessionTags))
        return false;
    writer.varint(pane.attendees.size());
    for (const TreeAttendee& attendee : pane.attendees) {
        writer.u32(attendee.clientID);
        writer.u8(std::to_underlying(attendee.kind));
    }
    if (context.isRestorable) {
        putText(writer, pane.facts.programTitle, maxHostLayoutTitleLength);
        putText(writer, pane.facts.currentDirectory, maxHostLayoutDirectoryLength);
        putText(writer, pane.facts.hostName, maxHostLayoutRemoteHostLength);
    }
    return true;
}

// Writes a split node and everything under it, its leaves taken in order
// from `panes` at `next`.
bool putSplit(WireWriter& writer, const TopologyNode& node, std::span<const TreePane> panes, std::size_t& next, std::size_t depth, LeafContext context)
{
    if (depth > maxSplitDepth)
        return false;
    if (node.isLeaf())
        return next < panes.size() && putLeaf(writer, node, panes[next++], context);
    if (node.children.size() < 2 || !std::isfinite(node.ratio) || node.ratio < 0.0 || node.ratio > 1.0)
        return false;
    writer.u8(nodeIsSplit);
    writer.u8(std::to_underlying(node.axis));
    std::uint64_t ratio = 0;
    std::memcpy(&ratio, &node.ratio, sizeof(ratio));
    writer.u64(ratio);
    putGeometry(writer, node);
    writer.varint(node.children.size());
    for (const TopologyNode& child : node.children) {
        if (!putSplit(writer, child, panes, next, depth + 1, context))
            return false;
    }
    return true;
}

// Reads a split node, its leaves appended to `panes` and counted against
// `leavesLeft`, which the tab's pane count bounds: a split cannot name more
// leaves than the tab has panes.
TopologyNode takeSplit(WireReader& reader, std::vector<TreePane>& panes, std::size_t depth, std::size_t& leavesLeft, LeafContext context)
{
    TopologyNode node;
    if (depth > maxSplitDepth) {
        reader.fail();
        return node;
    }
    const std::uint8_t kind = reader.u8();
    if (kind == nodeIsLeaf) {
        if (leavesLeft == 0) {
            reader.fail();
            return node;
        }
        --leavesLeft;
        TreePane pane;
        node.paneID.low = reader.u64();
        node.paneID.high = reader.u64();
        pane.id = Layout::PaneID { reader.u32() };
        pane.sessionID = reader.u32();
        takeGeometry(reader, node);
        node.structuralGeneration = reader.u64();
        pane.label = takeText(reader, Identity::maxLabelLength);
        pane.tags = takeTags(reader);
        pane.sessionLabel = takeText(reader, Identity::maxLabelLength);
        pane.sessionTags = takeTags(reader);
        // An attendee is a client id and a kind: five bytes.
        const std::size_t attendees = reader.count(maxTreeAttendeesPerPane, 5);
        for (std::size_t index = 0; index < attendees && reader.ok(); ++index) {
            TreeAttendee attendee;
            attendee.clientID = reader.u32();
            const std::uint8_t attendeeKind = reader.u8();
            if (attendeeKind > std::to_underlying(TreeAttendeeKind::Agent)) {
                reader.fail();
                break;
            }
            attendee.kind = static_cast<TreeAttendeeKind>(attendeeKind);
            pane.attendees.push_back(attendee);
        }
        pane.facts.rows = node.rows;
        pane.facts.columns = node.columns;
        if (context.isRestorable) {
            pane.facts.programTitle = takeText(reader, maxHostLayoutTitleLength);
            pane.facts.currentDirectory = takeText(reader, maxHostLayoutDirectoryLength);
            pane.facts.hostName = takeText(reader, maxHostLayoutRemoteHostLength);
        }
        panes.push_back(std::move(pane));
        return node;
    }
    if (kind != nodeIsSplit) {
        reader.fail();
        return node;
    }
    const std::uint8_t axis = reader.u8();
    const std::uint64_t ratio = reader.u64();
    std::memcpy(&node.ratio, &ratio, sizeof(ratio));
    takeGeometry(reader, node);
    // Every child is a leaf's worth of bytes at least, and two at least make
    // a split.
    const std::size_t children = reader.count(leavesLeft, 1);
    if (!reader.ok() || axis > std::to_underlying(Protocol::Sync::SplitAxis::Vertical) || !std::isfinite(node.ratio) || node.ratio < 0.0 || node.ratio > 1.0 || children < 2) {
        reader.fail();
        return node;
    }
    node.axis = static_cast<Protocol::Sync::SplitAxis>(axis);
    for (std::size_t index = 0; index < children && reader.ok(); ++index)
        node.children.push_back(takeSplit(reader, panes, depth + 1, leavesLeft, context));
    return node;
}

bool putTab(WireWriter& writer, const TreeTab& tab, LeafContext context)
{
    if (tab.panes.empty() || tab.panes.size() > static_cast<std::size_t>(Layout::maxPanesPerTab) || leafCount(tab.split) != tab.panes.size())
        return false;
    if (tab.hostFocusedPane >= tab.panes.size())
        return false;
    writer.u32(std::to_underlying(tab.id));
    putText(writer, tab.label, Identity::maxLabelLength);
    if (!putTags(writer, tab.tags))
        return false;
    putText(writer, tab.name, maxTreeNameBytes);
    writer.varint(tab.panes.size());
    writer.varint(tab.hostFocusedPane);
    std::size_t next = 0;
    return putSplit(writer, tab.split, tab.panes, next, 0, context) && next == tab.panes.size();
}

TreeTab takeTab(WireReader& reader, std::size_t& leavesLeftInWorkspace, LeafContext context)
{
    TreeTab tab;
    tab.id = Layout::TabID { reader.u32() };
    tab.label = takeText(reader, Identity::maxLabelLength);
    tab.tags = takeTags(reader);
    tab.name = takeText(reader, maxTreeNameBytes);
    // A tab with no pane has no pane to type into: the index below names
    // one of the panes or the body is refused.
    const std::size_t panes = reader.count(std::min(static_cast<std::size_t>(Layout::maxPanesPerTab), leavesLeftInWorkspace));
    tab.hostFocusedPane = takeIndex(reader, panes);
    if (!reader.ok())
        return tab;
    leavesLeftInWorkspace -= panes;
    std::size_t leavesLeft = panes;
    tab.split = takeSplit(reader, tab.panes, 0, leavesLeft, context);
    // The split's leaves are the tab's panes, no fewer.
    if (!reader.ok() || leavesLeft != 0)
        reader.fail();
    return tab;
}

bool putWindow(WireWriter& writer, const TreeWindow& window, LeafContext context)
{
    if (window.tabs.empty() || window.tabs.size() > static_cast<std::size_t>(Layout::maxTabsPerSurface) || window.hostActiveTab >= window.tabs.size())
        return false;
    if (window.hostZoomedPane && (window.hostZoomedPane->tab >= window.tabs.size() || window.hostZoomedPane->pane >= window.tabs[window.hostZoomedPane->tab].panes.size()))
        return false;
    writer.u32(std::to_underlying(window.id));
    putText(writer, window.label, Identity::maxLabelLength);
    if (!putTags(writer, window.tags))
        return false;
    writer.u8(window.hostZoomedPane ? windowHasZoomedPane : 0);
    writer.varint(window.tabs.size());
    writer.varint(window.hostActiveTab);
    if (window.hostZoomedPane) {
        writer.varint(window.hostZoomedPane->tab);
        writer.varint(window.hostZoomedPane->pane);
    }
    for (const TreeTab& tab : window.tabs) {
        if (!putTab(writer, tab, context))
            return false;
    }
    return true;
}

TreeWindow takeWindow(WireReader& reader, std::size_t& leavesLeftInWorkspace, LeafContext context)
{
    TreeWindow window;
    window.id = Layout::SurfaceID { reader.u32() };
    window.label = takeText(reader, Identity::maxLabelLength);
    window.tags = takeTags(reader);
    const std::uint8_t flags = takeFlags(reader, windowHasZoomedPane);
    // A window with no tab has no tab to show: the index below names one of
    // the tabs or the body is refused.
    const std::size_t tabs = reader.count(static_cast<std::size_t>(Layout::maxTabsPerSurface));
    window.hostActiveTab = takeIndex(reader, tabs);
    std::optional<TreeZoom> zoom;
    if ((flags & windowHasZoomedPane) != 0) {
        zoom = TreeZoom {};
        zoom->tab = takeIndex(reader, tabs);
        zoom->pane = static_cast<std::size_t>(reader.varint(static_cast<std::uint64_t>(Layout::maxPanesPerTab)));
    }
    for (std::size_t index = 0; index < tabs && reader.ok(); ++index) {
        window.tabs.push_back(takeTab(reader, leavesLeftInWorkspace, context));
        window.tabs.back().isHostActive = index == window.hostActiveTab;
    }
    // The zoomed pane is one of the panes of the tab it names.
    if (reader.ok() && zoom && zoom->pane >= window.tabs[zoom->tab].panes.size())
        reader.fail();
    window.hostZoomedPane = zoom;
    return window;
}

} // namespace

bool encodeWorkspaceStructure(std::vector<std::uint8_t>& output, const TreeWorkspace& workspace)
{
    if (workspace.name.size() > maxWorkspaceNameBytes || workspace.windows.size() > static_cast<std::size_t>(Layout::maxSurfaces))
        return false;
    // A workspace with no window has no window in front.
    if (workspace.windows.empty() ? workspace.hostActiveWindow != 0 : workspace.hostActiveWindow >= workspace.windows.size())
        return false;
    std::size_t leaves = 0;
    for (const TreeWindow& window : workspace.windows) {
        for (const TreeTab& tab : window.tabs)
            leaves += tab.panes.size();
    }
    if (leaves > maxStructureLeaves)
        return false;
    const LeafContext context { .isRestorable = workspace.isRestorable() };
    std::vector<std::uint8_t> body;
    WireWriter writer(body);
    writer.u64(workspace.revision);
    writer.u8(std::to_underlying(workspace.state));
    writer.u8(workspace.endsWhenDetached ? workspaceEndsWhenDetached : 0);
    writer.u32(workspace.id);
    putText(writer, workspace.name, Protocol::Sync::maxWorkspaceNameLength);
    putText(writer, workspace.label, Identity::maxLabelLength);
    if (!putTags(writer, workspace.tags))
        return false;
    writer.varint(workspace.windows.size());
    writer.varint(workspace.hostActiveWindow);
    for (const TreeWindow& window : workspace.windows) {
        if (!putWindow(writer, window, context))
            return false;
    }
    if (body.size() > maxWorkspaceStructureBytes)
        return false;
    output.insert(output.end(), body.begin(), body.end());
    return true;
}

std::optional<TreeWorkspace> decodeWorkspaceStructure(std::span<const std::uint8_t> body)
{
    if (body.size() > maxWorkspaceStructureBytes)
        return std::nullopt;
    WireReader reader(body);
    TreeWorkspace workspace;
    workspace.revision = reader.u64();
    const std::uint8_t state = reader.u8();
    if (state > std::to_underlying(TreeWorkspaceState::Restorable))
        return std::nullopt;
    workspace.state = static_cast<TreeWorkspaceState>(state);
    workspace.endsWhenDetached = (takeFlags(reader, workspaceEndsWhenDetached) & workspaceEndsWhenDetached) != 0;
    workspace.id = reader.u32();
    workspace.name = takeText(reader, Protocol::Sync::maxWorkspaceNameLength);
    workspace.label = takeText(reader, Identity::maxLabelLength);
    workspace.tags = takeTags(reader);
    const std::size_t windows = reader.count(static_cast<std::size_t>(Layout::maxSurfaces));
    const std::uint64_t active = reader.varint();
    if (!reader.ok() || (windows == 0 ? active != 0 : active >= windows))
        return std::nullopt;
    workspace.hostActiveWindow = static_cast<std::size_t>(active);
    const LeafContext context { .isRestorable = workspace.isRestorable() };
    std::size_t leavesLeft = maxStructureLeaves;
    for (std::size_t index = 0; index < windows && reader.ok(); ++index) {
        workspace.windows.push_back(takeWindow(reader, leavesLeft, context));
        workspace.windows.back().isHostActive = index == workspace.hostActiveWindow;
    }
    if (!reader.ok() || !reader.isAtEnd())
        return std::nullopt;
    return workspace;
}

} // namespace Ubiquitty::Session
