// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Rendering/Unifont.h — read access to the embedded GNU Unifont (BMP)
// glyph bitmaps (Source/Core/Rendering/UnifontData.*, OFL 1.1). The compressed blob
// is inflated lazily on first lookup. Glyphs are 16px tall, 8 or 16px wide,
// 1 bit per pixel, row-major MSB-first. Used by the headless screenshot
// rasterizer; portable, no platform dependency.
//

#include "Base/Types.h"

namespace Ubiquitty::Rendering::Unifont {

struct Glyph {
    const uint8_t* bits; // nullptr when the code point is absent
    int width; // 8 or 16 (height is always 16); 0 when absent
};

// Glyph for a BMP code point, or {nullptr, 0} if Unifont lacks it (or the
// code point is outside the BMP). Lazily inflates the embedded blob.
Glyph lookup(uint32_t codePoint);

} // namespace Ubiquitty::Rendering::Unifont
