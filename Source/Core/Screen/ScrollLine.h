// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty {
/// A single grid cell. Declared here so a scrollback line can point at cells without the grid's definition.
struct Cell;
}

/// One line of scrollback, as a borrowed span of cells plus the wrap flag that joins it to the next.
///

namespace Ubiquitty::Screen {

/// A view, not an owner: `cells` points into a LineStore block slab and is valid only while that slab is.
struct ScrollLine {
    // Points into a LineStore block slab, trimmed to `length` cells.
    Ubiquitty::Cell* cells;
    int length;
    // Soft-wrap join: line wrapped at the right margin and continues on the next line. Mirrors
    // Screen::lineWrapped for aged rows; the width-reflow path rejoins logical lines spanning the boundary.
    bool wrapped;
    // DECDWL/DECDHL of the row at capture time, so a double-width/height line keeps its attribute through
    // aging and rewrap. A history row carries its own copy; the live Screen::lineAttribute is not consulted.
    uint8_t lineAttribute;
    // OSC-133 prompt mark of the captured row, so it travels through the rewrap ring. Own copy; the live
    // Screen::mark view is not consulted for a history row.
    uint8_t mark;
    // Row holds any codePoint that might need shaping/RTL (Unicode::mayNeedShaping). getDisplayRow reads it
    // so a scrolled-back all-LTR row skips the shaping pass. Computed once at capture; a superset, so a
    // stale-true only wastes a pass, never misses a shape.
    uint8_t needsShaping : 1;
    // The cells are reserved but unwritten: the row's bytes wait in its block's text slab, and the
    // store expands them into `cells` on the first read (LineStore::appendDefaultASCII).
    uint8_t isCompact : 1;
};
// lineAttribute/mark/needsShaping/isCompact land in the tail pad after `length`, so the struct never grows
// past pointer + int + that word. Assert the invariant (not a fixed size) so it stays minimal on every target.
static_assert(sizeof(ScrollLine) == sizeof(Ubiquitty::Cell*) + sizeof(int) + 4, "lineAttribute/mark/needsShaping/isCompact must land in the existing tail pad");

} // namespace Ubiquitty::Screen
