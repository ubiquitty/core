// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// AttachTarget.h — what opening something in a host's tree attaches to.
//
// A person opens a row of the outline, or picks a workspace when a connect
// asks which. Either way the answer is one value: the session to attach to
// and the pane to show once attached, or the name the host restores a
// recorded workspace by, or that it cannot be opened and why. The connect
// rule (`RememberedSessions::decide`) lands a workspace through the same
// function, so what a row opens and what the rule attaches to are one
// answer, and no frontend looks up a session or a name for itself.
//

#include "Base/SwiftInterop.h"
#include "Core/Session/SessionTree.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Session {

/// How something is opened. The values are explicit because the Swift and
/// web mirrors pin them.
enum class AttachOpening : std::uint8_t {
    /// It cannot be opened; `AttachTarget::obstacle` says why.
    Cannot UBIQUITTY_SWIFT_NAME(cannot) = 0,
    /// By attaching to `AttachTarget::sessionID`.
    BySession UBIQUITTY_SWIFT_NAME(bySession) = 1,
    /// By asking the host for `AttachTarget::workspaceName`, which recreates
    /// the workspace from its record.
    ByRestoring UBIQUITTY_SWIFT_NAME(byRestoring) = 2,
    /// By showing what the reader already has open: `AttachTarget::sessionID`
    /// is the session the reader stated it holds the workspace open with, and
    /// nothing is attached. An outline answers it to a reader that stated what
    /// it has open; the connect rule never does.
    ByRevealing UBIQUITTY_SWIFT_NAME(byRevealing) = 3,
};

/// Why something cannot be opened.
enum class AttachObstacle : std::uint8_t {
    None UBIQUITTY_SWIFT_NAME(none) = 0,
    /// No session draws into it.
    NothingRuns UBIQUITTY_SWIFT_NAME(nothingRuns) = 1,
    /// It is a level of a workspace the host would restore: the workspace is
    /// restored whole, from its own row.
    PartOfARecord UBIQUITTY_SWIFT_NAME(partOfARecord) = 2,
    /// The host recorded the workspace under no name to restore it by.
    RecordedUnderNoName UBIQUITTY_SWIFT_NAME(recordedUnderNoName) = 3,
};

/// The sentence a thing that cannot be opened is explained with.
///
/// - Parameter obstacle: Why.
/// - Returns: A sentence; empty for `None`.
[[nodiscard]] constexpr std::string_view attachObstacleSentence(AttachObstacle obstacle)
{
    switch (obstacle) {
    case AttachObstacle::None:
        return {};
    case AttachObstacle::NothingRuns:
        return "Nothing runs here to attach to.";
    case AttachObstacle::PartOfARecord:
        return "This is part of a workspace nothing runs in. Open the workspace to bring it back.";
    case AttachObstacle::RecordedUnderNoName:
        return "The host kept this workspace under no name, so it cannot be brought back.";
    }
    return {};
}

/// What opening something attaches to.
struct AttachTarget {
    AttachOpening opening = AttachOpening::Cannot;
    /// The session to attach to, for `BySession`; the session the reader
    /// already holds the workspace open with, for `ByRevealing`; zero
    /// otherwise.
    std::uint32_t sessionID = 0;
    /// The workspace it is in, as the host's tree numbers it.
    std::uint32_t workspaceID = 0;
    /// The name of that workspace: what `ByRestoring` asks the host for, and
    /// what a connect remembers beside a session. Empty for one with no name.
    std::string workspaceName;
    /// The pane to show once attached, as the host's layout numbers it; zero
    /// when the host's own focus is where to land, and for a restore.
    std::uint32_t paneID = 0;
    AttachObstacle obstacle = AttachObstacle::None;

    friend bool operator==(const AttachTarget&, const AttachTarget&) = default;
};

