// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SessionOutline.h — the rows an outline of a host's sessions draws.
//
// A sidebar, a switcher and a remote session list each draw the same thing:
// a host's workspaces, their windows, tabs and panes as indented rows, each
// with a title, chips, a status mark and a time. What a row says is decided
// here, from a `Session::SessionTree` and the reader's own configuration, so
// that every frontend renders the rows and none decides them.
//
// The rules a row follows:
//
// - Order is the tree's: workspaces, each one's windows, each window's tabs,
//   each tab's panes, parent before child.
// - A level with one child gets no row for the child. A workspace of one
//   window has no window row, a window of one tab no tab row, a tab of one
//   pane no pane row; the row that remains stands for everything folded into
//   it, carries their identities, and what would have sat under them sits
//   under it. A workspace of one window of one tab of one pane is one row.
// - A title is `App::listedName`'s at the row's own level: a workspace's name,
//   a window's label, a tab's name, a pane's label. Where the level has none,
//   the title comes from further in, by the same rule there, down to the
//   title composed from the pane. So a tab of one labelled pane is called by
//   that label.
// - A name or a label never hides what is running. Where a row's title is a
//   name or a label, the row also carries, as a secondary text, the title
//   composed for it with no name and no label applied: for a pane its
//   program's, for a tab its focused pane's, for a window its shown tab's
//   and for a workspace its front window's.
// - A row keeps every label as a chip: its own where its level's label is a
//   chip, the label of each level folded into it, and on a row that is or
//   stands for one pane the label of the session behind it, except a label the
//   title already says.
// - A row's status, progress, tab colour, badge count, attention and newest
//   activity are the tree's roll-up for its node. The colour slot is
//   `App::statusColorSlot`'s, resolved against the reader's theme.
// - An open row says nothing a row drawn beneath it says. A closed parent
//   and a row with nothing under it state the whole roll-up; a parent whose
//   rows are shown, by the reader or by a search, keeps what is its own: its
//   title, its labels, the tags it earned, its status with its count, who is
//   attached, how many badges, and when anything under it last moved. What
//   it borrowed from below is left to the rows that bear it: the second
//   text that says what is running, the tags worn for the rows under it,
//   the progress, the bell, the notification and its words, the program's
//   colours, and the mark those make. A tab's own name, folded into a row
//   called by something above it, is the row's own and stays, and so does
//   a tab's colour on a row that is the tab or stands for it.
// - What each mark on a row is drawn in is decided here too: the status mark,
//   the progress mark, the bell and the notification each carry an ink, which
//   is the theme's colour or, where the reader said it has a colour of its own
//   for that mark and the theme names none, the reader's own
//   (`App::statusMarkInk`). A listed row is in no window of the reader's, so
//   its progress is tinted as a focused pane's in an active window is: the
//   accent, or the failure colour for a report in its error state. A bell has
//   no platform colour and is drawn in the theme's attention colour.
// - Times are facts, not text: how many seconds ago, or unknown. They are
//   measured against the host's own clock, never the reader's, so two
//   machines whose clocks disagree still agree on an age: the clock as the
//   tree states it, or that clock as a reader has kept it running since.
// - Tags come from two places and a row wears both. The reader's `[[tags]]`
//   rules are matched against each pane's host. The tags a person put on a
//   workspace, window, tab or pane are the host's, on that node, and are
//   drawn in the colour the reader's rules give the name. A parent also
//   wears what is under it, each name once. A rule that matches a profile
//   does not apply here: a profile is what the reader applied to a pane it
//   has open, and a listed pane the reader has not opened has none.
// - A workspace nothing runs in, which the host could restore, is drawn from
//   its record like any other, and every row says which kind of workspace it
//   is in. Its rows have no status, no activity and, below the workspace's
//   own row, no identity, so only that row can be addressed. A recorded
//   window or tab is opened and closed by where it is in the record instead,
//   which holds for as long as the workspace stays restorable: its record
//   does not change until it is restored or forgotten.
// - A closed parent hides everything under it and carries a summary of what
//   it hides.
// - A search keeps the rows that match and their ancestors, opens those
//   ancestors whatever their expansion state, and hides the rest. A row is
//   found by what it displays: its title, its secondary text and its label
//   chips; a row the search opened does not display a second text it
//   borrowed, and is not found by it. A row that is or stands for one pane is also found by that pane's
//   session's label, program and directory. A tag is matched on the row that
//   earned it, by a rule or by a person's hand, not on the parents that wear
//   it for that row.
// - A search compares letters without regard to ASCII case and every other
//   character exactly. Core has no Unicode case folding to use: its Unicode
//   module holds width, grapheme, joining, normalization and encoding tables
//   and `Unicode/ASCII.h` folds ASCII only, as the command palette's matcher
//   does; the scrollback search leaves case to its regular-expression engine.
//   A query in another script must be typed in the case it is displayed in.
//
// Pure: no clock, no preferences store and no theme are read from here. The
// tree states the time and the caller the other two.
//

#include "Base/SwiftInterop.h"
#include "Core/App/StatusRollup.h"
#include "Core/App/TabAccessory.h"
#include "Core/App/TitleCompose.h"
#include "Core/Color.h"
#include "Core/OSC/ProgressIndicator.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Session/AttachTarget.h"
#include "Core/Session/Identity.h"
#include "Core/Session/PaneActivity.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/Tags.h"
#include "Core/Themes/StatusColors.h"
#include "Core/Unicode/ASCII.h"

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

namespace Ubiquitty::App {

/// What names a row that has no identity, so that it can be closed: a window
/// or a tab of a workspace nothing runs in, by its place in that workspace's
/// record.
struct OutlineRecordedRowKey {
    /// The restorable workspace.
    std::uint32_t workspaceID = 0;
    /// Whether the row is a window's or a tab's.
    ListedLevel level = ListedLevel::Window;
    /// The window's index in the record, and for a tab its index in that
    /// window; zero on a window's row.
    std::size_t window = 0;
    std::size_t tab = 0;

    friend bool operator==(const OutlineRecordedRowKey&, const OutlineRecordedRowKey&) = default;
};

/// What the reader of an outline brings to it.
/// How much room a reader gives a row.
enum class OutlineDensity : std::uint8_t {
    /// A list with room for everything: every parent starts open.
    Regular UBIQUITTY_SWIFT_NAME(regular) = 0,
    /// A sidebar: every workspace starts closed but the one holding what the
    /// reader's window shows.
    Compact UBIQUITTY_SWIFT_NAME(compact) = 1,
};

/// A place in a host's tree, as a reader names what its window shows.
struct OutlinePlace {
    /// The workspace, as the host's tree numbers it.
    std::uint32_t workspaceID = 0;
    /// The pane the window types into, as the host's layout numbers it; zero
    /// when the reader knows only its session.
    std::uint32_t paneID = 0;
    /// The session that draws into that pane; read when `paneID` is zero.
    std::uint32_t sessionID = 0;

    friend bool operator==(const OutlinePlace&, const OutlinePlace&) = default;
};

/// A workspace of a host a reader already has open.
struct OutlineOpenWorkspace {
    /// The workspace, as the host's tree numbers it; zero for a reader that
    /// knows only the session, in which case the workspace is the one a
    /// pane of that session is in.
    std::uint32_t workspaceID = 0;
    /// The session the reader attached with, which names what it has open:
    /// a row's `open` hands it back.
    std::uint32_t sessionID = 0;

