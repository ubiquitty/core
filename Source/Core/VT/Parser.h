// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Parser.h
//
// Table-driven DEC/ANSI escape-sequence dispatch. The per-byte state driver
// (processByteInner) stays in VT.cpp; on a final byte it calls a dispatch API
// here, which does a table lookup keyed by (prefix, intermediate, final) and
// invokes the handler.
//
// Table sortedness is static_asserted at compile time. Tables are
// static constexpr POD arrays in .rodata (no STL); lookup is binary
// search. No exceptions, no RTTI (project-wide), C++23.

#pragma once

#include <string_view>

#include "Core/TerminalOperations.h"

namespace Ubiquitty::VT::Parser {

// Byte-driver states. This enum is the only spelling of them; transition() in
// VT.cpp switches on it directly.
enum class State : int {
    Ground, // normal printable/control bytes
    Escape, // just saw 0x1B; awaiting final/int
    EscapeIntermediate, // ESC SP <final>
    EscapeIntermediateGeneric, // ESC <0x20-0x2F other than SP/#/%/(/)/*/+/-/./> <final>
    Hash, // ESC #
    CharacterSet, // ESC ( / ) / * / +
    CharacterSetIntermediate, // ESC ( <0x20-0x2F> — DRCS designator intermediate; awaiting final
    CSI, // CSI parameter / intermediate / final
    OSC, // OSC string body
    OSCEscape, // OSC saw 0x1B; expect '\\' for ST
    DCS, // DCS header phase (parameters + intermediates)
    DCSPassthrough, // DCS payload phase, post-final-byte
    DCSEscape, // DCS saw 0x1B; expect '\\' for ST
    APC, // APC payload
    APCEscape, // APC saw 0x1B; expect '\\' for ST
    VT52Y, // VT52 ESC Y — awaiting row byte
    VT52YColumn, // VT52 ESC Y row — awaiting column byte
    SOSPMAPC, // PM payload
    SOSPMAPCEscape, // PM saw 0x1B; expect '\\'
    SOS, // ECMA-48 character string: only ST ends it
    SOSEscape, // SOS saw 0x1B; only a following backslash terminates
    VT52Ground, // DECANM=off — VT52 ground; shares
                // actions with Ground except ESC
    VT52Escape, // VT52 saw 0x1B; runs the VT52 grammar
    EscapeCodingSystem // ESC % <final> — select ISO 2022 or UTF-8
};

// transition() returns an (Action, State) pair per (state, byte); the
// driver performs the action and assigns the next state. Action names are
// "VerbObject" — what the action does, not the triggering byte.
enum class Action {
    Noop, // swallow byte; transition still applies

    // Ground actions.
    Print, // printable byte 0x20..  → putCharacter (UTF-8 aware)
    ExecuteBell,
    ExecuteAnswerback, // ENQ — transmit the answerback string
    ExecuteBackspace,
    ExecuteHorizontalTab,
    ExecuteLineFeed, // LF/VT/FF — honours LNM
    ExecuteCarriageReturn,
    ExecuteShiftOut,
    ExecuteShiftIn,
    EnterEscape, // 0x1B from Ground

    // C1 controls (8-bit) — fire in Ground only.
    C1Index, // 0x84 IND
    C1NextLine, // 0x85 NEL
    C1HorizontalTabSet, // 0x88 HTS
    C1HorizontalTabJustify, // 0x89 HTJ
    C1VerticalTabSet, // 0x8A VTS
    C1ReverseIndex, // 0x8D RI
    C1EnterDCS, // 0x90 — clears DCS state, transitions to DCS
    C1EnterCSI, // 0x9B
    C1EnterOSC, // 0x9D
    C1EnterAPC, // 0x9F
    C1SingleShift2, // 0x8E SS2 — next GL byte resolves in G2 (one-shot)
    C1SingleShift3, // 0x8F SS3 — next GL byte resolves in G3 (one-shot)
    C1StartGuardedArea, // 0x96 SPA — ISO 6429 guarded area start (7-bit ESC V)
    C1EndGuardedArea, // 0x97 EPA — ISO 6429 guarded area end (7-bit ESC W)
    C1Identify, // 0x9A — DECID, the 8-bit alias of ESC Z; replies primary DA

    // Escape (post-0x1B) state-entry actions.
    EnterCSI, // '[' — resetCSI
    EnterOSC, // ']' — clear osc.data
    EnterDCS, // 'P' — reset DCS state
    EnterAPC, // '_' — clear kittyAPC.data
    EnterCharacterSetG0, // '('
    EnterCharacterSetG1, // ')'
    EnterCharacterSetG2, // '*'
    EnterCharacterSetG3, // '+'
    EnterCharacterSet96G1, // '-' — 96-set designation into G1
    EnterCharacterSet96G2, // '.' — 96-set designation into G2
    EnterCharacterSet96G3, // '/' — 96-set designation into G3
    EscapeDispatch, // any other final → escLookup + run

