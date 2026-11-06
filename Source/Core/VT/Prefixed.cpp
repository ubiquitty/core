// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Prefixed.cpp
//
// Prefixed-CSI final-byte handler bodies: kitty-keyboard query/push/pop/set,
// xterm modifyKeys, DECRQM, DECSCUSR/DECSCA/DECSCL/DECSACE, the DEC rectangle
// ops dispatch, DA2/DA3/XTVERSION, DECRQCRA, DECPS, DECSET/DECRST, DEC DSR,
// DECST8C, DECSEL/DECSED, XTSMGRAPHICS. Cold. The csiPrefixedHandlers table +
// findCSIPrefixed seam stay in VT.cpp; handlers are defined here.

#include "Core/CellGrid.h"
#include "Core/CellPen.h"
#include "Core/Cursor/RenderState.h"
#include "Core/Diagnostics/DiagLog.h"
#include "Core/EffectGate.h"
#include "Core/Keys/Keymap.h"
#include "Core/Kitty/KeyboardState.h"
#include "Core/ReplyBuilder.h"
#include "Core/SGR.h"
#include "Core/Screen/Margins.h"
#include "Core/Screen/Operations.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/DECPSSynth.h"
#include "Core/VT/Internal.h"
#include "PAL/PAL.h"
#include <cstdint>
#include <cstdio>
#include <utility>

namespace Ubiquitty::VT {

// kitty keyboard protocol (https://sw.kovidgoyal.net/kitty/keyboard-protocol/):
// CSI ? u (query), < u (pop), > u (push), = u (set).
// One handler per (priv, final); each reads its own parameters.
void csiH_kitty_query(Ubiquitty::Terminal& t, char) { respondCSIParameter(t, "?", t.kittyKeyboardFlags[screenIndex(t)], -1, 'u'); }

void csiH_kitty_pop(Ubiquitty::Terminal& t, char) { Ubiquitty::Kitty::popFlags(t, parameterOr(t, 0, 1)); }

void csiH_kitty_push(Ubiquitty::Terminal& t, char) { Ubiquitty::Kitty::pushFlags(t, parameterOr(t, 0, 0)); }

void csiH_kitty_set(Ubiquitty::Terminal& t, char) { Ubiquitty::Kitty::setFlags(t, parameterOr(t, 0, 0), parameterOr(t, 1, 1)); }

// xterm key modifier options: XTMODKEYS (CSI > Pp ; Pv m) and XTFMTKEYS
// (CSI > Pp ; Pv f) set them, XTQMODKEYS (CSI ? Pp m) and XTQFMTKEYS
// (CSI ? Pp g) report them, and CSI > Pp n disables one.
//
// Category 5 (string macros) has no binding and stays off. Unknown resources
// report DEFAULT as 65535.
enum class KeyOptionTable : uint8_t {
    Modify,
    Format,
};

// A resource's initial value, the one RIS and DECSTR leave behind.
static uint8_t keyOptionInitialValue(KeyOptionTable table, long long resource)
{
    return table == KeyOptionTable::Modify ? VT::xtermKeyModifierInitialValues[static_cast<std::size_t>(resource)] : 0;
}

// The largest value the encoder gives its own encoding to, or -1 for a resource
// it does not read. modifyOtherKeys runs to 3, which wraps every printable key;
// formatOtherKeys chooses between CSI 27 ; m ; c ~ and CSI c ; m u.
static int keyOptionMaximumValue(KeyOptionTable table, long long resource)
{
    if (table == KeyOptionTable::Format)
        return resource == 4 || resource == 6 || resource == 7 ? 1 : -1;
    if (resource == 0)
        return 15;
    if (resource == 6 || resource == 7)
        return 4;
    return (resource >= 1 && resource <= 4) ? 3 : -1;
}

static uint8_t* keyOptionSlot(Ubiquitty::Terminal& t, KeyOptionTable table, long long resource)
{
    if (keyOptionMaximumValue(table, resource) < 0)
        return nullptr;
    const std::size_t index = static_cast<std::size_t>(resource);
    return table == KeyOptionTable::Modify ? &t.xtermKeyModifiers[index] : &t.xtermKeyFormats[index];
}

// An omitted resource is not resource 0: CSI > 0 ; 1 m names modifyKeyboard,
// so the resource is read with parameterOmitted rather than parameterOr.
static void setKeyOption(Ubiquitty::Terminal& t, KeyOptionTable table)
{
    if (t.parser.parameterCount == 0) {
        if (table == KeyOptionTable::Modify)
            t.xtermKeyModifiers = VT::xtermKeyModifierInitialValues;
        else
            t.xtermKeyFormats.fill(0);
        return;
    }
    if (parameterOmitted(t, 0))
        return;
    const long long resource = t.parser.parameters[0];
    uint8_t* slot = keyOptionSlot(t, table, resource);
    if (!slot)
        return;
    if (parameterOmitted(t, 1)) {
        *slot = keyOptionInitialValue(table, resource);
        return;
    }
    const long long value = t.parser.parameters[1];
    if (value < 0 || value > keyOptionMaximumValue(table, resource))
        return;
    *slot = static_cast<uint8_t>(value);
}

// One reply per resource, formatted as the control that restores it. An
// omitted or unknown resource reports DEFAULT, serialized by xterm as 65535.
static void reportKeyOptions(Ubiquitty::Terminal& t, KeyOptionTable table, char final)
{
    constexpr int noValue = 65535;
    for (int i = 0; i < std::max(1, t.parser.parameterCount); i++) {
        const int resource = parameterOmitted(t, i) ? noValue : static_cast<int>(t.parser.parameters[i]);
        int value = noValue;
        if (resource >= 0 && resource < static_cast<int>(VT::xtermKeyModifierCategoryCount)) {
            value = 0;
            if (const uint8_t* slot = keyOptionSlot(t, table, resource))
                value = *slot == Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled ? noValue : *slot;
        }
        respondCSIParameter(t, ">", resource, value, final);
    }
}

void csiH_xtmod_query_m(Ubiquitty::Terminal& t, char) { reportKeyOptions(t, KeyOptionTable::Modify, 'm'); }

// XTQFMTKEYS accepts the documented `CSI ? Pp g` and the `CSI > Pp g`
// implemented by xterm-411a. Unknown resources report DEFAULT (65535).
void csiH_xtfmt_query_g(Ubiquitty::Terminal& t, char)
{
    constexpr int lastFormatResource = 7;
    constexpr int noValue = 65535;
    for (int i = 0; i < t.parser.parameterCount; i++) {
        const int resource = parameterOr(t, i, 0);
        int value = noValue;
        if (resource >= 0 && resource <= lastFormatResource)
            value = t.xtermKeyFormats[static_cast<std::size_t>(resource)];
        respondCSIParameter(t, ">", resource, value, 'f');
    }
}

// Each saved slot includes the live indexed palette, every dynamic OSC colour
// and kitty's cursor_text, which the slot keeps beside them.
static constexpr std::array<uint16_t Ubiquitty::Terminal::*, 21> paletteChannels {
    &Ubiquitty::Terminal::selectionBackgroundRed,
    &Ubiquitty::Terminal::selectionBackgroundGreen,
    &Ubiquitty::Terminal::selectionBackgroundBlue,
    &Ubiquitty::Terminal::selectionForegroundRed,
    &Ubiquitty::Terminal::selectionForegroundGreen,
    &Ubiquitty::Terminal::selectionForegroundBlue,
    &Ubiquitty::Terminal::mousePointerForegroundRed,
    &Ubiquitty::Terminal::mousePointerForegroundGreen,
    &Ubiquitty::Terminal::mousePointerForegroundBlue,
    &Ubiquitty::Terminal::mousePointerBackgroundRed,
    &Ubiquitty::Terminal::mousePointerBackgroundGreen,
    &Ubiquitty::Terminal::mousePointerBackgroundBlue,
    &Ubiquitty::Terminal::tektronixForegroundRed,
    &Ubiquitty::Terminal::tektronixForegroundGreen,
    &Ubiquitty::Terminal::tektronixForegroundBlue,
    &Ubiquitty::Terminal::tektronixBackgroundRed,
    &Ubiquitty::Terminal::tektronixBackgroundGreen,
    &Ubiquitty::Terminal::tektronixBackgroundBlue,
    &Ubiquitty::Terminal::tektronixCursorRed,
    &Ubiquitty::Terminal::tektronixCursorGreen,
    &Ubiquitty::Terminal::tektronixCursorBlue,
};
static constexpr std::array<bool Ubiquitty::Terminal::*, 7> paletteSetFlags {
    &Ubiquitty::Terminal::selectionBackgroundSet,
    &Ubiquitty::Terminal::selectionForegroundSet,
    &Ubiquitty::Terminal::mousePointerForegroundSet,
    &Ubiquitty::Terminal::mousePointerBackgroundSet,
    &Ubiquitty::Terminal::tektronixForegroundSet,
    &Ubiquitty::Terminal::tektronixBackgroundSet,
    &Ubiquitty::Terminal::tektronixCursorSet,
};

void pushColors(Ubiquitty::Terminal& t, int parameter)
{
    if (!t.allowColorOperations || !Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetColor))
        return;
    const int slot = parameter > 0 ? parameter - 1 : t.savedPalettePosition;
    if (slot < 0 || slot >= static_cast<int>(Ubiquitty::Terminal::paletteStackDepth))
        return;
    if (!t.savedPalettes) {
        t.savedPalettes.reset(new (std::nothrow) Ubiquitty::Terminal::PaletteStack {});
        if (!t.savedPalettes)
            return;
    }
    auto& saved = (*t.savedPalettes)[static_cast<size_t>(slot)];
    saved.palette = t.palette;
    for (size_t channel = 0; channel < std::size(paletteChannels); ++channel)
        saved.dynamicChannels[channel] = t.*paletteChannels[channel];
    for (size_t flag = 0; flag < std::size(paletteSetFlags); ++flag)
        saved.dynamicSet[flag] = t.*paletteSetFlags[flag];
    saved.cursorTextColor = { t.cursorTextRed, t.cursorTextGreen, t.cursorTextBlue };
    saved.hasCursorTextColor = t.hasCursorTextColor;
    t.savedPaletteValid |= static_cast<uint16_t>(1u << slot);
    if (parameter == 0) {
        t.savedPalettePosition = static_cast<uint8_t>(slot + 1);
        t.savedPaletteCount = std::max(t.savedPaletteCount, t.savedPalettePosition);
    }
}

