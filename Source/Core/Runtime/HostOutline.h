// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// HostOutline.h — the outline of a host, as every frontend asks for it.
//
// `App::buildSessionOutline` decides the rows of a host's tree from a
// configuration. Most of that configuration is not a frontend's to choose:
// how a tab's and a pane's title are composed, which names a `[[tags]]` rule
// colours and what a status is drawn in are the person's preferences and
// theme, and Core holds both. What a frontend does choose is small: who is
// looking (the reader's half of a title's inputs), which rows the person has
// closed, and what the person is searching for.
//
// So a frontend fills a `HostOutlineRequest` and nothing else, and the rows
// it is handed are the same rows every other frontend is handed for the same
// request.
//
// `HostOutlineRows` is the same rows for a caller across a language seam:
// one scalar or one string a call, each row read by its index, because a
// vector of structures that hold strings and optionals does not cross every
// seam. Every field of `App::OutlineRow` has an accessor, and so do the words
// a printed listing says for a row (`App::SessionOutlineText.h`), so a
// frontend that draws rows words no status, count or state itself.
//

#include "Core/App/SessionOutline.h"
#include "Core/App/SessionOutlineText.h"
#include "Core/App/StatusRollup.h"
#include "Core/App/TitleCompose.h"
#include "Core/Color.h"
#include "Core/OSC/Activity.h"
#include "Core/Preferences/Preferences.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/TitleComposition.h"
#include "Core/Runtime/LaunchTheme.h"
#include "Core/Session/Identity.h"
#include "Core/Session/SessionTree.h"
#include "Core/Session/Tags.h"
#include "Core/Themes/StatusColors.h"
#include "Core/Themes/Themes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Ubiquitty::Runtime {

/// A workspace a reader has open, with the host it is on.
struct HostOutlineOpenWorkspace {
    /// The host, as `HostOutlineRequest::host` names hosts.
    std::string host;
    App::OutlineOpenWorkspace workspace;
};

/// What a frontend brings to an outline of a host.
struct HostOutlineRequest {
    /// The reader's half of a title's inputs.
    App::TitleViewerFacts viewer;
    /// Whether the reader draws the program's status below the title.
    bool showsProgramStatusSeparately = false; // bool-ok: names the reader's presentation
    /// The rows the person has closed. Every other parent is open.
    std::vector<Session::Identity::ObjectID> collapsed;
    /// The rows with no identity the person has closed, each as its row's
    /// `recordedKey` gave it.
    std::vector<App::OutlineRecordedRowKey> collapsedRecordedRows;
    /// What the person is searching for; empty for no search.
    std::string query;
    /// The catalog index of the theme the outline is drawn in, whose status
    /// colours the rows carry; negative for a reader that is no terminal and
    /// shows no theme of its own, which is handed the colours of the theme
    /// the person's configuration opens in (`readerStatusColors`).
    int themeIndex = -1;
    /// Which status marks the reader's platform has a colour of its own for;
    /// a row's inks say where that colour is the one to draw.
    App::PlatformStatusColors platformStatusColors;

    // ---- What the reader's own windows show --------------------------------
    //
    // A reader states these as it holds them, the same to every host's
    // outline: Core reads the ones that name this host and decides what
    // each row is to them.

    /// What the reader calls the host this outline lists; empty for this
    /// machine. Compared, exactly, with the hosts named below.
    std::string host;
    /// How much room the reader gives a row: a compact one's workspaces
    /// start closed, but the one that holds what its window shows.
    App::OutlineDensity density = App::OutlineDensity::Regular;
    /// The workspaces the person opened by hand.
    std::vector<Session::Identity::ObjectID> opened;
    /// Whether the reader's own window shows a host's pane, and which.
    bool isHereStated = false; // bool-ok: names the one condition it reports
    /// The host that pane is on, as `host` names hosts.
    std::string hereHost;
    /// The place on it: the workspace, and the pane the window types into
    /// by the host's number for it or by its session.
    App::OutlinePlace here;
    /// What the reader has open, on any host: each a host, a workspace and
    /// the session the reader attached with.
    std::vector<HostOutlineOpenWorkspace> alreadyOpen;

    /// States what the reader's own window shows.
    ///
    /// - Parameters:
    ///   - onHost: The host the window's session is attached to, as `host`
    ///     names hosts; empty for this machine.
    ///   - workspaceID: The workspace, as the host numbers it.
    ///   - paneID: The pane the window types into, as the host's layout
    ///     numbers it; zero when only its session is known.
    ///   - sessionID: The session that draws into that pane.
    void setHere(const std::string& onHost, std::uint32_t workspaceID, std::uint32_t paneID, std::uint32_t sessionID)
    {
        isHereStated = true;
        hereHost = onHost;
        here = { .workspaceID = workspaceID, .paneID = paneID, .sessionID = sessionID };
    }

    /// States one workspace the reader has open.
    ///
    /// - Parameters:
    ///   - onHost: The host it is on, as `host` names hosts.
    ///   - workspaceID: The workspace, as the host numbers it; zero when
    ///     only the session is known.
    ///   - sessionID: The session the reader attached with.
    void addAlreadyOpen(const std::string& onHost, std::uint32_t workspaceID, std::uint32_t sessionID)
    {
        alreadyOpen.push_back({ .host = onHost, .workspace = { .workspaceID = workspaceID, .sessionID = sessionID } });
    }

    /// The identity a kind and a number name.
    ///
    /// - Parameters:
    ///   - kind: The kind, as `Session::Identity::ObjectKind` numbers it.
    ///   - number: The identity's number.
    /// - Returns: The identity, or nothing for a number past the last kind
    ///   or an identity of zero, which names nothing.
    [[nodiscard]] static std::optional<Session::Identity::ObjectID> rowIdentity(std::uint8_t kind, std::uint64_t number)
    {
        if (kind >= Session::Identity::objectKindCount || number == 0)
            return std::nullopt;
        return Session::Identity::ObjectID { static_cast<Session::Identity::ObjectKind>(kind), number };
    }

