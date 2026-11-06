// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::Input {

// What an application-level transition asks of the pointer grabs a process
// holds. The grab is process-global — one hide counter, one dissociation — so
// no single surface can answer for it: a pane holding a lock while the app
// goes away leaves a hidden, dissociated cursor on a desktop with nothing left
// to release it. The per-surface hooks miss exactly the cases that matter
// here, and the ones they miss are not the same on every toolkit, so which
// transition demands what is decided once rather than per frontend.

/// The transitions a frontend reports about its application.
enum class AppLifecycle : std::uint8_t {
    /// The application became frontmost.
    Activated UBIQUITTY_SWIFT_NAME(activated),
    /// Another application took the foreground.
    Deactivated UBIQUITTY_SWIFT_NAME(deactivated),
    /// The process is going away.
    Terminating UBIQUITTY_SWIFT_NAME(terminating),
};

/// What to do with every grab the process holds.
enum class PointerLockSweep : std::uint8_t {
    /// Release each one outright, whatever its surface would otherwise want.
    ReleaseEveryGrab UBIQUITTY_SWIFT_NAME(releaseEveryGrab),
    /// Ask each surface again; only one that still wants a grab takes it.
    ReconcileEveryGrab UBIQUITTY_SWIFT_NAME(reconcileEveryGrab),
};

/// What an application transition asks of every pointer grab.
///
/// Leaving the foreground and going away are both releases, and neither is a
/// reconcile: a reconcile asks each surface whether it still wants the grab,
/// and a surface that cannot be reached — a pane already torn down, one whose
/// window never posted the edge — answers nothing at all. A release needs no
/// answer from anybody.
///
/// Coming back is the reconcile, and it has to be one rather than a blanket
/// re-engage: the application returning does not say which pane the reader is
/// in, and re-engaging every grab would hand the pointer to a pane that had
/// only been holding one when it was frontmost.
///
/// - Parameter event: The transition the frontend saw.
/// - Returns: What to do with the grabs.
[[nodiscard]] constexpr PointerLockSweep pointerLockSweep(AppLifecycle event)
{
    return event == AppLifecycle::Activated ? PointerLockSweep::ReconcileEveryGrab : PointerLockSweep::ReleaseEveryGrab;
}

} // namespace Ubiquitty::Input
