#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Check that every gate in Scripts is invoked by something.

A gate that no aggregate, hook or workflow calls runs when a person types it and
at no other time. It is the quietest failure in the set: a gate that stands down
for a missing tool at least prints SKIPPED, and a gate that never ran leaves
nothing in any log to notice. An uninvoked gate is easy to quote as evidence.

A script nothing invokes is either a gate to wire or a tool — a person runs
`check-merge-ready.sh` before a merge and there is no push at which it belongs
— and the tools are named below with what invokes them instead.

The bound in the other direction: a script named here as a tool that something
has since started calling fails, because the entry then describes an arrangement
that ended, and a list things get filed into is where they stop being read.

What this cannot see, stated rather than implied: whether the invocation is
reached. `check-tidy` is called from `check-style` and stands down in CI for a
missing compile database; `check-shell` is called and stands down where CI has
no shellcheck. Being
invoked is the floor, not the ceiling — the terms above it are which tool is
installed where it runs, and whether anything reads what it prints.

One more term, because it fails in the same silence: whether a gate that runs can
still fail. A rule that has stopped matching prints the same clean line as a tree
with nothing to report, so being invoked and being useful are separate facts.
`Scripts/run-gate-self-tests.py` holds that one for every gate carrying a
self-test, and reports how many it found. Most gates carry none and have no
answer to it — a gap this file cannot close and should not be read as covering.

Stdlib only.

    python3 Scripts/check-gates-invoked.py
