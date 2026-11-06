// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"

namespace Ubiquitty::Session {

/// Where a new terminal window goes, given where the last one was.
///
/// Rect arithmetic and nothing else. Which screen is "active", where a frame is
/// persisted and whether the platform offers its own restoration are each a
/// frontend's to answer; how the answers compose into a frame is not, and a
/// second frontend deciding it again is a second frontend that decides it
/// differently.
///
/// Coordinates are the platform's own — origin at the bottom-left, y growing
/// upward, as both AppKit and X11 place windows — so a caller hands over the
/// rects it already holds. The unit is points, but nothing here depends on
/// that.
namespace WindowPlacement {

/// One title bar's worth of offset. The step a cascade takes, and the inset a
/// wrapped cascade leaves at the edge it wraps to.
inline constexpr float cascadeStepInPoints = 22.0f;

/// Nudges a restored frame so a second window does not land on the first.
///
/// Down and to the right by one title bar, which is the offset that makes the
/// window beneath visibly a window rather than a shadow. A step that would
/// carry the frame off the visible area wraps back to the opposite edge
/// instead of walking off it — a cascade is a hint about which window is on
/// top, and it stops being one the moment it puts a title bar out of reach.
///
/// - Parameters:
///   - frame: The frame to nudge.
///   - visibleArea: The destination screen's usable area, menu bar and dock
///     already excluded.
/// - Returns: The nudged frame, the same size.
[[nodiscard]] constexpr FloatRect cascaded(FloatRect frame, FloatRect visibleArea)
{
    const float visibleMaxX = visibleArea.origin.x + visibleArea.size.width;
    const float visibleMaxY = visibleArea.origin.y + visibleArea.size.height;
    FloatRect stepped = frame;
    stepped.origin.x = frame.origin.x + cascadeStepInPoints;
    stepped.origin.y = frame.origin.y - cascadeStepInPoints;
    if (stepped.origin.x + stepped.size.width > visibleMaxX)
        stepped.origin.x = visibleArea.origin.x + cascadeStepInPoints;
    if (stepped.origin.y < visibleArea.origin.y)
        stepped.origin.y = visibleMaxY - stepped.size.height - cascadeStepInPoints;
    return stepped;
}

/// Re-homes a frame onto another screen, keeping where it sat on the old one.
///
/// A frame restored from what the user last had, or centred on a first launch,
/// describes a position on whichever screen it was saved from. Moving it by its
/// offset from the visible area's TOP-left rather than by its raw origin is
/// what makes that offset mean the same thing on a screen of another size: a
/// window a little below the menu bar stays a little below the menu bar, where
/// a bottom-anchored move would drop it by the height difference.
///
/// Clamped into the destination afterwards, in that order, because the offset
/// is the intent and the clamp is only what makes it fit.
///
/// - Parameters:
///   - frame: The frame to move.
///   - fromVisibleArea: The usable area of the screen it currently sits on.
///   - toVisibleArea: The usable area of the screen it should sit on.
/// - Returns: The re-homed frame, the same size.
[[nodiscard]] constexpr FloatRect movedToScreen(FloatRect frame, FloatRect fromVisibleArea, FloatRect toVisibleArea)
{
    const float fromMaxY = fromVisibleArea.origin.y + fromVisibleArea.size.height;
    const float toMaxX = toVisibleArea.origin.x + toVisibleArea.size.width;
    const float toMaxY = toVisibleArea.origin.y + toVisibleArea.size.height;
    const float offsetX = frame.origin.x - fromVisibleArea.origin.x;
    const float gapFromTop = fromMaxY - (frame.origin.y + frame.size.height);

    FloatRect moved = frame;
    moved.origin.x = toVisibleArea.origin.x + offsetX;
    moved.origin.y = toMaxY - gapFromTop - frame.size.height;
    if (moved.origin.x + moved.size.width > toMaxX)
        moved.origin.x = toMaxX - moved.size.width;
    if (moved.origin.x < toVisibleArea.origin.x)
        moved.origin.x = toVisibleArea.origin.x;
    if (moved.origin.y < toVisibleArea.origin.y)
        moved.origin.y = toVisibleArea.origin.y;
    if (moved.origin.y + moved.size.height > toMaxY)
        moved.origin.y = toMaxY - moved.size.height;
    return moved;
}

/// Whether a frame is worth remembering as where the next window opens.
///
/// A full-screen or minimised window is not describing a place the user chose
/// for a window to be, and a zero-size one is not describing a place at all —
/// restoring any of the three gives a window that opens wrong and reads as the
/// terminal having forgotten.
///
/// Nor is a window whose session ends with one program. That window belongs to
/// whatever was asked to run in it rather than to a person arranging a desktop,
/// and remembering where it sat makes a run of them hysteretic: each inherits
/// the frame the last one finished at, which is how a conformance suite ends up
/// walking its window across the screen.
///
/// - Parameters:
///   - frame: The frame in question.
///   - isFullScreen: Whether the window is in the platform's full-screen mode.
///   - isMiniaturized: Whether it is minimised.
///   - doesSessionEndWithChild: Whether this window's session ends when the
///     program it was launched with does.
/// - Returns: Whether to persist it.
[[nodiscard]] constexpr bool isWorthPersisting(FloatRect frame, bool isFullScreen, bool isMiniaturized, bool doesSessionEndWithChild)
{
    return !isFullScreen && !isMiniaturized && !doesSessionEndWithChild && frame.size.width > 0.0f
        && frame.size.height > 0.0f;
}

} // namespace WindowPlacement
} // namespace Ubiquitty::Session
