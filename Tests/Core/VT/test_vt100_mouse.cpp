// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Pointer reporting: which events are sent, in which encoding, under which
// mode.
//
// The tracking modes (X10, normal, button-event, any-event), the encodings that
// carry them (X10, UTF-8, SGR, URXVT, pixel), the DEC locator, and the highlight
// tracking protocol that hands the terminal a region and waits.
//
// Encoding and mode are separable in the protocol and inseparable in practice —
// a coordinate past 223 is a different byte sequence in every encoding, and a
// motion event is reportable in some modes and not others — so the cases sweep
// the product rather than the axes.

#include "Harness.h"
#include "VT100Fixture.h"

/// OSC 1337 SetColors=preset=<name> → WindowClient::selectColorPreset (live
/// theme switch). The frontend resolves the name; Core just relays it.
namespace {
struct PointerShapeClient : Ubiquitty::WindowClient {
    int calls = 0;
    Ubiquitty::MousePointerShape last = Ubiquitty::MousePointerShape::Default;
    void setMousePointerShape(Ubiquitty::MousePointerShape shape) override
    {
        calls++;
        last = shape;
    }
};
}

TEST(testHiliteTracking)
{
    Ubiquitty::Terminal t;
    char outputBuffer[32];
    Ubiquitty::hardReset(t);

    feed(t, "\033[?1001$p"); // DECRQM 1001 — reset by default
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1001;2$y") == 0);
    feed(t, "\033[?1001h");
    feed(t, "\033[?1001$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1001;1$y") == 0);

    // Host region reply CSI func;sx;sy;fr;lr T — cached, never blocks.
    feed(t, "\033[1;5;7;3;12T");
    CHECK(t.modes.hiliteRegionValid == true);
    CHECK(t.modes.hiliteStartX == 5);
    CHECK(t.modes.hiliteStartY == 7);
    CHECK(t.modes.hiliteFirstRow == 3);
    CHECK(t.modes.hiliteLastRow == 12);
    feed(t, "\033[0;1;1;1;1T"); // func=0 aborts highlighting
    CHECK(t.modes.hiliteRegionValid == false);

    // Hilite off: CSI Ps T is still SD (scroll down), not a region reply.
    feed(t, "\033[?1001l");
    feed(t, "\033[2T");
    CHECK(t.modes.hiliteRegionValid == false);
}

TEST(testHiliteBoundedWait)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    /// No await before mode 1001 is on: beginHiliteAwait is a no-op.
    Ubiquitty::Mouse::beginHiliteAwait(&t, 100);
    CHECK(t.modes.hiliteAwaiting == false);

    feed(t, "\033[?1001h");

    /// Press → arm the bounded wait at the press tick.
    Ubiquitty::Mouse::beginHiliteAwait(&t, 1000);
    CHECK(t.modes.hiliteAwaiting == true);
    CHECK(t.modes.hiliteRegionValid == false);

    // Before the deadline tickHilite leaves the await alone (no abort).
    CHECK(Ubiquitty::Mouse::tickHilite(&t, 1000) == false);
    CHECK(Ubiquitty::Mouse::tickHilite(&t, 1000 + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks - 1) == false);
    CHECK(t.modes.hiliteAwaiting == true);

    // XTHIMOUSE arrives before the deadline → region live, wait cleared.
    feed(t, "\033[1;3;4;2;6T");
    CHECK(t.modes.hiliteRegionValid == true);
    CHECK(t.modes.hiliteAwaiting == false);
    // Now-stale tick must not re-trigger anything.
    CHECK(Ubiquitty::Mouse::tickHilite(&t, 1000000) == false);
    CHECK(t.modes.hiliteRegionValid == true);

    /// Second press with no reply: advance the clock past the deadline and the
    /// await aborts — no hang, no stranded highlight.
    Ubiquitty::Mouse::beginHiliteAwait(&t, 2000);
    CHECK(t.modes.hiliteAwaiting == true);
    CHECK(t.modes.hiliteRegionValid == false); // fresh press drops the old region
    const uint32_t deadline = 2000 + Ubiquitty::Mouse::hiliteAwaitDeadlineTicks;
    CHECK(Ubiquitty::Mouse::tickHilite(&t, deadline) == true);
    CHECK(t.modes.hiliteAwaiting == false);
    CHECK(t.modes.hiliteRegionValid == false);
    // Idempotent once aborted.
    CHECK(Ubiquitty::Mouse::tickHilite(&t, deadline + 100) == false);

    /// Leaving mode 1001 mid-wait clears the await too.
    Ubiquitty::Mouse::beginHiliteAwait(&t, 3000);
    CHECK(t.modes.hiliteAwaiting == true);
    feed(t, "\033[?1001l");
    CHECK(t.modes.hiliteAwaiting == false);
    CHECK(t.modes.hiliteRegionValid == false);
}

