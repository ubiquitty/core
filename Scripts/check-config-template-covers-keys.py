#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-config-template-covers-keys.py — the shipped reference lists the keys.

`Source/Core/Preferences/PreferencesFileTemplate.cpp` writes the default TOML the app
ships, and says twice what that file is for:

    "its built-in default, so the file doubles as the reference for what can be set"
    "Every supported key appears once, commented, at its built-in default."

`Key.h` is not the list of settings, and reading it as one is easy: it
calls itself the "declarative WRITE recipe for every settings key a frontend
persists", so it holds what a UI control writes back. The read side is a separate
table in `Preferences.cpp` with 154 entries, and 32 of those are parseable with no row
here — `security.osc_allow`, `render.max_fps`, `keys.compose_key` and the rest of
what has no toggle. Nothing is lost by that: a frontend writes one key at a time
through `Preferences::writeKeyValue`, which patches the file rather than rewriting it,
so a hand-written key with no UI survives a UI write.

This gate is the write side only. That every DOCUMENTED key is a real one is the
other direction, and `Tests/Core/Preferences/test_preferences_template_parses.cpp` holds it
by parsing each in Strict mode.

Every key `Key.h` persists appears in the template under its own section.
Adding a row to the key table gives nobody a reason to touch the template, and
nothing else compares the two, so each omission is invisible alone and only a
count over the whole table shows the gap.

A key is looked for under its own section, because names are not unique across
sections: a match anywhere credits `quake.enabled` with the line documenting
`notifications.enabled`, `control.enable` with the one under `# [scripting]`,
and `control.token` with prose about a "BCP-47-style token".

Prose naming a key under its own heading counts, as a commented assignment
does: a reader cannot set a key whose name is nowhere in the reference. The
`title.pane_*` keys are named that way, with their inheritance rule and no
value, because an unset pane flag does not fall back to a value:
`paneTitleComposition` inherits the RESOLVED tab flag, except dimensions where
unset means off, so `# pane_directory = true` would name a default the pane
does not have.

A key's description is written wherever someone had to explain the feature,
which is rarely the config headers: the macOS settings catalog carries a title and
a detail paragraph for every setting with a toggle, `Docs/control-socket.md`
ships the `[control]` block, and the defaults of the `[title]` flags are in
`Preferences.cpp`'s constructor initialiser list rather than in
`PreferencesDefaults.h`. A transcription into the template starts from those.

KNOWN_ABSENT records any key the template does not name, so the gap cannot
grow: a key missing beyond it fails, and a recorded key that reaches the
template fails too, naming itself for deletion from the list. The record cannot
outlive its subject, which is what separates it from an allowlist that survives
the condition it excused.

Stdlib only.

    python3 Scripts/check-config-template-covers-keys.py
    python3 Scripts/check-config-template-covers-keys.py --self-test
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))

KEYS = os.path.join("Source", "Core", "Preferences", "Key.h")
TEMPLATE = os.path.join("Source", "Core", "Preferences", "PreferencesFileTemplate.cpp")

# `{ Key::AppearanceFollowSystem, "appearance", "follow_system", ... }`
ROW = re.compile(r'\{\s*Key::(\w+)\s*,\s*"([a-z_]+)"\s*,\s*"([a-z_0-9]+)"')

# A section header the template emits, either live —
#
#     "[selection]\n"
#
# or shown as a commented example, which teaches a reader the section exists
# just as well and is how [scripting], [triggers], [macros] and [[profiles]]
# appear:
#
#     "# [scripting]\n"
#
# Both count. What does not count is a bracketed word inside prose, which is why
# this wants the whole string literal rather than the brackets alone: the file's
# own comment about the [title] gap is not a [title] section, and a scan keyed
# on brackets reports it as one.
SECTION = re.compile(r'"(?:#\s*)?\[\[?([a-z_]+)\]\]?\\n"')

# Absent from the template, recorded so the gap cannot grow.
# Delete an entry when its key reaches the template — this fails until you do.
KNOWN_ABSENT = set()


def read(path):
    try:
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def sectionSpans(templateText):
    """Where each section's text runs, as {section: [(start, end), ...]}.

    A section ends where the next header begins, so a key is looked for under
    its own heading rather than anywhere in the file.
    """
    found = [(m.group(1), m.start()) for m in SECTION.finditer(templateText)]
    spans = {}
    for i, (name, start) in enumerate(found):
        end = found[i + 1][1] if i + 1 < len(found) else len(templateText)
        spans.setdefault(name, []).append((start, end))
    return spans


