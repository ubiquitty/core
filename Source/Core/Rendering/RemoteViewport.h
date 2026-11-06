// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/PaddingGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Ubiquitty::Rendering {

/// How a non-owner presents a host grid without changing its dimensions.
enum class RemoteViewportPolicy : std::uint8_t {
    Letterbox,
    Pan,
    Fit
};

/// Resolves a saved policy token; unknown values retain the default.
/// - Parameter token: The configuration token.
/// - Returns: The requested policy, or Letterbox.
[[nodiscard]] inline RemoteViewportPolicy remoteViewportPolicyFromString(std::string_view token)
{
    if (token == "pan")
        return RemoteViewportPolicy::Pan;
    return token == "fit" ? RemoteViewportPolicy::Fit : RemoteViewportPolicy::Letterbox;
}

/// Returns the canonical configuration token for a presentation policy.
/// - Parameter policy: The policy to serialize.
/// - Returns: Its stable token.
[[nodiscard]] inline std::string_view remoteViewportPolicyToString(RemoteViewportPolicy policy)
{
    if (policy == RemoteViewportPolicy::Pan)
        return "pan";
    return policy == RemoteViewportPolicy::Fit ? "fit" : "letterbox";
}

/// The scale and centered origin of a host grid inside a local viewport.
struct RemoteFitPlacement {
    float scale = 1.0f;
    FloatPoint origin;
};

/// Fits an unchanged grid uniformly without enlarging it or crossing its floor.
///
/// A viewport smaller than the floor-sized grid clips it symmetrically. All
/// lengths use the same unit; the caller supplies its existing font-size floor.
/// Invalid geometry retains native size rather than producing a nonfinite map.
/// - Parameters:
///   - viewport: The visible local content extent.
///   - cell: The natural cell extent.
///   - grid: The host's authoritative dimensions.
///   - minScale: The smallest permitted presentation scale, in (0, 1].
/// - Returns: The local transform; it never requests a host resize.
[[nodiscard]] inline RemoteFitPlacement remoteFitPlacement(FloatSize viewport, FloatSize cell, GridSize grid, float minScale)
{
    if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width < 0 || viewport.height < 0
        || !std::isfinite(cell.width) || !std::isfinite(cell.height) || cell.width <= 0 || cell.height <= 0
        || grid.columns <= 0 || grid.rows <= 0 || !std::isfinite(minScale) || minScale <= 0 || minScale > 1)
        return {};
    const float width = cell.width * static_cast<float>(grid.columns);
    const float height = cell.height * static_cast<float>(grid.rows);
    if (!std::isfinite(width) || !std::isfinite(height))
        return {};
    const float scale = std::clamp(std::min(viewport.width / width, viewport.height / height), minScale, 1.0f);
    return { scale, { (viewport.width - width * scale) * 0.5f, (viewport.height - height * scale) * 0.5f } };
}

/// A native-size view's local offset and the host cursor it may follow.
struct RemotePanRequest {
    FloatSize viewport;
    FloatSize cell;
    GridSize grid;
    CellPosition cursor;
    FloatPoint previousOffset;
    /// False while viewing history or while the host hides its cursor.
    bool shouldFollowCursor = true;
};