    /// The key of a row that has no identity: a window or a tab of a
    /// workspace nothing runs in.
    ///
    /// - Parameters:
    ///   - workspaceID: The restorable workspace.
    ///   - level: 1 for a window's row, 2 for a tab's, as `App::ListedLevel`
    ///     numbers them.
    ///   - window: The window's index in the record.
    ///   - tab: The tab's index in that window; zero on a window's row.
    /// - Returns: The key, or nothing for another level.
    [[nodiscard]] static std::optional<App::OutlineRecordedRowKey> recordedRowKey(std::uint32_t workspaceID, std::uint8_t level, std::size_t window, std::size_t tab)
    {
        if (level != std::to_underlying(App::ListedLevel::Window) && level != std::to_underlying(App::ListedLevel::Tab))
            return std::nullopt;
        return App::OutlineRecordedRowKey { .workspaceID = workspaceID, .level = static_cast<App::ListedLevel>(level), .window = window, .tab = tab };
    }

    /// Closes a row, so the rows under it are not listed.
    ///
    /// - Parameters:
    ///   - kind: The row's identity's kind.
    ///   - number: The identity's number.
    /// - Returns: Whether the two name a row; closing one already closed is
    ///   closing it.
    bool close(std::uint8_t kind, std::uint64_t number)
    {
        const std::optional<Session::Identity::ObjectID> id = rowIdentity(kind, number);
        if (id && std::ranges::find(collapsed, *id) == collapsed.end())
            collapsed.push_back(*id);
        if (id)
            std::erase(opened, *id);
        return id.has_value();
    }

    /// Opens a row `close` closed.
    ///
    /// - Parameters:
    ///   - kind: The row's identity's kind.
    ///   - number: The identity's number.
    /// - Returns: Whether the two name a row.
    bool open(std::uint8_t kind, std::uint64_t number)
    {
        const std::optional<Session::Identity::ObjectID> id = rowIdentity(kind, number);
        if (id)
            std::erase(collapsed, *id);
        // Remembered for a workspace, which a compact reader starts closed.
        if (id && id->kind == Session::Identity::ObjectKind::Workspace && std::ranges::find(opened, *id) == opened.end())
            opened.push_back(*id);
        return id.has_value();
    }

    /// Opens a row and every row under it, whatever was closed.
    ///
    /// - Parameters:
    ///   - tree: Everything the host holds.
    ///   - kind: The row's identity's kind.
    ///   - number: The identity's number.
    /// - Returns: Whether the tree holds the row.
    bool openAllUnder(const Session::SessionTree& tree, std::uint8_t kind, std::uint64_t number)
    {
        const std::optional<Session::Identity::ObjectID> id = rowIdentity(kind, number);
        return id && openAll(App::rowsAtAndUnder(tree, id, std::nullopt));
    }

    /// Opens a row that has no identity and every row under it.
    ///
    /// - Parameters:
    ///   - tree: Everything the host holds.
    ///   - workspaceID: The restorable workspace.
    ///   - level: 1 for a window's row, 2 for a tab's.
    ///   - window: The window's index in the record.
    ///   - tab: The tab's index in that window; zero on a window's row.
    /// - Returns: Whether the tree holds the row.
    bool openAllUnderRecorded(const Session::SessionTree& tree, std::uint32_t workspaceID, std::uint8_t level, std::size_t window, std::size_t tab)
    {
        const std::optional<App::OutlineRecordedRowKey> key = recordedRowKey(workspaceID, level, window, tab);
        return key && openAll(App::rowsAtAndUnder(tree, std::nullopt, key));
    }

    /// How many rows are closed by hand, for a caller across a language seam
    /// that keeps the fold state on its own side and reads it back after
    /// Core changed it.
    [[nodiscard]] std::size_t collapsedCount() const { return collapsed.size(); }
    /// One closed row's kind, as `Session::Identity::ObjectKind` numbers it.
    ///
    /// - Parameter index: Which, below ``collapsedCount``.
    /// - Returns: The kind; zero past the last.
    [[nodiscard]] std::uint8_t collapsedKind(std::size_t index) const { return index < collapsed.size() ? std::to_underlying(collapsed[index].kind) : 0; }
    /// One closed row's number.
    ///
    /// - Parameter index: Which, below ``collapsedCount``.
    /// - Returns: The number; zero past the last.
    [[nodiscard]] std::uint64_t collapsedNumber(std::size_t index) const { return index < collapsed.size() ? collapsed[index].number : 0; }
    /// How many workspaces are opened by hand.
    [[nodiscard]] std::size_t openedCount() const { return opened.size(); }
    /// One opened row's kind.
    ///
    /// - Parameter index: Which, below ``openedCount``.
    /// - Returns: The kind; zero past the last.
    [[nodiscard]] std::uint8_t openedKind(std::size_t index) const { return index < opened.size() ? std::to_underlying(opened[index].kind) : 0; }
    /// One opened row's number.
    ///
    /// - Parameter index: Which, below ``openedCount``.
    /// - Returns: The number; zero past the last.
    [[nodiscard]] std::uint64_t openedNumber(std::size_t index) const { return index < opened.size() ? opened[index].number : 0; }
    /// How many rows with no identity are closed by hand.
    [[nodiscard]] std::size_t collapsedRecordedCount() const { return collapsedRecordedRows.size(); }
    /// One such row's workspace.
    ///
    /// - Parameter index: Which, below ``collapsedRecordedCount``.
    /// - Returns: The workspace; zero past the last.
    [[nodiscard]] std::uint32_t collapsedRecordedWorkspaceID(std::size_t index) const { return index < collapsedRecordedRows.size() ? collapsedRecordedRows[index].workspaceID : 0; }
    /// One such row's level: 1 a window's, 2 a tab's.
    ///
    /// - Parameter index: Which, below ``collapsedRecordedCount``.
    /// - Returns: The level; zero past the last.
    [[nodiscard]] std::uint8_t collapsedRecordedLevel(std::size_t index) const { return index < collapsedRecordedRows.size() ? std::to_underlying(collapsedRecordedRows[index].level) : 0; }
    /// One such row's window, by its index in the record.
    ///
    /// - Parameter index: Which, below ``collapsedRecordedCount``.
    /// - Returns: The index; zero past the last.
    [[nodiscard]] std::size_t collapsedRecordedWindow(std::size_t index) const { return index < collapsedRecordedRows.size() ? collapsedRecordedRows[index].window : 0; }
    /// One such row's tab, by its index in that window.
    ///
    /// - Parameter index: Which, below ``collapsedRecordedCount``.
    /// - Returns: The index; zero past the last.
    [[nodiscard]] std::size_t collapsedRecordedTab(std::size_t index) const { return index < collapsedRecordedRows.size() ? collapsedRecordedRows[index].tab : 0; }

