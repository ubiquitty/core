// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/CellColor.h"

namespace Ubiquitty::Rendering {

/// The colour a link draws in on a theme, from the theme's two blues.
///
/// Whichever of ANSI blue and bright blue reads better against the default
/// background, lifted toward the default foreground until it clears a
/// legibility floor. One rule for the eight-bit ``CellPalette``, Core's
/// sixteen-bit ``TermPalette`` and the frame capture, so no two arms can
/// disagree about the colour exactly where the choice is interesting.
///
/// Its own unit rather than CellColor.cpp's because the daemon and the
/// agent trim that file and keep the frame capture, which needs this and
/// nothing else of it: a rule kept with the pen resolver it does not use
/// is a link error on every arm whose linker keeps what nothing reaches.
///
/// - Parameters:
///   - blue: The palette's ANSI blue, slot 4.
///   - brightBlue: Its bright twin, slot 12.
///   - background: The default background the contrast is measured against.
///   - foreground: The default foreground a low-contrast blue lifts toward.
/// - Returns: The link colour, opaque.
[[nodiscard]] ColorRGBA resolveLinkColor(ColorRGBA blue, ColorRGBA brightBlue, ColorRGBA background, ColorRGBA foreground);

} // namespace Ubiquitty::Rendering
