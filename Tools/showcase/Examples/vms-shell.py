#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Faux VAX/VMS V5.5-2 DCL shell.

Period-correct VT220/VT320 escape sequences throughout: cursor
positioning + save/restore (CUP, DECSC/DECRC), erase ops (ED/EL),
DEC special-graphics charset for box-drawn borders, SGR attributes
(bold / dim / reverse / blink / underline), DECTCEM cursor hide
during full-screen draws, DECDWL/DECDHL for banners, XTWINOPS 8 to
size the window to 80x24 on launch.
"""

import datetime
import os
import sys
import termios
import tty


# ─────── escape-sequence vocabulary ─────────────────────────────────
ESC = "\033"
CSI = ESC + "["

# Erasure + clear
CLEAR     = CSI + "2J" + CSI + "H"
EL_RIGHT  = CSI + "0K"   # erase line cursor → end
EL_ALL    = CSI + "2K"   # erase whole line

# Cursor positioning
def at(r: int, c: int) -> str:
    """CUP row;col 1-based."""
    return f"{CSI}{r};{c}H"
HOME      = CSI + "H"
HIDE_CURS = CSI + "?25l"      # DECTCEM off
SHOW_CURS = CSI + "?25h"      # DECTCEM on
SAVE_C    = ESC + "7"          # DECSC
RSTR_C    = ESC + "8"          # DECRC

# Line attributes (DECDWL/DECDHL)
DHL_TOP   = ESC + "#3"
DHL_BOT   = ESC + "#4"
DWL       = ESC + "#6"
SWL       = ESC + "#5"

# Window size: XTWINOPS for the initial 80x24, then DECCOLM only.
# DECCOLM resizes the terminal cell grid *without* growing the window —
# cells physically compress in 132-col mode, matching VT100 hardware.
# XTWINOPS goes through the window-resize path, which grows the window,
# defeating the compression — so we never send it for 80↔132 toggles.
RESIZE_80x24  = CSI + "8;24;80t"
# DECCOLM is ignored until mode 40 allows it: the switch to 132 allows it,
# and the switch back to 80 resets it once DECCOLM has been obeyed.
DECCOLM_132   = CSI + "?40h" + CSI + "?3h"
DECCOLM_80    = CSI + "?3l" + CSI + "?40l"

# Tab-stop control
HTS = ESC + "H"        # set horizontal tab stop at cursor column
TBC = CSI + "3g"       # clear all tab stops

# Charset designation
G0_GFX    = ESC + "(0"         # G0 = DEC special graphics (box-draw)
G0_ASCII  = ESC + "(B"         # G0 = US ASCII
SI        = "\x0f"             # invoke G0 as GL
SO        = "\x0e"             # invoke G1 as GL

# SGR
RESET     = CSI + "0m"
BOLD      = CSI + "1m"
DIM       = CSI + "2m"
ITAL      = CSI + "3m"
ULINE     = CSI + "4m"
BLINK     = CSI + "5m"
REV       = CSI + "7m"
NOREV     = CSI + "27m"
FG_BLACK  = CSI + "30m"
FG_RED    = CSI + "31m"
FG_GREEN  = CSI + "32m"
FG_YELLOW = CSI + "33m"
FG_BLUE   = CSI + "34m"
FG_MAGEN  = CSI + "35m"
FG_CYAN   = CSI + "36m"
FG_WHITE  = CSI + "37m"
BG_BLUE   = CSI + "44m"

# DEC special-graphics glyphs (printable byte → box piece when
# G0=DEC special graphics is active via \033(0 + SI):
#   l  ┌  k  ┐  m  └  j  ┘
#   q  ─  x  │
#   t  ├  u  ┤  w  ┬  v  ┴  n  ┼
TL, TR, BL, BR = "l", "k", "m", "j"
HORIZ, VERT    = "q", "x"
TLEFT, TRIGHT  = "t", "u"
TTOP, TBOT     = "w", "v"
CROSS          = "n"


def out(*chunks: str) -> None:
    sys.stdout.write("".join(chunks))
    sys.stdout.flush()


def read_uppercase_line() -> str:
    """Raw-mode keypress loop: echoes UPPERCASE characters regardless
    of what's typed (the VT220 CAPS LOCK convention). Handles BS / DEL
    to retract chars, RETURN to commit, Ctrl-C / Ctrl-D to bail.
    Returns the typed line; raises EOFError on Ctrl-D with empty buf."""
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setcbreak(fd)
    buf: list[str] = []
    try:
        while True:
            ch = os.read(fd, 1)
            if not ch:
                if not buf:
                    raise EOFError
                break
            b = ch[0]
            if b in (0x04,):                     # Ctrl-D
                if not buf:
                    raise EOFError
                continue
            if b in (0x03,):                     # Ctrl-C
                out("^C\n")
                buf = []
                return ""
            if b in (0x0a, 0x0d):                # RETURN
                out("\n")
                break
            if b in (0x7f, 0x08):                # DEL / BS
                if buf:
                    buf.pop()
                    out("\b \b")
                continue
            if b < 0x20 or b > 0x7e:             # non-printable
                continue
            c = chr(b).upper()
            buf.append(c)
            out(c)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
    return "".join(buf)


# ─────── primitives ─────────────────────────────────────────────────
def banner(text: str) -> None:
    """DECDHL pair: top half + bottom half = doubled-up heading."""
    out(DHL_TOP, text, "\n", DHL_BOT, text, "\n")


def box(row: int, col: int, height: int, width: int, title: str = "") -> None:
    """Draw a box-drawn frame using DEC special graphics. Inside-area
    after the call: rows (row+1..row+height-2), cols (col+1..col+width-2)."""
    out(G0_GFX, SI)
    # Top
    out(at(row, col), TL, HORIZ * (width - 2), TR)
    # Sides
    for r in range(1, height - 1):
        out(at(row + r, col), VERT)
        out(at(row + r, col + width - 1), VERT)
    # Bottom
    out(at(row + height - 1, col), BL, HORIZ * (width - 2), BR)
    out(SO, G0_ASCII)
    if title:
        title_pad = f" {title} "
        out(at(row, col + 2), BOLD, REV, title_pad, RESET)


def status_line(text: str) -> None:
    """Bottom of screen status (row 24), reverse-video, full width."""
    out(SAVE_C, at(24, 1), REV, f"{text:<80}", RESET, RSTR_C)


# ─────── login banner ───────────────────────────────────────────────
def show_login() -> None:
    out(HIDE_CURS, CLEAR, SWL)
    out("\n")
    banner("    Welcome to VAX/VMS")
    out("\n")
    out(DWL, "              Digital Equipment Corporation", SWL, "\n\n")
    out(f"    {REV}VAX/VMS Version V5.5-2{NOREV}",
        f"   on node {REV}ZK3-19::{NOREV}\n")
    out(f"    Last interactive login on Thursday,{DIM}  3-MAR-1994 09:41{RESET}\n")
    out(f"    Last non-interactive login on Wednesday,{DIM} 2-MAR-1994 23:00{RESET}\n\n")
    out(f"    {BLINK}You have 3 new mail messages.{RESET}\n\n")
    out(f"    Type {BOLD}HELP{RESET} for command list, ",
        f"{BOLD}LOGOUT{RESET} to disconnect.\n\n")
    status_line(" ZK3-19::DEMO    VAX/VMS V5.5-2    "
                "LAT terminal LTA1234:    HELP for help")
    out(SHOW_CURS)


# ─────── HELP screen ────────────────────────────────────────────────
def cmd_help() -> None:
    out(HIDE_CURS, CLEAR)
    banner(" VMS Help")
    out("\n")
    box(5, 4, 19, 72, "Command Verbs")
    rows = [
        ("HELP",          "Display this help screen"),
        ("DIR",           "List files in current directory"),
        ("SHOW USERS",    "Show currently logged-in users"),
        ("SHOW TIME",     "Show system date and time"),
        ("SHOW SYSTEM",   "Show system resource summary (or MEMORY)"),
        ("SHOW DEFAULT",  "Show current default directory"),
        ("SHOW PROCESS",  "Show this process info (boxed)"),
        ("PURGE",         "Purge old file versions"),
        ("FINGER",        "Berkeley-style user listing"),
        ("MAIL",          "Read and send electronic mail"),
        ("EDIT",          "Invoke EDT keypad editor (stub)"),
        ("VTTEST",        "VT101 character-set demo"),
        ("ORDER / FORM",  "VT131 Customer Order Entry form (132 col)"),
        ("SETUP / 132",   "VT100 SET-UP A (80-col / 132-col variant)"),
        ("SET WIDTH n",   "Switch to 80- or 132-column mode"),
        ("CLEAR",         "Clear the terminal screen"),
        ("LOGOUT",        "Disconnect from VMS"),
    ]
    for i, (name, desc) in enumerate(rows):
        out(at(7 + i, 6), f"{BOLD}{name:<14}{RESET}{desc}")
    status_line(" HELP   Type a verb at the $ prompt, RETURN to run it")
    out(SHOW_CURS, at(23, 1))


# ─────── DIR ────────────────────────────────────────────────────────
def cmd_dir() -> None:
    out(RESET)
    out(f"\nDirectory {REV}DISK$USER:[DEMO]{NOREV}\n\n")
    out(f"{BOLD}{'File':<20}{'Size':>6}  {'Created':<20}{'Owner':>14}{RESET}\n")
    out("─" * 70 + "\n")  # plain dash; box-drawing version below
    files = [
        ("LOGIN.COM;3",    12,   " 3-MAR-1994 09:40"),
        ("NOTES.TXT;7",    84,   " 2-MAR-1994 14:22"),
        ("BUDGET.DAT;1",  256,   " 1-MAR-1994 17:03"),
        ("REPORT.TEX;12", 1024, "28-FEB-1994 23:55"),
        ("FORTRAN.FOR;2",  48,  "25-FEB-1994 11:30"),
    ]
    for name, sz, when in files:
        out(f"{FG_CYAN}{name:<20}{RESET}{sz:>6}  {when:<20}{'[DEMO]':>14}\n")
    out(f"\n{BOLD}Total of 5 files, 1424 blocks.{RESET}\n\n")


# ─────── SHOW USERS ─────────────────────────────────────────────────
def cmd_show_users() -> None:
    out(HIDE_CURS, CLEAR)
    banner("  Logged-In Users")
    out("\n")
    box(5, 4, 11, 72, "Interactive Sessions")
    out(at(7, 6),
        f"{BOLD}{'Node':<8}{'Username':<11}{'PID':<11}{'Process Name':<20}"
        f"{'TTY':<8}{RESET}")
    users = [
        (FG_GREEN,  "ZK3-19",  "DEMO",     "0000043B", "DECW$SESSION",   "VTA42:"),
        (FG_YELLOW, "ZK3-19",  "SYSTEM",   "00000401", "AUDIT_SERVER",   "BG1:"),
        ("",        "ZK3-19",  "EFFEL",    "0000038A", "$REMACS",        "TXA0:"),
        ("",        "ZK3-19",  "JTANYA",   "0000049C", "$MAIL",          "TXA1:"),
        ("",        "ZK3-19",  "RTHOMSON", "00000211", "EDT",            "LTA32:"),
    ]
    for i, (col, node, name, pid, proc, tty) in enumerate(users):
        prefix = col if col else ""
        suffix = RESET if col else ""
        out(at(9 + i, 6),
            f"{prefix}{node:<8}{name:<11}{pid:<11}{proc:<20}{tty:<8}{suffix}")
    out(at(15, 6), f"{DIM}5 users connected.{RESET}")
    status_line(" SHOW USERS    Refresh: $ SHOW USERS    Return: RETURN")
    out(SHOW_CURS, at(23, 1))


# ─────── SHOW TIME ──────────────────────────────────────────────────
def cmd_show_time() -> None:
    now = datetime.datetime.now().strftime("%d-%b-%Y %H:%M:%S").upper()
    out(RESET, f"\n   The system time is {BOLD}{now}{RESET}\n\n")


# ─────── SHOW DEFAULT ───────────────────────────────────────────────
def cmd_show_default() -> None:
    """One-liner DCL response, value highlighted in reverse-video the
    way the real SHOW DEFAULT printed it on a VT terminal."""
    out(RESET, f"\n   {REV}DISK$USER:[DEMO]{NOREV}\n\n")


# ─────── SHOW PROCESS ───────────────────────────────────────────────
def cmd_show_process() -> None:
    """Boxed process-info display; mirrors the SHOW PROCESS/PRIV
    output: BOLD labels, REV values, DEC special-graphics border."""
    now = datetime.datetime.now().strftime("%d-%b-%Y %H:%M:%S").upper()
    out(HIDE_CURS, CLEAR)
    banner("   Process Info")
    out("\n")
    box(5, 4, 15, 72, "Process DEMO  (PID 0000043B)")
    fields = [
        ("Process name:",  "DEMO"),
        ("Username:",      "DEMO"),
        ("Account:",       "ENGINEERING"),
        ("UIC:",           "[GRP12,DEMO]"),
        ("PID:",           "0000043B"),
        ("Priority:",      "4   (base 4)"),
        ("Elapsed CPU:",   "0 00:14:31.07"),
        ("Page faults:",   "23,118"),
        ("Working set:",   "512 / 1024 / 4096 pages"),
        ("Direct I/O:",    "1,847   Buffered I/O: 9,302"),
        ("Login at:",      now),
    ]
    for i, (k, v) in enumerate(fields):
        out(at(7 + i, 6), f"{BOLD}{k:<16}{RESET} {REV}{v}{NOREV}")
    status_line(" SHOW PROCESS   $ HELP SHOW PROCESS for qualifiers")
    out(SHOW_CURS, at(23, 1))


# ─────── PURGE ──────────────────────────────────────────────────────
def cmd_purge() -> None:
    """Faux file-version cleanup. Period-correct %PURGE-I-FILPURG
    informational message for each old version deleted; trailing
    summary mirrors what the real PURGE printed."""
    out(RESET, "\n")
    deleted = [
        ("DISK$USER:[DEMO]LOGIN.COM;2",   2),
        ("DISK$USER:[DEMO]LOGIN.COM;1",   2),
        ("DISK$USER:[DEMO]NOTES.TXT;6",  12),
        ("DISK$USER:[DEMO]NOTES.TXT;5",  12),
        ("DISK$USER:[DEMO]REPORT.TEX;11", 162),
    ]
    for path, blocks in deleted:
        out(f"   {FG_YELLOW}%PURGE-I-FILPURG{RESET}, ",
            f"{FG_CYAN}{path}{RESET} deleted ({blocks} blocks)\n")
    total = sum(b for _, b in deleted)
    out(f"\n   {BOLD}Total of {len(deleted)} files deleted, "
        f"{total} blocks freed.{RESET}\n\n")


# ─────── FINGER ─────────────────────────────────────────────────────
def cmd_finger() -> None:
    """Berkeley-style finger over DECnet. Boxed listing with one
    BOLD-labelled .PLAN excerpt for the local user — a touch of
    1990s VAX flavour."""
    out(HIDE_CURS, CLEAR)
    banner("      Finger")
    out("\n")
    box(5, 4, 14, 72, "Users on ZK3-19::")
    out(at(7, 6),
        f"{BOLD}{'Login':<11}{'Name':<22}{'TTY':<9}{'Idle':<7}"
        f"{'When':<14}{RESET}")
    rows = [
        ("DEMO",     "Demo User",              "VTA42:",  "    ", "Thu 09:40"),
        ("SYSTEM",   "System Manager",         "BG1:",    "  -- ", "Mon 06:00"),
        ("EFFEL",    "F. F. Effelsberg",       "TXA0:",   "  12m", "Wed 22:11"),
        ("JTANYA",   "Jenny Tanya",            "TXA1:",   "   3m", "Thu 08:51"),
        ("RTHOMSON", "Robert Thomson",         "LTA32:",  "1:04 ", "Thu 07:36"),
    ]
    for i, (login, name, tty, idle, when) in enumerate(rows):
        col = FG_CYAN if login == "DEMO" else ""
        rst = RESET if col else ""
        out(at(9 + i, 6),
            f"{col}{login:<11}{rst}{name:<22}{tty:<9}{idle:<7}{when:<14}")
    out(at(15, 6), G0_GFX, SI, HORIZ * 68, SO, G0_ASCII)
    out(at(16, 6), f"{BOLD}Plan for DEMO:{RESET}")
    out(at(17, 6),
        f"  {DIM}Tune the DECnet sizing model.  Ship Q1 figures by Fri.{RESET}")
    status_line(" FINGER   $ FINGER user[@node] for a single account")
    out(SHOW_CURS, at(23, 1))


# ─────── SHOW SYSTEM ────────────────────────────────────────────────
def cmd_show_system() -> None:
    out(HIDE_CURS, CLEAR)
    banner(" System Resources")
    out("\n")
    box(5, 4, 13, 72, "Node ZK3-19  (VAX 11/780)")
    fields = [
        ("Uptime",       "14 days, 03:47:21"),
        ("Load avg",     "0.42 / 0.51 / 0.38"),
        ("Free pages",   f"4192 / 8192   ({BOLD}{FG_GREEN}51%% free{RESET})"),
        ("IO rate",      f"{FG_GREEN}12.3 KB/s{RESET}  (peak 89 KB/s)"),
        ("Free blocks",  "182,431 (DISK$USER:)"),
        ("Page faults",  "  3,128 /sec"),
        ("Login attempts", " 14 today, 2 failed"),
        ("Job count",    " 47 batch / 12 interactive"),
    ]
    for i, (k, v) in enumerate(fields):
        out(at(7 + i, 6), f"{BOLD}{k:<16}{RESET}{v}")
    status_line(" SHOW SYSTEM   $ HELP SHOW for related topics")
    out(SHOW_CURS, at(23, 1))


# ─────── MAIL — authentic subsystem ─────────────────────────────────
MAIL_MSGS = [
    ("SYSTEM",  "Backup completed successfully", " 3-MAR-1994 04:00",
     ("Nightly BACKUP/IMAGE completed at 04:00:12 on node ZK3-19.\n"
      "Volume label TAPE001 dismounted. Next scheduled run: tomorrow\n"
      "at 04:00.\n\n"
      "  Files saved:  14,832\n"
      "  Bytes:        2.4 GB\n"
      "  Errors:       0\n")),
    ("JTANYA",  "RE: Q4 capacity planning",      " 2-MAR-1994 16:14",
     ("DEMO,\n\n"
      "Re: your forecast — the load projections look right but I\n"
      "think the DECnet sizing is conservative. We hit 78% utilisation\n"
      "during the Feb close; another 15% in Q4 will push us past 90%.\n\n"
      "Let's chat at the staff meeting Thursday.\n\n"
      "--Jenny\n")),
    ("EFFEL",   "Patch tape arrived",            " 2-MAR-1994 11:09",
     ("ECO patch tape for VMS V5.5-2 came in this morning. Per the\n"
      "release notes we should apply:\n\n"
      "  VMSKIT-V0552-1     System kit (mandatory)\n"
      "  DECNET-V0552-1     DECnet patches\n"
      "  MAIL-V0552-1       MAIL fixes (recommended)\n\n"
      "Plan for a 30-min outage Friday evening.\n")),
]


def mail_list(selected: int) -> None:
    """Re-paint the mailbox listing with `selected` (0-based) highlighted."""
    out(HIDE_CURS, CLEAR)
    banner("   VAX/VMS MAIL")
    out("\n")
    box(5, 4, 9, 72, f"Mailbox NEWMAIL  ({len(MAIL_MSGS)} messages)")
    out(at(7, 6),
        f"{BOLD}{'#':<3}{'From':<12}{'Subject':<38}{'Date':<18}{RESET}")
    for i, (who, subj, when, _) in enumerate(MAIL_MSGS):
        row = 9 + i
        sel = (i == selected)
        col_fg = FG_YELLOW if who == "SYSTEM" else FG_CYAN
        if sel:
            out(at(row, 6),
                f"{REV}{i+1:<3}{who:<12}{subj:<38}{when:<18}{NOREV}")
        else:
            out(at(row, 6),
                f"{col_fg}{i+1:<3}{who:<12}{RESET}{subj:<38}{col_fg}{when:<18}{RESET}")
    status_line(" MAIL>  READ n   REPLY   DELETE   SEND   DIR   EXIT")


def mail_read(idx: int) -> None:
    """Display a single message in a box, return after RETURN keypress."""
    if idx < 0 or idx >= len(MAIL_MSGS):
        out(RESET, f"\n%MAIL-E-NOSUCHMSG, no message {idx+1}\n")
        return
    who, subj, when, body = MAIL_MSGS[idx]
    out(HIDE_CURS, CLEAR)
    banner("   VAX/VMS MAIL")
    out("\n")
    box(5, 4, 17, 72, f"Message {idx+1}  of {len(MAIL_MSGS)}")
    out(at(7,  6), f"{BOLD}From:{RESET}    {who}@ZK3-19::")
    out(at(8,  6), f"{BOLD}To:{RESET}      DEMO")
    out(at(9,  6), f"{BOLD}Subj:{RESET}    {subj}")
    out(at(10, 6), f"{BOLD}Date:{RESET}    {when.strip()}")
    out(at(11, 5), G0_GFX, SI, HORIZ * 70, SO, G0_ASCII)
    for i, line in enumerate(body.splitlines()):
        if 12 + i > 20:
            break
        out(at(12 + i, 6), line[:65])
    status_line(" MAIL>  Press RETURN to continue,  NEXT / PREV / DELETE / REPLY")


def cmd_mail() -> None:
    """MAIL subsystem: own prompt + command loop. EXIT returns to DCL."""
    selected = 0
    mail_list(selected)
    while True:
        out(RESET, at(23, 1), EL_ALL, "MAIL> ", SHOW_CURS)
        try:
            line = read_uppercase_line()
        except EOFError:
            out("\n")
            return
        parts = line.strip().split(maxsplit=1)
        if not parts:
            continue
        verb = parts[0]
        arg  = parts[1] if len(parts) > 1 else ""
        if verb in ("EXIT", "QUIT"):
            return
        if verb == "DIR":
            mail_list(selected)
            continue
        if verb == "READ":
            n = int(arg) - 1 if arg.isdigit() else selected
            mail_read(n)
            # wait for any key, then re-show list
            read_uppercase_line()
            selected = max(0, min(len(MAIL_MSGS) - 1, n))
            mail_list(selected)
            continue
        if verb == "NEXT":
            if selected < len(MAIL_MSGS) - 1:
                selected += 1
            mail_list(selected)
            continue
        if verb == "PREV":
            if selected > 0:
                selected -= 1
            mail_list(selected)
            continue
        if verb == "DELETE":
            del MAIL_MSGS[selected]
            if selected >= len(MAIL_MSGS) and selected > 0:
                selected -= 1
            if not MAIL_MSGS:
                out(at(23, 1), EL_ALL,
                    f"{RESET}%MAIL-I-NONEW, no messages, returning to DCL\n")
                return
            mail_list(selected)
            continue
        if verb in ("REPLY", "SEND", "ANSWER"):
            out(RESET, at(23, 1), EL_ALL,
                "%MAIL-W-NOTIMPL, command stubbed in this demo")
            continue
        out(RESET, at(23, 1), EL_ALL,
            f"%MAIL-W-IVVERB, no such MAIL command \\{verb}\\")


# ─────── unknown verb ───────────────────────────────────────────────
def cmd_unknown(verb: str) -> None:
    out(RESET)
    out(f"\n   {FG_YELLOW}%DCL-W-IVVERB, unrecognized command verb"
        f"{RESET} - check {BOLD}{verb}{RESET}\n",
        f"   {DIM}\\{verb}\\{RESET}\n\n")


# ─────── EDIT stub (period-correct EDT splash) ──────────────────────
def cmd_edit() -> None:
    out(HIDE_CURS, CLEAR, BG_BLUE, FG_WHITE)
    # Fill screen with bg
    for r in range(1, 25):
        out(at(r, 1), " " * 80)
    out(at(1, 30), BOLD, "EDT V3.0", RESET, BG_BLUE, FG_WHITE)
    out(at(3, 4), "Welcome to EDT keypad editor.")
    out(at(5, 4), "Type ", BOLD, "HELP", RESET, BG_BLUE, FG_WHITE,
        " then keypad ", BOLD, "PF2", RESET, BG_BLUE, FG_WHITE,
        " for help.")
    out(at(7, 4), "Press ", BOLD, "Ctrl-Z", RESET, BG_BLUE, FG_WHITE,
        " to exit.")
    out(at(24, 1), REV, f"{' EDT  *Main*   line 1 of 1   [End of buffer]':<80}",
        RESET)
    out(SHOW_CURS, at(9, 4))


# ─────── VTTEST — VT101 character-set demo ──────────────────────────
def cmd_vttest() -> None:
    """Faithful reproduction of the page-1 'character sets and display
    attributes' demo from every VT100/VT101 manual: USASCII, UK set,
    DEC special graphics, DECDWL, DECDHL. Period sequences only —
    ESC ( B / ESC ( A / ESC ( 0 for G0 designators, SI/so to invoke,
    ESC # 6 for double-width, ESC # 3 + ESC # 4 for double-height."""
    out(HIDE_CURS, CLEAR, SWL, RESET)
    out(at(1, 23), DHL_TOP, "DIGITAL VT101")
    out(at(2, 23), DHL_BOT, "DIGITAL VT101")
    out(at(4, 5),
        "General character sets may be selected in the VT101.")
    # USASCII (ESC ( B = G0 → USASCII)
    out(at(6, 5),
        "The normal set is the USASCII set of letters and graphics.")
    out(at(7, 7), ESC + "(B", SI,
        " !\"#$%&'()*+,-./0123456789:;<=>?@")
    out(at(8, 7),
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~")
    # UK set (ESC ( A = G0 → United Kingdom; only difference is # → £)
    out(at(10, 5),
        "There is a similar set for the United Kingdom.")
    out(at(11, 7), ESC + "(A", SI,
        " !\"#$%&'()*+,-./0123456789:;<=>?@")
    out(at(12, 7),
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~")
    out(ESC + "(B")
    # DEC special graphics (ESC ( 0). With G0=DEC GFX active via SI:
    #   l ┌  k ┐  m └  j ┘   q ─  x │   t ├  u ┤  w ┬  v ┴  n ┼
    #   a ▒ (shade)  ` ◆  ~ ·  o ⎺  p ⎻  r ⎼  s ⎽
    out(at(14, 5),
        "Special graphics may be needed.")
    out(at(15, 7), G0_GFX, SI,
        "lqqqwqqqk    lqqqqqk    lqwqk")
    out(at(16, 7),
        "x   x   x    x     x    x x x")
    out(at(17, 7),
        "tqqqnqqqu    tqqqqqu    nqnqn")
    out(at(18, 7),
        "mqqqvqqqj    mqqqqqj    mqvqj")
    out(SO, G0_ASCII)
    # DECDWL pair — each escape sets the current line's attribute
    out(at(19, 1), DWL, at(19, 5),
        "Double width may be selected on a line")
    out(at(20, 1), DWL, at(20, 5), "by line basis.")
    # DECDHL pair on rows 22-23, status line on 24 (stays SWL)
    out(at(21, 1), SWL, at(21, 5),
        "And there is the double height line:")
    out(at(22, 1), DHL_TOP, at(22, 16), "DOUBLE HEIGHT")
    out(at(23, 1), DHL_BOT, at(23, 16), "DOUBLE HEIGHT")
    status_line(" VTTEST   Press RETURN to return to DCL prompt")
    out(SHOW_CURS, at(24, 80))
    try:
        read_uppercase_line()
    except EOFError:
        pass
    out(SWL, RESET, CLEAR)


# ─────── SETUP — VT100 set-up a reproduction ────────────────────────
def _setup_paint_header() -> None:
    """Common set-up a header: DECDHL title + DECDWL+underlined exit
    hint. DEC's actual set-up a used double-width for the exit line so
    it'd remain legible at a glance during a hurried recovery from
    set-up mode — matches PCx80's reproduction."""
    out(at(1, 1), DHL_TOP, "SET-UP A")
    out(at(2, 1), DHL_BOT, "SET-UP A")
    out(at(3, 1), DWL, ULINE, 'TO EXIT PRESS "SET-UP"', RESET)


def cmd_setup() -> None:
    """PCx80-style set-up a: 80-column mode, ruler is 8 segments of
    '1234567890' with cols 6-10 of each segment in reverse video as a
    decade-counter aid. 'T' markers above the start of each REV block.
    Matches what most VT100 emulators (PCjs/PCx80, xterm with vt100)
    actually show users — readable, even if not strictly DEC-spec."""
    out(DECCOLM_80, HIDE_CURS, CLEAR, SWL, RESET)
    _setup_paint_header()
    # T markers at cols 6, 16, 26, ..., 76 — start of each REV block
    line23 = [" "] * 80
    for col in range(6, 81, 10):
        line23[col - 1] = "T"
    out(at(23, 1), "".join(line23))
    # Ruler: 8 × '1234567890', cols 6-10 of each segment in REV.
    ruler_parts = []
    for seg in range(8):
        ruler_parts.append("12345")
        ruler_parts.append(REV + "67890" + NOREV)
    out(at(24, 1), "".join(ruler_parts))
    out(SHOW_CURS, at(22, 1))
    try:
        read_uppercase_line()
    except EOFError:
        pass
    out(CLEAR, RESET)


def cmd_setup132() -> None:
    """DEC-correct set-up a: switches into 132-column mode (DECCOLM),
    paints the canonical 132-char tab-stop ruler with reverse-video
    bands at the eight default tabs (cols 9, 17, ..., 129) and 'T'
    markers above each tab. More authentic to the actual VT100 set-up
    screen but visually noisy at compressed widths."""
    out(DECCOLM_132, HIDE_CURS, CLEAR, SWL, RESET)
    _setup_paint_header()
    out(TBC)
    for col in range(9, 133, 8):
        out(at(1, col), HTS)
    line23 = [" "] * 132
    for col in range(9, 133, 8):
        line23[col - 1] = "T"
    out(at(23, 1), "".join(line23))
    ruler = ("1234567890" * 14)[:132]
    out(at(24, 1))
    col = 1
    while col <= 132:
        is_tab_band = False
        for tab in range(9, 133, 8):
            if tab <= col <= tab + 4:
                is_tab_band = True
                break
        if is_tab_band:
            out(REV, ruler[col - 1], NOREV)
        else:
            out(ruler[col - 1])
        col += 1
    out(SHOW_CURS, at(22, 1))
    try:
        read_uppercase_line()
    except EOFError:
        pass
    out(DECCOLM_80, CLEAR, RESET)


# ─────── ORDER — VT131 Customer Order Entry form ────────────────────
def _form_label(row: int, col: int, label: str, value: str,
                vwidth: int) -> None:
    """Render a 'Label: [value     ]' field. Label normal, value
    reverse-video inside brackets (period-correct VT131 field style)."""
    out(at(row, col), BOLD, label, RESET, " ", REV, f"{value:<{vwidth}}",
        NOREV)


def cmd_order() -> None:
    """VT131-style Customer Order Entry form. Switches to 132-column
    mode, paints a fully boxed form (DEC special graphics borders,
    reverse-video protected/value fields, BOLD field labels), a
    line-item table, an options checklist, and a totals box on the
    right. RETURN exits back to DCL."""
    out(DECCOLM_132, HIDE_CURS, CLEAR, SWL, RESET)
    # Title banner (DECDHL) + subtitle bar
    out(at(1, 50), DHL_TOP, "DIGITAL VT131")
    out(at(2, 50), DHL_BOT, "DIGITAL VT131")
    out(at(3, 1), BOLD, REV,
        f"{'      ACME WIDGETS, INC.    Customer Order Entry  v3.2    ORDER #INV-87432':<132}",
        RESET)
    # Customer + Order boxes — rows 4-8 (5 tall, 3 internal)
    box(4, 2, 5, 64, "Customer")
    _form_label(5, 4,  "Cust No:", "12345",                 8)
    _form_label(5, 24, "Name:",    "ACME WIDGETS INC",      20)
    _form_label(6, 4,  "Address:", "1138 INDUSTRIAL PKWY",  35)
    _form_label(7, 4,  "City:",    "BOSTON, MA  02139",     35)
    box(4, 68, 5, 64, "Order")
    _form_label(5, 70,  "Order No:", "INV-87432",     12)
    _form_label(5, 100, "Date:",     "03-MAR-1994",   12)
    _form_label(6, 70,  "Shipped:",  "03-MAR-1994",   12)
    _form_label(6, 100, "Due:",      "02-APR-1994",   12)
    _form_label(7, 70,  "Terms:",    "NET 60",        10)
    _form_label(7, 100, "Loc:",      "BOS",            5)
    # Line items table — rows 9-17 (9 tall, 7 internal: header+sep+5 items)
    box(9, 2, 9, 130, "Line Items")
    out(at(10, 4),
        f"{BOLD}{'Line':<6}{'Item No.':<14}{'Description':<46}"
        f"{'Qty':>5}{'Unit Price':>14}{'Extension':>14}{RESET}")
    out(at(11, 4), G0_GFX, SI, HORIZ * 126, SO, G0_ASCII)
    items = [
        ("001", "WDG-100",  "Standard Widget",                  12,  25.00,  300.00),
        ("002", "WDG-200H", "Heavy-Duty Widget",                 5, 100.00,  500.00),
        ("003", "GDT-300",  "Gadget, 3\" Brass",                 3, 250.00,  750.00),
        ("004", "THG-400",  "Thingamajig (matched set, 2 ea.)",  2, 150.00,  300.00),
        ("005", "GRMT-50",  "Grommet, rubber, 1/2\"",           24,   1.25,   30.00),
    ]
    for i, (ln, sku, desc, qty, unit, ext) in enumerate(items):
        out(at(12 + i, 4),
            f"{FG_CYAN}{ln:<6}{RESET}{sku:<14}{desc:<46}"
            f"{qty:>5}{unit:>14,.2f}{ext:>14,.2f}")
    # Options + Totals — rows 18-23 (6 tall, 4 internal). Single-column
    # so long option descriptions ("Field Service Contract - Tier 3")
    # don't get clipped at a 28-char cell width.
    box(18, 2, 6, 64, "Specific Options")
    opts = [
        ("X", "Spare Parts (+5.5%)"),
        (" ", "Maintenance Tape (RP07 cartridge)"),
        ("X", "Software Service - Year 2"),
        (" ", "Field Service Contract - Tier 3"),
    ]
    for i, (mark, desc) in enumerate(opts):
        out(at(19 + i, 4), "[", BOLD, FG_GREEN if mark == "X" else "", mark,
            RESET, "] ", desc)
    box(18, 68, 6, 64, "Totals")
    out(at(19, 70), f"{BOLD}Subtotal:{RESET}",
        f"{REV}{'1,880.00':>50}{NOREV}")
    out(at(20, 70), f"{BOLD}Tax (Mass 5%):{RESET}",
        f"{REV}{'   94.00':>45}{NOREV}")
    out(at(21, 70), f"{BOLD}{REV}{'TOTAL DUE:':<13}",
        f"{'1,974.00':>49}{NOREV}{RESET}")
    out(at(22, 70), f"{BOLD}Status:{RESET}  ", FG_GREEN, "Ready", RESET,
        "    F4 Save  F6 Print  F9 Cancel")
    # Status row 24 — full 132-col reverse-video bar
    out(at(24, 1), REV,
        f"{' VT131  ORDER ENTRY    PF1=Save PF2=Print PF3=Help PF4=Cancel    RETURN exits to DCL':<132}",
        RESET)
    out(SHOW_CURS, at(5, 14))
    try:
        read_uppercase_line()
    except EOFError:
        pass
    out(DECCOLM_80, CLEAR, RESET)


# ─────── SET WIDTH ──────────────────────────────────────────────────
def cmd_set_width(arg: str) -> None:
    if arg == "132":
        out(DECCOLM_132, CLEAR, RESET)
    elif arg == "80":
        out(DECCOLM_80, CLEAR, RESET)
    else:
        out(RESET,
            "\n   %SET-E-NOSUCHWIDTH, supported widths are 80 and 132\n\n")


# ─────── logout ─────────────────────────────────────────────────────
def logout() -> None:
    when = datetime.datetime.now().strftime("%H:%M:%S")
    out(RESET)
    out(f"\n   {REV}DEMO{NOREV} logged out at {BOLD}{when}{RESET}\n")
    out(f"   Connection terminated. {DIM}(SYSGEN connect time 00:14:31){RESET}\n\n")


# ─────── main loop ──────────────────────────────────────────────────
def main() -> None:
    out(RESIZE_80x24)
    show_login()
    while True:
        out(RESET, "$ ")
        try:
            line = read_uppercase_line()
        except EOFError:
            out("\n")
            break
        parts = line.strip().split(maxsplit=1)
        if not parts:
            continue
        verb = parts[0].upper()
        rest = parts[1].upper() if len(parts) > 1 else ""
        if verb == "HELP":
            cmd_help()
        elif verb == "DIR":
            cmd_dir()
        elif verb == "SHOW":
            if rest == "USERS":
                cmd_show_users()
            elif rest == "TIME":
                cmd_show_time()
            elif rest in ("SYSTEM", "MEMORY"):
                cmd_show_system()
            elif rest in ("DEFAULT", "DEF"):
                cmd_show_default()
            elif rest in ("PROCESS", "PROC", "PROCESS/PRIV"):
                cmd_show_process()
            else:
                out(RESET,
                    "\n   %SHOW-E-NOPARM, what to show?  Try USERS / TIME / "
                    "SYSTEM / DEFAULT / PROCESS\n\n")
        elif verb == "PURGE":
            cmd_purge()
        elif verb == "FINGER":
            cmd_finger()
        elif verb == "MAIL":
            cmd_mail()
        elif verb == "EDIT":
            cmd_edit()
        elif verb == "VTTEST":
            cmd_vttest()
        elif verb in ("ORDER", "FORM"):
            cmd_order()
        elif verb in ("SETUP", "SET-UP"):
            cmd_setup()
        elif verb in ("SETUP132", "SET-UP132"):
            cmd_setup132()
        elif verb == "SET":
            # SET WIDTH 80  /  SET WIDTH 132
            sub = rest.split()
            if len(sub) >= 2 and sub[0] == "WIDTH":
                cmd_set_width(sub[1])
            else:
                out(RESET,
                    "\n   %SET-E-NOPARM, try SET WIDTH 80 or SET WIDTH 132\n\n")
        elif verb in ("CLEAR", "CLS"):
            out(CLEAR)
        elif verb in ("LOGOUT", "EXIT", "QUIT", "BYE"):
            logout()
            break
        else:
            cmd_unknown(verb)


if __name__ == "__main__":
    main()
