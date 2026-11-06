// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The in-terminal form of a transient hint, for the arms with no native chrome.
//
// A hint reaches the user two ways. An arm with a window toolkit puts it where
// that platform puts status — macOS hangs it off the title bar beside the
// remote-status and presence pills. An arm with only the grid draws it here,
// into cells, and composites it like any other chrome (``layoutBeam`` is the
// same shape and the same compositing call).
//
// Drawn at the BOTTOM RIGHT: a shell's cursor sits bottom-left and its output
// runs down the left, so the right corner is the least likely to cover what the
// user is reading. It is also where the native arms put it, so the two forms
// teach the same place to look.

#include "Core/Rendering/CellCanvas.h"
#include "Core/Rendering/ChromeStyle.h"

#include <string_view>

namespace Ubiquitty::Rendering {

/// A drawn hint and where it sits on the grid.
///
/// `canvas.rows == 0` means nothing was drawn — no hint, or a grid too small to
/// hold one. A caller composites unconditionally; an empty canvas is a no-op
/// rather than a case to remember.
struct HintToastGeometry {
    CellCanvas canvas;
    int originRow = 0;
    int originColumn = 0;
};

/// Lays a hint out for a grid.
///
/// Degrades rather than refuses: a grid too narrow or too short for the text
/// draws nothing, because a frontend asked to composite a negative extent would
/// write outside its own buffer.
///
/// - Parameters:
///   - text: The hint, from ``Input::hintText``. Empty draws nothing.
///   - style: Colours already resolved against the live theme.
///   - gridRows: The terminal's visible rows.
///   - gridColumns: Its columns.
/// - Returns: The canvas and its position; an empty canvas when nothing is drawn.
[[nodiscard]] HintToastGeometry layoutHintToast(std::string_view text, const ChromeStyle& style,
    int gridRows, int gridColumns);

} // namespace Ubiquitty::Rendering
