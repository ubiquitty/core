// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/HandoverChannel.h"
#include "Core/Session/AdoptTransaction.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/SignaturePolicy.h"
#include "Platform/Host/TrustedLocalBinaries.h"

#include <sys/stat.h>

#include <array>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace Ubiquitty::Host {

/// Supplies holder-owned routing facts, never fields chosen by the requester.
struct HandoverAdmissionContext {
    /// True only for a complete preamble at the connection's first byte.
    bool isFirstRecord = false;
    /// The holder's original executable path; its current file is the successor.
    std::string_view installPath;
    /// The control directory containing the versioned listening sockets.
    std::string_view controlDirectory;
    /// The holder's own signature, read once as it started: a candidate the
    /// same team signed succeeds it wherever it lies (`successorRoute`).
    CodeSignature holderSignature {};
};

/// Retains the authenticated successor's listening descriptor without serving.
struct AdmittedHandoverRequest {
    Protocol::Sync::HandoverRequest request;
    PAL::UnixFileDescriptor listener;
    std::uint32_t peerProcessID = 0;
    BinaryIdentity binary;
};

/// Reads the path a listening Unix stream socket is bound at.
///
/// A descriptor that is not a listening Unix stream socket, or whose path
/// does not fit its address, has none.
/// - Parameter descriptor: The borrowed socket.
/// - Returns: The bound path, or nothing.
[[nodiscard]] std::optional<std::string> listeningSocketPath(int descriptor);

/// Checks the kernel user of a connected local stream channel.
///
/// This is the requester's first gate on the holder, not successor authority.
/// - Parameter channel: The borrowed connected AF_UNIX/SOCK_STREAM descriptor.
/// - Returns: Whether the kernel effective user equals this process's user.
[[nodiscard]] bool handoverPeerHasSameUser(int channel);

/// Pins the host's four installation-path candidates for this daemon's lifetime.
/// Open files prevent historical inode reuse from becoming adoption authority;
/// the file status captured with each rejects later in-place content
/// replacement. No peer supplies a path or updates this trust set.
///
/// A digest is computed the first time an adoption asks for it, from the held
/// file, and only while that file's status is still the captured one. The
/// daemon accepts no connection until its capture returns, and hashing a
/// debug host of hundreds of megabytes there held every first client, and the
/// launcher waiting for it, behind work only a handoff uses.
class LocalAdoptExecutables {
public:
    LocalAdoptExecutables() = default;
    /// Captures the running host, its live installation path, and their apps'
    /// interfaces (`interfacePathForHost`). Missing candidates grant no authority; no readable candidate
    /// refuses the capture. A new binary's daemon captures its own set.
    /// - Parameter hostPath: The host's kernel-resolved executable path.
    /// - Returns: A pinned trust owner, or refusal without any admitted executable.
    [[nodiscard]] static std::expected<LocalAdoptExecutables, Session::AdoptTransactionError> capture(const std::string& hostPath);

private:
    struct Executable {
        PAL::UnixFileDescriptor file;
        BinaryIdentity identity;
        /// The held file's status when it was captured: contents that still
        /// match it are the captured contents, and have its digest without
        /// reading the file again.
        struct stat captured {};
        /// The digest of the captured contents, once an adoption asked for it.
        /// A memo of the held file, which owns the contents.
        mutable std::optional<std::array<std::uint8_t, 32>> digest;
    };

    /// The digest of a held executable, computed from its file on first use.
    ///
    /// - Parameter executable: One of this set's admitted executables.
    /// - Returns: The digest, or `nullopt` when the file is no longer the one
    ///   captured or cannot be read; such a file grants no authority.
    [[nodiscard]] static std::optional<std::array<std::uint8_t, 32>> digestOf(const Executable& executable);

    std::array<Executable, 4> m_executables;
    friend std::optional<Session::AdoptAuthority> currentLocalAdoptAuthority(int, const LocalAdoptExecutables&);
};

