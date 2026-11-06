#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-backlog-growth.py — did this branch make a tolerated backlog worse?

`check-interface-docs.py` and `check-enums.sh` block a newly added violation and
tolerate the rest, so the backlog has no number and cannot be shown to be
measured correctly. The fix for that is a lower bound: a count that must not
silently shrink either, because a budget above the truth is a gate that has
never once been able to fail.

Recording the number in a file is the wrong way to get one. Several checkouts
push to this repository. A count recorded by one of them goes stale the moment another
lands anything, and the check then fails for everybody until somebody commits an
update — which is itself a push, racing the next checkout's. The tree is not at
fault when that blocks a push: the number is.

So the reference is COMPUTED, in a throwaway worktree, at the MERGE BASE of HEAD
and `origin/main` — the commit this branch forked from, not wherever main has
reached since. There is no shared file to race on, nothing to keep current, and
the question asked is the one meant: not "does this match what someone wrote
down" but "did this branch make it worse". A stale reference is impossible by
construction, which is what the lower bound protects against.

The merge base is what makes that question answerable rather than merely asked.
Against main's tip the count also carries everything main did in the meantime
with its sign flipped, so a branch that has added nothing reports additions in
proportion to how far behind it is — and the further behind, the more confident
and more wrong the number.

A real worktree, not `git checkout <ref> -- .`: that restores file CONTENTS while
`git ls-files` still reads the INDEX, so the gates would enumerate this branch's
file list and read the reference's text. The counts come back equal for the
wrong reason.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402

# Each gate, and what one finding looks like in its whole-tree output. The
# pattern counts FINDINGS rather than lines: the gates print a source excerpt
# under each finding, so a line count reads about twice the truth.
GATES = (
    ("check-interface-docs", ["python3", "./Scripts/check-interface-docs.py"],
     re.compile(r'no /// comment')),
    ("check-enums", ["./Scripts/check-enums.sh"],
     re.compile(r':\d+: (?:enum-hint|bool)')),
    # A stand-in type in a bench sharing a production type's name. Legal, often
    # right, and the thing a qualifying sweep turns into a redefinition.
    ("type-shadows", ["python3", "./Scripts/check-name-collisions.py", "--types"],
     re.compile(r'is declared in Tests/ and Source/')),
    # A free function whose name an enclosing namespace also declares. Both are
    # reachable from an unqualified call inside the inner one, so moving either
    # end silently changes which function a caller means.
    # Counts the SILENT case only — an inner declaration with the same arity as
    # the outer, which binds a bare call either way. The differing-arity ones are
    # hiding rather than overloading, so a wrong call fails to compile and needs
    # no budget.
    ("function-shadows", ["python3", "./Scripts/check-name-collisions.py", "--functions"],
     re.compile(r'an inner declaration takes the same number of parameters')),
)

REFERENCE = "origin/main"


# git exports these into a hook's environment, and they OVERRIDE the repository
# a git command discovers from its working directory. The gates enumerate their
# tree with `git ls-files`, so inherited values point the reference worktree's
# enumeration at the pushing worktree instead: it lists this branch's paths,
# every renamed file is absent there and silently skipped, and the reference
# count comes back short by whatever those files held. A rename then reads as
# growth that no line of the diff added.
#
# The push hook is the only caller that has them set, which is why this is
# invisible when the gate is run by hand.
GIT_ENVIRONMENT = (
    "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_NAMESPACE",
    "GIT_PREFIX", "GIT_CEILING_DIRECTORIES",
)


def tree_local_environment():
    """The environment with git's repository pointers removed.

    Every git command a gate runs must discover its repository from the
    directory it runs in, not from what the caller exported.
    """
    environment = dict(os.environ)
    for name in GIT_ENVIRONMENT:
        environment.pop(name, None)
    return environment