    /// Opens each of some rows.
    ///
    /// - Parameter rows: The rows, as `App::rowsAtAndUnder` found them.
    /// - Returns: Whether there were any.
    bool openAll(const App::OutlineRowKeys& rows)
    {
        for (const Session::Identity::ObjectID id : rows.identities)
            (void)open(std::to_underlying(id.kind), id.number);
        for (const App::OutlineRecordedRowKey& key : rows.recordedKeys)
            std::erase(collapsedRecordedRows, key);
        return !rows.identities.empty() || !rows.recordedKeys.empty();
    }

    /// Closes a row that has no identity.
    ///
    /// - Parameters:
    ///   - workspaceID: The restorable workspace.
    ///   - level: 1 for a window's row, 2 for a tab's.
    ///   - window: The window's index in the record.
    ///   - tab: The tab's index in that window; zero on a window's row.
    /// - Returns: Whether the level is a window's or a tab's.
    bool closeRecorded(std::uint32_t workspaceID, std::uint8_t level, std::size_t window, std::size_t tab)
    {
        const std::optional<App::OutlineRecordedRowKey> key = recordedRowKey(workspaceID, level, window, tab);
        if (key && std::ranges::find(collapsedRecordedRows, *key) == collapsedRecordedRows.end())
            collapsedRecordedRows.push_back(*key);
        return key.has_value();
    }

    /// Opens a row `closeRecorded` closed.
    ///
    /// - Parameters:
    ///   - workspaceID: The restorable workspace.
    ///   - level: 1 for a window's row, 2 for a tab's.
    ///   - window: The window's index in the record.
    ///   - tab: The tab's index in that window.
    /// - Returns: Whether the level is a window's or a tab's.
    bool openRecorded(std::uint32_t workspaceID, std::uint8_t level, std::size_t window, std::size_t tab)
    {
        const std::optional<App::OutlineRecordedRowKey> key = recordedRowKey(workspaceID, level, window, tab);
        if (key)
            std::erase(collapsedRecordedRows, *key);
        return key.has_value();
    }
};

namespace HostOutlineDetail {

/// A colour as one number, 0xRRGGBBAA.
///
/// - Parameter color: The colour.
/// - Returns: Its four channels, red in the high byte.
[[nodiscard]] constexpr std::uint32_t packed(ColorRGBA color)
{
    return (static_cast<std::uint32_t>(color.red) << 24) | (static_cast<std::uint32_t>(color.green) << 16) | (static_cast<std::uint32_t>(color.blue) << 8) | color.alpha;
}

} // namespace HostOutlineDetail

// The numbers a row's enumerations cross a seam as. The Swift runtime drive's
// values and the page's words spell the same order.
static_assert(std::to_underlying(App::ListedLevel::Workspace) == 0 && std::to_underlying(App::ListedLevel::Window) == 1 && std::to_underlying(App::ListedLevel::Tab) == 2 && std::to_underlying(App::ListedLevel::Pane) == 3);
static_assert(std::to_underlying(Session::Identity::ObjectKind::Workspace) == 0 && std::to_underlying(Session::Identity::ObjectKind::Session) == 1 && std::to_underlying(Session::Identity::ObjectKind::Window) == 2
    && std::to_underlying(Session::Identity::ObjectKind::Tab) == 3 && std::to_underlying(Session::Identity::ObjectKind::Pane) == 4 && std::to_underlying(Session::Identity::ObjectKind::Client) == 5);
static_assert(std::to_underlying(App::StatusKind::None) == 0 && std::to_underlying(App::StatusKind::Idle) == 1 && std::to_underlying(App::StatusKind::Running) == 2 && std::to_underlying(App::StatusKind::Working) == 3
    && std::to_underlying(App::StatusKind::Done) == 4 && std::to_underlying(App::StatusKind::Attention) == 5 && std::to_underlying(App::StatusKind::Blocked) == 6 && std::to_underlying(App::StatusKind::Failed) == 7);
static_assert(std::to_underlying(Themes::StatusColorSlot::Failed) == 0 && std::to_underlying(Themes::StatusColorSlot::Blocked) == 1 && std::to_underlying(Themes::StatusColorSlot::Attention) == 2
    && std::to_underlying(Themes::StatusColorSlot::Done) == 3 && std::to_underlying(Themes::StatusColorSlot::Working) == 4 && std::to_underlying(Themes::StatusColorSlot::Neutral) == 5
    && std::to_underlying(Themes::StatusColorSlot::Progress) == 6);
