// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/SwiftInterop.h"

#include <cmath>

// Portable per-cell pixel metrics driving the decoration pass and cell
// geometry. Platform backends fill these from their font engines or fallback
// geometry; the decoration algorithm is identical across backends. Plain POD,
// no platform/GPU/font-library types (enforced by Scripts/check-boundaries.py).

namespace Ubiquitty::Rendering {

/// Per-cell pixel metrics the decoration pass lays a frame out with.
///
/// Escapable: it holds no pointer, so a copy outlives the frame that made it and
/// Swift may return one by value.
struct UBIQUITTY_ESCAPABLE CellMetrics {
    float cellWidth = 0.0f;
    float cellHeight = 0.0f;
    float baselineFromTop = 0.0f; // baseline y measured from the cell top
    float underlineThickness = 1.0f; // solid-rule stroke (device px)
    float underlineDrop = 0.0f; // px below the baseline to the rule line
    /// The rule stroke and drop for a BOLD cell, which most faces set heavier
    /// and lower than the regular ones.
    ///
    /// A thickness at or below zero means the face offers no bold variant, and
    /// both the thickness and the drop then fall back to the regular pair —
    /// one sentinel for both, because a drop of zero is a real drop and cannot
    /// signal its own absence.
    float underlineThicknessBold = 0.0f;
    float underlineDropBold = 0.0f;
    /// How much of `underlineDrop` the caller asked for, device pixels.
    ///
    /// Folded into the drop as well, since every consumer wants the total. It is
    /// carried apart because the clamp keeping a rule inside its cell has to tell
    /// a rule pushed out by ROUNDING — which it corrects — from one moved down by
    /// the configured `underline_offset`, which it must leave alone.
    float underlineOffset = 0.0f;
    float underlineCurlyThickness = 0.0f; // unrounded curl weight; <=0 ⇒ underlineThickness
    float strikeThickness = 1.0f;
    /// The strike stroke for a BOLD cell. At or below zero means the face
    /// offers no bold variant and ``strikeThickness`` stands for both.
    float strikeThicknessBold = 0.0f;
    float xHeight = 0.0f; // px; strike rides baseline - xHeight*0.5
    bool skipInk = false; // cut underlines where glyph ink (descenders) crosses
    bool shouldUnderlineFollowInk = false; // a run's outer rule reaches the letter's ink past the cell box
    float skipInkPadding = 1.0f; // device-pixel dilation around crossing ink
    /// Device pixels per atlas pixel, independent of the cell's tracking.
    ///
    /// Glyph ink uses this scale while its pen advances by `cellWidth`. The
    /// skip-ink halo converts device pixels to atlas pixels by dividing by it.
    /// The two units coincide when the atlas uses the display's backing scale.
    float atlasToDevicePixels = 1.0f;

    /// Device pixels per point.
    ///
    /// The rule pass snaps edges and weights to whole device pixels, and the
    /// constants it snaps by are in points — a gap of one point, a drop of
    /// three quarters. A backend leaving this at 1 gets those constants in
    /// pixels, which is right only where the two coincide.
    float deviceScale = 1.0f;

    /// Uniform local presentation scale, separate from display backing scale.
    /// Image offsets and intrinsic pixel sizes share this transform with cells.
    float presentationScale = 1.0f;

