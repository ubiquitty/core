// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SessionTree.h — everything a host holds, as one tree a listing reads.
//
// A host's workspaces, each one's windows, each window's tabs, each tab's
// split tree and each split's panes, with what every level shows: its
// identity and label, its status rolled up from the panes under it, when
// anything under it last wrote or was typed into, and for a pane the inputs
// its title is composed from. A switcher, a sidebar, a remote session list
// and an agent's `ls` all read this one value.
//
// Pure: `buildSessionTree` reads `Layout::Workspace` models, a label book and
// a callback that answers for one pane, and returns a value. Nothing here
// knows a daemon. A host sends a client each workspace's structure
// (SessionTreeWire.h) and each pane's two records, and the client assembles
// the tree from them (`assembleSessionTree`), so a host and a client hold the
// same value: `deriveSessionTree` is the one place a level's status,
// activity, attendance and age are computed.
//
// A pane is described by the two records its host sends for it wherever it
// is bound (`PaneFacts`, `StatusRecords`; Protocol/Sync/Records.h). The
// title inputs, the status and the activity a listing reads off a pane are
// read from those, so a pane listed and a pane attached to are one set of
// facts.
//
// A window is the model's surface (`Layout::Surface`): its id is a
// `Layout::SurfaceID` and its identity kind is `Identity::ObjectKind::Window`.
// The tree says window, as the identity kinds, the layout record and every
// listing do; "surface" stays the layout model's word for the container.
//
// A tab's split is the Sync topology node (`Protocol::Sync::TopologyNode`):
// one representation of a split between the host's model and the wire. Its
// leaves, first child first, are the tab's `panes` in order; a leaf names the
// pane as the screen plane does and carries the generation of the frame that
// gave it its size.
//
// One thing here is provisional by design. A workspace's id comes from the host's session id space: a workspace takes
// the id of the session it was made around, and a restorable one takes the
// next unused session id. `workspace:5` and `session:5` are therefore both
// valid identities and usually the root session and its workspace, though
// neither implies the other once that session has ended. A person is shown a
// workspace by its name.
//
// Which window is in front, which tab a window shows, which pane a tab types
// into and which pane is zoomed are the host's values: the one answer its
// layout model holds, shared by everyone attached, and what a client that
// attaches adopts. They are named `host…` so that no client reads them as
// what it is itself looking at; an attachment's own view is not in this tree.
//
// Names and labels are kept apart at every level. A name or title is what a
// listing displays: a workspace's `name`, a tab's `name`, and for a window and
// a pane, which have none, the title composed for them. A label is the handle
// a target addresses the thing by, drawn beside the title as a chip and never
// instead of it, except that a label on a window or a pane is its title, as it
// is in a window's row and a pane's header. `App::listedName` is that rule.
//

#include "Core/App/ProgramActivity.h"
#include "Core/App/StatusRollup.h"
#include "Core/App/TitleCompose.h"
#include "Core/Panes/PaneTree.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/Topology.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Session/HostInstanceInfo.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/PaneActivity.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Ubiquitty::Session {

/// Who an attachment to a pane is.
enum class TreeAttendeeKind : std::uint8_t {
    Person,
    Agent,
};

/// One attachment to a pane.
struct TreeAttendee {
    /// The attachment's client identity on the host; what tells one attendee
    /// of two panes from two attendees.
    std::uint32_t clientID = 0;
    TreeAttendeeKind kind = TreeAttendeeKind::Person;

    friend bool operator==(const TreeAttendee&, const TreeAttendee&) = default;
};

/// How many people and agents are attached at or under a node, each counted
/// once however many panes under the node they are attached to.
struct TreeAttendance {
    std::uint32_t people = 0;
    std::uint32_t agents = 0;

    friend bool operator==(const TreeAttendance&, const TreeAttendance&) = default;

    /// Everyone attached.
    /// - Returns: People and agents together.
    [[nodiscard]] constexpr std::uint32_t total() const { return people + agents; }
};

/// What a host answers about one pane when a tree is built.
struct TreePaneFacts {
    /// The session drawing into the pane; zero when none does.
    std::uint32_t sessionID = 0;
    /// The pane as the screen plane names it, and the generation of its
    /// latest Structural frame: what its leaf of the split carries. Zero for
    /// a pane that publishes no screen.
    Protocol::Sync::ID128 paneID;
    std::uint64_t structuralGeneration = 0;
    /// What the host says about the pane beside its picture.
    Protocol::Sync::PaneFacts facts;
    /// The status its programs posted.
    Protocol::Sync::StatusRecords records;
    /// Everyone attached to the pane. One listed twice is counted once.
    std::vector<TreeAttendee> attendees;
};

/// One pane: a leaf of a tab's split tree.
struct TreePane {
    Layout::PaneID id = Layout::invalidPane;
    /// The session drawing into the pane; zero when none does.
    std::uint32_t sessionID = 0;
    /// The pane's label, empty when it has none.
    std::string label;
    /// The tags a person put on it, in the order they are drawn.
    std::vector<std::string> tags;
    /// The label of the session behind it, empty when it has none.
    std::string sessionLabel;
    /// The tags a person put on the session behind it.
    std::vector<std::string> sessionTags;
    /// What the pane's host says about it beside its picture, and the status
    /// its programs posted: the two records a bound pane is sent, whole. What
    /// follows down to `attendees` is read from these by `deriveSessionTree`.
    Protocol::Sync::PaneFacts facts;
    Protocol::Sync::StatusRecords records;
    /// The host's half of what `App::composeTitle` composes the pane's title
    /// from, for whichever surface a client draws it on; the reader supplies
    /// its own `App::TitleViewerFacts`. The grid and the directory a listing
    /// shows are the ones in here.
    App::PaneTitleFacts title;
    /// The name of the program holding the terminal, empty when unknown.
    std::string program;
    /// The badge the program set, empty when none.
    std::string badge;
    /// The pane's status as a node of the roll-up, with its tab colour, its
    /// badge count and what nobody has seen yet.
    App::RolledStatus status;
    /// When the pane last wrote and was last typed into.
    PaneActivity activity;
    /// Whether the pane is the one its tab types into, in the host's layout.
    bool isHostFocused = false; // bool-ok: names the one state it reports
    /// Whether the pane fills its window in the host's layout, while the
    /// split stays as it was.
    bool isHostZoomed = false; // bool-ok: names the one state it reports
    /// When the pane's session was created, in seconds since the Unix epoch
    /// on the host's wall clock; zero when unknown.
    std::uint64_t createdUnixSeconds = 0;
    /// Whether the host adopted the session's terminal from another process
    /// rather than starting its program itself.
    bool isAdopted = false; // bool-ok: names the one state it reports
    /// Everyone attached to the pane, each once, in the order the host gave.
    std::vector<TreeAttendee> attendees;
    /// How many of them are people and how many agents.
    TreeAttendance attendance;

