// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Core/Session/WorkspaceConnection.h"

#include <compare>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>

namespace Ubiquitty::Session {

/// Identifies an authenticated transport target and a particular host daemon.
struct WorkspaceAuthority {
    std::string endpointIdentity;
    std::string principal;
    std::string daemonSocket;
    std::uint64_t daemonInstanceID { 0 };
    auto operator<=>(const WorkspaceAuthority&) const = default;
};

/// Names a workspace independently of the pane initially shown by a frontend.
struct WorkspaceAttachRequest {
    WorkspaceAuthority authority;
    std::uint32_t workspaceID { 0 };
    std::uint32_t sessionID { 0 };
    std::string workspaceName;
    /// What the connection's HELLO asks when ``sessionID`` has ended. Read
    /// only when the request opens a connection; a reused one keeps its own.
    ReattachPolicy reattachPolicy { ReattachPolicy::AttachOrCreate };
};

/// Reuses workspace connections before transport construction or handshake.
///
/// Owners acquired before the host named their workspace can resolve to one
/// another owner already carries; the later one retires into the earlier
/// (`WorkspaceConnection::retireInto`), or is refused when it shows a pane the
/// earlier one shows.
///
/// The construction thread owns connection callbacks and subscription lifetimes.
/// Acquires from other threads fail before looking up or creating connections.
/// The registry and its connections are destroyed on that same event-loop thread.
class WorkspaceConnectionRegistry {
public:
    /// Describes an attachment that cannot safely share this registry.
    enum class Error {
        InvalidAuthority,
        DifferentEventLoop,
        CreationFailed,
        /// The owner has not been admitted to a workspace the host named.
        Unresolved,
        /// Another live owner already carries the workspace under this authority.
        Duplicate
    };
    /// Retains the selected owner and reports whether transport creation was avoided.
    struct Lease {
        std::shared_ptr<WorkspaceConnection> connection;
        bool wasReused { false };
    };
    /// Allocates an unstarted connection; the registry installs its identity before returning.
    using Create = std::function<std::shared_ptr<WorkspaceConnection>()>;

    /// Binds connection ownership to the current event-loop thread.
    WorkspaceConnectionRegistry();
    /// Removes all registry subscriptions on the owning event-loop thread.
    ~WorkspaceConnectionRegistry();
    WorkspaceConnectionRegistry(const WorkspaceConnectionRegistry&) = delete;
    WorkspaceConnectionRegistry& operator=(const WorkspaceConnectionRegistry&) = delete;
    /// Reuses an exact workspace identity, or authoritative session membership within its authority.
    ///
    /// The creation callback must not start a channel or re-enter the registry.
    /// All owners share the registry's construction event-loop thread.
    [[nodiscard]] std::expected<Lease, Error> acquire(const WorkspaceAttachRequest&, Create);

    /// Indexes a connected owner under the workspace its host admitted it to.
    ///
    /// For an owner that learned its workspace after it attached, as a local
    /// window share learns it from the host's answer to its publication; later
    /// acquires of that workspace reuse it. Nothing is changed when another
    /// live owner already carries the workspace: that owner stays indexed, and
    /// neither is refused.
    ///
    /// - Parameters:
    ///   - authority: The transport target the owner reaches; its acknowledged
    ///     daemon instance and socket complete the key.
    ///   - connection: The owner, attached and acknowledged.
    /// - Returns: Nothing once the owner is indexed, which it may already be.
    [[nodiscard]] std::expected<void, Error> adopt(const WorkspaceAuthority& authority, const std::shared_ptr<WorkspaceConnection>& connection);

    /// The live owner indexed for the workspace a connected owner carries,
    /// under the keys ``adopt`` gives it: the authority with the owner's
    /// acknowledged daemon instance, and with the daemon socket its host named.
    ///
    /// - Parameters:
    ///   - authority: The transport target the owner reaches.
    ///   - connection: The owner, connected to a workspace the host named.
    /// - Returns: The indexed owner, which may be `connection` itself; null
    ///   when none is, the one indexed failed, or the caller is not on the
    ///   registry's event loop.
    [[nodiscard]] std::shared_ptr<WorkspaceConnection> indexed(const WorkspaceAuthority& authority, const WorkspaceConnection& connection) const;

private:
    struct RegistryStorage;
    std::shared_ptr<RegistryStorage> m_state;
};

} // namespace Ubiquitty::Session
