// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/ParseState.h"
#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::feed;

#include <cstdio>
#include <cstring>

// Every reset here abandons an in-flight accumulation without emitting it. The
// property that matters is that an interrupted sequence never later flushes as
// though it had terminated, and that each reset touches only its own lane.

TEST(testGroundReflectsPartialSequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    CHECK(Ubiquitty::VT::isParserAtGround(&t));

    feed(t, "\033["); // CSI opened, parameters pending
    CHECK(!Ubiquitty::VT::isParserAtGround(&t));

    feed(t, "H"); // CUP completes it
    CHECK(Ubiquitty::VT::isParserAtGround(&t));
}

// A CSI cut short must not resume when unrelated bytes arrive after the reset:
// the pending parameters are gone, so the following text prints as text.
TEST(test_resetParseState_abandons_partial_csi)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 20);

    feed(t, "\033[3"); // a row count that never gets its final byte
    CHECK(!Ubiquitty::VT::isParserAtGround(&t));

    Ubiquitty::VT::resetParseState(&t);
    CHECK(Ubiquitty::VT::isParserAtGround(&t));

    feed(t, "H");
    const Ubiquitty::Cell* c = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
    CHECK(c != nullptr && c->codePoint == 'H'); // printed, not consumed as CUP
}

// An OSC interrupted mid-payload must not later deliver a truncated string.
TEST(test_resetOSCBuffer_drops_partial_title)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]0;before\007");
    CHECK(t.titleUTF8 == "before");

    feed(t, "\033]0;trunc"); // no ST
    Ubiquitty::VT::resetOSCBuffer(&t);
    feed(t, "\007"); // the terminator arrives after the buffer was dropped

    CHECK(t.titleUTF8 == "before"); // never became "trunc"
}

// The lanes are independent: resetting the OSC buffer must not disturb the
// parser's own state machine, and vice versa.
TEST(testResetsAreLaneLocal)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[1"); // CSI in flight
    Ubiquitty::VT::resetOSCBuffer(&t);
    CHECK(!Ubiquitty::VT::isParserAtGround(&t)); // CSI untouched

    Ubiquitty::VT::resetParseState(&t);
    CHECK(Ubiquitty::VT::isParserAtGround(&t));

    // The image lanes reset without reopening the parser.
    Ubiquitty::VT::resetKittyBuffers(&t);
    Ubiquitty::VT::resetSixelAccumulator(&t);
    CHECK(Ubiquitty::VT::isParserAtGround(&t));
}

UBIQUITTY_TEST_MAIN()
