// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "VT100Fixture.h"

// test_vt100_parser.cpp
//
// Unit coverage for the table-driven dispatch APIs in
// Source/Core/VT/Parser.h.
//
// These tests poke the lookup surface directly: they assert that
// findCSIPlain / findCSIPrefixed / findEscape return a non-null handler
// for sequences we explicitly register and nullptr for sequences we
// don't. Integration coverage (the handlers' actual effects) lives
// across Tests/Core/VT/ — this file is about the dispatch infrastructure.
//
// Why this matters for the rest of features_to_implement.txt:
//   - Adding DECDWL/DECDHL (ESC # 3..6), DECUDK (DCS Pid|...),
//     OSC 50, DEC locator, ... is "add one table entry next to
//     the handler." These tests pin the contract so a future
//     refactor that breaks the lookup surfaces it loudly.

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Parser.h"
#include <cstdio>

namespace VTP = Ubiquitty::VT::Parser;

// State::Ground == 0 by contract: a freshly-init'd Ubiquitty::Terminal carries it
// without a call (memset(0) leaves parseState == Ground), and the rest of
// the parser code-paths assume "no state to clean up" maps to the
// zero value.
// ---- Plain CSI table -------------------------------------------- *
//
// Sample a representative final from each "region" of the ASCII
// range — uppercase letters (cursor motion / erase / scroll),
// symbols (`, x), lowercase (sgr, mode, dsr). Any null here means
// a handler entry is missing from the table.

// ---- Prefixed CSI table ----------------------------------------- *
//
// The unified (priv, intermediate, final) table. Hit each prefix
// region: ' ' / '!' / '"' / '$' / '*' intermediates and '=' / '>'
// / '?' prefixes.

// ---- ESC table --------------------------------------------------

// ---- Integration smoke: handlers actually fire ------------------ *
//
// Pick one entry from each table and exercise the round-trip:
// feed bytes into the parser → observable side effect happens.
// Catches an entry the table accepts but whose handler never runs.

// ---- OSC table --------------------------------------------------

// ---- DCS table --------------------------------------------------

// ---- DFA: transition(state, byte) → (Action, NextState) -------- *
//
// The parser's authoritative state machine. Tests assert specific
// (state, byte) → (action, next_state) entries plus a fuzz pass
// that feeds pseudo-random bytes through the live parser and
// asserts no abort over 4096 steps.

// Walk every (state, byte) pair and confirm transition() never
// returns a NextState outside the enum range — catches a typo'd
// cast or missing-case that would leak undefined state.
/// Fuzz: feed 4096 pseudo-random bytes through the parser and
/// assert it doesn't abort or leave the parser in a wedged state.
/// Deterministic seed so failures reproduce.
TEST(testGroundIsZero) { CHECK(static_cast<int>(VTP::State::Ground) == 0); }

TEST(test_findCSIPlain_present)
{
    CHECK(VTP::findCSIPlain('@') != nullptr); // ICH
    CHECK(VTP::findCSIPlain('A') != nullptr); // CUU
    CHECK(VTP::findCSIPlain('H') != nullptr); // CUP
    CHECK(VTP::findCSIPlain('J') != nullptr); // ED
    CHECK(VTP::findCSIPlain('S') != nullptr); // SU
    CHECK(VTP::findCSIPlain('T') != nullptr); // SD
    CHECK(VTP::findCSIPlain('Z') != nullptr); // CBT
    CHECK(VTP::findCSIPlain('`') != nullptr); // HPA
    CHECK(VTP::findCSIPlain('b') != nullptr); // REP
    CHECK(VTP::findCSIPlain('c') != nullptr); // DA1
    CHECK(VTP::findCSIPlain('h') != nullptr); // SM
    CHECK(VTP::findCSIPlain('m') != nullptr); // SGR
    CHECK(VTP::findCSIPlain('n') != nullptr); // DSR
    CHECK(VTP::findCSIPlain('r') != nullptr); // DECSTBM
    CHECK(VTP::findCSIPlain('s') != nullptr); // SCOSC/DECSLRM
    CHECK(VTP::findCSIPlain('t') != nullptr); // XTWINOPS
    CHECK(VTP::findCSIPlain('u') != nullptr); // SCORC
    CHECK(VTP::findCSIPlain('x') != nullptr); // DECREQTPARM
}

TEST(test_findCSIPlain_misses)
{
    // Bytes outside the 0x40..0x7E final range should never resolve.
    CHECK(VTP::findCSIPlain(0x00) == nullptr);
    CHECK(VTP::findCSIPlain(0x1F) == nullptr);
    CHECK(VTP::findCSIPlain('!') == nullptr); // '!' is an intermediate
    CHECK(VTP::findCSIPlain('?') == nullptr); // '?' is a prefix
    CHECK(VTP::findCSIPlain('y') == nullptr); // DECTST — unhandled
    CHECK(VTP::findCSIPlain(0x7F) == nullptr);
}

