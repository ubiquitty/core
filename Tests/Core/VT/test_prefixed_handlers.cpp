// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The cold prefixed-CSI handlers, and the parameters that switch them off.
//
// Every handler here answers a sequence a program sends once at startup and
// then trusts for the rest of the session: which title encoding to use, which
// private modes were saved before a full-screen app took over, how many lines
// the page has, where the tab stops are, how large a sixel may be. Each has a
// parameter value that means "not this one, do nothing" — and that half went
// untested, because a case is written by sending the parameter that works.
//
// A handler that acts on the do-nothing parameter is worse than one that
// refuses the working one: the refusal shows up immediately, while the extra
// action lands silently in state the program will read back much later.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "Core/VT/Parser.h"

#include <cstdio>
#include <cstring>
#include <string>

using UbiquittyTest::drain;
using UbiquittyTest::feed;

namespace {

// The visible row as text, '.' for anything blank or non-ASCII.
std::string rowText(const Ubiquitty::Terminal& t, int row)
{
    std::string text;
    for (int column = 0; column < t.columns; ++column) {
        const Ubiquitty::Cell* here = UbiquittyTest::cell(t, row, column);
        const bool printable = here && here->codePoint >= 0x20 && here->codePoint < 0x7F;
        text.push_back(printable ? static_cast<char>(here->codePoint) : '.');
    }
    return text;
}

// The set of columns carrying a tab stop, as a string of '.' and 'T'.
std::string tabPattern(const Ubiquitty::Terminal& t, int upTo)
{
    std::string pattern;
    for (int column = 0; column < upTo; ++column)
        pattern.push_back(t.tabStops[column] ? 'T' : '.');
    return pattern;
}

} // namespace

// The xterm title-mode bits (CSI > Ps t sets, CSI > Ps T clears) tell the
// terminal which encoding to use for the labels it reports back. A program
// sets them once; nothing reads them again until a title query, so a bit set
// for the wrong Ps surfaces only as a mojibake title much later.
TEST(testTitleModeBitsSetAndClearPerSelector)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.titleModes == 0);

    // With no parameter nothing changes: the omitted parameter names no mode.
    feed(t, "\033[>t");
    CHECK(t.titleModes == 0);
    feed(t, "\033[>0t\033[>T");
    CHECK(t.titleModes == 0x01);
    feed(t, "\033[>0T");
    CHECK(t.titleModes == 0);

    // Each selector owns one bit, and the bits are independent.
    for (int ps = 0; ps <= 3; ++ps) {
        char set[16];
        std::snprintf(set, sizeof set, "\033[>%dt", ps);
        feed(t, set);
        CHECK_MSG(t.titleModes == (1u << ps), "one selector sets one bit");
        char clear[16];
        std::snprintf(clear, sizeof clear, "\033[>%dT", ps);
        feed(t, clear);
        CHECK(t.titleModes == 0);
    }

    // Several selectors in one sequence, then cleared the same way.
    feed(t, "\033[>1;3t");
    CHECK(t.titleModes == 0x0A);
    feed(t, "\033[>1;3T");
    CHECK(t.titleModes == 0);

    // A selector outside 0..3 is skipped rather than shifted into a bit that
    // belongs to another mode — 1 << 9 would land outside the byte entirely.
    feed(t, "\033[>9t");
    CHECK(t.titleModes == 0);
    feed(t, "\033[>1;9;2t");
    CHECK_MSG(t.titleModes == 0x06, "the valid selectors apply and the invalid one is skipped");
    feed(t, "\033[>1;9;2T");
    CHECK(t.titleModes == 0);
}

