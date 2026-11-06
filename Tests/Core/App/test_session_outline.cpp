// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// test_session_outline.cpp — the rows an outline draws for a host, from the
// same kind of hand-made hosts test_session_tree builds its trees from: a
// workspace of one pane, a window of several tabs with a split, a window of
// one tab, labels at every level, a host a tag rule matches. Each case states
// the rows, in order, and what each says.
//

#include "Harness.h"

#include "Core/App/SessionOutline.h"
#include "Core/App/SessionOutlineText.h"
#include "Core/App/StatusRollup.h"
#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Session/AttachTarget.h"
#include "Core/Session/Identity.h"
#include "Core/Session/LayoutTree.h"
#include "Core/Session/RememberedSessions.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/StatedPane.h"
#include "Core/Themes/StatusColors.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Ubiquitty::App::buildSessionOutline;
using Ubiquitty::App::ListedLevel;
using Ubiquitty::App::OutlineAge;
using Ubiquitty::App::OutlineConfiguration;
using Ubiquitty::App::OutlineRow;
using Ubiquitty::App::OutlineSummary;
using Ubiquitty::App::SessionOutline;
using Ubiquitty::App::StatusKind;
using Ubiquitty::Panes::Orientation;
using Ubiquitty::Session::buildSessionTree;
using Ubiquitty::Session::PaneActivity;
using Ubiquitty::Session::SessionTree;
using Ubiquitty::Session::TreeAttendance;
using Ubiquitty::Session::TreeAttendee;
using Ubiquitty::Session::TreeAttendeeKind;
using Ubiquitty::Session::TreePanePath;
using Ubiquitty::Session::TreeWorkspaceSource;
using Ubiquitty::Session::Identity::Allocator;
using Ubiquitty::Session::Identity::LabelBook;
using Ubiquitty::Session::Identity::ObjectID;
using Ubiquitty::Session::Identity::ObjectKind;
using Ubiquitty::Themes::StatusColorSlot;
namespace Layout = Ubiquitty::Session::Layout;
namespace OSC = Ubiquitty::OSC;

namespace {

/// What a test says about a pane; the tree is built from the records a host
/// would send for it (StatedPane.h).
using TreePaneFacts = UbiquittyTest::StatedPane;

TreePaneFacts pane(std::string programTitle, std::string program, std::string directory)
{
    TreePaneFacts facts;
    facts.title.programTitle = std::move(programTitle);
    facts.title.currentDirectory = std::move(directory);
    facts.title.columns = 80;
    facts.title.rows = 24;
    facts.program = std::move(program);
    facts.status.isAtPrompt = true;
    return facts;
}

/// A host of two workspaces, and the ids of everything in it.
///
///   workspace 2 "early"             one window, one tab, one pane (p1)
///   workspace 9 "late", label nine
///     window w1, label "ops"
///       tab t1 "build", label tb1   a | b, typing into b
///       tab t2                      one pane c, labelled "editor"
///     window w2                     one tab t3, label "solo": d | e, typing into e
///
/// Drawn, with every level of one child folded into the row above it:
///
///   early          the workspace, its window, its tab and p1
///   late
///     ops          w1
///       build      t1
///         a
///         b
///       editor     t2 and c
///     deploy       w2 and t3
///       d
///       e
struct Host {
    std::shared_ptr<Allocator> identities = std::make_shared<Allocator>();
    std::unique_ptr<Layout::Workspace> early = std::make_unique<Layout::Workspace>(identities);
    std::unique_ptr<Layout::Workspace> late = std::make_unique<Layout::Workspace>(identities);
    LabelBook labels;
    Ubiquitty::Session::Identity::TagBook tags;
    std::map<std::uint32_t, TreePaneFacts> facts;
    std::uint64_t hostNow = 100'000;
    Layout::SurfaceID earlyWindow {}, w1 {}, w2 {};
    Layout::TabID earlyTab {}, t1 {}, t2 {}, t3 {};
    Layout::PaneID p1 {}, a {}, b {}, c {}, d {}, e {};

    Host()
    {
        earlyWindow = early->surfaceIDAt(0);
        earlyTab = early->tabAt(earlyWindow, 0);
        p1 = early->focusedPane(earlyWindow);
        w1 = late->surfaceIDAt(0);
        t1 = late->tabAt(w1, 0);
        a = late->focusedPane(w1);
        b = late->splitFocusedPane(w1, Orientation::Vertical);
        t2 = late->addTab(w1);
        c = late->focusedPane(w1);
        w2 = late->addSurface();
        t3 = late->tabAt(w2, 0);
        d = late->focusedPane(w2);
        e = late->splitFocusedPane(w2, Orientation::Horizontal);
        (void)late->renameTab(w1, t1, "build");
        (void)late->setActiveTab(w1, t1);
        (void)late->setActiveSurface(w1);

        (void)labels.set({ ObjectKind::Workspace, 9 }, "nine");
        (void)labels.set({ ObjectKind::Window, std::to_underlying(w1) }, "ops");
        (void)labels.set({ ObjectKind::Tab, std::to_underlying(t1) }, "tb1");
        (void)labels.set({ ObjectKind::Tab, std::to_underlying(t3) }, "solo");
        (void)labels.set({ ObjectKind::Pane, std::to_underlying(c) }, "editor");
        (void)labels.set({ ObjectKind::Session, 77 }, "longrun");

        at(p1) = pane("zsh", "zsh", "/home/user");
        at(p1).activity = { 40'000, 10'000 };
        at(a) = pane("make all", "make", "/srv/app");
        at(a).title.hostName = "prod-1";
        at(a).status.programState = OSC::ProgramState::Blocked;
        at(a).activity = { 99'500, 0 };
        at(a).attendees = { { 1, TreeAttendeeKind::Person } };
        at(b) = pane("tests", "ctest", "/srv/app/build");
        at(b).status.programState = OSC::ProgramState::Done;
        at(b).status.hasUnseenNotification = true;
        at(b).badgeText = "x";
        at(b).activity = { 70'000, 95'000 };
        at(b).attendees = { { 1, TreeAttendeeKind::Person }, { 2, TreeAttendeeKind::Agent } };
        at(c) = pane("vim notes", "vim", "/home/user/notes");
        at(c).status.programState = OSC::ProgramState::Working;
        at(c).status.progress = { OSC::ProgressNormal, 40 };
        at(c).status.tabColor = Ubiquitty::ColorRGBA { 1, 2, 3, 255 };
        at(d) = pane("top", "top", "/");
        at(d).title.hostName = "PROD-2";
        at(d).sessionID = 77;
        at(e) = pane("deploy", "ansible", "/srv/deploy");
        at(e).status.programState = OSC::ProgramState::Error;
        at(e).activity = { 200'000, 0 };
    }

    TreePaneFacts& at(Layout::PaneID id) { return facts[std::to_underlying(id)]; }

    SessionTree tree()
    {
        const std::array sources { TreeWorkspaceSource { 9, "late", late.get() }, TreeWorkspaceSource { 2, "early", early.get() } };
        return buildSessionTree({ .revision = 31, .nowUnixMilliseconds = hostNow, .workspaces = sources, .tags = &tags }, labels, [this](std::uint32_t, Layout::PaneID id) { return facts[std::to_underlying(id)].treeFacts(); });
    }
};

Ubiquitty::Preferences::Effective::TitleComposition programTitleOnly()
{
    return {
        .showsDirectory = false,
        .showsDirectoryFullPath = false,
        .showsAppTitle = true,
        .showsProcess = false,
        .showsProcessArguments = false,
        .showsShellCommand = false,
        .showsTTYName = false,
        .showsProfileName = false,
        .showsDimensions = false,
        .showsProgramStatus = false,
        .baseTitle = {},
        .directoryHiddenFor = {},
        .hidesDirectoryInFullScreen = false,
    };
}

const std::vector<Ubiquitty::Preferences::TagEntry> productionRule {
    { .name = "production", .color = "red", .matchHost = "prod-*", .matchProfile = "", .prominence = "loud" },
    { .name = "anywhere", .color = "blue", .matchHost = "", .matchProfile = "", .prominence = "" },
};

OutlineConfiguration configuration()
{
    OutlineConfiguration made;
    made.tabTitle = programTitleOnly();
    made.paneTitle = programTitleOnly();
    made.viewer.fallbackName = "shell";
    made.tagRules = &productionRule;
    for (std::size_t slot = 0; slot < Ubiquitty::Themes::statusColorSlotCount; ++slot)
        made.statusColors.slots[slot] = { static_cast<std::uint8_t>(10 * (slot + 1)), 0, 0, 255 };
    return made;
}

/// Each row as `depth:kind:number`, the outline's shape at a glance.
std::vector<std::string> shape(const SessionOutline& outline)
{
    std::vector<std::string> rows;
    for (const OutlineRow& row : outline.rows)
        rows.push_back(std::to_string(row.depth) + ":" + Ubiquitty::Session::Identity::format(row.id));
    return rows;
}

std::string id(ObjectKind kind, std::uint32_t number) { return Ubiquitty::Session::Identity::format({ kind, number }); }

template<typename ID>
std::string id(ObjectKind kind, ID number) { return id(kind, std::to_underlying(number)); }

const OutlineRow* find(const SessionOutline& outline, ObjectID wanted)
{
    for (const OutlineRow& row : outline.rows) {
        if (row.id == wanted)
            return &row;
    }
    return nullptr;
}

std::vector<std::string> tagNames(const OutlineRow& row)
{
    std::vector<std::string> names;
    for (const auto& tag : row.tags)
        names.push_back(tag.name);
    return names;
}

/// The rows of `Host`, in the order they are drawn.
enum Row : std::size_t {
    Early,
    Late,
    W1,
    T1,
    A,
    B,
    T2,
    W2,
    D,
    E,
    RowCount,
};

std::vector<std::string> chips(std::initializer_list<const char*> list) { return { list.begin(), list.end() }; }

/// A parent's row as it is drawn closed, which is where it states the whole
/// roll-up of what is under it.
OutlineRow closedRow(const SessionTree& tree, OutlineConfiguration made, ObjectID parent)
{
    const std::array closed { parent };
    made.collapsed = closed;
    const SessionOutline outline = buildSessionOutline(tree, made);
    const OutlineRow* row = find(outline, parent);
    return row ? *row : OutlineRow {};
}

/// A workspace of one window whose tabs hold the given numbers of panes, and
/// the tree built from it with every pane a shell titled by its number.
struct Small {
    std::unique_ptr<Layout::Workspace> layout = std::make_unique<Layout::Workspace>();
    LabelBook labels;
    std::vector<Layout::TabID> tabs;
    std::vector<std::vector<Layout::PaneID>> panes;
    Layout::SurfaceID window {};
    std::string name = "work";

    explicit Small(std::initializer_list<int> panesPerTab)
    {
        window = layout->surfaceIDAt(0);
        bool isFirst = true;
        for (const int count : panesPerTab) {
            tabs.push_back(isFirst ? layout->tabAt(window, 0) : layout->addTab(window));
            isFirst = false;
            panes.emplace_back();
            panes.back().push_back(layout->focusedPane(window));
            for (int index = 1; index < count; ++index)
                panes.back().push_back(layout->splitFocusedPane(window, Orientation::Vertical));
        }
    }

    SessionTree tree()
    {
        const std::array sources { TreeWorkspaceSource { 3, name, layout.get() } };
        return buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 0, .workspaces = sources }, labels, [](std::uint32_t, Layout::PaneID id) {
            return pane("shell " + std::to_string(std::to_underlying(id)), "dash-prog", "/");
        });
    }
};

} // namespace

TEST(theRowsAreTheTreeInOrderWithEveryLevelOfOneChildFolded)
{
    Host host;
    const SessionTree tree = host.tree();
    const SessionOutline outline = buildSessionOutline(tree, configuration());

    CHECK_EQ(outline.revision, 31u);
    CHECK_EQ(outline.matchCount, 0u);
    const std::vector<std::string> expected {
        "0:" + id(ObjectKind::Workspace, 2u),
        "0:" + id(ObjectKind::Workspace, 9u),
        "1:" + id(ObjectKind::Window, host.w1),
        "2:" + id(ObjectKind::Tab, host.t1),
        "3:" + id(ObjectKind::Pane, host.a),
        "3:" + id(ObjectKind::Pane, host.b),
        "2:" + id(ObjectKind::Tab, host.t2),
        "1:" + id(ObjectKind::Window, host.w2),
        "2:" + id(ObjectKind::Pane, host.d),
        "2:" + id(ObjectKind::Pane, host.e),
    };
    CHECK(shape(outline) == expected);
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    if (outline.rows.size() != RowCount)
        return;

    const std::array levels { ListedLevel::Workspace, ListedLevel::Workspace, ListedLevel::Window, ListedLevel::Tab, ListedLevel::Pane, ListedLevel::Pane, ListedLevel::Tab, ListedLevel::Window,
        ListedLevel::Pane, ListedLevel::Pane };
    for (std::size_t index = 0; index < levels.size(); ++index)
        CHECK(outline.rows[index].level == levels[index]);

    // Where the innermost thing each row stands for is in the tree.
    CHECK(outline.rows[Early].path == (TreePanePath { 0, 0, 0, 0 }));
    CHECK(outline.rows[Late].path == (TreePanePath { 1, 0, 0, 0 }));
    CHECK(outline.rows[W1].path == (TreePanePath { 1, 0, 0, 0 }));
    CHECK(outline.rows[B].path == (TreePanePath { 1, 0, 0, 1 }));
    CHECK(outline.rows[T2].path == (TreePanePath { 1, 0, 1, 0 }));
    CHECK(outline.rows[W2].path == (TreePanePath { 1, 1, 0, 0 }));
    CHECK(outline.rows[E].path == (TreePanePath { 1, 1, 0, 1 }));
    CHECK(&tree.pane(outline.rows[E].path) == &tree.workspaces[1].windows[1].tabs[0].panes[1]);

    // What each row stands for besides itself.
    const OutlineRow& early = outline.rows[Early];
    CHECK(early.foldedWindow == (ObjectID { ObjectKind::Window, std::to_underlying(host.earlyWindow) }));
    CHECK(early.foldedTab == (ObjectID { ObjectKind::Tab, std::to_underlying(host.earlyTab) }));
    CHECK(early.foldedPane == (ObjectID { ObjectKind::Pane, std::to_underlying(host.p1) }));
    CHECK_MSG(!outline.rows[Late].foldedWindow && !outline.rows[Late].foldedTab && !outline.rows[Late].foldedPane, "a workspace of two windows folds nothing");
    CHECK(!outline.rows[W1].foldedWindow && !outline.rows[W1].foldedTab && !outline.rows[W1].foldedPane);
    CHECK(!outline.rows[T1].foldedPane && !outline.rows[T1].foldedTab);
    CHECK_MSG(outline.rows[T2].foldedPane == (ObjectID { ObjectKind::Pane, std::to_underlying(host.c) }) && !outline.rows[T2].foldedTab, "a tab of one pane stands for the pane");
    CHECK_MSG(outline.rows[W2].foldedTab == (ObjectID { ObjectKind::Tab, std::to_underlying(host.t3) }) && !outline.rows[W2].foldedPane && !outline.rows[W2].foldedWindow, "a window of one tab of two panes stands for the tab only");
    CHECK(!outline.rows[A].foldedPane && !outline.rows[A].foldedTab && !outline.rows[A].foldedWindow);

    // Everything is open, and a row with nothing under it has nothing to open.
    for (std::size_t index = 0; index < RowCount; ++index) {
        const OutlineRow& row = outline.rows[index];
        const bool hasRowsUnder = index == Late || index == W1 || index == T1 || index == W2;
        CHECK(row.isExpandable == hasRowsUnder);
        CHECK(row.isExpanded == row.isExpandable);
        CHECK(!row.collapsedSummary && !row.isMatch);
    }
}

