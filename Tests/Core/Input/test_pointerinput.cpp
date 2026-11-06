// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Mouse/MouseEvent.h"
#include "Harness.h"

// test_pointerinput.cpp — frontend-neutral pointer / tab-nav / scroll-follow
// policy (Input/PointerInput): the xterm button + modifier mapping, the
// accelerator-digit tab jump, and the hold-selection-while-tail-grows clamp.
// Pure C++, links standalone.

#include "Core/Input/PointerInput.h"
#include "Core/TerminalOperations.h"

#include <cstdio>

using Ubiquitty::Input::commandNumberTabPosition;
using Ubiquitty::Input::heldViewOffsetForSelection;
using Ubiquitty::Input::ModifierState;
using Ubiquitty::Input::MotionDedup;
using Ubiquitty::Input::PointerButton;
using Ubiquitty::Input::WheelDirection;
using Ubiquitty::Input::xtermButtonCode;
using Ubiquitty::Input::xtermMouseModifiers;
using Ubiquitty::Input::xtermWheelButtonCode;

TEST(pointerinputCases)
{

    // ---- xterm button code: standard left 0 / middle 1 / right 2 ----
    CHECK(xtermButtonCode(PointerButton::Left) == 0);
    CHECK(xtermButtonCode(PointerButton::Middle) == 1);
    CHECK(xtermButtonCode(PointerButton::Right) == 2);
    CHECK(xtermButtonCode(PointerButton::Other) == 0);
    // Extended buttons 8-11 → Cb 128-131 (0x80 high-button class). Each carries
    // the 0x80 bit and is distinct from the base buttons and the wheel.
    CHECK(xtermButtonCode(PointerButton::Button8) == 128);
    CHECK(xtermButtonCode(PointerButton::Button9) == 129);
    CHECK(xtermButtonCode(PointerButton::Button10) == 130);
    CHECK(xtermButtonCode(PointerButton::Button11) == 131);
    CHECK((xtermButtonCode(PointerButton::Button8) & 0x80) != 0);

    // ---- xterm modifier bits: shift 4, meta(alt) 8, control 16, OR'd ----
    CHECK(Ubiquitty::Mouse::Modifier::Shift == 4 && Ubiquitty::Mouse::Modifier::Meta == 8 && Ubiquitty::Mouse::Modifier::Control == 16); // wire values the encoder expects
    CHECK(xtermMouseModifiers(false, false, false) == 0);
    CHECK(xtermMouseModifiers(true, false, false) == Ubiquitty::Mouse::Modifier::Shift);
    CHECK(xtermMouseModifiers(false, true, false) == Ubiquitty::Mouse::Modifier::Meta);
    CHECK(xtermMouseModifiers(false, false, true) == Ubiquitty::Mouse::Modifier::Control);
    CHECK(xtermMouseModifiers(true, false, true) == (Ubiquitty::Mouse::Modifier::Shift | Ubiquitty::Mouse::Modifier::Control)); // 20
    CHECK(xtermMouseModifiers(true, true, true) == (Ubiquitty::Mouse::Modifier::Shift | Ubiquitty::Mouse::Modifier::Meta | Ubiquitty::Mouse::Modifier::Control)); // 28

    // ---- ⌘1–9 tab jump: accelerator-only + digit 1..9 ----
    CHECK(commandNumberTabPosition(ModifierState::AcceleratorOnly, '1') == 1);
    CHECK(commandNumberTabPosition(ModifierState::AcceleratorOnly, '9') == 9);
    CHECK(commandNumberTabPosition(ModifierState::AcceleratorOnly, '5') == 5);
    CHECK(commandNumberTabPosition(ModifierState::AcceleratorOnly, '0') == 0); // 0 is not a tab slot
    CHECK(commandNumberTabPosition(ModifierState::AcceleratorOnly, ':') == 0); // ':' == '9'+1, out of range
    CHECK(commandNumberTabPosition(ModifierState::AcceleratorOnly, 'a') == 0); // non-digit
    CHECK(commandNumberTabPosition(ModifierState::OtherModifiers, '3') == 0); // other modifiers down

    // ---- viewport-hold while selecting: push viewOffset by the rows appended ----
    CHECK(heldViewOffsetForSelection(0, 5, 3) == 3); // 3 appended ⇒ +3
    CHECK(heldViewOffsetForSelection(4, 5, 5) == 5); // 5 appended, clamped to sb 5
    CHECK(heldViewOffsetForSelection(3, 2, 0) == 3); // nothing appended ⇒ unchanged
    CHECK(heldViewOffsetForSelection(1, 4, 2) == 3); // history full: 2 appended, 1 retained ⇒ +2

    // ---- wheel / hover button codes (the magic numbers, defined once) ----
    CHECK(xtermWheelButtonCode(WheelDirection::Up) == 64); // wheel up
    CHECK(xtermWheelButtonCode(WheelDirection::Down) == 65); // wheel down
    // Horizontal wheel → left 66 / right 67 (xterm buttons 6/7). Both stay in
    // the 0x40 wheel class alongside up/down.
    CHECK(xtermWheelButtonCode(WheelDirection::Left) == 66);
    CHECK(xtermWheelButtonCode(WheelDirection::Right) == 67);
    CHECK((xtermWheelButtonCode(WheelDirection::Left) & 0x40) != 0);

    // ---- MotionDedup: cell-granular modes key off (row, column) ----
    {
        MotionDedup d;
        CHECK(d.shouldReport(0, 0)); // first ever cell ⇒ report
        CHECK(!d.shouldReport(0, 0)); // same cell ⇒ suppressed
        CHECK(d.shouldReport(0, 1)); // column moved ⇒ report
        CHECK(!d.shouldReport(0, 1)); // same ⇒ suppressed
        CHECK(d.shouldReport(1, 1)); // row moved ⇒ report
        CHECK(!d.shouldReport(1, 1)); // same ⇒ suppressed
        d.reset();
        CHECK(d.shouldReport(1, 1)); // after reset the same cell reports again
    }

    // ---- MotionDedup: SGR-pixel mode (1016) keys off device pixel ----
    {
        MotionDedup d;
        CHECK(d.shouldReport1016(10, 20)); // first ever pixel ⇒ report
        CHECK(!d.shouldReport1016(10, 20)); // same pixel ⇒ suppressed
        CHECK(d.shouldReport1016(11, 20)); // sub-cell x move ⇒ report
        CHECK(d.shouldReport1016(11, 21)); // sub-cell y move ⇒ report
        CHECK(!d.shouldReport1016(11, 21)); // same ⇒ suppressed
        d.reset();
        CHECK(d.shouldReport1016(11, 21)); // after reset the same pixel reports again
        // pixel 0,0 is a real coordinate, distinct from the INT_MIN sentinel.
        MotionDedup e;
        CHECK(e.shouldReport1016(0, 0)); // origin pixel reports on a fresh dedup
        CHECK(!e.shouldReport1016(0, 0)); // and then dedups
    }

    // ---- MotionDedup: noteCell keeps the cell sentinel coherent across a
    //      1016→cell mode flip (mirrors the macOS dual-sentinel update) ----
    {
        MotionDedup d;
        CHECK(d.shouldReport1016(40, 50)); // pixel-mode report
        d.noteCell(2, 3); // record the cell the pixel report landed in
        CHECK(!d.shouldReport(2, 3)); // mode flips to cell granularity ⇒ no re-fire
        CHECK(d.shouldReport(2, 4)); // a genuine cell move still reports
    }

    // --- wheelDirectionFromScrollDelta: dominant axis, vertical wins ties;
    //     the horizontal pair reports as xterm buttons 6/7 (Cb 66/67). ---
    {
        using Ubiquitty::Input::WheelDirection;
        using Ubiquitty::Input::wheelDirectionFromScrollDelta;
        using Ubiquitty::Input::xtermWheelButtonCode;
        CHECK(wheelDirectionFromScrollDelta(0, 5) == WheelDirection::Up);
        CHECK(wheelDirectionFromScrollDelta(0, -5) == WheelDirection::Down);
        CHECK(wheelDirectionFromScrollDelta(5, 0) == WheelDirection::Left); // rightward finger → content left
        CHECK(wheelDirectionFromScrollDelta(-5, 0) == WheelDirection::Right);
        CHECK(wheelDirectionFromScrollDelta(3, 5) == WheelDirection::Up); // vertical dominates
        CHECK(wheelDirectionFromScrollDelta(5, 3) == WheelDirection::Left); // horizontal dominates
        CHECK(wheelDirectionFromScrollDelta(4, 4) == WheelDirection::Up); // tie → vertical
        CHECK(xtermWheelButtonCode(WheelDirection::Left) == 66);
        CHECK(xtermWheelButtonCode(WheelDirection::Right) == 67);
    }

    // --- wheelDominantDelta: the delta on the axis the direction rule picks,
    // so a caller can throttle exactly the axis it is about to report.
    {
        using Ubiquitty::Input::wheelDominantDelta;
        CHECK(wheelDominantDelta(0, 3) == 3);
        CHECK(wheelDominantDelta(0, -3) == -3);
        CHECK(wheelDominantDelta(4, 0) == 4);
        CHECK(wheelDominantDelta(-4, 0) == -4);
        CHECK(wheelDominantDelta(2, 5) == 5);
        CHECK(wheelDominantDelta(5, 2) == 5);
        CHECK(wheelDominantDelta(3, 3) == 3); // tie → vertical
        CHECK(wheelDominantDelta(3, -3) == -3); // tie → vertical, sign kept
    }
}