def absent(keysText, templateText):
    """Keys `Key.h` persists that the template never names under their section.

    - Returns: (set of "section.key", number of rows read).

    Scoped to the section, because a key name is not unique across the file and
    a match anywhere credits the wrong one. Three keys read as documented under
    a bare substring test and are not: `quake.enabled` collects the line that
    documents `notifications.enabled`, `control.enable` the one under
    `# [scripting]`, and `control.token` two sentences of prose — "BCP-47-style
    token" and "selects a URL/path token". None of the three has a section in
    the template at all, so each is credited to text about something else.

    Whole-word rather than substring for the same reason, and the match stays
    loose within the section: the template writes a key as `# theme = "dark"`
    in one place and names it in prose in another, and under its own heading
    both do tell a reader the key exists.
    """
    rows = ROW.findall(keysText)
    spans = sectionSpans(templateText)
    gone = set()
    for _id, section, key in rows:
        word = re.compile(r"(?<![a-z0-9_])" + re.escape(key) + r"(?![a-z0-9_])")
        if not any(word.search(templateText[a:b]) for a, b in spans.get(section, ())):
            gone.add("%s.%s" % (section, key))
    return gone, len(rows)


def selfTest():
    ok = True

    keys = ('{ Key::AppearanceTheme, "appearance", "theme", SerializeKind::String },\n'
            '{ Key::TitleWindowProcess, "title", "window_process", SerializeKind::Bool },')
    gone, total = absent(keys, '"[appearance]\\n"\n"# theme = \\"dark\\"\\n"\n')
    if total != 2:
        print("self-test: rows misread, got %d" % total, file=sys.stderr)
        ok = False
    if gone != {"title.window_process"}:
        print("self-test: expected only title.window_process absent, got %s" % gone,
              file=sys.stderr)
        ok = False

    gone, _ = absent(keys, '"[appearance]\\n"\n"theme\\n"\n"[title]\\n"\n"window_process\\n"\n')
    if gone:
        print("self-test: a key named under its own section was reported absent",
              file=sys.stderr)
        ok = False

    # The collision this scoping exists for: the name is in the file, under a
    # different section, and a whole-file match credits it to the wrong key.
    twoSections = ('{ Key::NotificationsEnabled, "notifications", "enabled", '
                   'SerializeKind::Bool },\n'
                   '{ Key::QuakeEnabled, "quake", "enabled", SerializeKind::Bool },')
    gone, _ = absent(twoSections, '"[notifications]\\n"\n"# enabled = true\\n"\n')
    if gone != {"quake.enabled"}:
        print("self-test: quake.enabled was credited to the [notifications] line, got %s"
              % gone, file=sys.stderr)
        ok = False

    # Prose in brackets is not a section. The template's own comment about the
    # [title] gap must not open one, or every title key reads as documented.
    if sectionSpans("// [title] section among them — frozen so the gap\n"):
        print("self-test: a bracketed word in prose was read as a section header",
              file=sys.stderr)
        ok = False

    # A commented example teaches the section exists; it must open a span.
    if "scripting" not in sectionSpans('"# [scripting]\\n"\n"# enable = false\\n"\n'):
        print("self-test: a commented section header opened no span", file=sys.stderr)
        ok = False

    print("check-config-template-covers-keys: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    keysText, templateText = read(KEYS), read(TEMPLATE)
    if not keysText or not templateText:
        print("check-config-template-covers-keys: FAILED — could not read %s or %s, so "
              "nothing was compared" % (KEYS, TEMPLATE), file=sys.stderr)
        return 1

    gone, total = absent(keysText, templateText)
    if not total:
        print("check-config-template-covers-keys: FAILED — no rows parsed from %s, so "
              "every key was compared against nothing. The row shape changed." % KEYS,
              file=sys.stderr)
        return 1

    offenders = ["%s is persistable and the template never names it" % k
                 for k in sorted(gone - KNOWN_ABSENT)]
    offenders += ["%s reached the template — delete it from KNOWN_ABSENT" % k
                  for k in sorted(KNOWN_ABSENT - gone)]

    if offenders:
        status = gate_report.failed("check-config-template-covers-keys",
                                    "key(s) out of step with the shipped reference", offenders)
        print("\n  The template calls itself the reference for what can be set, so a key\n"
              "  it omits is a setting a reader cannot discover. A KNOWN_ABSENT entry\n"
              "  that is now present is a record outliving its subject.", file=sys.stderr)
        return status

    print("check-config-template-covers-keys: ok (%d persistable key(s): %d in the "
          "template, %d recorded absent and unmoved)"
          % (total, total - len(gone), len(gone)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