void popColors(Ubiquitty::Terminal& t, int parameter)
{
    if (!t.allowColorOperations || !Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetColor))
        return;
    const int slot = parameter > 0 ? parameter - 1 : static_cast<int>(t.savedPalettePosition) - 1;
    if (slot < 0 || slot >= static_cast<int>(Ubiquitty::Terminal::paletteStackDepth) || !(t.savedPaletteValid & (1u << slot)))
        return;
    const auto& saved = (*t.savedPalettes)[static_cast<size_t>(slot)];
    bool hasChanged = t.palette != saved.palette;
    t.palette = saved.palette;
    for (size_t channel = 0; channel < std::size(paletteChannels); ++channel) {
        hasChanged |= t.*paletteChannels[channel] != saved.dynamicChannels[channel];
        t.*paletteChannels[channel] = saved.dynamicChannels[channel];
    }
    for (size_t flag = 0; flag < std::size(paletteSetFlags); ++flag) {
        hasChanged |= t.*paletteSetFlags[flag] != saved.dynamicSet[flag];
        t.*paletteSetFlags[flag] = saved.dynamicSet[flag];
    }
    const Ubiquitty::PaletteRGB cursorText { t.cursorTextRed, t.cursorTextGreen, t.cursorTextBlue };
    hasChanged |= cursorText != saved.cursorTextColor || t.hasCursorTextColor != saved.hasCursorTextColor;
    t.cursorTextRed = saved.cursorTextColor.red;
    t.cursorTextGreen = saved.cursorTextColor.green;
    t.cursorTextBlue = saved.cursorTextColor.blue;
    t.hasCursorTextColor = saved.hasCursorTextColor;
    if (hasChanged)
        ++t.paletteEpoch;
    if (parameter == 0)
        t.savedPalettePosition = static_cast<uint8_t>(slot);
}

void csiH_xtpushcolors(Ubiquitty::Terminal& t, char)
{
    for (int index = 0; index < std::max(1, t.parser.parameterCount); ++index)
        pushColors(t, parameterOr(t, index, 0));
}

void csiH_xtpopcolors(Ubiquitty::Terminal& t, char)
{
    for (int index = 0; index < std::max(1, t.parser.parameterCount); ++index)
        popColors(t, parameterOr(t, index, 0));
}

void csiH_xtreportcolors(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[?");
    reply.putDecimal(t.savedPalettePosition);
    reply.put(';');
    reply.putDecimal(t.savedPaletteCount);
    reply.put("#Q");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::StatusReport, reply.span());
}

void csiH_xtmod_set_m(Ubiquitty::Terminal& t, char) { setKeyOption(t, KeyOptionTable::Modify); }

void csiH_xtmod_set_f(Ubiquitty::Terminal& t, char) { setKeyOption(t, KeyOptionTable::Format); }

// CSI > Pp n disables a resource, modifyFunctionKeys when none is named (xterm
// CASE_SET_MOD_FKEYS0). Disabled cursor and function keys send what the
// unmodified key sends. Keypad and other-key modification is off as well,
// but retains the disabled value for queries and restoration.
void csiH_xtmod_clear_n(Ubiquitty::Terminal& t, char)
{
    const long long resource = parameterOmitted(t, 0) ? 2 : t.parser.parameters[0];
    if (resource >= 0) {
        if (uint8_t* slot = keyOptionSlot(t, KeyOptionTable::Modify, resource))
            *slot = static_cast<uint8_t>(Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled);
    }
}

void csiH_decrqm_ansi(Ubiquitty::Terminal& t, char)
{
    // DECRQM arrived with the VT300; at a lower operating level the request is
    // not recognised, so nothing is emitted.
    if (t.conformanceLevel < VT::ConformanceLevel::VT300)
        return;
    const int ps = parameterOr(t, 0, 0);
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    reply.putDecimal(ps);
    reply.put(';');
    reply.putDecimal(decrqmANSIState(t, ps));
    reply.put("$y");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::ModeStateRead, reply.span());
}

void csiH_decrqm_dec(Ubiquitty::Terminal& t, char)
{
    // DECRQM arrived with the VT300; at a lower operating level the request is
    // not recognised, so nothing is emitted.
    if (t.conformanceLevel < VT::ConformanceLevel::VT300)
        return;
    const int ps = parameterOr(t, 0, 0);
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[?");
    reply.putDecimal(ps);
    reply.put(';');
    reply.putDecimal(decrqmDECState(t, ps));
    reply.put("$y");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::ModeStateRead, reply.span());
}

// xterm title modes. Ps selects which label encoding the bit governs. With no
// parameter nothing changes: the omitted parameter is DEFAULT, which names no
// mode (xterm charproc.c, CASE_SM_TITLE and CASE_RM_TITLE act only when
// nparam >= 1).
static void applyTitleModes(Ubiquitty::Terminal& t, Ubiquitty::Modes::ModeChange change)
{
    const bool isEnabled = (change == Ubiquitty::Modes::ModeChange::Set);
    for (int i = 0; i < t.parser.parameterCount; i++) {
        if (parameterOmitted(t, i))
            continue;
        const int ps = t.parser.parameters[i];
        if (ps < 0 || ps > 3)
            continue;
        const uint8_t bit = static_cast<uint8_t>(1u << ps);
        t.titleModes = isEnabled ? static_cast<uint8_t>(t.titleModes | bit) : static_cast<uint8_t>(t.titleModes & ~bit);
    }
}

void csiH_xtsave(Ubiquitty::Terminal& t, char)
{
    for (int i = 0; i < t.parser.parameterCount; i++)
        savePrivateMode(t, t.parser.parameters[i]);
}

void csiH_xtrestore(Ubiquitty::Terminal& t, char)
{
    for (int i = 0; i < t.parser.parameterCount; i++)
        restorePrivateMode(t, t.parser.parameters[i]);
}

