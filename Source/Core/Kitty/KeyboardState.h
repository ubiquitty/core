// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Forward.h"

// Kitty keyboard protocol, parser side: the per-screen flag STATE and its
// push/pop stack (CSI = u / CSI > u / CSI < u). Terminal owns the underlying
// fields; these entry points reach into it for the VT100 CSI handlers. Lives
// under Core/Kitty (not Input/) because Core must not include Input/.

namespace Ubiquitty::Kitty {

constexpr int maxKeyStackDepth = 8;

/// Sets the active screen's kitty keyboard flags — `CSI = flags ; mode u`.
///
/// Unknown bits are dropped rather than stored, so a request naming a flag this
/// build does not implement cannot leave the screen in a state nothing honours.
///
/// - Parameters:
///   - t: The terminal whose active screen is changed.
///   - flags: The requested bits.
///   - mode: 1 replaces, 2 ORs the given flags in, 3 clears them.
void setFlags(Ubiquitty::Terminal& t, int flags, int mode);

/// Pushes the current flags and replaces them — `CSI > flags u`.
///
/// A full stack evicts its OLDEST entry, FIFO, which the protocol specifies:
/// an application that pushes without popping loses its earliest saved state
/// rather than failing to push.
///
/// - Parameters:
///   - t: The terminal whose active screen is changed.
///   - flags: The replacement bits.
void pushFlags(Ubiquitty::Terminal& t, int flags);

/// Pops saved flags — `CSI < count u`.
///
/// A pop that empties the stack resets every flag to 0, overriding whatever the
/// bottom entry held: an application that over-pops lands in the protocol's
/// disabled state rather than in someone else's saved one.
///
/// - Parameters:
///   - t: The terminal whose active screen is changed.
///   - count: Entries to pop; the default and minimum is 1.
void popFlags(Ubiquitty::Terminal& t, int count);

} // namespace Ubiquitty::Kitty
