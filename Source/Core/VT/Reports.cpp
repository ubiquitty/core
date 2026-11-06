// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Reports.cpp
//
// Everything the terminal says back to the host: the DA / DA2 / DA3 and DSR
// handlers, and the encoders they call — DECRQM state lookups, the
// CSI-parameter reply encoder, primary Device AttributeSet, the color-scheme
// report, the in-band resize notice, the fixed-answer DEC DSR statuses, the
// DECRQSS value encoder, and the DEC locator report encoder.
//
// Handler and encoder together on purpose: device attributes is one question
// asked three ways and DSR has two forms, so filing them by the prefix byte
// that distinguishes them splits one feature across files and separates each
// handler from the encoder it exists to call.
//
// Cold relative to the parser hot loop. DECRQSS framing / dispatch and
// XTGETTCAP stay in VT.cpp.

#include "Core/BuildVersion.h"
#include "Core/CellGrid.h"
#include "Core/Keys/Keymap.h"
#include "Core/ReplyBuilder.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/DECPSSynth.h"
#include "Core/VT/Internal.h"
#include "Core/VT/Sound.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace Ubiquitty::VT {

// Local 2-value scheme tag for the DEC 2031 / DSR 996 color-scheme report.
// Distinct from Ubiquitty::Preferences::ColorScheme (the appearance enum); this is
// the on-the-wire dark/light answer only.
// Pinned, because Preferences::ColorScheme spells the same two names. The values
// happen to agree today; nothing holds them together, and a cast between a wire
// answer and an appearance setting would be wrong even when it is right.
enum class ColorScheme : uint8_t {
    Light = 0,
    Dark = 1
};

// The scheme the active default background reads as (Rec.601-ish 16-bit luma
// vs 0x8000 threshold). Falls back to Dark when no background hook is installed.
static ColorScheme defaultBackgroundColorScheme(Ubiquitty::Terminal& t)
{
    const Ubiquitty::PaletteRGB& background = t.palette.defaultBackground;
    unsigned luma = (3u * background.red + 6u * background.green + background.blue) / 10u;
    return luma < 0x8000u ? ColorScheme::Dark : ColorScheme::Light;
}

// Sends the DEC mode 2031 / DSR 996 color-scheme report (CSI ?997;Nn,
// 1 = dark / 2 = light). The DSR 996 reply and the unsolicited DECSET
// 2031 notification share this wire form, so both route through here.
void reportColorScheme(Ubiquitty::Terminal& t)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[?997;");
    reply.put(defaultBackgroundColorScheme(t) == ColorScheme::Dark ? '1' : '2');
    reply.put("n");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::StatusReport, reply.span());
}

void respondCSIParameter(Ubiquitty::Terminal& t, const char* prefix, int a, int b, char final)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    reply.put(prefix ? std::string_view(prefix) : std::string_view());
    reply.putDecimal(a);
    if (b >= 0) {
        reply.put(';');
        reply.putDecimal(b);
    }
    reply.put(final);
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::ModeStateRead, reply.span());
}

// In-band resize report (DEC private mode 2048): CSI 48 ; rows ; columns ;
// pixh ; pixw t, pixh/pixw the text-area size in device pixels. 0 when no cell
// pixel size is published yet (spec's "unknown"). No-op unless mode 2048 is set.
void emitInBandResize(Ubiquitty::Terminal& t)
{
    if (!t.modes.inBandResize)
        return;
    int pixHeight = static_cast<int>(t.cellPixelHeight) * t.rows;
    int pixWidth = static_cast<int>(t.cellPixelWidth) * t.columns;
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[48;");
    reply.putDecimal(t.rows);
    reply.put(';');
    reply.putDecimal(t.columns);
    reply.put(';');
    reply.putDecimal(pixHeight);
    reply.put(';');
    reply.putDecimal(pixWidth);
    reply.put('t');
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::TerminalSizeRead, reply.span());
}

void announceAmbiguousWidthIfMoved(Ubiquitty::Terminal& t)
{
    const int cells = static_cast<int>(t.ambiguousWidth);
    if (t.ambiguousWidthAnnounced == cells)
        return;
    t.ambiguousWidthAnnounced = cells;
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    reply.putDecimal(cells);
    reply.put('W');
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::ModeStateRead, reply.span());
}

