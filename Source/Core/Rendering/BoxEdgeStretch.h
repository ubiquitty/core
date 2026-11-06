// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Rendering/BoxEdgeStretch.h — the shared font-glyph edge-stretch for
// box-drawing characters: take a real font's box/line glyph and 9-slice-stretch
// its connecting strokes out to the cell edges so neighbours meet with no gap,
// without disturbing the font's stroke weight or style. All operate on an R8 coverage
// buffer (W*H, row-major, top-left origin). Pure C++.

#include <cstdint>

namespace Ubiquitty::Rendering {

// Which cell edges a box/block glyph's strokes should reach, from the portable
// BoxDrawing decomposition: a stub in a direction (or a unit rect touching that
// side) means the glyph connects to that edge. Returns false (all edges clear)
// when the codePoint has no box/block form (the caller skips edge-stretch).
bool boxReachEdges(unsigned codePoint, bool* left, bool* right, bool* top, bool* bottom);

// Translate an R8 buffer (W*H) vertically by `shift` rows (positive = down) in
// place, filling vacated rows with 0. Applies the box family's single uniform
// centring shift (Rendering::BoxDrawing::centerShiftY) before the edge-stretch so every box
// glyph's strokes move by the same amount and stay on a shared row.
void translateScratchVertical(std::uint8_t* px, int W, int H, int shift);

// 9-slice edge-stretch a box/block font glyph already rasterised into `px` (W*H,
// R8) so its connecting strokes reach the glyph/cell box [gx0,gx1)x[gy0,gy1) and
// the surrounding margin, so the cell tiles with its neighbours. The centre (the
// junction the font draws correctly) is left INTACT; only the four edge slabs the
// glyph reaches (reach{L,R,T,B}) are replicated outward from a clean near-edge
// stroke sample to (and past) the cell edge — keeping each stroke on the exact
// row/column the font drew it. No-op when no edge is reached.
void edgeStretchBoxScratch(std::uint8_t* px, int W, int H, int gx0, int gx1, int gy0, int gy1, bool reachL, bool reachR, bool reachT, bool reachB);

} // namespace Ubiquitty::Rendering
