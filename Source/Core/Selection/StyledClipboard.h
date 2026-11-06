// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Types.h"
#include "Core/Selection/CopyStyle.h"
#include "Core/Selection/Selection.h"
#include "Core/Selection/SelectionText.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// Selection → styled clipboard (RTF + HTML) policy: the portable generator that
// turns selected cells into RTF and HTML given a resolved per-cell style and a
// color mode. Pure — the caller supplies the grid through the same callbacks
// SelectionText uses, plus a StyledCellAt handing back each cell's FINAL 8-bit
// foreground/background (already resolved through the theme palette) and style
// bits. The generator never re-implements color resolution; it owns the
// run-length grouping, RTF/HTML escaping + framing, and the three color modes.

namespace Ubiquitty::Selection {

// One selected cell as the styled walk sees it: its code point, whether another
// cell already carries that glyph (a wide pair's trailing half, or a sized
// block's row below its top — either contributes no bytes), the final resolved
// foreground / background RGB, and the surviving text-style bits.
struct StyledCell {
    std::uint32_t codePoint;
    bool isContinuation;
    std::uint8_t foreground[3];
    std::uint8_t background[3];
    bool bold;
    bool italic;
    bool underline;
    // Only the first `codePointCount` entries are defined, as in TextCell.
    std::array<std::uint32_t, 16> codePoints;
    int codePointCount { 0 };
    // As in TextCell: a proportional run's cell's letters, in place of `codePoints`.
    std::span<const std::uint32_t> runCodePoints {};
    // As in TextCell: `runCodePoints` is what the selection holds, uncut.
    bool isUncut = false;
};

// Reads the fully-resolved style of (row, column). `row` is in the same absolute
// coordinate space SelectionText uses (absolute buffer rows on every frontend).
using StyledCellAt = StyledCell (*)(void* context, int row, int column);

// Font metadata the rich flavors embed. A null / empty face falls back to a
// generic monospace family so the output is still valid.
struct StyledFont {
    const char* faceName; // e.g. "SF Mono"; may be null
    double pointSize; // e.g. 13.0; <= 0 falls back to 12
};

// The three serialized flavors of one styled copy. `plain` matches
// SelectionText::selectionText; `rtf` is a complete RTF document; `html` a
// complete styled HTML fragment.
struct StyledClipboard {
    std::string plain;
    std::string rtf;
    std::string html;
};

/// A run's style after the color mode has been applied to it.
///
/// Which colors and which attribute bits actually survive into the output. Two adjacent runs comparing
/// equal here merge into one.
struct SurvivingStyle {
    bool hasForeground = false;
    bool hasBackground = false;
    std::array<std::uint8_t, 3> foreground = { 0, 0, 0 };
    std::array<std::uint8_t, 3> background = { 0, 0, 0 };
    bool isBold = false;
    bool isItalic = false;
    bool isUnderlined = false;