TEST(testHiliteEndReport)
{
    char outputBuffer[16];
    int n;

    // Short form: end column, end row — two bytes after CSI t.
    n = Ubiquitty::Mouse::Locator::encodeHiliteEndReport(
        /*startColumn*/ 5, /*startRow*/ 7, /*endColumn*/ 9, /*endRow*/ 11,
        /*mouseCol*/ 9, /*mouseRow*/ 11, Ubiquitty::Mouse::Locator::HiliteEndForm::RegionOnly, outputBuffer);
    CHECK(n == 5);
    CHECK(outputBuffer[0] == '\033' && outputBuffer[1] == '[' && outputBuffer[2] == 't');
    CHECK((unsigned char)outputBuffer[3] == (unsigned char)(9 + 0x20));
    CHECK((unsigned char)outputBuffer[4] == (unsigned char)(11 + 0x20));

    // Long form: startcol,startrow,endcol,endrow,mousecol,mouserow.
    n = Ubiquitty::Mouse::Locator::encodeHiliteEndReport(5, 7, 9, 11, 13, 15, Ubiquitty::Mouse::Locator::HiliteEndForm::RegionAndPointer, outputBuffer);
    CHECK(n == 9);
    CHECK(outputBuffer[0] == '\033' && outputBuffer[1] == '[' && outputBuffer[2] == 'T');
    CHECK((unsigned char)outputBuffer[3] == (unsigned char)(5 + 0x20));
    CHECK((unsigned char)outputBuffer[4] == (unsigned char)(7 + 0x20));
    CHECK((unsigned char)outputBuffer[5] == (unsigned char)(9 + 0x20));
    CHECK((unsigned char)outputBuffer[6] == (unsigned char)(11 + 0x20));
    CHECK((unsigned char)outputBuffer[7] == (unsigned char)(13 + 0x20));
    CHECK((unsigned char)outputBuffer[8] == (unsigned char)(15 + 0x20));

    // Coordinates clamp into the single-byte range (no overflow).
    n = Ubiquitty::Mouse::Locator::encodeHiliteEndReport(1, 1, 9999, 9999, 1, 1, Ubiquitty::Mouse::Locator::HiliteEndForm::RegionOnly, outputBuffer);
    CHECK(n == 5);
    CHECK((unsigned char)outputBuffer[3] == 0xFF);
    CHECK((unsigned char)outputBuffer[4] == 0xFF);

    // Too small a buffer reports failure, writes nothing.
    n = Ubiquitty::Mouse::Locator::encodeHiliteEndReport(1, 1, 1, 1, 1, 1, Ubiquitty::Mouse::Locator::HiliteEndForm::RegionAndPointer, { outputBuffer, 4 });
    CHECK(n == 0);
}

