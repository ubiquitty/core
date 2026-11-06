// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Accessibility/TextProjection.h"
#include "Core/CellPen.h"
#include "Core/OSC/TextSizing.h"
#include "Core/RichText/Blocks.h"
#include "Core/Screen/Addressing.h"

#include "Core/Cell.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/UTF8Decoder.h"
#include "Core/Unicode/UTF8Encoder.h"
#include "Core/VT/Constants.h"
#include <algorithm>
#include <array>

namespace Ubiquitty::Accessibility {

// UTF-16 code units for one codepoint: astral (>= U+10000) is a surrogate pair.
//
// A value that is not a scalar contributes nothing, because that is what
// appendUTF8 writes for it — and this count indexes the string appendUTF8 built.
// The two have to answer with the same set or a row declares a length its own
// bytes do not encode, and every later row's offset carries the difference. Those
// offsets are how a screen reader maps a selection back to text.
//
// Stated here rather than relied on from upstream: cells arrive validated from
// both producers today, and this holds for a producer that forgets.
static inline uint32_t utf16Units(uint32_t codePoint)
{
    if (!Ubiquitty::Unicode::isScalarValue(codePoint))
        return 0;
    return codePoint >= 0x10000u ? 2u : 1u;
}

TextProjection projectVisibleText(const Ubiquitty::Terminal& term, int viewOffset)
{
    TextProjection out;
    if (term.rows <= 0 || term.columns <= 0)
        return out;
    viewOffset = Screen::resolvedViewOffset(&term, viewOffset);

    const int rowCount = term.rows;
    const int columnCount = term.columns;
    out.columns = columnCount;
    out.lineRanges.reserve(static_cast<size_t>(rowCount));
    out.cellBoundaries.resize(static_cast<size_t>(rowCount) * (static_cast<size_t>(columnCount) + 1));
    uint32_t runningUTF16 = 0; // UTF-16 offset of the next row in out.text

    for (const auto& block : term.richTextBlocks.blocks(RichText::activeScreen(term))) {
        if (block.row + viewOffset >= rowCount || block.row + viewOffset + block.height <= 0)
            continue;
        const auto& layout = RichText::layoutOf(term, block);
        RichTextProjection capture { .block = block, .typographyEpoch = term.typography.epoch, .viewportTop = static_cast<std::uint32_t>(layout.firstVisibleLine()), .isShaped = layout.isShaped(), .isLayoutPending = term.typography.facts && term.typography.facts->shaping && !layout.isShaped(), .lines = {}, .spans = {}, .visibleRanges = {} };
        capture.block.row += viewOffset;
        for (std::size_t index = 0; index < layout.lineCount(); ++index)
            capture.lines.push_back(layout.line(index));
        capture.spans.assign(layout.shapedSpans().begin(), layout.shapedSpans().end());
        if (!capture.isLayoutPending) {
            for (std::size_t index = layout.firstVisibleLine(); index < layout.visibleLineCount(); ++index) {
                const auto line = layout.line(index);
                const int row = capture.block.row + static_cast<int>((line.top - layout.viewportTop()) / layout.cellHeight());
                if (row < 0 || row >= rowCount)
                    continue;
                const std::array full { RichText::VisibleRect { 0, 0, block.width, block.height } };
                const auto rectangles = block.visibleRects.empty() ? std::span<const RichText::VisibleRect>(full) : std::span<const RichText::VisibleRect>(block.visibleRects);
                const auto appendRange = [&](std::uint32_t first, std::uint32_t end, std::int64_t left, std::int64_t right) {
                    for (const auto& rectangle : rectangles) {
                        if (row < capture.block.row + rectangle.y || row >= capture.block.row + rectangle.y + rectangle.height
                            || left >= (rectangle.x + rectangle.width) * layout.cellWidth() || std::max(left + 1, right) <= rectangle.x * layout.cellWidth())
                            continue;
                        const int column = block.column + rectangle.x;
                        if (!capture.visibleRanges.empty() && capture.visibleRanges.back().row == row && capture.visibleRanges.back().column == column && capture.visibleRanges.back().end == first)
                            capture.visibleRanges.back().end = end;
                        else
                            capture.visibleRanges.push_back({ .first = first, .end = end, .row = row, .column = column });
                        break;
                    }
                };
                if (!layout.isShaped()) {
                    for (auto first = line.first; first < line.end;) {
                        const auto end = std::min(layout.graphemeEnd(first), line.end);
                        appendRange(first, end, line.origin + layout.leftEdge(line, first), line.origin + layout.rightEdge(end - 1));
                        first = end;
                    }
                    continue;
                }
                for (const auto& span : layout.shapedSpans()) {
                    if (span.line == index)
                        appendRange(span.first, span.end, std::min(span.leading, span.trailing), std::max(span.leading, span.trailing));
                }
            }
        }
        out.richText.push_back(std::move(capture));
    }
    std::ranges::sort(out.richText, [](const auto& left, const auto& right) {
        return left.block.row == right.block.row ? left.block.column < right.block.column : left.block.row < right.block.row;
    });

    for (int row = 0; row < rowCount; ++row) {
        std::string line;
        uint32_t lineUTF16 = 0;
        std::vector<RichTextProjection*> rowBlocks;
        for (auto& capture : out.richText) {
            if (row >= capture.block.row && row < capture.block.row + capture.block.height)
                rowBlocks.push_back(&capture);
        }
        std::ranges::sort(rowBlocks, [](const auto* left, const auto* right) { return left->block.column < right->block.column; });
        auto boundaries = std::span(out.cellBoundaries).subspan(static_cast<size_t>(row) * (static_cast<size_t>(columnCount) + 1), static_cast<size_t>(columnCount) + 1);
        const auto append = [&](uint32_t codePoint) {
            Unicode::appendUTF8(line, codePoint);
            lineUTF16 += utf16Units(codePoint);
        };
        for (int column = 0; column < columnCount; ++column) {
            boundaries[static_cast<size_t>(column)] = { lineUTF16, line.size() };
            const auto found = std::ranges::find_if(rowBlocks, [&](const auto* block) { return block->block.covers(row, column); });
            if (found != rowBlocks.end()) {
                auto* block = *found;
                for (auto& range : block->visibleRanges) {
                    if (range.row != row || range.column != column)
                        continue;
                    range.location = runningUTF16 + lineUTF16;
                    for (auto index = range.first; index < range.end; ++index) {
                        const auto codePoint = block->block.document.text[index];
                        if (!RichText::isLineBreak(codePoint))
                            append(codePoint);
                    }
                }
                continue;
            }
            // `visibleCell` answers a blank cell rather than null for every
            // address it cannot resolve, asserted in this projection's cases so
            // reading through the pointer stays checked somewhere.
            const Cell* cell = Ubiquitty::Screen::visibleCell(&term, viewOffset, row, column);
            if (Ubiquitty::isTextContinuation(*cell))
                continue; // another cell already carries this glyph
            const auto run = runTextCodePoints(&term, *cell);
            if (!run.empty()) {
                const auto* spec = term.textSizeTable.lookup(cell->pen.extendedAttributes.textSizeSpecID());
                const int width = claimedRunColumns(*cell, &term.textSizeTable, term.ambiguousWidth);
                // A scaled run's letters are laid out in its own cells, each
                // `scale` columns of the row.
                const int scale = spec ? std::max<int>(spec->scale, 1) : 1;
                std::vector<uint16_t> owners(run.size());
                if (!spec || !spec->isProportional || !OSC::TextSizing::proportionalRunCellOwners(term, run, std::max(1, width / scale), spec->horizontalAlign, OSC::TextSizing::runStyleOf(cell->pen), owners)
                    || !std::ranges::is_sorted(owners))
                    std::ranges::fill(owners, uint16_t { 0 });
                for (uint16_t& owner : owners)
                    owner = static_cast<uint16_t>(owner * scale);
                const int endColumn = column + std::min(width, columnCount - column);
                size_t next = 0;
                for (int runColumn = column; runColumn < endColumn; ++runColumn) {
                    boundaries[static_cast<size_t>(runColumn)] = { lineUTF16, line.size() };
                    while (next < run.size() && (owners[next] <= runColumn - column || runColumn + 1 == endColumn))
                        append(run[next++]);
                }
                column = endColumn - 1;
                continue;
            }
            if (cell->codePoint == wideContinuationSentinelCodePoint) {
                line.push_back(' ');
                lineUTF16 += 1;
                continue;
            }
            std::array<uint32_t, clusterPoolMaximumCodePoints> cluster;
            const int count = cellClusterCodePoints(&term, cell, cluster.data(), static_cast<int>(cluster.size()));
            for (int index = 0; index < count; ++index)
                append(cluster[static_cast<size_t>(index)]);
        }
        boundaries.back() = { lineUTF16, line.size() };
        // Trailing-space trim: every trailing char removed here is a space
        // (1 byte, 1 UTF-16 unit), so the UTF-16 length drops by the byte count.
        const std::size_t oldSize = line.size();
        const std::size_t lastGlyph = line.find_last_not_of(' ');
        if (lastGlyph == std::string::npos)
            line.clear();
        else
            line.resize(lastGlyph + 1);
        lineUTF16 -= static_cast<uint32_t>(oldSize - line.size());
        for (auto& boundary : boundaries) {
            boundary.location = runningUTF16 + std::min(boundary.location, lineUTF16);
            boundary.byteOffset = out.text.size() + std::min(boundary.byteOffset, line.size());
        }
        // A scaled run's text is read on its top band, and its lower bands
        // show the same letters: a position on a lower band is the letter
        // boundary above it, so a selection made on any band names the letters
        // it holds.
        for (int column = 0; column < columnCount; ++column) {
            const Cell* cell = Ubiquitty::Screen::visibleCell(&term, viewOffset, row, column);
            const int band = static_cast<int>(penTextBand(&cell->pen));
            if (band == 0 || band > row)
                continue;
            const auto* spec = term.textSizeTable.lookup(cell->pen.extendedAttributes.textSizeSpecID());
            if (!spec || !spec->isProportional || spec->scale < 2)
                continue;
            boundaries[static_cast<size_t>(column)] = out.cellBoundaries[static_cast<size_t>(row - band) * (static_cast<size_t>(columnCount) + 1) + static_cast<size_t>(column)];
        }

        out.lineRanges.push_back(LineRange { runningUTF16, lineUTF16 });
        out.text += line;
        runningUTF16 += lineUTF16;
        if (row + 1 < rowCount) {
            out.text.push_back('\n');
            runningUTF16 += 1;
        }
    }
    return out;
}

std::string visibleScreenText(const Ubiquitty::Terminal& term, int viewOffset)
{
    return projectVisibleText(term, viewOffset).text;
}

ProjectionRange positionInProjection(const TextProjection& projection, std::ptrdiff_t row, int column, int subColumn)
{
    if (row < 0 || static_cast<size_t>(row) >= projection.lineRanges.size() || projection.columns <= 0)
        return {};
    const auto width = static_cast<size_t>(projection.columns) + 1;
    const auto cell = static_cast<size_t>(row) * width + static_cast<size_t>(std::clamp(column, 0, projection.columns));
    if (cell >= projection.cellBoundaries.size())
        return {};
    const auto& boundary = projection.cellBoundaries[cell];
    uint32_t location = boundary.location;
    if (subColumn > 0 && column >= 0 && column < projection.columns && cell + 1 < projection.cellBoundaries.size()) {
        const size_t end = std::min(projection.cellBoundaries[cell + 1].byteOffset, projection.text.size());
        Unicode::UTF8Decoder decoder {};
        for (size_t byte = boundary.byteOffset; byte < end && subColumn > 0; ++byte) {
            uint32_t codePoint = 0;
            if (Unicode::utf8Feed(&decoder, static_cast<uint8_t>(projection.text[byte]), &codePoint) == Unicode::UTF8Status::Emit) {
                location += utf16Units(codePoint);
                --subColumn;
            }
        }
    }
    return { location, 0, true };
}

ProjectionRange caretInProjection(const TextProjection& projection, const Ubiquitty::Terminal& term, std::ptrdiff_t viewOffset)
{
    if (viewOffset != 0 || !term.screens.active)
        return {};
    const int cursorRow = term.screens.active->cursorY;
    if (cursorRow < 0 || cursorRow >= static_cast<int>(projection.lineRanges.size()))
        return {};
    for (const auto& capture : projection.richText) {
        const auto caret = capture.block.caret;
        if (caret == 65535 || !capture.block.covers(cursorRow, term.screens.active->cursorX))
            continue;
        for (const auto& range : capture.visibleRanges) {
            if (caret < range.first || caret > range.end)
                continue;
            auto location = range.location;
            for (std::uint32_t index = range.first; index < caret; ++index) {
                if (!RichText::isLineBreak(capture.block.document.text[index]))
                    location += utf16Units(capture.block.document.text[index]);
            }
            const auto& line = projection.lineRanges[static_cast<std::size_t>(range.row)];
            return { std::min(location, line.location + line.length), 0, true };
        }
    }
    return positionInProjection(projection, cursorRow, term.screens.active->cursorX);
}

} // namespace Ubiquitty::Accessibility
