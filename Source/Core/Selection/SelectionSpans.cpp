// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Selection/SelectionSpans.h"

#include "Core/RowMeta.h"

#include <algorithm>

namespace Ubiquitty::Selection {

namespace {

// The spans of a double-height pair's two rows made one: see the collector's
// `lineAttributeAt` contract.
int mirrorSpansAcrossDoubleHeightPairs(const Viewport& viewport, int written, ViewportSpans out,
    SelectionRowLineAttributeAt lineAttributeAt, void* lineAttributeContext)
{
    int count = written;
    for (int index = 0; index < written; ++index) {
        const Rendering::SelectionRowSpan span = out[static_cast<std::size_t>(index)];
        const int row = viewport.firstAbsoluteRow + span.visibleRow;
        const std::uint8_t attribute = lineAttributeAt(lineAttributeContext, row);
        int otherVisibleRow = -1;
        if (attribute == lineAttributeDoubleHeightTop && span.visibleRow + 1 < viewport.rows
            && lineAttributeAt(lineAttributeContext, row + 1) == lineAttributeDoubleHeightBottom) {
            otherVisibleRow = span.visibleRow + 1;
        } else if (attribute == lineAttributeDoubleHeightBottom && span.visibleRow > 0
            && lineAttributeAt(lineAttributeContext, row - 1) == lineAttributeDoubleHeightTop) {
            otherVisibleRow = span.visibleRow - 1;
        }
        if (otherVisibleRow < 0)
            continue;
        const auto other = std::find_if(out.begin(), out.begin() + count,
            [otherVisibleRow](const Rendering::SelectionRowSpan& candidate) { return candidate.visibleRow == otherVisibleRow; });
        if (other != out.begin() + count) {
            if (attribute == lineAttributeDoubleHeightTop) {
                other->column0 = span.column0;
                other->column1 = span.column1;
                other->subColumn0 = span.subColumn0;
                other->subColumn1 = span.subColumn1;
            }
            continue;
        }
        if (count >= static_cast<int>(out.size()))
            continue;
        out[static_cast<std::size_t>(count++)]
            = Rendering::SelectionRowSpan { otherVisibleRow, span.column0, span.column1, span.subColumn0, span.subColumn1 };
    }
    if (count > written) {
        std::sort(out.begin(), out.begin() + count,
            [](const Rendering::SelectionRowSpan& left, const Rendering::SelectionRowSpan& right) { return left.visibleRow < right.visibleRow; });
    }
    return count;
}

} // namespace

int collectViewportSpans(
    const SelectionBounds& bounds,
    SelectionMode mode,
    const Viewport& viewport,
    SelectionCellAt cellAt,
    void* cellContext,
    SelectionAttributesAt attributesAt,
    void* attributeContext,
    ViewportSpans out,
    SelectionRowLineAttributeAt lineAttributeAt,
    void* lineAttributeContext)
{
    if (viewport.rows <= 0 || viewport.columns <= 0 || out.empty())
        return 0;

    // The intersection of the selection and the viewport, in absolute rows. A
    // selection anchored deep in scrollback resolves to bounds spanning that
    // whole distance, and walking them would cost the scrollback per frame to
    // discover that almost none of it is on screen.
    const int lastAbsoluteRow = viewport.firstAbsoluteRow + viewport.rows - 1;
    const int firstRow = bounds.row0 > viewport.firstAbsoluteRow ? bounds.row0 : viewport.firstAbsoluteRow;
    const int lastRow = bounds.row1 < lastAbsoluteRow ? bounds.row1 : lastAbsoluteRow;

    int written = 0;
    for (int absoluteRow = firstRow; absoluteRow <= lastRow; ++absoluteRow) {
        if (written == static_cast<int>(out.size()))
            break;
        const int visibleRow = absoluteRow - viewport.firstAbsoluteRow;
        if (visibleRow < 0 || visibleRow >= viewport.rows)
            continue;
        const RowSpan span = rowSpan(
            &bounds, mode, absoluteRow, viewport.columns, cellAt, cellContext, attributesAt, attributeContext);
        // Two separate refusals, and neither is the other. `selected` is the
        // per-row verdict `rowSpan` reaches after its own void rules — which
        // CLAMP a highlight to the last glyph or EXPAND an empty row to the
        // whole line, rather than dropping rows for being blank.
        //
        // The inverted span is the case those rules produce and do not
        // themselves reject: a top endpoint far enough past the content sets
        // column0 to `columns`, leaving a span whose start is past its end.
        // Emitting it draws a quad of negative width, which reads as a shader
        // fault rather than as a row that should not have been there.
        if (!span.selected || span.column1 < span.column0)
            continue;
        // The cuts inside the two end cells, under the rule the copy reads them
        // by (``packedCellCut``): a character selection's, on the bounds' own
        // end cell, where the row's span still ends on it.
        const bool mayCut = mode == SelectionMode::Character;
        const int subColumn0 = mayCut && absoluteRow == bounds.row0 && span.column0 == bounds.column0 ? bounds.subColumn0 : 0;
        const int subColumn1 = mayCut && absoluteRow == bounds.row1 && span.column1 == bounds.column1 ? bounds.subColumn1 : 0;
        out[static_cast<std::size_t>(written)] = Rendering::SelectionRowSpan { visibleRow, span.column0, span.column1, subColumn0, subColumn1 };
        ++written;
    }
    if (written > 0 && lineAttributeAt)
        return mirrorSpansAcrossDoubleHeightPairs(viewport, written, out, lineAttributeAt, lineAttributeContext);
    return written;
}

} // namespace Ubiquitty::Selection
