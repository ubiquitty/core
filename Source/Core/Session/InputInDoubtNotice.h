// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// InputInDoubtNotice.h — what a pane says while some of what was typed into it
// may not have reached the host.
//
// The attachment decides which input is in doubt (`PaneInputStatus`). This
// decides what the person is told of it and what the two choices are called,
// once for every frontend: the warning that sending again can run a command
// twice is the reason the choice is the person's, so it reads the same
// wherever it is shown.
//

#include "Core/Protocol/Sync/ClientAttachment.h"

#include <optional>
#include <string>

namespace Ubiquitty::Session {

/// The notice a pane shows while its input awaits a decision. It never
/// blocks the pane: the picture stays readable, and what the person types
/// waits behind the input in doubt.
struct InputInDoubtNotice {
    /// The status line: what happened.
    std::string message;
    /// The sentence beneath it: why the choice is the person's.
    std::string detail;
    /// The choice that sends the input in doubt again, under the numbers it
    /// first had, so the host writes none of it twice.
    std::string resendButton;
    /// The choice that drops it and lets what waits behind it go.
    std::string discardButton;
};

/// Words the notice for a pane's input.
///
/// - Parameter status: The pane's input as the attachment decided it.
/// - Returns: The notice, or `nullopt` when no decision is awaited.
[[nodiscard]] inline std::optional<InputInDoubtNotice> inputInDoubtNotice(const Protocol::Sync::PaneInputStatus& status)
{
    if (!status.isAwaitingDecision())
        return std::nullopt;
    return InputInDoubtNotice {
        .message = "Some of what you typed may not have reached the host",
        .detail = "Sending it again could run a command twice. What you type next waits until you choose.",
        .resendButton = "Send Again",
        .discardButton = "Discard",
    };
}

} // namespace Ubiquitty::Session