void csiH_settitlemode(Ubiquitty::Terminal& t, char) { applyTitleModes(t, Ubiquitty::Modes::ModeChange::Set); }
void csiH_resettitlemode(Ubiquitty::Terminal& t, char) { applyTitleModes(t, Ubiquitty::Modes::ModeChange::Clear); }

void csiH_decscusr(Ubiquitty::Terminal& t, char)
{
    // SetCursorShape-gated (LowRiskInfo: a host that treats a remote cursor
    // reshape as display-spoofing restricts the tier). An unset policy answers
    // PAL's default.
    if (!Ubiquitty::effectAllowed(t, Ubiquitty::Capability::SetCursorShape))
        return; // host policy denied the cursor-shape write — skip it

    const int parameter = parameterOr(t, 0, 0);
    if (parameter > 7)
        return;
    // VT510 defines 0 and the omitted parameter as blinking block. xterm's
    // extension 7 restores the user's configured cursor shape and blink.
    t.cursorStyle = parameter == 7 ? Ubiquitty::Cursor::Style::FollowUserPreference
                                   : Ubiquitty::Cursor::styleFromCode(parameter == 0 ? 1 : parameter);
    const auto style = parameter == 7 ? Ubiquitty::Cursor::effectiveStyleForUserPreference(t) : t.cursorStyle;
    t.cursorBlinkAtt610 = Ubiquitty::Cursor::isBlinking(style);
}

void csiH_decsace(Ubiquitty::Terminal& t, char)
{
    int p = parameterOr(t, 0, 1);
    if (p == 2)
        t.modes.sace = 2;
    else
        t.modes.sace = 1;
}

// DECSCL (CSI Pl ; Pc " p) — Set Conformance Level.
// Pl: 61=VT100, 62=VT200, 63=VT300, 64=VT400, 65=VT500.
// Pc: C1 transmission. 0/2 = 8-bit controls allowed, 1 = 7-bit only.
// VT100 level (61) forces 7-bit regardless of Pc.
// An unrecognised Pl leaves the level unchanged. Pc is only honoured
// for VT200+ (the VT100 has no 8-bit transmission).
void csiH_decscl(Ubiquitty::Terminal& t, char)
{
    // Pl names the level and has no default: left out or 0, it names none and
    // the request is refused, with no reset (xterm charproc.c CASE_DECSCL).
    const int pl = (t.parser.parameterCount > 0) ? t.parser.parameters[0] : 0;
    if (pl < std::to_underlying(VT::ConformanceLevel::VT100) || pl > std::to_underlying(VT::ConformanceLevel::VT500))
        return;
    // Per DEC STD 070: selecting a conformance level performs a soft reset
    // (DECSTR semantics; cell contents survive) before the requested level and
    // C1 policy apply.
    softResetInternal(t);
    t.conformanceLevel = static_cast<VT::ConformanceLevel>(pl);
    if (pl == std::to_underlying(VT::ConformanceLevel::VT100)) {
        // VT100 has no 8-bit C1 transmission.
        t.transmit8bitC1 = false;
        return;
    }
    // Pc defaults to 8-bit; 0 also means 8-bit; only Pc=1 selects 7-bit.
    int pc = (t.parser.parameterCount > 1) ? t.parser.parameters[1] : 0;
    t.transmit8bitC1 = (pc != 1);
}

// VT400+ operations (rectangular-area editing, DECSLRM) are inert below VT400.
static inline bool levelAllowsVT400(const Ubiquitty::Terminal& t) { return t.conformanceLevel >= VT::ConformanceLevel::VT400; }

void csiH_decsmkr(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t) || !t.modes.shouldReportKeyPositions)
        return;
    for (int index = 0; index < t.parser.parameterCount; index += 2) {
        const int number = t.parser.parameters[index];
        const int action = index + 1 < t.parser.parameterCount ? t.parser.parameters[index + 1] : 0;
        if (number < 0 || number > 8 || action < 0 || action > 3)
            continue;
        const std::uint8_t mask = number ? static_cast<std::uint8_t>(1u << (number - 1)) : 0xff;
        t.keyPositionReportedModifiers &= ~mask;
        t.keyPositionDisabledModifiers &= ~mask;
        if (!action)
            t.keyPositionReportedModifiers |= mask & 0x30;
        else if (action == 2)
            t.keyPositionReportedModifiers |= mask;
        else if (action == 3)
            t.keyPositionDisabledModifiers |= mask;
    }
}

// DECSNLS (CSI Ps * |) — lines per screen. Same page-memory resize as DECSLPP,
// which is why the DECRQSS report for both reads back t->rows.
// DECSR — CSI Pn + p. DEC STD 070 resets the terminal to its power-up state and
// erases every host-settable state and buffer, the user-defined keys even when
// locked, which RIS's reconstruction of the Terminal already does. A Pn from 1
// to 16383 asks for the confirmation DECSRC, CSI Pn * q, once the reset is
// done; 0 or none asks for nothing, and a Pn past 16383 voids the whole
// sequence (VT510 DECSR). xterm ignores DECSR (VTPrsTbl.c).
void csiH_decsr(Ubiquitty::Terminal& t, char)
{
    const int confirmation = parameterOr(t, 0, 0);
    if (confirmation > 16383)
        return;
    Ubiquitty::hardReset(t);
    if (confirmation <= 0)
        return;
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    reply.putDecimal(confirmation);
    reply.put("*q");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::StatusReport, reply.span());
}

void csiH_decsnls(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    int lines = parameterOr(t, 0, 0);
    if (lines < 1)
        return;
    if (lines > Ubiquitty::maxRows)
        lines = Ubiquitty::maxRows;
    if (lines != t.rows)
        Ubiquitty::resize(t, lines, t.columns);
}

// VT510 DECSCPP changes page width without clearing the page, homing the
// cursor or resetting margins. Cropped columns are lost, never reflowed.
void csiH_decscpp(Ubiquitty::Terminal& t, char)
{
    if (t.conformanceLevel < VT::ConformanceLevel::VT300)
        return;
    const int columns = parameterOr(t, 0, 80);
    if (columns != 80 && columns != 132)
        return;
    if (columns < t.columns && t.grid) {
        // Fixed window resizing retains off-screen cells for a later grow.
        // DECSCPP instead loses them on every page, including the inactive one.
        const Ubiquitty::Cell blank {};
        for (int row = 0; row < t.rows; ++row) {
            const auto firstColumn = [&](Ubiquitty::ScreenMode screen) {
                const auto* first = t.grid->cellAt(screen, Ubiquitty::Row(row), Ubiquitty::Column(columns));
                return first && Ubiquitty::cellPen(*first).attributes.contains(VT::Attribute::WideContinuation) ? columns - 1 : columns;
            };
            const bool isAlternate = t.screens.active == &t.screens.alternate;
            t.grid->eraseRowRange(Ubiquitty::Row(row), Ubiquitty::Column(firstColumn(isAlternate ? Ubiquitty::ScreenMode::Alternate : Ubiquitty::ScreenMode::Primary)), Ubiquitty::Column(t.columns - 1), blank);
            t.grid->eraseRowRangeInactive(Ubiquitty::Row(row), Ubiquitty::Column(firstColumn(isAlternate ? Ubiquitty::ScreenMode::Primary : Ubiquitty::ScreenMode::Alternate)), Ubiquitty::Column(t.columns - 1), blank);
        }
    }
    if (columns != t.columns)
        Ubiquitty::resize(t, t.rows, columns, Ubiquitty::Modes::ReflowMode::Fixed);
}

