// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cmath>
#include <cstdint>

// SubpixelPhase — portable horizontal subpixel-phase selection for the
// subpixel-text render path. A glyph is rasterised once per (codePoint, phase);
// at draw time the cell's fractional pen X picks the nearest phase bucket and a
// pixel-snapped integer origin, so the glyph blits 1:1 onto the pixel grid with
// the right subpixel offset baked into its mask. Every frontend selects the
// IDENTICAL phase + snap, and the phase bits fed into the shared glyph key
// (monoGlyphKey / coverageClusterKey, bits 0-2) match across frontends.
//
// phaseCount lives here, colocated with the key derivations it feeds (the key's
// phase field is `phase & 0x7`, so phaseCount must stay <= 8). Header-only, no
// platform dependency.

namespace Ubiquitty::Rendering {

// Horizontal subpixel phases for the subpixel-text path: 8 -> 0.125px
// granularity. Each glyph is cached per (codePoint, phase); the value also
// feeds the 3 phase bits of the shared glyph key, so it must stay <= 8.
inline constexpr int subpixelPhaseCount = 8;

// The result of resolving a fractional pen X into a pixel-snapped origin plus
// a phase bucket. `snappedPenX` is the integer device-pixel column the glyph
// slot blits at; `phase` is in [0, phaseCount).
struct SubpixelPhase {
    int snappedPenX = 0;
    int phase = 0;
};

/// How a fractional pen picks its phase bucket.
///
/// `Nearest` is the portable contract: lround to the closest of `phaseCount`
/// buckets, and the glyph is rasterised at exactly that offset, so a backend
/// that positions continuously (FreeType, the bitmap families) draws it there.
/// `Floor` mirrors a rasteriser that quantises pens itself: CoreText floors a
/// fractional pen onto a grid whose pitch depends on the pixel size (see
/// nativeSubpixelPhaseCount), so the phase the pen lands in is the bucket
/// CoreText would have chosen for the true pen, and the atlas holds one mask
/// per bucket that CoreText actually distinguishes. Rounding to nearest over
/// such a rasteriser draws some columns one bucket right of where a native
/// CoreText draw at the same pen puts them.
enum class PhaseRounding : std::uint8_t {
    Nearest,
    Floor,
};

/// Returns the number of horizontal pen positions CoreText distinguishes inside one device pixel.
///
/// For a font rendered at `pixelSize` device pixels (point size times the
/// backing scale): fifths up to 8.33px, quarters to 11.11px, thirds to
/// 16.67px, halves to 33.33px, whole pixels above — the grid changes where
/// pixelSize crosses 100 / (3 n), i.e. CoreText positions to roughly 1/33 em.
/// Vertical pens always floor to whole pixels. The selector is the device
/// size: 12pt under a 2x CTM quantises like 24px.
/// Clamped to [1, 5]: below 6px the count was not measured, and 5 fits the
/// key's 3 phase bits with room.
///
/// - Parameter pixelSize: The font's em size in device pixels.
/// - Returns: The phase count, 1..5.
inline int nativeSubpixelPhaseCount(double pixelSize)
{
    if (!(pixelSize > 0.0))
        return 1;
    const double n = std::ceil(100.0 / (3.0 * pixelSize));
    if (n < 1.0)
        return 1;
    if (n > 5.0)
        return 5;
    return static_cast<int>(n);
}

/// Resolves a fractional pen X into an integer pixel origin and a phase bucket.
///
/// floor(penX) gives the base pixel; the fractional part picks one of
/// `phaseCount` buckets by `rounding`. Under Nearest a fraction that rounds up
/// to a full pixel carries into the next pixel (phase wraps to 0); under Floor
/// the phase never reaches phaseCount. Negative phases (only reachable from a
/// negative penX whose floor over-rounds) clamp to 0.
///
/// - Parameters:
///   - penX: The pen, in fractional device pixels.
///   - phaseCount: How many buckets one pixel divides into, at most 8.
///   - rounding: Nearest for the portable contract, Floor to mirror CoreText.
/// - Returns: The snapped origin and the phase.
inline SubpixelPhase phaseForPen(float penX, int phaseCount, PhaseRounding rounding = PhaseRounding::Nearest)
{
    SubpixelPhase out;
    int ix = static_cast<int>(std::floor(penX));
    const float fraction = (penX - ix) * phaseCount;
    int phase = rounding == PhaseRounding::Floor
        ? static_cast<int>(std::floor(fraction))
        : static_cast<int>(std::lround(fraction));
    if (phase >= phaseCount) {
        phase -= phaseCount;
        ix += 1;
    }
    if (phase < 0)
        phase = 0;
    out.snappedPenX = ix;
    out.phase = phase;
    return out;
}

} // namespace Ubiquitty::Rendering
