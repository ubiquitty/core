// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Rendering/GlyphKey.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace Ubiquitty::Rendering::Font {

enum class ScriptClass : std::uint8_t {
    Latin,
    Han,
    Kana,
    Hangul,
    Cyrillic,
    Greek,
    Arabic,
    Hebrew,
    Devanagari,
    Bengali,
    Gurmukhi,
    Thai,
    Lao,
    Khmer,
    Myanmar,
    Emoji,
    Symbols,
    Math,
    Other,
};

enum class Presentation : std::uint8_t {
    Default,
    Text,
    Emoji,
};

/// The OpenType `wght` axis value an SGR 1:n weight selects.
///
/// SGR carries a weight as 1..7 and a variable font wants a number on the axis,
/// so something has to map between them. That mapping is a property of the
/// protocol rather than of any backend: two frontends choosing different curves
/// render the same document at different weights, and nothing downstream can
/// attribute the difference to anything but the font.
///
/// Zero means the face's own weight and returns zero, which a backend reads as
/// "apply no variation" rather than as a valid axis value.
///
/// - Parameter sgrWeight: The SGR 1:n weight, 0 for the face's own.
/// - Returns: The `wght` axis value, or zero to apply none.
[[nodiscard]] constexpr float variationWeightForSGR(std::uint8_t sgrWeight)
{
    return sgrWeight == 0 ? 0.0f : 100.0f + 100.0f * static_cast<float>(sgrWeight);
}

/// The `wght` axis a configured weight asks for, or zero for the face's own.
///
/// Zero is the absence of a setting rather than a weight, and it has to stay
/// sayable: a face whose upright instance is not 400 would be moved by a
/// default that named a number, and a face with no `wght` axis cannot honour
/// one at all. Out-of-range values clamp to the axis rather than being
/// dropped, so a configuration written by hand lands somewhere legible.
///
/// - Parameter configuredWeight: The `font.weight` or `font.bold_weight` value.
/// - Returns: The axis value, or 0 to leave the face alone.
[[nodiscard]] constexpr float variationWeightForSetting(int configuredWeight)
{
    if (configuredWeight <= 0)
        return 0.0f;
    if (configuredWeight < 100)
        return 100.0f;
    if (configuredWeight > 900)
        return 900.0f;
    return static_cast<float>(configuredWeight);
}

/// The `wght` coordinate a face's own bold member sits at.
///
/// `font.bold_weight` defaults to zero, meaning "the face's own bold". A static
/// family answers that by having a Bold member; a variable face has no members
/// to fall back to, so an arm that places the axis itself must name the number
/// or bold text draws at the default coordinate — which is the upright weight,
/// making bold indistinguishable from regular rather than merely wrong.
///
/// Every arm names this number rather than asking the platform for its bold:
/// CoreText's bold trait on macOS 27 reaches Bold only from the Regular
/// instance of a variable face and answers any other instance with itself, so
/// a bold asked for by trait over a light upright is the upright again.
inline constexpr int faceOwnBoldWeight = 700;

/// The `wght` coordinate upright text sits at when `font.weight` is unset:
/// the upright weight in the units CSS and `usWeightClass` share.
///
/// Named rather than left to the face's own coordinate, because the platform
/// does not agree with itself about that coordinate: CoreText resolved
/// "Monaspace Neon Var" to its ExtraLight default instance on macOS 26 and to
/// its Regular instance on macOS 27, and FreeType places the axis default,
/// which for that face is ExtraLight too.
inline constexpr int uprightWeight = 400;

/// The reader's weight settings and the body's supported `wght` variation.
///
/// Named weights use zero for unset; the variation uses a non-finite value.
/// A backend leaves the variation unset when the face has no weight axis, so
/// an unsupported variation never changes a static face.
struct WeightSettings {
    int upright = 0; ///< `font.weight`.
    int bold = 0; ///< `font.bold_weight`.
    double uprightVariation = std::numeric_limits<double>::quiet_NaN(); ///< The supported `wght` from `font.variations`.
};

/// The `wght` a cell is drawn at, from the running program and the reader's
/// settings.
///
/// Four tiers in one order. An SGR 1:n weight is the running program naming
/// the weight of one run and outranks the rest. Otherwise every face but the
/// bold ones takes the supported `wght` variation, then `font.weight`, or
/// ``uprightWeight`` when neither is set, and
/// the bold faces take `font.bold_weight`, or the heavier of
/// ``faceOwnBoldWeight`` and the upright weight when that is unset.
///
/// The unset bold is never lighter than the effective upright weight. A bold
/// weight the reader set is drawn as written, below the upright too. Variation
/// coordinates retain their fractional precision and clamp at the backend's
/// supported range, just as the body's variation does.
///
/// Every arm asks here for the number and only applies it: on a `wght` axis,
/// clamped to the range the face offers, or by picking the nearest member of a
/// static family. A number any arm leaves to its platform is a number the arms
/// disagree on, and one configuration then draws one weight on macOS and
/// another on Linux with nothing downstream to attribute the difference to.
///
/// - Parameters:
///   - face: The cell's face.
///   - sgrWeight: The SGR 1:n weight, 0 for none.
///   - settings: The reader's weights, including the supported body variation.
/// - Returns: The `wght` value, before the backend clamps it to its range.
[[nodiscard]] constexpr double weightForFace(FontFace face, std::uint8_t sgrWeight, WeightSettings settings)
{
    const float requested = variationWeightForSGR(sgrWeight);
    if (requested > 0.0f)
        return requested;
    const float configuredUpright = variationWeightForSetting(settings.upright);
    const double upright = std::isfinite(settings.uprightVariation) ? settings.uprightVariation
        : configuredUpright > 0.0f                                  ? configuredUpright
                                                                    : uprightWeight;
    if (!isBold(face))
        return upright;
    const float configuredBold = variationWeightForSetting(settings.bold);
    if (configuredBold > 0.0f)
        return configuredBold;
    return upright > faceOwnBoldWeight ? upright : faceOwnBoldWeight;
}