// Rectangle coordinates for the whole DEC rect family (DECFRA/ERA/SERA/RARA/
// CARA/CRA/RQCRA). Two rules they all share:
//
//   * DECOM makes Pt/Pl relative to the scrolling region's origin, and the
//     region — not the page — becomes the addressable extent. Columns shift
//     only when DECSLRM margins are actually in effect, the same condition
//     DECXCPR reports a column under.
//   * An out-of-order rectangle (bottom above top, right left of left) is not
//     clamped into a 1x1 sliver: the operation is skipped whole. Clamping the
//     far edges down to the page happens first, so an oversized-but-ordered
//     rectangle still acts on its visible part.
//
// Returns false when the caller must do nothing; `out` is 0-based on success.
static bool decRectFromParameters(Ubiquitty::Terminal& t, int firstIndex, Ubiquitty::Screen::RectArea& out)
{
    const Ubiquitty::Screen::State* s = t.screens.active;
    int top = parameterOr(t, firstIndex + 0, 1);
    int left = parameterOr(t, firstIndex + 1, 1);
    int bot = parameterOr(t, firstIndex + 2, t.rows);
    int right = parameterOr(t, firstIndex + 3, t.columns);
    int rowLimit = t.rows;
    int columnLimit = t.columns;
    if (t.modes.originMode) {
        top += s->scrollTop;
        bot += s->scrollTop;
        rowLimit = s->scrollBottom + 1;
        if (t.modes.leftRightMarginMode) {
            left += s->scrollLeft;
            right += s->scrollLeft;
            columnLimit = s->scrollRight + 1;
        }
    }
    if (bot > rowLimit)
        bot = rowLimit;
    if (right > columnLimit)
        right = columnLimit;
    if (top > bot || left > right)
        return false;
    out = { top - 1, left - 1, bot - 1, right - 1 };
    return true;
}

void csiH_decfra(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    int ch = parameterOr(t, 0, 0x20);
    // DEC STD 070: Pch is a graphic in 32–126 or 160–255; outside those DECFRA
    // is a no-op (not substituted with a space). GR 0xA0–0xFF are Latin-1,
    // equal to U+00A0–U+00FF.
    if (!((ch >= 0x20 && ch <= 0x7E) || (ch >= 0xA0 && ch <= 0xFF)))
        return;
    Ubiquitty::Screen::RectArea r {};
    if (!decRectFromParameters(t, /*firstIndex*/ 1, r))
        return;
    // The code names a character of the set invoked in GL or GR (VT510
    // DECFRA), so it resolves as that character would print.
    Ubiquitty::Screen::rectFill(t, r, resolveGraphicCharacter(t, ch));
}

void csiH_decera(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::RectArea r {};
    if (!decRectFromParameters(t, /*firstIndex*/ 0, r))
        return;
    Ubiquitty::Screen::rectErase(t, r);
}

void csiH_decsera(Ubiquitty::Terminal& t, char)
{
    // DECSERA — selective erase rect; respects DECSCA-protected cells.
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::RectArea r {};
    if (!decRectFromParameters(t, /*firstIndex*/ 0, r))
        return;
    Ubiquitty::Screen::rectSelectiveErase(t, r);
}

// DECIC (CSI Ps ' }) / DECDC (CSI Ps ' ~) — insert/delete Ps blank columns at
// the cursor column, shifting the scroll region's rectangle. VT420 features,
// gated like the rectangle ops; the margin no-op rules live in the grid bodies.
void csiH_decic(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::insertColumns(t, parameterOr(t, 0, 1));
}

void csiH_decdc(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::deleteColumns(t, parameterOr(t, 0, 1));
}

// The rendition attributes DECCARA and DECRARA may change: the VT420's four,
// and Hidden (8 and 28), which xterm adds (ctlseqs: "Setting SGR 8 is an xterm
// extension"). Their Ps=0 selector means the four, not Hidden. Two things
// downstream rely on no other attribute being reachable: Screen/Rect.cpp
// scrubs an orphaned wide half without consulting Attribute::Protected, which
// is safe only while no control can rewrite half a pair's protection; and the
// wide flags themselves survive a rectangle whose edge splits a pair. Adding
// an attribute here is a change to both, not to a list.
static constexpr VT::AttributeSet rectangleRenditionAttributes = { VT::Attribute::Bold, VT::Attribute::Underline, VT::Attribute::Blink, VT::Attribute::Reverse };

void csiH_decrara(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::RectArea r {};
    if (!decRectFromParameters(t, /*firstIndex*/ 0, r))
        return;
    // SGR-like selectors after the four rect params choose which of
    // bold/underline/blink/reverse to reverse: 1/4/5/7 name one; 0 or none = all
    // four. DECRARA is pure reversal, so the set/clear forms (22/24/25/27) don't
    // apply. The selectors execute cumulatively in sequence (VT510 DECRARA), so
    // each reverses the attributes it names on top of the ones before it and a
    // repeated selector cancels.
    VT::AttributeSet toggleMask = {};
    bool sawSelector = false;
    for (int i = 4; i < t.parser.parameterCount; i++) {
        sawSelector = true;
        switch (t.parser.parameters[i]) {
        case 0:
            toggleMask.toggle(rectangleRenditionAttributes);
            break;
        case 1:
            toggleMask.toggle(VT::Attribute::Bold);
            break;
        case 4:
            toggleMask.toggle(VT::Attribute::Underline);
            break;
        case 5:
            toggleMask.toggle(VT::Attribute::Blink);
            break;
        case 7:
            toggleMask.toggle(VT::Attribute::Reverse);
            break;
        case 8:
            toggleMask.toggle(VT::Attribute::Hidden);
            break;
        default:
            break;
        }
    }
    if (!sawSelector)
        toggleMask = rectangleRenditionAttributes; // DECRARA default: reverse all four
    Ubiquitty::Screen::rectReverseAttributes(t, r, toggleMask);
}

void csiH_deccara(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::RectArea r {};
    if (!decRectFromParameters(t, /*firstIndex*/ 0, r))
        return;
    // The SGR-like selectors after the four rect params choose which of
    // bold/underline/blink/reverse to set (1/4/5/7) or clear (0/22/24/25/27).
    VT::AttributeSet setMask = {};
    VT::AttributeSet clearMask = {};
    bool sawSelector = false;
    for (int i = 4; i < t.parser.parameterCount; i++) {
        sawSelector = true;
        switch (t.parser.parameters[i]) {
        case 0:
            setMask = {};
            clearMask = rectangleRenditionAttributes;
            break;
        case 1:
            setMask.add(VT::Attribute::Bold);
            clearMask.remove(VT::Attribute::Bold);
            break;
        case 4:
            setMask.add(VT::Attribute::Underline);
            clearMask.remove(VT::Attribute::Underline);
            break;
        case 5:
            setMask.add(VT::Attribute::Blink);
            clearMask.remove(VT::Attribute::Blink);
            break;
        case 7:
            setMask.add(VT::Attribute::Reverse);
            clearMask.remove(VT::Attribute::Reverse);
            break;
        case 8:
            setMask.add(VT::Attribute::Hidden);
            clearMask.remove(VT::Attribute::Hidden);
            break;
        case 22:
            clearMask.add(VT::Attribute::Bold);
            setMask.remove(VT::Attribute::Bold);
            break;
        case 24:
            clearMask.add(VT::Attribute::Underline);
            setMask.remove(VT::Attribute::Underline);
            break;
        case 25:
            clearMask.add(VT::Attribute::Blink);
            setMask.remove(VT::Attribute::Blink);
            break;
        case 27:
            clearMask.add(VT::Attribute::Reverse);
            setMask.remove(VT::Attribute::Reverse);
            break;
        case 28:
            clearMask.add(VT::Attribute::Hidden);
            setMask.remove(VT::Attribute::Hidden);
            break;
        default:
            break;
        }
    }
    // DECCARA's default selector (none supplied) is 0 — all four off.
    if (!sawSelector)
        clearMask = rectangleRenditionAttributes;
    Ubiquitty::Screen::rectChangeAttributes(t, r, setMask.toRaw(), clearMask.toRaw());
}

void csiH_deccra(Ubiquitty::Terminal& t, char)
{
    if (!levelAllowsVT400(t))
        return;
    Ubiquitty::Screen::RectArea src {};
    if (!decRectFromParameters(t, /*firstIndex*/ 0, src))
        return;
    // The destination corner is origin-relative under DECOM exactly like the
    // source corners; only Ubiquitty::Screen::rectCopy's own page clipping bounds it from there.
    const Ubiquitty::Screen::State* s = t.screens.active;
    int dt = parameterOr(t, 5, 1);
    int dl = parameterOr(t, 6, 1);
    if (t.modes.originMode) {
        dt += s->scrollTop;
        if (t.modes.leftRightMarginMode)
            dl += s->scrollLeft;
    }
    Ubiquitty::Screen::rectCopy(t, src, dt, dl);
}

