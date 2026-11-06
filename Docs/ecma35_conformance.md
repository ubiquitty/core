<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# ECMA-35 / ISO/IEC 2022:1994 terminal reference

> Character Code Structure and Extension Techniques
> ECMA-35 6th edition (December 1994) ≡ ISO/IEC 2022:1994
>
> Parser and designation behavior: `Source/Core/VT/VT.cpp`,
> `VT/Escape.cpp`, `VT/CharacterSets.cpp`, `VT/Internal.h`, and `Terminal.h`.
>
> The tables describe the terminal's selected character-set repertoire and
> handling rules, not a claim of complete ISO 2022 conformance. Dispatch and
> model coverage are distinct from rendered-output verification.

---

## Overview

ECMA-35 defines the code structure for 7-bit and 8-bit character sets and
the escape-sequence mechanisms for designating, invoking, and switching
between them. It is structurally independent of ECMA-48 (which defines
individual control functions)—the two standards work in tandem: ECMA-35
provides the framing (which character set is active), ECMA-48 provides the
control functions within that frame.

**Ubiquitty is not a general-purpose ISO 2022 processor.** It implements
the VT220/VT510/xterm subset of ECMA-35 that a terminal emulator needs to
support:

- 94-character graphic set designation into G0–G3
- 96-character graphic set designation into G1–G3: ISO 8859-1 right half
  and DEC Supplemental Graphic, plus the VT510's ISO 8859-2, -5, -7, -8 and
  -9 right halves at conformance level VT500
- Locking shifts (LS0–LS3, LS1R–LS3R)
- Single shifts (SS2 through ESC N, SS3 through ESC O; the 8-bit 0x8E and
  0x8F only with a single-byte input coding)
- 7-bit/8-bit C1 transmission mode (S7C1T/S8C1T via ESC SP F/G)
- DOCS for UTF-8 (ESC % G, ESC % 8) and for ISO 8859-1 (ESC % @)
- DRCS soft-font designator (DECDLD)
- NRCS national replacement sets and the DEC graphic sets listed in §3.2

It does not implement the complete ECMA-35 framework: C0/C1 redesignation,
ACS announcements other than S7C1T/S8C1T, DOCS beyond the three finals above,
IRR, or multibyte 94n/96n sets.

