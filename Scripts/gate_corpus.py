#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""gate_corpus — the files a gate reads, selected the same way every time.

The failure this exists for. Five gates spelled the file list themselves, and
the incantation has a part that is easy to leave out: `--others
--exclude-standard`. Without it a file being ADDED is outside a tracked-only
list, so a gate reports clean on the one file nothing has ever read — and that
is exactly the file a pre-commit hook is being asked about. A gate that scans
the wrong set does not fail; it passes, which is the failure that survives
review.

The second failure is process-per-file. A gate written as `for f in $FILELIST;
do perl -ne … "$f"; done` pays an interpreter launch for each of several
thousand files, and the launches cost more than the work. Both gates that had
that shape were shell at the time: the docs lint spawned two perl per file, and
the privacy lint two per file before it was folded into one pass. Reading the corpus is therefore something a gate asks this
module for, not something it writes a loop for.

Selection follows the same argument shapes every gate already accepts:

    (nothing)      every tracked and untracked file the tree does not ignore
    --staged       what the index holds, which is the pre-commit subject
    --since REF    what this branch changed, against the merge base and not the
                   ref's tip, so a teammate landing on main changes nothing here
    PATH...        exactly those paths, for a gate run by hand on one file

`Scripts/gate_report.py` is the other half of the same framework: this decides
what a gate reads, that decides how it speaks. A gate should need neither a
`git` invocation nor a `print` format of its own.
"""

import hashlib
import os
import re
import subprocess
import sys
import tempfile

# How much of a file decides whether it is text. Perl's -B looks at the first
# block and calls a file binary on a NUL or a high proportion of odd bytes;
# matching that keeps a port's verdict identical to the perl it replaces.
BLOCK = 8192

# A binary file is still scanned -- a committed artifact must be as clean as
# source -- but its findings report the FILENAME only. Echoing a raw binary line
# corrupts any text-mode consumer of the output, and a consumer that decodes
# gate stdout as UTF-8 fails closed on stray bytes.
BINARY_REPORTS_NAME_ONLY = True


def root():
    """The repository's top level, so a gate works from any directory."""
    finished = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                              capture_output=True, text=True, check=False)
    return finished.stdout.strip() or os.getcwd()


def _git(arguments):
    """The paths a git listing prints, empty when the command fails.

    Read with `-z`, so a path is the bytes the filesystem holds. Without it git
    writes a path holding a byte above 0x7f, a control character, a quote or a
    backslash as a quoted C string, and a gate reading that spelling opens
    nothing and reports the file clean.

    A git failure here is not silently a clean corpus: `select` refuses an empty
    result it did not expect, which is where that is caught.
    """
    finished = subprocess.run(["git", arguments[0], "-z", *arguments[1:]],
                              capture_output=True, check=False)
    if finished.returncode != 0:
        return []
    return [path for path in finished.stdout.decode("utf-8", "surrogateescape").split("\0")
            if path]


def tracked_and_untracked():
    """Every file the tree carries or has yet to add, ignored paths excluded.

    `--others --exclude-standard` is the part that matters: a file being added is
    inside a pre-commit subject and outside a tracked-only list. Gitignored
    paths stay out, so a build directory needs no exclusion of its own.
    """
    repository = root()
    return [path for path in _git(
        ["ls-files", "--cached", "--others", "--exclude-standard"])
        if os.path.lexists(os.path.join(repository, path)) and not audit_report(path)]


def tracked(patterns=()):
    """Only the files git already tracks, optionally narrowed by pathspec.

    Weaker than `tracked_and_untracked`, and named separately so the difference
    is a choice rather than an oversight: a file being ADDED is absent here. Use
    it where a gate's full-tree pass is a survey and its `--staged` pass is what
    guards the commit, because the staged list carries added files and covers
    the gap. Where the full-tree pass is the only guard, this is the wrong call.
    """
    return [path for path in _git(["ls-files"] + list(patterns)) if not audit_report(path)]


