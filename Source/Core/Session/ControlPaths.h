// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// The shared ~/.ubiquitty control directory (0700) and the files inside it — both
// the daemon and the app live here, so the location + creation rule belong to
// neither exclusively. Portable POSIX; no PAL dependency.

#include <cstddef>
#include <expected>
#include <span>
#include <string>

namespace Ubiquitty::Session {

/// Writes the absolute control directory path.
///
/// `$UBIQUITTY_HOME` overrides it, which is what lets a test run isolated from
/// the developer's own control directory; otherwise `$HOME/.ubiquitty`.
///
/// - Parameter output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 with `output` emptied when neither
///   variable is available.
std::size_t controlDirectory(std::span<char> output);

/// Writes the absolute path of a named file in the control directory.
///
/// - Parameters:
///   - leaf: The file name inside the directory.
///   - output: Receives the NUL-terminated path.
/// - Returns: The length written, or 0 on failure.
std::size_t controlFilePath(const char* leaf, std::span<char> output);

// mkdir -p the control directory with mode 0700. Returns true if it exists
// after the call.
bool ensureControlDirectory();

/// Clears a stale listening socket so a fresh `bind` can claim its path.
///
/// The path may be user-configured, so an unconditional unlink here is a
/// deletion primitive pointed at whatever the user typed. Nothing is removed
/// but an owner-matching `AF_UNIX` socket proven stale:
///
///   - no entry            — nothing to clear, and the path is free.
///   - `lstat` fails       — refused; the object cannot be identified.
///   - a symlink           — refused; `lstat` never follows one, so it never
///                           reads as a socket and never resolves elsewhere.
///   - not a socket        — refused; a regular file, directory, fifo or device
///                           at a mistyped path is someone's data.
///   - another user's      — refused.
///   - a live listener     — refused; an instance is already answering there.
///   - proven dead         — re-examined, then unlinked.
///
/// - Parameters:
///   - path: The socket path about to be bound.
/// - Returns: Nothing on success. On refusal, a phrase naming it — the path is
///   untouched, and the reason travels as the failure rather than beside it.
std::expected<void, std::string> prepareSocketPathForBind(const std::string& path);

} // namespace Ubiquitty::Session
