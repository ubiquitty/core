// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"
#include "Core/Modes/TerminalModes.h"
#include "Forward.h"

#include <type_traits>

// The model half of a resize, separated from the report that follows it.

namespace Ubiquitty::Screen {

/// What a resize did, so the layer holding the wire can decide what the host is told.
///
/// The screen model has no output queue and cannot emit, so it reports its outcome
/// and ``Ubiquitty::resize`` turns that into a DEC mode 2048 notification.
///
/// The `[[nodiscard]]` is required rather than decorative. Mode 2048 requires
/// an in-band report on every geometry change, and a caller that dropped this
/// value would silence the report with no diagnostic at all — the terminal would
/// stop answering a mode it advertises. The compiler refuses instead.
struct [[nodiscard]] ResizeOutcome {
    /// Whether the geometry moved.
    ///
    /// False when the resize was a no-op: a null terminal, or dimensions that
    /// already match with no deferred history rewrap to settle. Those paths must
    /// not report, because nothing changed.
    bool geometryChanged = false;
};

static_assert(std::is_trivially_copyable_v<ResizeOutcome>);
static_assert(sizeof(ResizeOutcome) == sizeof(bool), "the outcome must cost what the bool it replaces cost");

/// Resizes the grid, reflowing and re-anchoring as the mode and urgency require.
///
/// Performs the model work only — reflow, bottom anchoring, tab-stop preservation
/// and dirty marking — and emits nothing.
///
/// - Parameters:
///   - t: The terminal to resize. A null terminal is a no-op.
///   - newRows: Requested row count, clamped to `[2, maxRows]`.
///   - newColumns: Requested column count, clamped to `[2, maxColumns]`.
///   - reflow: Whether surviving rows rewrap to the new width.
///   - anchor: Which edge keeps its content when the row count changes.
///   - urgency: `Interactive` may defer the history rewrap; `Settled` completes it.
/// - Returns: Whether the geometry moved, for the caller to report.
ResizeOutcome resizeGrid(Ubiquitty::Terminal* t, int newRows, int newColumns,
    Modes::ReflowMode reflow, Modes::AnchorMode anchor, Modes::ResizeUrgency urgency);

#if UBIQUITTY_ENABLE_TEST_HOOKS
/// Cells the reflow has copied, cumulatively: every cell appended to the row
/// under construction and every closed row's copy into the ring.
///
/// The work a rewrap does, counted rather than timed. A rewrap linear in a
/// line's length copies each cell a fixed number of times, so two lengths give
/// a ratio equal to the ratio of the lengths, and a rejoin that copies what it
/// already emitted gives one that grows with the length. Only the two copy
/// paths count; a copy added elsewhere is not seen here. Compiled into the one
/// test that asks.
extern std::uint64_t reflowCellsCopiedForTest;
#endif

} // namespace Ubiquitty::Screen