// DECRQPSR (CSI Ps $ w) — Request Presentation State Report. Ps=1 → DECCIR
// (cursor info), Ps=2 → DECTABSR (tab stops). DECTABSR reply: DCS 2 $ u
// <col>/<col>/... ST (1-based columns with tab stops, ascending, '/'-separated;
// empty body when none set).
// Map a Core characterSet enum to its DEC SCS designation final byte. Used by DECCIR.
static char characterSetDesignationFinal(int cs)
{
    switch (cs) {
    case characterSetDECSpecial:
        return '0';
    case characterSetUK:
        return 'A';
    case characterSetFrench:
        return 'R';
    case characterSetGerman:
        return 'K';
    case characterSetItalian:
        return 'Y';
    case characterSetNorwegianDanish:
        return 'E';
    case characterSetSwedish:
        return 'H';
    case characterSetFinnish:
        return 'C';
    case characterSetDutch:
        return '4';
    case characterSetSwiss:
        return '=';
    case characterSetSpanish:
        return 'Z';
    case characterSetFrenchCanadian:
        return 'Q';
    case characterSetLatin1:
        return 'A'; // 96-set final (preceded by a 96-set intermediate)
    case characterSetLatin2:
        return 'B';
    case characterSetGreek:
        return 'F';
    case characterSetHebrew:
        return 'H';
    case characterSetLatinCyrillic:
        return 'L';
    case characterSetLatin5:
        return 'M';
    case characterSetDECSupplemental:
        return '<';
    case characterSetDECTechnical:
        return '>';
    case characterSetUSASCII:
    default:
        return 'B';
    }
}

// DECRQDE (CSI " v) — Request Displayed Extent. Reply DECRPDE:
// CSI Ph ; Pw ; Pc ; Pr ; Pp " w (lines, columns, left column, top row,
// page). The whole page is displayed, anchored at column 1 / row 1, page 1.
void csiH_decrqde(Ubiquitty::Terminal& t, char)
{
    // A VT300 control: below that operating level it is not answered, as in
    // xterm (charproc.c CASE_DECRQDE).
    if (t.conformanceLevel < VT::ConformanceLevel::VT300)
        return;
    std::array<char, 48> buffer {};
    std::snprintf(buffer.data(), buffer.size(), "\033[%d;%d;1;1;1\"w", t.rows, t.columns);
    respond(t, Ubiquitty::Capability::TerminalSizeRead, buffer.data());
}

// DECRQUPSS (CSI & u) — Request User-Preferred Supplemental Set. Reply
// DECAUPSS: DCS Ps ! u D...D ST, naming the set a program assigned with
// DECAUPSS, or ISO Latin-1 (Ps=1, a 96-char set, final "A"), the set a Unicode
// terminal resolves GR against, until it does.
void csiH_decrqupss(Ubiquitty::Terminal& t, char)
{
    const bool isDECSupplemental = t.modes.userPreferredSupplementalSet == VT::SupplementalSet::DECSupplemental;
    respond(t, Ubiquitty::Capability::StatusReport, isDECSupplemental ? "\033P0!u%5\033\\" : "\033P1!uA\033\\");
}

// The widest DECTABSR reply a grid of `maxColumns` can produce, in bytes.
//
// Every column carries a stop, so each contributes its column number in decimal
// and the slash separating it from the one before — counted for the first stop
// too, which has none, so the answer is an over-estimate by one byte rather
// than a coincidence to be rechecked. The introducer, the terminator and
// snprintf's terminating null are the fixed part.
constexpr std::size_t dectabsrWidestReplyInBytes()
{
    std::size_t bytes = sizeof("\033P2$u") - 1 + sizeof("\033\\") - 1 + 1;
    for (int column = 1; column <= Ubiquitty::maxColumns; ++column) {
        std::size_t digits = 0;
        for (int value = column; value > 0; value /= 10)
            ++digits;
        bytes += digits + 1;
    }
    return bytes;
}

void csiH_decrqpsr(Ubiquitty::Terminal& t, char)
{
    int ps = parameterOr(t, 0, 0);
    if (ps == 1) {
        // DECCIR — Cursor Information Report. Reply:
        // DCS 1 $ u Pr;Pc;Pp;Srend;Satt;Sflag;Pgl;Pgr;Scss;Sdesig ST
        // The single-byte fields are 0x40-based bitmasks; Sdesig lists the
        // SCS designation finals for G0..G3.
        Ubiquitty::Screen::State* s = t.screens.active;
        const Pen* pen = &s->pen;
        const VT::AttributeSet a = pen->attributes;
        char srend = static_cast<char>(0x40 | (a.contains(VT::Attribute::Bold) ? 0x01 : 0) | (a.contains(VT::Attribute::Underline) ? 0x02 : 0) | (a.contains(VT::Attribute::Blink) ? 0x04 : 0) | (a.contains(VT::Attribute::Reverse) ? 0x08 : 0));
        char satt = static_cast<char>(0x40 | (a.contains(VT::Attribute::Protected) ? 0x01 : 0));
        // Sflag as the VT510 lays it out: origin mode, SS2, SS3, then the
        // pending autowrap, from the lowest bit up.
        char sflag = static_cast<char>(0x40 | (t.modes.originMode ? 0x01 : 0) | ((t.singleShift == 2) ? 0x02 : 0) | ((t.singleShift == 3) ? 0x04 : 0) | (s->wrapPending ? 0x08 : 0));
        // Pgr is the set invoked into GR. With no locking shift invoked, GR
        // holds G2, its power-up and reset invocation (DEC STD 070, 3.3; VT510
        // DECCIR).
        const int invokedGR = t.characterSetGR != 0 ? t.characterSetGR : 2;
        // Scss marks each set designated as a 96-character set, G0 in the
        // lowest bit (VT510 DECCIR), so its Sdesig final reads as the 96-set's.
        char scss = 0x40;
        for (std::size_t slot = 0; slot < t.characterSetG.size(); ++slot)
            if (isCharacterSet96(t.characterSetG[slot]) && t.characterSetG[slot] != characterSetDECSupplemental)
                scss = static_cast<char>(scss | (1 << slot));
        char buffer[80];
        std::snprintf(buffer, sizeof(buffer), "\033P1$u%d;%d;1;%c;%c;%c;%d;%d;%c;%c%c%c%c\033\\", s->cursorY + 1, s->cursorX + 1, srend, satt, sflag, t.characterSetActive, invokedGR, scss, characterSetDesignationFinal(t.characterSetG[0]), characterSetDesignationFinal(t.characterSetG[1]),
            characterSetDesignationFinal(t.characterSetG[2]), characterSetDesignationFinal(t.characterSetG[3]));
        respond(t, Ubiquitty::Capability::CursorStateRead, buffer);
        return;
    }
    if (ps != 2)
        return; // unknown selectors: no reply.
    // The whole reply is formatted before anyone knows its length, so this
    // buffer holds the widest one this grid can name even though `respond`
    // delivers at most `outputBuffer.size()` and drops the rest. `off`
    // accumulates what snprintf would have written rather than what it did, so
    // it is the value here that can name a position past the end — and
    // `sizeof(buffer) - off` is unsigned, so one byte past turns the remaining
    // space into very nearly the whole address space and the next call writes
    // without a bound. The request is `CSI 2 $ w`, so the column count is a
    // remote program's to choose.
    //
    // The assertion states the margin as arithmetic rather than leaving it to be
    // rediscovered: it is a relationship between two constants in different
    // files, and the one here is a width while the one it must cover is a sum of
    // decimal lengths.
    char buffer[8 + Ubiquitty::maxColumns * 4];
    static_assert(dectabsrWidestReplyInBytes() <= sizeof(buffer),
        "a grid this wide can name more tab stops than the reply buffer holds: widen the buffer with maxColumns");
    int off = 0;
    off += std::snprintf(buffer + off, sizeof(buffer) - off, "\033P2$u");
    bool first = true;
    for (int c = 0; c < t.columns; c++) {
        if (!t.tabStops[c])
            continue;
        // The guard the assertion cannot make: snprintf answers negative on an
        // encoding failure, which no width of buffer prevents.
        if (off < 0 || static_cast<std::size_t>(off) >= sizeof(buffer))
            break;
        off += std::snprintf(buffer + off, sizeof(buffer) - off, first ? "%d" : "/%d", c + 1);
        first = false;
    }
    if (off < 0 || static_cast<std::size_t>(off) >= sizeof(buffer))
        return; // nothing well-formed to send, and the terminator would write past the end
    const int terminator = std::snprintf(buffer + off, sizeof(buffer) - off, "\033\\");
    if (terminator < 0)
        return;
    // The bulk channel, because this reply's length is the grid's to decide. The
    // small path holds 256 bytes and drops anything longer whole, which for a
    // report a host is waiting on means the query never answers — and a stop in
    // every column of a wide grid is four thousand bytes. XTGETTCAP answers the
    // same way for the same reason.
    (void)respondBulk(t, Ubiquitty::Capability::ScreenContentRead, { buffer, static_cast<std::size_t>(off + terminator) });
}

