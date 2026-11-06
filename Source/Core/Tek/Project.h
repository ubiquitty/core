// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Tek/Project.h — portable Tektronix 4014 space -> device viewport
// projection. No platform types: pure C++ in namespace Ubiquitty.
//
// Tek 4014 space (see Source/Core/Tek/Tek4014.h):
// x in [0, tekGridWidth-1]   (4096 wide)
// y in [0, tekGridHeight-1]  (3120 tall)
// origin BOTTOM-left, y grows up.
//
// Device space (window / back-buffer):
// origin TOP-left, y grows DOWN, pixels.
//
// Two transforms a frontend needs, both owned here:
//
// 1. Y-FLIP. Tek y=0 (bottom) maps to the BOTTOM of the device rect;
// Tek y=max (top) maps to the TOP. So device_y grows opposite to
// Tek y.
//
// 2. ASPECT / LETTERBOX. The 4014 tube is 4:3-ish (4096:3120 ~ 1.31).
// A device rect of arbitrary shape would distort the drawing if we
// stretched each axis independently. Instead we fit the Tek space
// into the device rect at a SINGLE uniform scale (the smaller of
// the per-axis scales) and centre the result, leaving equal
// letterbox/pillarbox margins on the two slack edges. Vectors stay
// straight and circles stay round.
//
// Everything is bounded integer / float math (no heap, no Apple
// headers). A frontend builds one TekProjection from its device rect +
// the Tek screen dims (from tekScreenWidth / tekScreenHeight), then maps
// each primitive's Tek coordinates through it.
//

#include "Core/Tek/Primitive.h"

#include <cstdint>

#include <span>

namespace Ubiquitty::Tek {

/// A device point after projection, in pixels with a top-left origin.
///
/// Float rather than int so a frontend rounds once, at the draw call.
/// Intermediate maths stays sub-pixel exact, which is what keeps a long
/// projected vector from accumulating a visible stair-step.
struct DevicePoint {
    float x;
    float y;
};

/// A device size in pixels, such as one alpha glyph cell.
struct DeviceSize {
    float w;
    float h;
};

/// A device dash pattern: alternating pen-down and pen-up run lengths.
///
/// A frontend turns this into its own dashing primitive or a manual segment
/// walk. Run lengths scale with the projection, so a dotted line keeps the same
/// visual density at any viewport size rather than dissolving as it grows.
///
/// A `count` of 0 means solid — draw one continuous line, ignoring the arrays.
struct DashPattern {
    int count; // number of run-length entries used (0 = solid)
    float on[4]; // "pen down" run lengths, px
    float off[4]; // "pen up"   run lengths, px
};

/// A uniform-scale, Y-flipped, centred mapping from Tek space into a device rect.
///
/// Plain data, built once per paint by `makeProjection` and read by every
/// projection call in that frame.
struct Projection {
    // Tek source extent (normally tekGridWidth x tekGridHeight).
    int tekWidth;
    int tekHeight;

    // Device target rect (top-left origin), px.
    float devX; // left
    float devY; // top
    float devWidth; // width
    float devHeight; // height