/// The width percent every face is drawn at: `font.width`, or normal when it
/// is unset.
///
/// Percent of normal, the unit of the `wdth` axis by specification; a static
/// family reaches it through ``normalisedWidthTraitForSetting``.
///
/// - Parameter configuredWidthPercent: `font.width`, 0 when unset.
/// - Returns: The width percent.
[[nodiscard]] constexpr double widthForSetting(double configuredWidthPercent)
{
    return configuredWidthPercent > 0.0 ? configuredWidthPercent : 100.0;
}

/// The normalised weight trait a configured weight asks for.
///
/// Two kinds of family answer a weight setting, and only one of them has a
/// `wght` axis: a Nerd Font build ships static instances (measured — Monaspace
/// Neon NF reports zero variation axes, and its members resolve as
/// ExtraLight through ExtraBold). A trait reaches both, because the matcher
/// picks the family member nearest the value and a variable face applies its
/// axis, so this is the mapping a frontend selects a face with.
///
/// The scale is the one CoreText and AppKit share, -1 through 1 with 0 at
/// regular and 0.4 at bold, and its stops are the published `NSFont.Weight`
/// values rather than an even spread — the gap from regular to medium is a
/// quarter of the gap from light to regular. A value between two stops
/// interpolates, so a hand-written 450 lands between regular and medium
/// instead of snapping.
///
/// Only meaningful for a weight ``variationWeightForSetting`` reports as set;
/// zero here is regular, which is a weight rather than the absence of one.
///
/// - Parameter configuredWeight: The `font.weight` or `font.bold_weight` value.
/// - Returns: The normalised trait, -1 through 1.
[[nodiscard]] constexpr float normalisedWeightTraitForSetting(int configuredWeight)
{
    // The published stops, in weight order.
    constexpr int stopWeights[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900 };
    constexpr float stopTraits[] = { -0.80f, -0.60f, -0.40f, 0.0f, 0.23f, 0.30f, 0.40f, 0.56f, 0.62f };
    constexpr int stopCount = 9;
    if (configuredWeight <= stopWeights[0])
        return stopTraits[0];
    if (configuredWeight >= stopWeights[stopCount - 1])
        return stopTraits[stopCount - 1];
    for (int i = 1; i < stopCount; ++i) {
        if (configuredWeight > stopWeights[i])
            continue;
        const float span = static_cast<float>(stopWeights[i] - stopWeights[i - 1]);
        const float into = static_cast<float>(configuredWeight - stopWeights[i - 1]);
        return stopTraits[i - 1] + (stopTraits[i] - stopTraits[i - 1]) * (into / span);
    }
    return stopTraits[stopCount - 1];
}

/// The `font.weight` setting a normalised weight trait names: the inverse
/// of ``normalisedWeightTraitForSetting``, for a static family's members.
///
/// A member's trait is what the platform reports for it; the nearest
/// published stop is the setting that routes back to that member, so a
/// picker built from the family's members offers values the route honours.
/// Traits beyond the stops clamp to the end stops.
///
/// - Parameter trait: The normalised weight trait, -1 through 1.
/// - Returns: The setting, one of the published stops.
[[nodiscard]] constexpr int settingForNormalisedWeightTrait(float trait)
{
    constexpr int stopWeights[] = { 100, 200, 300, 400, 500, 600, 700, 800, 900 };
    constexpr float stopTraits[] = { -0.80f, -0.60f, -0.40f, 0.0f, 0.23f, 0.30f, 0.40f, 0.56f, 0.62f };
    constexpr int stopCount = 9;
    int nearest = 0;
    float nearestDistance = trait > stopTraits[0] ? trait - stopTraits[0] : stopTraits[0] - trait;
    for (int i = 1; i < stopCount; ++i) {
        const float distance = trait > stopTraits[i] ? trait - stopTraits[i] : stopTraits[i] - trait;
        if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = i;
        }
    }
    return stopWeights[nearest];
}