// XTSAVE / XTRESTORE (CSI ? Pm s / r) are how a full-screen program hands the
// modes back as it found them. The round trip is the contract: whatever the
// mode was at save time is what restore puts back, whichever way it was
// flipped in between.
TEST(testPrivateModesSaveAndRestoreRoundTrip)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Application cursor keys: set, save, clear, restore.
    feed(t, "\033[?1h");
    CHECK(t.modes.appCursorKeys);
    feed(t, "\033[?1s");
    feed(t, "\033[?1l");
    CHECK(!t.modes.appCursorKeys);
    feed(t, "\033[?1r");
    CHECK_MSG(t.modes.appCursorKeys, "restore puts back the saved state");

    // And the other direction, so the test cannot pass on a restore that
    // always sets.
    feed(t, "\033[?1l");
    feed(t, "\033[?1s");
    feed(t, "\033[?1h");
    feed(t, "\033[?1r");
    CHECK_MSG(!t.modes.appCursorKeys, "restore puts back a saved RESET too");

    // Several modes in one sequence, saved and restored together.
    feed(t, "\033[?7h\033[?25l\033[?2004h");
    feed(t, "\033[?7;25;2004s");
    feed(t, "\033[?7l\033[?25h\033[?2004l");
    feed(t, "\033[?7;25;2004r");
    CHECK(t.modes.autowrap);
    CHECK(!t.modes.isCursorVisible);
    CHECK(t.modes.bracketedPaste);

    // A mode never saved restores to reset, as xterm, the protocol's only
    // definition, restores it; DECANM stays, its reset being VT52.
    Ubiquitty::Terminal fresh;
    Ubiquitty::hardReset(fresh);
    feed(fresh, "\033[?7h");
    feed(fresh, "\033[?7;25r");
    CHECK(!fresh.modes.autowrap);
    CHECK(!fresh.modes.isCursorVisible);
    feed(fresh, "\033[?2r");
    CHECK(fresh.parser.parseState != Ubiquitty::VT::Parser::State::VT52Ground);

    // What was saved outlives RIS: the reset restores the terminal, not the
    // program's record of an earlier state.
    feed(fresh, "\033[?1h\033[?1s\033c");
    CHECK(!fresh.modes.appCursorKeys);
    feed(fresh, "\033[?1r");
    CHECK(fresh.modes.appCursorKeys);
}

// DECSNLS (CSI Ps * |) sets the page height. It is a VT400 operation, so it is
// inert at a lower operating level — and a program that lowered the level and
// then resized would otherwise get a page height the level says it cannot ask
// for.
TEST(testDECSNLSResizesOnlyAtVT400AndAbove)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);

    // At the default level the page resizes.
    feed(t, "\033[27*|");
    CHECK(t.rows == 27);
    CHECK(t.columns == 80); // width untouched

    // Zero and negative line counts are refused rather than collapsing the
    // page: a page of no lines has no cursor position.
    feed(t, "\033[0*|");
    CHECK(t.rows == 27);
    feed(t, "\033[*|"); // no parameter at all
    CHECK(t.rows == 27);

    // Past the grid ceiling it clamps rather than allocating what was asked.
    feed(t, "\033[9999*|");
    CHECK(t.rows == Ubiquitty::maxRows);

    // Asking for the height it already has changes nothing.
    Ubiquitty::resize(t, 24, 80);
    feed(t, "\033[24*|");
    CHECK(t.rows == 24);

    // Below VT400 it is inert.
    feed(t, "\033[63\"p"); // VT300
    feed(t, "\033[40*|");
    CHECK_MSG(t.rows == 24, "DECSNLS does nothing below VT400");
    feed(t, "\033[64\"p"); // back to VT400
    feed(t, "\033[40*|");
    CHECK(t.rows == 40);
}

// DECST8C (CSI ? Ps W) resets the tab grid to every eighth column, and only
// for Ps == 5. Any other parameter must leave the stops a program placed
// exactly where they are.
TEST(testDECST8CResetsTabsOnlyForPsFive)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 40);

    // Clear every stop, then place one nowhere near the eight-column grid.
    feed(t, "\033[3g"); // TBC 3: clear all
    feed(t, "\033[1;6H\033H"); // a stop at column 6
    const std::string custom = tabPattern(t, 24);
    CHECK(custom.find('T') != std::string::npos);

    // Every parameter but 5 leaves the custom layout alone.
    for (const char* query : { "\033[?W", "\033[?0W", "\033[?3W", "\033[?4W", "\033[?6W", "\033[?99W" }) {
        feed(t, query);
        CHECK_MSG(tabPattern(t, 24) == custom, "DECST8C ignores every parameter but 5");
    }

    // Ps == 5 rebuilds the eight-column grid.
    feed(t, "\033[?5W");
    const std::string rebuilt = tabPattern(t, 24);
    CHECK(rebuilt != custom);
    CHECK(rebuilt[0] == '.'); // column 0 carries no stop
    CHECK(rebuilt[8] == 'T');
    CHECK(rebuilt[16] == 'T');
    CHECK(rebuilt[9] == '.');
}