    friend bool operator==(const OutlineOpenWorkspace&, const OutlineOpenWorkspace&) = default;
};

/// What a row is to the place the reader's own window shows.
enum class OutlineHere : std::uint8_t {
    /// Nothing, or the reader named no place on this host.
    Elsewhere UBIQUITTY_SWIFT_NAME(elsewhere) = 0,
    /// The row is, or stands for, the pane the window types into; or the
    /// workspace, for a reader that named no pane the tree holds.
    Here UBIQUITTY_SWIFT_NAME(here) = 1,
    /// The place is under the row and no row drawn beneath says so: the row
    /// is closed, or a search hides what is under it.
    HoldsHere UBIQUITTY_SWIFT_NAME(holdsHere) = 2,
};

struct OutlineConfiguration {
    /// The reader's `[title]` composition for a tab's row and a pane's row.
    Preferences::Effective::TitleComposition tabTitle {};
    Preferences::Effective::TitleComposition paneTitle {};
    /// The reader's half of a title's inputs.
    TitleViewerFacts viewer;
    /// The reader's `[[tags]]` rules, in file order; null for none.
    const std::vector<Preferences::TagEntry>* tagRules = nullptr;
    /// The reader's theme's status colours.
    Themes::StatusColors statusColors;
    /// Which status marks the reader's platform has a colour of its own for.
    PlatformStatusColors platformStatusColors;
    /// The rows the reader has closed. Every other parent is open.
    std::span<const Session::Identity::ObjectID> collapsed;
    /// The rows with no identity the reader has closed, each as the row's
    /// `recordedKey` gave it.
    std::span<const OutlineRecordedRowKey> collapsedRecordedRows;
    /// What the reader is searching for; empty for no search. Terms are
    /// separated by spaces and every term must be found in a row.
    std::string_view query;
    /// The host's clock now, for a reader that has kept it running since the
    /// tree was stated; ages are measured against the tree's own when absent.
    std::optional<std::uint64_t> hostNowUnixMilliseconds;
    /// How much room the reader gives a row.
    OutlineDensity density = OutlineDensity::Regular;
    /// The workspaces the reader opened by hand, which a compact reader's
    /// workspaces need to be open: they start closed there.
    std::span<const Session::Identity::ObjectID> opened;
    /// What the reader's own window shows of this host, when it shows any of
    /// it: the row that is it says so, and a closed row over it says it
    /// holds it.
    std::optional<OutlinePlace> here;
    /// The workspaces of this host the reader already has open. A row in
    /// one is opened by showing what is open, not by attaching again.
    std::span<const OutlineOpenWorkspace> alreadyOpen;
};

/// How long ago something happened, as a fact a frontend words itself.
struct OutlineAge {
    /// Whether it ever happened.
    bool isKnown = false; // bool-ok: names the one condition it reports
    /// Whole seconds since, by the host's clock; zero when unknown.
    std::uint64_t seconds = 0;

    friend bool operator==(const OutlineAge&, const OutlineAge&) = default;
};

/// What a closed parent hides.
struct OutlineSummary {
    /// How many windows, tabs and panes are under the row. A level the row is
    /// itself, or is above none of, counts zero.
    std::uint32_t windows = 0;
    std::uint32_t tabs = 0;
    std::uint32_t panes = 0;
    /// The most urgent status under the row and how many panes share it.
    StatusKind worstStatus = StatusKind::None;
    std::uint32_t worstStatusCount = 0;
    /// The newest output and input under the row.
    Session::PaneActivity newestActivity;

    friend bool operator==(const OutlineSummary&, const OutlineSummary&) = default;
};

/// One level a row is or stands for, with what that node itself holds: its
/// own label and the tags a person put on it, apart from everything the row
/// wears from the levels folded into it, from below it, and from the
/// reader's rules. What an editor shows and what `label.set` and `tags.set`
/// replace.
struct OutlineOwnValues {
    /// The node: a workspace, a window, a tab, a pane, or the session behind
    /// a pane.
    Session::Identity::ObjectID id;
    /// Its own label, empty when it has none.
    std::string label;
    /// Its own hand-put tags, in the order they are drawn.
    std::vector<std::string> tags;
    /// The name a person gave it, which `workspace.rename` and `tab.rename`
    /// replace: a workspace's or a tab's. Empty for one with none, and on a
    /// level that takes no name.
    std::string name;

    friend bool operator==(const OutlineOwnValues&, const OutlineOwnValues&) = default;
};

/// A window a tab could be moved to.
struct OutlineTabDestination {
    /// The window.
    Session::Identity::ObjectID window;
    /// What the outline calls that window, as its own row is titled.
    std::string title;
    /// The place to ask for there: after its last tab. A drop among its
    /// tabs is accepted at every place from zero to this one.
    std::uint32_t index = 0;

    friend bool operator==(const OutlineTabDestination&, const OutlineTabDestination&) = default;
};

/// Where a tab stands among its window's tabs, and where the host would let
/// it go.
struct OutlineTabPlace {
    /// The tab, and the window it stands in.
    Session::Identity::ObjectID tab;
    Session::Identity::ObjectID window;
    /// Its place among that window's tabs, from zero, and how many there are.
    std::uint32_t index = 0;
    std::uint32_t tabCount = 0;
    /// Whether it can move one place earlier, and one later.
    bool canMoveEarlier = false; // bool-ok: names the one condition it reports
    bool canMoveLater = false; // bool-ok: names the one condition it reports
    /// The places in its own window the host would accept it at, lowest
    /// first: every place but the one it stands at. A drop is offered at
    /// these and nowhere else.
    std::vector<std::uint32_t> acceptedIndices;
    /// The other windows of its workspace that have room for it, in the
    /// workspace's order, each with what it is called and the place to ask
    /// for.
    std::vector<OutlineTabDestination> otherWindows;

    friend bool operator==(const OutlineTabPlace&, const OutlineTabPlace&) = default;
};

/// One row of the outline.
struct OutlineRow {
    /// What the row is.
    ListedLevel level = ListedLevel::Workspace;
    /// How far the row is indented: zero for a workspace, one more per level
    /// of rows above it.
    int depth = 0;
    /// The identity the row is addressed, opened and closed by.
    Session::Identity::ObjectID id;
    /// What the row is closed by when it has no identity: set on a window's
    /// and a tab's row in a workspace nothing runs in.
    std::optional<OutlineRecordedRowKey> recordedKey;
    /// The window, the tab and the pane the row also stands for, where a
    /// level with one child folded them into it.
    std::optional<Session::Identity::ObjectID> foldedWindow;
    std::optional<Session::Identity::ObjectID> foldedTab;
    std::optional<Session::Identity::ObjectID> foldedPane;
    /// Where the innermost thing the row stands for is in the tree; the
    /// indices below that level are zero.
    Session::TreePanePath path;

    /// Whether anything runs in the workspace the row is in: live, restored
    /// after the host restarted, or only restorable.
    Session::TreeWorkspaceState workspaceState = Session::TreeWorkspaceState::Live;
    /// On a workspace's row, whether the workspace ends when the last person
    /// on it detaches.
    bool endsWhenDetached = false; // bool-ok: names the one state it reports