/// The fractional OS/2 width class a width percent names.
///
/// The anchors are the OS/2 table's own (OpenType `usWidthClass` 1..9 at
/// 50, 62.5, 75, 87.5, 100, 112.5, 125, 150 and 200 percent — the same scale
/// CSS `font-stretch` and the `wdth` variation axis use, where the axis value
/// is percent of normal by definition). A percent between two anchors
/// interpolates the class fractionally, so a matcher fed the result prefers
/// the nearer member instead of snapping to a table row.
///
/// - Parameter widthPercent: The width, 50 through 200.
/// - Returns: The class, 1 through 9, fractional between anchors.
[[nodiscard]] constexpr double widthClassFromPercent(double widthPercent)
{
    constexpr double anchors[] = { 50.0, 62.5, 75.0, 87.5, 100.0, 112.5, 125.0, 150.0, 200.0 };
    if (widthPercent <= anchors[0])
        return 1.0;
    for (int upper = 1; upper < 9; ++upper) {
        if (widthPercent > anchors[upper])
            continue;
        const double span = anchors[upper] - anchors[upper - 1];
        return static_cast<double>(upper) + (widthPercent - anchors[upper - 1]) / span;
    }
    return 9.0;
}

/// The normalised CoreText width trait a configured width asks for.
///
/// The trait convention is `(usWidthClass - 5) / 10`, verifiable against the
/// system faces — SF Condensed carries class 3 and reports -0.2, SF
/// Compressed class 2 and -0.3, SF Expanded class 7 and 0.2 — so a value
/// derived any other way matches a different member than the class names.
/// Zero is the absence of a setting, as for the weights: the mapping is only
/// meaningful for a percent the configuration actually set.
///
/// A variable face with a `wdth` axis takes the percent on the axis directly
/// instead — the axis is percent-of-normal by specification — and this trait
/// serves the static families, whose width members the matcher resolves.
///
/// - Parameter widthPercent: The `font.width` value, 50 through 200.
/// - Returns: The normalised trait, -0.4 through 0.4.
[[nodiscard]] constexpr float normalisedWidthTraitForSetting(double widthPercent)
{
    return static_cast<float>((widthClassFromPercent(widthPercent) - 5.0) / 10.0);
}

enum class FontStyle : std::uint8_t {
    Regular,
    Bold,
    Italic,
    BoldItalic,
};

enum class CellWidthClass : std::uint8_t {
    Narrow,
    Wide,
    Zero,
    Control,
    Cluster,
};

enum class FontSource : std::uint8_t {
    Primary,
    RangeMap,
    ExplicitHan,
    AutoHanCascade,
    SymbolFallback,
    EmojiCascade,
    ScriptFallback,
    GenericCache,
    NativeCascade,
    Missing,
};

enum class RouteFlag : std::uint16_t {
    None = 0,
    MonoAtlas = 1u << 0,
    WideAtlas = 1u << 1,
    ColorAtlas = 1u << 2,
    LigatureAtlas = 1u << 3,
    DoubleAtlas = 1u << 4,
    SymbolPath = 1u << 5,
    ProceduralPath = 1u << 6,
    ShapedRun = 1u << 7,
    MissingGlyph = 1u << 8,
};

using RouteFlags = std::uint16_t;

constexpr RouteFlags routeFlag(RouteFlag flag) { return static_cast<RouteFlags>(flag); }

constexpr RouteFlags operator|(RouteFlag left, RouteFlag right) { return routeFlag(left) | routeFlag(right); }

constexpr RouteFlags operator|(RouteFlags left, RouteFlag right) { return left | routeFlag(right); }

constexpr bool hasRouteFlag(RouteFlags flags, RouteFlag flag) { return (flags & routeFlag(flag)) != 0; }

struct LanguageTag {
    std::string_view value;
};

struct FontHandle {
    std::uint32_t value { 0 };
    std::uint32_t generation { 0 };

    [[nodiscard]] constexpr bool isValid() const { return value != 0; }
    constexpr bool operator==(const FontHandle&) const = default;
};

struct GlyphID {
    std::uint32_t value { 0 };

    [[nodiscard]] constexpr bool isValid() const { return value != 0; }
};

struct FontRequest {
    std::span<const std::uint32_t> codePoints;
    ScriptClass script { ScriptClass::Other };
    Presentation presentation { Presentation::Default };
    FontStyle style { FontStyle::Regular };
    std::uint8_t variableWeight { 0 };
    CellWidthClass width { CellWidthClass::Narrow };
    LanguageTag language;
    std::uint32_t preferenceGeneration { 0 };
};

struct GlyphRoute {
    FontSource source { FontSource::Missing };
    FontHandle font;
    GlyphID glyph;
    RouteFlags flags { routeFlag(RouteFlag::MissingGlyph) };
    ScriptClass script { ScriptClass::Other };
    CellWidthClass width { CellWidthClass::Narrow };

    constexpr explicit operator bool() const { return source != FontSource::Missing; }
};

constexpr bool isMissing(const GlyphRoute& route) { return route.source == FontSource::Missing || hasRouteFlag(route.flags, RouteFlag::MissingGlyph); }

} // namespace Ubiquitty::Rendering::Font
