// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Reaching a peer on the same machine.
//
// A connected socket is a ``PollableByteStream`` and nothing more, so this
// contract covers only establishment — transfer needs nothing a byte stream does
// not already give.
//
// Establishment is where platforms diverge: Unix names an endpoint with a
// filesystem path, Windows offers named pipes and AF_UNIX under different length
// rules, and WebAssembly has no local transport at all.

namespace Ubiquitty::PAL {

/// Why a local endpoint could not be reached.
///
/// Distinguished rather than folded into a null return because recovery differs:
/// a stale endpoint is cleaned up and retried, a permission failure is shown to
/// the user, and an unsupported platform disables the feature instead of
/// retrying forever.
enum class LocalSocketError : uint8_t {
    None,
    /// The platform has no local-socket transport, as under WebAssembly.
    NotSupported,
    /// Longer than the platform's endpoint-name limit.
    PathTooLong,
    PermissionDenied,
    /// Nothing is listening on the name.
    NotFound,
    Failed,
};

/// Connects to a local endpoint named by `path`.
///
/// The returned stream is non-blocking and owns the connection; destroying it
/// closes it.
///
/// - Parameters:
///   - path: A filesystem-like endpoint name. The backend owns the length limit,
///     which differs per platform and is why ``LocalSocketError/PathTooLong``
///     exists instead of a caller-side constant.
///   - outError: Set on failure when non-null.
/// - Returns: The connected stream, or nullptr on failure.
[[nodiscard]] std::unique_ptr<PollableByteStream> connectLocal(std::string_view path, LocalSocketError* outError = nullptr);

/// Whether this platform has a local-socket transport at all.
///
/// Gate a feature on this rather than on a failed connect, so a build without
/// local sockets reports "unavailable" instead of "nothing is listening".
[[nodiscard]] bool localSocketsAvailable();

/// The identity the kernel reports for the peer of a connected local socket.
struct PeerIdentity {
    uint32_t userID = 0;
    uint32_t groupID = 0;
};

/// The kernel-verified identity of the peer on a connected local socket.
///
/// Every platform answers this, and none of them the same way — Darwin and the
/// BSDs through `getpeereid`, Linux through `SO_PEERCRED`. Asking here keeps the
/// matrix in one place: a caller that spells it itself acquires the platform
/// conditional, and two such callers enforce one security rule with a
/// hand-copied comment.
///
/// - Parameters:
///   - socket: A connected local socket. A socket of any other kind, or a
///     platform that cannot report the peer, yields nothing.
/// - Returns: The peer's identity, or nothing. Nothing is not "allow" — a caller
///   deciding access must treat it as a refusal, and an empty optional is the
///   only thing it can read on that path.
[[nodiscard]] std::optional<PeerIdentity> peerIdentity(const FileDescriptor& socket);

/// Whether the peer on a connected local socket runs as this process's user.
///
/// The access rule a local control socket needs, stated once. Filesystem
/// permissions carry most of it — the socket sits 0600 inside a 0700 directory —
/// and the case they let through is a root connector to a user's socket, which is
/// what this refuses. It also holds if the socket is ever relocated somewhere
/// shared, where the permissions alone would stop carrying the argument.
///
/// - Returns: True only when the kernel reports a peer whose effective user
///   matches this process's. A platform that cannot report the peer refuses.
[[nodiscard]] bool peerIsSameUser(const FileDescriptor& socket);

/// Wraps an already-open byte-oriented descriptor as a stream, taking ownership.
///
/// Any descriptor whose reads and writes are a byte sequence: a socket, a pipe
/// end, a PTY master. For a caller holding one it did not open here — inherited,
/// socket-activated, one end of a pipe pair it spawned a child around, or a
/// test's own ``socketpair``. Without this, such a caller has to hand raw
/// descriptors to something that wants a stream, which is the shape that put a
/// concrete POSIX handle in Core's interfaces.
///
/// The returned stream is non-blocking, matching every other stream this layer
/// hands out, so a consumer never has to ask which kind it holds.
///
/// - Parameters:
///   - fileDescriptor: An open byte-oriented descriptor. Ownership transfers,
///     including on failure, so the caller never closes it twice.
///   - readinessWaitStrategy: The descriptor's producer cadence. A PTY or
///     pipe shares its producer's kernel queue and probes first; a socket peer
///     waits first.
/// - Returns: The stream, or nothing when the descriptor is not usable.
[[nodiscard]] std::unique_ptr<PollableByteStream> adoptByteStream(int fileDescriptor, ReadinessWaitStrategy readinessWaitStrategy);

/// A short reason for a failed local-socket connection, for a caller's message.
///
/// Beside the enumeration so every caller reports the same reason for the same
/// failure. A caller wording it itself will say "connect failed" for a path that
/// was merely too long, which sends the reader to the wrong place.
///
/// - Returns: A stable, lowercase phrase. Never null.
[[nodiscard]] const char* describeLocalSocketError(LocalSocketError reason);

/// Creates the private directory a local socket is named inside, and confirms it.
///
/// The directory must end up owned by this user and reachable by nobody else,
/// because a socket is only as private as the directory naming it. Creating it
/// and checking it are one operation: a caller that creates then stats has a
/// window where another process substitutes a directory it controls.
///
/// Succeeds when the directory already exists and satisfies those conditions.
///
/// - Parameter path: Absolute path to create.
/// - Returns: Whether the directory exists, is private, and belongs to this user.
[[nodiscard]] bool ensurePrivateSocketDirectory(std::string_view path);

/// Clears a stale socket out of the way so a bind can succeed, or refuses.
///
/// A path left by a crashed process must be removed before rebinding, and a path
/// held by anything else must not be. Distinguishing them is a sequence, not a
/// question: whether the name is a socket rather than a symlink or a regular
/// file, whether this user owns it, whether a listener still answers it, and
/// whether the exact object examined is still there at the moment of removal.
/// Ordering is the security property, so it lives on one side of the seam.
///
/// Refuses whenever the answer is uncertain — an occupied path is left alone.
///
/// - Parameters:
///   - path: The socket path about to be bound.
/// - Returns: Nothing on success. On refusal, a phrase naming it — the reason
///   travels with the failure rather than beside it, so a caller cannot report
///   one it never received.
[[nodiscard]] std::expected<void, std::string> prepareLocalSocketPathForBind(std::string_view path);

/// Binds a local socket at `path`, owner-only, and listens on it.
///
/// The name is created under a umask that leaves it readable and writable by
/// this user alone, then set to `0600` explicitly, so there is no moment at
/// which another user could connect. Clear a stale name first with
/// ``prepareLocalSocketPathForBind``.
///
/// - Parameters:
///   - path: The socket path to bind.
///   - backlog: How many pending connections the kernel queues.
/// - Returns: The listening descriptor, or a phrase naming the failure.
[[nodiscard]] std::expected<UnixFileDescriptor, std::string> listenLocal(std::string_view path, int backlog);

/// Removes the name of a local socket this user owns.
///
/// Anything else at the path — another user's socket, a file, a symlink — is
/// left alone, so a listener tearing down cannot delete what replaced it.
///
/// - Parameter path: The socket path a listener bound.
void removeLocalSocket(std::string_view path);

/// Why a blocking socket read stopped.
///
/// A caller distinguishing these by errno is a caller that has to know which
/// codes a platform reports for a peer that left versus a wait that expired.
/// Naming them keeps that knowledge on this side of the seam.
enum class SocketReadOutcome : uint8_t {
    Bytes, /// Some bytes arrived; the count is written out.
    PeerClosed, /// The peer shut down cleanly. No bytes.
    Interrupted, /// A signal arrived before any byte did. The caller retries.
    TimedOut, /// The receive bound elapsed with nothing to read.
    Failed /// The socket is unusable. The caller stops.
};

/// Reads whatever has arrived, blocking until something does.
///
/// Blocks until bytes arrive, the peer leaves, a signal interrupts the wait, or
/// the receive bound set by `setReceiveTimeoutSeconds` expires. Returns as soon
/// as any bytes are available rather than filling `into`.
///
/// - Parameters:
///   - socket: A connected socket.
///   - into: Where to put what arrives. Must be non-empty.
///   - outBytes: Set to the count when the outcome is `Bytes`. Untouched otherwise.
/// - Returns: Which of the five things happened.
[[nodiscard]] SocketReadOutcome readSocket(int socket, std::span<char> into, std::size_t* outBytes);

/// Writes every byte, or reports that it could not.
///
/// A single write call may accept only part of a buffer, so this repeats until
/// the buffer is gone. A caller that writes once and checks only for a negative
/// result silently truncates its own output.
///
/// - Parameters:
///   - socket: A connected socket.
///   - bytes: The bytes to send. An empty span succeeds.
/// - Returns: Whether every byte was written.
[[nodiscard]] bool writeSocketAll(int socket, std::span<const char> bytes);

/// Breaks a socket in both directions without closing the descriptor.
///
/// Connected reads and writes stop waiting. A listening socket's accept need
/// not wake on every platform; accept loops use nonblocking readiness waits.
///
/// - Parameter socket: The socket to break.
void shutdownSocket(int socket);

/// The outcome of one nonblocking accept attempt.
enum class SocketAcceptOutcome : uint8_t {
    Connected, /// A peer arrived; the descriptor is written out.
    Interrupted, /// A signal arrived first. The caller retries.
    Retry, /// A pending peer or resource failure; retry after a bounded delay.
    Unusable /// The listener is broken or closed. The caller stops.
};

/// Takes the next connection waiting on a listening socket.
///
/// Makes the listener nonblocking so readiness lost between poll and accept
/// cannot trap the caller in the kernel. A connected client is returned in
/// blocking mode, independently of whether the platform inherits listener flags.
///
/// - Parameters:
///   - listener: A socket already placed in the listening state.
///   - outClient: Set to the connected socket when the outcome is `Connected`.
/// - Returns: Whether a peer arrived, the wait was interrupted, or an error is retryable.
[[nodiscard]] SocketAcceptOutcome acceptConnection(const UnixFileDescriptor& listener, UnixFileDescriptor* outClient);

/// Bounds how long a read on this socket waits, to finer than a second.
///
/// A handshake with one deadline across the whole exchange narrows the bound before every read, so
/// what is left of the deadline is that read's ceiling. That remainder is not a whole number of
/// seconds, and rounding it down to one either drops the bound entirely or spends the rest of the
/// budget on a single read.
///
/// - Parameters:
///   - socket: The connected socket to bound.
///   - timeout: How long to wait. Zero or negative blocks indefinitely, which is what the socket
///     option itself means by zero.
/// - Returns: Whether the bound was applied.
bool setReceiveTimeout(int socket, std::chrono::microseconds timeout);

/// Bounds how long a write on this socket waits, to finer than a second.
///
/// The companion to ``setReceiveTimeout``; see ``setSendTimeoutSeconds`` for why writes need their
/// own bound at all.
///
/// - Parameters:
///   - socket: The connected socket to bound.
///   - timeout: How long to wait. Zero or negative blocks indefinitely.
/// - Returns: Whether the bound was applied.
bool setSendTimeout(int socket, std::chrono::microseconds timeout);

/// Bounds how long a read on this socket waits before giving up.
///
/// A peer that connects and then says nothing holds its slot for as long as the
/// server is willing to block. This is what lets a bounded server reclaim the
/// slot without a reaper thread.
///
/// - Parameters:
///   - socket: The connected socket to bound.
///   - seconds: Whole seconds to wait. Zero blocks indefinitely.
/// - Returns: Whether the bound was applied.
bool setReceiveTimeoutSeconds(int socket, int seconds);

/// Bounds how long a write on this socket waits before giving up.
///
/// The receive bound does not cover writes, and a peer that stops reading
/// stalls the sender just as effectively as one that stops writing: once the
/// send buffer fills, `writeSocketAll` blocks with no deadline of its own. On
/// a bounded server that is one slot held indefinitely by a peer that has to
/// do nothing but connect and go quiet.
///
/// - Parameters:
///   - socket: The connected socket to bound.
///   - seconds: Whole seconds to wait. Zero blocks indefinitely.
/// - Returns: Whether the bound was applied.
bool setSendTimeoutSeconds(int socket, int seconds);

} // namespace Ubiquitty::PAL
