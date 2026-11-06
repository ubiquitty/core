#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-other-repositories — Docs/other-repositories.txt lists only names this repository uses.

The file excuses a path or symbol this repository's documents and comments name
and another repository owns. An entry that no file here names excuses nothing, and
one the owning repository no longer has excuses a pointer to a file that is gone.
The first is checked wherever the file is read; the second wherever the owning
repository's files are reachable (a library mount, or the monorepo's own tree).

    Scripts/check-other-repositories.py
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402
import ubq_roots  # noqa: E402

GATE = "check-other-repositories"
LEDGER = os.path.join("Docs", "other-repositories.txt")
TOKEN = re.compile(r"[A-Za-z0-9_./+@-]+")


def entries(root):
    """The (kind, repository, name) lines of the ledger, or None when the repository keeps none."""
    try:
        with open(os.path.join(root, LEDGER), encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError:
        return None
    found = []
    for line in lines:
        words = line.split("#", 1)[0].split()
        if len(words) == 3:
            found.append(tuple(words))
    return found


def tokens(root):
    """Every name a tracked text file of the repository could be pointing at, with its trailing sub-paths."""
    listing = subprocess.run(["git", "-C", root, "ls-files"], capture_output=True, text=True, check=True)
    names = set()
    for path in listing.stdout.split("\n"):
        if not path or path == LEDGER:
            continue
        try:
            with open(os.path.join(root, path), encoding="utf-8", errors="ignore") as handle:
                text = handle.read()
        except OSError:
            continue
        if "\0" in text:
            continue
        for found in TOKEN.findall(text):
            parts = found.strip(".").split("/")
            for start in range(len(parts)):
                names.add("/".join(parts[start:]))
    return names


def reachable(root, repository):
    """The checkout holding a repository's files, or None when this one cannot reach them."""
    if repository in ubq_roots.repositories(root):
        return root
    library = {"core": ubq_roots.core_root(root), "kit": ubq_roots.kit_root(root)}.get(repository)
    return library if library and library != root else None


def selfTest():
    """A name nothing here uses is refused, and a name a file uses is kept.

    - Returns: 0 when both hold, 1 otherwise.
    """
    import tempfile
    failures = []
    with tempfile.TemporaryDirectory() as directory:
        os.makedirs(os.path.join(directory, "Docs"))
        with open(os.path.join(directory, "Docs", "guide.md"), "w", encoding="utf-8") as handle:
            handle.write("See Source/Platform/macOS/Beam.swift for the palette.\n")
        os.makedirs(os.path.join(directory, "Source", "Platform", "macOS"))
        with open(os.path.join(directory, "Source", "Platform", "macOS", "Beam.swift"), "w", encoding="utf-8") as handle:
            handle.write("// the palette\n")
        subprocess.run(["git", "init", "-q", directory], check=True)
        subprocess.run(["git", "-C", directory, "add", "-A"], check=True)
        for ledger, expected in (("path app Source/Platform/macOS/Beam.swift\n", 0),
                                 ("path app Source/Platform/macOS/Beam.swift\n"
                                  "path app Source/Platform/macOS/Gone.swift\n", 1)):
            with open(os.path.join(directory, LEDGER), "w", encoding="utf-8") as handle:
                handle.write(ledger)
            subprocess.run(["git", "-C", directory, "add", "-A"], check=True)
            finished = subprocess.run([sys.executable, os.path.abspath(__file__)], cwd=directory,
                                      capture_output=True, text=True, check=False)
            if finished.returncode != expected:
                failures.append("ledger %r exited %d, expected %d" % (ledger, finished.returncode, expected))
    for failure in failures:
        print("%s --self-test: %s" % (GATE, failure), file=sys.stderr)
    if failures:
        return 1
    print("%s --self-test: ok (an entry nothing names is refused, one a file names is kept)" % GATE)
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()
    root = ubq_roots.subject_of(__file__)
    found = entries(root)
    if found is None:
        print("%s: ok (this repository keeps no ledger of other repositories' names)" % GATE)
        return 0
    named = tokens(root)
    unused, gone = [], []
    for kind, repository, name in found:
        if name not in named and os.path.basename(name) not in named:
            unused.append("%s %s %s" % (kind, repository, name))
            continue
        owner = reachable(root, repository)
        if kind == "path" and owner and not os.path.exists(os.path.join(owner, name)):
            gone.append("%s: %s" % (repository, name))
    if unused:
        return gate_report.failed(GATE, "ledger entr(ies) that nothing here names — delete them", unused)
    if gone:
        return gate_report.failed(GATE, "ledger path(s) the owning repository does not have", gone)
    return gate_report.ok(GATE, "%d entr(ies), each named here%s" % (
        len(found), " and present in the repository that owns it" if any(
            reachable(root, repository) for _kind, repository, _name in found) else ""))


if __name__ == "__main__":
    sys.exit(main())
