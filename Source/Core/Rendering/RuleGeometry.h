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
// the descender space. Ubiquitty's own rule sits a clear gap below the
// baseline instead (underlineGapInWeights), or lower where the face declares
// a lower one (`post.underlinePosition`); `[underline] metrics = "font"` draws
// the declared rule itself, as TextEdit does (declaredUnderlineRows).
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
#include <cstdint>

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
    /// The top of the face's declared underline, below the baseline; zero
    /// when the face declares none (RuleFaceMetrics::underlinePosition).
    float fontUnderlineTop = 0.0f;
    /// The face's declared underline thickness; zero when the face declares
    /// no position, so a font rule with no place to stand falls back to ours.
    float fontUnderlineThickness = 0.0f;
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
    /// The face's declared underline position: the distance from the baseline
    /// down to the TOP of the rule, positive down, as `post.underlinePosition`
    /// states it negated, in device pixels. Zero when the engine cannot read
    /// one; a rule declared at or above the baseline counts as none.
    ///
    /// FreeType reports the centre of the stem rather than its top, so its
    /// engine adds half the thickness back before it fills this.
    float underlinePosition = 0.0f;
    /// The weight the face's own underscore draws at, as the engine's glyph
    /// raster draws it: the coverage down a column through its middle.
    ///
    /// The face's rule as its designer drew it, and the weight its strokes are
    /// seen at: a declared underline thickness is a legacy number, half the
    /// stem in most faces, while the underscore matches the horizontals of the
    /// letters around it. Measured from the raster rather than the outline, so
    /// the stem darkening the engine draws text with is in it too.
    float underscoreWeight = 0.0f;
    /// The face's descent below the baseline.
    float descent = 0.0f;
    /// The face's line gap, which AppKit counts as part of the space the rule
    /// is centred in.
    float leading = 0.0f;
};

/// Returns the weight a face's own underscore draws at, read from its raster:
/// the coverage summed down each column across the middle third of its ink,
/// averaged, in the raster's pixels.
///
/// The one reading every font engine's underscore goes through, so a face
/// draws its rules at the same weight whichever engine set it. The middle
/// third, so ends a face rounds or tapers do not thin the bar, and a bar that
/// slants or swells is read at its typical weight rather than at one column.
///
/// - Parameters:
///   - coverage: `width * height` coverage bytes, row-major.
///   - width: The raster's width in pixels.
///   - height: The raster's height in pixels.
/// - Returns: The weight, or zero for a raster with no ink.
[[nodiscard]] inline float measuredUnderscoreWeight(const std::uint8_t* coverage, int width, int height)
{
    if (!coverage || width <= 0 || height <= 0)
        return 0.0f;
    const auto columnSum = [&](int column) {
        int sum = 0;
        for (int row = 0; row < height; ++row)
            sum += coverage[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column)];
        return sum;
    };
    int first = -1;
    int last = -1;
    for (int column = 0; column < width; ++column) {
        if (columnSum(column) > 0) {
            if (first < 0)
                first = column;
            last = column;
        }
    }
    if (first < 0)
        return 0.0f;
    const int span = last - first + 1;
    const int from = first + span / 3;
    const int to = std::max(from + 1, last + 1 - span / 3);
    long total = 0;
    for (int column = from; column < to; ++column)
        total += columnSum(column);
    return static_cast<float>(total) / (255.0f * static_cast<float>(to - from));
}

/// How far below the baseline a rule's top sits at least, in the rule's own
/// weights.
///
/// Faces declare an underline position, and the declarations disagree: one
/// face's rule all but touches its letters and another's floats a stroke and a
/// half below them. A gap a little under the rule's weight reads as one rhythm
/// in every face, letter, space, rule, and keeps a heavier rule from crowding
/// the text it underlines. The rule sits the larger of this gap and the face's
/// declared position below the baseline (underlineTopNoHigherThanDeclared), so a
/// face that asks for more room gets it and one that crowds its letters does
/// not.
inline constexpr float underlineGapInWeights = 0.85f;