// XTSMGRAPHICS (CSI ? Pi ; Pa ; Pv S) answers what the terminal will accept
// for sixel colour registers and geometry. An action byte outside the defined
// set gets the failure reply — a program reading a success reply it did not
// earn would size an image against a cap this terminal never agreed to.
TEST(testXTSMGRAPHICSAnswersEveryResourceAndRefusesBadActions)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char outputBuffer[128];

    // Geometry (Pi=2): the reads report the fixed cap; a reset or a set fails,
    // since the size is not the program's to change.
    for (const char* action : { "\033[?2;1;0S", "\033[?2;4;0S" }) {
        (void)drain(t, outputBuffer);
        feed(t, action);
        CHECK(drain(t, outputBuffer) > 0);
        CHECK_MSG(std::strncmp(outputBuffer, "\033[?2;0;", 7) == 0, "a read reports success");
    }
    for (const char* action : { "\033[?2;2;0S", "\033[?2;3;0S" }) {
        (void)drain(t, outputBuffer);
        feed(t, action);
        CHECK(drain(t, outputBuffer) > 0);
        CHECK_MSG(std::strcmp(outputBuffer, "\033[?2;3S") == 0, "a change fails");
    }

    // Anything but three parameters is not answered.
    for (const char* request : { "\033[?S", "\033[?1;1S", "\033[?1;1;0;0S", "\033[?2;3;100;100S" }) {
        (void)drain(t, outputBuffer);
        feed(t, request);
        CHECK(drain(t, outputBuffer) == 0);
    }

    // An undefined action on the same resource: the failure reply, not a cap.
    for (const char* action : { "\033[?2;0;0S", "\033[?2;9;0S" }) {
        (void)drain(t, outputBuffer);
        feed(t, action);
        CHECK(drain(t, outputBuffer) > 0);
        CHECK_MSG(std::strcmp(outputBuffer, "\033[?2;2S") == 0, "an undefined action is refused");
    }

    // Colour registers (Pi=1) answer on the same shape.
    (void)drain(t, outputBuffer);
    feed(t, "\033[?1;1;0S");
    CHECK(drain(t, outputBuffer) > 0);
    CHECK(std::strncmp(outputBuffer, "\033[?1;0;", 7) == 0);

    // ReGIS (Pi=3) is unsupported and says so rather than staying silent: a
    // program waiting for an answer would otherwise hang on its own read.
    (void)drain(t, outputBuffer);
    feed(t, "\033[?3;1;0S");
    CHECK(drain(t, outputBuffer) > 0);
}

