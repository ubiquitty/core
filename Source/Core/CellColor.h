// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/CellPen.h"
#include "Core/TerminalOperations.h"

// Per-cell color resolution shared between PALs: map Cell.pen to a final 8-bit
// RGB pair and apply the attribute modifiers (bold→bright, dim halves foreground
// brightness, reverse swap, hidden collapses foreground onto background). Both
// PALs call CellPalette::resolve() rather than carry their own copy. Palette is
// the 19-color block (16 ANSI + foreground/background/cursor), built from the
// selected theme.

namespace Ubiquitty {

// Declared rather than included: the palette is model, and a theme is the
// behaviour layer's parsed configuration. Naming it in one member signature
// costs nothing, where including it would put the model layer's header behind
// Values.
struct TermPalette;

namespace Themes {
struct Theme;
}

/// Whether a cell's foreground and background are swapped.
enum class ReverseMode : uint8_t {
    Normal,
    Reversed
};

/// The 256 colours a cell's pen indexes into, plus the three default slots.
///
/// Every colour a cell can carry is resolved against one of these, so the
/// palette is the whole of what a theme, OSC 4 and the preferences contribute
/// to a drawn cell — a backend that renders from anything else has forked.
struct CellPalette {
    // Full 256-color palette. 0-15 are the ANSI base (theme/preferences); 16-255
    // are the xterm cube + grayscale ramp, filled by fillExtended() and
    // overridable by OSC 4. resolve() is then a flat lookup.
    ColorRGBA indexed[256];
    ColorRGBA defaultForeground;
    ColorRGBA defaultBackground;
    ColorRGBA cursor;
    // Attribute::Bold promotes a standard foreground index (0..7) to its bright (8..15)
    // counterpart — the legacy "bold is bright" palette.
    bool brightenBold = false; // bool-ok: one of four mirrored spellings of the same preference — Values::brightenBold, CellInstanceBuilder's input and resolveCell's parameter — and test_renderer_color_parity checks they agree, which a predicate prefix on this one alone would obscure

    /// Fills `indexed[16..255]` with the xterm 6×6×6 cube and grayscale ramp.
    ///
    /// The ANSI base in 0..15 is left alone: a theme owns those slots, and this
    /// derives only the block no theme specifies.
    void fillExtended();

    /// Seeds the compiled-in neutral-dark default palette.
    ///
    /// Sourced from ``Core/TermPalette.h`` so a frontend that draws before any
    /// theme applies agrees with the headless path, which seeds the same block.
    /// A theme apply overwrites all of it.
    void seedDefaults();

    /// Resolves a pen to its final 8-bit foreground and background.
    ///
    /// Bold, dim, reverse and hidden compose in that order, so hidden collapses
    /// the foreground onto the background it ends up with rather than the one it
    /// started from.
    ///
    /// - Parameters:
    ///   - pen: The cell's SGR state.
    ///   - reverse: A swap layered on top of any ``Attribute::Reverse`` the pen
    ///     already carries, so the two cancel rather than compound.
    ///   - outForeground: Receives the resolved foreground, three bytes.
    ///   - outBackground: Receives the resolved background, three bytes.
    void resolve(const Pen& pen, ReverseMode reverse, uint8_t outForeground[3], uint8_t outBackground[3]) const;

    // Theme-derived default hyperlink color: whichever of ANSI blue (4) /
    // bright-blue (12) has the higher WCAG contrast against defaultBackground,
    // blended toward defaultForeground if it falls below the legibility floor.
    [[nodiscard]] ColorRGBA linkColor() const;

    [[nodiscard]] static CellPalette fromTheme(const Themes::Theme&);

    /// The palette a terminal is showing, as a renderer or a copy resolves
    /// cells through it.
    ///
    /// The terminal keeps sixteen-bit channels and every reader wants eight,
    /// so the fold happens here rather than in each frontend's mirror: a copy
    /// that derives its own eight-bit palette beside the renderer's is two
    /// roundings of one colour. `brightenBold` is left false, since it is a
    /// preference and not a fact of the palette; a caller sets it.
    ///
    /// - Parameter palette: The terminal's live palette, OSC overrides applied.
    /// - Returns: The same colours at eight bits per channel.
    [[nodiscard]] static CellPalette fromTerminalPalette(const TermPalette& palette);
};

} // namespace Ubiquitty
