// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// Source/Core/Tek/ColorPreference.h — the storage-tube colour types, apart from
// their resolvers so the terminal can hold the user's choice that a colour query
// and every renderer resolve against.

namespace Ubiquitty::Tek {

/// A storage-tube colour, one 16-bit channel each.
///
/// Sixteen bits because `OSC 15/16/18` set colours through the same
/// `xparsecolor` syntax the palette uses, which carries four hex digits per
/// channel. A renderer scales to whatever its pipeline wants.
struct TekColor {
    std::uint16_t red;
    std::uint16_t green;
    std::uint16_t blue;
};

/// Which colours a storage-tube frame draws with absent a host request.
enum class ColorMode : std::uint8_t {
    /// The terminal's own default foreground and background.
    Theme,
    /// The phosphor and tube colours the user chose.
    Custom,
};

/// The user's storage-tube colour choice.
///
/// `beam` and `background` are read only under ``ColorMode::Custom``; under
/// ``ColorMode::Theme`` the theme's own pair stands in, which is why they are
/// not defaulted to a phosphor here. A renderer holds one of these per view and
/// passes it to each resolver.
struct ColorPreference {
    ColorMode mode = ColorMode::Theme;
    TekColor beam {};
    TekColor background {};
};

} // namespace Ubiquitty::Tek