TEST(test_findCSIPrefixed_intermediates)
{
    CHECK(VTP::findCSIPrefixed(0, ' ', 'q') != nullptr); // DECSCUSR
    CHECK(VTP::findCSIPrefixed(0, '!', 'p') != nullptr); // DECSTR
    CHECK(VTP::findCSIPrefixed(0, '"', 'q') != nullptr); // DECSCA
    CHECK(VTP::findCSIPrefixed(0, '$', 'p') != nullptr); // DECRQM A
    CHECK(VTP::findCSIPrefixed(0, '$', 't') != nullptr); // DECRARA
    CHECK(VTP::findCSIPrefixed(0, '$', 'v') != nullptr); // DECCRA
    CHECK(VTP::findCSIPrefixed(0, '$', 'x') != nullptr); // DECFRA
    CHECK(VTP::findCSIPrefixed(0, '$', 'z') != nullptr); // DECERA
    CHECK(VTP::findCSIPrefixed(0, '*', 'x') != nullptr); // DECSACE
}

TEST(test_findCSIPrefixed_prefixes)
{
    CHECK(VTP::findCSIPrefixed('=', 0, 'c') != nullptr); // DA3
    CHECK(VTP::findCSIPrefixed('>', 0, 'c') != nullptr); // DA2
    CHECK(VTP::findCSIPrefixed('>', 0, 'q') != nullptr); // XTVERSION
    CHECK(VTP::findCSIPrefixed('?', '$', 'p') != nullptr); // DECRQM D
    // ?-prefix family.
    CHECK(VTP::findCSIPrefixed('?', 0, 'h') != nullptr); // DECSET
    CHECK(VTP::findCSIPrefixed('?', 0, 'l') != nullptr); // DECRST
    CHECK(VTP::findCSIPrefixed('?', 0, 'n') != nullptr); // DSR-DEC
    CHECK(VTP::findCSIPrefixed('?', 0, 'K') != nullptr); // DECSEL
    CHECK(VTP::findCSIPrefixed('?', 0, 'J') != nullptr); // DECSED
    CHECK(VTP::findCSIPrefixed('?', 0, 'W') != nullptr); // DECST8C
    // kitty-keyboard family
    CHECK(VTP::findCSIPrefixed('?', 0, 'u') != nullptr); // query
    CHECK(VTP::findCSIPrefixed('<', 0, 'u') != nullptr); // pop
    CHECK(VTP::findCSIPrefixed('=', 0, 'u') != nullptr); // set
    CHECK(VTP::findCSIPrefixed('>', 0, 'u') != nullptr); // push
    // xterm modifyKeys family
    CHECK(VTP::findCSIPrefixed('?', 0, 'm') != nullptr); // query modifiers
    CHECK(VTP::findCSIPrefixed('>', 0, 'g') != nullptr); // query fmt
    CHECK(VTP::findCSIPrefixed('>', 0, 'm') != nullptr); // set modifiers
    CHECK(VTP::findCSIPrefixed('>', 0, 'f') != nullptr); // set fmt
    CHECK(VTP::findCSIPrefixed('>', 0, 'n') != nullptr); // clear
}

TEST(test_findCSIPrefixed_misses)
{
    // Wrong-combination probes — every coordinate matters.
    CHECK(VTP::findCSIPrefixed(0, '$', 'q') == nullptr); // no $ q
    CHECK(VTP::findCSIPrefixed(0, '#', 'z') == nullptr); // no int
    CHECK(VTP::findCSIPrefixed('?', 0, 'X') == nullptr);
    CHECK(VTP::findCSIPrefixed('!', '$', 'x') == nullptr);
    CHECK(VTP::findCSIPrefixed('<', 0, 'c') == nullptr);
}

TEST(test_prefixedKey_packs_3_bytes)
{
    // The packing is part of the public contract: it encodes a cache
    // key for instrumentation hooks. Pin a representative
    // key so a future refactor that changes the bit layout breaks
    // loudly instead of silently shifting cache buckets.
    unsigned k1 = VTP::prefixedKey('?', '$', 'p');
    CHECK(k1 == (('?' << 16) | ('$' << 8) | 'p'));
    unsigned k0 = VTP::prefixedKey(0, ' ', 'q');
    CHECK(k0 == ((' ' << 8) | 'q'));
}

TEST(test_findEscape_present)
{
    CHECK(VTP::findEscape('7') != nullptr); // DECSC
    CHECK(VTP::findEscape('8') != nullptr); // DECRC
    CHECK(VTP::findEscape('=') != nullptr); // DECKPAM
    CHECK(VTP::findEscape('>') != nullptr); // DECKPNM
    CHECK(VTP::findEscape('D') != nullptr); // IND
    CHECK(VTP::findEscape('E') != nullptr); // NEL
    CHECK(VTP::findEscape('H') != nullptr); // HTS
    CHECK(VTP::findEscape('M') != nullptr); // RI
    CHECK(VTP::findEscape('N') != nullptr); // SS2
    CHECK(VTP::findEscape('O') != nullptr); // SS3
    CHECK(VTP::findEscape('Z') != nullptr); // DECID
    CHECK(VTP::findEscape('c') != nullptr); // RIS
}