    /// Device pixels the straight underline strokes rise by to clear the
    /// baseline, scaled with the type size.
    ///
    /// Dotted and curly rules do not take it: they sit on the line itself, and
    /// lifting them would leave the dots riding above the text they belong to.
    ///
    /// ``underlineStrokeLiftForEm`` derives it for a face that has an em. A
    /// rasterizer whose `FaceMetrics::fontSize` is zero has none, and sets this
    /// from its own geometry instead.
    float underlineStrokeLift = 0.0f;
};

/// Scales cell placement, glyph ink, and decorations by one local transform.
///
/// Atlas storage remains unchanged; its sampling ratio carries the smaller ink.
/// - Parameters:
///   - metrics: The natural frame metrics.
///   - scale: A finite positive uniform scale.
/// - Returns: Scaled metrics, or the original metrics for an invalid scale.
[[nodiscard]] inline CellMetrics scaledCellMetrics(CellMetrics metrics, float scale)
{
    if (!std::isfinite(scale) || scale <= 0 || scale == 1)
        return metrics;
    metrics.cellWidth *= scale;
    metrics.cellHeight *= scale;
    metrics.baselineFromTop *= scale;
    metrics.underlineThickness *= scale;
    metrics.underlineDrop *= scale;
    metrics.underlineThicknessBold *= scale;
    metrics.underlineDropBold *= scale;
    metrics.underlineOffset *= scale;
    metrics.underlineCurlyThickness *= scale;
    metrics.strikeThickness *= scale;
    metrics.strikeThicknessBold *= scale;
    metrics.xHeight *= scale;
    metrics.skipInkPadding *= scale;
    metrics.atlasToDevicePixels *= scale;
    metrics.deviceScale *= scale;
    metrics.presentationScale *= scale;
    metrics.underlineStrokeLift *= scale;
    return metrics;
}

/// Returns the underline lift for a face of the given em, in device pixels.
///
/// A straight rule sits on the baseline's drop, and this is what keeps its top
/// edge off the descender row — without it the rule lands one device row lower
/// and reads as a heavier underline than the same text on another backend.
///
/// Written once because every backend needs the constant. It is `em / 24` however
/// it is spelled: `0.5 * deviceScale * points / 12` folds the halving and the
/// twelfth together into the same expression, on an em already in device pixels.
///
/// Only for a face that has an em. A rasterizer reporting `FaceMetrics::fontSize`
/// zero keeps no face of its own, and a lift derived from that zero would be no
/// lift at all — which is the defect the value exists to prevent, not a neutral
/// default. Such a rasterizer sets the lift from its own box.
///
/// - Parameter emInDevicePixels: The face's em size, in device pixels.
/// - Returns: The lift in device pixels, or zero for a face with no em.
[[nodiscard]] inline constexpr float underlineStrokeLiftForEm(float emInDevicePixels)
{
    return emInDevicePixels > 0.0f ? emInDevicePixels / 24.0f : 0.0f;
}

// The two spellings the tree carried before this one, pinned as equal so a change
// to either is a compile error rather than a divergence between backends.
static_assert(underlineStrokeLiftForEm(24.0f) == 1.0f);
static_assert(underlineStrokeLiftForEm(12.0f) == 0.5f);
static_assert(underlineStrokeLiftForEm(0.0f) == 0.0f);

/// Rounds a cell dimension to whole device pixels, never to nothing.
///
/// A cell is an integer number of pixels — the atlas slot, the quad and the grid
/// all index by it — so the float a font engine reports has to land on one. Half
/// rounds up, and the result is clamped to 1 because a zero-width cell divides a
/// viewport into infinitely many columns, and a face with no metrics reports
/// zero.
///
/// Written once because three backends need it: the portable CPU raster, the
/// Linux CPU path and the Vulkan atlas. A drift between them puts the atlas and
/// the raster on different cell sizes, which reads as every glyph being offset
/// rather than as a rounding rule that moved.
///
/// - Parameter valueInPixels: The unrounded dimension, in device pixels.
/// - Returns: The dimension in whole device pixels, at least 1.
[[nodiscard]] inline constexpr int cellDimension(float valueInPixels)
{
    const int rounded = static_cast<int>(valueInPixels + 0.5f);
    return rounded > 0 ? rounded : 1;
}

// The behaviour the three copies shared, pinned so a change is a compile error
// rather than a divergence nobody attributes to rounding.
static_assert(cellDimension(7.4f) == 7);
static_assert(cellDimension(7.5f) == 8);
static_assert(cellDimension(0.0f) == 1);
static_assert(cellDimension(-3.0f) == 1);

/// A face's own vertical metrics, in device pixels, for geometry defined against
/// the TYPE rather than against the cell.
///
/// The OSC 133 prompt-mark bracket is a fraction of the point size and is centred
/// on the glyph ink box, so it needs the ascent and descent the face reports —
/// not ``CellMetrics::baselineFromTop`` and its complement, which have the line
/// spacing folded in and coincide with the face's own pair only while that
/// spacing is zero.
///
/// The four arrive together because a caller filling some of them gets
/// degenerate geometry rather than a diagnostic.
/// A face's own vertical metrics, for geometry defined against the type.
///
/// Escapable for the same reason as ``CellMetrics``: values only.
struct UBIQUITTY_ESCAPABLE FaceMetrics {
    /// The em size the face draws at, in device pixels.
    ///
    /// Fractional, because a reader may set a size between two points and the
    /// geometry measured against the type follows it.
    ///
    /// Zero means the rasterizer keeps no face of its own, which every consumer
    /// reads as "no type-relative geometry" rather than as a size of nothing —
    /// the prompt-mark pass draws nothing at a zero font size.
    double fontSize = 0.0;
    float ascent = 0.0f;
    float descent = 0.0f;
    /// The part of the line spacing that sits ABOVE the ascent.
    ///
    /// The spacing is split above and below rather than added below, so the
    /// glyph stays centred in its cell and every geometry measured from the
    /// baseline holds its distance whatever the spacing is. Adding it all below
    /// moves the text towards its own underline as the user increases the line
    /// height, which is the opposite of what that setting is for.
    ///
    /// The split is integer division, so an odd spacing puts the spare pixel
    /// below. Every rasterizer must round it the same way or two faces disagree
    /// by a pixel at odd spacings.
    float lineGapTopInPixels = 0.0f;
    /// How far the face's own `[` reaches above the baseline, in device pixels.
    ///
    /// The prompt mark is a bracket drawn in the gutter, and a face's vertical
    /// metrics say where its line fits while its bracket says where it frames a
    /// line of its own text. The two differ by enough to see: Monaspace Neon's
    /// `[` centres 6.7% of an em below its metrics' midpoint and Fira Code's
    /// 5.6% above, so a mark centred on the metrics stands a pixel or two off
    /// the brackets the user typed.
    float bracketInkTopInPixels = 0.0f;
    /// How far that `[` reaches below the baseline, in device pixels.
    ///
    /// Non-negative, like the top: both are distances from the baseline rather
    /// than coordinates, so neither carries the y direction of whoever measured
    /// it. Both zero means the face was not measured — a face with a `[` that
    /// has no ink at all does not exist — and the consumer centres on the
    /// ascent/descent midpoint instead.
    float bracketInkBottomInPixels = 0.0f;
};

/// Returns the side of the square a colour glyph occupies, in device pixels.
///
/// A colour master carries its own pixels rather than coverage, so it is drawn
/// rather than tinted and its aspect is part of the artwork: the destination
/// must be square or the emoji is compressed for a one-cell span and stretched
/// for a two-cell one. The largest square that fits the span the glyph occupies
/// is that square. The span is two cells because a colour glyph is East Asian
/// Wide, which is a fact about the base cell: a per-row scale reaching these
/// metrics would make the multiplier vary and this derivation would have to
/// take it as an argument rather than assume it.
///
/// The atlas slot is sized from this too, which is the point of naming it once.
/// A slot larger than the square it draws into makes every colour glyph a
/// RESAMPLE, and the resample is not neutral: the backends do not filter alike —
/// macOS samples the atlas linear, the web and the CPU compositor take the
/// nearest texel — so an emoji is smoothed on one and has whole rows dropped on
/// another. Equal, no filter is observable and the backends agree whatever each
/// one samples with.
///
/// Which of the two terms binds is a property of the face, not a constant. The
/// Unifont fallback (`Core/Rendering/UnifontRasterizer.h`) is 8 by 16, where
/// twice the width equals the height exactly, so the shipping default sits on
/// the boundary and either term gives the same answer there. A face
/// proportioned the other way puts the height inside the span and the height
/// decides. A caller that assumes the span term always wins has assumed a face;
/// the arithmetic below is the only place that answer is decided.
///
/// A whole number of device pixels, because an atlas slot is a whole number of
/// texels and the two must be the same number. A cell extent is fractional —
/// two cells of a 14.4px face is 28.8 — so a square left fractional is a slot of
/// 28 texels drawn across 28.8 pixels, which is a resample of a few percent
/// rather than none. The direction that cannot overflow is down: the square
/// fits inside the span it was measured from, and rounding up would put it a
/// texel past the cell.
///
/// - Parameter cellWidth: One cell's width in device pixels.
/// - Parameter cellHeight: One cell's height in device pixels.
/// - Returns: The square's side in whole device pixels; zero when either extent
///   is zero or negative.
[[nodiscard]] inline float colourGlyphSquareInPixels(float cellWidth, float cellHeight)
{
    if (cellWidth <= 0.0f || cellHeight <= 0.0f)
        return 0.0f;
    const float twoCells = 2.0f * cellWidth;
    const float square = twoCells < cellHeight ? twoCells : cellHeight;
    return std::floor(square);
}

} // namespace Ubiquitty::Rendering
