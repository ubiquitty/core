// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include <optional>

namespace Ubiquitty {
struct Terminal;
namespace Modes::DisplayIdle {

inline constexpr std::uint32_t crtSaveDelayTicks = 30 * 60 * 60;

/// Records host or keyboard activity and wakes a screen blanked by CRT saving.
/// - Parameters:
///   - terminal: The terminal receiving input.
///   - now: The input's time on its monotonic clock.
/// - Returns: Whether the screen became visible.
[[nodiscard]] bool noteInput(Terminal& terminal, Ticks now);

/// Applies the CRT saver deadline without changing any screen cells.
/// - Parameters:
///   - terminal: The terminal whose idle interval is checked.
///   - now: The present time on its monotonic clock.
/// - Returns: Whether the display state changed.
[[nodiscard]] bool advance(Terminal& terminal, Ticks now);

/// Reports when the idle timer next needs service.
/// - Parameters:
///   - terminal: The terminal owning the timer.
///   - now: The present time on its monotonic clock.
/// - Returns: Remaining ticks, zero when due, or no value while disabled or blanked.
[[nodiscard]] std::optional<std::uint32_t> ticksUntilDeadline(const Terminal& terminal, Ticks now);

}
}