def count_findings(directory, command, pattern, environment=None):
    """Run a gate over a tree and count what it reports, or None if it did not run.

    None is the answer that makes the comparison refuse, so everything that is
    not a real count has to arrive as None. Only one side of this comparison
    fails: a count that GREW is reported and returns 1, a count that SHRANK is
    reported and passes. So a gate that dies on the branch side scores zero
    against the reference's real number, lands in the shrank list, and prints
    that the branch removed the whole backlog -- the gate went dark and the run
    reported it as progress.

    - Parameters:
      - directory: the tree to run the gate in.
      - command: the gate invocation.
      - pattern: what one finding looks like in its output.
      - environment: the environment to run it in; the tree-local one by default.
    - Returns: the number of findings, or None when the gate did not run.
    """
    result = subprocess.run(command, cwd=directory, capture_output=True, text=True,
                            env=environment or tree_local_environment())
    text = result.stdout + result.stderr
    if not text.strip():
        return None
    # 0 is a clean tree and 1 is a tree with findings; both are the gate running.
    # 2 is this tree's refusal status -- 68 gates spell "I could not read my
    # inputs" that way -- and anything above it is a death. Neither is a count.
    if result.returncode >= 2:
        return None
    # An uncaught exception also exits 1, which is the status a gate uses to
    # report findings, so the status alone cannot separate a traceback from a
    # verdict. The banner can.
    if "Traceback (most recent call last)" in text:
        return None
    return sum(1 for line in text.split("\n") if pattern.search(line))


def gate_scripts(command):
    """The script paths a gate invocation runs."""
    return [a for a in command if a.endswith((".py", ".sh"))]


def reference_cache_path(root, base):
    """Where the counts for one merge base are remembered.

    - Parameters:
      - root: the repository root.
      - base: the merge-base commit the counts describe.
    - Returns: the file path, or None when git cannot say where .git is.
    """
    located = subprocess.run(
        ["git", "-C", root, "rev-parse", "--git-path", "ubiquitty-backlog-reference"],
        capture_output=True, text=True, env=tree_local_environment())
    if located.returncode != 0:
        return None
    directory = located.stdout.strip()
    if not directory:
        return None
    if not os.path.isabs(directory):
        directory = os.path.join(root, directory)
    return os.path.join(directory, base + ".json")


def cached_reference(root, base):
    """The counts already taken at this merge base, or None.

    A commit's tree does not change, so the counts taken there are fixed for as
    long as that commit exists — including the gate scripts that took them,
    which are part of the same tree. A different base is a different file, so
    this cannot answer about the wrong one.

    - Parameters:
      - root: the repository root.
      - base: the merge-base commit.
    - Returns: the counts dict, or None when nothing is remembered.
    """
    path = reference_cache_path(root, base)
    if not path or not os.path.exists(path):
        return None
    try:
        with open(path) as handle:
            remembered = json.load(handle)
    except (OSError, ValueError):
        return None
    if not isinstance(remembered, dict):
        return None
    # Every gate must be present. A cache written by an older GATES tuple would
    # otherwise answer for the gates it knew and silently omit the rest.
    if sorted(remembered) != sorted(name for name, _c, _p in GATES):
        return None
    if not all(isinstance(value, int) for value in remembered.values()):
        return None
    return remembered


def remember_reference(root, base, counts):
    """Write the counts for a merge base, best effort.

    - Parameters:
      - root: the repository root.
      - base: the merge-base commit.
      - counts: what the gates reported there.
    """
    path = reference_cache_path(root, base)
    if not path:
        return
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        temporary = path + ".partial"
        with open(temporary, "w") as handle:
            json.dump(counts, handle)
        os.replace(temporary, path)
    except OSError:
        pass


def script_at(root, base, script):
    """One gate script's bytes as of the merge base, or None when absent there.

    Reading them out of the commit rather than a worktree is what lets the
    comparison happen when the counts came from the cache and no worktree was
    ever made.

    - Parameters:
      - root: the repository root.
      - base: the merge-base commit.
      - script: a repository-relative path.
    - Returns: the bytes, or None.
    """
    shown = subprocess.run(["git", "-C", root, "show", "%s:%s" % (base, script)],
                           capture_output=True, env=tree_local_environment())
    if shown.returncode != 0:
        return None
    return shown.stdout