// When a hover may be re-resolved with no mouse move behind it.
TEST(shouldReResolveLinkHoverRefusesTheFourCases)
{
    using Ubiquitty::Input::HoverReResolveFacts;
    using Ubiquitty::Input::shouldReResolveLinkHover;

    const HoverReResolveFacts ready { .isSelecting = false,
        .isPointerHiddenForTyping = false,
        .isWindowKey = true,
        .isPointerInsideView = true };
    CHECK(shouldReResolveLinkHover(ready));

    // A drag owns the pointer: it is driving a selection, not asking what is
    // under it.
    HoverReResolveFacts dragging = ready;
    dragging.isSelecting = true;
    CHECK(!shouldReResolveLinkHover(dragging));

    // Typing put the pointer away, so there is nothing to put a hand on.
    HoverReResolveFacts hidden = ready;
    hidden.isPointerHiddenForTyping = true;
    CHECK(!shouldReResolveLinkHover(hidden));

    // The one that bites: native window tabs share a frame, so a background
    // tab re-resolving on its own output reads one pointer position with the
    // foreground tab, and a link under it would set the hand for the whole
    // screen.
    HoverReResolveFacts background = ready;
    background.isWindowKey = false;
    CHECK_MSG(!shouldReResolveLinkHover(background),
        "only the window that owns the pointer may answer for it");

    // Outside the view there is nothing of this view's under the pointer.
    HoverReResolveFacts outside = ready;
    outside.isPointerInsideView = false;
    CHECK(!shouldReResolveLinkHover(outside));
}

UBIQUITTY_TEST_MAIN()