    /// Reports whether more than one attachment has the pane.
    /// - Returns: Whether it is shared.
    [[nodiscard]] bool isShared() const { return attendance.total() > 1; }

    /// The pane's grid width.
    /// - Returns: Columns, zero when the host reported none.
    [[nodiscard]] int columns() const { return title.columns; }

    /// The pane's grid height.
    /// - Returns: Rows, zero when the host reported none.
    [[nodiscard]] int rows() const { return title.rows; }

    /// The directory the pane's program reported or runs in.
    /// - Returns: The directory, empty when unknown.
    [[nodiscard]] const std::string& directory() const { return title.currentDirectory; }
};

/// One tab: a split tree of panes.
struct TreeTab {
    Layout::TabID id = Layout::invalidTab;
    /// The tab's label, empty when it has none.
    std::string label;
    /// The tags a person put on it, in the order they are drawn.
    std::vector<std::string> tags;
    /// The name a person gave the tab, empty when they gave none.
    std::string name;
    /// How the tab is split. A leaf names its pane as the screen plane does
    /// and carries its grid and the generation of the frame that gave it that
    /// grid; a pane nothing runs in has no such name and a leaf of zeros.
    Protocol::Sync::TopologyNode split;
    /// The panes, in the order a walk of the split meets its leaves, first
    /// child first; never empty.
    std::vector<TreePane> panes;
    /// The index in `panes` of the pane the tab types into, in the host's
    /// layout.
    std::size_t hostFocusedPane = 0;
    /// Whether this is the tab its window shows, in the host's layout.
    bool isHostActive = false; // bool-ok: names the one state it reports
    /// The panes' statuses rolled up.
    App::RolledStatus status;
    /// The panes' newest activity.
    PaneActivity activity;
    /// Everyone attached to a pane under it, each counted once.
    TreeAttendance attendance;
    /// When the oldest pane under it was created; zero when none is known.
    std::uint64_t createdUnixSeconds = 0;

    /// The pane whose title inputs compose the tab's title: the one it types
    /// into.
    /// - Returns: The focused pane.
    [[nodiscard]] const TreePane& titlePane() const { return panes[hostFocusedPane]; }
};

/// The pane that fills a window: which of the window's tabs, and which of
/// that tab's panes.
struct TreeZoom {
    std::size_t tab = 0;
    std::size_t pane = 0;

    friend bool operator==(const TreeZoom&, const TreeZoom&) = default;
};

/// One window: an ordered run of tabs. The layout model's surface.
struct TreeWindow {
    Layout::SurfaceID id = Layout::invalidSurface;
    /// The window's label, empty when it has none.
    std::string label;
    /// The tags a person put on it, in the order they are drawn.
    std::vector<std::string> tags;
    /// The tabs, in tab order; never empty.
    std::vector<TreeTab> tabs;
    /// The index in `tabs` of the tab the window shows, in the host's layout.
    std::size_t hostActiveTab = 0;
    /// The pane that fills the window in the host's layout, when one does.
    /// A window has one zoom, as the layout model's surface does; a pane
    /// reads whether it is the one from `TreePane::isHostZoomed`.
    std::optional<TreeZoom> hostZoomedPane;
    /// Whether this is the window its workspace has in front, in the host's
    /// layout.
    bool isHostActive = false; // bool-ok: names the one state it reports
    /// The tabs' statuses rolled up.
    App::RolledStatus status;
    /// The tabs' newest activity.
    PaneActivity activity;
    /// Everyone attached to a pane under it, each counted once.
    TreeAttendance attendance;
    /// When the oldest pane under it was created; zero when none is known.
    std::uint64_t createdUnixSeconds = 0;

    /// The window's one tab, when it has exactly one.
    ///
    /// A listing draws such a window as that tab, on one row, and gives it no
    /// tab rows of its own.
    ///
    /// - Returns: The tab, or null for a window of several.
    [[nodiscard]] const TreeTab* soleTab() const { return tabs.size() == 1 ? &tabs.front() : nullptr; }

    /// Reports whether a listing draws rows for the window's tabs.
    /// - Returns: Whether it has more than one.
    [[nodiscard]] bool hasTabRows() const { return tabs.size() > 1; }
};

/// Whether anything runs in a workspace.
enum class TreeWorkspaceState : std::uint8_t {
    /// Its panes run.
    Live,
    /// Its panes run, in fresh shells: the host recreated it from its record
    /// after restarting, and what was on its screens before is gone.
    RestoredAfterHostRestart,
    /// Nothing runs in it. The host holds its record, and attaching to it
    /// recreates it.
    Restorable,
};

/// One workspace: what a person attaches to as one thing.
struct TreeWorkspace {
    /// Whether anything runs in it. A restorable workspace lists the
    /// windows, tabs and panes its record holds, with their recorded labels,
    /// titles, directories and grids. Those have no identity, no status and
    /// no activity: only the workspace itself can be addressed.
    TreeWorkspaceState state = TreeWorkspaceState::Live;
    /// Whether it ends when the last person on it detaches, instead of
    /// outliving its clients.
    bool endsWhenDetached = false; // bool-ok: names the one state it reports