TEST(testOSC22MousePointerShape)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    PointerShapeClient client;
    t.windowClient = &client;

    // Set: OSC 22 ; pointer → Pointer.
    feed(t, "\033]22;pointer\007");
    CHECK(client.calls == 1);
    CHECK(client.last == Ubiquitty::MousePointerShape::Pointer);

    // Empty payload resets to Default.
    feed(t, "\033]22;\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Default);

    // Set to text (I-beam) then push grab; the top is the live shape.
    feed(t, "\033]22;text\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Text);
    feed(t, "\033]22;>grab\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Grab);
    // Pop restores the prior shape (text).
    feed(t, "\033]22;<\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Text);

    // Comma list: last resolvable name wins.
    feed(t, "\033]22;crosshair,wait\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Wait);

    // Unknown name on a SET is ignored → stack empty → Default.
    feed(t, "\033]22;bogus-name\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Default);

    // Axis aliases share a bucket; a directional name keeps its direction.
    feed(t, "\033]22;ew-resize\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::ColResize);
    feed(t, "\033]22;row-resize\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::RowResize);
    feed(t, "\033]22;n-resize\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::NorthResize);

    // Pop on an empty stack stays at Default (no underflow).
    feed(t, "\033]22;\007"); // reset
    feed(t, "\033]22;<\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Default);

    // '?' support query replies framed as OSC 22 ; ... ST.
    char outputBuffer[256];
    feed(t, "\033]22;?pointer,bogus\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]22;1,0\033\\") == 0);

    t.windowClient = nullptr;
}

TEST(testOSC22CurrentQueryNamesEveryShape)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // __current__ reports the live shape by its canonical wire name, which for
    // the axis resize buckets differs from the alias that set it (col-resize
    // sets the bucket ew-resize names). One probe per portable shape, so a wire
    // name dropped from the reply switch fails on the shape it belongs to.
    struct ShapeProbe {
        const char* setName;
        const char* wireName;
    };
    static constexpr ShapeProbe probes[] = {
        { "default", "default" },
        { "pointer", "pointer" },
        { "text", "text" },
        { "vertical-text", "vertical-text" },
        { "crosshair", "crosshair" },
        { "cell", "cell" },
        { "move", "move" },
        { "grab", "grab" },
        { "grabbing", "grabbing" },
        { "no-drop", "no-drop" },
        { "not-allowed", "not-allowed" },
        { "alias", "alias" },
        { "copy", "copy" },
        { "help", "help" },
        { "progress", "progress" },
        { "wait", "wait" },
        { "zoom-in", "zoom-in" },
        { "zoom-out", "zoom-out" },
        { "col-resize", "ew-resize" },
        { "row-resize", "ns-resize" },
        { "nwse-resize", "nwse-resize" },
        { "nesw-resize", "nesw-resize" },
        { "n-resize", "n-resize" },
        { "ne-resize", "ne-resize" },
        { "e-resize", "e-resize" },
        { "se-resize", "se-resize" },
        { "s-resize", "s-resize" },
        { "sw-resize", "sw-resize" },
        { "w-resize", "w-resize" },
        { "nw-resize", "nw-resize" },
    };
    char outputBuffer[256];
    for (const ShapeProbe& probe : probes) {
        feed(t, "\033]22;");
        feed(t, probe.setName);
        feed(t, "\007");
        feed(t, "\033]22;?__current__\033\\");
        drain(t, outputBuffer);
        char want[64];
        std::snprintf(want, sizeof(want), "\033]22;%s\033\\", probe.wireName);
        CHECK_MSG(strcmp(outputBuffer, want) == 0, probe.setName);
    }

    // An empty stack reports 0, and __default__ / __grabbed__ both
    // resolve to "default" whatever the stack holds.
    feed(t, "\033]22;\007");
    feed(t, "\033]22;?__current__,__default__,__grabbed__\033\\");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033]22;0,default,default\033\\") == 0);
}

TEST(testOSC22StackOverflowDropsTheOldest)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    PointerShapeClient client;
    t.windowClient = &client;

    // The stack holds 16. Set text, then push 16 grabs: the 17th entry evicts
    // text from the bottom, so popping everything above the last survivor
    // lands on grab, and one more pop empties to Default — text never returns.
    feed(t, "\033]22;text\007");
    for (int i = 0; i < 16; ++i)
        feed(t, "\033]22;>grab\007");
    for (int i = 0; i < 15; ++i)
        feed(t, "\033]22;<\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Grab);
    feed(t, "\033]22;<\007");
    CHECK(client.last == Ubiquitty::MousePointerShape::Default);

    t.windowClient = nullptr;
}

TEST(testOSC22QueryReplyStopsAtItsBuffer)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // A name list whose answers outgrow the reply buffer stops appending and
    // still frames what it kept: the reply ends in ST at no more than the
    // buffer's capacity, rather than truncating mid-token or overrunning.
    std::string query = "\033]22;?";
    for (int i = 0; i < 60; ++i)
        query += "pointer,";
    query += "pointer\033\\";
    feed(t, query.c_str());
    char outputBuffer[512];
    const int written = drain(t, outputBuffer);
    CHECK(written > 0 && written <= 256);
    CHECK(strncmp(outputBuffer, "\033]22;1,", 7) == 0);
    CHECK(outputBuffer[written - 2] == '\033' && outputBuffer[written - 1] == '\\');
}

TEST(test_mouse_modes_gated_by_allowMouseOps)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    /// Denied: every mouse-reporting ENABLE is a no-op.
    Ubiquitty::Modes::HostPolicy::setMousePolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[?9h"); // X10
    feed(t, "\033[?1000h"); // normal
    feed(t, "\033[?1002h"); // button-event
    feed(t, "\033[?1003h"); // any-event
    feed(t, "\033[?1006h"); // SGR coords
    feed(t, "\033[?1016h"); // SGR-pixels coords
    CHECK(!t.modes.mouseX10);
    CHECK(!t.modes.mouseButtonTracking);
    CHECK(!t.modes.mouseCellMotion);
    CHECK(!t.modes.mouseAnyEvent);
    CHECK(!t.modes.mouseSGR);
    CHECK(!t.modes.mouseSGRPixels);

    /// Allowed (the init default): the ENABLE enters reporting as usual.
    Ubiquitty::Modes::HostPolicy::setMousePolicy(&t, Ubiquitty::OperationPolicy::Allowed);
    feed(t, "\033[?1000h");
    feed(t, "\033[?1006h");
    CHECK(t.modes.mouseButtonTracking);
    CHECK(t.modes.mouseSGR);

    /// A DISABLE (DECRST) always applies, even when re-denied — an app can
    /// still turn a mode back off.
    Ubiquitty::Modes::HostPolicy::setMousePolicy(&t, Ubiquitty::OperationPolicy::Denied);
    feed(t, "\033[?1000l");
    CHECK(!t.modes.mouseButtonTracking);
}

TEST(testMouseReportEncoder)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    int n;

    Ubiquitty::hardReset(t);

    t.modes.mouseAnyEvent = true; // tracking mode: the encodings below are orthogonal to it, but motion needs one
    t.modes.mouseSGR = true;
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n == 10);
    CHECK(memcmp(buffer, "\033[<0;10;5M", 10) == 0);

    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n == 10);
    CHECK(memcmp(buffer, "\033[<0;10;5m", 10) == 0);

    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::Left, 0, 0, Ubiquitty::Mouse::Modifier::Shift | Ubiquitty::Mouse::Modifier::Control, buffer);
    CHECK(n == 10);
    CHECK(memcmp(buffer, "\033[<52;1;1M", 10) == 0);

    t.modes.mouseSGR = false;
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[0] == 0x1B);
    CHECK(buffer[1] == '[');
    CHECK(buffer[2] == 'M');
    CHECK((unsigned char)buffer[3] == 32);
    CHECK((unsigned char)buffer[4] == 35);
    CHECK((unsigned char)buffer[5] == 34);

    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 0, Ubiquitty::Mouse::Modifier::Meta, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[3] == 40);
}

TEST(testDecLocatorDECELRDECSLEState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Power-on: disabled, no transitions selected.
    CHECK(t.locatorEnabled == false);
    CHECK(t.locatorReportDown == false);
    CHECK(t.locatorReportUp == false);

    // DECELR Ps1=1: enable, cells (default unit).
    feed(t, "\033[1'z");
    CHECK(t.locatorEnabled == true);
    CHECK(t.locatorOneShot == false);
    CHECK(t.locatorUseRows == true); // cells

    // DECELR Ps2=1: pixels.
    feed(t, "\033[1;1'z");
    CHECK(t.locatorUseRows == false); // pixels

    // DECELR Ps1=2: one-shot enable.
    feed(t, "\033[2'z");
    CHECK(t.locatorEnabled == true);
    CHECK(t.locatorOneShot == true);

    // DECELR Ps1=0: disable.
    feed(t, "\033[0'z");
    CHECK(t.locatorEnabled == false);
    CHECK(t.locatorOneShot == false);

    // DECSLE event selection.
    feed(t, "\033[1'{"); // report button-down
    CHECK(t.locatorReportDown == true);
    feed(t, "\033[3'{"); // report button-up
    CHECK(t.locatorReportUp == true);
    feed(t, "\033[2'{"); // stop reporting button-down
    CHECK(t.locatorReportDown == false);
    CHECK(t.locatorReportUp == true);
    feed(t, "\033[4'{"); // stop reporting button-up
    CHECK(t.locatorReportUp == false);
    // DECSLE 0: explicit-request-only, cancels selections + filter rect.
    feed(t, "\033[1;3'{"); // both on
    CHECK(t.locatorReportDown == true);
    CHECK(t.locatorReportUp == true);
    feed(t, "\033[0'{");
    CHECK(t.locatorReportDown == false);
    CHECK(t.locatorReportUp == false);
}

