// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Sync/NativeAttachment.h"

#include <cstdint>
#include <string>

namespace Ubiquitty::Protocol::Sync {

/// Whether a connection attaches a session or only addresses the host.
enum class HandshakeTarget : std::uint8_t {
    Session,
    Control,
};

/// What a client asks to attach, independent of the frames carrying its request.
///
/// The same viewport and agent label seed admission and the screen greeting.
/// A control request names no session; its wire representation is the codec's
/// responsibility.
struct HandshakeRequest {
    HandshakeTarget target = HandshakeTarget::Session;
    std::uint16_t columns = 0;
    std::uint16_t rows = 0;
    std::uint32_t sessionID = 0;
    HelloIntents intents;
    std::string agentLabel;
    std::string authToken;
    std::string sshHost;
    std::string workspaceName;
    std::uint8_t sessionFlags = 0;
    std::uint32_t workspaceID = 0;
    std::uint64_t expectedDaemonInstanceID = 0;

    /// Whether the request addresses the host without attaching a session.
    [[nodiscard]] bool isControlOnly() const { return target == HandshakeTarget::Control; }
    /// The kind the client declares; the host verifies it independently.
    [[nodiscard]] Sync::ClientKind clientKind() const { return agentLabel.empty() ? Sync::ClientKind::Human : Sync::ClientKind::Agent; }
};

}