TEST(everyLevelOfOneChildFoldsIntoTheRowAboveIt)
{
    // One window, one tab, one pane: one row, which is all four.
    {
        Small host { 1 };
        const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
        CHECK_EQ(outline.rows.size(), 1u);
        if (outline.rows.size() != 1)
            return;
        const OutlineRow& row = outline.rows[0];
        CHECK(row.level == ListedLevel::Workspace && row.depth == 0 && row.id == (ObjectID { ObjectKind::Workspace, 3 }));
        CHECK(row.foldedWindow && row.foldedTab && row.foldedPane);
        CHECK(row.foldedPane == (ObjectID { ObjectKind::Pane, std::to_underlying(host.panes[0][0]) }));
        CHECK_MSG(!row.isExpandable && !row.isExpanded && !row.collapsedSummary, "one row has nothing to open");
        CHECK_MSG(row.program == "dash-prog" && row.directory == "/" && row.columns == 80 && row.rows == 24, "and says what its one pane runs");
    }
    // One window, one tab, two panes: the panes sit under the workspace.
    {
        Small host { 2 };
        const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
        CHECK(shape(outline) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 3u), "1:" + id(ObjectKind::Pane, host.panes[0][0]), "1:" + id(ObjectKind::Pane, host.panes[0][1]) }));
        if (outline.rows.size() != 3)
            return;
        CHECK(outline.rows[0].foldedWindow && outline.rows[0].foldedTab && !outline.rows[0].foldedPane);
        CHECK(outline.rows[0].isExpandable && outline.rows[0].program.empty() && outline.rows[0].columns == 0);
        CHECK(outline.rows[1].level == ListedLevel::Pane && !outline.rows[1].isExpandable);
    }
    // One window of two tabs, one of one pane and one of two: the tabs sit
    // under the workspace, and only the second has rows under it.
    {
        Small host { 1, 2 };
        const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
        CHECK(shape(outline) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 3u), "1:" + id(ObjectKind::Tab, host.tabs[0]), "1:" + id(ObjectKind::Tab, host.tabs[1]), "2:" + id(ObjectKind::Pane, host.panes[1][0]), "2:" + id(ObjectKind::Pane, host.panes[1][1]) }));
        if (outline.rows.size() != 5)
            return;
        CHECK(outline.rows[0].foldedWindow && !outline.rows[0].foldedTab && !outline.rows[0].foldedPane);
        CHECK(outline.rows[1].foldedPane == (ObjectID { ObjectKind::Pane, std::to_underlying(host.panes[0][0]) }) && !outline.rows[1].isExpandable);
        CHECK(!outline.rows[2].foldedPane && outline.rows[2].isExpandable);
        CHECK_MSG(outline.rows[1].program == "dash-prog" && outline.rows[2].program.empty(), "a row that stands for one pane says what it runs; a row above two does not");
    }
    // Two windows: a window of one tab of one pane is one row, and a window of
    // one tab of two panes has them directly under it.
    {
        Small host { 1 };
        const Layout::SurfaceID second = host.layout->addSurface();
        const Layout::PaneID first = host.layout->focusedPane(second);
        const Layout::PaneID split = host.layout->splitFocusedPane(second, Orientation::Vertical);
        const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
        CHECK(shape(outline) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 3u), "1:" + id(ObjectKind::Window, host.window), "1:" + id(ObjectKind::Window, second), "2:" + id(ObjectKind::Pane, first), "2:" + id(ObjectKind::Pane, split) }));
        if (outline.rows.size() != 5)
            return;
        CHECK(!outline.rows[0].foldedWindow && outline.rows[0].isExpandable);
        CHECK(outline.rows[1].foldedTab && outline.rows[1].foldedPane && !outline.rows[1].isExpandable);
        CHECK(outline.rows[2].foldedTab && !outline.rows[2].foldedPane && outline.rows[2].isExpandable);
        // The window that is one pane is found by that pane's program, and the
        // window above two panes is not.
        OutlineConfiguration searching = configuration();
        searching.query = "dash-prog";
        const SessionOutline found = buildSessionOutline(host.tree(), searching);
        CHECK(shape(found) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 3u), "1:" + id(ObjectKind::Window, host.window), "1:" + id(ObjectKind::Window, second), "2:" + id(ObjectKind::Pane, first), "2:" + id(ObjectKind::Pane, split) }));
        CHECK(found.matchCount == 3 && found.rows[1].isMatch && !found.rows[2].isMatch && !found.rows[0].isMatch);
    }
}

TEST(aFoldedRowTakesTheInnermostTitleAndKeepsEveryLabel)
{
    Small host { 1 };
    const ObjectID workspace { ObjectKind::Workspace, 3 };
    const ObjectID window { ObjectKind::Window, std::to_underlying(host.window) };
    const ObjectID tab { ObjectKind::Tab, std::to_underlying(host.tabs[0]) };
    const ObjectID onlyPane { ObjectKind::Pane, std::to_underlying(host.panes[0][0]) };
    const std::string composed = "shell " + std::to_string(onlyPane.number);
    const auto row = [&] { return buildSessionOutline(host.tree(), configuration()).rows.at(0); };

    // Every level labelled and the workspace named: the name, and four chips.
    (void)host.labels.set(workspace, "wsl");
    (void)host.labels.set(window, "winl");
    (void)host.labels.set(tab, "tabl");
    (void)host.labels.set(onlyPane, "panel");
    (void)host.layout->renameTab(host.window, host.tabs[0], "tabname");
    CHECK_STR_EQ(row().title, "work");
    CHECK(row().labelChips == chips({ "wsl", "winl", "tabl", "panel" }));

    // No workspace name: the window's label is what the window is called, so
    // it is the title and no longer a chip.
    host.name.clear();
    CHECK_STR_EQ(row().title, "winl");
    CHECK(row().labelChips == chips({ "wsl", "tabl", "panel" }));
    // No window label: the tab's name.
    (void)host.labels.set(window, "");
    CHECK_STR_EQ(row().title, "tabname");
    CHECK(row().labelChips == chips({ "wsl", "tabl", "panel" }));
    // No tab name: the pane's label, which is what the pane is called.
    (void)host.layout->renameTab(host.window, host.tabs[0], "");
    CHECK_STR_EQ(row().title, "panel");
    CHECK_MSG(row().labelChips == chips({ "wsl", "tabl" }), "a tab's label is never a title");
    // No pane label: the folder the pane is in, here the root, rather than
    // what its program says, which stays the secondary text.
    (void)host.labels.set(onlyPane, "");
    CHECK_STR_EQ(row().title, "/");
    CHECK_STR_EQ(row().secondaryTitle, composed);
    CHECK(row().labelChips == chips({ "wsl", "tabl" }));
    // Nothing labelled at all.
    (void)host.labels.set(workspace, "");
    (void)host.labels.set(tab, "");
    CHECK_STR_EQ(row().title, "/");
    CHECK(row().labelChips.empty());

    // The same label at two levels is one chip.
    (void)host.labels.set(workspace, "same");
    (void)host.labels.set(tab, "same");
    host.name = "work";
    CHECK(row().labelChips == chips({ "same" }));

    // A tab of two panes is not called by a pane's label: its panes have rows.
    Small split { 2 };
    (void)split.labels.set({ ObjectKind::Pane, std::to_underlying(split.panes[0][1]) }, "panel");
    split.name.clear();
    const SessionOutline outline = buildSessionOutline(split.tree(), configuration());
    // The unnamed workspace standing for that tab is called by its folder.
    CHECK_MSG(outline.rows.size() == 3 && outline.rows[0].title == "/" && outline.rows[0].labelChips.empty(), "the workspace is called by its folder, not by a pane's label");
    CHECK(outline.rows[2].title == "panel" && outline.rows[2].labelChips.empty());
}