namespace AttachTargetDetail {

/// A target that cannot be opened, in a workspace.
[[nodiscard]] inline AttachTarget cannot(const TreeWorkspace& workspace, AttachObstacle obstacle)
{
    AttachTarget target;
    target.workspaceID = workspace.id;
    target.workspaceName = workspace.name;
    target.obstacle = obstacle;
    return target;
}

/// A target that attaches to a pane's session and shows that pane.
[[nodiscard]] inline AttachTarget onPane(const TreeWorkspace& workspace, const TreePane& pane)
{
    if (pane.sessionID == 0)
        return cannot(workspace, AttachObstacle::NothingRuns);
    AttachTarget target;
    target.opening = AttachOpening::BySession;
    target.sessionID = pane.sessionID;
    target.workspaceID = workspace.id;
    target.workspaceName = workspace.name;
    target.paneID = std::to_underlying(pane.id);
    return target;
}

/// The pane a tab is joined on: its host-focused one when a session draws
/// into it, else its first pane a session draws into; null for none.
[[nodiscard]] inline const TreePane* landingPane(const TreeTab& tab)
{
    if (tab.hostFocusedPane < tab.panes.size() && tab.panes[tab.hostFocusedPane].sessionID != 0)
        return &tab.panes[tab.hostFocusedPane];
    for (const TreePane& pane : tab.panes) {
        if (pane.sessionID != 0)
            return &pane;
    }
    return nullptr;
}

/// The pane a window is joined on: its shown tab's landing pane, else that
/// of its first tab that has one.
[[nodiscard]] inline const TreePane* landingPane(const TreeWindow& window)
{
    if (window.hostActiveTab < window.tabs.size()) {
        if (const TreePane* pane = landingPane(window.tabs[window.hostActiveTab]))
            return pane;
    }
    for (const TreeTab& tab : window.tabs) {
        if (const TreePane* pane = landingPane(tab))
            return pane;
    }
    return nullptr;
}

} // namespace AttachTargetDetail

/// What opening a pane attaches to: its session, shown at that pane.
///
/// - Parameters:
///   - workspace: The workspace the pane is in.
///   - pane: The pane.
/// - Returns: The target.
[[nodiscard]] inline AttachTarget attachTargetOf(const TreeWorkspace& workspace, const TreePane& pane)
{
    if (workspace.isRestorable())
        return AttachTargetDetail::cannot(workspace, AttachObstacle::PartOfARecord);
    return AttachTargetDetail::onPane(workspace, pane);
}

/// What opening a tab attaches to: the session of its host-focused pane,
/// else of its first pane a session draws into.
///
/// - Parameters:
///   - workspace: The workspace the tab is in.
///   - tab: The tab.
/// - Returns: The target.
[[nodiscard]] inline AttachTarget attachTargetOf(const TreeWorkspace& workspace, const TreeTab& tab)
{
    if (workspace.isRestorable())
        return AttachTargetDetail::cannot(workspace, AttachObstacle::PartOfARecord);
    const TreePane* pane = AttachTargetDetail::landingPane(tab);
    return pane ? AttachTargetDetail::onPane(workspace, *pane) : AttachTargetDetail::cannot(workspace, AttachObstacle::NothingRuns);
}

/// What opening a window attaches to: where its shown tab lands, else its
/// first tab that lands anywhere.
///
/// - Parameters:
///   - workspace: The workspace the window is in.
///   - window: The window.
/// - Returns: The target.
[[nodiscard]] inline AttachTarget attachTargetOf(const TreeWorkspace& workspace, const TreeWindow& window)
{
    if (workspace.isRestorable())
        return AttachTargetDetail::cannot(workspace, AttachObstacle::PartOfARecord);
    const TreePane* pane = AttachTargetDetail::landingPane(window);
    return pane ? AttachTargetDetail::onPane(workspace, *pane) : AttachTargetDetail::cannot(workspace, AttachObstacle::NothingRuns);
}

/// What opening a workspace attaches to.
///
/// A live one lands where its window in front lands, else in its first
/// window that lands anywhere: the pane the host's own focus is on when a
/// session draws into it. One the host would restore is asked for by its
/// name.
///
/// - Parameter workspace: The workspace.
/// - Returns: The target.
[[nodiscard]] inline AttachTarget attachTargetOf(const TreeWorkspace& workspace)
{
    if (workspace.isRestorable()) {
        if (workspace.name.empty())
            return AttachTargetDetail::cannot(workspace, AttachObstacle::RecordedUnderNoName);
        AttachTarget target;
        target.opening = AttachOpening::ByRestoring;
        target.workspaceID = workspace.id;
        target.workspaceName = workspace.name;
        return target;
    }
    const TreePane* pane = workspace.hostActiveWindow < workspace.windows.size() ? AttachTargetDetail::landingPane(workspace.windows[workspace.hostActiveWindow]) : nullptr;
    for (std::size_t index = 0; !pane && index < workspace.windows.size(); ++index)
        pane = AttachTargetDetail::landingPane(workspace.windows[index]);
    return pane ? AttachTargetDetail::onPane(workspace, *pane) : AttachTargetDetail::cannot(workspace, AttachObstacle::NothingRuns);
}

} // namespace Ubiquitty::Session
