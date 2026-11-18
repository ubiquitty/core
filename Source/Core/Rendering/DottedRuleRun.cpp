// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/DottedRuleRun.h"

#include <cmath>
#include <limits>

namespace Ubiquitty::Rendering {

namespace {

/// How near two edges must be to count as one, in device pixels: far below a
/// pixel, and above the float noise two cells' arithmetic leaves.
constexpr float edgeTolerance = 1.0f / 64.0f;

/// How far a dot's antialiased edge reaches past the dot, in device pixels.
constexpr float dotFringe = 0.5f;

} // namespace

void fitDotsBetweenCuts(const DottedRunFrame& frame, std::span<const float> cuts, std::vector<DottedStretch>& stretches)
{
    stretches.clear();
    if (!(frame.right > frame.left))
        return;
    const float pitch = frame.pitch;
    const float dot = frame.dot;
    if (!(pitch > 0.0f) || !(dot > 0.0f)) {
        stretches.push_back({ .left = frame.left, .right = frame.right, .origin = frame.origin });
        return;
    }
    // Each stretch is held to the run and kept clear of the one before, so a
    // fringe or a rounded origin never draws a pixel twice.
    float drawnTo = -std::numeric_limits<float>::infinity();
    const auto push = [&](float left, float right, float origin) {
        left = std::max({ left, frame.left, drawnTo });
        right = std::min(right, frame.right);
        if (!(right > left))
            return;
        stretches.push_back({ .left = left, .right = right, .origin = origin });
        drawnTo = right;
    };
    const auto lay = [&](float left, float right, bool isAfterCut, bool isClosed) {
        if (!(right > left))
            return;
        if (!isAfterCut) {
            // From the run's edge, on its own rhythm, to its last whole dot.
            float to = right;
            if (isClosed)
                to = frame.origin + std::floor((right - dot - frame.origin) / pitch + edgeTolerance) * pitch + dot + dotFringe;
            push(left, std::min(to, right), frame.origin);
            return;
        }
        if (!isClosed) {
            // Running on past the run: the rhythm, from its first whole dot.
            const float firstDot = frame.origin + std::ceil((left - frame.origin) / pitch - edgeTolerance) * pitch;
            push(firstDot - dotFringe, right, frame.origin);
            return;
        }
        const float length = right - left;
        if (length < dot - edgeTolerance)
            return;
        const float dots = std::floor((length - dot) / pitch + edgeTolerance) + 1.0f;
        const float extent = (dots - 1.0f) * pitch + dot;
        const float centred = left + (length - extent) * 0.5f;
        const float origin = frame.origin + std::round(centred - frame.origin);
        push(origin - dotFringe, origin + extent + dotFringe, origin);
    };
    float stretchLeft = frame.left;
    bool isAfterCut = false;
    for (std::size_t index = 0; index + 1 < cuts.size();) {
        const float cutLeft = cuts[index];
        float cutRight = cuts[index + 1];
        index += 2;
        while (index + 1 < cuts.size() && cuts[index] <= cutRight + edgeTolerance) {
            cutRight = std::max(cutRight, cuts[index + 1]);
            index += 2;
        }
        lay(stretchLeft, cutLeft, isAfterCut, true);
        stretchLeft = std::max(stretchLeft, cutRight);
        isAfterCut = true;
    }
    lay(stretchLeft, frame.right, isAfterCut, frame.isEnding);
}

bool DottedRuleRun::doesContinueWith(const DecorationInstance& rule) const
{
    if (m_cells.empty())
        return false;
    const DecorationInstance& last = m_cells.back().rule;
    const auto isNear = [](float a, float b) { return std::fabs(a - b) <= edgeTolerance; };
    return isNear(rule.topLeftInPixels.x, last.bottomRightInPixels.x)
        && isNear(rule.topLeftInPixels.y, last.topLeftInPixels.y)
        && isNear(rule.bottomRightInPixels.y, last.bottomRightInPixels.y)
        && isNear(rule.bandTopLeftInPixels.x, last.bandTopLeftInPixels.x)
        && isNear(rule.dashPeriodInPixels, last.dashPeriodInPixels);
}

void DottedRuleRun::hold(const DecorationInstance& rule, std::span<const float> cuts, DottedRunEdge rightEdge, std::size_t traceIndex)
{
    m_cells.push_back({ .rule = rule, .traceIndex = traceIndex });
    const float left = rule.topLeftInPixels.x;
    const float right = rule.bottomRightInPixels.x;
    for (std::size_t index = 0; index + 1 < cuts.size(); index += 2) {
        const float cutLeft = std::max(cuts[index], left);
        const float cutRight = std::min(cuts[index + 1], right);
        if (cutRight > cutLeft) {
            m_cuts.push_back(cutLeft);
            m_cuts.push_back(cutRight);
        }
    }
    m_rightEdge = rightEdge;
}

void DottedRuleRun::fit()
{
    if (m_cells.empty())
        return;
    const DecorationInstance& first = m_cells.front().rule;
    const DottedRunFrame frame {
        .left = first.topLeftInPixels.x,
        .right = m_cells.back().rule.bottomRightInPixels.x,
        .origin = first.bandTopLeftInPixels.x,
        .pitch = first.dashPeriodInPixels,
        .dot = first.bandBottomRightInPixels.y - first.bandTopLeftInPixels.y,
        .isEnding = m_rightEdge == DottedRunEdge::Ends,
    };
    fitDotsBetweenCuts(frame, m_cuts, m_stretches);
}

} // namespace Ubiquitty::Rendering
