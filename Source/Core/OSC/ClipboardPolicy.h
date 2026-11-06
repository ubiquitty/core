// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// OSC 52 clipboard read/write policy enums — shareable by Terminal.h (which cannot
// include Dispatch.h due to the Dispatch.h → Terminal.h inclusion cycle) and by
// Dispatch.h itself.

#include <cstdint>
#include <string_view>

namespace Ubiquitty::OSC {

/// OSC 52 READ ('?' query) security policy.
///   Allow  — read + respond inline.
///   Deny   — never read; reply with an empty data field.
///   Prompt — defer to frontend confirmation (default; the safe fail-closed choice).
enum class ClipboardReadPolicy : uint8_t {
    Prompt = 0,
    Allow = 1,
    Deny = 2
};

// Map a config string ("prompt" | "allow" | "deny", case-insensitive) to a
// ClipboardReadPolicy; unrecognized input returns Prompt (the safe default).
[[nodiscard]] ClipboardReadPolicy clipboardReadPolicyFromString(std::string_view s);

/// OSC 52 WRITE (SET) security policy. Prompt is absent: a write carries no
/// secret out of the machine, so there is nothing for a person to arbitrate.
///   Allow  — write the clipboard (default).
///   Deny   — never write; the SET is parsed + consumed but no-ops.
enum class ClipboardWritePolicy : uint8_t {
    Allow = 0,
    Deny = 1
};

// Map a config string ("allow" | "deny", case-insensitive) to a
// ClipboardWritePolicy; unrecognized input returns Allow (the default).
[[nodiscard]] ClipboardWritePolicy clipboardWritePolicyFromString(std::string_view s);

/// The OSC 52 read and write policy a config describes, as one value.
///
/// Folded once from a session's configuration and installed on every terminal
/// that configuration governs, so a terminal a Core module mints without
/// reading config — a tmux pane — carries the same pair the seeded ones do.
/// The defaults are the ones an unset `[security]` key resolves to.
struct OSC52Policy {
    ClipboardReadPolicy read { ClipboardReadPolicy::Prompt };
    ClipboardWritePolicy write { ClipboardWritePolicy::Allow };
};

} // namespace Ubiquitty::OSC
