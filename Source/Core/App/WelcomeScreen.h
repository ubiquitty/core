// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The first-launch Welcome screen, and the vocabulary of the buttons on it.
//
// The screen is VT bytes a real terminal pane renders rather than a native
// view: the app icon as a kitty graphics image, a DECDHL headline, a
// truecolor tagline plate, and `OSC 8` links whose targets are
// `ubiquitty:action/<name>`. A frontend writes the bytes into a
// pane and dispatches a clicked target through `actionForURI`, so the names the
// screen emits and the names a click is matched against are one table and
// cannot drift apart.
//
// Terminal output is attacker-controllable, and `OSC 8` separates a link's
// visible text from its target, so anything that can print can print one of
// these targets under any label. The vocabulary is therefore closed: a target
// names one of a fixed set of actions by exact string equality, never a
// method, a path or an argument. A frontend treats every target carrying the
// action prefix as its own, whether or not the name is known, so none of them
// ever reaches a link handler or the system opener. An action that writes
// anything is the frontend's to put behind a human's answer.
//
// Platform-free (check-boundaries.py: "Source/Core/App names no native
// framework").

#include "Base/Compiler.h"
#include "Base/SwiftInterop.h"
#include "Core/Color.h"
#include "Core/Preferences/Preferences.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty {
struct Terminal;
}

namespace Ubiquitty::App::Welcome {

/// One of the Welcome screen's buttons, as a click on it names it.
enum class UBIQUITTY_CLOSED_ENUM Action : std::uint8_t {
    /// A target carrying the action prefix and a name outside the table.
    Unrecognized UBIQUITTY_SWIFT_NAME(unrecognized),
    /// Installs shell integration into the shell startup files, and the
    /// terminfo entry.
    ShellIntegration UBIQUITTY_SWIFT_NAME(shellIntegration),
    /// Opens the Connect window.
    Connect UBIQUITTY_SWIFT_NAME(connect),
    /// Opens the preferences window on its Appearance pane.
    Settings UBIQUITTY_SWIFT_NAME(settings), // preferences-vocabulary-ok: the button's own name, for the macOS window
    /// Opens the preferences window on its Accessibility pane.
    Accessibility UBIQUITTY_SWIFT_NAME(accessibility),
};

/// Whether a link target belongs to the Welcome screen's own namespace.
///
/// Answers for the prefix alone, so an unknown name is still the frontend's:
/// no handler and no system opener sees a target this answers yes for.
///
/// - Parameter uri: A link target, as the terminal resolved it.
/// - Returns: Whether it begins `ubiquitty:action/`.
[[nodiscard]] bool isActionURI(std::string_view uri);

/// The action a link target names.
///
/// - Parameter uri: A link target, as the terminal resolved it.
/// - Returns: The action whose name follows the prefix exactly, or
///   `Action::Unrecognized` for any other target.
[[nodiscard]] Action actionForURI(std::string_view uri);

/// Whether the arm showing the screen draws inline images.
enum class UBIQUITTY_CLOSED_ENUM ImageDrawing : std::uint8_t {
    /// Images are drawn, at the cell box their placement names.
    Drawn UBIQUITTY_SWIFT_NAME(drawn),
    /// Images are decoded and stored but never reach the screen.
    Refused UBIQUITTY_SWIFT_NAME(refused),
};

/// The pane the screen is written into, as the frontend that opens it knows it.
///
/// Facts, not choices: which logo the screen draws, in which appearance, at
/// which size and where in its box, is decided from these by
/// ``screenContent``, so every arm that shows the screen draws the same one
/// for the same pane.
struct Pane {
    /// The pane's default background.
    ColorRGBA ground { 0, 0, 0 };
    /// Whether the arm draws images.
    ImageDrawing imageDrawing = ImageDrawing::Refused;
    /// The grid.
    int rows = 24;
    int columns = 80;
    /// One cell as the grid draws it, in device pixels: fractional, because an
    /// advance is. Zero leaves the logo's rows out, having no size to give the
    /// icon; ``paneOf`` never answers zero.
    double cellPixelWidth = 0;
    double cellPixelHeight = 0;
    /// Whether the cell is the one the frontend published, rather than the
    /// configured face's nominal cell standing in until it does.
    bool isGeometryPublished = false;
    /// Device pixels per point of the frontend that supplied the pane geometry.
    double deviceScale = 1;
};

/// The dark-ground tile's tone: the ground lifted this far in OKLab lightness.
inline constexpr double tileLightnessStep = 0.10;
/// How much of the ground's chroma the tile keeps, its hue unchanged.
inline constexpr double tileChromaShare = 0.6;
/// How much darker the tile's foot is than its head, in OKLab lightness, as
/// the document's own fill darkens down the icon.
inline constexpr double tileGradientDepth = 0.04;

/// The colours a dark-ground tile runs between, top to bottom.
struct TileTone {
    ColorRGBA top;
    ColorRGBA bottom;
};

/// The tone of the icon's tile on a dark ground: a lighter shade of the
/// ground's own colour, so the tile reads as part of the theme rather than a
/// neutral slab on it.
///
/// - Parameter ground: The pane's default background.
/// - Returns: The tile's top and bottom colours.
[[nodiscard]] TileTone tileTone(ColorRGBA ground);

/// The pane a live terminal is, for the screen about to be written into it.
///
/// Every fact is the terminal's own — the default background its theme gave
/// it, its grid, and the drawn cell its frontend published — except the two
/// only the arm knows. Before the frontend has published a cell, the
/// configured face's nominal cell stands in, and the pane says so.
///
/// - Parameters:
///   - terminal: The terminal the screen will be written into.
///   - config: The configuration naming the face, for the nominal cell.
///   - deviceScale: Device pixels per point where it is drawn.
///   - imageDrawing: Whether the arm draws images.
/// - Returns: The pane, ready for ``screenContent``.
[[nodiscard]] Pane paneOf(const Terminal& terminal, const Preferences::Values& config, double deviceScale, ImageDrawing imageDrawing);

/// The Welcome screen, as the bytes a pane renders.
///
/// Opens with a home and a clear, and fits an 80-by-24 grid with rows to
/// spare for the shell prompt that follows it: a first-launch reader never
/// opens the app to a screen already scrolled past its headline. A grid with
/// at least two rows to spare gets one above the screen, so at 80 by 24 the
/// logo sits a row below the top edge and two rows follow the prompt.
///
/// The logo is the app icon as a kitty graphics image in a box of 16 cells by
/// 8 rows, with a drop shadow: on a light ground the Default rendition, its
/// black tile set off against the screen; on a dark ground the icon's layers
/// composited over a tile in the ground's own tone (``tileTone``), sent as RGBA. It is drawn square, as large as the
/// box — or a smaller grid's space — allows, centred across the screen and,
/// down, in its rows and the blank row below them, so it sits as far below the
/// top as above the name. The render is the one whose pixel size is nearest at
/// or above that side, so the common face draws one texel per device pixel. The
/// buttons' plates and labels are derived from the pane's ground, and every
/// label reads on its plate at WCAG AA.
///
/// There is no text stand-in for the icon. Where it cannot be drawn — an arm
/// that draws no images, which none is today, or a space too small for a
/// recognisable icon — the logo's rows are left out and the name opens the
/// screen.
///
/// The image writes no cell, so the logo's rows hold no text to select, copy or
/// read aloud, and the first line a reader meets is the name.
///
/// - Parameter pane: The pane the bytes are written into.
/// - Returns: UTF-8 text with the VT sequences described above.
[[nodiscard]] std::string screenContent(const Pane& pane);

} // namespace Ubiquitty::App::Welcome