TEST(test_findEscape_misses)
{
    // Multi-byte ESC prefixes don't land in this table — they have
    // their own state transitions (HASH for #, CHARSET for (, ESC_SP
    // for SP).
    CHECK(VTP::findEscape('#') == nullptr);
    CHECK(VTP::findEscape('(') == nullptr);
    CHECK(VTP::findEscape(')') == nullptr);
    CHECK(VTP::findEscape('*') == nullptr);
    CHECK(VTP::findEscape(' ') == nullptr);
    CHECK(VTP::findEscape('P') == nullptr); // DCS opener
    CHECK(VTP::findEscape(']') == nullptr); // OSC opener
    CHECK(VTP::findEscape('Q') == nullptr); // undefined
}

TEST(test_findOSC_present)
{
    CHECK(VTP::findOSC(0) != nullptr); // title alias
    CHECK(VTP::findOSC(1) != nullptr);
    CHECK(VTP::findOSC(2) != nullptr);
    CHECK(VTP::findOSC(4) != nullptr); // palette set/query
    CHECK(VTP::findOSC(7) != nullptr); // currentDirectory notification
    CHECK(VTP::findOSC(9) != nullptr); // iTerm growl / progress
    CHECK(VTP::findOSC(10) != nullptr); // default foreground
    CHECK(VTP::findOSC(11) != nullptr); // default background
    CHECK(VTP::findOSC(12) != nullptr); // cursor colour
    CHECK(VTP::findOSC(17) != nullptr); // selection background
    CHECK(VTP::findOSC(8) != nullptr); // hyperlinks
    CHECK(VTP::findOSC(19) != nullptr); // selection foreground
    CHECK(VTP::findOSC(50) != nullptr); // set/query font
    CHECK(VTP::findOSC(52) != nullptr); // clipboard
    CHECK(VTP::findOSC(99) != nullptr); // kitty notification
    CHECK(VTP::findOSC(110) != nullptr); // reset foreground
    CHECK(VTP::findOSC(111) != nullptr); // reset background
    CHECK(VTP::findOSC(112) != nullptr); // reset cursor colour
    CHECK(VTP::findOSC(117) != nullptr); // reset selection background
    CHECK(VTP::findOSC(119) != nullptr); // reset selection foreground
    CHECK(VTP::findOSC(133) != nullptr); // prompt markers
    CHECK(VTP::findOSC(633) != nullptr); // VS Code shell integration
    CHECK(VTP::findOSC(777) != nullptr); // urxvt notify
    CHECK(VTP::findOSC(1337) != nullptr); // iTerm2 proprietary
}

TEST(test_findOSC_misses)
{
    CHECK(VTP::findOSC(3) == nullptr); // set X property — not supported
    CHECK(VTP::findOSC(7704) == nullptr); // Wezterm — not supported
    CHECK(VTP::findOSC(-1) == nullptr);
}

TEST(test_findDCS_present)
{
    CHECK(VTP::findDCS(0, 'q') != nullptr); // DECSIXEL
    CHECK(VTP::findDCS('$', 'q') != nullptr); // DECRQSS
    CHECK(VTP::findDCS('+', 'q') != nullptr); // XTGETTCAP
}

TEST(test_findDCS_misses)
{
    CHECK(VTP::findDCS(0, 'p') == nullptr);
    CHECK(VTP::findDCS('!', 'q') == nullptr);
    CHECK(VTP::findDCS('$', 'r') == nullptr);
    CHECK(VTP::findDCS('+', 'z') == nullptr);
}

TEST(testDCSHandlerExecutes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// DECRQSS "r" — request DECSTBM. Reply form is
    /// DCS 1 $ r <top>;<bot> r ST   on success.
    /// A freshly-init Ubiquitty::Terminal has the full-screen scroll region.
    Ubiquitty::processBytes(&t, "\033P$qr\033\\");
    char outputBuffer[64];
    int n = Ubiquitty::drainOutput(&t, { outputBuffer, sizeof(outputBuffer) - 1 });
    outputBuffer[n] = 0;
    CHECK(n > 0);
    CHECK(outputBuffer[0] == 0x1B && outputBuffer[1] == 'P' && outputBuffer[2] == '1' && outputBuffer[3] == '$' && outputBuffer[4] == 'r');
}

