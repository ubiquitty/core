# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Shell integration: prompt marks and the state-reporting OSCs.

The shell brackets each prompt and command with OSC 133 (or VS Code's OSC 633)
marks; Ubiquitty draws a bracket in the side gutters beside a prompt whose
command ran, tinted by its exit status. OSC 7, OSC 6 and OSC 1337 CurrentDir=
report the directory and document the window title and proxy icon follow.
Mode 7401 asserts a password prompt. Every entry puts back what it changed:
the directory goes back to the one this process started in.
"""

import os

from ..tui import CSI, OSC, ST, RESET, move
from . import Entry

TITLE = "Shell integration"
ORDER = 90


def _restore_directory():
    """Report the directory the showcase was started in, and no document."""
    return f"{OSC}6;{ST}{OSC}7;file://{os.getcwd()}{ST}"


def _cycle(osc, prompt, command, output, code):
    """One prompt-and-command cycle marked with OSC <osc> ; A/B/C/D."""
    return (f"{OSC}{osc};A{ST}{prompt}{OSC}{osc};B{ST}{command}"
            f"{OSC}{osc};C{ST}\r\n{output}\r\n{OSC}{osc};D;{code}{ST}")


def _write_lines(tui, row, column, text):
    """Write a CR LF stream so each line starts at `column`."""
    for offset, line in enumerate(text.split("\r\n")):
        tui.write(move(row + offset, column), line)


# ---------------------------------------------------------------------------
# OSC 133 and OSC 633 prompt marks
# ---------------------------------------------------------------------------

_OSC133_SEQ = _cycle(133, "user@host:~/project$ ", "make build",
                     "one output row between C and D", 0)

_OSC633_SEQ = (
    f"{OSC}633;A{ST}user@host:~/project$ {OSC}633;B{ST}"
    f"{OSC}633;E;make build{ST}{OSC}633;P;Cwd=/home/user/project{ST}"
    f"make build{OSC}633;C{ST}\r\n"
    f"one output row between C and D\r\n{OSC}633;D;0{ST}")


def _marks_page(tui, caption, labels, stream):
    row = tui.page(TITLE, caption)
    for offset, line in enumerate(labels):
        tui.label(row + offset, 4, line)
    _write_lines(tui, row + len(labels) + 1, 4, stream)
    return tui.footer()


def _demo_osc133(tui):
    return _marks_page(
        tui, "The prompt row gets a bracket in each side gutter once its command runs.",
        ["OSC 133 ; A  prompt   B  input   C  output   D ; 0  finished, exit 0"],
        _OSC133_SEQ)


def _demo_osc633(tui):
    return _marks_page(
        tui, "The same gutter bracket, from the VS Code spelling of the marks.",
        ["OSC 633 ; A, B, C, D   the OSC 133 marks",
         "OSC 633 ; E ; make build ST   the command line",
         "OSC 633 ; P ; Cwd=/home/user/project ST   the working directory"],
        _OSC633_SEQ)


# ---------------------------------------------------------------------------
# Exit status: the bracket of a failed command turns red
# ---------------------------------------------------------------------------

_EXIT_SEQ = (_cycle(133, "$ ", "true", "exited with 0", 0) + "\r\n"
             + _cycle(133, "$ ", "false", "exited with 1", 1))


def _demo_exit_status(tui):
    return _marks_page(
        tui, "The failed command's bracket is red; the successful one's is not.",
        ["OSC 133 ; D ; 0 ST   after true",
         "OSC 133 ; D ; 1 ST   after false"],
        _EXIT_SEQ)


# ---------------------------------------------------------------------------
# OSC 1337 shell state
# ---------------------------------------------------------------------------

_OSC1337_SEQ = (
    f"{OSC}1337;SetMark{ST}"
    f"{OSC}1337;CurrentDir=/home/user/project{ST}"
    f"{OSC}1337;RemoteHost=user@host{ST}"
    f"{OSC}1337;SetUserVar=git_branch=bWFpbg=={ST}")

_OSC1337_STEPS = (
    ("CursorShape=1", "the cursor is a bar"),
    ("CursorShape=2", "the cursor is an underline"),
    ("CursorShape=0", "the cursor is a block"),
    ("CurrentDir=/usr/share", "the window title names share"),
)


def _demo_osc1337(tui):
    row = tui.page(TITLE, "Each Space sends the next command; the cursor and then the title change.")
    for offset, (command, _) in enumerate(_OSC1337_STEPS):
        tui.label(row + offset, 4, f"OSC 1337 ; {command} ST")
    caret_row = row + len(_OSC1337_STEPS) + 1
    tui.label(caret_row, 4, "cursor:")
    step = 0
    try:
        while True:
            tui.keys(("Space", "send next"))
            tui.write(move(caret_row, 12))
            if step:
                tui.write(CSI + "?25h")
            tui.flush()
            key = tui.read_key()
            if key != " ":
                return key
            command, effect = _OSC1337_STEPS[step % len(_OSC1337_STEPS)]
            tui.write(f"{OSC}1337;{command}{ST}")
            tui.write(move(caret_row + 2, 4), CSI + "K")
            tui.result(caret_row + 2, 4, f"Sent {command}: {effect}.")
            step += 1
    finally:
        tui.write(CSI + "0 q", CSI + "?25l", _restore_directory())
        tui.flush()


# ---------------------------------------------------------------------------
# OSC 7 and OSC 6: directory and document
# ---------------------------------------------------------------------------

_OSC7_SEQ = f"{OSC}7;file://host/home/user/project{ST}"
_OSC6_SEQ = f"{OSC}6;file://host/home/user/project/README.md{ST}"

_LOCATION_STEPS = (
    (f"{OSC}7;file:///usr/share{ST}", "OSC 7 ; file:///usr/share ST",
     "The title names share and the proxy icon is that folder."),
    (f"{OSC}6;file:///etc/hosts{ST}", "OSC 6 ; file:///etc/hosts ST",
     "The proxy icon is now the hosts document."),
    (None, "OSC 6 ; ST, then OSC 7 back to this directory",
     "Title and proxy icon are back to this directory."),
)


def _demo_location(tui):
    row = tui.page(TITLE, "The window title and proxy icon follow the reported directory and file.")
    for offset, (_, label, _) in enumerate(_LOCATION_STEPS):
        tui.label(row + offset, 4, label)
    result_row = row + len(_LOCATION_STEPS) + 1
    step = 0
    try:
        while True:
            tui.keys(("Space", "send next"))
            tui.flush()
            key = tui.read_key()
            if key != " ":
                return key
            sequence, _, effect = _LOCATION_STEPS[step % len(_LOCATION_STEPS)]
            tui.write(sequence or _restore_directory())
            tui.write(move(result_row, 4), CSI + "K")
            tui.result(result_row, 4, effect)
            step += 1
    finally:
        tui.write(_restore_directory())
        tui.flush()


# ---------------------------------------------------------------------------
# Password mode (private mode 7401)
# ---------------------------------------------------------------------------

def _demo_password(tui):
    row = tui.page(TITLE, "A lock sits after the caret while the mode is set.")
    tui.label(row, 4, "CSI ? 7401 h   assert a password prompt")
    tui.label(row + 1, 4, "CSI ? 7401 l   clear it")
    prompt_row = row + 3
    tui.write(move(prompt_row, 4), "Password: ")
    on = True
    try:
        while True:
            tui.write(move(row + 5, 4), CSI + "K")
            tui.result(row + 5, 4, "Mode 7401 is set." if on else "Mode 7401 is reset.")
            tui.keys(("Space", "toggle"))
            tui.write(CSI + ("?7401h" if on else "?7401l"),
                      move(prompt_row, 14), CSI + "?25h")
            tui.flush()
            key = tui.read_key()
            if key != " ":
                return key
            on = not on
    finally:
        tui.write(CSI + "?7401l", CSI + "?25l", RESET)
        tui.flush()


ENTRIES = [
    Entry(
        title="Prompt marks (OSC 133)",
        explanation=(
            "A marks the prompt, B the start of input, C the command running and "
            "D its end with exit code 0. The text prints as ordinary text; once C "
            "arrives the prompt row carries a bracket in each side gutter, in the "
            "palette's blue for code 0."),
        run=_demo_osc133,
        sequence=_OSC133_SEQ,
        golden="shell_osc133_marks",
    ),
    Entry(
        title="VS Code prompt marks (OSC 633)",
        explanation=(
            "633 ; A to D set the same marks as OSC 133, so the prompt row gets "
            "the same bracket. E records the command line and P ; Cwd= the "
            "working directory. The exit code after D is not recorded, so the "
            "bracket stays blue."),
        run=_demo_osc633,
        sequence=_OSC633_SEQ,
        golden="shell_osc633_marks",
    ),
    Entry(
        title="Exit status (OSC 133 D)",
        explanation=(
            "A non-zero code after OSC 133 ; D tints that command's prompt "
            "bracket red. The cycle ending in D ; 0 keeps the blue bracket; the "
            "one ending in D ; 1 turns red."),
        run=_demo_exit_status,
        sequence=_EXIT_SEQ,
    ),
    Entry(
        title="iTerm2 shell state (OSC 1337)",
        explanation=(
            "CursorShape=1, 2 and 0 make the cursor a bar, an underline and a "
            "block; CurrentDir= sets the working directory the window title "
            "names. The cursor shape and directory are restored on return."),
        run=_demo_osc1337,
        sequence=_OSC1337_SEQ,
        golden="shell_osc1337_subcommands",
    ),
    Entry(
        title="Working directory and document (OSC 7, OSC 6)",
        explanation=(
            "OSC 7 with a file:// URL sets the working directory, which the "
            "window title names and the proxy icon stands on; OSC 6 names a "
            "document, which the proxy icon then follows. An empty OSC 6 clears "
            "it, and the starting directory is reported again on return."),
        run=_demo_location,
        sequence=_OSC7_SEQ + _OSC6_SEQ,
    ),
    Entry(
        title="Password mode (?7401)",
        explanation=(
            "Private mode 7401 asserts a password prompt: a lock is drawn just "
            "after the caret until the mode is reset. The application owns the "
            "state, so the prompt heuristic neither raises nor clears it."),
        run=_demo_password,
        sequence="Password: \x1b[?7401h",
    ),
]
