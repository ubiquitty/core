# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Fonts: the in-band font control (OSC 50), and the text whose shape the
[font] settings decide: ligatures, OpenType features, per-script fallback and
the glyphs the renderer draws itself.
"""

import os

from .. import tui as T
from ..tui import OSC, ST, BOLD, move, fg_truecolor
from . import Entry

# Static, correctly-shaped Arabic reference (baked at authoring time with PIL's
# raqm engine; decodes via stdlib zlib+base64, no PIL at runtime). Optional: if
# the asset is ever absent the fallback entry still runs, without the image.
try:
    from ..assets import arabic_reference as _AR
except Exception:  # pragma: no cover - asset is committed; the guard keeps the entry safe
    _AR = None

TITLE = "Fonts"
ORDER = 40

# OSC 50 is the in-band font control: `OSC 50 ; ? ST` queries the font and
# `OSC 50 ; #+N ST` / `#-N` bump the size by N points (rxvt).
OSC50_QUERY = f"{OSC}50;?{ST}"
_BUMP = 4


# ---------------------------------------------------------------------------
# OSC 50 — query the current font, optionally set, restore on exit
# ---------------------------------------------------------------------------

def _osc50_spec(reply):
    """Extract the font family from an OSC 50 reply, or None.

    A well-formed reply is `OSC 50 ; <family> ST` (ST or BEL terminated). The
    family is everything after the second `;`, with the terminator trimmed.
    """
    if not reply:
        return None
    text = reply.decode("latin-1") if isinstance(reply, bytes) else reply
    # Strip the OSC introducer (7-bit ESC ] or 8-bit 0x9d) and terminator.
    for intro in (OSC, "\x9d"):
        if text.startswith(intro):
            text = text[len(intro):]
            break
    for term in (ST, "\x07", "\x9c"):
        if text.endswith(term):
            text = text[:-len(term)]
            break
    parts = text.split(";", 2)
    if len(parts) == 3 and parts[0] == "50":
        return parts[2]
    return None


def _demo_osc50(tui):
    caption = "The terminal names its font; Space makes the text four points larger."
    bumped = False
    try:
        while True:
            row = tui.page(TITLE, caption)
            reply = tui.query(OSC50_QUERY)
            if not reply:
                tui.unavailable(row, "No reply to OSC 50 ; ? ST, so there is no font to show.")
                return tui.footer()
            tui.label(row, 4, "OSC 50 ; ? ST")
            tui.result(row, 20, T.truncate(T.decode_reply(reply), tui.cols - 21))
            family = _osc50_spec(reply)
            if family is not None:
                tui.label(row + 1, 4, "family")
                tui.result(row + 1, 20, T.truncate(family or "the configured font", tui.cols - 21))
            tui.label(row + 3, 4, f"OSC 50 ; #+{_BUMP} ST   larger" if not bumped
                      else f"OSC 50 ; #-{_BUMP} ST   back to the size it was")
            tui.keys(("Space", "shrink back" if bumped else "enlarge"))
            tui.flush()
            key = tui.read_key()
            if key != " ":
                return key
            tui.write(f"{OSC}50;#{'-' if bumped else '+'}{_BUMP}{ST}")
            tui.flush()
            bumped = not bumped
    finally:
        if bumped:
            tui.write(f"{OSC}50;#-{_BUMP}{ST}")
            tui.flush()


# ---------------------------------------------------------------------------
# Programming ligatures
# ---------------------------------------------------------------------------

# The operator rows, hoisted as a single-string snapshot fixture so the golden
# renders the same backdrop the entry prints.
_LIGATURE_SAMPLES = [
    ("Arrows", "-> => <- <-> <=> ->> ==>"),
    ("Comparison", "!= == === !== >= <= <=>"),
    ("Pipes / lambda", "|> <| ||> <|| \\> => |->"),
    ("Logic / bitwise", "&& || << >> >>> <<= >>="),
    ("Comments / repeats", "// /* */ /** ::: ... ;;"),
    ("Haskell / ML", ".= := =~ =/= <$> <*> >>="),
]
_LIGATURES_SEQ = "".join(f"{label}: {s}\r\n" for label, s in _LIGATURE_SAMPLES)


def _demo_ligatures(tui):
    row = tui.page(TITLE, "With [font] ligatures on, each operator draws as one glyph.")
    for label, sample in _LIGATURE_SAMPLES:
        tui.label(row, 4, label)
        tui.text(row, 24, sample, tui.fg_default(), BOLD)
        row += 2
    return tui.footer()


# ---------------------------------------------------------------------------
# OpenType features (a setting, not an escape sequence)
# ---------------------------------------------------------------------------

# The characters these features change, as a snapshot fixture: the ones
# calt / liga / zero / onum / ssNN reshape.
_OPENTYPE_SEQ = "0O 1lI fi fl -> != g a  0123456789\r\n"


def _demo_opentype(tui):
    row = tui.page(TITLE, "[font] features (zero, calt, liga, ssNN, onum) reshape the samples below.")
    for tag, sample in (("zero", "0O 0o 100 O0O"),
                        ("calt, liga", "-> != >= <= => fi fl ffi"),
                        ("ss01-ss20", "g a l y & @ $ *"),
                        ("onum", "0123456789")):
        tui.label(row, 4, tag)
        tui.text(row, 18, sample, tui.fg_default(), BOLD)
        row += 2
    tui.label(row, 4, 'features = "calt liga -dlig ss01 zero"')
    return tui.footer()


# ---------------------------------------------------------------------------
# Font fallback per script
# ---------------------------------------------------------------------------

# One line per script, hoisted as a snapshot fixture.
_FALLBACK_ROWS = [
    ("Latin", "covered by the [font] family"),
    ("CJK", "日本語 中文 한국어"),
    ("Arabic", "العربية مرحبا"),
    ("Cyrillic / Greek", "Привет Ελληνικά"),
    ("Emoji", "😀 🚀 🎉 🦀"),
    ("Mixed line", "one baseline 世界 العالم 🌍 Привет"),
]
_FALLBACK_SEQ = "".join(f"{label}: {s}\r\n" for label, s in _FALLBACK_ROWS)


def _image_protocol():
    """Pick an inline-image encoder for this terminal, or None if unsupported.

    There is no central capability probe in the toolkit, so this reads the
    usual environment hints (the same ones terminals advertise themselves by)
    and degrades to None when nothing inline-image-capable is detected. On None
    the reference image is skipped — never emitted as inert escape text.
    """
    if os.environ.get("KITTY_WINDOW_ID") or "kitty" in os.environ.get("TERM", ""):
        return T.image_kitty
    if os.environ.get("TERM_PROGRAM") in ("iTerm.app", "WezTerm", "Ubiquitty"):
        return T.image_iterm2
    # Sixel-capable terminals usually carry it in $TERM (e.g. xterm-256color is
    # ambiguous, but foot/mlterm/contour advertise sixel) — be conservative.
    term = os.environ.get("TERM", "")
    if "sixel" in term or term in ("foot", "contour"):
        return T.image_sixel
    return None


def _show_arabic_reference(tui, row, col):
    """Draw the raqm-shaped Arabic reference at (`row`, `col`), when an
    inline-image protocol is available; otherwise draw nothing."""
    enc = _image_protocol() if _AR is not None else None
    if enc is None:
        return
    tui.label(row, col, "raqm reference")
    tui.write(move(row + 1, col))
    if enc is T.image_iterm2:
        tui.write(enc(_AR.WIDTH, _AR.HEIGHT, _AR.RGBA, cells_high=max(1, _AR.HEIGHT // 18)))
    else:
        tui.write(enc(_AR.WIDTH, _AR.HEIGHT, _AR.RGBA))
    tui.flush()


def _demo_fallback(tui):
    row = tui.page(TITLE, "Every script draws from a fallback face, never a box, on one baseline.")
    first = row
    for label, sample in _FALLBACK_ROWS:
        tui.label(row, 4, label)
        tui.text(row, 22, sample, tui.fg_default(), BOLD)
        row += 2
    _show_arabic_reference(tui, first, 60)
    return tui.footer()


# ---------------------------------------------------------------------------
# Glyphs the renderer draws itself, and the two variation selectors
# ---------------------------------------------------------------------------

# The box-drawing, Powerline (U+E0B0..) and variation-selector rows, hoisted as
# a snapshot fixture matching what the entry prints.
_POWERLINE = "".join(chr(c) for c in range(0xe0b0, 0xe0b8))
_SYNTHESIS_SEQ = (
    "─ │ ┼ ╭ ╮ ╰ ╯ ═ ║ ╬ ┏ ┓ ┗ ┛ ░ ▒ ▓\r\n"
    + _POWERLINE + "\r\n"
    "bare ❤   text ❤︎   color ❤️"
    "      ⚠ ⚠︎ ⚠️\r\n"
)


def _demo_synthesis(tui):
    row = tui.page(TITLE, "Box-drawing and Powerline cells are drawn as geometry, with no seam.")
    t = tui.theme
    tui.label(row, 4, "U+2500-U+2593")
    tui.text(row, 22, "─ │ ┼ ╭ ╮ ╰ ╯ ═ ║ ╬ ┏ ┓ ┗ ┛ ░ ▒ ▓", fg_truecolor(*t.accent))
    tui.label(row + 2, 4, "U+E0B0-U+E0B7")
    tui.text(row + 2, 22, _POWERLINE, fg_truecolor(*t.accent2))
    tui.label(row + 4, 4, "bare, VS15, VS16")
    tui.text(row + 4, 22, "❤ ❤︎ ❤️   ⚠ ⚠︎ ⚠️", tui.fg_default(), BOLD)
    return tui.footer()


ENTRIES = [
    Entry(
        title="Font query and size (OSC 50)",
        explanation=(
            "OSC 50 ; ? ST answers OSC 50 ; <font> ST. OSC 50 ; #+N ST and #-N "
            "change the size by N points and keep the family; an empty payload "
            "returns to the configured font."),
        run=_demo_osc50,
        sequence="\x1b]50;?\x1b\\",
        expect=r"\x1b\]50;[^\x07\x1b]*(\x1b\\|\x07)",
        snapshot=False,
    ),
    Entry(
        title="Programming ligatures",
        explanation=(
            "With [font] ligatures on and a face that carries them, operators "
            "such as -> != >= |> draw as one connected glyph across their "
            "cells, and no column to the right moves."),
        run=_demo_ligatures,
        sequence=_LIGATURES_SEQ,
        golden="fonts_ligatures",
    ),
    Entry(
        title="OpenType features",
        explanation=(
            "[font] features names the OpenType features the shaper applies: "
            "zero slashes the digit 0, calt and liga fuse operator pairs, ssNN "
            "swaps letterforms and onum lowers the digits. A -tag turns one "
            "off."),
        run=_demo_opentype,
        sequence=_OPENTYPE_SEQ,
        golden="fonts_opentype_features",
    ),
    Entry(
        title="Font fallback per script",
        explanation=(
            "A code point the [font] family lacks draws from cjk_fallback, "
            "emoji_fallback, symbol_fallback or fallback, never as a box. Mixed "
            "scripts share one baseline and Arabic joins right to left."),
        run=_demo_fallback,
        sequence=_FALLBACK_SEQ,
        golden="fonts_fallback_scripts",
    ),
    Entry(
        title="Drawn glyphs (box drawing, Powerline)",
        explanation=(
            "Box-drawing cells and the U+E0B0-U+E0B7 Powerline separators are "
            "drawn as geometry from the cell rectangle, so runs meet edge to "
            "edge at any size. VS15 picks the text glyph and VS16 the emoji "
            "glyph."),
        run=_demo_synthesis,
        sequence=_SYNTHESIS_SEQ,
        golden="fonts_glyph_synthesis",
    ),
]