The DEC-assigned designators (NRCS, DEC Special Graphics, DEC Technical,
DECDLD/DRCS) are specified in [DEC STD 070 and the VT manuals](https://vt100.net/docs/);
the final-byte assignments a terminal is expected to accept are enumerated in
[xterm control sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).

---

## 1. Code Structure (ECMA-35 §6–§10)

### 1.1 7-bit and 8-bit code frameworks

| Feature | Requirement | Ubiquitty | Notes |
|---------|------------|-----------|-------|
| 7-bit code layout (§9.1) | GL (02/01–07/14) for graphic chars; C0 (00/00–01/15) for controls | ✅ | GL bytes resolve through G[characterSetActive], or through G2/G3 for one character after a single shift |
| 8-bit code layout (§8) | GL (02/01–07/14), GR (10/01–15/14), C0, C1 (08/00–09/15) | ✅ with a single-byte input coding | Raw 0x80–0x9F are C1 controls only when the input coding is single-byte: ESC % @, or an encoding setting of Latin-1, ISO 8859-15, KOI8-R or Windows-1252. There, GR resolves through G[characterSetGR] after LS1R–LS3R, or through G2/G3 under a single shift; with no GR invocation the input coding decodes 0xA0–0xFF. Under UTF-8, the default, those bytes are UTF-8 and GR is inert |
| C0 set | 32 control positions | Fixed | Terminal control handling is listed in [ECMA-48](ecma48_conformance.md) |
| C1 set | 32 control positions | Fixed | The terminal interprets the subset listed in [ECMA-48](ecma48_conformance.md). A C1 code point decoded from UTF-8 is ignored, except U+0085, which acts as NEL |
| G0–G3 code elements (§7.2) | Up to 4 graphic character sets | ✅ | characterSetG[0..3] |

### 1.2 Code-element invocation (shift functions)

| Shift | Code | Name | Ubiquitty | Notes |
|-------|------|------|-----------|-------|
| SI / LS0 | 0x0F | Shift In — invoke G0 into GL | ✅ | `actExecuteShiftIn` → characterSetActive=0; also executes in place inside an escape or control sequence |
| SO / LS1 | 0x0E | Shift Out — invoke G1 into GL | ✅ | `actExecuteShiftOut` → characterSetActive=1; also executes in place inside an escape or control sequence |
| LS2 | ESC 06/14 ('n') | Locking Shift 2 | ✅ | `escapeH_ls2` → characterSetActive=2 |
| LS3 | ESC 06/15 ('o') | Locking Shift 3 | ✅ | `escapeH_ls3` → characterSetActive=3 |
| LS1R | ESC 07/14 ('~') | Locking Shift 1 Right | ✅ | `escapeH_ls1r` → characterSetGR=1 |
| LS2R | ESC 07/13 ('}') | Locking Shift 2 Right | ✅ | `escapeH_ls2r` → characterSetGR=2 |
| LS3R | ESC 07/12 ('\|') | Locking Shift 3 Right | ✅ | `escapeH_ls3r` → characterSetGR=3 |
| SS2 | ESC 04/14 ('N') or 0x8E | Single Shift 2 (one char from G2) | ✅ | `escapeH_ss2` / `actC1SingleShift2` → singleShift=2; 0x8E only with a single-byte input coding |
| SS3 | ESC 04/15 ('O') or 0x8F | Single Shift 3 (one char from G3) | ✅ | `escapeH_ss3` / `actC1SingleShift3` → singleShift=3; 0x8F only with a single-byte input coding |

### 1.3 7-bit / 8-bit C1 transmission mode

| Function | Code | Ubiquitty | Notes |
|----------|------|-----------|-------|
| S7C1T | ESC 02/00 04/06 ('F') | ✅ | `actEscapeSpaceByte`: transmit8bitC1=false |
| S8C1T | ESC 02/00 04/07 ('G') | ✅ | `actEscapeSpaceByte`: transmit8bitC1=true |

The `transmit8bitC1` flag controls whether the Core sends C1 controls as
ESC Fe sequences or single 0x80–0x9F bytes: when it is set, `respond` and
`respondBulk` collapse every ESC Fe in a reply to its 8-bit byte, and
pointer-driven cursor keys are sent with CSI as 0x9B. DECSCL's second
parameter also sets it (Pc=1 selects 7-bit; level 61 forces 7-bit), DECRQSS
reports it as DECSCL's Pc, DECRQTSR and DECRSTS save and restore it, and
DECSTR and RIS clear it.

---

## 2. Escape Sequences (ECMA-35 §13)

### 2.1 Escape sequence structure

ECMA-35 defines the structure as `ESC I... F` where I are optional
Intermediate bytes (columns 02/00–02/15) and F is the Final byte (columns
03/00–07/14). Types are identified by the first Intermediate byte:

| Column | Type | 1st I byte | Function | Ubiquitty |
|--------|------|-----------|----------|-----------|
| 00–01 | — | — | Not used | N/A |
| 02 | nF | 02/00–02/15 | Code-identification functions (see table below) | Partial |
| 03 | Fp | 03/00–03/15 | Private control functions | Terminal-specific subset via escDispatch: DECBI, DECSC, DECRC, DECFI, DECKPAM, DECKPNM |
| 04–05 | Fe | 04/00–05/15 | C1 control functions | Subset: IND, NEL, HTS, HTJ, VTS, RI, SS2, SS3, CCH, SPA, EPA, and DECID at SCI's position (ESC Z) via escDispatch; CSI, DCS, OSC, SOS, PM, APC enter their states from the DFA, and ST ends a control string |
| 06–07 | Fs | 06/00–07/14 | Single additional control functions | Subset: RIS, LS2, LS3, LS1R–LS3R via escDispatch, plus memory lock/unlock (ESC l / ESC m) at unallocated positions; DMI, INT, EMI, CMD are ignored |

### 2.2 ECMA-35 escape sequence types (Table 3.b)

| Type | 1st I byte | Function | Ubiquitty | Notes |
|------|-----------|----------|-----------|-------|
| 0F | 02/00 | ANNOUNCE CODE STRUCTURE (ACS) | Partial | `ESC SP` → State::EscapeIntermediate; finals F/G are S7C1T/S8C1T (`actEscapeSpaceByte`), every other final is consumed with no effect |
| 1F | 02/01 | C0-DESIGNATE (CZD) | ❌ | Eaten by EscapeIntermediateGeneric → Noop |
| 2F | 02/02 | C1-DESIGNATE (C1D) | ❌ | Same — syntactically accepted, semantically ignored |
| 3F | 02/03 | Single control functions (6.5.2/6.5.3) | ✅ | ESC # is special-cased to `State::Hash` (not the generic 0x21–0x2F bucket); `actHashByte` implements DECDWL/DECDHL/DECSWL (line-attribute changes) and DECALN (full-screen fill) |
| 4F | 02/04 | Multibyte graphic set designation | ❌ | `ESC $` → EscapeIntermediateGeneric; multibyte sets not modelled |
| 5F | 02/05 | designate other coding system (DOCS) | Partial | `ESC %` → State::EscapeCodingSystem; `actSelectCodingSystem` selects UTF-8 for G and 8 and ISO 8859-1 for @, from the next byte. Other finals, and DOCS with a further intermediate (`ESC % / F`), are consumed with no effect |
| 6F | 02/06 | IDENTIFY REVISED REGISTRATION (IRR) | ❌ | `ESC &` → EscapeIntermediateGeneric; revision tracking not implemented |
| 7F | 02/07 | (reserved) | N/A | |
| 8F | 02/08 | G0-DESIGNATE 94-SET | ✅ | `ESC (` → State::CharacterSet, characterSetSelect=0, 94-set |
| 9F | 02/09 | G1-DESIGNATE 94-SET | ✅ | `ESC )` → State::CharacterSet, characterSetSelect=1, 94-set |
| 10F | 02/10 | G2-DESIGNATE 94-SET | ✅ | `ESC *` → State::CharacterSet, characterSetSelect=2, 94-set |
| 11F | 02/11 | G3-DESIGNATE 94-SET | ✅ | `ESC +` → State::CharacterSet, characterSetSelect=3, 94-set |
| 12F | 02/12 | (reserved) | N/A | |
| 13F | 02/13 | G1-DESIGNATE 96-SET | ✅ | `ESC -` → State::CharacterSet, characterSetSelect=1, 96-set |
| 14F | 02/14 | G2-DESIGNATE 96-SET | ✅ | `ESC .` → State::CharacterSet, characterSetSelect=2, 96-set |
| 15F | 02/15 | G3-DESIGNATE 96-SET | ✅ | `ESC /` → State::CharacterSet, characterSetSelect=3, 96-set |

### 2.3 Parser state machine handling

The DFA correctly routes each ESC-intermediate form:

| `State::Escape` first byte | Route | Next state | Handling |
|---|---|---|---|
| `0x20` (SP) | → `EscapeIntermediate` | `Ground` on next byte | `actEscapeSpaceByte`: S7C1T/S8C1T for F/G; a further intermediate goes to `EscapeIntermediateGeneric` |
| `0x23` (`#`) | → `State::Hash` | `Ground` on next byte | Dispatches to `actHashByte`: DECDWL/DECDHL/DECSWL/DECALN implemented |
| `0x25` (`%`) | → `EscapeCodingSystem` | `Ground` on next byte | Dispatches to `actSelectCodingSystem`: DOCS @, G, 8 |
| `0x21`, `0x22`, `0x24`, `0x26`, `0x27`, `0x2C` (other intermediates) | → `EscapeIntermediateGeneric` | collects further 0x20–0x2F, then Ground | All finals silently swallowed |
| `0x28`–`0x2B` (`( ) * +`) | → `State::CharacterSet` | characterSetSelect=0..3, is96CharacterSet=false | Designates 94-set by final byte (an intermediate, as in a DRCS designator or `ESC ( % 5`, passes through `State::CharacterSetIntermediate`) |
| `0x2D`–`0x2F` (`- . /`) | → `State::CharacterSet` | characterSetSelect=1..3, is96CharacterSet=true | Designates 96-set by final byte |
| `0x30`–`0x7E` except `P X [ ] ^ _` | → `EscapeDispatch` | Ground | C1 + single controls from escDispatch |

---

## 3. Character Set Designation (ECMA-35 §14)

### 3.1 Designation of control function sets (§14.2)

| Function | Escape | Ubiquitty | Notes |
|----------|--------|-----------|-------|
| C0-DESIGNATE (CZD) | ESC 02/01 F | ❌ | C0 is fixed to ECMA-48 (VT100); `EscapeIntermediateGeneric` consumes the sequence |
| C1-DESIGNATE (C1D) | ESC 02/02 F | ❌ | C1 is fixed to ECMA-48 (VT100); `EscapeIntermediateGeneric` consumes the sequence |

Ubiquitty always uses the ECMA-48 C0/C1 sets and makes no provision for
redesignating them. Unsupported designations leave the fixed control-set
interpretation unchanged.

### 3.2 Designation of 94-character graphic sets (§14.3.2, Table 6)

**Designation escape forms:**

| Acronym | Name | Escape I bytes | Ubiquitty | Notes |
|---------|------|----------------|-----------|-------|
| GZD4 | G0-DESIGNATE 94-SET | 02/08 | ✅ | ESC ( F → characterSetG[0] = cs |
| G1D4 | G1-DESIGNATE 94-SET | 02/09 | ✅ | ESC ) F → characterSetG[1] = cs |
| G2D4 | G2-DESIGNATE 94-SET | 02/10 | ✅ | ESC * F → characterSetG[2] = cs |
| G3D4 | G3-DESIGNATE 94-SET | 02/11 | ✅ | ESC + F → characterSetG[3] = cs |

**Hard-coded 94-sets (NRCS + DEC):**

| Final byte(s) | Set name | characterSet id | Type |
|---------------|----------|-----------|------|
| `B` | US-ASCII | characterSetUSASCII | 94-set |
| `0` | DEC Special Graphics (Line Drawing) | characterSetDECSpecial | 94-set |
| `A` | UK (pound sign) | characterSetUK | NRCS; `#` is `£` |
| `<`, `%5` | DEC Supplemental (94-set form) | characterSetDECSupplemental | 94-set → GL-folded |
| `R`, `f` | French | characterSetFrench | NRCS |
| `K` | German | characterSetGerman | NRCS |
| `Y` | Italian | characterSetItalian | NRCS |
| `E`, `6`, `` ` `` | Norwegian/Danish (Nordan) | characterSetNorwegianDanish | NRCS |
| `H`, `7` | Swedish | characterSetSwedish | NRCS |
| `C`, `5` | Finnish (same table as Swedish) | characterSetFinnish | NRCS |
| `4` | Dutch | characterSetDutch | NRCS |
| `=` | Swiss | characterSetSwiss | NRCS |
| `Z` | Spanish | characterSetSpanish | NRCS |
| `Q`, `9` | French Canadian | characterSetFrenchCanadian | NRCS |
| `>` | DEC Technical | characterSetDECTechnical | 94-set |

The table lists the 15 sets the 94-set path translates
(`designatedCharacterSet` in `Source/Core/VT/VT.cpp`). It is a selected
DEC/NRCS repertoire, not the full ECMA-35 registration set. The national
sets (UK through French Canadian) apply whether DECNRCM (mode 42) is set or
reset, at every conformance level. Sets that are named but
not carried (`1`, `2`, `I`, `J`, `%0`, `%2`, `%3`, `%6`, `%=`, `"4`, `">`,
`"?`, `&4`, `&5`) also select US-ASCII. Any other final leaves the slot
unchanged. In both of those cases (a final naming no carried set, or one that
falls back to US-ASCII), a downloaded soft font whose designator and 94-set
type match is designated instead (§3.5); a plain `ESC ( B` always selects
US-ASCII.

### 3.3 Designation of 96-character graphic sets (§14.3.2, Table 6)

| Acronym | Name | Escape I bytes | Ubiquitty | Notes |
|---------|------|----------------|-----------|-------|
| G1D6 | G1-DESIGNATE 96-SET | 02/13 | ✅ | 7 finals implemented |
| G2D6 | G2-DESIGNATE 96-SET | 02/14 | ✅ | 7 finals implemented |
| G3D6 | G3-DESIGNATE 96-SET | 02/15 | ✅ | 7 finals implemented |

**Hard-coded 96-sets:**

| Final byte | Set name | characterSet id | Notes |
|-----------|----------|-----------|-------|
| `A` | ISO 8859-1 right half (Latin-1) | characterSetLatin1 | Identity map (byte = code point) |
| `<` | DEC Supplemental Graphic | characterSetDECSupplemental | GR-natured, maps through `set96Translate` |
| `B` | ISO 8859-2 right half (Latin-2) | characterSetLatin2 | VT500 level only |
| `F` | ISO 8859-7 right half (Greek) | characterSetGreek | VT500 level only |
| `H` | ISO 8859-8 right half (Hebrew) | characterSetHebrew | VT500 level only |
| `L` | ISO 8859-5 right half (Latin/Cyrillic) | characterSetLatinCyrillic | VT500 level only |
| `M` | ISO 8859-9 right half (Latin-5) | characterSetLatin5 | VT500 level only |

Seven 96-sets exist. ECMA-35 registers 96-sets under the 63 finals 04/00–07/14
and leaves 03/00–03/15 for private use (DEC's `<` is one of these). The other
ISO 8859 parts (-3, -4, -6, -10, -11, -13 through -16) are not implemented.
The **DEC Technical Character Set** is not among the missing 96-sets: it is a
94-set with the private final `>`, listed in §3.2 above.

The 96-set arm handles finals `A` (Latin-1) and `<` (DEC Supplemental) at
every level, and `B`, `F`, `H`, `L`, `M` at the VT500 conformance level
(the initial level). Below VT500 those five designate no hard set. A 96-set
designation that names no carried set (an unlisted final, one of those five
below VT500, or any designation carrying an intermediate) designates the
downloaded soft font when its designator and 96-set type match, and otherwise
leaves the slot unchanged (`testISO96SetsDesignateAtVT500Level`).

### 3.4 Designation of multibyte graphic sets (§14.3.2, Table 6 — GnDMm)

| Acronym | Name | Escape | Ubiquitty | Notes |
|---------|------|--------|-----------|-------|
| GZDM4 | G0-DESIGNATE MULTIBYTE 94-SET | ESC 02/04 02/08 F | ❌ | EscapeIntermediateGeneric swallows; spec also carves out a legacy exception (ESC 02/04 F with no 2nd I byte when F is 04/00, 04/01, or 04/02) |
| G1DM4 | G1-DESIGNATE MULTIBYTE 94-SET | ESC 02/04 02/09 F | ❌ | Same |
| G2DM4 | G2-DESIGNATE MULTIBYTE 94-SET | ESC 02/04 02/10 F | ❌ | Same |
| G3DM4 | G3-DESIGNATE MULTIBYTE 94-SET | ESC 02/04 02/11 F | ❌ | Same |
| G1DM6 | G1-DESIGNATE MULTIBYTE 96-SET | ESC 02/04 02/13 F | ❌ | Same |
| G2DM6 | G2-DESIGNATE MULTIBYTE 96-SET | ESC 02/04 02/14 F | ❌ | Same |
| G3DM6 | G3-DESIGNATE MULTIBYTE 96-SET | ESC 02/04 02/15 F | ❌ | Same |

None of the multibyte graphic set designations are implemented. These would
be needed for CJK encoding switching (e.g., ESC $ ( C for KS X 1001 Korean,
ESC $ ( B for JIS X 0208 Japanese). Ubiquitty handles CJK via UTF-8
decoding rather than these multibyte designation sequences.

### 3.5 DRCS (§14.4)

| Feature | Ubiquitty | Notes |
|---------|-----------|-------|
| SELECT-DRCS indicator (02/00 in 2nd I byte) | ✅ | CharacterSetIntermediate byte stashed in `t.parser.characterSetIntermediate`; actCharacterSetByte checks characterSetMatchesSoftFont |
| ESC ( SP F — DRCS designation with intermediate | ✅ | `State::CharacterSet` → `State::CharacterSetIntermediate` → `actCharacterSetByte` |
| DECDLD soft font download | ✅ | DCS body parsed into `t->softFont`; `Core/Rendering/DRCSRaster.h` turns a downloaded matrix into atlas coverage |
| DRCS invocation via designation | ✅ | characterSetSoftDRCS (id=100) designated into characterSetG[n] |
| Glyph resolution at render time | ✅ | `CellInstanceBuilder` rasterises the cell's `drcsCodePointBase + n` through `DRCSRaster`, so every backend building instances inks it. Identity is the bitmap rather than the font serial: the serial wraps in 22 bits and the atlas is shared across panes, so an unverified alias would paint one pane's glyph under another's name. `Tests/Core/Rendering/test_drcs_raster.cpp` asserts ink at the bitmap's coordinates and blank elsewhere, which a tofu cell fails; `Tests/Core/Rendering/test_cell_builder_softfont.cpp` (`softFontAliasedKeyRerasterises`) covers the alias |

### 3.6 IRR — Identify Revised Registration (§14.5)

| Escape | Function | Ubiquitty | Notes |
|--------|----------|-----------|-------|
| ESC 02/06 F | IRR — identifies revision of the next designated set | ❌ | Eaten by EscapeIntermediateGeneric |

---

## 4. Code Announcement and Switching (ECMA-35 §15)

### 4.1 ACS — Announce Code Structure (§15.2)

| Escape | Function | Ubiquitty | Notes |
|--------|----------|-----------|-------|
| ESC 02/00 F | ACS — announces code structure facilities | ❌ | 62 facility numbers defined in Table 7; none implemented |

The ACS is used at the start of data interchange to declare which ECMA-35
facilities are in use (7-bit vs 8-bit, which G elements exist, level of
implementation, etc.). Ubiquitty does not read or emit ACS. None of the 62
facility numbers selects an ACS facility. `ESC SP F` and `ESC SP G` retain
their terminal-specific S7C1T/S8C1T meaning. Every other final after `ESC SP`,
and every `ESC SP` sequence carrying a further intermediate, is consumed with
no effect.

**Table 7 facility numbers:** 1–62 covering:

- 1–4: G0/G1 invocation modes (SI/SO vs direct invocation vs 7-bit/8-bit)
- 5: Full preservation of shift functions across 7-bit/8-bit transform
- 6–7: C1 representation (ESC Fe only vs 8-bit single bytes)
- 8–9: 94-only vs mix of 94/96 graphic sets
- 10–11: 7-bit vs 8-bit code
- 12–14: ECMA-43 Level 1/2/3
- 16: G0 invoked by SI/LS0
- 18–23: G1/G2/G3 invoked by specific locking shifts
- 26–27: SS2/SS3 invoke G2/G3
- 28: SS2/SS3 use GR columns in 8-bit
- 24–25, 29–62: reserved

### 4.2 DOCS — Designate Other Coding System (§15.4)

| Escape | Function | Ubiquitty | Notes |
|--------|----------|-----------|-------|
| ESC 02/05 F | DOCS — switch to other coding system | 🟡 | `ESC % G` and xterm's `ESC % 8` select UTF-8 (`actSelectCodingSystem`); other finals change nothing |
| ESC 02/05 04/00 | DOCS return to ECMA-35 coding | ✅ | `ESC % @` selects the single-byte Latin-1 decoder |
| ESC 02/05 02/15 F | DOCS with I byte 02/15: other system doesn't use DOCS return | ❌ | EscapeIntermediateGeneric consumes it (`testCodingSystemSelectorRejectsExtraIntermediatesAndCancellation`) |

Ubiquitty switches between UTF-8 and the 8-bit ECMA-35 coding only.
`ESC % G` (ISO 2022 → UTF-8, registered) and `ESC % @` (return to ISO 2022
from UTF-8) are recognised; the decoder switches after the final byte, so the
rest of the same write uses the selected coding
(`testCodingSystemSwitchAppliesInsideAWriteAndAcrossEverySplit`). Other coding
systems are not supported.

### 4.3 CMD — Coding Method Delimiter (§15.3)

| Escape | Function | Ubiquitty | Notes |
|--------|----------|-----------|-------|
| ESC 06/04 ('d') | CMD — return to outer data structure level | ❌ | Not in escapeHandlers table; escDispatch logs an `esc-miss` diagnostic and does nothing |

---

## 5. Levels of Implementation (ECMA-35 §10.3)

ECMA-35 defines four implementation levels for both 7-bit and 8-bit codes,
with C and A variants:

| Level | Description | 7-bit | 8-bit | Ubiquitty |
|-------|-------------|-------|-------|-----------|
| 1 | G0 only, SI to invoke (7-bit); G0 + G1 with GR shift status already present (8-bit) | Level 1 | Level 1C | ✅ (initial state) |
| 2 | Adds G1 + SO/SI, plus G2/G3 with SS2/SS3 (7-bit); adds G2/G3 with SS2/SS3 — G1/GR already present (8-bit) | Level 2 | Level 2C | ✅ |
| 3 | Adds locking shifts LS2/LS3 (7-bit); adds locking shifts LS0–LS3, LS1R–LS3R (8-bit) | Level 3 | Level 3C | ✅ |
| 4 | Full designation of G0–G3 within data | Level 4 | Level 4C | ✅ (94-sets); 🟡 (96-sets) |
| 2A | Level 2, except GR identified as the single-shift area | Level 2A | Level 2AC | ❌ |
| 3A | Level 3, except GR identified as the single-shift area | Level 3A | Level 3AC | ❌ |
| 4A | Level 4, except GR identified as the single-shift area | Level 4A | Level 4AC | ❌ |

Ubiquitty operates at approximately **Level 4C** for 8-bit codes and
**Level 4** for 7-bit codes, but without the ACS announcements, C0/C1
redesignation, DOCS beyond UTF-8, IRR, or multibyte set support that the
formal definitions require. Those approximate levels are not a formal
conformance claim.

---

## 6. Repertoire boundary and unknown input

The selected repertoire does not interpret ACS announcements, C0/C1
redesignation, DOCS other than UTF-8 and its return, IRR, CMD, multibyte
94ⁿ/96ⁿ sets, or registered graphic sets outside §3. Unsupported designation
sequences must not render their framing bytes as text or partially change an
active G slot.

`EscapeIntermediateGeneric` consumes generic intermediate sequences and their
final byte. Dedicated routes take precedence: `ESC #` changes line attributes,
`ESC SP F/G` selects C1 transmission policy, `ESC %` selects the coding
system, and graphic-set designators enter `State::CharacterSet`. A 96-set is
never designated into G0.

A downloaded font is matched by its designator and set size. Rendering must
bind the cell to the actual glyph bitmap; serial reuse or a shared atlas must
not alias another pane's font. Parser acceptance alone does not establish
glyph correctness.

## 7. Validation

`Tests/Core/VT/test_vt100_characterset.cpp` and
`Tests/Core/test_characterset_96set.cpp` cover designation, invocation,
GL/GR resolution, coding-system switching, and unsupported input.
`Tests/Core/Rendering/test_drcs_raster.cpp` covers downloaded bitmap ink and
bounds, and `Tests/Core/Rendering/test_cell_builder_softfont.cpp` covers
soft-font staging and alias rejection. Terminal byte-feed tests and renderer
tests answer different questions; neither a dispatch-table entry nor an
approximate implementation level substitutes for both.

The [ECMA-48 reference](ecma48_conformance.md) covers the control functions
interpreted within this code structure.
