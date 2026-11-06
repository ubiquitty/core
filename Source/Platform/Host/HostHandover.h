// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/HandoverAdmission.h"
#include "Platform/Host/HostTableHandover.h"
#include "Platform/Host/SessionHandover.h"

#include <chrono>

namespace Ubiquitty::Host {

/// Receiver-selected ceilings for the complete transaction, not per-pane claims.
struct HostHandoverLimits {
    SessionHandoverLimits session;
    HostTableHandover::Limits tables;
    std::size_t maxSessions = 0;
    std::size_t maxAggregateBytes = 0;
};

/// Owns one frozen session's record without changing its live PTY owner.
struct HostHandoverSession {
    Protocol::Sync::ID128 paneID;
    Protocol::Sync::HandoverBytes record;
};

/// An all-or-nothing copy of the frozen host, with no serving authority.
struct HostHandoverSnapshot {
    Protocol::Sync::ID128 predecessor;
    std::vector<HostHandoverSession> sessions;
    Protocol::Sync::HandoverBytes tables;
};

/// Joins private session and table owners before the service lease can move.
struct HostHandover {
    using Error = Protocol::Sync::HandoverBodyError;
    using Session = Daemon::Session;
    static constexpr int transactionTimeoutMilliseconds = 10'000;
    /// How long an admitted request waits, with no pane read, for this host
    /// to owe its clients nothing before its transaction starts: a frame or a
    /// reply part-written, or QUIC data a client has not acknowledged. Well
    /// inside the successor's transaction bound, so a request that cleared
    /// late still finds its successor waiting.
    static constexpr int maxOwedOutputWaitMilliseconds = 2'000;

    /// The ceilings both ends of a real transaction apply: the holder's capture
    /// refuses what the successor's receive would refuse, before any PTY moves.
    /// - Returns: Receiver-selected bounds, independent of any peer field.
    [[nodiscard]] static const HostHandoverLimits& transactionLimits();

    /// Owns every privately restored session and its cross-bound host tables.
    class Candidate {
    public:
        /// Releases unpublished owners without terminating the holder's children.
        ~Candidate();
        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;
        /// - Returns: The number of privately restored sessions.
        [[nodiscard]] std::size_t sessionCount() const { return m_sessions.size(); }

    private:
        friend struct HostHandover;
        friend struct HandoverAccess;
        explicit Candidate(Daemon& destination);
        Daemon& m_destination;
        std::unique_ptr<HostTableHandover::Candidate> m_tables;
        std::vector<std::unique_ptr<Session>> m_sessions;
    };

    /// Copies all owners only while the daemon's whole-host freeze is valid.
    /// - Parameters:
    ///   - daemon: The unchanged holder, frozen at accepted pane publications.
    ///   - now: Shared monotonic time for retained attachment ages.
    ///   - limits: Independent per-section and aggregate receiver bounds.
    /// - Returns: Complete privately owned bytes, or refusal without consuming state.
    [[nodiscard]] static std::expected<HostHandoverSnapshot, Error> capture(
        const Daemon& daemon, std::uint64_t now, const HostHandoverLimits& limits);

    /// Reconstructs every owner without touching the destination's live tables.
    /// Streams are consumed with KeepRunning even when another part is refused.
    /// The transport must first authenticate every endpoint and its geometry.
    /// - Parameters:
    ///   - daemon: Stable destination, not yet serving sessions or clients.
    ///   - snapshot: Complete authenticated records in announced pane order.
    ///   - streams: Exclusive inherited endpoints in the same order.
    ///   - predecessor: Identity authenticated by the surrounding transaction.
    ///   - now: Current shared monotonic time in microseconds.
    ///   - nowUnix: Current Unix time for token revalidation.
    ///   - predecessorSocketPath: Exact admitted path constraining carried storage paths.
    ///   - limits: Receiver ceilings independent of encoded configuration.
    /// - Returns: One unpublished candidate, or the check that failed;
    ///   destruction preserves the holder's children and files.
    [[nodiscard]] static std::expected<std::unique_ptr<Candidate>, HandoverPrepareRefusal> prepare(
        Daemon& daemon, HostHandoverSnapshot&& snapshot, std::vector<std::unique_ptr<PAL::Stream>> streams,
        Protocol::Sync::ID128 predecessor, std::uint64_t now, std::uint64_t nowUnix,
        std::string_view predecessorSocketPath, const HostHandoverLimits& limits);

    /// Serves one authenticated successor transaction from the holder.
    ///
    /// Runs synchronously on the reactor: no client is read and no PTY is read
    /// or written between the frozen capture and the verdict. The holder sends
    /// `Offer` with the service lease, then per pane `PaneFollows` with its
    /// master and the pane's record, then the host tables, and waits for the
    /// verdict. `Accept` makes it say `Bye{HandingOver}` to every connection,
    /// keep every child running, release the lease and send `Release`, which
    /// carries its own listening socket, the one its records are kept beside
    /// and the newest others it serves, up to `maxHandoverListeners`. A
    /// refusal, a closed channel or an elapsed deadline before `Release`
    /// cancels the lease and resumes service with every pane where it was.
    /// After `Release`, only a successor that never claimed the lease lets the
    /// holder resume; otherwise the successor owns every pane.
    /// - Parameters:
    ///   - daemon: The serving holder.
    ///   - channel: The admitted successor's exclusive channel, consumed.
    ///   - admitted: The authenticated request and the successor's process.
    /// - Returns: True only after this holder has relinquished service permanently.
    [[nodiscard]] static bool offer(Daemon& daemon, PAL::UnixFileDescriptor channel, AdmittedHandoverRequest admitted);

    /// Receives a requested predecessor into the not-yet-serving successor.
    ///
    /// The caller has sent Request and its own listener over this exclusive
    /// channel. Every announced size is checked against `transactionLimits`
    /// before its record is read, and every master is corroborated against the
    /// kernel before it becomes a stream. Nothing is published and no PTY is
    /// read until the holder releases the lease and this daemon claims it.
    /// - Parameters:
    ///   - daemon: The successor, bound but not yet serving.
    ///   - channel: The authenticated channel to the holder, consumed.
    ///   - predecessorSocketPath: The holder's admitted socket path.
    ///   - authenticatedPredecessorPID: The holder's kernel-verified process.
    ///   - deadline: The absolute end of the whole transaction.
    /// - Returns: Number of moved sessions, or refusal with the holder still authoritative.
    [[nodiscard]] static std::expected<std::size_t, Protocol::Sync::HandoverRefuseReason> inherit(
        Daemon& daemon, PAL::UnixFileDescriptor channel, std::string_view predecessorSocketPath,
        std::uint32_t authenticatedPredecessorPID, std::chrono::steady_clock::time_point deadline);

private:
    friend struct HandoverAccess;
    /// Moves only prevalidated owners. The transaction must hold the exclusive
    /// service lease and keep the destination reactor stopped throughout.
    static void commit(Candidate&&);
};

} // namespace Ubiquitty::Host