TEST(testDecLocatorDECRQLPReply)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    // Disabled → CSI 0 & w.
    feed(t, "\033['|");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[0&w") == 0);

    // Enabled, cursor parked at row 5 col 10, but no locator position yet:
    // DEC STD 070 DECRQLP answers Pe = 1 with the coordinates omitted, the
    // locator position undefined; the cursor is not an answer.
    feed(t, "\033[1'z");
    feed(t, "\033[5;10H");
    feed(t, "\033['|");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[1;0&w") == 0);

    // Publish a locator position (what the frontend does per pointer event) with
    // the left button held; the poll now reports it, not the cursor.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 3, 7, Ubiquitty::Mouse::Locator::buttonBit(0), outputBuffer);
    CHECK(n == 0); // nothing armed: position recorded, no report
    feed(t, "\033['|");
    n = drain(t, outputBuffer);
    CHECK(n > 0);
    // Pe=1 request response, Pb=4 left held, Pr=3, Pc=7, Pp=1.
    CHECK(strcmp(outputBuffer, "\033[1;4;3;7;1&w") == 0);
}

TEST(testDecLocatorOneShotDisables)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033[2'z"); // one-shot enable
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 1, 1, 0, outputBuffer);
    CHECK(n == 0);
    feed(t, "\033['|");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[1;0;1;1;1&w") == 0);

    // Second request: locator already auto-disabled → unavailable.
    feed(t, "\033['|");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[0&w") == 0);
}

// DEC STD 070 DECLRP: a one-shot locator is disabled by its single report, and a
// report with the position undefined is still that report.
TEST(testDecLocatorOneShotReportWithoutAPositionDisables)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    Ubiquitty::hardReset(t);

    feed(t, "\033[2'z\033['|");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[1;0&w") == 0);
    CHECK(!t.locatorEnabled);
    feed(t, "\033['|");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[0&w") == 0);
}

TEST(testDecLocatorFilterDefaultsToPosition)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033[1'z");
    // Locator at row 4 col 9.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 4, 9, 0, outputBuffer);
    CHECK(n == 0);

    // All bounds omitted → the rect collapses onto that cell.
    feed(t, "\033['w");
    CHECK(t.locatorFilterActive == true);
    CHECK(t.locatorFilterTop == 4);
    CHECK(t.locatorFilterLeft == 9);
    CHECK(t.locatorFilterBottom == 4);
    CHECK(t.locatorFilterRight == 9);

    // Any motion off that cell leaves the rect: Pe=10, then disarmed.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 4, 10, 0, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[10;0;4;10;1&w") == 0);
    CHECK(t.locatorFilterActive == false);

    // A partially specified rect fills the omitted edges from the position too.
    feed(t, "\033[2;;;'w");
    CHECK(t.locatorFilterTop == 2);
    CHECK(t.locatorFilterLeft == 10);
    CHECK(t.locatorFilterBottom == 4);
    CHECK(t.locatorFilterRight == 10);
}

// Motion inside the filter rectangle does not terminate reporting, and this
// pins the whole arc: an interior move is
// inert and repeatable, the exit still fires Pe=10 exactly once, and a
// persistent locator (DECELR Ps1 = 1) stays armed after that report.
TEST(testDecLocatorMotionInsideTheFilterRectangleIsInert)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033[1'z"); // persistent locator, not one-shot

    // A rectangle with room to move inside it, so an interior step is a real
    // interior step rather than an exit off a single-cell rect.
    feed(t, "\033[3;5;7;15'w");
    CHECK(t.locatorFilterActive == true);

    // Two successive interior moves: both silent, and neither erodes the arm.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 5, 10, 0, outputBuffer);
    CHECK(n == 0);
    CHECK(t.locatorFilterActive == true);
    CHECK(t.locatorEnabled == true);
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 5, 11, 0, outputBuffer);
    CHECK(n == 0);
    CHECK(t.locatorFilterActive == true);
    CHECK(t.locatorEnabled == true);

    // The interior position is still tracked, so DECRQLP can answer from it.
    CHECK(t.locatorRow == 5);
    CHECK(t.locatorColumn == 11);

    // Leaving fires once. Without this the inert-interior checks above could
    // pass on a rectangle that was never really armed.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::None, 5, 16, 0, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[10;0;5;16;1&w") == 0);
    CHECK(t.locatorFilterActive == false);

    // Persistent means persistent: the rectangle disarms, the locator does not.
    CHECK(t.locatorEnabled == true);
}

TEST(testDecLocatorEncodeReport)
{
    char outputBuffer[64];
    int n;

    // Pe=0 → short unavailable form, other fields suppressed.
    n = Ubiquitty::Mouse::Locator::encodeReport(0, 0, 0, 0, 0, outputBuffer);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[0&w") == 0);

    // An undefined position omits Pr, Pc and Pp; the event and buttons stay.
    n = Ubiquitty::Mouse::Locator::encodeReport(1, 2, 0, 0, 1, outputBuffer);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[1;2&w") == 0);

    // Pe=2 left-down, Pb=4 (left held), row 7, col 12, page 1.
    n = Ubiquitty::Mouse::Locator::encodeReport(2, 4, 7, 12, 1, outputBuffer);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[2;4;7;12;1&w") == 0);

    // Button-bit mapping: left=4, middle=2, right=1, M4=8.
    CHECK(Ubiquitty::Mouse::Locator::buttonBit(0) == 0x04);
    CHECK(Ubiquitty::Mouse::Locator::buttonBit(1) == 0x02);
    CHECK(Ubiquitty::Mouse::Locator::buttonBit(2) == 0x01);
    CHECK(Ubiquitty::Mouse::Locator::buttonBit(3) == 0x08);
}

TEST(testDecLocatorButtonEvent)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    // Enable + select both transitions.
    feed(t, "\033[1'z");
    feed(t, "\033[1'{"); // report down
    feed(t, "\033[3'{"); // report up

    /// Left button DOWN at row 3 col 5 (1-based), left held → Pb=4.
    int pb = Ubiquitty::Mouse::Locator::buttonBit(0);
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Press, /*button*/ 0,
        /*row*/ 3, /*col*/ 5, pb, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[2;4;3;5;1&w") == 0); // Pe=2 left-down

    // Left button up at the same cell, no buttons now held → Pb=0, Pe=3.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Release, /*button*/ 0, 3, 5, 0, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[3;0;3;5;1&w") == 0); // Pe=3 left-up

    // Right button DOWN → Pe=6, Pb=1.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Press, /*button*/ 2, 3, 5, Ubiquitty::Mouse::Locator::buttonBit(2), outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[6;1;3;5;1&w") == 0);
}

TEST(testDecLocatorEventSelectionGates)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033[1'z");
    feed(t, "\033[1'{"); // report DOWN only

    // DOWN reports.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Press, 0, 2, 2, Ubiquitty::Mouse::Locator::buttonBit(0), outputBuffer);
    CHECK(n > 0);

    // Up is filtered out (locatorReportUp false).
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Release, 0, 2, 2, 0, outputBuffer);
    CHECK(n == 0);

    // Disable the locator: even a selected DOWN emits nothing.
    feed(t, "\033[0'z");
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Press, 0, 2, 2, Ubiquitty::Mouse::Locator::buttonBit(0), outputBuffer);
    CHECK(n == 0);
    CHECK(Ubiquitty::Mouse::Locator::isActive(&t) == false);
}

TEST(testDecLocatorFilterRectangle)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);

    feed(t, "\033[1'z"); // enable
    // Filter rect: top=5 left=5 bottom=10 right=20 (1-based, inclusive).
    feed(t, "\033[5;5;10;20'w");
    CHECK(t.locatorFilterActive == true);
    CHECK(t.locatorFilterTop == 5);
    CHECK(t.locatorFilterLeft == 5);
    CHECK(t.locatorFilterBottom == 10);
    CHECK(t.locatorFilterRight == 20);

    // Motion still INSIDE the rect (row 7 col 8): no report.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, 3, 7, 8, 0, outputBuffer);
    CHECK(n == 0);
    CHECK(t.locatorFilterActive == true);

    // Motion OUTSIDE (row 2, above the rect): Pe=10 fires once.
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, 3, 2, 8, 0, outputBuffer);
    CHECK(n > 0);
    outputBuffer[n] = 0;
    CHECK(strcmp(outputBuffer, "\033[10;0;2;8;1&w") == 0);
    CHECK(t.locatorFilterActive == false); // disarmed after firing

    // A second leave does not re-fire (rect no longer armed).
    n = Ubiquitty::Mouse::Locator::reportEvent(&t, Ubiquitty::Mouse::EventType::Motion, 3, 1, 1, 0, outputBuffer);
    CHECK(n == 0);

    // DECEFR is ignored when the locator is disabled.
    feed(t, "\033[0'z");
    feed(t, "\033[1;1;2;2'w");
    CHECK(t.locatorFilterActive == false);
}