    /// Reports whether attaching to the workspace recreates it.
    /// - Returns: Whether nothing runs in it.
    [[nodiscard]] bool isRestorable() const { return state == TreeWorkspaceState::Restorable; }

    /// The id it is listed and attached by. From the host's session id space,
    /// so a session may have the same number.
    std::uint32_t id = 0;
    /// The revision of the workspace's structure as its host last described
    /// it: moved by every change to what a structure record says, and by
    /// nothing a pane's own records say.
    std::uint64_t revision = 0;
    /// The name a HELLO reaches it by, empty when unnamed.
    std::string name;
    /// The workspace's label, empty when it has none.
    std::string label;
    /// The tags a person put on it, in the order they are drawn.
    std::vector<std::string> tags;
    /// The windows, in the order they were opened.
    std::vector<TreeWindow> windows;
    /// The index in `windows` of the window in front, in the host's layout.
    std::size_t hostActiveWindow = 0;
    /// The windows' statuses rolled up.
    App::RolledStatus status;
    /// The windows' newest activity.
    PaneActivity activity;
    /// Everyone attached to a pane under it, each counted once.
    TreeAttendance attendance;
    /// When the oldest pane under it was created; zero when none is known.
    std::uint64_t createdUnixSeconds = 0;

    /// What the workspace is called when nobody named it.
    ///
    /// A word a person put on what it has in front comes first: its window's
    /// label, else its tab's name, else the label of that tab's one pane.
    /// With none, the last component of the directory its pane in front is
    /// in ("api" for `/srv/api`, "~" at home), the way a tab names its
    /// folder, rather than a composed title that can lead with the host and
    /// the whole path.
    ///
    /// One rule for every place that names a workspace, a listing's row and a
    /// question about ending it alike, so a workspace is not called one thing
    /// in one place and another elsewhere.
    ///
    /// - Returns: The name; empty when nothing in front is named and that
    ///   pane reports no directory.
    [[nodiscard]] std::string defaultName() const
    {
        if (windows.empty())
            return {};
        const TreeWindow& window = windows[hostActiveWindow < windows.size() ? hostActiveWindow : 0];
        if (!window.label.empty())
            return window.label;
        if (window.tabs.empty())
            return {};
        const TreeTab& tab = window.tabs[window.hostActiveTab < window.tabs.size() ? window.hostActiveTab : 0];
        if (!tab.name.empty())
            return tab.name;
        if (tab.panes.empty())
            return {};
        if (tab.panes.size() == 1 && !tab.panes.front().label.empty())
            return tab.panes.front().label;
        const App::PaneTitleFacts& facts = tab.panes[tab.hostFocusedPane < tab.panes.size() ? tab.hostFocusedPane : 0].title;
        return Title::titleDirectorySegment(facts.currentDirectory, facts.homeDirectory, Title::PathFormat::BasenameOnly);
    }
};

/// Where a pane is in a tree: an index at every level.
struct TreePanePath {
    std::size_t workspace = 0;
    std::size_t window = 0;
    std::size_t tab = 0;
    std::size_t pane = 0;

    friend bool operator==(const TreePanePath&, const TreePanePath&) = default;
};

/// Everything a host holds.
struct SessionTree {
    /// Other live daemon endpoints on this machine. Their object IDs remain separate.
    std::vector<HostInstanceInfo> hostInstances;
    /// Which state of the host this is. The host counts it; two trees of one
    /// revision are the same tree.
    std::uint64_t revision = 0;
    /// The host's wall clock when it described this state, in milliseconds
    /// since the Unix epoch. Every instant in the tree is the host's, so how
    /// long ago one was is measured against this and never against a
    /// reader's clock, which may disagree with the host's by any amount.
    std::uint64_t hostNowUnixMilliseconds = 0;
    /// The workspaces, ascending by id, which is the order they were made in.
    std::vector<TreeWorkspace> workspaces;
    /// The workspaces' statuses rolled up: the whole host as one mark.
    App::RolledStatus status;
    /// The newest activity anywhere.
    PaneActivity activity;
    /// Everyone attached to a pane under it, each counted once.
    TreeAttendance attendance;
    /// When the oldest pane under it was created; zero when none is known.
    std::uint64_t createdUnixSeconds = 0;

    /// Finds a pane.
    ///
    /// - Parameter pane: The pane's id, unique across a host's workspaces.
    /// - Returns: Its place, or nothing when no tab holds it.
    [[nodiscard]] std::optional<TreePanePath> locate(Layout::PaneID pane) const
    {
        // A restorable workspace's panes have no identity to be found by.
        if (pane == Layout::invalidPane)
            return std::nullopt;
        for (std::size_t workspace = 0; workspace < workspaces.size(); ++workspace) {
            const auto& windows = workspaces[workspace].windows;
            for (std::size_t window = 0; window < windows.size(); ++window) {
                const auto& tabs = windows[window].tabs;
                for (std::size_t tab = 0; tab < tabs.size(); ++tab) {
                    const auto& panes = tabs[tab].panes;
                    for (std::size_t index = 0; index < panes.size(); ++index) {
                        if (panes[index].id == pane)
                            return TreePanePath { workspace, window, tab, index };
                    }
                }
            }
        }
        return std::nullopt;
    }

    /// The pane at a place `locate` returned for this tree.
    ///
    /// - Parameter path: The place.
    /// - Returns: The pane.
    [[nodiscard]] const TreePane& pane(const TreePanePath& path) const { return workspaces[path.workspace].windows[path.window].tabs[path.tab].panes[path.pane]; }
};