// DA1 / DECID response, keyed to conformance level. VT200..VT500 report device
// class 62..65 followed by the VT510-numbered capabilities Ubiquitty implements:
// 1 = 132 columns (DECCOLM), 2 = printer port (MC into the print sink), 4 = sixel,
// 6 = selective erase (DECSCA/DECSEL/DECSED), 7 = soft character set (DECDLD),
// 8 = UDK (DECUDK), 9 = NRCS (DECNRCM + the national sets), 15 = DEC technical
// set (ESC ( > and the G1..G3 designations), 16 = locator port, 17 = terminal
// state interrogation (DECRQTSR), 18 = user windows, 21 = horizontal scrolling
// (SL/SR + DECSLRM), 22 = ANSI color, 28 = rectangular editing, 29 = ANSI text
// locator (DECELR/DECSLE/DECRQLP).
// Every number here answers: a host that probes any of them gets a reply rather
// than blocking until its own timeout.
// VT100 reports the classic "no options" form `CSI ? 1 ; 2 c`.
void emitPrimaryDA(Ubiquitty::Terminal& t)
{
    if (t.conformanceLevel <= VT::ConformanceLevel::VT100) {
        respond(t, Ubiquitty::Capability::DeviceAttributesRead, "\033[?1;2c");
        return;
    }
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[?");
    reply.putDecimal(static_cast<int>(std::to_underlying(t.conformanceLevel)));
    reply.put(";1;2;4;6;7;8;9;15;16;17;18;21;22;28;29c");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::DeviceAttributesRead, reply.span());
}

// DEC DSR selectors whose answer is a fixed capability statement rather than
// live state; returns false for a selector this does not own so the caller can
// keep looking. A recognised-but-unanswered DSR costs the host its whole read
// timeout, so every selector named here must reply even when the reply is
// "nothing to report".
//   ?5 n                 → CSI ? 0 n            no malfunction, as DSR 5 answers (xterm CASE_DSR)
//   ?62 n        DECMSR  → CSI 0 * {            no macro space (no DECDMAC)
//   ?63 ; Pid n  DECCKSR → DCS Pid ! ~ 0000 ST  checksum of an empty macro store
//   ?75 n                → CSI ? 70 n           data integrity: no link errors
//   ?85 n                → CSI ? 83 n           not configured for multi-session
bool emitDECDSRFixedStatus(Ubiquitty::Terminal& t, int ps, int pid)
{
    switch (ps) {
    case 5:
        respond(t, Ubiquitty::Capability::StatusReport, "\033[?0n");
        return true;
    case 62:
        respond(t, Ubiquitty::Capability::StatusReport, "\033[0*{");
        return true;
    case 63: {
        // Pid is echoed so a host can pair reply to request; it is a bare
        // numeric parameter on the wire, so a negative value would be malformed.
        if (pid < 0)
            pid = 0;
        char buffer[32];
        int n = std::snprintf(buffer, sizeof(buffer), "\033P%d!~0000\033\\", pid);
        if (n > 0 && n < static_cast<int>(sizeof(buffer)))
            respond(t, Ubiquitty::Capability::StatusReport, buffer);
        return true;
    }
    case 75:
        respond(t, Ubiquitty::Capability::StatusReport, "\033[?70n");
        return true;
    case 85:
        respond(t, Ubiquitty::Capability::StatusReport, "\033[?83n");
        return true;
    default:
        return false;
    }
}