TEST(testDecLocatorSoftReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[1'z");
    feed(t, "\033[1'{");
    feed(t, "\033[5;5;10;20'w");
    CHECK(t.locatorEnabled == true);
    CHECK(t.locatorFilterActive == true);

    feed(t, "\033[!p"); // DECSTR
    CHECK(t.locatorEnabled == false);
    CHECK(t.locatorOneShot == false);
    CHECK(t.locatorReportDown == false);
    CHECK(t.locatorReportUp == false);
    CHECK(t.locatorFilterActive == false);
}

TEST(testMouseReportX10Overflow)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    int n;
    Ubiquitty::hardReset(t);
    // A tracking mode is on so the encoder emits at all — a modeless
    // terminal refuses every report; which bytes the coordinates become is
    // what this case pins.
    t.modes.mouseButtonTracking = true;

    // column 222 is the last legal X10 column (222 + 33 = 255).
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 222, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[4] == 255);

    // column 223 → byte would be 256, no valid encoding.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 223, 0, buffer);
    CHECK(n == 0);

    // row overflow — same rule applied to the second coordinate.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 223, 0, 0, buffer);
    CHECK(n == 0);

    // SGR mode encodes via decimal digits and is therefore unaffected.
    t.modes.mouseSGR = true;
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 999, 999, 0, buffer);
    CHECK(n > 0);
}

TEST(testMouseReportUTF81005Clamp)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    Ubiquitty::hardReset(t);
    // The tracking mode makes the encoder emit; 1005 shapes the bytes.
    t.modes.mouseButtonTracking = true;
    t.modes.mouseUTF8Coordinates = true;
    /// Column 5000 is far past the ceiling (v = col+33 must stay <= 0x7FF, so the
    /// max column is 0x7FF-33 = 2014). Left press: Cb = 0+32 = 0x20 (1 byte), so the
    /// Cx bytes start at index 4.
    int n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 5000, 0, buffer);
    CHECK(n > 4);
    const unsigned char cxLead = (unsigned char)buffer[4];
    CHECK(cxLead >= 0xC0 && cxLead <= 0xDF); // a valid 2-byte lead, not a 3-byte 0xE0+
    CHECK(((unsigned char)buffer[5] & 0xC0) == 0x80); // a valid continuation byte
}

