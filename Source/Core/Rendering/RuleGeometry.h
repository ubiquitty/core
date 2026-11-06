// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Core/Rendering/RuleGeometry.h — underline and strike placement.
//
// Two policies. The size-only one is a function of the type size and the
// display scale and of nothing else, for a backend whose face reports no
// metrics (a bitmap family, the web arm). The face policy takes what the font
// engine measured about the face the grid is set in — its underline weight,
// its descent and leading — and draws what AppKit draws for that face:
// measured over eight faces at thirty-three sizes and two scales, AppKit
// paints `NSUnderlineStyleSingle` as ceil(underlineThickness × scale) whole
// device rows (every sample; round fails a third of them), and places the
// rule so its top row is floor(0.48·(descent + leading) − t/2) below the
// baseline (98 of 132 samples exact, the rest one row off) — the rule splits
// the descender space, and `post.underlinePosition` is not consulted (Monaco's
// 0.9pt metric and Courier's 5.6pt land within a row of each other).
//
// The face is the PRIMARY face, measured once: a fallback glyph on the row
// never changes the rule's weight, which is the objection a per-glyph reading
// of the metric would have earned.
//
// A backend supplies what only a font engine can measure — the cell box, the
// baseline, the x-height the strike rides — and clamps the drop to its own cell
// height, which is not known here.
//

#include "Base/SwiftInterop.h"
#include "Core/Rendering/CellMetrics.h"

#include <algorithm>
#include <cmath>

