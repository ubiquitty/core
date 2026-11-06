#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Finds array expansions that abort the script on the bash macOS ships.

macOS ships bash 3.2, where `"${arr[@]}"` on an EMPTY array is an unbound
variable rather than zero words. Under `set -u` that aborts the script at the
expansion, so an array filled conditionally kills its own script on exactly the
machine the script is written for. bash 4.4 and later expand it to nothing, so
the same script is correct everywhere else and the failure only reaches the
developer running it locally.

shellcheck does not report this at any severity — it is a version behaviour, not
a syntax question. So there is nothing else in the tree that can see it.

The guard is `${arr[@]+"${arr[@]}"}`: nothing when the array is unset, the same
quoted elements when it is not. The populated expansion is unchanged, so
applying it does not turn on whether a given array can reach empty.

## What is reported

An expansion is reported when every one of these holds:

  - the file sets `-u` (otherwise the expansion is merely empty, not fatal)
  - the array is initialised empty somewhere (`arr=()`), which is what makes an
    empty state reachable at all
  - the expansion is bare `"${arr[@]}"`, not either guard form
  - the file tests `${#arr[@]}` nowhere, since an author who counts the array
    has the empty case in hand and may be guarding by branch

The last two conditions are what keep this from reporting the whole tree, and
they are also where it goes blind: a file that counts one array and expands a
different one bare is not reported. A pass that reports more than a reader acts
on gets muted, and a muted pass finds nothing at all.
"""

import os
import re
import subprocess
from pathlib import Path
import sys
import tempfile

ARRAY_INITIALISED_EMPTY = re.compile(r"^\s*(?:local\s+|declare\s+-a\s+)?([A-Za-z_][A-Za-z0-9_]*)=\(\s*\)", re.M)
SETS_NOUNSET = re.compile(r"^\s*set\s+-[a-zA-Z]*u", re.M)


def findingsFor(path, text):
    """The bare expansions in one script that abort on bash 3.2.

    - Parameters:
      - path: The path to name in a finding.
      - text: The script's contents.
    - Returns: A list of (path, line number, array name, source line).
    """
    if not SETS_NOUNSET.search(text):
        return []

    found = []
    lines = text.split("\n")
    for name in sorted(set(ARRAY_INITIALISED_EMPTY.findall(text))):
        if "${#%s[@]}" % name in text:
            continue
        bare = re.compile(r'"\$\{%s\[@\]\}"' % re.escape(name))
        guards = ("${%s[@]+" % name, "${%s+" % name)
        for index, line in enumerate(lines):
            if line.lstrip().startswith("#"):
                continue
            for match in bare.finditer(line):
                before = line[: match.start()]
                if any(before.endswith(guard) or guard in before for guard in guards):
                    continue
                found.append((path, index + 1, name, line.strip()))
    return found


def shellScripts(root):
    """Every tracked shell script, including the hooks that have no extension.

    - Parameter root: The repository root.
    - Returns: Paths relative to the root.
    """
    listed = subprocess.run(
        ["git", "ls-files", "Scripts", ".githooks", "Tests"],
        cwd=root, capture_output=True, text=True, check=False,
    ).stdout.split()
    paths = []
    for entry in listed:
        if entry.endswith(".sh") or "/hooks/" in entry:
            paths.append(entry)
    return paths


def selfTest():
    """Checks the detector against a construct it must catch and one it must not.

    A checker for a defect nobody currently has is a claim, and this one would
    report zero on a clean tree whether it worked or not. Both directions are
    exercised, because a rule that flags everything also passes the first half.

    - Returns: Zero when both arms answered as they must.
    """
    bad = '#!/bin/bash\nset -euo pipefail\nf=()\n[ "$1" = x ] && f=(--x)\necho "${f[@]}"\n'
    guarded = bad.replace('"${f[@]}"', '${f[@]+"${f[@]}"}')
    counted = bad.replace('echo "${f[@]}"', 'if [ "${#f[@]}" -gt 0 ]; then echo "${f[@]}"; fi')
    unset = bad.replace("set -euo pipefail", "set -eo pipefail")

    arms = [
        ("the bare expansion is reported", bad, 1),
        ("the guarded expansion is not", guarded, 0),
        ("a counted array is left alone", counted, 0),
        ("without -u there is nothing to abort", unset, 0),
    ]
    failures = 0
    for description, source, expected in arms:
        actual = len(findingsFor("probe.sh", source))
        mark = "ok " if actual == expected else "FAIL"
        if actual != expected:
            failures += 1
        print("  %s %s (reported %d, must be %d)" % (mark, description, actual, expected))

    # The construct has to actually abort, or the rule is about nothing. Run it
    # on the interpreter macOS ships rather than on whatever `bash` resolves to.
    with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as handle:
        handle.write(bad)
        probe = handle.name
    try:
        completed = subprocess.run(["/bin/bash", probe, "y"], capture_output=True, text=True, check=False)
        aborts = "unbound variable" in completed.stderr
        if aborts:
            print("  ok  /bin/bash aborts on the construct (%s)" % completed.stderr.strip())
        else:
            # bash 4.4 stopped treating an empty array under `set -u` as unbound,
            # so a modern /bin/bash — the Linux container's — does not abort. The
            # rule still holds: macOS ships bash 3.2, which does, and the source
            # scan above runs on every platform. Demonstrate the abort where
            # /bin/bash exhibits it rather than failing where it does not.
            version = subprocess.run(["/bin/bash", "--version"], capture_output=True, text=True, check=False)
            firstLine = version.stdout.splitlines()[0] if version.stdout else "/bin/bash, version unknown"
            print("  --  /bin/bash does not abort here (%s); the construct still aborts the bash 3.2 macOS ships" % firstLine)
    finally:
        os.unlink(probe)

    print("check-empty-array-expansion: self-test %s" % ("ok" if failures == 0 else "FAILED"))
    return 1 if failures else 0


def main():
    # The self-test judges fixtures it carries, not the tree, so it must not be
    # gated behind finding a repository: below the check it fails on a caller
    # whose cwd is not a repo, reporting "not a git repository" about a test
    # that never needs one.
    if "--self-test" in sys.argv:
        return selfTest()

    # The subject is the repository git reports for the caller's directory, so a
    # repository that runs a library's copy of this gate judges its own scripts.
    # ctest runs the stanzas from the build directory, which is not a repository,
    # and there `git rev-parse` answers nothing; the repository holding this
    # file is the subject then, so the gate never refuses a tree it has not
    # looked at.
    here = Path(__file__).resolve().parent.parent
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=False).stdout.strip()
    if not root:
        root = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=here,
                              capture_output=True, text=True, check=False).stdout.strip() or str(here)
    if not os.path.isdir(os.path.join(root, ".git")) and not os.path.isfile(os.path.join(root, ".git")):
        print("check-empty-array-expansion: REFUSED — %s is not a repository, so no shell "
              "script was read and nothing was judged." % root, file=sys.stderr)
        return 2

    paths = shellScripts(root)
    findings = []
    for path in paths:
        full = os.path.join(root, path)
        try:
            with open(full, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        findings.extend(findingsFor(path, text))

    if findings:
        print("check-empty-array-expansion: bash 3.2 aborts on these when the array is empty:", file=sys.stderr)
        for path, line, name, source in findings:
            print("  %s:%d  %s" % (path, line, source), file=sys.stderr)
            print("      guard it:  ${%s[@]+\"${%s[@]}\"}" % (name, name), file=sys.stderr)
        return 1

    print("check-empty-array-expansion: ok (%d shell script(s); shellcheck reports this at no severity)"
          % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())
