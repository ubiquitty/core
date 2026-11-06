// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/CellCanvas.h — an off-screen grid of Cells, and the
// primitives for drawing chrome into it.
//
// Terminal chrome that must look identical on every frontend — the command
// palette, a find bar, an inspector overlay — is drawn here, into cells, and
// composited over the terminal's own grid at render time. One implementation
// produces the picture and every backend draws it with the machinery it already
// has for text, so there is no per-frontend UI code to drift.
//
// It stops where a widget toolkit would begin. Four primitives, no layout, no
// ownership, no event loop: the caller decides where things go and this puts
// cells where it is told.
//
// Why off-screen and composited, rather than written into the live grid. The
// terminal's grid belongs to the application on the other end of the PTY;
// writing chrome into it makes the chrome indistinguishable from output, loses
// what was underneath, and corrupts an Update's baseline that describes cells
// nobody sent. `Terminal::sixelDisplayMode`'s neighbourhood already carries the
// same reasoning for DECSSDT: kept separate from Screen so the data screen is
// unperturbed.

#include "Core/Cell.h"
#include <span>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Ubiquitty {

struct Pen; // Core/Pen.h

namespace Rendering {

/// A half-open rectangle in cells, `[row, row + height) x [column, column + width)`.
///
/// Half-open because every primitive here clips, and an inclusive bound turns a
/// zero-size rect into a one-cell one — which draws a stray cell rather than
/// nothing, and does it only in the degenerate case nobody tests.
struct GridRect {
    int row = 0;
    int column = 0;
    int width = 0;
    int height = 0;
};

/// Which box-drawing forms a border uses.
///
/// Named rather than passed as code points so a caller cannot assemble a border
/// from characters that do not join — the corners and the rules of one style are
/// chosen together or the seams show.
enum class BoxStyle : std::uint8_t {
    Light, ///< U+2500 family: ─ │ ┌ ┐ └ ┘
    Heavy, ///< U+2501 family: ━ ┃ ┏ ┓ ┗ ┛
    Double, ///< U+2550 family: ═ ║ ╔ ╗ ╚ ╝
    Rounded, ///< Light rules with U+256D-2570 corners: ╭ ╮ ╰ ╯
};

/// An off-screen grid of cells, sized once and drawn into.
///
/// Cells are value-initialised, which is a blank with a default pen — the same
/// bytes an erase produces, so an undrawn region composites as background rather
/// than as anything the caller has to remember to clear.
struct CellCanvas {
    std::vector<Cell> cells;
    int rows = 0;
    int columns = 0;

    /// Sizes the canvas and blanks it. A degenerate size yields an empty canvas
    /// that every primitive treats as fully clipped.
    ///
    /// Not `resize`, for two reasons. It discards the old contents where
    /// `std::vector::resize` preserves them, so the familiar name promised the
    /// opposite of what this does. And a member named `resize` is
    /// indistinguishable from a call to the free `Ubiquitty::resize(Terminal*,
    /// …)` to any sweep that tracks namespaces rather than class scope, which is
    /// every textual one.
    void resizeAndBlank(int rowCount, int columnCount);

    /// The cell at a position, or null when it is outside.
    [[nodiscard]] Cell* at(int row, int column);
    [[nodiscard]] const Cell* at(int row, int column) const;
};

/// Paints `rect` with a pen, leaving no glyph.
///
/// Clipped to the canvas, so a caller may pass a rect that hangs off any edge.
///
/// - Parameters:
///   - canvas: The canvas to draw into.
///   - rect: The region to paint, in canvas cells.
///   - pen: The pen every covered cell takes.
void fill(CellCanvas& canvas, GridRect rect, const Pen& pen);

/// Draws UTF-8 text left to right from a position, in one pen.
///
/// Stops at the canvas edge rather than wrapping: chrome that wrapped would
/// reflow its own border. A double-width grapheme that would straddle the last
/// column is DROPPED rather than half-drawn — half a wide pair is a grid state
/// the write path itself rejects, and drawing one produces an orphan that every
/// later pass has to tolerate.
///
/// Combining marks attach to their base as the parser attaches them, up to the
/// cell's inline capacity; a cluster that would need the overflow pool is
/// dropped, because that pool belongs to a Terminal and a canvas has none.
///
/// - Parameters:
///   - canvas: The canvas to draw into.
///   - row: Canvas row.
///   - column: Canvas column of the first cell.
///   - utf8: The text, decoded as UTF-8; invalid bytes draw U+FFFD.
///   - pen: The pen every drawn cell takes.
/// - Returns: Whether the whole string fitted. False means it was clipped, which
///   a caller sizing a field can treat as "elide".
bool drawText(CellCanvas& canvas, int row, int column, std::string_view utf8, const Pen& pen);

/// A drawn chrome surface and where it sits on the terminal grid.
///
/// The palette, a hint, an inspector: each is a canvas and an origin, and the
/// frame composites a LIST of them rather than one. A second overlay arriving
/// as a second parameter is how a render seam grows a parameter per surface,
/// and how two of them come to disagree about which draws on top — the order
/// here is the answer, last drawn wins.
struct ChromePlacement {
    const CellCanvas* canvas = nullptr;
    int originRow = 0;
    int originColumn = 0;
};

/// The chrome layers a frame composites, in draw order.
///
/// A named alias because `std::span<T, Extent>`'s extent is a non-type template
/// parameter that Swift's C++ interop has no spelling for: a bare span in a
/// signature is a function Swift cannot call, and the alias is the annotation
/// the interop needs rather than a workaround.
using ChromeLayers = std::span<const ChromePlacement>;

/// Whether any placement would draw.
///
/// - Parameter chrome: The layers.
/// - Returns: Whether compositing them would change a cell.
[[nodiscard]] bool hasDrawnChrome(ChromeLayers chrome);

/// Draws a box border around `rect`, in one style and one pen.
///
/// The interior is untouched — a caller wanting it filled calls ``fill`` first,
/// which keeps the border drawable over content.
///
/// A rect one row or one column thick collapses correctly: the two rules land on
/// the same line rather than drawing a border with no inside and a doubled edge.
///
/// - Parameters:
///   - canvas: The canvas to draw into.
///   - rect: The border's outer bounds.
///   - style: Which box-drawing family the rules and corners come from.
///   - pen: The pen every border cell takes.
void drawBox(CellCanvas& canvas, GridRect rect, BoxStyle style, const Pen& pen);

/// Composites a canvas over a destination grid at a position.
///
/// Clipped on every side, so a popover may be positioned partly off-grid and
/// draws the part that fits. Cells are copied whole — codepoint, pen, combining
/// — because a partial copy is what produces a glyph wearing another cell's
/// colour.
///
/// Blank canvas cells overwrite rather than showing through: a canvas is a
/// picture, not a mask, and a popover with a transparent interior would read as
/// a rendering fault. A caller wanting the terminal visible behind chrome does
/// not composite there.
///
/// - Parameters:
///   - destination: The grid to draw onto, `destinationRows * destinationColumns`, row-major.
///   - destinationRows: Its row count.
///   - destinationColumns: Its column count.
///   - canvas: The picture.
///   - atRow: Destination row of the canvas's top-left.
///   - atColumn: Destination column of the canvas's top-left.
void composite(Cell* destination, int destinationRows, int destinationColumns,
    const CellCanvas& canvas, int atRow, int atColumn);

} // namespace Rendering
} // namespace Ubiquitty