/// Returns a rule's weight as drawn: the nearest whole number of device rows,
/// never under one.
///
/// A rule's top sits on the pixel grid, and a weight that leaves part of a row
/// draws that row faint. Beside the solid rows it reads as blur rather than
/// weight, and the rule looks heavier and softer than the letters it sits
/// under. AppKit draws an underline in whole device pixels for the same reason:
/// TextEdit sets SF Mono at 12 points on a 2x display with a crisp 2-pixel
/// rule, where the exact underscore weight, 1.72, drew as a solid row and a
/// 72% one.
///
/// - Parameter thickness: The weight, in device pixels.
/// - Returns: The weight to draw.
[[nodiscard]] inline float ruleWeight(float thickness)
{
    return std::max(1.0f, std::round(thickness));
}

/// Computes where the rules sit for a face.
///
/// The stroke is the face's own rule: its underscore as the engine draws it,
/// or the declared underline weight where the engine could not measure one,
/// in whole device pixels and at least one (ruleWeight), so the gap below is
/// measured from the weight the builder draws.
/// The drop is the offset the builder anchors a straight rule's top at
/// (`baseline + drop − strokeLift`), chosen so that top lands a clear gap below
/// the baseline (underlineGapInWeights): the lift the builder subtracts is
/// added back here. The builder then takes the larger of that top and the
/// face's declared one (underlineTopNoHigherThanDeclared), which needs the cell's
/// baseline and so is not folded into the drop: the rule sits the larger of
/// 0.85 of its weight and the face's declared position below the baseline.
/// The declared position and thickness are passed through for that and for
/// `[underline] metrics = "font"`, both zero when the face declares no
/// position.
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
    const float faceWeight = face.underscoreWeight > 0.0f ? face.underscoreWeight : face.underlineThickness;
    if (faceWeight > 0.0f) {
        geometry.underlineThickness = ruleWeight(faceWeight);
        geometry.curlyThickness = geometry.underlineThickness;
        geometry.strikeThickness = geometry.underlineThickness;
    }
    if (face.descent > 0.0f) {
        // A clear gap below the baseline a little under the rule's own weight
        // (underlineGapInWeights), whole rows of it. The lift the builder
        // subtracts is added back here.
        const float top = std::max(1.0f, std::round(underlineGapInWeights * geometry.underlineThickness));
        geometry.underlineDrop = std::max(1.0f, top + underlineStrokeLiftForEm(fontSizeInDevicePixels));
    }
    if (face.underlinePosition > 0.0f && std::isfinite(face.underlinePosition)) {
        geometry.fontUnderlineTop = face.underlinePosition;
        geometry.fontUnderlineThickness = face.underlineThickness > 0.0f && std::isfinite(face.underlineThickness) ? face.underlineThickness : 0.0f;
    }
    return geometry;
}

/// How far inside a whole device row a declared edge may fall and still count
/// as on it, in device pixels.
///
/// A face's declared underline lands on the grid exactly at some sizes, two
/// points at 1x for a face declaring a tenth of an em, and the float sum of the
/// baseline and the position can miss that row by a rounding error. The rule
/// would then gain a whole row of weight, or drop a row, for a difference no
/// one could see.
inline constexpr float declaredRuleEdgeTolerance = 1.0f / 64.0f;

/// A rule's place on the device grid.
struct UBIQUITTY_ESCAPABLE RuleRows {
    /// The rule's top, a whole device row down the drawable.
    float top = 0.0f;
    /// The rule's weight, a whole number of rows, at least one.
    float weight = 1.0f;
};

/// Returns the baseline the glyphs stand on: the cell's baseline on the
/// nearest device row.
///
/// The atlas draws every glyph from a pen rounded to a whole row, so a face
/// whose layout puts the baseline 0.4 of a row lower than its glyphs stand
/// would set a rule declared 3.72 rows down from the layout baseline a row
/// lower than AppKit sets it from the glyphs' (JetBrains Mono at 12 points on
/// a 2x display).
///
/// - Parameter baselineY: The cell's layout baseline, in device pixels down
///   the drawable.
/// - Returns: The row boundary the glyphs' baseline lands on.
[[nodiscard]] inline float glyphBaseline(float baselineY) { return std::round(baselineY); }