/// Rederives adoption authority from the current physical local peer.
///
/// A cached peer label or inode does not establish a live process incarnation.
/// This checks the socket's kernel lifetime, a trusted running executable and
/// its full digest, then repeats the lifetime and executable checks after
/// hashing. It neither receives a descriptor nor grants ownership of a PTY.
/// Unsupported kernel lifetime verification refuses closed.
/// - Parameters:
///   - channel: Borrowed connected local stream, before any remote greeting.
///   - trusted: Host-owned open executables and digests, never wire values.
/// - Returns: Current kernel-derived facts, or no authority on any mismatch.
[[nodiscard]] std::optional<Session::AdoptAuthority> currentLocalAdoptAuthority(int channel, const LocalAdoptExecutables& trusted);

/// Retains positive kernel exit evidence for one authenticated process birth.
/// Socket closure, a missing process snapshot, and elapsed time prove nothing.
/// The caller separately proves that every admitted channel and operation drained
/// before using this evidence to retire transaction authority.
class LocalAdoptLifetime {
public:
    LocalAdoptLifetime(LocalAdoptLifetime&&) noexcept;
    LocalAdoptLifetime& operator=(LocalAdoptLifetime&&) noexcept;
    LocalAdoptLifetime(const LocalAdoptLifetime&) = delete;
    LocalAdoptLifetime& operator=(const LocalAdoptLifetime&) = delete;

    /// Registers an exit watch between fresh exact physical-authority checks.
    /// Unsupported kernel watches refuse admission rather than falling back to
    /// a PID lookup. The watch never signals or reaps the process.
    /// - Parameters:
    ///   - physicalSocket: The connected authenticated local stream, borrowed.
    ///   - expected: The exact kernel authority already established on that socket.
    ///   - trusted: The daemon-owned immutable executable trust set.
    /// - Returns: A retained exact watch, or refusal without lifetime evidence.
    [[nodiscard]] static std::expected<LocalAdoptLifetime, Session::AdoptTransactionError> capture(
        int physicalSocket, const Session::AdoptAuthority& expected, const LocalAdoptExecutables& trusted);

    /// Polls without waiting and latches only a matching kernel exit event.
    /// Errors and absence of an event leave retirement unproven.
    [[nodiscard]] bool didExit();
    /// Returns the immutable authority whose exact lifetime the watch retains.
    [[nodiscard]] const Session::AdoptAuthority& authority() const { return m_authority; }

private:
    LocalAdoptLifetime(PAL::UnixFileDescriptor, const Session::AdoptAuthority&);
    PAL::UnixFileDescriptor m_watch;
    Session::AdoptAuthority m_authority;
    bool m_didExit = false;
};

/// Authenticates a successor before any pane is quiesced or offered.
///
/// A successor is the file now at the holder's install path, or, on macOS, a
/// host binary the trusted team signed, as the holder is (`successorRoute`);
/// a release build admits only the second. The candidate's signature is read
/// once, from its running code.
///
/// The caller recognizes the exact preamble before its ordinary frame decoder,
/// receives one Request with exactly one descriptor through HandoverTransport,
/// and abandons the channel on refusal. This function performs no channel I/O,
/// process signaling, state transfer, or quiesce. Success grants only admission
/// to the separate handover transaction, not authority to serve its PTYs.
/// Linux requires SO_PEERPIDFD; unsupported kernels refuse this transaction.
/// - Parameters:
///   - channel: Exclusively owned, connected local stream, borrowed here.
///   - listener: Received descriptor, consumed and closed on every refusal.
///   - request: The decoded initial Request.
///   - context: Facts supplied by the holder, not the peer.
/// - Returns: Verified peer and listener ownership, or an explicit refusal.
[[nodiscard]] std::expected<AdmittedHandoverRequest, Protocol::Sync::HandoverRefuseReason> admitHandoverRequest(
    int channel, PAL::UnixFileDescriptor listener, const Protocol::Sync::HandoverRequest& request, const HandoverAdmissionContext& context);

} // namespace Ubiquitty::Host
