// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/OSC/TabStatus.h — the size of the OSC 21337 status label.
//
// The label is kept on the terminal and published on the lock-free runtime
// snapshot, so both need the same bound, and neither header should include the
// other to get it: the snapshot is copied per key press and must stay light.
// Pure scalars, no includes beyond the standard library.
//

#include <cstddef>

namespace Ubiquitty::OSC {

/// The longest OSC 21337 `status` label kept, in UTF-8 bytes.
///
/// Fixed rather than unbounded so the label rides the lock-free runtime
/// snapshot beside the colour it describes. The labels agents send are words
/// ("Waiting", "Working"); a longer one is cut at a code-point boundary.
inline constexpr std::size_t tabStatusTextCapacity = 48;

/// The longest iTerm2 badge kept (OSC 1337 SetBadgeFormat), in UTF-8 bytes.
///
/// A badge is a few words drawn large over a pane. A longer one is cut at a
/// code-point boundary, and an encoded payload past what this decodes to is
/// read no further.
inline constexpr std::size_t badgeTextCapacity = 256;

/// The longest title or body kept of a notification nobody has seen yet, in
/// UTF-8 bytes: the bound the notification itself is posted under.
inline constexpr std::size_t attentionTextCapacity = 255;

} // namespace Ubiquitty::OSC
