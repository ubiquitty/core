// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// SessionRows.h — a host's tree, read as one row a session.
//
// Some readers want a list, not an outline: `ubq ls`, the MCP server's
// `terminal_list`, a target resolved by a session's id or label, and the flat
// tables a frontend drew before it draws the outline. `sessionRows` is that
// list, derived from the `SessionTree` a listing assembles. It is never on a
// wire and never stored: a reader that holds a tree asks for its rows.
//
// A row says where its session stands, by identity and by what each level
// is called: the workspace, the window, the tab and the pane. No reader
// works out a session's workspace from a name.
//
// A workspace the host could restore is one row, as a list always showed it:
// nothing runs in it to list pane by pane, and what a person picks it by is
// its name and where its first pane was.
//

#include "Core/Session/SessionTree.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace Ubiquitty::Session {

/// One session of a host, with where it stands; or one workspace the host
/// could restore.
struct SessionRow {
    /// What the row is listed and chosen by: the session, or for a workspace
    /// nothing runs in, the workspace's id. The host numbers both from one
    /// space, so no two rows share one.
    std::uint32_t id = 0;
    /// The session; zero on the row of a workspace nothing runs in.
    std::uint32_t sessionID = 0;
    /// The session's label, empty when it has none.
    std::string label;

    /// The workspace the session is a pane of, what it is named and labelled,
    /// and whether anything runs in it.
    std::uint32_t workspaceID = 0;
    std::string workspaceName;
    std::string workspaceLabel;
    TreeWorkspaceState workspaceState = TreeWorkspaceState::Live;
    /// Whether the workspace ends when the last person on it detaches.
    bool doesWorkspaceEndWhenDetached = false; // bool-ok: names the one state it reports
    /// The window holding the session's pane, and its label.
    std::uint32_t windowID = 0;
    std::string windowLabel;
    /// The tab holding the pane, the title a person gave it, and its label.
    std::uint32_t tabID = 0;
    std::string tabTitle;
    std::string tabLabel;
    /// The pane the session draws into, and its label.
    std::uint32_t paneID = 0;
    std::string paneLabel;

    /// The title the session's program set.
    std::string title;
    /// The directory the program reported or runs in.
    std::string currentDirectory;
    /// The name of the program holding the terminal.
    std::string foregroundProcess;
    /// The pane's grid.
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    /// Whether the session's shell is at its prompt.
    bool isShellAtPrompt = false; // bool-ok: names the one state it reports
    /// How many clients are attached, and how many of them are people; each
    /// stops at 255.
    std::uint8_t attendeeCount = 0;
    std::uint8_t humanAttendeeCount = 0;
    /// When the session was created, in seconds since the Unix epoch on the
    /// host's clock; zero when unknown.
    std::uint64_t createdUnixSeconds = 0;
    /// Whether the host adopted the session's terminal from another process.
    bool isAdopted = false; // bool-ok: names the one state it reports

    /// Whether the shell is at its prompt.
    [[nodiscard]] bool isAtPrompt() const { return isShellAtPrompt; }
    /// Whether the session's workspace ends when the last person detaches.
    [[nodiscard]] bool endsWhenDetached() const { return doesWorkspaceEndWhenDetached; }
    /// Whether the row is a workspace the host could restore, in which
    /// nothing runs.
    [[nodiscard]] bool isRestorable() const { return workspaceState == TreeWorkspaceState::Restorable; }
    /// Whether the session's workspace was recreated from its record after
    /// the host restarted.
    [[nodiscard]] bool wasRestoredAfterHostRestart() const { return workspaceState == TreeWorkspaceState::RestoredAfterHostRestart; }
    /// Whether more than one client is attached.
    [[nodiscard]] bool isShared() const { return attendeeCount > 1; }

    friend bool operator==(const SessionRow&, const SessionRow&) = default;
};

/// Reads a tree as rows: one for every pane of a live workspace a session
/// draws into, by session, then one for every workspace the host could
/// restore, by workspace.
///
/// - Parameter tree: The host's tree.
/// - Returns: The rows. A pane no session draws into has none.
[[nodiscard]] inline std::vector<SessionRow> sessionRows(const SessionTree& tree)
{
    const auto saturated = [](std::size_t count) { return static_cast<std::uint8_t>(std::min<std::size_t>(count, 255)); };
    const auto describe = [&](SessionRow& row, const TreePane& pane) {
        row.label = pane.sessionLabel;
        row.paneLabel = pane.label;
        row.title = pane.title.programTitle;
        row.currentDirectory = pane.directory();
        row.columns = static_cast<std::uint16_t>(std::clamp(pane.columns(), 0, 0xFFFF));
        row.rows = static_cast<std::uint16_t>(std::clamp(pane.rows(), 0, 0xFFFF));
    };
    std::vector<SessionRow> live;
    std::vector<SessionRow> restorable;
    for (const TreeWorkspace& workspace : tree.workspaces) {
        SessionRow of;
        of.workspaceID = workspace.id;
        of.workspaceName = workspace.name;
        of.workspaceLabel = workspace.label;
        of.workspaceState = workspace.state;
        of.doesWorkspaceEndWhenDetached = workspace.endsWhenDetached;
        if (workspace.isRestorable()) {
            of.id = workspace.id;
            // Where its first pane was, as the record holds it.
            if (!workspace.windows.empty() && !workspace.windows.front().tabs.empty() && !workspace.windows.front().tabs.front().panes.empty())
                describe(of, workspace.windows.front().tabs.front().panes.front());
            restorable.push_back(std::move(of));
            continue;
        }
        for (const TreeWindow& window : workspace.windows) {
            for (const TreeTab& tab : window.tabs) {
                for (const TreePane& pane : tab.panes) {
                    if (pane.sessionID == 0)
                        continue;
                    SessionRow row = of;
                    row.id = pane.sessionID;
                    row.sessionID = pane.sessionID;
                    row.windowID = std::to_underlying(window.id);
                    row.windowLabel = window.label;
                    row.tabID = std::to_underlying(tab.id);
                    row.tabTitle = tab.name;
                    row.tabLabel = tab.label;
                    row.paneID = std::to_underlying(pane.id);
                    describe(row, pane);
                    row.foregroundProcess = pane.program;
                    row.isShellAtPrompt = pane.facts.isAtPrompt;
                    row.attendeeCount = saturated(pane.attendance.total());
                    row.humanAttendeeCount = saturated(pane.attendance.people);
                    row.createdUnixSeconds = pane.createdUnixSeconds;
                    row.isAdopted = pane.isAdopted;
                    live.push_back(std::move(row));
                }
            }
        }
    }
    std::ranges::stable_sort(live, {}, &SessionRow::sessionID);
    std::ranges::stable_sort(restorable, {}, &SessionRow::workspaceID);
    live.insert(live.end(), std::make_move_iterator(restorable.begin()), std::make_move_iterator(restorable.end()));
    return live;
}

} // namespace Ubiquitty::Session