// DECRSTS (DCS 1 $ p body ST) restores a terminal state a host saved earlier
// with DECTSR. The body is a wire string a program chose, so every way it can
// be wrong has to leave the terminal exactly as it was: a half-applied restore
// lands the session in a state that was never saved and that no host can name.
//
// The round trip anchors the sweep — save, perturb, restore, and the perturbed
// state must be gone — and then each malformed body is checked against a
// terminal that has been perturbed the same way, so "did nothing" is
// distinguishable from "did the right thing by accident".
TEST(testDECRSTSRestoresOnlyAWholeWellFormedBody)
{
    const auto perturb = [](Ubiquitty::Terminal& t) {
        feed(t, "\033[65;2\"p"); // VT500, 8-bit C1
        feed(t, "\033[?69h\033[3;9s"); // a left/right region
        feed(t, "\033[2;7r"); // a top/bottom region
    };
    // What a perturbed terminal looks like, so an untouched one is recognisable.
    const auto perturbed = [](const Ubiquitty::Terminal& t) {
        return t.screens.active->scrollTop == 1 && t.screens.active->scrollBottom == 6;
    };

    // (a) The round trip: the state is saved, perturbed, and put back.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 24, 80);
        feed(t, "\033[4;20r"); // the state worth restoring
        char outputBuffer[160];
        (void)drain(t, outputBuffer);
        feed(t, "\033[1$u"); // DECTSR — request terminal state
        const int written = drain(t, outputBuffer);
        CHECK(written > 0);
        // The reply is DCS 1 $ s <body> ST; feed it back as DCS 1 $ p <body> ST.
        const char* body = std::strstr(outputBuffer, "$s");
        CHECK(body != nullptr);
        if (!body)
            return;
        std::string payload(body + 2);
        const std::size_t terminator = payload.find('\033');
        if (terminator != std::string::npos)
            payload.resize(terminator);

        perturb(t);
        CHECK(perturbed(t));
        feed(t, ("\033P1$p" + payload + "\033\\").c_str());
        CHECK_MSG(t.screens.active->scrollTop == 3, "the saved region came back");
        CHECK(t.screens.active->scrollBottom == 19);

        // Every malformed variant of that same body, against a terminal
        // perturbed the same way: each must leave the perturbation in place.
        const std::string bodies[] = {
            payload.substr(0, payload.find(';')), // truncated to one field
            payload + ";9", // one field too many
            "0" + payload.substr(payload.find(';')), // wrong version
            payload + "x", // a byte that is not a digit or separator
            payload + ";", // a trailing separator with no field after it
            ";" + payload, // a leading separator with no field before it
            "99999999999" + payload.substr(payload.find(';')), // a field wider than any written
            "1;99;1;0;4;20;1;80", // a conformance level outside 61..65
            "1;65;1;0;0;20;1;80", // a top margin of zero
            "1;65;1;0;20;4;1;80", // top past bottom
            "1;65;1;0;4;9999;1;80", // a bottom past the page
            "1;65;1;0;4;20;9;3", // left past right
            "1;65;1;0;4;20;1;9999", // a right past the page
            "", // an empty body
        };
        for (const std::string& malformed : bodies) {
            Ubiquitty::Terminal victim;
            Ubiquitty::hardReset(victim);
            Ubiquitty::resize(victim, 24, 80);
            perturb(victim);
            feed(victim, ("\033P1$p" + malformed + "\033\\").c_str());
            CHECK_MSG(perturbed(victim), "a malformed DECRSTS body leaves the terminal untouched");
        }
    }

    // (b) The selector: DECRSTS is defined for Ps=1 only, and the DCS
    // dispatch keys on the intermediate and final alone — so a body arriving
    // under any other Ps reaches the handler and has to be refused there.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 24, 80);
        perturb(t);
        feed(t, "\033P2$p1;65;1;0;4;20;1;80\033\\");
        CHECK_MSG(perturbed(t), "a DECRSTS under the wrong selector is refused");
        feed(t, "\033P$p1;65;1;0;4;20;1;80\033\\"); // no selector at all
        CHECK(perturbed(t));
    }
}

// The VT400 rectangle family is inert below VT400. A program that lowered the
// operating level and then sent one would otherwise get an edit the level says
// it cannot ask for — and these edits rewrite cells, so the damage is on the
// screen rather than in a reply.
TEST(testTheRectangleFamilyIsInertBelowVT400)
{
    // Each op, with parameters that would visibly change the grid.
    const char* const operations[] = {
        "\033[1;1;2;4$z", // DECERA — erase rectangle
        "\033[2$r", // DECCARA with a selector
        "\033[1;1;2;4;7$t", // DECRARA — reverse attributes
        "\033[1;1;2;4;5;1;1$v", // DECCRA — copy rectangle
        "\033[2'}", // DECIC — insert column
        "\033[2'~", // DECDC — delete column
    };

    for (const char* operation : operations) {
        Ubiquitty::Terminal low;
        Ubiquitty::hardReset(low);
        Ubiquitty::resize(low, 6, 20);
        feed(low, "abcdefghij");
        feed(low, "\033[61\"p"); // VT100: below the family's level
        const std::string before = rowText(low, 0);
        feed(low, operation);
        CHECK_MSG(rowText(low, 0) == before, "a VT400 rectangle op is inert at VT100");
    }
}

