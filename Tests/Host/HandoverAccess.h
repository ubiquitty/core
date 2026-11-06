// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"

namespace Ubiquitty::Host {

/// Drives the daemon's handover boundary without a second reactor thread.
struct HandoverAccess {
    /// Names a session owned by the test daemon.
    using Session = Daemon::Session;
    /// Names one physical connection retained by the test daemon.
    using Client = Daemon::Client;
    /// Borrows a physical connection to exercise unresolved operation owners.
    /// - Parameters:
    ///   - daemon: The holder.
    ///   - index: An existing client's index.
    /// - Returns: The holder-owned physical client.
    static Client& client(Daemon& daemon, std::size_t index) { return daemon.m_clients.at(index); }
    /// Checks the production admission boundary without altering any owner.
    /// - Parameter daemon: The holder.
    /// - Returns: Whether a non-carried operation prevents quiescence.
    static bool hasBlockedOperations(const Daemon& daemon) { return daemon.hasHandoverBlockedOperations(); }
    /// Names what keeps the holder from handing over, as its refusal says it.
    /// - Parameter daemon: The holder.
    /// - Returns: The blocker, or `nullopt` when nothing blocks.
    static std::optional<Protocol::Sync::HandoverRefuseReason> blocker(const Daemon& daemon) { return daemon.handoverBlocker(); }
    /// Holds a question an agent's tree change waits on, as one being asked.
    /// - Parameter daemon: The holder.
    static void holdApprovalQuestion(Daemon& daemon) { daemon.m_structureConsents.emplace(1, Daemon::PendingStructureCall {}); }
    /// Holds a capability question waiting for an answer.
    /// - Parameter daemon: The holder.
    static void holdCapabilityQuestion(Daemon& daemon) { daemon.m_pendingCapabilityGrants.emplace(1, Daemon::PendingCapabilityGrant {}); }
    /// Forgets the questions `holdApprovalQuestion` and `holdCapabilityQuestion` held.
    /// - Parameter daemon: The holder.
    static void forgetQuestions(Daemon& daemon)
    {
        daemon.m_structureConsents.clear();
        daemon.m_pendingCapabilityGrants.clear();
    }
#ifdef UBIQUITTY_QUIC
    /// Installs a real queue owner without starting a network listener.
    /// - Parameter daemon: The holder with no existing QUIC listener.
    /// - Returns: The newly owned listener.
    static QUICListener& installQUIC(Daemon& daemon)
    {
        daemon.m_quic = std::make_unique<QUICListener>();
        return *daemon.m_quic;
    }
#endif
    /// Starts the production quiesce attempt.
    /// - Parameters:
    ///   - daemon: The holder.
    ///   - now: Monotonic microseconds.
    /// - Returns: Whether the attempt started.
    static bool begin(Daemon& daemon, std::uint64_t now) { return daemon.beginHandoverQuiesce(now); }
    /// Advances one bounded pass.
    /// - Parameters:
    ///   - daemon: The holder.
    ///   - now: Monotonic microseconds.
    static void step(Daemon& daemon, std::uint64_t now) { daemon.serviceHandoverQuiesce(now); }
    /// Checks every frozen publication.
    /// - Parameter daemon: The holder.
    /// - Returns: Whether the whole host can be captured.
    static bool isQuiesced(const Daemon& daemon) { return daemon.isQuiesced(); }
    /// Checks whether normal service is held.
    /// - Parameter daemon: The holder.
    /// - Returns: Whether an attempt remains active.
    static bool isActive(const Daemon& daemon) { return daemon.m_quiescingPanes.has_value(); }
    /// Reads the last attempt's refusal state.
    /// - Parameter daemon: The holder.
    /// - Returns: Whether the attempt failed.
    static bool didFail(const Daemon& daemon) { return daemon.m_didHandoverQuiesceFail; }
    /// Resumes normal service without consuming queued input.
    /// - Parameter daemon: The holder.
    static void end(Daemon& daemon) { daemon.endHandoverQuiesce(); }
    /// Runs the ordinary socket input entry point.
    /// - Parameters:
    ///   - daemon: The holder.
    ///   - index: The connected client's index.
    static void serviceClient(Daemon& daemon, std::size_t index) { daemon.serviceClient(index); }
    /// Drains a leased number of program-output bytes.
    /// - Parameters:
    ///   - daemon: The holder.
    ///   - session: The pane whose stream is read.
    ///   - maxBytes: The read allowance.
    /// - Returns: The actual drain result.
    static Runtime::DrainPassResult drain(Daemon& daemon, Daemon::Session& session, int maxBytes) { return daemon.servicePTY(session, maxBytes); }
    /// Reads one pane's quiesce phase.
    /// - Parameters:
    ///   - daemon: The holder with an active attempt.
    ///   - index: Its pane index.
    /// - Returns: The controller phase.
    static Runtime::QuiescePhase phase(const Daemon& daemon, std::size_t index) { return daemon.m_quiescingPanes->at(index).controller.phase(); }
    /// Counts bytes consumed after ordinary reads stop.
    /// - Parameters:
    ///   - daemon: The holder with an active attempt.
    ///   - index: Its pane index.
    /// - Returns: The spent one-byte allowances.
    static std::size_t groundBytes(const Daemon& daemon, std::size_t index) { return daemon.m_quiescingPanes->at(index).controller.groundBytesRead(); }
};

} // namespace Ubiquitty::Host
