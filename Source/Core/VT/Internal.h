// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// VT/Internal.h
//
// Cross-TU seam declarations for the VT100 emulator split — private to the
// VT100*.cpp group, never included by frontends. VT.cpp keeps the hot
// per-byte/per-cell core; cold concerns live in sibling TUs. Helpers reachable
// across the cut are extern and declared here (at global file scope, where the
// originals lived).

#pragma once

#include "Core/CellPen.h"
#include "Core/ReplyBuilder.h"
#include "Core/Screen/CellBlank.h"
#include "Core/Screen/Operations.h"
#include "Core/TerminalOperations.h"
#include "Core/TerminalShared.h"
#include "Core/VT/Parser.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Ubiquitty::VT {

/// Reports the national replacement setting of the active display.
/// - Parameters:
///   - terminal: The terminal whose main display or status line is selected.
/// - Returns: Whether the selected display uses national replacement characters.
[[nodiscard]] bool isNationalReplacementEnabled(const Ubiquitty::Terminal& terminal);

// DA2/XTVERSION patch level advertised in Secondary Device Attributes. Kept at
// a modern xterm patch level so apps gating caps on "xterm >= N" enable the
// features Ubiquitty supports. Used by the DA2/XTVERSION handlers.
inline constexpr int dA2Version = 387;

// CharacterSet designations. 0/1/2 are the original DEC sets. 3..12 are the NRCS
// family — 96-glyph US-ASCII variants replacing ~8-12 codepoints in 0x23..0x7E
// (pure lookup, see nrcsTranslate; only differing positions carry data).
constexpr int characterSetUSASCII = 0;
constexpr int characterSetDECSpecial = 1;
constexpr int characterSetUK = 2; // DEC/ISO UK national set — '#' renders as '£'.
constexpr int characterSetFrench = 3; // ESC ( R  or  ESC ( f
constexpr int characterSetGerman = 4; // ESC ( K
constexpr int characterSetItalian = 5; // ESC ( Y
constexpr int characterSetNorwegianDanish = 6; // Norwegian/Danish: ESC ( E  or  ESC ( 6
constexpr int characterSetSwedish = 7; // ESC ( H  or  ESC ( 7
constexpr int characterSetFinnish = 8; // ESC ( C  or  ESC ( 5
constexpr int characterSetDutch = 9; // ESC ( 4
constexpr int characterSetSwiss = 10; // ESC ( =
constexpr int characterSetSpanish = 11; // ESC ( Z
constexpr int characterSetFrenchCanadian = 12; // French Canadian: ESC ( Q  or  ESC ( 9

// 96-character sets (ECMA-35 / ISO 2022). Designated with ESC - / . / / into
// G1/G2/G3 (G0 cannot hold a 96-set); they fill 0xA0..0xFF and only make sense
// once invoked into GR. Everything <= 12 above is a 94-set occupying GL.
constexpr int characterSetLatin1 = 13; // ISO 8859-1 right half: ESC - A
constexpr int characterSetDECSupplemental = 14; // DEC Supplemental Graphic: ESC - <

// DEC Technical Character Set (VT330/VT340): ESC ( > and the G1..G3 forms.
// A 94-set numbered past the 96-set block rather than beside the other 94-sets:
// 0..12 is the contiguous range the NRCS arm range-tests, and every id crosses
// the wire raw in the DECSC sync snapshot (Sync/CheckpointLedger.h, savedCharacterSet), so
// renumbering an existing set would re-key snapshots written by older peers.
constexpr int characterSetDECTechnical = 15;

// The VT500 96-sets, the right halves of ISO 8859 parts (VT510 RM, SCS),
// numbered past DEC Technical so no existing id moves.
constexpr int characterSetLatin2 = 16; // ISO 8859-2: ESC - B
constexpr int characterSetGreek = 17; // ISO 8859-7: ESC - F
constexpr int characterSetHebrew = 18; // ISO 8859-8: ESC - H
constexpr int characterSetLatinCyrillic = 19; // ISO 8859-5: ESC - L
constexpr int characterSetLatin5 = 20; // ISO 8859-9: ESC - M

/// Whether a designation is a 96-character set.
///
/// - Parameter cs: A designation id.
/// - Returns: Whether `cs` names one of the 96-sets, which fill `0xA0..0xFF`.
constexpr bool isCharacterSet96(int cs) { return cs == characterSetLatin1 || cs == characterSetDECSupplemental || (cs >= characterSetLatin2 && cs <= characterSetLatin5); }

// DRCS soft character set (DECDLD). A Gn holding this id designates t->softFont;
// resolution intercepts before the 94-/96-set tables, so the value stays clear
// of the contiguous ranges above. DRCS cells carry drcsCodePointBase (SoftFont.h).
constexpr int characterSetSoftDRCS = 100;

// ---- Character-set translation (Core/VT/CharacterSets.cpp) -----------
// Pure lookups reached only after resolveGLByte has ruled out US-ASCII, so the
// ASCII fast path never calls across this seam.