/// One workspace a tree is built from.
struct TreeWorkspaceSource {
    /// The id it is listed and attached by.
    std::uint32_t id = 0;
    /// The name a HELLO reaches it by, empty when unnamed.
    std::string_view name;
    /// Its windows, tabs and panes; null contributes a workspace of none.
    const Layout::Workspace* layout = nullptr;
    /// Whether it ends when the last person on it detaches.
    bool endsWhenDetached = false; // bool-ok: names the one state it reports
    /// Whether the host recreated it from its record after restarting.
    bool wasRestoredAfterHostRestart = false; // bool-ok: names the one state it reports
};

/// One workspace nothing runs in, which the host could restore.
struct TreeRestorableSource {
    /// The id it is listed and attached by.
    std::uint32_t id = 0;
    /// What the host recorded of it; null contributes nothing.
    const HostLayoutRecord* record = nullptr;
};

/// What a tree is built from, besides the labels and the panes' facts.
struct TreeHost {
    /// The host's count of the state being described.
    std::uint64_t revision = 0;
    /// The host's wall clock, in milliseconds since the Unix epoch.
    std::uint64_t nowUnixMilliseconds = 0;
    /// The host's live workspaces.
    std::span<const TreeWorkspaceSource> workspaces {};
    /// The tags people put on the host's objects; null for none.
    const Identity::TagBook* tags = nullptr;
    /// The workspaces the host could restore from its records.
    std::span<const TreeRestorableSource> restorable {};
};

