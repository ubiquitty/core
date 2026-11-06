// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/HandoverBody.h"
#include "Platform/Host/Daemon.h"

namespace Ubiquitty::Host {

/// Maps the host's lifecycle owners to the portable pane record.
/// Compound owners are restored separately; these facts never authorize service.
struct SessionHandoverFacts {
    /// Borrows one frozen Session and copies its exact portable facts.
    /// - Parameter session: The unchanged holder under the quiescence lease.
    /// - Returns: Lifecycle, presentation and cleanup-path facts without consuming them.
    [[nodiscard]] static Protocol::Sync::SessionFacts capture(const Daemon::Session& session);

    /// Moves admitted facts into a fresh, unpublished Session.
    ///
    /// The candidate does not own cleanup of the carried paths until commit.
    /// Destroying a refused candidate therefore cannot unlink the holder's files.
    /// - Parameters:
    ///   - session: Fresh private owner, not yet installed in a daemon.
    ///   - facts: A complete decoded and admitted facts record.
    /// - Returns: Whether facts moved; an occupied receiver changes neither side.
    [[nodiscard]] static bool restore(Daemon::Session& session, Protocol::Sync::SessionFacts&& facts);
};

} // namespace Ubiquitty::Host
