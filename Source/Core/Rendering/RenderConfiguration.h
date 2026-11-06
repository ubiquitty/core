// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Everything a renderer is configured with, resolved, in one value.
//
// Every arm's renderer — the Swift surface renderer, the software reference,
// the Linux CPU and Vulkan paths, the browser — takes its configuration from
// this struct and nothing else, and the struct comes from one fold over the
// preferences (`Preferences::Effective::renderConfiguration`). A setting that
// reaches one arm and not another is then a renderer that ignores a field it
// was handed, which is visible in that renderer, rather than a frontend that
// forgot to hand it over, which is visible nowhere.
//
// Lengths are in points. One configuration serves surfaces on every display a
// window spans, and the backing scale is a fact of the surface and the frame,
// not of the configuration; a renderer multiplies once, at the frame, by the
// scale it is drawing at.
//
// Trivially copyable: it crosses to render threads and to Swift by value, and
// two configurations compare with `==` to decide whether anything moved. Text
// is held in fixed buffers for that reason; a value longer than its buffer is
// not stored and the field reads empty — its default — rather than truncated
// into a different family or path.

#include "Core/Accessibility/ColorblindMode.h"
#include "Core/Rendering/BackgroundImageLayout.h"
#include "Core/Rendering/BrailleLattice.h"
#include "Core/Rendering/FontMetrics.h"
#include "Core/Rendering/OverlayGeometry.h"
#include "Core/Rendering/RemoteViewport.h"
#include "Core/Rendering/ShadeStyle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace Ubiquitty::Rendering {

/// A short text field held inline.
///
/// - Note: `assign` zero-fills, so two fields holding the same text are
///   byte-identical and the defaulted `==` is exact.
template<std::size_t Capacity>
struct ConfigurationText {
    static_assert(Capacity <= 0xFFFF);
    std::array<char, Capacity> bytes {};
    std::uint16_t length = 0;

    /// Stores `text`, or empties the field when it does not fit.
    ///
    /// - Parameter text: The value to hold.
    /// - Returns: Whether it was stored.
    bool assign(std::string_view text)
    {
        bytes.fill('\0');
        length = 0;
        if (text.size() > Capacity)
            return false;
        for (std::size_t index = 0; index < text.size(); ++index)
            bytes[index] = text[index];
        length = static_cast<std::uint16_t>(text.size());
        return true;
    }

    /// The text, borrowed from this field.
    [[nodiscard]] std::string_view view() const { return { bytes.data(), length }; }
    /// The text, owned; the spelling Swift reads.
    [[nodiscard]] std::string string() const { return std::string(view()); }
    [[nodiscard]] bool isEmpty() const { return length == 0; }

    friend bool operator==(const ConfigurationText&, const ConfigurationText&) = default;
};

/// A font family name.
using FamilyText = ConfigurationText<128>;
/// A `font.features` or `font.variations` specification.
using FontSpecificationText = ConfigurationText<256>;
/// A file path.
using PathText = ConfigurationText<1024>;
/// A script, language or mode token in a fallback rule.
using TokenText = ConfigurationText<36>;

/// One ordered fallback rule: a script or a code-point range routed to a family.
struct FallbackRule {
    /// The script token, such as `han`; empty for a range rule.
    TokenText script;
    /// The preferred language tag, or empty.
    TokenText language;
    /// How the family joins the fallback order.
    TokenText mode;
    FamilyText family;
    /// The inclusive range, for a range rule; both zero for a script rule.
    std::uint32_t firstCodePoint = 0;
    std::uint32_t lastCodePoint = 0;
    FontMetrics::FallbackSizing sizing { FontMetrics::FallbackSizing::Inherit };

    friend bool operator==(const FallbackRule&, const FallbackRule&) = default;
};

/// The most fallback rules of each kind a configuration carries; later rules
/// in the document are not kept.
inline constexpr std::size_t fallbackRuleCapacity = 16;

/// A renderer's whole configuration.
///
/// The member initialisers are zeroes, not the default table: Rendering sits
/// below Preferences and cannot name it. The default table is the fold of an
/// empty document, `renderConfiguration(Values {})`, and a renderer is never
/// handed a default-constructed value.
struct RenderConfiguration {
    // --- Typography: these cut glyph sheets; a change re-measures. ---