TEST(testTransitionDocumentedArrows)
{
    using S = VTP::State;
    using A = VTP::Action;
    // Ground.
    CHECK(VTP::transition(S::Ground, 0x1B).action == A::EnterEscape);
    CHECK(VTP::transition(S::Ground, 0x1B).next == S::Escape);
    CHECK(VTP::transition(S::Ground, 0x07).action == A::ExecuteBell);
    CHECK(VTP::transition(S::Ground, 0x08).action == A::ExecuteBackspace);
    CHECK(VTP::transition(S::Ground, 0x09).action == A::ExecuteHorizontalTab);
    CHECK(VTP::transition(S::Ground, 0x0A).action == A::ExecuteLineFeed);
    CHECK(VTP::transition(S::Ground, 0x0D).action == A::ExecuteCarriageReturn);
    CHECK(VTP::transition(S::Ground, 0x0E).action == A::ExecuteShiftOut);
    CHECK(VTP::transition(S::Ground, 0x0F).action == A::ExecuteShiftIn);
    CHECK(VTP::transition(S::Ground, 'A').action == A::Print);
    CHECK(VTP::transition(S::Ground, 0x90).action == A::C1EnterDCS);
    CHECK(VTP::transition(S::Ground, 0x9B).action == A::C1EnterCSI);
    CHECK(VTP::transition(S::Ground, 0x9D).action == A::C1EnterOSC);
    CHECK(VTP::transition(S::Ground, 0x9F).action == A::C1EnterAPC);
    // Escape.
    CHECK(VTP::transition(S::Escape, '[').action == A::EnterCSI);
    CHECK(VTP::transition(S::Escape, '[').next == S::CSI);
    CHECK(VTP::transition(S::Escape, ']').action == A::EnterOSC);
    CHECK(VTP::transition(S::Escape, 'P').action == A::EnterDCS);
    CHECK(VTP::transition(S::Escape, '_').action == A::EnterAPC);
    CHECK(VTP::transition(S::Escape, '(').action == A::EnterCharacterSetG0);
    CHECK(VTP::transition(S::Escape, ')').action == A::EnterCharacterSetG1);
    CHECK(VTP::transition(S::Escape, '*').action == A::EnterCharacterSetG2);
    CHECK(VTP::transition(S::Escape, '+').action == A::EnterCharacterSetG3);
    CHECK(VTP::transition(S::Escape, '#').action == A::Noop);
    CHECK(VTP::transition(S::Escape, '#').next == S::Hash);
    CHECK(VTP::transition(S::Escape, ' ').action == A::Noop);
    CHECK(VTP::transition(S::Escape, ' ').next == S::EscapeIntermediate);
    CHECK(VTP::transition(S::Escape, 'D').action == A::EscapeDispatch);
    CHECK(VTP::transition(S::Escape, 'D').next == S::Ground);
    // CSI.
    CHECK(VTP::transition(S::CSI, '0').action == A::CSIParameterDigit);
    CHECK(VTP::transition(S::CSI, ';').action == A::CSIParameterSemicolon);
    CHECK(VTP::transition(S::CSI, ':').action == A::CSIParameterColon);
    CHECK(VTP::transition(S::CSI, '?').action == A::CSIPrivatePrefix);
    CHECK(VTP::transition(S::CSI, '$').action == A::CSIIntermediate);
    CHECK(VTP::transition(S::CSI, 'H').action == A::CSIDispatch);
    CHECK(VTP::transition(S::CSI, 'H').next == S::Ground);
    // C0 controls embedded in a CSI execute in place and the sequence
    // continues (VT500). BEL fires + stays in CSI; can cancels to
    // Ground; ESC restarts the escape; DEL is ignored (stays).
    CHECK(VTP::transition(S::CSI, 0x07).action == A::ExecuteBell);
    CHECK(VTP::transition(S::CSI, 0x07).next == S::CSI);
    CHECK(VTP::transition(S::CSI, 0x0D).action == A::ExecuteCarriageReturn);
    CHECK(VTP::transition(S::CSI, 0x0D).next == S::CSI);
    CHECK(VTP::transition(S::CSI, 0x18).next == S::Ground);
    CHECK(VTP::transition(S::CSI, 0x1B).action == A::EnterEscape);
    CHECK(VTP::transition(S::CSI, 0x1B).next == S::Escape);
    CHECK(VTP::transition(S::CSI, 0x7F).next == S::CSI);
    // OSC.
    CHECK(VTP::transition(S::OSC, 'x').action == A::OSCPut);
    CHECK(VTP::transition(S::OSC, 0x07).action == A::OSCBellEnd);
    CHECK(VTP::transition(S::OSC, 0x1B).action == A::Noop);
    CHECK(VTP::transition(S::OSC, 0x1B).next == S::OSCEscape);
    CHECK(VTP::transition(S::OSCEscape, '\\').action == A::OSCStringTerminatorEnd);
    CHECK(VTP::transition(S::OSCEscape, 'x').action == A::OSCEscapeOther);
    // ESC followed by anything but the terminator ends the string and is the
    // start of an escape, so the byte after it is read in the escape state.
    CHECK(VTP::transition(S::OSCEscape, 'x').next == S::Escape);
    // DCS — both phases.
    CHECK(VTP::transition(S::DCS, '$').action == A::DCSHeaderIntermediate);
    CHECK(VTP::transition(S::DCS, 'q').action == A::DCSHeaderFinal);
    CHECK(VTP::transition(S::DCS, 'q').next == S::DCSPassthrough);
    CHECK(VTP::transition(S::DCS, '5').action == A::DCSParameterDigit);
    CHECK(VTP::transition(S::DCS, ';').action == A::DCSParameterSemicolon);
    CHECK(VTP::transition(S::DCSPassthrough, 'x').action == A::DCSPayloadPut);
    // DCS and PM execute BEL; APC and SOS consume it.
    CHECK(VTP::transition(S::DCS, 0x07).action == A::StringBell);
    CHECK(VTP::transition(S::DCS, 0x07).next == S::DCS);
    CHECK(VTP::transition(S::DCSPassthrough, 0x07).action == A::StringBell);
    CHECK(VTP::transition(S::DCSPassthrough, 0x07).next == S::DCSPassthrough);
    CHECK(VTP::transition(S::APC, 0x07).action == A::Noop);
    CHECK(VTP::transition(S::APC, 0x07).next == S::APC);
    CHECK(VTP::transition(S::SOSPMAPC, 0x07).action == A::StringBell);
    CHECK(VTP::transition(S::SOSPMAPC, 0x07).next == S::SOSPMAPC);
    CHECK(VTP::transition(S::DCSPassthrough, 0x1B).action == A::Noop);
    CHECK(VTP::transition(S::DCSPassthrough, 0x1B).next == S::DCSEscape);
    CHECK(VTP::transition(S::DCSEscape, '\\').action == A::DCSStringTerminatorEnd);
    CHECK(VTP::transition(S::DCSEscape, 'x').action == A::DCSEscapeOther);
    // APC.
    CHECK(VTP::transition(S::APC, 'x').action == A::APCPut);
    CHECK(VTP::transition(S::APC, 0x1B).action == A::Noop);
    CHECK(VTP::transition(S::APC, 0x1B).next == S::APCEscape);
    CHECK(VTP::transition(S::APCEscape, '\\').action == A::APCStringTerminatorEnd);
    CHECK(VTP::transition(S::APCEscape, 'x').action == A::APCEscapeOther);
    CHECK(VTP::transition(S::APCEscape, 'x').next == S::Escape);
    CHECK(VTP::transition(S::SOSPMAPCEscape, 'x').action == A::StringEscapeOther);
    CHECK(VTP::transition(S::SOSPMAPCEscape, 'x').next == S::Escape);
    // Single-byte intermediate states.
    CHECK(VTP::transition(S::Hash, '8').action == A::HashByte);
    CHECK(VTP::transition(S::Hash, '8').next == S::Ground);
    CHECK(VTP::transition(S::CharacterSet, 'B').action == A::CharacterSetByte);
    CHECK(VTP::transition(S::EscapeIntermediate, 'F').action == A::EscapeSpaceByte);
    // A second intermediate makes a sequence Core does not define, consumed
    // through its final: `ESC # ! 6` and `ESC SP ! F` print nothing.
    CHECK(VTP::transition(S::Hash, '!').action == A::Noop);
    CHECK(VTP::transition(S::Hash, '!').next == S::EscapeIntermediateGeneric);
    CHECK(VTP::transition(S::EscapeIntermediate, '!').next == S::EscapeIntermediateGeneric);
    CHECK(VTP::transition(S::EscapeIntermediateGeneric, '6').next == S::Ground);
    CHECK(VTP::transition(S::EscapeIntermediateGeneric, '6').action == A::Noop);
    CHECK(VTP::transition(S::VT52Y, 'A').action == A::VT52YRow);
    CHECK(VTP::transition(S::VT52YColumn, 'B').action == A::VT52YColumn);
}