/// Maps one GL byte through a National Replacement Character Set.
///
/// Each NRCS replaces roughly 8-12 printable positions in `0x23..0x7E` with national
/// characters and matches US-ASCII everywhere else.
///
/// - Parameters:
///   - cs: An NRCS designation, `characterSetFrench` through `characterSetFrenchCanadian`.
///   - c: The GL byte to translate.
/// - Returns: The substitute code point, or `c` where the set matches ASCII.
uint16_t nrcsTranslate(int cs, uint8_t c);

/// Maps one byte through the DEC Special Graphics set.
///
/// Covers the line-drawing and symbol positions in `0x60..0x7E`.
///
/// - Parameter c: The GL byte to translate.
/// - Returns: The Unicode code point, or `c` outside the covered range.
uint16_t decSpecialToUnicode(uint8_t c);

/// Maps one byte through the DEC Technical Character Set of the VT330/VT340.
///
/// - Parameter c: The GL byte to translate.
/// - Returns: The Unicode code point, or `c` where the position is undefined.
uint16_t decTechnicalToUnicode(uint8_t c);

/// Maps one GR byte through a designated 96-character set.
///
/// The 96-sets fill `0xA0..0xFF`; a GL byte reaches here folded up into that range.
///
/// - Parameters:
///   - cs: A designation for which `isCharacterSet96` holds.
///   - c: The GR byte to translate.
/// - Returns: The Unicode code point, or `c` where the position is undefined.
uint16_t set96Translate(int cs, uint8_t c);

// ---- Cell / screen blanking + dirty tracking ------------------------

// ---- DECSASD status-display routing (VT/CSI.cpp) ----
// While the status line is the active display (DECSASD 1), one-row cursor/edit
// controls operate on statusRow/statusCursorX. Each helper returns true when it
// consumed the control (status active), false to fall through to the data screen.
/// Moves the status-line cursor to an absolute column.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - column: Zero-based target column.
/// - Returns: `true` when the status line consumed the control.
bool statusCursorMoveTo(Ubiquitty::Terminal& t, int column);
/// Moves the status-line cursor by a signed column delta.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - delta: Signed column count.
/// - Returns: `true` when the status line consumed the control.
bool statusCursorMoveBy(Ubiquitty::Terminal& t, int delta);
/// Advances the status-line cursor to the next tab stop.
///
/// - Parameter t: The terminal the control acts on.
/// - Returns: `true` when the status line consumed the control.
bool statusTab(Ubiquitty::Terminal& t);
/// Erases part of the status line, by the same mode codes as EL.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - mode: The erase mode, as the control spells it.
/// - Returns: `true` when the status line consumed the control.
bool statusEraseInLine(Ubiquitty::Terminal& t, int mode);
/// Erases part of the status line, by the mode codes of ED.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - mode: The erase mode, as the control spells it.
/// - Returns: `true` when the status line consumed the control.
bool statusEraseInDisplay(Ubiquitty::Terminal& t, int mode);
/// Applies erase to the DECSASD status line rather than the grid.
///
/// - Parameters:
///   - t: The terminal whose status line is active.
///   - n: How many characters.
/// - Returns: `true` when the status line consumed it.
bool statusEraseCharacters(Ubiquitty::Terminal& t, int n);
/// Applies insert to the DECSASD status line rather than the grid.
///
/// - Parameters:
///   - t: The terminal whose status line is active.
///   - n: How many characters.
/// - Returns: `true` when the status line consumed it.
bool statusInsertCharacters(Ubiquitty::Terminal& t, int n);
/// Applies delete to the DECSASD status line rather than the grid.
///
/// - Parameters:
///   - t: The terminal whose status line is active.
///   - n: How many characters.
/// - Returns: `true` when the status line consumed it.
bool statusDeleteCharacters(Ubiquitty::Terminal& t, int n);
/// Brings one screen up at a given size, with a blank grid and default state.
///
/// - Parameters:
///   - s: The screen to initialise.
///   - rows: Height in cells.
///   - columns: Width in cells.
void screenInit(Ubiquitty::Screen::State* s, int rows, int columns);
/// Clears a screen to blanks in the default pen, keeping its allocation.
///
/// - Parameters:
///   - s: The screen to clear.
///   - rows: Height in cells.
///   - columns: Width in cells.
void screenClearDefault(Ubiquitty::Screen::State* s, int rows, int columns);
/// Clears whichever display DECSASD has made active.
///
/// - Parameter t: The terminal the control acts on.
void mirrorClearActiveDisplay(Ubiquitty::Terminal& t);
/// Restores tab stops to one every eight columns.
///
/// - Parameter t: The terminal the control acts on.
void resetTabStops(Ubiquitty::Terminal& t);

// The DECSTR (soft reset) implementation, in VT/Modes.cpp. Declared here rather
// than kept file-static because three TUs reach it: CSI.cpp (DECSTR),
// Prefixed.cpp (DECSCL), and the public Ubiquitty::softReset in VT/Init.cpp.
/// Applies DECSTR's mode, pen, character-set and tab defaults.
///
/// - Parameter t: The terminal the control acts on.
void softResetInternal(Ubiquitty::Terminal& t);
/// Turns off bracketed paste, focus events and mouse tracking.
///
/// DECSTR leaves these xterm modes as the program set them; the reader's local
/// reset, the escape hatch for a session a dead program left wedged, clears
/// them as well.
///
/// - Parameter t: The terminal the reset acts on.
void releaseProgramInputModes(Ubiquitty::Terminal& t);