    /// The grid's family; empty for the house family list.
    FamilyText fontFamily;
    double fontSizePoints = 0;
    /// The family proportional runs are set in; empty for the platform's UI face.
    FamilyText proportionalFontFamily;
    /// The proportional em size; zero for one optical step over `fontSizePoints`.
    double proportionalFontSizePoints = 0;
    /// The family a glyph the base face lacks routes to.
    FamilyText symbolFallbackFamily;
    FamilyText cjkFallbackFamily;
    /// Empty for the platform cascade.
    FamilyText emojiFallbackFamily;
    std::array<FallbackRule, fallbackRuleCapacity> languageFallbacks {};
    std::uint8_t languageFallbackCount = 0;
    std::array<FallbackRule, fallbackRuleCapacity> rangeMaps {};
    std::uint8_t rangeMapCount = 0;
    /// The OpenType features to apply, in the `font.features` grammar.
    FontSpecificationText fontFeatures;
    /// The variation axes, `tag=N` each, in the `font.variations` grammar.
    FontSpecificationText fontVariations;
    bool hasLigatures = false;
    /// The upright `wght` axis, or zero for the face's own.
    std::int32_t fontWeight = 0;
    /// The bold `wght` axis, or zero for the face's own bold.
    std::int32_t boldFontWeight = 0;
    /// The width percent, or zero for the face's own.
    double fontWidthPercent = 0;
    /// Extra leading folded into the cell.
    double lineSpacingPoints = 0;
    /// Points added to the cell advance; may be negative.
    double characterSpacingPoints = 0;
    /// Vertical glyph shift within the cell, positive up.
    double baselineOffsetPoints = 0;
    bool shouldMatchFallbackSize = true;
    bool shouldBoostWideGlyphs = false;
    /// The optical multiplier on full-width monochrome ink, when boosted.
    double wideGlyphScale = 1;
    /// Whether braille draws as the renderer's dot lattice rather than from the face.
    bool shouldDrawBrailleGrid = false;
    BrailleStyle brailleStyle = defaultBrailleStyle;
    /// How the shade glyphs draw. Typography rather than frame style because
    /// the Linux glyph sheet bakes procedural shades into its slots.
    ShadeStyle shadeStyle = defaultShadeStyle;

    // --- Frame style: read at each frame's assembly; no sheet is keyed on it. ---

    /// Whether SGR bold promotes foreground 0-7 to 8-15.
    bool shouldBrightenBold = false;
    SelectionStyle selectionStyle = defaultSelectionStyle;
    RemoteViewportPolicy nonOwnerPresentation = RemoteViewportPolicy::Letterbox;
    /// A multiplier on the face's own underline thickness.
    double underlineThicknessScale = 1;
    double underlineOffsetPoints = 0;
    bool shouldUnderlineSkipInk = false;
    /// Whether a run's outer underline reaches the letter's ink past the cell.
    bool shouldUnderlineFollowInk = false;
    /// The halo around crossing ink the skip leaves.
    double underlineSkipInkPaddingPoints = 0;
    /// Whether link cells draw an underline of their own.
    bool shouldUnderlineLinks = false;
    /// The `link_style` index; zero is solid.
    std::int32_t linkUnderlineStyle = 0;
    /// Whether file and mailto links take their own underline styles.
    bool shouldStyleLinksPerKind = false;
    /// Whether link foregrounds take the system accent blue.
    bool shouldUseSystemBlueLinkColor = false;
    double paddingLeftPoints = 0;
    double paddingRightPoints = 0;
    double paddingTopPoints = 0;
    double paddingBottomPoints = 0;
    /// The top inset a window without a title bar reserves.
    double titlelessPaddingTopPoints = 0;
    /// Whether the frame is tagged Display P3 rather than sRGB.
    bool usesDisplayP3 = false;
    Accessibility::ColorblindMode colorblindMode = Accessibility::ColorblindMode::None;

    // --- Ground. ---

