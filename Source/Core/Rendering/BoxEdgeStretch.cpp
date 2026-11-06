// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Rendering/BoxEdgeStretch.cpp — shared font-glyph box-drawing edge-
// stretch over an R8 coverage buffer, independent of the font rasterizer.

#include "Core/Rendering/BoxEdgeStretch.h"

#include "Core/Rendering/BoxDrawing.h"

#include <algorithm>
#include <cstring>

namespace Ubiquitty::Rendering {

bool boxReachEdges(unsigned codePoint, bool* left, bool* right, bool* top, bool* bottom)
{
    *left = *right = *top = *bottom = false;
    // A dash is in the box family for the centring shift, never for the
    // stretch: its decomposition emits edge-reaching stubs like the solid
    // line's, but 9-slicing a dash glyph stretches its end segments, which
    // staggers the pattern under fractional advance and closes the gaps that
    // make it a dash. The font drew it cell-wide already.
    if (Rendering::BoxDrawing::isDash(codePoint))
        return false;
    Rendering::BoxDrawing::Glyph glyph;
    if (Rendering::BoxDrawing::decompose(codePoint, &glyph) <= 0)
        return false;
    for (int i = 0; i < glyph.count; ++i) {
        const Rendering::BoxDrawing::Primitive& p = glyph.prim[i];
        if (p.kind == Rendering::BoxDrawing::PrimitiveKind::Stub) {
            if (p.direction == Rendering::BoxDrawing::Direction::Left)
                *left = true;
            if (p.direction == Rendering::BoxDrawing::Direction::Right)
                *right = true;
            if (p.direction == Rendering::BoxDrawing::Direction::Up)
                *top = true;
            if (p.direction == Rendering::BoxDrawing::Direction::Down)
                *bottom = true;
        } else if (p.kind == Rendering::BoxDrawing::PrimitiveKind::Arc) {
            *left |= p.x0 <= 0.0f;
            *right |= p.x0 >= 1.0f;
            *top |= p.y0 <= 0.0f;
            *bottom |= p.y0 >= 1.0f;
        } else if (p.kind == Rendering::BoxDrawing::PrimitiveKind::Rect) {
            if (p.x0 <= 0.0f)
                *left = true;
            if (p.x1 >= 1.0f)
                *right = true;
            if (p.y0 <= 0.0f)
                *top = true;
            if (p.y1 >= 1.0f)
                *bottom = true;
        } else if (p.kind == Rendering::BoxDrawing::PrimitiveKind::EdgeLine) {
            // Reach is along the stroke's axis: a vertical edge line spanning
            // the unit cell continues into its neighbours above and below, so
            // a font-shipped glyph of this family stretches to meet them.
            if (p.direction == Rendering::BoxDrawing::Direction::Up) {
                if (p.y0 <= 0.0f)
                    *top = true;
                if (p.y1 >= 1.0f)
                    *bottom = true;
            } else {
                if (p.x0 <= 0.0f)
                    *left = true;
                if (p.x1 >= 1.0f)
                    *right = true;
            }
        }
    }
    return true;
}

void translateScratchVertical(std::uint8_t* px, int W, int H, int shift)
{
    if (shift == 0)
        return;
    if (shift >= H || shift <= -H) {
        std::memset(px, 0, static_cast<std::size_t>(W) * H);
        return;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(W);
    if (shift > 0) {
        // Move rows down: copy from bottom up, then clear the top `shift` rows.
        for (int y = H - 1; y >= shift; --y)
            std::memcpy(px + static_cast<std::size_t>(y) * rowBytes, px + static_cast<std::size_t>(y - shift) * rowBytes, rowBytes);
        std::memset(px, 0, static_cast<std::size_t>(shift) * rowBytes);
    } else {
        // Move rows up: copy from top down, then clear the bottom `-shift` rows.
        const int s = -shift;
        for (int y = 0; y < H - s; ++y)
            std::memcpy(px + static_cast<std::size_t>(y) * rowBytes, px + static_cast<std::size_t>(y + s) * rowBytes, rowBytes);
        std::memset(px + static_cast<std::size_t>(H - s) * rowBytes, 0, static_cast<std::size_t>(s) * rowBytes);
    }
}

void edgeStretchBoxScratch(std::uint8_t* px, int W, int H, int gx0, int gx1, int gy0, int gy1, bool reachL, bool reachR, bool reachT, bool reachB)
{
    if (!(reachL || reachR || reachT || reachB))
        return;
    auto at = [&](int x, int y) -> std::uint8_t& { return px[y * W + x]; };

    const int cx = (gx0 + gx1) / 2, cy = (gy0 + gy1) / 2;
    const std::uint8_t ink = 128;
    auto colHasInk = [&](int x) {
        for (int y = gy0; y < gy1; ++y)
            if (at(x, y) >= ink)
                return true;
        return false;
    };
    auto colMaxCoverage = [&](int x) {
        std::uint8_t maximum = 0;
        for (int y = gy0; y < gy1; ++y)
            maximum = std::max(maximum, at(x, y));
        return maximum;
    };
    auto rowHasInk = [&](int y) {
        for (int x = gx0; x < gx1; ++x)
            if (at(x, y) >= ink)
                return true;
        return false;
    };
    auto rowMaxCoverage = [&](int y) {
        std::uint8_t maximum = 0;
        for (int x = gx0; x < gx1; ++x)
            maximum = std::max(maximum, at(x, y));
        return maximum;
    };

    // Sample from the EDGE inward, not from a fixed inset. A double rule's outer
    // rail is the one nearest the edge; starting from an interior inset can land
    // on the inner rail and fill its gap to the edge, which thickens one side of
    // the box instead of extending the outer rail that should meet the border.
    if (reachL) {
        int sx = gx0;
        while (sx < cx && !colHasInk(sx))
            ++sx;
        if (sx + 1 < cx && colHasInk(sx + 1) && colMaxCoverage(sx + 1) > colMaxCoverage(sx))
            ++sx;
        if (colHasInk(sx)) {
            for (int y = gy0; y < gy1; ++y) {
                std::uint8_t v = at(sx, y);
                for (int x = 0; x < sx; ++x)
                    at(x, y) = v;
            }
        }
    }
    if (reachR) {
        int sx = gx1 - 1;
        while (sx > cx && !colHasInk(sx))
            --sx;
        if (sx - 1 > cx && colHasInk(sx - 1) && colMaxCoverage(sx - 1) > colMaxCoverage(sx))
            --sx;
        if (colHasInk(sx)) {
            for (int y = gy0; y < gy1; ++y) {
                std::uint8_t v = at(sx, y);
                for (int x = sx + 1; x < W; ++x)
                    at(x, y) = v;
            }
        }
    }
    if (reachT) {
        int sy = gy0;
        while (sy < cy && !rowHasInk(sy))
            ++sy;
        if (sy + 1 < cy && rowHasInk(sy + 1) && rowMaxCoverage(sy + 1) > rowMaxCoverage(sy))
            ++sy;
        if (rowHasInk(sy)) {
            for (int x = 0; x < W; ++x) {
                std::uint8_t v = at(x, sy);
                for (int y = 0; y < sy; ++y)
                    at(x, y) = v;
            }
        }
    }
    if (reachB) {
        int sy = gy1 - 1;
        while (sy > cy && !rowHasInk(sy))
            --sy;
        if (sy - 1 > cy && rowHasInk(sy - 1) && rowMaxCoverage(sy - 1) > rowMaxCoverage(sy))
            --sy;
        if (rowHasInk(sy)) {
            for (int x = 0; x < W; ++x) {
                std::uint8_t v = at(x, sy);
                for (int y = sy + 1; y < H; ++y)
                    at(x, y) = v;
            }
        }
    }
}

} // namespace Ubiquitty::Rendering