// ---- Reports / wire replies ----------------------------------------
// respond() and screenIndex() are in Core/TerminalShared.h: OSC and the Kitty
// decoder call them too, and neither is a parser concern, so their home is the
// terminal-wide seam rather than this VT-private header.
/// Reports the light or dark colour scheme the host asked for (DSR 996).
///
/// - Parameter t: The terminal the control acts on.
void reportColorScheme(Ubiquitty::Terminal& t);
/// Emits the in-band resize notification for the current geometry.
///
/// - Parameter t: The terminal the control acts on.
void emitInBandResize(Ubiquitty::Terminal& t);
/// Reports the ambiguous-width choice as mintty's CSI 1 W / CSI 2 W when it
/// differs from the one last reported.
///
/// Called at the head of each output chunk while ?7700 is set, so a program
/// that opted in re-measures its text once per change and never for the
/// choice it already knows.
///
/// - Parameter t: The terminal whose reply channel carries the report.
void announceAmbiguousWidthIfMoved(Ubiquitty::Terminal& t);

/// Emits one code point as graphic text, exactly as the parser would.
///
/// The whole graphic path — width, wrapping, cluster accumulation, the REP
/// predecessor — for a handler that holds text of its own rather than reading
/// it from the byte stream. OSC 66 is that caller: its payload carries the run
/// it sizes, and a glyph it cannot place has to reach the screen the ordinary
/// way rather than be dropped, which is what a terminal without the feature
/// already does with that text.
///
/// - Parameters:
///   - t: The terminal to write.
///   - codePoint: The scalar to emit.
void emitGraphicCodePoint(Ubiquitty::Terminal& t, std::uint32_t codePoint);

// ---- Password-prompt + DCS reset --------------------
/// Records whether the host has declared a password prompt.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - prompt: Whether a password prompt is now showing.
void setPasswordPromptActive(Ubiquitty::Terminal& t, Ubiquitty::Modes::PasswordPrompt prompt);
/// Abandons any partly-accumulated DCS string.
///
/// - Parameter t: The terminal the control acts on.
void dcsStateReset(Ubiquitty::Terminal& t);

// ---- Cross-TU seam for the cold handler split ---------
// The dispatch tables and parser hot loop stay fused in VT.cpp; cold handler
// bodies and the appliers they call live in sibling TUs. Hot helpers that stay
// in VT.cpp but are called by a moved body are promoted file-static → extern
// and declared here — not moved, only made visible across the cut.

// The CSI parameter accessor every handler uses (stays inlined in VT.cpp).
/// Reads a CSI parameter, or a default when it was omitted.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - index: Zero-based parameter position.
///   - dflt: Value to use when the parameter was omitted.
/// - Returns: The parameter's value, or `dflt` when it was omitted.
int parameterOr(const Ubiquitty::Terminal& t, int index, int dflt);
// True when parameter `index` was absent or written empty, so the control's
// default applies rather than the 0 the slot holds.
/// Answers whether a CSI parameter was absent rather than zero.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - index: Zero-based parameter position.
/// - Returns: `true` when the parameter was absent rather than zero.
bool parameterOmitted(const Ubiquitty::Terminal& t, int index);

// Cursor helpers stay fused in VT.cpp (inline-critical), exposed for the
// handler bodies that moved to sibling translation units.
/// Clamps the cursor into the grid, margins included.
///
/// - Parameter t: The terminal the control acts on.
void cursorClampFull(Ubiquitty::Terminal& t);
/// Moves the cursor left, stopping at the left margin.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - n: Repeat count.
void cursorBackward(Ubiquitty::Terminal& t, int n);
/// Moves the cursor right and clears pending wrap.
///
/// - Parameters:
///   - t: The terminal whose active display supplies the cursor and margins.
///   - count: Columns to move, clamped to the right margin or page edge.
void cursorForward(Ubiquitty::Terminal& t, int count);
/// Answers the rightmost column writing may reach.
///
/// - Parameter t: The terminal to read.
/// - Returns: The right margin when DECLRMM is on, otherwise the last column.
int effectiveRightVisible(const Ubiquitty::Terminal& t);

// Vertical motion stays fused (linefeed/reverseIndex reachable per-LF).
/// Moves down one row, scrolling the region at the bottom margin.
///
/// - Parameter t: The terminal the control acts on.
void linefeed(Ubiquitty::Terminal& t);
/// Ends OSC 133 I input or a right prompt at an explicit line break.
///
/// - Parameter terminal: The terminal whose primary-screen semantic role is updated.
void finishLineSemanticRole(Ubiquitty::Terminal& terminal);
/// Moves up one row, scrolling the region at the top margin.
///
/// - Parameter t: The terminal the control acts on.
void reverseIndex(Ubiquitty::Terminal& t);