// DECRQTSR (CSI Ps $ u) — Request Terminal State Report. Ps=1 answers DECTSR,
// DCS 1 $ s D…D ST. DEC STD 070 leaves D…D device-specific and tells the host
// not to interpret it: the entire contract is that handing the same string back
// as DECRSTS (DCS 1 $ p D…D ST) returns the terminal to the state that was
// reported, so this encoder and decrstsApply are one format and only move
// together. Ps=2 (DECCTR, the VT525 colour table) is not claimed — DEC's colour
// coordinates are integer percentages, so reporting a 16-bit palette through
// them and taking it back would quantise it, i.e. the report would not
// round-trip its own values. Ps=0 is DEC's error form. Neither draws a reply,
// matching csiH_decrqpsr on a selector this device does not have.

// The set-up state DECRSTS can put back, and only that: cursor, tab stops and
// the pen are the other half of DEC's state-report split and belong to
// DECRQPSR (DECCIR / DECTABSR). Each mode is read through the same DECRQM
// lookup that answers a host's mode probe, so a saved state cannot disagree
// with what DECRQM reports. A mode's index here is its bit in the report:
// appending is safe, reordering is not, and decTerminalStateVersion is what
// lets a changed layout be rejected instead of misread. DECCOLM (?3) is
// excluded because its setter resizes and erases the page — a state restore
// must not destroy screen content the report never carried.
struct DECSetupMode {
    bool privateMode;
    short ps;
};
static constexpr DECSetupMode decSetupModes[] = {
    { true, 1 }, // DECCKM
    { true, 4 }, // DECSCLM
    { true, 5 }, // DECSCNM
    { true, 6 }, // DECOM
    { true, 7 }, // DECAWM
    { true, 8 }, // DECARM
    { true, 25 }, // DECTCEM
    { true, 42 }, // DECNRCM
    { true, 45 }, // reverse wrap
    { true, 66 }, // DECNKM
    { true, 67 }, // DECBKM
    { true, 69 }, // DECLRMM
    { false, 2 }, // KAM
    { false, 4 }, // IRM
    { false, 12 }, // SRM
    { false, 20 }, // LNM
};
static constexpr int decSetupModeCount = static_cast<int>(sizeof(decSetupModes) / sizeof(decSetupModes[0]));
static constexpr unsigned decTerminalStateVersion = 1;
static constexpr int decTerminalStateFields = 8;

void csiH_decrqtsr(Ubiquitty::Terminal& t, char)
{
    if (parameterOr(t, 0, 0) != 1)
        return;
    unsigned flags = 0;
    for (int i = 0; i < decSetupModeCount; i++) {
        const int state = decSetupModes[i].privateMode ? decrqmDECState(t, decSetupModes[i].ps) : decrqmANSIState(t, decSetupModes[i].ps);
        if (state == 1 || state == 3) // DECRQM: set, or permanently set
            flags |= 1u << i;
    }
    const Ubiquitty::Screen::State* s = t.screens.active;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "\033P1$s%u;%u;%d;%u;%d;%d;%d;%d\033\\", decTerminalStateVersion, static_cast<unsigned>(std::to_underlying(t.conformanceLevel)), t.transmit8bitC1 ? 2 : 1, flags, s->scrollTop + 1, s->scrollBottom + 1,
        Ubiquitty::Screen::effectiveLeft(t) + 1, Ubiquitty::Screen::effectiveRight(t) + 1);
    respond(t, Ubiquitty::Capability::ModeStateRead, buffer);
}

// DECRSTS (DCS Ps $ p D…D ST) — Restore Terminal State, DECTSR's inverse.
// All-or-nothing: a body that is truncated, mis-versioned or malformed leaves
// the terminal untouched, because a half-applied restore lands it in a state
// that was never saved and that no host can name.
void decrstsApply(Ubiquitty::Terminal& t)
{
    if (t.dcsTruncated || t.dcsParamCount < 1 || t.dcsParameters[0] != 1)
        return;
    const char* p = t.parser.osc.data;
    const int length = t.parser.osc.length;
    if (!p || length <= 0)
        return;
    unsigned field[decTerminalStateFields] = {};
    int index = 0;
    bool digitSeen = false;
    for (int i = 0; i < length; i++) {
        const char c = p[i];
        if (c >= '0' && c <= '9') {
            if (field[index] > 0xFFFFFFu)
                return; // wider than any field this terminal writes
            field[index] = field[index] * 10u + static_cast<unsigned>(c - '0');
            digitSeen = true;
        } else if (c == ';') {
            if (!digitSeen || index + 1 >= decTerminalStateFields)
                return;
            index++;
            digitSeen = false;
        } else {
            return;
        }
    }
    if (index != decTerminalStateFields - 1 || !digitSeen || field[0] != decTerminalStateVersion)
        return;
    const unsigned level = field[1];
    if (level < static_cast<unsigned>(std::to_underlying(VT::ConformanceLevel::VT100)) || level > static_cast<unsigned>(std::to_underlying(VT::ConformanceLevel::VT500)))
        return;
    const int top = static_cast<int>(field[4]);
    const int bottom = static_cast<int>(field[5]);
    const int left = static_cast<int>(field[6]);
    const int right = static_cast<int>(field[7]);
    if (top < 1 || top >= bottom || bottom > t.rows || left < 1 || left >= right || right > t.columns)
        return;
    // DECSCL's soft reset is not run here: DECRSTS restores state, it
    // does not re-initialise the terminal.
    t.conformanceLevel = static_cast<VT::ConformanceLevel>(level);
    t.transmit8bitC1 = (field[2] == 2);
    // Modes before margins — DECLRMM's reset arm clears the left/right region,
    // so the reverse order would undo the margins just restored.
    for (int i = 0; i < decSetupModeCount; i++) {
        const bool on = (field[3] & (1u << i)) != 0;
        if (decSetupModes[i].privateMode)
            setPrivateMode(t, decSetupModes[i].ps, on ? Ubiquitty::Modes::ModeChange::Set : Ubiquitty::Modes::ModeChange::Clear);
        else
            setANSIMode(t, decSetupModes[i].ps, on ? Ubiquitty::Modes::ModeChange::Set : Ubiquitty::Modes::ModeChange::Clear);
    }
    Ubiquitty::Screen::State* s = t.screens.active;
    s->scrollTop = top - 1;
    s->scrollBottom = bottom - 1;
    s->scrollLeft = left - 1;
    s->scrollRight = right - 1;
    Screen::cursorClampOrigin(t); // restored margins can strand the cursor outside them
}

void csiH_xtchecksum(Ubiquitty::Terminal& t, char)
{
    if (t.conformanceLevel < ConformanceLevel::VT400)
        return;
    t.checksumExtensions = ChecksumExtensions::fromRaw(static_cast<uint8_t>(parameterOr(t, 0, 0) & checksumSupportedExtensions.toRaw()));
}

