// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The beam's per-pixel raster, shared verbatim by the software compositor and
// the Metal Tektronix pass. Every quantity a pixel's colour depends on is an
// integer computed from the pixel's address and the run's endpoints, so the
// CPU and the GPU reach the same coverage byte and the same halo level for the
// same pixel regardless of either compiler's floating-point contraction or
// approximate transcendental functions. The header uses scalar arithmetic only
// so Metal can consume it.

#if !defined(__METAL_VERSION__)
#include <cstdint>
#endif

namespace Ubiquitty {
namespace Tek {

#if defined(__METAL_VERSION__)
using BeamWide = long;
#else
using BeamWide = std::int64_t;
#endif

/// One pen-down run of the beam on the raster's fixed-point lattice.
///
/// Endpoints snap to a sixteenth of a pixel; distances come back in 256ths of
/// a pixel. The run is a capsule: the distance from a pixel's centre is to the
/// nearest point of the segment, so its ends are round and a degenerate run is
/// a disc, which is how a plotted point draws.
class BeamRun {
public:
    /// Returns the distance units per device pixel.
    ///
    /// - Returns: 256: a distance is in 256ths of a pixel.
    static constexpr int unitsPerPixel()
    {
        return 256;
    }

    /// Returns the distance reported for a pixel past any beam or halo.
    ///
    /// - Returns: The saturated distance, in 256ths of a pixel.
    static constexpr int farDistance()
    {
        return farInSubpixels() * (unitsPerPixel() / subpixelsPerPixel());
    }

    /// Converts a device-pixel length to distance units, rounded.
    ///
    /// - Parameter pixels: A finite, non-negative length.
    /// - Returns: The length in 256ths of a pixel.
    static constexpr int units(float pixels)
    {
        return rounded(pixels * static_cast<float>(unitsPerPixel()));
    }

    /// Creates a run between two device-pixel positions.
    ///
    /// - Parameters:
    ///   - startX: The start's horizontal position.
    ///   - startY: The start's vertical position.
    ///   - endX: The end's horizontal position.
    ///   - endY: The end's vertical position.
    constexpr BeamRun(float startX, float startY, float endX, float endY)
        : m_startX(snapped(startX))
        , m_startY(snapped(startY))
        , m_endX(snapped(endX))
        , m_endY(snapped(endY))
    {
    }

    /// Returns the distance from a pixel's centre to the run.
    ///
    /// - Parameters:
    ///   - x: The pixel's column.
    ///   - y: The pixel's row.
    /// - Returns: The floor of the Euclidean distance in 256ths of a pixel,
    ///   saturated at ``farDistance()``.
    constexpr int distanceToPixel(int x, int y) const
    {
        const BeamWide centreX = static_cast<BeamWide>(x) * subpixelsPerPixel() + subpixelsPerPixel() / 2;
        const BeamWide centreY = static_cast<BeamWide>(y) * subpixelsPerPixel() + subpixelsPerPixel() / 2;
        const BeamWide deltaX = static_cast<BeamWide>(m_endX) - m_startX;
        const BeamWide deltaY = static_cast<BeamWide>(m_endY) - m_startY;
        const BeamWide towardX = centreX - m_startX;
        const BeamWide towardY = centreY - m_startY;
        const BeamWide lengthSquared = deltaX * deltaX + deltaY * deltaY;
        const BeamWide along = towardX * deltaX + towardY * deltaY;
        if (lengthSquared == 0 || along <= 0)
            return distanceToOffset(towardX, towardY);
        if (along >= lengthSquared)
            return distanceToOffset(centreX - m_endX, centreY - m_endY);
        // Square to the segment: the cross product over the length. A cross
        // product past the bound lies farther than `farInSubpixels` from the
        // line, because the length is at most the sum of the deltas' sizes;
        // under it the square fits in 63 bits.
        BeamWide cross = towardX * deltaY - towardY * deltaX;
        cross = cross < 0 ? -cross : cross;
        const BeamWide reach = static_cast<BeamWide>(farInSubpixels()) * (magnitude(deltaX) + magnitude(deltaY));
        if (cross >= reach)
            return farDistance();
        // floor(cross^2 * scale^2 / length^2) without the 64-bit overflow the
        // direct product risks: quotient and remainder scale separately.
        const BeamWide scale = unitsPerPixel() / subpixelsPerPixel();
        const BeamWide squared = cross * cross;
        const BeamWide quotient = squared / lengthSquared;
        const BeamWide remainder = squared - quotient * lengthSquared;
        const BeamWide distanceSquared = quotient * scale * scale + (remainder * scale * scale) / lengthSquared;
        return saturated(squareRoot(distanceSquared));
    }

private:
    /// Returns the endpoint lattice's steps per device pixel.
    ///
    /// - Returns: 16: endpoints snap to a sixteenth of a pixel.
    static constexpr int subpixelsPerPixel()
    {
        return 16;
    }

    /// Returns the distance past which a pixel is reported far.
    ///
    /// 256 pixels: past any beam or halo, and small enough that the bounded
    /// cross product of a run across a 16384-pixel drawable squares in 63 bits.
    ///
    /// - Returns: The bound, in sixteenths of a pixel.
    static constexpr int farInSubpixels()
    {
        return 256 * subpixelsPerPixel();
    }