// Screen mutation moved to Core/Screen/Operations.h (namespace Ubiquitty::Screen::State).
// repeatPrecedingCharacter stays: REP is implemented in VT.cpp beside the emit path it
// replays, not in the Screen module.
/// Replays the last emitted character `n` more times, as REP asks.
///
/// - Parameters:
///   - t: The terminal to write into.
///   - n: How many additional copies to emit.
void repeatPrecedingCharacter(Ubiquitty::Terminal& t, int n);

// DEC rectangle-area ops live in Core/Screen/Operations.h (Ubiquitty::Screen::State).

// ---- VT/Modes.cpp — DECSET/DECRST + SM/RM appliers ---------------
/// Sets or clears one DEC private mode.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - p: The mode number.
///   - change: Whether to set the mode or clear it.
void setPrivateMode(Ubiquitty::Terminal& t, int p, Ubiquitty::Modes::ModeChange change);
/// Sets or clears one ANSI mode.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - p: The mode number.
///   - change: Whether to set the mode or clear it.
void setANSIMode(Ubiquitty::Terminal& t, int p, Ubiquitty::Modes::ModeChange change);
/// Saves a DEC private mode's state for a later restore (XTSAVE).
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - ps: The mode number.
void savePrivateMode(Ubiquitty::Terminal& t, int ps);
/// Restores a DEC private mode saved by XTSAVE.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - ps: The mode number.
void restorePrivateMode(Ubiquitty::Terminal& t, int ps);

// ---- VT/Reports.cpp — DSR/DA/DECRQM/locator report helpers -------
/// Writes a CSI reply carrying one or two numeric parameters.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - prefix: Bytes between the CSI and the parameters.
///   - a: First numeric parameter.
///   - b: Second numeric parameter, or -1 to omit it.
///   - final: The sequence's final byte.
void respondCSIParameter(Ubiquitty::Terminal& t, const char* prefix, int a, int b, char final);

// Destroys this terminal's undrained sixel decodes without integrating them
// (teardown: the Terminal a completion would write into is going away).
/// Abandons undrained sixel decodes without integrating them.
///
/// - Parameter t: The terminal being torn down, or null.
void discardPendingSixelDecodes(Ubiquitty::Terminal* t);
/// Answers DECRQM's state code for an ANSI mode.
///
/// - Parameters:
///   - t: The terminal to read.
///   - ps: The mode number.
/// - Returns: The DECRQM state code for that mode.
int decrqmANSIState(const Ubiquitty::Terminal& t, int ps);
/// Answers DECRQM's state code for a DEC private mode.
///
/// - Parameters:
///   - t: The terminal to read.
///   - ps: The mode number.
/// - Returns: The DECRQM state code for that mode.
int decrqmDECState(const Ubiquitty::Terminal& t, int ps);
/// Emits the primary Device Attributes reply (DA1).
///
/// - Parameter t: The terminal the control acts on.
void emitPrimaryDA(Ubiquitty::Terminal& t);
// True when ps was a fixed-answer DEC DSR and the reply has been sent.
/// Emits a DEC DSR reply whose content does not vary.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - ps: The report number.
///   - pid: The report's identifier.
/// - Returns: `true` when a reply was emitted.
bool emitDECDSRFixedStatus(Ubiquitty::Terminal& t, int ps, int pid);
/// Writes the bare `<value><selector>` body of a DECRQSS reply.
///
/// The caller frames it: this produces only the part between the `1$r` and the
/// terminator, so a zero return is "the host asked about a setting we do not
/// report" and the caller answers `0$r` instead of framing nothing.
///
/// A truncating output is a refusal rather than a short answer. Hosts compare a
/// DECRQSS reply literally, so half a value is worse than no reply — it names a
/// setting the terminal is not in.
///
/// - Parameters:
///   - t: The terminal whose settings are reported.
///   - selector: The DECRQSS selector, without the `$q` that introduced it.
///   - output: Receives the body. A span too small for the value and its NUL
///     yields 0 rather than a truncated body, so a caller exercising that
///     refusal passes a short span on purpose and gets a defined answer.
/// - Returns: The number of bytes written, or 0 for an unrecognised selector,
///   an empty output, or a value that would not fit.
int decrqssSettingReport(const Ubiquitty::Terminal& t, std::string_view selector, Ubiquitty::ReplyBuilder& output);

/// Starts the bounded preimage journal for one graphic character.
/// - Parameter terminal: The terminal receiving the graphic.
void beginGraphicCancellation(Terminal& terminal);
/// Invalidates the graphic preimage at a control or geometry change.
/// - Parameter terminal: The terminal whose predecessor is no longer graphic.
void discardGraphicCancellation(Terminal& terminal);
/// Saves cells before placement, including their row metadata.
/// - Parameters:
///   - terminal: The terminal receiving the graphic.
///   - row: The affected zero-based row.
///   - first: The first affected column, clamped to the screen.
///   - last: The last affected column, inclusive and clamped to the screen.
void captureGraphicCells(Terminal& terminal, int row, int first, int last);
/// Saves the row displaced by a wrapping line feed and executes that feed.
/// - Parameter terminal: The terminal whose graphic wraps.
void cancellableWrapLinefeed(Terminal& terminal);
/// Restores the immediately preceding graphic's writes and cursor.
/// - Parameter terminal: The terminal receiving CCH.
void cancelPreviousGraphic(Terminal& terminal);