// DECRQSS settings reported out of Terminal state. Writes the bare
// "<value><selector>" body — the DCS 1 $ r … ST framing is the caller's — and
// returns its length; 0 means "not one of ours", which the caller answers with
// the DCS 0 $ r ST "invalid request" form.
int decrqssSettingReport(const Ubiquitty::Terminal& t, std::string_view selector, Ubiquitty::ReplyBuilder& output)
{
    const char* const pt = selector.data();
    const int length = static_cast<int>(selector.size());
    if (!pt)
        return 0;
    const int before = output.length();
    if (length == 1 && pt[0] == 't') {
        // DECSLPP — page length. `CSI Ps t` with Ps >= 24 sets it by resizing
        // (csiH_xtwinops), so the live row count is the setting.
        output.putDecimal(t.rows);
        output.put("t");
    } else if (length == 2 && pt[0] == '*' && pt[1] == '|') {
        // DECSNLS — lines per screen; the same page geometry as DECSLPP.
        output.putDecimal(t.rows);
        output.put("*|");
    } else if (length == 2 && pt[0] == '*' && pt[1] == 'x') {
        // DECSACE — 0 = stream, 2 = rectangle. Ps 0 and Ps 1 select the same
        // extent, and the report normalises both to 0: hosts compare the reply
        // literally, so Ps 1 must never come back.
        output.putDecimal(t.modes.sace == 2 ? 2 : 0);
        output.put("*x");
    } else if (length == 2 && pt[0] == '$' && pt[1] == '}') {
        // DECSASD — 0 = data screen is the active display, 1 = status line.
        output.putDecimal(static_cast<int>(t.statusLineActive));
        output.put("$}");
    } else if (length >= 2 && pt[0] == '>' && pt[length - 1] == 't') {
        // XTSMTITLE (xterm misc.c, do_dcs): `> t` reports every title mode, 0
        // to 3, as 1 or 0; `> Ps t` reports mode Ps alone. A mode it does not
        // have is an invalid request.
        constexpr int lastTitleMode = 3;
        if (length == 2) {
            output.put('>');
            for (int mode = 0; mode <= lastTitleMode; ++mode) {
                if (mode > 0)
                    output.put(';');
                output.putDecimal((t.titleModes >> mode) & 1);
            }
            output.put('t');
        } else {
            int mode = 0;
            for (int index = 1; index < length - 1; ++index) {
                if (pt[index] < '0' || pt[index] > '9' || mode > lastTitleMode)
                    return 0;
                mode = mode * 10 + (pt[index] - '0');
            }
            if (mode > lastTitleMode)
                return 0;
            output.put('>');
            output.putDecimal((t.titleModes >> mode) & 1);
            output.put('t');
        }
    } else if (length >= 2 && pt[0] == '>' && (pt[length - 1] == 'm' || pt[length - 1] == 'f')) {
        // XTQMODKEYS and XTQFMTKEYS through DECRQSS (xterm do_dcs): `> Pp m`
        // reports a key-modifier resource and `> Pp f` a format one, each as
        // the control that restores it. The resources are the ones the CSI
        // queries answer: modifyKeyboard (held at 0) through modifyOtherKeys,
        // and formatOtherKeys.
        const bool isFormat = pt[length - 1] == 'f';
        int resource = 0;
        for (int index = 1; index < length - 1; ++index) {
            if (pt[index] < '0' || pt[index] > '9' || resource > 9)
                return 0;
            resource = resource * 10 + (pt[index] - '0');
        }
        if (length == 2 || resource >= static_cast<int>(VT::xtermKeyModifierCategoryCount))
            return 0;
        output.put('>');
        output.putDecimal(resource);
        output.put(';');
        const int value = isFormat ? t.xtermKeyFormats[static_cast<std::size_t>(resource)] : t.xtermKeyModifiers[static_cast<std::size_t>(resource)];
        output.putDecimal(value == Ubiquitty::Keys::Keymap::keymapModifyLevelDisabled ? 65535 : value);
        output.put(isFormat ? 'f' : 'm');
    } else if (length >= 3 && pt[length - 2] == ',' && pt[length - 1] == '|') {
        // DECAC — "Use Ps1 in DECRQSS to select the item to be reported"
        // (VT520 DECAC): `Ps1 ; Pf ; Pb , |`. An item never assigned has no
        // index to report, and is an invalid request.
        int item = 0;
        for (int index = 0; index < length - 2; ++index) {
            if (pt[index] < '0' || pt[index] > '9' || item > 9)
                return 0;
            item = item * 10 + (pt[index] - '0');
        }
        if (item != 1 && item != 2)
            return 0;
        const Ubiquitty::ColorAssignment& assignment = item == 1 ? t.normalTextColors : t.windowFrameColors;
        if (!Ubiquitty::isColorAssigned(assignment))
            return 0;
        output.putDecimal(item);
        output.put(';');
        output.putDecimal(assignment[0]);
        output.put(';');
        output.putDecimal(assignment[1]);
        output.put(",|");
    } else if (length >= 3 && pt[length - 2] == ',' && pt[length - 1] == '}') {
        // DECATC — "Use Ps1 in DECRQSS to select the attribute combination to
        // be reported" (VT520 DECATC): `Ps1 ; Pf ; Pb , }`. A combination never
        // assigned has no index to report, and is an invalid request.
        int combination = 0;
        for (int index = 0; index < length - 2; ++index) {
            if (pt[index] < '0' || pt[index] > '9' || combination > 9)
                return 0;
            combination = combination * 10 + (pt[index] - '0');
        }
        if (combination >= static_cast<int>(Ubiquitty::alternateTextCombinationCount))
            return 0;
        const Ubiquitty::ColorAssignment& assignment = Ubiquitty::alternateTextColorTable(t)[static_cast<std::size_t>(combination)];
        if (!Ubiquitty::isColorAssigned(assignment))
            return 0;
        output.putDecimal(combination);
        output.put(';');
        output.putDecimal(assignment[0]);
        output.put(';');
        output.putDecimal(assignment[1]);
        output.put(",}");
    } else if (length == 2 && pt[0] == '$' && pt[1] == '~') {
        // DECSSDT — 0 = no status line, 1 = indicator, 2 = host-writable.
        output.putDecimal(static_cast<int>(t.statusLineType));
        output.put("$~");
    } else if (length == 2 && pt[0] == ',' && pt[1] == '~') {
        // DECPS — not a setting but the extension's bounds (Docs/extensions.md):
        // the request's grammar with every parameter at its top, then the
        // sample bank's slots and the seconds a slot holds. A terminal whose
        // DECPS is the speaker alone answers the invalid form instead, which
        // is how a program keeps a score written for the channels from playing
        // there on one voice, every part at once.
        const auto putGroup = [&output](std::initializer_list<int> values) {
            bool isFirst = true;
            for (const int value : values) {
                if (!isFirst)
                    output.put(':');
                output.putDecimal(value);
                isFirst = false;
            }
        };
        putGroup({ toneMaxVolume, toneMaxChannel, toneMaxWave, toneMaxSemitonesDown, toneMaxEnvelopeMilliseconds, toneMaxEnvelopeMilliseconds, toneMaxVolume, toneMaxEnvelopeMilliseconds, toneMaxCutoffHertz, toneMaxCutoffHertz, toneMaxVolume, toneMaxPan });
        output.put(';');
        putGroup({ toneMaxDurationUnits, toneMaxDurationUnits, toneMaxGridUnits, toneMaxRepeat });
        output.put(';');
        putGroup({ toneNoteCount, toneMaxVolume, toneMaxEffect, toneMaxParameter });
        output.put(';');
        putGroup({ sampleSlotCount, static_cast<int>(sampleMaxFrames / sampleRateHertz) });
        output.put(",~");
    }
    // A selector that matched nothing appended nothing, and a match that did not
    // fit is the caller's to refuse whole.
    if (output.isTruncated())
        return 0;
    return output.length() - before;
}