    /// What the row is called.
    std::string title;
    /// What is running in what the row stands for, composed with no name and
    /// no label; empty when the title already says it.
    std::string secondaryTitle;
    /// The labels drawn beside the title, outermost first; none for none.
    std::vector<std::string> labelChips;
    /// The tags the row wears, each name once, in rule order per pane.
    Session::ResolvedTags tags;

    /// The node's status with its progress, tab colour, badge count and what
    /// nobody has seen yet.
    RolledStatus status;
    /// The theme slot the status mark is drawn in, and that slot's colour in
    /// the reader's theme.
    Themes::StatusColorSlot statusSlot = Themes::StatusColorSlot::Neutral;
    ColorRGBA statusColor {};
    /// The badge text of the pane the row is or stands for; empty on a row
    /// that stands for several.
    std::string badge;
    /// When anything under the row last wrote and was last typed into.
    Session::PaneActivity activity;
    OutlineAge sinceOutput;
    OutlineAge sinceInput;
    /// Who is attached under the row.
    Session::TreeAttendance attendance;

    /// The program, directory and grid of the pane the row is or stands
    /// for; empty and zero on a row that stands for several.
    std::string program;
    /// The program's own status string, separate from its title and roll-up.
    std::string programStatus;
    std::string directory;
    int columns = 0;
    int rows = 0;

    /// Whether the row is the host's current one at its level: the window in
    /// front, the tab its window shows, the pane its tab types into.
    bool isHostCurrent = false; // bool-ok: names the one state it reports
    /// Whether the row has rows under it to open.
    bool isExpandable = false; // bool-ok: names the one condition it reports
    /// Whether the rows under it are shown.
    bool isExpanded = false; // bool-ok: names the one state it reports
    /// What the row hides, when it is a parent that is not open.
    std::optional<OutlineSummary> collapsedSummary;
    /// Whether the row itself matches the search; false without one.
    bool isMatch = false; // bool-ok: names the one condition it reports

    // ---- What a row's actions read -----------------------------------------
    //
    // Facts Core states so that no frontend derives them from neighbouring
    // rows, a name or a title.