namespace Ubiquitty::Rendering {

/// The rule geometry for one type size, in device pixels.
struct UBIQUITTY_ESCAPABLE RuleGeometry {
    /// Baseline to the rule centre.
    float underlineDrop = 1.0f;
    /// Solid-rule stroke weight.
    float underlineThickness = 1.0f;
    /// Curl weight. Carried apart from the solid stroke because the curl pass
    /// rounds it differently.
    float curlyThickness = 1.0f;
    /// Strike stroke. Tracks the underline, so a heavier rule reads heavier
    /// struck through.
    float strikeThickness = 1.0f;
};

/// Computes where the rules sit for a type size.
///
/// The drop's two terms carry different units and cannot substitute for each
/// other: the size-relative term is already device pixels, the constant is in
/// points and scales with the display. Passing 1 for `deviceScale` puts every
/// rule a pixel high on a HiDPI screen.
///
/// Results are fractional. The paint pass snaps edges to whole pixels, and
/// rounding here would snap twice.
///
/// - Parameters:
///   - fontSizeInDevicePixels: The em size, already multiplied by the display
///     scale.
///   - deviceScale: That display scale, needed on its own for the constant
///     term.
/// - Returns: The geometry, every stroke at least one device pixel — a
///   sub-pixel rule is one the paint pass can round away entirely.
///
///   One device pixel and not one point. A point floor reads as the
///   display-independent choice and is not: at 2x it binds for every size under
///   16pt, so 6pt and 14pt both come out at exactly two device pixels and the
///   rule stops tracking the type it sits under. The weight that reads light at
///   2x is lost to the whole-pixel snap in the builder, not to this floor, and
///   it is fixed where it is lost.
[[nodiscard]] inline RuleGeometry ruleGeometry(float fontSizeInDevicePixels, float deviceScale)
{
    const float size = fontSizeInDevicePixels > 0.0f ? fontSizeInDevicePixels : 1.0f;
    const float scale = deviceScale > 0.0f ? deviceScale : 1.0f;

    RuleGeometry geometry;
    geometry.underlineDrop = std::max(1.0f, size / 18.0f + scale);
    geometry.underlineThickness = std::max(1.0f, size / 16.0f);
    geometry.curlyThickness = geometry.underlineThickness;
    geometry.strikeThickness = geometry.underlineThickness;
    return geometry;
}

/// What a font engine measured about the face the grid is set in, in device
/// pixels, for the face rule policy.
///
/// Zero in a field means the engine could not answer it, and the size-only
/// policy stands in for that part of the geometry.
struct UBIQUITTY_ESCAPABLE RuleFaceMetrics {
    /// The face's underline weight (`post.underlineThickness`, or the engine's
    /// synthesis for a face without one), scaled to device pixels.
    float underlineThickness = 0.0f;
    /// The face's descent below the baseline.
    float descent = 0.0f;
    /// The face's line gap, which AppKit counts as part of the space the rule
    /// is centred in.
    float leading = 0.0f;
};

/// Computes where the rules sit for a face, the way AppKit places them.
///
/// The stroke is the face's own underline weight, floored at one device pixel;
/// the builder snaps it up to whole pixels, which is the ceil AppKit applies.
/// The drop is the offset the builder anchors a straight rule's top at
/// (`baseline + drop − strokeLift`), chosen so that top lands on
/// floor(0.48·(descent + leading) − t/2): the lift the builder subtracts is
/// added back here, and the curly and dotted rules, which take no lift, sit
/// that much lower than the straight one — under the line rather than through
/// its descenders.
///
/// - Parameters:
///   - fontSizeInDevicePixels: The em size, already multiplied by the display
///     scale; what the size-only policy falls back on.
///   - deviceScale: The display scale.
///   - face: The face's measurements; a zero field keeps the size-only answer
///     for that part.
/// - Returns: The geometry, every stroke at least one device pixel.
[[nodiscard]] inline RuleGeometry ruleGeometry(float fontSizeInDevicePixels, float deviceScale, const RuleFaceMetrics& face)
{
    RuleGeometry geometry = ruleGeometry(fontSizeInDevicePixels, deviceScale);
    if (face.underlineThickness > 0.0f) {
        geometry.underlineThickness = std::max(1.0f, face.underlineThickness);
        geometry.curlyThickness = geometry.underlineThickness;
        geometry.strikeThickness = geometry.underlineThickness;
    }
    if (face.descent > 0.0f) {
        const float snappedStroke = std::ceil(geometry.underlineThickness);
        const float centre = 0.48f * (face.descent + std::max(0.0f, face.leading));
        const float top = std::floor(centre - 0.5f * snappedStroke);
        geometry.underlineDrop = std::max(1.0f, top + underlineStrokeLiftForEm(fontSizeInDevicePixels));
    }
    return geometry;
}

/// Returns a rule's weight in whole device rows: the ceiling AppKit paints a
/// face's underline at, never under one row.
///
/// - Parameter thickness: The weight, in device pixels.
/// - Returns: The rows the rule takes.
[[nodiscard]] inline float snappedRuleWeight(float thickness)
{
    const float whole = std::ceil(thickness);
    return whole < 1.0f ? 1.0f : whole;
}

/// Returns the top row of a single straight underline.
///
/// Centred on the rule line, with a floor at the line itself: above it is the
/// type, where skip-ink erases whatever crosses a stem, so a rule thick
/// enough to climb hangs from the line rather than straddling it, and a
/// hairline straddles it untouched. Biased down by half a point so a
/// fractional centre cannot smear the stroke across two rows.
///
/// - Parameters:
///   - ruleLine: The underline position, baseline plus drop less the stroke
///     lift, in device pixels down the drawable.
///   - weight: The rule's weight, already snapped to whole rows.
///   - deviceScale: Device pixels per point.
/// - Returns: The rule's top row, a whole device pixel.
[[nodiscard]] inline float singleUnderlineTop(float ruleLine, float weight, float deviceScale)
{
    return std::max(std::round(ruleLine - weight * 0.5f + 0.5f * deviceScale), std::round(ruleLine));
}

/// How far skip-ink looks for ink about each pixel of an underline, and in
/// how many steps.
struct UBIQUITTY_ESCAPABLE SkipInkReach {
    /// Across, in device pixels: the full padding, which is what opens the
    /// gap around a descender.
    float horizontal = 0.0f;
    /// Up and down, in device pixels: half the padding. The rule is already
    /// thin and on its line, so ink that crosses it is caught without
    /// reaching far; a full reach upward lets every letter sitting on the
    /// baseline nibble the rule.
    float vertical = 0.0f;
    /// Taps from the centre to the edge of the reach along each axis.
    int steps = 1;
};

/// Returns the reach of skip-ink's ink test for a padding: an ellipse of
/// taps, one a device pixel of padding, between one and eight.
/// Nonpositive or nonfinite padding has no reach, as a grid rule does.
///
/// - Parameter padding: The skip-ink padding, in device pixels.
/// - Returns: The reach.
[[nodiscard]] inline SkipInkReach skipInkReach(float padding)
{
    if (!(padding > 0.0f) || !std::isfinite(padding))
        return {};
    return { .horizontal = padding, .vertical = padding * 0.5f, .steps = static_cast<int>(std::min(std::max(std::ceil(padding), 1.0f), 8.0f)) };
}

/// Returns the top row of a strike, which rides the x-height's midpoint.
///
/// - Parameters:
///   - baseline: The baseline, in device pixels down the drawable.
///   - xHeight: The face's x-height, in device pixels.
///   - weight: The strike's weight, already snapped to whole rows.
/// - Returns: The strike's top row, a whole device pixel.
[[nodiscard]] inline float strikeTop(float baseline, float xHeight, float weight) { return std::round(baseline - xHeight * 0.5f - weight * 0.5f); }

/// Returns the space between the two strokes of a doubled rule.
///
/// A doubled rule reads as two only while the space between its strokes stays
/// in proportion to the strokes themselves. A gap fixed in device pixels holds
/// steady as the weight grows, so a heavy pair closes up and prints as one bar
/// with a scratch through it — the thing a double rule exists to be
/// distinguishable from, lost at exactly the weight the setting was raised to
/// reach. Tying the gap to the stroke holds the proportion at every weight and
/// on every display.
///
/// Shared by every doubled rule rather than spelled per decoration: the double
/// underline and the SGR 61/63 ideogram lines are the same idea turned ninety
/// degrees, and two spellings of one spacing drift the first time either moves.
///
/// The floor is measured in points rather than device pixels, for the reason the
/// stroke floor is not: one device pixel at 2x is half the separation the same
/// rule shows at 1x, so a pixel floor closes the pair up on exactly the displays
/// that can afford to keep it open.
///
/// - Parameter strokeThickness: The weight of one stroke, in device pixels.
/// - Parameter deviceScale: Device pixels per point on the target display.
/// - Returns: The gap in device pixels, never below one point, so the pair
///   cannot merge into a single mark on any display.
[[nodiscard]] inline float doubledRuleGap(float strokeThickness, float deviceScale)
{
    const float onePoint = deviceScale > 0.0f ? deviceScale : 1.0f;
    return std::max(onePoint, strokeThickness);
}

} // namespace Ubiquitty::Rendering