// DECCARA and DECRARA take a list of attribute selectors. The list is the
// whole instruction — a selector the terminal does not know must be skipped
// rather than folded into the previous one, and an explicit 0 means "all
// attributes off" rather than "no selector given".
TEST(testRectangleAttributeSelectors)
{
    // DECRARA with an explicit 0 selector, which is the "reverse everything"
    // case the no-selector default reaches by a different route.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 10);
        feed(t, "\033[1mAB"); // bold
        feed(t, "\033[1;1;1;2;0$t"); // reverse all attributes over the pair
        const Ubiquitty::Cell* first = UbiquittyTest::cell(t, 0, 0);
        CHECK(first != nullptr);
        // Whatever the mapping, the cell no longer carries what it did — a
        // selector that reached the default arm and did nothing would leave it.
        if (first)
            CHECK(Ubiquitty::cellPen(*first).attributes.contains(Ubiquitty::VT::Attribute::Reverse)
                || !Ubiquitty::cellPen(*first).attributes.contains(Ubiquitty::VT::Attribute::Bold));
    }

    // DECRARA with a selector nobody defines: skipped, and the valid one
    // beside it still applies.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 10);
        feed(t, "AB");
        feed(t, "\033[1;1;1;2;99;7$t"); // 99 undefined, 7 = reverse
        const Ubiquitty::Cell* first = UbiquittyTest::cell(t, 0, 0);
        CHECK(first != nullptr);
        if (first)
            CHECK_MSG(Ubiquitty::cellPen(*first).attributes.contains(Ubiquitty::VT::Attribute::Reverse),
                "the defined selector applies beside the undefined one");
    }

    // DECCARA's later selectors — the "off" halves, which turn an attribute
    // off rather than on, plus one nobody defines.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 4, 10);
        feed(t, "\033[1;4;5;7mAB"); // bold, underline, blink, reverse
        feed(t, "\033[1;1;1;2;24;25;27;99$r"); // underline off, blink off, reverse off, undefined
        const Ubiquitty::Cell* first = UbiquittyTest::cell(t, 0, 0);
        CHECK(first != nullptr);
        if (first) {
            const Ubiquitty::VT::AttributeSet attributes = Ubiquitty::cellPen(*first).attributes;
            CHECK(!attributes.contains(Ubiquitty::VT::Attribute::Underline));
            CHECK(!attributes.contains(Ubiquitty::VT::Attribute::Blink));
            CHECK(!attributes.contains(Ubiquitty::VT::Attribute::Reverse));
            CHECK_MSG(attributes.contains(Ubiquitty::VT::Attribute::Bold), "an unlisted attribute is left alone");
        }
    }

    // A rectangle whose corners are out of order is refused whole rather than
    // normalised: normalising would edit a region the program never named.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 6, 20);
        feed(t, "\033[1mABCDEFGH");
        const std::string before = rowText(t, 0);
        feed(t, "\033[999;1;1;1;1$r"); // top past bottom
        CHECK(rowText(t, 0) == before);
        feed(t, "\033[1;9;1;2;1$r"); // left past right
        CHECK(rowText(t, 0) == before);
    }
}

TEST(testMultipleCSIIntermediatesCannotDispatchTheirSuffix)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 5);
    feed(t, "abcde\033[1;2H\033[2!'~X");
    CHECK(rowText(t, 0) == "aXcde");
    feed(t, "\033[1;3H\033[2!'}Y");
    CHECK(rowText(t, 0) == "aXYde");
    feed(t, "\033[4h\033[ !p");
    CHECK(t.modes.insertMode);
}

TEST(testTerminalParameterReportsAreNotRequests)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2x\033[3x\033[99x");
    char output[256];
    CHECK(drain(t, output) == 0);
    feed(t, "\033[0x\033[1x");
    drain(t, output);
    CHECK(std::string(output) == "\033[2;1;1;128;128;1;0x\033[3;1;1;128;128;1;0x");
}

TEST(testSCOSCWithParametersDoesNotReplaceSavedCursor)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 6);
    feed(t, "\033[2;3H\033[s\033[1;1H\033[0s\033[1s\033[;s\033[uX");
    CHECK(rowText(t, 0) == "......");
    CHECK(rowText(t, 1) == "..X...");
}

