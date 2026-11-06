# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Text & Unicode: character sets and shifts, line attributes, and how
combining marks, wide and ambiguous characters, emoji, braille and ill-formed
UTF-8 land on the grid.
"""

import re

from .. import tui as T
from ..tui import (CSI, ESC, RESET, BOLD, move, fg_truecolor,
                   DECDWL, DECDHL_TOP, DECDHL_BOTTOM)
from . import Entry

TITLE = "Text and Unicode"
ORDER = 30

# DEC Special Character / Line Drawing set: selected with ESC ( 0 and exited
# with ESC ( B. While active, ASCII letters map to box-drawing glyphs (j..x).
SCS_DRAW = ESC + "(0"
SCS_ASCII = ESC + "(B"

# ---------------------------------------------------------------------------
# Deterministic snapshot fixtures
#
# Every Text & Unicode exemplar is deterministic, so each carries a probe
# `sequence` reproducing its glyph output as a self-contained string the
# headless snapshot tool renders. Line attributes / shift designations are
# included verbatim so the snapshot exercises the real control codes.
# ---------------------------------------------------------------------------

_DECGRAPHICS_SEQ = "".join(
    SCS_DRAW + ln + SCS_ASCII + "\r\n"
    for ln in ("lqqqwqqqk", "x   x   x", "tqqqnqqqu", "x   x   x", "mqqqvqqqj"))

_DOUBLESIZE_SEQ = (
    DECDWL + "Double-WIDTH line\r\n"
    + DECDHL_TOP + "Double-HEIGHT\r\n"
    + DECDHL_BOTTOM + "Double-HEIGHT\r\n"
    "normal line\r\n")

# Decomposed base+combining sequences (NFD), spelled with explicit code points
# so they are genuinely base-glyph + zero-width combining marks, not precomposed
# single code points. The trailing precomposed NFC é (U+00E9, one code point)
# is the comparison anchor: it must render identically to the decomposed
# e + U+0301 at the front.
_COMBINING_SEQ = (
    "e\u0301 "                          # e + U+0301 acute
    "a\u0308 "                          # a + U+0308 diaeresis
    "o\u0302\u0323 "                    # o + U+0302 circumflex + U+0323 dot below
    "n\u0303 "                          # n + U+0303 tilde
    "Z\u0301\u0302\u0303\u0304\u0306\u0309 "  # six marks stacked on a Z base
    "\u00e9\r\n"                        # precomposed NFC anchor (single code point)
)

_CJK_SEQ = (
    "│日本語のテキスト表示│\r\n"
    "│한국어 텍스트│\r\n"
    "│ASCII 混在 test 文字│\r\n"
    "│ＡＢＣ１２３│\r\n")

_EMOJI_SEQ = (
    "😀 🚀 🎉 🦀\r\n"
    "\U0001f468‍\U0001f469‍\U0001f467‍\U0001f466\r\n"
    "\U0001f469‍\U0001f4bb\r\n"
    "🇺🇸 🇯🇵 🇪🇺\r\n"
    "\U0001f44d \U0001f44d\U0001f3fb \U0001f44d\U0001f3fd \U0001f44d\U0001f3ff\r\n"
    "1️⃣ #️⃣\r\n")

# Braille patterns U+2800..U+28FF encode a 2x4 dot matrix per cell; the full
# block is a sub-cell raster the box-drawing family (eighth blocks / sextants,
# see "Box-drawing & blocks") does not cover. Two contiguous runs and a drawn
# shape reach across the 256-glyph range.
_BRAILLE_SEQ = (
    "".join(chr(c) for c in range(0x2800, 0x2820)) + "\r\n"
    + "".join(chr(c) for c in range(0x2840, 0x2860)) + "\r\n"
    + "⡏⠉⠉⢹  ⣏⣁⣀⣹  ⠿⠿⠿⠿\r\n")

_VARIATION_SEQ = "⚠ ⚠︎ ⚠️ ❤ ❤️ ✉️\r\n"


# The 96-character-set entry belongs to another lane and still draws its page
# through these two; every other entry uses tui.page and tui.footer.
def _header(tui, subtitle):
    tui.paint_background()
    tui.banner(1, "Text & Unicode")
    tui.text(4, 4, subtitle, tui.fg_muted(), T.ITALIC)


def _footer(tui):
    tui.status_bar([("Esc/q", "back")])
    return tui.wait_key()


# ---------------------------------------------------------------------------
# DEC special graphics line drawing
# ---------------------------------------------------------------------------

def _demo_decgraphics(tui):
    row = tui.page(TITLE, "Plain letters draw four boxes that share their walls.")
    t = tui.theme
    box = ["lqqqwqqqk", "x   x   x", "tqqqnqqqu", "x   x   x", "mqqqvqqqj"]
    for i, ln in enumerate(box):
        tui.write(move(row + i, 6), fg_truecolor(*t.accent), SCS_DRAW, ln, SCS_ASCII, RESET)
        tui.label(row + i, 20, f"ESC ( 0  {ln}  ESC ( B")
    return tui.footer()


# ---------------------------------------------------------------------------
# DECDWL / DECDHL
# ---------------------------------------------------------------------------

def _demo_doublesize(tui):
    row = tui.page(TITLE, "One row at double width, one title at double height, one normal row.")
    t = tui.theme
    tui.label(row, 4, "ESC # 6")
    tui.write(move(row + 1, 2), DECDWL, fg_truecolor(*t.accent2), BOLD, "Double width", RESET)
    tui.label(row + 3, 4, "ESC # 3, ESC # 4")
    tui.write(move(row + 4, 2), DECDHL_TOP, fg_truecolor(*t.title), BOLD, "Double height", RESET)
    tui.write(move(row + 5, 2), DECDHL_BOTTOM, fg_truecolor(*t.title), BOLD, "Double height", RESET)
    tui.label(row + 7, 4, "ESC # 5")
    tui.write(move(row + 8, 4), ESC + "#5", tui.fg_default(), "Single width", RESET)
    return tui.footer()


# ---------------------------------------------------------------------------
# Combining marks
# ---------------------------------------------------------------------------

def _demo_combining(tui):
    row = tui.page(TITLE, "Every mark sits on the letter before it; all the bars stand in one column.")
    samples = [
        ("e + U+0301", "e\u0301"),
        ("a + U+0308", "a\u0308"),
        ("o + U+0302 + U+0323", "o\u0302\u0323"),
        ("n + U+0303", "n\u0303"),
        ("Z + six marks", "Z\u0301\u0302\u0303\u0304\u0306\u0309"),
        ("U+00E9, precomposed", "\u00e9"),
    ]
    for label, sample in samples:
        tui.label(row, 4, label)
        tui.text(row, 28, sample + "|", tui.fg_default(), BOLD)
        row += 2
    return tui.footer()


# ---------------------------------------------------------------------------
# Wide CJK + alignment
# ---------------------------------------------------------------------------

def _demo_cjk(tui):
    row = tui.page(TITLE, "Each closing bar stands over its ruler's: two dots per wide glyph.")
    t = tui.theme
    for label, sample in (("CJK", "日本語のテキスト表示"), ("Hangul", "한국어 텍스트"),
                          ("Mixed", "ASCII 混在 test 文字"), ("Fullwidth", "ＡＢＣ１２３")):
        tui.label(row, 4, label)
        tui.text(row, 16, "│" + sample + "│", tui.fg_default(), BOLD)
        tui.text(row + 1, 16, "│" + "·" * T.display_width(sample) + "│", fg_truecolor(*t.accent2))
        row += 3
    return tui.footer()


# ---------------------------------------------------------------------------
# Emoji: ZWJ, flags, skin tone
# ---------------------------------------------------------------------------

def _demo_emoji(tui):
    row = tui.page(TITLE, "Each sequence draws as one glyph.")
    samples = [
        ("single code points", "😀 🚀 🎉 🦀"),
        ("ZWJ family", "\U0001f468‍\U0001f469‍\U0001f467‍\U0001f466"),
        ("ZWJ woman + laptop", "\U0001f469‍\U0001f4bb"),
        ("regional pairs, tag flag",
         "🇺🇸 🇯🇵 🇪🇺 🏴\U000e0067\U000e0062\U000e0073\U000e0063\U000e0074\U000e007f"),
        ("skin-tone modifiers",
         "\U0001f44d \U0001f44d\U0001f3fb \U0001f44d\U0001f3fd \U0001f44d\U0001f3ff"),
        ("VS16 + U+20E3 keycaps", "1️⃣ #️⃣"),
    ]
    for label, sample in samples:
        tui.label(row, 4, label)
        tui.text(row, 32, sample, tui.fg_default(), BOLD)
        row += 2
    return tui.footer()


# ---------------------------------------------------------------------------
# Braille patterns (U+2800..U+28FF) — a 2x4 sub-cell dot raster
# ---------------------------------------------------------------------------
# Block elements, shades, quadrants and legacy sextants live in the dedicated
# "Box-drawing & blocks" section; Braille is the one sub-cell raster with no
# home there, so it stays here.

def _demo_braille(tui):
    row = tui.page(TITLE, "Every cell shows its dots on a 2 by 4 grid; the second run adds dot 7.")
    t = tui.theme
    tui.label(row, 4, "U+2800-U+281F")
    tui.text(row, 22, "".join(chr(c) for c in range(0x2800, 0x2820)), fg_truecolor(*t.accent))
    tui.label(row + 2, 4, "U+2840-U+285F")
    tui.text(row + 2, 22, "".join(chr(c) for c in range(0x2840, 0x2860)), fg_truecolor(*t.accent2))
    tui.label(row + 4, 4, "drawn shapes")
    tui.text(row + 4, 22, "⡏⠉⠉⢹  ⣏⣁⣀⣹  ⠿⠿⠿⠿", fg_truecolor(*t.title))
    return tui.footer()


# ---------------------------------------------------------------------------
# Variation selectors
# ---------------------------------------------------------------------------

def _demo_variation(tui):
    row = tui.page(TITLE, "VS15 picks the text glyph and VS16 the emoji, where the font has both.")
    samples = [
        ("U+26A0", "⚠"), ("U+26A0 U+FE0E", "⚠︎"), ("U+26A0 U+FE0F", "⚠️"),
        ("U+2764", "❤"), ("U+2764 U+FE0F", "❤️"), ("U+2709 U+FE0F", "✉️"),
    ]
    for label, sample in samples:
        tui.label(row, 4, label)
        tui.text(row, 24, sample, tui.fg_default(), BOLD)
        row += 2
    return tui.footer()


# ---------------------------------------------------------------------------
# Shift functions & SCS designation to G2 / G3
# ---------------------------------------------------------------------------

# The four G-sets (G0..G3) each hold a designated character set; SCS escapes
# choose what each holds: ESC ( <f> -> G0, ESC ) <f> -> G1, ESC * <f> -> G2,
# ESC + <f> -> G3. Shifts choose which G-set GL/GR map to: SI (^O) -> G0,
# so (^N) -> G1 (locking); LS2 (ESC n) -> G2, LS3 (ESC o) -> G3 (locking);
# SS2 (ESC N) / SS3 (ESC O) shift the NEXT single char only.
_SS2 = ESC + "N"
_SS3 = ESC + "O"
_LS2 = ESC + "n"
_LS3 = ESC + "o"
_LS0 = "\x0f"  # SI: lock G0 into GL (back to ASCII)
# Designate the DEC special-graphics set into G2 and G3 for this entry.
_DESIG_G2_DRAW = ESC + "*0"
_DESIG_G3_DRAW = ESC + "+0"
_DESIG_G2_ASCII = ESC + "*B"
_DESIG_G3_ASCII = ESC + "+B"

# Snapshot fixture: designate the line-drawing set into G2 and G3, then reach it
# via SS2 (one char), SS3 (one char), and LS2/LS3 (locking). Letters q/x map to
# horizontal/vertical line glyphs while the set is active.
_SHIFTS_SEQ = (
    _DESIG_G2_DRAW + _DESIG_G3_DRAW
    + "SS2: " + _SS2 + "q" + " then ASCII again\r\n"
    + "SS3: " + _SS3 + "x" + " then ASCII again\r\n"
    + "LS2: " + _LS2 + "qqqxqqq" + _LS0 + " (SI puts G0 back)\r\n"
    + "LS3: " + _LS3 + "xqxqxqx" + _LS0 + " (SI puts G0 back)\r\n"
    + _DESIG_G2_ASCII + _DESIG_G3_ASCII)


def _demo_shifts(tui):
    row = tui.page(TITLE, "A single shift draws one line glyph; a locking shift draws a run until SI.")
    t = tui.theme
    tui.write(_DESIG_G2_DRAW, _DESIG_G3_DRAW)
    tui.label(row, 4, "ESC * 0  ESC + 0   line drawing into G2 and G3")
    tui.label(row + 2, 4, "SS2  ESC N q")
    tui.write(move(row + 2, 24), fg_truecolor(*t.accent), _SS2, "q", RESET, "q in ASCII again")
    tui.label(row + 3, 4, "SS3  ESC O x")
    tui.write(move(row + 3, 24), fg_truecolor(*t.accent), _SS3, "x", RESET, "x in ASCII again")
    tui.label(row + 5, 4, "LS2  ESC n ... SI")
    tui.write(move(row + 5, 24), fg_truecolor(*t.accent2), _LS2, "qqqxqqq", _LS0, RESET, " qqqxqqq")
    tui.label(row + 6, 4, "LS3  ESC o ... SI")
    tui.write(move(row + 6, 24), fg_truecolor(*t.accent2), _LS3, "xqxqxqx", _LS0, RESET, " xqxqxqx")
    tui.write(_DESIG_G2_ASCII, _DESIG_G3_ASCII)
    tui.label(row + 8, 4, "ESC * B  ESC + B   ASCII back into G2 and G3")
    return tui.footer()


# ---------------------------------------------------------------------------
# NRCS — National Replacement Character Sets
# ---------------------------------------------------------------------------

# NRCS designate a national variant into G0 (ESC ( <f>): in each variant a few
# ASCII punctuation positions are replaced by national letters. e.g. the UK set
# (ESC ( A) maps '#' -> POUND SIGN; the French set (ESC ( R) maps '#'->£, '@'->à,
# '['->°, '\\'->ç, ']'->§, '{'->é, '|'->ù, '}'->è; the German set (ESC ( K) maps
# '['->Ä '\\'->Ö ']'->Ü '{'->ä '|'->ö '}'->ü '~'->ß.
_NRCS_UK = ESC + "(A"
_NRCS_FR = ESC + "(R"
_NRCS_DE = ESC + "(K"
_NRCS_US = ESC + "(B"  # back to ASCII (US)

_NRCS_PROBE = r"#@[\]{|}~"  # ASCII positions NRCS variants replace

_NRCS_SEQ = (
    "US/ASCII: " + _NRCS_US + _NRCS_PROBE + "\r\n"
    "UK   (A): " + _NRCS_UK + _NRCS_PROBE + _NRCS_US + "\r\n"
    "FR   (R): " + _NRCS_FR + _NRCS_PROBE + _NRCS_US + "\r\n"
    "DE   (K): " + _NRCS_DE + _NRCS_PROBE + _NRCS_US + "\r\n")


def _demo_nrcs(tui):
    row = tui.page(TITLE, "The same nine bytes " + _NRCS_PROBE + " print differently in each set.")
    t = tui.theme
    for label, desig in (("ESC ( B  US", _NRCS_US), ("ESC ( A  UK", _NRCS_UK),
                         ("ESC ( R  French", _NRCS_FR), ("ESC ( K  German", _NRCS_DE)):
        tui.label(row, 4, label)
        tui.write(move(row, 24), fg_truecolor(*t.accent2), BOLD, desig, _NRCS_PROBE, _NRCS_US, RESET)
        row += 2
    return tui.footer()


# ---------------------------------------------------------------------------
# 96-character sets in GR — Latin-1 and DEC Supplemental via LS1R
# ---------------------------------------------------------------------------

# 96-sets (ECMA-35) cannot go in G0; they designate into G1/G2/G3 via the 96-set
# intermediates ESC - / . / / . Finals: ESC - A = ISO 8859-1 (Latin-1) right
# half, ESC - < = DEC Supplemental Graphic. GR (bytes 0xA0..0xFF) exists only in
# an 8-bit coding: under UTF-8 those bytes are encoded characters, so a set
# invoked into GR by LS1R (ESC ~) changes nothing. The sets are shown through GL
# instead: SO invokes G1 into GL, and GL bytes 0x40..0x57 then address the set's
# positions 0xC0..0xD7. DECSC/DECRC bracket the entry, so it returns with the
# designations and shifts it found.
_DECSC = ESC + "7"
_DECRC = ESC + "8"
_DESIG_G1_LATIN1 = ESC + "-A"
_DESIG_G1_DECSUPP = ESC + "-<"
_LS1R = ESC + "~"  # locking shift G1 into GR
_SO = "\x0e"  # LS1: G1 -> GL
_SI = "\x0f"  # LS0: G0 -> GL
_GL_SAMPLE = "".join(chr(c) for c in range(0x40, 0x58))  # positions 0xC0..0xD7
_UTF8_SAMPLE = bytes(range(0xC0, 0xD8)).decode("latin-1")  # U+00C0..U+00D7

_SET96_SEQ = (
    _DECSC
    + _DESIG_G1_LATIN1 + _SO + _GL_SAMPLE + _SI + "\r\n"
    + _DESIG_G1_DECSUPP + _SO + _GL_SAMPLE + _SI + "\r\n"
    + _LS1R + _UTF8_SAMPLE + "\r\n"
    + _DECRC
    # What the menu draws next: UTF-8 box drawing, intact once DECRC has run.
    + move(5, 1) + "\u250c\u2500\u2500\u2500\u2500\u2510\r\n"
    + "\u2514\u2500\u2500\u2500\u2500\u2518")


def _demo_set96(tui):
    _header(tui, "96-character sets: Latin-1 and DEC Supplemental")
    t = tui.theme
    tui.text(6, 4,
             "A 96-set goes into G1, G2 or G3, never G0. SO puts G1 into GL, "
             "where the bytes 40 to 57 address the set's C0 to D7.",
             tui.fg_default())
    tui.write(_DECSC)
    tui.text(9, 4, "G1 = ISO 8859-1 (ESC - A), SO:", tui.fg_muted())
    tui.write(move(9, 44), fg_truecolor(*t.accent), _DESIG_G1_LATIN1, _SO,
              _GL_SAMPLE, _SI, RESET)
    tui.text(11, 4, "G1 = DEC Supplemental (ESC - <), SO:", tui.fg_muted())
    tui.write(move(11, 44), fg_truecolor(*t.accent2), _DESIG_G1_DECSUPP, _SO,
              _GL_SAMPLE, _SI, RESET)
    tui.text(13, 4, "LS1R (ESC ~), then U+00C0..U+00D7 as UTF-8:", tui.fg_muted())
    tui.write(move(13, 44), fg_truecolor(*t.accent), _LS1R, _UTF8_SAMPLE, RESET)
    tui.write(_DECRC)
    tui.comment_block(15, "The DEC row parts from the Latin-1 row at D0 and "
             "D7, where DEC puts OE and Y-diaeresis. The third row is UTF-8, "
             "and GR applies only to an 8-bit coding, so LS1R leaves it "
             "identical to the Latin-1 row. DECSC and DECRC bracket the page, "
             "so the menu gets back the designations and shifts it had.")
    return _footer(tui)


# ---------------------------------------------------------------------------
# Invalid UTF-8 policy — U+FFFD substitution
# ---------------------------------------------------------------------------

# Malformed UTF-8 byte sequences: a lone continuation byte, a truncated 2-byte
# lead, a truncated 3-byte sequence, an overlong encoding of '/', and a 0xFF
# that never appears in UTF-8. The default policy emits U+FFFD (REPLACEMENT
# CHARACTER) per ill-formed subsequence (xterm/kitty/wezterm parity); an
# alternate policy drops them. Kept as raw `bytes` so the live entry and the
# headless probe both feed the exact ill-formed bytes (a str would be UTF-8
# re-encoded on the way out, turning 0x80 into a *valid* two-byte U+0080).
_INVALID_SAMPLES = [
    ("lone continuation 0x80", b"\x80"),
    ("truncated 2-byte lead 0xC3", b"\xc3"),
    ("truncated 3-byte 0xE2 0x82", b"\xe2\x82"),
    ("overlong '/' 0xC0 0xAF", b"\xc0\xaf"),
    ("0xFF (never valid)", b"\xff"),
]
_INVALID_UTF8 = b"".join(
    b"before:" + raw + b" after\r\n" for _label, raw in _INVALID_SAMPLES)


def _write_raw(tui, raw):
    """Send `raw` unchanged: a str would leave UTF-8 re-encoded, masking the
    very bytes under test. A sink without a byte stream (the snapshot capture)
    gets Python's own U+FFFD decoding, which follows the same maximal-subpart
    rule."""
    tui.flush()
    stream = getattr(tui.out, "buffer", None)
    if stream is None:
        tui.write(raw.decode("utf-8", errors="replace"))
        return
    stream.write(raw)
    stream.flush()


def _demo_invalid_utf8(tui):
    row = tui.page(TITLE, "Each ill-formed sequence draws one U+FFFD; the overlong / draws two.")
    t = tui.theme
    for label, raw in _INVALID_SAMPLES:
        tui.label(row, 4, label)
        tui.write(move(row, 34), fg_truecolor(*t.accent2), "[", RESET)
        _write_raw(tui, raw)
        tui.write(fg_truecolor(*t.accent2), "]", RESET, tui.fg_default(), " after", RESET)
        row += 1
    return tui.footer()


# ---------------------------------------------------------------------------
# East-Asian ambiguous width
# ---------------------------------------------------------------------------

# Characters whose East_Asian_Width is "Ambiguous": one cell by default, two
# under a legacy-CJK ambiguous_width=2 setting. The choice is a terminal
# setting rather than an escape, so this entry prints the samples against a
# one-cell ruler and says what each value does.
_AMBIGUOUS = "± × ÷ ° µ ¶ § © ® ← → ↑ ↓ ☆ ★ ○ ● ◇ ◆ Ⅰ Ⅱ Ⅲ"


def _demo_ambiguous_width(tui):
    row = tui.page(TITLE, "At the default width of one cell, the closing bars line up.")
    t = tui.theme
    tui.text(row, 4, "│" + _AMBIGUOUS + "│", fg_truecolor(*t.accent), BOLD)
    tui.text(row + 1, 4, "│" + "·" * T.display_width(_AMBIGUOUS) + "│", fg_truecolor(*t.accent2))
    tui.write(move(row + 3, 4), "±")
    match = re.search(rb"\x1b\[\d+;(\d+)R", tui.query(CSI + "6n"))
    tui.write(move(row + 3, 4), " ")
    tui.label(row + 3, 4, "± then CSI 6 n")
    if match:
        cells = int(match.group(1)) - 4
        tui.result(row + 3, 22, f"cursor moved {cells} cell{'s' if cells != 1 else ''}")
    tui.label(row + 5, 4, "[terminal] ambiguous_width = 1 or 2 sets it.")
    return tui.footer()



# ---------------------------------------------------------------------------
# Glyph-assembly pieces: code points that are parts of a glyph, not glyphs.
#
# U+239B..U+23B1, U+2320/2321, U+23AE, U+23B7, U+23D0 and U+2E59..U+2E5C are
# pieces. A tall parenthesis is an upper hook, some extensions and a lower
# hook, one to a row, and it reads as one bracket only when each piece's stem
# stands in the same column and its ink spans exactly its cell.
#
# Unicode calls them compatibility characters and says a tall bracket is built
# from pieces precisely because scaling an outline cannot make one; TeX's
# extensible recipe and OpenType's MATH GlyphAssembly say the same in their own
# vocabulary, and both distinguish the piece that repeats from the ones that do
# not. Only the extensions may be stretched; a hook or a brace's middle is a
# fixed shape.
#
# The horizontal set below is the same subject on the other axis. Only U+23AF
# is an extension there, and it tiles into one rule; the over and under
# brackets that follow it are whole glyphs, so repeating one draws that many
# brackets rather than one wide one. What the row shows is how the cell-to-cell
# join is rendered on this axis.
# ---------------------------------------------------------------------------

_ASSEMBLY_ROWS = [
    "\u239b\u239e  \u23a1\u23a4  \u23a7\u23ab  \u2320  \u23d0  \u2e59\u2e5a",
    "\u239c\u239f  \u23a2\u23a5  \u23aa\u23aa  \u23ae  \u23d0  \u2e5b\u2e5c",
    "\u239c\u239f  \u23a2\u23a5  \u23a8\u23ac  \u23ae  \u23d0  \u23b0\u23b1",
    "\u239c\u239f  \u23a2\u23a5  \u23aa\u23aa  \u23ae  \u23d0  \u23b2",
    "\u239d\u23a0  \u23a3\u23a6  \u23a9\u23ad  \u2321  \u23b7  \u23b3",
]
_ASSEMBLY_SEQ = "\r\n".join(_ASSEMBLY_ROWS) + "\r\n"

_ASSEMBLY_HORIZONTAL_SEQ = (
    "\u23af\u23af\u23af\u23af\r\n"
    "\u23b4\u23b4\u23b4\u23b4\r\n"
    "\u23b5\u23b5\u23b5\u23b5\r\n"
    "\u23dc\u23dc\u23dc\u23dc\r\n"
    "\u23dd\u23dd\u23dd\u23dd\r\n"
    "\u23de\u23de\u23de\u23de\r\n"
    "\u23df\u23df\u23df\u23df\r\n"
    "\u23e0\u23e0\u23e0\u23e0\r\n"
    "\u23e1\u23e1\u23e1\u23e1\r\n")


def _demo_assembly(tui):
    row = tui.page(TITLE, "Each column reads as one tall bracket with no gap where two rows meet.")
    t = tui.theme
    for index, line in enumerate(_ASSEMBLY_ROWS):
        tui.text(row + 1 + index, 6, line, fg_truecolor(*t.accent), BOLD)
    tui.label(row + 7, 6, "( )  [ ]  { }  ∫  extension  halves")
    return tui.footer()


def _demo_assembly_horizontal(tui):
    row = tui.page(TITLE, "U+23AF joins into one rule; each bracket below closes in its own cell.")
    t = tui.theme
    labels = ["U+23AF extension", "U+23B4 U+23B5 square", "", "U+23DC U+23DD round", "",
              "U+23DE U+23DF curly", "", "U+23E0 U+23E1 tortoise shell", ""]
    for index, line in enumerate(_ASSEMBLY_HORIZONTAL_SEQ.strip().splitlines()):
        tui.text(row + index, 6, line, fg_truecolor(*t.accent2), BOLD)
        if labels[index]:
            tui.label(row + index, 14, labels[index])
    return tui.footer()


ENTRIES = [
    Entry(
        title="Tall bracket pieces (U+239B-U+23B3)",
        explanation=(
            "Each column is one tall bracket built from pieces, one to a row. "
            "Every stem stays in one column and its ink spans its cell, so the "
            "stroke runs unbroken top to bottom; the curly bracket's middle "
            "piece sits centred, not stretched."),
        run=_demo_assembly,
        sequence=_ASSEMBLY_SEQ,
        golden="text_glyph_assembly_vertical",
    ),
    Entry(
        title="Wide bracket pieces (U+23AF-U+23E1)",
        explanation=(
            "U+23AF is an extension piece and joins into one rule across four "
            "cells. The over and under brackets below it are whole glyphs, so "
            "four in a row read as four brackets, each closed in its own cell."),
        run=_demo_assembly_horizontal,
        sequence=_ASSEMBLY_HORIZONTAL_SEQ,
        golden="text_glyph_assembly_horizontal",
    ),
    Entry(
        title="DEC line drawing (ESC ( 0)",
        explanation=(
            "ESC ( 0 makes ASCII letters stand for box-drawing glyphs: l k m j "
            "corners, q and x lines, t u w v tees, n the cross. Five rows draw "
            "four boxes that share walls; ESC ( B restores ASCII after each "
            "row."),
        run=_demo_decgraphics,
        sequence=_DECGRAPHICS_SEQ,
        golden="text_dec_linedraw",
    ),
    Entry(
        title="Double width and height (DECDWL, DECDHL)",
        explanation=(
            "ESC # 6 draws a row at double width, one glyph to two cells. ESC # "
            "3 and ESC # 4 draw the top and bottom halves of one double-height "
            "title. A row without a line attribute stays normal size."),
        run=_demo_doublesize,
        sequence=_DOUBLESIZE_SEQ,
        golden="text_double_size",
    ),
    Entry(
        title="Combining marks (U+0300-U+036F)",
        explanation=(
            "Each combining mark sits on the cell of the base before it and "
            "takes no cell of its own, so every row starts in one column. The "
            "decomposed e with U+0301 matches the precomposed U+00E9."),
        run=_demo_combining,
        sequence=_COMBINING_SEQ,
        golden="text_combining_marks",
    ),
    Entry(
        title="Wide CJK alignment",
        explanation=(
            "Every CJK, Hangul and fullwidth glyph takes two cells and every "
            "ASCII character one, so each row's closing bar stands over its "
            "ruler's bar."),
        run=_demo_cjk,
        sequence=_CJK_SEQ,
        golden="text_cjk_align",
    ),
    Entry(
        title="Emoji sequences",
        explanation=(
            "A ZWJ sequence draws one composite glyph; a regional-indicator "
            "pair or tag sequence draws a flag; a skin-tone modifier recolours "
            "its base; VS16 with U+20E3 draws a keycap."),
        run=_demo_emoji,
        sequence=_EMOJI_SEQ,
        golden="text_emoji",
    ),
    Entry(
        title="Braille patterns (U+2800-U+28FF)",
        explanation=(
            "Each braille cell draws its dots on a 2 by 4 grid at an even "
            "pitch, so dots either side of a cell boundary sit as far apart as "
            "dots within a cell. font.braille_grid = false takes the font's "
            "glyphs instead."),
        run=_demo_braille,
        sequence=_BRAILLE_SEQ,
        golden="text_braille",
    ),
    Entry(
        title="Variation selectors (VS15, VS16)",
        explanation=(
            "VS16 (U+FE0F) asks for the emoji presentation and VS15 (U+FE0E) "
            "for the text one, so the three U+26A0 rows differ where the font "
            "carries both. Neither selector takes a cell."),
        run=_demo_variation,
        sequence=_VARIATION_SEQ,
        golden="text_variation_selectors",
    ),
    Entry(
        title="Shifts into G2 and G3 (SS2, SS3, LS2, LS3)",
        explanation=(
            "With line drawing designated into G2 and G3, SS2 and SS3 take one "
            "character from them and the next is ASCII again; LS2 and LS3 lock "
            "them in until SI. G2 and G3 return to ASCII afterwards."),
        run=_demo_shifts,
        sequence=_SHIFTS_SEQ,
        golden="text_shifts_scs",
    ),
    Entry(
        title="National replacement sets (NRCS)",
        explanation=(
            "A national set in G0 re-maps a few ASCII punctuation positions: UK "
            "turns # into a pound sign, French changes eight of the nine probe "
            "bytes, German brings umlauts and eszett. ESC ( B restores ASCII."),
        run=_demo_nrcs,
        sequence=_NRCS_SEQ,
        golden="text_nrcs",
    ),
    Entry(
        title="96-char sets in GR (Latin-1 / DEC Supplemental)",
        explanation=(
            "A 96-set designates into G1, G2 or G3 (ESC -, ESC ., ESC /, never "
            "G0). ESC - A designates ISO 8859-1 and ESC - < the DEC "
            "Supplemental Graphic set; SO puts G1 into GL, where 40 to 57 "
            "reach the set's C0 to D7. A locking shift right (LS1R, ESC ~) "
            "invokes G1 into GR, bytes A0 to FF, which exist only in an 8-bit "
            "coding: under UTF-8 it leaves the text unchanged. DECSC and DECRC "
            "bracket the entry, so it leaves the character sets as it found "
            "them."),
        run=_demo_set96,
        sequence=_SET96_SEQ,
        golden="text_set96_gr",
    ),
    Entry(
        title="Invalid UTF-8 (U+FFFD)",
        explanation=(
            "Every maximal ill-formed subsequence becomes one U+FFFD; the "
            "overlong 0xC0 0xAF becomes two. The text after the bad bytes "
            "prints unchanged and the parser keeps going."),
        run=_demo_invalid_utf8,
        sequence=_INVALID_UTF8,
        golden="text_invalid_utf8",
    ),
    Entry(
        title="East Asian ambiguous width",
        explanation=(
            "Characters of ambiguous East Asian width take one cell under "
            "[terminal] ambiguous_width = 1, the default, and two under 2. The "
            "cursor position report after one of them shows the width in force."),
        run=_demo_ambiguous_width,
        sequence="± × ÷ ° ← → ☆ ○\r\n",
        golden="text_ambiguous_width",
    ),
]