    /// The whole-window cell-background alpha asked for, 0-1.
    double backgroundOpacity = 1;
    /// The extra alpha of default-background cells alone, 0-1.
    double defaultBackgroundOpacity = 1;
    /// The behind-window blur radius; zero is no blur.
    double blurRadiusPoints = 0;
    /// Whether ordinary text receives a perceptual contrast floor.
    bool shouldEnhanceTextContrast = false;
    /// Whether low-contrast text is strengthened over an active blur.
    bool shouldEnhanceTextOnBlur = false;
    bool usesBackgroundVibrancy = false;
    /// The picture painted behind the grid; empty for none.
    PathText backgroundImagePath;
    double backgroundImageOpacity = 1;
    BackgroundImageMode backgroundImageMode = BackgroundImageMode::Fill;

    // --- Cursor and bell, as the frame draws them. The shape, blink and glide
    //     are terminal state, seeded by `applyCursorPreferences`. ---

    /// The cursor colour's alpha, 0-1.
    double cursorAlpha = 1;
    /// Whether the glyph under a block cursor inverts.
    bool shouldCursorInvertText = false;
    /// Whether cursor and SGR blink fade rather than switch.
    bool shouldFadeBlink = false;
    /// Whether the bell flashes the frame.
    bool isBellVisual = false;
    /// The flash's peak opacity, 0-1.
    double bellFlashPeakAlpha = 0;

    // --- Tek storage tube. ---

    /// Whether the custom phosphor pair replaces the theme's.
    bool usesCustomTekColors = false;
    /// Packed 0xRRGGBB.
    std::uint32_t tekBeamRGB = 0;
    /// Packed 0xRRGGBB.
    std::uint32_t tekBackgroundRGB = 0;
    /// Whether the writing spot draws at the beam's rest.
    bool drawsTekWritingSpot = false;
    /// Whether the additive bloom runs under the strokes.
    bool drawsTekGlowVectors = false;

    // --- Pacing. ---

    /// The continuous-output frame cap; zero is the panel's own rate.
    std::int32_t maxFramesPerSecond = 0;
    /// Whether presentation waits for the display's refresh.
    bool isPresentSynchronized = false;

    /// One language fallback rule, by value; the rule at `index` or an empty
    /// one past `languageFallbackCount`.
    [[nodiscard]] FallbackRule languageFallbackAt(std::size_t index) const
    {
        return index < languageFallbackCount ? languageFallbacks[index] : FallbackRule {};
    }

    /// One range-map rule, by value; as ``languageFallbackAt``.
    [[nodiscard]] FallbackRule rangeMapAt(std::size_t index) const { return index < rangeMapCount ? rangeMaps[index] : FallbackRule {}; }

    /// Whether `other` cuts the same glyph sheets: every typography field agrees.
    [[nodiscard]] bool hasSameTypography(const RenderConfiguration& other) const
    {
        return fontFamily == other.fontFamily && fontSizePoints == other.fontSizePoints
            && proportionalFontFamily == other.proportionalFontFamily && proportionalFontSizePoints == other.proportionalFontSizePoints
            && symbolFallbackFamily == other.symbolFallbackFamily && cjkFallbackFamily == other.cjkFallbackFamily
            && emojiFallbackFamily == other.emojiFallbackFamily && languageFallbacks == other.languageFallbacks
            && languageFallbackCount == other.languageFallbackCount && rangeMaps == other.rangeMaps && rangeMapCount == other.rangeMapCount
            && fontFeatures == other.fontFeatures && fontVariations == other.fontVariations && hasLigatures == other.hasLigatures
            && fontWeight == other.fontWeight && boldFontWeight == other.boldFontWeight && fontWidthPercent == other.fontWidthPercent
            && lineSpacingPoints == other.lineSpacingPoints && characterSpacingPoints == other.characterSpacingPoints
            && baselineOffsetPoints == other.baselineOffsetPoints && shouldBoostWideGlyphs == other.shouldBoostWideGlyphs
            && shouldMatchFallbackSize == other.shouldMatchFallbackSize
            && wideGlyphScale == other.wideGlyphScale && shouldDrawBrailleGrid == other.shouldDrawBrailleGrid
            && brailleStyle == other.brailleStyle && shadeStyle == other.shadeStyle;
    }

    friend bool operator==(const RenderConfiguration&, const RenderConfiguration&) = default;
};

static_assert(std::is_trivially_copyable_v<RenderConfiguration>);

} // namespace Ubiquitty::Rendering
