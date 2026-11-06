// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// DECRQM answers what a mode is currently set to, and the DECRQSS settings
// selectors answer for the page geometry and the status line. Applications
// compare these replies literally, so the bytes are the contract; this pins them
// so a change to how they are assembled has to produce the same ones.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>

using UbiquittyTest::feed;

namespace {

std::string replyTo(Ubiquitty::Terminal& t, const std::string& request)
{
    t.outputLength = 0;
    feed(t, request);
    return std::string(t.outputBuffer.data(), static_cast<std::size_t>(t.outputLength));
}

Ubiquitty::Terminal& freshTerminal(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.outputLength = 0;
    return t;
}

} // namespace

TEST(decrqmAnswersForAnANSIMode)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    // IRM (mode 4), reset at start, then set: the state digit is what moves.
    CHECK(replyTo(t, "\x1b[4$p") == "\x1b[4;2$y");
    feed(t, "\x1b[4h");
    CHECK(replyTo(t, "\x1b[4$p") == "\x1b[4;1$y");
    feed(t, "\x1b[4l");
    CHECK(replyTo(t, "\x1b[4$p") == "\x1b[4;2$y");
}

TEST(decrqmAnswersForADECMode)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    // DECTCEM (mode 25) — the cursor is shown at start.
    CHECK(replyTo(t, "\x1b[?25$p") == "\x1b[?25;1$y");
    feed(t, "\x1b[?25l");
    CHECK(replyTo(t, "\x1b[?25$p") == "\x1b[?25;2$y");
    feed(t, "\x1b[?25h");
    CHECK(replyTo(t, "\x1b[?25$p") == "\x1b[?25;1$y");
}

// DEC STD 070 cursor coupling: DECHCCM (60) is reset by default, DECVCCM (61)
// and DECPCCM (64) set, and each reports what the last DECSET or DECRST made
// it. The page is never larger than the window, so nothing moves.
TEST(decrqmReportsTheCursorCouplingModes)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    CHECK(replyTo(t, "\x1b[?60$p\x1b[?61$p\x1b[?64$p") == "\x1b[?60;2$y\x1b[?61;1$y\x1b[?64;1$y");
    feed(t, "\x1b[?60h\x1b[?61l\x1b[?64l");
    CHECK(replyTo(t, "\x1b[?60$p\x1b[?61$p\x1b[?64$p") == "\x1b[?60;1$y\x1b[?61;2$y\x1b[?64;2$y");
    feed(t, "\x1b[?60l\x1b[?61h\x1b[?64h");
    CHECK(replyTo(t, "\x1b[?60$p\x1b[?61$p\x1b[?64$p") == "\x1b[?60;2$y\x1b[?61;1$y\x1b[?64;1$y");

    // RIS puts the defaults back.
    feed(t, "\x1b[?60h\x1b[?61l\033c");
    CHECK(replyTo(t, "\x1b[?60$p\x1b[?61$p\x1b[?64$p") == "\x1b[?60;2$y\x1b[?61;1$y\x1b[?64;1$y");
}

TEST(decrqmCarriesTheModeNumberItWasAsked)
{
    // The reply echoes the mode, so a multi-digit one has to survive the
    // rendering intact.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    CHECK(replyTo(t, "\x1b[?2026$p").starts_with("\x1b[?2026;"));
    CHECK(replyTo(t, "\x1b[?1049$p").starts_with("\x1b[?1049;"));
    CHECK(replyTo(t, "\x1b[?1$p").starts_with("\x1b[?1;"));
}

TEST(decrqmReportsAnUnknownModeAsNotRecognised)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    // State 0 — the terminal does not recognise the mode.
    CHECK(replyTo(t, "\x1b[?64738$p") == "\x1b[?64738;0$y");
}

TEST(theSettingSelectorsReportThePageAndStatusLine)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    // DECSLPP and DECSNLS both report the live row count.
    CHECK(replyTo(t, "\x1bP$qt\x1b\\") == "\x1bP1$r24t\x1b\\");
    CHECK(replyTo(t, "\x1bP$q*|\x1b\\") == "\x1bP1$r24*|\x1b\\");

    // DECSACE — 0 is stream, 2 is rectangle, and Ps 1 normalises to 0 because
    // hosts compare the reply literally.
    CHECK(replyTo(t, "\x1bP$q*x\x1b\\") == "\x1bP1$r0*x\x1b\\");
    feed(t, "\x1b[2*x");
    CHECK(replyTo(t, "\x1bP$q*x\x1b\\") == "\x1bP1$r2*x\x1b\\");
    feed(t, "\x1b[1*x");
    CHECK(replyTo(t, "\x1bP$q*x\x1b\\") == "\x1bP1$r0*x\x1b\\");

    // DECSASD and DECSSDT.
    CHECK(replyTo(t, "\x1bP$q$}\x1b\\") == "\x1bP1$r0$}\x1b\\");
    CHECK(replyTo(t, "\x1bP$q$~\x1b\\") == "\x1bP1$r0$~\x1b\\");
}

TEST(theSettingSelectorsFollowTheGeometry)
{
    // The page reports are read from the live rows, so a resize moves them.
    Ubiquitty::Terminal t;
    freshTerminal(t);
    Ubiquitty::resize(t, 50, 100);

    CHECK(replyTo(t, "\x1bP$qt\x1b\\") == "\x1bP1$r50t\x1b\\");
    CHECK(replyTo(t, "\x1bP$q*|\x1b\\") == "\x1bP1$r50*|\x1b\\");
}

TEST(theColourSchemeReportNamesTheScheme)
{
    Ubiquitty::Terminal t;
    freshTerminal(t);

    // DSR 996 — 1 is dark, 2 is light.
    const std::string reply = replyTo(t, "\x1b[?996n");
    CHECK(reply == "\x1b[?997;1n" || reply == "\x1b[?997;2n");
}

UBIQUITTY_TEST_MAIN()
