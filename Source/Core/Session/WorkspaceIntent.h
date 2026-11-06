// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>

namespace Ubiquitty::Session {

/// One local workspace intent, translated to a registered operation-specific Call.
enum class WorkspaceIntentKind : std::uint8_t {
    AddSurface = 0, // paneID: the pane it opens from, whose theme the new session takes
    CloseSurface = 1,
    SetActiveSurface = 2,
    AddTab = 3, // paneID: as AddSurface
    CloseTab = 4,
    SetActiveTab = 5,
    SplitFocusedPane = 6, // orientation, ratio
    CloseFocusedPane = 7,
    ClosePane = 8, // paneID
    FocusPane = 9, // paneID
    MoveFocus = 10, // direction
    MovePane = 11, // paneID beside targetPaneID; orientation, side
    SwapPanes = 12, // paneID with targetPaneID
    MovePaneToTab = 13, // paneID into targetTabID beside targetPaneID; orientation, side
    ExtractPaneToTab = 14, // paneID
    ExtractPaneToSurface = 15, // paneID
    BalanceActiveTab = 16,
    AdjustFocusedPaneRatio = 17, // direction, ratio as the step
    SetDividerRatio = 18, // splitIndex, ratio
    ToggleZoom = 19, // paneID
    // Names are authoritative host state and travel in the workspace's
    // structure.
    RenameWorkspace = 20, // title
    RenameTab = 21, // tabID, title
    MoveTabToPane = 22, // tabID, targetPaneID, orientation, side
    MoveTabToSurface = 23, // tabID, targetPaneID identifies the destination surface
    ExtractTabToSurface = 24, // tabID
    MovePaneToNewTab = 25, // paneID, targetPaneID identifies the destination surface
    // Labels are authoritative host state and travel in the structure. An empty
    // title clears one; a title the host's label rule refuses is not performed.
    LabelSurface = 26, // surfaceID, title
    LabelPane = 27, // paneID, title
    LabelSession = 28, // paneID names the pane whose session is labelled; title
    LabelClient = 29, // title; labels the sending client itself
};

/// A frontend's local workspace request. The selected targets and requested
/// state are validated against the observed tree before a canonical Call is
/// formed; this value has no serialized payload or independent authority.
struct WorkspaceIntent {
    std::uint32_t requestID {}; // the frontend callback's request identity
    std::uint32_t workspaceID {};
    WorkspaceIntentKind verb {};
    std::uint32_t surfaceID {};
    std::uint32_t tabID {};
    std::uint32_t paneID {};
    std::uint32_t targetPaneID {};
    std::uint32_t targetTabID {};
    std::uint8_t orientation {}; // Panes::Orientation
    std::uint8_t side {}; // Panes::InsertSide
    std::uint8_t direction {}; // Panes::Direction
    std::uint8_t insertTarget {}; // Panes::InsertTarget
    float ratio {}; // 0 to 1
    std::uint16_t splitIndex {};
    std::string title; // UTF-8; the renames
};

/// The local completion of a canonical workspace Call. A refused operation
/// retains its request identity and names no newly created object.
struct WorkspaceIntentResult {
    std::uint32_t requestID {};
    bool wasPerformed {};
    std::uint32_t surfaceID {}; // a surface the verb minted, else 0
    std::uint32_t tabID {}; // a tab the verb minted, else 0
    std::uint32_t paneID {}; // a pane the verb minted, else 0
    std::uint32_t sessionID {}; // the session behind a minted pane, else 0
};

} // namespace Ubiquitty::Session
