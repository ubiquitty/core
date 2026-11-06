// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/CellGrid.h"
#include "Core/LineStore.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/HistoryLines.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/VT/Internal.h"
#include <algorithm>

namespace Ubiquitty::VT {

static std::expected<void, StorageError> reserveGraphicCapture(Terminal::GraphicCancellation& undo, std::size_t cells, std::size_t metadata)
{
    if (cells > 4 * (maxRows + 2) * maxColumns - undo.cells.size()
        || metadata > (maxRows + 2) * (maxRows + 2) - undo.metadata.size()
        || undo.operations.size() >= 4 * maxRows + 8)
        return std::unexpected(StorageError::OverCapacity);
    if (auto reserved = undo.cells.tryReserve(undo.cells.size() + cells); !reserved)
        return reserved;
    if (auto reserved = undo.metadata.tryReserve(undo.metadata.size() + metadata); !reserved)
        return reserved;
    return undo.operations.tryReserve(undo.operations.size() + 1);
}

void beginGraphicCancellation(Terminal& t)
{
    if (!t.graphicCancellation)
        t.graphicCancellation.reset(new (std::nothrow) Terminal::GraphicCancellation);
    if (!t.graphicCancellation)
        return;
    auto& undo = *t.graphicCancellation;
    undo.isValid = t.grid != nullptr;
    undo.isCapturing = undo.isValid;
    undo.cells.clear();
    undo.metadata.clear();
    undo.operations.clear();
    undo.isAlternate = t.screens.isUsingAlternateScreen;
    undo.isStatus = t.statusLineActive;
    undo.rows = t.rows;
    undo.columns = t.columns;
    undo.cursorX = undo.isStatus ? t.statusCursorX : t.screens.active->cursorX;
    undo.cursorY = t.screens.active->cursorY;
    undo.wrapPending = t.screens.active->wrapPending;
    undo.singleShift = t.singleShift;
    undo.triggerWrapRow = t.triggerWrapEvaluatedRow;
    undo.predecessor = t.parser.precedingItem;
    undo.didAlignedTabOverflow = t.didAlignedTabOverflow;
    undo.didEmitAlignedGraphic = false;
    undo.isClusterActive = t.parser.clusterActive;
    undo.clusterRow = t.parser.clusterRow;
    undo.clusterColumn = t.parser.clusterColumn;
    undo.clusterScreen = t.parser.clusterScreen;
    undo.clusterLast = t.parser.clusterLast;
    undo.clusterState = t.parser.clusterState;
    undo.clusterAtX = t.parser.clusterAtX;
    undo.clusterAtY = t.parser.clusterAtY;
    undo.isClusterAtWrap = t.parser.clusterAtWrap;
    undo.clusterCodePoints = t.parser.clusterCodePoints;
    undo.clusterCodepointLength = t.parser.clusterCodepointLength;
}

void discardGraphicCancellation(Terminal& t)
{
    if (t.graphicCancellation) {
        t.graphicCancellation->isValid = false;
        t.graphicCancellation->isCapturing = false;
    }
}

void captureGraphicCells(Terminal& t, int row, int first, int last)
{
    if (!t.graphicCancellation)
        return;
    auto& undo = *t.graphicCancellation;
    if (!undo.isCapturing || !t.grid || row < 0 || row >= t.rows)
        return;
    first = std::clamp(first, 0, t.columns - 1);
    last = std::clamp(last, first, t.columns - 1);
    if (!reserveGraphicCapture(undo, static_cast<std::size_t>(last - first + 1), 1)) {
        discardGraphicCancellation(t);
        return;
    }
    auto& operation = undo.operations.emplace_back();
    operation.row = row;
    operation.column = first;
    operation.count = last - first + 1;
    operation.cellOffset = static_cast<std::uint32_t>(undo.cells.size());
    operation.metaOffset = static_cast<std::uint32_t>(undo.metadata.size());
    operation.isStatus = undo.isStatus;
    undo.metadata.push_back(t.grid->rowMeta(row));
    for (int column = first; column <= last; ++column) {
        if (undo.isStatus)
            undo.cells.push_back(Rendering::statusCellAt(t, column));
        else
            undo.cells.push_back(*t.grid->logicalCellValueAt(Row(row), Column(column)));
    }
}

void cancellableWrapLinefeed(Terminal& t)
{
    if (!t.graphicCancellation) {
        linefeed(t);
        return;
    }
    auto& undo = *t.graphicCancellation;
    if (!undo.isCapturing || !t.grid) {
        linefeed(t);
        return;
    }
    Screen::scrollLaneFlush(t);
    Screen::bandLaneFlushActive(t);
    const auto& screen = *t.screens.active;
    const bool isScrolling = screen.cursorY == screen.scrollBottom
        && (!t.modes.leftRightMarginMode || (screen.cursorX >= screen.scrollLeft && screen.cursorX <= screen.scrollRight));
    const bool isPartial = t.modes.leftRightMarginMode && (screen.scrollLeft != 0 || screen.scrollRight != t.columns - 1);
    if (!isScrolling || isPartial) {
        const int firstRow = isScrolling ? screen.scrollTop : screen.cursorY;
        const int lastRow = isScrolling ? screen.scrollBottom : screen.cursorY;
        for (int row = firstRow; row <= lastRow; ++row)
            captureGraphicCells(t, row, 0, t.columns - 1);
        linefeed(t);
        return;
    }
    if (!reserveGraphicCapture(undo, static_cast<std::size_t>(t.columns), static_cast<std::size_t>(screen.scrollBottom - screen.scrollTop + 1))) {
        discardGraphicCancellation(t);
        linefeed(t);
        return;
    }
    auto& operation = undo.operations.emplace_back();
    operation.isScroll = true;
    operation.row = screen.scrollTop;
    operation.bottom = screen.scrollBottom;
    operation.count = t.columns;
    operation.cellOffset = static_cast<std::uint32_t>(undo.cells.size());
    operation.metaOffset = static_cast<std::uint32_t>(undo.metadata.size());
    for (int column = 0; column < t.columns; ++column)
        undo.cells.push_back(*t.grid->logicalCellValueAt(Row(screen.scrollTop), Column(column)));
    for (int row = screen.scrollTop; row <= screen.scrollBottom; ++row)
        undo.metadata.push_back(t.grid->rowMeta(row));
    const bool shouldCaptureHistory = !undo.isAlternate && screen.scrollTop == 0 && screen.scrollBottom == t.rows - 1;
    const long long appended = t.lineStore ? t.lineStore->totalAppended() : 0;
    if (shouldCaptureHistory && t.lineStore) {
        const auto front = t.lineStore->frontLine();
        operation.historyFront = front.number;
        operation.doesHistoryContinueLine = front.continuity == LineStore::LineContinuity::ContinuesLine;
        if (t.lineStore->count() == t.lineStore->capacity()) {
            const auto& oldest = *t.lineStore->at(0);
            if (!undo.cells.tryReserve(undo.cells.size() + static_cast<std::size_t>(oldest.length))) {
                discardGraphicCancellation(t);
                linefeed(t);
                return;
            }
            operation.didEvictHistory = true;
            operation.historyLength = oldest.length;
            operation.historyOffset = static_cast<std::uint32_t>(undo.cells.size());
            operation.historyMeta.lineAttribute = oldest.lineAttribute;
            operation.historyMeta.oscMark = oldest.mark;
            operation.historyMeta.set(RowFlag::Wrapped, oldest.wrapped);
            operation.historyMeta.set(RowFlag::NeedsShaping, oldest.needsShaping);
            for (const auto& cell : std::span(oldest.cells, static_cast<std::size_t>(oldest.length)))
                undo.cells.push_back(cell);
        }
    }
    linefeed(t);
    operation.didAppendHistory = shouldCaptureHistory && t.lineStore && t.lineStore->totalAppended() != appended;
    operation.didEvictHistory &= operation.didAppendHistory;
}

void cancelPreviousGraphic(Terminal& t)
{
    if (!t.graphicCancellation)
        return;
    auto& undo = *t.graphicCancellation;
    if (!undo.isValid || t.parser.precedingItem.kind != PrecedingItem::Kind::Graphic
        || !t.grid || undo.rows != t.rows || undo.columns != t.columns
        || undo.isAlternate != t.screens.isUsingAlternateScreen || undo.isStatus != (t.statusLineActive != 0))
        return;
    undo.isValid = false;
    undo.isCapturing = false;
    Screen::scrollLaneFlush(t);
    Screen::bandLaneFlushActive(t);
    int appended = 0;
    int evicted = 0;
    const Terminal::GraphicCancellation::Operation* firstAppend = nullptr;
    for (const auto& operation : undo.operations) {
        appended += operation.didAppendHistory;
        evicted += operation.didEvictHistory;
        if (operation.didAppendHistory && !firstAppend)
            firstAppend = &operation;
    }
    if (appended && t.lineStore) {
        const int originalCount = t.lineStore->count() - appended + evicted;
        if (originalCount < 0)
            return;
        const int restoreCount = std::min(originalCount, evicted);
        FallibleVector<StoredLine> oldest;
        if (!oldest.tryReserve(static_cast<std::size_t>(restoreCount)))
            return;
        for (const auto& operation : undo.operations) {
            if (!operation.didEvictHistory || static_cast<int>(oldest.size()) == restoreCount)
                continue;
            const auto& meta = operation.historyMeta;
            oldest.push_back({ undo.cells.data() + operation.historyOffset, operation.historyLength,
                meta.has(RowFlag::Wrapped), meta.lineAttribute, meta.oscMark, meta.has(RowFlag::NeedsShaping), false });
        }
        const int dropped = std::min(appended, t.lineStore->count());
        if (oldest.empty())
            t.lineStore->dropNewest(dropped);
        else {
            const LineStore::FrontLine front { firstAppend->historyFront,
                firstAppend->doesHistoryContinueLine ? LineStore::LineContinuity::ContinuesLine : LineStore::LineContinuity::StartsLine };
            if (!t.lineStore->prepend(oldest, front, dropped))
                return;
        }
        t.scrollbackCount = t.lineStore->count();
        ++t.absoluteRowEpoch;
        if (evicted) {
            const auto continuity = t.lineStore->frontLine().continuity;
            Screen::retireHistory(t);
            t.lineStore->restartNumbering(t.historyEpochBase, continuity);
        }
    }
    // The undo puts rows back by copying them, not by moving lines, so a pin
    // cannot follow it: the buffer's positions go, and the lines still to
    // complete take fresh names, because the undone rows' numbers are reused.
    const bool hasUndoneScroll = std::ranges::any_of(undo.operations, [](const auto& operation) { return operation.isScroll; });
    if (appended || hasUndoneScroll) {
        Screen::forgetPositions(t, Screen::activeBuffer(t));
        Screen::startFreshLineNames(t);
    }
    for (auto it = undo.operations.rbegin(); it != undo.operations.rend(); ++it) {
        const auto& operation = *it;
        if (operation.isScroll) {
            t.grid->scrollRegion(Row(operation.row), Row(operation.bottom), -1, defaultBlankCell);
            for (int row = operation.row; row <= operation.bottom; ++row)
                t.grid->rowMeta(row) = undo.metadata[operation.metaOffset + static_cast<std::size_t>(row - operation.row)];
        }
        if (operation.isStatus) {
            auto cells = Rendering::ensureStatusRow(t);
            std::copy_n(undo.cells.data() + operation.cellOffset, operation.count, cells.data() + operation.column);
        } else {
            t.grid->setRowSpan(Row(operation.row), Column(operation.column), { undo.cells.data() + operation.cellOffset, operation.count });
            t.grid->rowMeta(operation.row) = undo.metadata[operation.metaOffset];
        }
    }
    if (undo.isStatus)
        t.statusCursorX = undo.cursorX;
    else {
        t.screens.active->cursorX = undo.cursorX;
        t.screens.active->cursorY = undo.cursorY;
        t.screens.active->wrapPending = undo.wrapPending;
    }
    t.singleShift = undo.singleShift;
    t.triggerWrapEvaluatedRow = undo.triggerWrapRow;
    t.parser.precedingItem = undo.predecessor;
    t.parser.clusterActive = undo.isClusterActive;
    t.parser.clusterRow = undo.clusterRow;
    t.parser.clusterColumn = undo.clusterColumn;
    t.parser.clusterScreen = undo.clusterScreen;
    t.parser.clusterLast = undo.clusterLast;
    t.parser.clusterState = undo.clusterState;
    t.parser.clusterAtX = undo.clusterAtX;
    t.parser.clusterAtY = undo.clusterAtY;
    t.parser.clusterAtWrap = undo.isClusterAtWrap;
    t.parser.clusterCodePoints = undo.clusterCodePoints;
    t.parser.clusterCodepointLength = undo.clusterCodepointLength;
    if (undo.didEmitAlignedGraphic && t.alignedTabTerminal)
        cancelPreviousGraphic(*t.alignedTabTerminal);
    t.didAlignedTabOverflow = undo.didAlignedTabOverflow;
    Screen::markAllDirty(t);
}

} // namespace Ubiquitty::VT
