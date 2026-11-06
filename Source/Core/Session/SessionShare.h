// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Sharing a running local session into the local ubiquitty-host daemon: the
// portable half of "Share This Window".
//
// The daemon adopts the PTY that already exists rather than forking a shell of
// its own, so the shell keeps its history, its children and its controlling
// terminal. Four things decide whether and where that happens, and every
// frontend needs the same answer to each:
//
//   - which socket the daemon a given ubiquitty-host binary starts binds — a
//     per-binary leaf in this user's control directory, and nothing at all when
//     the binary's identity cannot be computed, so a share fails closed rather
//     than reaching a well-known socket some other binary's daemon owns;
//   - whether that daemon is running — a listener on the socket, not the
//     socket file a dead one leaves — and starting it when it is not;
//   - what the ADOPT_SESSION frame carries — one complete terminal continuation,
//     including both screens, every retained history row, and host-only stores;
//     unfinished model work is refused rather than completed during capture;
//   - how the master descriptor crosses to the daemon, which is the one step no
//     portable code can take. It is a function the platform supplies
//     (Platform/POSIX/SessionHandoff.h), named here by its shape.
//
// The session side — stopping the local reader, attaching as the adopted
// session's thin client, joining a shared window's panes to one connection,
// and resuming on reclaim — is TerminalSession's.

#include "Base/SwiftInterop.h"
#include "Core/Protocol/Sync/HandoverBody.h"
#include "Core/Session/AdoptTransaction.h"
#include "Core/Session/RestorePlan.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {
struct HandoverTerminalLimits;
}