TEST(testTransitionAllStatesValid)
{
    using S = VTP::State;
    int outOfRange = 0;
    for (int s = 0; s <= static_cast<int>(S::EscapeCodingSystem); s++) {
        S state = (S)s;
        for (int b = 0; b < 256; b++) {
            VTP::Transition tr = VTP::transition(state, (unsigned char)b);
            int n = (int)tr.next;
            if (n < 0 || n > static_cast<int>(S::EscapeCodingSystem)) {
                std::printf("  out-of-range next state=%d byte=0x%02X "
                            "→ next=%d\n",
                    s, b, n);
                outOfRange++;
            }
        }
    }
    CHECK(outOfRange == 0);
}

TEST(testParserFuzzDoesntAbort)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned rng = 0xC0FFEE42u;
    for (int i = 0; i < 4096; i++) {
        rng = rng * 1664525u + 1013904223u;
        unsigned char b;
        unsigned r = rng & 0xFFu;
        if (r < 128)
            b = (unsigned char)(0x20 + (r % 0x5F));
        else if (r < 192) {
            unsigned char picks[] = { 0x07, 0x08, 0x0A, 0x0D, 0x1B, '[', ']', 'P', '_', '#', '(' };
            b = picks[r % (sizeof(picks) / sizeof(picks[0]))];
        } else
            b = (unsigned char)r;
        Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(&b), static_cast<std::size_t>(1)));
    }
    /// Parser must terminate any in-flight sequence cleanly by RIS.
    Ubiquitty::processBytes(&t, "\033c");
    CHECK(t.parser.parseState == VTP::State::Ground);
}

