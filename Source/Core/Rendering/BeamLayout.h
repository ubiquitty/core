// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/BeamLayout.h — the palette's picture, in cells.
//
// One state plus one style plus a grid size gives one canvas and one position.
// Pure, so a golden image asserts the picture rather than the frontend that drew
// it — which is the whole basis for claiming Beam looks the same everywhere
// rather than four frontends agreeing to try.
//
// Nothing here knows about a terminal, a session, a keystroke or a wire. It is
// given what to draw and says where the cells go.

#include "Core/CommandPalette/BeamState.h"
#include "Core/Rendering/CellCanvas.h"
#include "Core/Rendering/ChromeStyle.h"

namespace Ubiquitty::Rendering {

/// A drawn palette and where it sits on the grid.
///
/// `canvas.rows == 0` means the palette drew nothing — closed, or a grid too
/// small to hold it. A caller composites unconditionally; an empty canvas is a
/// no-op rather than a case to remember.
struct BeamGeometry {
    CellCanvas canvas;
    int originRow = 0;
    int originColumn = 0;
    /// How many RESULT rows were drawn, excluding the border, query, rule and
    /// status. This is the session's scroll window, and a caller must hand it
    /// back with `BeamSession::setViewportRows`.
    ///
    /// The layout is the only thing that knows the number: it depends on the
    /// grid, on how many results there are, and on the height cap. A session
    /// told a different figure scrolls against a box that is not the one on
    /// screen — on a short grid the selection walks off the bottom and never
    /// scrolls into view, so arrow-down appears to do nothing and the palette
    /// reads as frozen.
    int bodyRows = 0;
};

/// Lays the palette out for a grid.
///
/// Degrades rather than refuses: a grid narrower or shorter than the popover's
/// minimum shrinks it to what fits, and one too small for even that draws
/// nothing. Neither produces a negative extent, because a frontend asked to
/// composite one would write outside its own buffer.
///
/// - Parameters:
///   - state: What the palette is showing.
///   - style: Colours already resolved against the live theme.
///   - gridRows: The terminal's visible rows.
///   - gridColumns: Its columns.
/// - Returns: The canvas and its position; an empty canvas when nothing is drawn.
[[nodiscard]] BeamGeometry layoutBeam(const CommandPalette::BeamState& state, const ChromeStyle& style,
    int gridRows, int gridColumns);

} // namespace Ubiquitty::Rendering