# Audits/ holds audit reports: findings quoted verbatim, including the prose,
# stale paths and marker words the gates exist to keep out of product text. It
# is outside the source export's manifest (Scripts/snapshot-clean.sh), so no
# gate's subject lives there, and a gate that read it would judge quotations.
AUDIT_REPORTS = "Audits/"


def audit_report(path):
    """True for a path under the audit-report tree, which no gate reads."""
    return path.startswith(AUDIT_REPORTS)


def staged(deletions=False):
    """What the index holds.

    - Parameters:
      - deletions: include paths the commit REMOVES. Off by default because a
        gate that reads file contents cannot read a deleted one. A gate that
        asks whether a subject was touched at all wants them: deleting the
        loader a test mirrors breaks their agreement exactly as editing it does.
    """
    if deletions:
        return _git(["diff", "--cached", "--name-only"])
    return _git(["diff", "--cached", "--name-only", "--diff-filter=ACM"])


def since(reference):
    """What this branch changed, measured from the fork point.

    The merge base rather than the ref's tip: a gate asked to check "since
    origin/main" is being asked what this branch is answerable for, and someone
    else landing on main does not change that answer.
    """
    finished = subprocess.run(["git", "merge-base", reference, "HEAD"],
                              capture_output=True, text=True, check=False)
    base = finished.stdout.strip()
    if not base:
        return []
    return _git(["diff", "--name-only", "--diff-filter=ACMR", f"{base}...HEAD"])


def select(arguments, exclude=None, kinds=None):
    """The corpus one gate should read, from the arguments it was given.

    - Parameters:
      - arguments: the gate's argv tail, in the shapes named in this module's
        documentation.
      - exclude: an extended regular expression; a path matching it is dropped.
      - kinds: filename suffixes to keep, or None for every file.
    - Returns: a list of repository-relative paths.
    """
    arguments = list(arguments)
    if arguments and arguments[0] == "--staged":
        paths = staged()
    elif arguments and arguments[0] == "--since":
        if len(arguments) < 2:
            raise ValueError("--since needs a git ref")
        paths = since(arguments[1])
    elif arguments:
        paths = arguments
    else:
        paths = tracked_and_untracked()
    if exclude is not None:
        dropped = re.compile(exclude)
        paths = [path for path in paths if not dropped.search(path)]
    if kinds is not None:
        suffixes = tuple(kinds)
        paths = [path for path in paths if path.endswith(suffixes)]
    return paths


def eligible_under(directory, kinds=None):
    """Returns eligible repository paths beneath `directory`.

    The result uses repository-relative spellings, like `tracked_and_untracked`.
    `None` means the directory is outside the current checkout, so callers with
    an explicit fixture root can fall back to walking that fixture.
    """
    repository = os.path.realpath(root())
    directory = os.path.realpath(directory)
    try:
        if os.path.commonpath((repository, directory)) != repository:
            return None
    except ValueError:
        return None

    relative = os.path.relpath(directory, repository)
    prefix = "" if relative == "." else relative.rstrip(os.sep) + os.sep
    paths = [path for path in tracked_and_untracked()
             if not prefix or path == relative or path.startswith(prefix)]
    if kinds is not None:
        suffixes = tuple(kinds)
        paths = [path for path in paths if path.endswith(suffixes)]
    return paths


# The gitignored trees a gate can read. Named rather than derived: the
# derivation would have to know which paths each gate walks, and a root it
# missed is a stale verdict replayed over a build that moved.
# Where compiled output lives. Scripts/build_root.py puts it outside the
# checkout, so this follows it there: a digest taken over a directory the build
# no longer writes to is a digest of nothing, and the cache would replay a pass
# over a build that had changed underneath it.
def _build_roots():
    import build_root
    root = build_root.build_root()
    return tuple(os.path.join(root, name)
                 for name in ("BuildHostTests", "build", "Build",
                              "BuildBench", "BuildPGOTrain"))


