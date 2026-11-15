// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/PaddingGeometry.h"

// Strict native hit testing compares against the sum after the width product
// has rounded to double. Contracting them changes a half-open boundary by one
// double and rejects a point the native surface places inside its last cell.
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace Ubiquitty::Rendering {

std::optional<CellPosition> pointToCellIfInside(DoublePoint point, DoublePoint gridOrigin, DoubleSize cell, GridSize grid)
{
    if (!(cell.width > 0 && cell.height > 0))
        return std::nullopt;
    if (!(grid.columns > 0 && grid.rows > 0))
        return std::nullopt;
    const double gridWidth = static_cast<double>(grid.columns) * cell.width;
    if (!(point.x >= gridOrigin.x && point.x < gridOrigin.x + gridWidth))
        return std::nullopt;
    const CellPosition position = pointToCell({ static_cast<float>(point.x), static_cast<float>(point.y) },
        { static_cast<float>(gridOrigin.x), static_cast<float>(gridOrigin.y) },
        { static_cast<float>(cell.width), static_cast<float>(cell.height) }, grid, RowClamp::Unclamped);
    if (position.row < 0 || position.row >= grid.rows || position.column < 0 || position.column >= grid.columns)
        return std::nullopt;
    return position;
}

} // namespace Ubiquitty::Rendering
