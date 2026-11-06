// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Whether the app should hold secure keyboard entry, and what to say it for.
//
// Four facts decide it and none is platform-specific: the standing preference,
// the at-a-password-prompt preference, whether a pane is actually at one, and
// whether the app is frontmost. The DECISION is here so there is one of it;
// what a platform owns is the mechanism it holds the resource with —
// EnableSecureEventInput on macOS, whatever an X11 or Wayland frontend would
// use — and the badge it draws to say so.
//
// The frontmost term is the one worth stating. Secure input is a process-wide
// grab: an app holding it in the background keeps every other app from reading
// its own keystrokes, which users experience as a broken keyboard elsewhere on
// the system rather than as a security feature.
//
// The facts arrive as booleans rather than as a settings fold, so the decision
// carries no dependency on the settings model. The caller already holds them.

#include <cstdint>

namespace Ubiquitty::Security {

/// Why secure keyboard entry is being held, which is what a badge says.
enum class SecureInputReason : std::uint8_t {
    /// Not held.
    None = 0,
    /// The standing preference asked for it, whatever is on screen.
    Always = 1,
    /// A pane is at a password prompt and the preference follows prompts.
    PasswordPrompt = 2,
};

/// The facts a secure-input hold is decided from.
///
/// A struct rather than four parameters, for the same reason the clipboard
/// scope takes one: four bare booleans at a call site cannot be read, and a
/// transposed pair still compiles.
struct SecureInputFacts {
    /// The standing preference: `[security] secure_keyboard_entry`.
    bool isAlwaysPreferred = false;
    /// `[security] secure_keyboard_entry_at_password_prompts`.
    bool isPreferredAtPasswordPrompts = false;
    /// Whether any pane of this app is at a password prompt.
    bool isAnyPaneAtPasswordPrompt = false;
    /// Whether the app is frontmost.
    ///
    /// Secure input is a process-wide grab, so holding it in the background
    /// takes the keyboard from whatever the user actually switched to.
    bool isApplicationActive = false;
};

/// What holds secure input right now, if anything.
///
/// - Parameter facts: What is known about the app and its panes.
/// - Returns: The reason, or ``SecureInputReason/None``.
[[nodiscard]] constexpr SecureInputReason secureInputReason(SecureInputFacts facts)
{
    if (!facts.isApplicationActive)
        return SecureInputReason::None;
    if (facts.isAlwaysPreferred)
        return SecureInputReason::Always;
    if (facts.isAnyPaneAtPasswordPrompt && facts.isPreferredAtPasswordPrompts)
        return SecureInputReason::PasswordPrompt;
    return SecureInputReason::None;
}

/// Whether secure keyboard entry should be held.
///
/// - Parameter facts: What is known about the app and its panes.
/// - Returns: Whether to hold it.
[[nodiscard]] constexpr bool shouldHoldSecureInput(SecureInputFacts facts)
{
    return secureInputReason(facts) != SecureInputReason::None;
}

} // namespace Ubiquitty::Security