TEST(testMouseReportGuards)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    Ubiquitty::hardReset(t);
    // A modeless terminal refuses everything, which would make each guard
    // below pass for the wrong reason; the mode isolates the guard under
    // test.
    t.modes.mouseButtonTracking = true;

    // nullptr out buffer.
    CHECK(Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 0, 0, std::span<char> { static_cast<char*>(nullptr), sizeof(buffer) }) == 0);
    // Zero-length out buffer.
    CHECK(Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 0, 0, {}) == 0);
    // Negative row / column.
    CHECK(Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, -1, 0, 0, buffer) == 0);
    CHECK(Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, -1, 0, buffer) == 0);
    // X10 mode needs a buffer of at least 6 bytes.
    CHECK(Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 0, 0, { buffer, 5 }) == 0);
    /// Negative button is coerced to LEFT, so the call still succeeds.
    int n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, -1, 0, 0, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[3] == 32 + Ubiquitty::Mouse::Button::Left);

    // SGR mode overflow: a 5-byte out buffer can't hold the
    // minimum SGR encoding (`\033[<0;1;1M`).
    t.modes.mouseSGR = true;
    CHECK(Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 0, 0, { buffer, 5 }) == 0);
}

TEST(testDecRelativePointerLockMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseRelativePointerLock == false);

    // Enable: the modes bit flips on.
    feed(t, "\033[?7403h");
    CHECK(t.modes.mouseRelativePointerLock == true);

    // DECRQM reports "set" (1).
    char reply[32];
    feed(t, "\033[?7403$p");
    drain(t, reply);
    CHECK(strcmp(reply, "\033[?7403;1$y") == 0);

    // SAFETY: a soft reset (DECSTR) must clear the lock (opposite of 7401/7402).
    feed(t, "\033[!p");
    CHECK(t.modes.mouseRelativePointerLock == false);
    feed(t, "\033[?7403$p");
    drain(t, reply);
    CHECK(strcmp(reply, "\033[?7403;2$y") == 0);

    // Disable path also clears.
    feed(t, "\033[?7403h");
    CHECK(t.modes.mouseRelativePointerLock == true);
    feed(t, "\033[?7403l");
    CHECK(t.modes.mouseRelativePointerLock == false);
}

TEST(testMouse1000Normal)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseButtonTracking == false);
    feed(t, "\033[?1000h");
    CHECK(t.modes.mouseButtonTracking == true);
    feed(t, "\033[?1000l");
    CHECK(t.modes.mouseButtonTracking == false);
}

TEST(testMouse9X10PressOnly)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    char outputBuffer[64];
    int n;

    Ubiquitty::hardReset(t);

    // DECSET 9 sets the true-X10 flag (mouseX10), not mouseButtonTracking.
    CHECK(t.modes.mouseX10 == false);
    feed(t, "\033[?9h");
    CHECK(t.modes.mouseX10 == true);
    CHECK(t.modes.mouseButtonTracking == false);

    // A press emits one legacy report with a bare button Cb (no modifiers).
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 1, 2, Ubiquitty::Mouse::Modifier::Shift | Ubiquitty::Mouse::Modifier::Control, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[3] == 32); // button 0 + 32, no mod bits
    CHECK((unsigned char)buffer[4] == 35); // column 2 + 33
    CHECK((unsigned char)buffer[5] == 34); // row 1 + 33

    // A release under X10 produces no report.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 0);

    // Motion under X10 produces no report.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 0);

    // DECRQM ?9 reports set (1), not unrecognized (0).
    feed(t, "\033[?9$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?9;1$y") == 0);

    feed(t, "\033[?9l");
    CHECK(t.modes.mouseX10 == false);
    feed(t, "\033[?9$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?9;2$y") == 0);
}

TEST(testMouse1000EmitsRelease)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    char outputBuffer[64];
    int n;

    Ubiquitty::hardReset(t);
    feed(t, "\033[?1000h");
    CHECK(t.modes.mouseButtonTracking == true);

    // Press → report.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[3] == 32);

    // Release → legacy button-3 report (32 + 3).
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[3] == 35); // release code 3 + 32

    // DECRQM ?1000 reports set via mouseButtonTracking.
    feed(t, "\033[?1000$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1000;1$y") == 0);
}

TEST(testMouse9YieldsToRicherMode)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    int n;

    Ubiquitty::hardReset(t);
    feed(t, "\033[?9h");
    feed(t, "\033[?1000h");
    CHECK(t.modes.mouseX10 == false);
    CHECK(t.modes.mouseButtonTracking == true);

    // Release is no longer suppressed because normal mode is active.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[3] == 35);
}

TEST(testMouseTrackingFamilyMutuallyExclusive)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];

    Ubiquitty::hardReset(t);
    feed(t, "\033[?1003h");
    CHECK(t.modes.mouseAnyEvent == true);
    feed(t, "\033[?1000h");
    CHECK(t.modes.mouseButtonTracking == true);
    CHECK(t.modes.mouseAnyEvent == false); // superseded, not left set alongside 1000

    feed(t, "\033[?1003$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1003;2$y") == 0); // reset — 1000 won

    feed(t, "\033[?1000$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1000;1$y") == 0); // set
}

