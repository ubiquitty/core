// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Host {

inline constexpr std::chrono::milliseconds attachWriteFailureDrainBudget { 5000 };

/// Whether the bridge was started inside an SSH session.
///
/// sshd sets `SSH_CONNECTION`, and the older `SSH_CLIENT`, for every session
/// it starts, a command channel included, so a bridge the SSH bootstrap runs
/// carries one and a bridge an application on this machine starts does not.
/// The bridge then marks its connection, so the client's sessions are gated as
/// a remote client's. Anything that can set the environment is already the
/// owner, and the mark only lowers what a session may do.
///
/// - Parameters:
///   - sshConnection: The bridge's `SSH_CONNECTION`, or null when unset.
///   - sshClient: The bridge's `SSH_CLIENT`, or null when unset.
/// - Returns: Whether either names a connection.
[[nodiscard]] constexpr bool isStartedBySSH(const char* sshConnection, const char* sshClient)
{
    return (sshConnection && *sshConnection) || (sshClient && *sshClient);
}

/// How long a bridge waits for the daemon to acknowledge its relay mark.
inline constexpr std::chrono::milliseconds relayMarkAcknowledgementBudget { 5000 };

/// What a daemon made of a bridge's relay mark.
enum class RelayMarkOutcome : std::uint8_t {
    /// The daemon lowered the connection; it is the client's carrier.
    Marked,
    /// The daemon acknowledged the mark without applying it: it does not take
    /// this bridge for its own, so the person would be gated as a program on
    /// this host rather than as one on another machine.
    Unrecognised,
    /// No acknowledgement: the write failed, the daemon closed, the reply was
    /// something else or none came in time. A daemon older than the mark
    /// closes on it, and so does a current one that fails; the two cannot be
    /// told apart from here, so neither attaches.
    Unconfirmed,
};

/// Marks a fresh daemon connection as relayed from SSH and waits for the
/// daemon's acknowledgement, before any of the client's bytes are read.
///
/// Only ``RelayMarkOutcome/Marked`` lets the bridge relay: anything less would
/// attach a person on another machine as one on this host, or as a program
/// on it. Every wait, for writing and for reading, is bounded by the budget,
/// and nothing past the acknowledgement is read, so the carrier's own stream
/// starts untouched.
///
/// - Parameters:
///   - daemonFileDescriptor: The connected daemon socket, blocking or not.
///   - budget: How long the whole exchange may take.
/// - Returns: What the daemon made of the mark.
[[nodiscard]] RelayMarkOutcome markRelayedFromSSH(int daemonFileDescriptor, std::chrono::milliseconds budget = relayMarkAcknowledgementBudget);

/// Which daemon a bridge attached to.
enum class AttachedDaemon : std::uint8_t {
    /// The one this bridge's own binary runs, at the socket named for the
    /// binary's contents: this version.
    OwnBinary,
    /// One a `--socket` pin named, which may be another version's.
    Named,
};

/// Which daemon a pinned socket names.
///
/// A pin may name this binary's own leaf, which is this version's daemon,
/// however it was reached.
///
/// - Parameters:
///   - pinnedSocket: The leaf a `--socket` pin named.
///   - ownLeaf: The leaf this binary's contents name; empty when it could not
///     be read.
/// - Returns: ``AttachedDaemon/OwnBinary`` when they are the same leaf.
[[nodiscard]] AttachedDaemon attachedDaemonOf(std::string_view pinnedSocket, std::string_view ownLeaf);

/// What a person reads when a bridge relaying an SSH connection stops before
/// attaching, saying only what changes the outcome.
///
/// This binary's own daemon is this version, so its failure to confirm is
/// passing and connecting again is the remedy, and no restart makes it
/// recognise a program it does not. A daemon a pin named may be an older
/// version, which neither confirms nor recognises this one; starting this
/// binary's daemon makes it take over every live session of an older one at
/// the same install path before it accepts a client, so that is the restart
/// offered. The command is shell-quoted so it can be pasted as it stands.
///
/// - Parameters:
///   - outcome: Why it stopped; never ``RelayMarkOutcome/Marked``.
///   - selfPath: This bridge's own executable.
///   - daemon: Which daemon it attached to.
/// - Returns: The sentence, without the program's name.
[[nodiscard]] std::string relayMarkRefusal(RelayMarkOutcome outcome, std::string_view selfPath, AttachedDaemon daemon);

/// Relays an established daemon connection without interpreting its frames.
///
/// The descriptors are borrowed, nonblocking, and remain owned by the caller,
/// which handles SIGPIPE. A failed daemon write stops input forwarding but
/// leaves its readable reply tail available to stdout. That failure alone
/// starts an absolute drain deadline; ordinary relay and detach have no new
/// timeout. Progress does not extend the deadline.
/// - Parameters:
///   - daemonFileDescriptor: The connected daemon socket.
///   - inputFileDescriptor: The client's input stream.
///   - outputFileDescriptor: The client's output stream.
///   - writeFailureDrainBudget: A positive bound on failed-write teardown.
/// - Returns: Zero, preserving an established attach's transport exit status.
int relayAttach(int daemonFileDescriptor, int inputFileDescriptor, int outputFileDescriptor,
    std::chrono::milliseconds writeFailureDrainBudget = attachWriteFailureDrainBudget);

} // namespace Ubiquitty::Host