void csiH_decrqcra(Ubiquitty::Terminal& t, char)
{
    // DECRQCRA — Request Checksum of Rectangular Area (VT420+). Reply:
    // DCS Pid ! ~ <4 upper-hex digits> ST — the low 16 bits of the selected sum.
    //
    // DEC specifies the region and reply format, not the checksum algorithm.
    // XTCHECKSUM selects video attributes, blank/uninitialized cells, Unicode
    // width and combining characters independently of DEC's region rules.
    int processID = parameterOr(t, 0, 0);
    // Page zero covers all page memory and ignores the rectangle and DECOM.
    // Nonzero page numbers clamp to the sole page this model implements.
    Ubiquitty::Screen::RectArea r { 0, 0, t.rows - 1, t.columns - 1 };
    if (parameterOr(t, 1, 0) != 0 && !decRectFromParameters(t, 2, r))
        return;
    unsigned sum = 0;
    std::array<uint32_t, clusterPoolMaximumCodePoints> cluster {};
    if (t.grid) {
        for (int row = r.top; row <= r.bot; row++) {
            for (int column = r.left; column <= r.right; column++) {
                const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column));
                if (!cell || cell->pen.attributes.contains(Attribute::WideContinuation))
                    continue;
                if (cell->codePoint == 0 && t.checksumExtensions.contains(ChecksumExtension::OmitUninitialized))
                    continue;
                unsigned value = cell->codePoint ? cell->codePoint : ' ';
                if (!t.checksumExtensions.contains(ChecksumExtension::FullCodePoints))
                    value &= 0xFFu;
                else {
                    const int length = cellClusterCodePoints(&t, cell, cluster.data(), static_cast<int>(cluster.size()));
                    for (int index = 1; index < length; ++index)
                        value += cluster[static_cast<std::size_t>(index)];
                }
                if (!t.checksumExtensions.contains(ChecksumExtension::OmitAttributes)) {
                    const AttributeSet attributes = cell->pen.attributes;
                    if (attributes.contains(Attribute::Protected))
                        value += 0x04;
                    if (attributes.contains(Attribute::Hidden))
                        value += 0x08;
                    if (attributes.contains(Attribute::Underline))
                        value += 0x10;
                    if (attributes.contains(Attribute::Reverse))
                        value += 0x20;
                    if (attributes.contains(Attribute::Blink))
                        value += 0x40;
                    if (attributes.contains(Attribute::Bold))
                        value += 0x80;
                }
                if (value != ' ' || t.checksumExtensions.contains(ChecksumExtension::IncludeBlanks))
                    sum += value;
            }
        }
    }
    unsigned cksum = (t.checksumExtensions.contains(ChecksumExtension::Positive) ? sum : 0u - sum) & 0xFFFFu;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "\033P%d!~%04X\033\\", processID, cksum);
    respond(t, Ubiquitty::Capability::ScreenContentRead, buffer);
}

// XTREPORTSGR (CSI Pt ; Pl ; Pb ; Pr # |): the rendition every cell of the
// rectangle shares, as the SGR that sets it (xterm misc.c, xtermReportSGR). An
// attribute survives only if every cell has it, a colour only if every cell has
// the same one. The rectangle is DECRQCRA's, with its defaults, origin-mode
// offset and clipping; a never-written cell has the plain rendition.
void csiH_xtreportsgr(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Pen common {};
    Ubiquitty::penReset(&common);
    Ubiquitty::Screen::RectArea r {};
    if (t.grid && decRectFromParameters(t, /*firstIndex*/ 0, r)) {
        bool isFirst = true;
        for (int row = r.top; row <= r.bot; row++) {
            for (int column = r.left; column <= r.right; column++) {
                Ubiquitty::Pen pen {};
                Ubiquitty::penReset(&pen);
                if (const Ubiquitty::Cell* cell = t.grid->cellAt(Ubiquitty::Row(row), Ubiquitty::Column(column)))
                    pen = Ubiquitty::cellPen(*cell);
                if (isFirst) {
                    common = pen;
                    isFirst = false;
                    continue;
                }
                common.attributes = Ubiquitty::VT::AttributeSet::fromRaw(common.attributes.toRaw() & pen.attributes.toRaw());
                if (Ubiquitty::penUnderlineShape(&common) != Ubiquitty::penUnderlineShape(&pen))
                    common.attributes.remove(Ubiquitty::VT::Attribute::Underline);
                if (!pen.extendedAttributes.flags.contains(Ubiquitty::VT::ExtendedAttribute::BlinkFast))
                    common.extendedAttributes.flags.remove(Ubiquitty::VT::ExtendedAttribute::BlinkFast);
                if (common.foregroundIndex != pen.foregroundIndex || (pen.foregroundIndex == Ubiquitty::colorRGB && (common.foregroundRed != pen.foregroundRed || common.foregroundGreen != pen.foregroundGreen || common.foregroundBlue != pen.foregroundBlue)))
                    common.foregroundIndex = Ubiquitty::colorDefault;
                if (common.backgroundIndex != pen.backgroundIndex || (pen.backgroundIndex == Ubiquitty::colorRGB && (common.backgroundRed != pen.backgroundRed || common.backgroundGreen != pen.backgroundGreen || common.backgroundBlue != pen.backgroundBlue)))
                    common.backgroundIndex = Ubiquitty::colorDefault;
            }
        }
    }
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    Ubiquitty::SGR::writeParameters(common, reply);
    reply.put('m');
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::ScreenContentRead, reply.span());
}

// DECPS (CSI Pvol ; Pdur ; Pnote... , ~) — Play Sound (VT520). Pvol 0..7,
// Pdur in 1/32-s units, Pnote(s) played in sequence (0 = rest, 1..25 map to
// chromatic C5..C7, the VT520's two octaves; 26..49 go on up to C9, so with
// the transposition sub-parameter's four octaves down the range is eight).
// Colon sub-parameters carry what Ubiquitty adds:
// Pvol:wave:channel:attack:release:down, Pdur:gap, Pnote:volume; the seam's
// PlayTonesRequest says what each means. Routes to PAL::playTones (NULL until
// a frontend installs a synth; DECPS is still parsed + consumed).
static uint32_t decpsNoteFreq(int note)
{
    if (note <= 0 || note > VT::toneNoteCount)
        return 0; // 0 = rest; past C9 unsupported.
    // C5..C9 chromatic scale: freq = round(523 Hz * 2^((n-1)/12)).
    // Precomputed so this TU stays float-free on the hot dispatch path.
    static const uint16_t freq[VT::toneNoteCount] = {
        523,
        554,
        587,
        622,
        659,
        698,
        740,
        784,
        831,
        880,
        932,
        988,
        1047,
        1109,
        1175,
        1245,
        1319,
        1397,
        1480,
        1568,
        1661,
        1760,
        1865,
        1976,
        2093,
        2217,
        2349,
        2489,
        2637,
        2794,
        2960,
        3136,
        3322,
        3520,
        3729,
        3951,
        4186,
        4435,
        4699,
        4978,
        5274,
        5588,
        5920,
        6272,
        6645,
        7040,
        7459,
        7902,
        8372,
    };
    return freq[note - 1];
}

