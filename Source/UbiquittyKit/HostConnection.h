// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UbiquittyKit/Session.h"
#include <expected>
#include <memory>
#include <string>

namespace UbiquittyKit {
/// An explicitly installed host, reached by local pipes or noninteractive SSH.
struct HostConnectionOptions {
    std::string hostBinary = "ubiquitty-host";
    std::string sshDestination;
    std::string socketName;
};
/// Builds bounded subprocess carriers for a Session or Workspace.
///
/// Starts the selected daemon only on the first connection without an explicit
/// socket. Reconnects use the host's acknowledged socket. SSH uses BatchMode
/// and never deploys a host or changes authentication configuration.
///
/// - Parameter options: The installed executable, optional SSH target and socket.
/// - Returns: The owned factory, or a validation error without starting anything.
[[nodiscard]] std::expected<std::unique_ptr<Ubiquitty::Session::ChannelFactory>, std::string> makeHostChannelFactory(HostConnectionOptions options);
} // namespace UbiquittyKit
