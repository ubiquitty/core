// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Escape.cpp
//
// The ESC final-byte dispatch group: escapeH_* handlers, the sorted
// EscapeEntry table + binary-search lookup, escDispatch, and the VT52 escape
// sub-grammar (vt52EscapeDispatch). Cold relative to the parser DFA. The
// non-ANSI legacy grammars stay fused in VT.cpp (they are hot there).

#include "Core/Diagnostics/DiagLog.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Parser.h"
#include <cstddef>
#include <cstdint>

namespace Ubiquitty::VT {

using ParserState = Ubiquitty::VT::Parser::State;

// Single-byte ESC sequences; multi-byte forms route to their own per-state
// paths and never reach this table. Sorted-by-final is static_asserted.
//
// DECBI (ESC 6) / DECFI (ESC 9) — VT420 back/forward index, gated at VT400+
// like the other column operations; below that the escapes are inert.
static void escapeH_decbi(Ubiquitty::Terminal& t, char)
{
    if (t.conformanceLevel >= ConformanceLevel::VT400)
        Ubiquitty::Screen::backIndex(t);
}

static void escapeH_decfi(Ubiquitty::Terminal& t, char)
{
    if (t.conformanceLevel >= VT::ConformanceLevel::VT400)
        Ubiquitty::Screen::forwardIndex(t);
}

static void escapeH_decsc(Ubiquitty::Terminal& t, char) { Ubiquitty::Screen::saveCursor(t); }

static void escapeH_decrc(Ubiquitty::Terminal& t, char)
{
    finishAlignedTabString(t);
    Ubiquitty::Screen::restoreCursor(t);
}

static void escapeH_ind(Ubiquitty::Terminal& t, char)
{
    finishAlignedTabString(t);
    linefeed(t);
}

static void escapeH_ri(Ubiquitty::Terminal& t, char)
{
    finishAlignedTabString(t);
    reverseIndex(t);
    Screen::noteStringStart(t);
}

static void escapeH_nel(Ubiquitty::Terminal& t, char)
{
    finishAlignedTabString(t);
    finishLineSemanticRole(t);
    // NEL is IND then CR, in that order. The order is required under DECLRMM:
    // the index's scroll suppression tests the column the cursor had before the
    // CR, so a NEL issued outside the left/right margins must not scroll. Its CR
    // component honors the left margin as the CR control (0x0D) does;
    // Ubiquitty::Screen::effectiveLeft is 0 when DECLRMM is off.
    linefeed(t);
    Ubiquitty::Screen::State* s = t.screens.active;
    s->cursorX = Screen::returnColumn(t);
    Screen::cursorClampOrigin(t);
    Screen::noteStringStart(t);
}

static void escapeH_hts(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    if (s->cursorX < 0 || s->cursorX >= t.columns)
        return;
    t.tabStops[s->cursorX] = true;
    if (!t.tabAlignments.empty()) {
        t.tabAlignments[s->cursorX] = CharacterTabAlignment::Plain;
        if (!t.tabAlignmentTargets.empty())
            t.tabAlignmentTargets[s->cursorX] = 0;
    }
}

void escapeH_vts(Ubiquitty::Terminal& t, char)
{
    t.lineTabStops[static_cast<std::size_t>(t.screens.active->cursorY)] = true;
}

static void escapeH_htj(Ubiquitty::Terminal& t, char) { Screen::justifyActiveField(t); }

static void escapeH_ris(Ubiquitty::Terminal& t, char) { Ubiquitty::hardReset(t); }

static void escapeH_deckpam(Ubiquitty::Terminal& t, char) { t.modes.appKeypad = true; }

static void escapeH_deckpnm(Ubiquitty::Terminal& t, char) { t.modes.appKeypad = false; }

static void escapeH_ss2(Ubiquitty::Terminal& t, char) { t.singleShift = 2; }

static void escapeH_ss3(Ubiquitty::Terminal& t, char) { t.singleShift = 3; }

// ECMA-35 locking shifts: LS2/LS3 invoke G2/G3 into GL; LS1R/LS2R/LS3R
// invoke G1/G2/G3 into GR. GL (characterSetActive) is consulted for every
// 0x20..0x7F byte; GR (characterSetGR) for every 0xA0..0xFF byte of a
// single-byte input encoding once a GR set is invoked (resolveGRByte). Under
// UTF-8 GR is recorded but inert. SI/SO are LS0/LS1.
static void escapeH_ls2(Ubiquitty::Terminal& t, char) { t.characterSetActive = 2; }

static void escapeH_ls3(Ubiquitty::Terminal& t, char) { t.characterSetActive = 3; }

static void escapeH_ls1r(Ubiquitty::Terminal& t, char) { t.characterSetGR = 1; }

static void escapeH_ls2r(Ubiquitty::Terminal& t, char) { t.characterSetGR = 2; }

static void escapeH_ls3r(Ubiquitty::Terminal& t, char) { t.characterSetGR = 3; }

static void escapeH_decid(Ubiquitty::Terminal& t, char)
{
    // DECID — VT100 alias for DA1. Suppress in VT52 mode (VT52 has
    // its own ESC Z → "\033/Z" identify in vt52EscDispatch).
    if (!t.modes.vt52Mode)
        emitPrimaryDA(t);
}

// HP memory lock protects the rows above the cursor by moving the top margin.
// Unlock restores the first row without changing the bottom margin or cursor.
static void escapeH_memoryLock(Ubiquitty::Terminal& t, char)
{
    auto& screen = *t.screens.active;
    if (screen.cursorY < screen.scrollBottom)
        screen.scrollTop = screen.cursorY;
}

static void escapeH_memoryUnlock(Ubiquitty::Terminal& t, char)
{
    t.screens.active->scrollTop = 0;
}

struct EscapeEntry {
    char final;
    void (*fn)(Ubiquitty::Terminal&, char);
};

// Sorted by final byte (ASCII).
static constexpr EscapeEntry escapeHandlers[] = {
    { '6', escapeH_decbi },
    { '7', escapeH_decsc },
    { '8', escapeH_decrc },
    { '9', escapeH_decfi },
    { '=', escapeH_deckpam },
    { '>', escapeH_deckpnm },
    { 'D', escapeH_ind },
    { 'E', escapeH_nel },
    { 'H', escapeH_hts },
    { 'I', escapeH_htj },
    { 'J', escapeH_vts },
    { 'M', escapeH_ri },
    { 'N', escapeH_ss2 },
    { 'O', escapeH_ss3 },
    { 'T', [](Terminal& t, char) { cancelPreviousGraphic(t); } },
    { 'V', escapeH_spa },
    { 'W', escapeH_epa },
    { 'Z', escapeH_decid },
    { 'c', escapeH_ris },
    { 'l', escapeH_memoryLock },
    { 'm', escapeH_memoryUnlock },
    { 'n', escapeH_ls2 },
    { 'o', escapeH_ls3 },
    { '|', escapeH_ls3r },
    { '}', escapeH_ls2r },
    { '~', escapeH_ls1r },
};

template<size_t N> constexpr bool escapeTableSortedStrict(const EscapeEntry (&a)[N], size_t i = 1) { return (i >= N) || ((static_cast<unsigned char>(a[i - 1].final) < static_cast<unsigned char>(a[i].final)) && escapeTableSortedStrict(a, i + 1)); }
static_assert(escapeTableSortedStrict(escapeHandlers), "escapeHandlers must be sorted by final byte");

static const EscapeEntry* escapeLookup(char final)
{
    int lo = 0, hi = static_cast<int>(sizeof(escapeHandlers) / sizeof(escapeHandlers[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (escapeHandlers[mid].final == final)
            return &escapeHandlers[mid];
        if (static_cast<unsigned char>(escapeHandlers[mid].final) < static_cast<unsigned char>(final))
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

void escDispatch(Ubiquitty::Terminal& t, char c)
{
    if (c != 'T')
        discardGraphicCancellation(t);
    const EscapeEntry* e = escapeLookup(c);
    if (e)
        e->fn(t, c);
    else
        // Unknown ESC byte: swallow silently (VT220 undefined-final behaviour),
        // emit a diag event for capture journals.
        Diagnostics::diagLog(t, "esc-miss", static_cast<uint32_t>(static_cast<uint8_t>(c)));
}

// VT52 single-byte ESC dispatch. Subset of the VT52 grammar that
// apps still emit (mostly the cursor / clear / address operations). Bytes
// outside this set are silently dropped.
void vt52EscapeDispatch(Ubiquitty::Terminal& t, uint8_t c)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    switch (c) {
    case 'A': // cursor up
        if (s->cursorY > 0)
            s->cursorY--;
        break;
    case 'B': // cursor down
        if (s->cursorY < t.rows - 1)
            s->cursorY++;
        break;
    case 'C': // cursor right
        if (s->cursorX < t.columns - 1)
            s->cursorX++;
        break;
    case 'D': // cursor left
        if (s->cursorX > 0)
            s->cursorX--;
        break;
    case 'H': // cursor home
        s->cursorX = 0;
        s->cursorY = 0;
        break;
    case 'I': // reverse line feed
        reverseIndex(t);
        break;
    case 'J': // clear from cursor to end of screen
        Ubiquitty::Screen::eraseInDisplay(t, 0);
        break;
    case 'K': // clear from cursor to end of line
        Ubiquitty::Screen::eraseInLine(t, 0);
        break;
    case 'Y': // cursor address — next two bytes are row + 0x20, column + 0x20
        t.parser.parseState = ParserState::VT52Y;
        return;
    case 'Z': // identify — host expects "ESC / Z"
        respond(t, Ubiquitty::Capability::DeviceAttributesRead, "\033/Z");
        break;
    case '=': // enter alternate keypad mode
        t.modes.appKeypad = true;
        break;
    case '>': // exit alternate keypad mode
        t.modes.appKeypad = false;
        break;
    case 'F': // enter graphics mode — print 0x60..0x7E as DEC special graphics
        t.modes.vt52Graphics = true;
        break;
    case 'G': // exit graphics mode
        t.modes.vt52Graphics = false;
        break;
    case '<': // return to ANSI mode (the only escape route from VT52)
        t.modes.vt52Mode = false;
        t.modes.vt52Graphics = false; // graphics mode does not survive the ANSI switch
        break;
    default:
        // Remaining VT52 escapes (keypad reports, etc.) are unmapped and
        // silently dropped.
        break;
    }
    // Return to the ground matching current mode: still VT52 → VT52Ground;
    // exited via '<' → Ground.
    t.parser.parseState = t.modes.vt52Mode ? ParserState::VT52Ground : ParserState::Ground;
}

// Parser seam: forward the table lookup as the public Handler the DFA calls.

} // namespace Ubiquitty::VT

namespace Ubiquitty::VT::Parser {
Handler findEscape(char final)
{
    const EscapeEntry* e = escapeLookup(final);
    return e ? e->fn : nullptr;
}
} // namespace Ubiquitty::VT::Parser