static_assert(std::to_underlying(OSC::ProgramBlockKind::None) == 0 && std::to_underlying(OSC::ProgramBlockKind::Permission) == 1 && std::to_underlying(OSC::ProgramBlockKind::Question) == 2 && std::to_underlying(OSC::ProgramBlockKind::Auth) == 3);
static_assert(std::to_underlying(App::StatusMark::Progress) == 0 && std::to_underlying(App::StatusMark::ProgressInactive) == 1 && std::to_underlying(App::StatusMark::ProgressError) == 2 && std::to_underlying(App::StatusMark::Notification) == 3);
static_assert(std::to_underlying(Session::TreeWorkspaceState::Live) == 0 && std::to_underlying(Session::TreeWorkspaceState::RestoredAfterHostRestart) == 1 && std::to_underlying(Session::TreeWorkspaceState::Restorable) == 2);

/// An outline's rows, read one field of one row a call.
class HostOutlineRows {
public:
    HostOutlineRows() = default;

    /// Holds an outline.
    ///
    /// - Parameter outline: The rows.
    explicit HostOutlineRows(App::SessionOutline outline)
        : m_outline(std::move(outline))
    {
    }

    /// The rows as Core built them.
    [[nodiscard]] const App::SessionOutline& outline() const { return m_outline; }

    /// How many rows there are to draw.
    [[nodiscard]] std::size_t count() const { return m_outline.rows.size(); }

    /// The tree revision the rows describe.
    [[nodiscard]] std::uint64_t revision() const { return m_outline.revision; }

    /// How many rows match the search; zero without one.
    [[nodiscard]] std::size_t matchCount() const { return m_outline.matchCount; }

    /// Whether the rows are a search's: the reader's query held a term.
    [[nodiscard]] bool isSearching() const { return m_outline.isSearching; }

