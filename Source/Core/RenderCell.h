// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/CellPen.h"
#include "Core/Cursor/Blink.h"

#include <cstdint>

namespace Ubiquitty {

// Render-time cell view. Frontends consume this struct as if it were a
// fully-resolved cell; they do not need to know about Attribute::Blink or
// any other time-dependent attribute. The struct is the background-and-foreground-and-
// codePoint the screen should show at this (row, column) right now.
struct RenderCell {
    uint32_t codePoint; // canonical; 0 == paint background only
    Pen pen; // foreground/background + attributes as the renderer should see them
    float blinkAlpha; // 0..1 ink opacity for SGR 5/6; 1.0 when not blinking.
                      // Renderers multiply glyph + decoration alpha by this.
                      // background never blinks.
};

// Resolve a raw cell into its render-time form for the given monotonic
// tick. Pure function. Applies the SGR 5 / 6 blink curve via blinkAlpha
// (same step function as the cursor caret). Future time-dependent
// policies (e.g. inverse cursor cell, search highlight pulse) hang off
// the same hook.
//
// Callers passing a NULL `raw` get an all-blank RenderCell.
static inline RenderCell resolveRenderCell(const Cell* raw, uint32_t monotonicTicks)
{
    RenderCell out;
    if (!raw) {
        out.codePoint = 0;
        out.pen = Pen {};
        out.blinkAlpha = 1.0f;
        return out;
    }
    out.codePoint = raw->codePoint;
    out.pen = cellPen(*raw);
    out.blinkAlpha = 1.0f;
    if (out.pen.attributes.contains(Ubiquitty::VT::Attribute::Blink)) {
        const bool isFastBlink = out.pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
        const uint32_t period = isFastBlink ? (Cursor::Blink::caretPeriodTicks / 2) : Cursor::Blink::caretPeriodTicks;
        out.blinkAlpha = blinkAlpha(monotonicTicks, period);
    }
    return out;
}

} // namespace Ubiquitty
