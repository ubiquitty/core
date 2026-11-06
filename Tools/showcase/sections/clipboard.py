# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Clipboard & paste: OSC 52 writes and reads, bracketed paste (mode 2004),
the control-character filter on unframed pastes, and the OSC 5522 extended
clipboard.

The write entries put text on the clipboard and then show what a paste
delivers back to this program, so the round trip is visible on the page.
Every entry that changes mode 2004 puts back the state it found.
"""

import base64
import os
import re
import select

from .. import tui as T
from ..tui import CSI, OSC, ST
from . import Entry

TITLE = "Clipboard and paste"
ORDER = 130

DEMO_TEXT = "OSC 52 clipboard write"
DEMO_B64 = base64.b64encode(DEMO_TEXT.encode()).decode("ascii")
OSC52_SET = f"{OSC}52;c;{DEMO_B64}{ST}"
PRIMARY_TEXT = "OSC 52 primary selection write"

_PASTE_START = b"\x1b[200~"
_PASTE_END = b"\x1b[201~"


def _osc52(selector, text):
    return f"{OSC}52;{selector};{base64.b64encode(text.encode()).decode('ascii')}{ST}"


def _caret(data):
    """Show bytes as text with every control byte caret-escaped."""
    out = []
    for byte in data:
        if byte < 0x20:
            out.append("^" + chr(byte + 0x40))
        elif byte == 0x7F:
            out.append("^?")
        else:
            out.append(chr(byte))
    return "".join(out)


def _read_burst(tui):
    """Block for input and return one burst of raw bytes, or b"" off a terminal.

    Reads the fd directly so paste framing and control bytes survive intact.
    """
    if not os.isatty(tui._fd):
        return b""
    select.select([tui._fd], [], [])
    chunks = []
    while True:
        r, _, _ = select.select([tui._fd], [], [], 0.05 if chunks else 0)
        if not r and chunks:
            break
        try:
            data = os.read(tui._fd, 65536)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks)


def _await_paste(tui, verb="paste"):
    """Wait for a paste; return (bytes, None), or (None, key) for Esc or no terminal."""
    tui.keys(("Cmd-V", verb))
    tui.flush()
    data = _read_burst(tui)
    if not data:
        return None, "q"
    if data == b"\x1b":
        return None, "esc"
    return data, None


def _bracketed_state(tui):
    """Whether mode 2004 is set, per DECRQM; None when the terminal does not say."""
    match = re.search(rb"\x1b\[\?2004;([0-4])\$y", tui.query(CSI + "?2004$p"))
    if not match or match.group(1) not in (b"1", b"2"):
        return None
    return match.group(1) == b"1"


def _restore_bracketed(tui, was_set):
    tui.write(CSI + ("?2004h" if was_set else "?2004l"))
    tui.flush()


def _unframe(data):
    return data.replace(_PASTE_START, b"").replace(_PASTE_END, b"")


# ---------------------------------------------------------------------------
# OSC 52 writes
# ---------------------------------------------------------------------------

def _demo_set(tui):
    row = tui.page(TITLE, "Space writes two selections; pasting brings the text back here.")
    tui.label(row, 4, f"OSC 52 ; c ; <base64 of '{DEMO_TEXT}'> ST")
    tui.label(row + 1, 4, f"OSC 52 ; p ; <base64 of '{PRIMARY_TEXT}'> ST")
    tui.keys(("Space", "write"))
    tui.flush()
    key = tui.read_key()
    if key != " ":
        return key
    tui.write(OSC52_SET, _osc52("p", PRIMARY_TEXT))
    tui.result(row + 3, 4, "Written. Cmd-V pastes selection c; a middle click pastes p.")
    was_set = _bracketed_state(tui)
    tui.write(CSI + "?2004h")
    try:
        data, key = _await_paste(tui)
        if data is None:
            return key
        tui.label(row + 5, 4, "pasted:")
        tui.result(row + 5, 13, T.truncate(_caret(_unframe(data)), tui.cols - 14))
        return tui.footer()
    finally:
        _restore_bracketed(tui, bool(was_set))


_LONG_TEXT = "Ubiquitty clipboard stress: " + "0123456789" * 15


def _demo_long(tui):
    b64 = base64.b64encode(_LONG_TEXT.encode()).decode("ascii")
    row = tui.page(TITLE, "One OSC 52 carries the whole body; the paste brings back every byte.")
    tui.label(row, 4, f"OSC 52 ; c ; <{len(b64)} base64 characters> ST   {len(_LONG_TEXT)} bytes")
    width = tui.cols - 8
    lines = [b64[offset:offset + width] for offset in range(0, len(b64), width)]
    for index, line in enumerate(lines):
        tui.label(row + 1 + index, 4, line)
    tui.write(f"{OSC}52;c;{b64}{ST}")
    result_row = row + len(lines) + 2
    was_set = _bracketed_state(tui)
    tui.write(CSI + "?2004h")
    try:
        data, key = _await_paste(tui)
        if data is None:
            return key
        pasted = _unframe(data)
        verdict = "identical to what was written" if pasted == _LONG_TEXT.encode() else "different from what was written"
        tui.result(result_row, 4, f"Pasted {len(pasted)} bytes, {verdict}.")
        return tui.footer()
    finally:
        _restore_bracketed(tui, bool(was_set))


# ---------------------------------------------------------------------------
# OSC 52 read
# ---------------------------------------------------------------------------

def _decode_osc52(reply):
    match = re.search(rb"52;[^;]*;([A-Za-z0-9+/=]*)", reply or b"")
    if not match or not match.group(1):
        return None
    try:
        return base64.b64decode(match.group(1)).decode("utf-8", "replace")
    except ValueError:
        return None


def _demo_read(tui):
    row = tui.page(TITLE, "The terminal answers with the clipboard's text, once the read is allowed.")
    tui.label(row, 4, "OSC 52 ; c ; ? ST")
    reply = tui.query(f"{OSC}52;c;?{ST}", timeout=5.0)
    decoded = _decode_osc52(reply)
    if decoded is None:
        tui.unavailable(row + 2, "No clipboard text came back; the read was refused or not answered.")
    else:
        tui.result(row + 2, 4, T.truncate(decoded, tui.cols - 6))
    return tui.footer()


# ---------------------------------------------------------------------------
# Bracketed paste and the unframed-paste filter
# ---------------------------------------------------------------------------

def _demo_bracketed(tui):
    row = tui.page(TITLE, "A paste arrives between ESC [ 200~ and ESC [ 201~; typed keys do not.")
    tui.label(row, 4, "CSI ? 2004 h")
    was_set = _bracketed_state(tui)
    tui.write(CSI + "?2004h")
    try:
        data, key = _await_paste(tui)
        if data is None:
            return key
        tui.label(row + 2, 4, "received:")
        tui.result(row + 2, 14, T.truncate(_caret(data), tui.cols - 15))
        return tui.footer()
    finally:
        _restore_bracketed(tui, bool(was_set))


# Visible text between the controls a paste must not type at a shell, plus a
# TAB and a newline that survive and a trailing DEL.
HOSTILE_PASTE = "git\x03 push\x04 --force\x1a\x07\tlogin\nrm -rf /\x7f"


def _demo_sanitize(tui):
    row = tui.page(TITLE, "Space puts control bytes on the clipboard; the paste arrives without them.")
    tui.label(row, 4, "CSI ? 2004 l, then OSC 52 ; c ; <base64 of the line below> ST")
    tui.label(row + 1, 4, "written:")
    tui.text(row + 1, 14, _caret(HOSTILE_PASTE.encode()))
    was_set = _bracketed_state(tui)
    tui.write(CSI + "?2004l")
    try:
        tui.keys(("Space", "write"))
        tui.flush()
        key = tui.read_key()
        if key != " ":
            return key
        tui.write(_osc52("c", HOSTILE_PASTE))
        data, key = _await_paste(tui)
        if data is None:
            return key
        tui.label(row + 2, 4, "pasted:")
        tui.result(row + 2, 14, T.truncate(_caret(data), tui.cols - 15))
        return tui.footer()
    finally:
        _restore_bracketed(tui, bool(was_set))


# ---------------------------------------------------------------------------
# OSC 5522 extended clipboard
# ---------------------------------------------------------------------------

MIME_TEXT_B64 = base64.b64encode(b"text/plain").decode("ascii")
MIME_LIST_B64 = base64.b64encode(b".").decode("ascii")
_WRITE_SEQ = (f"{OSC}5522;type=write{ST}"
              f"{OSC}5522;type=wdata:mime={MIME_TEXT_B64};"
              + base64.b64encode(b"OSC 5522 write chunk").decode("ascii")
              + f"{ST}{OSC}5522;type=wdata{ST}")


def _reply_page(caption, label, sequence, timeout=0.3):
    def run(tui):
        row = tui.page(TITLE, caption)
        tui.label(row, 4, label)
        reply = tui.query(sequence, timeout=timeout)
        if reply:
            tui.result(row + 2, 4, T.truncate(T.decode_reply(reply), tui.cols - 6))
        else:
            tui.unavailable(row + 2, "No reply came back.")
        return tui.footer()
    return run


ENTRIES = [
    Entry(
        title="Set clipboard (OSC 52)",
        explanation=(
            "OSC 52 ; c ; <base64> ST puts the decoded text on the system "
            "clipboard and OSC 52 ; p on the primary selection; a paste then "
            "delivers exactly that text. A write policy that denies OSC 52 "
            "leaves the clipboard as it was."),
        run=_demo_set,
        sequence=OSC52_SET,
        expect="(consumed, nothing printed: a clipboard write answers nothing)",
    ),
    Entry(
        title="Read clipboard (OSC 52)",
        explanation=(
            "Where reads are allowed, OSC 52 ; c ; ? ST answers OSC 52 ; c ; "
            "<base64> ST with the clipboard's text. A denied read answers an "
            "empty payload; with no clipboard behind the terminal nothing comes "
            "back."),
        run=_demo_read,
        sequence=f"{OSC}52;c;?{ST}",
        expect="consumed, nothing printed",
    ),
    Entry(
        title="Long clipboard write (OSC 52)",
        explanation=(
            "One OSC 52 with 240 base64 characters is taken whole: a paste "
            "returns all 178 bytes, with no cut at a buffer boundary."),
        run=_demo_long,
    ),
    Entry(
        title="Bracketed paste (?2004)",
        explanation=(
            "After CSI ? 2004 h a paste reaches the program as ESC [ 200~ <text> "
            "ESC [ 201~ while typed keys arrive unframed. The mode is put back "
            "as it was on return."),
        run=_demo_bracketed,
        sequence=f"{CSI}?2004h{CSI}?2004$p",
        expect=r"\x1b\[\?2004;1\$y",
        snapshot=False,
    ),
    Entry(
        title="Paste control filter (?2004 off)",
        explanation=(
            "With bracketed paste off, a paste drops C0 controls other than TAB "
            "and ESC, and DEL, so Ctrl-C, Ctrl-D, Ctrl-Z and BEL never reach the "
            "program as keys. TAB stays and a newline becomes one CR."),
        run=_demo_sanitize,
        sequence=f"{CSI}?2004l{CSI}?2004$p",
        expect=r"\x1b\[\?2004;2\$y",
        snapshot=False,
        golden="clipboard_paste_sanitize",
    ),
    Entry(
        title="Extended clipboard detection (?5522)",
        explanation=(
            "DECRQM for mode 5522 answers CSI ? 5522 ; 2 $ y: known and reset, "
            "which a program reads as the OSC 5522 clipboard being available."),
        run=_reply_page("The mode report says the extended clipboard is available.",
                        "CSI ? 5522 $ p", f"{CSI}?5522$p"),
        sequence=f"{CSI}?5522$p",
        expect=r"\x1b\[\?5522;2\$y",
        snapshot=False,
    ),
    Entry(
        title="Servable types (OSC 5522)",
        explanation=(
            "A read of base64('.') lists the types the terminal can serve, with "
            "no consent prompt: status=OK, one status=DATA whose payload decodes "
            "to 'text/plain', then status=DONE."),
        run=_reply_page("The terminal lists the clipboard types it can serve.",
                        "OSC 5522 ; type=read ; Lg== ST", f"{OSC}5522;type=read;{MIME_LIST_B64}{ST}"),
        sequence=f"{OSC}5522;type=read;{MIME_LIST_B64}{ST}",
        expect=r"status=DATA:mime=Lg==;dGV4dC9wbGFpbg==",
        snapshot=False,
    ),
    Entry(
        title="Text read (OSC 5522)",
        explanation=(
            "A read of text/plain asks for clipboard-read consent, then answers "
            "status=OK, one status=DATA per chunk and status=DONE. A refusal "
            "answers EPERM, and a terminal with no clipboard ENOSYS."),
        run=_reply_page("After the consent prompt, the clipboard text comes back in chunks.",
                        "OSC 5522 ; type=read ; dGV4dC9wbGFpbg== ST",
                        f"{OSC}5522;type=read;{MIME_TEXT_B64}{ST}", timeout=5.0),
        sequence=f"{OSC}5522;type=read;{MIME_TEXT_B64}{ST}",
        expect=r"\x1b\]5522;type=read:status=ENOSYS\x1b\\",
        snapshot=False,
    ),
    Entry(
        title="Write session (OSC 5522)",
        explanation=(
            "type=write opens a session, a wdata with a mime carries a chunk, and "
            "a wdata with none commits it. The commit answers status=DONE with "
            "the text on the clipboard, or EIO where there is no clipboard."),
        run=_reply_page("The commit is acknowledged once the text is on the clipboard.",
                        "OSC 5522 ; type=write, one type=wdata chunk, an empty wdata",
                        _WRITE_SEQ),
        sequence=_WRITE_SEQ,
        expect=r"\x1b\]5522;type=write:status=EIO\x1b\\",
        snapshot=False,
    ),
]
