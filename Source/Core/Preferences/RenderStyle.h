// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/Core/Preferences/RenderStyle.h — a renderer's configuration, as the
// style a frame is drawn with.
//
// The fields are points in a RenderConfiguration and device pixels in a frame
// style, and the conversion is one multiplication per field. Written per
// frontend it is one assignment per field per frontend, and an omitted
// assignment leaves the field at its neutral default: the setting has no
// effect, the frame is otherwise correct, and nothing reports it.
// `underline.offset` reached only macOS that way, and a Linux render of
// `offset = 6, thickness = 3.0` was byte-identical to one that named neither.
//
// Several functions rather than one because the fields have several consumers.
// The rules, the cursor, the bell and the tube are read by the frame capture,
// which every renderer runs. The spacing and padding are read by
// renderTerminalSnapshotPNG, which lays a rendered grid on a page — a renderer
// that composes its own image reads neither, so setting them there would be a
// call that cannot fail and cannot work.
//

#include "Core/Attributes/LinkRun.h"
#include "Core/CellGrid.h"
#include "Core/Preferences/PreferencesAccessors.h"
#include "Core/Preferences/RenderConfigurationFold.h"
#include "Core/Rendering/RenderConfiguration.h"
#include "Core/Rendering/TerminalSnapshot.h"

#include <cmath>
#include <cstdint>