// ---- VT/Escape.cpp — ESC + VT52 dispatch entry points ------------
// Called by the actionHandlers table (actEscapeDispatch / actVT52Escape),
// which stays fused in VT.cpp.
/// Dispatches a completed escape sequence by its final byte.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - c: The sequence's final byte.
void escDispatch(Ubiquitty::Terminal& t, char c);
/// Dispatches a VT52-mode escape by its final byte.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - c: The sequence's final byte.
void vt52EscapeDispatch(Ubiquitty::Terminal& t, uint8_t c);

// ---- VT/Sequences/Protection.cpp — the cell-protection family ----

/// Starts an ECMA guarded area without changing DECSCA protection.
/// - Parameter t: The terminal whose pen is guarded.
void escapeH_spa(Ubiquitty::Terminal& t, char);

/// Ends an ECMA guarded area without changing DECSCA protection.
/// - Parameter t: The terminal whose pen is no longer guarded.
void escapeH_epa(Ubiquitty::Terminal& t, char);

/// Clears the current pen's protection without changing existing cells.
/// - Parameter t: The terminal being soft-reset.
void protectionReset(Ubiquitty::Terminal& t);

// ---- VT/CSI.cpp — plain (un-prefixed) CSI handler bodies ---------
// Named by the csiHandlers table + cSIJumpTable in VT.cpp; signature
// matches CSIHandler.
/// Inserts blank cells at the cursor, shifting the rest of the row right (ICH).
///
/// - Parameter t: The terminal the control acts on.
void csiH_ich(Ubiquitty::Terminal& t, char);
/// Moves the cursor up, stopping at the top margin (CUU).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cuu(Ubiquitty::Terminal& t, char);
/// Moves the cursor down, stopping at the bottom margin (CUD).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cud(Ubiquitty::Terminal& t, char);
/// Moves the cursor right, stopping at the right margin (CUF).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cuf(Ubiquitty::Terminal& t, char);
/// Moves the data position horizontally, bounded by origin mode.
///
/// Scrolling margins apply only while origin mode is set.
/// - Parameter t: The terminal the control acts on.
void csiH_hpr(Ubiquitty::Terminal& t, char);
/// Moves the data position backward by columns (HPB).
///
/// - Parameter t: The terminal whose origin mode bounds movement.
void csiH_hpb(Ubiquitty::Terminal& t, char);
/// Moves the data position backward by lines (VPB).
///
/// - Parameter t: The terminal whose origin mode bounds movement.
void csiH_vpb(Ubiquitty::Terminal& t, char);
/// Moves the data position vertically, bounded by origin mode.
///
/// Scrolling margins apply only while origin mode is set.
/// - Parameter t: The terminal the control acts on.
void csiH_vpr(Ubiquitty::Terminal& t, char);

/// Moves the cursor left, stopping at the left margin (CUB).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cub(Ubiquitty::Terminal& t, char);
/// Moves the cursor to the first column some rows down (CNL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cnl(Ubiquitty::Terminal& t, char);
/// Moves the cursor to the first column some rows up (CPL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cpl(Ubiquitty::Terminal& t, char);
/// Moves the cursor to an absolute column in the current row (CHA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cha(Ubiquitty::Terminal& t, char);
/// Moves the cursor to an absolute row and column (CUP).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cup(Ubiquitty::Terminal& t, char);
/// Advances the cursor by whole tab stops (CHT).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cht(Ubiquitty::Terminal& t, char);

/// Sets a line tabulation stop at the cursor's row.
/// - Parameter t: The terminal receiving VTS.
void escapeH_vts(Ubiquitty::Terminal& t, char);

/// Moves to the requested following line tabulation stop.
/// - Parameter t: The terminal receiving CVT.
void csiH_cvt(Ubiquitty::Terminal& t, char);