TEST(aRowIsCalledByItsNameAndWearsItsLabelAsAChip)
{
    Host host;
    const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    if (outline.rows.size() != RowCount)
        return;

    // A workspace: its name, and its label beside it.
    CHECK_STR_EQ(outline.rows[Early].title, "early");
    CHECK(outline.rows[Early].labelChips.empty());
    CHECK_STR_EQ(outline.rows[Late].title, "late");
    CHECK(outline.rows[Late].labelChips == chips({ "nine" }));
    // A labelled window: the label is its title, and is not said twice.
    CHECK_STR_EQ(outline.rows[W1].title, "ops");
    CHECK(outline.rows[W1].labelChips.empty());
    // A window of one tab: that tab's title, composed from the pane it types
    // into, and the tab's label, since the row is the tab's too.
    CHECK_STR_EQ(outline.rows[W2].title, "deploy");
    CHECK(outline.rows[W2].labelChips == chips({ "solo" }));
    // A tab: the name a person gave it; the label is a chip.
    CHECK_STR_EQ(outline.rows[T1].title, "build");
    CHECK(outline.rows[T1].labelChips == chips({ "tb1" }));
    // A tab of one labelled pane: the pane's label.
    CHECK_STR_EQ(outline.rows[T2].title, "editor");
    CHECK(outline.rows[T2].labelChips.empty());
    // A pane: its composed title.
    CHECK_STR_EQ(outline.rows[A].title, "make all");
    CHECK_STR_EQ(outline.rows[B].title, "tests");
    CHECK(outline.rows[A].labelChips.empty());

    // An unnamed workspace is called by the word a person put on what it has
    // in front: its window's label.
    Host unnamed;
    const std::array sources { TreeWorkspaceSource { 9, "", unnamed.late.get() } };
    const auto unnamedOutline = [&](std::uint64_t revision) {
        const SessionTree tree = buildSessionTree({ .revision = revision, .nowUnixMilliseconds = 100'000, .workspaces = sources }, unnamed.labels, [&](std::uint32_t, Layout::PaneID which) { return unnamed.at(which); });
        return buildSessionOutline(tree, configuration());
    };
    const SessionOutline other = unnamedOutline(1);
    CHECK_STR_EQ(other.rows[0].title, "ops");
    CHECK(other.rows[0].labelChips == chips({ "nine" }));

    // An unlabelled window of several tabs is called what the tab it shows
    // is, and so is the workspace: here the label of that tab's one pane.
    (void)unnamed.labels.set({ ObjectKind::Window, std::to_underlying(unnamed.w1) }, "");
    (void)unnamed.late->setActiveTab(unnamed.w1, unnamed.t2);
    const SessionOutline byActiveTab = unnamedOutline(2);
    CHECK_STR_EQ(byActiveTab.rows[1].title, "editor");
    CHECK_MSG(byActiveTab.rows[0].title == "editor", "and its unnamed workspace after it");

    // With nothing in front named, the folder of the pane in front, not the
    // composed title: /home/user/notes is "notes".
    (void)unnamed.labels.set({ ObjectKind::Pane, std::to_underlying(unnamed.c) }, "");
    const SessionOutline byFolder = unnamedOutline(3);
    CHECK_STR_EQ(byFolder.rows[0].title, "notes");
    CHECK_STR_EQ(byFolder.rows[1].title, "vim notes");

    // A pane that reports no directory leaves the composed title.
    unnamed.at(unnamed.c).title.currentDirectory.clear();
    CHECK_STR_EQ(unnamedOutline(4).rows[0].title, "vim notes");

    // The reader's composition decides a composed title; a name is a name.
    OutlineConfiguration silent = configuration();
    silent.tabTitle.showsAppTitle = false;
    silent.paneTitle.showsAppTitle = false;
    const SessionOutline fallback = buildSessionOutline(host.tree(), silent);
    CHECK_MSG(fallback.rows[W2].title == "shell" && fallback.rows[A].title == "shell", "with no part showing, the reader's fallback names the row");
    CHECK_STR_EQ(fallback.rows[T1].title, "build");
    CHECK_STR_EQ(fallback.rows[T2].title, "editor");
    silent.tabTitle.showsAppTitle = true;
    const SessionOutline tabsOnly = buildSessionOutline(host.tree(), silent);
    CHECK_MSG(tabsOnly.rows[W2].title == "deploy" && tabsOnly.rows[A].title == "shell", "a tab's title and a pane's row each read their own composition");
}

TEST(aNameOrALabelNeverHidesWhatIsRunning)
{
    Host host;
    (void)host.labels.set({ ObjectKind::Pane, std::to_underlying(host.a) }, "builder");
    const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    if (outline.rows.size() != RowCount)
        return;

    // A labelled pane: the label, and what its program says.
    CHECK_STR_EQ(outline.rows[A].title, "builder");
    CHECK_STR_EQ(outline.rows[A].secondaryTitle, "make all");
    // An unlabelled pane's title is already what its program says.
    CHECK_STR_EQ(outline.rows[B].title, "tests");
    CHECK_STR_EQ(outline.rows[B].secondaryTitle, "");
    // A labelled window: what the tab it shows is running.
    // What a parent borrows from below it says closed; open, the row that
    // runs it is drawn beneath and says it.
    const auto closed = [&](Row row) { return closedRow(host.tree(), configuration(), outline.rows[row].id); };
    CHECK_STR_EQ(outline.rows[W1].title, "ops");
    CHECK_STR_EQ(closed(W1).secondaryTitle, "tests");
    CHECK_STR_EQ(outline.rows[W1].secondaryTitle, "");
    // An unlabelled window is already called by it.
    CHECK_STR_EQ(outline.rows[W2].title, "deploy");
    CHECK_STR_EQ(outline.rows[W2].secondaryTitle, "");
    // A named tab: what its focused pane is running.
    CHECK_STR_EQ(outline.rows[T1].title, "build");
    CHECK_STR_EQ(closed(T1).secondaryTitle, "tests");
    CHECK_STR_EQ(outline.rows[T1].secondaryTitle, "");
    // A tab called by its one pane's label.
    CHECK_STR_EQ(outline.rows[T2].title, "editor");
    CHECK_STR_EQ(outline.rows[T2].secondaryTitle, "vim notes");
    // A named workspace: its front window's shown tab; and one that is one
    // row, its one pane.
    CHECK_STR_EQ(closed(Late).secondaryTitle, "tests");
    CHECK_STR_EQ(outline.rows[Late].secondaryTitle, "");
    CHECK_STR_EQ(outline.rows[Early].title, "early");
    CHECK_STR_EQ(outline.rows[Early].secondaryTitle, "zsh");

    // The window's secondary text follows the tab it shows.
    (void)host.late->setActiveTab(host.w1, host.t2);
    CHECK_STR_EQ(closed(W1).secondaryTitle, "vim notes");
    CHECK_STR_EQ(closed(Late).secondaryTitle, "vim notes");
    (void)host.late->setActiveSurface(host.w2);
    CHECK_STR_EQ(closed(Late).secondaryTitle, "deploy");

    // Each level reads its own composition.
    OutlineConfiguration made = configuration();
    made.paneTitle.showsAppTitle = false;
    const SessionOutline panesSilent = buildSessionOutline(host.tree(), made);
    CHECK_MSG(panesSilent.rows[A].secondaryTitle == "shell" && panesSilent.rows[T2].secondaryTitle == "vim notes", "a pane's by the pane composition, a tab's by the tab's");

    // A row hidden behind a label is found by what it runs.
    made = configuration();
    made.query = "all";
    const SessionOutline found = buildSessionOutline(host.tree(), made);
    CHECK(found.matchCount == 1 && found.rows.back().id == (ObjectID { ObjectKind::Pane, std::to_underlying(host.a) }));
}

TEST(aRowShowsItsNodesRollUpAndAPaneItsOwnFacts)
{
    Host host;
    const OutlineConfiguration made = configuration();
    const SessionOutline outline = buildSessionOutline(host.tree(), made);
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    if (outline.rows.size() != RowCount)
        return;
    const auto slotColor = [&](StatusColorSlot slot) { return made.statusColors.color(slot).red; };

    // Panes.
    const OutlineRow& a = outline.rows[A];
    CHECK(a.status.kind == StatusKind::Blocked && a.statusSlot == StatusColorSlot::Blocked && a.statusColor.red == slotColor(StatusColorSlot::Blocked));
    CHECK_STR_EQ(a.program, "make");
    CHECK_STR_EQ(a.directory, "/srv/app");
    CHECK(a.columns == 80 && a.rows == 24);
    CHECK(a.attendance == (TreeAttendance { 1, 0 }));
    const OutlineRow& b = outline.rows[B];
    CHECK_MSG(b.status.kind == StatusKind::Attention && b.statusSlot == StatusColorSlot::Attention, "an unseen notification outranks a finished job");
    CHECK(b.status.hasUnseenNotification && !b.status.hasUnseenBell);
    CHECK_STR_EQ(b.badge, "x");
    CHECK(b.status.badgeCount == 1);
    CHECK_MSG(b.isHostCurrent && !a.isHostCurrent, "the tab types into b");

    // A tab that stands for its one pane says what the pane does.
    const OutlineRow& t2 = outline.rows[T2];
    CHECK(t2.status.kind == StatusKind::Working && t2.statusSlot == StatusColorSlot::Working && t2.statusColor.red == slotColor(StatusColorSlot::Working));
    CHECK(t2.status.progress.state == OSC::ProgressNormal && t2.status.progress.value == 40);
    CHECK(t2.status.tabColor && t2.status.tabColor->blue == 3);
    CHECK_STR_EQ(t2.program, "vim");
    CHECK_STR_EQ(t2.directory, "/home/user/notes");
    CHECK_MSG(!t2.isHostCurrent, "and is current as a tab is: the window shows the other one");

    // A workspace that stands for its one pane.
    const OutlineRow& early = outline.rows[Early];
    CHECK(early.status.kind == StatusKind::Idle && early.statusSlot == StatusColorSlot::Neutral && early.statusColor.red == slotColor(StatusColorSlot::Neutral));
    CHECK_STR_EQ(early.program, "zsh");
    CHECK_STR_EQ(early.directory, "/home/user");
    CHECK(early.columns == 80 && early.rows == 24);
    CHECK(!early.isHostCurrent);

    // Times are ages by the host's clock, as the tree states it.
    CHECK(early.activity == (PaneActivity { 40'000, 10'000 }));
    CHECK(early.sinceOutput == (OutlineAge { true, 60 }) && early.sinceInput == (OutlineAge { true, 90 }));
    // One age for a place that shows one: the newer of the two, and unknown
    // only when neither is known.
    CHECK_MSG(early.sinceActivity == (OutlineAge { true, 60 }), "it wrote more recently than it was typed into");
    CHECK_MSG(outline.rows[B].sinceActivity == outline.rows[B].sinceInput && outline.rows[B].sinceInput.seconds < outline.rows[B].sinceOutput.seconds, "typed into more recently than it wrote");
    CHECK_MSG(a.sinceActivity == a.sinceOutput && !a.sinceInput.isKnown, "one known age is the newer of the two");
    CHECK(outline.rows[D].sinceActivity == OutlineAge {});
    CHECK_MSG(a.sinceOutput == (OutlineAge { true, 0 }), "half a second ago is no whole second ago");
    CHECK_MSG(a.sinceInput == OutlineAge {}, "never typed into is not zero seconds ago");
    CHECK_MSG(outline.rows[E].sinceOutput == (OutlineAge { true, 0 }), "an instant after the host described its state is not a time in the future");
    Host later;
    later.hostNow = 400'000;
    const SessionOutline aged = buildSessionOutline(later.tree(), made);
    CHECK_MSG(aged.rows[Early].sinceOutput == (OutlineAge { true, 360 }) && aged.rows[E].sinceOutput == (OutlineAge { true, 200 }), "the same instants are older in a tree the host described later");

    // Parents: the roll-up of what is under them.
    const OutlineRow& t1 = outline.rows[T1];
    CHECK(t1.status.kind == StatusKind::Blocked && t1.status.count == 1 && t1.status.paneCount == 2);
    CHECK_MSG(!t1.status.hasUnseenNotification && t1.status.badgeCount == 1, "open, the notification is pane b's to show; the count is the tab's");
    CHECK(closedRow(host.tree(), made, t1.id).status.hasUnseenNotification);
    CHECK(t1.statusSlot == StatusColorSlot::Blocked);
    CHECK(t1.activity == (PaneActivity { 99'500, 95'000 }));
    CHECK(t1.sinceInput == (OutlineAge { true, 5 }));
    CHECK_MSG(t1.attendance == (TreeAttendance { 1, 1 }), "the person on both panes is one person on the tab");
    CHECK(t1.badge.empty() && t1.program.empty() && t1.directory.empty() && t1.columns == 0 && t1.rows == 0);
    CHECK_MSG(t1.isHostCurrent, "the window shows the first tab");
    const OutlineRow& w1 = outline.rows[W1];
    CHECK(w1.status.kind == StatusKind::Blocked && w1.status.paneCount == 3 && w1.status.progress.state == OSC::ProgressNone);
    CHECK(closedRow(host.tree(), made, w1.id).status.progress.state == OSC::ProgressNormal);
    CHECK(w1.isHostCurrent && !outline.rows[W2].isHostCurrent);
    const OutlineRow& w2 = outline.rows[W2];
    CHECK(w2.status.kind == StatusKind::Failed && w2.statusSlot == StatusColorSlot::Failed && w2.statusColor.red == slotColor(StatusColorSlot::Failed));
    CHECK(w2.program.empty() && w2.badge.empty());
    const OutlineRow& late = outline.rows[Late];
    CHECK(late.status.kind == StatusKind::Failed && late.status.count == 1 && late.status.paneCount == 5);
    CHECK(late.attendance == (TreeAttendance { 1, 1 }));
    CHECK(late.activity == (PaneActivity { 200'000, 95'000 }));
    CHECK(!late.isHostCurrent);
}

TEST(aMarkIsDrawnInTheThemesColourUnlessOnlyThePlatformHasOne)
{
    using Ubiquitty::App::StatusMarkInk;
    Host host;
    // Pane e's progress report is in its error state, so its window's is.
    host.at(host.e).status.progress = { OSC::ProgressError, 0 };
    const auto slotColor = [](const OutlineConfiguration& made, StatusColorSlot slot) { return made.statusColors.color(slot).red; };
    const auto isThemes = [](const StatusMarkInk& ink, std::uint8_t red) { return !ink.isPlatformColor && ink.color.red == red; };

    // A reader with no colours of its own is handed the theme's for every
    // mark: the status kind's slot, the progress slot, and the attention
    // slot for a bell and for a notification.
    const OutlineConfiguration plain = configuration();
    const SessionOutline themed = buildSessionOutline(host.tree(), plain);
    REQUIRE(themed.rows.size() == RowCount);
    for (const OutlineRow& row : themed.rows) {
        CHECK(isThemes(row.statusInk, row.statusColor.red));
        CHECK(isThemes(row.bellInk, slotColor(plain, StatusColorSlot::Attention)));
        CHECK(isThemes(row.notificationInk, slotColor(plain, StatusColorSlot::Attention)));
    }
    CHECK(isThemes(themed.rows[T2].progressInk, slotColor(plain, StatusColorSlot::Progress)));
    CHECK(themed.rows[T2].progressMark == Ubiquitty::App::StatusMark::Progress && themed.rows[W2].progressMark == Ubiquitty::App::StatusMark::ProgressError);
    CHECK_MSG(isThemes(themed.rows[W2].progressInk, slotColor(plain, StatusColorSlot::Failed)), "a progress report in its error state is drawn as a failure");

    // A reader that has its own draws those where the theme names none. The
    // status kind's mark and the bell have no platform colour to defer to.
    OutlineConfiguration native = configuration();
    native.platformStatusColors = { .hasProgress = true, .hasProgressInactive = true, .hasProgressError = true, .hasNotification = true };
    const SessionOutline deferred = buildSessionOutline(host.tree(), native);
    REQUIRE(deferred.rows.size() == RowCount);
    CHECK(deferred.rows[T2].progressInk.isPlatformColor);
    CHECK(deferred.rows[W2].progressInk.isPlatformColor);
    CHECK(deferred.rows[B].notificationInk.isPlatformColor);
    CHECK(isThemes(deferred.rows[B].statusInk, deferred.rows[B].statusColor.red));
    CHECK(isThemes(deferred.rows[B].bellInk, slotColor(native, StatusColorSlot::Attention)));

    // A colour the theme names wins over the platform's, slot by slot.
    native.statusColors.namedSlots = static_cast<std::uint8_t>(1U << static_cast<unsigned>(StatusColorSlot::Progress));
    const SessionOutline named = buildSessionOutline(host.tree(), native);
    REQUIRE(named.rows.size() == RowCount);
    CHECK(isThemes(named.rows[T2].progressInk, slotColor(native, StatusColorSlot::Progress)));
    CHECK_MSG(named.rows[W2].progressInk.isPlatformColor, "the failed slot is still unnamed");
    CHECK(named.rows[B].notificationInk.isPlatformColor);
}

TEST(tagsAreTheReadersRulesAgainstEachPanesHost)
{
    Host host;
    const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    if (outline.rows.size() != RowCount)
        return;
    CHECK(tagNames(outline.rows[A]) == std::vector<std::string> { "production" });
    CHECK_MSG(tagNames(outline.rows[D]) == std::vector<std::string> { "production" }, "a host matches whatever its case");
    CHECK(outline.rows[A].tags[0].color.set && outline.rows[A].tags[0].prominence == Ubiquitty::Session::TagProminence::Loud);
    CHECK_MSG(outline.rows[B].tags.empty() && outline.rows[Early].tags.empty(), "a pane on no named host matches no host rule");
    // Closed parents wear their panes' tags, each name once; open, the
    // panes drawn beneath wear their own.
    const auto closed = [&](Row row) { return closedRow(host.tree(), configuration(), outline.rows[row].id); };
    CHECK(tagNames(closed(T1)) == std::vector<std::string> { "production" });
    CHECK(tagNames(closed(W1)) == std::vector<std::string> { "production" });
    CHECK(tagNames(closed(W2)) == std::vector<std::string> { "production" });
    CHECK_MSG(tagNames(closed(Late)) == std::vector<std::string> { "production" }, "two production panes are one tag on their workspace");
    for (const Row row : { T1, W1, W2, Late })
        CHECK(outline.rows[row].isExpanded && outline.rows[row].tags.empty());
    CHECK(outline.rows[T2].tags.empty());

    // A rule for every host marks a pane on no named host too, as it marks a
    // local window.
    const std::vector<Ubiquitty::Preferences::TagEntry> everywhere {
        { .name = "all", .color = "", .matchHost = "*", .matchProfile = "", .prominence = "" },
    };
    OutlineConfiguration wide = configuration();
    wide.tagRules = &everywhere;
    for (const OutlineRow& row : buildSessionOutline(host.tree(), wide).rows)
        CHECK(tagNames(row.isExpanded ? closedRow(host.tree(), wide, row.id) : row) == std::vector<std::string> { "all" });

    // Without rules nothing is tagged.
    OutlineConfiguration bare = configuration();
    bare.tagRules = nullptr;
    for (const OutlineRow& row : buildSessionOutline(host.tree(), bare).rows)
        CHECK(row.tags.empty());

    // Two rules on one host: both, in rule order, on the pane and above it.
    const std::vector<Ubiquitty::Preferences::TagEntry> two {
        { .name = "first", .color = "", .matchHost = "prod-1", .matchProfile = "", .prominence = "" },
        { .name = "production", .color = "red", .matchHost = "prod-*", .matchProfile = "", .prominence = "" },
    };
    bare.tagRules = &two;
    const SessionOutline both = buildSessionOutline(host.tree(), bare);
    CHECK(tagNames(both.rows[A]) == (std::vector<std::string> { "first", "production" }));
    CHECK(tagNames(closedRow(host.tree(), bare, both.rows[Late].id)) == (std::vector<std::string> { "first", "production" }));
    CHECK(tagNames(both.rows[D]) == std::vector<std::string> { "production" });

    // Two untitled rules are two dots, in rule order, rather than one: an
    // untitled tag is told from another by its colour, not by a name it
    // does not have.
    const std::vector<Ubiquitty::Preferences::TagEntry> dots {
        { .name = "", .color = "green", .matchHost = "prod-1", .matchProfile = "", .prominence = "" },
        { .name = "", .color = "blue", .matchHost = "prod-*", .matchProfile = "", .prominence = "" },
    };
    bare.tagRules = &dots;
    const SessionOutline dotted = buildSessionOutline(host.tree(), bare);
    REQUIRE(dotted.rows[A].tags.size() == 2u);
    CHECK(dotted.rows[A].tags[0].colorName == "green" && dotted.rows[A].tags[1].colorName == "blue");
    const OutlineRow closedDots = closedRow(host.tree(), bare, dotted.rows[Late].id);
    REQUIRE(closedDots.tags.size() == 2u);
    CHECK(closedDots.tags[0].colorName == "green" && closedDots.tags[1].colorName == "blue");

    // A row that stands for one pane wears what the rules give that pane's host.
    Host folded;
    folded.at(folded.p1).title.hostName = "prod-9";
    folded.at(folded.c).title.hostName = "prod-9";
    const SessionOutline standing = buildSessionOutline(folded.tree(), configuration());
    CHECK(tagNames(standing.rows[Early]) == std::vector<std::string> { "production" });
    CHECK(tagNames(standing.rows[T2]) == std::vector<std::string> { "production" });
}

TEST(aTagAPersonPutOnANodeIsWornThereAndAboveInTheReadersColour)
{
    Host host;
    const auto put = [&](ObjectID id, std::vector<std::string> names) { (void)host.tags.set(id, names); };
    put({ ObjectKind::Workspace, 2 }, { "mine" });
    put({ ObjectKind::Window, std::to_underlying(host.earlyWindow) }, { "its window's" });
    put({ ObjectKind::Tab, std::to_underlying(host.earlyTab) }, { "its tab's" });
    put({ ObjectKind::Pane, std::to_underlying(host.p1) }, { "its pane's", "MINE" });
    put({ ObjectKind::Window, std::to_underlying(host.w1) }, { "anywhere", "Production" });
    put({ ObjectKind::Tab, std::to_underlying(host.t2) }, { "review" });
    put({ ObjectKind::Tab, std::to_underlying(host.t3) }, { "folded" });
    put({ ObjectKind::Pane, std::to_underlying(host.a) }, { "red", "PRODUCTION" });
    put({ ObjectKind::Pane, std::to_underlying(host.c) }, { "draft" });
    put({ ObjectKind::Pane, std::to_underlying(host.d) }, { "watched" });
    put({ ObjectKind::Session, 77 }, { "sticky", "Watched" });
    OutlineConfiguration made = configuration();
    SessionOutline outline = buildSessionOutline(host.tree(), made);
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    if (outline.rows.size() != RowCount)
        return;

    CHECK_MSG(tagNames(outline.rows[Early]) == (std::vector<std::string> { "mine", "its window's", "its tab's", "its pane's" }), "a row wears the tags of everything folded into it, outermost first");
    // The window's own, then, closed, what is under it.
    const auto closed = [&](Row row) { return closedRow(host.tree(), made, outline.rows[row].id); };
    CHECK(tagNames(closed(W1)) == (std::vector<std::string> { "anywhere", "Production", "red", "review", "draft" }));
    CHECK(tagNames(outline.rows[W1]) == (std::vector<std::string> { "anywhere", "Production" }));
    CHECK_MSG(outline.rows[W1].tags[0].color.set && outline.rows[W1].tags[0].color.blue == 255, "a name the reader's rules colour is drawn in that colour");
    CHECK_MSG(outline.rows[W1].tags[1].prominence == Ubiquitty::Session::TagProminence::Loud, "and as loudly as the rules say");
    // A pane: what the rules gave its host, then what a person added; a name
    // both give is one tag.
    CHECK(tagNames(outline.rows[A]) == (std::vector<std::string> { "production", "red" }));
    CHECK_MSG(outline.rows[A].tags[1].color.set && outline.rows[B].tags.empty(), "a tag named for a colour is that colour, and its neighbour pane wears nothing");
    CHECK(tagNames(closed(T1)) == (std::vector<std::string> { "production", "red" }));
    CHECK_MSG(outline.rows[T1].tags.empty(), "an open tab nobody tagged wears nothing for the panes drawn under it");
    CHECK_MSG(tagNames(outline.rows[T2]) == (std::vector<std::string> { "review", "draft" }), "a tab standing for its one pane wears the pane's tags after its own");
    CHECK_MSG(tagNames(outline.rows[D]) == (std::vector<std::string> { "production", "watched", "sticky" }), "a pane wears its own tags, then those of the session behind it, a name both carry once");
    CHECK_MSG(tagNames(closed(W2)) == (std::vector<std::string> { "folded", "production", "watched", "sticky" }), "a window standing for its one tab wears the tab's tags");
    CHECK_MSG(tagNames(outline.rows[W2]) == (std::vector<std::string> { "folded" }), "and open, the tab's are still its own");
    CHECK(tagNames(closed(Late)) == (std::vector<std::string> { "anywhere", "Production", "red", "review", "draft", "folded", "watched", "sticky" }));
    CHECK(outline.rows[Late].tags.empty());

    // Found on the row a person tagged, not on the rows that wear it for it.
    made.query = "review";
    outline = buildSessionOutline(host.tree(), made);
    CHECK_EQ(outline.matchCount, 1u);
    CHECK(outline.rows.size() == 3 && outline.rows[2].isMatch && outline.rows[2].id == (ObjectID { ObjectKind::Tab, std::to_underlying(host.t2) }));
    made.query = "draft";
    outline = buildSessionOutline(host.tree(), made);
    CHECK_MSG(outline.matchCount == 1 && outline.rows.back().id == (ObjectID { ObjectKind::Tab, std::to_underlying(host.t2) }), "a folded pane's tag finds the row that stands for it");
    made.query = "folded";
    outline = buildSessionOutline(host.tree(), made);
    CHECK(outline.matchCount == 1 && outline.rows.back().id == (ObjectID { ObjectKind::Window, std::to_underlying(host.w2) }));
    made.query = "window's";
    outline = buildSessionOutline(host.tree(), made);
    CHECK(outline.rows.size() == 1 && outline.rows[0].isMatch && outline.rows[0].id == (ObjectID { ObjectKind::Workspace, 2 }));
    made.query = "sticky";
    outline = buildSessionOutline(host.tree(), made);
    CHECK_MSG(outline.matchCount == 1 && outline.rows.back().id == (ObjectID { ObjectKind::Pane, std::to_underlying(host.d) }), "a session's tag finds the pane the session draws into");
}

TEST(aClosedParentHidesItsRowsAndSaysWhatItHides)
{
    Host host;
    OutlineConfiguration made = configuration();
    const std::array closed { ObjectID { ObjectKind::Tab, std::to_underlying(host.t1) }, ObjectID { ObjectKind::Window, std::to_underlying(host.w2) }, ObjectID { ObjectKind::Tab, std::to_underlying(host.t2) },
        ObjectID { ObjectKind::Workspace, 2 }, ObjectID { ObjectKind::Tab, std::to_underlying(host.t3) } };
    made.collapsed = closed;
    SessionOutline outline = buildSessionOutline(host.tree(), made);
    const std::vector<std::string> expected {
        "0:" + id(ObjectKind::Workspace, 2u),
        "0:" + id(ObjectKind::Workspace, 9u),
        "1:" + id(ObjectKind::Window, host.w1),
        "2:" + id(ObjectKind::Tab, host.t1),
        "2:" + id(ObjectKind::Tab, host.t2),
        "1:" + id(ObjectKind::Window, host.w2),
    };
    CHECK(shape(outline) == expected);
    if (outline.rows.size() != expected.size())
        return;

    const OutlineRow& t1 = outline.rows[3];
    CHECK(t1.isExpandable && !t1.isExpanded);
    CHECK(t1.collapsedSummary == (OutlineSummary { 0, 0, 2, StatusKind::Blocked, 1, PaneActivity { 99'500, 95'000 } }));
    CHECK_MSG(t1.status.kind == StatusKind::Blocked && tagNames(t1) == std::vector<std::string> { "production" }, "a closed row still shows its roll-up and its tags");
    const OutlineRow& w2 = outline.rows[5];
    CHECK(!w2.isExpanded);
    CHECK_MSG(w2.collapsedSummary == (OutlineSummary { 0, 1, 2, StatusKind::Failed, 1, PaneActivity { 200'000, 0 } }), "a window of one tab hides that tab and its panes");
    // Open rows carry no summary, and a row with nothing under it cannot be closed.
    CHECK(outline.rows[2].isExpanded && !outline.rows[2].collapsedSummary);
    CHECK_MSG(!outline.rows[4].isExpandable && !outline.rows[4].isExpanded && !outline.rows[4].collapsedSummary, "a tab that stands for its one pane");
    CHECK_MSG(!outline.rows[0].isExpandable && !outline.rows[0].isExpanded && !outline.rows[0].collapsedSummary, "a workspace that is one row");

    // A closed workspace hides every level; a closed row under it stays closed
    // for when it is opened.
    const std::array outer { ObjectID { ObjectKind::Workspace, 9 }, ObjectID { ObjectKind::Tab, std::to_underlying(host.t1) } };
    made.collapsed = outer;
    outline = buildSessionOutline(host.tree(), made);
    CHECK(shape(outline) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 2u), "0:" + id(ObjectKind::Workspace, 9u) }));
    CHECK(outline.rows[1].collapsedSummary == (OutlineSummary { 2, 3, 5, StatusKind::Failed, 1, PaneActivity { 200'000, 95'000 } }));

    // A window of several tabs counts them.
    const std::array window { ObjectID { ObjectKind::Window, std::to_underlying(host.w1) } };
    made.collapsed = window;
    outline = buildSessionOutline(host.tree(), made);
    const OutlineRow* w1 = find(outline, window[0]);
    CHECK(w1 && w1->collapsedSummary == (OutlineSummary { 0, 2, 3, StatusKind::Blocked, 1, PaneActivity { 99'500, 95'000 } }));
    CHECK(find(outline, { ObjectKind::Tab, std::to_underlying(host.t1) }) == nullptr && find(outline, { ObjectKind::Tab, std::to_underlying(host.t2) }) == nullptr);
    CHECK(find(outline, { ObjectKind::Pane, std::to_underlying(host.d) }) != nullptr);

    // A workspace that folded its window and tab counts them among what it hides.
    Small small { 2 };
    const std::array smallClosed { ObjectID { ObjectKind::Workspace, 3 } };
    made.collapsed = smallClosed;
    outline = buildSessionOutline(small.tree(), made);
    CHECK(outline.rows.size() == 1 && outline.rows[0].collapsedSummary && outline.rows[0].collapsedSummary->windows == 1 && outline.rows[0].collapsedSummary->tabs == 1 && outline.rows[0].collapsedSummary->panes == 2);
}

TEST(aSearchKeepsWhatMatchesAndOpensTheWayToIt)
{
    Host host;
    OutlineConfiguration made = configuration();
    const auto search = [&](std::string_view query) {
        made.query = query;
        return buildSessionOutline(host.tree(), made);
    };
    const auto matched = [](const SessionOutline& outline) {
        std::vector<std::string> ids;
        for (const OutlineRow& row : outline.rows) {
            if (row.isMatch)
                ids.push_back(Ubiquitty::Session::Identity::format(row.id));
        }
        return ids;
    };
    const std::string late = "0:" + id(ObjectKind::Workspace, 9u);
    const std::string w1 = "1:" + id(ObjectKind::Window, host.w1);
    const std::string w2 = "1:" + id(ObjectKind::Window, host.w2);
    const std::string t1 = "2:" + id(ObjectKind::Tab, host.t1);
    const std::string t2 = "2:" + id(ObjectKind::Tab, host.t2);

    // By program: the pane, under its tab, window and workspace; nothing else.
    SessionOutline outline = search("ctest");
    CHECK(shape(outline) == (std::vector<std::string> { late, w1, t1, "3:" + id(ObjectKind::Pane, host.b) }));
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Pane, host.b) });
    CHECK_EQ(outline.matchCount, 1u);
    CHECK_MSG(outline.rows[0].isExpanded && outline.rows[1].isExpanded && outline.rows[2].isExpanded && !outline.rows[2].collapsedSummary, "the way to a match is open");

    // By directory, whatever the case, and through a closed ancestor.
    const std::array closed { ObjectID { ObjectKind::Workspace, 9 }, ObjectID { ObjectKind::Window, std::to_underlying(host.w2) } };
    made.collapsed = closed;
    outline = search("SRV/DEPLOY");
    CHECK(shape(outline) == (std::vector<std::string> { late, w2, "2:" + id(ObjectKind::Pane, host.e) }));
    CHECK_MSG(outline.rows[0].isExpanded && outline.rows[1].isExpanded, "a search opens what the reader had closed");
    made.collapsed = {};

    // By tag: the panes that earned it.
    outline = search("production");
    CHECK(shape(outline) == (std::vector<std::string> { late, w1, t1, "3:" + id(ObjectKind::Pane, host.a), w2, "2:" + id(ObjectKind::Pane, host.d) }));
    CHECK(matched(outline) == (std::vector<std::string> { id(ObjectKind::Pane, host.a), id(ObjectKind::Pane, host.d) }));
    CHECK_EQ(outline.matchCount, 2u);

    // A row that stands for one pane is found by what that pane runs and where.
    outline = search("notes");
    CHECK(shape(outline) == (std::vector<std::string> { late, w1, t2 }));
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Tab, host.t2) });
    outline = search("zsh");
    CHECK_MSG(shape(outline) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 2u) }) && outline.matchCount == 1, "a workspace that is one row is found by its pane's program");
    outline = search("/home/user");
    CHECK(matched(outline) == (std::vector<std::string> { id(ObjectKind::Workspace, 2u), id(ObjectKind::Tab, host.t2) }));

    // By label: a title that is a label, a chip, a folded tab's chip.
    outline = search("editor");
    CHECK(shape(outline) == (std::vector<std::string> { late, w1, t2 }));
    outline = search("solo");
    CHECK(shape(outline) == (std::vector<std::string> { late, w2 }));
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Window, host.w2) });
    CHECK_MSG(outline.rows[1].isExpandable && !outline.rows[1].isExpanded && outline.rows[1].collapsedSummary.has_value(), "a matching parent with no matching rows under it is shown closed");
    outline = search("TB1");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Tab, host.t1) });

    // By the label of the session behind a pane.
    outline = search("longrun");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Pane, host.d) });
    // What a row is found by it shows: the session's label is a chip on the
    // row of its pane, and on no other.
    for (const OutlineRow& row : outline.rows) {
        const bool isItsPane = row.isMatch; // bool-ok: a local reading
        CHECK_MSG((std::ranges::find(row.labelChips, "longrun") != row.labelChips.end()) == isItsPane, "the session's label is a chip on its pane's row alone");
    }

    // By title and by name, at every level.
    outline = search("build");
    CHECK_MSG(matched(outline) == (std::vector<std::string> { id(ObjectKind::Tab, host.t1), id(ObjectKind::Pane, host.b) }), "the tab called build, and the pane in /srv/app/build");
    outline = search("late");
    CHECK(shape(outline) == std::vector<std::string> { late });
    outline = search("nine");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Workspace, 9u) });
    outline = search("ops");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Window, host.w1) });
    outline = search("early");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Workspace, 2u) });

    // Every term must be found in the same row, in any of its texts.
    outline = search("make  /srv");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Pane, host.a) });
    outline = search("make production");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Pane, host.a) });
    outline = search("make deploy");
    CHECK_MSG(outline.rows.empty() && outline.matchCount == 0, "terms found in two different rows match neither");
    outline = search("nothing-like-this");
    CHECK(outline.rows.empty());

    // Letters outside ASCII compare exactly: found in the case displayed, not
    // in the other.
    (void)host.labels.set({ ObjectKind::Pane, std::to_underlying(host.a) }, "éditeur Z");
    outline = search("éDITEUR z");
    CHECK(matched(outline) == std::vector<std::string> { id(ObjectKind::Pane, host.a) });
    outline = search("Éditeur");
    CHECK_MSG(outline.rows.empty(), "an accented capital does not find its small letter");
    (void)host.labels.set({ ObjectKind::Pane, std::to_underlying(host.a) }, "");

    // A blank query is no search.
    outline = search("  \t ");
    CHECK(outline.rows.size() == RowCount && outline.matchCount == 0);
    for (const OutlineRow& row : outline.rows)
        CHECK(!row.isMatch);
}

