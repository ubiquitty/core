// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_mouse_encode_guards.cpp — the paths on which Mouse::encodeReport
// declines to encode.
//
// The encodings themselves are covered elsewhere; these cases cover each
// guard that refuses. A fuzzer or a misbehaving frontend reaches those guards
// first, and a caller is likeliest to mishandle them, so every refusal has to
// write nothing at all rather than a short or partial report.

#include "Harness.h"

#include "Core/Input/PointerInput.h"
#include "Core/Mouse/Mouse.h"
#include "Core/Mouse/MouseEvent.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

using Ubiquitty::Mouse::EventType;
namespace Button = Ubiquitty::Mouse::Button;

constexpr std::size_t reportCapacity = 64;

/// An event as the encoder takes it, defaulting to a left press at the origin
/// with no modifiers and no pixel position.
struct PointerEvent {
    EventType type = EventType::Press;
    int button = Button::Left;
    int row = 0;
    int column = 0;
    int modifiers = 0;
    int pixelX = -1;
    int pixelY = -1;
};

/// What one call to the encoder left in a buffer that started zeroed.
struct EncodedReport {
    int written = 0;
    std::array<char, reportCapacity> bytes {};

    /// The bytes up to the first NUL, which a refusal leaves at the front.
    std::string_view text() const { return bytes.data(); }
};

/// Sets DEC private mode `mode` on `terminal`.
void enablePrivateMode(Ubiquitty::Terminal& terminal, int mode)
{
    Ubiquitty::processBytes(&terminal, "\033[?" + std::to_string(mode) + "h");
}

/// Returns an 80-column, 25-row terminal with each of `privateModes` set, in order.
std::unique_ptr<Ubiquitty::Terminal> makeTerminal(std::initializer_list<int> privateModes = {})
{
    auto terminal = std::make_unique<Ubiquitty::Terminal>();
    Ubiquitty::resize(*terminal, 25, 80);
    for (const int mode : privateModes)
        enablePrivateMode(*terminal, mode);
    return terminal;
}

/// Encodes `event` for `terminal`, offering the encoder only the first
/// `capacity` bytes of a zeroed buffer.
EncodedReport encode(const Ubiquitty::Terminal& terminal, const PointerEvent& event, std::size_t capacity = reportCapacity)
{
    EncodedReport report;
    report.written = Ubiquitty::Mouse::encodeReport(&terminal, event.type, event.button, event.row, event.column,
        event.modifiers, std::span(report.bytes).first(capacity), event.pixelX, event.pixelY);
    return report;
}

/// Checks that the encoder declined outright: no byte written, not even a prefix.
void checkRefused(const EncodedReport& report, const char* context)
{
    CHECK_MSG(report.written <= 0, context);
    CHECK_MSG(report.bytes.front() == '\0', context);
}

TEST(testNegativeCoordinatesRefused)
{
    const auto terminal = makeTerminal({ 1000 }); // VT200 button reporting

    struct OffGridPlacement {
        int row;
        int column;
        const char* label;
    };
    const OffGridPlacement placements[] = {
        { -1, 0, "negative row refused" },
        { 0, -1, "negative column refused" },
        { -5, -5, "both negative refused" },
    };
    for (const OffGridPlacement& placement : placements)
        checkRefused(encode(*terminal, { .row = placement.row, .column = placement.column }), placement.label);
}

// A report that does not fit is refused whole. An escape sequence cut short
// on its way to the application is worse than none, because the application
// resynchronises in the middle of the report.
TEST(testUndersizedBufferRefused)
{
    const auto terminal = makeTerminal({ 1000 });
    for (std::size_t capacity = 0; capacity <= 2; ++capacity)
        checkRefused(encode(*terminal, {}, capacity), "undersized buffer refused");
}

TEST(testUndersizedBufferRefusedForSGR)
{
    const auto terminal = makeTerminal({ 1006, 1000 }); // SGR extended reporting, then VT200
    for (std::size_t capacity = 0; capacity <= 3; ++capacity)
        checkRefused(encode(*terminal, {}, capacity), "undersized SGR buffer refused");
}