    /// What opening the row attaches to: the session and the pane to show
    /// once attached, the name a workspace nothing runs in is restored by,
    /// or that it cannot be opened and why (`Session::attachTargetOf`, which
    /// the connect rule lands a workspace through too).
    Session::AttachTarget open;
    /// Every level the row is or stands for, outermost first, each with its
    /// own label and its own hand-put tags: the workspace, the window, the
    /// tab and the pane where the row is or folded one, and the session
    /// behind that pane. A level with no identity, as in a workspace nothing
    /// runs in, is not among them: there is nothing to set it on.
    std::vector<OutlineOwnValues> own;
    /// Where the tab the row is or stands for sits and may go; none on a row
    /// that is no tab's, and in a workspace nothing runs in.
    std::optional<OutlineTabPlace> tabPlace;
    /// The words of the newest notification nobody has seen, on the pane the
    /// row is or stands for, or else on the first pane under it that has
    /// one; empty when `status.hasUnseenNotification` is false.
    std::string attentionTitle;
    std::string attentionBody;
    /// The one mark the row shows for what runs under it, ranked by the rule
    /// a tab's accessory slot is ranked by (`App::tabAccessory`), with a
    /// row never the one the reader is looking at: a progress ring, a bell,
    /// the program's colour as a dot, a notification's dot, or none.
    App::TabAccessory accessory = App::TabAccessory::None;
    /// The ink that mark is drawn in when the program chose one: its
    /// indicator colour, for `StatusDot` and `ColouredBell`. None for a mark
    /// drawn in the theme's own ink.
    std::optional<ColorRGBA> accessoryInk;
    /// What the row is to the place the reader's own window shows. One row
    /// at most answers `Here` or `HoldsHere`.
    OutlineHere here = OutlineHere::Elsewhere;
    /// What each mark on the row is drawn in: the theme's colour, or the
    /// reader's platform's own. The progress, bell and notification inks are
    /// read only on a row that has that mark.
    StatusMarkInk statusInk;
    StatusMarkInk progressInk;
    StatusMarkInk bellInk;
    StatusMarkInk notificationInk;
    /// Which mark the row's progress is drawn as, and so which of its
    /// platform's colours `progressInk` means when it says the platform's.
    StatusMark progressMark = StatusMark::Progress;
    /// How long ago anything last happened under the row, for a place that
    /// shows one age: the newer of `sinceOutput` and `sinceInput`, unknown
    /// when neither is known.
    OutlineAge sinceActivity;
};

/// What the mark beside a current row means, for a tooltip and for a
/// reader that cannot see it.
///
/// - Parameter row: The row.
/// - Returns: The words for a row that is the host's current one at its
///   level; empty for one that is not.
[[nodiscard]] constexpr std::string_view hostCurrentWords(const OutlineRow& row)
{
    if (!row.isHostCurrent)
        return {};
    switch (row.level) {
    case ListedLevel::Workspace:
        return "The workspace in front on this host";
    case ListedLevel::Window:
        return "The window in front on this host";
    case ListedLevel::Tab:
        return "The tab its window shows";
    case ListedLevel::Pane:
        return "The pane its tab types into";
    }
    return {};
}

/// The outline: the rows to draw, in order.
struct SessionOutline {
    /// The tree revision the rows describe.
    std::uint64_t revision = 0;
    std::vector<OutlineRow> rows;
    /// How many rows match the search; zero without one.
    std::size_t matchCount = 0;
    /// Whether the reader's query held a term, so the rows are a search's:
    /// only what matches and the way to it, opened whatever was closed.
    bool isSearching = false; // bool-ok: names the one state it reports
};

namespace SessionOutlineDetail {

/// What a row stands for below its own level, as folding found it.
struct Stand {
    /// The window, set on a window's row and where a workspace folded one.
    const Session::TreeWindow* window = nullptr;
    /// The tab, set on a tab's row and where a window folded one.
    const Session::TreeTab* tab = nullptr;
    /// The pane, where a tab folded one.
    const Session::TreePane* pane = nullptr;
    /// Whether the row's second text is the name of a tab folded into it:
    /// the row's own, where any other second text is borrowed from below.
    bool isSecondaryTheTabsName = false; // bool-ok: names the one condition it reports
};

/// A row before it is known whether it is shown.
struct Candidate {
    OutlineRow row;
    /// The candidate above it, or `noParent`.
    std::size_t parent = 0;
    /// What the row hides when closed.
    OutlineSummary summary;
    /// The tags the row earned itself, by rule or by hand.
    Session::ResolvedTags earnedTags;
    /// Whether anything under it matches the search.
    bool hasMatchBelow = false; // bool-ok: names the one condition it reports
    /// The second text that is the row's own; empty where the row's is
    /// borrowed from what is under it.
    std::string ownSecondaryTitle;
    /// Whether the row matches the search by what it shows when open.
    bool isMatchWhenOpen = false; // bool-ok: names the one condition it reports
    /// The pane the row is or stands for; null on a row that is several.
    const Session::TreePane* standsForPane = nullptr;
};

inline constexpr std::size_t noParent = static_cast<std::size_t>(-1);

/// Reports whether `needle` occurs in `haystack`, ignoring ASCII case.
///
/// - Parameters:
///   - haystack: The text searched.
///   - needle: The term; never empty.
/// - Returns: Whether it occurs.
[[nodiscard]] inline bool containsFolded(std::string_view haystack, std::string_view needle)
{
    return !std::ranges::search(haystack, needle, [](char a, char b) { return Unicode::toASCIILower(a) == Unicode::toASCIILower(b); }).empty();
}

/// Splits a query into its terms.
///
/// - Parameter query: The reader's text.
/// - Returns: The runs between spaces and tabs; none for a blank query.
[[nodiscard]] inline std::vector<std::string_view> queryTerms(std::string_view query)
{
    std::vector<std::string_view> terms;
    std::size_t start = 0;
    while (start < query.size()) {
        const std::size_t end = std::min(query.find_first_of(" \t", start), query.size());
        if (end > start)
            terms.push_back(query.substr(start, end - start));
        start = end + 1;
    }
    return terms;
}

/// Reports whether every term is found in at least one of a row's texts.
///
/// - Parameters:
///   - terms: The query's terms; none matches nothing.
///   - texts: What the row can be found by.
///   - tags: The tags the row earned itself, by rule or by hand, found by
///     name.
/// - Returns: Whether the row matches.
[[nodiscard]] inline bool matchesAll(std::span<const std::string_view> terms, std::span<const std::string_view> texts, const Session::ResolvedTags& tags)
{
    if (terms.empty())
        return false;
    return std::ranges::all_of(terms, [&](std::string_view term) {
        return std::ranges::any_of(texts, [&](std::string_view text) { return containsFolded(text, term); })
            || std::ranges::any_of(tags, [&](const Session::ResolvedTag& tag) { return containsFolded(tag.name, term); });
    });
}

/// Adds the tags of `more` that `tags` does not hold yet, keeping the order
/// of both.
///
/// Two tags are one by `Session::isSameTag`: titled tags by name, untitled
/// ones by colour and icon, so two rules' dots of different colours are both
/// kept.
///
/// - Parameters:
///   - tags: The tags gathered so far.
///   - more: Tags to add.
inline void appendDistinctTags(Session::ResolvedTags& tags, const Session::ResolvedTags& more)
{
    Session::appendTags(tags, more);
}

/// Adds the tags a person put on a node, in the colour the reader's rules
/// give each name.
///
/// - Parameters:
///   - tags: The tags gathered so far; a name already present is kept.
///   - names: The node's hand-put tags.
///   - rules: The reader's `[[tags]]` rules.
inline void appendHandTags(Session::ResolvedTags& tags, std::span<const std::string> names, const std::vector<Preferences::TagEntry>& rules)
{
    Session::ResolvedTags resolved;
    for (const std::string& name : names)
        resolved.push_back(Session::resolveManualTag(rules, name));
    appendDistinctTags(tags, resolved);
}

/// How long before `now` a stamp was taken.
///
/// - Parameters:
///   - stampUnixMilliseconds: The instant, zero for never.
///   - nowUnixMilliseconds: The host's clock when it described its state.
/// - Returns: The age; zero seconds for a stamp taken after that.
[[nodiscard]] constexpr OutlineAge ageOf(std::uint64_t stampUnixMilliseconds, std::uint64_t nowUnixMilliseconds)
{
    if (stampUnixMilliseconds == 0)
        return {};
    return { .isKnown = true, .seconds = nowUnixMilliseconds > stampUnixMilliseconds ? (nowUnixMilliseconds - stampUnixMilliseconds) / 1000 : 0 };
}

/// Fills what every row takes from its node's roll-up.
///
/// - Parameters:
///   - row: The row.
///   - node: The row's node in the tree: a workspace, window, tab or pane.
///   - configuration: The reader's configuration.
///   - hostNowUnixMilliseconds: The host's clock, which the node's instants
///     are measured against.
template<typename Node>
void fillRollUp(OutlineRow& row, const Node& node, const OutlineConfiguration& configuration, std::uint64_t hostNowUnixMilliseconds)
{
    row.status = node.status;
    row.statusSlot = statusColorSlot(node.status.kind);
    row.statusColor = configuration.statusColors.color(row.statusSlot);
    row.statusInk = { .isPlatformColor = false, .color = row.statusColor };
    const OSC::ProgressTint tint = OSC::progressTint(node.status.progress.state, OSC::PaneFocusState::Focused, OSC::PaneLayout::SinglePane, OSC::WindowActivityState::Active);
    row.progressMark = progressStatusMark(tint);
    row.progressInk = statusMarkInk(row.progressMark, configuration.statusColors, configuration.platformStatusColors);
    row.bellInk = { .isPlatformColor = false, .color = configuration.statusColors.color(Themes::StatusColorSlot::Attention) };
    row.notificationInk = statusMarkInk(StatusMark::Notification, configuration.statusColors, configuration.platformStatusColors);
    row.activity = node.activity;
    row.sinceOutput = ageOf(node.activity.lastOutputUnixMilliseconds, hostNowUnixMilliseconds);
    row.sinceInput = ageOf(node.activity.lastInputUnixMilliseconds, hostNowUnixMilliseconds);
    row.sinceActivity = ageOf(std::max(node.activity.lastOutputUnixMilliseconds, node.activity.lastInputUnixMilliseconds), hostNowUnixMilliseconds);
    row.attendance = node.attendance;
}

/// The levels below its workspace a row is or stands for.
struct RowNodes {
    const Session::TreeWindow* window = nullptr;
    const Session::TreeTab* tab = nullptr;
    const Session::TreePane* pane = nullptr;
};

/// Reports whether a session draws into a pane of a workspace.
///
/// - Parameters:
///   - workspace: The workspace.
///   - sessionID: The session; zero is none.
/// - Returns: Whether one of its panes is that session's.
[[nodiscard]] inline bool holdsSession(const Session::TreeWorkspace& workspace, std::uint32_t sessionID)
{
    if (sessionID == 0)
        return false;
    for (const Session::TreeWindow& window : workspace.windows) {
        for (const Session::TreeTab& tab : window.tabs) {
            if (std::ranges::any_of(tab.panes, [&](const Session::TreePane& pane) { return pane.sessionID == sessionID; }))
                return true;
        }
    }
    return false;
}

/// The session a reader holds a workspace open with.
///
/// - Parameters:
///   - workspace: A workspace something runs in.
///   - alreadyOpen: What the reader said it has open on this host.
/// - Returns: The session the first entry for this workspace names; nothing
///   when the reader has it open nowhere.
[[nodiscard]] inline std::optional<std::uint32_t> sessionHoldingOpen(const Session::TreeWorkspace& workspace, std::span<const OutlineOpenWorkspace> alreadyOpen)
{
    for (const OutlineOpenWorkspace& held : alreadyOpen) {
        if (held.sessionID == 0)
            continue;
        if (held.workspaceID != 0 ? held.workspaceID == workspace.id : holdsSession(workspace, held.sessionID))
            return held.sessionID;
    }
    return std::nullopt;
}

/// Fills what a row's actions read: what opening it attaches to, each
/// level's own label and tags, where its tab may go, what its notification
/// says, and the one mark it shows.
///
/// - Parameters:
///   - row: The row, its level, path and roll-up filled.
///   - workspace: The workspace the row is in.
///   - nodes: The levels below the workspace the row is or stands for.
///   - windowTitles: What the outline calls each window of the workspace,
///     in the workspace's order.
///   - alreadyOpen: The workspaces of this host the reader has open.
inline void fillActionFacts(OutlineRow& row, const Session::TreeWorkspace& workspace, const RowNodes& nodes, std::span<const std::string> windowTitles, std::span<const OutlineOpenWorkspace> alreadyOpen)
{
    using Session::Identity::ObjectID;
    using Session::Identity::ObjectKind;
    switch (row.level) {
    case ListedLevel::Workspace:
        row.open = Session::attachTargetOf(workspace);
        break;
    case ListedLevel::Window:
        row.open = nodes.window ? Session::attachTargetOf(workspace, *nodes.window) : Session::AttachTarget {};
        break;
    case ListedLevel::Tab:
        row.open = nodes.tab ? Session::attachTargetOf(workspace, *nodes.tab) : Session::AttachTarget {};
        break;
    case ListedLevel::Pane:
        row.open = nodes.pane ? Session::attachTargetOf(workspace, *nodes.pane) : Session::AttachTarget {};
        break;
    }
    // What the reader already has open is shown, not attached again: the
    // one answer names the session the reader holds it open with, and keeps
    // the pane to land on.
    if (row.open.opening == Session::AttachOpening::BySession) {
        if (const std::optional<std::uint32_t> held = sessionHoldingOpen(workspace, alreadyOpen)) {
            row.open.opening = Session::AttachOpening::ByRevealing;
            row.open.sessionID = *held;
        }
    }

    const auto addOwn = [&](ObjectID id, const std::string& label, const std::vector<std::string>& tags, const std::string& name) {
        if (id.isValid())
            row.own.push_back({ .id = id, .label = label, .tags = tags, .name = name });
    };
    if (row.level == ListedLevel::Workspace)
        addOwn({ ObjectKind::Workspace, workspace.id }, workspace.label, workspace.tags, workspace.name);
    if (nodes.window)
        addOwn({ ObjectKind::Window, std::to_underlying(nodes.window->id) }, nodes.window->label, nodes.window->tags, {});
    if (nodes.tab)
        addOwn({ ObjectKind::Tab, std::to_underlying(nodes.tab->id) }, nodes.tab->label, nodes.tab->tags, nodes.tab->name);
    if (nodes.pane) {
        addOwn({ ObjectKind::Pane, std::to_underlying(nodes.pane->id) }, nodes.pane->label, nodes.pane->tags, {});
        addOwn({ ObjectKind::Session, nodes.pane->sessionID }, nodes.pane->sessionLabel, nodes.pane->sessionTags, {});
    }

    // A tab's window is the one its row's path names: the row's own, or the
    // one a level above folded.
    if (nodes.tab && !workspace.isRestorable() && row.path.window < workspace.windows.size()) {
        const Session::TreeWindow& within = workspace.windows[row.path.window];
        const auto found = std::ranges::find_if(within.tabs, [&](const Session::TreeTab& tab) { return &tab == nodes.tab; });
        if (found != within.tabs.end()) {
            OutlineTabPlace place;
            place.tab = { ObjectKind::Tab, std::to_underlying(nodes.tab->id) };
            place.window = { ObjectKind::Window, std::to_underlying(within.id) };
            place.index = static_cast<std::uint32_t>(found - within.tabs.begin());
            place.tabCount = static_cast<std::uint32_t>(within.tabs.size());
            place.canMoveEarlier = place.index > 0;
            place.canMoveLater = place.index + 1 < place.tabCount;
            for (std::uint32_t target = 0; target < place.tabCount; ++target) {
                if (target != place.index)
                    place.acceptedIndices.push_back(target);
            }
            for (std::size_t index = 0; index < workspace.windows.size(); ++index) {
                const Session::TreeWindow& other = workspace.windows[index];
                if (&other != &within && other.tabs.size() < static_cast<std::size_t>(Session::Layout::maxTabsPerSurface))
                    place.otherWindows.push_back({ .window = { ObjectKind::Window, std::to_underlying(other.id) }, .title = index < windowTitles.size() ? windowTitles[index] : std::string(), .index = static_cast<std::uint32_t>(other.tabs.size()) });
            }
            row.tabPlace = std::move(place);
        }
    }

    // The notification of the pane the row is or stands for, else of the
    // first pane under it that has one nobody has seen.
    const Session::TreePane* about = nodes.pane;
    const auto seek = [&](const Session::TreeTab& tab) {
        for (const Session::TreePane& pane : tab.panes) {
            if (!about && pane.facts.attention.hasUnseenNotification)
                about = &pane;
        }
    };
    if (!about && nodes.tab) {
        seek(*nodes.tab);
    } else if (!about) {
        for (const Session::TreeWindow& window : workspace.windows) {
            if (nodes.window && &window != nodes.window)
                continue;
            for (const Session::TreeTab& tab : window.tabs)
                seek(tab);
        }
    }
    if (about && about->facts.attention.hasUnseenNotification) {
        row.attentionTitle = about->facts.attention.notificationTitle;
        row.attentionBody = about->facts.attention.notificationBody;
    }

    App::TabAccessoryFacts said {};
    said.hasProgress = row.status.progress.state != OSC::ProgressNone;
    said.hasStatusColour = row.status.indicatorColor.has_value();
    said.hasUnheardBell = row.status.hasUnseenBell;
    said.hasUnseenNotification = row.status.hasUnseenNotification;
    row.accessory = App::tabAccessory(said);
    if (row.accessory == App::TabAccessory::StatusDot || row.accessory == App::TabAccessory::ColouredBell)
        row.accessoryInk = row.status.indicatorColor;
}

/// Leaves an open row with what is its own.
///
/// What it borrowed from the rows now drawn beneath it is theirs to say:
/// the second text that says what is running, the tags worn for them, the
/// progress, the bell, the notification and its words, the program's
/// indicator colour, and the mark those make. A tab's colour stays on a
/// row that is the tab or stands for it.
///
/// - Parameter candidate: The row, open, with what it earned itself.
inline void keepOwnFacts(Candidate& candidate)
{
    OutlineRow& row = candidate.row;
    row.secondaryTitle = candidate.ownSecondaryTitle;
    row.programStatus.clear();
    row.tags = candidate.earnedTags;
    row.status.progress = {};
    row.status.hasUnseenBell = false;
    row.status.hasUnseenNotification = false;
    row.status.indicatorColor.reset();
    // A tab's colour is asked of the tab, whichever of its panes asked: on
    // a row that is a tab or stands for one it is the row's own.
    if (row.level != ListedLevel::Tab && !row.foldedTab)
        row.status.tabColor.reset();
    row.attentionTitle.clear();
    row.attentionBody.clear();
    row.accessory = App::TabAccessory::None;
    row.accessoryInk.reset();
}

/// What a row hides when closed, but for its counts.
///
/// - Parameter row: The row, its roll-up filled.
/// - Returns: The summary with no counts.
[[nodiscard]] inline OutlineSummary summaryOf(const OutlineRow& row)
{
    OutlineSummary summary;
    summary.worstStatus = row.status.kind;
    summary.worstStatusCount = row.status.count;
    summary.newestActivity = row.activity;
    return summary;
}

} // namespace SessionOutlineDetail

/// Builds the rows an outline draws for a tree.
///
/// The rules are the ones at the top of this file. Two calls with the same
/// tree and configuration return the same rows.
///
/// - Parameters:
///   - tree: Everything the host holds.
///   - configuration: The reader's title settings, tag rules, theme colours,
///     closed rows and search.
/// - Returns: The rows, in the order they are drawn.
[[nodiscard]] inline SessionOutline buildSessionOutline(const Session::SessionTree& tree, const OutlineConfiguration& configuration)
{
    using namespace SessionOutlineDetail;
    using Session::Identity::ObjectID;
    using Session::Identity::ObjectKind;

    const std::vector<std::string_view> terms = queryTerms(configuration.query);
    const bool isSearching = !terms.empty();
    static const std::vector<Preferences::TagEntry> noRules;
    const std::vector<Preferences::TagEntry>& rules = configuration.tagRules ? *configuration.tagRules : noRules;
    const std::uint64_t hostNow = configuration.hostNowUnixMilliseconds.value_or(tree.hostNowUnixMilliseconds);

    std::vector<Candidate> candidates;
    // What the outline calls each window of the workspace being drawn.
    std::span<const std::string> windowTitlesOfWorkspace;
    // The row is matched before it is moved from: what it is found by views
    // its own strings.
    const auto addCandidate = [&](OutlineRow&& row, std::size_t parent, const RowNodes& nodes, OutlineSummary summary, std::string ownSecondaryTitle) {
        const Session::TreePane* pane = nodes.pane;
        fillActionFacts(row, tree.workspaces[row.path.workspace], nodes, windowTitlesOfWorkspace, configuration.alreadyOpen);
        // The second text first, so that an open row is matched without one
        // it borrowed.
        std::vector<std::string_view> texts { row.secondaryTitle, row.title };
        texts.insert(texts.end(), row.labelChips.begin(), row.labelChips.end());
        if (pane)
            texts.insert(texts.end(), { std::string_view(pane->sessionLabel), std::string_view(pane->program), std::string_view(pane->directory()) });
        row.isMatch = matchesAll(terms, texts, row.tags);
        texts.front() = ownSecondaryTitle;
        const bool isMatchWhenOpen = matchesAll(terms, texts, row.tags); // bool-ok: a local verdict
        row.depth = parent == noParent ? 0 : candidates[parent].row.depth + 1;
        row.workspaceState = tree.workspaces[row.path.workspace].state;
        if (!row.id.isValid() && row.level != ListedLevel::Pane)
            row.recordedKey = OutlineRecordedRowKey { tree.workspaces[row.path.workspace].id, row.level, row.path.window, row.level == ListedLevel::Tab ? row.path.tab : 0 };
        Session::ResolvedTags earned = row.tags;
        candidates.push_back({ .row = std::move(row), .parent = parent, .summary = summary, .earnedTags = std::move(earned), .hasMatchBelow = false, .ownSecondaryTitle = std::move(ownSecondaryTitle), .isMatchWhenOpen = isMatchWhenOpen, .standsForPane = pane });
        return candidates.size() - 1;
    };
    // What runs is said beside a title that is a name or a label, and not
    // beside a title that is already it.
    const auto setSecondary = [](OutlineRow& row, std::string_view running) {
        if (running != row.title)
            row.secondaryTitle = running;
    };
    // A label the title already says, or a chip already there, is not said again.
    const auto addChip = [](OutlineRow& row, std::string_view label) {
        if (label.empty() || label == row.title || std::ranges::find(row.labelChips, label) != row.labelChips.end())
            return;
        row.labelChips.emplace_back(label);
    };
    // What a row that is one pane, or stands for one, takes from it.
    const auto takePane = [&](OutlineRow& row, const Session::TreePane& pane) {
        // A pane on no named host is matched as a local window is: under no
        // host name at all.
        const std::array<std::string_view, 1> hostNames { pane.title.hostName };
        appendDistinctTags(row.tags, Session::resolveTags(rules, std::span(hostNames).first(pane.title.hostName.empty() ? 0 : 1), {}));
        appendHandTags(row.tags, pane.tags, rules);
        appendHandTags(row.tags, pane.sessionTags, rules);
        // The label of the session behind the pane, which a row is found by.
        addChip(row, pane.sessionLabel);
        row.badge = pane.badge;
        row.program = pane.program;
        row.programStatus = pane.title.programStatus;
        row.directory = pane.directory();
        row.columns = pane.columns();
        row.rows = pane.rows();
    };
    const auto composeListedTitle = [&](const PaneTitleFacts& pane, const Preferences::Effective::TitleComposition& composition, Session::TitleSurface surface) {
        TitleViewerFacts viewer = configuration.viewer;
        if (!viewer.windowHostName.empty() && pane.hostName != viewer.windowHostName)
            viewer.doPaneHeadersNameHost = true;
        return composeTitle(pane, viewer, composition, surface);
    };
    // A row stands for a tab: a tab of one pane folds that pane into it.
    const auto standForTab = [&](OutlineRow& row, Stand& stand, const Session::TreeTab& tab) {
        stand.tab = &tab;
        row.programStatus = tab.titlePane().title.programStatus;
        if (tab.panes.size() != 1)
            return;
        stand.pane = &tab.panes.front();
        row.foldedPane = ObjectID { ObjectKind::Pane, std::to_underlying(stand.pane->id) };
        addChip(row, stand.pane->label);
        takePane(row, *stand.pane);
    };
    // A row stands for a window: a window of one tab folds that tab into it.
    const auto standForWindow = [&](OutlineRow& row, Stand& stand, const Session::TreeWindow& window, std::string_view tabTitle) {
        stand.window = &window;
        if (window.tabs.size() != 1)
            return;
        const Session::TreeTab& tab = window.tabs.front();
        row.foldedTab = ObjectID { ObjectKind::Tab, std::to_underlying(tab.id) };
        // A name a person gave the tab is still said when the row is called
        // by something above it, a window's label or a workspace's name: it
        // is the row's second text, where a tab's own row would have it first.
        if (!tab.name.empty() && tabTitle != row.title) {
            row.secondaryTitle = tabTitle;
            stand.isSecondaryTheTabsName = true;
        }
        addChip(row, tab.label);
        appendHandTags(row.tags, tab.tags, rules);
        standForTab(row, stand, tab);
    };

    for (std::size_t workspaceIndex = 0; workspaceIndex < tree.workspaces.size(); ++workspaceIndex) {
        const Session::TreeWorkspace& workspace = tree.workspaces[workspaceIndex];

        // Titles are composed from the leaves up: a tab's from its focused
        // pane, a window's from its tabs, a workspace's from its windows. A
        // tab with no name and one pane is called what that pane is, so a
        // label on the pane is not lost with the pane's row.
        // Beside each, the same title with no name and no label applied: what
        // is running there.
        std::vector<std::vector<std::string>> tabTitles(workspace.windows.size());
        std::vector<std::vector<std::string>> tabRunning(workspace.windows.size());
        std::vector<std::string> windowTitles;
        std::vector<std::string> windowRunning;
        for (std::size_t windowIndex = 0; windowIndex < workspace.windows.size(); ++windowIndex) {
            const Session::TreeWindow& window = workspace.windows[windowIndex];
            for (const Session::TreeTab& tab : window.tabs) {
                const Session::TreePane& titled = tab.titlePane();
                std::string title = composeListedTitle(titled.title, configuration.tabTitle, Session::TitleSurface::Tab).title;
                if (tab.name.empty() && tab.panes.size() == 1)
                    title = listedName(ListedLevel::Pane, title, titled.label).title;
                tabTitles[windowIndex].push_back(std::move(title));
                // A tab answers to its window's title when it has none of its
                // own, so what runs there is composed with neither.
                PaneTitleFacts unnamed = titled.title;
                unnamed.tabTitle.clear();
                unnamed.windowTitle.clear();
                tabRunning[windowIndex].push_back(composeListedTitle(unnamed, configuration.tabTitle, Session::TitleSurface::Tab).title);
            }
            windowTitles.push_back(listedWindowTitle(window.label, tabTitles[windowIndex], window.hostActiveTab).title);
            windowRunning.push_back(listedWindowTitle({}, tabRunning[windowIndex], window.hostActiveTab).title);
        }
        windowTitlesOfWorkspace = windowTitles;

        const auto emitPaneRows = [&](std::size_t parent, std::size_t windowIndex, std::size_t tabIndex) {
            const Session::TreeTab& tab = workspace.windows[windowIndex].tabs[tabIndex];
            for (std::size_t paneIndex = 0; paneIndex < tab.panes.size(); ++paneIndex) {
                const Session::TreePane& pane = tab.panes[paneIndex];
                OutlineRow row;
                row.level = ListedLevel::Pane;
                row.id = { ObjectKind::Pane, std::to_underlying(pane.id) };
                row.path = { workspaceIndex, windowIndex, tabIndex, paneIndex };
                row.title = composeListedTitle(pane.title, configuration.paneTitle, Session::TitleSurface::Pane).title;
                PaneTitleFacts unlabelled = pane.title;
                unlabelled.paneLabel.clear();
                setSecondary(row, composeListedTitle(unlabelled, configuration.paneTitle, Session::TitleSurface::Pane).title);
                takePane(row, pane);
                fillRollUp(row, pane, configuration, hostNow);
                row.isHostCurrent = pane.isHostFocused;
                addCandidate(std::move(row), parent, { .window = nullptr, .tab = nullptr, .pane = &pane }, {}, {});
            }
        };
        const auto emitTabRows = [&](std::size_t parent, std::size_t windowIndex) {
            const Session::TreeWindow& window = workspace.windows[windowIndex];
            for (std::size_t tabIndex = 0; tabIndex < window.tabs.size(); ++tabIndex) {
                const Session::TreeTab& tab = window.tabs[tabIndex];
                OutlineRow row;
                row.level = ListedLevel::Tab;
                row.id = { ObjectKind::Tab, std::to_underlying(tab.id) };
                row.path = { workspaceIndex, windowIndex, tabIndex, 0 };
                const ListedName name = listedName(ListedLevel::Tab, tabTitles[windowIndex][tabIndex], tab.label);
                row.title = name.title;
                setSecondary(row, tabRunning[windowIndex][tabIndex]);
                addChip(row, name.labelChip);
                appendHandTags(row.tags, tab.tags, rules);
                fillRollUp(row, tab, configuration, hostNow);
                row.isHostCurrent = tab.isHostActive;
                Stand stand;
                standForTab(row, stand, tab);
                row.isExpandable = !stand.pane;
                OutlineSummary summary = summaryOf(row);
                summary.panes = static_cast<std::uint32_t>(tab.panes.size());
                const std::size_t candidate = addCandidate(std::move(row), parent, { .window = nullptr, .tab = &tab, .pane = stand.pane }, summary, {});
                if (!stand.pane)
                    emitPaneRows(candidate, windowIndex, tabIndex);
            }
        };
        // What sits under a row that is a window or stands for one.
        const auto emitUnderWindow = [&](std::size_t parent, std::size_t windowIndex, const Stand& stand) {
            if (!stand.tab)
                emitTabRows(parent, windowIndex);
            else if (!stand.pane)
                emitPaneRows(parent, windowIndex, 0);
        };
        const auto tabCount = [](const Session::TreeWorkspace& counted) {
            std::uint32_t count = 0;
            for (const Session::TreeWindow& window : counted.windows)
                count += static_cast<std::uint32_t>(window.tabs.size());
            return count;
        };
        const auto paneCount = [](const Session::TreeWindow& counted) {
            std::uint32_t count = 0;
            for (const Session::TreeTab& tab : counted.tabs)
                count += static_cast<std::uint32_t>(tab.panes.size());
            return count;
        };

        OutlineRow workspaceRow;
        workspaceRow.level = ListedLevel::Workspace;
        workspaceRow.id = { ObjectKind::Workspace, workspace.id };
        workspaceRow.path.workspace = workspaceIndex;
        const ListedName workspaceName = listedName(ListedLevel::Workspace, listedWorkspaceTitle(workspace.name, workspace.defaultName(), windowTitles, workspace.hostActiveWindow), workspace.label);
        workspaceRow.title = workspaceName.title;
        setSecondary(workspaceRow, listedWorkspaceTitle({}, {}, windowRunning, workspace.hostActiveWindow));
        addChip(workspaceRow, workspaceName.labelChip);
        appendHandTags(workspaceRow.tags, workspace.tags, rules);
        fillRollUp(workspaceRow, workspace, configuration, hostNow);
        workspaceRow.endsWhenDetached = workspace.endsWhenDetached;
        Stand workspaceStand;
        // A workspace of one window folds that window into its row.
        if (workspace.windows.size() == 1) {
            const Session::TreeWindow& window = workspace.windows.front();
            workspaceRow.foldedWindow = ObjectID { ObjectKind::Window, std::to_underlying(window.id) };
            addChip(workspaceRow, window.label);
            appendHandTags(workspaceRow.tags, window.tags, rules);
            standForWindow(workspaceRow, workspaceStand, window, tabTitles[0].size() == 1 ? std::string_view(tabTitles[0].front()) : std::string_view());
        }
        workspaceRow.isExpandable = !workspace.windows.empty() && !workspaceStand.pane;
        OutlineSummary workspaceSummary = summaryOf(workspaceRow);
        workspaceSummary.windows = static_cast<std::uint32_t>(workspace.windows.size());
        workspaceSummary.tabs = tabCount(workspace);
        for (const Session::TreeWindow& window : workspace.windows)
            workspaceSummary.panes += paneCount(window);
        std::string workspaceOwnSecondary = workspaceStand.isSecondaryTheTabsName ? workspaceRow.secondaryTitle : std::string();
        const std::size_t workspaceCandidate = addCandidate(std::move(workspaceRow), noParent, { .window = workspaceStand.window, .tab = workspaceStand.tab, .pane = workspaceStand.pane }, workspaceSummary, std::move(workspaceOwnSecondary));
        if (workspaceStand.window) {
            emitUnderWindow(workspaceCandidate, 0, workspaceStand);
            continue;
        }
        for (std::size_t windowIndex = 0; windowIndex < workspace.windows.size(); ++windowIndex) {
            const Session::TreeWindow& window = workspace.windows[windowIndex];
            OutlineRow row;
            row.level = ListedLevel::Window;
            row.id = { ObjectKind::Window, std::to_underlying(window.id) };
            row.path = { workspaceIndex, windowIndex, 0, 0 };
            row.title = windowTitles[windowIndex];
            setSecondary(row, windowRunning[windowIndex]);
            appendHandTags(row.tags, window.tags, rules);
            fillRollUp(row, window, configuration, hostNow);
            row.isHostCurrent = window.isHostActive;
            Stand stand;
            standForWindow(row, stand, window, tabTitles[windowIndex].size() == 1 ? std::string_view(tabTitles[windowIndex].front()) : std::string_view());
            row.isExpandable = !stand.pane;
            OutlineSummary summary = summaryOf(row);
            summary.tabs = static_cast<std::uint32_t>(window.tabs.size());
            summary.panes = paneCount(window);
            std::string ownSecondary = stand.isSecondaryTheTabsName ? row.secondaryTitle : std::string();
            const std::size_t candidate = addCandidate(std::move(row), workspaceCandidate, { .window = &window, .tab = stand.tab, .pane = stand.pane }, summary, std::move(ownSecondary));
            emitUnderWindow(candidate, windowIndex, stand);
        }
    }

    // A parent wears its own tags first, then what is under it in the order
    // it is drawn.
    for (const Candidate& candidate : candidates) {
        for (std::size_t ancestor = candidate.parent; ancestor != noParent; ancestor = candidates[ancestor].parent)
            appendDistinctTags(candidates[ancestor].row.tags, candidate.earnedTags);
    }

    if (isSearching) {
        // From the last row up, so that a row knows whether the search
        // opens it before its parent asks whether it matches.
        for (std::size_t index = candidates.size(); index-- > 0;) {
            Candidate& candidate = candidates[index];
            if (candidate.row.isExpandable && candidate.hasMatchBelow)
                candidate.row.isMatch = candidate.isMatchWhenOpen;
            if (candidate.parent != noParent && (candidate.row.isMatch || candidate.hasMatchBelow))
                candidates[candidate.parent].hasMatchBelow = true;
        }
    }

    // The candidate that is the place the reader's window shows: the row
    // that is or stands for its pane, else its workspace's row.
    std::size_t hereCandidate = noParent;
    if (configuration.here && configuration.here->workspaceID != 0) {
        const OutlinePlace& here = *configuration.here;
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            const OutlineRow& row = candidates[index].row;
            const Session::TreeWorkspace& workspace = tree.workspaces[row.path.workspace];
            if (workspace.id != here.workspaceID)
                continue;
            if (row.level == ListedLevel::Workspace && hereCandidate == noParent)
                hereCandidate = index;
            const Session::TreePane* pane = candidates[index].standsForPane;
            if (pane && (here.paneID != 0 ? std::to_underlying(pane->id) == here.paneID : (here.sessionID != 0 && pane->sessionID == here.sessionID))) {
                hereCandidate = index;
                break;
            }
        }
    }
    // The workspace's own candidate, which a compact reader keeps open.
    std::size_t hereWorkspace = hereCandidate;
    while (hereWorkspace != noParent && candidates[hereWorkspace].parent != noParent)
        hereWorkspace = candidates[hereWorkspace].parent;