def carried_by_library(root, script, libraries):
    """Whether the counter is a library's: absent from the tree, present in a library.

    Both sides of the comparison run the library's one copy, so the instrument is
    the same by construction.
    """
    return (not os.path.exists(os.path.join(root, script))
            and any(os.path.exists(os.path.join(library, script)) for library in libraries))


def same_instrument_at_base(root, base, command, libraries=()):
    """Whether this tree and the merge base would run the same gate, byte for byte.

    The worktree-free counterpart of measured_by_the_same_instrument.

    - Parameters:
      - root: the repository root.
      - base: the merge-base commit.
      - command: the gate invocation.
    - Returns: True when every script the gate runs is identical at both.
    """
    for script in gate_scripts(command):
        if carried_by_library(root, script, libraries):
            continue
        there = script_at(root, base, script)
        if there is None:
            return False
        try:
            with open(os.path.join(root, script), "rb") as handle:
                if handle.read() != there:
                    return False
        except OSError:
            return False
    return True


def measured_by_the_same_instrument(here, there, command, libraries=()):
    """True when both trees would run the same gate script, byte for byte.

    A count is only comparable to another count the same program produced.
    Changing a gate — adding a mode, widening a pattern, fixing a parser —
    changes what it counts, so comparing across the change measures the edit
    rather than the tree. A gate that is NEW on this branch has no counterpart
    at all and would otherwise report its entire population as growth, which
    makes adding one impossible.
    """
    for script in gate_scripts(command):
        if carried_by_library(here, script, libraries):
            continue
        a, b = os.path.join(here, script), os.path.join(there, script)
        if not os.path.exists(b):
            return False
        try:
            with open(a, "rb") as first, open(b, "rb") as second:
                if first.read() != second.read():
                    return False
        except OSError:
            return False
    return True


# What the counters read. A tree holding none of these is one they cannot have
# counted, whatever number they printed.
COUNTED_SUFFIXES = (".h", ".hpp", ".cpp", ".cc", ".mm", ".m", ".swift")


def source_population(directory):
    """How many files under the tree the counters could read.

    - Parameter directory: the tree to size.
    - Returns: the number of source files under Source/ and Tests/.
    """
    found = 0
    for area in ("Source", "Tests"):
        base = os.path.join(directory, area)
        for _directory, subdirectories, names in os.walk(base):
            subdirectories[:] = [d for d in subdirectories
                                 if d not in ("ThirdParty", "compat", "build")]
            found += sum(1 for name in names if name.endswith(COUNTED_SUFFIXES))
    return found


def libraries_of(subject):
    """The library checkouts a repository carries, which hold the counters it lacks.

    - Returns: the distinct core and kit roots that are not the subject itself.
    """
    roots = (ubq_roots.core_root(subject), ubq_roots.kit_root(subject))
    return [root for root in dict.fromkeys(roots) if os.path.realpath(root) != os.path.realpath(subject)]


def resolved(directory, command, libraries):
    """The invocation with each `./Scripts/` script read from the checkout that holds it.

    A counter the tree carries is the tree's, so the reference and the branch are
    measured by their own; one only a library carries is the library's checkout's
    on both sides, so both are measured by the same instrument.
    """
    found = []
    for argument in command:
        if argument.startswith("./Scripts/") and not os.path.exists(os.path.join(directory, argument)):
            for library in libraries:
                if os.path.exists(os.path.join(library, argument)):
                    argument = os.path.join(library, argument)
                    break
        found.append(argument)
    return found


