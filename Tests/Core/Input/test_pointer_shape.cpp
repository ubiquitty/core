// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_pointer_shape.cpp — which pointer shape the terminal's state calls for.
//
// The cases overlap, so the ORDER is the whole content of this decision and a
// test that only checked each shape in isolation would pass on any permutation.
// Every case below pins a rank against the one directly beneath it: both
// conditions hold, and the assertion is which one wins.
//
// The shapes are portable; the mapping to a native cursor is not, and stays in
// the frontends.

#include "Harness.h"
#include <memory>

#include "Core/Input/PointerShape.h"
#include "Core/OSC/MousePointerShape.h"
#include "Core/Runtime/RuntimeLink.h"
#include "Core/Tek/Tek4014.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstring>

namespace {

using namespace Ubiquitty;
using Input::PointerContext;
using Input::pointerShape;

std::unique_ptr<Terminal> makeTerminal()
{
    auto t = std::make_unique<Terminal>();
    Ubiquitty::resize(*t, 24, 80);
    return t;
}

void feed(Terminal& t, const char* bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

// Puts an application-requested shape on the OSC 22 stack.
void pushAppShape(Terminal& t, MousePointerShape shape)
{
    t.mousePointerShapeStack[0] = shape;
    t.mousePointerShapeStackLength = 1;
}

TEST(testPlainTextIsAnIBeam)
{
    auto t = makeTerminal();
    CHECK_MSG(pointerShape(t.get(), {}) == MousePointerShape::Text,
        "selectable text advertises itself");
    t.reset();
}

// Nothing to select past the end of a row.
TEST(testTheTrailingVoidIsNotSelectable)
{
    auto t = makeTerminal();
    PointerContext context;
    context.isOverTextVoid = true;
    CHECK(pointerShape(t.get(), context) == MousePointerShape::Default);
    t.reset();
}

// A drag already running keeps the I-beam out over the void.
//
// The two facts are true together for most of a real drag — a reader sweeping
// to the end of a line passes the last glyph long before releasing — and the
// arrow appearing mid-gesture reads as the drag having been dropped. The
// affordance answers a question nobody is asking any more: the pointer is
// driving a selection, not advertising where text is.
TEST(testADragInFlightKeepsTheIBeamOverTheVoid)
{
    auto t = makeTerminal();
    PointerContext context;
    context.isOverTextVoid = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Default,
        "with no drag running the void is not selectable");

    context.isSelectionDragInFlight = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Text,
        "a drag in flight keeps the I-beam over the void");

    // Text is never a void, dragged over or not: the rung reads both facts,
    // so neither alone decides it.
    context.isOverTextVoid = false;
    CHECK(pointerShape(t.get(), context) == MousePointerShape::Text);
    context.isSelectionDragInFlight = false;
    CHECK(pointerShape(t.get(), context) == MousePointerShape::Text);
    context.isOverTextVoid = true;
    context.isSelectionDragInFlight = true;

    // It outranks the void and nothing above it: a tracking application still
    // owns the pointer, because a drag it received opened no selection.
    feed(*t, "\033[?1000h");
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Default,
        "a tracking application still outranks it");
    t.reset();
}

// --- Rank boundaries. Each sets both conditions and names the winner. --------

// A tracking application owns the clicks, so an I-beam would advertise a
// selection the click will never make.
TEST(testTrackingOutranksTheIBeam)
{
    auto t = makeTerminal();
    CHECK(pointerShape(t.get(), {}) == MousePointerShape::Text);

    feed(*t, "\033[?1000h");
    CHECK_MSG(pointerShape(t.get(), {}) == MousePointerShape::Default,
        "a tracking application outranks the selectable-text affordance");

    // Default is also what the trailing-void rank below produces, so the pair
    // above only discriminates while the void is false — which it is, by
    // default. Naming it so a later edit cannot quietly set it.
    PointerContext context;
    context.isOverTextVoid = false;
    CHECK(pointerShape(t.get(), context) == MousePointerShape::Default);
    t.reset();
}

// Rectangular select is shown over a tracking application only when the
// frontend's bypass is armed — otherwise the application still owns the pointer.
TEST(testRectangularSelectYieldsToTrackingUnlessBypassed)
{
    auto t = makeTerminal();
    PointerContext context;
    context.isRectangularSelectArmed = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Crosshair,
        "armed with nothing tracking, the crosshair shows");

    feed(*t, "\033[?1002h");
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Default,
        "a tracking application outranks an armed rectangular select");

    context.selectionBypassesMouseReporting = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Crosshair,
        "unless the drag is allowed to bypass reporting");
    t.reset();
}