/// Returns the top row of the face's declared underline, snapped down to the
/// row it starts in, measured from the baseline the glyphs stand on.
///
/// `underline_offset` moves it as it moves the drop, so the offset reads the
/// same whichever rule ends up lower.
///
/// - Parameters:
///   - metrics: The frame's cell metrics, carrying the declared top and the
///     offset.
///   - baselineY: The cell's baseline, in device pixels down the drawable.
/// - Returns: The row; meaningful only where the face declares a position.
[[nodiscard]] inline float declaredUnderlineTop(const CellMetrics& metrics, float baselineY)
{
    return std::floor(glyphBaseline(baselineY) + metrics.fontUnderlineTop + metrics.underlineOffset + declaredRuleEdgeTolerance);
}

/// Returns an underline top moved down to the face's declared one where that
/// is lower: the rule sits the larger of its own clear gap and the face's
/// declared position below the baseline.
///
/// A face that declares a roomy underline, JetBrains Mono's 3.72 pixels at 12
/// points on a 2x display, gets its three clear rows under the stems as
/// TextEdit draws them; one that declares a rule crowding its letters,
/// Monaspace Neon's 0.84, keeps the gap. The weight is not the face's: it stays
/// the underscore's (ruleWeight).
///
/// - Parameters:
///   - top: The rule's own top row (singleUnderlineTop, or a pair's upper rule).
///   - metrics: The frame's cell metrics, carrying the declared top and the
///     offset.
///   - baselineY: The cell's baseline, in device pixels down the drawable.
/// - Returns: The lower of the two rows, the one further from the text, or
///   `top` for a face that declares no position.
[[nodiscard]] inline float underlineTopNoHigherThanDeclared(float top, const CellMetrics& metrics, float baselineY)
{
    return metrics.fontUnderlineTop > 0.0f ? std::max(top, declaredUnderlineTop(metrics, baselineY)) : top;
}

/// Whether a cell's underlines take the face's declared rule
/// (`[underline] metrics = "font"`), which needs both its position and its
/// thickness; a face missing either keeps the ubiquitty rule.
///
/// - Parameter metrics: The frame's cell metrics.
/// - Returns: Whether the declared rule stands in for ours.
[[nodiscard]] inline bool usesDeclaredUnderline(const CellMetrics& metrics)
{
    return metrics.underlineMetrics == RuleMetricsSource::Font && metrics.fontUnderlineTop > 0.0f && metrics.fontUnderlineThickness > 0.0f;
}

/// Returns the face's declared underline on the device grid, as AppKit draws
/// it: the rectangle from `baseline + position` down by the thickness, snapped
/// outward to whole rows.
///
/// TextEdit draws SF Mono at 12 points on a 2x display, declaring 2.34 pixels
/// down and 1.17 thick, on rows 2 and 3 below the baseline; JetBrains Mono,
/// declaring 3.72 and 1.20, on rows 3 and 4. `underline_offset` moves the
/// rectangle and `[underline] thickness` has already scaled its thickness
/// (applyRuleStyle).
///
/// - Parameters:
///   - metrics: The frame's cell metrics, carrying the declared rule and the
///     offset.
///   - baselineY: The cell's baseline, in device pixels down the drawable.
/// - Returns: The rule's top row and its weight in whole rows, at least one.
[[nodiscard]] inline RuleRows declaredUnderlineRows(const CellMetrics& metrics, float baselineY)
{
    const float top = glyphBaseline(baselineY) + metrics.fontUnderlineTop + metrics.underlineOffset;
    const float upper = std::floor(top + declaredRuleEdgeTolerance);
    const float lower = std::ceil(top + metrics.fontUnderlineThickness - declaredRuleEdgeTolerance);
    return { .top = upper, .weight = std::max(1.0f, lower - upper) };
}