namespace SessionTreeDetail {

/// The tags a person put on one object.
///
/// - Parameters:
///   - book: The host's tag book, or null.
///   - id: The object.
/// - Returns: Its tags in order; none without a book.
[[nodiscard]] inline std::vector<std::string> tagsOf(const Identity::TagBook* book, Identity::ObjectID id)
{
    if (!book)
        return {};
    const std::span<const std::string> held = book->tags(id);
    return { held.begin(), held.end() };
}

/// Appends the attendees of `more` that `attendees` does not hold yet.
///
/// - Parameters:
///   - attendees: The attendees gathered so far, each client once.
///   - more: Attendees to add; one already present by client is skipped.
inline void appendDistinct(std::vector<TreeAttendee>& attendees, std::span<const TreeAttendee> more)
{
    for (const TreeAttendee& attendee : more) {
        if (std::ranges::find(attendees, attendee.clientID, &TreeAttendee::clientID) == attendees.end())
            attendees.push_back(attendee);
    }
}

/// Counts distinct attendees by kind.
///
/// - Parameter attendees: Attendees, each client once.
/// - Returns: The people and the agents among them.
[[nodiscard]] inline TreeAttendance attendanceOf(std::span<const TreeAttendee> attendees)
{
    TreeAttendance attendance;
    for (const TreeAttendee& attendee : attendees) {
        if (attendee.kind == TreeAttendeeKind::Agent)
            ++attendance.agents;
        else
            ++attendance.people;
    }
    return attendance;
}

/// The earlier of two creation times, where zero is a time nobody recorded.
///
/// - Parameters:
///   - a: One time.
///   - b: The other.
/// - Returns: The earlier known one; zero when neither is known.
[[nodiscard]] constexpr std::uint64_t earlierCreation(std::uint64_t a, std::uint64_t b)
{
    if (a == 0 || b == 0)
        return std::max(a, b);
    return std::min(a, b);
}

/// A cell count as a node carries it.
///
/// - Parameter cells: The count.
/// - Returns: The count, held to what sixteen bits say.
[[nodiscard]] constexpr std::uint16_t nodeCells(int cells)
{
    return static_cast<std::uint16_t>(std::clamp(cells, 0, 65'535));
}

/// Sizes a leaf in cells by the host layout's rule for a pane's grid.
///
/// - Parameters:
///   - leaf: The leaf.
///   - rows: The pane's rows as its owner reports them.
///   - columns: The pane's columns.
inline void sizeLeaf(Protocol::Sync::TopologyNode& leaf, int rows, int columns)
{
    const Protocol::Tmux::PaneLayoutExtent extent = Protocol::Tmux::leafLayoutExtent({ .columns = columns, .rows = rows });
    leaf.rows = nodeCells(extent.rows);
    leaf.columns = nodeCells(extent.columns);
}

/// Sizes a split in cells from its children, already sized, by the host
/// layout's one rule: the sum along the split's axis and the larger across.
/// A split of more than two children folds them in order.
///
/// - Parameter split: The split.
inline void sizeSplit(Protocol::Sync::TopologyNode& split)
{
    Protocol::Tmux::PaneLayoutExtent extent { .columns = 0, .rows = 0 };
    bool isFirst = true; // bool-ok: marks the fold's seed
    for (const Protocol::Sync::TopologyNode& child : split.children) {
        const Protocol::Tmux::PaneLayoutExtent childExtent { .columns = child.columns, .rows = child.rows };
        extent = isFirst ? childExtent : Protocol::Tmux::splitLayoutExtent(split.axis == Protocol::Sync::SplitAxis::Horizontal, extent, childExtent);
        isFirst = false;
    }
    split.rows = nodeCells(extent.rows);
    split.columns = nodeCells(extent.columns);
}

/// Places a sized node and everything under it: a split's first child at
/// the split's own corner, and each child after it past the ones before,
/// along the split's axis.
///
/// - Parameters:
///   - node: The node, sized.
///   - offsetRows: Where its rectangle starts, in rows from the tab's top.
///   - offsetColumns: Where it starts, in columns from the tab's left.
inline void placeNode(Protocol::Sync::TopologyNode& node, int offsetRows, int offsetColumns)
{
    node.offsetRows = nodeCells(offsetRows);
    node.offsetColumns = nodeCells(offsetColumns);
    for (Protocol::Sync::TopologyNode& child : node.children) {
        placeNode(child, offsetRows, offsetColumns);
        if (node.axis == Protocol::Sync::SplitAxis::Horizontal)
            offsetColumns += child.columns;
        else
            offsetRows += child.rows;
    }
}

/// Appends the subtree at `nodeIndex` of a tab's split, parent first, each
/// node sized in cells.
///
/// - Parameters:
///   - split: The tab's split tree in the layout model.
///   - nodeIndex: The node of `split` to append, with everything under it.
///   - workspaceID: The workspace holding the tab, as panes are asked about.
///   - labels: The host's label book.
///   - tags: The host's tag book, or null.
///   - factsForPane: Answers for one pane.
///   - tab: The tab whose `panes` grow by the leaves under the node.
/// - Returns: The node, as the tab's split holds it.
template<typename FactsForPane>
Protocol::Sync::TopologyNode appendNode(const Panes::Tree& split, int nodeIndex, std::uint32_t workspaceID, const Identity::LabelBook& labels, const Identity::TagBook* tags, FactsForPane& factsForPane, TreeTab& tab)
{
    Protocol::Sync::TopologyNode node;
    if (split.isLeaf(nodeIndex)) {
        const Layout::PaneID id { static_cast<std::uint32_t>(split.node(nodeIndex).paneID) };
        TreePaneFacts facts = factsForPane(workspaceID, id);
        TreePane pane;
        pane.id = id;
        pane.sessionID = facts.sessionID;
        pane.label = labels.label({ Identity::ObjectKind::Pane, std::to_underlying(id) });
        pane.tags = tagsOf(tags, { Identity::ObjectKind::Pane, std::to_underlying(id) });
        if (facts.sessionID) {
            pane.sessionLabel = labels.label({ Identity::ObjectKind::Session, facts.sessionID });
            pane.sessionTags = tagsOf(tags, { Identity::ObjectKind::Session, facts.sessionID });
        }
        pane.facts = Protocol::Sync::boundedForWire(std::move(facts.facts));
        pane.records = Protocol::Sync::boundedForWire(std::move(facts.records));
        appendDistinct(pane.attendees, facts.attendees);
        node.paneID = facts.paneID;
        sizeLeaf(node, pane.facts.rows, pane.facts.columns);
        node.structuralGeneration = facts.structuralGeneration;
        tab.panes.push_back(std::move(pane));
        return node;
    }
    const Panes::Node source = split.node(nodeIndex);
    // The pane tree's Vertical is a vertical divider, its children side by
    // side; the topology's Horizontal is the axis they are laid along.
    node.axis = source.orientation == Panes::Orientation::Vertical ? Protocol::Sync::SplitAxis::Horizontal : Protocol::Sync::SplitAxis::Vertical;
    node.ratio = source.ratio;
    node.children.push_back(appendNode(split, source.first, workspaceID, labels, tags, factsForPane, tab));
    node.children.push_back(appendNode(split, source.second, workspaceID, labels, tags, factsForPane, tab));
    sizeSplit(node);
    return node;
}

/// Rolls a level's children up into the level.
///
/// - Parameters:
///   - parent: The level whose `status`, `activity`, `attendance` and
///     `createdUnixSeconds` are set.
///   - children: The level under it, each already rolled up.
///   - attendees: Everyone attached under the level, each client once; what
///     the count is taken from, since a sum of the children's counts would
///     count someone attached under two of them twice.
template<typename Parent, typename Child>
void rollUp(Parent& parent, const std::vector<Child>& children, std::span<const TreeAttendee> attendees)
{
    std::vector<App::RolledStatus> statuses;
    std::vector<PaneActivity> activities;
    statuses.reserve(children.size());
    activities.reserve(children.size());
    for (const Child& child : children) {
        statuses.push_back(child.status);
        activities.push_back(child.activity);
    }
    parent.status = App::rollUpStatus(statuses);
    parent.activity = newestActivity(activities);
    parent.attendance = attendanceOf(attendees);
    parent.createdUnixSeconds = 0;
    for (const Child& child : children)
        parent.createdUnixSeconds = earlierCreation(parent.createdUnixSeconds, child.createdUnixSeconds);
}

/// Appends the subtree at `node` of a recorded tab's split, parent first.
///
/// - Parameters:
///   - node: The recorded node.
///   - facts: The record's per-pane facts, in leaf order.
///   - leafOrdinal: How many leaves of the record came before this subtree;
///     advanced past the ones in it.
///   - tab: The tab whose `panes` grow by the leaves under the node.
/// - Returns: The node, as the tab's split holds it.
inline Protocol::Sync::TopologyNode appendRecordedNode(const PaneNode& node, std::span<const HostLayoutPane> facts, std::size_t& leafOrdinal, TreeTab& tab)
{
    Protocol::Sync::TopologyNode split;
    // A split with fewer than two children describes no split.
    if (node.leaf || node.children.size() < 2) {
        const HostLayoutPane recorded = leafOrdinal < facts.size() ? facts[leafOrdinal] : HostLayoutPane {};
        ++leafOrdinal;
        TreePane pane;
        pane.label = recorded.label;
        pane.sessionLabel = recorded.sessionLabel;
        pane.tags = recorded.tags;
        pane.sessionTags = recorded.sessionTags;
        // What the record holds of a pane nothing runs in: the title it had,
        // where it was, how large, and the host it reached.
        pane.facts.programTitle = node.title;
        pane.facts.currentDirectory = node.directory;
        pane.facts.columns = recorded.columns;
        pane.facts.rows = recorded.rows;
        if (node.origin.transport == Transport::OriginKind::SSH)
            pane.facts.hostName = node.origin.host;
        pane.facts = Protocol::Sync::boundedForWire(std::move(pane.facts));
        sizeLeaf(split, pane.facts.rows, pane.facts.columns);
        tab.panes.push_back(std::move(pane));
        return split;
    }
    // A record's horizontal split stacks its children, one above the other.
    split.axis = node.horizontalSplit ? Protocol::Sync::SplitAxis::Vertical : Protocol::Sync::SplitAxis::Horizontal;
    split.ratio = node.ratio;
    split.children.push_back(appendRecordedNode(node.children[0], facts, leafOrdinal, tab));
    split.children.push_back(appendRecordedNode(node.children[1], facts, leafOrdinal, tab));
    sizeSplit(split);
    return split;
}

/// Describes a workspace nothing runs in from its record, before anything
/// is derived.
///
/// - Parameter source: The record and the id it is listed by.
/// - Returns: The workspace, its windows, tabs and panes as recorded.
[[nodiscard]] inline TreeWorkspace restorableWorkspace(const TreeRestorableSource& source)
{
    const HostLayoutRecord& record = *source.record;
    TreeWorkspace workspace;
    workspace.state = TreeWorkspaceState::Restorable;
    workspace.id = source.id;
    workspace.name = record.arrangement.name;
    workspace.label = record.workspaceLabel;
    workspace.tags = record.workspaceTags;
    std::size_t tabOrdinal = 0;
    std::size_t leafOrdinal = 0;
    for (std::size_t windowIndex = 0; windowIndex < record.arrangement.windows.size(); ++windowIndex) {
        const WorkspaceWindow& recorded = record.arrangement.windows[windowIndex];
        if (recorded.tabs.empty())
            continue;
        TreeWindow window;
        if (windowIndex < record.windowLabels.size())
            window.label = record.windowLabels[windowIndex];
        if (windowIndex < record.windowTags.size())
            window.tags = record.windowTags[windowIndex];
        window.isHostActive = static_cast<int>(windowIndex) == record.arrangement.activeWindow;
        if (window.isHostActive)
            workspace.hostActiveWindow = workspace.windows.size();
        window.hostActiveTab = static_cast<std::size_t>(std::clamp(recorded.activeTab, 0, static_cast<int>(recorded.tabs.size()) - 1));
        for (std::size_t tabIndex = 0; tabIndex < recorded.tabs.size(); ++tabIndex, ++tabOrdinal) {
            const WorkspaceTab& recordedTab = recorded.tabs[tabIndex];
            TreeTab tab;
            tab.name = recordedTab.title;
            if (tabOrdinal < record.tabLabels.size())
                tab.label = record.tabLabels[tabOrdinal];
            if (tabOrdinal < record.tabTags.size())
                tab.tags = record.tabTags[tabOrdinal];
            tab.isHostActive = tabIndex == window.hostActiveTab;
            tab.split = appendRecordedNode(recordedTab.root, record.panes, leafOrdinal, tab);
            placeNode(tab.split, 0, 0);
            tab.hostFocusedPane = static_cast<std::size_t>(std::clamp(recordedTab.focusedPaneIndex, 0, static_cast<int>(tab.panes.size()) - 1));
            // A record names the zoomed pane by its place in the shown tab.
            if (tab.isHostActive && recorded.zoomedPaneIndex >= 0 && static_cast<std::size_t>(recorded.zoomedPaneIndex) < tab.panes.size())
                window.hostZoomedPane = TreeZoom { .tab = tabIndex, .pane = static_cast<std::size_t>(recorded.zoomedPaneIndex) };
            window.tabs.push_back(std::move(tab));
        }
        workspace.windows.push_back(std::move(window));
    }
    return workspace;
}

} // namespace SessionTreeDetail

namespace SessionTreeDetail {

/// Makes a text well-formed UTF-8 (Core/Unicode/UTF8Repair.h).
///
/// - Parameter text: The text, repaired in place.
inline void repairOne(std::string& text) { (void)Unicode::repairUTF8(text); }

/// Repairs a published status label without copying already valid UTF-8.
inline void repairOne(FallibleString& text)
{
    if (Unicode::wellFormedUTF8PrefixLength(text.view()) == text.size())
        return;
    std::string repaired = text.copyToString();
    (void)Unicode::repairUTF8(repaired);
    text = repaired;
}

/// Makes every text of a list well-formed UTF-8.
///
/// - Parameter texts: The texts, each repaired in place.
inline void repairOne(std::vector<std::string>& texts)
{
    for (std::string& text : texts)
        repairOne(text);
}

/// Makes every text a program wrote into a pane's facts well-formed UTF-8.
///
/// - Parameter facts: The facts, repaired in place.
inline void repairOne(Protocol::Sync::PaneFacts& facts)
{
    for (std::string* text : { &facts.programTitle, &facts.currentDirectory, &facts.homeDirectory, &facts.currentFileURL, &facts.processChain, &facts.processChainWithArguments, &facts.shellCommandName,
             &facts.terminalName, &facts.hostName, &facts.foreground.name, &facts.badgeText, &facts.attention.notificationTitle, &facts.attention.notificationBody })
        repairOne(*text);
    if (facts.indicator)
        repairOne(facts.indicator->label);
    for (Protocol::Sync::UserVariableFact& variable : facts.userVariables) {
        repairOne(variable.name);
        repairOne(variable.value);
    }
}

/// Makes every text of a pane's status set well-formed UTF-8.
///
/// - Parameter records: The set, repaired in place.
inline void repairOne(Protocol::Sync::StatusRecords& records)
{
    for (OSC::ProgramStatus& status : records.store.records) {
        for (auto* text : { &status.id, &status.app, &status.title, &status.message })
            repairOne(*text);
    }
}

/// Makes every text handed to it well-formed UTF-8.
///
/// A name, a label and a tag are bytes a person typed, and a pane's records
/// bytes a program wrote; all of them cross a connection as bytes. A tree is
/// what a listing reads and every outline is built from, so this is the one
/// place those bytes become text: an ill-formed sequence is U+FFFD from here
/// on, in the tree and in everything derived from it.
///
/// - Parameter texts: Texts, lists of texts, a pane's facts or its status
///   set, each repaired in place.
template<typename... Texts>
void repairText(Texts&... texts)
{
    (repairOne(texts), ...);
}

} // namespace SessionTreeDetail

/// Derives everything a tree computes from what it was built from, in place.
///
/// What a tree is built from is what a host sends: every level's identity,
/// labels, tags and names, which window, tab and pane the host's layout has
/// in front, each tab's split and who is attached in a workspace's
/// structure, and for each pane the two records its host sends for it.
/// Everything else is computed here, so a host that built a tree and a
/// client that assembled one hold equal values:
///
/// - a pane's title inputs, with the three the tree decides (`paneLabel` is
///   the pane's label, `tabTitle` the name of the tab holding it, and
///   `windowTitle` the label of its window), its program, badge, status,
///   activity, age, and whether it is focused or zoomed in the host's layout;
/// - every level's status and activity, rolled up from the level under it
///   with `App::rollUpStatus` and `newestActivity`, so a workspace's status
///   is what folding its panes directly would give;
/// - every level's attendance, each client counted once however many panes
///   under the level it is attached to, and the age of its oldest pane.
///
/// Every text in the tree is made well-formed UTF-8 first
/// (`SessionTreeDetail::repairText`): names, labels, tags and each pane's
/// records arrive as bytes, and a reader of a tree is handed text.
///
/// A workspace nothing runs in has panes of no status and no activity: they
/// are counted, and nothing else.
///
/// - Parameter tree: The tree; its derived members are overwritten.
inline void deriveSessionTree(SessionTree& tree)
{
    using SessionTreeDetail::repairText;
    std::vector<TreeAttendee> treeAttendees;
    for (TreeWorkspace& workspace : tree.workspaces) {
        repairText(workspace.name, workspace.label, workspace.tags);
        std::vector<TreeAttendee> workspaceAttendees;
        for (TreeWindow& window : workspace.windows) {
            repairText(window.label, window.tags);
            std::vector<TreeAttendee> windowAttendees;
            for (TreeTab& tab : window.tabs) {
                repairText(tab.name, tab.label, tab.tags);
                std::vector<TreeAttendee> tabAttendees;
                for (std::size_t index = 0; index < tab.panes.size(); ++index) {
                    TreePane& pane = tab.panes[index];
                    // Before anything is derived from them, so what is
                    // derived is text too.
                    repairText(pane.label, pane.sessionLabel, pane.tags, pane.sessionTags, pane.facts, pane.records);
                    pane.title = App::paneTitleFacts(pane.facts, pane.records);
                    pane.title.paneLabel = pane.label;
                    pane.title.tabTitle = tab.name;
                    pane.title.windowTitle = window.label;
                    pane.program = pane.facts.foreground.name;
                    pane.badge = pane.facts.badgeText;
                    pane.activity = { pane.facts.activity.lastOutputUnixMilliseconds, pane.facts.activity.lastInputUnixMilliseconds };
                    pane.createdUnixSeconds = pane.facts.createdUnixSeconds;
                    pane.isAdopted = pane.facts.isAdopted;
                    pane.isHostFocused = index == tab.hostFocusedPane;
                    pane.isHostZoomed = window.hostZoomedPane == TreeZoom { .tab = static_cast<std::size_t>(&tab - window.tabs.data()), .pane = index };
                    if (workspace.isRestorable()) {
                        // A pane nothing runs in: counted, and of no status.
                        pane.status = {};
                        pane.status.paneCount = 1;
                    } else {
                        pane.status = App::paneStatus(App::paneStatusFacts(pane.facts, pane.records));
                    }
                    std::vector<TreeAttendee> distinct;
                    SessionTreeDetail::appendDistinct(distinct, pane.attendees);
                    pane.attendees = std::move(distinct);
                    pane.attendance = SessionTreeDetail::attendanceOf(pane.attendees);
                    SessionTreeDetail::appendDistinct(tabAttendees, pane.attendees);
                }
                SessionTreeDetail::rollUp(tab, tab.panes, tabAttendees);
                SessionTreeDetail::appendDistinct(windowAttendees, tabAttendees);
            }
            SessionTreeDetail::rollUp(window, window.tabs, windowAttendees);
            SessionTreeDetail::appendDistinct(workspaceAttendees, windowAttendees);
        }
        SessionTreeDetail::rollUp(workspace, workspace.windows, workspaceAttendees);
        SessionTreeDetail::appendDistinct(treeAttendees, workspaceAttendees);
    }
    SessionTreeDetail::rollUp(tree, tree.workspaces, treeAttendees);
}

/// Builds the tree of everything a host holds.
///
/// A workspace the host could restore is listed among the live ones, from
/// its record: the same shape, with no identity below the workspace and no
/// status or activity anywhere in it.
///
/// The order is stable: workspaces ascending by id whatever order they are
/// given in, live and restorable together, a workspace's windows in the
/// order they were opened, a window's tabs in tab order, and a tab's panes
/// first child first. Two builds from the same state are equal.
///
/// What each level shows beyond what it is built from is `deriveSessionTree`'s.
///
/// - Parameters:
///   - host: The host's workspaces, its revision and its clock.
///   - labels: The host's label book.
///   - factsForPane: Called once per pane as `factsForPane(workspaceID, pane)`
///     and returns its `TreePaneFacts`.
/// - Returns: The tree.
template<typename FactsForPane>
[[nodiscard]] SessionTree buildSessionTree(const TreeHost& host, const Identity::LabelBook& labels, FactsForPane&& factsForPane)
{
    using Identity::ObjectKind;
    SessionTree tree;
    tree.revision = host.revision;
    tree.hostNowUnixMilliseconds = host.nowUnixMilliseconds;
    tree.workspaces.reserve(host.workspaces.size() + host.restorable.size());
    for (const TreeWorkspaceSource& liveSource : host.workspaces) {
        const TreeWorkspaceSource* source = &liveSource;
        TreeWorkspace workspace;
        workspace.state = source->wasRestoredAfterHostRestart ? TreeWorkspaceState::RestoredAfterHostRestart : TreeWorkspaceState::Live;
        workspace.endsWhenDetached = source->endsWhenDetached;
        workspace.id = source->id;
        workspace.name = source->name;
        workspace.label = labels.label({ ObjectKind::Workspace, source->id });
        workspace.tags = SessionTreeDetail::tagsOf(host.tags, { ObjectKind::Workspace, source->id });
        const int surfaceCount = source->layout ? source->layout->surfaceCount() : 0;
        for (int surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex) {
            const Layout::Workspace& layout = *source->layout;
            const Layout::Surface* surface = layout.surface(layout.surfaceIDAt(surfaceIndex));
            if (!surface || surface->tabCount <= 0)
                continue;
            TreeWindow window;
            window.id = surface->id;
            window.label = labels.label({ ObjectKind::Window, std::to_underlying(surface->id) });
            window.tags = SessionTreeDetail::tagsOf(host.tags, { ObjectKind::Window, std::to_underlying(surface->id) });
            window.isHostActive = surface->id == layout.activeSurface();
            if (window.isHostActive)
                workspace.hostActiveWindow = workspace.windows.size();
            window.hostActiveTab = static_cast<std::size_t>(std::clamp(surface->activeTab, 0, surface->tabCount - 1));
            for (int tabIndex = 0; tabIndex < surface->tabCount; ++tabIndex) {
                const Layout::Tab& layoutTab = surface->tabs[tabIndex];
                TreeTab tab;
                tab.id = layoutTab.id;
                tab.label = labels.label({ ObjectKind::Tab, std::to_underlying(layoutTab.id) });
                tab.tags = SessionTreeDetail::tagsOf(host.tags, { ObjectKind::Tab, std::to_underlying(layoutTab.id) });
                tab.name = layoutTab.title;
                tab.isHostActive = static_cast<std::size_t>(tabIndex) == window.hostActiveTab;
                tab.split = SessionTreeDetail::appendNode(layoutTab.panes, layoutTab.panes.root(), workspace.id, labels, host.tags, factsForPane, tab);
                SessionTreeDetail::placeNode(tab.split, 0, 0);
                const Layout::PaneID focused { static_cast<std::uint32_t>(layoutTab.panes.focusedPaneID()) };
                for (std::size_t index = 0; index < tab.panes.size(); ++index) {
                    if (tab.panes[index].id == focused)
                        tab.hostFocusedPane = index;
                    if (surface->zoomedPane != Layout::invalidPane && tab.panes[index].id == surface->zoomedPane)
                        window.hostZoomedPane = TreeZoom { .tab = window.tabs.size(), .pane = index };
                }
                window.tabs.push_back(std::move(tab));
            }
            workspace.windows.push_back(std::move(window));
        }
        tree.workspaces.push_back(std::move(workspace));
    }
    for (const TreeRestorableSource& source : host.restorable) {
        if (source.record)
            tree.workspaces.push_back(SessionTreeDetail::restorableWorkspace(source));
    }
    std::ranges::stable_sort(tree.workspaces, {}, &TreeWorkspace::id);
    deriveSessionTree(tree);
    return tree;
}

/// What a client holds of one pane beside the structure it was told: the two
/// records its host sent for it, either of which may not have arrived.
struct HeldPaneRecords {
    const Protocol::Sync::PaneFacts* facts = nullptr;
    const Protocol::Sync::StatusRecords* status = nullptr;
};

/// Assembles the tree a client lists from the workspaces' structures it was
/// sent and the records it holds for their panes.
///
/// A workspace's structure says where everything is and what it is called; a
/// pane's facts and status say what it is doing. They reach a client apart
/// and in either order, so a pane whose records have not arrived is listed
/// with its structure alone: its grid, its labels and who is attached. A
/// pane of a restorable workspace has no records to wait for, and keeps what
/// its structure recorded of it. The derivation is the host's own, so a host
/// and a client that hold the same structures and records hold equal trees.
///
/// - Parameters:
///   - structures: The workspaces as their structure records described
///     them, in any order.
///   - revision: What moves when anything in the result may have.
///   - hostNowUnixMilliseconds: The host's wall clock, which ages are read
///     against.
///   - recordsForPane: Called with a pane's `ID128`; returns what is held.
/// - Returns: The tree, its workspaces in id order.
template<typename RecordsForPane>
[[nodiscard]] SessionTree assembleSessionTree(std::span<const TreeWorkspace> structures, std::uint64_t revision, std::uint64_t hostNowUnixMilliseconds, RecordsForPane&& recordsForPane)
{
    SessionTree tree;
    tree.revision = revision;
    tree.hostNowUnixMilliseconds = hostNowUnixMilliseconds;
    tree.workspaces.assign(structures.begin(), structures.end());
    for (TreeWorkspace& workspace : tree.workspaces) {
        if (workspace.isRestorable())
            continue;
        for (TreeWindow& window : workspace.windows) {
            for (TreeTab& tab : window.tabs) {
                std::size_t index = 0;
                const auto fill = [&](const auto& self, const Protocol::Sync::TopologyNode& node) -> void {
                    if (!node.isLeaf()) {
                        for (const Protocol::Sync::TopologyNode& child : node.children)
                            self(self, child);
                        return;
                    }
                    if (index >= tab.panes.size())
                        return;
                    TreePane& pane = tab.panes[index++];
                    const HeldPaneRecords held = recordsForPane(node.paneID);
                    pane.facts = held.facts ? *held.facts : Protocol::Sync::PaneFacts {};
                    pane.records = held.status ? *held.status : Protocol::Sync::StatusRecords {};
                    // Until its facts arrive a pane is as large as its leaf.
                    if (!held.facts) {
                        pane.facts.rows = node.rows;
                        pane.facts.columns = node.columns;
                    }
                };
                fill(fill, tab.split);
            }
        }
    }
    std::ranges::stable_sort(tree.workspaces, {}, &TreeWorkspace::id);
    deriveSessionTree(tree);
    return tree;
}

} // namespace Ubiquitty::Session