TEST(aWorkspaceTheHostCouldRestoreIsDrawnFromItsRecord)
{
    using Ubiquitty::Session::HostLayoutRecord;
    using Ubiquitty::Session::PaneNode;
    using Ubiquitty::Session::TreeRestorableSource;
    using Ubiquitty::Session::TreeWorkspaceState;
    using Ubiquitty::Session::WorkspaceTab;
    using Ubiquitty::Session::WorkspaceWindow;
    const auto leaf = [](std::string directory, std::string title) {
        PaneNode node = PaneNode::makeLeaf({}, std::move(title));
        node.directory = std::move(directory);
        return node;
    };
    // "kept": a window labelled main with one tab of two panes, and a window
    // of one tab of one pane.
    HostLayoutRecord record;
    record.arrangement.name = "kept";
    record.workspaceLabel = "k";
    WorkspaceWindow first;
    WorkspaceTab split;
    split.root = PaneNode::makeSplit(false, 0.5, leaf("/var/log", "tail"), leaf("/srv", "vim"));
    first.tabs.push_back(split);
    record.arrangement.windows.push_back(first);
    WorkspaceWindow second;
    WorkspaceTab single;
    single.root = leaf("/home/user", "shell");
    second.tabs.push_back(single);
    record.arrangement.windows.push_back(second);
    record.windowLabels = { "main", "" };
    record.tabLabels = { "tailing", "" };
    record.windowTags = { { "production" }, {} };
    record.panes = { { 100, 30, "", "", {}, {} }, { 50, 15, "", "scratch", {}, {} }, { 80, 24, "", "", {}, {} } };

    Host host;
    const std::array sources {
        TreeWorkspaceSource { .id = 2, .name = "early", .layout = host.early.get(), .endsWhenDetached = true, .wasRestoredAfterHostRestart = true },
        TreeWorkspaceSource { .id = 9, .name = "late", .layout = host.late.get() },
    };
    const std::array restorable { TreeRestorableSource { 5, &record } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources, .restorable = restorable }, host.labels, [&](std::uint32_t, Layout::PaneID which) { return host.at(which); });
    OutlineConfiguration made = configuration();
    SessionOutline outline = buildSessionOutline(tree, made);

    // early, then kept's five rows, then late's nine.
    CHECK_EQ(outline.rows.size(), 15u);
    if (outline.rows.size() != 15)
        return;
    CHECK(outline.rows[0].workspaceState == TreeWorkspaceState::RestoredAfterHostRestart && outline.rows[0].endsWhenDetached);
    for (std::size_t index = 1; index <= 5; ++index)
        CHECK(outline.rows[index].workspaceState == TreeWorkspaceState::Restorable);
    for (std::size_t index = 6; index < outline.rows.size(); ++index)
        CHECK(outline.rows[index].workspaceState == TreeWorkspaceState::Live && !outline.rows[index].endsWhenDetached);

    const OutlineRow& kept = outline.rows[1];
    CHECK(kept.level == ListedLevel::Workspace && kept.id == (ObjectID { ObjectKind::Workspace, 5 }) && kept.depth == 0);
    CHECK_STR_EQ(kept.title, "kept");
    CHECK(kept.labelChips == chips({ "k" }));
    CHECK(kept.isExpandable && kept.isExpanded && !kept.endsWhenDetached);
    CHECK_MSG(kept.status.kind == StatusKind::None && kept.status.paneCount == 3 && kept.statusSlot == StatusColorSlot::Neutral, "nothing runs in it");
    CHECK(kept.sinceOutput == OutlineAge {} && kept.sinceInput == OutlineAge {} && kept.attendance.total() == 0);
    CHECK_MSG(kept.tags.empty(), "open, it wears nothing for the rows under it");

    // Its recorded windows, tabs and panes, folded as any others are.
    const OutlineRow& main = outline.rows[2];
    CHECK(main.level == ListedLevel::Window && main.depth == 1 && !main.id.isValid());
    CHECK_STR_EQ(main.title, "main");
    CHECK(main.labelChips == chips({ "tailing" }) && tagNames(main) == std::vector<std::string> { "production" });
    CHECK(main.isExpandable && main.isExpanded);
    CHECK(outline.rows[3].level == ListedLevel::Pane && outline.rows[3].depth == 2 && outline.rows[3].title == "tail" && outline.rows[3].directory == "/var/log");
    CHECK(outline.rows[3].columns == 100 && outline.rows[3].rows == 30 && !outline.rows[3].id.isValid());
    CHECK(outline.rows[4].title == "vim" && outline.rows[4].columns == 50 && outline.rows[4].isHostCurrent == false);
    // The second window is one row: its tab and its pane are folded into it.
    const SessionOutline alone = buildSessionOutline(buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 0, .workspaces = {}, .restorable = restorable }, LabelBook {}, [](std::uint32_t, Layout::PaneID) { return Ubiquitty::Session::TreePaneFacts {}; }), made);
    CHECK_EQ(alone.rows.size(), 5u);
    CHECK(alone.rows.size() == 5 && alone.rows[4].level == ListedLevel::Window && alone.rows[4].title == "shell" && alone.rows[4].directory == "/home/user" && !alone.rows[4].isExpandable);
    CHECK_MSG(outline.rows[5].level == ListedLevel::Window && outline.rows[5].title == "shell", "the same row in the full outline");

    // A recorded window or tab has no identity and is closed by its place in
    // the record, which its row states.
    using Ubiquitty::App::OutlineRecordedRowKey;
    CHECK_MSG(!kept.recordedKey && !outline.rows[0].recordedKey && !outline.rows[7].recordedKey, "a row with an identity has no other key");
    CHECK(main.recordedKey == (OutlineRecordedRowKey { 5, ListedLevel::Window, 0, 0 }));
    CHECK(outline.rows[5].recordedKey == (OutlineRecordedRowKey { 5, ListedLevel::Window, 1, 0 }));
    CHECK_MSG(!outline.rows[3].recordedKey, "a pane's row has nothing under it to close");
    const std::array closedMain { *main.recordedKey };
    made.collapsedRecordedRows = closedMain;
    SessionOutline folded = buildSessionOutline(tree, made);
    CHECK_EQ(folded.rows.size(), 13u);
    CHECK(folded.rows.size() == 13 && !folded.rows[2].isExpanded && folded.rows[2].collapsedSummary == (OutlineSummary { 0, 1, 2, StatusKind::None, 0, PaneActivity {} }));
    CHECK_MSG(folded.rows.size() == 13 && folded.rows[3].title == "shell" && folded.rows[1].isExpanded, "its neighbour window and its workspace stay as they were");
    // A key for another workspace, another window or another level closes nothing here.
    const std::array elsewhere { OutlineRecordedRowKey { 6, ListedLevel::Window, 0, 0 }, OutlineRecordedRowKey { 5, ListedLevel::Tab, 0, 0 }, OutlineRecordedRowKey { 5, ListedLevel::Window, 7, 0 },
        OutlineRecordedRowKey { 9, ListedLevel::Window, 0, 0 } };
    made.collapsedRecordedRows = elsewhere;
    CHECK(buildSessionOutline(tree, made).rows.size() == 15);
    made.collapsedRecordedRows = {};

    // A recorded tab, in a window of several: closed by window and tab index.
    HostLayoutRecord tabbed = record;
    tabbed.arrangement.windows[1].tabs.push_back(split);
    tabbed.panes.push_back({ 80, 24, "", "", {}, {} });
    tabbed.panes.push_back({ 80, 24, "", "", {}, {} });
    const std::array tabbedSource { TreeRestorableSource { 5, &tabbed } };
    const SessionTree tabbedTree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 0, .workspaces = {}, .restorable = tabbedSource }, LabelBook {}, [](std::uint32_t, Layout::PaneID) { return Ubiquitty::Session::TreePaneFacts {}; });
    SessionOutline tabs = buildSessionOutline(tabbedTree, made);
    // kept; main, tail, vim; the second window, its tab of one pane, its tab of two with their rows.
    CHECK_EQ(tabs.rows.size(), 9u);
    if (tabs.rows.size() != 9)
        return;
    CHECK(tabs.rows[5].level == ListedLevel::Tab && tabs.rows[5].recordedKey == (OutlineRecordedRowKey { 5, ListedLevel::Tab, 1, 0 }) && !tabs.rows[5].isExpandable);
    CHECK(tabs.rows[6].level == ListedLevel::Tab && tabs.rows[6].recordedKey == (OutlineRecordedRowKey { 5, ListedLevel::Tab, 1, 1 }) && tabs.rows[6].isExpanded);
    const std::array closedTab { *tabs.rows[6].recordedKey };
    made.collapsedRecordedRows = closedTab;
    tabs = buildSessionOutline(tabbedTree, made);
    CHECK(tabs.rows.size() == 7 && !tabs.rows[6].isExpanded && tabs.rows[6].collapsedSummary && tabs.rows[6].collapsedSummary->panes == 2);
    CHECK_MSG(tabs.rows.size() == 7 && tabs.rows[4].isExpanded && tabs.rows[2].level == ListedLevel::Pane, "the window holding the tab, and the other window's panes, stay open");
    // The window of that index is another row, with another key.
    const std::array closedWindow { OutlineRecordedRowKey { 5, ListedLevel::Window, 1, 0 } };
    made.collapsedRecordedRows = closedWindow;
    tabs = buildSessionOutline(tabbedTree, made);
    CHECK(tabs.rows.size() == 5 && !tabs.rows[4].isExpanded && tabs.rows[4].collapsedSummary && tabs.rows[4].collapsedSummary->tabs == 2 && tabs.rows[4].collapsedSummary->panes == 3);
    made.collapsedRecordedRows = {};

    // A recorded row is not closed by an identity: naming none closes nothing.
    const std::array closedNone { ObjectID { ObjectKind::Window, 0 }, ObjectID { ObjectKind::Tab, 0 } };
    made.collapsed = closedNone;
    CHECK(buildSessionOutline(tree, made).rows.size() == 15);
    const std::array closed { ObjectID { ObjectKind::Workspace, 5 } };
    made.collapsed = closed;
    outline = buildSessionOutline(tree, made);
    CHECK_EQ(outline.rows.size(), 11u);
    CHECK(outline.rows[1].collapsedSummary == (OutlineSummary { 2, 2, 3, StatusKind::None, 0, PaneActivity {} }));
    made.collapsed = {};

    // Found like any other row, and still marked as what it is.
    made.query = "scratch";
    outline = buildSessionOutline(tree, made);
    CHECK(outline.rows.size() == 3 && outline.matchCount == 1 && outline.rows[2].isMatch && outline.rows[2].title == "vim" && outline.rows[2].workspaceState == TreeWorkspaceState::Restorable);
}

