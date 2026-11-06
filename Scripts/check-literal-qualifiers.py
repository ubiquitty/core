#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-literal-qualifiers.py — a namespace qualifier that leaked into a string.

A textual rename that adds a namespace rewrites identifiers, and a protocol
token spelled as a string literal looks exactly like an identifier:

    static constexpr char cursorShape[] = "CursorShape=";

becomes `"Cursor::CursorShape="`. It compiles. Nothing warns. The terminal
stops recognising OSC 1337 CursorShape=, and the failure surfaces — if it
surfaces at all — as a parser bug rather than as a changed string, because the
parser is the thing that stopped matching.

The compiler cannot help here: both spellings are valid strings. A newly
qualified protocol token, key, or filename is therefore worth checking.

The rule: a string literal whose content begins with a qualified name and holds
no space is reported. Prose mentioning a C++ name has spaces; a token does not.

Canonical GObject notify details, such as `notify::gtk-fontconfig-timestamp`,
use `::` as signal syntax. Recognize that grammar instead of listing individual
properties. Other findings are ratcheted against the file's previous literals:
a rename introduces a new occurrence, while editing an existing line does not.

    python3 Scripts/check-literal-qualifiers.py --self-test
"""

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE_SUFFIXES = (".h", ".hpp", ".cpp", ".mm", ".m", ".c")

# The content of a string literal, starting with `name::name`. Escapes are not
# handled: a literal containing an escaped quote splits into two matches, and
# both halves are checked, so nothing is missed by the simplification.
QUALIFIED_LITERAL = re.compile(r'"([A-Za-z_]\w*(?:::\w+)+[^"]*)"')

# notify details must use canonical property names: ASCII alphanumeric segments
# separated by hyphens, starting with a letter. Match the complete token so
# underscores, added namespaces and trailing protocol punctuation stay visible.
# https://docs.gtk.org/gobject/class.ParamSpec.html#parameter-names
GOBJECT_NOTIFY = re.compile(r"notify::[A-Za-z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)*")


def offenders_in(text):
    """Yields (line number, literal) for each qualified, space-free literal."""
    for number, line in enumerate(text.split("\n"), 1):
        for match in QUALIFIED_LITERAL.finditer(line):
            body = match.group(1)
            if " " not in body and not GOBJECT_NOTIFY.fullmatch(body):
                yield number, body


def sources(root):
    for directory, subdirectories, files in os.walk(os.path.join(root, "Source")):
        subdirectories[:] = [d for d in subdirectories
                             if d not in ("ThirdParty", "compat", "build", ".git")]
        for name in files:
            if name.endswith(SOURCE_SUFFIXES):
                yield os.path.join(directory, name)


def literals_at_head(root, relative):
    """The qualified literals this file already had before the change.

    The hazard is a qualifier being INTRODUCED, not a line that legitimately
    holds one being edited. Blocking on "the literal appears on an added line"
    fires when someone touches the line for an unrelated reason, which teaches
    people the check is noise. Comparing against the pre-image asks the question
    that was meant.
    """
    show = subprocess.run(["git", "show", "HEAD:%s" % relative],
                          cwd=root, capture_output=True, text=True)
    if show.returncode != 0:
        return set()  # a new file: every literal in it is introduced here
    return {body for _, body in offenders_in(show.stdout)}


def staged_paths(root):
    result = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
                            cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return {os.path.normpath(os.path.join(root, name))
            for name in result.stdout.split("\n") if name}


def main(argv):
    if "--self-test" in argv[1:]:
        return selfTest()
    staged_only = "--staged" in argv[1:]
    positional = [a for a in argv[1:] if not a.startswith("--")]
    root = positional[0] if positional else "."

    staged = staged_paths(root) if staged_only else None

    blocking, pre_existing = [], []
    scanned = 0
    for path in sorted(sources(root)):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        scanned += 1
        relative = os.path.relpath(path, root)
        already = None
        for number, body in offenders_in(text):
            entry = (relative, number, body)
            if staged is None or os.path.normpath(path) not in staged:
                pre_existing.append(entry)
                continue
            if already is None:
                already = literals_at_head(root, relative)
            (pre_existing if body in already else blocking).append(entry)

    for relative, number, body in blocking:
        print('%s:%d: a namespace qualifier inside a string literal: "%s"'
              % (relative, number, body), file=sys.stderr)
    for relative, number, body in pre_existing:
        print('%s:%d: qualifier in a literal: "%s" (pre-existing)'
              % (relative, number, body), file=sys.stderr)

    if blocking:
        print("", file=sys.stderr)
        print("check-literal-qualifiers: %d literal(s) carry a C++ qualifier."
              % len(blocking), file=sys.stderr)
        print("  Check whether a textual rename changed a protocol token, key or filename.", file=sys.stderr)
        print("  Canonical GObject notify::<property-name> signals are recognized; malformed", file=sys.stderr)
        print("  details and other qualified tokens still require inspection.", file=sys.stderr)
        return 1
    if pre_existing:
        print("check-literal-qualifiers: ok for this change (%d file(s) scanned, "
              "%d pre-existing above)" % (scanned, len(pre_existing)))
        return 0
    print("check-literal-qualifiers: ok (%d source file(s) scanned)" % scanned)
    return 0


def selfTest():
    """Checks signal grammar and the staged pre-image ratchet without GTK."""
    cases = (
        ('"notify::position"', []),
        ('"notify::gtk-fontconfig-timestamp"', []),
        ('"notify::x-1-ASCII9"', []),
        ('"notify::Property"', []),
        ('"notify::"\n"is-active"', []),
        ('"notify::9position"', [(1, "notify::9position")]),
        ('"notify::position-"', [(1, "notify::position-")]),
        ('"notify::gtk--fontconfig"', [(1, "notify::gtk--fontconfig")]),
        ('"notify::gtk_fontconfig_timestamp"', [(1, "notify::gtk_fontconfig_timestamp")]),
        ('"notify::gtk-fontconfig_timestamp"', [(1, "notify::gtk-fontconfig_timestamp")]),
        ('"notify::pösition"', [(1, "notify::pösition")]),
        ('"notify::position="', [(1, "notify::position=")]),
        ('"notify::position::Value"', [(1, "notify::position::Value")]),
        ('"Notify::position"', [(1, "Notify::position")]),
        ('"GTK::notify::position"', [(1, "GTK::notify::position")]),
        ('"Cursor::CursorShape="', [(1, "Cursor::CursorShape=")]),
        ('"std::string"', [(1, "std::string")]),
        ('"Cursor::CursorShape is named in prose"', []),
        ('"CursorShape="', []),
        ('"notify::position"; "Core::Token";\n"notify::gtk_fontconfig";',
         [(1, "Core::Token"), (2, "notify::gtk_fontconfig")]),
    )
    failures = []
    for source, expected in cases:
        actual = list(offenders_in(source))
        if actual != expected:
            failures.append("%r: got %r, expected %r" % (source, actual, expected))

    # Exercise the real --staged command in an independent repository. Inherited
    # Git variables must not point this fixture at a hook caller's live index.
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("GIT_")}
    with tempfile.TemporaryDirectory(prefix="literal-qualifiers-") as root:
        def git(*arguments):
            return subprocess.run(["git", "-C", root, *arguments], env=environment,
                                  capture_output=True, text=True, check=True).stdout.strip()

        git("init", "-q")
        source = Path(root, "Source", "Fixture.cpp")
        source.parent.mkdir()
        previous = 'const char* existing = "Legacy::Token";\n'
        source.write_text(previous, encoding="utf-8")
        git("add", "Source/Fixture.cpp")
        tree = git("write-tree")
        commit = git("-c", "user.name=Gate Test", "-c", "user.email=gate@example.invalid",
                     "-c", "commit.gpgsign=false", "commit-tree", tree, "-m", "fixture")
        git("update-ref", "HEAD", commit)

        def check(contents):
            source.write_text(contents, encoding="utf-8")
            git("add", "Source/Fixture.cpp")
            return subprocess.run([sys.executable, os.path.abspath(__file__), "--staged", root],
                                  env=environment, capture_output=True, text=True)

        valid = previous + 'const char* signal = "notify::gtk-fontconfig-timestamp";\n'
        accepted = check(valid)
        if accepted.returncode or "notify::gtk-fontconfig-timestamp" in accepted.stderr:
            failures.append("new canonical signal failed the staged check: " + accepted.stderr)
        rejected = check(valid + 'const char* renamed = "Cursor::CursorShape=";\n'
                         'const char* malformed = "notify::gtk_fontconfig_timestamp";\n')
        for token in ("Cursor::CursorShape=", "notify::gtk_fontconfig_timestamp"):
            if rejected.returncode != 1 or token not in rejected.stderr:
                failures.append("new malformed token escaped the staged check: " + token)
        if "notify::gtk-fontconfig-timestamp" in rejected.stderr:
            failures.append("a valid signal was reported beside an unrelated qualifier")
        if "Legacy::Token\" (pre-existing)" not in rejected.stderr:
            failures.append("the pre-image ratchet stopped recognizing existing qualifiers")

    for failure in failures:
        print("check-literal-qualifiers --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-literal-qualifiers --self-test: ok (%d grammar cases and staged ratchet)" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
