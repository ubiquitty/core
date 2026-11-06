// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Core/Themes/StatusColors.h — the colours a theme gives a status.
//
// A status mark (a failed job, a program waiting on someone, work in flight)
// is drawn in chrome, beside text the theme coloured. A literal RGB would keep
// a status dot one blue under every theme, so these are the theme's own slots
// for it: a frontend asks for a slot and draws what it is given.
//
// The defaults come from the theme's ANSI colours, which are the colours its
// author already chose to mean these things in program output (red for an
// error, yellow for a warning, green for success), so a theme that names no
// status colour still has ones that belong to it.
//
// Pure: colours in, colours out.
//

#include "Core/Color.h"
#include "Core/TermPalette.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Ubiquitty::Themes {

/// The statuses a theme has a colour for.
enum class StatusColorSlot : std::uint8_t {
    /// Something went wrong.
    Failed,
    /// A program is waiting on a person.
    Blocked,
    /// A bell or a notification nobody has seen.
    Attention,
    /// A program finished.
    Done,
    /// Work is in flight.
    Working,
    /// Nothing to report: idle, or running without having said anything.
    Neutral,
    /// A job's reported progress (OSC 9;4), where it is neither failed nor
    /// dimmed. Apart from `Working` because a platform draws progress in its
    /// own accent unless the theme says otherwise.
    Progress,
};

/// How many slots ``StatusColorSlot`` names.
inline constexpr std::size_t statusColorSlotCount = 7;

/// The byte that stands for "no slot" where a slot travels as one.
inline constexpr std::uint8_t noStatusColorSlot = 0xFF;

/// A slot's key in a theme file, after `status_`: `status_failed = "#rrggbb"`.
///
/// - Parameter slot: The status.
/// - Returns: The word the reader and the writer share.
[[nodiscard]] constexpr std::string_view statusColorSlotName(StatusColorSlot slot)
{
    switch (slot) {
    case StatusColorSlot::Failed:
        return "failed";
    case StatusColorSlot::Blocked:
        return "blocked";
    case StatusColorSlot::Attention:
        return "attention";
    case StatusColorSlot::Done:
        return "done";
    case StatusColorSlot::Working:
        return "working";
    case StatusColorSlot::Neutral:
        return "neutral";
    case StatusColorSlot::Progress:
        return "progress";
    }
    return {};
}

/// The status colours a theme file names for itself, slot by slot; an absent
/// one leaves the slot to ``deriveStatusColors``.
using StatusColorOverrides = std::array<std::optional<ColorRGBA>, statusColorSlotCount>;

/// The colour of each slot for a theme that names none of its own.
///
/// Failed is the theme's red, blocked its yellow, attention its magenta, done
/// its green, and working and progress its blue: the ANSI slots 1, 3, 5, 2 and
/// 4. Neutral is halfway between the foreground and the background, which
/// reads as a quiet mark on either a dark or a light theme.
///
/// - Parameters:
///   - ansi: The theme's sixteen ANSI colours.
///   - foreground: The theme's default foreground.
///   - background: The theme's default background.
/// - Returns: The derived colours, opaque, by slot.
[[nodiscard]] constexpr std::array<ColorRGBA, statusColorSlotCount> derivedStatusColorSlots(std::span<const ColorRGBA, 16> ansi, ColorRGBA foreground, ColorRGBA background)
{
    const auto opaque = [](ColorRGBA color) { return ColorRGBA { color.red, color.green, color.blue }; };
    const auto midpoint = [](std::uint8_t first, std::uint8_t second) { return static_cast<std::uint8_t>((static_cast<unsigned>(first) + second + 1) / 2); };
    std::array<ColorRGBA, statusColorSlotCount> slots {};
    slots[static_cast<std::size_t>(StatusColorSlot::Failed)] = opaque(ansi[1]);
    slots[static_cast<std::size_t>(StatusColorSlot::Blocked)] = opaque(ansi[3]);
    slots[static_cast<std::size_t>(StatusColorSlot::Attention)] = opaque(ansi[5]);
    slots[static_cast<std::size_t>(StatusColorSlot::Done)] = opaque(ansi[2]);
    slots[static_cast<std::size_t>(StatusColorSlot::Working)] = opaque(ansi[4]);
    slots[static_cast<std::size_t>(StatusColorSlot::Progress)] = opaque(ansi[4]);
    slots[static_cast<std::size_t>(StatusColorSlot::Neutral)] = { midpoint(foreground.red, background.red), midpoint(foreground.green, background.green), midpoint(foreground.blue, background.blue) };
    return slots;
}

