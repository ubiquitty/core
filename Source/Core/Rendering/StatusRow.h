// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/SwiftInterop.h"
#include "Core/Cell.h"
#include "Core/GridBounds.h"
#include "Forward.h"
#include <array>
#include <span>

// DECSSDT status-row assembly: the one place the status line's cells derive
// from the model, whichever frontend draws them. Type 2 copies the
// host-writable row with unwritten cells normalised to the canonical blank;
// type 1 formats the terminal-drawn indicator. Both are live-view state — a
// scrolled-back frame shows history, and the status line belongs to the tail.

namespace Ubiquitty::Rendering {

/// The named cell view the Swift importer generates a safe overload for.
using StatusRowCells = std::span<Cell>;

/// The DECSSDT type-2 row's cells.
///
/// 32 KB, so `Terminal` holds it by pointer and the first write allocates it.
struct StatusRowStorage {
    std::array<Cell, Ubiquitty::maxColumns> cells {};
};

// Bulk is the reason it is behind a pointer; below this it would not be.
static_assert(sizeof(StatusRowStorage) > 1024 * sizeof(void*),
    "the status row is held by pointer on the strength of its size");

/// The row for writing, allocating it on first use. Spans `maxColumns` cells.
///
/// A span rather than a `Cell*`: `operator[]` is bounds-checked under the tree's
/// hardened-library build. Every caller has already tested `statusLineActive`.
[[nodiscard]] StatusRowCells ensureStatusRow(Ubiquitty::Terminal& t);

/// The row for reading. Empty exactly when no write has allocated it.
[[nodiscard]] std::span<const Cell> statusRow(const Ubiquitty::Terminal& t);

/// One cell, or `defaultBlankCell` when the row is absent or `column` is out of
/// range. Per-cell loops should take the span once instead.
[[nodiscard]] const Cell& statusCellAt(const Ubiquitty::Terminal& t, int column);

/// Releases the row. Sound only where no cell can still name it.
void releaseStatusRow(Ubiquitty::Terminal& t);

/// Fills the status row's cells for a frame at the given view.
///
/// - Parameters:
///   - t: The terminal.
///   - rows: The frame's grid rows.
///   - columns: The frame's grid columns; `output` must hold at least this
///     many cells.
///   - viewOffset: The frame's scrollback offset; a nonzero offset
///     deactivates the row.
///   - output: Destination for the cells.
/// - Returns: Whether a status line is active and `output`'s first `columns`
///   cells were filled.
[[nodiscard]] bool fillStatusRow(const Ubiquitty::Terminal* t, int rows, int columns, int viewOffset, StatusRowCells output UBIQUITTY_NOESCAPE);

} // namespace Ubiquitty::Rendering
