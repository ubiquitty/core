// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Rendering/HintToastLayout.h"

#include "Core/Pen.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/Width.h"

#include <array>
#include <span>

namespace Ubiquitty::Rendering {

namespace {

// A border on each side and a space of padding inside it.
constexpr int chromeColumns = 4;
// The border rows above and below the text.
constexpr int chromeRows = 2;

// A packed 0xRRGGBB becomes a truecolor Pen — the same shape an SGR 38;2 cell
// carries, so no new render path is involved.
Pen penFor(std::uint32_t foreground, std::uint32_t background)
{
    Pen pen {};
    pen.foregroundIndex = colorRGB;
    pen.foregroundRed = static_cast<std::uint8_t>((foreground >> 16) & 0xFF);
    pen.foregroundGreen = static_cast<std::uint8_t>((foreground >> 8) & 0xFF);
    pen.foregroundBlue = static_cast<std::uint8_t>(foreground & 0xFF);
    pen.backgroundIndex = colorRGB;
    pen.backgroundRed = static_cast<std::uint8_t>((background >> 16) & 0xFF);
    pen.backgroundGreen = static_cast<std::uint8_t>((background >> 8) & 0xFF);
    pen.backgroundBlue = static_cast<std::uint8_t>(background & 0xFF);
    return pen;
}

// The columns the text occupies once drawn. The hint carries the modifier
// glyphs (⌘ ⌥ ⇧ ⌃) and a separator, so a byte count would size the box wrong
// on every one of them.
[[nodiscard]] int textColumns(std::string_view text)
{
    int columns = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto byte = static_cast<unsigned char>(text[at]);
        if (byte < 0x80) {
            // ASCII, including the space the decoder below stops at.
            columns += 1;
            ++at;
            continue;
        }
        std::array<std::uint32_t, 16> codePoints {};
        int bytesConsumed = 0;
        const int decoded = Ubiquitty::Unicode::utf8DecodeRun(
            std::span<const char> { text.data() + at, text.size() - at },
            std::span<std::uint32_t> { codePoints }, &bytesConsumed);
        if (decoded <= 0 || bytesConsumed <= 0) {
            // An invalid byte draws U+FFFD, which is one column wide.
            columns += 1;
            ++at;
            continue;
        }
        for (int i = 0; i < decoded; ++i) {
            // The measure `drawText` places the toast's cells with.
            const int width = Ubiquitty::Unicode::Width::forCodePoint(codePoints[static_cast<std::size_t>(i)], Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow);
            columns += width > 0 ? width : 0;
        }
        at += static_cast<std::size_t>(bytesConsumed);
    }
    return columns;
}

}

HintToastGeometry layoutHintToast(std::string_view text, const ChromeStyle& style, int gridRows, int gridColumns)
{
    HintToastGeometry geometry;
    if (text.empty() || gridRows <= 0 || gridColumns <= 0)
        return geometry;

    const int columns = textColumns(text) + chromeColumns;
    const int rows = 1 + chromeRows;
    // A grid that cannot hold the whole hint draws none of it: half a sentence
    // teaching a gesture teaches nothing, and eliding it would leave the reader
    // knowing only that something was withheld.
    if (columns > gridColumns || rows > gridRows)
        return geometry;

    geometry.canvas.resizeAndBlank(rows, columns);
    const Pen surfacePen = penFor(style.text, style.surface);
    const Pen borderPen = penFor(style.border, style.surface);
    const GridRect box { .row = 0, .column = 0, .width = columns, .height = rows };
    fill(geometry.canvas, box, surfacePen);
    drawBox(geometry.canvas, box, BoxStyle::Rounded, borderPen);
    drawText(geometry.canvas, 1, 2, text, surfacePen);

    // Bottom right, held off the edge by a cell where the grid affords one. A
    // box flush against the corner reads as part of the frame rather than as
    // something the terminal is saying.
    const int rowMargin = gridRows > rows ? 1 : 0;
    const int columnMargin = gridColumns > columns ? 1 : 0;
    geometry.originRow = gridRows - rows - rowMargin;
    geometry.originColumn = gridColumns - columns - columnMargin;
    return geometry;
}

} // namespace Ubiquitty::Rendering
