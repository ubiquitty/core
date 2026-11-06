// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/OverlayAssembly.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

/// Coalesces cursor redraw excursions without changing the terminal model.
class CursorPresentation {
public:
    /// The viewport in which a retained cursor position remains meaningful.
    struct Geometry {
        int rows = 0;
        int columns = 0;
        int viewOffset = 0;
        float cellWidth = 0;
        float cellHeight = 0;
        float originX = 0;
        float originY = 0;
        float viewportWidth = 0;
        float viewportHeight = 0;
        bool isAlternateActive = false;
        bool hasStatusLineOnBottomRow = false;

        bool operator==(const Geometry&) const = default;
    };

    /// Complete cursor appearance, including while the program hides it.
    struct Frame {
        CursorRenderState cursor;
        Geometry geometry;
        bool isEligible = false;
        bool isProgramVisible = false;
    };

    /// Resolves the cursor to present at the caller's monotonic timestamp.
    ///
    /// A program-hidden cursor retains its last displayed position and style
    /// for at most 50 milliseconds. Repeated hidden updates do not extend that
    /// deadline. Blink, focus, and reader opacity follow the current frame.
    ///
    /// - Parameters:
    ///   - frame: The captured appearance and viewport eligibility.
    ///   - nowSeconds: The current monotonic timestamp.
    ///   - intervalSeconds: The movement-coalescing interval, capped at 16
    ///     milliseconds; a nonpositive interval disables presentation holds.
    ///   - bypass: Whether nearby input-driven movement should be immediate.
    /// - Returns: The presentation cursor, with a negative row when hidden.
    [[nodiscard]] CursorRenderState update(const Frame& frame, double nowSeconds, double intervalSeconds, bool bypass) // bool-ok: consumes the caller's per-frame recent-input predicate.
    {
        if (!std::isfinite(nowSeconds) || (m_hasGeometry && (frame.geometry != m_geometry || nowSeconds < m_lastSeconds))) {
            reset();
        }
        m_geometry = frame.geometry;
        m_hasGeometry = true;
        m_lastSeconds = nowSeconds;
        m_isHolding = false;
        const bool isEligible = frame.isEligible && frame.cursor.row >= 0 && frame.cursor.row < frame.geometry.rows
            && frame.cursor.column >= 0 && frame.cursor.column < frame.geometry.columns
            && !(frame.geometry.hasStatusLineOnBottomRow && frame.cursor.row == frame.geometry.rows - 1);
        const bool isEnabled = std::isfinite(nowSeconds) && std::isfinite(intervalSeconds) && intervalSeconds > 0;
        if (!isEligible || !isEnabled) {
            clearDisplayed();
            return isEligible && frame.isProgramVisible ? accept(frame.cursor) : hidden(frame.cursor);
        }
        if (!frame.isProgramVisible) {
            m_moveDeadlineSeconds = 0;
            if (!m_hasDisplayed) {
                return hidden(frame.cursor);
            }
            if (m_hideDeadlineSeconds == 0) {
                m_hideDeadlineSeconds = nowSeconds + hiddenGraceSeconds;
            }
            if (nowSeconds >= m_hideDeadlineSeconds) {
                clearDisplayed();
                return hidden(frame.cursor);
            }
            return hold(frame.cursor);
        }
        m_hideDeadlineSeconds = 0;
        if (!m_hasDisplayed) {
            return accept(frame.cursor);
        }
        const bool isSamePosition = frame.cursor.row == m_displayed.row && frame.cursor.column == m_displayed.column;
        const bool isNearbyInput = bypass && frame.cursor.row == m_displayed.row
            && std::abs(static_cast<double>(frame.cursor.column) - m_displayed.column) <= 2;
        if (isSamePosition || isNearbyInput || (m_moveDeadlineSeconds != 0 && nowSeconds >= m_moveDeadlineSeconds)) {
            return accept(frame.cursor);
        }
        if (m_moveDeadlineSeconds == 0) {
            m_moveDeadlineSeconds = nowSeconds + std::min(intervalSeconds, maxMovementHoldSeconds);
        }
        return hold(frame.cursor);
    }

    /// Forgets presentation state when the surface or terminal is replaced.
    void reset()
    {
        clearDisplayed();
        m_hasGeometry = false;
        m_lastSeconds = 0;
    }

    /// The next required repaint timestamp, or zero when no hold is pending.
    [[nodiscard]] double deadlineSeconds() const { return m_hideDeadlineSeconds != 0 ? m_hideDeadlineSeconds : m_moveDeadlineSeconds; }

    /// Whether the last update retained a stationary presentation cursor.
    [[nodiscard]] bool isHolding() const { return m_isHolding; }

private:
    static constexpr double maxMovementHoldSeconds = 0.016;
    static constexpr double hiddenGraceSeconds = 0.050;

    /// Removes the drawable position without altering the captured appearance.
    ///
    /// - Parameter cursor: The frame's current cursor appearance.
    /// - Returns: A cursor with no drawable row or in-flight slide.
    static CursorRenderState hidden(CursorRenderState cursor)
    {
        cursor.row = -1;
        cursor.isSliding = false;
        return cursor;
    }

    /// Commits a presentation anchor and ends both pending deadlines.
    ///
    /// - Parameter cursor: The complete appearance selected for this frame.
    /// - Returns: The cursor retained as the next frame's presentation anchor.
    CursorRenderState accept(const CursorRenderState& cursor)
    {
        m_displayed = cursor;
        m_hasDisplayed = true;
        m_moveDeadlineSeconds = 0;
        m_hideDeadlineSeconds = 0;
        return cursor;
    }

    /// Retains position and style while following the current appearance clock.
    ///
    /// - Parameter current: The current blink weight, focus, and reader opacity.
    /// - Returns: The retained cursor with current visibility weights and no slide.
    CursorRenderState hold(const CursorRenderState& current)
    {
        m_isHolding = true;
        m_displayed.mixA = current.mixA;
        m_displayed.focused = current.focused;
        m_displayed.rgb.w = current.rgb.w;
        m_displayed.isSliding = false;
        m_displayed.columnFractional = 0;
        m_displayed.vrowFractional = 0;
        return m_displayed;
    }

    /// Discards retained cursor state without invalidating the viewport identity.
    void clearDisplayed()
    {
        m_displayed = CursorRenderState {};
        m_hasDisplayed = false;
        m_isHolding = false;
        m_moveDeadlineSeconds = 0;
        m_hideDeadlineSeconds = 0;
    }

    CursorRenderState m_displayed;
    Geometry m_geometry;
    double m_lastSeconds = 0;
    double m_moveDeadlineSeconds = 0;
    double m_hideDeadlineSeconds = 0;
    bool m_hasDisplayed = false;
    bool m_hasGeometry = false;
    bool m_isHolding = false;
};

} // namespace Ubiquitty::Rendering
