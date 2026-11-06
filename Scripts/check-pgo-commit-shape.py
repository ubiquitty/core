#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Hold a profile change to its own commit, with its sidecar.

A retrain rewrites a tracked binary every checkout builds from. Landing it inside an
unrelated commit costs two things that no other check recovers.

It becomes unfindable. A retrain inside a sixteen-file refactor is invisible to
anyone scanning subjects, so `git log -- Support/pgo/` is the only route to the
history of the artefact, and a reader who does not already suspect a retrain
has no reason to run it.

And it silently invalidates measurements in every other worktree. A checkout holding
a lane percentage taken against the previous blob is now quoting a `(profile,
tree)` pair that no longer exists, with nothing announcing the change, and a
lane delta published against it can only be withdrawn, not corrected.

The rule is the convention most profile commits already follow, enforced.

Two bounds, because a rule that fires in one direction rots:

    a profile change carrying unrelated files  -> FAIL, split the commit
    a profile change without its sidecar       -> FAIL, they describe each other

The second matters as much as the first. `portable-core.trained-at` names the
tree the counters were gathered on, and `pgo-staleness.sh` prefers it over the
blob's own commit precisely because a profile trained in a worktree and rebased
lands on top of commits it predates. A profile whose sidecar did not move is a
profile whose recorded provenance is another training run's.