/// Applies each CTC tab-stop selector in order.
/// - Parameter t: The terminal receiving CTC.
void csiH_ctc(Ubiquitty::Terminal& t, char);
/// Erases part or all of the display (ED).
///
/// - Parameter t: The terminal the control acts on.
void csiH_ed(Ubiquitty::Terminal& t, char);
/// Erases part or all of the cursor's row (EL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_el(Ubiquitty::Terminal& t, char);
/// Erases part or all of the active field (EF).
///
/// - Parameter t: The terminal receiving EF.
void csiH_ef(Ubiquitty::Terminal& t, char);
/// Erases part or all of the active qualified area (EA).
///
/// - Parameter t: The terminal receiving EA.
void csiH_ea(Ubiquitty::Terminal& t, char);
/// Inserts blank rows at the cursor, pushing the rest down (IL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_il(Ubiquitty::Terminal& t, char);
/// Deletes rows at the cursor, pulling the rest up (DL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_dl(Ubiquitty::Terminal& t, char);
/// Deletes cells at the cursor, pulling the rest of the row left (DCH).
///
/// - Parameter t: The terminal the control acts on.
void csiH_dch(Ubiquitty::Terminal& t, char);
/// Scrolls the region up, revealing blank rows at the bottom (SU).
///
/// - Parameter t: The terminal the control acts on.
void csiH_su(Ubiquitty::Terminal& t, char);
/// Scrolls the region down, revealing blank rows at the top (SD).
///
/// - Parameter t: The terminal the control acts on.
void csiH_sd(Ubiquitty::Terminal& t, char);
/// Blanks cells at the cursor without moving the rest of the row (ECH).
///
/// - Parameter t: The terminal the control acts on.
void csiH_ech(Ubiquitty::Terminal& t, char);
/// Moves the cursor back by whole tab stops (CBT).
///
/// - Parameter t: The terminal the control acts on.
void csiH_cbt(Ubiquitty::Terminal& t, char);
/// Repeats the last printed character (REP).
///
/// - Parameter t: The terminal the control acts on.
void csiH_rep(Ubiquitty::Terminal& t, char);
/// Reports what the terminal claims to be (DA1).
///
/// - Parameter t: The terminal the control acts on.
void csiH_da(Ubiquitty::Terminal& t, char);
/// Moves the cursor to an absolute row in the current column (VPA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_vpa(Ubiquitty::Terminal& t, char);
/// Chooses what the status line displays (DECSSDT).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decssdt(Ubiquitty::Terminal& t, char);
/// Chooses whether writes land on the status line or the grid (DECSASD).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsasd(Ubiquitty::Terminal& t, char);
/// Assigns the colours of normal text or of the window frame (DECAC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decac(Ubiquitty::Terminal& t, char);
/// Assigns the colours of one attribute combination (DECATC), held for DECRQSS.
///
/// - Parameter t: The terminal the control acts on.
void csiH_decatc(Ubiquitty::Terminal& t, char);
/// Enables or disables locator position reporting (DECELR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decelr(Ubiquitty::Terminal& t, char);
/// Chooses which locator events are reported (DECSLE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsle(Ubiquitty::Terminal& t, char);
/// Reports the locator's current position (DECRQLP).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqlp(Ubiquitty::Terminal& t, char);
/// Arms the filter rectangle a locator report fires on leaving (DECEFR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decefr(Ubiquitty::Terminal& t, char);
/// Inserts blank columns across the scroll region (DECIC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decic(Ubiquitty::Terminal& t, char);
/// Deletes columns across the scroll region (DECDC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decdc(Ubiquitty::Terminal& t, char);
/// Reports the key type the host asked about (DECRQKT).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqkt(Ubiquitty::Terminal& t, char);
/// Moves the cursor to an absolute column in the current row (HPA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_hpa(Ubiquitty::Terminal& t, char);
/// Reports the terminal's serial parameters (DECREQTPARM).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decreqtparm(Ubiquitty::Terminal& t, char);
/// Clears one tab stop or all of them (TBC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_tbc(Ubiquitty::Terminal& t, char);
/// Removes the character tab stop at the specified column (TSR).
///
/// - Parameter t: The terminal whose tab stop is removed.
void csiH_tsr(Ubiquitty::Terminal& t, char);
/// Defines the alignment and target character of a named character tab stop.
/// - Parameters:
///   - t: The terminal whose stop is defined.
///   - final: The TAC, TALE, TATE or TCC final byte.
void csiH_alignedTab(Ubiquitty::Terminal& t, char final);
/// Resolves a character code through the invoked graphic sets, as printing it
/// would, without consuming a pending single shift.
///
/// - Parameters:
///   - t: The terminal whose designations and invocations apply.
///   - value: A code in 32...127 (GL) or 160...255 (GR).
/// - Returns: The code point the character prints as.
uint32_t resolveGraphicCharacter(Ubiquitty::Terminal& t, int value);
/// Ends the field started by HT without moving its text or cursor.
/// - Parameter t: The terminal whose aligned field is complete.
void finishAlignedTabString(Ubiquitty::Terminal& t);
/// Sets the column used by carriage return and implicit line starts (SLH).
///
/// - Parameter t: The terminal whose line home is set.
void csiH_slh(Ubiquitty::Terminal& t, char);
/// Sets the position carriage return and next line move to when SIMD selects the line limit (SLL).
///
/// - Parameter t: The terminal whose line limit is set.
void csiH_sll(Ubiquitty::Terminal& t, char);
/// Selects the direction of implicit movement for carriage return and next line (SIMD).
///
/// - Parameter t: The terminal whose direction is selected.
void csiH_simd(Ubiquitty::Terminal& t, char);
/// Updates one component from the other and moves to the first position (SPD).
///
/// - Parameter t: The terminal the control acts on.
void csiH_spd(Ubiquitty::Terminal& t, char);
/// Ends a string of graphic characters and positions it on its line (QUAD).
///
/// - Parameter t: The terminal the control acts on.
void csiH_quad(Ubiquitty::Terminal& t, char);
/// Sets whether subsequent text belongs to a guarded qualified area (DAQ).
///
/// - Parameter t: The terminal whose active pen receives the qualification.
void csiH_daq(Ubiquitty::Terminal& t, char);
/// Sets one or more ANSI modes (SM).
///
/// - Parameter t: The terminal the control acts on.
void csiH_sm(Ubiquitty::Terminal& t, char);
/// Clears one or more ANSI modes (RM).
///
/// - Parameter t: The terminal the control acts on.
void csiH_rm(Ubiquitty::Terminal& t, char);
/// Applies graphic rendition attributes to the pen (SGR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_sgr(Ubiquitty::Terminal& t, char);
/// Reports cursor position or terminal status (DSR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_dsr(Ubiquitty::Terminal& t, char);
/// Sets the top and bottom margins of the scroll region (DECSTBM).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decstbm(Ubiquitty::Terminal& t, char);
/// Saves the cursor position, SCO-style (SCOSC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_scosc(Ubiquitty::Terminal& t, char);
/// Restores the cursor position saved SCO-style (SCORC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_scorc(Ubiquitty::Terminal& t, char);
/// Scrolls the region left (SL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_sl(Ubiquitty::Terminal& t, char);
/// Scrolls the region right (SR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_sr(Ubiquitty::Terminal& t, char);
/// Applies a soft reset, leaving screen contents alone (DECSTR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decstr(Ubiquitty::Terminal& t, char);
/// Routes screen or line content to the printer (MC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_mc(Ubiquitty::Terminal& t, char);
/// Routes content to the printer under DEC private parameters (MC).
///
/// - Parameter t: The terminal the control acts on.
void csiH_mc_dec(Ubiquitty::Terminal& t, char);
/// Shows the next page (NP).
///
/// - Parameter t: The terminal the control acts on.
/// Shows the preceding page (PP).
///
/// - Parameter t: The terminal the control acts on.
/// Performs a window operation, or reports window geometry (XTWINOPS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtwinops(Ubiquitty::Terminal& t, char);
/// Reports the title stack's length and maximum index (XTTITLEPOS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xttitlepos(Ubiquitty::Terminal& t, char);
/// Saves the current colors.
///
/// - Parameters:
///   - t: The terminal whose colors are saved.
///   - parameter: Zero to push, or a one-based slot.
void pushColors(Ubiquitty::Terminal& t, int parameter);
/// Restores saved colors.
///
/// - Parameters:
///   - t: The terminal whose colors are restored.
///   - parameter: Zero to pop, or a one-based slot.
void popColors(Ubiquitty::Terminal& t, int parameter);
/// Stores the current palette in a bounded stack or an explicit slot.
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtpushcolors(Ubiquitty::Terminal& t, char);
/// Restores a saved palette, including its dynamic colour overrides.
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtpopcolors(Ubiquitty::Terminal& t, char);
/// Reports the palette stack position and high-water mark.
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtreportcolors(Ubiquitty::Terminal& t, char);

