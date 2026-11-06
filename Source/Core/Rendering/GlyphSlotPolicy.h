// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Geometry.h"

// Source/Core/Rendering/GlyphSlotPolicy.h — the portable glyph-slot SIZING policy.
//
// A glyph whose ink spills past the cell box (Nerd-Font icons, powerline
// separators, an oversized fallback face) needs the atlas slot grown to a wide
// CANVAS, or the rasteriser crops it at slot-fill time. This is the one rule for
// how much margin to add per side: it folds the primary + glyph-fallback face
// overhang into a uniform margin, sized so a fallback glyph CENTERED on the cell
// overhangs only HALF its excess width each side (keeping the slot ~2× the cell,
// not ~3×). The glyph quad blits the full slot (atlasRectForSlotFull), so the extra
// width renders into the margins.
//
// Pure arithmetic over font metrics the platform supplies; a frontend feeds its
// local font / fallback bbox through it. Portable: no platform headers
// (Scripts/check-boundaries.py).

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

// Font metrics a platform gathers to size the slot. Bounding boxes are
// baseline-relative and y-up, in ATLAS pixels (the font built at fontSize*scale).
// double (not float) so the math stays byte-identical to the macOS floating-point
// path; the fold below is the verbatim macOS rule.
struct GlyphOverhangInputs {
    double advanceWidth = 0.0; // cell advance (≈ cell width)
    double ascent = 0.0; // cell ascent above the baseline
    double descent = 0.0; // cell descent below the baseline
    double lineHeight = 0.0; // full cell line height (the vertical clamp basis)
    // Primary monospace face bounding box.
    double primaryBboxX = 0.0;
    double primaryBboxY = 0.0;
    double primaryBboxWidth = 0.0;
    double primaryBboxHeight = 0.0;
    // Glyph-fallback (Nerd Font) face bounding box; folded in only when present.
    bool hasFallback = false;
    double fallbackBboxX = 0.0;
    double fallbackBboxY = 0.0;
    double fallbackBboxWidth = 0.0;
    double fallbackBboxHeight = 0.0;
};

// The slot geometry the policy resolves. slotWidthInPixels/slotHeightInPixels are the atlas slot
// (canvas) dims; glyphWidthInPixels is the cell-box width the non-subpixel UV maps.
struct GlyphSlot {
    int glyphWidthInPixels = 1;
    IntSize marginInPixels {}; // canvas margin on each side, per axis
    int slotWidthInPixels = 1; // glyphWidthInPixels + 2*marginInPixels.width + 1 (subpixel phase headroom)
    int slotHeightInPixels = 1; // ceil(lineHeight) + 2*marginInPixels.height
};

// The sizing policy. Clamped so a pathological fallback face can't blow up the
// atlas: the margin is bounded per side at one cell horizontally and half a
// line vertically, so the slot itself cannot exceed 3*cell+1 wide or
// 2*lineHeight tall.
/// How many distinct glyphs one screenful of cells can demand at once.
///
/// A frame may reference a different glyph in every cell, so a sheet holding
/// fewer slots than the screen holds cells can be asked for more than it has
/// while every one of them is still being drawn. It then evicts a slot a live
/// cell samples, and that cell draws whatever takes the slot next: a blank
/// where a letter was, another letter, or — where the sheets differ in shape —
/// a master from the doubled sheet at twice the size. The faults read as
/// unrelated glitches and are one exhaustion.
///
/// Sized from the largest screen rather than the current window, because the
/// sheet is shared across a font configuration and outlives any one window's
/// size: a window grown after the sheet was cut would otherwise be the case
/// that overflows it.
///
/// - Parameters:
///   - screenInPixels: The largest screen the window could occupy.
///   - cellInPixels: One cell's extent at the current font size.
/// - Returns: The slot count that cannot be exceeded by one frame, or zero
///   for a degenerate cell.
[[nodiscard]] inline int worstCaseGlyphSlotsForScreen(IntSize screenExtent, IntSize cellExtent)
{
    if (screenExtent.width <= 0 || screenExtent.height <= 0 || cellExtent.width <= 0 || cellExtent.height <= 0)
        return 0;
    const int columns = (screenExtent.width + cellExtent.width - 1) / cellExtent.width;
    const int rows = (screenExtent.height + cellExtent.height - 1) / cellExtent.height;
    return columns * rows;
}

inline GlyphSlot computeGlyphSlot(const GlyphOverhangInputs& in)
{
    GlyphSlot out;
    out.glyphWidthInPixels = static_cast<int>(std::ceil(in.advanceWidth));
    if (out.glyphWidthInPixels < 1)
        out.glyphWidthInPixels = 1;

    double overLeft = in.primaryBboxX < 0.0 ? -in.primaryBboxX : 0.0;
    double overRight = (in.primaryBboxX + in.primaryBboxWidth) - in.advanceWidth;
    double overTop = (in.primaryBboxY + in.primaryBboxHeight) - in.ascent;
    double overBottom = (-in.primaryBboxY) - in.descent;
    if (in.hasFallback) {
        // The fallback is centered on the cell box per glyph, so the widest one
        // overhangs only HALF its excess width each side.
        const double halfExcess = (in.fallbackBboxWidth - in.advanceWidth) * 0.5;
        overLeft = std::max(overLeft, halfExcess);
        overRight = std::max(overRight, halfExcess);
        // Fallback glyphs keep the text baseline (not vertically centered), so
        // the top/bottom overhang is the natural amount past ascent / descent.
        overTop = std::max(overTop, (in.fallbackBboxY + in.fallbackBboxHeight) - in.ascent);
        overBottom = std::max(overBottom, (-in.fallbackBboxY) - in.descent);
    }
    double marginX = std::ceil(std::max({ overLeft, overRight, 0.0 }));
    double marginY = std::ceil(std::max({ overTop, overBottom, 0.0 }));
    if (marginX < 0.0)
        marginX = 0.0;
    if (marginX > static_cast<double>(out.glyphWidthInPixels))
        marginX = static_cast<double>(out.glyphWidthInPixels);
    if (marginY < 0.0)
        marginY = 0.0;
    if (marginY > in.lineHeight * 0.5)
        marginY = in.lineHeight * 0.5;
    out.marginInPixels = { static_cast<int>(marginX), static_cast<int>(marginY) };

    // +1px width: the subpixel path shifts a glyph right by up to 7/8px for its
    // phase. Slot = cell box + margins on every side.
    out.slotWidthInPixels = out.glyphWidthInPixels + 2 * out.marginInPixels.width + 1;
    out.slotHeightInPixels = static_cast<int>(std::ceil(in.lineHeight)) + 2 * out.marginInPixels.height;
    if (out.slotHeightInPixels < 1)
        out.slotHeightInPixels = 1;
    return out;
}

