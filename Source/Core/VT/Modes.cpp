// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Modes.cpp
//
// DEC private-mode (DECSET/DECRST) + ANSI-mode (SM/RM) appliers and the DECSTR
// soft reset. Cold relative to the parser hot loop. Each applier is a switch
// over a mode number, called from the CSI dispatch handlers in VT.cpp.

#include "Core/CellGrid.h"
#include "Core/Modes/DisplayIdle.h"
#include "Core/OSC/Dispatch.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Operations.h"
#include "Core/Tek/Tek4014.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Width.h"
#include "Core/VT/Internal.h"

namespace Ubiquitty::VT {

using ParserState = Ubiquitty::VT::Parser::State;

// The cursor (with the deferred-wrap latch) is TERMINAL state, not per-buffer
// state: an alternate-screen flip must leave it exactly where it stood.
// Ubiquitty stores the cursor on Screen, so every flip that is not itself a
// save/restore has to carry it across by hand.
static void switchScreenCarryingCursor(Ubiquitty::Terminal& t, Ubiquitty::Modes::ScreenChoice screen)
{
    const bool isAlternate = (screen == Ubiquitty::Modes::ScreenChoice::Alternate);
    if (isAlternate == t.screens.isUsingAlternateScreen)
        return;
    const Ubiquitty::Screen::State* from = t.screens.active;
    const int cursorX = from->cursorX;
    const int cursorY = from->cursorY;
    const bool wrapPending = from->wrapPending;
    Ubiquitty::Screen::applyAlternateScreenChange(t, screen);
    Ubiquitty::Screen::State* to = t.screens.active;
    to->cursorX = cursorX;
    to->cursorY = cursorY;
    to->wrapPending = wrapPending;
}

// XTSAVE: remember whether the mode is currently set, so XTRESTORE can put back
// either state. A restore with nothing saved leaves the mode alone.
void savePrivateMode(Ubiquitty::Terminal& t, int ps)
{
    if (ps <= 0 || ps > 0xFFFF)
        return;
    const uint16_t key = static_cast<uint16_t>(ps);
    const bool state = decrqmDECState(t, ps) == 1;
    int free = -1;
    for (int i = 0; i < Ubiquitty::Terminal::savedPrivateModeSlots; i++) {
        if (t.savedPrivateModeNumbers[i] == key) {
            t.savedPrivateModeStates[i] = state;
            return;
        }
        if (free < 0 && t.savedPrivateModeNumbers[i] == 0)
            free = i;
    }
    if (free < 0) { // full — drop the oldest
        for (int i = 1; i < Ubiquitty::Terminal::savedPrivateModeSlots; i++) {
            t.savedPrivateModeNumbers[i - 1] = t.savedPrivateModeNumbers[i];
            t.savedPrivateModeStates[i - 1] = t.savedPrivateModeStates[i];
        }
        free = Ubiquitty::Terminal::savedPrivateModeSlots - 1;
    }
    t.savedPrivateModeNumbers[free] = key;
    t.savedPrivateModeStates[free] = state;
}

void restorePrivateMode(Ubiquitty::Terminal& t, int ps)
{
    if (ps <= 0 || ps > 0xFFFF)
        return;
    const uint16_t key = static_cast<uint16_t>(ps);
    for (int i = 0; i < Ubiquitty::Terminal::savedPrivateModeSlots; i++) {
        if (t.savedPrivateModeNumbers[i] == key) {
            setPrivateMode(t, ps, t.savedPrivateModeStates[i] ? Ubiquitty::Modes::ModeChange::Set : Ubiquitty::Modes::ModeChange::Clear);
            return;
        }
    }
    // A mode never saved restores to reset: xterm's saved-mode record starts
    // with every mode reset, and XTRESTORE has no definition but xterm's.
    // DECANM is left alone: its reset state is VT52, which a program restoring
    // a mode it never saved did not ask for.
    if (ps != 2)
        setPrivateMode(t, ps, Ubiquitty::Modes::ModeChange::Clear);
}

void setPrivateMode(Ubiquitty::Terminal& t, int p, Ubiquitty::Modes::ModeChange change)
{
    const bool isEnabled = (change == Ubiquitty::Modes::ModeChange::Set);
    Ubiquitty::Screen::State* s = t.screens.active;
    switch (p) {
    case 1:
        t.modes.appCursorKeys = isEnabled;
        break;
    case 2:
        // DECANM. DECSET 2 = ANSI (vt52Mode=false); DECRST 2 = VT52. The parser
        // state must follow the flag — VT52 runs VT52Ground/Escape.
        t.modes.vt52Mode = !isEnabled;
        t.modes.vt52Graphics = false; // a mode switch starts in non-graphics
        t.parser.parseState = t.modes.vt52Mode ? ParserState::VT52Ground : ParserState::Ground;
        break;
    case 3:
        // DECCOLM — 80/132 column switch: reset scroll region, cursor home,
        // resize columns (row count kept). Gated by DECSET 40 (allow80To132):
        // with 40 reset, DECCOLM is ignored entirely.
        {
            if (!t.modes.allow80To132)
                break;
            int newColumns = isEnabled ? 132 : 80;
            if (newColumns != t.columns) {
                Ubiquitty::resize(t, t.rows, newColumns);
            }
            t.modes.columns132 = isEnabled;
            s->scrollTop = 0;
            s->scrollBottom = t.rows - 1;
            // The new page width invalidates any DECSLRM region: clear to full
            // width and drop DECLRMM.
            s->scrollLeft = 0;
            s->scrollRight = t.columns - 1;
            t.modes.leftRightMarginMode = false;
            // DECNCSM (?95) clears this flag to keep the existing contents, but
            // it is a VT500 feature: at a lower DECSCL operating level the
            // column change erases regardless of the flag's value.
            if (t.modes.clearScreenOnColumnChange || t.conformanceLevel < VT::ConformanceLevel::VT500)
                Ubiquitty::Screen::eraseInDisplay(t, 2);
            s->cursorX = 0;
            s->cursorY = 0;
            // Notify the frontend to re-evaluate window pixel size + atlas.
            // DECCOLM resizes the window, so it is the same host escape XTWINOPS
            // resize is and answers to the same capability.
            if (auto* c = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::WindowMutate))
                c->setColumnMode(isEnabled);
        }
        break;
    case 95:
        // DECNCSM — set ⇒ do not clear the screen on a DECCOLM column change.
        // Stored at every level; the DECCOLM site honours it only at VT500.
        t.modes.clearScreenOnColumnChange = !isEnabled;
        break;
    case 12:
        // xterm's AT&T 610 cursor blink, the flag DECSCUSR also sets (xterm
        // charproc.c dpmodes and CASE_DECSCUSR). It is reported, not drawn:
        // the reader's blink preference governs the default shape.
        t.cursorBlinkAtt610 = isEnabled;
        break;
    case 40:
        // Allow 80↔132 column switch (gates DECCOLM / ?3).
        t.modes.allow80To132 = isEnabled;
        break;
    case 30:
        // rxvt show/hide scrollbar. The bit rides the mode snapshot both macOS
        // shells read (Runtime::ModeSnapshot::isScrollbarVisible) rather than a
        // client push only one of them installed, and round-trips via DECRQM.
        // A bar follows it on the drain that delivered the bytes, which is
        // every ordinary case; a mode buried mid-flood inside the publish floor
        // waits for the next due publish, bounded and never lost.
        // Gated because taking a scroll bar away is a window mutation: the mode
        // is the effect here, so a request a policy refuses must not be stored
        // either — there is no later site left to refuse it at.
        if (Ubiquitty::effectAllowed(t, Ubiquitty::Capability::WindowMutate))
            t.scrollbarVisible = isEnabled;
        break;
    case 41:
        // xterm "more(1) fix". The deferred-wrap model is structurally immune to
        // the doubled-linefeed bug: a margin char only ARMS wrapPending, and
        // linefeed() clears it and advances exactly one row. Flag stored for DECRQM.
        t.moreFix = isEnabled;
        break;
    case 18:
        // DECPFF (Print Form Feed mode), VT220. Set ⇒ a form feed (FF) is
        // appended to the print sink after each print-screen / auto-print
        // page; reset ⇒ no terminating FF. Consumed by mediaCopyPrintScreen.
        t.printFormFeed = isEnabled;
        break;
    case 19:
        // DECPEX (Print Extent mode), VT220. Set ⇒ print the full screen;
        // reset ⇒ print only the scrolling region (between DECSTBM margins).
        // Consumed by mediaCopyPrintScreen.
        t.printExtentFullScreen = isEnabled;
        break;
    case 46:
        // Start/stop session logging. The mode is recorded and reported by
        // DECRQM, as an xterm built to toggle logging does. SECURITY: bytes
        // reach a file only through sessionLogFile, which only the frontend
        // installs, so host output can open no file; and it cannot stop a
        // user's File-menu capture (that uses sessionLogCaptureEnabled).
        t.loggingEnabled = isEnabled;
        break;
    case 35:
        // Font-shifting (rxvt). Single-font render, so no glyph effect; flag
        // round-trips through DECRQM.
        t.modes.fontShifting = isEnabled;
        break;
    case 5: // DECSCNM — reverse video. Renderer swaps fg/bg at draw time; flag
        // every row dirty so the next paint reflects it.
        if (t.modes.reverseVideo != isEnabled) {
            t.modes.reverseVideo = isEnabled;
            Ubiquitty::Screen::markAllDirty(t);
        }
        break;
    case 6: // DECOM
        t.modes.originMode = isEnabled;
        s->cursorX = 0;
        s->cursorY = isEnabled ? s->scrollTop : 0;
        Screen::cursorClampOrigin(t);
        break;
    case 38:
        // DECSET 38 — enter Tektronix 4014. DECRST 38 is not a defined exit
        // (Tek mode is left with ESC ETX / ESC CAN); only the isEnabled path acts.
        if (isEnabled)
            Ubiquitty::tekEnter(t);
        break;
    case 7400:
        // Ubiquitty-private: Tek 4014 bright-pen override (1 = on, 2 = off),
        // overriding the config default until a reset clears it.
        t.tekBrightPenOverride = isEnabled ? 1 : 2;
        break;
    case 4:
        // DECSCLM — smooth scroll. Cosmetic on a whole-frame renderer; stored
        // for DECRQM round-trip.
        t.modes.smoothScroll = isEnabled;
        break;
    case 7:
        t.modes.autowrap = isEnabled;
        break;
    case 8:
        // DECARM — auto-repeat. Host OS owns key-repeat timing; stored for DECRQM.
        t.modes.autoRepeat = isEnabled;
        break;
    case 25:
        if (!isEnabled && t.modes.isCursorVisible)
            ++t.cursorHideCount;
        t.modes.isCursorVisible = isEnabled;
        break;
    case 34:
        t.shouldWriteRightToLeft = isEnabled;
        break;
    case 96:
        t.shouldCopyRightToLeft = isEnabled;
        break;
    case 102:
        t.shouldIgnorePrinterNUL = isEnabled;
        break;
    case 36:
    case 42: {
        // DECHEM and DECNRCM select opposite states of the same setting, held
        // per display, reported by DECRQM and reset by DECSTR. It selects the
        // keyboard dialect's 7-bit set (DEC STD 070 DECNRCM), which Core has no
        // dialect for, so it changes nothing on the display: a national set
        // designates with it set or reset (designatedCharacterSet).
        const bool shouldUseNational = p == 42 ? isEnabled : !isEnabled;
        if (t.statusLineActive)
            t.isStatusNationalReplacementEnabled = shouldUseNational;
        else
            t.modes.nrcMode = shouldUseNational;
        break;
    }
    case 44:
        // Margin bell at column (columns - nMarginBell); nMarginBell defaults to 10.
        t.marginBell = isEnabled;
        break;
    case 45:
        // Reverse-wrap: Backspace / CUB at the left margin goes back to the
        // right margin of the previous row when that row soft-wrapped into
        // this one. Behavior lives in cursorBackward().
        t.modes.reverseWrap = isEnabled;
        break;
    case 1045:
        // Extended reverse wrap. 45 assists editing one long wrapped line, so it
        // re-enters only a row that soft-wrapped; 1045 is the original geometric
        // behaviour that wraps around the margin rectangle unconditionally.
        t.modes.reverseWrapExtended = isEnabled;
        break;
    case 60:
        // DECHCCM — horizontal cursor coupling. Held for DECRQM: the page is
        // never wider than the window, so there is nothing to couple.
        t.modes.isHorizontalCursorCoupled = isEnabled;
        break;
    case 61:
        // DECVCCM — vertical cursor coupling, set by default; stored inverted.
        t.modes.isVerticalCursorUncoupled = !isEnabled;
        break;
    case 64:
        // DECPCCM — page cursor coupling, set by default; stored inverted.
        t.modes.isPageCursorUncoupled = !isEnabled;
        break;
    case 66:
        // DECNKM — numeric keypad mode. Set selects the APPLICATION keypad,
        // reset the numeric one: the same state ESC = / ESC > toggle, so both
        // routes must write the one flag or a probe sees them disagree.
        t.modes.appKeypad = isEnabled;
        break;
    case 67:
        // DECBKM — backarrow key sends BS (0x08) when set, DEL (0x7F) when
        // reset. Encoding lives in the Input layer; core stores + reports via DECRQM.
        t.modes.backspaceKeySendsBackspace = isEnabled;
        break;
    case 1034:
        // eightBitInput — set (with metaSendsEscape/alternateSendsEscape off)
        // → an Alt/Meta key sets the 8th bit instead of ESC-prefixing.
        t.modes.eightBitInput = isEnabled;
        break;
    case 1036:
        // metaSendsEscape — Alt/Meta key sends ESC then the key. Takes
        // precedence over eightBitInput. Default on.
        t.modes.metaSendsEscape = isEnabled;
        break;
    case 1035:
        t.modes.shouldHonorNumLock = isEnabled;
        break;
    case 1039:
        // alternateSendsEscape — Alt-modifier analog of 1036 (macOS has a single
        // Alt/Meta modifier, so either flag triggers the ESC prefix).
        t.modes.alternateSendsEscape = isEnabled;
        break;
    case 1037:
        // deleteSendsDel — set → editing-keypad Delete sends DEL (0x7F); reset
        // (default) → VT220 Remove CSI 3 ~. Encoding lives in the Input layer.
        t.modes.deleteKeySendsDelete = isEnabled;
        break;
    case 1050: // terminfo/termcap function-key mode
        t.modes.keyboardType = isEnabled ? Keys::KeyboardType::Termcap : Keys::KeyboardType::Default;
        break;
    case 1051: // Sun function-key mode
        t.modes.keyboardType = isEnabled ? Keys::KeyboardType::Sun : Keys::KeyboardType::Default;
        break;
    case 1052: // HP function-key mode
        t.modes.keyboardType = isEnabled ? Keys::KeyboardType::HP : Keys::KeyboardType::Default;
        break;
    case 1053: // SCO function-key mode
        t.modes.keyboardType = isEnabled ? Keys::KeyboardType::SCO : Keys::KeyboardType::Default;
        break;
    case 1060: // legacy (X11R6) keyboard emulation
        t.modes.keyboardType = isEnabled ? Keys::KeyboardType::Legacy : Keys::KeyboardType::Default;
        break;
    case 1061: // VT220 keyboard emulation
        t.modes.keyboardType = isEnabled ? Keys::KeyboardType::VT220 : Keys::KeyboardType::Default;
        break;
    case 1046:
        // DECSET 1046 — gate the alternate-screen feature. Set enables
        // (default); reset disables modes 47/1047/1049 and, if currently
        // on the alternate screen, forces back to primary.
        t.modes.allowAlternateScreen = isEnabled;
        if (!isEnabled && t.screens.isUsingAlternateScreen)
            switchScreenCarryingCursor(t, Ubiquitty::Modes::ScreenChoice::Primary);
        break;
    case 47:
        // Switch only — 47 never clears the alternate buffer, on entry or exit,
        // and never moves the cursor.
        if (!t.modes.allowAlternateScreen)
            break;
        switchScreenCarryingCursor(t, isEnabled ? Ubiquitty::Modes::ScreenChoice::Alternate : Ubiquitty::Modes::ScreenChoice::Primary);
        break;
    case 1047:
        // No-op when the alternate-screen feature is gated off (DECSET 1046 reset).
        if (!t.modes.allowAlternateScreen)
            break;
        // DECRST 1047 clears the alternate buffer before returning to the
        // primary screen, so a later re-entry shows a clean alternate screen
        // rather than the previous session's stale content. The erase runs
        // while the alternate screen is still active.
        if (!isEnabled && t.screens.isUsingAlternateScreen)
            Ubiquitty::Screen::eraseInDisplay(t, 2);
        switchScreenCarryingCursor(t, isEnabled ? Ubiquitty::Modes::ScreenChoice::Alternate : Ubiquitty::Modes::ScreenChoice::Primary);
        break;
    case 81:
        t.modes.shouldReportKeyPositions = isEnabled;
        break;
    case 1048:
        if (isEnabled)
            Ubiquitty::Screen::saveCursor(t);
        else
            Ubiquitty::Screen::restoreCursor(t);
        break;
    case 1049:
        // No-op when the alternate-screen feature is gated off (DECSET 1046 reset).
        if (!t.modes.allowAlternateScreen)
            break;
        // Save the cursor, switch, and clear the alternate screen; the cursor
        // stays where it was, as xterm leaves it (charproc.c dpmodes,
        // ToAlternate). Mode 1049 is xterm's, so xterm is its authority.
        if (isEnabled) {
            Ubiquitty::Screen::saveCursor(t);
            switchScreenCarryingCursor(t, Ubiquitty::Modes::ScreenChoice::Alternate);
            Ubiquitty::Screen::eraseInDisplay(t, 2);
        } else {
            Ubiquitty::Screen::applyAlternateScreenChange(t, Ubiquitty::Modes::ScreenChoice::Primary);
            Ubiquitty::Screen::restoreCursor(t);
        }
        break;
    case 9:
    case 1000:
    case 1001:
    case 1002:
    case 1003:
    case 1005:
    case 1006:
    case 1015:
    case 1016:
        // Mouse-tracking modes + extended-coordinate encodings.
        // allowMouseOperations gates the ENABLE only; a DISABLE (DECRST) always
        // applies. These are two mutually-exclusive families (tracking
        // 9/1000/1001/1002/1003, encoding 1005/1006/1015/1016) — only the latest
        // within a family stays set, so enabling any member clears its siblings.
        // The tracking family is one value in xterm (charproc.c
        // really_set_mousemode), so resetting any member turns tracking off;
        // an encoding is reset only by its own number.
        if (isEnabled && !t.allowMouseOperations)
            break;
        if (isEnabled || p == 9 || (p >= 1000 && p <= 1003)) {
            switch (p) {
            case 9:
            case 1000:
            case 1001:
            case 1002:
            case 1003:
                t.modes.mouseX10 = false;
                t.modes.mouseButtonTracking = false;
                if ((p != 1001 || !isEnabled) && t.modes.mouseHilite) {
                    t.modes.mouseHilite = false;
                    t.modes.hiliteRegionValid = false;
                    t.modes.hiliteAwaiting = false;
                }
                t.modes.mouseCellMotion = false;
                t.modes.mouseAnyEvent = false;
                break;
            case 1005:
            case 1006:
            case 1015:
            case 1016:
                t.modes.mouseUTF8Coordinates = false;
                t.modes.mouseSGR = false;
                t.modes.mouseURXVT = false;
                t.modes.mouseSGRPixels = false;
                break;
            }
        }
        switch (p) {
        case 9:
            t.modes.mouseX10 = isEnabled;
            break;
        case 1000:
            t.modes.mouseButtonTracking = isEnabled;
            break;
        case 1001:
            // Hilite tracking. Leaving the mode (DECRST, or a sibling taking
            // over above) clears any pending region and the bounded
            // XTHIMOUSE wait.
            t.modes.mouseHilite = isEnabled;
            if (!isEnabled) {
                t.modes.hiliteRegionValid = false;
                t.modes.hiliteAwaiting = false;
            }
            break;
        case 1002:
            t.modes.mouseCellMotion = isEnabled;
            break;
        case 1003:
            t.modes.mouseAnyEvent = isEnabled;
            break;
        case 1005:
            // UTF-8 extended mouse coordinates.
            t.modes.mouseUTF8Coordinates = isEnabled;
            break;
        case 1006:
            t.modes.mouseSGR = isEnabled;
            break;
        case 1015:
            // URXVT extended mouse coordinates (decimal).
            t.modes.mouseURXVT = isEnabled;
            break;
        case 1016:
            // SGR-pixels extended mouse coordinates.
            t.modes.mouseSGRPixels = isEnabled;
            break;
        }
        break;
    case 1007:
        // Alternate scroll mode (wheel→cursor-key, realized by the frontend via
        // Modes::alternateScrollActive/Keys). Not a reporting mode, so ungated.
        t.modes.alternateScrollMode = isEnabled;
        break;
    case 1004:
        t.modes.focusReporting = isEnabled;
        break;
    case 1010:
        t.modes.scrollOnOutput = isEnabled;
        break;
    case 1011:
        t.modes.scrollOnKeypress = isEnabled;
        break;
    case 2031:
        t.modes.colorThemeReporting = isEnabled;
        break;
    case 1042:
        // Bell sets the window urgency hint (raise-urgency-on-bell, xterm).
        t.bellRaisesUrgency = isEnabled;
        break;
    case 1043:
        // Bell raises/focuses the window (pop-on-bell, xterm).
        t.popOnBell = isEnabled;
        break;
    case 1044:
        // keepClipboard (xterm) — set → a mouse selection populates the system
        // clipboard without an explicit copy. Wiring lives in the frontend.
        t.modes.keepClipboard = isEnabled;
        break;
    case 2001:
        t.modes.shouldMoveReadlinePointOnClick = isEnabled;
        break;
    case 2002:
        t.modes.shouldMoveReadlinePointBeforePaste = isEnabled;
        break;
    case 2003:
        t.modes.shouldDeleteReadlineSelection = isEnabled;
        break;
    case 97:
        t.modes.shouldSaveCRT = isEnabled;
        if (!isEnabled)
            (void)Ubiquitty::Modes::DisplayIdle::advance(t, Ubiquitty::terminalNow(t).value_or(Ubiquitty::Ticks(t.displayActivityTick)));
        break;
    case 101:
        if (isEnabled)
            t.isAnswerbackConcealed = true;
        break;
    case 5522:
        if (!isEnabled || t.allowPasteControls)
            t.modes.shouldReportPasteEvents = isEnabled;
        break;
    case 2004:
        // Bracketed-paste mode. allowPasteControls gates the ENABLE only:
        // a locked-down session refuses to wrap pasted text in the
        // paste-bracket escapes, but a DISABLE (DECRST) always applies.
        if (isEnabled && !t.allowPasteControls)
            break;
        t.modes.bracketedPaste = isEnabled;
        break;
    case 2005:
        if (!isEnabled || t.allowPasteControls)
            t.modes.shouldQuotePasteCharacters = isEnabled;
        break;
    case 2006:
        if (!isEnabled || t.allowPasteControls)
            t.modes.shouldPreservePasteNewlines = isEnabled;
        break;
    case 69:
        // DECLRMM — left/right margin mode. Enabling lets DECSLRM
        // (CSI Pl ; Pr s) install non-default left/right margins.
        // Disabling resets to full-width margins and forgets any
        // previously-set narrow region.
        t.modes.leftRightMarginMode = isEnabled;
        if (isEnabled) {
            for (int row = 0; row < t.rows; ++row) {
                t.screens.primary.lineAttribute[row] = lineAttributeNormal;
                t.screens.alternate.lineAttribute[row] = lineAttributeNormal;
            }
            Ubiquitty::Screen::markAllDirty(t);
        }
        if (!isEnabled) {
            s->scrollLeft = 0;
            s->scrollRight = t.columns - 1;
        }
        break;
    case 80:
        // DECSDM — Sixel CellGrid Mode. Set = cursor stays in place after an
        // image; reset (default) = cursor drops below it. Read in
        // sixelDecodeCompletion for cursor-advance.
        t.modes.sixelDisplayMode = isEnabled;
        break;
    case 117:
        // DECECM — erase colour mode: set, erases blank to the screen's default
        // colours; reset, to the pen's (background colour erase).
        t.modes.shouldEraseToDefaultColors = isEnabled;
        break;
    case 1070:
        // xterm private colour registers: set, each sixel starts from the default
        // palette; reset, the registers carry over from the previous image.
        t.modes.hasPrivateSixelColorRegisters = isEnabled;
        break;
    case 8452:
        // xterm: a finished sixel leaves the cursor to the right of the image.
        t.modes.shouldLeaveCursorRightOfSixel = isEnabled;
        break;
    case 7700:
        // mintty ambiguous-width reporting: CSI 1 W / CSI 2 W when the choice
        // changes. Nothing is sent on enable — the report is the change — so
        // the choice in force now is recorded as already known.
        t.modes.shouldReportAmbiguousWidth = isEnabled;
        if (isEnabled)
            t.ambiguousWidthAnnounced = static_cast<int>(t.ambiguousWidth);
        break;
    case 7723:
        // mintty rewrap on resize: reset makes every resize geometry-only.
        t.modes.shouldRewrapOnResize = isEnabled;
        break;
    case 77096:
        // foot input method editor: reset hands keys to the encoder directly,
        // past the frontend's IME, until set again.
        t.modes.allowsInputMethodEditor = isEnabled;
        break;
    case 43:
        // DECGEPM (VT340 graphics expanded print): recorded for DECRQM.
        t.modes.shouldExpandGraphicsPrint = isEnabled;
        break;
    case 68:
        // DECKBUM (VT510 keyboard usage): set = data processing keys, reset =
        // typewriter keys. The frontend's layout owns the keys; recorded for DECRQM.
        t.modes.shouldUseDataProcessingKeys = isEnabled;
        break;
    case 1014:
        // xterm fastScroll. Frames are drawn whole, so it is kept for DECRQM.
        t.modes.shouldScrollFast = isEnabled;
        break;
    case 1040:
        // xterm keepSelection. The frontend keeps the selection; kept for DECRQM.
        t.modes.shouldKeepSelection = isEnabled;
        break;
    case 1041:
        // xterm selectToClipboard. One pasteboard on every platform here; kept
        // for DECRQM.
        t.modes.shouldSelectToClipboard = isEnabled;
        break;
    case 2026:
        // Synchronized output. On isEnabled the PAL stops painting until the mode
        // clears (the grid still consumes input). On disable the rows the block
        // wrote are its damage and the next walk presents them as one frame; a
        // full-redraw request here made every synchronized frame rebuild every
        // row on every arm. A hold the watchdog expired still repaints whole
        // (Modes::SyncOutput::expire), as does DECSTR.
        if (isEnabled && !t.modes.syncOutput) {
            t.modes.syncOutput = true;
            t.modes.syncOutputTouchTicks = 0;
        } else if (!isEnabled && t.modes.syncOutput) {
            t.modes.syncOutput = false;
            t.modes.syncOutputTouchTicks = 0;
            ++t.syncOutputCloseCount;
        }
        break;
    case 2027:
        // Unicode grapheme-clustering is unconditional (the only path attaching
        // continuation codepoints to a cell); flag is cosmetic only, DECRQM
        // reports 2027 permanently set (3) regardless.
        t.modes.graphemeClustering = isEnabled;
        break;
    case 2048:
        // In-band resize notifications: every size change emits a report
        // (emitInBandResize). Enabling emits one immediately (spec's "report on
        // isEnabled") so the host learns the current size.
        t.modes.inBandResize = isEnabled;
        if (isEnabled)
            emitInBandResize(t);
        break;
    case 7401:
        // Password input mode (Ubiquitty private). An app/shell can assert it
        // directly; the heuristic path sets the same flag. The frontend shows a
        // lock indicator and masks local echo.
        t.isPasswordPromptAsserted = isEnabled;
        setPasswordPromptActive(t, isEnabled ? Ubiquitty::Modes::PasswordPrompt::Active : Ubiquitty::Modes::PasswordPrompt::Inactive);
        break;
    case 7402:
        // Edge-to-edge "fullscreen content" mode (Ubiquitty private). Core holds
        // the bit + drives the frontend seam; inset zeroing / letterbox / contain-fit
        // live in the frontend. Not cleared by softResetInternal (matching 2048/7401).
        t.modes.fullscreenContent = isEnabled;
        if (auto* c = Ubiquitty::effectClient(t, t.windowClient, Ubiquitty::Capability::WindowMutate))
            c->setEdgeToEdge(isEnabled);
        break;
    case 7403:
        // Relative pointer-lock ("mouselook") mode (Ubiquitty private). Core holds
        // the bit; the frontend does the lock/hide/warp + teardown. CLEARED by
        // softResetInternal (unlike 7401/7402) — a stranded hidden OS cursor is dangerous.
        t.modes.mouseRelativePointerLock = isEnabled;
        break;
    default:
        break;
    }
}