Keyed on the PATH, never on the subject. `perf(pgo)` / `chore(pgo)` /
`build(pgo)` are the conventional prefixes and a bundled retrain need not carry
one, so a check reading subjects passes exactly the commit it exists to catch.
"""

import argparse
import subprocess
import sys
from pathlib import Path

# Every git call is anchored here rather than to the caller's cwd. ctest runs
# this gate from the build directory, which is not a repository, and `git show`
# then exits 128 -- reaching check=True and ending the gate in a traceback
# instead of a verdict. The gate lives in Scripts/, so the repository is its
# parent, and that is true wherever it is invoked from.
ROOT = Path(__file__).resolve().parent.parent

PROFILE_DIRECTORY = "Support/pgo/"
PROFILE_SUFFIX = ".profdata"
SIDECAR_SUFFIX = ".trained-at"

# A retrain as the convention lands it: the profile and its sidecar, nothing else.
RETRAIN = ["Support/pgo/portable-core.profdata", "Support/pgo/portable-core.trained-at"]

# The file list of the OSC refactor that carried a retrain, the self-test's
# large violation.
OSC_REFACTOR_WITH_RETRAIN = [
    "Source/Core/OSC/Clipboard.cpp",
    "Source/Core/OSC/Color.cpp",
    "Source/Core/OSC/Dispatch.cpp",
    "Source/Core/OSC/Internal.h",
    "Source/Core/OSC/MousePointerShape.cpp",
    "Source/Core/OSC/Notify.cpp",
    "Source/Core/OSC/State.cpp",
    "Source/Core/Terminal.h",
    "Source/Platform/macOS/UBWindowController.mm",
    *RETRAIN,
    "Tests/Core/VT/VT100Fixture.h",
    "Tests/Core/VT/test_vt100_osc.cpp",
    "Tests/Core/test_lower_capability_gates.cpp",
    "Tests/Core/test_osc52_per_term_policy.cpp",
    "Tests/Core/test_terminfo.cpp",
]


def stagedFiles():
    """Returns the repository-relative paths staged for commit.

    - Returns: a list of paths, added/copied/modified/renamed only.
    """
    result = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        cwd=ROOT, capture_output=True, text=True, check=True)
    return [line for line in result.stdout.splitlines() if line]


def commitFiles(commit):
    """Returns the paths a commit touches.

    - Parameter commit: any revision git resolves.
    - Returns: a list of repository-relative paths.
    """
    result = subprocess.run(
        ["git", "show", "--name-only", "--format=", commit],
        cwd=ROOT, capture_output=True, text=True, check=True)
    return [line for line in result.stdout.splitlines() if line]


def judge(paths):
    """Classifies a file set against the two bounds.

    A set touching no profile is not this check's business and returns no
    findings; a set is only judged once it carries one.

    - Parameter paths: repository-relative paths in the commit or index.
    - Returns: a list of finding strings, empty when the shape is right.
    """
    profiles = [p for p in paths
                if p.startswith(PROFILE_DIRECTORY) and p.endswith(PROFILE_SUFFIX)]
    if not profiles:
        return []

    findings = []
    unrelated = [p for p in paths if not p.startswith(PROFILE_DIRECTORY)]
    if unrelated:
        findings.append("carries " + str(len(unrelated)) + " unrelated file(s): "
                        + ", ".join(sorted(unrelated)[:6])
                        + ("…" if len(unrelated) > 6 else ""))

    sidecars = {p for p in paths if p.endswith(SIDECAR_SUFFIX)}
    for profile in profiles:
        expected = profile[: -len(PROFILE_SUFFIX)] + SIDECAR_SUFFIX
        if expected not in sidecars:
            findings.append("changes " + profile + " without " + expected)
        if Path(profile).name.startswith("linux-"):
            toolchain = profile[:-len(PROFILE_SUFFIX)] + ".trained-toolchain"
            if toolchain not in paths:
                findings.append("changes " + profile + " without " + toolchain)
    return findings


def report(findings, subject):
    """Prints findings against a named subject.

    - Parameters:
      - findings: the strings judge() returned.
      - subject: what was inspected, for the message.
    - Returns: 1 when there were findings, 0 otherwise.
    """
    if not findings:
        return 0
    print("check-pgo-commit-shape: " + subject, file=sys.stderr)
    for finding in findings:
        print("    " + finding, file=sys.stderr)
    print("  A retrain rewrites a binary every checkout builds from. Inside another", file=sys.stderr)
    print("  commit it is unfindable by subject, and it invalidates every other", file=sys.stderr)
    print("  worktree's in-flight measurements with nothing announcing it.", file=sys.stderr)
    print("  Split the profile and its sidecar into their own commit.", file=sys.stderr)
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--commit", help="judge a commit instead of the index")
    parser.add_argument("--self-test", action="store_true",
                        help="prove both bounds against the recorded violations")
    arguments = parser.parse_args()

    try:
        return run(arguments)
    except subprocess.CalledProcessError as failure:
        # A gate that cannot reach git has not judged the tree, and saying so is
        # the whole job here. A traceback says neither "clean" nor "dirty" -- a
        # reader cannot tell a finding from a broken gate, which is the one
        # distinction a gate exists to make.
        command = " ".join(failure.cmd) if isinstance(failure.cmd, list) else str(failure.cmd)
        print("check-pgo-commit-shape: REFUSED — git could not answer, so nothing was judged.",
              file=sys.stderr)
        print("  " + command + " exited " + str(failure.returncode), file=sys.stderr)
        if failure.stderr:
            print("  " + failure.stderr.strip().splitlines()[0], file=sys.stderr)
        return 2


def run(arguments):

    if arguments.self_test:
        return selfTest()

    if arguments.commit:
        return report(judge(commitFiles(arguments.commit)),
                      "commit " + arguments.commit + " bundles a profile change")

    paths = stagedFiles()
    status = report(judge(paths), "this commit bundles a profile change")
    if status == 0 and any(p.startswith(PROFILE_DIRECTORY) and p.endswith(PROFILE_SUFFIX)
                           for p in paths):
        print("check-pgo-commit-shape: ok (profile and sidecar, nothing else)")
    return status


def selfTest():
    """Proves both bounds fire, and that a clean retrain does not.

    The fixtures are file lists of real bundled retrains, inlined so the proof
    holds in any clone.

    - Returns: 0 when every arm behaves as stated, 1 otherwise.
    """
    failures = []

    # Bundled into a sixteen-file OSC refactor, which a subject-keyed check
    # would pass.
    if not judge(OSC_REFACTOR_WITH_RETRAIN):
        failures.append("a retrain bundled with 14 unrelated files was not flagged")

    # The smaller violation, so the rule is not keyed to a large diff.
    if not judge(["Source/Core/OSC/Dispatch.cpp", *RETRAIN]):
        failures.append("a retrain bundled with one unrelated file was not flagged")

    # A retrain in its own commit, profile and sidecar together.
    if judge(RETRAIN):
        failures.append("a retrain of profile and sidecar only was flagged")

    # A commit touching no profile at all is not this check's business.
    if judge(["Scripts/check-render-parity.sh"]):
        failures.append("a commit with no profile change was flagged")

    # The sidecar bound: a profile without its sidecar.
    if not judge(RETRAIN[:1]):
        failures.append("a profile change without its sidecar was not flagged")

    for architecture in ("x86_64", "aarch64"):
        stem = f"Support/pgo/linux-{architecture}-core"
        linux = [stem + suffix for suffix in (".profdata", ".trained-at", ".trained-toolchain")]
        if judge(linux):
            failures.append(f"a Linux {architecture} retrain with provenance was flagged")
        if not judge(linux[:-1]):
            failures.append(f"a Linux {architecture} retrain without provenance was accepted")

    for failure in failures:
        print("check-pgo-commit-shape --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-pgo-commit-shape --self-test: ok (both recorded violations fire, "
          "a clean retrain and an unrelated commit do not, and the sidecar bound "
          "holds both ways)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
