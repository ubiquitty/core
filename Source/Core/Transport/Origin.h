// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// where a session's bytes come from, as a portable property. ORIGIN (local PTY,
// ssh-tmux, daemon, …) is a transport detail the frontend renders as a badge but
// never branches on. Stamped by the session's creator. Header-only, dependency-free.

#include <cstdint>
#include <string>

namespace Ubiquitty::Transport {

/// How a session's bytes reach us.
///
/// Additive: a new backend appends a value rather than renumbering, because the
/// value is stamped into captured workspaces. Most code asks `isRemote()` and
/// nothing else — a frontend renders the kind as a badge and must not branch on
/// it, or a new transport becomes a change in every frontend.
enum class OriginKind : std::uint8_t {
    Local = 0, // a local PTY on this machine (login shell, local tmux)
    SSH = 1, // reached over ssh — today's `ssh <host> tmux -CC …`
    Daemon = 2, // a ubiquitty-host daemon over the Sync protocol (ssh-stdio or QUIC)
};

/// Where a session's bytes come from, stamped by whoever created it.
struct Origin {
    OriginKind transport = OriginKind::Local;
    // "[user@]host" for SSH; the daemon host (or "local" for a loopback
    // daemon) for Daemon; empty for Local.
    std::string host;

    /// Returns whether the bytes cross a machine boundary.
    ///
    /// The one question most callers have, so that adding a transport does not
    /// add a case anywhere that only needed local-versus-not.
    [[nodiscard]] bool isRemote() const { return transport != OriginKind::Local; }
};

} // namespace Ubiquitty::Transport
