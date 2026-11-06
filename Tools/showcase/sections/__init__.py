# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Section auto-discovery and the section-module contract.

A *section* is one Python module in this package. Adding a section is exactly
"drop one file here" — `discover()` walks the package with pkgutil, imports
every non-underscore module, validates the contract, and returns them sorted by
ORDER. The showcase menu is built from that list.

The contract
------------
Each section module must define:

    TITLE: str          Human-readable section name shown in the menu.
    ORDER: int          Sort key (lower = earlier). Pick spaced values
                        (10, 20, 30, ...) so sections can be inserted between.
    ENTRIES: list[Entry]  The demos within the section.

Each `Entry` (see the dataclass below) has:

    title: str          Short name shown in the section's entry list.
    explanation: str    What the user should look for / the expected behavior.
                        Keep it precise — these are written so they can later
                        be turned into automated assertions. Do not claim a
                        feature "works"; describe what *correct* looks like.
    run: callable        run(tui) -> None. Emits the escape sequences and
                        renders the demo. May draw interactive sub-screens; it
                        should block until a key is pressed and then return so
                        control flows back to the menu. Do not enter/exit the
                        alt-screen or raw mode — the framework owns that.

Each `Entry` also carries machine-readable probe overrides. The live demo is
the source of truth shared with the automated snapshot harness (surfaced as
JSON via `showcase.py --probes`):

    sequence: str|bytes|None  An exact minimal byte stream for the snapshot.
                        When omitted, showcase.py captures the entry's live
                        rendering through a non-interactive TUI.
    expect: str|None    An optional regex for the terminal reply. An entry
                        is snapshotted whether or not it also checks a reply,
                        unless `snapshot` is False.
    golden: str|None    An optional stable golden tag. When omitted, one is
                        derived from the section module and entry title.
    snapshot: bool      False for an entry whose feature draws nothing on the
                        grid — a mode, a report, a clipboard or keyboard
                        protocol. Its frame would be the blank one every such
                        entry shares, which proves nothing about the feature,
                        so the entry takes no picture and asserts its reply
                        instead; `expect` is then required. An entry whose
                        contract is that nothing reaches the grid (a string
                        the parser consumes) keeps its picture, because there
                        the blank frame is the assertion; it is listed in
                        BLANK_BY_CONTRACT in Scripts/check-showcase-goldens.py.

`validate_module()` type-checks overrides and rejects non-executable assertions.

Modules whose name starts with "_" (e.g. _TEMPLATE) are skipped by discovery.

Copy `_TEMPLATE.py` to start a new section.
"""

import importlib
import inspect
import pkgutil
import re
from dataclasses import dataclass
from typing import Callable, List, Optional, Union


@dataclass
class Entry:
    title: str
    explanation: str
    run: Callable  # run(tui) -> None
    # Explicit minimal sequences and golden names override the deterministic
    # contract derived from run(), the section module, and the entry title.
    sequence: Optional[Union[str, bytes]] = None
    expect: Optional[str] = None
    golden: Optional[str] = None
    snapshot: bool = True
    # True when the golden pins only the page the entry draws, because what the
    # title names cannot be captured in one frame: a screen reader's speech, a
    # sound, a desktop notification, or a rate measured over time. The matrix
    # then calls its contract a page render rather than a snapshot of the
    # feature.
    descriptive: bool = False


@dataclass
class Section:
    title: str
    order: int
    entries: List[Entry]
    module_name: str


REQUIRED_ATTRS = ("TITLE", "ORDER", "ENTRIES")
GOLDEN_NAME = re.compile(r"^[a-z0-9][a-z0-9_]*$")


def validate_module(mod):
    """Validate a candidate section module against the contract.

    Returns a `Section` on success; raises ValueError with a precise message
    on the first violation (used by both discovery and `--self-test`).
    """
    name = mod.__name__.rsplit(".", 1)[-1]
    for attr in REQUIRED_ATTRS:
        if not hasattr(mod, attr):
            raise ValueError(f"section '{name}' is missing required '{attr}'")
    if not isinstance(mod.TITLE, str) or not mod.TITLE:
        raise ValueError(f"section '{name}': TITLE must be a non-empty str")
    if not isinstance(mod.ORDER, int):
        raise ValueError(f"section '{name}': ORDER must be an int")
    if not isinstance(mod.ENTRIES, (list, tuple)) or not mod.ENTRIES:
        raise ValueError(f"section '{name}': ENTRIES must be a non-empty list")
    entry_titles = set()
    for i, e in enumerate(mod.ENTRIES):
        if not isinstance(e, Entry):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}] is not a tui Entry")
        if not e.title or not isinstance(e.title, str):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].title must be a non-empty str")
        if e.title in entry_titles:
            raise ValueError(
                f"section '{name}': duplicate entry title {e.title!r}")
        entry_titles.add(e.title)
        if not e.explanation or not isinstance(e.explanation, str):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].explanation must be set")
        if not callable(e.run):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].run must be callable")
        try:
            inspect.signature(e.run).bind(object())
        except (TypeError, ValueError) as error:
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].run must accept one TUI") from error
        if e.sequence is not None and not isinstance(e.sequence, (str, bytes)):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].sequence must be str/bytes")
        if e.expect is not None and not isinstance(e.expect, str):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].expect must be a str")
        if e.expect is not None and e.expect.lstrip().startswith("manual:"):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].expect must be machine-checkable")
        if e.expect is not None and e.sequence is None:
            raise ValueError(
                f"section '{name}': ENTRIES[{i}] reply check needs an exact sequence")
        if not isinstance(e.snapshot, bool):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].snapshot must be a bool")
        if not e.snapshot and e.expect is None:
            raise ValueError(
                f"section '{name}': ENTRIES[{i}] takes no picture and asserts no "
                "reply, so it tests nothing")
        if e.expect is not None and "consumed, nothing printed" not in e.expect:
            try:
                re.compile(e.expect)
            except re.error as error:
                raise ValueError(
                    f"section '{name}': ENTRIES[{i}].expect is not a regex") from error
        if not isinstance(e.descriptive, bool):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].descriptive must be a bool")
        if e.descriptive and e.expect is not None:
            raise ValueError(
                f"section '{name}': ENTRIES[{i}] is descriptive, so it asserts no reply")
        if e.golden is not None and not isinstance(e.golden, str):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].golden must be a str")
        if e.golden is not None and not GOLDEN_NAME.fullmatch(e.golden):
            raise ValueError(
                f"section '{name}': ENTRIES[{i}].golden must be lowercase snake_case")
    return Section(mod.TITLE, mod.ORDER, list(mod.ENTRIES), name)


def discover():
    """Import and validate every section module; return them sorted by ORDER.

    Underscore-prefixed modules (templates, helpers) are skipped.
    """
    sections = []
    for info in pkgutil.iter_modules(__path__):
        if info.name.startswith("_"):
            continue
        mod = importlib.import_module(f"{__name__}.{info.name}")
        sections.append(validate_module(mod))
    titles = [section.title for section in sections]
    if len(titles) != len(set(titles)):
        raise ValueError("showcase section titles must be unique")
    orders = [section.order for section in sections]
    if len(orders) != len(set(orders)):
        raise ValueError("showcase section ORDER values must be unique")
    sections.sort(key=lambda s: (s.order, s.title))
    return sections
