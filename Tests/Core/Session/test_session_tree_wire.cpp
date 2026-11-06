// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_session_tree_wire.cpp — a workspace's structure as bytes
// (SessionTreeWire.h).
//
// A host describes each workspace in a record of its own, and a pane's facts
// and status reach a client in the two records every bound pane is sent. A
// client that holds a host's structures and its panes' records assembles the
// tree its host built: every level's identity, labels and tags, each tab's
// split with each node's cells, and everything computed from them. A
// structure carries no pane record, so nothing a pane does changes it. Every
// count is checked against its cap before an element is read, a body the
// decoder refuses yields nothing, and a workspace at every cap fits its
// bound.

#include "Harness.h"

#include "Core/OSC/Activity.h"
#include "Core/Panes/PaneTreeNode.h"
#include "Core/Protocol/Sync/Records.h"
#include "Core/Protocol/Sync/Wire.h"
#include "Core/Protocol/Tmux/LayoutNode.h"
#include "Core/Protocol/Tmux/PaneLock.h"
#include "Core/Session/HostLayoutRecord.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/SessionTreeWire.h"
#include "Core/Session/StatedPane.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace Ubiquitty::Session;
using Ubiquitty::App::StatusKind;
using Ubiquitty::Panes::Orientation;
using Ubiquitty::Protocol::Sync::ID128;
using Ubiquitty::Protocol::Sync::SplitAxis;
using Ubiquitty::Protocol::Sync::TopologyNode;
using Ubiquitty::Protocol::Sync::WireWriter;
using Ubiquitty::Session::Identity::Allocator;
using Ubiquitty::Session::Identity::LabelBook;
using Ubiquitty::Session::Identity::ObjectKind;
using Ubiquitty::Session::Identity::TagBook;
using UbiquittyTest::sameStatus;
using UbiquittyTest::StatedPane;
namespace OSC = Ubiquitty::OSC;

namespace {

/// The zoom a window holds on one pane of one of its tabs.
std::optional<Ubiquitty::Session::TreeZoom> zoomAt(std::size_t tab, std::size_t pane)
{
    return Ubiquitty::Session::TreeZoom { .tab = tab, .pane = pane };
}

std::vector<std::uint8_t> encoded(const TreeWorkspace& workspace)
{
    std::vector<std::uint8_t> body;
    CHECK(encodeWorkspaceStructure(body, workspace));
    return body;
}

/// Compares everything two trees hold at one level that the levels share.
template<typename Level>
bool sameLevel(const Level& a, const Level& b)
{
    return sameStatus(a.status, b.status) && a.activity == b.activity && a.attendance == b.attendance && a.createdUnixSeconds == b.createdUnixSeconds && a.label == b.label && a.tags == b.tags;
}

bool samePane(const TreePane& a, const TreePane& b)
{
    const auto& ta = a.title;
    const auto& tb = b.title;
    return a.id == b.id && a.sessionID == b.sessionID && a.sessionLabel == b.sessionLabel && a.sessionTags == b.sessionTags && a.facts == b.facts && a.records == b.records
        && ta.programTitle == tb.programTitle && ta.currentDirectory == tb.currentDirectory && ta.processChain == tb.processChain && ta.programStatus == tb.programStatus
        && ta.columns == tb.columns && ta.rows == tb.rows && ta.paneLabel == tb.paneLabel && ta.tabTitle == tb.tabTitle && ta.windowTitle == tb.windowTitle && ta.hostName == tb.hostName
        && a.program == b.program && a.badge == b.badge && a.isHostFocused == b.isHostFocused && a.isHostZoomed == b.isHostZoomed && a.isAdopted == b.isAdopted
        && a.attendees == b.attendees && sameLevel(a, b);
}

/// Compares two trees member by member, derived members included.
std::string treeDifference(const SessionTree& a, const SessionTree& b)
{
    if (a.revision != b.revision || a.hostNowUnixMilliseconds != b.hostNowUnixMilliseconds || a.workspaces.size() != b.workspaces.size())
        return "the trees' heads differ";
    if (!sameStatus(a.status, b.status) || !(a.activity == b.activity) || !(a.attendance == b.attendance) || a.createdUnixSeconds != b.createdUnixSeconds)
        return "the trees' roll-ups differ";
    for (std::size_t w = 0; w < a.workspaces.size(); ++w) {
        const TreeWorkspace& wa = a.workspaces[w];
        const TreeWorkspace& wb = b.workspaces[w];
        const std::string at = "workspace " + std::to_string(w);
        if (wa.state != wb.state || wa.endsWhenDetached != wb.endsWhenDetached || wa.id != wb.id || wa.revision != wb.revision || wa.name != wb.name || wa.hostActiveWindow != wb.hostActiveWindow || wa.windows.size() != wb.windows.size() || !sameLevel(wa, wb))
            return at + " differs";
        for (std::size_t n = 0; n < wa.windows.size(); ++n) {
            const TreeWindow& na = wa.windows[n];
            const TreeWindow& nb = wb.windows[n];
            if (na.id != nb.id || na.hostActiveTab != nb.hostActiveTab || na.hostZoomedPane != nb.hostZoomedPane || na.isHostActive != nb.isHostActive || na.tabs.size() != nb.tabs.size() || !sameLevel(na, nb))
                return at + " window " + std::to_string(n) + " differs";
            for (std::size_t t = 0; t < na.tabs.size(); ++t) {
                const TreeTab& ta = na.tabs[t];
                const TreeTab& tb = nb.tabs[t];
                const std::string tabAt = at + " window " + std::to_string(n) + " tab " + std::to_string(t);
                if (ta.id != tb.id || ta.name != tb.name || !(ta.split == tb.split) || ta.hostFocusedPane != tb.hostFocusedPane || ta.isHostActive != tb.isHostActive || ta.panes.size() != tb.panes.size() || !sameLevel(ta, tb))
                    return tabAt + " differs";
                for (std::size_t p = 0; p < ta.panes.size(); ++p) {
                    if (!samePane(ta.panes[p], tb.panes[p]))
                        return tabAt + " pane " + std::to_string(p) + " differs";
                }
            }
        }
    }
    return {};
}

/// A host of two live workspaces and one it could restore: a window of two
/// tabs, one split three ways, a second window, labels and tags at every
/// level, panes of every status, and people and an agent attached.
struct Host {
    std::shared_ptr<Allocator> identities = std::make_shared<Allocator>();
    std::unique_ptr<Layout::Workspace> work = std::make_unique<Layout::Workspace>(identities);
    std::unique_ptr<Layout::Workspace> side = std::make_unique<Layout::Workspace>(identities);
    LabelBook labels;
    TagBook tags;
    HostLayoutRecord record;
    std::map<std::uint32_t, StatedPane> panes;
    Layout::SurfaceID w1 {}, w2 {};
    Layout::TabID t1 {}, t2 {};
    Layout::PaneID a {}, b {}, c {}, d {}, e {}, f {};