def counts_at(directory, libraries=(), environment=None):
    """Every counter's number for a tree, or None when one could not be taken.

    The population is checked before the counters run. Three of the four print a
    number rather than refusing when they read no files -- measured against an
    empty tree: check-interface-docs, type-shadows and function-shadows each
    report 0, and only check-enums refuses. A tree that failed to materialise
    therefore reports every backlog at zero, and since only a count that GREW
    fails, that arrives as a backlog cleared rather than as a tree unread.
    """
    population = source_population(directory)
    if population == 0:
        return None, ("%s holds no source file the counters read, so their "
                      "numbers would describe a tree nobody scanned" % directory)

    counts = {}
    for name, command, pattern in GATES:
        command = resolved(directory, command, libraries)
        # A gate whose script is not in this tree has no counterpart here, which
        # is not the same as one that ran and said nothing. That is the case
        # measured_by_the_same_instrument already reasons about -- a gate NEW on
        # this branch -- and refusing on it made adding or renaming one
        # impossible: the reference cannot run what does not exist there.
        # Omitted rather than counted, so the comparison below never reaches it.
        if any(not os.path.exists(script if os.path.isabs(script) else os.path.join(directory, script))
               for script in gate_scripts(command)):
            continue
        found = count_findings(directory, command, pattern, environment)
        if found is None:
            return None, "%s did not run" % name
        counts[name] = found
    return counts, None


def selfTest():
    """Checks that a count which was not taken cannot arrive as a number.

    Every arm here is a way of not counting. They matter because only a count
    that GREW fails: one that shrank is printed and passes, so anything that
    scores zero without reading reports the backlog as cleared.

    - Returns: 0 when every arm refuses as it should, 1 otherwise.
    """
    failures = []
    work = tempfile.mkdtemp(prefix="backlog-self-test-")
    pattern = re.compile(r"no /// comment")

    def gate(name, body):
        path = os.path.join(work, name)
        with open(path, "w") as handle:
            handle.write(body)
        return ["python3", path]

    # A death mid-run. Its traceback exits 1, the same status a gate uses to
    # report findings, so the status alone cannot tell them apart.
    crashed = count_findings(work, gate("crash.py", "raise RuntimeError('read failed')\n"), pattern)
    if crashed is not None:
        failures.append("a gate that raised scored %r rather than refusing" % crashed)

    # A refusal. 2 is what this tree's gates return for "I could not read my
    # inputs", and that is not a count of zero.
    refused = count_findings(work, gate(
        "refuse.py",
        "import sys\nprint('REFUSED', file=sys.stderr)\nsys.exit(2)\n"), pattern)
    if refused is not None:
        failures.append("a gate that refused scored %r rather than refusing" % refused)

    # A gate that ran and found things still counts: 1 is a verdict, not a death.
    counted = count_findings(work, gate(
        "found.py",
        "import sys\nprint('a.h:1: declaration has no /// comment')\n"
        "print('b.h:2: declaration has no /// comment')\nsys.exit(1)\n"), pattern)
    if counted != 2:
        failures.append("a gate reporting two findings scored %r" % counted)

    # A tree the counters cannot have read. Three of the four print a number
    # here rather than refusing, which is why the population is checked before
    # they run at all.
    empty = os.path.join(work, "empty")
    os.makedirs(os.path.join(empty, "Source"), exist_ok=True)
    os.makedirs(os.path.join(empty, "Tests"), exist_ok=True)
    if source_population(empty) != 0:
        failures.append("an empty tree reported a population")
    # Without the population check the counters are actually run against the
    # empty tree, and a tree with no Scripts/ raises rather than returning. Both
    # outcomes are the same defect: the refusal did not happen before the count.
    try:
        counts, _refusal = counts_at(empty)
    except OSError as error:
        failures.append("an empty tree reached the counters and raised (%s); the "
                        "population is meant to be checked first" % type(error).__name__)
    else:
        if counts is not None:
            failures.append("an empty tree produced counts %r rather than refusing" % counts)

    # A remembered reference must be complete and numeric, or it answers for the
    # gates it knew and silently omits the rest. Written straight to the cache
    # path so the validation is what is being tested, not the writer.
    # A repository, because the cache path is `git rev-parse --git-path`: in a
    # bare temp directory it comes back None and every arm below silently does
    # nothing. Measured — the arms passed against a stripped validation until
    # this init went in.
    # The environment matters here for the reason this file documents at the top:
    # git exports GIT_DIR into a hook, and an init that inherits it makes no
    # repository at `work` at all. Run under the push hook without stripping it,
    # the cache arms below reported that they had checked nothing -- which is the
    # arm doing its job on itself.
    subprocess.run(["git", "init", "-q", work], capture_output=True,
                   env=tree_local_environment())
    base = "0" * 40
    path = reference_cache_path(work, base)
    if not path:
        failures.append("no cache path in a repository, so the cache arms checked nothing")
    else:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        every = {name: 0 for name, _c, _p in GATES}
        for label, payload in (
                ("a cache missing a gate", {k: v for k, v in list(every.items())[:-1]}),
                ("a cache with a non-integer count", dict(every, **{list(every)[0]: "many"})),
                ("a cache naming an unknown gate", dict(every, **{"not-a-gate": 0})),
        ):
            with open(path, "w") as handle:
                json.dump(payload, handle)
            if cached_reference(work, base) is not None:
                failures.append("%s was accepted" % label)
        with open(path, "w") as handle:
            json.dump(every, handle)
        if cached_reference(work, base) != every:
            failures.append("a complete cache was rejected")

    # And the tree this runs in must still be countable, or the arm above would
    # pass by refusing everything.
    if source_population(os.path.abspath(".")) == 0:
        failures.append("the working tree reported no source files, so the empty-tree "
                        "arm above proves nothing")

    shutil.rmtree(work, ignore_errors=True)

    for failure in failures:
        print("check-backlog-growth --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-backlog-growth --self-test: ok (a crash, a refusal and an unread "
          "tree each refuse; a real verdict still counts)")
    return 0


