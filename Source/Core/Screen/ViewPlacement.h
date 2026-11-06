// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>

namespace Ubiquitty::Screen {

/// Where a revealed row is placed in the viewport.
///
/// A jump chooses one, and the choice is a behaviour a user compares across
/// frontends: the same command block reached the same way has to land in the
/// same place. The two differ because what the reader wants to see differs —
/// a match is read with its surrounding context, a command block is read with
/// its output, and output runs downward.
enum class RevealPlacement {
    /// A search hit, one third down. Context above and below both matter.
    Match = 0,
    /// A shell command's prompt row, one quarter down, so more of the output
    /// it produced fits below it.
    CommandBlock = 1,
};

/// Returns the visible row a placement puts the revealed row on.
///
/// - Parameters:
///   - rows: The viewport's height in rows.
///   - placement: Which jump this is.
/// - Returns: A visible row index, 0 when the viewport is too short to divide.
[[nodiscard]] constexpr int landingRowFor(int rows, RevealPlacement placement)
{
    return placement == RevealPlacement::Match ? rows / 3 : rows / 4;
}

/// Returns the scroll offset that brings `absoluteRow` into view.
///
/// Inverts the address map `visibleCell` uses — absolute = scrollbackCount -
/// offset + visibleRow — so solving for the landing row gives the offset. The
/// result is clamped to `[0, scrollbackCount]`, where 0 is the live tail and
/// `scrollbackCount` is the oldest row, which is what makes a row past either
/// end resolve to the nearest end rather than to a viewport that does not exist.
///
/// - Parameters:
///   - scrollbackCount: Rows of history above the live screen; 0 on the
///     alternate screen, which keeps none.
///   - rows: The viewport's height in rows.
///   - absoluteRow: The row to reveal, counted from the oldest row of history.
///   - placement: Which jump this is.
/// - Returns: The offset to assign to the renderer's view.
[[nodiscard]] constexpr int viewOffsetToReveal(int scrollbackCount, int rows, int absoluteRow, RevealPlacement placement)
{
    int offset = scrollbackCount - absoluteRow + landingRowFor(rows, placement);
    if (offset < 0)
        offset = 0;
    if (offset > scrollbackCount)
        offset = scrollbackCount;
    return offset;
}

// The placements are distinct, and a command block sits higher than a match so
// more of its output is below it. Written as a check rather than as prose
// because the two constants are three characters apart and a frontend reading
// only one of them cannot see the relationship.
static_assert(landingRowFor(24, RevealPlacement::CommandBlock) < landingRowFor(24, RevealPlacement::Match));
static_assert(landingRowFor(0, RevealPlacement::Match) == 0 && landingRowFor(0, RevealPlacement::CommandBlock) == 0);

// The map is inverted, not approximated: with room on both sides, the revealed
// row lands exactly on the placement's row. `absolute = scrollbackCount -
// offset + visibleRow` read back through the offset this returns.
static_assert(1000 - viewOffsetToReveal(1000, 24, 500, RevealPlacement::Match) + landingRowFor(24, RevealPlacement::Match) == 500);
static_assert(1000 - viewOffsetToReveal(1000, 24, 500, RevealPlacement::CommandBlock) + landingRowFor(24, RevealPlacement::CommandBlock) == 500);

// Both ends clamp. A row below the live tail cannot pull the view past it, and
// a row above the oldest cannot pull it past the top.
static_assert(viewOffsetToReveal(1000, 24, 1200, RevealPlacement::Match) == 0);
static_assert(viewOffsetToReveal(1000, 24, -50, RevealPlacement::Match) == 1000);
static_assert(viewOffsetToReveal(0, 24, 0, RevealPlacement::Match) == 0);

} // namespace Ubiquitty::Screen