    SessionOutline outline;
    outline.revision = tree.revision;
    outline.isSearching = isSearching;
    // Whether each candidate's children are shown; a child reads its parent's.
    std::vector<bool> doesShowChildren(candidates.size(), false);
    std::vector<bool> isShown(candidates.size(), false);
    // Where each shown candidate's row is in the outline.
    std::vector<std::size_t> rowOf(candidates.size(), noParent);
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        Candidate& candidate = candidates[index];
        OutlineRow& row = candidate.row;
        const bool isParentOpen = candidate.parent == noParent || (isShown[candidate.parent] && doesShowChildren[candidate.parent]);
        if (isSearching) {
            isShown[index] = row.isMatch || candidate.hasMatchBelow;
            row.isExpanded = row.isExpandable && candidate.hasMatchBelow;
        } else {
            isShown[index] = isParentOpen;
            // A row is closed by its identity, or with none by its place
            // in its workspace's record.
            const bool isClosedByHand = row.recordedKey
                ? std::ranges::find(configuration.collapsedRecordedRows, *row.recordedKey) != configuration.collapsedRecordedRows.end()
                : std::ranges::find(configuration.collapsed, row.id) != configuration.collapsed.end();
            // A compact reader's workspaces start closed, but the one that
            // holds what its window shows and the ones opened by hand.
            const bool doesStartClosed = configuration.density == OutlineDensity::Compact && row.level == ListedLevel::Workspace && index != hereWorkspace
                && std::ranges::find(configuration.opened, row.id) == configuration.opened.end();
            row.isExpanded = row.isExpandable && !isClosedByHand && !doesStartClosed;
        }
        doesShowChildren[index] = row.isExpanded;
        if (!isShown[index])
            continue;
        if (row.isExpandable && !row.isExpanded)
            row.collapsedSummary = candidate.summary;
        if (row.isExpanded)
            keepOwnFacts(candidate);
        if (row.isMatch)
            ++outline.matchCount;
        rowOf[index] = outline.rows.size();
        outline.rows.push_back(std::move(row));
    }
    // The row that is here says so; when it is not drawn, the nearest row
    // over it that is says it holds it.
    if (hereCandidate != noParent) {
        if (rowOf[hereCandidate] != noParent) {
            outline.rows[rowOf[hereCandidate]].here = OutlineHere::Here;
        } else {
            for (std::size_t ancestor = candidates[hereCandidate].parent; ancestor != noParent; ancestor = candidates[ancestor].parent) {
                if (rowOf[ancestor] != noParent) {
                    outline.rows[rowOf[ancestor]].here = OutlineHere::HoldsHere;
                    break;
                }
            }
        }
    }
    return outline;
}