    /// Returns true when the two styles would emit identical framing.
    ///
    /// Compared as bytes, which the layout and the construction together make exact: the fields pack
    /// without padding, and a color the style does not carry stays zero rather than stale, so two styles
    /// agree byte for byte in the cases a field-by-field compare would call equal and in no others. The
    /// walk asks this once per cell, where a run of comparisons that each branch on which colors are
    /// present costs more than the whole record does to read.
    ///
    /// - Parameters:
    ///   - other: The style to compare against.
    /// - Returns: True when the colors and every attribute bit agree.
    bool operator==(const SurvivingStyle& other) const { return std::memcmp(this, &other, sizeof(SurvivingStyle)) == 0; }
};
// The byte comparison above is a claim about this layout: eleven bytes, one-byte aligned, nothing
// between the fields for a compare to read as a difference.
static_assert(sizeof(SurvivingStyle) == 11, "SurvivingStyle is compared as bytes and must not gain padding");
static_assert(alignof(SurvivingStyle) == 1, "SurvivingStyle is compared as bytes and must not gain alignment padding");

/// One run of consecutive selected cells sharing a style.
///
/// Addresses ``StyledRuns::text`` rather than the grid, so it stays valid once the selection is gone. A
/// break run carries no text and ends the line.
struct StyledRun {
    SurvivingStyle style;
    std::size_t textOffset = 0;
    std::size_t textLength = 0;
    bool isBreak = false;
};

/// Which serialized flavors an emission produces.
///
/// The plain text is not among them: the walk has it already, and no caller declines it.
enum class StyledFlavors : std::uint8_t {
    None = 0,
    RTF = 1 << 0,
    HTML = 1 << 1,
    All = RTF | HTML,
};

/// Returns true when `which` asks for `flavor`.
///
/// - Parameters:
///   - which: The requested set.
///   - flavor: The single flavor to test for.
/// - Returns: True when the flavor is in the set.
[[nodiscard]] constexpr bool wants(StyledFlavors which, StyledFlavors flavor)
{
    return (static_cast<std::uint8_t>(which) & static_cast<std::uint8_t>(flavor)) != 0;
}

/// A styled copy's runs, the code points they address, and the framing a flavor needs.
///
/// Self-contained by construction — no cell pointer, no row index, no borrowed string — so a caller may
/// hold one and serialize a flavor long after the selection was taken, on a buffer that has since moved.
/// That is what lets a frontend hand the system a promise rather than three documents nobody asked for.
struct StyledRuns {
    std::string plain;
    std::vector<StyledRun> runs;
    std::u32string text;
    std::string fontFaceName; // owned, not borrowed; already resolved through the fallback
    double fontPointSize = 0.0; // already resolved through the fallback
    StyleColorMode colorMode = StyleColorMode::Full;
    std::array<std::uint8_t, 3> defaultForeground = { 0, 0, 0 };
    std::array<std::uint8_t, 3> defaultBackground = { 0, 0, 0 };
};

/// Groups selected text into styled runs without serializing a flavor.
///
/// Reads the grid while the selected rows remain stable. Each parameter has
/// the same meaning as on `buildStyledClipboard`.
///
/// - Parameters:
///   - b: The selection's inclusive row bounds.
///   - mode: The selection's endpoint and clipping policy.
///   - columns: The grid width.
///   - cellAt: Reads a cell's leading code point, or zero for a void cell.
///   - cellContext: The scalar reader's context.
///   - textAt: Reads cluster text without resolving a style.
///   - textContext: The cluster reader's context.
///   - styledAt: Reads text and resolved style for an emitted cell.
///   - styledContext: The styled reader's context.
///   - newline: Controls joins across soft-wrapped rows.
///   - rowWrapped: Reports soft wrapping, or null for no joins.
///   - wrappedContext: The wrapping reader's context.
///   - colorMode: Selects the style fields retained in the output.
///   - font: The output font metadata.
///   - defaultForeground: The theme's foreground color.
///   - defaultBackground: The theme's paper color.
///   - rowExtent: Reports stored row width, or null to scan the selected span.
///   - extentContext: The row-extent reader's context.
/// - Returns: Owned text and runs that require no further grid access.
[[nodiscard]] StyledRuns buildStyledRuns(const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, TextCellAt textAt, void* textContext, StyledCellAt styledAt, void* styledContext, SoftWrapNewline newline, Ubiquitty::SelectionRowWrappedCallback rowWrapped,
    void* wrappedContext, StyleColorMode colorMode, const StyledFont& font, const std::array<std::uint8_t, 3>& defaultForeground, const std::array<std::uint8_t, 3>& defaultBackground, Ubiquitty::SelectionRowExtentAt rowExtent = nullptr, void* extentContext = nullptr);

/// Reverses grapheme order within each copied line, retaining styles and line order.
/// - Parameter walked: The selected runs, after soft-wrap joins and trimming.
void reverseCopyLines(StyledRuns& walked);

// Serializes the flavors `which` asks for from an already-walked selection.
//
// Reaches no grid: everything it reads is in `walked`, which is why a caller may redeem a promise with
// it after the buffer has moved. The plain text rides along from the walk whatever `which` says.
[[nodiscard]] StyledClipboard emitStyledFlavors(const StyledRuns& walked, StyledFlavors which);

/// Serializes selected text into plain, HTML, and RTF clipboard flavors.
///
/// Uses the same clipping and trailing-blank policy as plain selection text.
/// A run whose background matches the theme's paper omits its background span.
///
/// - Parameters:
///   - b: The selection's inclusive row bounds.
///   - mode: The selection's endpoint and clipping policy.
///   - columns: The grid width.
///   - cellAt: Reads a cell's leading code point, or zero for a void cell.
///   - cellContext: The scalar reader's context.
///   - textAt: Reads cluster text without resolving an unused style.
///   - textContext: The cluster reader's context.
///   - styledAt: Reads text and resolved style for an emitted cell.
///   - styledContext: The styled reader's context.
///   - newline: Controls joins across soft-wrapped rows.
///   - rowWrapped: Reports soft wrapping, or null for no joins.
///   - wrappedContext: The wrapping reader's context.
///   - colorMode: Selects the style fields retained in the output.
///   - font: The output font metadata.
///   - defaultForeground: The theme's foreground color.
///   - defaultBackground: The theme's paper color.
///   - rowExtent: Reports stored row width, or null to scan the selected span.
///   - extentContext: The row-extent reader's context.
/// - Returns: All three complete clipboard flavors.
[[nodiscard]] StyledClipboard buildStyledClipboard(const Ubiquitty::SelectionBounds* b, Ubiquitty::SelectionMode mode, int columns, Ubiquitty::SelectionCellAt cellAt, void* cellContext, TextCellAt textAt, void* textContext, StyledCellAt styledAt, void* styledContext, SoftWrapNewline newline, Ubiquitty::SelectionRowWrappedCallback rowWrapped,
    void* wrappedContext, StyleColorMode colorMode, const StyledFont& font, const std::array<std::uint8_t, 3>& defaultForeground, const std::array<std::uint8_t, 3>& defaultBackground, Ubiquitty::SelectionRowExtentAt rowExtent = nullptr, void* extentContext = nullptr);

} // namespace Ubiquitty::Selection