// DECRQM (request mode) — reply form per VT220 manual:
// ANSI: CSI Ps ; Pm $ y
// DEC : CSI ? Ps ; Pm $ y
// Pm = 1 set, 2 reset, 3 permanent set, 4 permanent reset, 0 unknown.
// Ubiquitty doesn't model "permanent" so every recognised mode reports as
// 1 or 2; unknown modes report 0.
// DECRQM answers 0 only for a mode the terminal does not RECOGNISE. An ECMA-48
// mode we know of but will never implement is 4 (permanently reset): 0 invites
// the host to keep probing, 4 tells it the answer is final.
int decrqmANSIState(const Ubiquitty::Terminal& t, int ps)
{
    switch (ps) {
    case 2:
        return t.modes.keyboardLocked ? 1 : 2; // KAM (keyboard action)
    case 4:
        return t.modes.insertMode ? 1 : 2; // IRM (insert mode)
    case 12:
        return t.modes.srm ? 1 : 2; // SRM (send/receive)
    case 20:
        return t.modes.lnm ? 1 : 2; // LNM
    // CRM (control representation): recognised and never entered, so reset
    // even after a program sets it, as xterm answers and the VT510's DECRPM
    // table lists it. Controls keep being performed.
    case 3:
        return 2;
    // Recognised, permanently reset: printer/transfer/area-qualification modes a
    // character-imaging terminal has no component to implement. GATM, SRTM, VEM,
    // HEM, PUM, FEAM, FETM, MATM, TTM, SATM, TSM, and EBM (eliminated outright by
    // the 5th edition, Annex F.5.1).
    case 1:
    case 5:
    case 7:
    case 10:
    case 11:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
        return 4;
    default:
        return 0;
    }
}