TEST(testCSIPlainHandlerExecutes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// CUP 5;7 → cursor lands at row 4 column 6 (0-based).
    Ubiquitty::processBytes(&t, "\033[5;7H");
    CHECK(t.screens.active->cursorX == 6);
    CHECK(t.screens.active->cursorY == 4);
}

TEST(testCSIPrefixedHandlerExecutes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// DECSCUSR 4 (steady underline) → cursorStyle = SteadyUnderline.
    Ubiquitty::processBytes(&t, "\033[4 q");
    CHECK(t.cursorStyle == Ubiquitty::Cursor::Style::SteadyUnderline);
}

TEST(testESCHandlerExecutes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// DECKPAM → appKeypad = true.
    Ubiquitty::processBytes(&t, "\033=");
    CHECK(t.modes.appKeypad == true);
    /// DECKPNM → back to false.
    Ubiquitty::processBytes(&t, "\033>");
    CHECK(t.modes.appKeypad == false);
}

TEST(testOSCHandlerExecutes)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    /// OSC 2 ; "hello" ST — sets window title.
    Ubiquitty::processBytes(&t, "\033]2;hello\033\\");
    CHECK(t.titleChanged == true);
    // Title bytes land in Terminal::title (NUL-terminated by the copy).
    CHECK(!t.titleUTF8.empty() && t.titleUTF8[0] == 'h');
    CHECK(t.titleUTF8.size() > 4 && t.titleUTF8[4] == 'o');
}

// Raw 8-bit C1 controls under a single-byte encoding reach the same action
// handlers as their ESC-prefixed forms. IND (0x84) line-feeds, HTS (0x88) sets
// a tab stop at the cursor, RI (0x8D) reverse-indexes, and the string
// introducers (0x90 DCS, 0x9D OSC, 0x9F APC) open their states — the per-byte
// driver's C1 action table in action.
TEST(testRawC1ControlsExecuteTheirActions)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    Ubiquitty::resize(t, 5, 10);

    feed(t, "ab");
    CHECK(cell(t, 0, 0)->codePoint == 'a');
    feed(t, "\x84"); // IND: line feed without CR
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 2); // column preserved
    CHECK(cell(t, 0, 1)->codePoint == 'b'); // glyphs stay where printed

    feed(t, "\x88"); // HTS: tab stop at column 2
    CHECK(t.tabStops[2] == true);

    feed(t, "\x8D"); // RI: reverse index
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 2);

    feed(t, "\x90"); // DCS introducer
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::DCS);
    feed(t, "\x9C"); // ST closes it
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(t, "\x9D"); // OSC introducer
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::OSC);
    feed(t, "\x07"); // BEL closes it
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(t, "\x9F"); // APC introducer
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::APC);
    feed(t, "\x9C"); // ST closes it
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

// A raw C1 ST (0x9C) terminates an open DCS string when 8-bit terminators are
// enabled; the string-state arm of the C1 dispatch runs per state.
TEST(testRawC1STTerminatesOpenStrings)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);

    feed(t, "\x90"); // DCS introducer
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::DCS);
    feed(t, "abc"); // header final byte 'a' opens the payload phase
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::DCSPassthrough);
    feed(t, "\x9C"); // C1 ST: string-state terminate
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

// REP after a non-repeatable preceding item (an OSC) hits the Kind::None arm:
// nothing is replayed, and the next graphic still lands normally.
TEST(testRepAfterNonRepeatableItemIsNoop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033]0;title\007"); // OSC: preceding item is None
    feed(t, "\033[3b"); // REP x3 of nothing
    feed(t, "x");
    CHECK(cell(t, 0, 0)->codePoint == 'x');
}

// A single-byte (Latin-1) feed routes C1 bytes through the per-byte driver and
// transcodes the rest in batched runs: the transcode staging path is the one
// that maps each input byte through encDecodeToUTF8.
TEST(testLatin1TranscodeRoutesC1PerByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    Ubiquitty::resize(t, 5, 10);

    feed(t, "abc\x84"
            "def"); // IND lands between the two runs
    CHECK(t.screens.active->cursorY == 1); // line feed, column preserved
    CHECK(cell(t, 0, 2)->codePoint == 'c');
    CHECK(cell(t, 1, 3)->codePoint == 'd');
    CHECK(cell(t, 1, 5)->codePoint == 'f');
}

// The sync-clear stop mirrors into the transcoding loop: under a single-byte
// encoding, processBytesUpToSyncClear returns before the byte that cleared
// syncOutput, and the caller's next feed delivers the tail.
TEST(testLatin1SyncClearStopsTranscode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);

    const char stream[] = "\033[?2026hab\033[?2026lXYZ";
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(stream, sizeof(stream) - 1));
    CHECK(consumed < static_cast<int>(sizeof(stream) - 1)); // the sync window exists
    feed(t, std::string_view(stream + consumed, static_cast<std::size_t>(sizeof(stream) - 1 - consumed)));
    CHECK(cell(t, 0, 2)->codePoint == 'X');
    CHECK(cell(t, 0, 3)->codePoint == 'Y');
    CHECK(cell(t, 0, 4)->codePoint == 'Z');
}