    /// What the row is: 0 a workspace, 1 a window, 2 a tab, 3 a pane.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t level(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->level) : 0;
    }

    /// How far the row is indented.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] int depth(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->depth : 0;
    }

    /// The kind of the identity the row is addressed by, as `Session::Identity::ObjectKind` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t idKind(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->id.kind) : 0;
    }

    /// The number of the identity the row is addressed by; zero for a row with none.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t idNumber(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->id.number : 0;
    }

    /// Whether the row is closed by its place in a restorable workspace's record.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasRecordedKey(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->recordedKey.has_value() : false;
    }

    /// The restorable workspace a recorded row is in.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t recordedWorkspaceID(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->recordedKey ? row->recordedKey->workspaceID : 0 : 0;
    }

    /// Whether a recorded row is a window's (1) or a tab's (2).
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t recordedLevel(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->recordedKey ? std::to_underlying(row->recordedKey->level) : 0 : 0;
    }

    /// A recorded row's window index.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t recordedWindow(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->recordedKey ? row->recordedKey->window : 0 : 0;
    }

    /// A recorded row's tab index in its window.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t recordedTab(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->recordedKey ? row->recordedKey->tab : 0 : 0;
    }

    /// The window the row also stands for; zero for none.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t foldedWindowNumber(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->foldedWindow ? row->foldedWindow->number : 0 : 0;
    }

    /// The tab the row also stands for; zero for none.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t foldedTabNumber(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->foldedTab ? row->foldedTab->number : 0 : 0;
    }

    /// The pane the row also stands for; zero for none.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t foldedPaneNumber(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->foldedPane ? row->foldedPane->number : 0 : 0;
    }

    /// The workspace's index in the tree.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t pathWorkspace(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->path.workspace : 0;
    }

    /// The window's index in its workspace.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t pathWindow(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->path.window : 0;
    }

    /// The tab's index in its window.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t pathTab(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->path.tab : 0;
    }

    /// The pane's index in its tab.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t pathPane(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->path.pane : 0;
    }

    /// Whether anything runs in the row's workspace: 0 live, 1 restored after the host restarted, 2 restorable.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t workspaceState(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->workspaceState) : 0;
    }

    /// On a workspace's row, whether it ends when the last person detaches.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool doesEndWhenDetached(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->endsWhenDetached : false;
    }

    /// What the row is called.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string title(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->title : std::string {};
    }

    /// What is running in what the row stands for; empty when the title says it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string secondaryTitle(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->secondaryTitle : std::string {};
    }

    /// The row's status, as `App::StatusKind` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t statusKind(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->status.kind) : 0;
    }

    /// How many panes under the row share its status.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t statusCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.count : 0;
    }

    /// How many panes are under the row.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t statusPaneCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.paneCount : 0;
    }

    /// What a blocked program waits for, as `OSC::ProgramBlockKind` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t blockKind(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->status.blockKind) : 0;
    }

    /// The progress state, as OSC 9;4 numbers it; zero for none.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t progressState(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.progress.state : 0;
    }

    /// The progress, in percent.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t progressValue(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.progress.value : 0;
    }

    /// Whether a bell rang under the row that nobody has seen.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasUnseenBell(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.hasUnseenBell : false;
    }

    /// Whether a notification under the row is unseen.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasUnseenNotification(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.hasUnseenNotification : false;
    }

    /// Whether a program under the row set an indicator colour.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasIndicatorColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.indicatorColor.has_value() : false;
    }

    /// The indicator colour, as 0xRRGGBBAA.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t indicatorColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.indicatorColor ? HostOutlineDetail::packed(*row->status.indicatorColor) : 0 : 0;
    }

    /// Whether a program under the row set a tab colour.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasTabColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.tabColor.has_value() : false;
    }

    /// The tab colour, as 0xRRGGBBAA.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t tabColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.tabColor ? HostOutlineDetail::packed(*row->status.tabColor) : 0 : 0;
    }

    /// How many panes under the row wear a badge.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t badgeCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->status.badgeCount : 0;
    }

    /// The theme slot the status mark is drawn in, as `Themes::StatusColorSlot` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t statusSlot(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->statusSlot) : 0;
    }

    /// That slot's colour in the reader's theme, as 0xRRGGBBAA.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t statusColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? HostOutlineDetail::packed(row->statusColor) : 0;
    }

    /// Whether the status mark is drawn in the platform's own colour.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isStatusInkPlatform(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->statusInk.isPlatformColor : false;
    }

    /// The theme's colour for the status mark, as 0xRRGGBBAA; drawn unless the platform's is.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t statusInkColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? HostOutlineDetail::packed(row->statusInk.color) : 0;
    }

    /// Whether the progress mark is drawn in the platform's own colour.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isProgressInkPlatform(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->progressInk.isPlatformColor : false;
    }

    /// Which mark the row's progress is drawn as, as `App::StatusMark` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t progressMark(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->progressMark) : 0;
    }

    /// The theme's colour for the progress mark, as 0xRRGGBBAA; drawn unless the platform's is.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t progressInkColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? HostOutlineDetail::packed(row->progressInk.color) : 0;
    }

    /// Whether the bell mark is drawn in the platform's own colour.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isBellInkPlatform(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->bellInk.isPlatformColor : false;
    }

    /// The theme's colour for the bell mark, as 0xRRGGBBAA; drawn unless the platform's is.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t bellInkColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? HostOutlineDetail::packed(row->bellInk.color) : 0;
    }

    /// Whether the notification mark is drawn in the platform's own colour.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isNotificationInkPlatform(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->notificationInk.isPlatformColor : false;
    }

    /// The theme's colour for the notification mark, as 0xRRGGBBAA; drawn unless the platform's is.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t notificationInkColor(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? HostOutlineDetail::packed(row->notificationInk.color) : 0;
    }

    /// The row's status in words, as a printed listing says it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The words; empty for a row of no status and past the last row.
    [[nodiscard]] std::string statusWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::SessionOutlineTextDetail::statusWords(row->status) : std::string {};
    }

    /// What a blocked program under the row waits for, as a word.
    ///
    /// - Parameter index: The row.
    /// - Returns: The word; empty when nothing is blocked or it did not say.
    [[nodiscard]] std::string blockKindWord(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::string(App::blockKindWord(row->status.blockKind)) : std::string {};
    }

    /// The row's progress in words.
    ///
    /// - Parameter index: The row.
    /// - Returns: The words; empty with no report and past the last row.
    [[nodiscard]] std::string progressWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::SessionOutlineTextDetail::progressWords(row->status.progress) : std::string {};
    }

    /// Who is attached under the row, in words.
    ///
    /// - Parameter index: The row.
    /// - Returns: The words; empty for nobody and past the last row.
    [[nodiscard]] std::string attendanceWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::SessionOutlineTextDetail::attendanceWords(row->attendance) : std::string {};
    }

    /// What a closed row hides, in words.
    ///
    /// - Parameter index: The row.
    /// - Returns: The words; empty for a row that hides nothing.
    [[nodiscard]] std::string collapsedWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->collapsedSummary ? App::SessionOutlineTextDetail::summaryWords(*row->collapsedSummary) : std::string {};
    }

    /// What a row says of its workspace's state.
    ///
    /// - Parameter index: The row.
    /// - Returns: The word on a workspace's row that is not live; empty otherwise.
    [[nodiscard]] std::string stateWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::SessionOutlineTextDetail::stateWords(*row) : std::string {};
    }

    /// What a workspace's row says of how long the workspace lasts.
    ///
    /// - Parameter index: The row.
    /// - Returns: The words on the row of a workspace that ends when the last person detaches; empty otherwise.
    [[nodiscard]] std::string lifetimeWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::SessionOutlineTextDetail::lifetimeWords(*row) : std::string {};
    }

    /// The badge text of the pane the row is or stands for.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string badge(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->badge : std::string {};
    }

    /// When anything under the row last wrote, by the host's clock; zero for never.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t lastOutputUnixMilliseconds(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->activity.lastOutputUnixMilliseconds : 0;
    }

    /// When anything under the row was last typed into; zero for never.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t lastInputUnixMilliseconds(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->activity.lastInputUnixMilliseconds : 0;
    }

    /// Whether anything under the row ever wrote.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isOutputAgeKnown(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->sinceOutput.isKnown : false;
    }

    /// Whole seconds since the last output, by the host's clock.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t secondsSinceOutput(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->sinceOutput.seconds : 0;
    }

    /// Whether anything under the row was ever typed into.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isInputAgeKnown(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->sinceInput.isKnown : false;
    }

    /// Whole seconds since the last input, by the host's clock.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t secondsSinceInput(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->sinceInput.seconds : 0;
    }

    /// Whether anything ever wrote under the row or was typed into there.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isActivityAgeKnown(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->sinceActivity.isKnown : false;
    }

    /// Whole seconds since the newer of the last output and the last input, by the host's clock.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t secondsSinceActivity(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->sinceActivity.seconds : 0;
    }

    /// How many people are attached under the row.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t attachedPeople(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->attendance.people : 0;
    }

    /// How many agents are attached under the row.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t attachedAgents(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->attendance.agents : 0;
    }

    /// The program of the pane the row is or stands for.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string program(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->program : std::string {};
    }

    /// The program's own status string for a row that represents one pane.
    [[nodiscard]] std::string programStatus(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->programStatus : std::string {};
    }

    /// The directory of the pane the row is or stands for.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string directory(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->directory : std::string {};
    }

    /// The width in cells of the pane the row is or stands for.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] int columns(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->columns : 0;
    }

    /// The height in cells of the pane the row is or stands for.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] int rows(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->rows : 0;
    }

    /// Whether the row is the host's current one at its level.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isHostCurrent(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->isHostCurrent : false;
    }

    /// Whether the row has rows under it to open.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isExpandable(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->isExpandable : false;
    }

    /// Whether the rows under it are shown.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isExpanded(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->isExpanded : false;
    }

    /// Whether the row is a closed parent, which carries a summary of what it hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasCollapsedSummary(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary.has_value() : false;
    }

    /// How many windows a closed row hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t collapsedWindows(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? row->collapsedSummary->windows : 0 : 0;
    }

    /// How many tabs a closed row hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t collapsedTabs(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? row->collapsedSummary->tabs : 0 : 0;
    }

    /// How many panes a closed row hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t collapsedPanes(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? row->collapsedSummary->panes : 0 : 0;
    }

    // ---- What a row's actions read -----------------------------------------

    /// How the row is opened, as `Session::AttachOpening` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; `Cannot` past the last row.
    [[nodiscard]] std::uint8_t openOpening(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->open.opening) : 0;
    }

    /// The session opening the row attaches to; zero unless it opens by one.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t openSessionID(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->open.sessionID : 0;
    }

    /// The workspace opening the row lands in.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t openWorkspaceID(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->open.workspaceID : 0;
    }

    /// That workspace's name: what a restore asks the host for.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string openWorkspaceName(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->open.workspaceName : std::string {};
    }

    /// The pane to show once attached, as the host's layout numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t openPaneID(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->open.paneID : 0;
    }

    /// Why the row cannot be opened, as a sentence; empty when it can.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string openObstacle(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::string(Session::attachObstacleSentence(row->open.obstacle)) : std::string {};
    }

    /// How many levels the row is or stands for, each with its own label
    /// and tags.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t ownCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->own.size() : 0;
    }

    /// One of those levels' kind, as `Session::Identity::ObjectKind` numbers it.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::uint8_t ownKind(std::size_t index, std::size_t level) const
    {
        const App::OutlineOwnValues* own = ownAt(index, level);
        return own ? std::to_underlying(own->id.kind) : 0;
    }

    /// One of those levels' number.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::uint64_t ownNumber(std::size_t index, std::size_t level) const
    {
        const App::OutlineOwnValues* own = ownAt(index, level);
        return own ? own->id.number : 0;
    }

    /// One of those levels' own label.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::string ownLabel(std::size_t index, std::size_t level) const
    {
        const App::OutlineOwnValues* own = ownAt(index, level);
        return own ? own->label : std::string {};
    }

    /// The name a person gave one of those levels: a workspace's or a
    /// tab's, which a rename replaces.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::string ownName(std::size_t index, std::size_t level) const
    {
        const App::OutlineOwnValues* own = ownAt(index, level);
        return own ? own->name : std::string {};
    }

    /// How many tags a person put on one of those levels.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::size_t ownTagCount(std::size_t index, std::size_t level) const
    {
        const App::OutlineOwnValues* own = ownAt(index, level);
        return own ? own->tags.size() : 0;
    }

    /// One of those tags.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    ///   - tag: Which tag, in the order they are drawn.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::string ownTag(std::size_t index, std::size_t level, std::size_t tag) const
    {
        const App::OutlineOwnValues* own = ownAt(index, level);
        return own && tag < own->tags.size() ? own->tags[tag] : std::string {};
    }

    /// Whether the row is or stands for a tab that has a place to state.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasTabPlace(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->tabPlace.has_value() : false;
    }

    /// The number of the tab the place is of.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t tabPlaceTab(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->tab.number : 0;
    }

    /// The number of the window that tab stands in.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t tabPlaceWindow(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->window.number : 0;
    }

    /// The tab's place among its window's tabs, from zero.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t tabPlaceIndex(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->index : 0;
    }

    /// How many tabs that window has.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t tabPlaceTabCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->tabCount : 0;
    }

    /// Whether the tab can move one place earlier.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool canTabMoveEarlier(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->canMoveEarlier : false;
    }

    /// Whether the tab can move one place later.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool canTabMoveLater(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->canMoveLater : false;
    }

    /// How many other windows of its workspace have room for the tab.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::size_t tabPlaceOtherWindowCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->otherWindows.size() : 0;
    }

    /// The number of one of those windows.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - window: Which window, in the workspace's order.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::uint64_t tabPlaceOtherWindow(std::size_t index, std::size_t window) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace && window < row->tabPlace->otherWindows.size() ? row->tabPlace->otherWindows[window].window.number : 0;
    }

    /// What the outline calls one of those windows.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - window: Which window, in the workspace's order.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::string tabPlaceOtherWindowTitle(std::size_t index, std::size_t window) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace && window < row->tabPlace->otherWindows.size() ? row->tabPlace->otherWindows[window].title : std::string {};
    }

    /// The place to ask for in one of those windows: after its last tab.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - window: Which window, in the workspace's order.
    /// - Returns: The value; its resting value past the last.
    [[nodiscard]] std::uint32_t tabPlaceOtherWindowIndex(std::size_t index, std::size_t window) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace && window < row->tabPlace->otherWindows.size() ? row->tabPlace->otherWindows[window].index : 0;
    }

    /// The second line a compact reader gives the row, when the row needs
    /// a person (`App::compactSecondLine`).
    ///
    /// - Parameter index: The row.
    /// - Returns: The line; empty for a row that needs none.
    [[nodiscard]] std::string secondLine(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::compactSecondLine(*row) : std::string {};
    }

    /// How many lines a hover or a detail popover shows for the row
    /// (`App::detailLines`).
    ///
    /// - Parameter index: The row.
    /// - Returns: The count; zero past the last row.
    [[nodiscard]] std::size_t detailCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? App::detailLines(*row).size() : 0;
    }

    /// What one of those lines is about.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - line: Which line, in the order they are shown.
    /// - Returns: The label; empty past the last.
    [[nodiscard]] std::string detailLabel(std::size_t index, std::size_t line) const
    {
        const App::OutlineRow* row = rowAt(index);
        const std::vector<App::OutlineDetailLine> lines = row ? App::detailLines(*row) : std::vector<App::OutlineDetailLine> {};
        return line < lines.size() ? lines[line].label : std::string {};
    }

    /// What one of those lines says.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - line: Which line, in the order they are shown.
    /// - Returns: The value; empty past the last.
    [[nodiscard]] std::string detailValue(std::size_t index, std::size_t line) const
    {
        const App::OutlineRow* row = rowAt(index);
        const std::vector<App::OutlineDetailLine> lines = row ? App::detailLines(*row) : std::vector<App::OutlineDetailLine> {};
        return line < lines.size() ? lines[line].value : std::string {};
    }

    /// How many places in its own window the host would accept the row's
    /// tab at.
    ///
    /// - Parameter index: The row.
    /// - Returns: The count; zero on a row that is no tab's.
    [[nodiscard]] std::size_t tabPlaceAcceptedIndexCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace ? row->tabPlace->acceptedIndices.size() : 0;
    }

    /// One of those places.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - place: Which, lowest first.
    /// - Returns: The place; zero past the last.
    [[nodiscard]] std::uint32_t tabPlaceAcceptedIndex(std::size_t index, std::size_t place) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->tabPlace && place < row->tabPlace->acceptedIndices.size() ? row->tabPlace->acceptedIndices[place] : 0;
    }

    /// What the row is to the place the reader's own window shows, as
    /// `App::OutlineHere` numbers it: 0 elsewhere, 1 here, 2 a row that
    /// holds it and is the nearest one drawn.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t here(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->here) : 0;
    }

    /// What the mark beside a current row means, for a tooltip and for a
    /// reader that cannot see it (`App::hostCurrentWords`).
    ///
    /// - Parameter index: The row.
    /// - Returns: The words; empty for a row that is not current.
    [[nodiscard]] std::string hostCurrentWords(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::string(App::hostCurrentWords(*row)) : std::string {};
    }

    /// The title of the newest notification nobody has seen under the row.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string attentionTitle(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->attentionTitle : std::string {};
    }

    /// The body of that notification.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::string attentionBody(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->attentionBody : std::string {};
    }

    /// The one mark the row shows, as `App::TabAccessory` numbers it.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t accessory(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? std::to_underlying(row->accessory) : 0;
    }

    /// Whether the program chose that mark's ink.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool hasAccessoryInk(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->accessoryInk.has_value() : false;
    }

    /// That ink, as 0xRRGGBBAA; zero when the theme's is used.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t accessoryInk(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && row->accessoryInk ? HostOutlineDetail::packed(*row->accessoryInk) : 0;
    }

    /// The most urgent status a closed row hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint8_t collapsedWorstStatus(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? std::to_underlying(row->collapsedSummary->worstStatus) : 0 : 0;
    }

    /// How many hidden panes share that status.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint32_t collapsedWorstStatusCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? row->collapsedSummary->worstStatusCount : 0 : 0;
    }

    /// The newest output a closed row hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t collapsedNewestOutputUnixMilliseconds(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? row->collapsedSummary->newestActivity.lastOutputUnixMilliseconds : 0 : 0;
    }

    /// The newest input a closed row hides.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] std::uint64_t collapsedNewestInputUnixMilliseconds(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->collapsedSummary ? row->collapsedSummary->newestActivity.lastInputUnixMilliseconds : 0 : 0;
    }

    /// Whether the row itself matches the search.
    ///
    /// - Parameter index: The row.
    /// - Returns: The value; its resting value past the last row.
    [[nodiscard]] bool isMatch(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->isMatch : false;
    }

    /// How many labels are drawn beside a row's title.
    ///
    /// - Parameter index: The row.
    /// - Returns: The count; zero past the last row.
    [[nodiscard]] std::size_t labelChipCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->labelChips.size() : 0;
    }

    /// One of a row's labels, outermost first.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - chip: The label's place.
    /// - Returns: The label; empty past the last.
    [[nodiscard]] std::string labelChip(std::size_t index, std::size_t chip) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && chip < row->labelChips.size() ? row->labelChips[chip] : std::string {};
    }

    /// How many tags a row wears.
    ///
    /// - Parameter index: The row.
    /// - Returns: The count; zero past the last row.
    [[nodiscard]] std::size_t tagCount(std::size_t index) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row ? row->tags.size() : 0;
    }

    /// The name of one of a row's tags.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: The name; empty past the last.
    [[nodiscard]] std::string tagName(std::size_t index, std::size_t tag) const
    {
        const Session::ResolvedTag* resolved = tagAt(index, tag);
        return resolved ? resolved->name : std::string {};
    }

    /// Whether one of a row's tags is drawn with its name, rather than as the
    /// dot an untitled tag, or one whose rule hides its name, is drawn as.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: `Session::ResolvedTag::drawsName`; false past the last.
    [[nodiscard]] bool isTagNameShown(std::size_t index, std::size_t tag) const
    {
        const Session::ResolvedTag* resolved = tagAt(index, tag);
        return resolved && resolved->drawsName();
    }

    /// What a screen reader speaks for one of a row's tags: its name, or for
    /// an untitled tag, drawn as a dot, `Session::tagSpokenName`.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: The words; empty past the last.
    [[nodiscard]] std::string tagSpokenName(std::size_t index, std::size_t tag) const
    {
        const Session::ResolvedTag* resolved = tagAt(index, tag);
        return resolved ? Session::tagSpokenName(*resolved) : std::string {};
    }

    /// Whether the reader's rules give one of a row's tags a colour.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: Whether `tagColor` is one to draw.
    [[nodiscard]] bool hasTagColor(std::size_t index, std::size_t tag) const
    {
        const Session::ResolvedTag* resolved = tagAt(index, tag);
        return resolved && resolved->color.set;
    }

    /// The colour of one of a row's tags, as 0xRRGGBBAA.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: The colour; zero for a tag with none.
    [[nodiscard]] std::uint32_t tagColor(std::size_t index, std::size_t tag) const
    {
        const Session::ResolvedTag* resolved = tagAt(index, tag);
        return resolved && resolved->color.set ? HostOutlineDetail::packed({ resolved->color.red, resolved->color.green, resolved->color.blue, resolved->color.alpha }) : 0;
    }

    /// Whether one of a row's tags is drawn loud.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: Whether its rule made it prominent.
    [[nodiscard]] bool isTagLoud(std::size_t index, std::size_t tag) const
    {
        const Session::ResolvedTag* resolved = tagAt(index, tag);
        return resolved && resolved->prominence == Session::TagProminence::Loud;
    }

