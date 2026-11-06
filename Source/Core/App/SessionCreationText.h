// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::App {

/// Explains the partial result when a session was created but its separate
/// label operation was refused. The session remains available to the user.
///
/// - Parameters:
///   - session: The created session's identity.
///   - pane: Its pane, zero only when the listing could not resolve it.
///   - reason: The label operation's refusal, denial or timeout.
///   - wasCommandRequested: Whether an optional command was supplied.
/// - Returns: Wording that never claims complete success or silent rollback.
[[nodiscard]] inline std::string sessionCreatedWithoutLabelText(std::uint32_t session, std::uint32_t pane, std::string_view reason, bool wasCommandRequested) // bool-ok: a fact about the original request, not a mode selected by the caller
{
    std::string text = "Created session:" + std::to_string(session);
    text += pane ? " (pane:" + std::to_string(pane) + ")" : " (pane identity unavailable)";
    text += ", but its label was not applied: ";
    text += reason;
    text += ". The session remains available.";
    if (wasCommandRequested)
        text += " The requested command was not started.";
    return text;
}

} // namespace Ubiquitty::App