// The linear reach-through modifier held over a tracking application shows the
// I-beam: the drag it promises is the one the press will make. A link still
// takes the hand above it, and the trailing void still takes the arrow.
TEST(testTheReachThroughModifierShowsTheIBeamOverATrackingApplication)
{
    auto t = makeTerminal();
    feed(*t, "\033[?1002h");
    PointerContext context;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Default,
        "with nothing held, the application owns the pointer");

    context.selectionBypassesMouseReporting = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Text,
        "held, a drag reaches through, and the I-beam says so before the press");

    context.isOverLink = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Pointer,
        "a link under the held modifier still takes the hand");

    context.isOverLink = false;
    context.isOverTextVoid = true;
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Default,
        "and past the end of the row there is still nothing to reach for");
    t.reset();
}

// In a chromeless window the drag wins regardless of what else is armed.
TEST(testWindowDragOutranksRectangularSelect)
{
    auto t = makeTerminal();
    PointerContext context;
    context.isRectangularSelectArmed = true;
    context.isWindowDragArmed = true;
    CHECK(pointerShape(t.get(), context) == MousePointerShape::Grab);
    t.reset();
}

// A vector surface has no text cells, so never an I-beam — and a live
// graphic-input crosshair outranks the plain arrow.
TEST(testTektronixOutranksSelectionAffordances)
{
    auto t = makeTerminal();
    PointerContext context;
    context.isWindowDragArmed = true;
    context.isRectangularSelectArmed = true;

    feed(*t, "\033[?38h"); // DECSET 38 — enter Tektronix mode
    CHECK_MSG(t->isTekModeActive, "the terminal is in Tek mode");
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Default,
        "Tek outranks both the window drag and the rectangular select");

    feed(*t, "\033\032"); // ESC SUB — graphic input, crosshair up
    CHECK_MSG(Ubiquitty::tekGINActive(*t), "graphic input is armed");
    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Crosshair,
        "and graphic input outranks the plain Tek arrow");
    t.reset();
}

// An application driving widgets owns the pointer.
TEST(testApplicationShapeOutranksTektronixAndTracking)
{
    auto t = makeTerminal();
    feed(*t, "\033[?38h");
    feed(*t, "\033[?1003h");
    pushAppShape(*t, MousePointerShape::Wait);

    CHECK_MSG(pointerShape(t.get(), {}) == MousePointerShape::Wait,
        "the application's own shape outranks Tek and tracking");
    t.reset();
}

// The one thing an application cannot take: a hyperlink stays clickable, so its
// affordance outranks whatever shape the application asked for.
TEST(testALinkOutranksTheApplicationShape)
{
    auto t = makeTerminal();
    pushAppShape(*t, MousePointerShape::Crosshair);
    PointerContext context;
    context.isOverLink = true;

    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Pointer,
        "a hyperlink stays clickable under an application-requested shape");
    t.reset();
}

// Nothing is drawn while the pointer is grabbed for relative motion, so this
// outranks every other case including a link.
//
// Asserted against a state whose every other rank yields something else. Default
// is what four ranks produce, so a case that merely sets the lock alongside
// conditions that also resolve to Default cannot tell which one answered — it
// would keep passing with the lock rank deleted.
TEST(testPointerLockOutranksEverything)
{
    auto t = makeTerminal();
    feed(*t, "\033[?38h"); // Tek would give Default
    pushAppShape(*t, MousePointerShape::Wait); // the app shape would give Wait
    PointerContext context;
    context.isOverLink = true; // a link would give Pointer
    context.isRectangularSelectArmed = true;

    CHECK_MSG(pointerShape(t.get(), context) == MousePointerShape::Pointer,
        "unlocked, the link wins — so Default below can only be the lock");

    context.isPointerLocked = true;
    CHECK(pointerShape(t.get(), context) == MousePointerShape::Default);
    t.reset();
}

