// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Input/PointerLadder.h"

#include "Core/Modes/Scroll.h"
#include "Core/Mouse/Hilite.h"
#include "Core/Mouse/Locator.h"
#include "Core/Mouse/Mouse.h"
#include "Core/RichText/Blocks.h"
#include "Core/Tek/Tek4014.h"
#include "Core/TerminalOperations.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>

namespace Ubiquitty::Input {

namespace {

// The caret a fact's point stands on: the nearest cell boundary, by the same
// rounding a frontend's geometry uses, moved inside a proportional run to the
// letter the layout puts there. A fact without a measured point keeps the
// boundary at its cell's left edge, which is a whole-cell answer in the
// caret's own coordinates rather than a different gesture.
[[nodiscard]] Selection::CaretResolution resolvedCaret(Terminal& terminal, const PointerFact& fact)
{
    const int columns = std::max(terminal.columns, 1);
    const int boundary = std::isfinite(fact.xInCells)
        ? std::clamp(static_cast<int>(std::floor(fact.xInCells + 0.5)), 0, columns)
        : std::clamp(static_cast<int>(fact.column), 0, columns);
    return Selection::resolveSelectionCaret(terminal, static_cast<int>(fact.viewOffset), static_cast<int>(fact.row), boundary, fact.xInCells, fact.yInRows);
}

[[nodiscard]] Mouse::EventType eventType(const PointerFact& fact) noexcept
{
    switch (fact.kind) {
    case PointerFact::Kind::Press:
        return Mouse::EventType::Press;
    case PointerFact::Kind::Release:
        return Mouse::EventType::Release;
    case PointerFact::Kind::Motion:
        return Mouse::EventType::Motion;
    }
    return Mouse::EventType::Motion;
}

[[nodiscard]] PointerReportingState reportingState(const Terminal& terminal, const PointerFact& fact, const PointerPolicy& policy) noexcept
{
    const PointerMoment moment {
        .held = { .hasCommand = fact.hasCommand, .hasControl = fact.hasControl, .hasAlternate = fact.hasAlternate, .hasShift = fact.hasShift },
        .isMouseReporting = Mouse::tracksPointer(&terminal),
    };
    return pointerReportingState(moment, policy);
}

// The DEC locator (DECELR): a report beside whatever the rungs below decide.
// A press or release the locator took is consumed -- an application that
// asked for DECLRP must not also see a context menu -- while motion is never
// consumed: a DECEFR rectangle leave is a one-shot, and swallowing the move
// would strand the hover affordance for the rest of the gesture.
[[nodiscard]] bool reportLocator(Terminal& terminal, const PointerFact& fact, int buttonCode, PointerRoute& route) noexcept
{
    if (!terminal.locatorEnabled)
        return false;
    // DECELR Ps2 = 1 selects device pixels; the DECLRP and the DECEFR rectangle
    // share that unit, so the report is resolved in whichever the host asked for.
    const bool usesPixels = !terminal.locatorUseRows && fact.pixelX >= 0 && fact.pixelY >= 0;
    const int row = static_cast<int>(usesPixels ? fact.pixelY + 1 : fact.row + 1);
    const int column = static_cast<int>(usesPixels ? fact.pixelX + 1 : fact.column + 1);
    route.locatorReportLength = Mouse::Locator::reportEvent(&terminal, eventType(fact), buttonCode, row, column,
        static_cast<int>(fact.heldButtonBits), std::span<char> { route.locatorReport });
    return route.locatorReportLength > 0 && fact.kind != PointerFact::Kind::Motion;
}

// The encoded mouse report, subject to xterm's dedup: motion reports only when
// the coordinate changes, at the granularity the mode names (device pixels
// under 1016 when the caller resolved one, cells otherwise), and a press notes
// its cell so a click that wobbles inside it stays a click.
[[nodiscard]] bool encodeReport(PointerLadder& ladder, const Terminal& terminal, const PointerFact& fact, int buttonCode, int modifiers, PointerRoute& route) noexcept
{
    const int row = static_cast<int>(fact.row);
    const int column = static_cast<int>(fact.column);
    int pixelX = static_cast<int>(fact.pixelX);
    int pixelY = static_cast<int>(fact.pixelY);
    // Clamped here rather than by each frontend, to the extent CSI 14 t
    // advertises: the drawable under DECSET 7402, the text area otherwise. A
    // frontend clamping to the grid it fitted reports, under the mode, a space
    // about a percent smaller than the one the image fills, and the pointer
    // overshoots the picture's edge. Before the dedup, so two pixels past the
    // edge are one report.
    if (pixelX >= 0 && pixelY >= 0) {
        const IntSize extent = reportedTextAreaPixels(terminal);
        if (extent.width > 0 && pixelX > extent.width)
            pixelX = extent.width;
        if (extent.height > 0 && pixelY > extent.height)
            pixelY = extent.height;
    }
    // M=1 is per block: applications opt in without changing ordinary mouse
    // reports elsewhere. Do not deduplicate at cell resolution; several carets
    // can lie in a single cell. Shift's native selection bypass runs above us.
    const RichText::Block* block = RichText::blockCovering(terminal, RichText::activeScreen(terminal), row, column);
    if (block && block->pointerMode == RichText::PointerMode::Characters && !fact.viewOffset) {
        const int allowed = encodeMouseReport(terminal, eventType(fact), buttonCode, row, column, modifiers,
            OutboundBytes { route.report.data(), route.report.size() }, pixelX, pixelY);
        if (!allowed)
            return false;
        const RichText::Layout& layout = RichText::layoutOf(terminal, *block);
        const auto x = static_cast<std::int64_t>(((std::isfinite(fact.xInCells) ? fact.xInCells : column + 0.5) - block->column) * layout.cellWidth());
        const auto y = static_cast<std::int64_t>(((std::isfinite(fact.yInRows) ? fact.yInRows : row + 0.5) - block->row) * layout.cellHeight());
        const std::uint32_t under = layout.characterAt(x, y);
        const std::uint32_t caret = layout.caretAt(x, y);
        const char event = fact.kind == PointerFact::Kind::Press ? 'p' : fact.kind == PointerFact::Kind::Release ? 'r'
                                                                                                                 : 'm';
        route.reportLength = std::snprintf(route.report.data(), route.report.size(),
            "\x1b]7405;a=p:i=%u:D=%u:e=%c:b=%d:m=%d:x=%d:y=%d:c=%u:k=%u\x1b\\",
            static_cast<unsigned>(block->id), block->documentRevision, event, buttonCode, modifiers, column + 1, row + 1, caret, under);
        return route.reportLength > 0 && static_cast<std::size_t>(route.reportLength) < route.report.size();
    }
    if (fact.kind == PointerFact::Kind::Motion) {
        if (terminal.modes.mouseSGRPixels && pixelX >= 0) {
            if (!ladder.dedup.shouldReport1016(pixelX, pixelY))
                return false;
            ladder.dedup.noteCell(row, column);
        } else if (!ladder.dedup.shouldReport(row, column)) {
            return false;
        }
    } else if (fact.kind == PointerFact::Kind::Press) {
        ladder.dedup.notePress(row, column);
    }
    route.reportLength = encodeMouseReport(terminal, eventType(fact), buttonCode, row, column, modifiers,
        OutboundBytes { route.report.data(), route.report.size() }, pixelX, pixelY);
    if (route.reportLength > 0)
        route.reported = PointerRoute::Reported { .type = eventType(fact), .buttonCode = buttonCode, .modifiers = modifiers, .row = row, .column = column, .pixelX = pixelX, .pixelY = pixelY };
    return route.reportLength > 0;
}

// The mode-1001 end report: the press cell as the region start, the release
// cell as the end and the mouse position. Sent whether or not the host ever
// replied with a region -- the protocol still wants the release coordinates --
// and the teardown clears any region the host highlighted.
void finishHilite(PointerLadder& ladder, Terminal& terminal, int endColumn, int endRow, PointerRoute& route) noexcept
{
    ladder.isHiliteGestureActive = false;
    const int column = endColumn >= 1 ? endColumn : ladder.hiliteStartColumn;
    const int row = endRow >= 1 ? endRow : ladder.hiliteStartRow;
    route.reportLength = Mouse::Locator::encodeHiliteEndReport(ladder.hiliteStartColumn, ladder.hiliteStartRow,
        column, row, column, row, Mouse::Locator::HiliteEndForm::RegionOnly, std::span<char> { route.report });
    terminal.modes.hiliteRegionValid = false;
    terminal.modes.hiliteAwaiting = false;
    route.needsRepublish = true;
    route.needsDisplay = true;
    route.wasConsumed = true;
}

// A reported left press arms the mode-1001 gesture. xterm blocks its event
// loop here until the host replies with XTHIMOUSE; this never waits: the await
// is bounded, `Mouse::tickHilite` aborts it past the deadline.
void beginHilite(PointerLadder& ladder, Terminal& terminal, const PointerFact& fact, std::uint32_t nowTicks, PointerRoute& route) noexcept
{
    if (!terminal.modes.mouseHilite)
        return;
    Mouse::beginHiliteAwait(&terminal, nowTicks);
    ladder.hiliteStartColumn = static_cast<int>(fact.column) + 1;
    ladder.hiliteStartRow = static_cast<int>(fact.row) + 1;
    ladder.isHiliteGestureActive = true;
    route.needsRepublish = true;
    route.needsDisplay = true;
}

// Whether the event stays local instead of reporting. Shift is xterm's own
// bypass and the reader's linear selection; the block chord reaches through the
// same way for a rectangular one; the platform's own selection gesture is the
// bypass of a surface with no modifier to hold; a drag extending an open local
// gesture stays with it whatever the modifier does mid-drag.
[[nodiscard]] bool keepsLocal(const PointerLadder& ladder, const PointerReportingState& state, const PointerFact& fact, bool isLeft) noexcept
{
    if (!state.isMouseReporting)
        return true;
    if (!isLeft)
        return false;
    if (fact.isSelectionGesture)
        return true;
    if (fact.kind == PointerFact::Kind::Motion)
        return ladder.isSelectionGestureOpen || state.isSelectionOverrideModifierArmed || state.isBlockSelectionOverrideModifierArmed;
    return allowsLocalDragSelect(state);
}

// The default selection modifier has a teaching hint. Customized chords do not
// reuse that text, because it would describe a gesture that reports to the app.
[[nodiscard]] Hint swallowedDragHint(const PointerFact& fact) noexcept
{
    // No keys to reach through with, so there is no lesson: the text names a
    // modifier, and one the reader does not have teaches a gesture they
    // cannot perform.
    if (!fact.hasModifierKeys)
        return Hint::None;
    return Hint::SelectWithShift;
}

// A drag the application swallowed, far enough from its press that the user
// was plainly dragging rather than clicking. Two cells: one is a click whose
// pointer wobbled, and a selection worth making crosses more than that.
constexpr std::ptrdiff_t swallowedDragCells = 2;

void moveReadlinePoint(Terminal& terminal, const PointerFact& fact, PointerRoute& route)
{
    const auto& screen = *terminal.screens.active;
    const int row = static_cast<int>(fact.row);
    for (int line = std::min(row, screen.cursorY); line < std::max(row, screen.cursorY); ++line) {
        if (!terminal.grid->rowMeta(line).has(RowFlag::Wrapped))
            return;
    }
    const int column = std::isfinite(fact.xInCells)
        ? static_cast<int>(std::clamp(std::floor(fact.xInCells + 0.5), 0.0, static_cast<double>(terminal.columns)))
        : static_cast<int>(fact.column);
    const int distance = column + (row - screen.cursorY) * terminal.columns - screen.cursorX;
    if (!distance)
        return;
    const std::array<char, 3> key = terminal.transmit8bitC1
        ? std::array<char, 3> { static_cast<char>(0x9b), distance > 0 ? 'C' : 'D', 0 }
        : std::array<char, 3> { '\033', terminal.modes.appCursorKeys ? 'O' : '[', distance > 0 ? 'C' : 'D' };
    const std::size_t width = terminal.transmit8bitC1 ? 2 : 3;
    const std::size_t count = static_cast<std::size_t>(std::abs(distance));
    if (!route.readlineInput.tryResize(count * width))
        return;
    for (std::size_t index = 0; index < count; ++index)
        std::copy_n(key.begin(), width, route.readlineInput.begin() + static_cast<std::ptrdiff_t>(index * width));
}

bool isReadlinePointerEligible(const Terminal& terminal, const PointerFact& fact)
{
    return !terminal.modes.keyboardLocked && !Mouse::tracksPointer(&terminal)
        && !fact.hasShift && !fact.hasAlternate && !fact.hasControl && !fact.hasCommand
        && !fact.isSelectionGesture && !fact.isWindowActivating && !fact.viewOffset
        && fact.row >= 0 && fact.row < terminal.rows && fact.column >= 0 && fact.column < terminal.columns;
}

void routeReadline(PointerLadder& ladder, Terminal& terminal, const PointerFact& fact, std::uint32_t nowTicks, PointerRoute& route)
{
    if (!isReadlinePointerEligible(terminal, fact)) {
        ladder.isReadlineClickPending = false;
        return;
    }
    if (fact.kind == PointerFact::Kind::Press) {
        ladder.isReadlineClickPending = terminal.modes.shouldMoveReadlinePointOnClick
            && fact.button == PointerButton::Left && fact.clickCount == 1;
        if (ladder.isReadlineClickPending) {
            ladder.readlinePressRow = static_cast<int>(fact.row);
            ladder.readlinePressColumn = static_cast<int>(fact.column);
            ladder.readlinePressTicks = nowTicks;
        }
        if (terminal.modes.shouldMoveReadlinePointBeforePaste && fact.button == PointerButton::Middle)
            moveReadlinePoint(terminal, fact, route);
    } else if (fact.kind == PointerFact::Kind::Motion && fact.isDragging) {
        if (fact.row != ladder.readlinePressRow || fact.column != ladder.readlinePressColumn)
            ladder.isReadlineClickPending = false;
    } else if (fact.kind == PointerFact::Kind::Release) {
        if (ladder.isReadlineClickPending && terminal.modes.shouldMoveReadlinePointOnClick
            && fact.button == PointerButton::Left && fact.clickCount == 1
            && fact.row == ladder.readlinePressRow && fact.column == ladder.readlinePressColumn
            && nowTicks - ladder.readlinePressTicks <= 30)
            moveReadlinePoint(terminal, fact, route);
        ladder.isReadlineClickPending = false;
    }
}

void deleteReadlineSelection(Terminal& terminal, Selection::Session& selection, const PointerPolicy& policy, PointerRoute& route)
{
    if (!policy.readlineEraseCharacter || Selection::isRectangular(&selection.state))
        return;
    const auto bounds = Selection::selectedBounds(selection, terminal);
    if (!bounds || bounds->subColumn0 || bounds->subColumn1)
        return;
    const int firstVisibleRow = Selection::resolveSelectionCaret(terminal, 0, 0, 0).absoluteRow;
    const int startRow = bounds->row0 - firstVisibleRow;
    const int endRow = bounds->row1 - firstVisibleRow;
    if (startRow < 0 || endRow >= terminal.rows || startRow > endRow)
        return;
    const int cursorRow = terminal.screens.active->cursorY;
    for (int row = std::min(startRow, cursorRow); row < std::max(endRow, cursorRow); ++row) {
        if (!terminal.grid->rowMeta(row).has(RowFlag::Wrapped))
            return;
    }
    const int count = (endRow - startRow) * terminal.columns + bounds->column1 + 1 - bounds->column0;
    if (count <= 0)
        return;
    PointerFact end;
    end.row = endRow;
    end.column = bounds->column1 + 1;
    moveReadlinePoint(terminal, end, route);
    const auto movementBytes = route.readlineInput.size();
    const int distance = static_cast<int>(end.column) + (endRow - cursorRow) * terminal.columns - terminal.screens.active->cursorX;
    if (distance && !movementBytes)
        return;
    if (!route.readlineInput.tryResize(movementBytes + static_cast<std::size_t>(count))) {
        route.readlineInput.clear();
        return;
    }
    std::fill(route.readlineInput.begin() + static_cast<std::ptrdiff_t>(movementBytes), route.readlineInput.end(), static_cast<char>(*policy.readlineEraseCharacter));
}

void anchorReadlineExtension(Selection::Session& selection, Terminal& terminal, const PointerFact& fact)
{
    const auto bounds = Selection::selectedBounds(selection, terminal);
    if (!bounds)
        return;
    const auto pointer = Selection::resolveSelectionCaret(terminal, 0, static_cast<int>(fact.row), static_cast<int>(fact.column));
    const auto position = static_cast<std::int64_t>(pointer.absoluteRow) * terminal.columns + fact.column;
    const auto start = static_cast<std::int64_t>(bounds->row0) * terminal.columns + bounds->column0;
    const auto end = static_cast<std::int64_t>(bounds->row1) * terminal.columns + bounds->column1;
    const bool shouldExtendStart = std::abs(position - start) < std::abs(position - end);
    selection.state.anchorRow = shouldExtendStart ? bounds->row1 : bounds->row0;
    selection.state.anchorColumn = shouldExtendStart ? bounds->column1 : bounds->column0;
}

[[nodiscard]] bool isSwallowedDrag(const PointerLadder& ladder, const PointerFact& fact) noexcept
{
    if (fact.kind != PointerFact::Kind::Motion || !fact.isDragging)
        return false;
    // A modifier is held: whatever happened, the user is not waiting to be
    // told a modifier exists.
    if (fact.hasShift || fact.hasCommand || fact.hasAlternate)
        return false;
    if (ladder.reportedPressRow < 0 || ladder.reportedPressColumn < 0)
        return false;
    const std::ptrdiff_t rows = fact.row - ladder.reportedPressRow;
    const std::ptrdiff_t columns = fact.column - ladder.reportedPressColumn;
    const std::ptrdiff_t rowDistance = rows < 0 ? -rows : rows;
    const std::ptrdiff_t columnDistance = columns < 0 ? -columns : columns;
    return rowDistance >= swallowedDragCells || columnDistance >= swallowedDragCells;
}

} // namespace

PointerRoute routePointer(PointerLadder& ladder, Terminal& terminal, Selection::Session* selection, const PointerFact& fact, const PointerPolicy& policy, std::uint32_t nowTicks)
{
    PointerRoute route;
    const bool isLeft = fact.button == PointerButton::Left;
    const bool canReadlineDelete = terminal.modes.shouldDeleteReadlineSelection
        && !terminal.modes.mouseRelativePointerLock && !terminal.locatorEnabled && !terminal.isTekModeActive
        && isReadlinePointerEligible(terminal, fact);
    const bool isReadlineRight = fact.button == PointerButton::Right && canReadlineDelete;
    if (!canReadlineDelete || (fact.kind == PointerFact::Kind::Press && !isReadlineRight)) {
        ladder.hasReadlineRightRelease = false;
        ladder.isReadlineDeletePending = false;
    }
    const int modifiers = xtermMouseModifiers(fact.hasShift, fact.hasAlternate, fact.hasControl);
    const int buttonCode = xtermButtonCode(fact.button);
    if (fact.kind == PointerFact::Kind::Press || terminal.modes.mouseRelativePointerLock
        || terminal.locatorEnabled || terminal.isTekModeActive || Mouse::tracksPointer(&terminal))
        ladder.isReadlineClickPending = false;

    // Relative pointer lock (DECSET 7403): the host owns the pointer, so
    // every event is its and none is a selection or a link. The report rides
    // the SGR-pixels frame with the motion delta in the pixel fields -- zero
    // on a button event -- and without that frame there is nothing to send.
    // A sample with no motion is swallowed rather than flooding the host with
    // a zero delta.
    if (terminal.modes.mouseRelativePointerLock) {
        route.wasConsumed = true;
        if (!terminal.modes.mouseSGRPixels)
            return route;
        int deltaX = 0;
        int deltaY = 0;
        if (fact.kind == PointerFact::Kind::Motion) {
            deltaX = static_cast<int>(fact.deltaX);
            deltaY = static_cast<int>(fact.deltaY);
            if (deltaX == 0 && deltaY == 0)
                return route;
        }
        route.reportLength = encodeMouseReport(terminal, eventType(fact), buttonCode, 0, 0, modifiers,
            OutboundBytes { route.report.data(), route.report.size() }, deltaX, deltaY);
        return route;
    }
    if (reportLocator(terminal, fact, buttonCode, route)) {
        route.wasConsumed = true;
        return route;
    }

    // Tek 4014 GIN: while the crosshair is armed the pointer aims it, and a
    // click is the GIN trigger. The device-to-Tek conversion is the
    // renderer's, so the frontend performs it on this verdict.
    if (terminal.isTekModeActive && tekGINActive(terminal)) {
        route.wantsTekGIN = true;
        route.wasConsumed = fact.kind != PointerFact::Kind::Release;
        return route;
    }

    // The 1001 gesture outranks the plain report on its button: the press is
    // both reported and armed, the release becomes the hilite end report.
    if (isLeft && fact.kind == PointerFact::Kind::Release && ladder.isHiliteGestureActive) {
        finishHilite(ladder, terminal, static_cast<int>(fact.column) + 1, static_cast<int>(fact.row) + 1, route);
        return route;
    }

    const PointerReportingState state = reportingState(terminal, fact, policy);
    if (!keepsLocal(ladder, state, fact, isLeft)) {
        if (encodeReport(ladder, terminal, fact, buttonCode, modifiers, route)) {
            route.wasConsumed = true;
            // A reported press is input to the host and drops the local
            // selection like a keystroke would -- except the press that
            // brought the window forward.
            if (fact.kind == PointerFact::Kind::Press && selection) {
                Selection::GestureStyle reported;
                reported.isWindowActivating = fact.isWindowActivating;
                route.didChangeSelection = Selection::clearSelectionForReportedPress(*selection, reported);
                route.needsDisplay = route.didChangeSelection;
            }
            if (isLeft && fact.kind == PointerFact::Kind::Press) {
                beginHilite(ladder, terminal, fact, nowTicks, route);
                // Where the swallowed-drag test measures from; a new press is
                // a new gesture, and so a new lesson.
                ladder.reportedPressRow = fact.row;
                ladder.reportedPressColumn = fact.column;
                ladder.hasTaughtThisDrag = false;
            }
            // The teaching moment: the drag went to the application and a
            // modifier would have kept it here.
            if (isLeft && !ladder.hasUsedSelectionOverride && !ladder.hasTaughtThisDrag
                && isSwallowedDrag(ladder, fact) && shouldShowHint(ladder.selectionHintShowCount)) {
                // The lesson is chosen before the budget is spent: a reader
                // with no modifier keys is owed no lesson, and charging them
                // for one would let three silent drags use up the teaching a
                // keyboard arriving later would have earned.
                const Hint lesson = policy.selectChord.hasShift ? swallowedDragHint(fact) : Hint::None;
                if (lesson != Hint::None) {
                    ++ladder.selectionHintShowCount;
                    ladder.hasTaughtThisDrag = true;
                    raiseHint(ladder.hintClock, lesson, nowTicks);
                    route.hint = lesson;
                    // The lesson is chrome the frame must draw.
                    route.needsDisplay = true;
                }
            }
        }
        return route;
    }

    routeReadline(ladder, terminal, fact, nowTicks, route);

    if ((!isLeft && !isReadlineRight) || !selection)
        return route;
    // Reaching through while an application holds the mouse is the gesture the
    // hint teaches; having performed it once, the user is not told again.
    if (state.isMouseReporting && (state.isSelectionOverrideModifierArmed || state.isBlockSelectionOverrideModifierArmed || fact.isSelectionGesture))
        ladder.hasUsedSelectionOverride = true;
    if (fact.kind == PointerFact::Kind::Press) {
        ladder.reportedPressRow = -1;
        ladder.reportedPressColumn = -1;
        ladder.hasTaughtThisDrag = false;
    }
    switch (fact.kind) {
    case PointerFact::Kind::Press: {
        if (isReadlineRight) {
            ladder.isReadlineDeletePending = ladder.hasReadlineRightRelease && fact.clickCount == 2
                && fact.row == ladder.readlineRightRow && fact.column == ladder.readlineRightColumn
                && nowTicks - ladder.readlineRightReleaseTicks <= 30;
            ladder.readlineRightPressTicks = nowTicks;
            ladder.hasReadlineRightRelease = false;
            anchorReadlineExtension(*selection, terminal, fact);
        }
        Selection::GestureStyle style;
        style.isRectangular = state.isBlockSelectionOverrideModifierArmed;
        // Shift extends a standing selection where the surface is already
        // ours. Where an application is holding the mouse it is instead the
        // bypass that made this selection possible at all, so it begins a
        // fresh one — the reader reached through to select here, not to grow
        // something they made before the application took the mouse.
        style.mode = isReadlineRight || (fact.hasShift && !state.isMouseReporting) ? Selection::GestureMode::Extend : Selection::GestureMode::Fresh;
        style.smartToken = policy.smartToken;
        style.isWindowActivating = fact.isWindowActivating;
        route.didChangeSelection = Selection::beginGesture(*selection, terminal, static_cast<int>(fact.row), static_cast<int>(fact.column),
            isReadlineRight ? 1 : static_cast<int>(fact.clickCount), static_cast<int>(fact.viewOffset), style, fact.xInCells, fact.yInRows);
        ladder.isSelectionGestureOpen = Selection::isGestureOpen(*selection);
        // A single click gets its caret from the pointer. Word presses keep
        // any character bounds that beginGesture resolved inside packed text.
        if (!isReadlineRight && ladder.isSelectionGestureOpen && isCaretDrivenGesture(fact)) {
            Selection::configureCharacterCaretGesture(*selection, terminal, resolvedCaret(terminal, fact));
        }
        route.wasConsumed = true;
        route.didOpenSelectionGesture = ladder.isSelectionGestureOpen;
        break;
    }
    case PointerFact::Kind::Motion:
        if (fact.isDragging && ladder.isSelectionGestureOpen) {
            int row = static_cast<int>(fact.row);
            int column = static_cast<int>(fact.column);
            const bool isRectangular = selection->gesture.dragBegan
                ? Selection::isRectangular(&selection->state)
                : selection->gestureStyle.isRectangular;
            const bool isBeyondViewport = !isRectangular && terminal.rows > 0 && std::isfinite(fact.yInRows)
                && (fact.yInRows < 0 || fact.yInRows >= terminal.rows);
            if (isBeyondViewport) {
                // Beyond a vertical edge, the linear selection includes that
                // whole row; its trailing void is no longer under the pointer.
                row = fact.yInRows < 0 ? 0 : terminal.rows - 1;
                column = fact.yInRows < 0 ? 0 : terminal.columns;
            }
            // A gesture opened on carets moves on carets to its end: a motion
            // routed on cells would re-anchor it on the cell the model keeps
            // as its lower end, which on a drag towards the left is the
            // pointer's side, and the selection would collapse under it.
            if (selection->usesCharacterCarets && selection->gesture.clickCount != 2) {
                const Selection::CaretResolution head = isBeyondViewport
                    ? Selection::resolveSelectionCaret(terminal, static_cast<int>(fact.viewOffset), row, column, column, row)
                    : resolvedCaret(terminal, fact);
                route.didChangeSelection = Selection::dragCharacterCaretGesture(*selection, terminal, head, static_cast<int>(fact.viewOffset));
            } else {
                route.didChangeSelection = Selection::dragGesture(*selection, terminal, row, column, static_cast<int>(fact.viewOffset),
                    isBeyondViewport ? column : fact.xInCells, isBeyondViewport ? row : fact.yInRows);
            }
            route.wasConsumed = true;
        }
        break;
    case PointerFact::Kind::Release:
        if (ladder.isSelectionGestureOpen) {
            ladder.isSelectionGestureOpen = false;
            route.wasConsumed = true;
            route.didChangeSelection = true;
            route.didFinishSelectionGesture = Selection::endGesture(*selection);
            if (isReadlineRight) {
                const bool shouldDelete = ladder.isReadlineDeletePending && fact.clickCount == 2
                    && fact.row == ladder.readlineRightRow && fact.column == ladder.readlineRightColumn
                    && nowTicks - ladder.readlineRightPressTicks <= 30;
                if (shouldDelete)
                    deleteReadlineSelection(terminal, *selection, policy, route);
                ladder.isReadlineDeletePending = false;
                ladder.hasReadlineRightRelease = !shouldDelete;
                ladder.readlineRightRow = static_cast<int>(fact.row);
                ladder.readlineRightColumn = static_cast<int>(fact.column);
                ladder.readlineRightReleaseTicks = nowTicks;
            }
        }
        break;
    }
    route.needsDisplay = route.needsDisplay || route.didChangeSelection;
    return route;
}

PointerRoute seedLocator(Terminal& terminal, const PointerFact& fact)
{
    PointerRoute route;
    PointerFact stationary = fact;
    stationary.kind = PointerFact::Kind::Motion;
    stationary.button = PointerButton::None;
    (void)reportLocator(terminal, stationary, xtermButtonCode(PointerButton::None), route);
    return route;
}

ScrollRoute routeScroll(PointerLadder& ladder, Terminal& terminal, const ScrollFact& fact, const Screen::ScrollTuning::Policy& tuning)
{
    ScrollRoute route;
    const double dominant = wheelDominantDelta(fact.deltaX, fact.deltaY);
    const WheelDirection direction = wheelDirectionFromScrollDelta(fact.deltaX, fact.deltaY);
    const bool isVertical = direction == WheelDirection::Up || direction == WheelDirection::Down;
    const Screen::ScrollTuning::InputDeviceType device = fact.isPrecise
        ? Screen::ScrollTuning::InputDeviceType::Trackpad
        : Screen::ScrollTuning::InputDeviceType::Wheel;
    FractionalAccumulator& accumulator = isVertical ? ladder.scrollAccumulator : ladder.scrollAccumulatorHorizontal;

    // A locked pointer's wheel is the host's whatever the tracking modes say,
    // and rides the locked frame with a zero delta; without the SGR-pixels
    // frame the lock has nothing to ride, and the event is swallowed.
    const bool isLocked = terminal.modes.mouseRelativePointerLock;
    const bool shouldNavigateAlternateScreen = Modes::alternateScrollActive(&terminal)
        || (fact.isDirectTouch && terminal.screens.isUsingAlternateScreen);
    const Modes::WheelRoute wheelRoute = isLocked
        ? Modes::WheelRoute::ReportToHost
        : Modes::wheelRoute(Mouse::tracksPointer(&terminal), isVertical && shouldNavigateAlternateScreen);
    route.isViewportScroll = wheelRoute == Modes::WheelRoute::ScrollView;
    if (dominant == 0.0) {
        return route;
    }
    if (isLocked && !terminal.modes.mouseSGRPixels) {
        route.wasConsumed = true;
        return route;
    }

    switch (wheelRoute) {
    case Modes::WheelRoute::ReportToHost: {
        // A notched wheel is one report per event. A continuous surface folds
        // to lines first, so a flick scrolls an application as far as it would
        // scroll history, and a slow swipe banks its residue until a line falls.
        int reports = 1;
        if (fact.isPrecise) {
            reports = Screen::ScrollTuning::linesForDelta(dominant, tuning, accumulator, device);
            reports = reports < 0 ? -reports : reports;
        }
        const int modifiers = xtermMouseModifiers(fact.hasShift, fact.hasAlternate, fact.hasControl);
        const int buttonCode = xtermWheelButtonCode(direction);
        const int pixelX = isLocked ? 0 : static_cast<int>(fact.pixelX);
        const int pixelY = isLocked ? 0 : static_cast<int>(fact.pixelY);
        for (int i = 0; i < reports; ++i) {
            const std::size_t used = static_cast<std::size_t>(route.byteLength);
            const int written = encodeMouseReport(terminal, Mouse::EventType::Press, buttonCode, static_cast<int>(fact.row),
                static_cast<int>(fact.column), modifiers, OutboundBytes { route.bytes.data() + used, route.bytes.size() - used },
                pixelX, pixelY);
            if (written <= 0)
                break;
            route.byteLength += written;
        }
        route.wasConsumed = route.byteLength > 0;
        return route;
    }
    case Modes::WheelRoute::AlternateScrollKeys: {
        const int lines = Screen::ScrollTuning::linesForDelta(fact.deltaY, tuning, accumulator, device);
        if (lines == 0)
            return route;
        const Modes::CursorKeyMode cursorKeys = terminal.modes.appCursorKeys ? Modes::CursorKeyMode::Application : Modes::CursorKeyMode::Normal;
        route.byteLength = Modes::alternateScrollBurst(cursorKeys, lines > 0 ? Modes::ScrollDirection::Up : Modes::ScrollDirection::Down,
            lines < 0 ? -lines : lines, std::span<char> { route.bytes });
        route.wasConsumed = true;
        return route;
    }
    case Modes::WheelRoute::ScrollView: {
        const int lines = Screen::ScrollTuning::linesForDelta(dominant, tuning, accumulator, device);
        if (lines == 0)
            return route;
        route.linesToOffsetView = lines;
        route.wasConsumed = true;
        return route;
    }
    }
    return route;
}

PointerPolicy pointerPolicyFromPreferences(const Preferences::Values& values)
{
    PointerPolicy policy;
    policy.smartToken = Preferences::Effective::smartDoubleClick(values) ? Selection::SmartTokenMode::Enabled : Selection::SmartTokenMode::Disabled;
    policy.isLinkOverrideEnabled = Preferences::Effective::linksOverrideReporting(values);
    policy.blockSelectChord = chordFromSpec(Preferences::Effective::blockSelectModifier(values));
    policy.selectChord = chordFromSpec(Preferences::Effective::selectModifier(values));
    return policy;
}

PointerReportingState pointerReportingState(const PointerMoment& moment, const PointerPolicy& policy) noexcept
{
    PointerReportingState state;
    state.isMouseReporting = moment.isMouseReporting;
    state.isSelectionOverrideModifierArmed = chordArmed(moment.held, policy.selectChord);
    state.isBlockSelectionOverrideModifierArmed = moment.isBlockSelectionInFlight || chordArmed(moment.held, policy.blockSelectChord);
    state.isLinkModifierArmed = chordArmed(moment.held, linkChord)
        && !(moment.isMouseReporting && state.isSelectionOverrideModifierArmed);
    state.isControlModePane = policy.isControlModePane;
    state.isLinkOverrideEnabled = policy.isLinkOverrideEnabled
        && !(moment.isMouseReporting && state.isSelectionOverrideModifierArmed);
    return state;
}

HeldModifiers chordFromSpec(std::string_view spec)
{
    HeldModifiers chord;
    std::string_view rest = spec;
    while (!rest.empty()) {
        const std::size_t plus = rest.find('+');
        std::string_view token = rest.substr(0, plus);
        rest = plus == std::string_view::npos ? std::string_view {} : rest.substr(plus + 1);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        std::array<char, 8> lowered {};
        if (token.empty() || token.size() >= lowered.size())
            continue;
        for (std::size_t i = 0; i < token.size(); ++i)
            lowered[i] = (token[i] >= 'A' && token[i] <= 'Z') ? static_cast<char>(token[i] - 'A' + 'a') : token[i];
        const std::string_view name { lowered.data(), token.size() };
        if (name == "cmd" || name == "command" || name == "super" || name == "win")
            chord.hasCommand = true;
        else if (name == "ctrl" || name == "control")
            chord.hasControl = true;
        else if (name == "alt" || name == "opt" || name == "option" || name == "meta")
            chord.hasAlternate = true;
        else if (name == "shift")
            chord.hasShift = true;
    }
    return chord;
}

bool windowDragArmed(HeldModifiers modifiers, std::string_view dragModifierSpec, WindowChrome chrome)
{
    // A chromeless window has no title bar to grab, so Command over the body
    // stands in for one. It is checked first because it holds whatever else
    // the reader configured: a spec that also names Command still matches
    // through the exact test below.
    if (chrome == WindowChrome::Chromeless && modifiers.hasCommand)
        return true;
    return chordArmed(modifiers, chordFromSpec(dragModifierSpec));
}

bool isWindowDragPress(const PointerFact& fact, std::string_view dragModifierSpec, WindowChrome chrome)
{
    if (fact.kind != PointerFact::Kind::Press)
        return false;
    const HeldModifiers modifiers { .hasCommand = fact.hasCommand,
        .hasControl = fact.hasControl,
        .hasAlternate = fact.hasAlternate,
        .hasShift = fact.hasShift };
    return windowDragArmed(modifiers, dragModifierSpec, chrome);
}

void notePointerLockGrab(PointerLadder& ladder, PointerLockGrab grab)
{
    const bool isHeld = grab == PointerLockGrab::Held;
    // Counted on the edge, not the report: a frontend that says "held" twice
    // for one capture has one capture.
    if (isHeld && !ladder.isPointerLockHeld)
        ++ladder.pointerLockEngagements;
    ladder.isPointerLockHeld = isHeld;
}

Hint hintShowing(const PointerLadder& ladder, std::uint32_t nowTicks)
{
    // The way out of a trap outranks a lesson about a drag: the reader is
    // still in the trap. The budget counts captures, so the first few teach
    // and an application that locks on every click stops being answered.
    if (ladder.isPointerLockHeld && shouldShowHint(ladder.pointerLockEngagements - 1))
        return Hint::PointerLockRelease;
    return hintShowing(ladder.hintClock, nowTicks);
}

std::uint32_t hintExpiryTicks(const PointerLadder& ladder)
{
    // A standing lock lesson has no clock: it ends with the grab, and the
    // frontend asks again on that report rather than on a tick.
    if (ladder.isPointerLockHeld && shouldShowHint(ladder.pointerLockEngagements - 1))
        return 0;
    return hintExpiryTicks(ladder.hintClock);
}

void notePointerLockChord(PointerLadder& ladder, const PointerLockKeyState& keys, std::uint32_t nowTicks)
{
    // Armed only while the grab is held: a chord with nothing to release is a
    // real shortcut, and arming it would steal the shortcut's hold.
    const bool isArmed = ladder.isPointerLockHeld && isPointerLockBreakoutArmed(keys);
    if (isArmed && !ladder.pointerLockChord.isArmed)
        ladder.pointerLockChord.armedAtTicks = nowTicks;
    ladder.pointerLockChord.isArmed = isArmed;
}

PointerLockChordVerdict tickPointerLockChord(PointerLadder& ladder, const PointerLockKeyState& keysNow, std::uint32_t nowTicks)
{
    // Re-checked against the live keys: a release in the last instant aborts.
    if (!ladder.pointerLockChord.isArmed || !ladder.isPointerLockHeld || !isPointerLockBreakoutArmed(keysNow)) {
        ladder.pointerLockChord.isArmed = false;
        return PointerLockChordVerdict::Keep;
    }
    if (nowTicks - ladder.pointerLockChord.armedAtTicks < pointerLockBreakoutHoldTicks)
        return PointerLockChordVerdict::Keep;
    ladder.pointerLockChord.isArmed = false;
    return PointerLockChordVerdict::BreakOut;
}

std::uint32_t pointerLockChordTicksUntilDeadline(const PointerLadder& ladder, std::uint32_t nowTicks)
{
    if (!ladder.pointerLockChord.isArmed)
        return 0;
    const std::uint32_t deadline = ladder.pointerLockChord.armedAtTicks + pointerLockBreakoutHoldTicks;
    const auto left = static_cast<std::int32_t>(deadline - nowTicks);
    return left > 0 ? static_cast<std::uint32_t>(left) : 0;
}

} // namespace Ubiquitty::Input
