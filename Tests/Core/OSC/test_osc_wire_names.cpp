// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Core/OSC/test_osc_wire_names.cpp — the exact bytes this terminal answers
// to, written as bytes.
//
// An OSC key is a wire contract with every program that emits it. Changing
// "CursorShape=" to anything else compiles, links, ships, and silently stops the
// sequence working — no warning, no crash, and every surrounding test still
// green, because an unrecognised sequence produces no effect to assert on.
//
// A behavioural test spelling the sequence as a readable string literal does not
// guard this. A tree-wide edit that rewrites the source constant rewrites the
// test's literal in the same pass, both move together, and the test passes while
// the contract is gone. That happened during a namespace refactor here: a
// bare-identifier rewrite turned the constant into "Cursor::CursorShape=", and
// the existing test failed only because the two rewrites disagreed by accident.
//
// So the keys below are byte arrays. A rewrite that matches identifiers cannot
// see them, a human editing them has to mean it, and the assertion is that the
// terminal reacts — which is the only evidence the wire contract still holds.
//

#include "Assertions.h"
#include "TerminalFixture.h"

#include "Core/Screen/Addressing.h"
#include "Core/Screen/MarkValues.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

// OSC introducer and BEL terminator, spelled the same way — a test that wrote
// "\033]" would reintroduce exactly the fragility this file exists to avoid.
constexpr char oscStart[] = { 0x1b, ']' };
constexpr char bel = 0x07;

std::string sequence(std::string_view body)
{
    std::string out(oscStart, sizeof(oscStart));
    out += body;
    out += bel;
    return out;
}

// The iTerm2 OSC 1337 keys, byte by byte. Spelled out rather than written as
// "1337;CursorShape=" so no identifier-shaped edit can reach them.
const std::string osc1337 { '1', '3', '3', '7', ';' };
const std::string keyCursorShape { 'C', 'u', 'r', 's', 'o', 'r', 'S', 'h', 'a', 'p', 'e', '=' };
const std::string keyCurrentDirectory { 'C', 'u', 'r', 'r', 'e', 'n', 't', 'D', 'i', 'r', '=' };
const std::string keyRemoteHost { 'R', 'e', 'm', 'o', 't', 'e', 'H', 'o', 's', 't', '=' };
const std::string keySetMark { 'S', 'e', 't', 'M', 'a', 'r', 'k' };

} // namespace

int main()
{
    // Each case asserts the terminal CHANGED, so a key that stops being
    // recognised fails here rather than going quiet.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        const auto before = t.cursorStyle;
        UbiquittyTest::feed(t, sequence(osc1337 + keyCursorShape + "1"));
        CHECK(t.cursorStyle != before);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        UbiquittyTest::feed(t, sequence(osc1337 + keyCurrentDirectory + "/tmp/wire-name-probe"));
        CHECK(t.currentDirectory == "/tmp/wire-name-probe");
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        UbiquittyTest::feed(t, sequence(osc1337 + keyRemoteHost + "user@wire-name-probe"));
        CHECK(t.remoteHost == "user@wire-name-probe");
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::resize(t, 24, 80);
        Ubiquitty::hardReset(t);
        UbiquittyTest::feed(t, sequence(osc1337 + keySetMark));
        CHECK((Ubiquitty::Screen::rowMark(&t, t.screens.active->cursorY) & Ubiquitty::Screen::osc133MarkPromptStart) != 0);
    }
    // A key this terminal does not answer to must change nothing — otherwise the
    // cases above could pass on a parser that accepted anything at all.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        const auto before = t.cursorStyle;
        UbiquittyTest::feed(t, sequence(osc1337 + std::string { 'N', 'o', 't', 'A', 'K', 'e', 'y', '=' } + "1"));
        CHECK(t.cursorStyle == before);
        CHECK(t.currentDirectory.empty());
    }

    if (failures == 0)
        std::printf("osc_wire_names: the OSC 1337 keys this terminal answers to are unchanged, asserted as bytes\n");
    return failures == 0 ? 0 : 1;
}
