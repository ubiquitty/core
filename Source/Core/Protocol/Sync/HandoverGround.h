// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Bits/OptionSet.h"

#include <cstdint>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::Protocol::Sync {

/// A terminal-owned operation that must finish before its handover snapshot.
enum class HandoverBlocker : std::uint32_t {
    None = 0,
    ParserMidSequence = 1u << 0,
    IncompleteCharacter = 1u << 1,
    LegacySequence = 1u << 2,
    DeviceControlString = 1u << 3,
    ClipboardChunkOpen = 1u << 4,
    ClipboardReadPending = 1u << 5,
    ExtendedClipboardOpen = 1u << 6,
    ImageTransferOpen = 1u << 7,
    ImageDecodePending = 1u << 8,
    NotificationOpen = 1u << 9,
    SynchronizedOutput = 1u << 10,
    DeferredScreenMutation = 1u << 11,
    ExternalSinkOpen = 1u << 12,
    FontOperationPending = 1u << 13,
    IntegrationActive = 1u << 14,
    RichTextMeasurement = 1u << 15,
    NativeDrag = 1u << 16,
    /// ERM is set, GRCM is reset, or a DAQ qualified area is defined: host state
    /// that no handover section carries yet, so a handover waits for it to end.
    ErasureQualification = 1u << 17,
};

using HandoverBlockers = Bits::OptionSet<HandoverBlocker>;

/// Inspects terminal-owned work without draining, resetting, or completing it.
///
/// An empty result is necessary, not sufficient, for a whole-host handover:
/// the owner must also freeze session callbacks, pending PTY/input ownership,
/// authority, effects, and transports at a coherent publication boundary.
/// Installed sinks are not invoked. The owner holds its model lock throughout.
///
/// - Parameter terminal: The terminal whose pending work is inspected.
/// - Returns: Every terminal blocker, without modifying any program state.
[[nodiscard]] HandoverBlockers terminalHandoverBlockers(const Terminal& terminal);

} // namespace Ubiquitty::Protocol::Sync