void setANSIMode(Ubiquitty::Terminal& t, int p, Ubiquitty::Modes::ModeChange change)
{
    const bool isEnabled = (change == Ubiquitty::Modes::ModeChange::Set);
    switch (p) {
    case 2:
        // KAM — Keyboard Action Mode. Set locks the keyboard; Core only stores
        // the state and reports it, the frontend drops input while it holds.
        t.modes.keyboardLocked = isEnabled;
        break;
    case 4:
        // IRM — Insert/Replace Mode. Set = printable chars insert and
        // shift the line right; reset = overwrite (the default).
        t.modes.insertMode = isEnabled;
        break;
    case 6:
        // ERM — Erasure Mode (ECMA-48 7.2.4). Set = erasure also reaches the
        // contents of protected areas; reset = it spares them. DECRQM keeps
        // answering 0: the mode is not one a DEC terminal reports.
        t.erasureMode = isEnabled ? VT::ErasureMode::All : VT::ErasureMode::Protect;
        break;
    case 12:
        // SRM — Send/Receive Mode. Set = no local echo (full duplex; the
        // host echoes). Reset = local echo on (the terminal displays the
        // keys it transmits). srm tracks the set state; the frontend
        // echoes typed input when srm is reset.
        t.modes.srm = isEnabled;
        break;
    case 21:
        // GRCM — Graphic Rendition Combination Mode (ECMA-48 7.2.8). Set =
        // cumulative, as every DEC terminal behaves; reset = each SGR replaces
        // the rendition. Cumulative is the state until a program resets it.
        t.renditionCombination = isEnabled ? VT::RenditionCombination::Cumulative : VT::RenditionCombination::Replacing;
        break;
    case 20:
        // LNM — Line Feed/New Line Mode. Set → LF/VT/FF act as NEL (CR+LF).
        // Cleared by default.
        t.modes.lnm = isEnabled;
        break;
    default:
        // Other ANSI modes (KAM, …) are accepted and dropped: none affect what
        // the grid holds or how it draws.
        break;
    }
}

