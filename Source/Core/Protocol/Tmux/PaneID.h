// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The tmux control-mode pane-id type (`%N`). A plain alias for `int` (not a strong
// type) so every call site keeps working unchanged; it documents that the value is
// in tmux's %N namespace, not a window id (`@N`) or a count.
//
// Distinct from Ubiquitty::Session::Layout::PaneID (a uint32_t, LayoutTree.h) — Core's own
// split-pane identity, a different id space; call sites bridging the two static_cast.

namespace Ubiquitty::Protocol::Tmux {

using PaneID = int;

// The "no pane" / "unset" sentinel used across the tmux subsystem.
inline constexpr PaneID invalidPaneID = -1;

} // namespace Ubiquitty::Protocol::Tmux