// Which motion is reportable is a property of the active tracking mode, not of
// the frontend: 1000 tracks button transitions only, 1002 adds motion while a
// button is held, 1003 adds free motion. A frontend re-deciding this before
// calling holds a copy of the rule, and a new one starts with no gate at all
// and leaks motion into a 1000-only session. The encoder knows every mode flag, so it decides.
TEST(testMotionGatedByTrackingMode)
{
    // Pinned against the wire, not against itself: a client decodes this Cb as
    // "no button", so the literal is the protocol and not an implementation choice.
    CHECK(Ubiquitty::Mouse::Button::None == 3);

    const PointerEvent press { .type = EventType::Press, .button = Button::Left, .row = 3, .column = 4 };
    const PointerEvent drag { .type = EventType::Motion, .button = Button::Left, .row = 3, .column = 4 };
    const PointerEvent hover { .type = EventType::Motion, .button = Button::None, .row = 3, .column = 4 };

    {
        const auto terminal = makeTerminal({ 1000 }); // VT200: transitions only
        checkRefused(encode(*terminal, hover), "1000 refuses button-less motion");
        checkRefused(encode(*terminal, drag), "1000 refuses drag motion");
        CHECK_MSG(encode(*terminal, press).written > 0, "1000 still reports a press");
    }

    {
        const auto terminal = makeTerminal({ 1002 }); // cell motion: only while held
        CHECK_MSG(encode(*terminal, drag).written > 0, "1002 reports drag motion");
        checkRefused(encode(*terminal, hover), "1002 refuses button-less motion");
    }

    {
        const auto terminal = makeTerminal({ 1003 }); // any-event: everything moves
        CHECK_MSG(encode(*terminal, hover).written > 0, "1003 reports button-less motion");
        CHECK_MSG(encode(*terminal, drag).written > 0, "1003 reports drag motion");
    }
}

// Relative pointer-lock (private 7403) delivers motion deltas to a mouselook
// client, so the tracking-mode gate above must not reach it: a game that locks
// the pointer and reads ?1016 deltas never enables 1003, and gating on 1003
// would silence the only thing the mode exists to carry. macOS takes the lock
// path in mouseMoved: before any tracking check, which is the behavior to match.
TEST(testPointerLockMotionBypassesTheTrackingGate)
{
    // SGR-pixels, the frame 7403 rides, then relative pointer-lock.
    const auto terminal = makeTerminal({ 1016, 7403 });
    const PointerEvent delta { .type = EventType::Motion, .button = Button::None, .pixelX = 5, .pixelY = -7 };

    // No tracking mode is on at all — under the gate this would be refused.
    const EncodedReport locked = encode(*terminal, delta);
    CHECK_MSG(locked.written > 0, "locked motion reports without a tracking mode");
    // The delta rides center-biased so the SGR body stays positive: 0x8000 +- d.
    CHECK_CONTAINS(locked.text(), ";32773;32761");

    // Dropping the lock puts the same event back under the gate.
    Ubiquitty::processBytes(terminal.get(), "\033[?7403l");
    checkRefused(encode(*terminal, delta), "unlocked motion is gated again");
}

// X10 (DECSET 9) is press-only, and the gate must not change which rule wins:
// a richer mode alongside it takes over, and X10 alone still refuses motion.
TEST(testX10AndGateInteraction)
{
    const PointerEvent hover { .type = EventType::Motion, .button = Button::None, .row = 3, .column = 4 };

    {
        const auto terminal = makeTerminal({ 9 });
        checkRefused(encode(*terminal, hover), "X10 alone refuses motion");
        checkRefused(encode(*terminal, { .type = EventType::Release, .row = 3, .column = 4 }), "X10 alone refuses release");
    }

    {
        // X10 plus any-event: the richer mode wins, so motion reports.
        const auto terminal = makeTerminal({ 9, 1003 });
        CHECK_MSG(encode(*terminal, hover).written > 0, "1003 alongside X10 reports motion");
    }
}

