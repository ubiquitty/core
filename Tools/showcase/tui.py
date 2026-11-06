# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Terminal toolkit for the Ubiquitty feature showcase.

Stdlib only (Python 3.8+). Emits raw ANSI / CSI / OSC / DCS sequences via
stdout so the demo itself exercises the terminal features it documents.

This module avoids curses because curses abstracts away the very escape
sequences this app exists to demonstrate. Every primitive here writes bytes a
human can inspect with a protocol logger.

Terminal state (alt-screen, raw mode, cursor visibility) is always restored on
exit via a try/finally + SIGINT/SIGTERM handlers in `Tui.session`, so a crash
never leaves the controlling terminal wedged.
"""

import os
import signal
import sys
import termios
import textwrap
import tty
from contextlib import contextmanager

ESC = "\x1b"
CSI = ESC + "["
OSC = ESC + "]"
DCS = ESC + "P"
ST = ESC + "\\"  # String Terminator (7-bit form)
BEL = "\x07"


def osc_set_default_fg(rgb):
    """OSC 10: set the terminal's DEFAULT foreground color (rgb 0..255 tuple)."""
    return f"{OSC}10;rgb:{rgb[0]:02x}/{rgb[1]:02x}/{rgb[2]:02x}{ST}"


def osc_set_default_bg(rgb):
    """OSC 11: set the terminal's DEFAULT background color (rgb 0..255 tuple)."""
    return f"{OSC}11;rgb:{rgb[0]:02x}/{rgb[1]:02x}/{rgb[2]:02x}{ST}"


# OSC 110 / 111 reset the default foreground / background to the user's
# configured Ubiquitty theme values.
OSC_RESET_DEFAULT_FG = OSC + "110" + ST
OSC_RESET_DEFAULT_BG = OSC + "111" + ST


# ---------------------------------------------------------------------------
# Low-level escape builders
#
# These return strings rather than writing directly so callers can compose and
# batch them; `Tui.write` flushes.
# ---------------------------------------------------------------------------


def sgr(*codes):
    """Build a Select-Graphic-Rendition sequence: CSI <codes> m."""
    return CSI + ";".join(str(c) for c in codes) + "m"


RESET = CSI + "0m"
BOLD = CSI + "1m"
DIM = CSI + "2m"
ITALIC = CSI + "3m"
UNDERLINE = CSI + "4m"
REVERSE = CSI + "7m"


def fg_truecolor(r, g, b):
    """Truecolor foreground via the colon sub-parameter form (38:2::r:g:b).

    The colon form is the ITU-T T.416 / ISO 8613-6 canonical encoding; many
    terminals also accept the legacy semicolon form (38;2;r;g;b). The color
    section demonstrates both explicitly.
    """
    return f"{CSI}38:2::{r}:{g}:{b}m"


def bg_truecolor(r, g, b):
    return f"{CSI}48:2::{r}:{g}:{b}m"


def fg_256(i):
    return f"{CSI}38;5;{i}m"


def bg_256(i):
    return f"{CSI}48;5;{i}m"


def move(row, col):
    """Cursor Position: CSI <row> ; <col> H (1-based)."""
    return f"{CSI}{row};{col}H"


CLEAR = CSI + "2J"
CLEAR_LINE = CSI + "2K"
HIDE_CURSOR = CSI + "?25l"
SHOW_CURSOR = CSI + "?25h"
SAVE_CURSOR = ESC + "7"
RESTORE_CURSOR = ESC + "8"

# Alternate screen buffer (xterm 1049: save cursor + switch + clear).
ALT_SCREEN_ON = CSI + "?1049h"
ALT_SCREEN_OFF = CSI + "?1049l"

# SGR mouse reporting (1006) with any-event tracking (1003) so the menu sees
# button-less motion (hover), not just clicks. 1000 = button events only,
# 1002 = motion while a button is held, 1003 = all motion.
MOUSE_ON = CSI + "?1003h" + CSI + "?1006h"
MOUSE_OFF = CSI + "?1006l" + CSI + "?1003l"

# DEC double-width / double-height line attributes. These are line-level
# attributes (ESC # ...), not SGR; they affect the whole row the cursor is on.
DECDWL = ESC + "#6"  # double-width single-height
DECDHL_TOP = ESC + "#3"  # double-height top half
DECDHL_BOTTOM = ESC + "#4"  # double-height bottom half
DECSWL = ESC + "#5"  # back to single-width single-height


# ---------------------------------------------------------------------------
# Box-drawing
# ---------------------------------------------------------------------------

# U+2500 family. Two styles: rounded (soft corners) and double-line.
ROUNDED = {
    "tl": "╭",
    "tr": "╮",
    "bl": "╰",
    "br": "╯",
    "h": "─",
    "v": "│",
}
DOUBLE = {
    "tl": "╔",
    "tr": "╗",
    "bl": "╚",
    "br": "╝",
    "h": "═",
    "v": "║",
}
LIGHT = {
    "tl": "┌",
    "tr": "┐",
    "bl": "└",
    "br": "┘",
    "h": "─",
    "v": "│",
}


# ---------------------------------------------------------------------------
# Display width
#
# A minimal east-asian-width / combining-mark aware width so panels and the
# menu align when content contains wide CJK, emoji, or combining marks. This is
# heuristic by design — the text_unicode section is precisely where a human
# verifies the terminal's *actual* layout against these assumptions.
# ---------------------------------------------------------------------------