    // Uniform scale (device px per Tek unit) and the centring offset of
    // the fitted, letterboxed Tek rect inside the device rect.
    float scale;
    float offsetX; // device px from devX to the fitted rect's left
    float offsetY; // device px from devY to the fitted rect's top
};

/// Builds a projection fitting the Tek extent into a device rect.
///
/// One uniform scale, centred, so the result letterboxes or pillarboxes rather
/// than distorting. Degenerate input — any non-positive dimension — yields a
/// projection with scale 0 that maps everything to the device rect's origin: a
/// caller guarding its rects never reaches this, and one that does not stays
/// bounded instead of dividing by zero.
///
/// - Parameters:
///   - tekWidth: Tek-space width, normally `tekGridWidth`.
///   - tekHeight: Tek-space height, normally `tekGridHeight`.
///   - devX: Device rect left edge, pixels.
///   - devY: Device rect top edge, pixels.
///   - devWidth: Device rect width, pixels.
///   - devHeight: Device rect height, pixels.
/// - Returns: The projection to hand every other call in this header.
Projection makeProjection(int tekWidth, int tekHeight, float devX, float devY, float devWidth, float devHeight);

/// Maps a Tek-space point to a device point.
///
/// Applies the Y-flip — Tek's origin is bottom-left, the device's top-left —
/// along with the uniform scale and the centring offset.
///
/// - Parameters:
///   - p: The projection built for this frame.
///   - tekX: Tek-space x.
///   - tekY: Tek-space y.
/// - Returns: The device point, in pixels.
DevicePoint project(const Projection& p, int tekX, int tekY);

/// Maps a device point back to Tek space, the inverse of `project`.
///
/// The result is clamped to [0, tekWidth-1] x [0, tekHeight-1], because the
/// caller is normally a pointer position and a click outside the letterboxed
/// rect must still name a Tek cell the GIN report can carry.
///
/// - Parameters:
///   - p: The projection built for this frame.
///   - devX: Device x in window-local pixels.
///   - devY: Device y in window-local pixels.
///   - outTekX: Receives the Tek-space x.
///   - outTekY: Receives the Tek-space y.
void unproject(const Projection& p, float devX, float devY, int* outTekX, int* outTekY);

/// Returns the device dash pattern for a Tek line style.
///
/// Run lengths are scaled by the projection, so dot and dash density is
/// viewport-independent.
///
/// - Parameters:
///   - p: The projection built for this frame.
///   - lineStyle: A Tek `LineStyle` value.
/// - Returns: The pattern; `count` is 0 for a solid line.
DashPattern dashForLineStyle(const Projection& p, unsigned lineStyle);

/// The most runs one line may contribute to a stroke walk.
///
/// A dash pattern finer than its line is long would otherwise spend a whole
/// output on one primitive and drop every primitive after it, so the walk caps
/// each line here and a caller sizes its output from the same number.
inline constexpr int maxStrokeRunsPerLine = 64;

/// One pen-down run the beam draws, in device pixels.
///
/// A dashed line contributes one per stamped run, a solid line exactly one, and
/// a plotted point one run whose ends coincide. The beam is round, so a caller
/// strokes each run as a capsule: a degenerate run draws the beam's spot.
struct StrokeRun {
    DevicePoint start;
    DevicePoint end;
};

/// The beam's drawn geometry at one device scale, in device pixels.
///
/// One description every arm draws from: the crisp stroke's half-width, the
/// bloom halo's kernel, and the margin the projection reserves so a vector on
/// the edge of the address space draws whole, halo included. A frontend hands
/// it the backing scale and draws what it says rather than deriving a width or
/// a halo of its own.
///
/// The halo is a radial kernel over the distance from a point to the nearest
/// point of a run's segment, so it caps the run's ends and wraps its corners
/// with the same round falloff it has across the line. Overlapping halos
/// combine by maximum: the bloom of a polyline is the kernel of the distance
/// to the polyline, with no seam at a joint and no hot spot where two runs
/// meet.
struct BeamGeometry {
    /// Half the crisp stroke's width.
    float halfThickness;
    /// The Gaussian spread of the bloom.
    float haloSigma;
    /// The distance at which the bloom reaches zero.
    float haloReach;
    /// The bloom's strength on the stroke's centre line, a fraction of the beam.
    float haloPeak;
    /// The margin `makeBeamProjection` keeps clear on every side of the device
    /// rect.
    float edgeInset;
};

/// Returns the beam geometry for a backing scale.
///
/// Every length but the stroke width is proportional to `deviceScale`, so a
/// projection built in points at scale 1 and one built in pixels at the backing
/// scale place a Tek address at the same spot on the glass. The stroke is at
/// least one device pixel wide whatever the scale.
///
/// - Parameter deviceScale: Device pixels per point; non-positive reads as 1.
/// - Returns: The geometry, in device pixels.
BeamGeometry beamGeometry(float deviceScale);

/// Builds the projection a frame of the tube is drawn and hit-tested with.
///
/// The Tek extent is fitted into the device rect inset on every side by the
/// beam's `edgeInset`, so a vector at an address-space edge — a line along
/// y = 0 is the common one — lands with its stroke and its halo inside the
/// rect instead of half beyond it. Drawing and pointer unprojection both build
/// from here, so a GIN report names the address under the drawn stroke.
///
/// - Parameters:
///   - tekWidth: Tek-space width.
///   - tekHeight: Tek-space height.
///   - devX: Device rect left edge.
///   - devY: Device rect top edge.
///   - devWidth: Device rect width.
///   - devHeight: Device rect height.
///   - deviceScale: Device units per point: the backing scale for a drawable
///     in pixels, 1 for a view rect in points.
/// - Returns: The projection.
Projection makeBeamProjection(int tekWidth, int tekHeight, float devX, float devY, float devWidth, float devHeight, float deviceScale);

/// Returns the distance from a device point to the nearest point of a run.
///
/// - Parameters:
///   - run: The run, whose segment may be degenerate.
///   - point: The device point.
/// - Returns: The Euclidean distance, in device pixels.
float distanceToRun(const StrokeRun& run, DevicePoint point);

/// Returns the bloom's strength at a distance from the stroke's centre line.
///
/// A Gaussian of spread `haloSigma`, lowered and renormalised so it meets zero
/// at `haloReach` with no step and peaks at `haloPeak` on the line. The Metal
/// glow shader evaluates the same expression from the same geometry.
///
/// - Parameters:
///   - beam: The beam geometry.
///   - distance: Distance from the run, from ``distanceToRun``.
/// - Returns: The bloom's strength in [0, `haloPeak`].
float haloIntensity(const BeamGeometry& beam, float distance);

/// One entry of the halo table: the sRGB-encoded colour the bloom lifts a
/// pixel to at one distance from the beam.
///
/// The alpha byte is unused padding, so the table is a texel-shaped `uchar4`
/// to the Metal glow pass.
struct HaloLevel {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;
};

// Named for Swift, which reaches the table's buffer through it.
using HaloLevelSpan = std::span<HaloLevel>;

/// Returns how many entries the halo table holds for a beam.
///
/// - Parameter beam: The beam geometry.
/// - Returns: One entry per ``haloLevelsPerPixel()`` step out to the reach.
int haloLevelCount(const BeamGeometry& beam);

/// Fills the halo table for a beam colour.
///
/// Entry `i` is the beam colour scaled in linear light by ``haloIntensity`` at
/// `i / haloLevelsPerPixel()` pixels and encoded back to sRGB bytes. Both the
/// software compositor and the Metal glow pass index it with
/// ``haloLevelIndex`` over ``BeamRun::distanceToPixel``, so every arm lifts a
/// pixel to the same bytes; the bloom composites by per-channel maximum, which
/// commutes with the encoding.
///
/// - Parameters:
///   - beam: The beam geometry.
///   - red: The beam's sRGB red byte.
///   - green: The beam's sRGB green byte.
///   - blue: The beam's sRGB blue byte.
///   - out: Receives the table; entries past ``haloLevelCount`` are not
///     written.
/// - Returns: The number of entries written.
int fillHaloLevels(const BeamGeometry& beam, std::uint8_t red, std::uint8_t green, std::uint8_t blue, HaloLevelSpan out);

// The named aliases Swift reaches the stroke walk through: the importer hands
// a NAMED span alias the buffer-pointer initialiser, where the direct spelling
// imports only as an opaque generic.
using PrimitiveSpan = std::span<const Primitive>;
using StrokeRunSpan = std::span<StrokeRun>;

/// Subdivides a segment into the runs a dashed line style draws.
///
/// The pattern is stamped along the segment from its start, alternating a
/// pen-down length with a pen-up one and cycling through the entries; only the
/// pen-down runs come back, so a caller strokes each returned run and needs to
/// know nothing about the style.
///
/// A pattern with no entries, and a segment shorter than half a device pixel,
/// both yield the whole segment as one run: a solid line and a dot are the same
/// stroke to the caller, and subdividing below the pixel would round every run
/// to the same two endpoints anyway.
///
/// A non-positive run length in the pattern is treated as one pixel. Zero would
/// advance the walk by nothing and the segment would never be consumed, so the
/// bound here is what makes the loop finite for a pattern the terminal is free
/// to have built badly.
///
/// - Parameters:
///   - start: The segment's first endpoint, projected.
///   - end: The segment's second endpoint, projected.
///   - dash: The pattern, from ``dashForLineStyle``.
///   - out: Receives the runs; writing stops when it is full.
/// - Returns: The number of runs written, never more than `out.size()`.
int dashRunsForSegment(DevicePoint start, DevicePoint end, const DashPattern& dash, StrokeRunSpan out);

/// The most runs one alpha glyph contributes to a stroke walk.
inline constexpr int maxStrokeRunsPerGlyph = 24;

/// Fills `out` with the strokes an alpha glyph draws, in device pixels.
///
/// The 4014 wrote its characters with the beam, as short vectors inside a cell
/// whose size the character size sets, and that is what this draws: a stroke
/// letterform on a four-by-six grid, scaled into the glyph's cell with its
/// baseline a little above the cell's bottom, the way the tube's alpha cursor
/// sits at the bottom of the line. Lower case draws as the capital, which is
/// the repertoire the base 4014 had; a code point with no pattern draws the
/// hollow box the tube showed for a byte it could not letter; a space draws
/// nothing. A frontend that draws the walk's runs draws text with them, and
/// the software compositor strokes the same runs, so every arm letters alike.
///
/// - Parameters:
///   - p: The projection built for this frame.
///   - glyph: A `Glyph` primitive; any other kind yields no runs.
///   - out: Receives the runs; writing stops when it is full.
/// - Returns: The number of runs written, never more than `out.size()`.
int glyphStrokeRuns(const Projection& p, const Primitive& glyph, StrokeRunSpan out);

/// Fills `out` with the runs a display list's strokes draw.
///
/// Walks `primitives` in order, projecting each and turning it into runs: a line
/// through its style's dash pattern, a point into one degenerate run at its
/// position, a glyph into the strokes of its letterform through
/// ``glyphStrokeRuns``.
///
/// Writing stops when `out` fills, and the return says how many were written
/// rather than how many the list wanted: a display list is bounded but a dash
/// pattern finer than its lines are long is not, so the caller's extent is the
/// budget.
///
/// - Parameters:
///   - primitives: The display list, committed or working.
///   - projection: The projection built for this frame.
///   - out: Receives the runs.
/// - Returns: The number of runs written, never more than `out.size()`.
int buildStrokeRuns(PrimitiveSpan primitives, const Projection& projection, StrokeRunSpan out);

/// Returns the device size of one alpha glyph cell for a Tek character size.
///
/// The four 4014 sizes are defined by how many character columns fit across the
/// tube — 74, 81, 121 and 133 — rather than by a point size, so a frontend
/// cannot derive the cell from a font and has to ask.
///
/// - Parameters:
///   - p: The projection built for this frame.
///   - charSize: A Tek `CharSize` value.
/// - Returns: The device width and height of one alpha cell.
DeviceSize charCellSize(const Projection& p, unsigned charSize);

} // namespace Ubiquitty::Tek
