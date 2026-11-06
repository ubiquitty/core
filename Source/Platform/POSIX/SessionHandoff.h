// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Handing a running shell to the local ubiquitty-host daemon, and taking it back.
//
// The daemon ADOPTs a PTY that already exists rather than forking one of its
// own, so the shell keeps its history, its children and its controlling
// terminal, and the app that started it becomes an ordinary passive client.
//
// Native sockets, peer credentials, entropy, and descriptor transfer stay behind
// this seam; Core retains the portable ownership attempt and supplies its policy.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Ubiquitty::Session {

class AdoptClientAttempt;
struct ShareHandoffLimits;
struct ShareHandoffResult;

/// Prepares and commits a running PTY with a complete raw terminal continuation.
///
/// The caller has already withdrawn local I/O and teardown. A retained attempt
/// marks uncertainty before any Commit byte can be sent. Lost replies are queried
/// on freshly selected channels pinned to that same daemon and random operation.
/// An unresolved result never permits the retained master to resume or close.
///
/// - Parameters:
///   - socketPath: The daemon's control socket.
///   - terminalBody: The complete immutable `captureShareTerminal` result.
///   - masterFileDescriptor: The live PTY master to hand over. It stays open;
///     closing is permitted only after confirmed host activation retains it.
///   - limits: Explicit finite transport-work and record bounds.
/// - Returns: Exact ownership evidence and the same retained attempt.
[[nodiscard]] ShareHandoffResult reHomeViaAdopt(const char* socketPath, std::span<const std::byte> terminalBody, int masterFileDescriptor, const ShareHandoffLimits& limits);

/// Queries and aborts only the original operation without sending a descriptor.
/// - Returns: Exact ownership evidence, retaining the attempt while unresolved.
[[nodiscard]] ShareHandoffResult resolveAdopt(const char* socketPath, std::unique_ptr<AdoptClientAttempt> attempt, const ShareHandoffLimits& limits);

/// ``reHomeViaAdopt`` as the address a session's share takes.
///
/// `Runtime::TerminalSession::shareIntoLocalDaemon` performs every step of a
/// share but this one, and reaches it through a `Session::AdoptHandoff`
/// (Core/Session/SessionShare.h) because Core names no native handle. The
/// address crosses as a number for the reason a hosted-window factory's does:
/// the frontends that pass it cannot name the function type.
///
/// - Returns: A `Session::AdoptHandoff`, as an address.
std::uintptr_t adoptHandoffAddress();

/// Returns `resolveAdopt` in the portable `Session::AdoptResolve` shape.
[[nodiscard]] std::uintptr_t resolveHandoffAddress();

} // namespace Ubiquitty::Session
