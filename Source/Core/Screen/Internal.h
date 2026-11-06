// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Cross-TU seam for the Screen module — private to Core/Screen/*.cpp, never
// included by a frontend. The same role VT/Internal.h plays for the parser.
//
// Only primitives that genuinely have more than one caller across the split
// belong here. A helper used by one file stays file-static in that file: making
// it extern to satisfy a reader's sense of symmetry costs the compiler the
// single-TU visibility it inlines on.

#include <cstdint>

namespace Ubiquitty {

struct Terminal;

namespace Screen {

// Move rows within a rectangle, the shared primitive under region scrolling and
// line insert/delete — IL and DL are a region scroll bounded by the cursor row,
// which is why Edit.cpp reaches for these rather than repeating the memmove.
//
// `top`/`bot` and `left`/`right` are inclusive, 0-based. `n` rows move; the
// vacated rows are blanked with the screen's scroll-blank pen. Cold: reached per
// sequence, never per byte.
/// Scrolls a rectangle up, blanking the rows it reveals at the bottom.
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - top: Zero-based first row.
///   - bot: Zero-based last row.
///   - left: Zero-based first column.
///   - right: Zero-based last column.
///   - n: How many rows to move.
void scrollRectUp(Ubiquitty::Terminal& t, int top, int bot, int left, int right, int n);
/// Scrolls a rectangle down, blanking the rows it reveals at the top.
///
/// - Parameters:
///   - t: The terminal to scroll.
///   - top: Zero-based first row.
///   - bot: Zero-based last row.
///   - left: Zero-based first column.
///   - right: Zero-based last column.
///   - n: How many rows to move.
void scrollRectDown(Ubiquitty::Terminal& t, int top, int bot, int left, int right, int n);

/// Pulls `k` live rows down into the space freed by dropping history rows.
///
/// `dropRowsBefore` and `dropRowsToPreviousMark` both free history from the top
/// and have to close the gap the same way, so the re-anchoring lives here rather
/// than in each. `k` is clamped to the available history and the cursor row
/// follows the slide.
///
/// - Parameters:
///   - t: The terminal whose live rows move.
///   - k: How many rows to pull down. Non-positive does nothing.
void pullHistoryDown(Ubiquitty::Terminal* t, int k);

} // namespace Screen

} // namespace Ubiquitty
