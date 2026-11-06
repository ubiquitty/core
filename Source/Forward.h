// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Forward.h — centralised forward declarations for project types
// that are commonly referenced by pointer or reference across module
// headers.
//
// Including this file resolves the type name without dragging the
// full include graph of the defining header.
//
// On typedefs: this file does not redeclare the
// `typedef struct Foo Foo;` aliases that the defining headers carry.
// Consumers of a forward declaration spell the type as `struct Foo*`.
//

// === C-compatible forward declarations ============================

// Core/TerminalOperations.h — the terminal model. Ubiquitty::Terminal carries the parser state,
// screens, scrollback, kitty state, tmux client, and every other
// per-session datum the parser/event-loop touches.
namespace Ubiquitty {
struct Terminal;
}

// === C++ namespaced forward declarations ==========================

namespace Ubiquitty {

class CellGrid; // sole mutator + reader of the visible cell grid
class PaintSession; // RAII iterator over pending scrolls + dirty runs

// Strong row / column wrappers — pass by value, but headers that
// reference them by reference (`Row r`, `Column c` as parameters)
// still need at least a forward declaration when the full
// Core/StrongIndex.h is undesirable. Tiny PODs around an int.
template<typename Tag> class StrongIndex;
using Row = StrongIndex<struct RowTag>;
using Column = StrongIndex<struct ColumnTag>;

struct RowView; // contiguous span of cells from a row
struct PaintRun; // one same-pen run yielded by PaintSession

namespace Screen {
struct PendingScroll; // one queued scroll yielded by PaintSession
}

// Per-subsystem Painter classes owned by the platform window (e.g.
// UBTerminalView on macOS). Headers that pass them only by pointer /
// reference resolve the type through this forward declaration instead
// of pulling the defining header transitively.
class BackBuffer;
class CellGrid;
class Chrome;
class CursorPainter;
class GlyphRenderer;
class SelectionPainter;

} // namespace Ubiquitty