void releaseProgramInputModes(Ubiquitty::Terminal& t)
{
    t.modes.bracketedPaste = false;
    t.modes.shouldReportPasteEvents = false;
    t.modes.shouldMoveReadlinePointOnClick = false;
    t.modes.shouldMoveReadlinePointBeforePaste = false;
    t.modes.shouldDeleteReadlineSelection = false;
    t.modes.shouldQuotePasteCharacters = false;
    t.modes.shouldPreservePasteNewlines = false;
    t.modes.focusReporting = false;
    t.modes.mouseX10 = t.modes.mouseButtonTracking = t.modes.mouseCellMotion = t.modes.mouseAnyEvent = false;
    t.modes.mouseHilite = false;
    t.modes.hiliteRegionValid = false;
    t.modes.hiliteAwaiting = false;
    t.modes.mouseSGR = t.modes.mouseUTF8Coordinates = t.modes.mouseURXVT = t.modes.mouseSGRPixels = false;
}

// Per VT510: cursor visible, autowrap, no origin mode, default scroll
// region, default SGR, default tabs. Does not clear screen.
void softResetInternal(Ubiquitty::Terminal& t)
{
    Ubiquitty::Screen::State* s = t.screens.active;
    t.modes.isCursorVisible = true;
    // Drop any app-selected DECSCUSR style back to the power-on sentinel so the
    // cursor follows the user's configured shape/blink again (RIS does this by
    // reconstructing the Terminal; DECSTR must do it explicitly).
    t.cursorStyle = Ubiquitty::Cursor::Style::FollowUserPreference;
    // xterm's ReallyReset clears the program's blink request (charproc.c).
    t.cursorBlinkAtt610 = false;
    t.modes.autowrap = true;
    t.modes.insertMode = false;
    // ERM and GRCM return to their initial states: protected areas spared, SGR cumulative.
    t.erasureMode = VT::ErasureMode::Protect;
    t.renditionCombination = VT::RenditionCombination::Cumulative;
    // Keyboard action mode returns to unlocked (VT510 DECSTR), so a program
    // that set KAM and exited cannot leave the keyboard locked.
    t.modes.keyboardLocked = false;
    t.shouldWriteRightToLeft = false;
    t.modes.originMode = false;
    t.modes.appCursorKeys = false;
    t.modes.appKeypad = false;
    t.modes.shouldReportKeyPositions = false;
    t.keyPositionReportedModifiers = 0x30;
    t.keyPositionDisabledModifiers = 0;
    // Bracketed paste, focus events and mouse tracking are xterm's, not in
    // DEC STD 070's DECSTR list, and xterm's ReallyReset clears them only for
    // RIS (charproc.c), so a soft reset leaves them as the program set them.
    // The local reset a reader asks for releases them too: see
    // releaseProgramInputModes.
    t.modes.colorThemeReporting = false;
    t.kittyKeyboardFlags[0] = t.kittyKeyboardFlags[1] = 0;
    t.kittyKeyboardStackLength[0] = t.kittyKeyboardStackLength[1] = 0;
    t.xtermKeyModifiers = VT::xtermKeyModifierInitialValues;
    t.xtermKeyFormats.fill(0);
    t.checksumExtensions = checksumInitialExtensions;
    // 7403 relative pointer lock: a soft reset must release it — a stranded
    // hidden/dissociated OS cursor is a system-wide hazard.
    t.modes.mouseRelativePointerLock = false;
    // A pending hilite (1001) handshake answers a request from before the
    // reset, so it is dropped even though the tracking mode stays.
    t.modes.hiliteRegionValid = false;
    t.modes.hiliteAwaiting = false;
    // DEC VT300 locator (DECELR/DECSLE/DECEFR) returns to its power-on
    // disabled state on a soft reset. The remembered position
    // goes too: a re-armed locator must not answer DECRQLP from before the reset.
    t.locatorEnabled = false;
    t.locatorOneShot = false;
    t.locatorUseRows = false;
    t.locatorReportDown = false;
    t.locatorReportUp = false;
    t.locatorFilterActive = false;
    t.locatorRow = 0;
    t.locatorColumn = 0;
    t.locatorButtons = 0;
    t.modes.reverseWrap = false;
    t.modes.reverseWrapExtended = false;
    s->scrollTop = 0;
    s->scrollBottom = t.rows - 1;
    // A soft reset resets all margins, not just DECSTBM's.
    t.modes.leftRightMarginMode = false;
    s->scrollLeft = 0;
    s->scrollRight = t.columns - 1;
    Ubiquitty::Screen::forgetMargins(*s);
    s->wrapPending = false;
    s->hasSavedCursor = false;
    // A soft reset exits DECSASD status-line input; writes return to the data screen.
    t.statusLineActive = 0;
    t.statusCursorX = 0;
    penReset(&s->pen);
    penApplyNormalTextColors(&s->pen, t.normalTextColors);
    // DECSTR resets the indexed palette as RIS does (xterm charproc.c:14405).
    Ubiquitty::OSC::resetIndexedColors(t);
    protectionReset(t);
    t.characterSetG[0] = characterSetUSASCII;
    t.characterSetG[1] = characterSetUSASCII;
    t.characterSetG[2] = characterSetUSASCII;
    t.characterSetG[3] = characterSetUSASCII;
    t.characterSetActive = 0;
    t.characterSetGR = 0;
    // Character set mode returns to multinational (DEC STD 070 DECSTR, VT510
    // DECSTR: DECNRCM), on the status line as on the main display.
    t.modes.nrcMode = false;
    t.isStatusNationalReplacementEnabled = false;
    t.parser.is96CharacterSet = false;
    t.singleShift = 0;
    // DECAUPSS returns to the default set (VT510 DECSTR: "Set selected in
    // Set-Up"; DEC STD 070 DECSTR: UPSS).
    t.modes.userPreferredSupplementalSet = {};
    t.modes.lnm = false;
    t.modes.srm = true;
    t.modes.autoRepeat = true; // DECARM on by default (auto-repeat enabled)
    // DECSTR preserves the DECSCL operating level (only RIS resets it). C1
    // transmission returns to the 7-bit power-on policy; DECSDM clears.
    t.transmit8bitC1 = false;
    t.modes.sixelDisplayMode = false;
    // DECSCNM is not in the DECSTR list, but leaving reverse-video on after a
    // soft reset is confusing; reset it and repaint.
    if (t.modes.reverseVideo) {
        t.modes.reverseVideo = false;
        Ubiquitty::Screen::markAllDirty(t);
    }
    // DECSET 2026 is not in the DECSTR spec, but clearing a stuck syncOutput
    // here is required: otherwise the dirty-row paint short-circuits and the
    // screen freezes until the watchdog fires. Also force a full redraw so the
    // held frame doesn't ship a stale-cursor XOR artifact.
    if (t.modes.syncOutput) {
        t.modes.syncOutput = false;
        t.modes.syncOutputTouchTicks = 0;
        if (t.grid)
            t.grid->requestFullRedraw();
    }
    // Tek mode is a byte-routing state, not a rendering one: while it is set,
    // processByteInner sends every host byte to the Tek machine and the VT
    // dispatch is unreachable. So nothing arriving over the PTY can leave it —
    // not even RIS, whose 'c' (0x63) falls in Tek's 0x60-0x6F line-type range
    // and is read as a line style. Leaving Tek is reachable only from inside the
    // Tek machine (ESC ETX / ESC CAN, or CSI ?38l), which a stream that entered
    // and stopped never sends.
    //
    // This function is the user's escape hatch — the GUI's "Reset Terminal", and
    // the host's DECSTR — and it already unwinds the other states a dead peer
    // can strand: a hidden pointer under relative-motion lock, the mouse-hilite
    // handshake, an unterminated synchronised-output frame. A pane stuck in Tek
    // renders nothing at all, so it belongs on that list.
    if (t.isTekModeActive)
        Ubiquitty::tekExit(t);
    t.tekBrightPenOverride = 0; // back to the config default on soft reset
    // Tab stops are not reset here, because DECSTR's reset list (DEC STD 070)
    // excludes them — only RIS / DECST8C / TBC touch tab stops.
}

} // namespace Ubiquitty::VT
