<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ECMA-48 terminal reference

This reference maps ECMA-48, 5th edition (June 1991), and related terminal
extensions to Core's dispatch and model behavior. The sources are
`Source/Core/VT/CSI.cpp`, `VT/Escape.cpp`, `VT/Prefixed.cpp`,
`VT/Modes.cpp`, `VT/Reports.cpp`, `VT/CharacterSets.cpp`, `VT/VT.cpp`, the
OSC handlers in `Source/Core/OSC/`, and the kitty graphics and keyboard
protocol handlers in `Source/Core/Kitty/`.

A dispatch entry is not proof of rendering, authorization, or end-to-end
transport correctness. The tables describe the selected terminal repertoire;
page-layout, printer, and communication functions outside that repertoire are
not an implementation schedule.

XTFMTKEYS (`CSI > f`) with no parameters resets all key format resources to
their initial values. With a resource and no value, it resets only that resource,
as specified by xterm's control-sequence reference.

## Checking the model with esctest2

[esctest2](https://github.com/ThomasDickey/esctest2) drives a terminal over a PTY
and uses replies such as DECRQCRA to check its model. A checksum readback is
not a rendered-pixel assertion. Use `Scripts/run-esctest.sh`; its target must
accept `-e <program> [args…]` so the suite actually runs in place of the shell.

A reproducible result records the target binary and version, suite commit,
expected-terminal profile, all option flags, and capability policy. Screen
readback is `ScreenContentRead`, a ReadExfil capability, so a denied or
unanswered Prompt can cause readback timeouts independently of terminal
semantics. Mode and attribute reports may have different capability tiers.

The following historical observations retain their original provenance:
suite `664be3c` (2025-08-24), `--expected-terminal=xterm`,
`Scripts/run-esctest.sh`, 2026-07-29. They are not a result for an unspecified
current binary.

- macOS app, local origin:

  - 517 passed
  - 3 non-conforming
  - 35 known bugs

- Daemon + browser, agent origin with the default daemon policy:

  - 272 passed
  - 255 failed

The daemon run did not record known-bug counts. For explicit, isolated
conformance configuration, see
[Host capability policy](capability-host-policy.md#conformance-test-policy).
A historical pass/fail split does not by itself attribute every failure to the
capability gate.

The suite's expected-terminal profile selects checksum conventions and
known-bug exemptions. Its `xterm` profile applies checksum negation according
to `--xterm-checksum`; `iTerm2` uses a different convention. Profile changes
therefore do not produce directly comparable counts. Options such as
`xtermWinopsEnabled` can move failures between failure and known-bug columns.
Reverse-wrap cases also depend on `--xterm-reverse-wrap`.

Keep individual disagreements visible, with their expected and actual bytes
or state. Neither a high score nor a documented difference proves that a
particular behavior is correct; check the specification and a focused
regression. Raw run logs belong with the run's recorded environment, not as
timeless source-tree status.

**Legend**

| Mark | Meaning |
|------|---------|
| ✅ | The listed syntax has a dispatch/model behavior; notes define its scope. |
| 🟡 | Partial (syntax dispatched but limited semantics, or alias-only) |
| ❌ | Outside the interpreted repertoire (no dispatch entry, or consumed without the specified effect). |
| ⏭️ | Not applicable to a terminal emulator (printer/page-layout/communication control) |

**Reference specifications.** ECMA-48 5th ed. is the primary standard for every
control function below. Where a row covers a private extension the standard does
not assign, its published wire format is:

| Surface | Specification |
|---|---|
| DEC private modes, VT reports | [DEC STD 070 / VT manuals](https://vt100.net/docs/) |
| xterm private modes, XTWINOPS, XTGETTCAP, XTVERSION | [xterm control sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html) |
| OSC 8 hyperlinks | [OSC 8 specification](https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda) |
| OSC 9 notification; OSC 1337 (inline image, CWD, marks, user variables, attention) | [iTerm2 escape codes](https://iterm2.com/documentation-escape-codes.html) |
| OSC 21337 session status (tab status) | [iTerm2 session status](https://iterm2.com/documentation-session-status.html) |
| Kitty graphics protocol (APC `_G`) | [kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/) |
| Kitty keyboard protocol (CSI `>`/`=`/`<`/`?` … `u`) | [kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/) |

---

## 1. C0 Set (00/00–01/15)

32 control characters in columns 00–01 of the 7-bit code table.

| Acronym | Code   | Name                            | Ubiquitty | Notes |
|---------|--------|---------------------------------|-----------|-------|
| NUL     | 00/00  | NULL                            | ✅ | Swallowed by parser |
| SOH     | 00/01  | start of heading                | ⏭️ | ISO 1745 transmission control |
| STX     | 00/02  | start of text                   | ⏭️ | ISO 1745 transmission control |
| ETX     | 00/03  | end of text                     | ⏭️ | ISO 1745 transmission control |
| EOT     | 00/04  | end of transmission             | ⏭️ | ISO 1745 transmission control |
| ENQ     | 00/05  | ENQUIRY                         | 🟡 | `actExecuteAnswerback` sends the configured answerback string through `respond` (`Capability::Answerback`); an empty answerback sends nothing. Not a Device Status Report; DSR is the separate CSI Ps n mechanism |
| ACK     | 00/06  | ACKNOWLEDGE                     | ⏭️ | Communication protocol |
| BEL     | 00/07  | BELL                            | ✅ | Audible/visual bell |
| BS      | 00/08  | BACKSPACE                       | ✅ | Cursor backward |
| HT      | 00/09  | CHARACTER TABULATION            | ✅ | Tab to next stop |
| LF      | 00/10  | LINE FEED                       | ✅ | Newline with scroll |
| VT      | 00/11  | LINE TABULATION                 | ✅ | Dispatched identically to LF/FF (Action::ExecuteLineFeed, honours LNM): an ordinary scrolling linefeed, not a move to the next line tabulation stop. The stops VTS sets are used by CVT (`csiH_cvt`) |
| FF      | 00/12  | FORM FEED                       | ✅ | Same as LF in terminal context |
| CR      | 00/13  | CARRIAGE RETURN                 | ✅ | Return to column 0 |
| SO      | 00/14  | shift-out (LS1)                 | ✅ | Invoke G1 into GL |
| SI      | 00/15  | shift-in (LS0)                  | ✅ | Invoke G0 into GL |
| DLE     | 01/00  | DATA LINK ESCAPE                | ⏭️ | ISO 1745 transmission control |
| DC1     | 01/01  | device control one              | ⏭️ | Ancillary device control |
| DC2     | 01/02  | device control two              | ⏭️ | Ancillary device control |
| DC3     | 01/03  | DEVICE CONTROL THREE            | ⏭️ | Ancillary device control |
| DC4     | 01/04  | DEVICE CONTROL FOUR             | ⏭️ | Ancillary device control |
| NAK     | 01/05  | NEGATIVE ACKNOWLEDGE            | ⏭️ | Communication protocol |
| SYN     | 01/06  | SYNCHRONOUS IDLE                | ⏭️ | Communication protocol |
| ETB     | 01/07  | end of transmission block       | ⏭️ | Communication protocol |
| `CAN`   | 01/08  | CANCEL                          | ✅ | Cancels the escape sequence, control sequence or control string in progress (an SOS string excepted); no effect in Ground |
| EM      | 01/09  | end of medium                   | ⏭️ | Physical medium marker |
| SUB     | 01/10  | SUBSTITUTE                      | ✅ | `actExecuteSubstitute` (Action::ExecuteSubstitute) draws the error character as a printed character: U+2426 at level 2 and above, U+2592 at VT100 level (`Source/Core/VT/Constants.h`). In Escape/CSI/DCS/OSC/PM/APC states it first abandons the sequence in progress, as `CAN` does |
| ESC     | 01/11  | ESCAPE                          | ✅ | Code extension introducer |
| IS4/FS  | 01/12  | INFORMATION SEPARATOR FOUR      | ⏭️ | File separator (data delimiters) |
| IS3/GS  | 01/13  | INFORMATION SEPARATOR THREE     | ⏭️ | Group separator |
| IS2/RS  | 01/14  | information separator two       | ⏭️ | Record separator |
| IS1/US  | 01/15  | information separator one       | ⏭️ | Unit separator |

**C0 summary**: 14/32 implemented or handled; 18/32 are transmission/communication/device-control functions not relevant to a terminal emulator.

---

## 2. C1 Set (08/00–09/15)

32 control functions in columns 08–09 of the 8-bit code table (or ESC Fe in 7-bit). The 8-bit codes act under a single-byte input encoding. Under UTF-8 input only the ESC Fe forms act: a decoded U+0085 performs NEL and any other decoded C1 code point is ignored, a raw byte 0x80–0x9F is malformed input (replacement policy), and a raw 0x9C inside an open control string is payload, not ST. Under other multibyte codings a raw 0x9C still ends an open control string.

| Acronym | 8-bit | ESC Fe | Name                            | Ubiquitty | Notes |
|---------|-------|--------|---------------------------------|-----------|-------|
| —       | 08/00 | —      | (reserved)                      | — | Reserved |
| —       | 08/01 | —      | (reserved)                      | — | Reserved |
| BPH     | 08/02 | ESC 04/02 | break permitted here            | ❌ | Text composition |
| NBH     | 08/03 | ESC 04/03 | no break here                   | ❌ | Text composition |
| —       | 08/04 | ESC D  | (reserved; formerly IND)        | — | Reserved (5th ed. removed IND — the control function coded as C1 08/04 — per Annex F.8.2). Ubiquitty still wires ESC D and 8-bit 0x84 as a legacy compatibility alias (escapeHandlers `'D'` → escapeH_ind, Action::C1Index → linefeed) |
| NEL     | 08/05 | ESC 04/05 | NEXT LINE                       | ✅ | CR+LF in one |
| SSA     | 08/06 | ESC 04/06 | start of selected area          | ❌ | Area definition |
| ESA     | 08/07 | ESC 04/07 | end of selected area            | ❌ | Area definition |
| HTS     | 08/08 | ESC 04/08 | CHARACTER TABULATION SET        | ✅ | Set tab stop |
| HTJ     | 08/09 | ESC 04/09 | CHAR TABULATION WITH JUSTIFICATION | ✅ | Shifts field contents to end before the next tab stop; erased trailing cells are excluded |
| VTS | 08/10 | ESC 04/10 | LINE TABULATION SET | ✅ | Sets a line tabulation stop; both forms |
| PLD     | 08/11 | ESC 04/11 | PARTIAL LINE FORWARD (subscript) | ❌ | Subscript/superscript |
| PLU     | 08/12 | ESC 04/12 | PARTIAL LINE BACKWARD (superscript) | ❌ | Subscript/superscript |
| RI      | 08/13 | ESC 04/13 | REVERSE LINE FEED               | ✅ | Reverse index |
| SS2     | 08/14 | ESC 04/14 | single-shift two                | ✅ | ISO 2022 code extension |
| SS3     | 08/15 | ESC 04/15 | SINGLE-SHIFT THREE              | ✅ | ISO 2022 code extension |
| DCS     | 09/00 | ESC 05/00 | DEVICE CONTROL STRING           | ✅ | DCS opener |
| PU1     | 09/01 | ESC 05/01 | private use one                 | ❌ | Private use |
| PU2     | 09/02 | ESC 05/02 | private use two                 | ❌ | Private use |
| STS     | 09/03 | ESC 05/03 | SET TRANSMIT STATE              | ⏭️ | Communication |
| CCH     | 09/04 | ESC 05/04 | CANCEL CHARACTER                | ✅ | Cancels the preceding graphic, restoring overwritten cells, insertion shifts, wrapping, and scrolling |
| MW      | 09/05 | ESC 05/05 | MESSAGE WAITING                 | ❌ | Set message waiting indicator |
| SPA     | 09/06 | ESC 05/06 | start of guarded area           | ✅ | `escapeH_spa` / `actC1StartGuardedArea` (7-bit ESC V and 8-bit 0x96) adds `ProtectedMode::ISO` and sets the pen's guarded flag (`penSetGuarded`); ED/EL/ECH/EF step over guarded cells (`Source/Core/Screen/Erase.cpp`) |
| EPA     | 09/07 | ESC 05/07 | end of guarded area             | ✅ | `escapeH_epa` / `actC1EndGuardedArea` (7-bit ESC W and 8-bit 0x97) clears the pen's guarded flag and keeps `ProtectedMode::ISO`, so cells SPA guarded go on surviving erasure |
| SOS     | 09/08 | ESC 05/08 | start of string                 | 🟡 | Accepted as control string delimiter, content ignored; only ST ends it |
| —       | 09/09 | —      | (reserved)                      | — | Reserved |
| SCI     | 09/10 | ESC 05/10 | SINGLE CHARACTER INTRODUCER     | 🟡 | Code point repurposed as DEC-private DECID: 8-bit 0x9A (Action::C1Identify) and 7-bit ESC Z both call emitPrimaryDA — real, working DA1 response, not the reserved SCI function itself |
| CSI     | 09/11 | ESC 05/11 | CONTROL SEQUENCE INTRODUCER     | ✅ | Foundation of all CSI dispatch |
| ST      | 09/12 | ESC 05/12 | STRING TERMINATOR               | ✅ | Terminates control strings |
| OSC     | 09/13 | ESC 05/13 | OPERATING SYSTEM COMMAND        | ✅ | OSC opener |
| PM      | 09/14 | ESC 05/14 | PRIVACY MESSAGE                 | 🟡 | Accepted, content ignored |
| APC     | 09/15 | ESC 05/15 | APPLICATION PROGRAM COMMAND     | ✅ | Fully parsed and dispatched: actAPCPut/actAPCEnd accumulate the payload and, when the per-introducer gate (`OSC::apcAllowed`) admits it, call Ubiquitty::Kitty::dispatch (the Kitty graphics-protocol handler) — content is used, not ignored |

**C1 summary**: 15/32 implemented; 3/32 partial (SOS/PM — string delimiters parsed but content ignored; SCI — code point repurposed for DEC-private DECID rather than the reserved SCI function); 9/32 not implemented (text-composition, area-definition, partial-line, private-use, and message-waiting functions); 1/32 not applicable to a terminal emulator (STS — communication protocol); 4/32 reserved/unallocated (08/00, 08/01, 08/04, 09/09).

---

## 3. Independent Control Functions (ESC Fs)

2-character escape sequences ESC 06/00–07/14. Table 5 of ECMA-48.

| Acronym | Code   | Name                            | Ubiquitty | Notes |
|---------|--------|---------------------------------|-----------|-------|
| DMI     | ESC 06/00 | DISABLE MANUAL INPUT            | ❌ | Keyboard enable/disable |
| INT     | ESC 06/01 | INTERRUPT                       | ❌ | Process interrupt |
| EMI     | ESC 06/02 | ENABLE MANUAL INPUT             | ❌ | Keyboard enable/disable |
| RIS     | ESC 06/03 | reset to initial state          | ✅ | `ESC c` — full reset |
| CMD     | ESC 06/04 | CODING METHOD DELIMITER         | ❌ | Code extension delimiting |
| LS2     | ESC 06/14 | locking-shift two               | ✅ | ISO 2022, G2→GL |
| LS3     | ESC 06/15 | LOCKING-SHIFT THREE             | ✅ | ISO 2022, G3→GL |
| LS3R    | ESC 07/12 | LOCKING-SHIFT THREE RIGHT       | ✅ | ISO 2022, G3→GR |
| LS2R    | ESC 07/13 | locking-shift two right         | ✅ | ISO 2022, G2→GR |
| LS1R    | ESC 07/14 | locking-shift one right         | ✅ | ISO 2022, G1→GR |

**Independent control function summary**: 6/10 implemented (all ISO 2022 shifts + RIS); 4/10 missing (DMI, INT, EMI, CMD — manual-input, interrupt and coding-method functions).

---

## 4. ECMA-48 CSI Final Bytes (Table 3 — no intermediate byte)

From Table 3 of ECMA-48. Final bytes 04/00–06/15 in column/row pairs. Does not include the private-use finals 07/00–07/14.

### 4.1 Cursor Movement

| Acronym | Final | ECMA-48 § | Name                     | Ubiquitty | Notes |
|---------|-------|-----------|--------------------------|-----------|-------|
| ICH     | @     | 8.3.64    | INSERT CHARACTER          | ✅ | CSI Pn @ |
| CUU     | A     | 8.3.22    | cursor up                 | ✅ | CSI Pn A |
| CUD     | B     | 8.3.19    | CURSOR DOWN               | ✅ | CSI Pn B |
| CUF     | C     | 8.3.20    | CURSOR RIGHT              | ✅ | CSI Pn C |
| CUB     | D     | 8.3.18    | CURSOR LEFT               | ✅ | CSI Pn D |
| CNL     | E     | 8.3.12    | CURSOR NEXT LINE          | ✅ | CSI Pn E |
| CPL     | F     | 8.3.13    | CURSOR PRECEDING LINE     | ✅ | CSI Pn F |
| CHA     | G     | 8.3.9     | CURSOR CHARACTER ABSOLUTE | ✅ | CSI Pn G |
| CUP     | H     | 8.3.21    | CURSOR POSITION           | ✅ | CSI Pn;Pn H |
| CHT     | I     | 8.3.10    | CURSOR FORWARD TABULATION | ✅ | CSI Pn I |
| CTC | W | 8.3.17 | CURSOR TABULATION CONTROL | ✅ | Selectors 0–6; TSM is permanently reset (MULTIPLE: one set of character stops for every line, DECRQM reports 4), so 4 clears all of them, as 5 does |
| CVT | Y | 8.3.23 | CURSOR LINE TABULATION | ✅ | Advances to following line tabulation stops, preserving the column |
| CBT     | Z     | 8.3.7     | CURSOR BACKWARD TABULATION| ✅ | CSI Pn Z |

### 4.2 Erase & Editing

| Acronym | Final | ECMA-48 § | Name                     | Ubiquitty | Notes |
|---------|-------|-----------|--------------------------|-----------|-------|
| ED      | J     | 8.3.39    | erase in page             | ✅ | CSI Ps J |
| EL      | K     | 8.3.41    | erase in line             | ✅ | CSI Ps K |
| IL      | L     | 8.3.67    | INSERT LINE               | ✅ | CSI Pn L |
| DL      | M     | 8.3.32    | DELETE LINE               | ✅ | CSI Pn M |
| DCH     | P     | 8.3.26    | DELETE CHARACTER          | ✅ | CSI Pn P |
| ECH     | X     | 8.3.38    | ERASE CHARACTER           | ✅ | CSI Pn X |
| EF      | N     | 8.3.40    | erase in field            | ✅ | Selectors 0–2 erase within the two tab stops bounding the active field; guarded cells are kept unless ERM is set |
| EA      | O     | 8.3.37    | erase in area             | ✅ | CSI Ps O (04/15); the area is the span from one DAQ to the next, with DCSM in PRESENTATION; honours ERM |

### 4.3 Scroll & Page

| Acronym | Final | ECMA-48 § | Name                     | Ubiquitty | Notes |
|---------|-------|-----------|--------------------------|-----------|-------|
| SU      | S     | 8.3.147   | scroll up                 | ✅ | CSI Pn S |
| SD      | T     | 8.3.113   | SCROLL DOWN               | ✅ | CSI Pn T; five or more parameters are XTHIMOUSE's reply, not SD |
| NP      | U     | 8.3.87    | NEXT PAGE                 | ❌ | CSI Pn U — ignored: display memory is the one page on screen, and a terminal with one page ignores NP (VT510 RM) |
| PP      | V     | 8.3.95    | PRECEDING PAGE            | ❌ | CSI Pn V — ignored, as NP is |

### 4.4 Position Reporting & Addressing

| Acronym | Final | ECMA-48 § | Name                     | Ubiquitty | Notes |
|---------|-------|-----------|--------------------------|-----------|-------|
| HPA     | `     | 8.3.57    | CHARACTER POSITION ABSOLUTE | ✅ | CSI Pn \` — column absolute |
| HPR     | a     | 8.3.59    | CHARACTER POSITION FORWARD | ✅ | CSI Pn a — unlike CUF, not bounded by the right margin; stops at the screen edge (at the margins under DECOM) |
| REP     | b     | 8.3.103   | REPEAT                    | ✅ | CSI Pn b; an explicit 0 repeats nothing |
| DA      | c     | 8.3.24    | DEVICE ATTRIBUTES         | ✅ | CSI Ps c (DA1; a nonzero Ps draws no reply). DA2 (CSI > c) and DA3 (CSI = c) are in csiPrefixedHandlers |
| VPA     | d     | 8.3.158   | LINE POSITION ABSOLUTE    | ✅ | CSI Pn d — row absolute |
| VPR     | e     | 8.3.160   | LINE POSITION FORWARD     | ✅ | CSI Pn e — unlike CUD, not bounded by the scroll region; stops at the last line (at the bottom margin under DECOM) |
| HVP     | f     | 8.3.63    | character and line position | ✅ | CSI Pn;Pn f (alias for CUP) |
| TBC     | g     | 8.3.154   | TABULATION CLEAR          | ✅ | CSI Ps g |
| SM      | h     | 8.3.125   | SET MODE                  | ✅ | CSI Ps h — KAM (2), IRM (4), SRM (12), LNM (20); other ANSI modes are accepted and dropped |
| MC      | i     | 8.3.82    | MEDIA COPY                | ✅ | CSI Ps i — 0 prints the screen (the scroll region unless DECPEX is set) via mediaCopyPrintScreen to Terminal::printSinkFile; 5 and 4 start and stop the printer controller, which passes data to the sink (consumePrinterBytes) |
| HPB     | j     | 8.3.58    | CHARACTER POSITION BACKWARD | ✅ | CSI Pn j (06/10) |
| VPB     | k     | 8.3.159   | LINE POSITION BACKWARD    | ✅ | CSI Pn k (06/11) |
| RM      | l     | 8.3.106   | RESET MODE                | ✅ | CSI Ps l — the modes SM sets |
| SGR     | m     | 8.3.117   | SELECT GRAPHIC RENDITION  | ✅ | CSI Ps m (full ECMA-48 + extensions) |
| DSR     | n     | 8.3.35    | DEVICE STATUS REPORT      | ✅ | CSI Ps n — 5 replies CSI 0 n; 6 replies CPR, relative to the margins under DECOM |
| DAQ     | o     | 8.3.25    | DEFINE AREA QUALIFICATION | 🟡 | Records each qualified area; selectors 1 and 8 protect against erasure; local field input qualifications are unsupported |

### 4.5 Other ECMA-48 Table 3 Finals (no ECMA-48 handler in Ubiquitty `csiHandlers`)

These Table 3 functions have no handler of their own in csiHandlers and are not listed above:

| Acronym | Final | ECMA-48 § | Name                          | Ubiquitty | Notes |
|---------|-------|-----------|-------------------------------|-----------|-------|
| SEE     | Q     | 8.3.115   | SELECT EDITING EXTENT         | ❌ | CSI Ps Q (05/01) |
| CPR     | R     | 8.3.14    | ACTIVE POSITION REPORT        | 🟡 | Send-only (DSR response), not received |
| SRS     | [     | 8.3.137   | START REVERSED STRING         | ❌ | CSI Ps [ (05/11) |
| PTX     | \     | 8.3.99    | PARALLEL TEXTS                | ❌ | CSI Ps \ (05/12) |
| SDS     | ]     | 8.3.114   | START DIRECTED STRING         | ❌ | CSI Ps ] (05/13) — slot taken by csiH_linuxPower (Linux console 9, 13, 14); SDS's 0–2 are ignored |
| SIMD    | ^     | 8.3.120   | SELECT IMPLICIT MOVEMENT DIRECTION | ✅ | CSI Ps ^ (05/14): 1 sends CR and NEL to the line limit set by SLL, 0 to the line home; no data moves |

### 4.6 ECMA-48 CSI with Intermediate Byte (Table 4 — CSI 02/00 F)

These are the ECMA-48 CSI final bytes with intermediate byte 02/00 (Table 4):

| Acronym | Encoding | ECMA-48 § | Name                          | Ubiquitty | Notes |
|---------|----------|-----------|-------------------------------|-----------|-------|
| SL      | CSI 02/00 04/00 | 8.3.121 | SCROLL LEFT | ✅ | CSI (SPACE) @ in csiPrefixedHandlers — intermediate byte is SPACE (02/00), not apostrophe |
| SR      | CSI 02/00 04/01 | 8.3.135 | SCROLL RIGHT | ✅ | CSI (SPACE) A in csiPrefixedHandlers — intermediate byte is SPACE (02/00), not apostrophe |
| GSM     | CSI 02/00 04/02 | 8.3.55 | GRAPHIC SIZE MODIFICATION | ❌ | |
| GSS     | CSI 02/00 04/03 | 8.3.56 | GRAPHIC SIZE SELECTION | ❌ | |
| FNT     | CSI 02/00 04/04 | 8.3.53 | FONT SELECTION | ❌ | |
| TSS     | CSI 02/00 04/05 | 8.3.157 | THIN SPACE SPECIFICATION | ❌ | |
| JFY     | CSI 02/00 04/06 | 8.3.73 | JUSTIFY | ❌ | |
| SPI     | CSI 02/00 04/07 | 8.3.132 | SPACING INCREMENT | ❌ | |
| QUAD    | CSI 02/00 04/08 | 8.3.102 | QUAD | 🟡 | Flush to the line home (0), centre (2) and flush to the line limit (4); leaders (1, 3, 5) and both margins (6) end the string without moving it |
| SSU     | CSI 02/00 04/09 | 8.3.139 | SELECT SIZE UNIT | ❌ | |
| PFS     | CSI 02/00 04/10 | 8.3.91 | PAGE FORMAT SELECTION | ❌ | |
| SHS     | CSI 02/00 04/11 | 8.3.118 | SELECT CHARACTER SPACING | ❌ | |
| SVS     | CSI 02/00 04/12 | 8.3.149 | SELECT LINE SPACING | ❌ | |
| IGS     | CSI 02/00 04/13 | 8.3.66 | IDENTIFY GRAPHIC SUBREPERTOIRE | ❌ | |
| IDCS    | CSI 02/00 04/15 | 8.3.65 | IDENTIFY DEVICE CONTROL STRING | ❌ | |
| PPA     | CSI 02/00 05/00 | 8.3.96 | PAGE POSITION ABSOLUTE | ❌ | ignored, as NP is |
| PPR     | CSI 02/00 05/01 | 8.3.98 | PAGE POSITION FORWARD | ❌ | ignored, as NP is |
| PPB     | CSI 02/00 05/02 | 8.3.97 | PAGE POSITION BACKWARD | ❌ | ignored, as NP is |
| SPD     | CSI 02/00 05/03 | 8.3.130 | SELECT PRESENTATION DIRECTIONS | 🟡 | Ps2 of 1 or 2 moves to the first position of the first line; the grid keeps its horizontal, top-to-bottom, left-to-right directions |
| DTA     | CSI 02/00 05/04 | 8.3.36 | DIMENSION TEXT AREA | ❌ | |
| SLH     | CSI 02/00 05/05 | 8.3.122 | SET LINE HOME | ✅ | Establishes the line home without moving the cursor |
| SLL     | CSI 02/00 05/06 | 8.3.123 | SET LINE LIMIT | ✅ | Establishes the line limit CR and NEL move to when SIMD is 1, without moving the cursor |
| FNK     | CSI 02/00 05/07 | 8.3.52 | FUNCTION KEY | ❌ | |
| SPQR    | CSI 02/00 05/08 | 8.3.134 | select print quality and rapidity | ❌ | |
| SEF     | CSI 02/00 05/09 | 8.3.116 | sheet eject and feed | ❌ | |
| PEC     | CSI 02/00 05/10 | 8.3.90 | presentation expand or contract | ❌ | |
| SSW     | CSI 02/00 05/11 | 8.3.140 | SET SPACE WIDTH | ❌ | |
| SACS    | CSI 02/00 05/12 | 8.3.107 | SET ADDITIONAL CHARACTER SEPARATION | ❌ | |
| SAPV    | CSI 02/00 05/13 | 8.3.108 | SELECT ALT PRESENTATION VARIANTS | ❌ | |
| STAB    | CSI 02/00 05/14 | 8.3.144 | SELECTIVE TABULATION | ❌ | |
| GCC     | CSI 02/00 05/15 | 8.3.54 | GRAPHIC CHARACTER COMBINATION | ❌ | |
| TATE    | CSI 02/00 06/00 | 8.3.153 | TABULATION ALIGNED TRAILING EDGE | ✅ | Sets an aligned stop at column Pn; HT aligns the following string; HT, CR and NEL end it |
| TALE    | CSI 02/00 06/01 | 8.3.152 | TABULATION ALIGNED LEADING EDGE | ✅ | Sets an aligned stop at column Pn; HT aligns the following string; HT, CR and NEL end it |
| TAC     | CSI 02/00 06/02 | 8.3.151 | TABULATION ALIGNED CENTRED | ✅ | Sets an aligned stop at column Pn; HT aligns the following string; HT, CR and NEL end it |
| TCC     | CSI 02/00 06/03 | 8.3.155 | TABULATION CENTRED ON CHARACTER | ✅ | Sets a stop at column Pn centred on character Pn2 (default SPACE); HT aligns the following string; HT, CR and NEL end it |
| TSR     | CSI 02/00 06/04 | 8.3.156 | TABULATION STOP REMOVE | ✅ | Removes the specified character stop; preserves the cursor and other stops |
| SCO     | CSI 02/00 06/05 | 8.3.110 | SET CHARACTER ORIENTATION | ❌ | |
| SRCS    | CSI 02/00 06/06 | 8.3.136 | SET REDUCED CHARACTER SEPARATION | ❌ | |
| SCS     | CSI 02/00 06/07 | 8.3.112 | SET CHARACTER SPACING | ❌ | |
| SLS     | CSI 02/00 06/08 | 8.3.124 | SET LINE SPACING | ❌ | |
| SPH     | CSI 02/00 06/09 | 8.3.131 | SET PAGE HOME | ❌ | |
| SPL     | CSI 02/00 06/10 | 8.3.133 | SET PAGE LIMIT | ❌ | |
| SCP     | CSI 02/00 06/11 | 8.3.111 | SELECT CHARACTER PATH | ❌ | |

---

## 5. SGR Parameters (ECMA-48 §8.3.117)

SGR parameter handling (`Ubiquitty::SGR::apply`, `Source/Core/SGR.cpp`):

| Ps | ECMA-48 Name | Ubiquitty | Notes |
|----|--------------|-----------|-------|
| 0  | Default rendition | ✅ | Reset all attributes |
| 1  | Bold / increased intensity | ✅ | Bare `1`; `1:n` is variable weight (below) |
| 2  | Faint, decreased intensity / second colour | ✅ | |
| 3  | Italicized | ✅ | |
| 4  | Singly underlined | ✅ | Bare `4`; `4:n` selects a style (below) |
| 5  | Slowly blinking | ✅ | |
| 6  | Rapidly blinking | ✅ | Blink plus the BlinkFast flag; reported back as 6 |
| 7  | Negative image | ✅ | |
| 8  | Concealed characters | ✅ | |
| 9  | Crossed-out | ✅ | |
| 10 | Primary (default) font | ✅ | Also clears Fraktur (20) |
| 11–19 | Alternative fonts | ✅ | Every slot folds to one alternate-font state (fontAlternate); the slot number is not stored |
| 20 | Fraktur (Gothic) | ✅ | Carried as a distinct font state |
| 21 | Doubly underlined | ✅ | Double underline, not an intensity control |
| 22 | Normal colour / normal intensity | ✅ | Clears bold, faint and the 1:n weight |
| 23 | Not italicized, not fraktur | ✅ | Resets Fraktur to primary; an 11–19 selection is left |
| 24 | Not underlined | ✅ | |
| 25 | Steady (not blinking) | ✅ | Clears both blink rates |
| 26 | Reserved (proportional spacing, CCITT T.61) | ✅ | Reserved in ECMA-48; stored as the pen's proportional bit (penSetProportional), not rendered; 50 and 0 clear it |
| 27 | Positive image | ✅ | |
| 28 | Revealed characters | ✅ | |
| 29 | Not crossed out | ✅ | |
| 30–37 | Foreground colours | ✅ | 30-37 + bright 90-97 |
| 38 | Reserved (ISO 8613-6/T.416 foreground) | ✅ | 38:2:r:g:b, 38:2::r:g:b (or 38;2;r;g;b) truecolor, or 38:5:n (or 38;5;n) palette index (parseExtendedColor); a component or index outside 0–255 leaves the colour unchanged |
| 39 | Default foreground colour | ✅ | |
| 40–47 | Background colours | ✅ | 40-47 + bright 100-107 |
| 48 | Reserved (ISO 8613-6/T.416 background) | ✅ | Same colon-parametric as 38 |
| 49 | Default background colour | ✅ | |
| 50 | Reserved (cancel 26) | ✅ | Clears the proportional bit set by 26 |
| 51 | Framed | ✅ | |
| 52 | Encircled | ✅ | |
| 53 | Overlined | ✅ | |
| 54 | Not framed, not encircled | ✅ | |
| 55 | Not overlined | ✅ | |
| 56 | Reserved | ✅ | No-op (reserved in ECMA-48): no dispatch case, no pen effect (`Tests/Core/VT/test_vt100_sgr.cpp:testRetiredFillSelectorsSelectNothing`); its colon run is consumed like any unknown selector's (`testASelectorTakesItsColonRunWithIt`) |
| 57 | Reserved | ✅ | No-op (reserved in ECMA-48), as 56; same tests |
| 58–59 | Reserved | ✅ | Reserved in ECMA-48; used as underline colour — see "Beyond ECMA-48" below, not a no-op |
| 60–64 | Ideogram underline/overline/stress | ✅ | SGR.cpp ideogramForCode/penSetIdeogram (one mutually exclusive state), Rendering/CellInstanceBuilder.cpp decoration geometry |
| 65 | Cancel ideogram attributes | ✅ | SGR.cpp ideogramForCode maps 65 to ideogramNone |
| **Beyond ECMA-48** | | | |
| 4:0–4:5 | Colon-underline styles | ✅ | 4:0=none, 1:single, 2:double, 3:curly, 4:dotted, 5:dashed; any other style draws single |
| 58 | Underline colour | ✅ | Same 5:n / 2:r:g:b grammar as 38/48 (parseExtendedColor into underlineIndex/Red/Green/Blue) |
| 59 | Default underline colour | ✅ | Underline follows the text colour |
| 1:n | Variable font weight (0–7) | ✅ | applyWeightSelector clamps n to 0–7 and stores it in extendedAttributes; orthogonal to bold; 22 and 0 clear it |
| 73 | Superscript | ✅ | |
| 74 | Subscript | ✅ | |
| 75 | Script default | ✅ | |
| 76 | Emoji forcing | ✅ | 76:1 colour, 76:2 text, 76:0 or bare 76 automatic (applyEmojiPresentationSelector) |

**SGR interpretation:** the table includes defined parameters, reserved selectors, and extensions; consuming a reserved value is not implementing an additional standard function. Reserved 56/57 and any unknown parameter are ignored together with their colon sub-parameters; reserved 26/50 set and clear a stored proportional bit that nothing renders; reserved 38/48 drive ISO 8613-6/T.416 colour, and reserved 58/59 the underline-colour extension. Beyond the base standard, Ubiquitty additionally implements colon-underline styles (4:0–4:5), variable font weight (1:n), and superscript/subscript/script-default/emoji-forcing (73–76).

---

## 6. ANSI Modes (ECMA-48 §7.2 — SM/RM parameters)

| Ps | Mode | Name | Ubiquitty | Notes |
|----|------|------|-----------|-------|
| 1  | GATM | GUARDED AREA TRANSFER MODE | ❌ | Guarded-area transfer; DECRQM reports 4 (permanently reset) |
| 2  | KAM | KEYBOARD ACTION MODE | 🟡 | setANSIMode case 2 stores `t.modes.keyboardLocked`, DECRQM reports 1/2, DECSTR clears it; typed input is not discarded while it is set; the only effect is suspending readline click-to-move (PointerLadder.cpp isReadlinePointerEligible) |
| 3  | CRM | CONTROL REPRESENTATION MODE | ❌ | Recognised, never entered: DECRQM reports 2 even after set; controls keep being performed |
| 4  | IRM | INSERTION REPLACEMENT MODE | ✅ | Insert/replace |
| 5  | SRTM | STATUS REPORT TRANSFER MODE | ❌ | Diagnostic mode; DECRQM reports 4 |
| 6  | ERM | ERASURE MODE | ✅ | Set erases guarded areas too (ED, EL, ECH, EF, EA); DECRQM answers 0 |
| 7  | VEM | LINE EDITING MODE | ❌ | Line insert/delete direction; DECRQM reports 4 |
| 8  | BDSM | BI-DIRECTIONAL SUPPORT MODE | ❌ | BiDi support; DECRQM reports 0 |
| 9  | DCSM | DEVICE COMPONENT SELECT MODE | ❌ | Data vs presentation component; DECRQM reports 0 |
| 10 | HEM | CHARACTER EDITING MODE | ❌ | Character insert/delete direction; DECRQM reports 4 |
| 11 | PUM | POSITIONING UNIT MODE | ❌ | Character vs SIZE UNIT positioning, deprecated (Annex F.4.1); DECRQM reports 4 |
| 12 | SRM | SEND/RECEIVE MODE | ✅ | `t.modes.srm`, set at power-on and reset; reset turns on local echo (KeyEncode.h shouldEchoInputLocally); queryable via DECRQM |
| 13 | FEAM | FORMAT EFFECTOR ACTION MODE | ❌ | Execute vs store formatters; DECRQM reports 4 |
| 14 | FETM | FORMAT EFFECTOR TRANSFER MODE | ❌ | Formatter transfer mode; DECRQM reports 4 |
| 15 | MATM | MULTIPLE AREA TRANSFER MODE | ❌ | Area transfer; DECRQM reports 4 |
| 16 | TTM | TRANSFER TERMINATION MODE | ❌ | Transfer termination; DECRQM reports 4 |
| 17 | SATM | SELECTED AREA TRANSFER MODE | ❌ | Area selection; DECRQM reports 4 |
| 18 | TSM | TABULATION STOP MODE | ❌ | Multi-/single-line tab stops; DECRQM reports 4 |
| 19 | EBM | EDITING BOUNDARY MODE | ⏭️ | Eliminated by the 5th edition ("shall not be used", Annex F.5.1); DECRQM reports 4 |
| 20 | LNM | LINE FEED/NEW LINE MODE | ✅ | Formally eliminated by the 5th edition ("shall not be used", Annex F.5.2), but Ubiquitty still implements it, a widespread terminal convention: set, LF/VT/FF act as NEL and Return sends CR LF (Keymap.cpp) |
| 21 | GRCM | GRAPHIC RENDITION COMBINATION MODE | ✅ | Cumulative until reset; reset makes each SGR replace the rendition; DECRQM answers 0 |
| 22 | ZDM | ZERO DEFAULT MODE | ❌ | Deprecated (Annex F.4.2); DECRQM reports 0 |

**ANSI modes summary**: 5/22 ANSI modes implemented (IRM, SRM, LNM, ERM, GRCM); 1/22 partial (KAM — state stored and reported, keyboard not locked); 15/22 not implemented, most of them printer, transfer, area-qualification or BiDi modes a character-imaging terminal has no component for; mode 19 (formerly EBM) is eliminated by the 5th edition (Annex F.5.1) and shall not be used. DECRQM (decrqmANSIState) answers 1/2 for KAM, IRM, SRM and LNM, 2 for CRM, 4 (permanently reset) for modes 1, 5, 7, 10, 11 and 13–19, and 0 for 6, 8, 9, 21 and 22.

---

## 7. DEC Private Modes (VT/Modes.cpp)

Private modes are dispatched through the VT mode handlers: `setPrivateMode` in VT/Modes.cpp sets them, `decrqmDECState` in VT/Reports.cpp answers DECRQM.

| Mode | Name | Ubiquitty | Notes |
|------|------|-----------|-------|
| 1 | DECCKM — Cursor Keys Application | ✅ | |
| 2 | DECANM — ANSI/VT52 | ✅ | |
| 3 | DECCOLM — 132 Column | ✅ | Ignored while mode 40 is reset |
| 4 | DECSCLM — Smooth Scroll | 🟡 | `t.modes.smoothScroll`; stored for DECRQM, no draw effect |
| 5 | DECSCNM — Light/Reverse Video | ✅ | |
| 6 | DECOM — Origin Mode | ✅ | |
| 7 | DECAWM — Auto Wrap | ✅ | |
| 8 | DECARM — Auto Repeat | ✅ | |
| 9 | X10 Mouse | ✅ | |
| 10 | Show/Hide Toolbar (rxvt) | ✅ | Permanently reset (DECRQM 4): no frontend has a terminal toolbar — not DECLRMM; the real DECLRMM is mode 69 below |
| 12 | Start Blinking Cursor (ATT610) | 🟡 | `t.cursorBlinkAtt610`, the flag DECSCUSR also sets; reported, not drawn |
| 13, 14 | Blinking cursor (read-only) / XOR of blink control | ✅ | 13 reports the reader's blink preference; 14 is permanently reset (DECRQM 4) |
| 18 | DECPFF — Print Form Feed | ✅ | |
| 19 | DECPEX — Print Extent | ✅ | |
| 20 | (none) | ❌ | Not recognised as a private mode; DECRQM answers 0 (ANSI mode 20 is LNM) |
| 25 | DECTCEM — Show/Hide Cursor | ✅ | |
| 30 | Show/Hide Scrollbar (rxvt) | ✅ | `t.scrollbarVisible`, read through `Runtime::ModeSnapshot::isScrollbarVisible`; gated by the WindowMutate capability — not DECRSNM |
| 34 | DECRLM — Right-to-Left | ✅ | `t.shouldWriteRightToLeft` |
| 35 | Font-shifting (rxvt) | 🟡 | Recorded for DECRQM; single-font render |
| 36 | DECHEM — Hebrew Encoding | ✅ | The opposite state of mode 42's setting |
| 38 | DECTEK — Tektronix Mode | ✅ | |
| 40 | Allow 80→132 | ✅ | |
| 41 | more(1) fix | ✅ | `t.moreFix`; set, an HT at an armed deferred wrap commits the wrap first, so the tab lands on the next row's first stop; reset, the tab stays at the right margin with the wrap still armed |
| 42 | DECNRCM — National Replacement | ✅ | |
| 43 | DECGEPM — Graphics Expanded Print | 🟡 | Recorded for DECRQM |
| 44 | Margin Bell | ✅ | `t.marginBell`; rings once as the cursor reaches `columns - 10` (`t.marginBellColumn`), re-armed when it moves left of that column |
| 45 | Reverse Wrap | ✅ | |
| 46 | Start Logging | ✅ | Sets `t.loggingEnabled`; host output is written to `sessionLogFile`, which only the frontend installs (`setSessionLogPath`); with no file installed the mode is a no-op |
| 47 | Alternate Screen (xterm) | ✅ | |
| 60, 61, 64, 73 | DECHCCM, DECVCCM, DECPCCM, DECXRLM | ✅ | Recognised and permanently reset (DECRQM 4) |
| 66 | DECNKM — Numeric/Application Keypad | ✅ | `case 66` in `setPrivateMode`/`decrqmDECState` (`t.modes.appKeypad`); dual-routed — both CSI ?66 h/l and ESC =/> (DECKPAM/DECKPNM) toggle the same flag so a probe sees them agree |
| 67 | DECBKM — Backarrow Key | ✅ | |
| 68 | DECKBUM — Keyboard Usage | 🟡 | Recorded for DECRQM; the frontend's layout owns the keys |
| 69 | DECLRMM — Left/Right Margin Mode | ✅ | |
| 80 | DECSDM — Sixel Display Mode | ✅ | |
| 81 | DECKPM — Key Position Mode | ✅ | `t.modes.shouldReportKeyPositions`; set, keys send key-position reports instead of characters (`encodeKey` in Runtime/KeyEncode.h) |
| 95 | DECNCSM — No Clear Screen On Columns Change | ✅ | |
| 96 | DECRLCM — Right-to-Left Copy | ✅ | `t.shouldCopyRightToLeft` |
| 97 | DECCRTSM — CRT Save | ✅ | `t.modes.shouldSaveCRT`; reset by default, since the VT510's default protects a CRT the emulator does not have; set, 30 idle minutes blank the display and input restores it |
| 101 | DECCANSM — Conceal Answerback | ✅ | Set conceals the answerback; reset does not reveal it |
| 102 | DECNULM — Ignore NUL | ✅ | `t.shouldIgnorePrinterNUL` |
| 117 | DECECM — Erase Colour Mode | ✅ | `t.modes.shouldEraseToDefaultColors`; set, ED/EL/ECH/DECERA and the status-line erases blank to the default colours; reset, to the pen's (background colour erase) — `Screen::eraseCell(Cell*, const Terminal&)` |
| 1000–1003, 1005–1006 | Mouse tracking modes (normal, highlight, button-event, any-event, UTF-8, SGR) | ✅ | Tracking modes exclude one another, as do the coordinate encodings; enabling is gated by `t.allowMouseOperations` |
| 1004 | Focus Reporting | ✅ | `t.modes.focusReporting` — a separate feature from mouse tracking, dispatched by its own case |
| 1007 | Alternate Scroll | ✅ | |
| 1010–1011 | Scroll to bottom on output / key | ✅ | |
| 1014 | fastScroll (xterm) | 🟡 | Recorded for DECRQM; frames are drawn whole |
| 1015 | urxvt Mouse Mode | ✅ | |
| 1016 | SGR-Pixels Mouse Mode | ✅ | |
| 1020–1023 | UTF-8 input, ambiguous wide, emoji wide, private-use width (read-only) | ✅ | DECRQM reports the state; a set or reset does not change it |
| 1034 | Interpret Meta | ✅ | |
| 1035 | Special modifiers for Alt and NumLock | ✅ | `t.modes.shouldHonorNumLock`, default set; NumLock overrides the application keypad |
| 1036 | Meta Sends Escape | ✅ | |
| 1037 | Delete Sends DEL | ✅ | `t.modes.deleteKeySendsDelete`; set, editing-keypad Delete sends DEL; reset, VT220 Remove `CSI 3 ~` |
| 1039 | Alt Sends Escape | ✅ | |
| 1040–1041 | Keep selection / select to clipboard | 🟡 | `t.modes.shouldKeepSelection`, `t.modes.shouldSelectToClipboard`; recorded for DECRQM |
| 1042–1044 | Bell raises urgency / pop-on-bell / keep clipboard on select | ✅ | 1042 (bellRaisesUrgency), 1043 (popOnBell), 1044 (keepClipboard) |
| 1045 | Extended Reverse Wrap | ✅ | `t.modes.reverseWrapExtended` |
| 1046 | Allow Alternate Screen Buffer | ✅ | Gates 47/1047/1049 |
| 1047 | Alternate Screen (xterm) | ✅ | |
| 1048 | Save/Restore Cursor (xterm) | ✅ | |
| 1049 | Alt Screen + Save Cursor (xterm) | ✅ | |
| 1050–1053, 1060, 1061 | termcap, Sun, HP, SCO, legacy and VT220 function keys | ✅ | One `t.modes.keyboardType`; setting one selects it, resetting returns to the default |
| 1070 | Private colour registers (xterm) | ✅ | `t.modes.hasPrivateSixelColorRegisters`, default set; reset carries the palette in `t.sixelColorRegisters` from one sixel to the next |
| 2001 | Readline button 1 | ✅ | Clean click sends cursor keys on the current logical line |
| 2002 | Readline button 2 | ✅ | Moves the point before middle-button paste |
| 2003 | Readline button 3 | ✅ | `t.modes.shouldDeleteReadlineSelection`; a right double click moves to the selection end and erases the selection |
| 2004 | Bracketed Paste | ✅ | Enabling is gated by `t.allowPasteControls`, as for 2005 and 2006 |
| 2005 | Readline character quoting | ✅ | Ctrl-V prefixes each pasted byte |
| 2006 | Readline literal newlines | ✅ | Preserves LF and CR bytes in pasted text |
| 2026 | Synchronized Output | ✅ | |
| 2027 | Unicode Grapheme Clustering | 🟡 | `t.modes.graphemeClustering` — clustering is unconditional and DECRQM permanently reports 3 (set); not "wait mode (copy mode)" |
| 2031 | Color Theme Reporting | ✅ | |
| 2048 | In-Band Resize Reporting | ✅ | Emits a report on enable (`emitInBandResize`) |
| 5522 | Paste events (kitty) | ✅ | `t.modes.shouldReportPasteEvents`; enabling is gated by `t.allowPasteControls` |
| 7400–7403 | Tek bright pen, password input, fullscreen content, relative pointer lock (Ubiquitty) | ✅ | Ubiquitty-private modes |
| 7700 | Ambiguous-width reporting (mintty) | ✅ | `t.modes.shouldReportAmbiguousWidth`; a change of the ambiguous-width choice reports `CSI 1 W` / `CSI 2 W` at the next output (`VT::announceAmbiguousWidthIfMoved`) |
| 7723 | Rewrap on resize (mintty) | ✅ | `t.modes.shouldRewrapOnResize`, default set; reset makes `shouldReflowOnResize` answer no, a geometry-only resize |
| 8452 | Sixel cursor to the right of the graphic (xterm) | ✅ | `t.modes.shouldLeaveCursorRightOfSixel`; the finished sixel leaves the cursor on its last row beside it, wrap deferred at the right edge |
| 8840 | Ambiguous as double width (Tanasinn) | ❌ | This private mode is not recognized. Ambiguous-width configuration is per terminal; it does not imply support for this mode |
| 77096 | Input method editor (foot) | 🟡 | `t.modes.allowsInputMethodEditor`, default set; published in `ModeSnapshot::allowsInputMethodEditor` and reported by DECRQM; no frontend consults it |

**Private modes summary**: `setPrivateMode` handles 92 private mode numbers; `decrqmDECState` recognises 103.

---

## 8. DCS Sub-handlers

| Sequence | Name | Ubiquitty | Notes |
|----------|------|-----------|-------|
| DCS q   | Sixel Graphics | ✅ | |
| DCS {   | DECDLD — Soft Character Set | ✅ | Download parses + stores into `t.softFont`; `Core/Rendering/DRCSRaster.h` rasterises the bitmap into atlas coverage and `CellInstanceBuilder` routes every backend through it (see `ecma35_conformance.md` §3.5; `Tests/Core/Rendering/test_drcs_raster.cpp`) |
| DCS \|   | DECUDK — User-Defined Keys | ✅ | |
| DCS ! u | DECAUPSS — Assign User-Preference Supplemental Set | ✅ | `dcsH_decaupss`: Ps 0 with `%5` selects DEC Supplemental, Ps 1 with `A` selects ISO Latin-1; any other assignment is ignored (DEC STD 070 5.8.2) |
| DCS $ p | DECRSTS — Restore Terminal State | ✅ | `dcsH_decrsts` → `decrstsApply` in `Source/Core/VT/Prefixed.cpp`; all-or-nothing restore of DECRQTSR-saved state (conformance level, C1 policy, setup modes, margins) |
| DCS $ q | DECRQSS — Request Status String | ✅ | |
| DCS $ t | DECRSPS — Restore Presentation State | ✅ | `dcsH_decrsps`: Ps 1 restores DECCIR cursor information, Ps 2 restores DECTABSR tab stops |
| DCS + Q | XTGETXRES — Query Resources | ✅ | `dcsH_xtgetxres`: `DCS 1 + R name=value ST` in hex for the resources Core holds; gated with XTGETTCAP |
| DCS + p | XTSETTCAP — Set Termcap Data | ✅ | `dcsH_xtsettcap` selects the description XTGETTCAP answers from (Ubiquitty or Linux); an unknown name rings the bell and keeps the selection. Gated with XTGETTCAP |
| DCS + q | XTGETTCAP — Termcap Query | ✅ | |
| DCS + s | Downloadable samples (Ubiquitty) | ✅ | `dcsH_sample`: base64 WAV for DECPS's sample voice, by slot and tag; an empty payload clears the slot (see `extensions.md`) |
| DCS tmux; | tmux passthrough | ✅ | `dcsDispatch` un-doubles ESC and re-feeds the inner bytes, nesting bounded by `maxDCSTmuxPassthroughDepth`; any other unrecognised DCS is consumed |

**DCS summary**: 12/12 implemented.

---

## 9. OSC Sub-handlers

| OSC Ps | Name | Ubiquitty | Notes |
|--------|------|-----------|-------|
| 0 | Set Icon Name + Window Title | ✅ | |
| 1 | Set Icon Name | ✅ | Independent of the window title |
| 2 | Set Window Title | ✅ | |
| 4 | Set/Query Color Number | ✅ | Dynamic colour change |
| 5 | Change special colour (Xterm) | ✅ | |
| 6 | Current Document/File URL; iTerm2 tab colour | ✅ | Two sequences share the code. A payload opening with `1;bg;` is iTerm2's tab colour: `1;bg;<red\|green\|blue>;brightness;<0-255>` sets one channel and `1;bg;*;default` clears it (`handleTabColor`, read through `Ubiquitty::tabColor`); a malformed one changes nothing and is never taken for a URL. Any other payload is the current file URL (`handleCurrentFileURL`, `Terminal::currentFileURL`), the file analog of OSC 7 |
| 7 | Current Working Directory | ✅ | |
| 8 | Hyperlink | ✅ | OSC 8 |
| 9 | Progress Indicator; iTerm2 notification | ✅ | `handleOSC9`: ConEmu `4;<state>;<value>` progress, `9;<path>` working directory and `12` prompt mark; any other non-empty payload is an iTerm2 desktop notification body |
| 10–19 | Dynamic colour (fg `10`, bg `11`, cursor `12`, pointer foreground/background `13`/`14`, Tek foreground/background `15`/`16`, selection background `17`, Tek cursor `18`, selection foreground `19`) | ✅ | Every code enters `handleDynamicColors` in `Source/Core/OSC/Color.cpp`: one control's `;`-separated fields apply to successive codes through 19, and each `?` field answers under its own code in a reply of its own |
| 21 | Kitty color control | ✅ | Named foreground, background, cursor and selection colors; indexed palette; set, reset and query. Dynamic selection colors are supported; unsupported keys receive `unknown` replies. This interpretation conflicts with DEC's DECSWT, which the conformance report takes as the authority: Ubiquitty passes 1 of 47 `osc-kitty-color` cases there (`results/live/ubiquitty.json`) |
| 22 | Set Mouse Pointer Shape | ✅ | `handleMousePointerShape` sets, queries, pushes (`>`) and pops (`<`) the mouse pointer shape on a per-screen stack, not the text cursor shape (that is CSI Ps SP q / DECSCUSR) |
| 50 | Set Font | ✅ | |
| 52 | Clipboard Access | ✅ | Gated by config |
| 60–62 | XTQALLOWED, XTQDISALLOWED, XTQALLOWABLE | ✅ | 60 lists the enabled allow categories in xterm's order; 61 and 62 take a category and list its refused operations and every operation it gates. Data on 60, or none on 61/62, draws no reply |
| 66 | Text sizing (kitty) | 🟡 | Metadata parses (strict-refuse), and `s`, `w`, `n`, `d`, `v` and `h` draw. `CellInstanceBuilder` owns the geometry — the band crop, the tiling across the claim, and the fractional area `fractionalRenderArea` places by `v` and `h` — so every arm renders one layout. A glyph is drawn sharp where the backend rasterises it at the size it is drawn: Metal through its sized entries, and `SoftwareRenderer` and `VulkanGlyphAtlas` through `sizedGlyph` and `sizedWideGlyph` over a FreeType face. A bitmap face such as Unifont declines and the cell raster is magnified, which is that face's picture. That magnified crop gives each band its own share of the cell box on every arm — the cell quad, the mono subpixel quad, and the wide and doubled sheets — whichever part of its slot the backend's rect names (`SlotRectConvention`). Only Metal carries a double master; the other two report none, so a block doubled by its row takes the crop there. A fractional two-cell or colour glyph stands in its fractional area on every arm, drawn from its 1x raster mapped into that area. A fractional block on a DECDWL or DECDHL row, fractional box-drawing and soft-font cells, and decorations under a fractional block draw at the full block. A frontend opts in through `Terminal::drawsSizedText`, which the app session runtime, the Linux frontend and the host daemon set; with it clear a sized run prints at ordinary size, so the protocol's detection reads a terminal without the feature. Proportional runs (`p=1`, [proportional text](proportional-text.md)) are a separate fact, `Terminal::drawsProportionalText`: with it clear a measure query goes unanswered and a run prints as ordinary text |
| 72 | Kitty drag and drop | ✅ | `DragDrop::receive`; reads require a native drop and offers require a native gesture |
| 99 | Kitty desktop notification | ✅ | |
| 104–119 | Reset dynamic colours | 🟡 | 104 takes a `;`-separated index list (bare resets all 256), read as xterm's misc.c reads it, past the list's end; 105 is consumed and changes nothing, since no special-colour resource is configurable (`resetSpecialColor`), and Ubiquitty passes 3 of 11 `osc-reset-special-colors` cases in the conformance report; 110–119 each reset the one colour their code names and do not chain |
| 133 | Shell Integration Marks | ✅ | |
| 633 | VS Code Shell Integration | ✅ | `handleOSC633` |
| 666 | VTE terminal properties | ✅ | Typed progress hint and value, resets, and empty query replies; ST termination is required |
| 777 | urxvt perl-extension dispatch | ✅ | `handleOSC777` raises `777;notify;<title>[;<body>]` as a desktop notification; any other extension is consumed without reaching the grid and reported to the `DiagnosticsClient` as an unknown code |
| 1337 | iTerm2 proprietary keys | ✅ | Handled: `SetMark`, `CurrentDir=`, `RemoteHost=`, `SetUserVar=`, `SetBadgeFormat=`, `File=` (inline image), `RequestAttention=`, `CursorShape=`, `StealFocus`, `SetColors=preset=`, `Copy=`, `ReportCellSize`, `ReportVariable=`. `ReportCellSize` answers the cell height and width in points; `ReportVariable=<base64 name>` answers a base64 value for `columns`, `rows`, `name`, `terminalWindowName`, `terminalIconName`, `path` and `user.*` (a `session.` prefix is optional), empty for any other name. `SetBadgeFormat=<base64>` stores the badge text (`Ubiquitty::badgeText`), at most 256 UTF-8 bytes with control characters dropped; an empty value clears it, malformed base64 keeps the badge in force, and iTerm2's `\\(…)` interpolations are kept as text, not evaluated. Any other key is consumed with no effect |
| 3008 | Hierarchical contexts (UAPI.15) | ✅ | `handleContext`; see [Tab status](tab-status.md) |
| 5522 | Kitty clipboard | ✅ | `handleExtendedClipboard`; reads and writes keep the OSC 52 clipboard policy |
| 7402 | Fullscreen content display (Ubiquitty) | ✅ | Fit and letterbox for mode 7402; see `extensions.md` |
| 7404 | Rich text blocks (Ubiquitty) | ✅ | `RichTextBlocks::handle`; see `extensions.md` |
| 7501 | Program status | ✅ | `handleProgramStatus`; `OSC 7501;? ST` is answered in kind; see [Tab status](tab-status.md) |
| 21337 | Tab status | ✅ | Bounded status label and indicator; see [Tab status](tab-status.md) |
| 30001/30101 | Kitty push/pop colours | ✅ | Bare payload pushes or pops the colour stack shared with XTPUSHCOLORS/XTPOPCOLORS |

OSC interpretation, renderer support, and capability authorization are separate.
In particular, accepted text-sizing metadata does not establish that a renderer
applies every sizing field.

---

## 10. ECMA-48 Control Strings (Delimiters)

| Delimiter | Name | Ubiquitty | Notes |
|-----------|------|-----------|-------|
| APC       | APPLICATION PROGRAM COMMAND | ✅ | Parsed and dispatched to the Kitty graphics protocol (Ubiquitty::Kitty::dispatch); content is used, not ignored |
| DCS       | DEVICE CONTROL STRING | ✅ | Dispatches recognized DCS sub-protocols |
| OSC       | OPERATING SYSTEM COMMAND | ✅ | Dispatches recognized OSC commands |
| PM        | PRIVACY MESSAGE | 🟡 | Parsed but content ignored |
| SOS       | start of string | 🟡 | Parsed but content ignored |
| ST        | STRING TERMINATOR | ✅ | Terminates all control strings |

---

## Validation boundaries

Use focused Core tests for byte grammar, state transitions, replies, reset
semantics, and bounds. Use renderer and platform tests for glyphs, decoration,
input, clipboard, window operations, and other external effects. Remote sessions
also require transport and reconnect tests; local parser or snapshot coverage
does not prove the Sync path.

The [feature test matrix](feature_test_matrix.md) identifies snapshot and reply
fixtures. A snapshot is a regression comparison against its baseline, not an
independent conformance oracle. Unsupported functions must be consumed or
refused according to the parser contract without exposing stray framing bytes
or invoking an unrelated operation.