// The same stop on the main UTF-8 loop: the loop returns at the DECRST that
// clears the sync window, and the tail feed delivers the rest. (Sync mode
// defers the snapshot, not the cell writes — the window's bytes print.)
TEST(testSyncClearStopsMainLoop)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    const char stream[] = "\033[?2026hab\033[?2026lXYZ";
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(stream, sizeof(stream) - 1));
    CHECK(consumed < static_cast<int>(sizeof(stream) - 1)); // the sync window exists
    feed(t, std::string_view(stream + consumed, static_cast<std::size_t>(sizeof(stream) - 1 - consumed)));
    CHECK(cell(t, 0, 2)->codePoint == 'X');
    CHECK(cell(t, 0, 3)->codePoint == 'Y');
    CHECK(cell(t, 0, 4)->codePoint == 'Z');
}

// REP of an ESC sequence replays it through the recorded sequence's dispatch:
// a hash sequence (DECALN) refills the screen, an SCS designation re-selects
// the set, and a 96-set designation keeps its own registry.
TEST(testREPReplaysESCSequences)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033#8"); // DECALN records the hash sequence
    feed(t, "\033[2b"); // REP x2 refills the screen
    CHECK(cell(t, 0, 0)->codePoint == 'E');

    feed(t, "\033(0\033[2b"); // G0 = DEC special graphics, REP x2
    feed(t, "\033[1;1H`"); // 0x60 through DEC special maps to U+25C6
    CHECK(cell(t, 0, 0)->codePoint == 0x25C6);

    feed(t, "\033.A\033[2b"); // G2 = ISO-8859-1 96-set, REP x2
    feed(t, "\033[1;1H\033N`"); // SS2 invokes G2; 0x60 folds up to 0xE0 = à
    CHECK(cell(t, 0, 0)->codePoint == 0xE0);

    feed(t, "\033-A\033[2b"); // G1 = ISO-8859-1 96-set, REP x2 (the '-' designator)
    feed(t, "\033}"); // LS1R: GR now invokes G1
    feed(t, "\xA0"); // 0xA0 through Latin-1 G1 folds up to 0xE0 = à
    CHECK(cell(t, 0, 0)->codePoint == 0xE0);

    feed(t, "\033 G\033[2b"); // ESC SP G (ANSI graphics rendition), REP x2
}

// REP of an ESC sequence whose intermediate isn't a designation (or hash/space)
// takes the replay's default arm: the sequence is dropped, nothing changes.
TEST(testREPReplaysUnknownIntermediateEscape)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033%G"); // UTF-8 designation (ESC % G) — intermediate 0x25
    feed(t, "\033[2b"); // REP x2 replays it through the unknown-intermediate arm
    feed(t, "x");
    CHECK(cell(t, 0, 0)->codePoint == 'x'); // nothing on screen from the REP
}

// The stop also fires when a per-byte dispatch clears the window: a soft
// reset (ESC c) arriving mid-feed clears syncOutput through the per-byte
// driver, so the loop returns right after the reset byte.
TEST(testSyncClearStopOnPerByteReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033[?2026h"); // sync on — the next call starts inside the window
    const char stream[] = "\033cXYZ"; // RIS clears the window mid-feed
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(stream, sizeof(stream) - 1));
    CHECK(consumed < static_cast<int>(sizeof(stream) - 1)); // stopped at the reset
    feed(t, std::string_view(stream + consumed, static_cast<std::size_t>(sizeof(stream) - 1 - consumed)));
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(cell(t, 0, 1)->codePoint == 'Y');
    CHECK(cell(t, 0, 2)->codePoint == 'Z');
}

// A C1 control spelled in UTF-8 (C2 80..C2 9F) draws nothing and moves
// nothing: in a UTF-8 stream it is not a control, and it has no graphic. Both
// the per-character path and the batched text run must drop it.
TEST(testC1CodePointsFromUTF8AreIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\xC2\x9B"
            "2;2H");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == '2');
    CHECK(t.screens.active->cursorX == 5);

    Ubiquitty::hardReset(t);
    feed(t, "\xC3\xA9\xC2\x88\xC3\xA9"); // é, U+0088, é: one batched run
    CHECK(cell(t, 0, 0)->codePoint == 0xE9);
    CHECK(cell(t, 0, 1)->codePoint == 0xE9);
    CHECK(t.screens.active->cursorX == 2);
}