namespace Ubiquitty::Session {

/// How a share ended, which is all a frontend has to say about it.
enum class ShareOutcome : std::uint8_t {
    /// The daemon adopted the session's PTY. Whether a thin client started
    /// with it is the operation's to say.
    Shared UBIQUITTY_SWIFT_NAME(shared),
    /// The session has nothing to hand over — no running local shell, or it is
    /// shared already. Nothing was attempted.
    NotShareable UBIQUITTY_SWIFT_NAME(notShareable),
    /// The host binary's identity, or the control directory, could not be
    /// computed, so there is no socket this share may use.
    NoDaemonSocket UBIQUITTY_SWIFT_NAME(noDaemonSocket),
    /// No daemon answered on the socket, and starting one did not change that.
    DaemonDidNotStart UBIQUITTY_SWIFT_NAME(daemonDidNotStart),
    /// The daemon did not adopt the PTY. The session is as it was.
    HandoffRefused UBIQUITTY_SWIFT_NAME(handoffRefused),
    /// Commit may have reached the daemon. The inactive local owner must remain.
    Unresolved UBIQUITTY_SWIFT_NAME(unresolved),
    /// The terminal state exceeds a share's encoded or component limits.
    /// Nothing was attempted.
    TooLongToShare UBIQUITTY_SWIFT_NAME(tooLongToShare),
    /// The daemon already holds as many sessions as it serves, so it did not
    /// adopt the PTY. Nothing about the session was wrong; it is as it was.
    HostFull UBIQUITTY_SWIFT_NAME(hostFull),
};

/// Identifies an adopted session within one daemon process.
struct AdoptedSession {
    std::uint32_t sessionID = 0;
    std::uint64_t daemonInstanceID = 0;
};

/// Whether one committed share may ride the carrier another's attach opened.
///
/// One attachment of a daemon carries every pane of a workspace it holds, so
/// a shared window's panes need one carrier between them, but only while each
/// PTY went to the same daemon process and is reached through the same
/// bridge. A share into another daemon, or one reached another way, is
/// another authority and keeps a carrier of its own.
///
/// - Parameters:
///   - anchorBridge: The bridge command the anchor's share reaches its daemon by.
///   - anchor: The session and daemon the anchor's PTY went to.
///   - memberBridge: The bridge command of the pane that would join it.
///   - member: The session and daemon that pane's PTY went to.
/// - Returns: Whether the member may be one more endpoint of the anchor's carrier.
[[nodiscard]] inline bool isSameLocalShareAuthority(std::string_view anchorBridge, const AdoptedSession& anchor, std::string_view memberBridge, const AdoptedSession& member)
{
    return anchor.sessionID != 0 && member.sessionID != 0 && anchor.sessionID != member.sessionID && anchor.daemonInstanceID != 0
        && anchor.daemonInstanceID == member.daemonInstanceID && anchorBridge == memberBridge;
}

/// Selects whether the retained local endpoint may resume or must remain inactive.
enum class ShareOwnership : std::uint8_t {
    Local,
    Committed,
    Unresolved,
};

/// Explicit transport-work bounds for one local adoption and its resolution.
struct ShareHandoffLimits {
    std::size_t maxRecordBytes = 0;
    std::uint64_t silenceMicroseconds = 0;
    std::uint64_t applyMicrosecondsPerMebibyte = 0;
    std::size_t resolutionAttempts = 0;
};

/// Retains the same operation and ownership gate across transport failures.
/// An unresolved result requires the caller to retain its inactive local master.
struct ShareHandoffResult {
    ShareOwnership ownership = ShareOwnership::Local;
    AdoptedSession adopted {};
    std::unique_ptr<AdoptClientAttempt> attempt {};
};

/// Chooses finite local handoff transport bounds independently of carried bytes.
/// The silence allowance bounds an idle exchange; the per-MiB allowance permits
/// bounded whole-terminal preparation. Two fresh resolution channels limit work
/// after a lost reply without treating silence as an ownership decision.
[[nodiscard]] ShareHandoffLimits localShareHandoffLimits();

/// Prepares and commits a live PTY master with a complete terminal continuation.
///
/// The descriptor accompanies only Prepare's first byte. The caller suspends all
/// local I/O and teardown before calling and retains that owner while Unresolved.
///
/// - Parameters:
///   - socketPath: The daemon's control socket, NUL-terminated.
///   - terminalBody: The complete immutable raw terminal continuation.
///   - masterFileDescriptor: The PTY master to hand over.
///   - limits: Explicit work and encoded-record bounds.
/// - Returns: Proven ownership and the exact retained attempt, never inferred failure.
using AdoptHandoff = ShareHandoffResult (*)(const char* socketPath, std::span<const std::byte> terminalBody, int masterFileDescriptor, const ShareHandoffLimits& limits);

/// Resolves the same attempted operation through bounded Status and Abort exchanges.
/// No descriptor or replacement Prepare is sent, and a different daemon refuses.
using AdoptResolve = ShareHandoffResult (*)(const char* socketPath, std::unique_ptr<AdoptClientAttempt> attempt, const ShareHandoffLimits& limits);

/// The socket a ubiquitty-host binary's local daemon binds, in this user's
/// control directory (`$UBIQUITTY_HOME`, else `~/.ubiquitty`).
///
/// The leaf is ``socketLeafForBinary`` (DaemonSocketLeaf.h), the one the host
/// computes for itself, so the two sides name the same socket.
///
/// - Parameter hostBinaryPath: The binary a share starts the daemon from.
/// - Returns: The absolute socket path, or empty when either the leaf or the
///   control directory is uncomputable, so the caller refuses to share.
[[nodiscard]] std::string localDaemonSocketPath(const std::string& hostBinaryPath);

/// Where this machine's host is installed: `ubiquitty-host` in the control
/// directory, the path an SSH bootstrap runs it from.
///
/// - Returns: The absolute path, or empty when the control directory is
///   uncomputable.
[[nodiscard]] std::string localHostInstallPath();

/// Brings up the local daemon a ubiquitty-host binary owns, unless one already
/// answers on its socket.
///
/// Runs `hostBinaryPath --ensure-daemon`, which detaches the daemon and exits
/// once it is started, then waits up to three seconds for the socket to take a
/// connection. Blocks for that long at most; a share is a menu action, and the
/// daemon it waits for is local.
///
/// - Parameters:
///   - hostBinaryPath: The binary to start the daemon from.
///   - socketPath: The socket that daemon binds, from ``localDaemonSocketPath``.
/// - Returns: Whether a daemon answers there — false when the binary could not
///   be run, exited with a failure, or started nothing that listened in time.
[[nodiscard]] bool ensureLocalDaemon(const std::string& hostBinaryPath, const std::string& socketPath);

/// The daemon a quit hands this machine's local shells to, brought up when
/// nothing answers on its socket (``ensureLocalDaemon``).
///
/// All three facts come from the one binary, so the shells a quit hands over
/// and the tabs that rejoin them name the same daemon: its control socket, the
/// versioned leaf a restore pins, and the bridge that reaches it.
///
/// - Parameter hostBinaryPath: The ubiquitty-host a restore reaches the daemon
///   through.
/// - Returns: The target; all empty when the binary has no identity or no
///   daemon answers, so the quit hands nothing over.
[[nodiscard]] LocalDaemonTarget readyLocalDaemon(const std::string& hostBinaryPath);

/// How encoding a share ended.
enum class ShareEncoding : std::uint8_t {
    /// The terminal's body is captured.
    Encoded,
    /// The terminal has no valid grid, is not quiescent, or cannot be captured whole.
    NothingToShare,
    /// The complete terminal exceeds an encoded or component limit.
    /// Nothing is written: a share never drops retained state.
    TooLong,
};

/// Captures one whole immutable terminal body without local handoff framing.
/// The caller holds the model lock and its quiescence lease throughout capture.
/// - Parameters:
///   - term: The unchanged authoritative terminal.
///   - maxBodyBytes: The independent local encoded-body ceiling.
/// - Returns: Complete owned bytes, or refusal without source mutation.
[[nodiscard]] std::expected<Protocol::Sync::HandoverBytes, ShareEncoding> captureShareTerminal(const Ubiquitty::Terminal& term, std::size_t maxBodyBytes);

/// The heading a frontend shows for a share that did not happen.
///
/// - Parameter outcome: How the share ended.
/// - Returns: The heading; empty for an outcome with nothing to tell.
[[nodiscard]] std::string shareFailureTitle(ShareOutcome outcome);

/// What a frontend says under the heading of a share that did not happen.
///
/// - Parameter outcome: How the share ended.
/// - Returns: The text, naming the limit when the window is past it; empty
///   for an outcome with nothing to tell.
[[nodiscard]] std::string shareFailureDetail(ShareOutcome outcome);

} // namespace Ubiquitty::Session
