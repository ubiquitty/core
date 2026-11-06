// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Color.h"
#include "Core/Tek/ColorPreference.h"
#include "Core/Terminal.h"

// Source/Core/Tek/Colors.h — which colour a storage-tube frame draws with.
//
// Three sources can name the same colour and they are not equal: the host's
// OSC 15/16/18, the colours the user picked for Custom mode, and the terminal's
// own theme. The order between them is a policy every renderer of Tek output
// has to apply the same way — a frontend that resolves it privately draws a
// session's vectors in a colour another frontend would not.
//
// Channels are 16-bit because that is what OSC carries. Narrowing here would
// discard the low byte of a colour the host asked for by name, and it would do
// it in the one place no frontend can put back.

namespace Ubiquitty::Tek {

/// Widens an 8-bit channel to 16 bits without changing the colour.
///
/// The factor is 257 rather than 256: it maps 0 to 0 and 255 to 65535, so a
/// theme colour widened here and scaled back by any renderer is the colour it
/// started as. Shifting by eight would map 255 to 65280 and darken every theme
/// colour by a quarter of a percent, which reads as correct and is not.
///
/// - Parameter channel: The channel, 0-255.
/// - Returns: The same channel, 0-65535.
[[nodiscard]] constexpr std::uint16_t widenChannel(std::uint8_t channel)
{
    return static_cast<std::uint16_t>(channel * 257);
}

/// Widens an RGBA colour to a storage-tube colour, dropping alpha.
///
/// Alpha has no meaning on a storage tube: the beam either wrote a point or it
/// did not, and the tube's background is opaque by construction.
///
/// - Parameter color: The 8-bit colour.
/// - Returns: The same colour in 16-bit channels.
[[nodiscard]] constexpr TekColor widenColor(ColorRGBA color)
{
    return { widenChannel(color.red), widenChannel(color.green), widenChannel(color.blue) };
}

/// A packed `0xRRGGBB` colour — the config's spelling — as a tube colour.
///
/// Here rather than in each frontend because the unpacking order is a fact
/// about the config's format, and two arms unpacking it are two chances to
/// swap a channel.
///
/// - Parameter packed: The colour, red in the high byte.
/// - Returns: The same colour in 16-bit channels.
[[nodiscard]] constexpr TekColor tekColorFromPackedRGB(std::uint32_t packed)
{
    return widenColor(ColorRGBA {
        static_cast<std::uint8_t>((packed >> 16) & 0xFF),
        static_cast<std::uint8_t>((packed >> 8) & 0xFF),
        static_cast<std::uint8_t>(packed & 0xFF),
        0xFF });
}

/// How far down its box an alpha glyph's baseline sits on the tube, from the
/// atlas geometry the glyph sheet was cut with.
///
/// One formula for both macOS arms: each computing it from its own metrics is
/// the fork where a margin change moves one arm's text and not the other's.
/// Clamped into the unit interval; a degenerate box answers the conventional
/// 0.8 rather than dividing by nothing.
///
/// - Parameters:
///   - ascentInPixels: The face's ascent, in atlas pixels.
///   - lineGapTopInPixels: The top line-gap inset, in atlas pixels.
///   - slotHeightInPixels: The atlas slot's height.
///   - glyphMarginYInPixels: The vertical margin around the glyph box.
/// - Returns: The baseline's fraction of the glyph box, 0–1.
[[nodiscard]] constexpr float tekGlyphBaselineFraction(float ascentInPixels, float lineGapTopInPixels, int slotHeightInPixels, int glyphMarginYInPixels)
{
    const float glyphBoxHeight = static_cast<float>(slotHeightInPixels - 2 * glyphMarginYInPixels);
    if (glyphBoxHeight <= 0.0f)
        return 0.8f;
    const float fraction = (ascentInPixels + lineGapTopInPixels) / glyphBoxHeight;
    if (fraction < 0.0f)
        return 0.0f;
    return fraction > 1.0f ? 1.0f : fraction;
}

/// The colour the storage tube clears to.
///
/// `OSC 16` wins wherever the host set it: naming the tube colour is an
/// explicit per-session request, and a user preference that overrode it would
/// leave the host no way to ask. Otherwise Custom mode's tube colour, and
/// otherwise the theme's background.
///
/// - Parameters:
///   - terminal: The terminal whose `OSC 16` state decides whether the host asked.
///   - themeBackground: The terminal's own default background.
///   - preference: The user's mode and custom colours.
/// - Returns: The clear colour, 16-bit channels.
[[nodiscard]] inline TekColor tekBackground(const Terminal& terminal, TekColor themeBackground, const ColorPreference& preference)
{
    if (terminal.tektronixBackgroundSet)
        return { terminal.tektronixBackgroundRed, terminal.tektronixBackgroundGreen, terminal.tektronixBackgroundBlue };
    if (preference.mode == ColorMode::Custom)
        return preference.background;
    return themeBackground;
}

[[nodiscard]] inline TekColor tekBackground(const Terminal& terminal, ColorRGBA themeBackground, const ColorPreference& preference)
{
    return tekBackground(terminal, widenColor(themeBackground), preference);
}

/// The colour the beam writes with.
///
/// Same order as ``tekBackground`` and for the same reason, through `OSC 15`.
///
/// - Parameters:
///   - terminal: The terminal whose `OSC 15` state decides whether the host asked.
///   - themeForeground: The terminal's own default foreground.
///   - preference: The user's mode and custom colours.
/// - Returns: The beam colour, 16-bit channels.
[[nodiscard]] inline TekColor tekBeam(const Terminal& terminal, TekColor themeForeground, const ColorPreference& preference)
{
    if (terminal.tektronixForegroundSet)
        return { terminal.tektronixForegroundRed, terminal.tektronixForegroundGreen, terminal.tektronixForegroundBlue };
    if (preference.mode == ColorMode::Custom)
        return preference.beam;
    return themeForeground;
}

[[nodiscard]] inline TekColor tekBeam(const Terminal& terminal, ColorRGBA themeForeground, const ColorPreference& preference)
{
    return tekBeam(terminal, widenColor(themeForeground), preference);
}

/// The user's setting for the storage tube's writing spot.
///
/// Named rather than a bare bool because it is the LOWER tier of a precedence:
/// at the call site `Drawn` says which of the two answers is the one a host
/// request would be overriding, where `true` says only that something is on.
enum class WritingSpotSetting : std::uint8_t {
    /// The spot is not drawn unless the host asks for it.
    Suppressed,
    /// The spot is drawn unless the host suppresses it.
    Drawn,
};

/// Whether the storage tube draws its writing spot this frame.
///
/// The spot marks where the beam last wrote. `DECSET/DECRST 7400` lets a Tek
/// app turn it off for the length of a dense draw and back on afterwards, so
/// the host's request outranks the user's setting for as long as it stands —
/// the same order the colour resolvers apply, and for the same reason.
///
/// The default arrives as an argument because `Terminal` holds only the
/// override. The setting behind it is a preference for the whole app rather
/// than state belonging to one session, and a terminal that stored it would
/// have to be told when it changed.
///
/// - Parameters:
///   - terminal: The terminal whose `DECSET 7400` state decides whether the
///     host asked; 1 forces the spot on, 2 forces it off, 0 defers.
///   - configured: The user's resolved `[tek] bright_pen` setting.
/// - Returns: Whether to draw the spot.
[[nodiscard]] inline bool isWritingSpotDrawn(const Terminal& terminal, WritingSpotSetting configured)
{
    if (terminal.tekBrightPenOverride == 1)
        return true;
    if (terminal.tekBrightPenOverride == 2)
        return false;
    return configured == WritingSpotSetting::Drawn;
}

/// The colour the GIN crosshair and the writing spot draw with.
///
/// Falls back to the beam rather than to a theme colour: the crosshair marks
/// where the beam is, and a cursor that stopped matching it would read as a
/// second, unrelated mark. Only `OSC 18` separates them.
///
/// - Parameters:
///   - terminal: The terminal whose `OSC 18` state decides whether the host asked.
///   - beam: The resolved beam colour, from ``tekBeam``.
/// - Returns: The cursor colour, 16-bit channels.
[[nodiscard]] inline TekColor tekCursor(const Terminal& terminal, TekColor beam)
{
    if (terminal.tektronixCursorSet)
        return { terminal.tektronixCursorRed, terminal.tektronixCursorGreen, terminal.tektronixCursorBlue };
    return beam;
}

} // namespace Ubiquitty::Tek
