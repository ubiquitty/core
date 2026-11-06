// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Fixed on-disk locations for the ubiquitty-host daemon's control files, all
// under ~/.ubiquitty/ (created 0700 by the daemon). Portable POSIX; no PAL dep.
//   host-<id>.sock — a daemon's unix-domain listening socket clients attach to.
//   host-<id>.pid — its process-ID file, guarding one daemon per socket.
//   host-<id>.log — a detached daemon's stderr, one file per daemon socket.

#include <cstddef>
#include <span>
#include <string_view>

namespace Ubiquitty::Host {

/// Writes the absolute control directory path.
///
/// The Host backend's view of ``Session::controlDirectory``, which it names its
/// own files inside.
///
/// - Parameter output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 with `output` emptied when `$HOME` is
///   unavailable.
std::size_t controlDirectory(std::span<char> output);

/// Writes the log file path paired with a socket leaf: `host-<id>.log`.
///
/// Each daemon owns its socket, so each writes a log of its own, and a line in
/// one is never another daemon's. Applies ``socketPathForName``'s leaf
/// validation first, so a rejected socket name cannot reach a log path.
///
/// - Parameters:
///   - socketLeaf: The socket file name the log belongs to.
///   - output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 on failure.
std::size_t logfilePathForSocketName(const char* socketLeaf, std::span<char> output);

/// Answers whether a file name is a daemon socket leaf.
///
/// - Parameter name: The file name.
/// - Returns: Whether it is the per-binary `host-<16 hex>.sock` form, the only
///   one a daemon binds.
bool isHostSocketLeaf(std::string_view name);

/// Checks a direct, same-user socket inside the account's private control directory.
///
/// The leaf itself must be a socket, never a symlink to another daemon. This
/// validates discovery within the account trust boundary; it does not
/// authenticate an executable against other code running as the same user.
/// - Parameter path: The NUL-terminated candidate path.
/// - Returns: Whether the candidate has the daemon naming and ownership contract.
[[nodiscard]] bool isOwnHostSocketPath(const char* path);

/// Writes the absolute path of a named socket leaf.
///
/// Only a leaf ``isHostSocketLeaf`` accepts is joined — failing closed rather
/// than ever handing back a shared leaf — so a separator or traversal in
/// `leaf` never reaches the control-directory join.
///
/// - Parameters:
///   - leaf: The socket file name.
///   - output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 on failure.
std::size_t socketPathForName(const char* leaf, std::span<char> output);

/// Writes the process-ID file path paired with a socket leaf.
///
/// Applies ``socketPathForName``'s leaf validation before deriving
/// `host-<id>.pid`, so a rejected socket name cannot reach a process-ID path
/// either.
///
/// - Parameters:
///   - socketLeaf: The socket file name the process-ID file belongs to.
///   - output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 on failure.
std::size_t processIDFilePathForSocketName(const char* socketLeaf, std::span<char> output);

/// Writes the absolute path of the most recently created daemon socket.
///
/// Versioned sockets let several coexist across an auto-update, so a consumer
/// that connects by fixed name and cannot derive the binary hash needs the
/// newest. The result MAY name a stale socket — the caller probes liveness on
/// connect rather than trusting the mtime.
///
/// - Parameter output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 when no socket exists.
std::size_t newestSocketPath(std::span<char> output);

// Visit each daemon socket, NEWEST bind-mtime first, calling visit(path, context)
// until it returns true (then returns true) or candidates are exhausted (false).
// Lets a consumer connect to the first that ANSWERS, skipping a stale newest
// socket for a live older one.
bool eachHostSocketNewestFirst(bool (*visit)(const char* path, void* context), void* context);

/// Whether something listens at a Unix socket path: a connection is
/// accepted, or would be once the listener's backlog drains.
///
/// A listener whose backlog is full counts as listening. Where the kernel
/// refuses it as it refuses a socket nothing listens on, the kernel's record
/// of which processes hold a listener there decides, and whatever cannot be
/// read counts as a listener.
///
/// - Parameter path: The socket's absolute path.
/// - Returns: False only when the path names no socket or nothing listens there.
[[nodiscard]] bool isSocketPathServed(const char* path);

// mkdir -p ~/.ubiquitty with mode 0700. Returns true if it exists after
// the call.
bool ensureControlDirectory();

} // namespace Ubiquitty::Host