TEST(aHostOfNothingHasNoRowsAndAnEmptyWorkspaceHasOne)
{
    const SessionTree none = buildSessionTree({ .revision = 4, .nowUnixMilliseconds = 100'000, .workspaces = {} }, LabelBook {}, [](std::uint32_t, Layout::PaneID) { return Ubiquitty::Session::TreePaneFacts {}; });
    const SessionOutline empty = buildSessionOutline(none, configuration());
    CHECK(empty.rows.empty() && empty.revision == 4);

    const std::array sources { TreeWorkspaceSource { 5, "hollow", nullptr } };
    const SessionTree tree = buildSessionTree({ .revision = 1, .nowUnixMilliseconds = 100'000, .workspaces = sources }, LabelBook {}, [](std::uint32_t, Layout::PaneID) { return Ubiquitty::Session::TreePaneFacts {}; });
    const SessionOutline outline = buildSessionOutline(tree, configuration());
    CHECK_EQ(outline.rows.size(), 1u);
    if (outline.rows.empty())
        return;
    CHECK_STR_EQ(outline.rows[0].title, "hollow");
    CHECK_MSG(!outline.rows[0].isExpandable && !outline.rows[0].isExpanded && !outline.rows[0].collapsedSummary, "a workspace of nothing has nothing to open");
    CHECK(!outline.rows[0].foldedWindow && !outline.rows[0].foldedTab && !outline.rows[0].foldedPane);
    CHECK(outline.rows[0].status.kind == StatusKind::None && outline.rows[0].statusSlot == StatusColorSlot::Neutral);
    CHECK(outline.rows[0].sinceOutput == OutlineAge {});
}

// ---- What a row's actions read ----------------------------------------------

namespace {

using Ubiquitty::App::OutlineOwnValues;
using Ubiquitty::App::OutlineTabPlace;
using Ubiquitty::App::TabAccessory;
using Ubiquitty::Session::AttachObstacle;
using Ubiquitty::Session::AttachOpening;
using Ubiquitty::Session::AttachPlan;
using Ubiquitty::Session::AttachTarget;
using Ubiquitty::Session::attachTargetOf;
using Ubiquitty::Session::RememberedSessions;
using Ubiquitty::Session::TreeWorkspace;

/// `Host` with a session behind every pane.
Host sessionedHost()
{
    Host host;
    host.at(host.p1).sessionID = 21;
    host.at(host.a).sessionID = 11;
    host.at(host.b).sessionID = 12;
    host.at(host.c).sessionID = 13;
    host.at(host.e).sessionID = 15;
    return host;
}

ObjectID identity(ObjectKind kind, std::uint64_t number) { return { kind, number }; }

} // namespace

TEST(everyRowOpensWhatTheConnectRuleLandsOn)
{
    Host host = sessionedHost();
    SessionTree tree = host.tree();
    // A workspace the host could restore, of two tabs, and one it kept under
    // no name.
    TreeWorkspace kept;
    kept.id = 60;
    kept.name = "kept";
    kept.state = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    kept.windows.emplace_back();
    kept.windows[0].tabs.resize(2);
    kept.windows[0].tabs[0].panes.emplace_back();
    kept.windows[0].tabs[1].panes.emplace_back();
    TreeWorkspace nameless = kept;
    nameless.id = 61;
    nameless.name.clear();
    tree.workspaces.push_back(kept);
    tree.workspaces.push_back(nameless);
    const SessionOutline outline = buildSessionOutline(tree, configuration());
    REQUIRE(outline.rows.size() > static_cast<std::size_t>(RowCount));

    std::size_t opened = 0;
    for (const OutlineRow& row : outline.rows) {
        const TreeWorkspace& workspace = tree.workspaces[row.path.workspace];
        CHECK_EQ(row.open.workspaceID, workspace.id);
        CHECK_STR_EQ(row.open.workspaceName, workspace.name);
        if (row.open.opening == AttachOpening::BySession) {
            ++opened;
            // Choosing what the row opens is what the rule attaches to, in
            // the row's own workspace.
            const auto decision = RememberedSessions::decide(std::nullopt, "", &tree, row.open.sessionID);
            CHECK(decision.plan == AttachPlan::AttachByID && decision.sessionID == row.open.sessionID && decision.workspaceID == row.open.workspaceID);
            CHECK_MSG(row.open.paneID != 0 && row.open.obstacle == AttachObstacle::None, "a row that opens says which pane is shown");
        }
        if (row.level == ListedLevel::Workspace && workspace.id != 61) {
            // A workspace's row opens what asking for it by name lands on.
            const auto decision = RememberedSessions::decide(std::nullopt, workspace.name, &tree);
            CHECK(decision.plan == AttachPlan::AttachNamed && decision.workspaceID == row.open.workspaceID && decision.sessionID == row.open.sessionID);
            CHECK(row.open == attachTargetOf(workspace));
        }
    }
    CHECK_EQ(opened, static_cast<std::size_t>(RowCount));

    // A pane's row opens that pane.
    const OutlineRow& a = outline.rows[A];
    CHECK(a.open.sessionID == 11 && a.open.paneID == std::to_underlying(host.a) && a.open.workspaceID == 9);
    CHECK(outline.rows[D].open.sessionID == 77 && outline.rows[D].open.paneID == std::to_underlying(host.d));
    // A tab's row opens where the host's focus is in it; a window's, where
    // its shown tab lands; the workspace's, where its window in front lands.
    CHECK_MSG(outline.rows[T1].open.sessionID == 12 && outline.rows[T1].open.paneID == std::to_underlying(host.b), "a tab lands on the pane it types into");
    CHECK(outline.rows[W1].open == outline.rows[T1].open);
    CHECK(outline.rows[Late].open == outline.rows[W1].open);
    CHECK_MSG(outline.rows[T2].open.sessionID == 13, "a tab that is not shown still opens at its own pane");
    CHECK(outline.rows[W2].open.sessionID == 15 && outline.rows[W2].open.paneID == std::to_underlying(host.e));
    CHECK(outline.rows[Early].open.sessionID == 21);

    // The rows of what nothing runs in: the workspace by its name, nothing
    // below it, and nothing at all for one kept under no name.
    const auto rowOf = [&](std::uint32_t workspaceID, ListedLevel level) {
        const auto found = std::ranges::find_if(outline.rows, [&](const OutlineRow& row) { return tree.workspaces[row.path.workspace].id == workspaceID && row.level == level; });
        return found != outline.rows.end() ? &*found : nullptr;
    };
    const OutlineRow* restored = rowOf(60, ListedLevel::Workspace);
    REQUIRE(restored != nullptr);
    CHECK(restored->open.opening == AttachOpening::ByRestoring && restored->open.sessionID == 0 && restored->open.paneID == 0 && restored->open.workspaceName == "kept");
    const OutlineRow* part = rowOf(60, ListedLevel::Tab);
    REQUIRE(part != nullptr);
    CHECK(part->open.opening == AttachOpening::Cannot && part->open.obstacle == AttachObstacle::PartOfARecord && part->open.workspaceID == 60);
    const OutlineRow* lost = rowOf(61, ListedLevel::Workspace);
    REQUIRE(lost != nullptr);
    CHECK(lost->open.opening == AttachOpening::Cannot && lost->open.obstacle == AttachObstacle::RecordedUnderNoName);
    // A pane no session draws into cannot be opened, and says why; the tab
    // it is in lands on a neighbour.
    Host sparse = sessionedHost();
    sparse.at(sparse.b).sessionID = 0;
    const SessionOutline thinner = buildSessionOutline(sparse.tree(), configuration());
    CHECK(thinner.rows[B].open.opening == AttachOpening::Cannot && thinner.rows[B].open.obstacle == AttachObstacle::NothingRuns);
    CHECK(thinner.rows[T1].open.sessionID == 11);
    for (const AttachObstacle obstacle : { AttachObstacle::NothingRuns, AttachObstacle::PartOfARecord, AttachObstacle::RecordedUnderNoName })
        CHECK(!Ubiquitty::Session::attachObstacleSentence(obstacle).empty());
    CHECK(Ubiquitty::Session::attachObstacleSentence(AttachObstacle::None).empty());
}

TEST(aRowStatesEachLevelsOwnLabelAndTagsApartFromWhatItWears)
{
    Host host = sessionedHost();
    const auto put = [&](ObjectID id, std::vector<std::string> names) { (void)host.tags.set(id, names); };
    const ObjectID earlyWorkspace = identity(ObjectKind::Workspace, 2);
    const ObjectID earlyWindow = identity(ObjectKind::Window, std::to_underlying(host.earlyWindow));
    const ObjectID earlyTab = identity(ObjectKind::Tab, std::to_underlying(host.earlyTab));
    const ObjectID earlyPane = identity(ObjectKind::Pane, std::to_underlying(host.p1));
    const ObjectID earlySession = identity(ObjectKind::Session, 21);
    put(earlyWorkspace, { "ws" });
    put(earlyWindow, { "win" });
    put(earlyTab, { "tab", "shared" });
    put(earlyPane, { "pane", "shared" });
    put(earlySession, { "session" });
    (void)host.labels.set(earlyTab, "the tab");
    (void)host.labels.set(earlySession, "the session");
    const OutlineConfiguration made = configuration();
    SessionOutline outline = buildSessionOutline(host.tree(), made);
    REQUIRE(outline.rows.size() == RowCount);

    // The folded row is all five levels, outermost first, each with its own.
    const OutlineRow& early = outline.rows[Early];
    REQUIRE(early.own.size() == 5);
    CHECK(early.own[0] == (OutlineOwnValues { earlyWorkspace, "", { "ws" }, "early" }));
    CHECK(early.own[1] == (OutlineOwnValues { earlyWindow, "", { "win" }, "" }));
    CHECK(early.own[2] == (OutlineOwnValues { earlyTab, "the tab", { "tab", "shared" }, "" }));
    CHECK(early.own[3] == (OutlineOwnValues { earlyPane, "", { "pane", "shared" }, "" }));
    CHECK(early.own[4] == (OutlineOwnValues { earlySession, "the session", { "session" }, "" }));
    // What it wears is the merge: every name once.
    CHECK(tagNames(early) == (std::vector<std::string> { "ws", "win", "tab", "shared", "pane", "session" }));

    // An edit of one level, made from what the row said that level holds,
    // loses nothing of the others.
    std::vector<std::string> edited = early.own[2].tags;
    edited.push_back("added");
    put(earlyTab, edited);
    (void)host.labels.set(earlyPane, "runner");
    outline = buildSessionOutline(host.tree(), made);
    const OutlineRow& again = outline.rows[Early];
    REQUIRE(again.own.size() == 5);
    CHECK(again.own[2] == (OutlineOwnValues { earlyTab, "the tab", { "tab", "shared", "added" }, "" }));
    CHECK(again.own[3] == (OutlineOwnValues { earlyPane, "runner", { "pane", "shared" }, "" }));
    CHECK(again.own[0].tags == std::vector<std::string> { "ws" } && again.own[1].tags == std::vector<std::string> { "win" } && again.own[4].tags == std::vector<std::string> { "session" });

    // A row is only the levels it is or stands for.
    const auto kinds = [](const OutlineRow& row) {
        std::vector<ObjectKind> listed;
        for (const OutlineOwnValues& level : row.own)
            listed.push_back(level.id.kind);
        return listed;
    };
    CHECK(kinds(outline.rows[Late]) == std::vector<ObjectKind> { ObjectKind::Workspace });
    CHECK(kinds(outline.rows[W1]) == std::vector<ObjectKind> { ObjectKind::Window });
    CHECK(kinds(outline.rows[T1]) == std::vector<ObjectKind> { ObjectKind::Tab });
    CHECK(kinds(outline.rows[A]) == (std::vector<ObjectKind> { ObjectKind::Pane, ObjectKind::Session }));
    CHECK(kinds(outline.rows[T2]) == (std::vector<ObjectKind> { ObjectKind::Tab, ObjectKind::Pane, ObjectKind::Session }));
    CHECK(kinds(outline.rows[W2]) == (std::vector<ObjectKind> { ObjectKind::Window, ObjectKind::Tab }));
    CHECK(outline.rows[T1].own[0].label == "tb1" && outline.rows[W1].own[0].label == "ops" && outline.rows[Late].own[0].label == "nine");
    CHECK(outline.rows[T2].own[1].label == "editor" && outline.rows[D].own[1].label == "longrun" && outline.rows[D].own[1].id == identity(ObjectKind::Session, 77));
    // The mark beside a current row is said in words, by level.
    CHECK_STR_EQ(Ubiquitty::App::hostCurrentWords(outline.rows[W1]), "The window in front on this host");
    CHECK_STR_EQ(Ubiquitty::App::hostCurrentWords(outline.rows[T1]), "The tab its window shows");
    CHECK_STR_EQ(Ubiquitty::App::hostCurrentWords(outline.rows[B]), "The pane its tab types into");
    CHECK(Ubiquitty::App::hostCurrentWords(outline.rows[A]).empty() && Ubiquitty::App::hostCurrentWords(outline.rows[W2]).empty() && Ubiquitty::App::hostCurrentWords(outline.rows[Late]).empty());
    // A workspace's and a tab's own name, which a rename replaces, apart
    // from what the row is called: a level that takes no name has none.
    CHECK_STR_EQ(outline.rows[T1].own[0].name, "build");
    CHECK_STR_EQ(outline.rows[Late].own[0].name, "late");
    CHECK_MSG(outline.rows[T2].title == "editor" && outline.rows[T2].own[0].name.empty(), "a tab called by its pane's label has no name of its own");
    CHECK(outline.rows[W1].own[0].name.empty() && outline.rows[A].own[0].name.empty() && outline.rows[A].own[1].name.empty());
    (void)host.late->renameTab(host.w2, host.t3, "rollout");
    const SessionOutline renamed = buildSessionOutline(host.tree(), configuration());
    REQUIRE(renamed.rows[W2].own.size() == 2);
    CHECK_MSG(renamed.rows[W2].own[1].id.kind == ObjectKind::Tab && renamed.rows[W2].own[1].name == "rollout", "a row standing for a tab states the tab's name as that level's own");
    // A pane no session draws into has no session to set anything on.
    Host bare;
    const SessionOutline unbacked = buildSessionOutline(bare.tree(), made);
    CHECK(kinds(unbacked.rows[A]) == std::vector<ObjectKind> { ObjectKind::Pane });
}

TEST(aTabsRowSaysWhereItStandsAndWhereItMayGo)
{
    Host host = sessionedHost();
    const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
    REQUIRE(outline.rows.size() == RowCount);
    const ObjectID w1 = identity(ObjectKind::Window, std::to_underlying(host.w1));
    const ObjectID w2 = identity(ObjectKind::Window, std::to_underlying(host.w2));
    const auto tab = [](Layout::TabID id) { return ObjectID { ObjectKind::Tab, std::to_underlying(id) }; };
    CHECK(outline.rows[T1].tabPlace == (OutlineTabPlace { .tab = tab(host.t1), .window = w1, .index = 0, .tabCount = 2, .canMoveEarlier = false, .canMoveLater = true, .acceptedIndices = { 1 }, .otherWindows = { { w2, "deploy", 1 } } }));
    CHECK(outline.rows[T2].tabPlace == (OutlineTabPlace { .tab = tab(host.t2), .window = w1, .index = 1, .tabCount = 2, .canMoveEarlier = true, .canMoveLater = false, .acceptedIndices = { 0 }, .otherWindows = { { w2, "deploy", 1 } } }));
    // A row standing for a tab says the same of it.
    CHECK(outline.rows[W2].tabPlace == (OutlineTabPlace { .tab = tab(host.t3), .window = w2, .index = 0, .tabCount = 1, .canMoveEarlier = false, .canMoveLater = false, .acceptedIndices = {}, .otherWindows = { { w1, "ops", 2 } } }));
    CHECK_MSG(outline.rows[W2].tabPlace->otherWindows[0].title == outline.rows[W1].title && outline.rows[T1].tabPlace->otherWindows[0].title == outline.rows[W2].title, "a window is called what its own row is called");
    REQUIRE(outline.rows[Early].tabPlace.has_value());
    CHECK(outline.rows[Early].tabPlace->tab == tab(host.earlyTab) && outline.rows[Early].tabPlace->otherWindows.empty() && outline.rows[Early].tabPlace->tabCount == 1);
    // A row that is no tab's says nothing.
    for (const Row row : { Late, W1, A, B, D, E })
        CHECK(!outline.rows[row].tabPlace.has_value());
    // A drop within its own window is accepted at every place but its own.
    while (host.late->tabCount(host.w1) < 4)
        (void)host.late->addTab(host.w1);
    const SessionOutline four = buildSessionOutline(host.tree(), configuration());
    const OutlineRow* second = find(four, tab(host.t2));
    REQUIRE(second != nullptr && second->tabPlace.has_value());
    CHECK(second->tabPlace->index == 1 && second->tabPlace->tabCount == 4 && second->tabPlace->acceptedIndices == (std::vector<std::uint32_t> { 0, 2, 3 }));
    // A window holding every tab it may is nowhere to go.
    while (host.late->tabCount(host.w2) < Layout::maxTabsPerSurface)
        (void)host.late->addTab(host.w2);
    const SessionOutline full = buildSessionOutline(host.tree(), configuration());
    REQUIRE(full.rows[T1].tabPlace.has_value());
    CHECK(full.rows[T1].tabPlace->otherWindows.empty());
}

TEST(aRowSaysWhatItsNotificationSaysAndShowsTheOneMarkATabWould)
{
    Host host = sessionedHost();
    // Pane b is the fixture's one unseen notification.
    host.at(host.b).notificationTitle = "Build finished";
    host.at(host.b).notificationBody = "0 errors";
    SessionOutline outline = buildSessionOutline(host.tree(), configuration());
    REQUIRE(outline.rows.size() == RowCount);
    // The pane says it; a parent says it closed, and open leaves it to the
    // pane drawn beneath.
    CHECK_STR_EQ(outline.rows[B].attentionTitle, "Build finished");
    CHECK_STR_EQ(outline.rows[B].attentionBody, "0 errors");
    for (const Row row : { T1, W1, Late }) {
        const OutlineRow closed = closedRow(host.tree(), configuration(), outline.rows[row].id);
        CHECK_STR_EQ(closed.attentionTitle, "Build finished");
        CHECK_STR_EQ(closed.attentionBody, "0 errors");
        CHECK_MSG(closed.accessory != TabAccessory::None, "its one mark, by the tab strip's ranking of everything under it");
        CHECK(outline.rows[row].attentionTitle.empty() && outline.rows[row].attentionBody.empty() && outline.rows[row].accessory == TabAccessory::None);
    }
    for (const Row row : { A, T2, W2, D, E, Early })
        CHECK(outline.rows[row].attentionTitle.empty() && outline.rows[row].attentionBody.empty());
    CHECK(outline.rows[B].accessory == TabAccessory::NotificationDot && !outline.rows[B].accessoryInk);
    CHECK(outline.rows[A].accessory == TabAccessory::None);
    // A pane that stands for its row keeps its own, whatever is beside it.
    host.at(host.c).status.hasUnseenNotification = true;
    host.at(host.c).notificationTitle = "c's";
    outline = buildSessionOutline(host.tree(), configuration());
    CHECK_STR_EQ(outline.rows[T2].attentionTitle, "c's");
    CHECK_MSG(closedRow(host.tree(), configuration(), outline.rows[W1].id).attentionTitle == "Build finished", "a parent says the first under it");

    // The mark is the one a tab's slot would show, the program's colour its
    // ink: progress over a bell, a bell in the colour, the colour alone as a
    // dot, and a notification under them.
    const Ubiquitty::ColorRGBA chosen { 10, 20, 30, 255 };
    Host marks = sessionedHost();
    marks.at(marks.a).status.indicatorColor = chosen;
    marks.at(marks.d).status.hasUnseenBell = true;
    marks.at(marks.e).status.hasUnseenBell = true;
    marks.at(marks.e).status.indicatorColor = chosen;
    marks.at(marks.e).status.hasUnseenNotification = true;
    marks.at(marks.p1).status.hasUnseenBell = true;
    marks.at(marks.p1).status.progress = { OSC::ProgressNormal, 40 };
    const SessionOutline marked = buildSessionOutline(marks.tree(), configuration());
    CHECK(marked.rows[A].accessory == TabAccessory::StatusDot);
    REQUIRE(marked.rows[A].accessoryInk.has_value());
    CHECK(marked.rows[A].accessoryInk->red == 10 && marked.rows[A].accessoryInk->green == 20 && marked.rows[A].accessoryInk->blue == 30);
    CHECK(marked.rows[D].accessory == TabAccessory::Bell && !marked.rows[D].accessoryInk);
    CHECK_MSG(marked.rows[T2].accessory == TabAccessory::ProgressRing, "the fixture's pane c reports progress");
    CHECK(marked.rows[E].accessory == TabAccessory::ColouredBell && marked.rows[E].accessoryInk.has_value());
    CHECK(marked.rows[Early].accessory == TabAccessory::ProgressRing && !marked.rows[Early].accessoryInk);
    // The rule is the tab strip's own, asked with nobody looking at the row.
    for (const OutlineRow& row : marked.rows) {
        Ubiquitty::App::TabAccessoryFacts said {};
        said.hasProgress = row.status.progress.state != OSC::ProgressNone;
        said.hasStatusColour = row.status.indicatorColor.has_value();
        said.hasUnheardBell = row.status.hasUnseenBell;
        said.hasUnseenNotification = row.status.hasUnseenNotification;
        CHECK(row.accessory == Ubiquitty::App::tabAccessory(said));
    }
}

/// An open row says nothing a row drawn beneath it says; closed, or opened
/// by nobody, it states the whole roll-up.
TEST(anOpenRowKeepsItsOwnFactsAndLeavesTheRestToTheRowsBeneath)
{
    Host host = sessionedHost();
    host.at(host.b).notificationTitle = "Build finished";
    host.at(host.b).status.hasUnseenBell = true;
    host.at(host.a).status.indicatorColor = Ubiquitty::ColorRGBA { 10, 20, 30, 255 };
    host.at(host.a).status.tabColor = Ubiquitty::ColorRGBA { 1, 2, 9, 255 };
    host.at(host.d).status.tabColor = Ubiquitty::ColorRGBA { 4, 5, 6, 255 };
    host.at(host.e).status.tabColor = Ubiquitty::ColorRGBA { 4, 5, 6, 255 };
    (void)host.tags.set({ ObjectKind::Window, std::to_underlying(host.w1) }, std::vector<std::string> { "mine" });
    const OutlineConfiguration made = configuration();
    const SessionOutline open = buildSessionOutline(host.tree(), made);
    REQUIRE(open.rows.size() == RowCount);

    for (const Row parent : { T1, W1, Late }) {
        const OutlineRow& row = open.rows[parent];
        const OutlineRow closed = closedRow(host.tree(), made, row.id);
        REQUIRE(row.isExpanded && !closed.isExpanded && closed.isExpandable);
        // Closed: everything under it, as one row.
        CHECK(!closed.secondaryTitle.empty());
        CHECK(closed.status.hasUnseenNotification && closed.status.hasUnseenBell);
        // The colour is the most urgent pane's, which under the workspace is
        // a pane that chose none.
        CHECK(parent == Late || closed.status.indicatorColor.has_value());
        CHECK_STR_EQ(closed.attentionTitle, "Build finished");
        CHECK(closed.accessory != TabAccessory::None);
        CHECK(std::ranges::contains(tagNames(closed), std::string("production")));
        CHECK(closed.collapsedSummary.has_value());
        // Open: its own, and nothing the rows beneath say.
        CHECK(row.secondaryTitle.empty());
        CHECK(!row.status.hasUnseenNotification && !row.status.hasUnseenBell && !row.status.indicatorColor);
        CHECK(row.status.progress.state == OSC::ProgressNone);
        CHECK(row.attentionTitle.empty() && row.attentionBody.empty());
        CHECK(row.accessory == TabAccessory::None && !row.accessoryInk);
        CHECK(!row.collapsedSummary);
        // What stays is the same either way: what it is called, its status
        // with its count, who is attached, and when anything under it moved.
        CHECK(row.title == closed.title && row.labelChips == closed.labelChips);
        CHECK(row.status.kind == closed.status.kind && row.status.count == closed.status.count && row.status.paneCount == closed.status.paneCount && row.status.blockKind == closed.status.blockKind);
        CHECK(row.statusSlot == closed.statusSlot && row.status.badgeCount == closed.status.badgeCount);
        CHECK(row.attendance == closed.attendance && row.activity == closed.activity && row.sinceOutput == closed.sinceOutput && row.sinceInput == closed.sinceInput);
        CHECK(row.open == closed.open && row.own == closed.own && row.tabPlace == closed.tabPlace);
    }
    // A tab's colour is the tab's own, whichever of its panes asked for it:
    // an open tab keeps it, and so does an open window that stands for its
    // one tab. A window of several tabs and a workspace leave it to them.
    CHECK(open.rows[T1].isExpanded && open.rows[T1].status.tabColor && open.rows[T1].status.tabColor->blue == 9);
    CHECK(open.rows[W2].isExpanded && open.rows[W2].foldedTab && open.rows[W2].status.tabColor && open.rows[W2].status.tabColor->blue == 6);
    CHECK(!open.rows[W1].status.tabColor && !open.rows[Late].status.tabColor);
    CHECK(closedRow(host.tree(), made, open.rows[W1].id).status.tabColor.has_value());
    CHECK_MSG(open.rows[T1].status.badgeCount == 1, "and the count of its badges");
    CHECK_MSG(tagNames(open.rows[W1]) == std::vector<std::string> { "mine" }, "the tag a person put on the window is the window's own");
    CHECK(open.rows[T1].tags.empty() && open.rows[Late].tags.empty());
    // The rows that bear them still say them.
    CHECK(open.rows[B].status.hasUnseenNotification && open.rows[B].attentionTitle == "Build finished" && open.rows[B].status.hasUnseenBell);
    CHECK(open.rows[A].status.indicatorColor.has_value() && tagNames(open.rows[A]) == std::vector<std::string> { "production" });
    // A row with nothing drawn beneath it is all there is to say it.
    CHECK(!open.rows[T2].isExpandable && open.rows[T2].status.progress.state == OSC::ProgressNormal && !open.rows[T2].secondaryTitle.empty());

    // A closed row under an open one states its roll-up; the open one above
    // it still states only its own.
    OutlineConfiguration inner = made;
    const std::array closedTab { open.rows[T1].id };
    inner.collapsed = closedTab;
    const SessionOutline mixed = buildSessionOutline(host.tree(), inner);
    const OutlineRow* tab = find(mixed, open.rows[T1].id);
    const OutlineRow* window = find(mixed, open.rows[W1].id);
    REQUIRE(tab != nullptr && window != nullptr);
    CHECK(tab->status.hasUnseenNotification && tab->attentionTitle == "Build finished" && !tab->secondaryTitle.empty());
    CHECK(!window->status.hasUnseenNotification && window->attentionTitle.empty() && window->secondaryTitle.empty());

    // A search opens the way to what it found, and a row it opened is open:
    // closing it in the configuration changes nothing.
    OutlineConfiguration searching = made;
    searching.query = "tests";
    const std::array everyParent { open.rows[T1].id, open.rows[W1].id, open.rows[Late].id };
    searching.collapsed = everyParent;
    const SessionOutline found = buildSessionOutline(host.tree(), searching);
    for (const Row parent : { T1, W1, Late }) {
        const OutlineRow* row = find(found, open.rows[parent].id);
        REQUIRE(row != nullptr);
        CHECK(row->isExpanded && row->secondaryTitle.empty() && !row->status.hasUnseenNotification && row->attentionTitle.empty() && row->accessory == TabAccessory::None && !row->collapsedSummary);
        // Closed they say "tests" as what runs under them; opened by the
        // search they do not show it, and are not found by it.
        CHECK(!row->isMatch);
    }
    CHECK_MSG(found.matchCount == 1 && found.rows.back().id == open.rows[B].id, "the pane that runs it is the one row found");
    // A parent nothing under which matches is not opened, and is found by
    // the second text it shows.
    searching.query = "vim";
    const SessionOutline byRunning = buildSessionOutline(host.tree(), searching);
    CHECK(std::ranges::all_of(byRunning.rows, [](const OutlineRow& row) { return !row.isExpanded || !row.isMatch || row.secondaryTitle.empty(); }));
}

/// The reader says what its own window shows; one row at most answers.
TEST(theRowTheReadersWindowShowsSaysSoAndTheNearestRowOverItHoldsIt)
{
    using Ubiquitty::App::OutlineHere;
    using Ubiquitty::App::OutlinePlace;
    Host host = sessionedHost();
    const SessionTree tree = host.tree();
    const auto built = [&](OutlineConfiguration made) { return buildSessionOutline(tree, made); };
    // Which row answers, and how; nothing for no row. Never two.
    const auto answer = [](const SessionOutline& outline) -> std::pair<std::string, OutlineHere> {
        std::pair<std::string, OutlineHere> found { "", OutlineHere::Elsewhere };
        std::size_t answers = 0;
        for (const OutlineRow& row : outline.rows) {
            if (row.here == OutlineHere::Elsewhere)
                continue;
            ++answers;
            found = { Ubiquitty::Session::Identity::format(row.id), row.here };
        }
        CHECK(answers <= 1);
        return found;
    };
    const auto pane = [](Layout::PaneID id) { return std::to_underlying(id); };
    OutlineConfiguration made = configuration();
    CHECK_MSG(answer(built(made)).second == OutlineHere::Elsewhere, "a reader that names no place is nowhere");

    // By the pane the window types into, and by its session alone.
    made.here = OutlinePlace { .workspaceID = 9, .paneID = pane(host.b), .sessionID = 0 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Pane, host.b), OutlineHere::Here));
    made.here = OutlinePlace { .workspaceID = 9, .paneID = 0, .sessionID = 11 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Pane, host.a), OutlineHere::Here));
    // The pane's number is what is read when both are given.
    made.here = OutlinePlace { .workspaceID = 9, .paneID = pane(host.b), .sessionID = 11 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Pane, host.b), OutlineHere::Here));
    // A row that stands for the pane is it.
    made.here = OutlinePlace { .workspaceID = 9, .paneID = pane(host.c), .sessionID = 0 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Tab, host.t2), OutlineHere::Here));
    made.here = OutlinePlace { .workspaceID = 2, .paneID = pane(host.p1), .sessionID = 0 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Workspace, 2u), OutlineHere::Here));

    // Closed over it, the nearest row that is drawn holds it.
    made.here = OutlinePlace { .workspaceID = 9, .paneID = pane(host.b), .sessionID = 0 };
    const std::array closedTab { ObjectID { ObjectKind::Tab, std::to_underlying(host.t1) } };
    made.collapsed = closedTab;
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Tab, host.t1), OutlineHere::HoldsHere));
    const std::array closedWindow { closedTab[0], ObjectID { ObjectKind::Window, std::to_underlying(host.w1) } };
    made.collapsed = closedWindow;
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Window, host.w1), OutlineHere::HoldsHere));
    const std::array closedWorkspace { ObjectID { ObjectKind::Workspace, 9 } };
    made.collapsed = closedWorkspace;
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Workspace, 9u), OutlineHere::HoldsHere));
    made.collapsed = {};
    // A search that hides it: the nearest row it kept.
    made.query = "vim";
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Window, host.w1), OutlineHere::HoldsHere));
    made.query = {};

    // A pane the tree does not hold in a workspace it does is that
    // workspace; a workspace it does not hold is nowhere, and so is none.
    made.here = OutlinePlace { .workspaceID = 9, .paneID = 4'000'000, .sessionID = 0 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Workspace, 9u), OutlineHere::Here));
    made.here = OutlinePlace { .workspaceID = 9, .paneID = 0, .sessionID = 0 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Workspace, 9u), OutlineHere::Here));
    made.here = OutlinePlace { .workspaceID = 4'000, .paneID = pane(host.b), .sessionID = 12 };
    CHECK(answer(built(made)).second == OutlineHere::Elsewhere);
    made.here = OutlinePlace { .workspaceID = 0, .paneID = pane(host.b), .sessionID = 12 };
    CHECK(answer(built(made)).second == OutlineHere::Elsewhere);
    // The same pane number in another workspace is not it.
    made.here = OutlinePlace { .workspaceID = 2, .paneID = pane(host.b), .sessionID = 0 };
    CHECK(answer(built(made)) == std::pair(id(ObjectKind::Workspace, 2u), OutlineHere::Here));
}

