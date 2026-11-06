// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

using UbiquittyTest::cell;

// test_conformance_parser — control-string parsing pins for the DCS tmux
// passthrough wrapper (Source/Core/VT/VT.cpp dcsDispatch + the DCSEscape
// transitions).
//
// tmux doubles every ESC in a passthrough payload, so `ESC ESC` is one literal
// payload ESC and only an undoubled `ESC \` closes the frame. Both pins here
// cover failures whose symptom is the same and is invisible on the grid: the
// parser is left inside a control-string state, silently eating every later byte.

#include "Core/CellGrid.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Parser.h"

#include <cstdio>
#include <cstring>
#include <string>

int runParserConformanceTests();

static void feed(Ubiquitty::Terminal& t, const char* s, int n) { Ubiquitty::processBytes(&t, std::string_view(s, static_cast<std::size_t>(n))); }
static void feed(Ubiquitty::Terminal& t, const char* s) { feed(t, s, static_cast<int>(strlen(s))); }

// The whole failure class in one assertion: a parser parked in OSC/DCS/APC keeps
// consuming program output forever, so anything printed afterwards vanishes.
static void checkStreamStillLive(Ubiquitty::Terminal& t, int row)
{
    char position[32];
    snprintf(position, sizeof(position), "\033[%d;1H", row + 1);
    feed(t, position);
    feed(t, "Z");
    CHECK(cell(t, row, 0)->codePoint == 'Z');
}

// A payload's own `ESC ESC \` is a literal ESC + '\', not the frame terminator.
// Reading it as the terminator ended the DCS early, left the recursive feed
// parked mid-OSC, and swallowed everything after it.
static void testTmuxPayloadStringTerminatorIsLiteral()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    // DCS tmux; <OSC 2 "hi" ST, every ESC doubled> ST
    feed(t, "\033Ptmux;\033\033]0;hi\033\033\\\033\\");
    CHECK(t.titleUTF8 == "hi");
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    checkStreamStillLive(t, 0);
}

// The undoubled `ESC \` still closes the frame.
static void testTmuxFrameTerminatorStillCloses()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033Ptmux;\033\033]0;one\033\033\\\033\\");
    CHECK(t.titleUTF8 == "one");
    feed(t, "\033Ptmux;\033\033]0;two\033\033\\\033\\");
    CHECK(t.titleUTF8 == "two");
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

// The payload was copied into a 256-byte local and the truncated prefix was
// dispatched anyway. Past ~180 bytes of clipboard text the standard
// tmux/vim-oscyank idiom exceeds that, so the base64 was cut mid-string.
static void testTmuxPayloadSurvivesPastTheOldLocalBuffer()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 500);
    const int count = 300; // past a 256-byte local, which would cap this
    std::string frame = "\033Ptmux;";
    frame.append(static_cast<size_t>(count), 'A');
    frame += "\033\\";
    feed(t, frame.data(), static_cast<int>(frame.size()));
    for (int column = 0; column < count; ++column)
        CHECK(cell(t, 0, column)->codePoint == 'A');
    CHECK(cell(t, 0, count)->codePoint != 'A');
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

// A long inner OSC is the real-world form: truncation cut the base64 and parked
// the parser inside the unterminated OSC.
static void testTmuxLongInnerOSCIsDeliveredWhole()
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    const std::string title(400, 'x');
    std::string frame = "\033Ptmux;\033\033]0;";
    frame += title;
    frame += "\033\033\\\033\\";
    feed(t, frame.data(), static_cast<int>(frame.size()));
    CHECK(t.titleUTF8.size() == title.size());
    CHECK(t.titleUTF8 == title);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    checkStreamStillLive(t, 0);
}

// An EMPTY CSI parameter selects the control's default, which is not always what
// an explicit 0 selects (XTWINOPS 8: empty keeps the axis, 0 fits the display).
// Both land in the slot as the integer 0, so only the emptiness record separates
// them — and the batched parameter scanner must agree with the per-byte DFA, or
// the same bytes parse differently depending on how the write was chunked.
static void checkEmptyParameter(const char* label, bool byteAtATime)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    const char* seq = "\033[8;;140t";
    if (byteAtATime) {
        for (const char* p = seq; *p; ++p)
            feed(t, p, 1);
    } else {
        feed(t, seq);
    }
    // Parameter 1 was written empty; parameter 2 carried digits.
    if (!Ubiquitty::VT::parameterOmitted(t, 1))
        printf("FAIL %s: empty parameter 1 not recorded as omitted\n", label), failures++;
    if (Ubiquitty::VT::parameterOmitted(t, 2))
        printf("FAIL %s: parameter 2 has digits but reads omitted\n", label), failures++;

    Ubiquitty::Terminal z;
    Ubiquitty::hardReset(z);
    Ubiquitty::resize(z, 24, 80);
    feed(z, "\033[8;0;140t");
    if (Ubiquitty::VT::parameterOmitted(z, 1))
        printf("FAIL %s: explicit 0 reads as omitted\n", label), failures++;
}

static void testEmptyParameterDiffersFromExplicitZero()
{
    checkEmptyParameter("batched", false);
    checkEmptyParameter("per-byte", true);
}

int runParserConformanceTests()
{
    testEmptyParameterDiffersFromExplicitZero();
    testTmuxPayloadStringTerminatorIsLiteral();
    testTmuxFrameTerminatorStillCloses();
    testTmuxPayloadSurvivesPastTheOldLocalBuffer();
    testTmuxLongInnerOSCIsDeliveredWhole();

    if (failures == 0)
        printf("test_conformance_parser: all passed\n");
    else
        printf("test_conformance_parser: %d FAILED\n", failures);
    return failures;
}

// Defined out when several cluster conformance files are linked into one
// binary; that driver supplies main and calls the run*Tests entry points.
#ifndef UBIQUITTY_CONFORMANCE_TESTS_COMBINED
int main() { return runParserConformanceTests() == 0 ? 0 : 1; }
#endif