/// The slots a reader with no theme at all is handed: the ones the palette
/// a terminal starts with derives. No theme is not no colour, so this is
/// also what a `StatusColors` nobody filled holds.
///
/// - Returns: The colours, by slot.
[[nodiscard]] inline const std::array<ColorRGBA, statusColorSlotCount>& defaultStatusColorSlots()
{
    static const std::array<ColorRGBA, statusColorSlotCount> slots = [] {
        TermPalette palette;
        palette.seedDefaults();
        const auto narrow = [](PaletteRGB color) { return ColorRGBA { channel16To8(color.red), channel16To8(color.green), channel16To8(color.blue) }; };
        std::array<ColorRGBA, 16> base {};
        for (std::size_t index = 0; index < base.size(); ++index)
            base[index] = narrow(palette.indexed[index]);
        return derivedStatusColorSlots(base, narrow(palette.defaultForeground), narrow(palette.defaultBackground));
    }();
    return slots;
}

/// A theme's status colours, one per slot.
struct StatusColors {
    std::array<ColorRGBA, statusColorSlotCount> slots = defaultStatusColorSlots();
    /// Which slots the theme named, one bit per ``StatusColorSlot``. A slot it
    /// did not name holds its derived colour, which a platform with a colour
    /// of its own for that mark is drawn over (``App::statusMarkInk``).
    std::uint8_t namedSlots = 0;

    /// Whether the theme named a slot rather than leaving it derived.
    ///
    /// - Parameter slot: The status.
    /// - Returns: Whether its colour came from the theme file.
    [[nodiscard]] constexpr bool isNamed(StatusColorSlot slot) const { return (namedSlots >> static_cast<unsigned>(slot)) & 1U; }

    /// The colour of one slot.
    ///
    /// - Parameter slot: The status.
    /// - Returns: Its colour.
    [[nodiscard]] constexpr ColorRGBA color(StatusColorSlot slot) const { return slots[static_cast<std::size_t>(slot)]; }
};

/// The status colours a theme has when it names none of its own.
///
/// - Parameters:
///   - ansi: The theme's sixteen ANSI colours.
///   - foreground: The theme's default foreground.
///   - background: The theme's default background.
/// - Returns: The colours ``derivedStatusColorSlots`` derives, none named.
[[nodiscard]] constexpr StatusColors deriveStatusColors(std::span<const ColorRGBA, 16> ansi, ColorRGBA foreground, ColorRGBA background)
{
    return StatusColors { .slots = derivedStatusColorSlots(ansi, foreground, background), .namedSlots = 0 };
}

/// A theme's status colours: what it names, over what it derives.
///
/// - Parameters:
///   - derived: The defaults, from ``deriveStatusColors``.
///   - overrides: What the theme file names.
/// - Returns: One colour per slot.
[[nodiscard]] constexpr StatusColors withStatusColorOverrides(StatusColors derived, const StatusColorOverrides& overrides)
{
    for (std::size_t slot = 0; slot < statusColorSlotCount; ++slot) {
        if (overrides[slot]) {
            derived.slots[slot] = *overrides[slot];
            derived.namedSlots = static_cast<std::uint8_t>(derived.namedSlots | (1U << slot));
        }
    }
    return derived;
}

} // namespace Ubiquitty::Themes
