// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdint>

// Which colour-vision deficiency the palette is adapted for.
//
// Its own header because the type crosses a layer: the transform that consumes
// it is model code (Accessibility/Palette.h) and the token parser that produces
// it is behaviour code (Values), so a shared home has to be reachable from
// below. Palette.h drags a whole CellPalette with it and Preferences.h drags the
// entire configuration, which is why neither is that home.

namespace Ubiquitty::Accessibility {

/// Which colour-vision deficiency the palette adaptation targets.
///
/// `None` leaves the palette untouched; each named deficiency selects a
/// built-in CVD-safe remap. Persisted under `[appearance] colorblind_mode` as
/// the lowercase token, and the values match the Values popup index — both
/// wire forms, so an enumerator's number is as much a contract as its name.
enum class ColorblindMode : std::uint8_t {
    None = 0,
    Deuteranopia = 1,
    Protanopia = 2,
    Tritanopia = 3,
};

} // namespace Ubiquitty::Accessibility