// The stack's top is the live shape, so a push while one is set replaces it and
// popping the last one returns the terminal to its own affordances.
TEST(testTheApplicationShapeFollowsTheStackTop)
{
    auto t = makeTerminal();
    t->mousePointerShapeStack[0] = MousePointerShape::Wait;
    t->mousePointerShapeStack[1] = MousePointerShape::Help;
    t->mousePointerShapeStackLength = 2;
    CHECK_MSG(pointerShape(t.get(), {}) == MousePointerShape::Help, "the top is the live shape");

    t->mousePointerShapeStackLength = 0;
    CHECK_MSG(pointerShape(t.get(), {}) == MousePointerShape::Text, "an empty stack returns the I-beam");
    t.reset();
}

// A frontend without a terminal still gets an answer rather than a crash, and
// the answer is the fall-through: with no terminal nothing is tracking and no
// application has asked for anything, so the text affordance is correct.
TEST(testNoTerminalStillResolves)
{
    CHECK(pointerShape(nullptr, {}) == MousePointerShape::Text);

    PointerContext context;
    context.isOverLink = true;
    CHECK_MSG(pointerShape(nullptr, context) == MousePointerShape::Pointer,
        "and frontend-known facts still rank");

    context = {};
    context.isPointerLocked = true;
    CHECK(pointerShape(nullptr, context) == MousePointerShape::Default);
}

// The stack's top is one reading, shared by the dispatcher's report, the
// ladder and the published snapshot. SET is a separate answer from the shape:
// an empty stack reads Default and is not an ask.
TEST(testTheStackTopIsOneReading)
{
    auto t = makeTerminal();
    CHECK(!OSC::isAppPointerShapeSet(*t));
    CHECK(OSC::appRequestedPointerShape(*t) == MousePointerShape::Default);

    pushAppShape(*t, MousePointerShape::Help);
    CHECK(OSC::isAppPointerShapeSet(*t));
    CHECK_MSG(OSC::appRequestedPointerShape(*t) == MousePointerShape::Help, "the top is the live shape");

    pushAppShape(*t, MousePointerShape::Default);
    CHECK_MSG(OSC::isAppPointerShapeSet(*t), "an explicit default is still an ask");
    t.reset();
}

// The published snapshot ranks exactly as the live model does, because a
// frontend off the lock reads the same ladder rather than a shorter copy of
// it. Each state is asserted against the answer the model-side cases above
// pin for the same facts.
TEST(testTheSnapshotRanksAsTheModelDoes)
{
    Runtime::RuntimeSnapshot snapshot;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Text, "plain text is an I-beam off the snapshot");

    snapshot.mouseReportsActive = true;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Default, "a tracking application owns the clicks");
    PointerContext context;
    context.isRectangularSelectArmed = true;
    CHECK_MSG(pointerShape(snapshot, context) == MousePointerShape::Default, "rectangular select yields to tracking");
    context.selectionBypassesMouseReporting = true;
    CHECK_MSG(pointerShape(snapshot, context) == MousePointerShape::Crosshair, "unless the drag bypasses it");

    snapshot.isAppPointerShapeSet = true;
    snapshot.appRequestedPointerShape = MousePointerShape::Wait;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Wait, "the application's shape outranks tracking");
    context = {};
    context.isOverLink = true;
    CHECK_MSG(pointerShape(snapshot, context) == MousePointerShape::Pointer, "and a link outranks the application");
    context.isPointerLocked = true;
    CHECK_MSG(pointerShape(snapshot, context) == MousePointerShape::Default, "and the lock outranks the link");

    // An explicit "default" is an ask, honoured over the affordances below
    // it: the SET bit is what separates it from no ask at all.
    snapshot = {};
    snapshot.isAppPointerShapeSet = true;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Default, "an explicit default shows the arrow");
    snapshot.isAppPointerShapeSet = false;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Text, "no ask shows the terminal's own I-beam");

    snapshot = {};
    snapshot.isTekModeActive = true;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Default, "Tektronix is never an I-beam");
    snapshot.isTekGraphicInputActive = true;
    CHECK_MSG(pointerShape(snapshot, {}) == MousePointerShape::Crosshair, "Tek graphic input is the crosshair");
    context = {};
    context.isWindowDragArmed = true;
    snapshot = {};
    CHECK_MSG(pointerShape(snapshot, context) == MousePointerShape::Grab, "a chromeless window drag is the hand");
    context = {};
    context.isOverTextVoid = true;
    CHECK_MSG(pointerShape(snapshot, context) == MousePointerShape::Default, "the trailing void is not selectable");
}

} // namespace

UBIQUITTY_TEST_MAIN()
