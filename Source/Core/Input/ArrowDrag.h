// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"
#include "Core/Keys/KeyTranslation.h"

#include <cmath>
#include <cstdint>

namespace Ubiquitty::Input {

/// The distances that shape an arrow drag, in the frontend's layout units.
///
/// The frontend chooses them for its screen and input device; this file only
/// relies on their order. A dead zone at least as wide as a step would send an
/// arrow from inside the radius that still counts as a tap.
struct ArrowDragMetrics {
    /// The travel along the drag's axis that sends one arrow. Positive.
    double stepDistance = 0;
    /// The radius around the touch-down point within which the touch is still
    /// a tap and holds no axis. Less than ``stepDistance``.
    double deadZoneRadius = 0;
};

/// The axis an arrow drag sends arrows along.
enum class ArrowDragAxis : std::uint8_t {
    /// The touch is inside the dead zone and has picked no axis yet.
    None UBIQUITTY_SWIFT_NAME(none),
    /// Left and right arrows.
    Horizontal UBIQUITTY_SWIFT_NAME(horizontal),
    /// Up and down arrows.
    Vertical UBIQUITTY_SWIFT_NAME(vertical),
};

/// The arrows one movement of the finger sends: `count` presses of `arrow`.
///
/// A count of zero sends nothing, and its arrow is `VirtualKey::None`.
struct ArrowDragSteps {
    /// The arrow to press.
    Keys::VirtualKey arrow = Keys::VirtualKey::None;
    /// How many times to press it.
    int count = 0;
};

/// The most arrows one movement reports.
///
/// A real finger moves a few points between touch events. A larger jump is a
/// synthesized or corrupt event, and sending its whole length at once would
/// flood the program; the remainder is reported by the next movement instead.
inline constexpr int maxArrowDragStepsPerMove = 64;

/// Turns a drag on an arrow key into arrow presses, like a trackpad nub.
///
/// The touch-down point is the origin. Until the finger leaves the dead zone
/// around it the touch is a tap and sends nothing. On leaving, the drag locks
/// to the axis it moved further along, and from then on only its offset along
/// that axis counts: each time the offset crosses the next multiple of the step
/// distance, one arrow goes in that direction, and crossing back sends the
/// opposite arrow, one step at a time. A finger held still sends nothing.
///
/// The axis holds for the whole drag rather than being chosen again per step.
/// A thumb on a small key wanders across the axis it means, and a choice per
/// step turns that wander into the other axis's arrows: in a shell, a cursor
/// move interleaved with history recalls replaces the line being edited. A
/// drag that returns to the dead zone with its arrows cancelled out drops its
/// axis, so the next direction can be picked without lifting the finger.
///
/// Crossing back requires a full step past the last arrow, not the boundary
/// alone, so a finger resting on a boundary does not alternate arrows.
class ArrowDrag {
public:
    /// Starts a drag at the touch-down point.
    ///
    /// - Parameter metrics: The step and dead zone, with the dead zone smaller
    ///   than the step.
    explicit ArrowDrag(ArrowDragMetrics metrics)
        : m_metrics(metrics)
    {
    }

    /// Follows the finger to a new position.
    ///
    /// - Parameters:
    ///   - translationX: The horizontal offset from the touch-down point,
    ///     positive to the right.
    ///   - translationY: The vertical offset from the touch-down point,
    ///     positive downward.
    /// - Returns: The arrows to send for this movement, at most
    ///   ``maxArrowDragStepsPerMove`` of them.
    [[nodiscard]] ArrowDragSteps moveTo(double translationX, double translationY)
    {
        // A NaN fails every comparison below; an infinity is clamped like any
        // long jump. Neither reaches the conversion to int unbounded.
        if (std::isnan(translationX) || std::isnan(translationY) || !(m_metrics.stepDistance > 0))
            return {};
        const double radiusSquared = m_metrics.deadZoneRadius * m_metrics.deadZoneRadius;
        const bool isInsideDeadZone = translationX * translationX + translationY * translationY <= radiusSquared;
        if (m_axis == ArrowDragAxis::None) {
            if (isInsideDeadZone)
                return {};
            m_hasLeftDeadZone = true;
            m_axis = std::abs(translationX) >= std::abs(translationY) ? ArrowDragAxis::Horizontal : ArrowDragAxis::Vertical;
            m_step = 0;
        } else if (isInsideDeadZone && !m_step) {
            m_axis = ArrowDragAxis::None;
            return {};
        }

        const double offset = m_axis == ArrowDragAxis::Horizontal ? translationX : translationY;
        // Truncation toward zero is the full-step hysteresis: the step changes
        // only once the offset is a whole step away from the last arrow sent.
        double stepsAway = (offset - static_cast<double>(m_step) * m_metrics.stepDistance) / m_metrics.stepDistance;
        if (stepsAway > maxArrowDragStepsPerMove)
            stepsAway = maxArrowDragStepsPerMove;
        else if (stepsAway < -maxArrowDragStepsPerMove)
            stepsAway = -maxArrowDragStepsPerMove;
        const int steps = static_cast<int>(stepsAway);
        if (!steps)
            return {};
        m_step += steps;

        ArrowDragSteps result;
        result.count = steps > 0 ? steps : -steps;
        if (m_axis == ArrowDragAxis::Horizontal)
            result.arrow = steps > 0 ? Keys::VirtualKey::RightArrow : Keys::VirtualKey::LeftArrow;
        else
            result.arrow = steps > 0 ? Keys::VirtualKey::DownArrow : Keys::VirtualKey::UpArrow;
        return result;
    }

    /// The axis the drag is locked to, or `none` inside the dead zone.
    [[nodiscard]] ArrowDragAxis axis() const { return m_axis; }

    /// Whether the finger has ever left the dead zone. A touch that never did
    /// is a tap, which the frontend answers with the key under the finger.
    [[nodiscard]] bool hasLeftDeadZone() const { return m_hasLeftDeadZone; }

    /// The arrows sent so far along the locked axis, net of those sent back:
    /// positive to the right or downward.
    [[nodiscard]] int netSteps() const { return m_step; }

private:
    ArrowDragMetrics m_metrics;
    ArrowDragAxis m_axis = ArrowDragAxis::None;
    /// The step the last arrow reached: the offset is within one step of
    /// `m_step * stepDistance` on the side it came from.
    int m_step = 0;
    bool m_hasLeftDeadZone = false;
};

} // namespace Ubiquitty::Input
