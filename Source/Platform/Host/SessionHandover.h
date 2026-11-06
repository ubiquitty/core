// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Preferences/HandoverValues.h"
#include "Platform/Host/HandoverEffects.h"
#include "Platform/Host/HandoverRuntime.h"
#include "Platform/Host/SessionHandoverFacts.h"

namespace Ubiquitty::Host {

/// Receiver-selected ceilings for one complete host session, not sender policy.
struct SessionHandoverLimits {
    Protocol::Sync::HandoverPaneLimits pane;
    HandoverEffectsLimits effects;
    HandoverRuntimeLimits runtime;
    Preferences::HandoverValuesLimits configuration;
    std::size_t maxRecordBytes = 0;
};

/// Joins the actual session's owners without publishing a partial successor.
struct SessionHandover {
    using Session = Daemon::Session;

    /// Captures a session held by the daemon's whole-host quiescence lease.
    /// No terminal seed, publication, input drain or capability check runs here.
    /// - Parameters:
    ///   - session: Frozen holder with its original stream and input owners.
    ///   - limits: Independent receiver bounds for the entire session.
    /// - Returns: Exact owned record or refusal, without consuming holder state.
    [[nodiscard]] static std::expected<Protocol::Sync::HandoverBytes, Protocol::Sync::HandoverBodyError> capture(
        const Session& session, const SessionHandoverLimits& limits);

    /// Restores and binds an unpublished session without servicing its stream.
    /// The caller must authenticate and validate the inherited PTY first. Its
    /// process-close policy becomes KeepRunning immediately, including refusal.
    /// Neither the stream's child nor pasted files become this candidate's
    /// cleanup responsibility until the whole-host commit transfers ownership.
    /// - Parameters:
    ///   - daemon: Stable successor whose carried host tables precede service.
    ///   - bytes: One complete private session record.
    ///   - stream: Exclusively owned inherited endpoint, consumed on every path.
    ///   - limits: Receiver bounds independent of the incoming configuration.
    /// - Returns: A private bound session, or refusal without touching live owners.
    [[nodiscard]] static std::expected<std::unique_ptr<Session>, Protocol::Sync::HandoverBodyError> restore(
        Daemon& daemon, std::span<const std::uint8_t> bytes, std::unique_ptr<PAL::Stream> stream,
        const SessionHandoverLimits& limits);
};

} // namespace Ubiquitty::Host
