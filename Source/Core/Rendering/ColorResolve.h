// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/Rendering/CellInstance.h"
#include <cmath>

namespace Ubiquitty {

struct Pen; // Core/Pen.h

struct TermPalette; // Core/TermPalette.h

namespace Rendering {

// 8-bit RGB, the portable resolver's output (no alpha — the Core 16-bit
// TermPalette carries none; macOS per-color opacity is added by the Metal
// backend and stays backend-specific).
struct RGB8 {
    uint8_t red, green, blue;
};

struct ResolvedCell {
    RGB8 foreground;
    RGB8 background;
};

/// Raises low-contrast text lightness contrast while preserving hidden and equal-color text.
///
/// - Parameters:
///   - resolved: The colors after reverse, hidden, and dim attributes resolve.
///   - pen: The attributes whose dim hierarchy remains visible.
/// - Returns: The adjusted foreground and unchanged background.
[[nodiscard]] ResolvedCell enhanceTextContrast(ResolvedCell resolved, const Pen& pen);

/// Returns continuous relative brightness times alpha for neutral text between
/// the appearance's neutral endpoint and theme foreground. Primary and chromatic ink return zero.
[[nodiscard]] float secondaryTextCoverage(Vec4 foreground, Vec4 background, Vec4 defaultForeground, Vec4 defaultBackground);

/// Holds the sRGB gamma conversion as a static member rather than a
/// namespace-level free function.
///
/// Swift's C++ importer carries member functions where it drops
/// namespace-level free functions — the same reason
/// ``AnimationScheduleInputs::nextDelaySeconds`` is a member rather than a
/// free function (`AnimationSchedule.h`).
struct SRGBGamma {
    /// Converts one sRGB-encoded channel to linear light.
    ///
    /// The IEC 61966-2-1 piecewise curve: a linear segment below the
    /// 0.04045 knee, a 2.4-power curve above it. Every Metal clear colour
    /// linearizes its sRGB background through this curve to match the
    /// shaders' linear output, and the WCAG luminance weighting a theme's
    /// link colour is picked against uses the same curve on the same
    /// 0..1 channel — one function underlies both.
    ///
    /// - Parameter channel: The sRGB channel, 0..1 (uncapped outside that
    ///   range, so a caller channel-dividing an 8-bit value by 255 is safe).
    /// - Returns: The linear-light channel.
    [[nodiscard]] static double linearize(double channel)
    {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    }