/// Keeps the visible host cursor inside a bounded native-size viewport.
///
/// All lengths use the same unit. An offset moves only when the cursor leaves
/// the visible rectangle or a resize makes the previous offset out of range.
/// The result is a local presentation offset, never a host resize request.
/// - Parameter request: The immutable grid, viewport, cursor and previous offset.
/// - Returns: The nonnegative offset into the host picture; zero for invalid geometry.
[[nodiscard]] inline FloatPoint remotePanOffset(const RemotePanRequest& request)
{
    auto offsetForAxis = [&](float viewport, float cell, int count, int cursor, float previous) {
        if (!std::isfinite(viewport) || !std::isfinite(cell) || viewport <= 0 || cell <= 0 || count <= 0)
            return 0.0f;
        const float extent = cell * static_cast<float>(count);
        if (!std::isfinite(extent))
            return 0.0f;
        const float maximum = std::max(0.0f, extent - viewport);
        float offset = std::isfinite(previous) ? std::clamp(previous, 0.0f, maximum) : 0.0f;
        if (request.shouldFollowCursor) {
            const float start = static_cast<float>(std::clamp(cursor, 0, count - 1)) * cell;
            // A cell wider than the viewport keeps its leading edge visible.
            const float end = start + std::min(cell, viewport);
            if (start < offset)
                offset = start;
            else if (end > offset + viewport)
                offset = end - viewport;
        }
        return std::clamp(offset, 0.0f, maximum);
    };
    return {
        offsetForAxis(request.viewport.width, request.cell.width, request.grid.columns, request.cursor.column, request.previousOffset.x),
        offsetForAxis(request.viewport.height, request.cell.height, request.grid.rows, request.cursor.row, request.previousOffset.y)
    };
}

/// A complete picture's local placement; the host's grid and pixels stay intact.
struct RemoteViewportPlacement {
    float scale = 1.0f;
    FloatPoint origin {};
    FloatPoint panOffset {};

    /// Converts a viewport point back into the unscaled host picture.
    [[nodiscard]] FloatPoint picturePoint(FloatPoint point) const
    {
        return { (point.x - origin.x) / scale, (point.y - origin.y) / scale };
    }

    /// Converts a host-picture point into this viewport.
    [[nodiscard]] FloatPoint viewportPoint(FloatPoint point) const
    {
        return { origin.x + point.x * scale, origin.y + point.y * scale };
    }
};

/// The authority and geometry supplied by a pane's current model owner.
struct RemoteViewportRequest {
    RemoteViewportPolicy policy = RemoteViewportPolicy::Letterbox;
    FloatSize viewport {};
    FloatSize cell {};
    GridSize grid {};
    CellPosition cursor {};
    FloatPoint previousPanOffset {};
    float minimumScale = 1.0f;
    bool isGeometryOwnedElsewhere = false;
    /// A program-controlled graphics viewport retains its own placement.
    bool isProgramViewport = false;
    bool shouldFollowCursor = true;
};

/// Places an immutable picture using the same policy on every frontend.
///
/// The compositor applies this once to the complete picture, so glyphs, rules,
/// images, cursor and overlays cannot acquire different transforms. Authority
/// comes from the current attachment, never from retained screen dimensions.
/// - Parameter request: Current owner facts and local presentation geometry.
/// - Returns: Identity for an owner, a program viewport, or invalid geometry.
[[nodiscard]] inline RemoteViewportPlacement remoteViewportPlacement(const RemoteViewportRequest& request)
{
    if (!request.isGeometryOwnedElsewhere || request.isProgramViewport
        || !std::isfinite(request.viewport.width) || !std::isfinite(request.viewport.height)
        || request.viewport.width <= 0 || request.viewport.height <= 0
        || !std::isfinite(request.cell.width) || !std::isfinite(request.cell.height)
        || request.cell.width <= 0 || request.cell.height <= 0
        || request.grid.columns <= 0 || request.grid.rows <= 0)
        return {};
    if (request.policy == RemoteViewportPolicy::Pan) {
        const FloatPoint offset = remotePanOffset({ request.viewport, request.cell, request.grid,
            request.cursor, request.previousPanOffset, request.shouldFollowCursor });
        return { 1.0f, { -offset.x, -offset.y }, offset };
    }
    const float floor = request.policy == RemoteViewportPolicy::Fit ? request.minimumScale : 1.0f;
    const RemoteFitPlacement fit = remoteFitPlacement(request.viewport, request.cell, request.grid, floor);
    return { fit.scale, fit.origin, {} };
}

} // namespace Ubiquitty::Rendering