/// How many ligature strips one screenful of cells can demand at once.
///
/// A run covers two cells at least, so half the cells bounds the strips a
/// frame can ask for. A sheet below that bound evicts a slot a live row still
/// draws from, and that row samples whatever lands there next.
///
/// - Parameter cellsOnScreen: The worst-case cell count, from
///   ``worstCaseGlyphSlotsForScreen``.
/// - Returns: The strip count one frame cannot exceed.
[[nodiscard]] inline int worstCaseLigatureStripsForCells(int cellsOnScreen)
{
    return cellsOnScreen <= 0 ? 0 : (cellsOnScreen + 1) / 2;
}

/// Whether a sheet's pins have grown far enough to threaten the next frame.
///
/// Half the sheet, because a frame that can allocate half a sheet without
/// evicting a pin cannot be refused at all when the sheet is cut for twice
/// the screen — which is the sizing ``worstCaseGlyphSlotsForScreen`` exists
/// to give it. Asking one step before the refusal is what keeps a cell from
/// drawing blank: a refusal is already a hole on screen, and the frame that
/// repairs it is the one after.
///
/// - Parameters:
///   - capacity: The sheet's slots.
///   - pinnedSlots: Slots no lease will let go, from
///     ``GlyphSlotCache::pinnedSlots``.
/// - Returns: Whether the view should re-baseline its pins.
[[nodiscard]] inline bool pinsCrowdOutNextFrame(int capacity, int pinnedSlots)
{
    return capacity > 0 && pinnedSlots * 2 > capacity;
}

/// Whether a sheet's pins have grown far enough that refusal is imminent.
///
/// Seven eighths, past the crowding line above: the gap between the two is
/// what a throttled correction is allowed to trade for fewer full rebuilds,
/// and this line is where the trade stops. Between here and full there may be
/// less headroom than one frame's demand, and a refused strip draws its cells
/// unhealed — a wrong pixel, not a slow one.
///
/// - Parameters:
///   - capacity: The sheet's slots.
///   - pinnedSlots: Slots no lease will let go.
/// - Returns: Whether the correction must not wait out its throttle.
[[nodiscard]] inline bool pinsNearRefusal(int capacity, int pinnedSlots)
{
    return capacity > 0 && pinnedSlots * 8 > capacity * 7;
}

/// The per-view throttle deciding when sheet pressure earns a full rebuild.
///
/// One decision for every frontend: spelled per arm it drifts, and an arm that
/// never reads the verdict draws fresh runs unhealed in the windows between
/// corrections. The state is two cooldown counters;
/// the policy is three rules, in priority order:
///
/// - a refusal already drew wrong pixels, and imminent refusal is about to:
///   both correct on the short throttle, because the only thing a longer wait
///   buys is more frames of unhealed text;
/// - crowding — pins past half the sheet — corrects on the same short
///   throttle, which bounds the full-rebuild rate a busy screen can sustain;
/// - a quiet sheet asks for nothing.
///
/// The caller grants the rebuild and re-arms via ``noteRebaselined``; asking
/// again before then keeps returning true, which is what lets an arm that
/// missed one frame's request honour it the next.
struct SheetPressureThrottle {
    /// Frames left before a crowding-driven correction may fire again.
    int framesUntilCrowdingAllowed { 0 };

    /// How many frames the short throttle spans.
    ///
    /// Fifteen at the 60 Hz frame clock — four corrections a second at
    /// sustained pressure, which bounds the cost while keeping the window a
    /// refusal can hide in shorter than a blink.
    static constexpr int throttleFrames = 15;

    /// Ticks the throttle and answers whether this frame earns a correction.
    ///
    /// - Parameters:
    ///   - isCrowding: Whether pins passed the crowding line on any sheet.
    ///   - isRefusalImminent: Whether pins passed the refusal-imminent line.
    ///   - hadRefusals: Whether the frame just built was refused a slot.
    /// - Returns: Whether the view should grant a full rebuild.
    [[nodiscard]] bool wantsCorrection(bool isCrowding, bool isRefusalImminent, bool hadRefusals)
    {
        if (framesUntilCrowdingAllowed > 0)
            --framesUntilCrowdingAllowed;
        // Wrong pixels outrank the throttle's economy: the throttle exists to
        // bound the cost of correcting a HEALTHY frame early, and neither of
        // these frames is healthy.
        if (hadRefusals || isRefusalImminent)
            return true;
        return framesUntilCrowdingAllowed <= 0 && isCrowding;
    }

    /// Re-arms the throttle after a granted correction.
    void noteRebaselined() { framesUntilCrowdingAllowed = throttleFrames; }
};

} // namespace Ubiquitty::Rendering
