#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Check that no tracked source file has quietly lost its contents.

Every other check in Scripts reads what a file says. That is a class with one
shared blind spot: a file with nothing in it violates nothing. The prose rules,
the boundary checks, the enum-switch walk and the name-collision scan all pass
over a Core source replaced by an empty file, because each of them looks for a
defect in content and there is no content to hold one.

Only a linker notices, and the symbol it names belongs to a function no change
in the diff mentions, so the error points away from the cause rather than at
it.

    open(path, "w").write(open(path).read().replace(a, b))

Python opens the write handle first, which truncates the file, so the read that
follows returns nothing and the replace writes it back empty. A rewrite loop over
many files does this silently and reports success.

Two bounds, because the first cannot catch a rewrite that leaves a stub.

    a tracked source file with no non-blank line     absolute, always checked
    a file that loses most of its bytes in a change  compared against the merge
                                                     base, when run with --diff

The first is the floor and it is cheap. The second is what catches a rewrite that
leaves a stub rather than nothing: five surviving lines out of a thousand is the
same defect and passes the floor.

The merge base is the basis, not the branch tip. A branch that started before an
unrelated deletion landed would read that deletion as its own. The base is the
tree this change was written against, so the comparison answers what the change
itself did.

A deletion is not a loss. A file removed on purpose is absent from the tree and
is not compared; only a file that still exists and has been hollowed out counts.

Nothing in the branch under check can excuse a reduction. Its author writes
every commit message and every file in it, so any marker it carried could be
written by the same hand, or the same script, that hollowed the file. An
intended reduction stays red until it lands: the decision to merge past this
check belongs to the reviewer of that change, and once it is on the base the
comparison no longer reads it as this branch's.

Stdlib only.

    python3 Scripts/check-file-content-loss.py
    python3 Scripts/check-file-content-loss.py --diff origin/main
    python3 Scripts/check-file-content-loss.py --self-test
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402
from pathlib import Path

SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".m", ".mm", ".swift",
            ".js", ".mjs", ".jsx", ".ts", ".tsx", ".py", ".sh", ".metal",
            ".el", ".frag", ".vert", ".cmake")

# Vendored code is upstream's to shape, and a generated table can legitimately
# be regenerated to nothing when its input set empties.
SKIP_MARKERS = ("ThirdParty/", "third-party-licenses/", "node_modules/")

# How much of a file may disappear in one change before it wants a person.
# A rewrite that keeps under a third of a file is either a gutting somebody
# meant, which is rare and worth naming in the message, or an accident.
SURVIVAL_FLOOR = 0.30

# Below this a file is too small for a ratio to mean anything: a 200-byte header
# losing half its bytes is a comment being tightened, not a body disappearing.
MINIMUM_BYTES = 400


def tracked(root):
    listing = subprocess.run(["git", "-C", root, "ls-files"],
                             capture_output=True, text=True)
    for path in listing.stdout.split("\n"):
        if not path.endswith(SUFFIXES):
            continue
        if any(marker in path for marker in SKIP_MARKERS):
            continue
        yield path


def contentLines(text):
    """Non-blank lines. A file of blank lines carries no more than an empty one."""
    return [line for line in text.split("\n") if line.strip()]


def shrankTooFar(before, after):
    """Whether a file kept too little of what it had.

    A file that did not exist before, or was too small for a ratio to mean
    anything, is exempt: a 40-byte header dropping to 10 is not what this
    watches, and naming it would make the check unusable.

    - Parameter before: Its size at the merge base, or None when it is new.
    - Parameter after: Its size now.
    - Returns: True when the loss is large enough to name.
    """
    if before is None or before < MINIMUM_BYTES:
        return False
    return after < before * SURVIVAL_FLOOR