import unicodedata


def char_width(ch):
    """Best-effort display columns for a single character: 0, 1, or 2."""
    if ch == "\x00":
        return 0
    o = ord(ch)
    # Combining marks and zero-width joiners/selectors occupy no cell.
    if unicodedata.combining(ch):
        return 0
    if o in (0x200D, 0xFE0F, 0xFE0E) or 0x200B <= o <= 0x200F:
        return 0
    if unicodedata.east_asian_width(ch) in ("W", "F"):
        return 2
    # Emoji presentation defaults to wide for common ranges.
    if (0x1F300 <= o <= 0x1FAFF) or (0x2600 <= o <= 0x27BF):
        return 2
    return 1


def display_width(s):
    """Sum of `char_width` over a string (heuristic; ignores ZWJ collapsing)."""
    return sum(char_width(c) for c in s)


def clip(s, width):
    """Cut `s` to at most `width` display columns, with no ellipsis."""
    out, w = [], 0
    for ch in s:
        cw = char_width(ch)
        if w + cw > width:
            break
        out.append(ch)
        w += cw
    return "".join(out)


def truncate(s, width):
    """Truncate `s` to at most `width` display columns, adding an ellipsis."""
    if display_width(s) <= width:
        return s
    out, w = [], 0
    for ch in s:
        cw = char_width(ch)
        if w + cw > width - 1:
            break
        out.append(ch)
        w += cw
    return "".join(out) + "…"


# ---------------------------------------------------------------------------
# Reply decoding
#
# Terminal replies (DA, DSR, DECRQSS, OSC color queries, ...) come back as raw
# control sequences. `decode_reply` renders them caret-escaped and token-spaced
# so a human can read the framing, e.g. b"\x1b[?62;1;6c" -> "ESC [ ? 62 ; 1 ; 6 c".
# ---------------------------------------------------------------------------

# Friendly names for the control bytes that frame a reply.
_CTRL_NAMES = {
    0x1B: "ESC",
    0x9B: "CSI",
    0x90: "DCS",
    0x9C: "ST",
    0x07: "BEL",
    0x9D: "OSC",
    0x00: "NUL",
}


def decode_reply(data):
    """Render a raw reply (bytes or str) as a caret-escaped, spaced string.

    Multi-digit numeric runs are kept together (so "62" reads as one token,
    not "6 2"); control bytes use friendly names; the 7-bit ESC-introduced
    forms (ESC [, ESC ], ESC P, ESC \\) are surfaced as their introducer plus
    final. Other printable bytes pass through as-is. Empty input -> "<none>".
    """
    if isinstance(data, str):
        data = data.encode("latin-1", "replace")
    if not data:
        return "<none>"
    tokens = []
    num = ""
    for byte in data:
        if 0x30 <= byte <= 0x39:  # ASCII digit — accumulate the run
            num += chr(byte)
            continue
        if num:
            tokens.append(num)
            num = ""
        if byte in _CTRL_NAMES:
            tokens.append(_CTRL_NAMES[byte])
        elif byte < 0x20:
            tokens.append("^" + chr(byte + 0x40))  # caret notation
        elif byte == 0x7F:
            tokens.append("^?")
        elif byte < 0x7F:
            tokens.append(chr(byte))
        else:
            tokens.append(f"\\x{byte:02x}")
    if num:
        tokens.append(num)
    return " ".join(tokens)


# ---------------------------------------------------------------------------
# Theme — a single place to tune the showcase's look.
# ---------------------------------------------------------------------------


class Theme:
    bg = (16, 18, 26)
    panel = (24, 27, 38)
    panel_alt = (32, 36, 50)
    text = (208, 214, 228)
    dim = (120, 128, 148)
    accent = (122, 162, 247)  # blue
    accent2 = (158, 206, 106)  # green
    highlight_bg = (54, 66, 110)
    highlight_fg = (236, 242, 255)
    title = (187, 154, 247)  # purple
    border = (68, 76, 102)
    border_hot = (122, 162, 247)


# ---------------------------------------------------------------------------
# The Tui object: owns stdout, terminal state, and rendering primitives.
# ---------------------------------------------------------------------------