TEST(testMouseEncodingFamilyMutuallyExclusive)
{
    Ubiquitty::Terminal t;
    char buffer[32];
    char outputBuffer[64];

    Ubiquitty::hardReset(t);
    feed(t, "\033[?1000h"); // need a tracking mode active to encode a report
    feed(t, "\033[?1006h");
    CHECK(t.modes.mouseSGR == true);
    feed(t, "\033[?1015h");
    CHECK(t.modes.mouseURXVT == true);
    CHECK(t.modes.mouseSGR == false); // superseded, not left set alongside 1015

    feed(t, "\033[?1006$p");
    drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[?1006;2$y") == 0); // reset — 1015 won

    /// Encoder now emits URXVT decimal coordinates ("\x1b[Cb;Cx;CyM"), not
    /// SGR ("\x1b[<Cb;Cx;Cy M/m") — the '<' marker is the SGR tell.
    int n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n > 0);
    CHECK(buffer[0] == '\033' && buffer[1] == '[' && buffer[2] != '<');
    CHECK(buffer[n - 1] == 'M'); // URXVT always terminates 'M', even on press
}

TEST(testMouse1006SGR)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseSGR == false);
    feed(t, "\033[?1006h");
    CHECK(t.modes.mouseSGR == true);
    feed(t, "\033[?1006l");
    CHECK(t.modes.mouseSGR == false);
}

TEST(testMouse1002CellMotion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseCellMotion == false);
    feed(t, "\033[?1002h");
    CHECK(t.modes.mouseCellMotion == true);
    feed(t, "\033[?1002l");
    CHECK(t.modes.mouseCellMotion == false);
}

TEST(testMouse1003AnyEvent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseAnyEvent == false);
    feed(t, "\033[?1003h");
    CHECK(t.modes.mouseAnyEvent == true);
    feed(t, "\033[?1003l");
    CHECK(t.modes.mouseAnyEvent == false);
}

// The tracking modes are one setting: resetting any of them turns tracking
// off, even one that was not the mode set. An encoding is reset only by its
// own number.
TEST(testResettingAnyTrackingModeTurnsTrackingOff)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?1002h\033[?1000l");
    CHECK(!t.modes.mouseCellMotion);
    feed(t, "\033[?1001h\033[?9l");
    CHECK(!t.modes.mouseHilite);
    feed(t, "\033[?1006h\033[?1015l");
    CHECK(t.modes.mouseSGR);
}

TEST(testMouseModesAreIndependent)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Enabling normal tracking must not touch SGR encoding — tracking
    // mode and coordinate encoding are independent private modes.
    feed(t, "\033[?1000h");
    feed(t, "\033[?1006h");
    CHECK(t.modes.mouseButtonTracking == true);
    CHECK(t.modes.mouseSGR == true);
    // Clearing one leaves the other alone.
    feed(t, "\033[?1000l");
    CHECK(t.modes.mouseButtonTracking == false);
    CHECK(t.modes.mouseSGR == true);
    feed(t, "\033[?1006l");
    CHECK(t.modes.mouseSGR == false);
}

TEST(testMouse1005UTF8Mode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseUTF8Coordinates == false);
    testStoredModeRoundtrip(t, 1005, &t.modes.mouseUTF8Coordinates, "\033[?1005h", "\033[?1005l", "\033[?1005;1$y", "\033[?1005;2$y");
}

TEST(testMouse1015URXVTMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseURXVT == false);
    testStoredModeRoundtrip(t, 1015, &t.modes.mouseURXVT, "\033[?1015h", "\033[?1015l", "\033[?1015;1$y", "\033[?1015;2$y");
}

TEST(testMouse1016SGRPixelsMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.mouseSGRPixels == false);
    testStoredModeRoundtrip(t, 1016, &t.modes.mouseSGRPixels, "\033[?1016h", "\033[?1016l", "\033[?1016;1$y", "\033[?1016;2$y");
}

/// UTF-8 (1005): small coords are byte-identical to the legacy CSI M
/// form; large coords (>0x7F) widen to a 2-byte UTF-8 sequence.
TEST(testMouseEncoder1005UTF8)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    // The tracking mode makes the encoder emit; 1005 shapes the bytes.
    t.modes.mouseButtonTracking = true;
    t.modes.mouseUTF8Coordinates = true;
    // Press at row 1, column 2, no modifiers → same 6 bytes as legacy.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 1, 2, 0, buffer);
    CHECK(n == 6);
    CHECK((unsigned char)buffer[0] == 0x1B);
    CHECK(buffer[1] == '[');
    CHECK(buffer[2] == 'M');
    CHECK((unsigned char)buffer[3] == 32); // Cb = button + 32
    CHECK((unsigned char)buffer[4] == 35); // Cx = column + 33 = 2 + 33
    CHECK((unsigned char)buffer[5] == 34); // Cy = row + 33 = 1 + 33
    // A column past 94 (column 200 → Cx = 233) widens to two UTF-8 bytes.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 0, 200, 0, buffer);
    // 3 prefix + Cb(1) + Cx(2) + Cy(1) = 7
    CHECK(n == 7);
    CHECK((unsigned char)buffer[3] == 32); // Cb single byte
    CHECK((unsigned char)buffer[4] == 0xC3); // 233 = 0xE9 → C3 A9
    CHECK((unsigned char)buffer[5] == 0xA9);
    CHECK((unsigned char)buffer[6] == 33); // Cy = row 0 + 33
}