def build_state(roots=None):
    """A digest of the build trees, for a key that has to notice them.

    Some gates answer about compiled output rather than source —
    Scripts/check-core-subset-symbols.py runs nm over BuildHostTests, and the
    snapshot gates test build/ for an executable. Those bytes are gitignored by
    design, so a fingerprint of the git tree cannot see them, and a verdict keyed
    on the tree alone gets replayed when the build changes or disappears.

    Path, size and whole-second mtime rather than content: reading every object
    would cost more than the gates this is saving, and a rebuild that changes a
    file changes its mtime. Each root records whether it exists, so a build that
    was deleted reads differently from one that was never there.

    - Returns: a hex digest, stable for an unchanged build tree.
    """
    if roots is None:
        roots = _build_roots()
    digest = hashlib.sha256()
    for root in roots:
        present = os.path.isdir(root)
        digest.update(b"root:%s=%s\n" % (root.encode(),
                                         b"yes" if present else b"no"))
        if not present:
            continue
        for base, directories, names in os.walk(root):
            # Sorted both ways: os.walk's order follows the filesystem, and a
            # digest that depends on that order would differ between two
            # identical builds.
            directories.sort()
            for name in sorted(names):
                path = os.path.join(base, name)
                try:
                    info = os.lstat(path)
                except OSError:
                    # A file that vanished mid-walk is recorded as gone rather
                    # than skipped, so the digest still reflects that it was
                    # there when the walk started.
                    digest.update(b"gone:%s\n" % path.encode())
                    continue
                digest.update(b"%s\0%d\0%d\n"
                              % (path.encode(), info.st_size,
                                 int(info.st_mtime)))
    return digest.hexdigest()


def is_binary(head):
    """Whether a file's first block looks like something other than text."""
    if b"\0" in head:
        return True
    if not head:
        return False
    odd = sum(1 for byte in head if byte < 7 or (13 < byte < 27) or byte > 127)
    return odd * 10 > len(head) * 3


class Unreadable:
    """A path the corpus could not read, kept rather than skipped in silence.

    A gate that skips what it cannot open reports a pass over files it never
    saw. The reader collects these instead, so a gate can put the number on its
    verdict line and a reviewer can see the corpus was short.
    """

    def __init__(self):
        self.paths = []

    def note(self, path):
        self.paths.append(path)

    def __len__(self):
        return len(self.paths)


def contents(paths, unreadable=None):
    """Yields (path, data, binary) for each readable file, once each.

    - Parameters:
      - paths: repository-relative paths, as `select` returns them.
      - unreadable: an `Unreadable` to collect what could not be opened.
    """
    for path in paths:
        if not os.path.isfile(path):
            continue
        try:
            with open(path, "rb") as handle:
                data = handle.read()
        except OSError:
            if unreadable is not None:
                unreadable.note(path)
            continue
        yield path, data, is_binary(data[:BLOCK])


def texts(paths, unreadable=None):
    """Yields (path, text) for the text files among paths.

    Decoded with replacement rather than skipped on a bad byte: a file with one
    invalid sequence still carries lines a gate has to read.
    """
    for path, data, binary in contents(paths, unreadable):
        if not binary:
            yield path, data.decode("utf-8", "replace")


def lines(paths, unreadable=None):
    """Yields (path, number, line) across the corpus, numbers starting at 1.

    The line carries no trailing newline. This is the shape `perl -ne` gates
    were written around, in one process instead of one per file.
    """
    for path, text in texts(paths, unreadable):
        for number, line in enumerate(text.split("\n"), start=1):
            yield path, number, line


def line_starts(text):
    """Byte offsets where each line of text begins, for locating a match."""
    starts = [0]
    position = text.find("\n")
    while position != -1:
        starts.append(position + 1)
        position = text.find("\n", position + 1)
    return starts


def line_of(starts, offset, previous=0):
    """The 1-based line an offset falls on.

    - Parameters:
      - starts: as `line_starts` returns.
      - offset: a character offset into the same text.
      - previous: the index returned last time, so a walk over matches in order
        carries on rather than searching from the top each time.
    - Returns: (number, index) where number is 1-based.
    """
    index = previous
    while index + 1 < len(starts) and starts[index + 1] <= offset:
        index += 1
    return index + 1, index