class Tui:
    def __init__(self, out=None, own_theme=False):
        self.out = out or sys.stdout
        self.theme = Theme
        # When False (default) the showcase renders in the TERMINAL's own theme:
        # surface backgrounds emit SGR 49 (default bg) and default text emits
        # SGR 39 (default fg), so the user's real Ubiquitty theme shows through.
        # When True the demo's baked-in dark theme is pinned (OSC 10/11) and the
        # same surfaces paint the demo bg/fg as truecolor (see `session`).
        self.own_theme = own_theme
        self._fd = self.out.fileno()
        self._saved_termios = None
        # Read end of the signal self-pipe (see session()); non-None only inside
        # a session. read_key() selects on it so SIGWINCH wakes a blocked read.
        self._wakeup_r = None
        self.rows, self.cols = self.get_size()

    # -- raw output -----------------------------------------------------
    def write(self, *parts):
        self.out.write("".join(parts))

    def flush(self):
        self.out.flush()

    # -- surface colors (theme-aware) -----------------------------------
    # The showcase's surface bg/default-fg either track the demo theme
    # (own_theme on) or defer to the terminal's own defaults (own_theme off),
    # so call sites paint the surface through these instead of hardcoding
    # truecolor. Accent / semantic colors stay truecolor regardless.
    def bg_default(self):
        """Surface background: demo bg as truecolor, or SGR 49 (terminal default)."""
        return bg_truecolor(*self.theme.bg) if self.own_theme else sgr(49)

    def fg_default(self):
        """Default text: demo fg as truecolor, or SGR 39 (terminal default)."""
        return fg_truecolor(*self.theme.text) if self.own_theme else sgr(39)

    def fg_muted(self):
        """Secondary/label text: demo dim as truecolor, or SGR 90 (terminal
        bright-black). Stays legible on the terminal's own background instead of
        the demo theme's dim RGB, which washes out on a light surface."""
        return fg_truecolor(*self.theme.dim) if self.own_theme else sgr(90)

    def panel_fill(self):
        """Card/panel interior fill: demo panel as truecolor, or terminal default bg."""
        return bg_truecolor(*self.theme.panel) if self.own_theme else sgr(49)

    # -- sizing ---------------------------------------------------------
    def get_size(self):
        """Window size in (rows, cols).

        Prefers the kernel's TIOCGWINSZ ioctl (authoritative, pixel-accurate
        rows/cols), falls back to $LINES/$COLUMNS, then a sane default. The
        CSI 18t in-band report is exercised interactively by the reporting
        section; here we want a value before raw mode is set up.
        """
        try:
            import fcntl
            import struct

            packed = fcntl.ioctl(self._fd, termios.TIOCGWINSZ, b"\x00" * 8)
            rows, cols, _, _ = struct.unpack("HHHH", packed)
            if rows and cols:
                return rows, cols
        except Exception:
            pass
        try:
            return int(os.environ["LINES"]), int(os.environ["COLUMNS"])
        except (KeyError, ValueError):
            return 24, 80

    def refresh_size(self):
        self.rows, self.cols = self.get_size()
        return self.rows, self.cols

    # -- terminal lifecycle --------------------------------------------
    @contextmanager
    def session(self, mouse=False):
        """Enter alt-screen + raw mode; restore everything on any exit path.

        Installs SIGINT/SIGTERM handlers that raise KeyboardInterrupt so the
        finally-block restore always runs; a SIGWINCH handler refreshes size.

        With `own_theme` on, entry pins the terminal's DEFAULT foreground
        (OSC 10) and background (OSC 11) to the demo's own theme so the showcase
        renders identically regardless of the user's Ubiquitty theme: the padded
        window border (which uses the terminal default bg, not a cell fill)
        matches the demo bg, and any text relying on the default fg stays legible
        on it (otherwise a light Ubiquitty theme would draw dark default text on
        the demo's dark bg). Both are reset (OSC 110 / 111) in the same finally
        that restores alt-screen / mouse, so a crash never leaves the theme
        pinned. With `own_theme` off (the default) neither OSC pin is emitted —
        surfaces paint with SGR 49/39 so the user's real Ubiquitty theme shows
        through — and nothing needs resetting.
        """
        self._saved_termios = termios.tcgetattr(self._fd)
        prev_int = signal.getsignal(signal.SIGINT)
        prev_term = signal.getsignal(signal.SIGTERM)
        prev_winch = signal.getsignal(signal.SIGWINCH)

        def _stop(_signo, _frame):
            raise KeyboardInterrupt

        def _winch(_signo, _frame):
            self.refresh_size()

        # Signal self-pipe: set_wakeup_fd writes the signal number into the pipe
        # for every signal that has a Python handler, so a SIGWINCH arriving
        # while read_key() is blocked in select() makes that select return at
        # once (PEP 475 would otherwise silently retry the syscall and the demo
        # wouldn't repaint until the next keypress). The write end must be
        # non-blocking. A SIGWINCH handler must be installed for the byte to be
        # written, since Python ignores SIGWINCH by default.
        self._wakeup_r, wakeup_w = os.pipe()
        os.set_blocking(self._wakeup_r, False)
        os.set_blocking(wakeup_w, False)
        prev_wakeup = signal.set_wakeup_fd(wakeup_w)

        try:
            signal.signal(signal.SIGINT, _stop)
            signal.signal(signal.SIGTERM, _stop)
            signal.signal(signal.SIGWINCH, _winch)
            tty.setraw(self._fd)
            self.write(ALT_SCREEN_ON, HIDE_CURSOR)
            if self.own_theme:
                # Pin the default fg/bg to the demo theme so the showcase looks
                # the same under any Ubiquitty theme (and the padded border
                # matches). Skipped by default so the user's theme shows through.
                self.write(
                    osc_set_default_fg(self.theme.text),
                    osc_set_default_bg(self.theme.bg),
                )
            if mouse:
                self.write(MOUSE_ON)
            self.paint_background()
            self.flush()
            yield self
        finally:
            if mouse:
                self.write(MOUSE_OFF)
            if self.own_theme:
                # Reset the default fg/bg back to the user's Ubiquitty theme.
                self.write(OSC_RESET_DEFAULT_FG, OSC_RESET_DEFAULT_BG)
            self.write(SHOW_CURSOR, ALT_SCREEN_OFF, RESET)
            self.flush()
            termios.tcsetattr(self._fd, termios.TCSADRAIN, self._saved_termios)
            signal.signal(signal.SIGINT, prev_int)
            signal.signal(signal.SIGTERM, prev_term)
            signal.signal(signal.SIGWINCH, prev_winch)
            signal.set_wakeup_fd(prev_wakeup)
            os.close(self._wakeup_r)
            os.close(wakeup_w)
            self._wakeup_r = None

    def set_mouse(self, on):
        """Toggle SGR mouse reporting. Mouse is a menu-navigation affordance;
        the framework disables it while a section demo runs so the demo area
        gets clean input unless that demo opts back in."""
        self.write(MOUSE_ON if on else MOUSE_OFF)
        self.flush()

    # -- background / fills --------------------------------------------
    def paint_background(self):
        """Clear the whole screen with the surface background.

        own_theme on: the demo bg. off: the terminal's default bg (SGR 49), so
        the user's Ubiquitty theme shows through.
        """
        self.write(self.bg_default(), CLEAR, move(1, 1))

    # -- input ----------------------------------------------------------
    def read_key(self):
        """Read one logical key, decoding common CSI/SS3 escape sequences.

        Returns a friendly name for navigation keys ("up", "down", "enter",
        "esc", ...) or the raw character. Mouse reports (CSI < ... M/m) are
        returned as a ("mouse", button, col, row, pressed) tuple. Returns
        "resize" when the window changed size (SIGWINCH) so callers can repaint
        at the new size instead of blocking until the next keypress.
        """
        import select

        # Block until the tty has input or a signal wrote to the wakeup pipe.
        # set_wakeup_fd defeats PEP 475's silent EINTR-retry, so a SIGWINCH
        # mid-resize wakes this at once and we report "resize" live.
        while True:
            watch = [self._fd] if self._wakeup_r is None else [self._fd, self._wakeup_r]
            r, _, _ = select.select(watch, [], [])
            if self._wakeup_r is not None and self._wakeup_r in r:
                try:
                    os.read(self._wakeup_r, 256)  # drain coalesced signal bytes
                except OSError:
                    pass
                self.refresh_size()
                return "resize"
            if self._fd in r:
                break
        ch = os.read(self._fd, 1).decode("latin-1")
        if ch != ESC:
            if ch in ("\r", "\n"):
                return "enter"
            if ch == "\x7f" or ch == "\x08":
                return "backspace"
            if ch == "\x03":  # Ctrl-C in raw mode
                raise KeyboardInterrupt
            return ch
        # Possible escape sequence; peek without blocking forever.
        seq = self._read_escape_tail()
        if seq is None:
            return "esc"
        return self._decode_escape(seq)

    def _read_escape_tail(self):
        """Read the remainder of an escape sequence after the initial ESC.

        Uses a short select() poll: a lone ESC (no follow-on bytes) is the
        Escape key, while CSI/SS3 sequences arrive as a burst.
        """
        import select

        r, _, _ = select.select([self._fd], [], [], 0.02)
        if not r:
            return None
        b2 = os.read(self._fd, 1).decode("latin-1")
        if b2 == "[":
            buf = "["
            # CSI: gather until a final byte in @..~ range.
            while True:
                r, _, _ = select.select([self._fd], [], [], 0.05)
                if not r:
                    break
                c = os.read(self._fd, 1).decode("latin-1")
                buf += c
                if "@" <= c <= "~":
                    break
            return buf
        if b2 == "O":
            r, _, _ = select.select([self._fd], [], [], 0.05)
            if r:
                return "O" + os.read(self._fd, 1).decode("latin-1")
            return "O"
        return b2

    # Finals that make a CSI a terminal REPORT rather than a key.
    #
    # These arrive because the showcase asked, or because a mode it enabled
    # says to send them -- focus in/out under DECSET 1004, a locator report
    # under DECELR, a cursor position report, a device attributes reply. They
    # are the terminal answering CORRECTLY, and a demo that treats an answer as
    # a keypress dismisses its own page the moment the feature it is
    # demonstrating starts working.
    #
    # Read as keys, these answers surface in user testing as terminal defects:
    # "Focus reporting (DECSET 1004) exits the test on focus change" and
    # "Hilite mouse tracking exits the test on release" are both this loop.
    #
    # `I` and `O` are focus in/out. `R` is CPR, `c` device attributes, `n` DSR,
    # `t` the window-op replies, `w` the DECELR locator report (which carries an
    # `&` intermediate), `y` DECTST, `S` XTSMGRAPHICS. None of them is a key
    # this demo binds, and none should ever dismiss a page.
    _REPORT_FINALS = frozenset("IORcntwyS")

    def _decode_escape(self, seq):
        csi_keys = {
            "A": "up",
            "B": "down",
            "C": "right",
            "D": "left",
            "H": "home",
            "F": "end",
            "5~": "pageup",
            "6~": "pagedown",
        }
        ss3_keys = {"A": "up", "B": "down", "C": "right", "D": "left"}
        if seq.startswith("["):
            body = seq[1:]
            # SGR mouse: <b;c;rM or <b;c;rm
            if body.startswith("<") and body[-1] in ("M", "m"):
                try:
                    nums, final = body[1:-1], body[-1]
                    b, c, r = (int(x) for x in nums.split(";"))
                    return ("mouse", b, c, r, final == "M")
                except ValueError:
                    return "unknown"
            key = csi_keys.get(body)
            if key is not None:
                return key
            # Checked after the key table, so a binding always wins: CSI A is
            # the up arrow, never a report, however the final byte is spelled.
            if body and body[-1] in self._REPORT_FINALS:
                return ("report", seq)
            return "unknown"
        if seq.startswith("O"):
            return ss3_keys.get(seq[1:], "unknown")
        return "esc"

    # -- query / response capture --------------------------------------
    def query(self, seq, timeout=0.3):
        """Emit a query escape and read the terminal's raw reply from stdin.

        Writes `seq` (str or bytes), then drains stdin with a `select` poll on
        the tty fd until the reply stalls for `timeout` seconds. Returns the
        raw reply bytes (empty `b""` on timeout or off-TTY).

        Intended to run inside `session()`'s raw mode: in cooked mode the reply
        would be line-buffered and echoed. Off a TTY there is no terminal to
        answer, so this degrades to empty bytes rather than blocking. A timeout
        must never leave the session wedged — it only stops reading, it does not
        touch termios.
        """
        import select

        if not os.isatty(self._fd):
            return b""
        data = seq.encode("latin-1") if isinstance(seq, str) else seq
        self.out.write(data.decode("latin-1"))
        self.flush()
        chunks = []
        # First wait up to `timeout` for any reply at all; then keep reading
        # while bytes arrive back-to-back so a multi-byte report is captured
        # whole. A short inter-byte gap signals the reply is complete.
        deadline_gap = timeout
        while True:
            r, _, _ = select.select([self._fd], [], [], deadline_gap)
            if not r:
                break
            try:
                b = os.read(self._fd, 4096)
            except OSError:
                break
            if not b:
                break
            chunks.append(b)
            deadline_gap = 0.05  # subsequent bytes of one report arrive fast
        return b"".join(chunks)

    # -- text rendering -------------------------------------------------
    def text(self, row, col, s, *styles):
        """Write styled text at an absolute position; auto-resets after."""
        self.write(move(row, col), *styles, s, RESET)

    def comment_block(self, row, s, *, pad=6, style=ITALIC):
        """Render an explanatory paragraph as its own inset block: indented `pad`
        columns from both edges and word-wrapped to that width, so prose never
        runs edge-to-edge (which wraps mid-word at the screen boundary). Muted +
        italic. `s` may contain explicit newlines (paragraph breaks). Returns the
        first row after the block."""
        width = max(10, self.cols - 2 * pad)
        lines = []
        for para in s.split("\n"):
            lines.extend(textwrap.wrap(para, width=width) or [""])
        for i, line in enumerate(lines):
            self.text(row + i, pad + 1, line, self.fg_muted(), style)
        return row + len(lines)

    def hline(self, row, col, width, ch, *styles):
        self.write(move(row, col), *styles, ch * width, RESET)

    def center(self, row, s, *styles, width=None):
        """Center `s` horizontally on `row` within `width` (default screen)."""
        width = width or self.cols
        pad = max(0, (width - display_width(s)) // 2) + 1
        self.text(row, pad, s, *styles)

    # -- panels ---------------------------------------------------------
    def panel(
        self, row, col, height, width, title=None, style=ROUNDED, border=None, fill=None
    ):
        """Draw a bordered panel; optionally fill the interior and add a title.

        Coordinates are 1-based; `height`/`width` include the border. The
        interior is filled with `fill` (an RGB tuple) so panels read as solid
        cards against the background.
        """
        t = self.theme
        bcol = fg_truecolor(*(border or t.border))
        # An explicit fill is always honored as truecolor; the default interior
        # defers to the surface so cards sit on the terminal's own theme when
        # own_theme is off.
        fillbg = bg_truecolor(*fill) if fill is not None else self.panel_fill()
        b = style
        # Top border with optional inset title.
        top = b["tl"] + b["h"] * (width - 2) + b["tr"]
        self.write(move(row, col), bcol, fillbg, top, RESET)
        for i in range(1, height - 1):
            self.write(move(row + i, col), bcol, fillbg, b["v"], RESET)
            self.write(fillbg, " " * (width - 2), RESET)
            self.write(bcol, fillbg, b["v"], RESET)
        bottom = b["bl"] + b["h"] * (width - 2) + b["br"]
        self.write(move(row + height - 1, col), bcol, fillbg, bottom, RESET)
        if title:
            label = " " + truncate(title, width - 4) + " "
            self.text(row, col + 2, label, fillbg, fg_truecolor(*t.title), BOLD)

    def fill_rows(self, row, col, height, width, fill):
        """Repaint a rectangular interior region with a background color."""
        bg = bg_truecolor(*fill)
        for i in range(height):
            self.write(move(row + i, col), bg, " " * width, RESET)

    # -- banner ---------------------------------------------------------
    def banner(self, row, text, double_height=True):
        """Render a centered title using DEC double-width / double-height.

        With `double_height`, the same text is emitted twice on consecutive
        rows: once with DECDHL top-half (ESC#3) and once with bottom-half
        (ESC#4); the terminal composites them into one giant title. With only
        double-width, a single DECDWL (ESC#6) row is used.

        Centering accounts for the doubling: glyphs occupy two cells each, so
        the effective text width is doubled when computing the left pad.
        """
        t = self.theme
        # DECDHL/DECDWL make the line double-width, so the cursor addresses
        # double-width cells and only cols/2 of them fit. Center the title in
        # that halved column space — not the full width, which pushes it right.
        # A title wider than that space is truncated: the excess would wrap
        # onto the row below at single size.
        usable = self.cols // 2
        text = truncate(text, usable)
        pad = max(0, (usable - display_width(text)) // 2) + 1
        # Clear and paint both halves with the surface bg. CLEAR_LINE erases
        # with the current SGR background, so it must be set first — otherwise
        # the second row clears with the post-RESET default bg and the two
        # halves of the title get mismatched backgrounds.
        bg = self.bg_default()
        if double_height:
            self.write(move(row, 1), bg, CLEAR_LINE)
            self.write(
                move(row, pad),
                DECDHL_TOP,
                bg,
                fg_truecolor(*t.title),
                BOLD,
                text,
                RESET,
            )
            self.write(move(row + 1, 1), bg, CLEAR_LINE)
            self.write(
                move(row + 1, pad),
                DECDHL_BOTTOM,
                bg,
                fg_truecolor(*t.title),
                BOLD,
                text,
                RESET,
            )
        else:
            self.write(move(row, 1), bg, CLEAR_LINE)
            self.write(
                move(row, pad), DECDWL, bg, fg_truecolor(*t.title), BOLD, text, RESET
            )

    # -- status bar -----------------------------------------------------
    def status_bar(self, keys):
        """Render a bottom status bar from a list of (key, label) pairs.

        Nothing is written past the last column. A character there wraps below
        the bottom row and scrolls the screen up a line, which takes the top
        row, and with it the upper half of a DECDHL banner, off the screen.
        When the pairs do not all fit, the ones before the last are dropped
        from the right, so the bar keeps the key that leaves the page.
        """
        t = self.theme
        row = self.rows
        self.write(move(row, 1), bg_truecolor(*t.panel_alt), " " * self.cols, RESET)
        keys = list(keys)

        # The column of the last label's last character, the bar starting at
        # column 2 and three spaces separating one pair from the next.
        def end(pairs):
            return 1 + sum(display_width(key) + 1 + display_width(label) + 3 for key, label in pairs) - 3

        while len(keys) > 1 and end(keys) > self.cols:
            del keys[-2]
        col = 2
        for key, label in keys:
            room = self.cols - col + 1
            if display_width(key) + 1 + display_width(label) > room:
                key = clip(key, room)
                label = truncate(label, room - display_width(key) - 1) if room - display_width(key) > 1 else ""
            tail = clip(" " + label + "   ", room - display_width(key))
            seg = (
                bg_truecolor(*t.panel_alt)
                + fg_truecolor(*t.accent)
                + BOLD
                + key
                + RESET
                + bg_truecolor(*t.panel_alt)
                + fg_truecolor(*t.dim)
                + tail
                + RESET
            )
            self.write(move(row, col), seg)
            col += display_width(key) + display_width(label) + 4

    def wait_key(self, prompt=None):
        """Block for one key; with a `prompt`, show it as a hint above the bar."""
        if prompt:
            self.center(self.rows - 1, prompt, self.bg_default(), self.fg_muted(),
                        ITALIC)
        self.flush()
        # Loop rather than return the first thing that arrives: a report is not
        # a keypress, and the pages that enable focus or locator reporting are
        # exactly the ones a user sits and looks at, so a report is LIKELY to
        # arrive before the key does.
        while True:
            key = self.read_key()
            if isinstance(key, tuple) and key and key[0] == "report":
                continue
            return key


    # -- entry pages ----------------------------------------------------
    # Every showcase entry draws the same frame: the section banner, one caption
    # line saying what to look at, the demonstration, and the key bar. Sequence
    # names are muted, results the terminal produced are green, and an entry
    # that cannot demonstrate on this terminal says so in one muted line.
    def page(self, title, caption):
        """Clear, draw the section banner and the caption; return the first body row."""
        self.paint_background()
        self.banner(1, title)
        self.text(4, 4, truncate(caption, self.cols - 6), self.fg_default())
        return 6

    def label(self, row, col, s):
        """A sequence name or other secondary text."""
        self.text(row, col, s, self.fg_muted())

    def result(self, row, col, s):
        """Something the terminal answered or did."""
        self.text(row, col, s, fg_truecolor(*self.theme.accent2))

    def unavailable(self, row, reason):
        """The one line an entry prints instead of a demonstration it cannot run."""
        self.text(row, 4, truncate(reason, self.cols - 6), self.fg_muted(), ITALIC)

    def keys(self, *actions):
        """Draw the key bar: the entry's own actions, then Esc."""
        self.status_bar(list(actions) + [("Esc", "back")])

    def footer(self, *actions):
        """Draw the key bar and block for one key."""
        self.keys(*actions)
        return self.wait_key()


# ---------------------------------------------------------------------------
# Inline image embedding
#
# Three independent in-band image protocols so the graphics section can A/B the
# same picture across terminals: Sixel (DCS-framed), Kitty graphics (APC-framed
# base64 RGBA), and iTerm2 OSC 1337 File= (base64 of an image container).
#
# All take raw pixel data and return the escape string; nothing is written
# here. A tiny RGBA test image is generated in `test_image` so the demo needs
# no external assets and stays stdlib-only.
# ---------------------------------------------------------------------------

import base64
import struct
import zlib


def test_image(width=64, height=64, kind="gradient"):
    """Build a small RGBA test image as a flat bytes buffer (row-major).

    `kind` selects the pattern: "gradient" is a red/green diagonal blend with a
    blue ramp down the rows; "checkerboard" alternates 8x8 black/white tiles.
    Returns (width, height, rgba_bytes) — 4 bytes per pixel, fully opaque.
    """
    buf = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 4
            if kind == "checkerboard":
                on = ((x // 8) + (y // 8)) % 2 == 0
                v = 255 if on else 0
                buf[i : i + 4] = bytes((v, v, v, 255))
            else:
                r = int(255 * x / max(1, width - 1))
                g = int(255 * (1 - x / max(1, width - 1)))
                b = int(255 * y / max(1, height - 1))
                buf[i : i + 4] = bytes((r, g, b, 255))
    return width, height, bytes(buf)


def _png_bytes(width, height, rgba):
    """Encode an RGBA buffer as a minimal PNG (stdlib zlib only).

    iTerm2's OSC 1337 File= expects an image container (PNG/GIF/...), not raw
    pixels, so the iTerm2 helper wraps the pixels in this hand-built PNG.
    """

    def chunk(tag, payload):
        c = tag + payload
        return (
            struct.pack(">I", len(payload))
            + c
            + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        )

    # IHDR: 8-bit truecolor with alpha (color type 6).
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    # Each scanline is prefixed with a filter-type byte (0 = none).
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride : (y + 1) * stride])
    idat = zlib.compress(bytes(raw), 9)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", idat)
        + chunk(b"IEND", b"")
    )


def _sixel_rle(bytes_run):
    """Run-length encode a list of sixel data chars using the `!<n><c>` form.

    Sixel allows `!<count><char>` to repeat one data byte; runs of three or
    more are worth compressing. Returns the encoded string for one color pass.
    """
    out = []
    i = 0
    n = len(bytes_run)
    while i < n:
        c = bytes_run[i]
        j = i + 1
        while j < n and bytes_run[j] == c:
            j += 1
        count = j - i
        if count >= 3:
            out.append(f"!{count}{c}")
        else:
            out.append(c * count)
        i = j
    return "".join(out)


def image_sixel(width, height, rgba, max_colors=64):
    """Encode an RGBA buffer as a Sixel image string (DCS P1;P2;P3 q ... ST).

    Sixel stacks six vertical pixels per data byte and overlays one color pass
    per band. Correctness hinges on four things this encoder gets right:

    - DCS introducer with the three numeric parameters `0;0;0` before `q` (some
      parsers reject a bare `q`).
    - A BOUNDED palette: the RGBA test images can contain thousands of distinct
      colors, so colors are quantized to a small cube (`max_colors`) — emitting
      hundreds of registers makes the stream enormous and some parsers drop it.
    - Per band, only the colors that ACTUALLY appear are emitted (not every
      register), each as `#idx` + RLE-compressed data + `$` (overlay at band
      start); the band ends with `-` to advance six rows.
    - `!<n><char>` run-length compression so solid spans stay compact.

    Returns the full `DCS … ST` string ready to write to the terminal.
    """
    # Quantize to a small color cube so the register count (and the emitted
    # stream) stays bounded regardless of how many distinct colors are present.
    # `levels` per channel gives at most levels**3 registers; pick the largest
    # cube that fits under max_colors.
    levels = 1
    while (levels + 1) ** 3 <= max_colors:
        levels += 1
    step = max(1, 256 // levels)

    palette = {}  # quantized (cr,cg,cb) in 0..100 -> register index
    indexed = bytearray(width * height)
    for p in range(width * height):
        r, g, b = rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]
        # Snap each channel to a cube level, then to the 0..100 sixel scale.
        qr = min(levels - 1, r // step)
        qg = min(levels - 1, g // step)
        qb = min(levels - 1, b // step)
        key = (
            qr * 100 // max(1, levels - 1),
            qg * 100 // max(1, levels - 1),
            qb * 100 // max(1, levels - 1),
        )
        idx = palette.get(key)
        if idx is None:
            idx = len(palette)
            palette[key] = idx
        indexed[p] = idx

    out = [DCS + "0;0;0q"]
    for (cr, cg, cb), idx in palette.items():
        out.append(f"#{idx};2;{cr};{cg};{cb}")
    # Emit band by band; each band is six pixel rows tall.
    for band in range(0, height, 6):
        # Which registers appear in this band? Emit only those, in order.
        present = []
        seen = set()
        for x in range(width):
            for dy in range(6):
                y = band + dy
                if y < height:
                    idx = indexed[y * width + x]
                    if idx not in seen:
                        seen.add(idx)
                        present.append(idx)
        for idx in present:
            row_bytes = []
            for x in range(width):
                bits = 0
                for dy in range(6):
                    y = band + dy
                    if y < height and indexed[y * width + x] == idx:
                        bits |= 1 << dy
                row_bytes.append(chr(0x3F + bits))  # sixel data byte
            out.append(f"#{idx}" + _sixel_rle(row_bytes))
            out.append("$")  # carriage return: overlay next color at band start
        out.append("-")  # advance to the next band
    out.append(ST)
    return "".join(out)


def image_kitty(width, height, rgba, columns=None, rows=None):
    """Encode an RGBA buffer for the Kitty graphics protocol (APC _G ... ST).

    Transmits raw 32-bit RGBA (`f=32`) directly placed (`a=T`), base64-encoded
    and split into <=4096-byte chunks with `m=1` continuation flags as the
    protocol requires. `columns`/`rows` optionally scale the placement to a
    cell box.
    """
    placement = ""
    if columns:
        placement += f",c={columns}"
    if rows:
        placement += f",r={rows}"
    # q=2 suppresses the protocol's OK/error response APC. The demo reads keys
    # straight off stdin; an unsolicited _G…;OK reply would otherwise be misread
    # as a keypress and bounce the entry back to the menu the instant the image
    # lands.
    return _kitty_chunked(
        f"a=T,f=32,s={width},v={height}{placement},q=2", rgba
    )


def _kitty_chunked(control_first, rgba):
    """Base64 + <=4096-byte chunk one RGBA payload into a Kitty APC stream.

    `control_first` is the control block for the FIRST chunk (e.g. the a=T
    transmit-and-place keys or the a=f frame keys); continuation chunks carry
    only `m=`. Shared by the still-image and per-frame transmit helpers.
    """
    payload = base64.b64encode(rgba).decode("ascii")
    parts = [payload[i : i + 4096] for i in range(0, len(payload), 4096)] or [""]
    out = []
    for i, part in enumerate(parts):
        more = 1 if i < len(parts) - 1 else 0
        ctrl = f"{control_first},m={more}" if i == 0 else f"m={more}"
        out.append(f"{ESC}_G{ctrl};{part}{ST}")
    return "".join(out)


def image_kitty_animation(width, height, frames, image_id=42, loop=True):
    """Drive a real multi-frame Kitty animation (a=T base, a=f frames, a=a play).

    `frames` is a list of (rgba_bytes, delay_ms) pairs; the first is the base
    frame transmitted-and-placed (a=T) under an explicit image id `i=`, the rest
    are appended with a=f (the per-frame delay rides Kitty's `z=` key), and a
    final a=a control with c=3 (loop) / c=2 (run once) starts playback. Every
    block uses q=2 to suppress the OK/error reply APC (the demo reads stdin
    directly and would misread it as a keypress).

    This exercises the host's frame-advance hook end to end: a terminal that
    never ticks the animation freezes on the base frame; one that does cycles
    through every frame.
    """
    base_rgba, _base_delay = frames[0]
    out = [
        _kitty_chunked(
            f"a=T,f=32,s={width},v={height},i={image_id},q=2", base_rgba
        )
    ]
    for rgba, delay_ms in frames[1:]:
        # a=f appends a frame; Kitty re-uses z= as the frame's display delay.
        out.append(
            _kitty_chunked(
                f"a=f,f=32,s={width},v={height},i={image_id},z={delay_ms},q=2",
                rgba,
            )
        )
    # a=a animation control: c=3 loop forever, c=2 run once. r= is a 1-based
    # frame jump (omitted: start at frame 1).
    play = 3 if loop else 2
    out.append(f"{ESC}_Ga=a,i={image_id},c={play},q=2{ST}")
    return "".join(out)


def test_image_frames(width=64, height=64, count=6):
    """Build `count` RGBA frames that sweep a bright bar across the image.

    Each frame paints a dark field with a vertical bar at a column that marches
    left→right, so a working animation reads as a bar scanning across; a frozen
    one shows the bar parked at the first column. Returns a list of
    (rgba_bytes, delay_ms) pairs ready for `image_kitty_animation`.
    """
    frames = []
    bar_width = max(2, width // count)
    for f in range(count):
        buf = bytearray(width * height * 4)
        bar_x = int((width - bar_width) * f / max(1, count - 1))
        for y in range(height):
            for x in range(width):
                i = (y * width + x) * 4
                if bar_x <= x < bar_x + bar_width:
                    # Bright cyan bar.
                    buf[i : i + 4] = bytes((40, 220, 230, 255))
                else:
                    # Dim blue field with a faint vertical gradient.
                    b = 30 + int(60 * y / max(1, height - 1))
                    buf[i : i + 4] = bytes((10, 20, b, 255))
        frames.append((bytes(buf), 120))
    return frames


def image_iterm2(
    width, height, rgba, name="showcase.png", cells_wide=None, cells_high=None
):
    """Encode an RGBA buffer as an iTerm2 inline image (OSC 1337 File=).

    The pixels are wrapped in a PNG (iTerm2 expects an image file, not raw
    RGBA), base64-encoded, and announced with `inline=1`. Optional
    `width`/`height` cell sizing is expressed in terminal cells.
    """
    png = _png_bytes(width, height, rgba)
    b64 = base64.b64encode(png).decode("ascii")
    args = [
        f"name={base64.b64encode(name.encode()).decode('ascii')}",
        f"size={len(png)}",
        "inline=1",
    ]
    if cells_wide:
        args.append(f"width={cells_wide}")
    if cells_high:
        args.append(f"height={cells_high}")
    return f"{OSC}1337;File={';'.join(args)}:{b64}{BEL}"
