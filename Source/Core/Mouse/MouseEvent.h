// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/SwiftInterop.h"

namespace Ubiquitty::Mouse {

/// Mouse event kind fed to the report encoders. The values are the encoders' own
/// dispatch codes, not a wire format — the xterm and SGR encodings derive their
/// button bytes from this plus the button number.
/// The Swift names carry the language's own case convention, so a Swift
/// caller writes `.press` rather than the C++ spelling.
enum class EventType : int {
    /// A button went down.
    Press UBIQUITTY_SWIFT_NAME(press) = 0,
    /// A button came up.
    Release UBIQUITTY_SWIFT_NAME(release) = 1,
    /// The pointer moved.
    Motion UBIQUITTY_SWIFT_NAME(motion) = 2,
};

/// The named xterm Cb button values.
///
/// Named constants rather than an enumeration because the field is open and
/// class-encoded: the wheel occupies 64-67 (the 0x40 class) and the extended
/// buttons 128-131 (the 0x80 class), so a closed type would claim a completeness
/// the protocol does not have and force a cast at every site that computes a
/// class value rather than naming one.
namespace Button {
constexpr int Left = 0;
/// The Cb a report carries when no button is held — hover under any-event
/// tracking, and every release under the encodings that do not name a button.
constexpr int None = 3;
} // namespace Button

/// The modifier bits the encoder ORs into a report's Cb byte.
///
/// Constants for the same reason as ``Button``: they are combined into the same
/// open integer the frontend assembles, alongside bits this table does not name.
namespace Modifier {
constexpr int Shift = 4;
constexpr int Meta = 8;
constexpr int Control = 16;
} // namespace Modifier

} // namespace Ubiquitty::Mouse