/// Returns the top of the second rule of a pair: the whole row nearest the gap
/// below the first, so it starts on the pixel grid as the first does, and never
/// without a clear row between them.
///
/// - Parameters:
///   - top: The first rule's top, on the pixel grid.
///   - weight: The first rule's weight.
///   - gap: The gap the pair asks for.
/// - Returns: The second rule's top.
[[nodiscard]] inline float pairedRuleTop(float top, float weight, float gap)
{
    return std::max(std::round(top + weight + gap), std::ceil(top + weight) + 1.0f);
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

/// Returns the single straight underline's top row and weight for a cell,
/// from whichever source `[underline] metrics` names.
///
/// Every underline shape is laid from these two, so they move together. The
/// ubiquitty rule is the underscore's weight in whole rows (ruleWeight), its
/// top the lower of singleUnderlineTop and the face's declared top
/// (underlineTopNoHigherThanDeclared); the font rule is the face's declared one
/// (declaredUnderlineRows), where the face declares one.
///
/// - Parameters:
///   - metrics: The frame's cell metrics.
///   - baselineY: The cell's baseline, in device pixels down the drawable.
///   - ruleLine: The ubiquitty rule line, baseline plus drop less the stroke
///     lift, in device pixels down the drawable.
///   - thickness: The ubiquitty weight before it is snapped: the bold or the
///     regular rule's.
/// - Returns: The rule's top row and weight; the top not yet held inside the
///   cell.
[[nodiscard]] inline RuleRows singleUnderlineRows(const CellMetrics& metrics, float baselineY, float ruleLine, float thickness)
{
    if (usesDeclaredUnderline(metrics))
        return declaredUnderlineRows(metrics, baselineY);
    const float weight = ruleWeight(thickness);
    return { .top = underlineTopNoHigherThanDeclared(singleUnderlineTop(ruleLine, weight, metrics.deviceScale), metrics, baselineY), .weight = weight };
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
/// - Returns: The gap in device pixels: the share of the stroke the rule keeps
///   from the baseline above it (underlineGapInWeights), so the pair's two
///   spaces read as one rhythm, and never below one point, so the pair cannot
///   merge into a single mark on any display.
[[nodiscard]] inline float doubledRuleGap(float strokeThickness, float deviceScale)
{
    const float onePoint = deviceScale > 0.0f ? deviceScale : 1.0f;
    return std::max(onePoint, underlineGapInWeights * strokeThickness);
}

/// A curly rule's circular arcs, as every renderer draws them: half a
/// wavelength across each, alternately cresting toward the text and dipping
/// away from it, meeting on the band's centre line with matching tangents.
struct UBIQUITTY_ESCAPABLE CurlArcs {
    /// One arc's width: half a wavelength.
    float span = 0.0f;
    /// The radius of each arc's circle.
    float radius = 0.0f;
    /// How far an arc swings from the centre line.
    float swing = 0.0f;
    /// The centre line, down the band from its top.
    float middle = 0.0f;
};

/// Returns the arcs of a curl repeating every `wavelength` in a band
/// `bandHeight` tall, its stroke `thickness` wide.
[[nodiscard]] inline CurlArcs curlArcs(float wavelength, float bandHeight, float thickness)
{
    const float span = std::max(wavelength, 2.0f) * 0.5f;
    const float swing = std::max(bandHeight * 0.5f - thickness * 0.5f - 0.5f, 0.25f);
    return { .span = span, .radius = (span * span * 0.25f + swing * swing) / (2.0f * swing), .swing = swing, .middle = bandHeight * 0.5f };
}

/// Returns where a curl's stroke is centred at `localX` across its band,
/// down from the band's top.
[[nodiscard]] inline float curlCentreY(const CurlArcs& arcs, float localX)
{
    const float arc = std::floor(localX / arcs.span);
    // Even arcs crest toward the text, odd ones dip away from it.
    const float side = arc - 2.0f * std::floor(arc * 0.5f) < 0.5f ? -1.0f : 1.0f;
    const float deltaX = localX - (arc * arcs.span + arcs.span * 0.5f);
    const float circleY = arcs.middle - side * (arcs.radius - arcs.swing);
    return circleY + side * std::sqrt(std::max(arcs.radius * arcs.radius - deltaX * deltaX, 0.0f));
}

} // namespace Ubiquitty::Rendering