def selfTest():
    ok = True

    # What counts as content. A file of blank lines is as empty as one holding
    # nothing, which is why this reads lines rather than bytes.
    for text, want in (("", 0), ("\n\n   \n\t\n", 0), ("a\n\nb\n", 2), ("  x  \n", 1)):
        got = len(contentLines(text))
        if got != want:
            print("self-test: contentLines(%r) counted %d, wanted %d" % (text, got, want),
                  file=sys.stderr)
            ok = False

    # The ratio, either side of its floor. 1000 to 100 is the gutting this
    # exists for; 1000 to 900 is an ordinary edit and has to stay silent, or the
    # check fires on every deletion and gets muted.
    cases = ((1000, 100, True), (1000, 900, False), (1000, 299, True), (1000, 301, False),
             (None, 0, False), (MINIMUM_BYTES - 1, 0, False), (MINIMUM_BYTES, 0, True))
    for before, after, want in cases:
        got = shrankTooFar(before, after)
        if got != want:
            print("self-test: shrankTooFar(%s, %d) said %s, wanted %s"
                  % (before, after, got, want), file=sys.stderr)
            ok = False

    # The whole comparison, on a repository built for it: a stub committed on a
    # branch is named whatever its commit message says, and a file that kept
    # its content is not.
    with tempfile.TemporaryDirectory(prefix="ubq-content-loss-") as root:
        def git(*arguments):
            return subprocess.run(
                ["git", "-C", root, "-c", "user.name=Gate Test",
                 "-c", "user.email=gate@example.invalid", "-c", "commit.gpgsign=false",
                 "-c", "core.hooksPath=/dev/null", *arguments],
                capture_output=True, text=True, check=True).stdout.strip()

        git("init", "-q")
        Path(root, "Stub.h").write_text("// declaration\n" * 100)
        Path(root, "Kept.h").write_text("// declaration\n" * 100)
        git("add", "Stub.h", "Kept.h")
        git("commit", "-qm", "Base")
        base = git("rev-parse", "HEAD")
        Path(root, "Stub.h").write_text("// retained declaration\n")
        Path(root, "Kept.h").write_text("// declaration\n" * 90)
        git("add", "Stub.h", "Kept.h")
        git("commit", "-qm", "Retire declarations\n\n"
            "Reviewed-Content-Reduction: Stub.h\nReviewed-by: Somebody Else")
        _, hollow, shrunk, foundBase = compare(root, base)
        shrunkPaths = [path for path, _, _ in shrunk]
        if foundBase != base:
            print("self-test: the comparison did not find its merge base", file=sys.stderr)
            ok = False
        if shrunkPaths != ["Stub.h"] or hollow:
            print("self-test: a stub committed with a message excusing it was reported as "
                  "shrunk %s and hollow %s, wanted shrunk ['Stub.h'] and nothing hollow"
                  % (shrunkPaths, hollow), file=sys.stderr)
            ok = False

    print("check-file-content-loss: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def blobSizes(root, revision):
    """{path: byte size} at a revision, from one `git ls-tree` rather than a
    `git show` per file.

    Reading each blob to count its lines costs a subprocess per file. The
    comparison only needs to know that a file got much smaller, and a blob size
    answers that in one call.
    """
    listing = subprocess.run(
        ["git", "-C", root, "ls-tree", "-r", "-l", revision],
        capture_output=True, text=True)
    sizes = {}
    for line in listing.stdout.split("\n"):
        # <mode> blob <sha> <size>\t<path>
        head, _, path = line.partition("\t")
        if not path:
            continue
        fields = head.split()
        if len(fields) == 4 and fields[3].isdigit():
            sizes[path] = int(fields[3])
    return sizes


def mergeBase(root, against):
    found = subprocess.run(["git", "-C", root, "merge-base", "HEAD", against],
                           capture_output=True, text=True)
    return found.stdout.strip() if found.returncode == 0 else None


def compare(root, against):
    """Measures every tracked source file against both bounds.

    - Parameter root: the checkout.
    - Parameter against: the ref whose merge base is the basis, or None to
      check the floor alone.
    - Returns: (paths, hollow, shrunk, base): the files walked, those with no
      content, those below the floor as (path, before, after), and the merge
      base, None when there was none to compare against.
    """
    paths = list(tracked(root))
    hollow, shrunk = [], []
    base = mergeBase(root, against) if against else None
    previousSizes = blobSizes(root, base) if base else {}
    for path in paths:
        full = os.path.join(root, path)
        try:
            with open(full, encoding="utf-8", errors="replace") as handle:
                lines = contentLines(handle.read())
        except OSError:
            continue
        if not lines:
            hollow.append(path)
            continue
        before = previousSizes.get(path)
        after = os.path.getsize(full)
        if shrankTooFar(before, after):
            shrunk.append((path, before, after))
    return paths, hollow, shrunk, base


def main(argv):
    if "--self-test" in argv:
        return selfTest()

    root = ubq_roots.subject_root()
    against = None
    arguments = argv[1:]
    while arguments:
        item = arguments.pop(0)
        if item == "--diff":
            against = arguments.pop(0) if arguments else "origin/main"

    paths, hollow, shrunk, base = compare(root, against)
    if not paths:
        print("check-file-content-loss: no tracked source files matched — the "
              "walk found nothing,", file=sys.stderr)
        print("  which is not the same as nothing being wrong.", file=sys.stderr)
        return 2
    if against and not base:
        print("check-file-content-loss: no merge base with %s — comparing "
              "against the tip would" % against, file=sys.stderr)
        print("  report an unrelated deletion as this change's doing. Not "
              "compared.", file=sys.stderr)

    if hollow:
        print("check-file-content-loss: a tracked source file has no content",
              file=sys.stderr)
        for path in hollow:
            print("  %s" % path, file=sys.stderr)
    for path, before, after in shrunk:
        print("check-file-content-loss: %s kept %d of %d bytes (%.0f%%)"
              % (path, after, before, 100.0 * after / before), file=sys.stderr)

    if hollow or shrunk:
        print("""
  Every other check here reads what a file says, so none of them can see this:
  a file with no content violates no rule about content. A truncated source
  surfaces as a link error naming a symbol the change never mentioned, which
  points away from the cause.

  The usual cause is a rewrite that opened the file for writing before reading
  it. Read the whole file into a variable first, then write.

  If the file was reduced on purpose, say so in the change's description. Nothing
  in the branch can excuse it: the reviewer decides whether it lands, and once
  it has, the base carries the reduction and this check no longer reads it.""", file=sys.stderr)
        return 1

    if base:
        print("check-file-content-loss: ok (%d files, none hollow, none below "
              "%.0f%% against %s)" % (len(paths), SURVIVAL_FLOOR * 100, base[:9]))
    else:
        # Without a basis the second bound has nothing to compare, and an `ok`
        # that does not say so reads as both bounds passing. A rewrite leaving a
        # five-line stub clears the floor and is exactly what the comparison is
        # for, so the difference between the two verdicts is a real difference in
        # what was checked.
        print("check-file-content-loss: ok (%d files, none hollow; no --diff, "
              "so nothing was compared for shrinkage)" % len(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
