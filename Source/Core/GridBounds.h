// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/Types.h"

// Fixed upper bounds on the cell grid, and the size a fresh terminal starts at.
// Stores are heap-allocated to the live rows x columns; these cap what a resize
// may ask for, they are not the allocation size.

// Sanity ceiling, not the working size — the cell store and scrollback
// allocate dynamically to actual rows/columns. These clamp a hostile/buggy
// resize (for example, `CSI 8;999999;999999 t`) from OOMing and size a few small
// fixed buffers (status line, per-row attribute arrays, row scratch).
//
// sizeof(Cell) is 32: 4 (ch) + 20 (Pen) + 4 (one inline combining slot) +
// 4 (combiningLength + 2-byte link id + clusterID). A cluster longer than
// the one inline continuation slot spills WHOLE to the cluster pool
// (Cell::clusterID), so the cell stays 32 B + memcmp-comparable.
//
// CellGrid::init and the resize path CLAMP to these rather than failing, so a
// ceiling low enough to be reached in practice yields a terminal that silently
// reports a smaller size than its window — every layout then disagrees with no
// error raised anywhere. maxRows was 256, which a full-screen window on a large
// display at a small font can exceed. 1024 puts both dimensions out of reach of
// any shipping display at any usable font size while still catching a hostile
// `CSI 8;999999;999999 t`. Raising it is close to free: CellGrid grows capacity
// on demand (roundUpCapacity), and the only statically-sized array is
// rowMetaFallback[maxRows] at 4 B/row.

namespace Ubiquitty {
constexpr int maxColumns = 1024;
constexpr int maxRows = 1024;
/// The floor a resize applies: a one-row or one-column terminal cannot hold
/// a scroll region, so a smaller request is represented at this size.
constexpr int minColumns = 2;
constexpr int minRows = 2;
/// The geometry a resize represents a request at: floored and ceilinged, so a
/// caller that sizes something else to the request can ask the same question.
///
/// - Parameter rows: The rows asked for.
/// - Returns: The rows the grid holds for that request.
constexpr int clampGridRows(int rows) { return rows < minRows ? minRows : rows > maxRows ? maxRows
                                                                                         : rows; }
/// - Parameter columns: The columns asked for.
/// - Returns: The columns the grid holds for that request.
constexpr int clampGridColumns(int columns) { return columns < minColumns ? minColumns : columns > maxColumns ? maxColumns
                                                                                                              : columns; }
/// Default startup size. Most servers assume 80x24 absent NAWS.
constexpr int defaultColumns = 80;
constexpr int defaultRows = 24;

} // namespace Ubiquitty