TEST(testRectangleUnderlineUpdatesTheRenderedShape)
{
    for (const int extent : { 1, 2 }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 3, 5);
        feed(t, extent == 1 ? "\033[1*x" : "\033[2*x");
        feed(t, "AB\033[2;1HCD\033[1;1;2;2;4$r");
        for (const int row : { 0, 1 }) {
            const auto* here = UbiquittyTest::cell(t, row, 0);
            CHECK(here != nullptr);
            if (here)
                CHECK(Ubiquitty::penUnderlineShape(&Ubiquitty::cellPen(*here)) == Ubiquitty::VT::underlineSingle);
        }
        feed(t, "\033[1;1;2;2;4$t");
        const auto* here = UbiquittyTest::cell(t, 0, 0);
        CHECK(here != nullptr);
        if (here)
            CHECK(Ubiquitty::penUnderlineShape(&Ubiquitty::cellPen(*here)) == Ubiquitty::VT::underlineNone);
        feed(t, "\033[1;1;2;2;4$t\033[1;1;2;2;24$r");
        here = UbiquittyTest::cell(t, 1, 0);
        CHECK(here != nullptr);
        if (here)
            CHECK(Ubiquitty::penUnderlineShape(&Ubiquitty::cellPen(*here)) == Ubiquitty::VT::underlineNone);
    }
}

TEST(testUndefinedProtectionAndCursorStyleSelectorsPreserveState)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[1\"q\033[3\"qA\033[0\"q\033[9\"qB");
    const auto* first = UbiquittyTest::cell(t, 0, 0);
    const auto* second = UbiquittyTest::cell(t, 0, 1);
    CHECK(first && Ubiquitty::cellPen(*first).attributes.contains(Ubiquitty::VT::Attribute::Protected));
    CHECK(second && !Ubiquitty::cellPen(*second).attributes.contains(Ubiquitty::VT::Attribute::Protected));
    feed(t, "\033[4 q");
    const auto style = t.cursorStyle;
    const bool isBlinking = t.cursorBlinkAtt610;
    feed(t, "\033[8 q\033[99 q");
    CHECK(t.cursorStyle == style);
    CHECK(t.cursorBlinkAtt610 == isBlinking);
}

TEST(testDCSHeadersCannotDispatchAnUnknownPrefix)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char output[256];
    for (const char* header : { "!+q", "0+q", "?+q", "+0q", "+;q", ":+q", "!+Q", "0+Q", "!$q", "0$q" }) {
        feed(t, "\033P");
        feed(t, header);
        feed(t, "544E\033\\");
        CHECK(drain(t, output) == 0);
    }
    feed(t, "\033P!+pxterm\033\\\033P0+pxterm\033\\");
    CHECK(!t.bellPending);
    feed(t, "\033P+q544E\033\\");
    CHECK(drain(t, output) > 0);
}

TEST(testDECSCPPPreservesPageAndMarginsWithoutReflow)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 132);
    feed(t, "\033[2;4r\033[1;1HA\033[1;80HB\033[1;132HC\033[80$|");
    CHECK(t.columns == 80);
    CHECK(t.screens.active->cursorX == 79);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->scrollTop == 1);
    CHECK(t.screens.active->scrollBottom == 3);
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 'A');
    CHECK(UbiquittyTest::cell(t, 0, 79)->codePoint == 'B');
    CHECK(rowText(t, 1) == std::string(80, '.'));
    feed(t, "\033[132$|");
    CHECK(t.columns == 132);
    const auto cropped = UbiquittyTest::cell(t, 0, 131)->codePoint;
    CHECK(cropped == 0 || cropped == ' ');
    feed(t, "\033[100$|");
    CHECK(t.columns == 132);
    feed(t, "\033[0$|");
    CHECK(t.columns == 80);
}

