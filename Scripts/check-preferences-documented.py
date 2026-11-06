#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Every setting Core reads is documented in the file the user edits.

`Preferences.cpp` carries the key table -- the `[section] key` pairs the TOML
parser binds to fields -- and `PreferencesFileTemplate.cpp` carries the
commented config Ubiquitty writes on first run. The template is the only place
most people ever learn a setting exists: it is not reference documentation
beside the code, it is the file in front of them.

Nothing held the two together. `[selection] command_overrides_reporting` was
readable, defaulted on, and decided whether a Cmd-drag selects text over a
mouse-tracking application -- and it appeared nowhere in the template, so the
only way to find it was to read Core. Four others were in the same state.

A key absent from the template is not a documentation debt to be paid later; it
is a setting that does not exist as far as its user is concerned.

    ./Scripts/check-preferences-documented.py
    ./Scripts/check-preferences-documented.py --self-test

Stdlib only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KEY_TABLE = ROOT / "Source/Core/Preferences/Preferences.cpp"
TEMPLATE = ROOT / "Source/Core/Preferences/PreferencesFileTemplate.cpp"

# A row of the key table: { "section", "key", &Values::field }. The two string
# literals are what the TOML parser matches, so they are what a template must
# name; the field spelling is Core's business and drifts from the key on
# purpose (set_locale_environment -> setsLocaleEnvironment).
KEY_ROW = re.compile(r'\{\s*"([a-z0-9_]+)"\s*,\s*"([a-z0-9_]+)"\s*,')


def settingKeys(source: str) -> set[tuple[str, str]]:
    """The (section, key) pairs the parser binds.

    - Parameter source: `Preferences.cpp`'s text.
    - Returns: every bound key.
    """
    return {(m.group(1), m.group(2)) for m in KEY_ROW.finditer(source)}


def undocumented(keys: set[tuple[str, str]], template: str) -> list[tuple[str, str]]:
    """The keys the template never names.

    Matched on the key alone rather than on `key =`: a template line may show a
    setting inside a table header or an example rather than as an assignment,
    and naming it at all is what makes it findable.

    - Parameters:
      - keys: The bound keys.
      - template: `PreferencesFileTemplate.cpp`'s text.
    - Returns: the missing keys, section-ordered.
    """
    return sorted(pair for pair in keys if pair[1] not in template)


def selfTest() -> int:
    """Proves the check can fail: a template missing a key must be caught.

    A gate whose negative case is never exercised passes on an empty input as
    readily as on a correct one.

    - Returns: 0 when both the positive and negative case answer as expected.
    """
    keys = settingKeys('{ "selection", "copy_on_select", &Values::copyOnSelect },\n'
                       '{ "font", "size", &Values::fontSize },\n')
    if keys != {("selection", "copy_on_select"), ("font", "size")}:
        print(f"check-preferences-documented: self-test FAILED — parsed {keys}", file=sys.stderr)
        return 1
    complete = '"# copy_on_select = false\\n" "# size = 12\\n"'
    if undocumented(keys, complete):
        print("check-preferences-documented: self-test FAILED — a documented pair read as missing", file=sys.stderr)
        return 1
    if undocumented(keys, '"# size = 12\\n"') != [("selection", "copy_on_select")]:
        print("check-preferences-documented: self-test FAILED — a missing pair was not caught", file=sys.stderr)
        return 1
    print("check-preferences-documented: self-test ok (2 keys parsed; the missing case is caught)")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return selfTest()
    for path in (KEY_TABLE, TEMPLATE):
        if not path.is_file():
            print(f"check-preferences-documented: FAIL — {path} not found", file=sys.stderr)
            return 1
    keys = settingKeys(KEY_TABLE.read_text(encoding="utf-8"))
    if not keys:
        # An empty parse would pass every key vacuously.
        print("check-preferences-documented: FAIL — no keys parsed from the table", file=sys.stderr)
        return 1
    missing = undocumented(keys, TEMPLATE.read_text(encoding="utf-8"))
    if missing:
        print(f"check-preferences-documented: FAIL — {len(missing)} setting(s) Core reads that the "
              "template never names:", file=sys.stderr)
        for section, key in missing:
            print(f"  [{section}] {key}", file=sys.stderr)
        print("  Document each in Source/Core/Preferences/PreferencesFileTemplate.cpp — the template "
              "is where a user finds a setting exists.", file=sys.stderr)
        return 1
    print(f"check-preferences-documented: ok ({len(keys)} setting(s), every one named in the template)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
