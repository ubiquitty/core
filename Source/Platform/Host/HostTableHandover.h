// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/Daemon.h"
#include "Platform/Host/HandoverAuthorityTables.h"
#include "Platform/Host/HandoverGlobalTables.h"
#include "Platform/Host/HandoverTables.h"

namespace Ubiquitty::Host {

struct HostHandover;

/// Why a successor refused what a holder handed it.
struct HandoverPrepareRefusal {
    /// The error the refusal sent to the holder is derived from.
    Protocol::Sync::HandoverBodyError error = Protocol::Sync::HandoverBodyError::Malformed;
    /// The check that failed, in words for the host log.
    std::string check;
};

/// Builds the host's table owners without exposing a partially restored daemon.
struct HostTableHandover {
    /// Independent aggregate and nested bounds for one host table record.
    struct Limits {
        std::size_t maxRecordBytes = 0;
        HandoverAttachmentLimits attachments;
        HandoverWorkspaceLimits workspaces;
        HandoverAuthorityLimits authority;
        HandoverGlobalLimits globals;
    };

    /// Owns unpublished tables and their private workspace allocator.
    class Candidate {
    public:
        /// Releases unpublished owners without executing deferred host work.
        ~Candidate();
        /// Transfers unpublished owners without publishing or servicing them.
        /// - Parameter source: Candidate consumed by this move.
        Candidate(Candidate&&) noexcept;
        /// Replaces unpublished owners without executing either candidate's work.
        /// - Parameter source: Candidate consumed by this move.
        /// - Returns: This candidate after taking the source's owners.
        Candidate& operator=(Candidate&&) noexcept;
        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;

        /// Checks complete session identity, placement and retained authority bindings.
        /// - Parameter sessions: The independently restored unpublished sessions.
        /// - Returns: Whether every session has exactly one valid owner and binding.
        [[nodiscard]] bool validateSessions(std::span<const std::unique_ptr<Daemon::Session>> sessions) const;

        /// Forgets the baselines attachments keep for panes they no longer
        /// hold when the pane's host is neither among the carried host
        /// identities nor the host of a pane handed over.
        ///
        /// Such a pane has closed and its host has left the resume window. A
        /// later bind of it is sent a checkpoint, and the rest of the
        /// handover goes ahead.
        ///
        /// - Parameter sessions: The independently restored unpublished sessions.
        /// - Returns: How many baselines were forgotten.
        std::size_t forgetBaselinesOfDepartedPanes(std::span<const std::unique_ptr<Daemon::Session>> sessions);

        /// Borrows the prepared attachment table.
        /// - Returns: The table, valid for this candidate's lifetime.
        [[nodiscard]] const Protocol::Sync::AttachmentTable& attachments() const;

        /// Borrows the exact decoded topology and allocator high-waters.
        /// - Returns: Immutable predecessor tables, valid for this candidate's lifetime.
        [[nodiscard]] const HandoverWorkspaceTables& workspaces() const;
        /// Borrows the exact pending consent, grant and bootstrap-token identities.
        /// - Returns: Immutable predecessor authority, without consuming grants.
        [[nodiscard]] const HandoverAuthorityTables& authority() const;
        /// Borrows retained journals, timers and owed operations.
        /// - Returns: Immutable predecessor global state.
        [[nodiscard]] const HandoverGlobalTables& globals() const;

    private:
        friend struct HostTableHandover;
        struct OwnedTables;
        explicit Candidate(std::unique_ptr<OwnedTables>);
        /// Whether a pane minted by `issuer` is known: a host among the
        /// carried identities, or the host of a pane handed over.
        [[nodiscard]] bool isKnownIssuer(std::span<const std::unique_ptr<Daemon::Session>> sessions, std::uint64_t issuer) const;
        std::unique_ptr<OwnedTables> m_state;
    };

    /// Captures frozen host tables into one bounded length-delimited record.
    /// Physical RPC owners must already be settled; ordinary queued input is carried
    /// by the session record and is not discarded by this operation.
    /// - Parameters:
    ///   - daemon: Quiesced predecessor, unchanged throughout the capture.
    ///   - now: Shared monotonic freeze time in microseconds.
    ///   - limits: Independent receiver bounds for the entire record.
    /// - Returns: Owned record or refusal without changing predecessor state.
    [[nodiscard]] static std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> capture(
        const Daemon& daemon, std::uint64_t now, const Limits& limits);

    /// Whether workspace and event records at these paths are kept beside
    /// the socket at `socketPath`.
    ///
    /// A successor takes a holder's records only beside the socket it reached
    /// the holder through, so a holder keeps that socket among those it hands
    /// on.
    ///
    /// - Parameters:
    ///   - layoutRecordPath: Where the workspace records are written.
    ///   - backgroundEventPath: Where the background events are written.
    ///   - socketPath: A listening socket's path.
    /// - Returns: Whether both paths are that socket's own.
    [[nodiscard]] static bool areRecordsKeptBeside(std::string_view layoutRecordPath, std::string_view backgroundEventPath, std::string_view socketPath);

    /// Prepares private tables and revalidates authority against current admission.
    /// No path is opened or written except the existing current-root token check.
    /// - Parameters:
    ///   - daemon: Stable unpublished successor with its admitted control root.
    ///   - bytes: One authenticated predecessor table record.
    ///   - predecessor: Full issuer identity verified by local admission.
    ///   - now: Current shared monotonic time in microseconds.
    ///   - nowUnix: Current Unix time in seconds for token expiry.
    ///   - expectedPredecessorSocketPath: Exact socket path pinned by admission,
    ///     beside which the carried records must be kept.
    ///   - limits: Receiver-owned aggregate and section bounds.
    /// - Returns: Unpublished candidate, or the failed check without changing the daemon.
    [[nodiscard]] static std::expected<std::unique_ptr<Candidate>, HandoverPrepareRefusal> prepare(
        Daemon& daemon, std::span<const std::uint8_t> bytes, Protocol::Sync::ID128 predecessor,
        std::uint64_t now, std::uint64_t nowUnix, std::string_view expectedPredecessorSocketPath, const Limits& limits);

private:
    friend struct HostHandover;
    friend struct HandoverAccess;
    /// Publishes tables only as part of the already-validated whole-host transaction.
    /// - Parameters:
    ///   - daemon: Stable successor, still excluded from service.
    ///   - candidate: Fully cross-bound candidate, consumed without callbacks or I/O.
    static void commit(Daemon& daemon, Candidate&& candidate);
};

} // namespace Ubiquitty::Host
