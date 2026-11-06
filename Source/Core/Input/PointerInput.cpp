// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PointerInput.cpp — see PointerInput.h. Pointer/tab/scroll-follow policy.

#include "Core/Input/PointerInput.h"

#include <cmath>

#include "Core/TerminalOperations.h"

namespace Ubiquitty::Input {

int xtermButtonCode(PointerButton button)
{
    switch (button) {
    case PointerButton::Left:
        return 0;
    case PointerButton::Middle:
        return 1;
    case PointerButton::Right:
        return 2;
    // Extended buttons 8-11 → Cb 128-131 (0x80 high-button class), distinct
    // from the wheel class (0x40) and the base buttons (0-2).
    case PointerButton::Button8:
        return 128;
    case PointerButton::Button9:
        return 129;
    case PointerButton::Button10:
        return 130;
    case PointerButton::Button11:
        return 131;
    case PointerButton::Other:
        return 0;
    case PointerButton::None:
        return 3;
    }
    // Reached only by a value outside the enumeration. The `default:` that used
    // to carry this sat beside a case with the same body and cost -Wswitch: with
    // every enumerator covered, a default makes adding one compile silently.
    return 0;
}

int xtermWheelButtonCode(WheelDirection direction)
{
    // xterm wheel class (0x40): up = button 4 (Cb 64), down = button 5 (Cb 65),
    // and the horizontal wheel is buttons 6/7 → left = Cb 66, right = Cb 67.
    switch (direction) {
    case WheelDirection::Up:
        return 64;
    case WheelDirection::Down:
        return 65;
    case WheelDirection::Left:
        return 66;
    case WheelDirection::Right:
        return 67;
    }
    return 65; // unreachable; keeps the compiler happy on the enum switch
}

WheelDirection wheelDirectionFromScrollDelta(double deltaX, double deltaY)
{
    // Dominant axis, vertical winning ties. deltaX > 0 (rightward finger) scrolls
    // content left → Left, matching xterm's horizontal-wheel convention.
    if (std::fabs(deltaY) >= std::fabs(deltaX))
        return deltaY > 0.0 ? WheelDirection::Up : WheelDirection::Down;
    return deltaX > 0.0 ? WheelDirection::Left : WheelDirection::Right;
}

double wheelDominantDelta(double deltaX, double deltaY)
{
    return std::fabs(deltaY) >= std::fabs(deltaX) ? deltaY : deltaX;
}

bool MotionDedup::shouldReport(int row, int column)
{
    if (row == m_lastRow && column == m_lastColumn)
        return false;
    m_lastRow = row;
    m_lastColumn = column;
    return true;
}

bool MotionDedup::shouldReport1016(int px, int py)
{
    if (px == m_lastPixelX && py == m_lastPixelY)
        return false;
    m_lastPixelX = px;
    m_lastPixelY = py;
    return true;
}

void MotionDedup::noteCell(int row, int column)
{
    m_lastRow = row;
    m_lastColumn = column;
}

void MotionDedup::reset()
{
    m_lastRow = -1;
    m_lastColumn = -1;
    m_lastPixelX = sentinelPixel;
    m_lastPixelY = sentinelPixel;
}

void MotionDedup::notePress(int row, int column)
{
    reset();
    noteCell(row, column);
}

int xtermMouseModifiers(bool shift, bool alternate, bool control)
{
    int modifiers = 0;
    if (shift)
        modifiers |= Ubiquitty::Mouse::Modifier::Shift;
    if (alternate)
        modifiers |= Ubiquitty::Mouse::Modifier::Meta;
    if (control)
        modifiers |= Ubiquitty::Mouse::Modifier::Control;
    return modifiers;
}

bool shouldReResolveLinkHover(const HoverReResolveFacts& facts)
{
    return !facts.isSelecting && !facts.isPointerHiddenForTyping && facts.isWindowKey
        && facts.isPointerInsideView;
}

int commandNumberTabPosition(ModifierState modifiers, uint32_t character)
{
    if (modifiers == ModifierState::OtherModifiers)
        return 0;
    if (character < '1' || character > '9')
        return 0;
    return static_cast<int>(character - '0');
}

int heldViewOffsetForSelection(int viewOffset, int scrollbackCount, int rowsAppended)
{
    if (rowsAppended <= 0)
        return viewOffset; // nothing left the screen — nothing to compensate
    int vo = viewOffset + rowsAppended;
    if (vo > scrollbackCount)
        vo = scrollbackCount;
    return vo;
}

std::ptrdiff_t viewOffsetAfterOutput(ViewportHold hold, std::ptrdiff_t viewOffset, std::ptrdiff_t scrollbackCount, std::ptrdiff_t rowsAppended)
{
    if (hold == ViewportHold::Follow)
        return 0;
    return heldViewOffsetForSelection(static_cast<int>(viewOffset), static_cast<int>(scrollbackCount), static_cast<int>(rowsAppended));
}

bool isSnapPolicyConsulted(SelectionPresence selection, std::ptrdiff_t viewOffset)
{
    return selection != SelectionPresence::Present && viewOffset > 0;
}

ViewportHold viewportHoldForOutput(SelectionPresence selection, std::ptrdiff_t viewOffset, OutputSnapPolicy snapPolicy)
{
    if (viewOffset <= 0) {
        return ViewportHold::Follow;
    }
    if (!isSnapPolicyConsulted(selection, viewOffset)) {
        // A standing selection back in history holds its rows without asking
        // the mode. The tail returned above has no rows to hold.
        return ViewportHold::Hold;
    }
    return snapPolicy == OutputSnapPolicy::SnapToTail ? ViewportHold::Follow : ViewportHold::Hold;
}

ViewportAfterOutput viewportAfterOutput(SelectionPresence selection, std::ptrdiff_t viewOffset, OutputSnapPolicy snapPolicy, std::ptrdiff_t scrollbackCount, std::ptrdiff_t rowsAppended)
{
    const ViewportHold hold = viewportHoldForOutput(selection, viewOffset, snapPolicy);
    return { viewOffsetAfterOutput(hold, viewOffset, scrollbackCount, rowsAppended), hold };
}

} // namespace Ubiquitty::Input