    // CSI accumulation.
    CSIPrivatePrefix, // '<' / '=' / '>' / '?' at parameter start
    CSIParameterDigit, // '0'..'9'
    CSIParameterSemicolon, // ';' new parameter slot
    CSIParameterColon, // ':' SGR sub-parameter introducer
    CSIIntermediate, // 0x20..0x2F
    CSIDispatch, // 0x40..0x7E final

    // OSC.
    OSCPut,
    OSCBellEnd, // 0x07 terminator
    OSCStringTerminatorEnd, // '\\' after ESC: dispatch
    OSCEscapeOther, // ESC <not-\\>: the string ends, and the byte begins an escape

    // DCS — header phase + payload.
    DCSHeaderIntermediate, // 0x20..0x2F in header
    DCSHeaderFinal, // 0x40..0x7E in header — stash + lookup table
    DCSParameterDigit, // '0'..'9' in header — append to current dcsParameters slot
    DCSParameterSemicolon, // ';' in header — open next dcsParameters slot
    DCSPayloadPut, // in payload phase: route to sixel or osc.data
    StringBell, // 0x07 inside DCS or PM: rings without terminating
    DCSStringTerminatorEnd, // '\\' after ESC: terminate
    DCSEscapeOther, // ESC <not-\\>: tmux-style doubled-ESC literal

    // APC (Kitty graphics).
    APCPut,
    APCStringTerminatorEnd,
    APCEscapeOther, // ESC <not-\\>: the string ends, and the byte begins an escape

    // Single-byte intermediate-state finals.
    HashByte, // ESC # <X>
    CharacterSetByte, // ESC ( <X> etc.
    CharacterSetIntermediateByte, // ESC ( <0x20-0x2F> — stash the DRCS designator intermediate
    EscapeSpaceByte, // ESC SP <X>
    VT52YRow,
    VT52YColumn,

    // VT52.
    VT52Escape, // VT52 ESC <X> — single byte dispatch

    // SOS / PM entry (ESC X / ESC ^ / 0x98 / 0x9E). The string is consumed
    // silently, but entering it must clear the REP predecessor: control
    // strings are excluded from repeat (see PrecedingItem in Terminal.h).
    EnterSOSPM,
    // ESC <not-\\> inside PM: the string ends, and the byte after the
    // ESC begins an escape.
    StringEscapeOther,
    // SUB (0x1A): abandons any sequence in progress and draws the error
    // character in its place, as a printed character would be drawn.
    ExecuteSubstitute,

    SelectCodingSystem, // ESC % @ / G / 8

    // Sentinel — must remain last. Used for compile-time table sizing.
    _ActionCount
};

// Per-byte transition: pure function over the DEC DFA state graph.
// Returns the (action, next_state) pair the driver should perform.
// VT52 mode is not in this graph — the driver short-circuits to a
// separate VT52 path before invoking the DFA.
struct Transition {
    Action action;
    State next;
};
Transition transition(State current, unsigned char byte);

// `final` is the byte that completed the sequence; the handler reads
// parameters / intermediates off `Terminal`.
using Handler = void (*)(Ubiquitty::Terminal& t, char final);

// `CSI [parameters] final` — no prefix, no intermediate.
Handler findCSIPlain(char final);

// Prefixed/intermediate CSI. priv == 0 means no prefix; intermediate == 0
// means none. Key = (priv << 16) | (intermediate << 8) | final.
Handler findCSIPrefixed(unsigned char priv, unsigned char intermediate, char final);

// Single-byte ESC sequences. Multi-byte forms (ESC # n, ESC ( X, ESC SP X)
// route through their own per-state paths in VT.cpp.
Handler findEscape(char final);

// OSC Ps ; payload ST. Handler takes a payload pointer (after "Ps;").
// nullptr for an unregistered Ps — the driver drops it silently, never printing it.
// The payload the dispatcher already measured. `Terminal::parser.osc` is a
// ByteBuffer holding `length` beside `data`, so a handler taking a bare pointer
// re-derives a count its caller was holding — and OSC 1337 File= carries a whole
// base64 image, so that count costs a scan of the image to read five bytes.
//
// The buffer stays NUL-terminated at `data[length]` (VT.cpp, actOSCEnd), so a
// handler not yet reading the view can take `payload.data()` as a C string.
using OSCHandler = void (*)(Ubiquitty::Terminal& t, std::string_view payload);
OSCHandler findOSC(int ps);

// DCS sub-protocols keyed by (intermediate, final); intermediate == 0 means
// none. Registered (see dcsHandlers in VT.cpp): DECSIXEL, DECDLD, DECUDK,
// DECRSTS, DECRQSS, XTGETTCAP and the sample download ('+','s'). Miss →
// nullptr, driver falls back to the tmux-passthrough handler.
using DCSHandler = void (*)(Ubiquitty::Terminal& t);
DCSHandler findDCS(unsigned char intermediate, char final);

// Pull-based key formation (test hook + DCS handlers).
inline unsigned prefixedKey(unsigned char priv, unsigned char intermediate, char final) { return (static_cast<unsigned>(priv) << 16) | (static_cast<unsigned>(intermediate) << 8) | static_cast<unsigned char>(final); }

} // namespace Ubiquitty::VT::Parser