"""

import pathlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ubq_roots  # noqa: E402

REPO = ubq_roots.subject_root()

GATE = re.compile(r"^Scripts/check-[a-z0-9-]+\.(?:sh|py)$")

# Files that can run something. A document mentioning a gate's name is
# describing it, and a baseline recording which gate wrote it is data — neither
# invokes anything, and counting them would report a gate as wired because
# somebody wrote about it.
CALLER_SUFFIXES = (".sh", ".py", ".yml", ".yaml", ".cmake")

# Scripts a person runs at a moment no automation can pick. Each says what
# invokes it, because "nothing calls it" and "a person calls it when X" are
# indistinguishable from the file itself.
# A gate that READS another gate's source is not calling it. Declared as a pair
# rather than inferred: separating a name that runs from a name that is merely
# spelled needs to know which position it sits in, and caller-shaped files hold
# mentions of both kinds — an assignment that a later line executes through a
# variable looks exactly like one that does not.
# Guessing that apart is the confusion this gate is downstream of, so the few
# reading relationships are named and everything else stays a caller.
READS_RATHER_THAN_CALLS = {}

# Scripts a person runs at a moment no automation can pick are named, with the
# reason, by `tool` lines of Scripts/gate-ledger.<repository>.txt.
TOOLS = ubq_roots.ledger(REPO)[2]



# A line that prints the name is telling a reader to run it, which is the
# opposite of running it. Detecting the difference is this file's own subject: a
# gate named in a suggestion is exactly as uninvoked as one named nowhere, and
# counting the mention turns the advice into evidence that the advice was taken.
# `section` is this repository's own heading printer, and it takes the gate's
# name as its text — the most common way a gate is named without being run.
PRINTS = re.compile(r"\b(?:echo|printf|print|section|std::println)\b")
COMMENT = re.compile(r"^\s*(?:#|//)")
# `FOO_OUT="$TMPDIR_RUN/check-foo-out"` names a gate to build a path from it.
ASSIGNMENT = re.compile(r"^\s*[A-Za-z_][A-Za-z0-9_]*=")


def callsIt(lines, name):
    """Whether the text runs `name` rather than mentioning it.

    - Parameter lines: The file's lines, split once by the caller.
    - Parameter name: The gate's basename.
    - Returns: True when at least one line invokes it — names it as a path, out-
      side a comment, an assignment and a print.

    Two forms are a call. A path reaches the gate through its directory —
    `./Scripts/check-acronyms.py`, `"$ROOT/Scripts/check-slop.py"`, or through `bash` —
    so the basename is preceded by a slash, quoted or not. A bare token outside any
    quote is a shell word: a gate's basename on its own line inside a `CHECKS=(...)`
    array that a loop invokes through a variable, where the literal basename appears
    only as the list element. A bare name INSIDE a quote is prose — a dict value naming a
    gate a design might extend, a sentence, a label. That last form is the one that
    let a gate whose only surviving reference was such a sentence report as wired
    after its real call was deleted. Every occurrence on a line is examined, so a
    name in a printed label and again as a command still counts as run.
    """
    for line in lines:
        if COMMENT.match(line) or ASSIGNMENT.match(line):
            continue
        start = 0
        while True:
            position = line.find(name, start)
            if position < 0:
                break
            start = position + 1
            printing = PRINTS.search(line)
            if printing and printing.start() < position:
                continue
            before = line[:position]
            is_path = position > 0 and line[position - 1] == "/"
            in_quote = before.count('"') % 2 == 1 or before.count("'") % 2 == 1
            if is_path or not in_quote:
                return True
    return False


def tracked():
    # `--others --exclude-standard` puts a file the tree does not track yet in the
    # set, and keeps everything gitignored out. A gate being added is staged, so
    # it is already inside the hook's population; a tracked-only listing has
    # never heard of it and reports it as a TOOLS entry naming nothing.
    #
    # That verdict's remedy is "delete the entry with the script", which is the
    # dangerous direction: acting on it removes the registration for a script
    # that is about to exist, and for a Metal-dependent gate it would take real
    # coverage with it. A silent pass leaves the code alone; this one gets acted
    # on, and the action is the damage.
    listing = subprocess.run(
        ["git", "-C", REPO, "ls-files", "--cached", "--others", "--exclude-standard"],
        capture_output=True, text=True)
    # Narrowed to what is on disk. `--cached` is the INDEX, which still lists a
    # file deleted from the working tree, so a gate ported away and not yet
    # staged reads as one that exists and is invoked by nothing — a finding about
    # the index dressed as a finding about the tree. `--others` covers the other
    # direction, a new gate not yet added.
    return [path for path in listing.stdout.split("\n")
            if path and (pathlib.Path(REPO) / path).exists()]


def main():
    paths = tracked()
    gates = sorted(path for path in paths if GATE.match(path))
    if not gates:
        print("check-gates-invoked: no Scripts/check-* matched — the naming "
              "convention changed and", file=sys.stderr)
        print("  nothing was compared.", file=sys.stderr)
        return 2

    texts = {}
    for path in paths:
        # This file names every gate it excuses, as data. Counting itself as a
        # caller reports each of them as wired — and it does so only once the
        # file is tracked, so the census passes while untracked and fails on the
        # commit that adds it.
        if os.path.abspath(os.path.join(REPO, path)) == os.path.abspath(__file__):
            continue
        if not (path.endswith(CALLER_SUFFIXES)
                or path.startswith("Scripts/hooks")
                or path == "Scripts/hooks-dispatch"):
            continue
        try:
            with open(os.path.join(REPO, path), encoding="utf-8",
                      errors="replace") as handle:
                texts[path] = handle.read()
        except OSError:
            continue

    # Split each file once rather than once per gate. The scan below asks one
    # question per gate of every file, and re-splitting the text for each of
    # them would multiply the regex work by the number of gates.
    splitLines = {path: text.split("\n") for path, text in texts.items()}

    invoked = {}
    exercised = {}
    for gate in gates:
        name = os.path.basename(gate)
        # A file that does not contain the name anywhere cannot call it, and
        # answering that with a substring search skips the line walk for almost
        # every pair -- most files mention no gate at all.
        callers = sorted(path for path, text in texts.items()
                         if path != gate and name in text
                         and callsIt(splitLines[path], name)
                         and (path, name) not in READS_RATHER_THAN_CALLS)
        # A caller under Tests/ tests the gate rather than using it. Being
        # tested and being run are different facts, and counting the first as
        # the second is how a gate with a thorough test suite and no caller
        # reads as wired.
        exercised[gate] = [path for path in callers if path.startswith("Tests/")]
        invoked[gate] = [path for path in callers if not path.startswith("Tests/")]

    failures = []
    for gate in gates:
        name = os.path.basename(gate)
        if invoked[gate]:
            if name in TOOLS:
                failures.append("  %s is named a tool (\"%s\") but %s calls it — "
                                "delete the entry"
                                % (name, TOOLS[name], invoked[gate][0]))
            continue
        if name in TOOLS:
            continue
        if exercised[gate]:
            failures.append("  %s is exercised by %s and invoked by nothing else. "
                            "A test proves it works; it does not point it at the "
                            "tree. Wire it, or name it in TOOLS."
                            % (name, exercised[gate][0]))
        else:
            failures.append("  %s is called by nothing. A gate nobody invokes leaves "
                            "no line in any log, so it reads the same as one that "
                            "passed." % name)

    for name in sorted(TOOLS):
        if not any(os.path.basename(gate) == name for gate in gates):
            failures.append("  %s is named a tool but no such script exists — "
                            "delete the entry with the script" % name)

    # Both directions, because an excuse for a mention that has become a call
    # reads exactly like one that is still holding.
    for (reader, name), why in sorted(READS_RATHER_THAN_CALLS.items()):
        if reader not in texts:
            failures.append("  READS_RATHER_THAN_CALLS names %s, which is not a "
                            "file this walks — delete the pair" % reader)
        elif name not in texts[reader]:
            failures.append("  %s no longer names %s (\"%s\") — delete the pair, "
                            "so the next mention counts as a call"
                            % (reader, name, why))

    if failures:
        print("check-gates-invoked: a gate runs only when someone types it",
              file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("""
  Wire it into Scripts/check-style.py, Scripts/pre-push-gates.sh, a hook, or a
  workflow — or name it in TOOLS with what invokes it instead.""",
              file=sys.stderr)
        return 1

    wired = sum(1 for gate in gates if invoked[gate])
    # The verdict's three numbers are read as a partition, so they have to be
    # one. They are counted separately — one over gates, one over TOOLS — and a
    # ratio whose parts are drawn from different populations is the shape
    # ubiquitty-performance once found here: a numerator of 108 tested against a
    # denominator of 99, which nothing could ever reach.
    # The bound above catches the case that produces it here; this is what makes
    # the line readable as a partition rather than three unrelated tallies.
    if wired + len(TOOLS) != len(gates):
        print("check-gates-invoked: %d wired + %d run by hand does not account "
              "for %d gates" % (wired, len(TOOLS), len(gates)), file=sys.stderr)
        return 1
    print("check-gates-invoked: ok (%d gates = %d invoked by something + %d run "
          "by hand with a stated reason)" % (len(gates), wired, len(TOOLS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