/// The rows a reader closes and opens that are one row and everything
/// under it, whatever is closed now.
struct OutlineRowKeys {
    /// The rows that have an identity.
    std::vector<Session::Identity::ObjectID> identities;
    /// The rows that have none, each by its place in its workspace's record.
    std::vector<OutlineRecordedRowKey> recordedKeys;
};

/// Finds a row and every row under it, for a reader that opens them all.
///
/// - Parameters:
///   - tree: Everything the host holds.
///   - identity: The row's identity, for a row that has one.
///   - recordedKey: The row's key, for a row that has none.
/// - Returns: The row first, then the rows under it in the order they are
///   drawn; none when the tree holds no such row.
[[nodiscard]] inline OutlineRowKeys rowsAtAndUnder(const Session::SessionTree& tree, std::optional<Session::Identity::ObjectID> identity, std::optional<OutlineRecordedRowKey> recordedKey)
{
    OutlineRowKeys keys;
    // Every parent open and no search: the rows the tree has.
    const SessionOutline whole = buildSessionOutline(tree, {});
    const auto top = std::ranges::find_if(whole.rows, [&](const OutlineRow& row) {
        return row.recordedKey ? (recordedKey && *row.recordedKey == *recordedKey) : (identity && row.id.isValid() && row.id == *identity);
    });
    if (top == whole.rows.end())
        return keys;
    for (auto row = top; row != whole.rows.end() && (row == top || row->depth > top->depth); ++row) {
        if (row->recordedKey)
            keys.recordedKeys.push_back(*row->recordedKey);
        else if (row->id.isValid())
            keys.identities.push_back(row->id);
    }
    return keys;
}

} // namespace Ubiquitty::App
