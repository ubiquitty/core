// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Tek {

// Primitive kinds in the storage-tube display list.
enum class PrimitiveKind : uint8_t {
    Line = 0, // line segment (x0,y0)->(x1,y1), lineStyle
    Point, // single plotted point (x,y), lineStyle
    Glyph, // alpha glyph: codePoint at baseline (x,y), charSize
};

// Line / vector pattern. Matches ESC `..d (h..o fold onto these).
enum class LineStyle : uint8_t {
    Solid = 0,
    Dotted,
    DotDash,
    ShortDash,
    LongDash,
};

// Four 4014 character sizes (largest..smallest glyph).
enum class CharSize : uint8_t {
    Large = 0, // ESC 8 — 35x74
    Second, // ESC 9 — 38x81
    Third, // ESC : — 58x121
    Small, // ESC ; — 64x133
};

// One display-list entry. POD; a tagged union by `kind`.
// Coordinates are in Tek space (0..4095 x, 0..3119 y, origin BL).
struct Primitive {
    PrimitiveKind kind;
    LineStyle lineStyle; // for LINE / POINT
    CharSize charSize; // for CHAR
    uint8_t pad;
    int16_t x0; // LINE start / POINT pos / CHAR baseline x
    int16_t y0;
    int16_t x1; // LINE end (unused for POINT / CHAR)
    int16_t y1;
    uint32_t codePoint; // CHAR glyph (Unicode); 0 otherwise
};

} // namespace Ubiquitty::Tek
