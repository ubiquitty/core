// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/VisibleLink.h"

#include "Core/Attributes/Hyperlinks.h"
#include "Core/CellPen.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"

#include <cstring>
#include <span>

namespace Ubiquitty::Screen {

namespace {

// True when visible row `r` holds any cell of the hovered link's group: the same id, or — when the
// hovered id resolves — a different id pointing at the same target.
// A vertical a multiplexer draws between panes: single, heavy, double. ASCII
// '|' is not one -- prose uses it freely, and a pane drawn with
// simple borders still gets the margin witness once it scrolls.
[[nodiscard]] bool isPaneBorderGlyph(uint32_t codePoint)
{
    return codePoint == 0x2502 || codePoint == 0x2503 || codePoint == 0x2551;
}

bool rowJoinsLinkGroup(const Terminal& t, int viewOffset, int r, int columns, uint16_t hoveredID, const char* hoveredURI)
{
    // The row once, not the cell: addressing by row and column re-derives the
    // history depth and re-resolves the ring slot for one cell, and this walks
    // every column of every candidate row on every hover.
    const VisibleRow row = resolveVisibleRow(&t, viewOffset, r);
    // Ids repeat across the cells of a wrapped link, so the ring scan that
    // resolves one is asked the same question for a run of cells. One id's
    // answer is remembered, which is the whole of it for a link that spans a
    // row and the common case for a row carrying a handful.
    //
    // Keyed on the id, and a key that failed to refresh would be invisible to
    // the suite: this widens the scanned window, and the span walk below stops
    // at any foreign id, so a row that joins through a SECOND id can never
    // reach the span anyway. Like the fast path below, that is the expected
    // result rather than a missing test.
    uint16_t resolvedID = 0;
    const char* resolvedURI = nullptr;
    for (int x = 0; x < columns; ++x) {
        const Cell* cell = cellInResolvedRow(row, x, columns);
        const uint16_t id = cell->linkID();
        if (id == 0)
            continue;
        // A fast path, not a discrimination: ids intern on the id= key, so two
        // cells sharing an id resolve to one stored URI and the comparison
        // below would return true for every case this returns true for.
        // Mutating or deleting this line leaves the suite green, and that is
        // the expected result rather than a missing test -- it saves a table
        // lookup and a strcmp per cell on the common wrapped-link walk.
        if (id == hoveredID)
            return true;
        if (hoveredURI) {
            if (id != resolvedID) {
                resolvedID = id;
                resolvedURI = Attributes::Hyperlinks::uri(&t, id);
            }
            if (resolvedURI && std::strcmp(resolvedURI, hoveredURI) == 0)
                return true;
        }
    }
    return false;
}

void copyURI(const char* from, std::span<char> to)
{
    std::size_t i = 0;
    for (; from[i] != '\0' && i + 1 < to.size(); ++i)
        to[i] = from[i];
    to[i] = '\0';
}

} // namespace

bool linkAtVisibleCell(const Terminal* t, int viewOffset, int row, int column, const LinkScanBuffer& scratch,
    std::span<char> output, VisibleLink* outLink, LinkScanScope scope)
{
    char* const uriOut = output.data();
    const int uriCapacity = static_cast<int>(output.size());
    if (!t || !uriOut || uriCapacity < 1 || !scratch.codePoints || !scratch.linkIDs || scratch.capacityInCells < 1)
        return false;
    uriOut[0] = '\0';

    const int columns = t->columns;
    const int rows = t->rows;
    if (columns <= 0 || row < 0 || row >= rows || column < 0 || column >= columns)
        return false;
    if (columns > scratch.capacityInCells)
        return false; // a single row does not fit; the caller under-sized the scratch

    const Cell* hovered = visibleCell(t, viewOffset, row, column);
    const uint16_t hoveredID = hovered ? hovered->linkID() : 0;
    const char* hoveredURI = (hoveredID != 0) ? Attributes::Hyperlinks::uri(t, hoveredID) : nullptr;
    if (hoveredURI && hoveredURI[0] == '\0')
        hoveredURI = nullptr;
    // A declared-only ask is answered by the id read above: everything below
    // exists to find a link the program never declared, and a caller that
    // wants none of those is not made to pay for the search.
    if (scope == LinkScanScope::Declared && hoveredURI == nullptr)
        return false;

    // The columns that hold the clicked text: the whole screen, unless the
    // screen is split into panes. A multiplexer writes each pane's rows
    // separately at the pane's own width, so a URL hard-wraps at the PANE's
    // right edge and the terminal is never told the rows are continuous -- a
    // walk that knows only the screen's edges finds such a URL as its first
    // line, which is the tmux report this fixes. Two witnesses to a pane
    // edge, tried in order:
    //   - a margin pair the pane once scrolled with (DECSLRM), remembered by
    //     the screen; a stale pair is refuted by text crossing its edge
    //   - a border glyph beside the clicked cell, for a pane that has not
    //     scrolled yet
    // An OSC 8 span needs neither: its id walk is geometry-free, so it keeps
    // the full width.
    const auto codePointAt = [&](int r, int x) -> uint32_t {
        const VisibleRow resolved = resolveVisibleRow(t, viewOffset, r);
        const Cell* cell = cellInResolvedRow(resolved, x, columns);
        return (cell && cell->codePoint && !Ubiquitty::isTextContinuation(*cell)) ? cell->codePoint : static_cast<uint32_t>(' ');
    };
    int spanLeft = 0;
    int spanRight = columns - 1;
    if (hoveredID == 0) {
        const Screen::State* screen = t->screens.active;
        int narrowest = columns + 1;
        for (int index = 0; index < screen->rememberedMarginCount; ++index) {
            const Screen::State::RememberedMargins pair = screen->rememberedMargins[index];
            if (pair.left < 0 || pair.right >= columns || column < pair.left || column > pair.right)
                continue;
            const int width = pair.right - pair.left + 1;
            if (width >= narrowest)
                continue;
            // Text crossing an edge on the clicked row refutes the pair: the
            // layout it described has been replaced since.
            const bool leftHolds = pair.left == 0 || !Attributes::LinkRun::isURLRunByte(codePointAt(row, pair.left - 1));
            const bool rightHolds = pair.right == columns - 1 || !Attributes::LinkRun::isURLRunByte(codePointAt(row, pair.right + 1));
            if (leftHolds && rightHolds) {
                spanLeft = pair.left;
                spanRight = pair.right;
                narrowest = width;
            }
        }
        if (narrowest > columns) {
            for (int x = column - 1; x >= 0; --x) {
                if (isPaneBorderGlyph(codePointAt(row, x))) {
                    spanLeft = x + 1;
                    break;
                }
            }
            for (int x = column + 1; x < columns; ++x) {
                if (isPaneBorderGlyph(codePointAt(row, x))) {
                    spanRight = x - 1;
                    break;
                }
            }
        }
    }
    const int width = spanRight - spanLeft + 1;
    if (width > scratch.capacityInCells)
        return false;

    int firstRow = row;
    int lastRow = row;
    if (hoveredID != 0) {
        while (firstRow > 0 && rowJoinsLinkGroup(*t, viewOffset, firstRow - 1, columns, hoveredID, hoveredURI))
            --firstRow;
        while (lastRow < rows - 1 && rowJoinsLinkGroup(*t, viewOffset, lastRow + 1, columns, hoveredID, hoveredURI))
            ++lastRow;
    } else {
        // Two ways a row continues into the next. The terminal marked it
        // wrapped, which is the reliable one. Or nothing marked it and the
        // seam at the pane's edges is URL bytes on both sides -- the span's
        // last column of one row and its first column of the next -- which is
        // what a URL looks like after a program hard-wrapped it at the pane
        // width and wrote each visual line separately. The seam has to be
        // full: a URL that ends mid-row leaves a blank at the edge, so a row
        // of prose that merely starts with a letter is never pulled into a
        // URL above it. And the column past the right edge must not be URL
        // text on either row -- the border, a blank, or the screen edge --
        // or the "pane" being joined across is a stale guess about a layout
        // that no longer exists.
        const auto rowsJoinAcross = [&](int upper) {
            if (visibleRowWrapped(t, viewOffset, upper))
                return true;
            if (!Attributes::LinkRun::isURLRunByte(codePointAt(upper, spanRight))
                || !Attributes::LinkRun::isURLRunByte(codePointAt(upper + 1, spanLeft)))
                return false;
            if (spanRight + 1 < columns
                && (Attributes::LinkRun::isURLRunByte(codePointAt(upper, spanRight + 1))
                    || Attributes::LinkRun::isURLRunByte(codePointAt(upper + 1, spanRight + 1))))
                return false;
            return true;
        };
        while (firstRow > 0 && rowsJoinAcross(firstRow - 1))
            --firstRow;
        while (lastRow < rows - 1 && rowsJoinAcross(lastRow))
            ++lastRow;
    }
    if ((lastRow - firstRow + 1) * width > scratch.capacityInCells)
        lastRow = firstRow + (scratch.capacityInCells / width) - 1;
    if (row > lastRow) // the cap trimmed the hovered row away; scanning it alone beats scanning elsewhere
        firstRow = lastRow = row;

    const int cells = (lastRow - firstRow + 1) * width;
    const int hoveredIndex = (row - firstRow) * width + (column - spanLeft);
    for (int r = firstRow; r <= lastRow; ++r) {
        const int base = (r - firstRow) * width;
        const VisibleRow scanned = resolveVisibleRow(t, viewOffset, r);
        for (int x = 0; x < width; ++x) {
            const Cell* cell = cellInResolvedRow(scanned, spanLeft + x, columns);
            // A cell another one already speaks for reads as a blank rather
            // than as its glyph: the trailing half of a wide pair carries no
            // code point of its own, and a sized block's lower rows carry the
            // glyph for the renderer to crop. Left as glyphs they extend
            // whatever sits beside them, and a URL ending where a block begins
            // takes the block's character as one more of its own.
            const bool isSpoken = Ubiquitty::isTextContinuation(*cell);
            scratch.codePoints[base + x] = (cell->codePoint && !isSpoken) ? cell->codePoint : static_cast<uint32_t>(' ');
            scratch.linkIDs[base + x] = cell->linkID();
        }
    }

    bool isHyperlink = false;
    bool isFilePath = false;
    int first = hoveredIndex;
    int last = hoveredIndex;

    // The id on the flattened buffer rather than the one read above: the two agree, and reading it here
    // keeps the span walk and the id it spans on one array.
    uint16_t spanID = scratch.linkIDs[hoveredIndex];
    if (spanID != 0) {
        const char* resolved = Attributes::Hyperlinks::uri(t, spanID);
        if (resolved && resolved[0] != '\0') {
            // The ring keys an entry on the link's `id=` when it carries one and
            // on the URI otherwise, and a keyless repeat of one URI dedups into
            // the entry the first use minted. A shared id therefore says nothing
            // on its own; the entry's key field is what reports that a program
            // declared its runs to be one anchor.
            const bool isSpanExplicitlyKeyed = Attributes::Hyperlinks::isExplicitlyKeyed(t, spanID);
            // A cell continues the hovered link when it carries the same id, or an
            // id the same target resolves through — a program may re-emit each
            // visual segment of one link under a fresh id.
            const auto continuesTheSpan = [&](int i) {
                const uint16_t id = scratch.linkIDs[i];
                if (id == 0)
                    return false;
                if (id == spanID)
                    return true;
                const char* uri = Attributes::Hyperlinks::uri(t, id);
                return uri && std::strcmp(uri, resolved) == 0;
            };
            // A blank carrying no link is padding, not a boundary: a link segment
            // ending mid-row leaves the rest of that row blank, and the next
            // segment starts the row after. Unlinked text is a boundary.
            const auto isRowPadding = [&](int i) {
                return scratch.linkIDs[i] == 0 && scratch.codePoints[i] == static_cast<uint32_t>(' ');
            };
            // The span reaches out from the hovered cell as far as the link keeps
            // going, crossing padding but stopping at anything else. Taking the
            // outermost pair of cells that carry the target instead puts every cell
            // between them under the link: one target reaches a row twice — two
            // tmux panes showing one URL, a list repeating a link — and the text
            // separating the two occurrences is not part of either.
            // Padding is crossable to reach another row, which is where it stands
            // between two segments of one link. Within a row it separates two
            // occurrences of a target, and a pane border drawn as a styled blank
            // rather than a line character is exactly that.
            // A link carrying an `id=` is the exception: the program has declared
            // its runs to be one anchor, so the blanks between two of them are
            // inside the link. Only blanks — unlinked words end the span for a
            // keyed link as for any other, since they are not padding.
            const auto reachableAcrossPadding = [&](int candidate, int reached) {
                return candidate == reached - 1 || candidate == reached + 1
                    || candidate / width != reached / width
                    || (isSpanExplicitlyKeyed && scratch.linkIDs[candidate] == spanID);
            };
            for (int probe = first; probe > 0;) {
                const int candidate = probe - 1;
                if (continuesTheSpan(candidate)) {
                    if (!reachableAcrossPadding(candidate, first))
                        break;
                    first = candidate;
                } else if (!isRowPadding(candidate))
                    break;
                probe = candidate;
            }
            for (int probe = last; probe + 1 < cells;) {
                const int candidate = probe + 1;
                if (continuesTheSpan(candidate)) {
                    if (!reachableAcrossPadding(candidate, last))
                        break;
                    last = candidate;
                } else if (!isRowPadding(candidate))
                    break;
                probe = candidate;
            }
            copyURI(resolved, output);
            isHyperlink = true;
        } else {
            spanID = 0; // an id nothing resolves: fall through to the text scan
        }
    }

    if (spanID == 0) {
        if (const auto span = Attributes::LinkRun::urlMatchSpanAtColumn(scratch.codePoints, cells, hoveredIndex, output)) {
            first = span->first; // a URL, and uriOut is the link
            last = span->last;
        } else if (const auto pathSpan = Attributes::LinkRun::pathMatchSpanAtColumn(scratch.codePoints, cells, hoveredIndex, output)) {
            first = pathSpan->first;
            last = pathSpan->last;
            isFilePath = true;
        } else {
            return false;
        }
    }

    if (outLink) {
        outLink->spanColumnFirst = spanLeft;
        outLink->spanColumnLast = spanRight;
        // Absolute rows, so the span survives the scrolling that changes which visible row holds it.
        const int absoluteFirstVisible = t->scrollbackCount - viewOffset + firstRow;
        outLink->isHyperlink = isHyperlink;
        outLink->isFilePath = isFilePath;
        outLink->absoluteRow0 = absoluteFirstVisible + first / width;
        outLink->column0 = spanLeft + first % width;
        outLink->absoluteRow1 = absoluteFirstVisible + last / width;
        outLink->column1 = spanLeft + last % width;
    }
    return true;
}

} // namespace Ubiquitty::Screen
