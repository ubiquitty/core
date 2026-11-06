// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Runtime/SurfaceFocus.h"
#include "Harness.h"

// test_surface_focus.cpp — pins Runtime::surfaceFocus: which conjunction of
// window facts is focus, and that arrival alone latches the caret. Every macOS
// window kind reads it, so it is pinned here rather than once per frontend;
// each case below drops exactly one term of the conjunction. Also pins
// Runtime::touchSurfaceFocus, the touch shells' rule. Bare libc++; no platform
// code.

using Ubiquitty::Modes::FocusState;
using Ubiquitty::Runtime::surfaceFocus;
using Ubiquitty::Runtime::SurfaceFocusFacts;
using Ubiquitty::Runtime::TouchKeyboardHolder;
using Ubiquitty::Runtime::touchSurfaceFocus;

namespace {

SurfaceFocusFacts focusedFacts()
{
    SurfaceFocusFacts facts;
    facts.isWindowKey = true;
    facts.isFirstResponder = true;
    facts.isApplicationActive = true;
    return facts;
}

} // namespace

TEST(testFocusTakesAllThreeFacts)
{
    CHECK(surfaceFocus(focusedFacts(), FocusState::Unfocused).focus == FocusState::Focused);
}

// The responder-only rule: a window losing key status moved no responder, so a
// shell reading only the responder kept the caret blinking through a switch to
// another application.
TEST(testAWindowLosingKeyStatusUnfocusesItsPane)
{
    SurfaceFocusFacts facts = focusedFacts();
    facts.isWindowKey = false;
    CHECK(surfaceFocus(facts, FocusState::Focused).focus == FocusState::Unfocused);
}

// The window-level rule: a split window is key as a whole, and a pane fed the
// window's answer keeps a caret it has no keyboard for.
TEST(testAPaneThatIsNotTheResponderIsUnfocused)
{
    SurfaceFocusFacts facts = focusedFacts();
    facts.isFirstResponder = false;
    CHECK(surfaceFocus(facts, FocusState::Focused).focus == FocusState::Unfocused);
}

TEST(testAnInactiveApplicationUnfocusesEveryPane)
{
    SurfaceFocusFacts facts = focusedFacts();
    facts.isApplicationActive = false;
    CHECK(surfaceFocus(facts, FocusState::Focused).focus == FocusState::Unfocused);
}

// The note is the arrival edge only: a recompute answering the state already
// published would re-stamp the hold, and a window under a run of edges would
// never blink.
TEST(testTheActivityNoteIsTheArrivalEdgeOnly)
{
    CHECK(surfaceFocus(focusedFacts(), FocusState::Unfocused).wantsActivityNote);
    CHECK_MSG(!surfaceFocus(focusedFacts(), FocusState::Focused).wantsActivityNote, "a recompute that changes nothing does not re-stamp the hold");
    SurfaceFocusFacts away = focusedFacts();
    away.isWindowKey = false;
    CHECK(!surfaceFocus(away, FocusState::Unfocused).wantsActivityNote);
    CHECK(!surfaceFocus(away, FocusState::Focused).wantsActivityNote);
}

// The touch rule reads who holds the keyboard, never the surface's own
// responder status: the surface takes first responder for the selection menu
// and hands it to the keyboard's input view on the next tap, and a rule that
// reads that handover as focus leaving keeps the hollow outline from then on
// while every key reaches the shell.
TEST(testATouchTerminalHoldingItsKeyboardIsFocused)
{
    CHECK(touchSurfaceFocus(TouchKeyboardHolder::Terminal, FocusState::Unfocused).focus == FocusState::Focused);
    CHECK(touchSurfaceFocus(TouchKeyboardHolder::Terminal, FocusState::Focused).focus == FocusState::Focused);
}

TEST(testAnotherTextInputUnfocusesATouchTerminal)
{
    CHECK(touchSurfaceFocus(TouchKeyboardHolder::OtherTextInput, FocusState::Focused).focus == FocusState::Unfocused);
    CHECK(touchSurfaceFocus(TouchKeyboardHolder::OtherTextInput, FocusState::Unfocused).focus == FocusState::Unfocused);
}

TEST(testTheTouchActivityNoteIsTheArrivalEdgeOnly)
{
    CHECK_MSG(touchSurfaceFocus(TouchKeyboardHolder::Terminal, FocusState::Unfocused).wantsActivityNote, "a find field closing holds the caret solid as it returns");
    CHECK(!touchSurfaceFocus(TouchKeyboardHolder::Terminal, FocusState::Focused).wantsActivityNote);
    CHECK(!touchSurfaceFocus(TouchKeyboardHolder::OtherTextInput, FocusState::Focused).wantsActivityNote);
}

UBIQUITTY_TEST_MAIN()