/// Saves the pen's named attributes (XTPUSHSGR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtpushsgr(Ubiquitty::Terminal& t, char);
/// Restores the attributes the last XTPUSHSGR saved (XTPOPSGR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtpopsgr(Ubiquitty::Terminal& t, char);
/// Applies the Linux console's setterm controls (setterm).
///
/// - Parameter t: The terminal the control acts on.
void csiH_linuxPower(Ubiquitty::Terminal& t, char);
// printSinkWriteRow stays reachable from linefeed (auto-print path).
/// Writes one grid row to the configured print sink.
///
/// - Parameters:
///   - t: The terminal the control acts on.
///   - row: Zero-based grid row.
void printSinkWriteRow(Ubiquitty::Terminal& t, int row);

/// Diverts raw printer data through the first controller-off sequence.
/// - Returns: The number of consumed bytes, including the off sequence.
[[nodiscard]] std::size_t consumePrinterBytes(Ubiquitty::Terminal& t, std::span<const char> bytes);

// ---- VT/Prefixed.cpp — prefixed-CSI handler bodies ---------------
// Named by the csiPrefixedHandlers table in VT.cpp.
/// Reports the active Kitty keyboard protocol flags (Kitty keyboard).
///
/// - Parameter t: The terminal the control acts on.
void csiH_kitty_query(Ubiquitty::Terminal& t, char);
/// Pops Kitty keyboard flags off the stack (Kitty keyboard).
///
/// - Parameter t: The terminal the control acts on.
void csiH_kitty_pop(Ubiquitty::Terminal& t, char);
/// Pushes the current Kitty keyboard flags onto the stack (Kitty keyboard).
///
/// - Parameter t: The terminal the control acts on.
void csiH_kitty_push(Ubiquitty::Terminal& t, char);
/// Sets the Kitty keyboard protocol flags (Kitty keyboard).
///
/// - Parameter t: The terminal the control acts on.
void csiH_kitty_set(Ubiquitty::Terminal& t, char);
/// Reports an xterm key-modifier resource (XTMODKEYS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtmod_query_m(Ubiquitty::Terminal& t, char);
/// Reports xterm key format resources (XTQFMTKEYS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtfmt_query_g(Ubiquitty::Terminal& t, char);
/// Sets an xterm key-modifier resource (XTMODKEYS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtmod_set_m(Ubiquitty::Terminal& t, char);
/// Sets an xterm key-modifier resource to a given level (XTMODKEYS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtmod_set_f(Ubiquitty::Terminal& t, char);
/// Restores an xterm key-modifier resource to its default (XTMODKEYS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtmod_clear_n(Ubiquitty::Terminal& t, char);
/// Reports whether an ANSI mode is set (DECRQM).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqm_ansi(Ubiquitty::Terminal& t, char);
/// Reports whether a DEC private mode is set (DECRQM).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqm_dec(Ubiquitty::Terminal& t, char);
/// Chooses the cursor's shape and whether it blinks (DECSCUSR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decscusr(Ubiquitty::Terminal& t, char);
/// Marks following cells erasable or protected (DECSCA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsca(Ubiquitty::Terminal& t, char);
/// Sets the number of lines the screen shows (DECSNLS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsnls(Ubiquitty::Terminal& t, char);
/// Sets page width to 80 or 132 columns without reflowing its contents (DECSCPP).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decscpp(Ubiquitty::Terminal& t, char);
/// Selects the report/local/disabled action for each DEC modifier key.
void csiH_decsmkr(Ubiquitty::Terminal&, char);

