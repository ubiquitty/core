// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A client's granted role bounds each frame before dispatch. The
// credential's drive scope and each method's admission checks still apply.

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Roles.h"

#include <cstddef>
#include <iterator>

namespace Ubiquitty::Protocol::Sync {

/// The roles allowed to send one client-to-host frame kind.
struct FrameGate {
    FrameKind kind;
    Roles senders;
};

/// Every frame kind a client can send, in wire order. Call admission also
/// checks the operation it carries.
inline constexpr FrameGate frameGates[] {
    { FrameKind::Greet, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Facts, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Ack, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Probe, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Echo, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::BulkChunk, { Role::Owner, Role::Driver, Role::Agent } },
    { FrameKind::Bye, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::TypographyFacts, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Call, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::ProportionalFaces, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Resync, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::View, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::HistoryWant, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Keys, { Role::Owner, Role::Driver, Role::Agent } },
    { FrameKind::Pointer, { Role::Owner, Role::Driver, Role::Agent } },
    { FrameKind::Paste, { Role::Owner, Role::Driver, Role::Agent } },
    { FrameKind::PasteAbort, { Role::Owner, Role::Driver, Role::Agent } },
    { FrameKind::Raw, { Role::Owner, Role::Driver, Role::Agent } },
    { FrameKind::Focus, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::Viewport, { Role::Owner, Role::Driver } },
    { FrameKind::ClipboardData, { Role::Owner, Role::Driver } },
    { FrameKind::ImageWant, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::RichTextMeasured, { Role::Owner, Role::Driver, Role::Viewer, Role::Agent } },
    { FrameKind::NativeDragEvent, { Role::Owner, Role::Driver } },
};

static_assert([] {
    for (std::size_t index = 0; index < std::size(frameGates); ++index) {
        if (frameGates[index].senders.isEmpty())
            return false;
        if (index && std::to_underlying(frameGates[index - 1].kind) >= std::to_underlying(frameGates[index].kind))
            return false;
    }
    return true;
}());

/// Checks a frame's sender against its granted role.
///
/// - Parameters:
///   - kind: The frame kind.
///   - role: The role the host granted this connection.
/// - Returns: Whether that role may send the kind.
[[nodiscard]] constexpr bool admitsSender(FrameKind kind, Role role)
{
    for (const FrameGate& gate : frameGates) {
        if (gate.kind == kind)
            return gate.senders.contains(role);
    }
    return false;
}

} // namespace Ubiquitty::Protocol::Sync