namespace Ubiquitty::Preferences::Effective {

/// Extra rows of space between lines, in device pixels.
///
/// Whole pixels: the cell height a rasterizer is built with is an integer, and
/// a frontend that rounded differently from this would raster its glyphs at one
/// height and lay its rows out at another.
///
/// - Parameters:
///   - configuration: What the configuration resolved to.
///   - deviceScale: Device pixels per point.
/// - Returns: The spacing to build both the rasterizer and the style with.
[[nodiscard]] inline int lineSpacingInPixels(const Rendering::RenderConfiguration& configuration, double deviceScale)
{
    return static_cast<int>(std::lround(configuration.lineSpacingPoints * deviceScale));
}

/// Converts character spacing to the bitmap cell's integer pixel lattice.
///
/// - Parameters:
///   - configuration: The resolved font configuration.
///   - deviceScale: Device pixels per point.
/// - Returns: The horizontal spacing shared by the rasterizer and frame style.
[[nodiscard]] inline int characterSpacingInPixels(const Rendering::RenderConfiguration& configuration, double deviceScale)
{
    return static_cast<int>(std::lround(configuration.characterSpacingPoints * deviceScale));
}

/// Fills the styling the frame capture reads.
///
/// The underline and strike weights, bold brightening, and the braille and
/// selection routes beside them: every renderer runs the capture, so a field
/// filled here reaches all of them, which is the property that puts them in
/// one function rather than the subject they share.
///
/// - Parameters:
///   - style: Filled in place. Fields the configuration does not name are left
///     alone.
///   - configuration: What the configuration resolved to.
///   - deviceScale: Device pixels per point.
inline void applyRuleSettings(Rendering::TerminalSnapshotStyle& style, const Rendering::RenderConfiguration& configuration, double deviceScale)
{
    // A multiplier on the thickness the metric derives, so it carries no scale.
    style.underlineThicknessScale = static_cast<float>(configuration.underlineThicknessScale);
    style.underlineOffsetInPixels = static_cast<float>(configuration.underlineOffsetPoints * deviceScale);
    style.shouldUnderlineSkipInk = configuration.shouldUnderlineSkipInk;
    style.shouldUnderlineFollowInk = configuration.shouldUnderlineFollowInk;
    style.underlineSkipInkPaddingInPixels = static_cast<float>(configuration.underlineSkipInkPaddingPoints * deviceScale);
    style.shouldBrightenBold = configuration.shouldBrightenBold;
    style.shouldDrawBrailleGrid = configuration.shouldDrawBrailleGrid;
    style.brailleStyle = configuration.brailleStyle;
    style.shadeStyle = configuration.shadeStyle;
    style.selectionStyle = configuration.selectionStyle;
}

/// Fills the line spacing and the page insets a configuration names.
///
/// Read by renderTerminalSnapshotPNG, which grows the image around the grid it
/// rendered. A caller drawing into a surface it sized itself insets that
/// surface instead, and these fields would do nothing for it.
///
/// - Parameters:
///   - style: Filled in place. Fields the configuration does not name are left
///     alone.
///   - configuration: What the configuration resolved to.
///   - deviceScale: Device pixels per point.
inline void applyPageSettings(Rendering::TerminalSnapshotStyle& style, const Rendering::RenderConfiguration& configuration, double deviceScale)
{
    style.lineSpacingInPixels = lineSpacingInPixels(configuration, deviceScale);
    style.characterSpacingInPixels = characterSpacingInPixels(configuration, deviceScale);
    style.paddingInPixels = {
        static_cast<float>(configuration.paddingLeftPoints * deviceScale),
        static_cast<float>(configuration.paddingRightPoints * deviceScale),
        static_cast<float>(configuration.paddingTopPoints * deviceScale),
        static_cast<float>(configuration.paddingBottomPoints * deviceScale),
    };
}

/// Fills the cursor styling a configuration names.
///
/// Read through the frame capture, like the rules. `shouldCursorInvertText`
/// defaults to true in the options struct and `cursorAlpha` to solid, so a
/// frontend that never sets them draws a contrasting glyph under an opaque
/// block on every machine whose reader asked otherwise.
///
/// - Parameters:
///   - options: Filled in place.
///   - configuration: What the configuration resolved to.
inline void applyCursorSettings(Rendering::TerminalSnapshotOptions& options, const Rendering::RenderConfiguration& configuration)
{
    options.shouldCursorInvertText = configuration.shouldCursorInvertText;
    options.cursorAlpha = static_cast<float>(configuration.cursorAlpha);
}

/// The bell flash's strength at a tick, under a configuration.
///
/// The envelope is Core's (`bellFlashAlpha`); the peak and whether the bell
/// flashes at all are the configuration's. A frontend with a clock passes the
/// tick the flash started and the tick it is drawing; a still frame drawn as
/// the bell rings passes the same tick for both and draws the peak.
///
/// - Parameters:
///   - configuration: What the configuration resolved to.
///   - startTick: When the flash started; zero for none.
///   - nowTicks: The tick the frame is drawn at.
/// - Returns: The flash's alpha, zero when the bell does not flash.
[[nodiscard]] inline float bellFlashAlpha(const Rendering::RenderConfiguration& configuration, std::uint32_t startTick, std::uint32_t nowTicks)
{
    if (!configuration.isBellVisual)
        return 0.0f;
    return Ubiquitty::bellFlashAlpha(startTick, nowTicks, static_cast<float>(configuration.bellFlashPeakAlpha));
}

/// Fills the frame's background image from the configuration.
///
/// The image is the caller's decode of `[appearance] background_image`, which
/// it loads when the path changes; the mode and the opacity are the fold's, so
/// every software arm places and fades the image by the same settings.
///
/// - Parameters:
///   - options: Filled in place.
///   - configuration: What the configuration resolved to.
///   - pixels: The decoded image, borrowed for the frame; null or empty draws none.
inline void applyBackgroundImageSettings(Rendering::TerminalSnapshotOptions& options, const Rendering::RenderConfiguration& configuration,
    const Rendering::BackgroundImagePixels* pixels)
{
    options.backgroundImage.pixels = (pixels && !pixels->isEmpty() && !configuration.backgroundImagePath.isEmpty()) ? pixels : nullptr;
    options.backgroundImage.mode = configuration.backgroundImageMode;
    options.backgroundImage.opacity = static_cast<float>(configuration.backgroundImageOpacity);
}

/// Fills the Tektronix colours a configuration names.
///
/// The custom phosphor pair or the theme's and the bloom:
/// one fold for every arm, so a renderer cannot disagree with another about a
/// colour the reader set.
///
/// - Parameters:
///   - options: Filled in place.
///   - configuration: What the configuration resolved to.
inline void applyTekSettings(Rendering::TerminalSnapshotOptions& options, const Rendering::RenderConfiguration& configuration)
{
    const auto unpack = [](std::uint32_t packed) {
        return Tek::widenColor(ColorRGBA { static_cast<std::uint8_t>((packed >> 16) & 0xFFu),
            static_cast<std::uint8_t>((packed >> 8) & 0xFFu), static_cast<std::uint8_t>(packed & 0xFFu), 255 });
    };
    options.tekColors.mode = configuration.usesCustomTekColors ? Tek::ColorMode::Custom : Tek::ColorMode::Theme;
    options.tekColors.beam = unpack(configuration.tekBeamRGB);
    options.tekColors.background = unpack(configuration.tekBackgroundRGB);
    options.shouldDrawTekGlow = configuration.drawsTekGlowVectors;
}

/// How `applyLinkSettings` decides per-kind link styling.
enum class PerKindLinkStyling {
    /// The `link_style_per_kind` setting decides.
    FromSettings,
    /// Per-kind styling and link underlines on regardless of the settings.
    Forced,
};

/// Fills the link styling a configuration names.
///
/// Whether link cells draw an underline of their own, which underline, and
/// whether a file or mailto link takes its own style, resolved once here. One
/// fold for every arm, the portable renderer included: it draws the reference
/// the others are compared against, and a frontend that spells this itself
/// owns a copy that drifts from the reference the first time either is
/// edited, which reads as a link drawn right disagreeing with a reference
/// drawn wrong. The link colour is not named here: an options struct that
/// leaves it unset takes the palette's own link colour in the capture, so a
/// frontend without a system accent needs no colour of its own.
///
/// - Parameters:
///   - options: Filled in place. Fields these settings do not name are left
///     alone.
///   - configuration: What the configuration resolved to.
///   - terminal: The terminal whose link ring resolves a link id to its kind,
///     installed as the per-kind resolver when per-kind styling is on. Null
///     leaves every link at the base style.
///   - perKind: Whether per-kind styling follows the setting or is forced on,
///     with underlines forced with it, for a caller exercising the per-kind
///     rules.
inline void applyLinkSettings(Rendering::TerminalSnapshotOptions& options, const Rendering::RenderConfiguration& configuration,
    const Ubiquitty::Terminal* terminal, PerKindLinkStyling perKind = PerKindLinkStyling::FromSettings)
{
    const bool isForced = perKind == PerKindLinkStyling::Forced;
    const bool shouldStylePerKind = isForced || configuration.shouldStyleLinksPerKind;
    options.shouldUnderlineLinks = configuration.shouldUnderlineLinks || isForced;
    options.linkUnderlineStyle = linkUnderlineVTStyle(configuration.linkUnderlineStyle);
    if (shouldStylePerKind && terminal != nullptr) {
        // The frontend supplies only the lookup. Which scheme reads dotted and
        // which dashed is Core's default, and a second copy elsewhere drifts
        // from it the first time either is edited.
        options.linkKindResolverContext = const_cast<void*>(static_cast<const void*>(terminal));
        options.linkKindResolver = [](std::uint16_t identifier, void* context) -> std::uint8_t {
            return static_cast<std::uint8_t>(
                Attributes::LinkRun::kindForLinkID(static_cast<const Ubiquitty::Terminal*>(context), identifier));
        };
    }
}

} // namespace Ubiquitty::Preferences::Effective