int decrqmDECState(const Ubiquitty::Terminal& t, int ps)
{
    switch (ps) {
    case 1:
        return t.modes.appCursorKeys ? 1 : 2; // DECCKM
    case 2:
        return t.modes.vt52Mode ? 2 : 1; // DECANM (reset=VT52)
    case 3:
        return t.modes.columns132 ? 1 : 2; // DECCOLM
    case 95:
        return t.modes.clearScreenOnColumnChange ? 2 : 1; // DECNCSM (set = no clear)
    case 4:
        return t.modes.smoothScroll ? 1 : 2; // DECSCLM
    case 5:
        return t.modes.reverseVideo ? 1 : 2; // DECSCNM
    case 6:
        return t.modes.originMode ? 1 : 2; // DECOM
    case 7:
        return t.modes.autowrap ? 1 : 2; // DECAWM
    case 8:
        return t.modes.autoRepeat ? 1 : 2; // DECARM
    case 38:
        return t.isTekModeActive ? 1 : 2; // DECTEK (Tektronix 4014)
    case 97:
        return t.modes.shouldSaveCRT ? 1 : 2;
    case 101:
        return t.isAnswerbackConcealed ? 1 : 2;
    case 5522:
        return t.modes.shouldReportPasteEvents ? 1 : 2;
    case 7400:
        return t.tekBrightPenOverride == 1 ? 1 : 2; // Tek bright-pen override
    case 12:
        return t.cursorBlinkAtt610 ? 1 : 2; // At&T 610 cursor blink
    case 25:
        return t.modes.isCursorVisible ? 1 : 2; // DECTCEM
    case 35:
        return t.modes.fontShifting ? 1 : 2; // font-shifting (rxvt)
    case 40:
        return t.modes.allow80To132 ? 1 : 2; // allow 80↔132
    case 10:
        // rxvt's show-toolbar. No frontend has a terminal toolbar, so it is
        // permanently reset, as a default xterm answers (misc.c do_dec_rqm).
        return 4;
    case 30:
        return t.scrollbarVisible ? 1 : 2; // show scrollbar (rxvt)
    case 41:
        return t.moreFix ? 1 : 2; // more(1) fix
    case 46:
        return t.loggingEnabled ? 1 : 2; // start/stop logging
    case 18:
        return t.printFormFeed ? 1 : 2; // DECPFF print form feed
    case 19:
        return t.printExtentFullScreen ? 1 : 2; // DECPEX print extent
    case 34:
        return t.shouldWriteRightToLeft ? 1 : 2;
    case 96:
        return t.shouldCopyRightToLeft ? 1 : 2;
    case 102:
        return t.shouldIgnorePrinterNUL ? 1 : 2;
    case 36:
        return isNationalReplacementEnabled(t) ? 2 : 1;
    case 42:
        return isNationalReplacementEnabled(t) ? 1 : 2;
    case 44:
        return t.marginBell ? 1 : 2; // margin bell
    case 45:
        return t.modes.reverseWrap ? 1 : 2; // reverse-wrap
    case 1042:
        return t.bellRaisesUrgency ? 1 : 2; // bell raises urgency
    case 1043:
        return t.popOnBell ? 1 : 2; // pop-on-bell
    case 1044:
        return t.modes.keepClipboard ? 1 : 2; // keepClipboard (reuse selection)
    case 47:
    case 1047:
    case 1049:
        return t.screens.isUsingAlternateScreen ? 1 : 2; // alternate screen
    case 1046:
        return t.modes.allowAlternateScreen ? 1 : 2; // alternate-screen feature enable
    case 1048:
        return t.screens.active->hasSavedCursor ? 1 : 2; // save-cursor (DECSC-style)
    // DEC STD 070 cursor coupling: held and reported as set, so a program
    // that sets or resets one reads back what it asked for; the page here is
    // never larger than the window, so coupling moves nothing.
    case 60: // DECHCCM — reset by default
        return t.modes.isHorizontalCursorCoupled ? 1 : 2;
    case 61: // DECVCCM — set by default
        return t.modes.isVerticalCursorUncoupled ? 2 : 1;
    case 64: // DECPCCM — set by default
        return t.modes.isPageCursorUncoupled ? 2 : 1;
    // Recognised, permanently reset (VT510 DECRPM table; xterm misc.c
    // do_dec_rqm answers 4 at level 3 and up): DECXRLM limits a transmit
    // rate a pseudo-terminal does not have. A set is ignored, so the answer
    // is final.
    case 73: // DECXRLM
        return 4;
    case 81:
        return t.modes.shouldReportKeyPositions ? 1 : 2;
    case 66:
        return t.modes.appKeypad ? 1 : 2; // DECNKM — the ESC = / ESC > state
    case 1045:
        return t.modes.reverseWrapExtended ? 1 : 2;
    case 67:
        return t.modes.backspaceKeySendsBackspace ? 1 : 2; // DECBKM
    case 1034:
        return t.modes.eightBitInput ? 1 : 2; // eightBitInput
    case 1036:
        return t.modes.metaSendsEscape ? 1 : 2; // metaSendsEscape
    case 1039:
        return t.modes.alternateSendsEscape ? 1 : 2; // alternateSendsEscape
    case 1037:
        return t.modes.deleteKeySendsDelete ? 1 : 2; // deleteKeySendsDelete
    case 1050: // terminfo/termcap function keys
        return t.modes.keyboardType == Keys::KeyboardType::Termcap ? 1 : 2;
    case 1051: // Sun function keys
        return t.modes.keyboardType == Keys::KeyboardType::Sun ? 1 : 2;
    case 1052: // HP function keys
        return t.modes.keyboardType == Keys::KeyboardType::HP ? 1 : 2;
    case 1053: // SCO function keys
        return t.modes.keyboardType == Keys::KeyboardType::SCO ? 1 : 2;
    case 1060: // legacy (X11R6) keyboard emulation
        return t.modes.keyboardType == Keys::KeyboardType::Legacy ? 1 : 2;
    case 1061: // VT220 keyboard emulation
        return t.modes.keyboardType == Keys::KeyboardType::VT220 ? 1 : 2;
    case 1035:
        return t.modes.shouldHonorNumLock ? 1 : 2;
    case 69:
        return t.modes.leftRightMarginMode ? 1 : 2; // DECLRMM
    case 80:
        return t.modes.sixelDisplayMode ? 1 : 2; // DECSDM
    case 117:
        return t.modes.shouldEraseToDefaultColors ? 1 : 2; // DECECM
    case 1070:
        return t.modes.hasPrivateSixelColorRegisters ? 1 : 2; // private colour registers
    case 8452:
        return t.modes.shouldLeaveCursorRightOfSixel ? 1 : 2; // sixel cursor to the right
    case 7700:
        return t.modes.shouldReportAmbiguousWidth ? 1 : 2; // ambiguous-width reporting
    case 43:
        return t.modes.shouldExpandGraphicsPrint ? 1 : 2; // DECGEPM
    case 68:
        return t.modes.shouldUseDataProcessingKeys ? 1 : 2; // DECKBUM
    case 1014:
        return t.modes.shouldScrollFast ? 1 : 2; // xterm fastScroll
    case 1040:
        return t.modes.shouldKeepSelection ? 1 : 2; // xterm keepSelection
    case 1041:
        return t.modes.shouldSelectToClipboard ? 1 : 2; // xterm selectToClipboard
    // xterm's read-only resource reports (misc.c do_dec_rqm): a set or reset
    // does not change them. 13 is the reader's blink preference, xterm's
    // cursorBlink resource; 1020 whether input is UTF-8; 1021 whether
    // ambiguous-width characters are wide, xterm's cjkWidth; 1022 whether
    // emoji are wide, which Core always makes them.
    case 13:
        return t.cursorBlinkPref ? 1 : 2;
    case 1020:
        return Ubiquitty::Unicode::encodingIsUTF8(t.parser.inputEncoding.enc) ? 1 : 2;
    case 1021:
        return t.ambiguousWidth == Ubiquitty::Unicode::Width::AmbiguousWidth::Wide ? 1 : 2;
    case 1022:
    case 1023: // privateWidth: private-use glyphs are always single-cell
        return 1;
    // xterm's cursorBlinkXOR (14) combines the escape-set blink with the menu's;
    // here the program's request is reported, never drawn, so nothing is
    // combined.
    case 14:
        return 4;
    case 7723:
        return t.modes.shouldRewrapOnResize ? 1 : 2; // rewrap on resize
    case 77096:
        return t.modes.allowsInputMethodEditor ? 1 : 2; // input method editor
    case 9:
        return t.modes.mouseX10 ? 1 : 2; // X10 mouse (press-only)
    case 1000:
        return t.modes.mouseButtonTracking ? 1 : 2; // normal mouse (press+release)
    case 1001:
        return t.modes.mouseHilite ? 1 : 2; // hilite tracking
    case 1002:
        return t.modes.mouseCellMotion ? 1 : 2;
    case 1003:
        return t.modes.mouseAnyEvent ? 1 : 2;
    case 1004:
        return t.modes.focusReporting ? 1 : 2;
    case 1010:
        return t.modes.scrollOnOutput ? 1 : 2; // scroll-on-output
    case 1011:
        return t.modes.scrollOnKeypress ? 1 : 2; // scroll-on-keypress
    case 1005:
        return t.modes.mouseUTF8Coordinates ? 1 : 2; // UTF-8 mouse coords
    case 1006:
        return t.modes.mouseSGR ? 1 : 2; // SGR mouse
    case 1007:
        return t.modes.alternateScrollMode.asRequested() ? 1 : 2; // alternate scroll
    case 1015:
        return t.modes.mouseURXVT ? 1 : 2; // URXVT mouse coords
    case 1016:
        return t.modes.mouseSGRPixels ? 1 : 2; // SGR-pixels mouse coords
    case 2001:
        return t.modes.shouldMoveReadlinePointOnClick ? 1 : 2;
    case 2002:
        return t.modes.shouldMoveReadlinePointBeforePaste ? 1 : 2;
    case 2003:
        return t.modes.shouldDeleteReadlineSelection ? 1 : 2;
    case 2004:
        return t.modes.bracketedPaste ? 1 : 2;
    case 2005:
        return t.modes.shouldQuotePasteCharacters ? 1 : 2;
    case 2006:
        return t.modes.shouldPreservePasteNewlines ? 1 : 2;
    case 2026:
        return t.modes.syncOutput ? 1 : 2; // synchronized output
    case 2027:
        // Grapheme clustering is permanently active (the only path attaching
        // combining marks / VS / ZWJ to a cell), so report 3 (permanently set)
        // rather than the cosmetic graphemeClustering flag's value.
        return 3; // DECRPM permanently set
    case 2031:
        return t.modes.colorThemeReporting ? 1 : 2; // color-scheme notify
    case 2048:
        return t.modes.inBandResize ? 1 : 2; // in-band resize
    case 7401:
        return t.passwordPromptActive ? 1 : 2; // password input mode
    case 7402:
        return t.modes.fullscreenContent ? 1 : 2; // fullscreen content mode
    case 7403:
        return t.modes.mouseRelativePointerLock ? 1 : 2; // relative pointer-lock (mouselook)
    default:
        return 0;
    }
}