// An ESC while an escape sequence still waits for its final starts a new
// sequence; the first is abandoned rather than finished by the ESC.
TEST(testEscapeInsideAnEscapeSequenceStartsANewOne)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    for (const char* opener : { "\033#", "\033(", "\033 ", "\033%", "\033$(" }) {
        Ubiquitty::hardReset(t);
        feed(t, opener);
        feed(t, "\033[2;3HX");
        CHECK_MSG(cell(t, 1, 2)->codePoint == 'X', opener);
        CHECK_MSG(cell(t, 0, 0)->codePoint == 0, opener);
    }
    // From every other state an ESC does the same: a CSI at any stage, a
    // control string's header or payload, and PM and APC, where an ESC
    // not followed by the terminator ends the string.
    for (const char* opener : { "\033[", "\033[2", "\033[2 ", "\033[?", "\033[2?", "\033P", "\033P1$", "\033Pzhi", "\033]0;x", "\033^", "\033_" }) {
        Ubiquitty::hardReset(t);
        feed(t, opener);
        feed(t, "\033[2;3HX");
        CHECK_MSG(cell(t, 1, 2)->codePoint == 'X', opener);
        CHECK_MSG(cell(t, 0, 0)->codePoint == 0, opener);
    }
    // A C0 control executes in place and the sequence carries on.
    Ubiquitty::hardReset(t);
    feed(t, "AB\033#\r8");
    CHECK(cell(t, 0, 0)->codePoint == 'E'); // DECALN after the CR
}

// A private marker after a parameter, an intermediate or another marker, or a
// parameter after an intermediate, makes the CSI malformed: it is consumed
// through its final and does nothing.
TEST(testMalformedControlSequenceIsIgnored)
{
    Ubiquitty::Terminal t;
    for (const char* malformed : { "\033[2?3H", "\033[??5h", "\033[2 3H", "\033[?1$2p" }) {
        Ubiquitty::hardReset(t);
        feed(t, malformed);
        feed(t, "X");
        CHECK_MSG(cell(t, 0, 0)->codePoint == 'X', malformed);
        char reply[64];
        CHECK_MSG(drain(t, reply) == 0, malformed);
    }
    // A well-formed sequence after one still acts.
    Ubiquitty::hardReset(t);
    feed(t, "\033[2?3H\033[2;3HX");
    CHECK(cell(t, 1, 2)->codePoint == 'X');
}

// Only SGR and DECPS give a colon a meaning; any other control carrying one is
// ignored rather than read as if the colon were a semicolon.
TEST(testSubparametersAreTakenOnlyWhereDefined)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2:3HX");
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    feed(t, "\033[4:3mY");
    CHECK(Ubiquitty::penUnderlineShape(&cell(t, 0, 1)->pen) == Ubiquitty::VT::underlineCurly);
}

// An ESC that is neither ST nor a doubled ESC ends an unterminated DCS without
// dispatching it, and the bytes after it are an escape sequence again.
TEST(testEscapeAbandonsAnUnterminatedDeviceControlString)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\033Pzhi\033[2;2HB");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 1)->codePoint == 'B');
    char reply[64];
    CHECK(drain(t, reply) == 0);
    // An abandoned request answers nothing either.
    Ubiquitty::hardReset(t);
    feed(t, "\033P$qm\033[HC");
    CHECK(drain(t, reply) == 0);
    CHECK(cell(t, 0, 0)->codePoint == 'C');
}

// An ESC that is not ST abandons an unterminated OSC: the title it would have
// set is never set, and the bytes after the ESC are an escape sequence again.
TEST(testEscapeAbandonsAnUnterminatedOperatingSystemCommand)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033]2;cut\0337X");
    CHECK(t.titleUTF8.empty());
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(t.screens.active->hasSavedCursor);
    // A string ended by ST is still carried out.
    feed(t, "\033]2;kept\033\\");
    CHECK(t.titleUTF8 == "kept");
}

// A C1 control spelled in UTF-8 between the bytes of a sequence is ignored and
// the sequence carries on: ESC U+0085 D is IND, and a CSI keeps its parameters.
TEST(testUTF8C1InsideASequenceIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033\xC2\x85"
            "DX");
    CHECK(cell(t, 1, 0)->codePoint == 'X');
    CHECK(cell(t, 0, 0)->codePoint == 0);
    feed(t, "\033[3\xC2\x9B;4HY");
    CHECK(cell(t, 2, 3)->codePoint == 'Y');
}

// A second intermediate byte names a control Core does not have, so nothing
// dispatches: `CSI 1 # # {` is not XTPUSHSGR, nor `CSI * # |` XTREPORTSGR.
TEST(testASecondIntermediateDispatchesNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    char buffer[64] = {};
    feed(t, "\033[1m\033[1##{\033[0m\033[#}A");
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    feed(t, "\033[1;1;1;1*#|");
    CHECK(drain(t, buffer) == 0);
}

// A CSI with two intermediates is no control: the pair and the final byte
// together name it, so neither intermediate alone selects one.
TEST(testCSIWithTwoIntermediatesIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 5);
    feed(t, "abcde\033[1;2H\033[2!'}\033[2!'~X");
    CHECK(cell(t, 0, 1)->codePoint == 'X');
    CHECK(cell(t, 0, 2)->codePoint == 'c');
    feed(t, "\033[4h\033[ !p");
    CHECK(t.modes.insertMode);
}

UBIQUITTY_TEST_MAIN()