    Host()
    {
        w1 = work->surfaceIDAt(0);
        t1 = work->tabAt(w1, 0);
        a = work->focusedPane(w1);
        b = work->splitFocusedPane(w1, Orientation::Vertical, 0.25f);
        c = work->splitFocusedPane(w1, Orientation::Horizontal, 0.5f);
        t2 = work->addTab(w1);
        d = work->focusedPane(w1);
        w2 = work->addSurface();
        e = work->focusedPane(w2);
        (void)work->renameTab(w1, t1, "build");
        (void)work->setActiveTab(w1, t1);
        (void)work->toggleZoom(w1, b);
        (void)work->setActiveSurface(w2);
        f = side->focusedPane(side->surfaceIDAt(0));

        (void)labels.set({ ObjectKind::Workspace, 9 }, "nine");
        (void)labels.set({ ObjectKind::Window, std::to_underlying(w1) }, "ops");
        (void)labels.set({ ObjectKind::Tab, std::to_underlying(t1) }, "tb1");
        (void)labels.set({ ObjectKind::Pane, std::to_underlying(c) }, "editor");
        (void)labels.set({ ObjectKind::Session, 77 }, "longrun");
        (void)tags.set({ ObjectKind::Workspace, 9 }, std::vector<std::string> { "prod", "eu" });
        (void)tags.set({ ObjectKind::Window, std::to_underlying(w1) }, std::vector<std::string> { "deploy" });
        (void)tags.set({ ObjectKind::Tab, std::to_underlying(t1) }, std::vector<std::string> { "ci" });
        (void)tags.set({ ObjectKind::Pane, std::to_underlying(c) }, std::vector<std::string> { "mine" });

        at(a) = stated("make all", "make", "/srv/app", 1);
        at(a).status.programState = OSC::ProgramState::Blocked;
        at(a).status.blockKind = OSC::ProgramBlockKind::Permission;
        at(a).activity = { 99'500, 10 };
        at(a).attendees = { { 1, TreeAttendeeKind::Person } };
        at(a).createdUnixSeconds = 500;
        at(b) = stated("tests", "ctest", "/srv/app/build", 2);
        at(b).status.programState = OSC::ProgramState::Done;
        at(b).status.hasUnseenNotification = true;
        at(b).badgeText = "x";
        at(b).activity = { 70'000, 95'000 };
        at(b).attendees = { { 1, TreeAttendeeKind::Person }, { 2, TreeAttendeeKind::Agent }, { 1, TreeAttendeeKind::Person } };
        at(b).createdUnixSeconds = 300;
        at(c) = stated("vim notes", "vim", "/home/user/notes", 3);
        at(c).status.programState = OSC::ProgramState::Working;
        at(c).status.progress = { OSC::ProgressNormal, 40 };
        at(c).status.tabColor = Ubiquitty::ColorRGBA { 1, 2, 3, 255 };
        at(c).status.indicatorColor = Ubiquitty::ColorRGBA { 9, 8, 7, 255 };
        at(c).structuralGeneration = 31;
        at(d) = stated("top", "top", "/", 4);
        at(d).title.hostName = "prod-2";
        at(d).sessionID = 77;
        at(d).status.isAtPrompt = true;
        at(d).status.exitStatus = 3;
        at(d).isAdopted = true;
        at(e) = stated("deploy", "ansible", "/srv/deploy", 5);
        at(e).status.programState = OSC::ProgramState::Error;
        at(e).status.hasUnseenBell = true;
        at(e).activity = { 200'000, 0 };
        at(f) = stated("zsh", "zsh", "/home/user", 6);
        at(f).status.isAtPrompt = true;

        const auto leaf = [](std::string directory, std::string title, Ubiquitty::Transport::Origin origin = {}) {
            PaneNode node = PaneNode::makeLeaf(std::move(origin), std::move(title));
            node.directory = std::move(directory);
            return node;
        };
        record.arrangement.name = "kept";
        record.workspaceLabel = "k";
        WorkspaceWindow first;
        WorkspaceTab single;
        single.root = leaf("/home/user", "shell");
        first.tabs.push_back(single);
        WorkspaceTab split;
        split.title = "logs";
        split.root = PaneNode::makeSplit(false, 0.25, leaf("/var/log", "tail"), PaneNode::makeSplit(true, 0.5, leaf("/tmp", "less"), leaf("/srv", "vim", Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::SSH, "user@box" })));
        split.focusedPaneIndex = 2;
        first.tabs.push_back(split);
        first.activeTab = 1;
        first.zoomedPaneIndex = 0;
        record.arrangement.windows.push_back(first);
        record.arrangement.activeWindow = 0;
        record.windowLabels = { "main" };
        record.tabLabels = { "", "tailing" };
        record.windowTags = { { "production" } };
        record.panes = { { 80, 24, "", "", {}, {} }, { 100, 30, "tailer", "", {}, {} }, { 50, 15, "", "scratch", {}, {} }, { 50, 14, "", "", {}, {} } };
    }

    static StatedPane stated(std::string programTitle, std::string program, std::string directory, std::uint64_t paneNumber)
    {
        StatedPane pane;
        pane.title.programTitle = std::move(programTitle);
        pane.title.currentDirectory = std::move(directory);
        pane.title.columns = 80 + static_cast<int>(paneNumber);
        pane.title.rows = 24 + static_cast<int>(paneNumber);
        pane.program = std::move(program);
        pane.paneID = { .low = paneNumber, .high = 7 };
        pane.structuralGeneration = paneNumber;
        return pane;
    }

    StatedPane& at(Layout::PaneID id) { return panes[std::to_underlying(id)]; }

    SessionTree tree()
    {
        const std::array sources {
            TreeWorkspaceSource { .id = 9, .name = "work", .layout = work.get(), .endsWhenDetached = true },
            TreeWorkspaceSource { .id = 2, .name = "", .layout = side.get(), .wasRestoredAfterHostRestart = true },
        };
        const std::array restorable { TreeRestorableSource { 5, &record } };
        return buildSessionTree({ .revision = 31, .nowUnixMilliseconds = 250'000, .workspaces = sources, .tags = &tags, .restorable = restorable }, labels,
            [this](std::uint32_t, Layout::PaneID id) { return panes[std::to_underlying(id)].treeFacts(); });
    }
};

/// What a client holds of a host's panes: the two records each was sent,
/// by the pane's name on the screen plane.
struct HeldRecords {
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::pair<Ubiquitty::Protocol::Sync::PaneFacts, Ubiquitty::Protocol::Sync::StatusRecords>> held;

    explicit HeldRecords(const Host& host)
    {
        for (const auto& [id, pane] : host.panes) {
            const TreePaneFacts facts = pane.treeFacts();
            held[{ facts.paneID.low, facts.paneID.high }] = { Ubiquitty::Protocol::Sync::boundedForWire(facts.facts), Ubiquitty::Protocol::Sync::boundedForWire(facts.records) };
        }
    }

    HeldRecords() = default;

    [[nodiscard]] HeldPaneRecords operator()(ID128 paneID) const
    {
        const auto found = held.find({ paneID.low, paneID.high });
        return found == held.end() ? HeldPaneRecords {} : HeldPaneRecords { .facts = &found->second.first, .status = &found->second.second };
    }
};

/// Sends every workspace of a tree through its structure record.
std::vector<TreeWorkspace> structuresOf(const SessionTree& tree)
{
    std::vector<TreeWorkspace> structures;
    for (const TreeWorkspace& workspace : tree.workspaces) {
        std::optional<TreeWorkspace> read = decodeWorkspaceStructure(encoded(workspace));
        CHECK_MSG(read.has_value(), "a structure its encoder wrote was not read");
        if (!read)
            return structures;
        structures.push_back(std::move(*read));
    }
    return structures;
}

/// A workspace of one window, one tab and `panes` panes, each split off the
/// one before so the tab nests one split a pane.
TreeWorkspace chainedWorkspace(std::size_t panes)
{
    TreeWorkspace workspace;
    workspace.id = 1;
    workspace.revision = 4;
    TreeWindow window;
    TreeTab tab;
    for (std::size_t index = 0; index < panes; ++index) {
        TreePane pane;
        pane.id = Layout::PaneID { static_cast<std::uint32_t>(index + 1) };
        tab.panes.push_back(std::move(pane));
    }
    // Built from the last pane up, so the leaves read in pane order.
    TopologyNode node;
    node.paneID = { .low = panes, .high = 0 };
    for (std::size_t index = panes - 1; index > 0; --index) {
        TopologyNode leaf;
        leaf.paneID = { .low = index, .high = 0 };
        TopologyNode split;
        split.children.push_back(std::move(leaf));
        split.children.push_back(std::move(node));
        node = std::move(split);
    }
    tab.split = std::move(node);
    window.tabs.push_back(std::move(tab));
    workspace.windows.push_back(std::move(window));
    return workspace;
}

/// Checks a node and everything under it against the layout the string
/// parsed to, and against where the host's rule places each child.
void checkCells(const TopologyNode& node, const Ubiquitty::Protocol::Tmux::LayoutNode& parsed, int offsetRows, int offsetColumns)
{
    using Ubiquitty::Protocol::Tmux::Split;
    CHECK_EQ(static_cast<int>(node.columns), parsed.width);
    CHECK_EQ(static_cast<int>(node.rows), parsed.height);
    CHECK_EQ(static_cast<int>(node.offsetRows), offsetRows);
    CHECK_EQ(static_cast<int>(node.offsetColumns), offsetColumns);
    CHECK(node.isLeaf() == parsed.isLeaf());
    if (node.isLeaf() || parsed.isLeaf())
        return;
    CHECK((node.axis == SplitAxis::Horizontal) == (parsed.split == Split::Columns));
    REQUIRE(node.children.size() == parsed.children.size());
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        checkCells(node.children[index], parsed.children[index], offsetRows, offsetColumns);
        if (node.axis == SplitAxis::Horizontal)
            offsetColumns += node.children[index].columns;
        else
            offsetRows += node.children[index].rows;
    }
}

} // namespace

TEST(aTreeAssembledFromStructuresAndRecordsEqualsTheTreeItsHostBuilt)
{
    Host host;
    const SessionTree built = host.tree();
    REQUIRE(built.workspaces.size() == 3);
    // The tree holds what the test is about: a split, a zoom, a restorable
    // workspace, statuses that roll up and people counted once.
    REQUIRE(built.workspaces[2].windows[0].tabs[0].panes.size() == 3);
    CHECK(built.workspaces[1].isRestorable());
    CHECK(built.status.kind != StatusKind::None && built.status.paneCount == 10);
    CHECK(built.attendance.people == 1 && built.attendance.agents == 1);

    const std::vector<TreeWorkspace> structures = structuresOf(built);
    REQUIRE(structures.size() == 3);
    const SessionTree read = assembleSessionTree(structures, built.revision, built.hostNowUnixMilliseconds, HeldRecords(host));
    const std::string difference = treeDifference(built, read);
    CHECK_MSG(difference.empty(), difference.c_str());
    // What a structure reads, it writes back byte for byte.
    for (std::size_t index = 0; index < structures.size(); ++index)
        CHECK(encoded(structures[index]) == encoded(built.workspaces[index]));
    // The structures arrive in any order.
    std::vector<TreeWorkspace> reversed(structures.rbegin(), structures.rend());
    CHECK(treeDifference(built, assembleSessionTree(reversed, built.revision, built.hostNowUnixMilliseconds, HeldRecords(host))).empty());

    // Spot checks of what was derived on the reading side.
    const TreeWorkspace& work = read.workspaces[2];
    CHECK(work.id == 9 && work.name == "work" && work.label == "nine" && work.endsWhenDetached);
    CHECK((work.tags == std::vector<std::string> { "prod", "eu" }));
    CHECK(work.hostActiveWindow == 1 && work.windows[1].isHostActive && !work.windows[0].isHostActive);
    const TreeTab& tab = work.windows[0].tabs[0];
    CHECK(tab.name == "build" && tab.label == "tb1" && tab.isHostActive);
    // A vertical divider: the two halves are side by side.
    CHECK(tab.split.children.size() == 2 && tab.split.axis == SplitAxis::Horizontal && tab.split.children[1].axis == SplitAxis::Vertical);
    CHECK(tab.split.children[1].children[1].paneID == (ID128 { .low = 3, .high = 7 }) && tab.split.children[1].children[1].structuralGeneration == 31);
    CHECK(tab.status.kind == StatusKind::Blocked && tab.status.paneCount == 3 && tab.status.badgeCount == 1);
    CHECK(tab.attendance.people == 1 && tab.attendance.agents == 1);
    CHECK_EQ(tab.createdUnixSeconds, 300u);
    const TreePane& editor = tab.panes[2];
    CHECK(editor.id == host.c && editor.label == "editor" && editor.title.paneLabel == "editor" && editor.title.tabTitle == "build" && editor.title.windowTitle == "ops");
    CHECK(editor.program == "vim" && editor.title.programTitle == "vim notes" && editor.title.currentDirectory == "/home/user/notes");
    CHECK(editor.status.kind == StatusKind::Working && editor.status.progress.value == 40);
    CHECK(tab.panes[1].isHostZoomed && tab.panes[1].attendees.size() == 2);
    const TreePane& top = work.windows[0].tabs[1].panes[0];
    CHECK(top.sessionID == 77 && top.sessionLabel == "longrun" && top.isAdopted && top.title.hostName == "prod-2");
    CHECK(top.status.kind == StatusKind::Failed);
    // The restorable workspace: no identity below it, no status, its record's
    // titles, directories and grids.
    const TreeWorkspace& kept = read.workspaces[1];
    CHECK(kept.isRestorable() && kept.id == 5 && kept.name == "kept" && kept.label == "k");
    CHECK(kept.status.kind == StatusKind::None && kept.status.paneCount == 4);
    const TreeTab& logs = kept.windows[0].tabs[1];
    CHECK(logs.id == Layout::invalidTab && logs.name == "logs" && logs.label == "tailing" && logs.hostFocusedPane == 2);
    CHECK(logs.panes[0].id == Layout::invalidPane && logs.panes[0].label == "tailer" && logs.panes[0].title.programTitle == "tail" && logs.panes[0].title.columns == 100);
    CHECK(logs.panes[2].title.hostName == "user@box" && logs.panes[2].title.currentDirectory == "/srv");
    CHECK(read.workspaces[0].state == TreeWorkspaceState::RestoredAfterHostRestart);
}

TEST(aStructureCarriesNothingAPaneDoes)
{
    Host host;
    const std::vector<std::uint8_t> before = encoded(host.tree().workspaces[2]);
    // Everything a pane's two records say moves, and its structure does not.
    host.at(host.a).title.programTitle = "another title entirely";
    host.at(host.a).title.currentDirectory = "/somewhere/else";
    host.at(host.a).status.programState = OSC::ProgramState::Done;
    host.at(host.a).status.hasUnseenBell = true;
    host.at(host.a).activity = { 1, 2 };
    host.at(host.a).badgeText = "badge";
    host.at(host.a).program = "other";
    CHECK(encoded(host.tree().workspaces[2]) == before);
    // What it does carry moves it: the grid, a label, who is attached.
    host.at(host.a).title.columns += 1;
    CHECK(encoded(host.tree().workspaces[2]) != before);
    host.at(host.a).title.columns -= 1;
    host.at(host.a).attendees.push_back({ 9, TreeAttendeeKind::Agent });
    CHECK(encoded(host.tree().workspaces[2]) != before);

    // A structure read back holds no record of a running pane.
    const std::optional<TreeWorkspace> read = decodeWorkspaceStructure(before);
    REQUIRE(read.has_value());
    const TreePane& pane = read->windows[0].tabs[0].panes[0];
    CHECK(pane.facts.programTitle.empty() && pane.facts.currentDirectory.empty() && pane.records.store.records.empty());
    CHECK(pane.id == host.a && pane.facts.columns == 81 && pane.facts.rows == 25);
}

TEST(aPaneWhoseRecordsHaveNotArrivedIsListedFromItsStructure)
{
    Host host;
    const SessionTree built = host.tree();
    const std::vector<TreeWorkspace> structures = structuresOf(built);
    const SessionTree bare = assembleSessionTree(structures, 1, 2, HeldRecords());
    CHECK_EQ(bare.revision, 1u);
    CHECK_EQ(bare.hostNowUnixMilliseconds, 2u);
    const TreeTab& tab = bare.workspaces[2].windows[0].tabs[0];
    REQUIRE(tab.panes.size() == 3);
    const TreePane& editor = tab.panes[2];
    CHECK(editor.id == host.c && editor.label == "editor" && (editor.tags == std::vector<std::string> { "mine" }));
    CHECK(editor.columns() == 83 && editor.rows() == 27);
    CHECK(editor.title.programTitle.empty() && editor.program.empty() && editor.records.store.records.empty());
    CHECK(tab.panes[1].attendance.people == 1 && tab.panes[1].attendance.agents == 1);
    // A restorable workspace waits for no record: its leaves carried them.
    CHECK(bare.workspaces[1].windows[0].tabs[1].panes[0].title.programTitle == "tail");
    // One pane's records arrive: that pane fills in and no other.
    HeldRecords one;
    const TreePaneFacts facts = host.at(host.c).treeFacts();
    one.held[{ facts.paneID.low, facts.paneID.high }] = { facts.facts, facts.records };
    const SessionTree partial = assembleSessionTree(structures, 1, 2, one);
    CHECK(partial.workspaces[2].windows[0].tabs[0].panes[2].title.programTitle == "vim notes");
    CHECK(partial.workspaces[2].windows[0].tabs[0].panes[0].title.programTitle.empty());
}

TEST(everyNodeCarriesTheCellsTheHostsLayoutStringSays)
{
    using namespace Ubiquitty::Protocol;
    Host host;
    // More shapes than the first tab's: a row of three, and a column whose
    // first half is itself split, with grids that differ on every pane.
    const Layout::PaneID g = host.side->splitFocusedPane(host.side->surfaceIDAt(0), Orientation::Horizontal, 0.5f);
    const Layout::PaneID h = host.side->splitFocusedPane(host.side->surfaceIDAt(0), Orientation::Vertical, 0.3f);
    host.at(g) = Host::stated("g", "g", "/", 7);
    host.at(h) = Host::stated("h", "h", "/", 8);
    host.at(h).title.columns = 13;
    host.at(h).title.rows = 5;
    const SessionTree tree = host.tree();
    const auto dimensions = [](void* context, Tmux::PaneID paneID) {
        const StatedPane& pane = static_cast<Host*>(context)->panes[static_cast<std::uint32_t>(paneID)];
        return Tmux::PaneDimensions { .columns = pane.title.columns, .rows = pane.title.rows };
    };
    const auto check = [&](const Layout::Workspace& layout, Layout::SurfaceID surface, Layout::TabID tabID, const TreeTab& tab) {
        const Ubiquitty::Panes::Tree* panes = layout.paneTree(surface, tabID);
        REQUIRE(panes != nullptr);
        std::string said = "0000,";
        (void)Tmux::emitPaneLayout(*panes, panes->root(), dimensions, &host, said);
        Tmux::LayoutNode parsed;
        REQUIRE(Tmux::parseLayoutTree(said, parsed));
        checkCells(tab.split, parsed, 0, 0);
    };
    check(*host.work, host.w1, host.t1, tree.workspaces[2].windows[0].tabs[0]);
    check(*host.work, host.w1, host.t2, tree.workspaces[2].windows[0].tabs[1]);
    check(*host.side, host.side->surfaceIDAt(0), host.side->tabAt(host.side->surfaceIDAt(0), 0), tree.workspaces[0].windows[0].tabs[0]);
    // The numbers themselves, for the first tab: a at 81x25 beside b over c.
    const TopologyNode& root = tree.workspaces[2].windows[0].tabs[0].split;
    CHECK(root.columns == 81 + 83 && root.rows == 26 + 27);
    CHECK(root.children[1].offsetColumns == 81 && root.children[1].offsetRows == 0 && root.children[1].columns == 83);
    CHECK(root.children[1].children[1].offsetRows == 26 && root.children[1].children[1].offsetColumns == 81);
    // And they cross the wire.
    const std::optional<TreeWorkspace> read = decodeWorkspaceStructure(encoded(tree.workspaces[2]));
    REQUIRE(read.has_value());
    CHECK(read->windows[0].tabs[0].split == root);
    // A pane the host has not sized is a cell, as the string says it.
    host.at(host.e).title.columns = 0;
    host.at(host.e).title.rows = 0;
    const TopologyNode unsized = host.tree().workspaces[2].windows[1].tabs[0].split;
    CHECK(unsized.columns == 1 && unsized.rows == 1);
}

TEST(theZoomIsItsWindows)
{
    Host host;
    const SessionTree built = host.tree();
    const TreeWindow& window = built.workspaces[2].windows[0];
    // The host zoomed b, the second pane of the window's first tab.
    CHECK(window.hostZoomedPane == zoomAt(0, 1));
    CHECK(window.tabs[0].panes[1].isHostZoomed && !window.tabs[0].panes[0].isHostZoomed && !window.tabs[1].panes[0].isHostZoomed);
    CHECK(!built.workspaces[2].windows[1].hostZoomedPane);
    // A record names the zoomed pane in the tab its window showed.
    const TreeWindow& kept = built.workspaces[1].windows[0];
    CHECK(kept.hostZoomedPane == zoomAt(1, 0));
    CHECK(kept.tabs[1].panes[0].isHostZoomed && !kept.tabs[0].panes[0].isHostZoomed);
    // Both cross the wire, and the pane's own answer is derived from them.
    const std::vector<TreeWorkspace> structures = structuresOf(built);
    CHECK(structures[2].windows[0].hostZoomedPane == window.hostZoomedPane && structures[1].windows[0].hostZoomedPane == kept.hostZoomedPane);
    const SessionTree read = assembleSessionTree(structures, 0, 0, HeldRecords(host));
    CHECK(read.workspaces[2].windows[0].tabs[0].panes[1].isHostZoomed && read.workspaces[1].windows[0].tabs[1].panes[0].isHostZoomed);
    // Unzoomed, no pane is.
    (void)host.work->toggleZoom(host.w1, host.b);
    const SessionTree unzoomed = host.tree();
    CHECK(!unzoomed.workspaces[2].windows[0].hostZoomedPane && !unzoomed.workspaces[2].windows[0].tabs[0].panes[1].isHostZoomed);
    // Zoomed in the window's second tab, the zoom names that tab.
    CHECK(host.work->setActiveTab(host.w1, host.t2));
    const Layout::PaneID beside = host.work->splitFocusedPane(host.w1, Orientation::Vertical, 0.5f);
    host.at(beside) = Host::stated("beside", "sh", "/", 9);
    CHECK(host.work->toggleZoom(host.w1, host.d));
    const SessionTree second = host.tree();
    CHECK(second.workspaces[2].windows[0].hostZoomedPane == zoomAt(1, 0));
    CHECK(second.workspaces[2].windows[0].tabs[1].panes[0].isHostZoomed && !second.workspaces[2].windows[0].tabs[0].panes[0].isHostZoomed);
}

TEST(derivingATreeAgainChangesNothing)
{
    Host host;
    const SessionTree built = host.tree();
    SessionTree again = built;
    deriveSessionTree(again);
    CHECK(treeDifference(built, again).empty());
}

TEST(aWorkspaceAtEveryLimitFitsItsBound)
{
    // Sixty-four panes, a tab each, in sixteen windows; every text at its
    // cap, every tag slot full, every pane with the most attendees a leaf
    // names, and the recorded texts a restorable leaf carries.
    TreeWorkspace workspace;
    workspace.state = TreeWorkspaceState::Restorable;
    workspace.id = 0xFFFF'FFFF;
    workspace.revision = ~std::uint64_t { 0 };
    workspace.endsWhenDetached = true;
    workspace.name.assign(maxTreeNameBytes, 'n');
    const std::string label(Identity::maxLabelLength, 'l');
    const std::vector<std::string> tags(Identity::maxTagsPerObject, label);
    workspace.label = label;
    workspace.tags = tags;
    std::uint32_t next = 1;
    for (int windowIndex = 0; windowIndex < Layout::maxSurfaces; ++windowIndex) {
        TreeWindow window;
        window.id = Layout::SurfaceID { next++ };
        window.label = label;
        window.tags = tags;
        for (std::size_t tabIndex = 0; tabIndex < maxStructureLeaves / static_cast<std::size_t>(Layout::maxSurfaces); ++tabIndex) {
            TreeTab tab;
            tab.id = Layout::TabID { next++ };
            tab.label = label;
            tab.tags = tags;
            tab.name.assign(maxTreeNameBytes, 't');
            TreePane pane;
            pane.id = Layout::PaneID { next++ };
            pane.sessionID = 0xFFFF'FFFF;
            pane.label = label;
            pane.tags = tags;
            pane.sessionLabel = label;
            pane.sessionTags = tags;
            for (std::size_t attendee = 0; attendee < maxTreeAttendeesPerPane; ++attendee)
                pane.attendees.push_back({ static_cast<std::uint32_t>(attendee + 1), TreeAttendeeKind::Agent });
            pane.facts.programTitle.assign(maxHostLayoutTitleLength, 'p');
            pane.facts.currentDirectory.assign(maxHostLayoutDirectoryLength, 'd');
            pane.facts.hostName.assign(maxHostLayoutRemoteHostLength, 'h');
            tab.panes.push_back(std::move(pane));
            tab.split.paneID = { .low = ~std::uint64_t { 0 }, .high = ~std::uint64_t { 0 } };
            tab.split.rows = tab.split.columns = tab.split.offsetRows = tab.split.offsetColumns = 0xFFFF;
            tab.split.structuralGeneration = ~std::uint64_t { 0 };
            window.tabs.push_back(std::move(tab));
        }
        window.hostZoomedPane = TreeZoom { .tab = window.tabs.size() - 1, .pane = 0 };
        window.hostActiveTab = window.tabs.size() - 1;
        workspace.windows.push_back(std::move(window));
    }
    workspace.hostActiveWindow = workspace.windows.size() - 1;
    std::vector<std::uint8_t> body;
    REQUIRE(encodeWorkspaceStructure(body, workspace));
    CHECK_MSG(body.size() <= maxWorkspaceStructureBytes, ("a workspace at every cap takes " + std::to_string(body.size()) + " bytes, past the bound of " + std::to_string(maxWorkspaceStructureBytes)).c_str());
    // The bound is no looser than the caps make it: within a tenth.
    CHECK(body.size() * 10 >= maxWorkspaceStructureBytes * 9);
    const std::optional<TreeWorkspace> read = decodeWorkspaceStructure(body);
    REQUIRE(read.has_value());
    CHECK(encoded(*read) == body);

    // One tab of the most panes a tab holds, each split off the one before:
    // the deepest nest a layout holds, read by the one nesting bound.
    const TreeWorkspace deepest = chainedWorkspace(static_cast<std::size_t>(Layout::maxPanesPerTab));
    std::vector<std::uint8_t> deep;
    REQUIRE(encodeWorkspaceStructure(deep, deepest));
    const std::optional<TreeWorkspace> readDeep = decodeWorkspaceStructure(deep);
    REQUIRE(readDeep.has_value());
    CHECK(readDeep->windows[0].tabs[0].panes.size() == static_cast<std::size_t>(Layout::maxPanesPerTab));
    CHECK(readDeep->revision == 4);
}

TEST(aWorkspaceNameIsWholeAtItsBoundAndRefusedPastIt)
{
    TreeWorkspace workspace = chainedWorkspace(1);
    workspace.name.assign(maxWorkspaceNameBytes, 'n');
    workspace.label.assign(Identity::maxLabelLength + 9, 'l');
    workspace.windows[0].tabs[0].name.assign(maxTreeNameBytes + 1, 't');
    const std::optional<TreeWorkspace> read = decodeWorkspaceStructure(encoded(workspace));
    REQUIRE(read.has_value());
    CHECK(read->name == workspace.name);
    CHECK_EQ(read->label.size(), Identity::maxLabelLength);
    CHECK_EQ(read->windows[0].tabs[0].name.size(), maxTreeNameBytes);
    workspace.name.push_back('n');
    std::vector<std::uint8_t> untouched { 0xAB };
    CHECK(!encodeWorkspaceStructure(untouched, workspace));
    CHECK(untouched == std::vector<std::uint8_t> { 0xAB });
}

TEST(aWorkspaceNoHostHoldsIsRefusedWholeByItsEncoder)
{
    const auto refused = [](const TreeWorkspace& workspace) {
        std::vector<std::uint8_t> body { 0xAB };
        const bool wasWritten = encodeWorkspaceStructure(body, workspace); // bool-ok: the encoder's verdict
        // A refused workspace leaves the buffer as it was.
        return !wasWritten && body == std::vector<std::uint8_t> { 0xAB };
    };
    const TreeWorkspace plain = chainedWorkspace(3);
    CHECK(!refused(plain));

    TreeWorkspace manyWindows = plain;
    while (manyWindows.windows.size() <= static_cast<std::size_t>(Layout::maxSurfaces))
        manyWindows.windows.push_back(manyWindows.windows.front());
    CHECK(refused(manyWindows));

    TreeWorkspace manyTabs = chainedWorkspace(1);
    while (manyTabs.windows[0].tabs.size() <= static_cast<std::size_t>(Layout::maxTabsPerSurface))
        manyTabs.windows[0].tabs.push_back(manyTabs.windows[0].tabs.front());
    CHECK(refused(manyTabs));

    // More panes than a host runs, though no tab holds too many.
    TreeWorkspace manyPanes = chainedWorkspace(33);
    manyPanes.windows[0].tabs.push_back(manyPanes.windows[0].tabs.front());
    CHECK(refused(manyPanes));
    CHECK(refused(chainedWorkspace(static_cast<std::size_t>(Layout::maxPanesPerTab) + 1)));

    TreeWorkspace manyTags = plain;
    manyTags.tags.assign(Identity::maxTagsPerObject + 1, "t");
    CHECK(refused(manyTags));
    TreeWorkspace manyPaneTags = plain;
    manyPaneTags.windows[0].tabs[0].panes[1].tags.assign(Identity::maxTagsPerObject + 1, "t");
    CHECK(refused(manyPaneTags));
    TreeWorkspace manySessionTags = plain;
    manySessionTags.windows[0].tabs[0].panes[1].sessionTags.assign(Identity::maxTagsPerObject + 1, "t");
    CHECK(refused(manySessionTags));
    manySessionTags.windows[0].tabs[0].panes[1].sessionTags.pop_back();
    CHECK(!refused(manySessionTags));
    TreeWorkspace crowded = plain;
    crowded.windows[0].tabs[0].panes[0].attendees.assign(maxTreeAttendeesPerPane + 1, TreeAttendee {});
    CHECK(refused(crowded));

    TreeWorkspace noTab = plain;
    noTab.windows[0].tabs.clear();
    CHECK(refused(noTab));
    TreeWorkspace noPane = plain;
    noPane.windows[0].tabs[0].panes.clear();
    CHECK(refused(noPane));
    // A split whose leaves are not the tab's panes, either way.
    TreeWorkspace fewerLeaves = plain;
    fewerLeaves.windows[0].tabs[0].panes.push_back(TreePane {});
    CHECK(refused(fewerLeaves));
    TreeWorkspace moreLeaves = plain;
    moreLeaves.windows[0].tabs[0].panes.pop_back();
    CHECK(refused(moreLeaves));

    TreeWorkspace focus = plain;
    focus.windows[0].tabs[0].hostFocusedPane = 3;
    CHECK(refused(focus));
    TreeWorkspace activeTab = plain;
    activeTab.windows[0].hostActiveTab = 1;
    CHECK(refused(activeTab));
    TreeWorkspace activeWindow = plain;
    activeWindow.hostActiveWindow = 1;
    CHECK(refused(activeWindow));
    TreeWorkspace zoomTab = plain;
    zoomTab.windows[0].hostZoomedPane = TreeZoom { .tab = 1, .pane = 0 };
    CHECK(refused(zoomTab));
    TreeWorkspace zoomPane = plain;
    zoomPane.windows[0].hostZoomedPane = TreeZoom { .tab = 0, .pane = 3 };
    CHECK(refused(zoomPane));
    zoomPane.windows[0].hostZoomedPane = TreeZoom { .tab = 0, .pane = 2 };
    CHECK(!refused(zoomPane));

    for (const double ratio : { -0.1, 1.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() }) {
        TreeWorkspace bent = plain;
        bent.windows[0].tabs[0].split.ratio = ratio;
        CHECK(refused(bent));
    }
    TreeWorkspace oneChild = plain;
    oneChild.windows[0].tabs[0].split.children.pop_back();
    oneChild.windows[0].tabs[0].panes.resize(1);
    CHECK(refused(oneChild));
    // A workspace with no window says so, and has none in front.
    TreeWorkspace empty;
    CHECK(!refused(empty));
    empty.hostActiveWindow = 1;
    CHECK(refused(empty));
}

TEST(aBodyNoHostSendsIsRefusedByTheDecoder)
{
    Host host;
    const SessionTree built = host.tree();
    for (const TreeWorkspace& workspace : built.workspaces) {
        const std::vector<std::uint8_t> body = encoded(workspace);
        REQUIRE(decodeWorkspaceStructure(body).has_value());
        // Cut anywhere, it is not a structure; nor with a byte after it.
        for (std::size_t length = 0; length < body.size(); ++length)
            CHECK_MSG(!decodeWorkspaceStructure(std::span { body }.first(length)).has_value(), ("a body cut to " + std::to_string(length) + " bytes was read").c_str());
        std::vector<std::uint8_t> trailing = body;
        trailing.push_back(0);
        CHECK(!decodeWorkspaceStructure(trailing).has_value());
    }
    // More panes than a host runs, though no tab holds too many: two tabs
    // of thirty-three. The encoder writes no such body, so it is spliced
    // from one tab's bytes, which begin past the workspace's and the
    // window's heads.
    {
        const auto twoTabsOf = [](std::size_t panes) {
            const std::vector<std::uint8_t> one = encoded(chainedWorkspace(panes));
            constexpr std::size_t tabCountOffset = 8 + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 1;
            constexpr std::size_t tabOffset = tabCountOffset + 2;
            std::vector<std::uint8_t> two(one.begin(), one.end());
            two[tabCountOffset] = 2;
            two.insert(two.end(), one.begin() + static_cast<std::ptrdiff_t>(tabOffset), one.end());
            return two;
        };
        CHECK(decodeWorkspaceStructure(twoTabsOf(32)).has_value());
        CHECK(!decodeWorkspaceStructure(twoTabsOf(33)).has_value());
    }
    // A body past the bound is refused before it is read.
    CHECK(!decodeWorkspaceStructure(std::vector<std::uint8_t>(maxWorkspaceStructureBytes + 1, 0)).has_value());

    // The head of a workspace with nothing in it: revision, state, flags,
    // id, an empty name, label and tag list, no windows, none in front.
    const std::vector<std::uint8_t> empty = encoded(TreeWorkspace {});
    REQUIRE(empty.size() == 8 + 1 + 1 + 4 + 1 + 1 + 1 + 1 + 1);
    REQUIRE(decodeWorkspaceStructure(empty).has_value());
    const auto bent = [&](std::size_t offset, std::uint8_t value) {
        std::vector<std::uint8_t> changed = empty;
        changed[offset] = value;
        return decodeWorkspaceStructure(changed).has_value();
    };
    // A state past the last, a flag no flag names, a window in front of none.
    CHECK(!bent(8, 3));
    CHECK(bent(8, 2));
    CHECK(!bent(9, 0x02));
    CHECK(bent(9, 0x01));
    CHECK(!bent(empty.size() - 1, 1));
    // More tags than an object holds, before any is read.
    CHECK(!bent(empty.size() - 3, static_cast<std::uint8_t>(Identity::maxTagsPerObject + 1)));
}

TEST(aTamperedStructureIsRefusedByTheDecoder)
{
    // One window, one tab, two panes side by side, built by hand so each
    // field sits where the test bends it.
    struct Tab {
        std::uint8_t windowFlags = 0;
        std::uint64_t activeTab = 0;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> zoom {};
        std::uint64_t panes = 2;
        std::uint64_t focused = 0;
        std::uint8_t rootKind = 1;
        std::uint8_t axis = 0;
        double ratio = 0.5;
        std::uint64_t children = 2;
        std::uint8_t secondKind = 0;
        std::uint8_t attendeeKind = 1;
        std::uint64_t leaves = 2;
        std::uint8_t state = 0;
    };
    const auto bodyOf = [](const Tab& tab) {
        std::vector<std::uint8_t> out;
        WireWriter writer(out);
        writer.u64(7);
        writer.u8(tab.state);
        writer.u8(0);
        writer.u32(1);
        writer.varint(0);
        writer.varint(0);
        writer.varint(0);
        writer.varint(1);
        writer.varint(0);
        // The window.
        writer.u32(10);
        writer.varint(0);
        writer.varint(0);
        writer.u8(tab.windowFlags);
        writer.varint(1);
        writer.varint(tab.activeTab);
        if (tab.zoom) {
            writer.varint(tab.zoom->first);
            writer.varint(tab.zoom->second);
        }
        // The tab.
        writer.u32(20);
        writer.varint(0);
        writer.varint(0);
        writer.varint(0);
        writer.varint(tab.panes);
        writer.varint(tab.focused);
        const auto leaf = [&](std::uint8_t kind, std::uint32_t id) {
            writer.u8(kind);
            writer.u64(id);
            writer.u64(0);
            writer.u32(id);
            writer.u32(0);
            for (int cell = 0; cell < 4; ++cell)
                writer.u16(1);
            writer.u64(0);
            // No label, no tags, no session label and none of its tags.
            writer.varint(0);
            writer.varint(0);
            writer.varint(0);
            writer.varint(0);
            writer.varint(1);
            writer.u32(5);
            writer.u8(tab.attendeeKind);
            if (tab.state == 2) {
                writer.varint(0);
                writer.varint(0);
                writer.varint(0);
            }
        };
        writer.u8(tab.rootKind);
        writer.u8(tab.axis);
        std::uint64_t ratio = 0;
        std::memcpy(&ratio, &tab.ratio, sizeof(ratio));
        writer.u64(ratio);
        for (int cell = 0; cell < 4; ++cell)
            writer.u16(2);
        writer.varint(tab.children);
        for (std::uint64_t index = 0; index < tab.leaves; ++index)
            leaf(index == 1 ? tab.secondKind : 0, static_cast<std::uint32_t>(index + 1));
        return out;
    };
    const auto reads = [&](const Tab& tab) { return decodeWorkspaceStructure(bodyOf(tab)).has_value(); };
    REQUIRE(reads(Tab {}));
    const std::optional<TreeWorkspace> plain = decodeWorkspaceStructure(bodyOf(Tab {}));
    CHECK(plain->revision == 7 && plain->windows[0].tabs[0].panes.size() == 2 && plain->windows[0].tabs[0].panes[1].id == Layout::PaneID { 2 });
    CHECK(plain->windows[0].tabs[0].isHostActive && plain->windows[0].isHostActive);
    CHECK(reads(Tab { .state = 2 }));
    // An index naming no tab or pane.
    CHECK(!reads(Tab { .activeTab = 1 }));
    CHECK(!reads(Tab { .focused = 2 }));
    CHECK(reads(Tab { .focused = 1 }));
    // A zoom that names something, and ones that name nothing.
    CHECK(reads(Tab { .windowFlags = 1, .zoom = std::pair<std::uint64_t, std::uint64_t> { 0, 1 } }));
    CHECK(!reads(Tab { .windowFlags = 1, .zoom = std::pair<std::uint64_t, std::uint64_t> { 1, 0 } }));
    CHECK(!reads(Tab { .windowFlags = 1, .zoom = std::pair<std::uint64_t, std::uint64_t> { 0, 2 } }));
    CHECK(!reads(Tab { .windowFlags = 2 }));
    // A tab that says more panes, or fewer, than its split has leaves.
    CHECK(!reads(Tab { .panes = 3 }));
    CHECK(!reads(Tab { .panes = 1 }));
    CHECK(!reads(Tab { .panes = 0 }));
    CHECK(!reads(Tab { .panes = static_cast<std::uint64_t>(Layout::maxPanesPerTab) + 1 }));
    // A node of no kind, an axis past the last, a ratio that is no share, a
    // split of one, an attendee of no kind.
    CHECK(!reads(Tab { .rootKind = 2 }));
    CHECK(!reads(Tab { .secondKind = 2 }));
    CHECK(!reads(Tab { .axis = 2 }));
    CHECK(reads(Tab { .axis = 1 }));
    CHECK(!reads(Tab { .ratio = 1.5 }));
    CHECK(!reads(Tab { .ratio = std::numeric_limits<double>::quiet_NaN() }));
    CHECK(!reads(Tab { .children = 1, .leaves = 1 }));
    CHECK(!reads(Tab { .attendeeKind = 2 }));
    CHECK(!reads(Tab { .state = 3 }));
}

UBIQUITTY_TEST_MAIN()
