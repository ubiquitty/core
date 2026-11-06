// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/PaletteRGB.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Ubiquitty::OSC::XParseColor {

/// Parses an X11 color specification into 16-bit RGB.
///
/// Accepts the forms X11's `XParseColor` defines: `#RGB..`, `rgb:R/G/B`, `rgbi:R/G/B` and the
/// device-independent `CIEXYZ` / `CIEuvY` / `CIExyY` / `CIELab` / `CIELuv` / `TekHVC` spaces,
/// and failing those a colour name from X.Org's colour database (`red`, `light sky blue`),
/// matched without regard to case or spaces.
/// Answers match Xlib's arithmetic, including its default Tektronix screen characterization and
/// its gamut compression, so a channel read back is always a replicated byte.
///
/// The `hsl:` Ubiquitty extension is not handled here — the caller owns it.
///
/// - Parameters:
///   - spec: The specification text. Its end terminates the spec, as `;` does, so
///     a NUL inside the view is payload and rejects the colour like any other
///     trailing byte would.
/// - Returns: The colour, or nothing on anything Xlib would reject. The empty
///   result is what lets a caller keep the colour it already had — the three
///   out-parameters this replaced carried that contract in their documentation
///   and not in their types, so a caller could read them after a rejection.
[[nodiscard]] std::optional<PaletteRGB> parseColorSpecification(std::string_view spec);

/// Parses kitty's color encoding, with linear, clamped `rgbi:` components.
///
/// - Parameter specification: The encoded color, without an alpha suffix.
/// - Returns: The color, or nothing for malformed input.
[[nodiscard]] std::optional<PaletteRGB> parseKittyColorSpecification(std::string_view specification);

} // namespace Ubiquitty::OSC::XParseColor