/// What the reader already has open is shown, not attached again: the one
/// answer a row gives names the session the reader holds it open with.
TEST(aRowOfAWorkspaceTheReaderHasOpenIsRevealedNotAttached)
{
    using Ubiquitty::App::OutlineOpenWorkspace;
    using Ubiquitty::Session::AttachOpening;
    Host host = sessionedHost();
    SessionTree tree = host.tree();
    TreeWorkspace kept;
    kept.id = 60;
    kept.name = "kept";
    kept.state = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    kept.windows.emplace_back().tabs.emplace_back().panes.emplace_back();
    tree.workspaces.push_back(kept);
    OutlineConfiguration made = configuration();
    const SessionOutline plain = buildSessionOutline(tree, made);

    const std::array held { OutlineOpenWorkspace { .workspaceID = 9, .sessionID = 500 }, OutlineOpenWorkspace { .workspaceID = 9, .sessionID = 600 }, OutlineOpenWorkspace { .workspaceID = 60, .sessionID = 700 } };
    made.alreadyOpen = held;
    const SessionOutline open = buildSessionOutline(tree, made);
    REQUIRE(open.rows.size() == plain.rows.size());
    for (std::size_t index = 0; index < open.rows.size(); ++index) {
        const OutlineRow& row = open.rows[index];
        const OutlineRow& was = plain.rows[index];
        const bool isInIt = tree.workspaces[row.path.workspace].id == 9;
        if (isInIt && was.open.opening == AttachOpening::BySession) {
            // The reader's own session, and the pane to land on kept.
            CHECK(row.open.opening == AttachOpening::ByRevealing && row.open.sessionID == 500);
            CHECK(row.open.paneID == was.open.paneID && row.open.workspaceID == 9 && row.open.workspaceName == was.open.workspaceName);
        } else {
            CHECK_MSG(row.open == was.open, "what the reader has not open, what cannot be opened and what is restored are as they were");
        }
    }
    CHECK(find(open, { ObjectKind::Pane, std::to_underlying(host.b) })->open.opening == AttachOpening::ByRevealing);
    CHECK(open.rows[Early].open.opening == AttachOpening::BySession);
    CHECK_MSG(find(open, { ObjectKind::Workspace, 60 })->open.opening == AttachOpening::ByRestoring, "nothing runs in it, so nothing of it is open");

    // A reader that knows only the session it attached with: the workspace
    // is the one a pane of that session is in.
    const std::array bySession { OutlineOpenWorkspace { .workspaceID = 0, .sessionID = 21 }, OutlineOpenWorkspace { .workspaceID = 0, .sessionID = 0 }, OutlineOpenWorkspace { .workspaceID = 0, .sessionID = 4'000 } };
    made.alreadyOpen = bySession;
    const SessionOutline early = buildSessionOutline(tree, made);
    CHECK(early.rows[Early].open.opening == AttachOpening::ByRevealing && early.rows[Early].open.sessionID == 21);
    CHECK(early.rows[Late].open.opening == AttachOpening::BySession);
}

/// A sidebar's workspaces start closed but the one holding what its window
/// shows; what the person opened or closed by hand stays so.
TEST(aCompactReadersWorkspacesStartClosedButTheOneThatHoldsHere)
{
    using Ubiquitty::App::OutlineDensity;
    using Ubiquitty::App::OutlineHere;
    using Ubiquitty::App::OutlinePlace;
    Host host = sessionedHost();
    const SessionTree tree = host.tree();
    const ObjectID late { ObjectKind::Workspace, 9 };
    OutlineConfiguration made = configuration();
    made.density = OutlineDensity::Compact;
    // Nothing here: every workspace that has rows under it is closed.
    SessionOutline outline = buildSessionOutline(tree, made);
    CHECK(shape(outline) == (std::vector<std::string> { "0:" + id(ObjectKind::Workspace, 2u), "0:" + id(ObjectKind::Workspace, 9u) }));
    CHECK(outline.rows[1].isExpandable && !outline.rows[1].isExpanded && outline.rows[1].collapsedSummary.has_value());
    // The one that holds here is open, and only it; the rows under it are
    // open as they are in any reader.
    made.here = OutlinePlace { .workspaceID = 9, .paneID = std::to_underlying(host.b), .sessionID = 0 };
    outline = buildSessionOutline(tree, made);
    CHECK_EQ(outline.rows.size(), static_cast<std::size_t>(RowCount));
    CHECK(outline.rows[Late].isExpanded && outline.rows[B].here == OutlineHere::Here);
    // Closed by hand it stays closed, and says it holds here.
    const std::array closed { late };
    made.collapsed = closed;
    outline = buildSessionOutline(tree, made);
    CHECK(outline.rows.size() == 2 && outline.rows[1].here == OutlineHere::HoldsHere);
    // Opened by hand it is open with nothing here.
    made.collapsed = {};
    made.here.reset();
    made.opened = closed;
    CHECK_EQ(buildSessionOutline(tree, made).rows.size(), static_cast<std::size_t>(RowCount));
    // A regular reader's start open, whatever is here.
    made.opened = {};
    made.density = OutlineDensity::Regular;
    CHECK_EQ(buildSessionOutline(tree, made).rows.size(), static_cast<std::size_t>(RowCount));
    // A search opens the way to what it found in either.
    made.density = OutlineDensity::Compact;
    made.query = "tests";
    CHECK(buildSessionOutline(tree, made).matchCount == 1);

    // "Expand all under this row" opens the row and every row under it.
    const Ubiquitty::App::OutlineRowKeys under = Ubiquitty::App::rowsAtAndUnder(tree, ObjectID { ObjectKind::Window, std::to_underlying(host.w1) }, std::nullopt);
    CHECK(under.identities == (std::vector<ObjectID> { { ObjectKind::Window, std::to_underlying(host.w1) }, { ObjectKind::Tab, std::to_underlying(host.t1) }, { ObjectKind::Pane, std::to_underlying(host.a) }, { ObjectKind::Pane, std::to_underlying(host.b) }, { ObjectKind::Tab, std::to_underlying(host.t2) } }));
    CHECK(under.recordedKeys.empty());
    CHECK_EQ(Ubiquitty::App::rowsAtAndUnder(tree, late, std::nullopt).identities.size(), static_cast<std::size_t>(RowCount) - 1);
    const Ubiquitty::App::OutlineRowKeys none = Ubiquitty::App::rowsAtAndUnder(tree, ObjectID { ObjectKind::Tab, 4'000'000 }, std::nullopt);
    CHECK(none.identities.empty() && none.recordedKeys.empty());
    CHECK(Ubiquitty::App::rowsAtAndUnder(tree, std::nullopt, std::nullopt).identities.empty());
}

/// A compact reader gives a second line to a row that needs a person, and a
/// hover lists a row's facts in Core's words and order.
TEST(aRowSaysItsSecondLineAndItsDetailLines)
{
    using Ubiquitty::App::compactSecondLine;
    using Ubiquitty::App::detailLines;
    using Ubiquitty::App::OutlineDetailLine;
    Host host = sessionedHost();
    host.at(host.b).notificationTitle = "Build finished";
    host.at(host.b).notificationBody = "0 errors";
    const OutlineConfiguration made = configuration();
    const SessionOutline outline = buildSessionOutline(host.tree(), made);
    REQUIRE(outline.rows.size() == RowCount);
    // What a notification said; a status that needs a person, in words; and
    // nothing for a row that is merely running.
    CHECK_STR_EQ(compactSecondLine(outline.rows[B]), "Build finished");
    CHECK_STR_EQ(compactSecondLine(outline.rows[A]), "blocked");
    CHECK(compactSecondLine(outline.rows[T2]).empty() && compactSecondLine(outline.rows[Early]).empty());
    // An open parent leaves it to the row beneath; closed, it says it.
    for (const Row parent : { T1, W1, Late })
        CHECK(compactSecondLine(outline.rows[parent]).empty());
    CHECK_STR_EQ(compactSecondLine(closedRow(host.tree(), made, outline.rows[T1].id)), "Build finished");
    CHECK_STR_EQ(compactSecondLine(closedRow(host.tree(), made, outline.rows[W2].id)), "failed 1/2");
    // A notification with a body and no title is said by its body.
    host.at(host.b).notificationTitle.clear();
    CHECK_STR_EQ(compactSecondLine(buildSessionOutline(host.tree(), made).rows[B]), "0 errors");
    host.at(host.b).notificationBody.clear();
    CHECK_MSG(compactSecondLine(buildSessionOutline(host.tree(), made).rows[B]) == "attention", "one with no words is said by its status");

    // The lines of a pane, in order, each fact once; a fact it lacks has no line.
    const std::vector<OutlineDetailLine> pane = detailLines(outline.rows[B]);
    std::vector<std::string> labels;
    for (const OutlineDetailLine& line : pane)
        labels.push_back(line.label);
    CHECK(labels == (std::vector<std::string> { "Program", "Directory", "Size", "Status", "Notification", "Attached", "Last output", "Last input" }));
    const auto value = [](const std::vector<OutlineDetailLine>& lines, std::string_view label) {
        const auto found = std::ranges::find(lines, label, &OutlineDetailLine::label);
        return found != lines.end() ? found->value : std::string();
    };
    CHECK_STR_EQ(value(pane, "Size"), "80×24");
    CHECK_STR_EQ(value(pane, "Notification"), "Build finished: 0 errors");
    CHECK_STR_EQ(value(pane, "Status"), "attention");
    CHECK(!value(pane, "Program").empty() && !value(pane, "Directory").empty() && !value(pane, "Attached").empty());
    CHECK(value(pane, "Last input").ends_with(" ago") || value(pane, "Last input") == "just now");
    CHECK_STR_EQ(value(detailLines(outline.rows[T2]), "Progress"), "40%");
    // Labels and tags, each joined; a workspace nothing runs in says so.
    (void)host.labels.set({ ObjectKind::Tab, std::to_underlying(host.t1) }, "tb1");
    (void)host.tags.set({ ObjectKind::Pane, std::to_underlying(host.a) }, std::vector<std::string> { "red", "review" });
    const SessionOutline labelled = buildSessionOutline(host.tree(), made);
    CHECK_STR_EQ(value(detailLines(labelled.rows[A]), "Tags"), "production, red, review");
    CHECK_STR_EQ(value(detailLines(closedRow(host.tree(), made, labelled.rows[T1].id)), "Labels"), "tb1");
    CHECK(value(detailLines(labelled.rows[B]), "Tags").empty() && value(detailLines(labelled.rows[B]), "Labels").empty());
    OutlineRow recorded;
    recorded.level = ListedLevel::Workspace;
    recorded.workspaceState = Ubiquitty::Session::TreeWorkspaceState::Restorable;
    CHECK(detailLines(recorded) == (std::vector<OutlineDetailLine> { { "State", "restorable" } }));
    CHECK(detailLines(OutlineRow {}).empty());
}

TEST(aTabsNameIsStillSaidOnARowCalledBySomethingAboveIt)
{
    Host host = sessionedHost();
    // The one tab of a labelled window is given a name.
    (void)host.labels.set({ ObjectKind::Window, std::to_underlying(host.w2) }, "deploys");
    (void)host.late->renameTab(host.w2, host.t3, "rollout");
    const SessionOutline outline = buildSessionOutline(host.tree(), configuration());
    REQUIRE(outline.rows.size() == RowCount);
    const OutlineRow& window = outline.rows[W2];
    CHECK_STR_EQ(window.title, "deploys");
    CHECK_MSG(window.secondaryTitle.find("rollout") != std::string::npos, "the tab's name is the row's second text");
    // Unnamed, the second text is what runs there, as it was.
    Host plain = sessionedHost();
    (void)plain.labels.set({ ObjectKind::Window, std::to_underlying(plain.w2) }, "deploys");
    const SessionOutline unnamed = buildSessionOutline(plain.tree(), configuration());
    CHECK(unnamed.rows[W2].secondaryTitle.find("rollout") == std::string::npos);
    // A workspace folding its one window and tab says it too.
    Host folded = sessionedHost();
    (void)folded.early->renameTab(folded.earlyWindow, folded.earlyTab, "scratch");
    const SessionOutline early = buildSessionOutline(folded.tree(), configuration());
    CHECK_STR_EQ(early.rows[Early].title, "early");
    CHECK(early.rows[Early].secondaryTitle.find("scratch") != std::string::npos);
}

UBIQUITTY_TEST_MAIN()