/// Performs a secure reset (DECSR), confirming it with DECSRC when asked.
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsr(Ubiquitty::Terminal& t, char);
/// Enables xterm title-reporting modes (XTSMTITLE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_settitlemode(Ubiquitty::Terminal& t, char);
/// Saves DEC private modes for a later restore (XTSAVE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtsave(Ubiquitty::Terminal& t, char);
/// Restores DEC private modes saved earlier (XTRESTORE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtrestore(Ubiquitty::Terminal& t, char);
/// Disables xterm title-reporting modes (XTRMTITLE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_resettitlemode(Ubiquitty::Terminal& t, char);
/// Chooses whether rectangle operations act on a stream or a block (DECSACE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsace(Ubiquitty::Terminal& t, char);
/// Selects the VT conformance level (DECSCL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decscl(Ubiquitty::Terminal& t, char);
/// Fills a rectangle with one character (DECFRA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decfra(Ubiquitty::Terminal& t, char);
/// Erases a rectangle (DECERA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decera(Ubiquitty::Terminal& t, char);
/// Erases the unprotected cells of a rectangle (DECSERA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsera(Ubiquitty::Terminal& t, char);
/// Reverses attributes across a rectangle (DECRARA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrara(Ubiquitty::Terminal& t, char);
/// Changes attributes across a rectangle (DECCARA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_deccara(Ubiquitty::Terminal& t, char);
/// Copies one rectangle over another (DECCRA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_deccra(Ubiquitty::Terminal& t, char);
/// Reports the displayed extent of the page (DECRQDE).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqde(Ubiquitty::Terminal& t, char);
/// Reports the user-preferred supplemental character set (DECRQUPSS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqupss(Ubiquitty::Terminal& t, char);
/// Reports presentation state: cursor, margins and tab stops (DECRQPSR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqpsr(Ubiquitty::Terminal& t, char);
/// Reports terminal state as a restorable string (DECRQTSR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqtsr(Ubiquitty::Terminal& t, char);
// DECRSTS body applier, called from the DCS ($ p) table in VT.cpp. Lives
// beside csiH_decrqtsr because the two share the report's field layout.
/// Applies a terminal state report restored by DECRSTS.
///
/// - Parameter t: The terminal the control acts on.
void decrstsApply(Ubiquitty::Terminal& t);
/// Reports the terminal's firmware identity (DA2).
///
/// - Parameter t: The terminal the control acts on.
void csiH_da2(Ubiquitty::Terminal& t, char);
/// Reports the terminal's name and version (XTVERSION).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtversion(Ubiquitty::Terminal& t, char);
/// Reports the terminal's unit identity (DA3).
///
/// - Parameter t: The terminal the control acts on.
void csiH_da3(Ubiquitty::Terminal& t, char);
/// Reports a checksum over a rectangle (DECRQCRA).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrqcra(Ubiquitty::Terminal& t, char);
/// Selects the XTCHECKSUM calculation flags for subsequent DECRQCRA requests.
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtchecksum(Ubiquitty::Terminal& t, char);
/// Reports the rendition every cell of a rectangle shares (XTREPORTSGR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtreportsgr(Ubiquitty::Terminal& t, char);
/// Plays a tone of a given pitch and duration (DECPS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decps(Ubiquitty::Terminal& t, char);
/// Sets one or more DEC private modes (DECSET).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decset(Ubiquitty::Terminal& t, char);
/// Clears one or more DEC private modes (DECRST).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decrst(Ubiquitty::Terminal& t, char);
/// Reports DEC-private status (DSR).
///
/// - Parameter t: The terminal the control acts on.
void csiH_dsr_dec(Ubiquitty::Terminal& t, char);
/// Sets a tab stop every eight columns (DECST8C).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decst8c(Ubiquitty::Terminal& t, char);
/// Erases the unprotected cells of the cursor's row (DECSEL).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsel(Ubiquitty::Terminal& t, char);
/// Erases the unprotected cells of the display (DECSED).
///
/// - Parameter t: The terminal the control acts on.
void csiH_decsed(Ubiquitty::Terminal& t, char);
/// Sets or reports a graphics capability limit (XTSMGRAPHICS).
///
/// - Parameter t: The terminal the control acts on.
void csiH_xtsmgraphics(Ubiquitty::Terminal& t, char);

} // namespace Ubiquitty::VT