// ---- the handlers, beside the encoders they call ----
//
// Device attributes is one question asked three ways — DA (`CSI c`), DA2
// (`CSI > c`), DA3 (`CSI = c`) — and DSR likewise has an ANSI and a DEC form.
// Filing them by prefix byte puts one feature in two files and separates each
// handler from the encoder it exists to call.

void csiH_da(Ubiquitty::Terminal& t, char)
{
    // Only Ps=0 (default, or no parameter) is defined for DA1; non-zero Ps is
    // reserved, so it draws no reply.
    if (parameterOr(t, 0, 0) != 0)
        return;
    emitPrimaryDA(t);
}
void csiH_da2(Ubiquitty::Terminal& t, char)
{
    // Only Ps=0 (default, or no parameter) is defined for DA2; non-zero Ps is
    // reserved, so it draws no reply (same guard as csiH_da's DA1).
    if (parameterOr(t, 0, 0) != 0)
        return;
    // Secondary DA — CSI > Pp ; Pv ; Pc c. Pp is the terminal MODEL, and it must
    // track the conformance level DA1 reports: answering a VT420 model under a
    // VT500 operating level contradicts our own DA1, and a host that keys feature
    // detection off DA2 would gate away everything above VT420.
    // Pv is the firmware/patch level apps probe for "xterm >= N".
    int model = 64; // VT520
    switch (t.conformanceLevel) {
    case VT::ConformanceLevel::VT100:
        model = 0;
        break;
    case VT::ConformanceLevel::VT200:
        model = 1; // VT220
        break;
    case VT::ConformanceLevel::VT300:
        model = 24; // VT320
        break;
    case VT::ConformanceLevel::VT400:
        model = 41; // VT420
        break;
    case VT::ConformanceLevel::VT500:
        model = 64; // VT520
        break;
    }
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[>");
    reply.putDecimal(model);
    reply.put(';');
    reply.putDecimal(dA2Version);
    reply.put(";0c");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::DeviceAttributesRead, reply.span());
}
void csiH_da3(Ubiquitty::Terminal& t, char)
{
    // Only Ps=0 (default, or no parameter) is defined for DA3; non-zero Ps is
    // reserved, so it draws no reply (same guard as csiH_da's DA1).
    if (parameterOr(t, 0, 0) != 0)
        return;
    // DCS ! | <8 hex digits> ST.  "4D414352" = ascii "MACR".
    respond(t, Ubiquitty::Capability::DeviceAttributesRead, "\033P!|4D414352\033\\");
}
// XTVERSION answers only `CSI > q` and `CSI > 0 q`, as xterm does; another
// parameter asks for nothing this terminal reports. The version in the
// parentheses is the one this binary was built as.
void csiH_xtversion(Ubiquitty::Terminal& t, char)
{
    if (parameterOr(t, 0, 0) != 0)
        return;
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033P>|Ubiquitty(");
    reply.put(Ubiquitty::buildVersion());
    reply.put(")\033\\");
    if (reply.isSendable())
        respond(t, Ubiquitty::Capability::DeviceAttributesRead, reply.span());
}
void csiH_dsr(Ubiquitty::Terminal& t, char)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    int p = parameterOr(t, 0, 0);
    if (p == 5) {
        respond(t, Ubiquitty::Capability::StatusReport, "\033[0n");
    } else if (p == 6) {
        // CPR — when DECOM is set, the reported row/column are RELATIVE
        // to the active scroll region (and to scrollLeft under DECLRMM).
        // Subtract scrollTop / scrollLeft so a cursor at the region
        // origin reports as (1,1).
        int row = s->cursorY + 1;
        int column = s->cursorX + 1;
        if (t.modes.originMode) {
            row -= s->scrollTop;
            if (t.modes.leftRightMarginMode)
                column -= s->scrollLeft;
        }
        if (row < 1)
            row = 1;
        if (column < 1)
            column = 1;
        Ubiquitty::ReplyBuilder reply;
        reply.put("\033[");
        reply.putDecimal(row);
        reply.put(';');
        reply.putDecimal(column);
        reply.put('R');
        if (reply.isSendable())
            respond(t, Ubiquitty::Capability::CursorStateRead, reply.span());
    }
}
void csiH_dsr_dec(Ubiquitty::Terminal& t, char)
{
    // CSI ? Ps n — DEC-private DSR.
    // ?6n   → DECXCPR, extended cursor position with page id
    // ?15n  → printer status   → ?13n (no printer)
    // ?25n  → UDK status       → ?20n unlocked, ?21n locked
    // ?26n  → keyboard status  → ?27;1;0;0n (North American, ready)
    // ?996n → kitty light/dark theme report
    int p = parameterOr(t, 0, 0);
    if (p == 996) {
        reportColorScheme(t);
        return;
    }
    // ?62 / ?63 / ?75 / ?85 answer a fixed capability statement.
    if (emitDECDSRFixedStatus(t, p, parameterOr(t, 1, 0)))
        return;
    if (p == 15) { // no printer connected
        respond(t, Ubiquitty::Capability::StatusReport, "\033[?13n");
        return;
    }
    if (p == 25) {
        // DEC STD 070 11-7: CSI ? 20 n while the keys can be loaded, CSI ? 21 n
        // once a DECUDK with Pl 0 has locked them. xterm always answers 20
        // (charproc.c), never taking the lock.
        const bool isLocked = t.udk && t.udk->locked;
        respond(t, Ubiquitty::Capability::StatusReport, isLocked ? "\033[?21n" : "\033[?20n");
        return;
    }
    if (p == 26) { // keyboard dialect 1 (NA), ready
        respond(t, Ubiquitty::Capability::StatusReport, "\033[?27;1;0;0n");
        return;
    }
    if (p == 6) {
        Ubiquitty::Screen::State* s = t.screens.active;
        int row = s->cursorY + 1;
        int column = s->cursorX + 1;
        if (t.modes.originMode) {
            row -= s->scrollTop;
            if (t.modes.leftRightMarginMode)
                column -= s->scrollLeft;
        }
        if (row < 1)
            row = 1;
        if (column < 1)
            column = 1;
        Ubiquitty::ReplyBuilder reply;
        reply.put("\033[?");
        reply.putDecimal(row);
        reply.put(';');
        reply.putDecimal(column);
        reply.put(";1R");
        if (reply.isSendable())
            respond(t, Ubiquitty::Capability::CursorStateRead, reply.span());
        return;
    }
    if (p == 7410) {
        // Ubiquitty: sound status — whether anyone is going to hear this.
        // A terminal with no synth and one whose user turned sound off both
        // consume a request in silence, so a program that wants a chime cannot
        // otherwise tell that it made no noise and should show something
        // instead. The bank's occupancy rides along: it is the same question
        // one step later, and costs no second round trip.
        const Ubiquitty::PAL::SoundStatusCallback statusOf = Ubiquitty::PAL::soundStatus();
        const Ubiquitty::PAL::SoundStatus status = statusOf ? statusOf() : Ubiquitty::PAL::SoundStatus {};
        Ubiquitty::ReplyBuilder reply;
        reply.put("\033[?7410;");
        reply.putDecimal(!status.hasSynth ? 0 : (status.isAudible ? 1 : 2));
        reply.put(';');
        reply.putDecimal(status.volumePercent);
        reply.put(';');
        reply.putDecimal(static_cast<int>(t.sampleBank ? t.sampleBank->loadedMask() : 0u));
        reply.put('n');
        if (reply.isSendable())
            respond(t, Ubiquitty::Capability::StatusReport, reply.span());
        return;
    }
    if (p == 7411) {
        // Ubiquitty: which slot holds the sample a program tagged, 0 for none.
        // The reattach question — a session outlives the program that filled
        // the bank, so "is my chime still there" cannot be answered by asking
        // whether a slot is occupied.
        const uint16_t tag = static_cast<uint16_t>(parameterOr(t, 1, 0));
        const int slot = t.sampleBank ? t.sampleBank->slotForTag(tag) : 0;
        Ubiquitty::ReplyBuilder reply;
        reply.put("\033[?7411;");
        reply.putDecimal(tag);
        reply.put(';');
        reply.putDecimal(slot);
        reply.put('n');
        if (reply.isSendable())
            respond(t, Ubiquitty::Capability::StatusReport, reply.span());
        return;
    }
    if (p == 55) { // request locator status: 50 = available, 53 = none
        // Ubiquitty fully implements the DEC locator (DECELR/DECSLE/DECEFR/
        // DECLRP below), so the answer is always "ready".
        respond(t, Ubiquitty::Capability::LocatorReport, "\033[?50n");
        return;
    }
    if (p == 56) { // request locator type: ?57;Ps n (0 none, 1 mouse, 2 tablet)
        respond(t, Ubiquitty::Capability::LocatorReport, "\033[?57;1n");
        return;
    }
    // Other ? Ps values: silently swallowed.
}

} // namespace Ubiquitty::VT