    /// Returns a value's magnitude.
    ///
    /// - Parameter value: The signed value.
    /// - Returns: Its absolute value.
    static constexpr BeamWide magnitude(BeamWide value)
    {
        return value < 0 ? -value : value;
    }

    /// Returns the length of an offset from an endpoint.
    ///
    /// - Parameters:
    ///   - offsetX: The horizontal offset, in sixteenths of a pixel.
    ///   - offsetY: The vertical offset, in sixteenths of a pixel.
    /// - Returns: The floored length in 256ths of a pixel, saturated.
    static constexpr int distanceToOffset(BeamWide offsetX, BeamWide offsetY)
    {
        if (magnitude(offsetX) >= farInSubpixels() || magnitude(offsetY) >= farInSubpixels())
            return farDistance();
        const BeamWide scale = unitsPerPixel() / subpixelsPerPixel();
        return saturated(squareRoot((offsetX * offsetX + offsetY * offsetY) * scale * scale));
    }

    /// Clamps a distance to ``farDistance()``.
    ///
    /// - Parameter distance: A non-negative distance in 256ths of a pixel.
    /// - Returns: The distance, or ``farDistance()`` past it.
    static constexpr int saturated(BeamWide distance)
    {
        return distance < farDistance() ? static_cast<int>(distance) : farDistance();
    }

    /// Returns the integer square root, digit by digit.
    ///
    /// Shifts, adds and compares only, so the result is the exact floor on
    /// every compiler and under any floating-point mode.
    ///
    /// - Parameter value: A non-negative value below 2^62.
    /// - Returns: The floor of its square root.
    static constexpr BeamWide squareRoot(BeamWide value)
    {
        BeamWide result = 0;
        BeamWide bit = static_cast<BeamWide>(1) << 60;
        while (bit > value)
            bit >>= 2;
        while (bit != 0) {
            if (value >= result + bit) {
                value -= result + bit;
                result = (result >> 1) + bit;
            } else {
                result >>= 1;
            }
            bit >>= 2;
        }
        return result;
    }

    /// Rounds to the nearest integer, ties away from zero.
    ///
    /// No half is added to the float, so only the truncation rounds; a caller
    /// scales by a power of two first, which is exact.
    ///
    /// - Parameter value: A finite value representable as an integer.
    /// - Returns: The nearest integer.
    static constexpr int rounded(float value)
    {
        const int integral = static_cast<int>(value);
        const float fraction = value - static_cast<float>(integral);
        return integral + (fraction >= 0.5f) - (fraction <= -0.5f);
    }

    /// Snaps a device-pixel coordinate to the endpoint lattice.
    ///
    /// - Parameter value: The coordinate, in device pixels.
    /// - Returns: The coordinate in sixteenths of a pixel.
    static constexpr int snapped(float value)
    {
        return rounded(value * static_cast<float>(subpixelsPerPixel()));
    }

    int m_startX;
    int m_startY;
    int m_endX;
    int m_endY;
};

/// Returns the fixed-point unity of ``beamCoverage``.
///
/// - Returns: 2^24, full coverage.
constexpr int beamCoverageScale()
{
    return 1 << 24;
}

/// Returns the beam core's coverage of a pixel at a distance from the run.
///
/// Full inside the half-width less half a pixel, none past it plus half a
/// pixel, and a smoothstep across the one-pixel feather between: the
/// antialiased edge of the stroke, and at the run's ends its round cap.
///
/// - Parameters:
///   - distance: From ``BeamRun::distanceToPixel``.
///   - halfThickness: The stroke's half-width, from ``BeamRun::units``.
/// - Returns: The coverage, out of ``beamCoverageScale()``.
constexpr int beamCoverage(int distance, int halfThickness)
{
    const int feather = BeamRun::unitsPerPixel();
    int step = distance - (halfThickness - feather / 2);
    step = step < 0 ? 0 : (step > feather ? feather : step);
    // feather^3 is the scale: 3t^2 - 2t^3 with t = step / feather.
    return beamCoverageScale() - step * step * (3 * feather - 2 * step);
}

/// Returns a coverage as the alpha byte the composite mixes with.
///
/// - Parameter coverage: From ``beamCoverage``.
/// - Returns: The coverage scaled to 0...255, rounded half up.
constexpr int beamCoverageByte(int coverage)
{
    return static_cast<int>((255u * static_cast<unsigned>(coverage) + (1u << 23)) >> 24);
}

/// Returns the halo table's entries per device pixel of distance.
///
/// - Returns: 32: the table steps every 32nd of a pixel.
constexpr int haloLevelsPerPixel()
{
    return 32;
}

/// Returns the halo table's index for a distance.
///
/// - Parameter distance: From ``BeamRun::distanceToPixel``.
/// - Returns: The entry whose level lights the pixel.
constexpr int haloLevelIndex(int distance)
{
    return distance / (BeamRun::unitsPerPixel() / haloLevelsPerPixel());
}

} // namespace Tek
} // namespace Ubiquitty
