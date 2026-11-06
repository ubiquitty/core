// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::PAL {

/// What the device's current network path costs, as the platform observes it.
///
/// A fact the frontend reports upward, as it reports a path move: only the
/// platform can see the path. Core decides what the cost means for a
/// connection (`Facts.link`, Docs/sync.md §6.6). A platform with no source
/// for it reports `Unmetered`, which is what a connection assumes before any
/// report.
///
/// In a header of its own so the session headers carry one enum and not the
/// client seams. Closed and Swift-named because the Apple path watch maps
/// `NWPath` onto it with an exhaustive switch.
enum class UBIQUITTY_CLOSED_ENUM NetworkCost : std::uint8_t {
    /// Nothing says the path costs anything.
    Unmetered UBIQUITTY_SWIFT_NAME(unmetered),
    /// The path is paid by the byte or rationed: cellular, or a hotspot
    /// tethered over it (`NWPath.isExpensive`).
    Expensive UBIQUITTY_SWIFT_NAME(expensive),
    /// The person asked the system to save data on this path (Low Data Mode,
    /// `NWPath.isConstrained`).
    Constrained UBIQUITTY_SWIFT_NAME(constrained),
};

} // namespace Ubiquitty::PAL