TEST(testDoubleWidthDiscardsTheHiddenHalf)
{
    for (const char* sequence : { "\033#3", "\033#4", "\033#6" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::resize(t, 2, 10);
        feed(t, "ABCDEFGHIJ");
        feed(t, sequence);
        CHECK(t.screens.active->cursorX == 4);
        feed(t, "\033#5");
        CHECK(rowText(t, 0) == "ABCDE.....");
    }
}

TEST(testAnonymousKittyTransmissionIsSilentButQueryAnswers)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char output[256];
    feed(t, "\033_Gf=24,s=1,v=1;AAAA\033\\");
    CHECK(drain(t, output) == 0);
    feed(t, "\033_Ga=q,f=24,s=1,v=1;AAAA\033\\");
    CHECK(drain(t, output) > 0);
}

// ---- residue ledger: VT/Prefixed.cpp ---------------------------------
// What stays dark after the cases above and the DECCIR round trip in
// test_vt100_reports, and why each arm cannot execute:
//
// - DECRQCRA's null-cell skip inside the checksum walk: the walk's bounds come
//   from decRectFromParameters, which has already clamped the rectangle to the
//   grid, so cellAt cannot answer null for any coordinate the loop visits.
// - DECPS's negative clamps on volume and duration: both come from
//   parameterOr, and a CSI parameter is a run of digits — the parser never
//   produces a negative one, so only the upper clamps can run.

// DECRQSS reports the title modes in xterm's XTSMTITLE form: every mode with
// `> t`, one with `> Ps t`, and a mode it does not have as an invalid request.
TEST(testDECRQSSReportsTheTitleModes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[128] = {};
    feed(t, "\033P$q>t\033\\");
    CHECK(UbiquittyTest::drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033P1$r>0;0;0;0t\033\\") == 0);
    feed(t, "\033[>0;2t\033P$q>t\033\\");
    CHECK(UbiquittyTest::drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033P1$r>1;0;1;0t\033\\") == 0);
    feed(t, "\033P$q>1t\033\\");
    CHECK(UbiquittyTest::drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033P1$r>0t\033\\") == 0);
    feed(t, "\033P$q>9t\033\\");
    CHECK(UbiquittyTest::drain(t, buffer) > 0);
    CHECK(std::strcmp(buffer, "\033P0$r\033\\") == 0);
}

TEST(testPaletteStackRestoresColorsAndHonorsIndexedSlots)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const auto original = t.palette;
    feed(t, "\033[#P\033]4;1;#123456\007\033]13;#fedcba\007");
    const auto first = t.palette;
    feed(t, "\033[0#P\033]4;1;#abcdef\007\033]13;#010203\007");
    feed(t, "\033[#Q");
    CHECK(t.palette == first);
    CHECK(t.mousePointerForegroundSet);
    CHECK(t.mousePointerForegroundRed == 0xfefe);
    CHECK(t.savedPalettePosition == 1);
    CHECK(t.savedPaletteCount == 2);
    feed(t, "\033[0#Q");
    CHECK(t.palette == original);
    CHECK(!t.mousePointerForegroundSet);
    const auto epoch = t.paletteEpoch;
    feed(t, "\033[#Q");
    CHECK(t.paletteEpoch == epoch);

    // Indexed operations leave the stack position alone and retain the slot.
    feed(t, "\033[10#P\033]4;1;#123456\007\033[10#Q");
    CHECK(t.palette == original);
    CHECK(t.savedPalettePosition == 0);
    feed(t, "\033[11#P\033[11#Q\033[#R");
    char reply[64] = {};
    CHECK(drain(t, reply) > 0);
    CHECK(std::string(reply) == "\033[?0;2#Q");
    t.allowColorOperations = false;
    feed(t, "\033[#P");
    CHECK(t.savedPalettePosition == 0);
    Ubiquitty::hardReset(t);
    CHECK(t.savedPaletteValid == 0);
}

TEST(testPaletteStackOverflowIsBoundedAndDoesNotLoseSavedEntries)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (int index = 0; index < 12; ++index)
        feed(t, "\033[#P");
    CHECK(t.savedPalettePosition == 10);
    CHECK(t.savedPaletteCount == 10);
    for (int index = 0; index < 12; ++index)
        feed(t, "\033[#Q");
    CHECK(t.savedPalettePosition == 0);
    CHECK(t.savedPaletteCount == 10);
    feed(t, "\033[>4;1f\033[?4g\033[>4g");
    char reply[64] = {};
    CHECK(drain(t, reply) > 0);
    CHECK(std::string(reply) == "\033[>4;1f\033[>4;1f");
}

TEST(testConformanceAndSingleSelectorControlsRespectTheirHeaders)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[4h\033[\"p");
    CHECK(t.modes.insertMode);
    feed(t, "\033[3g\033[?5;1W");
    CHECK(!t.tabStops[8]);
    feed(t, "\033[?5W");
    CHECK(t.tabStops[8]);
    feed(t, "\033P2;1$t3/7\033\\");
    CHECK(t.tabStops[8]);
    CHECK(!t.tabStops[2]);
    feed(t, "\033[61;1\"p\033P$qm\033\\\033[\"v");
    char reply[64] = {};
    CHECK(drain(t, reply) == 0);
}

TEST(testMemoryLockChangesOnlyTheTopMargin)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feed(t, "\033[1;3r\033[2;3H\033l");
    CHECK(t.screens.active->scrollTop == 1);
    CHECK(t.screens.active->scrollBottom == 2);
    feed(t, "\033m");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == 2);
    CHECK(t.screens.active->cursorX == 2);
    CHECK(t.screens.active->cursorY == 1);
}