private:
    /// One row, or null past the last.
    ///
    /// - Parameter index: The row.
    /// - Returns: The row.
    [[nodiscard]] const App::OutlineRow* rowAt(std::size_t index) const
    {
        return index < m_outline.rows.size() ? &m_outline.rows[index] : nullptr;
    }

    /// One level a row is or stands for, or null past either end.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - level: Which level, outermost first.
    /// - Returns: The level.
    [[nodiscard]] const App::OutlineOwnValues* ownAt(std::size_t index, std::size_t level) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && level < row->own.size() ? &row->own[level] : nullptr;
    }

    /// One tag of one row, or null past either end.
    ///
    /// - Parameters:
    ///   - index: The row.
    ///   - tag: The tag's place.
    /// - Returns: The tag.
    [[nodiscard]] const Session::ResolvedTag* tagAt(std::size_t index, std::size_t tag) const
    {
        const App::OutlineRow* row = rowAt(index);
        return row && tag < row->tags.size() ? &row->tags[tag] : nullptr;
    }

    App::SessionOutline m_outline;
};

/// The status colours a reader's outline is drawn in.
///
/// A reader that shows a theme names it. One that is no terminal, or names
/// a theme the catalog does not hold, is drawn in the theme the person's
/// configuration opens in (`launchThemeIndex`): the inks of the terminal a
/// connect from the list would open, not the bare palette a terminal with
/// no theme starts with.
///
/// - Parameters:
///   - values: The person's preferences.
///   - themeIndex: The catalog index of the reader's theme; negative for a
///     reader that shows none.
/// - Returns: That theme's colours; the ones the default palette derives
///   (`Themes::defaultStatusColorSlots()`) only when the catalog is empty.
[[nodiscard]] inline Themes::StatusColors readerStatusColors(const Preferences::Values& values, int themeIndex)
{
    const Themes::Theme* theme = themeIndex >= 0 ? Themes::get(themeIndex) : nullptr;
    if (!theme)
        theme = Themes::get(launchThemeIndex(values));
    return theme ? theme->statusColors() : Themes::StatusColors {};
}