    /// Converts one linear-light channel back to its sRGB encoding.
    ///
    /// The inverse of `linearize`, so a colour mixed in linear light can be
    /// handed on as the encoded value every palette and layer colour is.
    ///
    /// - Parameter linear: The linear-light channel, 0..1.
    /// - Returns: The sRGB-encoded channel.
    [[nodiscard]] static double encode(double linear)
    {
        return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    }
};

/// Resolves a pen and palette to the final foreground and background.
///
/// The single portable resolver, keyed on the Core 16-bit `TermPalette` and
/// emitting 8-bit RGB. Each step reads what the one before it wrote, so the
/// order applies exactly as written: resolve both sides; bold-as-bright (`brightenBold` and `Bold` and a
/// literal ANSI 0..7 foreground remaps to 8..15); reverse (`Reverse` XOR
/// `reverseAlso` swaps the two); hidden (foreground becomes background); dim
/// (foreground becomes the midpoint), LAST so it survives the reverse.
///
/// - Parameters:
///   - pen: The cell's pen.
///   - palette: The live palette.
///   - reverseAlso: An extra swap layered on the pen's own, for selection and
///     global reverse video. // bool-ok: it composes with `Reverse` by XOR, which is what a second state would have to spell out anyway
///   - brightenBold: Whether bold remaps an ANSI 0..7 foreground to 8..15. // bool-ok: a rendering preference with exactly two settings
/// - Returns: The resolved foreground and background.
///
/// The reference rather than the shipping path: every production caller takes
/// the ``Palette8`` overload below, and the parity tests assert that one against
/// this. So the two are byte-identical by intent and not interchangeable in
/// practice — routing a render path through this one would drop the attribute
/// the other carries, and deleting this one would leave that claim with nothing
/// to check against.
ResolvedCell resolveCell(const Pen& pen, const TermPalette& palette, bool reverseAlso, bool brightenBold); // bool-ok: both compose with the pen's own state — reverseAlso XORs with `Reverse` and brightenBold gates a remap — so an enum would name the same two cases twice

/// A ``TermPalette`` narrowed to 8 bits once.
///
/// Narrowing a 16-bit channel is `(v * 255 + 32768) / 65535` — an integer
/// divide — and resolving a cell runs it six times, nine when bold remaps to a
/// bright index. None of it depends on the pen: the same 259 slots narrow to
/// the same bytes for every cell in the frame.
///
/// A caller resolving one cell should not build this. A caller walking a grid
/// should build it once and never divide again — on a screen of varied pens the
/// memo misses nearly every cell, so the divides are per cell rather than per
/// distinct pen.
struct Palette8 {
    /// All 256 slots and the two defaults — 774 bytes, narrowed once a frame.
    ///
    /// All 256 slots rather than the sixteen ANSI ones, because 256-colour is
    /// what a syntax highlighter and a status line emit, so indices 16..255 are
    /// common in the workloads that matter.
    RGB8 indexed[256];
    RGB8 defaultForeground;
    RGB8 defaultBackground;
};

/// Narrows every slot of a palette to 8 bits.
///
/// - Parameter palette: The 16-bit palette to narrow.
/// - Returns: The same colours, one byte per channel.
[[nodiscard]] Palette8 narrowPalette(const TermPalette& palette);

/// Resolves a cell's colours against an already-narrowed palette.
///
/// Byte-identical to the ``TermPalette`` overload for every input — it is the
/// same function with the narrowing lifted out — so the two can be swapped
/// under a differential harness without moving a pixel.
///
/// - Parameters:
///   - pen: The cell's pen: colour indices, direct RGB, and attributes.
///   - palette: The frame's palette, already narrowed by ``narrowPalette``.
///   - reverseAlso: An extra foreground/background swap on top of
///     ``Attribute::Reverse``, for selection and global reverse video.
///   - brightenBold: Whether bold remaps a literal ANSI foreground 0..7 to its
///     bright 8..15 twin.
/// - Returns: The final foreground and background, 8 bits per channel.
///
/// Kept out of line. Both render paths call this from a per-cell loop behind a
/// resolved-colour memo, so on content whose pens repeat the call is branched
/// over; inlined, the body would bloat that loop for content whose pens change
/// per cell. The attribute binds only while the PGO profile applies to this
/// function (`Scripts/check-pgo-applies.py`), so judge it under the shipping
/// flags with a profile that applies.
[[gnu::noinline]] ResolvedCell resolveCell(const Pen& pen, const Palette8& palette, bool reverseAlso, bool brightenBold); // bool-ok: mirrors the TermPalette overload exactly, which is the claim being made

// Resolve a single palette color slot to 8-bit RGB, for chrome / fills that
// aren't full cells. colorDefault picks the default per `foreground`; a 0..255
// index hits the live palette; colorRGB / out-of-range fall back to the default.
RGB8 resolvePaletteColor(const TermPalette&, uint16_t index, bool foreground);

/// Resolves the underline color for a cell.
///
/// `colorDefault` follows the resolved foreground, which is why the foreground
/// is passed in rather than resolved again — SGR 59 and SGR 0 both leave the
/// underline tracking the text, and re-resolving would drop the reverse and dim
/// stages `resolveCell` already applied.
///
/// - Parameters:
///   - pen: The cell's pen. `colorRGB` uses its underline r/g/b; a 0..255 index
///     hits the live palette.
///   - palette: The live palette.
///   - resolvedForeground: The foreground `resolveCell` produced for this cell.
/// - Returns: The underline color.
RGB8 resolveUnderlineColor(const Pen& pen, const TermPalette& palette, RGB8 resolvedForeground);

/// Resolves the opacity a cell's decoration rules draw at.
///
/// The rule takes the opacity of the colour it NAMES, which is the only answer
/// that stays right when the two differ. Borrowing the text's opacity for an SGR
/// 58 underline draws a fully opaque palette colour at the text's translucency
/// and a translucent one at full strength; ignoring the palette's alpha
/// altogether draws every SGR 58 rule opaque under text that fades. Both look
/// like a compositing fault in the shader rather than a colour resolved from the
/// wrong slot.
///
/// Paired with ``resolveUnderlineColor``, which selects the colour by the same
/// four cases in the same order. Answering one of them differently here draws a
/// rule whose opacity belongs to a colour it is not painted in.
///
/// - Parameters:
///   - pen: The cell's pen, whose `underlineIndex` selects the case.
///   - paletteAlpha: Per-index opacity, 256 entries, or null for opaque
///     throughout — the protocol cannot set it, so only a theme carries it.
///   - resolvedForegroundAlpha: The alpha of the foreground this cell resolved
///     to, already carrying its own palette opacity and the blink curve.
///   - blinkCurve: The SGR 5/6 factor on its own, for the cases that take their
///     opacity from somewhere other than the foreground and must still blink
///     with it.
/// - Returns: The rule opacity, 0..1.
float resolveUnderlineAlpha(const Pen& pen, const std::uint8_t* paletteAlpha, float resolvedForegroundAlpha, float blinkCurve);

/// What a focused block cursor draws the glyph under it in.
struct CursorTextInk {
    /// Where the glyph's color comes from.
    enum class Source : std::uint8_t {
        /// The cell's own foreground.
        Cell,
        /// The Rec.709-luma grayscale inverse of the cursor color.
        Inverse,
        /// `color`, the one kitty's `cursor_text` names.
        Color,
    };
    /// Where the glyph's color comes from; the cell's own unless asked.
    Source source = Source::Cell;
    /// The glyph's color, 0..1, read only for ``Source/Color``.
    Vec4 color { 0.0f, 0.0f, 0.0f, 1.0f };
};

/// The cell colors a focused block cursor draws over.
struct CursorBake {
    Vec4 foreground;
    Vec4 background;
};

/// Bakes a focused block cursor into its cell's colors.
///
/// Lerps the cell background toward the cursor color by `mixA`; unless the ink
/// is the cell's own, also lerps the glyph foreground toward the ink's color
/// and its alpha toward opaque. Lerp form x += (target - x) * mixA, so a
/// `mixA` of 0 leaves the cell untouched.
///
/// - Parameters:
///   - cellForeground: The glyph's resolved color, 0..1.
///   - cellBackground: The cell's resolved background, 0..1.
///   - cursorRGB: The cursor color, 0..1.
///   - mixA: The blink weight, 0..1.
///   - ink: What the glyph under the cursor is drawn in.
/// - Returns: The cell's foreground and background under the cursor.
CursorBake bakeBlockCursor(Vec4 cellForeground, Vec4 cellBackground, Vec4 cursorRGB, float mixA, CursorTextInk ink);

} // namespace Rendering
} // namespace Ubiquitty
