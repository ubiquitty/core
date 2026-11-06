// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/RichText/Blocks.h"

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/EffectGate.h"
#include "Core/OSC/RichTextBlocks.h"
#include "Core/RowMeta.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Screen/SizedBlock.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>

namespace Ubiquitty::RichText {

void requestBlockMeasurements(Terminal& t)
{
    if (!t.typography.facts || !t.typography.facts->shaping || t.typography.shaper.function)
        return;
    const auto blocks = t.richTextBlocks.blocks(activeScreen(t));
    const auto retired = std::ranges::remove_if(t.richTextMeasurements, [&](const auto& pending) {
        return pending.blockSerial && (pending.typographyEpoch != t.typography.epoch || std::ranges::none_of(blocks, [&](const auto& block) { return block.serial == pending.blockSerial; }));
    });
    t.richTextMeasurements.erase(retired.begin(), retired.end());
    for (const auto& block : blocks) {
        if (t.richTextMeasurements.size() >= maxPendingMeasurements)
            break;
        if (block.row >= t.rows || block.row + block.height <= 0 || block.document.text.empty() || layoutOf(t, block).isShaped()
            || std::ranges::any_of(t.richTextMeasurements, [&](const auto& pending) { return pending.blockSerial == block.serial; }))
            continue;
        if (!++t.richTextMeasurementToken)
            ++t.richTextMeasurementToken;
        MeasureRequest request { t.richTextMeasurementToken, 0,
            { block.width, block.height, block.overflow, block.alignment, block.lineMode, block.padding, block.viewportTop },
            {}, true };
        if (!request.document.tryAssign(block.document))
            continue;
        if (!t.richTextMeasurements.tryPushBack({ std::move(request), 0, 0, block.serial, t.typography.epoch, false, terminalNow(t).value_or(Ticks {}) }))
            break;
    }
}

bool adoptBlockMeasurement(Terminal& t, const PendingMeasurement& pending, Measurement measurement)
{
    if (!pending.blockSerial || pending.typographyEpoch != t.typography.epoch || !measurement.isShaped)
        return false;
    const auto blocks = t.richTextBlocks.blocks(activeScreen(t));
    const auto found = std::ranges::find_if(blocks, [&](const auto& block) { return block.serial == pending.blockSerial; });
    return found != blocks.end() && t.richTextLayouts.adopt(*found, t.typography, std::move(measurement.geometry));
}

namespace {

/// The cell a block's rectangle holds: a written space in the placing pen,
/// marked covered. A written space rather than the unwritten blank, so trims,
/// reflow and the void rule treat the rectangle as content.
Cell coveredCell(const Ubiquitty::Screen::State* s)
{
    Cell cell;
    Ubiquitty::Screen::eraseCell(&cell, s);
    cell.codePoint = ' ';
    Pen pen = cellPen(cell);
    pen.extendedAttributes.setTextSizeSpecID(0);
    penSetTextBand(&pen, 0);
    penSetProportional(&pen, false);
    penSetRichTextCovered(&pen, true);
    cellSetPen(cell, pen);
    return cell;
}

[[nodiscard]] bool isCovered(const Cell& cell) { return penRichTextCovered(&cellPen(cell)); }

/// Clears the cover from the on-screen cells a block leaves, so a later block
/// placed over them is not taken for its continuation.
void uncoverCells(Terminal& t, ScreenMode screen, const Block& block)
{
    if (!t.grid)
        return;
    const int firstRow = std::max(block.row, 0);
    const int endRow = std::min(block.row + static_cast<int>(block.height), t.rows);
    const int endColumn = std::min(static_cast<int>(block.column) + static_cast<int>(block.width), t.columns);
    for (int row = firstRow; row < endRow; ++row) {
        for (int column = block.column; column < endColumn; ++column) {
            if (!block.covers(row, column))
                continue;
            const Cell* cell = t.grid->cellAt(screen, Row(row), Column(column));
            if (!cell || !isCovered(*cell))
                continue;
            Cell uncovered = *cell;
            Pen pen = cellPen(uncovered);
            penSetRichTextCovered(&pen, false);
            cellSetPen(uncovered, pen);
            t.grid->setRowSpan(screen, Row(row), Column(column), RowView { &uncovered, 1 });
        }
    }
}

void removeAt(Terminal& t, ScreenMode screen, std::size_t position)
{
    uncoverCells(t, screen, t.richTextBlocks.blocks(screen)[position]);
    t.richTextBlocks.remove(screen, position);
    t.richTextLayouts.retainOnly(t.richTextBlocks);
}

/// Whether every on-screen cell of a block still holds its cover.
[[nodiscard]] bool isIntact(const Terminal& t, ScreenMode screen, const Block& block)
{
    if (block.column + block.width > t.columns || block.row + static_cast<int>(block.height) > t.rows)
        return false;
    const int firstRow = std::max(block.row, 0);
    for (int row = firstRow; row < block.row + static_cast<int>(block.height); ++row) {
        const RowView cells = t.grid->rowCells(screen, Row(row));
        if (!cells.data || cells.count < block.column + block.width)
            return false;
        for (int column = block.column; column < block.column + block.width; ++column) {
            if (!block.covers(row, column))
                continue;
            if (!isCovered(cells.data[column]))
                return false;
        }
    }
    return true;
}

[[nodiscard]] bool hasDoubledRow(const Terminal& t, int firstRow, int endRow)
{
    for (int row = std::max(firstRow, 0); row < std::min(endRow, t.rows); ++row) {
        if (t.grid->rowMeta(row).lineAttribute != Ubiquitty::lineAttributeNormal)
            return true;
    }
    return false;
}

} // namespace

ScreenMode activeScreen(const Terminal& t)
{
    return t.screens.isUsingAlternateScreen ? ScreenMode::Alternate : ScreenMode::Primary;
}

std::expected<void, PlacementError> placeBlock(Terminal& t, const Placement& placement, Document document)
{
    if (!t.grid || placement.width < 1 || placement.height < 1 || !validVisibleRects(placement.visibleRects, placement.width, placement.height)
        || (placement.caret != 65535 && placement.caret > document.text.size()) || placement.viewportTop > document.text.size()
        || (placement.viewportTop && placement.lineMode != LineMode::Terminal)
        || (placement.pointerMode == PointerMode::Characters && (!placement.id || !placement.documentRevision)))
        return std::unexpected(PlacementError::Geometry);
    Ubiquitty::Screen::scrollLaneFlush(t);
    Ubiquitty::Screen::State* s = t.screens.active;
    const int width = placement.width;
    const int height = placement.height;
    const int column = s->cursorX;
    const int row = s->cursorY;
    if (height > t.rows || width > t.columns - column)
        return std::unexpected(PlacementError::Geometry);
    // A block reaching past the bottom scrolls the screen up by the excess
    // first, which only a full-screen region does the way a line feed would.
    const int excess = std::max(0, row + height - t.rows);
    if (excess && (s->scrollTop != 0 || s->scrollBottom != t.rows - 1 || Ubiquitty::Screen::isRowNarrowedByMargins(t, s)))
        return std::unexpected(PlacementError::Geometry);
    if (hasDoubledRow(t, row, row + height))
        return std::unexpected(PlacementError::Geometry);
    if (!effectAllowed(t, Capability::RichTextRender))
        return std::unexpected(PlacementError::Denied);

    const ScreenMode screen = activeScreen(t);
    const std::array full { VisibleRect { 0, 0, placement.width, placement.height } };
    const auto rectangles = placement.visibleRects.empty() ? std::span<const VisibleRect>(full) : std::span<const VisibleRect>(placement.visibleRects);
    const std::span<const Block> existing = t.richTextBlocks.blocks(screen);
    std::array<bool, maxBlocksPerScreen> leaving {};
    for (std::size_t position = 0; position < existing.size(); ++position) {
        const Block& block = existing[position];
        const bool isSameID = placement.id != 0 && block.id == placement.id;
        if (isSameID && placement.documentRevision && block.documentRevision == placement.documentRevision && block.document != document)
            return std::unexpected(PlacementError::Revision);
        const std::array heldFull { VisibleRect { 0, 0, block.width, block.height } };
        const auto heldRectangles = block.visibleRects.empty() ? std::span<const VisibleRect>(heldFull) : std::span<const VisibleRect>(block.visibleRects);
        const bool isOverlapping = std::ranges::any_of(heldRectangles, [&](const auto& held) {
            return std::ranges::any_of(rectangles, [&](const auto& incoming) {
                return block.row + held.y < row + incoming.y + incoming.height && row + incoming.y < block.row + held.y + held.height
                    && block.column + held.x < column + incoming.x + incoming.width && column + incoming.x < block.column + held.x + held.width;
            });
        });
        leaving[position] = isSameID || isOverlapping;
    }
    Block staged {
        .id = placement.id,
        .documentRevision = placement.documentRevision,
        .row = row - excess,
        .column = static_cast<std::uint16_t>(column),
        .width = placement.width,
        .height = placement.height,
        .overflow = placement.overflow,
        .alignment = placement.alignment,
        .lineMode = placement.lineMode,
        .pointerMode = placement.pointerMode,
        .lifetime = placement.lifetime,
        .caret = placement.caret,
        .viewportTop = placement.viewportTop,
        .padding = placement.padding,
        .verticalPlacement = placement.verticalPlacement,
        .document = std::move(document),
    };
    if (!staged.visibleRects.tryAssign(placement.visibleRects))
        return std::unexpected(PlacementError::Limit);
    const auto count = existing.size();
    const auto admission = t.richTextBlocks.planAdmission(screen, staged, std::span(leaving.data(), count));
    if (!admission || !t.richTextBlocks.tryReserve(screen, admission->screenCapacity))
        return std::unexpected(PlacementError::Limit);

    for (std::size_t position = count; position-- > 0;) {
        if (leaving[position])
            removeAt(t, screen, position);
    }
    if (admission->historyBoundary != std::numeric_limits<std::int32_t>::min()) {
        Ubiquitty::Screen::dropRowsBefore(&t, t.scrollbackCount + admission->historyBoundary);
        reconcileBlocks(t);
    }
    if (excess)
        Ubiquitty::Screen::scrollUp(t, excess);
    const int top = row - excess;
    const Cell covered = coveredCell(s);
    std::array<Cell, maxColumns> span;
    std::fill_n(span.begin(), width, covered);
    for (const auto& rectangle : rectangles) {
        Ubiquitty::Screen::rectErase(t, { top + rectangle.y, column + rectangle.x, top + rectangle.y + rectangle.height - 1, column + rectangle.x + rectangle.width - 1 });
        for (int coveredRow = top + rectangle.y; coveredRow < top + rectangle.y + rectangle.height; ++coveredRow)
            t.grid->setRowSpan(Row(coveredRow), Column(column + rectangle.x), RowView { span.data(), rectangle.width });
    }
    Ubiquitty::Screen::scrubBlockDamage(t, top, top + height - 1);

    t.richTextBlocks.add(screen, std::move(staged));

    if (placement.cursor == CursorPolicy::Stay)
        return {};
    int below = top + height;
    if (below >= t.rows) {
        if (s->scrollTop == 0 && s->scrollBottom == t.rows - 1)
            Ubiquitty::Screen::scrollUp(t, 1);
        below = t.rows - 1;
    }
    s->cursorY = below;
    s->cursorX = column;
    s->wrapPending = false;
    return {};
}

void removeBlocks(Terminal& t, std::optional<std::uint16_t> id)
{
    if (id && !*id)
        return;
    if (!effectAllowed(t, Capability::RichTextRender))
        return;
    const ScreenMode screen = activeScreen(t);
    for (std::size_t position = t.richTextBlocks.blocks(screen).size(); position-- > 0;) {
        if (!id || t.richTextBlocks.blocks(screen)[position].id == *id)
            removeAt(t, screen, position);
    }
}

void removeOverlays(Terminal& t)
{
    const ScreenMode screen = activeScreen(t);
    for (std::size_t position = t.richTextBlocks.blocks(screen).size(); position-- > 0;) {
        if (t.richTextBlocks.blocks(screen)[position].lifetime == Lifetime::Overlay)
            removeAt(t, screen, position);
    }
}

void reconcileBlocks(Terminal& t)
{
    if (t.richTextBlocks.isEmpty() || !t.grid)
        return;
    // A batched scroll lane run left open across a parse return has moved the
    // covered cells and not yet the blocks: committed first, the blocks ride
    // the scroll before their cells are checked. A pane with no block pays
    // nothing, and the lane stays open for it.
    Ubiquitty::Screen::scrollLaneFlush(t);
    for (const ScreenMode screen : { ScreenMode::Primary, ScreenMode::Alternate }) {
        // The alternate screen has no scrollback, so a block that left its top
        // is gone with the rows.
        const int history = screen == ScreenMode::Primary ? t.scrollbackCount : 0;
        for (std::size_t position = t.richTextBlocks.blocks(screen).size(); position-- > 0;) {
            const Block& block = t.richTextBlocks.blocks(screen)[position];
            if (block.row < -history || block.row + block.height > t.rows || block.column + block.width > t.columns
                || (block.lifetime == Lifetime::Cells && !isIntact(t, screen, block)))
                removeAt(t, screen, position);
        }
    }
}

void scrollBlocksIntoHistory(Terminal& t, int lines)
{
    if (lines <= 0)
        return;
    for (Block& block : t.richTextBlocks.mutableBlocks(ScreenMode::Primary))
        block.row -= lines;
    // A block whose top left the scrollback has lost its first rows; the
    // reconcile pass removes it with the ones its history no longer holds.
}

void noteRowsShifted(Terminal& t, int top, int bottom, int rows)
{
    if (t.richTextBlocks.isEmpty())
        return;
    const ScreenMode screen = activeScreen(t);
    // A move between margins moves part of every row it touches.
    const bool isWholeRows = !Ubiquitty::Screen::isRowNarrowedByMargins(t, t.screens.active);
    for (std::size_t position = t.richTextBlocks.blocks(screen).size(); position-- > 0;) {
        Block& block = t.richTextBlocks.mutableBlocks(screen)[position];
        const int blockBottom = block.row + static_cast<int>(block.height) - 1;
        if (blockBottom < top || block.row > bottom)
            continue;
        const bool isInside = block.row >= top && blockBottom <= bottom;
        // A block the move carries past the band's edge loses the rows that
        // left it.
        const bool willStayInside = block.row - rows >= top && blockBottom - rows <= bottom;
        if (isWholeRows && isInside && willStayInside) {
            block.row -= rows;
            continue;
        }
        removeAt(t, screen, position);
    }
}

void shiftBlocks(Terminal& t, ScreenMode screen, int rows)
{
    for (Block& block : t.richTextBlocks.mutableBlocks(screen))
        block.row += rows;
}

const Block* blockCovering(const Terminal& t, ScreenMode screen, int screenRow, int column)
{
    for (const Block& block : t.richTextBlocks.blocks(screen)) {
        if (block.covers(screenRow, column))
            return &block;
    }
    return nullptr;
}

std::size_t copyableLength(const Terminal& t, const Block& block)
{
    return layoutOf(t, block).copyableEnd();
}

const Layout& layoutOf(const Terminal& t, const Block& block)
{
    return t.richTextLayouts.layoutOf(block, t.typography);
}

bool adoptTypography(Terminal& t, const TypographyFacts& facts)
{
    if (!isAdoptable(facts))
        return false;
    if (t.typography.facts && *t.typography.facts == facts)
        return true;
    t.typography.facts = std::make_shared<const TypographyFacts>(facts);
    ++t.typography.epoch;
    OSC::RichTextBlocks::typographyChanged(t);
    return true;
}

TypographySummary typographySummary(const Terminal& t)
{
    if (!t.typography.facts)
        return { .cellWidth = 0, .cellHeight = 0, .faceCount = 0, .epoch = t.typography.epoch };
    const TypographyFacts& facts = *t.typography.facts;
    return { .cellWidth = facts.cellWidth, .cellHeight = facts.cellHeight, .faceCount = facts.faceCount, .epoch = t.typography.epoch };
}

void adoptTypography(Terminal& t, const std::shared_ptr<const TypographyFacts>& typography, TypographyAuthority authority)
{
    if ((authority == TypographyAuthority::Unchanged && t.typography.facts == typography) || (typography && !isAdoptable(*typography)))
        return;
    t.typography.facts = typography;
    ++t.typography.epoch;
    OSC::RichTextBlocks::typographyChanged(t);
}

} // namespace Ubiquitty::RichText
