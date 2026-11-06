#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Ubiquitty terminal feature showcase — entry point.

An interactive TUI that demonstrates terminal features section by section and
is itself built from those features. Its complete executable contract drives
the app snapshot and reply regression suite; it can also run interactively in
other terminals for comparison.

Run:
    python3 Tools/showcase/showcase.py              # interactive (in the terminal's own theme)
    python3 Tools/showcase/showcase.py --own-theme  # pin the demo's baked-in dark theme
    python3 Tools/showcase/showcase.py --list       # print the section/entry tree (no TTY)
    python3 Tools/showcase/showcase.py --self-test  # import every section, validate contract
    python3 Tools/showcase/showcase.py --probes     # dump probe contract as JSON (no TTY)

Stdlib only; Python 3.8+. Terminal state is restored on every exit path.
"""

import argparse
import json
import os
import re
import sys
from io import StringIO
from pathlib import Path

# Run as a script (`python3 Tools/showcase/showcase.py`) or a module
# (`python3 -m showcase.showcase` with Tools/ on the path). The sections use
# package-relative imports (`from .. import tui`), so the directory stays a
# package named `showcase`; parent.parent is Tools/, which carries it on sys.path.
if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from showcase import tui as T
    from showcase.sections import discover
else:
    from . import tui as T
    from .sections import discover


# ---------------------------------------------------------------------------
# Non-interactive entry points (CI-friendly; no raw mode, no alt-screen)
# ---------------------------------------------------------------------------

def _probe_tags(entry):
    """Compact [seq|exp|gold] coverage badge for an entry.

    Each slot shows a letter when the corresponding probe field is set and a
    dot when absent, so test coverage is legible at a glance in --list.
    """
    return "[{}{}{}]".format(
        "s",
        "e" if entry.expect is not None else ".",
        "g" if entry.snapshot else ".",
    )


def cmd_list():
    """Print the section/entry tree as plain text, with probe coverage badges."""
    sections = discover()
    for s in sections:
        print(f"[{s.order:>4}] {s.title}  ({s.module_name})")
        for e in s.entries:
            print(f"        {_probe_tags(e)} - {e.title}")
    n_entries = sum(len(s.entries) for s in sections)
    n_seq = n_entries
    n_exp = sum(e.expect is not None for s in sections for e in s.entries)
    n_gold = sum(e.snapshot for s in sections for e in s.entries)
    print(f"\n{len(sections)} section(s), {n_entries} entr(ies).")
    print(f"probes: [s]equence={n_seq}  [e]xpect={n_exp}  [g]olden={n_gold}")
    return 0


class _CaptureTui(T.Tui):
    """Record one demo screen without requiring a controlling terminal."""

    def __init__(self):
        self.out = StringIO()
        self.theme = T.Theme
        self.own_theme = False
        self.rows, self.cols = 24, 80
        self._wakeup_r = None
        self._saved_termios = None
        self._fd, writer = os.pipe()
        os.write(writer, b"qq")
        os.close(writer)

    def refresh_size(self):
        return self.rows, self.cols

    def read_key(self):
        return "q"

    def query(self, sequence, timeout=0.3):
        del timeout
        if isinstance(sequence, bytes):
            sequence = sequence.decode("latin-1")
        self.write(sequence)
        return b""

    def captured(self):
        return self.out.getvalue()

    def close(self):
        os.close(self._fd)


def _captured_sequence(entry):
    tui = _CaptureTui()
    try:
        entry.run(tui)
        return tui.captured()
    except Exception as error:
        raise RuntimeError(f"cannot capture showcase entry {entry.title!r}: {error}") from error
    finally:
        tui.close()


def _derived_golden(module_name, title):
    slug = re.sub(r"[^a-z0-9]+", "_", title.lower()).strip("_")
    return f"{module_name}_{slug}"


def build_probe_contract(sections=None):
    """Return the complete, executable snapshot/reply contract."""
    sections = discover() if sections is None else sections
    out = []
    seen_goldens = set()
    for section in sections:
        for entry in section.entries:
            sequence = (entry.sequence if entry.sequence is not None
                        else _captured_sequence(entry))
            sequence_encoding = "latin-1" if isinstance(sequence, bytes) else "utf-8"
            if sequence_encoding == "latin-1":
                sequence = sequence.decode("latin-1")
            if not sequence:
                raise ValueError(
                    f"showcase entry has an empty sequence: {section.title} / "
                    f"{entry.title}")
            golden = entry.golden or _derived_golden(section.module_name, entry.title)
            if golden in seen_goldens:
                raise ValueError(f"duplicate showcase golden tag: {golden}")
            seen_goldens.add(golden)
            out.append({
                "section": section.title,
                "module": section.module_name,
                "title": entry.title,
                "sequence": sequence,
                "sequence_encoding": sequence_encoding,
                "expect": entry.expect,
                "golden": golden,
                "snapshot": entry.snapshot,
                "descriptive": entry.descriptive,
            })
    return out


def cmd_probes():
    """Dump each entry's machine-readable probe contract as JSON.

    Per entry this returns an explicit minimal sequence or a deterministic
    capture of the live demo, plus a stable golden tag and any reply assertion.
    Bytes sequences are surfaced as latin-1 strings so JSON round-trips them.
    """
    print(json.dumps(build_probe_contract(), indent=2, ensure_ascii=False))
    return 0


def cmd_self_test():
    """Import & validate every section against the contract.

    `discover()` already validates; we additionally confirm each entry's run is
    callable with the right arity-friendly signature and report a summary.
    """
    try:
        sections = discover()
        probes = build_probe_contract(sections)
    except Exception as exc:  # a contract violation must name itself, not the caller
        print(f"SELF-TEST FAILED: {exc}", file=sys.stderr)
        return 1
    problems = []
    for s in sections:
        for e in s.entries:
            if not callable(e.run):
                problems.append(f"{s.module_name}/{e.title}: run not callable")
    n_entries = sum(len(s.entries) for s in sections)
    if len(probes) != n_entries:
        problems.append(
            f"probe contract has {len(probes)} records for {n_entries} entries")
    if problems:
        print("SELF-TEST FAILED:", file=sys.stderr)
        for p in problems:
            print("  " + p, file=sys.stderr)
        return 1
    n_seq = n_entries
    n_exp = sum(e.expect is not None for s in sections for e in s.entries)
    n_gold = sum(e.snapshot for s in sections for e in s.entries)
    print(f"SELF-TEST OK: {len(sections)} section(s), "
          f"{n_entries} entr(ies) satisfy the contract.")
    for s in sections:
        tagged = len(s.entries)
        print(f"  ok  {s.title} ({len(s.entries)} entries, "
              f"{tagged} with probes)")
    print(f"  probes: [s]equence={n_seq}  [e]xpect={n_exp}  "
          f"[g]olden={n_gold}")
    return 0


# ---------------------------------------------------------------------------
# Interactive menu
# ---------------------------------------------------------------------------

def _blend(a, b, frac):
    """Linear blend of two (r, g, b) tuples; frac=0 → a, 1 → b."""
    return tuple(int(round(a[i] * (1.0 - frac) + b[i] * frac)) for i in range(3))


class Menu:
    """Two-pane main menu: section/entry list (left) + description (right)."""

    def __init__(self, tui, sections):
        self.tui = tui
        self.sections = sections
        self.sel = 0          # index into the flattened list
        self.scroll = 0       # index of the first visible list row (viewport top)
        self.in_section = None  # Section when drilled into entry list
        self.hover = None     # list index under the mouse pointer, or None
        # Per-level selection memory so navigating BACK lands where you left:
        # the section-list index when drilled in, and each section's last entry.
        self.section_sel = 0
        self.entry_sel = {}   # section title -> last selected entry index

    # -- flattened navigation targets ---------------------------------
    def _items(self):
        """Current list of (label, kind, ref) tuples for the left pane."""
        if self.in_section is None:
            return [(s.title, "section", s) for s in self.sections]
        return [(e.title, "entry", e) for e in self.in_section.entries]

    def _description(self):
        items = self._items()
        if not items:
            return "", ""
        label, kind, ref = items[self.sel]
        if kind == "section":
            lines = [f"{len(ref.entries)} demo(s):", ""]
            lines += [f"  • {e.title}" for e in ref.entries]
            return ref.title, "\n".join(lines)
        return ref.title, ref.explanation

    # -- rendering -----------------------------------------------------
    def render(self):
        tui = self.tui
        t = tui.theme
        tui.refresh_size()
        # Full clear before each menu repaint: same terminal, same surface on
        # both shells, so a stale retained grid must not survive a section switch.
        tui.write(tui.bg_default(), "\x1b[2J", "\x1b[H")
        tui.paint_background()
        tui.banner(1, "Ubiquitty Showcase")

        left_w = max(26, tui.cols // 3)
        right_x = left_w + 5
        right_w = tui.cols - right_x - 2
        top = 4
        body_h = tui.rows - top - 2

        crumb = ("Sections" if self.in_section is None
                 else f"Sections / {self.in_section.title}")
        tui.panel(top, 3, body_h, left_w, title=crumb,
                  border=t.border_hot)
        tui.panel(top, right_x, body_h, right_w, title="Description",
                  border=t.border)

        items = self._items()
        # Scrolling viewport: keep the selection inside the visible window so
        # lists longer than the panel stay fully reachable.
        visible = max(1, body_h - 3)
        n = len(items)
        if self.sel < self.scroll:
            self.scroll = self.sel
        elif self.sel >= self.scroll + visible:
            self.scroll = self.sel - visible + 1
        self.scroll = max(0, min(self.scroll, max(0, n - visible)))
        for vis_row in range(visible):
            i = self.scroll + vis_row
            if i >= n:
                break
            label, kind, _ref = items[i]
            row = top + 2 + vis_row
            marker = "›" if i == self.sel else ("·" if i == self.hover else " ")
            text = f" {marker} {T.truncate(label, left_w - 8)}"
            if i == self.sel:
                tui.write(move(row, 5),
                          T.bg_truecolor(*t.highlight_bg),
                          T.fg_truecolor(*t.highlight_fg), T.BOLD,
                          text.ljust(left_w - 4), T.RESET)
            elif i == self.hover:
                # Hover: a lighter wash toward the selection color, so the row
                # under the pointer reads as a live target without competing
                # with the keyboard selection.
                hb = _blend(t.panel, t.highlight_bg, 0.4)
                tui.write(move(row, 5),
                          T.bg_truecolor(*hb),
                          tui.fg_default(),
                          text.ljust(left_w - 4), T.RESET)
            else:
                tui.write(move(row, 5),
                          tui.panel_fill(),
                          tui.fg_default(), text, T.RESET)
        # More-above / more-below arrows on the panel's right border.
        if self.scroll > 0:
            tui.write(move(top + 2, 3 + left_w - 2),
                      T.fg_truecolor(*t.accent), "↑", T.RESET)
        if self.scroll + visible < n:
            tui.write(move(top + body_h - 2, 3 + left_w - 2),
                      T.fg_truecolor(*t.accent), "↓", T.RESET)

        title, desc = self._description()
        tui.text(top + 2, right_x + 3, T.truncate(title, right_w - 6),
                 T.fg_truecolor(*t.accent2), T.BOLD)
        self._wrap(desc, top + 4, right_x + 3, right_w - 6,
                   top + body_h - 2)

        if self.in_section is None:
            keys = [("↑↓/jk", "move"), ("Enter", "open"),
                    ("Hover/Click", "select"), ("Wheel", "scroll"),
                    ("q", "quit")]
        else:
            keys = [("↑↓/jk", "move"), ("Enter", "run"),
                    ("Hover/Click", "select"), ("Esc", "back")]
        tui.status_bar(keys)
        tui.flush()

    def _wrap(self, text, row, col, width, max_row):
        """Word-wrap `text` into the right pane, honoring explicit newlines."""
        t = self.tui.theme
        for para in text.split("\n"):
            if row > max_row:
                break
            if not para:
                row += 1
                continue
            line = ""
            for word in para.split(" "):
                trial = (line + " " + word).strip()
                if T.display_width(trial) > width:
                    self.tui.text(row, col, line, self.tui.fg_default())
                    row += 1
                    line = word
                    if row > max_row:
                        return
                else:
                    line = trial
            if line and row <= max_row:
                self.tui.text(row, col, line, self.tui.fg_default())
                row += 1

    # -- input handling ------------------------------------------------
    def _hit_test(self, click_row, click_col):
        """Map an SGR mouse click to a list index, or None."""
        top = 4
        body_h = self.tui.rows - top - 2
        first = top + 2
        visible = max(1, body_h - 3)
        vis = click_row - first
        items = self._items()
        if 0 <= vis < visible and click_col < (max(26, self.tui.cols // 3) + 5):
            idx = self.scroll + vis
            if 0 <= idx < len(items):
                return idx
        return None

    def run(self):
        tui = self.tui
        while True:
            self.render()
            key = tui.read_key()
            items = self._items()

            # A terminal report is the terminal answering a mode this showcase
            # turned on, not input. Falling through would merely repaint on
            # every focus change; saying so keeps it from being read as one of
            # the string keys below when someone adds a binding.
            if isinstance(key, tuple) and key and key[0] == "report":
                continue

            if isinstance(key, tuple) and key[0] == "mouse":
                _, b, c, r, pressed = key
                # Decode the xterm button byte: bit 0x20 = motion, 0x40 =
                # wheel, low 2 bits = button (with 0x40 set, 0 = up / 1 = down).
                motion = (b & 0x20) != 0
                wheel = (b & 0x40) != 0
                if wheel:
                    if pressed:
                        self.sel = ((self.sel + (1 if (b & 1) else -1))
                                    % max(1, len(items)))
                    continue
                if motion:
                    # Hover: track the row under the pointer; only repaint when
                    # it actually changes so a smooth move doesn't thrash.
                    h = self._hit_test(r, c)
                    if h != self.hover:
                        self.hover = h
                    continue
                # Button event: a left press selects + activates the target.
                if pressed and (b & 0x03) == 0:
                    hit = self._hit_test(r, c)
                    if hit is not None:
                        self.sel = hit
                        self.hover = None
                        self._activate()
                continue

            if key in ("down", "j"):
                self.sel = (self.sel + 1) % max(1, len(items))
            elif key in ("up", "k"):
                self.sel = (self.sel - 1) % max(1, len(items))
            elif key in ("pagedown",):
                self.sel = min(len(items) - 1, self.sel + 5)
            elif key in ("pageup",):
                self.sel = max(0, self.sel - 5)
            elif key in ("enter", "right", "l"):
                self._activate()
            elif key in ("esc", "left", "h", "backspace"):
                if self.in_section is not None:
                    # Remember where we were in this section, then restore the
                    # section-list selection we drilled in from.
                    self.entry_sel[self.in_section.title] = self.sel
                    self.in_section = None
                    self.sel = self.section_sel
                    self.hover = None
                else:
                    return
            elif key == "q":
                return

    def _activate(self):
        items = self._items()
        if not items:
            return
        _label, kind, ref = items[self.sel]
        if kind == "section":
            # Stash the section-list position so BACK returns here, then drill
            # in restoring this section's last entry (clamped to its length).
            self.section_sel = self.sel
            self.in_section = ref
            self.sel = min(self.entry_sel.get(ref.title, 0), max(0, len(ref.entries) - 1))
            self.scroll = 0
            self.hover = None
        else:
            # Mouse is a menu-navigation affordance only — disable it while the
            # demo runs so its area gets clean keyboard input (a mouse-test demo
            # can re-enable it itself), then restore it for the menu.
            self.tui.set_mouse(False)
            try:
                # A section paints once then waits; if its wait woke on SIGWINCH
                # (read_key/wait_key returned "resize") re-run it so it repaints
                # live at the new size, like the menu does.
                while ref.run(self.tui) == "resize":
                    pass
            finally:
                self.tui.set_mouse(True)
                self.hover = None


move = T.move  # local alias used by Menu.render


def cmd_interactive(own_theme=False):
    sections = discover()
    if not sections:
        print("No sections discovered under Tools/showcase/sections/.", file=sys.stderr)
        return 1
    tui = T.Tui(own_theme=own_theme)
    try:
        with tui.session(mouse=True):
            Menu(tui, sections).run()
    except KeyboardInterrupt:
        pass  # session() restores terminal state in its finally block
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    command = parser.add_mutually_exclusive_group()
    command.add_argument("--list", action="store_true",
                         help="print the section and entry catalog")
    command.add_argument("--self-test", action="store_true",
                         help="validate discovery and the regression contract")
    command.add_argument("--probes", action="store_true",
                         help="emit the executable regression contract as JSON")
    parser.add_argument("--own-theme", action="store_true",
                        help="pin the interactive showcase's built-in theme")
    args = parser.parse_args(argv)
    if args.own_theme and (args.list or args.self_test or args.probes):
        parser.error("--own-theme applies only to interactive mode")
    if args.list:
        return cmd_list()
    if args.self_test:
        return cmd_self_test()
    if args.probes:
        return cmd_probes()
    # --own-theme pins the demo's baked-in dark theme (OSC 10/11); without it
    # the showcase renders in the terminal's own theme so the user's real
    # Ubiquitty theme shows through.
    if not sys.stdout.isatty():
        print("showcase.py needs a TTY for interactive mode; "
              "try --list or --self-test.", file=sys.stderr)
        return 2
    return cmd_interactive(own_theme=args.own_theme)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