def main(argv):
    if "--self-test" in argv:
        return selfTest()
    root = ubq_roots.subject_root()
    libraries = libraries_of(root)
    # The libraries are the current checkout's on both sides of the comparison: a
    # reference worktree holds no submodule contents.
    environment = tree_local_environment()
    if libraries:
        environment["UBQ_CORE_ROOT"] = ubq_roots.core_root(root)
        environment["UBQ_KIT_ROOT"] = ubq_roots.kit_root(root)

    here, refusal = counts_at(root, libraries, environment)
    if here is None:
        print("check-backlog-growth: %s." % refusal, file=sys.stderr)
        print("  A count that was not taken is not a backlog of zero, and only a "
              "count that", file=sys.stderr)
        print("  grew fails here -- so an untaken one would pass as a backlog "
              "cleared.", file=sys.stderr)
        return 2

    # A tree with no reference to compare against — a shallow clone, a fresh
    # checkout with no remote — gets the counts and no verdict. Reporting "no
    # growth" there would be a comparison nobody made.
    resolved = subprocess.run(["git", "rev-parse", "--verify", "--quiet", REFERENCE],
                              cwd=root, capture_output=True, text=True,
                              env=tree_local_environment())
    if resolved.returncode != 0:
        print("check-backlog-growth: no %s to compare against; counted only." % REFERENCE)
        print("  " + ", ".join("%s=%d" % (n, here[n]) for n in sorted(here)))
        return 0

    # The MERGE BASE, not the reference's tip. "Did this branch make it worse"
    # is a question about what this branch added to the commit it forked from;
    # against the tip it also answers for everything main did meanwhile, with
    # the sign inverted. A branch that is merely BEHIND scores every violation
    # main removed as one this branch added, and the diagnostic it prints — a
    # file moved, a gate widened, a suppression lifted — describes none of what
    # happened.
    based = subprocess.run(["git", "merge-base", "HEAD", REFERENCE],
                           cwd=root, capture_output=True, text=True,
                           env=tree_local_environment())
    if based.returncode != 0:
        print("check-backlog-growth: HEAD and %s share no history; counted only."
              % REFERENCE)
        print("  " + ", ".join("%s=%d" % (n, here[n]) for n in sorted(here)))
        return 0
    base = based.stdout.strip()

    # The reference side is a commit, and a commit's tree does not change — so
    # the counts taken there are the same on every push that forks from it, and
    # the gate scripts that took them are part of that same tree. Remembering
    # them by merge base skips a worktree and four whole-tree scans, which is
    # half of what this check costs.
    #
    # Nothing about this branch is cached. The near side is recomputed every
    # run, because that is the side the question is about.
    there = cached_reference(root, base)
    if there is not None:
        comparableNow = {name for name, command, _ in GATES
                         if same_instrument_at_base(root, base, command, libraries)}
        refusal = None
    else:
        reference = tempfile.mkdtemp(prefix="ubq-backlog-")
        shutil.rmtree(reference)  # git worktree add wants the path absent
        added = subprocess.run(["git", "worktree", "add", "--detach", "--quiet",
                                reference, base],
                               cwd=root, capture_output=True, text=True,
                               env=tree_local_environment())
        if added.returncode != 0:
            print("check-backlog-growth: could not create a worktree at %s" % base,
                  file=sys.stderr)
            print("  %s" % added.stderr.strip().split("\n")[0], file=sys.stderr)
            return 2

        try:
            there, refusal = counts_at(reference, libraries, environment)
            comparableNow = {name for name, command, _ in GATES
                             if measured_by_the_same_instrument(root, reference, command, libraries)}
        finally:
            subprocess.run(["git", "worktree", "remove", "--force", reference],
                           cwd=root, capture_output=True, text=True,
                           env=tree_local_environment())

        # Only a complete set is remembered. A refusal means the counts were
        # never taken, and writing that down would make the refusal permanent.
        if there is not None:
            remember_reference(root, base, there)

    if there is None:
        print("check-backlog-growth: at %s, %s." % (REFERENCE, refusal), file=sys.stderr)
        print("  The reference is what this branch is measured against; without it "
              "there is", file=sys.stderr)
        print("  no comparison to report.", file=sys.stderr)
        return 2

    # Only gates whose script is identical in both trees are comparable.
    comparable = comparableNow
    changed = sorted(set(here) - comparable)
    for name in changed:
        print("check-backlog-growth: %s changed on this branch; its count (%d) is not"
              % (name, here[name]))
        print("  compared, because a number is only comparable to one the same program made.")

    grew = [(name, there[name], here[name]) for name in sorted(here)
            if name in comparable and here[name] > there.get(name, 0)]
    shrank = [(name, there[name], here[name]) for name in sorted(here)
              if name in comparable and here[name] < there.get(name, 0)]

    for name, was, now in grew:
        print("%s: this branch adds %d (%d -> %d)" % (name, now - was, was, now),
              file=sys.stderr)
    for name, was, now in shrank:
        print("%s: this branch removes %d (%d -> %d)" % (name, was - now, was, now))

    if grew:
        print("", file=sys.stderr)
        print("check-backlog-growth: this branch grows a tolerated backlog.", file=sys.stderr)
        print("  The added-line ratchets did not see it, so it arrived another way -", file=sys.stderr)
        print("  a file moved, a gate widened, a suppression lifted, or a name that", file=sys.stderr)
        print("  reads as a new violation where it sits now.", file=sys.stderr)
        return 1

    # The base, not REFERENCE: naming the branch it did not compare against is
    # how a reader concludes the tip was checked and this branch is level with it.
    print("check-backlog-growth: ok (" +
          ", ".join("%s=%d" % (n, here[n]) for n in sorted(here)) +
          ", none grown vs the %s merge base %s)" % (REFERENCE, base[:9]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
