<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ubiquitty Feature Showcase

An interactive TUI and the source catalog for Ubiquitty's app-driven regression
suite. Every entry is rendered by `Tests/Snapshots/run.py`; reply-producing
entries also assert their outbound protocol bytes.

It uses the Python 3.8+ standard library without `pip` dependencies.
Interactive mode requires a POSIX TTY. It emits raw ANSI/CSI/OSC/DCS sequences
directly, without curses.

## Run

```sh
python3 Tools/showcase/showcase.py             # interactive (needs a TTY)
python3 Tools/showcase/showcase.py --list      # print the section/entry tree, no TTY
python3 Tools/showcase/showcase.py --self-test # import every section, validate contract
python3 Tools/showcase/showcase.py --probes    # dump per-entry probe contract as JSON
python3 Tools/showcase/showcase.py --help
```

`--list` and `--self-test` annotate each entry with a `[seg]` badge:
`s`napshot sequence, optional reply `e`xpectation, and `g`olden. Every entry has
`s` and `g`. `--probes` emits the complete executable contract as JSON,
including the sequence encoding; `Tests/Snapshots/run.py` consumes it directly.
The committed coverage index is generated from that contract at
`Docs/feature_test_matrix.md`; it records interactive availability separately
from the mandatory automated snapshot/reply contract.

In the menu: arrows or `j`/`k` to move, `Enter`/`→` to open a section then run
an entry, `Esc`/`←` to go back, `q` to quit. SGR mouse clicks also select.

## Layout

```
Tools/showcase/
  showcase.py          entry point: menu, --list, --self-test, --probes
  tui.py               toolkit: escapes, sizing, raw-mode session, the entry
                       page helpers, banner (DECDWL/DECDHL), key bar, input
  sections/
    __init__.py        the section-module contract and auto-discovery
    _TEMPLATE.py       copyable skeleton (underscore => skipped by discovery)
    window_title.py    exemplar: every mutator sent and undone, a skip line
                       where no window answers
  Examples/            stand-alone demos (see below)
```

## Examples

`Examples/` holds stand-alone demos that use the same raw sequences without
the menu: `new_year.py`, `vms-shell.py`, and `evolution.py`, the first-launch
show, which plays the history of the terminal one era per chapter, dumps one
byte file per chapter for `--snapshot`, and bakes a POSIX sh player that needs
no Python.

## Adding a section

Copy `sections/_TEMPLATE.py` to `sections/<name>.py`. Discovery is automatic
(`pkgutil`), sorted by `ORDER`; the order is the tour, from what a person sees
first (color, text, fonts, images) to protocol plumbing and legacy modes last.

```python
from .. import tui as T
from ..tui import CSI
from . import Entry

TITLE = "My section"
ORDER = 305

def _demo_underline(tui):
    row = tui.page(TITLE, "The word below is underlined; the rest of the row is not.")
    tui.write(T.move(row, 4), CSI + "4m", "underlined", T.RESET, " plain")
    tui.label(row + 2, 4, "CSI 4 m   underline on, CSI 0 m off")
    return tui.footer()

ENTRIES = [
    Entry(title="Underline (SGR 4)",
          explanation="SGR 4 underlines the run that follows and SGR 0 ends it.",
          run=_demo_underline),
]
```

Run `python3 Tools/showcase/showcase.py --self-test`, list the entry's snapshot
identifier with `python3 Tests/Snapshots/run.py --list`, create only that
baseline with
`python3 Tests/Snapshots/run.py --backend portable --update-missing <identifier>`,
and look at the PNG before accepting it.

### Entry rules

- A section module defines `TITLE: str`, `ORDER: int`, `ENTRIES: list[Entry]`.
  An `Entry` has `title`, `explanation` and `run(tui)`.
- An entry shows the thing happening. `tui.page(TITLE, caption)` draws the
  banner and one caption line saying what to look at; the body makes the
  terminal do it, with `tui.label` for sequence names and `tui.result` for
  what the terminal answered; `tui.footer(("Space", "verb"))` draws the key
  bar, which always ends in Esc, and waits.
- Every sequence a page names, it sends, and it restores what it changed
  before returning. A page never lists what it does not do.
