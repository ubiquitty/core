// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>

namespace Ubiquitty::Rendering {

/// The horizontal cell allocation inside a padded coverage slot, in pixels.
struct ClusterRasterBounds {
    double left { 0 };
    double width { 0 };
};

/// One horizontal transform applied to every glyph in a shaped cluster.
struct ClusterHorizontalFit {
    double scaleX { 1 };
    double translationX { 0 };
};

/// Fits the union of a cluster's ink and advance without enlarging it.
///
/// - Parameters:
///   - sourceLeft: The measured interval's left edge in slot coordinates.
///   - sourceRight: The measured interval's right edge in slot coordinates.
///   - target: The allocated cell interval, excluding atlas padding.
/// - Returns: Identity when contained or invalid; otherwise a translation and
///   optional horizontal contraction. Vertical positions remain unchanged.
[[nodiscard]] inline ClusterHorizontalFit fitClusterHorizontal(double sourceLeft, double sourceRight, ClusterRasterBounds target)
{
    const double targetRight = target.left + target.width;
    const double sourceWidth = sourceRight - sourceLeft;
    if (!std::isfinite(sourceLeft) || !std::isfinite(sourceRight) || !std::isfinite(sourceWidth)
        || !std::isfinite(target.left) || !std::isfinite(target.width) || !std::isfinite(targetRight)
        || sourceWidth < 0 || target.width <= 0)
        return {};
    if (sourceLeft >= target.left && sourceRight <= targetRight)
        return {};
    if (sourceWidth <= target.width) {
        const double translation = sourceLeft < target.left ? target.left - sourceLeft : targetRight - sourceRight;
        return std::isfinite(translation) ? ClusterHorizontalFit { 1, translation } : ClusterHorizontalFit {};
    }
    const double scale = target.width / sourceWidth;
    const double translation = target.left - sourceLeft * scale;
    return scale > 0 && std::isfinite(translation) ? ClusterHorizontalFit { scale, translation } : ClusterHorizontalFit {};
}

} // namespace Ubiquitty::Rendering