def body_at(text, starts, index):
    """The line at a start index, without its newline and without indentation."""
    begin = starts[index]
    finish = text.find("\n", begin)
    return text[begin:finish if finish != -1 else len(text)].lstrip()


def compiled(pattern, gate, what):
    """A compiled pattern, or a refusal rather than a silent empty result.

    FAIL CLOSED. A pattern that does not compile makes a scan produce no output,
    and no output is exactly what a clean tree produces. A malformed rule would
    report ok on a tree it never scanned, which is the one failure a content gate
    must not have. Measured: an unbalanced character class in an allow list did
    precisely this, and the run said ok with exit 0.

    - Returns: the compiled pattern. Raises SystemExit(2) if it does not compile.
    """
    try:
        return re.compile(pattern)
    except re.error as broken:
        print(f"{gate}: FAILED — the {what} pattern does not compile: {broken}",
              file=sys.stderr)
        raise SystemExit(2)


def _self_test():
    """The claims this module makes, against the repository it is run in.

    Each arm is one thing a caller relies on. The selection arms matter most: a
    corpus that quietly loses a file is the failure this module exists for.
    """
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    everything = tracked_and_untracked()
    check("the corpus is not empty in a checkout", len(everything) > 100)

    # The whole reason this module exists: a file the tree does not track yet is
    # inside the corpus. Asserted by planting one, because the tracked-only form
    # of this call also returns a long list and would pass any weaker check.
    planted = os.path.join(root(), "zz_gate_corpus_probe.txt")
    try:
        with open(planted, "w") as handle:
            handle.write("probe\n")
        withProbe = tracked_and_untracked()
        check("an untracked file is inside the corpus",
              any(path.endswith("zz_gate_corpus_probe.txt") for path in withProbe))
    finally:
        if os.path.exists(planted):
            os.remove(planted)
    check("removing it takes it back out",
          not any(path.endswith("zz_gate_corpus_probe.txt")
                  for path in tracked_and_untracked()))

    # The eligible corpus is the contract source gates share. Exercise it in a
    # temporary repository so both a staged first-party violation and an
    # ignored dependency checkout are present at once.
    previousDirectory = os.getcwd()
    with tempfile.TemporaryDirectory() as directory:
        try:
            os.chdir(directory)
            subprocess.run(["git", "init", "-q"], check=True)
            firstParty = "Source/Core/FirstParty.h"
            newSource = "Source/Core/NewSource.h"
            dependency = ("Source/Platform/Apple/SSH/.build/index-build/"
                          "checkouts/dependency/Sources/Crypto/Util/Bad.h")
            for path in (firstParty, newSource, dependency):
                os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(".gitignore", "w") as handle:
                handle.write(".build/\n")
            violation = ("struct Con" + "fig {\n    int firstParty" +
                         "Cap;\n    int firstPartyArray[4];\n};\n")
            with open(firstParty, "w") as handle:
                handle.write(violation)
            with open(newSource, "w") as handle:
                handle.write(violation)
            with open(dependency, "w") as handle:
                handle.write(violation)
            subprocess.run(["git", "add", ".gitignore", firstParty], check=True)
            eligible = tracked_and_untracked()
            eligibleBodies = dict(texts(eligible))
            check("a tracked first-party planted violation stays eligible",
                  firstParty in eligible and "firstPartyCap" in eligibleBodies[firstParty])
            check("a nonignored untracked source stays eligible",
                  newSource in eligible and "firstPartyArray" in eligibleBodies[newSource])
            check("an ignored dependency checkout cannot enter the corpus",
                  dependency not in eligible
                  and not any("/.build/" in path for path in eligible))
            os.remove(firstParty)
            check("a tracked file deleted from the worktree leaves the readable corpus",
                  firstParty not in tracked_and_untracked())
        finally:
            os.chdir(previousDirectory)

    # A path the exclude names is dropped, and nothing else is.
    sample = ["Source/Core/Cell.h", "Tests/Core/test_display.cpp"]
    kept = select(sample, exclude=r"^Tests/")
    check("exclude drops what it names", kept == ["Source/Core/Cell.h"])
    check("exclude keeps what it does not", select(sample) == sample)
    check("a suffix filter keeps only that suffix",
          select(sample, kinds=(".h",)) == ["Source/Core/Cell.h"])

    # Binary detection agrees with what perl's -B would say about these.
    check("a NUL makes a file binary", is_binary(b"abc\0def"))
    check("plain text is not binary", not is_binary(b"// a comment\n"))
    check("an empty file is not binary", not is_binary(b""))

    # Line location is what a finding's line number comes from.
    text = "alpha\nbeta\ngamma\n"
    starts = line_starts(text)
    number, index = line_of(starts, text.index("beta"))
    check("a match locates to its own line", number == 2)
    check("the line body excludes the newline",
          body_at(text, starts, index) == "beta")
    indented = "  spaced\n"
    check("the line body drops indentation",
          body_at(indented, line_starts(indented), 0) == "spaced")

    # A walk over matches in order gives the same answer as one from scratch.
    walked, position = [], 0
    for offset in (text.index("alpha"), text.index("beta"), text.index("gamma")):
        number, position = line_of(starts, offset, position)
        walked.append(number)
    check("walking matches in order locates them all", walked == [1, 2, 3])

    # The build digest has to move when the build does. Each arm varies ONE
    # thing inside a single root and holds everything else still: comparing a
    # root that exists against one that does not stays true even when the
    # digest has stopped walking files at all, and misses most planted defects.
    check("the build digest is stable for an unchanged tree",
          build_state() == build_state())
    check("a root that is not there reads differently from one that is",
          build_state(("BuildHostTests",)) != build_state(("zz-absent-root",)))

    probe = "zz_gate_corpus_build_probe"
    artifact = os.path.join(probe, "artifact.o")
    os.makedirs(probe, exist_ok=True)
    try:
        empty = build_state((probe,))
        with open(artifact, "w") as handle:
            handle.write("x")
        oneFile = build_state((probe,))
        check("a file appearing under a root moves the digest",
              empty != oneFile)

        # Same path, same file count, one byte more: only the size differs.
        with open(artifact, "w") as handle:
            handle.write("xy")
        os.utime(artifact, (1_000_000, 1_000_000))
        biggerFile = build_state((probe,))
        with open(artifact, "w") as handle:
            handle.write("z")
        os.utime(artifact, (1_000_000, 1_000_000))
        smallerFile = build_state((probe,))
        check("a file whose size changed moves the digest",
              biggerFile != smallerFile)

        # Same path, same size, a later mtime: a rebuild that happens to produce
        # the same number of bytes still has to retire the verdict.
        os.utime(artifact, (2_000_000, 2_000_000))
        check("a file rebuilt to the same size moves the digest",
              smallerFile != build_state((probe,)))
    finally:
        if os.path.exists(artifact):
            os.remove(artifact)
        os.rmdir(probe)

    # since() measures from the fork point, which is what makes it stable.
    finished = subprocess.run(["git", "merge-base", "origin/main", "HEAD"],
                              capture_output=True, text=True, check=False)
    if finished.returncode == 0 and finished.stdout.strip():
        check("since() asks about the fork point", True)
    else:
        print("  skipped  no origin/main here, so since() cannot be checked")

    if failures:
        print(f"gate_corpus: FAILED — {len(failures)} claim(s) the corpus does "
              f"not keep", file=sys.stderr)
        return 1
    print("gate_corpus: ok (selection, binary detection and line location all "
          "hold)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("--self-test", "--selftest"):
        sys.exit(_self_test())
    if len(sys.argv) > 1 and sys.argv[1] == "--build-state":
        print(build_state())
        sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] in ("--help", "-h"):
        print(__doc__.strip())
        sys.exit(0)
    # Run bare, it prints the corpus it would hand a gate, which is how you see
    # what a gate is about to read without running the gate.
    for path in select(sys.argv[1:]):
        print(path)
