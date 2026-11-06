# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Hyperlinks: OSC 8 explicit links and URLs the hover matcher detects.

OSC 8 ; <parameters> ; <URI> ST opens a link and OSC 8 ; ; ST closes it; the
only defined parameter, id=, joins separate runs into one link. An OSC 8 link
opens on a plain click; a detected URL is the terminal's guess and needs Cmd.
Either opens only for a launchable scheme (http, https, file, mailto, ftp,
ftps, ssh, sftp).
"""

from ..tui import CSI, OSC, RESET, ST, fg_truecolor, move
from . import Entry

TITLE = "Hyperlinks"
ORDER = 80


def _osc8_open(uri, parameters=""):
    return f"{OSC}8;{parameters};{uri}{ST}"


_OSC8_CLOSE = f"{OSC}8;;{ST}"


def _link(uri, text, parameters=""):
    return _osc8_open(uri, parameters) + text + _OSC8_CLOSE


_BASIC_SEQ = _link("https://example.com", "link text")


def _demo_basic(tui):
    row = tui.page(TITLE, "Hovering 'link text' underlines exactly its nine cells; a click opens it.")
    tui.label(row, 4, "OSC 8 ; ; https://example.com ST  link text  OSC 8 ; ; ST")
    tui.write(move(row + 2, 4), "before ", _BASIC_SEQ, " after")
    return tui.footer()


_ID_SEQ = (_link("https://example.com/manual", "first run", "id=foo")
           + _link("https://example.com/manual", "second run", "id=foo"))


def _demo_id(tui):
    row = tui.page(TITLE, "Hovering either run underlines both; the plain text between stays plain.")
    tui.label(row, 4, "OSC 8 ; id=foo ; https://example.com/manual ST, twice")
    tui.write(move(row + 2, 4),
              _link("https://example.com/manual", "first run", "id=foo"),
              "   plain text   ",
              _link("https://example.com/manual", "second run", "id=foo"))
    return tui.footer()


_SCHEMES_SEQ = (_link("https://example.com", "https link") + "\r\n"
                + _link("file:///etc/hosts", "file link") + "\r\n"
                + _link("mailto:user@example.com", "mailto link"))

_SCHEME_ROWS = (
    ("https://example.com", "https link"),
    ("file:///etc/hosts", "file link"),
    ("mailto:user@example.com", "mailto link"),
    ("javascript:alert(1)", "refused link"),
)


def _demo_schemes(tui):
    row = tui.page(TITLE, "Each link opens in its own handler; the javascript: link never opens.")
    for offset, (uri, text) in enumerate(_SCHEME_ROWS):
        tui.label(row + offset, 4, f"OSC 8 ; ; {uri} ST")
        tui.write(move(row + offset, 44), _link(uri, text))
    return tui.footer()


_AUTODETECT_SEQ = (
    "https://www.example.com/page\r\nwww.example.org\r\nfile:///var/log/system.log\r\n")


def _demo_autodetect(tui):
    row = tui.page(TITLE, "With Cmd held, hovering a URL underlines it and Cmd-click opens it.")
    tui.label(row, 4, "plain text, no OSC 8")
    for offset, line in enumerate(_AUTODETECT_SEQ.split("\r\n")[:3]):
        tui.write(move(row + 2 + offset, 4), line)
    return tui.footer()


_STYLED_SEQ = (_osc8_open("https://example.com/styled") + f"{CSI}4:5m"
               + f"{CSI}38:2::152:195:121m" + "styled link" + RESET + _OSC8_CLOSE)


def _demo_styled(tui):
    row = tui.page(TITLE, "The link draws green with a dashed underline and still opens on a click.")
    tui.label(row, 4, "OSC 8 ; ; https://example.com/styled ST")
    tui.label(row + 1, 4, "CSI 4:5 m   dashed underline")
    tui.label(row + 2, 4, "CSI 38:2::152:195:121 m   green")
    tui.write(move(row + 4, 4), f"{CSI}4:5m", fg_truecolor(152, 195, 121),
              _link("https://example.com/styled", "styled link"), RESET)
    return tui.footer()


ENTRIES = [
    Entry(
        title="Hyperlink (OSC 8)",
        explanation=(
            "The nine cells of 'link text' between the open and the empty-URI "
            "close carry the link: they underline on hover, the pointer becomes "
            "a hand, and a click opens https://example.com. The cells around "
            "them carry nothing."),
        run=_demo_basic,
        sequence=_BASIC_SEQ,
        golden="hyperlinks_basic",
    ),
    Entry(
        title="Shared link across runs (OSC 8 id=)",
        explanation=(
            "Two runs opened with the same id=foo are one link: hovering either "
            "underlines both, the text between stays unmarked, and either opens "
            "https://example.com/manual."),
        run=_demo_id,
        sequence=_ID_SEQ,
        golden="hyperlinks_id_shared",
    ),
    Entry(
        title="Link schemes (OSC 8)",
        explanation=(
            "https, file and mailto links each open in their own handler. A "
            "scheme outside http, https, file, mailto, ftp, ftps, ssh and sftp, "
            "such as javascript:, renders and underlines, and its click is "
            "refused."),
        run=_demo_schemes,
        sequence=_SCHEMES_SEQ,
        golden="hyperlinks_schemes",
    ),
    Entry(
        title="Detected URLs (plain text)",
        explanation=(
            "URLs printed as plain text carry no link, and the hover matcher "
            "finds them anyway: with Cmd held they underline and Cmd-click "
            "opens them. A bare www. run opens as https://."),
        run=_demo_autodetect,
        sequence=_AUTODETECT_SEQ,
        golden="hyperlinks_autodetect",
    ),
    Entry(
        title="Styled link (OSC 8 with SGR)",
        explanation=(
            "SGR decides how a link looks and OSC 8 which cells it covers: the "
            "run draws green under a dashed underline instead of the default "
            "affordance, and a click still opens https://example.com/styled."),
        run=_demo_styled,
        sequence=_STYLED_SEQ,
        golden="hyperlinks_styled",
    ),
]