void csiH_decps(Ubiquitty::Terminal& t, char)
{
    // The semicolon-separated groups: a slot, then the slots a colon joined
    // to it. Group 0 is the volume and the voice, group 1 the duration and
    // its gap, the rest one note each.
    const auto& parser = t.parser;
    int groupStart[VT::maxParameters];
    int groupLength[VT::maxParameters];
    int groups = 0;
    for (int i = 0; i < parser.parameterCount; ++i) {
        if (i == 0 || !parser.parameterSeparatorWasColon[static_cast<std::size_t>(i)]) {
            groupStart[groups] = i;
            groupLength[groups] = 1;
            ++groups;
        } else {
            ++groupLength[groups - 1];
        }
    }
    const auto has = [&](int group, int piece) { return group < groups && piece < groupLength[group]; };
    const auto piece = [&](int group, int index, int low, int high) {
        if (!has(group, index))
            return low;
        const std::uint32_t value = parser.parameters[static_cast<std::size_t>(groupStart[group] + index)];
        return value > static_cast<std::uint32_t>(high) ? high : static_cast<int>(value);
    };
    Ubiquitty::PAL::PlayTonesRequest request;
    // The head reads as a tracker's row does: the volume, then where (the
    // channel), what (the voice and its register), the envelope in its
    // order, the filters, and the send.
    request.volume = static_cast<uint8_t>(piece(0, 0, 0, VT::toneMaxVolume));
    request.channel = static_cast<uint8_t>(piece(0, 1, 0, VT::toneMaxChannel));
    request.wave = static_cast<uint8_t>(piece(0, 2, 0, VT::toneMaxWave));
    request.semitonesDown = static_cast<uint8_t>(piece(0, 3, 0, VT::toneMaxSemitonesDown));
    if (has(0, 4))
        request.attackMilliseconds = static_cast<uint16_t>(piece(0, 4, 0, VT::toneMaxEnvelopeMilliseconds));
    request.decayMilliseconds = static_cast<uint16_t>(piece(0, 5, 0, VT::toneMaxEnvelopeMilliseconds));
    request.sustainLevel = static_cast<uint8_t>(has(0, 6) ? piece(0, 6, 0, VT::toneMaxVolume) : VT::toneMaxVolume);
    if (has(0, 7))
        request.releaseMilliseconds = static_cast<uint16_t>(piece(0, 7, 0, VT::toneMaxEnvelopeMilliseconds));
    request.lowpassHertz = static_cast<uint16_t>(piece(0, 8, 0, VT::toneMaxCutoffHertz));
    request.highpassHertz = static_cast<uint16_t>(piece(0, 9, 0, VT::toneMaxCutoffHertz));
    request.reverbSend = static_cast<uint8_t>(piece(0, 10, 0, VT::toneMaxVolume));
    // Absent is the centre: a request that names no pan is centred, never hard
    // to one side.
    request.pan = static_cast<uint8_t>(has(0, 11) ? piece(0, 11, 0, VT::toneMaxPan) : VT::tonePanCentre);
    const bool isSampleVoice = request.wave == 6;
    // The VT520 bounds the duration at 255 units, just under eight seconds.
    const int durationUnits = piece(1, 0, 0, VT::toneMaxDurationUnits);
    request.durationMilliseconds = static_cast<uint32_t>(durationUnits) * 1000u / 32u;
    request.gapMilliseconds = static_cast<uint32_t>(piece(1, 1, 0, durationUnits)) * 1000u / 32u;
    request.gridUnits = static_cast<uint8_t>(piece(1, 2, 0, VT::toneMaxGridUnits));
    request.repeat = static_cast<uint8_t>(piece(1, 3, 0, VT::toneMaxRepeat));
    for (int group = 2; group < groups && request.count < Ubiquitty::PAL::PlayTonesRequest::maxNotes; ++group) {
        const int note = piece(group, 0, 0, 99999);
        request.freqs[request.count] = decpsNoteFreq(note);
        request.noteVolumes[request.count] = static_cast<uint8_t>(piece(group, 1, 0, VT::toneMaxVolume));
        request.noteEffects[request.count] = static_cast<uint8_t>(piece(group, 2, 0, VT::toneMaxEffect));
        request.noteParameters[request.count] = static_cast<uint8_t>(piece(group, 3, 0, VT::toneMaxParameter));
        // On the sample voice the first sounding note names the slot; every
        // sounding note replays it.
        if (isSampleVoice && request.sampleSlot == 0 && note >= 1 && note <= Ubiquitty::VT::sampleSlotCount)
            request.sampleSlot = static_cast<uint8_t>(note);
        ++request.count;
    }
    if (request.volume != 0 && request.count == 0)
        return; // no notes — nothing to play; volume 0 is a stop and carries none.
    if (isSampleVoice && t.sampleBank) {
        if (const std::vector<int16_t>* pcm = t.sampleBank->slot(request.sampleSlot)) {
            request.samplePCM = pcm->data();
            request.sampleFrames = static_cast<uint32_t>(pcm->size());
        }
    }
    // Sound is the one effect here that leaves the screen: it reaches a person
    // who is not looking at it, and the people beside them. A request that
    // starts sound is gated; one that stops it is not, because the answer to
    // "may this program make noise" being no is not a reason to strand a
    // channel it already started sounding.
    if (request.volume != 0 && !Ubiquitty::effectAllowed(t, Ubiquitty::Capability::PlaySound))
        return;
    // A host forwards the request to the windows attached to the session; a
    // window parsing for itself plays it here.
    const auto admission = Ubiquitty::ensurePendingSound(t).push(request, t.sampleBank ? t.sampleBank->serial : 0);
    if (admission != PendingSoundAdmission::Queued)
        Ubiquitty::Diagnostics::diagLog(t, "sound-queue-refused", static_cast<uint32_t>(admission));
    if (Ubiquitty::PAL::PlayTonesCallback fn = Ubiquitty::PAL::playTones())
        fn(request);
}

void csiH_decset(Ubiquitty::Terminal& t, char)
{
    for (int i = 0; i < t.parser.parameterCount; i++)
        setPrivateMode(t, t.parser.parameters[i], Ubiquitty::Modes::ModeChange::Set);
}

void csiH_decrst(Ubiquitty::Terminal& t, char)
{
    for (int i = 0; i < t.parser.parameterCount; i++)
        setPrivateMode(t, t.parser.parameters[i], Ubiquitty::Modes::ModeChange::Clear);
}

void csiH_decst8c(Ubiquitty::Terminal& t, char)
{
    // DECST8C — reset tab stops to every 8th column. Only `CSI ? 5 W` is the
    // control (VT510 DECST8C); another parameter, or a second one, is not.
    if (t.parser.parameterCount != 1 || parameterOr(t, 0, 0) != 5)
        return;
    for (int i = 0; i < Ubiquitty::maxColumns; i++)
        t.tabStops[i] = (i < t.columns && i % 8 == 0 && i != 0);
}

void csiH_xtsmgraphics(Ubiquitty::Terminal& t, char)
{
    // XTSMGRAPHICS — CSI ? Pi ; Pa ; Pv S. Pi = item (1 = sixel colour
    // registers, 2 = sixel geometry); Pa = action (1 read, 2 reset, 3 set,
    // 4 read the maximum). Reply CSI ? Pi ; Ps [; value...] S, Ps = 0 success,
    // 1 error in Pi, 2 error in Pa, 3 failure (ctlseqs). Exactly three
    // parameters, or there is no reply (xterm charproc.c, CASE_GRAPHICS_ATTRIBUTES:
    // `nparam != 3`). ReGIS geometry (3) is an unknown item in a build without
    // ReGIS, as xterm's is.
    if (t.parser.parameterCount != 3)
        return;
    const int item = parameterOr(t, 0, 0);
    const int action = parameterOr(t, 1, 0);
    char buffer[48];
    const bool isKnownAction = action >= 1 && action <= 4;
    if (item == 1) {
        // Sixel colour registers. The decoder holds palette[256][3]
        // (Source/Core/Image/Sixel/Decoder.h), so the cap is 256. Pa=3 (set)
        // is accepted as a no-op that still reports the fixed 256.
        if (isKnownAction)
            respond(t, Ubiquitty::Capability::GraphicsCapabilityRead, "\033[?1;0;256S");
        else
            respond(t, Ubiquitty::Capability::GraphicsCapabilityRead, "\033[?1;2S");
        return;
    }
    if (item == 2) {
        // Sixel geometry. The decode path bounds images by maxSixelWidth and
        // maxSixelHeight (1000×1000) in sixelDecodeCompletion, which a program
        // may read but not change: "disallows modifying those sizes" (ctlseqs),
        // so a reset or a set fails.
        if (action == 1 || action == 4) {
            snprintf(buffer, sizeof(buffer), "\033[?2;0;%d;%dS", 1000, 1000);
            respond(t, Ubiquitty::Capability::GraphicsCapabilityRead, buffer);
        } else if (isKnownAction) {
            respond(t, Ubiquitty::Capability::GraphicsCapabilityRead, "\033[?2;3S");
        } else {
            respond(t, Ubiquitty::Capability::GraphicsCapabilityRead, "\033[?2;2S");
        }
        return;
    }
    snprintf(buffer, sizeof(buffer), "\033[?%d;1S", item);
    respond(t, Ubiquitty::Capability::GraphicsCapabilityRead, buffer);
}

} // namespace Ubiquitty::VT