- An entry that cannot demonstrate on the terminal it runs in prints one
  `tui.unavailable` line and nothing else.
- Titles are sentence case with the sequence in parentheses:
  `Palette override (OSC 4)`. The `explanation` is the contract shown in the
  menu's Description pane: what correct looks like, in at most three sentences.
- `run(tui)` never enters or leaves the alternate screen or raw mode; the
  menu owns both.

#### Regression contract

Every entry produces one snapshot. By default the contract captures `run(tui)`
with a deterministic 80x24 non-interactive TUI and derives the golden name from
the section module and entry title. Three fields can refine that default:

- `sequence: str | bytes | None` — a smaller exact byte stream when the whole
  demo frame would add noise. The JSON contract preserves whether it is UTF-8
  text or byte-oriented Latin-1.
- `expect: str | None` — an additional machine-checkable reply regex. Empty
  replies use the established `consumed, nothing printed` phrase.
- `golden: str | None` — a stable explicit tag when the derived name should not
  follow the entry title.

Duplicate golden tags, non-executable expectations, missing baselines, and
orphaned baselines fail validation. There is no manual-only test category.

## Toolkit primitives (`Tools/showcase/tui.py`)

- Escape builders: `sgr`, `fg_truecolor`/`bg_truecolor` (colon form),
  `fg_256`/`bg_256`, `move`, plus constants `RESET`/`BOLD`/`UNDERLINE`/...,
  `DECDWL`/`DECDHL_TOP`/`DECDHL_BOTTOM`, box-drawing sets `ROUNDED`/`DOUBLE`/
  `LIGHT`.
- Width helpers: `char_width`, `display_width`, `truncate` (east-asian +
  combining-mark aware) for alignment.
- `Tui.session(mouse=False)`: context manager — alt-screen + raw mode + cursor
  hide, with SIGINT/SIGTERM/SIGWINCH handling. It restores terminal state on
  normal exit and handled signals; an uncatchable kill cannot run cleanup.
- Sizing: `get_size`/`refresh_size` (TIOCGWINSZ ioctl, then `$LINES`/
  `$COLUMNS`, then 24x80).
- Entry pages: `page` (banner and caption, returns the first body row),
  `label`, `result`, `unavailable`, `keys` (the key bar, ending in Esc) and
  `footer` (key bar, then wait for a key).
- Rendering: `text`, `hline`, `center`, `panel` (bordered card with title),
  `fill_rows`, `banner` (centered DEC double-height/width title), `status_bar`,
  `wait_key`.
- Input: `read_key` decodes CSI/SS3 navigation keys and SGR mouse reports into
  friendly names / `("mouse", b, col, row, pressed)` tuples.
- Query / response: `Tui.query(seq, timeout=0.3)` emits a query escape and
  reads the terminal's raw reply from stdin (via `select` on the tty fd inside
  the raw-mode `session()`); returns raw bytes, or `b""` on timeout / off-TTY
  (it never breaks raw mode on timeout). `decode_reply(data)` renders a reply
  caret-escaped and token-spaced for display, e.g.
  `b"\x1b[?62;1;6c"` → `ESC [ ? 62 ; 1 ; 6 c`. This unblocks the response-based
  sections (DA1/2/3, DSR, DECRQM, DECRQSS, DECRQCRA, CSI-t reports, OSC
  color queries). The live read path needs a real terminal to exercise; off a
  TTY it degrades to empty bytes.
- Inline images: `test_image(w, h, kind)` builds a small RGBA buffer in code
  (a red/green/blue `"gradient"` or an 8x8 `"checkerboard"`, no external
  files). `image_sixel`, `image_kitty`, and `image_iterm2` each take that
  pixel data and return the escape string for one protocol so the graphics
  section can A/B the same picture: Sixel (`DCS q … ST`), Kitty graphics
  (`APC _G …;<base64 RGBA> ST`, chunked at 4096 bytes), and iTerm2 OSC 1337
  `File=` (pixels wrapped in a hand-built PNG via stdlib `zlib`, base64-encoded,
  `inline=1`). All are dependency-free.