TEST(testMemoryLockCannotMakeAnEmptyScrollRegion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 5);
    feed(t, "A\r\nB\r\nC\033l\r\nD");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 'B');
    CHECK(UbiquittyTest::cell(t, 1, 0)->codePoint == 'C');
    CHECK(UbiquittyTest::cell(t, 2, 0)->codePoint == 'D');
    feed(t, "\033[1;2r\033[3;1H\033l");
    CHECK(t.screens.active->scrollTop == 0);
    CHECK(t.screens.active->scrollBottom == 1);
}

TEST(testDoubleHeightPendingWrapCannotRetainTheHiddenHalf)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 10);
    feed(t, "\033[2;1H\033#4X\033[1;10HJ\033#3\033[1;1HX\033#5\033[2;1H\033#5\033[1;2H");
    CHECK(UbiquittyTest::cell(t, 0, 0)->codePoint == 'X');
    CHECK(UbiquittyTest::cell(t, 1, 0)->codePoint == 'X');
    CHECK(UbiquittyTest::cell(t, 0, 9)->codePoint == 0 || UbiquittyTest::cell(t, 0, 9)->codePoint == ' ');
    CHECK(t.screens.active->cursorX == 1);
    CHECK(t.screens.active->cursorY == 0);
}

TEST(testResourceQueryReportsActualTitlePolicyAndAnswerback)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.allowTitleOperations = false;
    Ubiquitty::setAnswerbackString(t, "Hi");
    feed(t, "\033P+Q616C6C6F775469746C654F7073;616E737765726261636B537472696E67\033\\");
    char reply[256] = {};
    CHECK(drain(t, reply) > 0);
    CHECK(std::string(reply) == "\033P1+R616C6C6F775469746C654F7073=66616C7365;616E737765726261636B537472696E67=4869\033\\");
}

TEST(testDataPositionMovementHonorsOriginModeRatherThanScrollMargins)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 4, 8);
    feed(t, "\033[?69h\033[2;5s\033[2;3r\033[2;2H\033[99a\033[99e");
    CHECK(t.screens.active->cursorX == 7);
    CHECK(t.screens.active->cursorY == 3);
    feed(t, "\033[?6h\033[99a\033[99e");
    CHECK(t.screens.active->cursorX == 4);
    CHECK(t.screens.active->cursorY == 2);
}

TEST(testLeftRightMarginModeRestoresSingleWidthOnBothScreens)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 8);
    feed(t, "\033#6\033[?47h\033#3\033[?69h");
    CHECK(t.screens.primary.lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    CHECK(t.screens.alternate.lineAttribute[0] == Ubiquitty::lineAttributeNormal);
    feed(t, "\033#6");
    CHECK(t.screens.alternate.lineAttribute[0] == Ubiquitty::lineAttributeNormal);
}

UBIQUITTY_TEST_MAIN()