/// URXVT (1015): decimal CSI Cb ; Cx ; Cy M with Cb carrying the +32
/// bias and always an 'M' final (no release distinction).
TEST(testMouseEncoder1015URXVT)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    t.modes.mouseAnyEvent = true; // tracking mode: the encodings below are orthogonal to it, but motion needs one
    t.modes.mouseURXVT = true;
    // Press left at row 4, column 9 → Cb = 0 + 32 = 32; Cx=10; Cy=5.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[32;10;5M", n) == 0);
    // Release reports code 3 + 32 = 35, still 'M'.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[35;10;5M", n) == 0);
    // Motion adds 32: 0 + 32 (motion) + 32 (bias) = 64.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::Left, 0, 0, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[64;1;1M", n) == 0);
}

/// SGR-pixels (1016): CSI < Cb ; Px ; Py M/m where Px/Py are device
/// pixels = cell coord × cell pixel size + 1.
TEST(testMouseEncoder1016SGRPixels)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    t.modes.mouseAnyEvent = true; // tracking mode: the encodings below are orthogonal to it, but motion needs one
    t.modes.mouseSGRPixels = true;
    // Press left at row 4, column 9 → Px = 9*8+1 = 73; Py = 4*16+1 = 65.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<0;73;65M", n) == 0);
    // Release uses lowercase 'm'.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<0;73;65m", n) == 0);
    // Motion adds 32 to Cb.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, Ubiquitty::Mouse::Button::Left, 0, 0, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<32;1;1M", n) == 0);
}

/// SGR-pixels takes precedence over plain SGR when both are enabled.
TEST(testMouseEncoder1016Beats1006)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    t.modes.mouseSGR = true;
    t.modes.mouseAnyEvent = true; // tracking mode: the encodings below are orthogonal to it, but motion needs one
    t.modes.mouseSGRPixels = true;
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer);
    CHECK(n > 0);
    // Pixel coords (73;65), not cell coords (10;5).
    CHECK(memcmp(buffer, "\033[<0;73;65M", n) == 0);
}

/// Any-event (1003) button-less hover motion: the frontend passes the
/// "no button" code 3; the encoder adds the +32 motion bias, yielding
/// Cb = 35 in the SGR (1006) motion form CSI < 35 ; column ; row M. This is
/// what drives Emacs mouse-face / link highlight under DECSET 1003.
TEST(testMouseEncoder1003NoButtonMotion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    t.modes.mouseAnyEvent = true;
    t.modes.mouseSGR = true; // SGR 1006 envelope, the common pairing
    // Button 3 (no button) motion at row 4, column 9 → Cb = 3 + 32 = 35.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, 3, 4, 9, 0, buffer);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<35;10;5M", n) == 0);
}

/// SGR-pixels (1016) with a real device-pixel position: when the frontend
/// supplies pixelX/pixelY, those are reported verbatim (true sub-cell
/// resolution) rather than the cell-quantized column*cellWidth+1 / row*cellHeight+1
/// fallback. Two positions inside the same cell must encode differently.
TEST(testMouseEncoder1016RealPixels)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    t.modes.mouseAnyEvent = true; // tracking mode: the encodings below are orthogonal to it, but motion needs one
    t.modes.mouseSGRPixels = true;
    // Cell-quantized fallback for (row 4, column 9) would be (73;65). With a
    // real pixel (77;70) inside that same cell, report the real pixel.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, 3, 4, 9, 0, buffer, 77, 70);
    CHECK(n > 0);
    // Cb = 3 + 32 = 35; pixels 77;70 (not the cell-quantized 73;65).
    CHECK(memcmp(buffer, "\033[<35;77;70M", n) == 0);
    // A different sub-cell pixel in the same cell encodes differently.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, 3, 4, 9, 0, buffer, 78, 70);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<35;78;70M", n) == 0);
    // No pixels supplied (-1, -1) → the cell-quantized fallback survives.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer, -1, -1);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<0;73;65M", n) == 0);
}

/// Relative pointer-lock (DECSET 7403) rides the ?1016 frame, but the px;py are a
/// CENTER-BIASED signed motion delta (fixed C = 0x8000), not an absolute position:
/// the client recovers the per-report delta as (px - 0x8000, py - 0x8000). Two
/// things must hold — a NEGATIVE delta must not trip the absolute cell-quantized
/// fallback (the pixelX>=0 branch), and a button event must carry a ZERO delta so
/// it encodes unambiguously as C;C. Clearing 7403 restores absolute ?1016.
TEST(testMouseEncoder2502RelativePointerLock)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[32];
    int n;
    t.cellPixelWidth = 8;
    t.cellPixelHeight = 16;
    t.modes.mouseAnyEvent = true; // tracking mode: the encodings below are orthogonal to it, but motion needs one
    t.modes.mouseSGRPixels = true;
    t.modes.mouseRelativePointerLock = true;

    // Motion delta (-5, +3) → center-biased (0x8000-5; 0x8000+3) = (32763; 32771),
    // not the absolute column*cellWidth+1 the negative pixelX would fall back to.
    // Cb = 3 (motion button) + 32 = 35.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, 3, 4, 9, 0, buffer, -5, 3);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<35;32763;32771M", n) == 0);

    // A button PRESS carries a zero delta: px = py = 0x8000 = 32768.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Press, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer, 0, 0);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<0;32768;32768M", n) == 0);

    // Release: same zero delta, 'm' terminator.
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Release, Ubiquitty::Mouse::Button::Left, 4, 9, 0, buffer, 0, 0);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<0;32768;32768m", n) == 0);

    // Clearing 7403 restores absolute ?1016: the same positive pixel reports
    // verbatim, never biased.
    t.modes.mouseRelativePointerLock = false;
    n = Ubiquitty::encodeMouseReport(t, Ubiquitty::Mouse::EventType::Motion, 3, 4, 9, 0, buffer, 77, 70);
    CHECK(n > 0);
    CHECK(memcmp(buffer, "\033[<35;77;70M", n) == 0);
}

UBIQUITTY_TEST_MAIN()