/// Builds the rows of a host's tree for one reader.
///
/// - Parameters:
///   - tree: Everything the host holds.
///   - values: The person's preferences: the tab and pane title
///     compositions and the `[[tags]]` rules are read from them.
///   - statusColors: The colours of the theme the outline is drawn in.
///   - request: The reader's own half.
///   - hostNowUnixMilliseconds: The host's clock now, where the reader has
///     kept it running since the tree was assembled; the tree's own when
///     absent.
/// - Returns: The rows, in the order they are drawn.
[[nodiscard]] inline App::SessionOutline buildHostOutline(const Session::SessionTree& tree, const Preferences::Values& values, const Themes::StatusColors& statusColors, const HostOutlineRequest& request, std::optional<std::uint64_t> hostNowUnixMilliseconds = std::nullopt)
{
    App::OutlineConfiguration configuration;
    // Composed as `Effective::tabTitleComposition` and `paneTitleComposition`
    // compose them, from the header that defines the composition, so a
    // build that trims those accessors (the agent's) draws the same outline.
    configuration.tabTitle = Preferences::TitleComposition::compose(values, Preferences::TitleComposition::Surface::Tab, nullptr);
    configuration.paneTitle = Preferences::TitleComposition::compose(values, Preferences::TitleComposition::Surface::Pane, &configuration.tabTitle);
    if (request.showsProgramStatusSeparately) {
        configuration.tabTitle.showsProgramStatus = false;
        configuration.paneTitle.showsProgramStatus = false;
    }
    configuration.viewer = request.viewer;
    configuration.tagRules = &values.tags;
    configuration.statusColors = statusColors;
    configuration.platformStatusColors = request.platformStatusColors;
    configuration.collapsed = request.collapsed;
    configuration.collapsedRecordedRows = request.collapsedRecordedRows;
    configuration.query = request.query;
    configuration.hostNowUnixMilliseconds = hostNowUnixMilliseconds;
    configuration.density = request.density;
    configuration.opened = request.opened;
    // What the reader's windows show of this host, and of no other.
    if (request.isHereStated && request.hereHost == request.host)
        configuration.here = request.here;
    std::vector<App::OutlineOpenWorkspace> openHere;
    for (const HostOutlineOpenWorkspace& held : request.alreadyOpen) {
        if (held.host == request.host)
            openHere.push_back(held.workspace);
    }
    configuration.alreadyOpen = openHere;
    return App::buildSessionOutline(tree, configuration);
}

} // namespace Ubiquitty::Runtime