// A terminal with no tracking mode set reports nothing, whatever the caller
// asks for. The guard lives in the encoder rather than in each frontend
// because a frontend that trusted the encoder's refusal, and had no guard of
// its own, sent the legacy bytes into a modeless application's input on every
// click. Only pointer lock is exempt, and testPointerLockMotionBypassesTheTrackingGate
// holds that half.
TEST(testModelessTerminalReportsNothing)
{
    // An encoding mode is not a tracking mode: SGR and its pixel form say how a
    // report is spelled, not that one is wanted. A terminal carrying only these
    // is still modeless, and each was a way past a guard that asked whether any
    // mouse mode at all was on.
    const auto terminal = makeTerminal({ 1006, 1016 });

    // Every event the encoder takes, and the wheel, which rides a Press with a
    // button in the 0x40 class rather than an event type of its own.
    const PointerEvent events[] = {
        { .type = EventType::Press, .button = Button::Left },
        { .type = EventType::Release, .button = Button::Left },
        { .type = EventType::Motion, .button = Button::None },
        // 64 is the wheel-up Cb: the wheel has no event type, it rides a press
        // with a button in the 0x40 class.
        { .type = EventType::Press, .button = 64 },
    };
    for (const PointerEvent& event : events)
        checkRefused(encode(*terminal, event), "a terminal with no tracking mode reports nothing");

    // The same terminal, one tracking mode later, does encode — so the refusals
    // above are the missing mode and not a fixture that could never report.
    enablePrivateMode(*terminal, 1000);
    CHECK_MSG(encode(*terminal, {}).written > 0, "the same press encodes once a tracking mode is on");
}

// The predicate is public and the frontends ask it directly, so the null they
// can hold is its own to answer rather than each caller's to check.
TEST(testANullTerminalTracksNothing)
{
    CHECK_MSG(!Ubiquitty::Mouse::tracksPointer(nullptr), "a null terminal tracks no pointer");
}

// Modifiers arrive as an open integer the frontend assembles, beside bits this
// encoder's table does not name. A negative one drives the Cb below zero,
// where the legacy form would write a byte the parser reads as another button
// entirely, so it is refused rather than encoded.
TEST(testANegativeModifierIsRefused)
{
    const auto terminal = makeTerminal({ 1000 });
    checkRefused(encode(*terminal, { .modifiers = -64 }), "a negative modifier has no Cb to write");
}

// 1005 writes a three-byte lead and then one or two bytes per coordinate, so a
// buffer can run out in two different places: before the lead, and before a
// coordinate past 0x7F that needs its continuation byte. Both are refusals,
// and both leave the buffer as they found it.
TEST(testUndersizedBufferRefusedForUTF8Coordinates)
{
    const auto terminal = makeTerminal({ 1005, 1000 }); // UTF-8 coordinates, then VT200
    for (std::size_t capacity = 0; capacity <= 2; ++capacity)
        checkRefused(encode(*terminal, {}, capacity), "undersized UTF-8 buffer refused before the lead");

    // Room for the lead and the single-byte Cb, none for the column past 0x7F.
    const PointerEvent farColumn { .column = 200 };
    checkRefused(encode(*terminal, farColumn, 4), "a two-byte coordinate refused for want of room");

    // The same report with room encodes, so the refusals above are the buffer.
    CHECK_MSG(encode(*terminal, farColumn).written > 0, "the same report encodes into a buffer that fits it");
}

// The positive control. Every refusal above would pass just as well against
// an encoder that refused everything, so a plain press with reporting on and a
// buffer that fits must produce a report.
TEST(testValidReportStillEncodes)
{
    const auto terminal = makeTerminal({ 1000 });
    const EncodedReport report = encode(*terminal, {});
    CHECK_MSG(report.written > 0, "a valid press encodes something");
    CHECK_MSG(report.bytes.front() == '\033', "the report starts with ESC");
}

} // namespace

UBIQUITTY_TEST_MAIN()
