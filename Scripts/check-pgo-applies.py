#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-pgo-applies.py — how much of the profile the compiler could still use.

`-fprofile-use` decides per function whether the recorded counters still describe
the body in front of it, and says so when they do not: a control-flow change
moves the function's CFG hash, the counters are discarded, and the body compiles
without them. Every build path in this tree suppresses that diagnostic —
the macOS app's build, build-perf-bench.sh and run-bench.sh all pass
`-Wno-backend-plugin` alongside the two `-Wno-profile-instr-*` flags — so the
loss is real, per-function, and invisible. This compiles the portable-core list
with those three restored and reports what the compiler said.

Why the mass and not the count. One orphaned function holding a billion counters
outranks fifty cold ones, and a count of functions treats them alike. The lines
carry `up to N count discarded`, so the output ranks itself by the quantity that
decides how much optimisation was actually lost.

Why not a commit count. `pgo-staleness.sh` answers how far the tree has moved
since the profile was trained, which is a proxy with five independent ways to be
wrong: the baseline (the file's own commit rather than the trained-at sidecar),
the operator (a date filter rather than ancestry), the scope (one subdirectory
quoted as the whole), the direction (`A..B` returns zero when B is behind A, not
an error and not a negative), and whose tree — the sidecar resolves to different
commits at different refs, so two checkouts can read the same number about different
pairs. None of them apply here, because this is not a comparison of refs. It is
what the compiler decided about these bodies against this profile.

The three ways a zero can arise, and why the denominator is printed. A zero means
the profile fits, or the diagnostics were suppressed after all, or nothing was
compiled — and a warm build directory emits nothing for what it does not rebuild,
which looks exactly like a perfect profile. So the count of translation units
that actually compiled in this run is reported beside the finding, taken from
this run rather than from the source list, and a run that compiles nothing
refuses rather than reporting a ratio over an empty population.

The profile is identified by content, not by path. `Support/pgo/portable-core.profdata`
is a different blob in every worktree that has not converged, so a figure quoted
without the blob it describes cannot be checked by anyone else.

A verdict needs a budget, and the caller supplies it. That a function lost its
counters says its own placement is gone; it does not say the body was compiled as
though no profile existed, because its callees' counters are still valid and the
module's summary thresholds still move it. So the mass is a number to compare
against a threshold rather than a failure on its own.

Both callers supply one. `pgo-train.sh` passes `--max-discarded 0`: a freshly
trained profile that already discards anything describes a tree it was not
trained on. `pre-push-gates.sh` passes `--max-new-discarded 30000` against
`origin/main`, a figure measured from this tree's own drift and recorded at that
call site.

`pre-push-gates.sh` asks the whole-tree question as well, with
`--recorded-budget`: the total the profile discards against this tree, against
the budget `pgo-train.sh` recorded beside the profile at the retrain. The
diff-scoped bound reads one push at a time and only the units it changed —
`changed_sources` keys on the portable-core unit list, so a header edit that
reshapes every function inlining it compiles nothing there and reads zero. What
the per-push bound cannot add up, the recorded budget does. Measured 2026-10-28,
two days after a retrain whose every unit discarded nothing: 81 functions and
13,305,832,948 counters detached — the byte loop, the character emitters, the
scroll and the cell-instance walk among them — with every push between the two
under the per-push bound.

A compiler that cannot read the profile stands the gate down. GCC takes a
directory of .gcda files rather than LLVM's format, and a clang older than the
LLVM that wrote the profile refuses its format version; every build on such a
machine compiles without the profile, so there is no application to measure.
The question is put through Scripts/pgo-flags.sh, which is what decides whether
a build is handed the profile, and the run prints a SKIPPED line naming the
compiler and exits 0. UBIQUITTY_PGO=REQUIRE, as CI's PGO stages set, turns the
stand-down into a refusal, the way it turns a build without the profile into a
failure.

What this cannot see, so that a zero is read for what it is. The diagnostic
covers a body the profile KNEW and no longer matches. A body the profile never
knew — a function added since the last retrain — compiles with no counters and
draws no diagnostic at all: measured, by compiling a namespaced function absent
from the profile and confirming the emitted symbol against an empty diagnostic
stream. So a clean run means nothing here has DRIFTED, not that everything here
is profiled. `check-profile-orphans.py` is the closest signal for the other half,
and it compares namespace paths rather than symbols, which is approximate by
construction and says so.
"""

import argparse
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import tempfile
import sys

import build_jobs
import gate_report
import pgo_localize

GATE = "check-pgo-applies"

# Where the rooted copies of the profile go for one run. Every consumer build
# reads the profile rooted at its own checkout (Scripts/pgo_localize.py), so the
# probe does too: probed with the relative-keyed blob it would see no internal-
# linkage function at all, discarded or not.
ROOTED = tempfile.TemporaryDirectory(prefix="check-pgo-applies-")

# The internal-linkage function the keying proof asks after: the copy walk's
# per-cell read, in an anonymous namespace and on a lane the trainer runs, so
# a profile that reaches internal functions gives it an entry count.
KEYED_UNIT = os.path.join("Source", "Core", "Selection", "TerminalSelection.cpp")
KEYED_SYMBOL = "_ZN9Ubiquitty9Selection12_GLOBAL__N_110textCellAtEPvii"


def consumer_profile(profile: str, tree: str) -> str:
    """Returns `profile` keyed for a build of `tree`, as every consumer reads it."""
    return pgo_localize.localized_profile(profile, tree, ROOTED.name)


PGO_FLAGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pgo-flags.sh")

# The two questions pgo-flags.sh asks before a build is handed the profile: is
# the compiler clang, and does it accept this file. Asked through the script
# itself, so this gate and every build agree about which compilers apply it.
COMPILER_PROBE = r"""
compiler="$1" profile="$2" flags="$3"
set --
. "$flags"
if ! ubiquitty_compiler_reads_llvm_profile "$compiler"; then
    echo "$compiler does not read LLVM's profile format"
    exit 1
fi
if ! reason="$(ubiquitty_compiler_profile_refusal "$compiler" "$profile")"; then
    echo "$compiler cannot read the profile (${reason:-the probe compile failed})"
    exit 1
fi
"""


def compiler_refusal(cxx: str, profile: str) -> str | None:
    """Says why `cxx` cannot read `profile`, or returns None when it can.

    - Parameters:
      - cxx: The compiler command, as $CXX spells it.
      - profile: The profile's path.
    - Returns: The reason the compiler gave, or None.
    """
    done = subprocess.run(["bash", "-c", COMPILER_PROBE, "compiler-probe", cxx, profile, PGO_FLAGS],
                          capture_output=True, text=True)
    if done.returncode == 0:
        return None
    return done.stdout.strip() or f"{cxx} cannot read the profile"


def stand_down(cxx: str, profile: str) -> int | None:
    """The verdict for a compiler that cannot read the profile, or None when it can."""
    refusal = compiler_refusal(cxx, profile)
    if refusal is None:
        return None
    if os.environ.get("UBIQUITTY_PGO") == "REQUIRE":
        print("  UBIQUITTY_PGO=REQUIRE: a compiler that cannot apply the profile is a failure here.",
              file=sys.stderr)
        return gate_report.refused(GATE, refusal)
    print(f"{GATE}: SKIPPED — {refusal}, so no build here applies it and there is nothing to measure")
    return 0


def keying_proven(root: str, cxx: str, profile: str, optimisation: str) -> tuple[bool, str]:
    """Whether a consumer-shaped compile gives an internal function its counters.

    Compiles the unit at its absolute path, as CMake and the bench scripts do,
    and reads the IR for the function's entry count. A profile that names
    internal functions by another path gives none, and says nothing about it.
    """
    with tempfile.TemporaryDirectory() as scratch:
        ir = os.path.join(scratch, "unit.ll")
        command = [
            cxx, "-std=c++23", optimisation, "-DNDEBUG", "-fno-exceptions", "-fno-rtti",
            f"-fprofile-use={profile}",
            "-Wno-profile-instr-unprofiled", "-Wno-profile-instr-out-of-date", "-Wno-backend-plugin",
            "-I", os.path.join(root, "Source"),
            "-I", os.path.join(root, "Source", "ThirdParty", "simdutf"),
            "-I", os.path.join(root, "Source", "ThirdParty", "re2"),
            "-emit-llvm", "-S", os.path.join(root, KEYED_UNIT), "-o", ir,
        ]
        done = subprocess.run(command, cwd=root, capture_output=True, text=True)
        if done.returncode != 0:
            return False, f"{KEYED_UNIT} did not compile: {done.stderr.strip().splitlines()[:1]}"
        with open(ir, encoding="utf-8", errors="replace") as stream:
            for line in stream:
                if line.startswith("define") and f"@{KEYED_SYMBOL}(" in line:
                    if "!prof" in line:
                        return True, f"{KEYED_SYMBOL} has an entry count"
                    return False, f"{KEYED_SYMBOL} compiled without counters"
        return False, f"{KEYED_SYMBOL} was not emitted by {KEYED_UNIT}"


def prove_keying(root: str, cxx: str, profile: str, optimisation: str) -> int | None:
    """Fails the gate when internal-linkage functions would build unprofiled."""
    proven, detail = keying_proven(root, cxx, profile, optimisation)
    print(f"  internal-linkage keying: {detail}")
    if proven:
        return None
    print("  Every function in an anonymous namespace, every static and every lambda "
          "would compile without counters. Consumers must read the profile through "
          "Scripts/pgo_localize.py.", file=sys.stderr)
    gate_report.finding(KEYED_UNIT, GATE, detail)
    return gate_report.failed(GATE, "the profile does not reach internal-linkage functions")

# One diagnostic is one line: the file, the symbol, the hash that failed to
# match, and the counters given up because of it.
#
#   warning: <file>: function control flow change detected (hash mismatch) \
#       <mangled> Hash = <n> up to <m> count discarded [-Wbackend-plugin]
DISCARD = re.compile(
    r"hash mismatch\)\s+(?P<symbol>\S+)\s+Hash\s*=\s*\d+\s+up to (?P<mass>\d+) count discarded"
)
# The weaker pattern the strict one is checked against. A build that emits the
# flag on a line the strict pattern cannot read is a format this parser does not
# know, and reporting no discards for it would be the silent zero this gate
# exists to remove.
MENTIONS_DISCARD = re.compile(r"count discarded")

# Restored, in the order the build scripts suppress them. Only the first carries
# a finding here: `-Wbackend-plugin` reports a body the profile knows and no
# longer matches. The other two describe instrumented profiles
# (`-fprofile-instr-generate`) and say nothing against the IR profile this tree
# ships — measured by compiling a namespaced function absent from the profile,
# confirming the symbol was emitted, and reading an empty diagnostic stream.
# Restored regardless, so a toolchain that does emit them is read rather than
# suppressed here as well as in the build.
DIAGNOSTICS = [
    "-Wbackend-plugin",
    "-Wprofile-instr-out-of-date",
    "-Wprofile-instr-unprofiled",
]

# The total-drift budget, recorded beside the profile.
#
# The diff-scoped bound reads one push at a time and only the units it changed.
# What it cannot add up is the tree: pushes each under the bound, and header
# edits it never compiles, leave the profile detached from the hot set with
# every push gate green. So the profile carries a second number, written by
# pgo-train.sh at the one moment a fresh profile and its tree are both in hand:
# the total it discards then (zero, or the training run failed) plus an
# allowance. `--recorded-budget` refuses once the total against the current
# tree exceeds it, and `--record-drift-budget` is how pgo-train.sh writes it.
#
# The allowance is ten pushes at the per-push bound pre-push-gates.sh applies;
# the self-test holds PER_PUSH_BUDGET equal to the figure in that file. Ten,
# because the masses this tree produces are two populations with nothing
# between them: accumulated cold drift sits far below the allowance and one
# hot function detaching sits far above it, so one hot detachment refuses on
# its own.
PER_PUSH_BUDGET = 30000
DRIFT_ALLOWANCE_PUSHES = 10
DRIFT_ALLOWANCE = PER_PUSH_BUDGET * DRIFT_ALLOWANCE_PUSHES

# From this share of the allowance the gate says so and still passes, so a
# retrain is sequenced before the push that would be refused. Whole percent:
# the comparison is integer arithmetic, and a fraction would make the warning's
# edge depend on rounding.
DRIFT_WARNING_PERCENT = 60

PROFILE_SUFFIX = ".profdata"
BUDGET_SUFFIX = ".drift-budget"
TREE_SUFFIX = ".trained-tree"

# How a budget sidecar stands to the profile beside it.
ABSENT, MALFORMED, STALE, CURRENT = "absent", "malformed", "stale", "current"

# Where a total stands against a budget.
WITHIN, WARNING, EXCEEDED = "within", "warning", "exceeded"


def sidecar(profile: str, suffix: str) -> str:
    """The sidecar path for `profile`: the profile's stem plus `suffix`.

    The same rule pgo-staleness.sh reads `.trained-at` back with, so a profile
    written to a private path by UBQ_PGO_TRAIN_OUT carries its sidecars there.
    """
    stem = profile[:-len(PROFILE_SUFFIX)] if profile.endswith(PROFILE_SUFFIX) else profile
    return stem + suffix


def read_words(path: str) -> dict[str, str]:
    """Reads a sidecar of `key value` lines.

    - Returns: each key and its value; a line with another shape is skipped.
    """
    words: dict[str, str] = {}
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            parts = line.split()
            if len(parts) == 2:
                words[parts[0]] = parts[1]
    return words


def read_drift_budget(path: str) -> dict[str, int | str]:
    """Reads the budget sidecar pgo-train.sh wrote.

    - Returns: `trained_tree` and `baseline_tree`, and `baseline_functions`,
      `baseline_counters` and `drift_budget` as integers.
    - Raises: ValueError when a line is missing or carries no count, so a
      sidecar this cannot read is reported rather than read as a zero budget.
    """
    words = read_words(path)
    budget: dict[str, int | str] = {}
    for key in ("trained_tree", "baseline_tree"):
        if key not in words:
            raise ValueError(f"{path}: no {key} line")
        budget[key] = words[key]
    for key in ("baseline_functions", "baseline_counters", "drift_budget"):
        if key not in words:
            raise ValueError(f"{path}: no {key} line")
        if not words[key].isdigit():
            raise ValueError(f"{path}: {key} is {words[key]!r}, not a count")
        budget[key] = int(words[key])
    return budget


def write_drift_budget(path: str, trainedTree: str, baselineTree: str, functions: int,
                       counters: int, budget: int) -> None:
    """Writes the budget sidecar.

    - Parameters:
      - trainedTree: The Source/ tree the profile describes, which keys the
        sidecar to the profile.
      - baselineTree: The Source/ tree the baseline was measured against: the
        same tree at a retrain, and a later one where a budget is recorded for
        a profile already in use.
      - functions, counters: The baseline, as the measurement reported it.
      - budget: The bound, the baseline plus the allowance.
    """
    with open(path, "w", encoding="utf-8") as stream:
        stream.write(f"trained_tree {trainedTree}\n"
                     f"baseline_tree {baselineTree}\n"
                     f"baseline_functions {functions}\n"
                     f"baseline_counters {counters}\n"
                     f"drift_budget {budget}\n")


def trained_tree(profile: str) -> str | None:
    """The Source/ tree the profile's `.trained-tree` sidecar names, or None."""
    path = sidecar(profile, TREE_SUFFIX)
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as stream:
        value = stream.read().strip()
    return value or None


def budget_standing(profile: str) -> tuple[str, dict | None, str]:
    """How the budget sidecar beside `profile` stands to the profile.

    - Returns: `ABSENT`, `MALFORMED`, `STALE` or `CURRENT`; the budget where one
      was read; and one line saying why.

    Stale is a sidecar naming another Source/ tree than `.trained-tree` does:
    the profile was retrained and the budget was not, so the number describes a
    training run this profile is not from. Read as absent it would switch the
    bound off at exactly the retrain that should have reset it, so it refuses.
    """
    path = sidecar(profile, BUDGET_SUFFIX)
    name = os.path.basename(path)
    if not os.path.isfile(path):
        return ABSENT, None, f"no {name} beside the profile"
    try:
        budget = read_drift_budget(path)
    except ValueError as problem:
        return MALFORMED, None, str(problem)
    tree = trained_tree(profile)
    if tree is None:
        return STALE, budget, (f"{name} is keyed to Source/ tree {budget['trained_tree']} "
                               f"and no {TREE_SUFFIX} sidecar names one")
    if budget["trained_tree"] != tree:
        return STALE, budget, (f"{name} was recorded for Source/ tree {budget['trained_tree']}, "
                               f"and {TREE_SUFFIX} names {tree}")
    return CURRENT, budget, f"recorded for Source/ tree {tree}"


def judge_total(total: int, baseline: int, budget: int) -> tuple[str, int]:
    """Where `total` stands between `baseline` and `budget`.

    The share is of the allowance, the room between the baseline and the
    budget, because that is what a push spends: a budget recorded above a
    baseline of a million would otherwise read as three quarters used the
    moment it was written.

    - Returns: `EXCEEDED` past the budget, `WARNING` from `DRIFT_WARNING_PERCENT`
      of the allowance, `WITHIN` below; and the share of the allowance used, in
      whole percent, zero for a total under the baseline.
    """
    allowance = budget - baseline
    used = max(0, total - baseline)
    share = (used * 100) // allowance if allowance > 0 else (0 if used == 0 else 100)
    if total > budget:
        return EXCEEDED, share
    if used * 100 >= allowance * DRIFT_WARNING_PERCENT:
        return WARNING, share
    return WITHIN, share


def source_tree(root: str) -> str:
    """The Source/ tree HEAD carries, by the rule pgo-train.sh records it with."""
    done = subprocess.run(["git", "-C", root, "rev-parse", "HEAD:Source"],
                          capture_output=True, text=True)
    return done.stdout.strip() if done.returncode == 0 else ""


def repository_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def profile_identity(root: str, profile: str) -> tuple[str, str]:
    """Returns the profile's content hash and how it stands to the committed artifact.

    A run against a freshly written profile and a run against the one the tree
    ships print the same shape of verdict, and `pgo-train.sh` invokes this on the
    file it has just produced — at the default path, which is the tracked one. A
    reader who takes that line for a statement about the shipping profile carries
    away the opposite of what was measured, so the standing is named rather than
    left to a hash comparison nobody thinks to make.

    - Returns: the twelve-character content hash, and one of `committed`,
      `uncommitted` or `untracked`.
    """
    def git(*arguments: str) -> str | None:
        try:
            out = subprocess.run(["git", *arguments], cwd=root,
                                 capture_output=True, text=True, check=True)
            return out.stdout.strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            return None

    content = git("hash-object", profile)
    if content is None:
        return "unhashable", "untracked"

    relative = os.path.relpath(profile, root)
    committed = git("rev-parse", f"HEAD:{relative}")
    if committed is None:
        return content[:12], "untracked"
    return content[:12], "committed" if committed == content else "uncommitted"


def source_list(root: str) -> list[str]:
    out = subprocess.run(
        [os.path.join(root, "Scripts", "gen-portable-core-sources.sh"), "--print"],
        cwd=root, capture_output=True, text=True, check=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def compile_one(root: str, cxx: str, profile: str, relative: str, optimisation: str):
    """Compiles one translation unit with the profile applied and the diagnostics restored.

    - Returns: the unit's path, whether it compiled, and its diagnostic text.
    """
    command = [
        cxx, "-std=c++23", optimisation, "-DNDEBUG",
        "-fno-exceptions", "-fno-rtti", "-pthread",
        f"-fprofile-use={profile}",
        *DIAGNOSTICS,
        "-I", os.path.join(root, "Source"),
        "-I", os.path.join(root, "Source", "ThirdParty", "simdutf"),
        "-I", os.path.join(root, "Source", "ThirdParty", "re2"),
        "-c", os.path.join(root, relative), "-o", os.devnull,
    ]
    done = subprocess.run(command, cwd=root, capture_output=True, text=True)
    return relative, done.returncode == 0, done.stderr


def changed_sources(root: str, ref: str) -> tuple[list[str], list[str]]:
    """Returns the changed portable-core units, split by comparability.

    Three-dot against the merge base, never the tip: a branch behind `ref`
    would otherwise report every commit it lacks as its own change.

    - Returns: units present on both sides, and units new since the base —
      a new unit has no baseline, and a new function has no counters in the
      profile at all, so the second list is reported rather than compared.
    """
    out = subprocess.run(["git", "-C", root, "diff", "--name-only", f"{ref}...HEAD"],
                         capture_output=True, text=True, check=True)
    changed = {line.strip() for line in out.stdout.splitlines() if line.strip()}
    portable = set(source_list(root))
    base = subprocess.run(["git", "-C", root, "merge-base", ref, "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    both, new = [], []
    for unit in sorted(changed & portable):
        exists = subprocess.run(["git", "-C", root, "cat-file", "-e", f"{base}:{unit}"],
                                capture_output=True)
        (both if exists.returncode == 0 else new).append(unit)
    return both, new


def commitsBehind(root: str, ref: str) -> int:
    """How many commits `ref` carries that the baseline does not.

    The mass this gate reports is measured against the merge base, so it grows
    with a branch's AGE as well as with what the branch changed: every commit
    on the ref since the fork is a commit whose profile drift lands in the
    comparison. A branch forked yesterday reads zero for the same edit that
    reads tens of thousands on one forked last month.

    Returned so the verdict can say which it is looking at. Zero on any git
    failure — a distance this cannot measure must not become a claim.
    """
    try:
        base = subprocess.run(["git", "-C", root, "merge-base", ref, "HEAD"],
                              capture_output=True, text=True, check=True).stdout.strip()
        out = subprocess.run(["git", "-C", root, "rev-list", "--count", f"{base}..{ref}"],
                             capture_output=True, text=True, check=True)
        return int(out.stdout.strip() or "0")
    except (subprocess.CalledProcessError, ValueError):
        return 0


# Past this many commits behind, the reported mass is more plausibly the
# branch's age than its diff. Not a threshold the verdict turns on — the budget
# still decides pass or fail — only the point where the message starts naming a
# rebase before it names a retrain. Chosen from the budget commit's own table,
# inside the gap where the mass becomes material rather than at either edge.
staleBranchCommits = 50


class BaselineWorktree:
    """A detached worktree at the merge base, removed on exit.

    A temp-file copy of one unit would not compile — the includes are relative
    to a whole tree — so the baseline is a real checkout sharing the object
    store. GIT_DIR and GIT_WORK_TREE are scrubbed from the child environment:
    a hook exports them, and a git inheriting them targets the wrong repository
    however -C is spelled.
    """

    def __init__(self, root: str, ref: str):
        self.root = root
        self.base = subprocess.run(["git", "-C", root, "merge-base", ref, "HEAD"],
                                   capture_output=True, text=True, check=True).stdout.strip()
        self.path = tempfile.mkdtemp(prefix="pgo-baseline-")

    def environment(self) -> dict[str, str]:
        scrubbed = dict(os.environ)
        scrubbed.pop("GIT_DIR", None)
        scrubbed.pop("GIT_WORK_TREE", None)
        return scrubbed

    def __enter__(self) -> str:
        subprocess.run(["git", "-C", self.root, "worktree", "add", "--detach",
                        self.path, self.base],
                       capture_output=True, text=True, check=True,
                       env=self.environment())
        return self.path

    def __exit__(self, *_exception) -> None:
        subprocess.run(["git", "-C", self.root, "worktree", "remove", "--force", self.path],
                       capture_output=True, env=self.environment())
        subprocess.run(["git", "-C", self.root, "worktree", "prune"],
                       capture_output=True, env=self.environment())


def massBySymbol(root: str, cxx: str, profile: str, units: list[str],
                 optimisation: str, jobs: int):
    """Compiles the units in one tree and folds their discards per symbol.

    - Returns: ({symbol: mass}, compiled count, failed list, unparsed count).
    """
    mass: dict[str, int] = {}
    compiled, unparsed = 0, 0
    failed: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(compile_one, root, cxx, profile, unit, optimisation)
                   for unit in units]
        for future in concurrent.futures.as_completed(futures):
            relative, ok, diagnostics = future.result()
            if not ok:
                failed.append(relative)
                continue
            compiled += 1
            unit_records, unit_unparsed = discarded_records(diagnostics)
            unparsed += unit_unparsed
            for symbol, unit_mass in unit_records:
                mass[symbol] = mass.get(symbol, 0) + unit_mass
    return mass, compiled, failed, unparsed


def changed_from(root: str, arguments) -> int:
    """The diff-scoped mode: what does this branch detach that the base did not?

    check-profile-orphans.py (pre-commit) catches the RENAME half — a namespace
    move re-keys a symbol without touching an instruction. This catches the CFG
    half: an edit to a hot function's shape makes its counters unmatchable, the
    compiler discards them under -fprofile-use, and the function is demoted with
    nothing saying so. The buildCellInstances orphaning was two such commits and
    1,014,423,075 counters, found weeks later; per branch the number is small
    and attributable.
    """
    profile = os.path.join(root, arguments.profile)
    if not os.path.isfile(profile):
        return gate_report.refused(
            GATE, f"no profile at {arguments.profile}", location=arguments.profile)

    both, new = changed_sources(root, arguments.changed_from)
    if not both and not new:
        return gate_report.ok(
            GATE, f"no portable-core translation unit changed against "
            f"{arguments.changed_from}; nothing to compare")

    cxx = os.environ.get("CXX") or shutil.which("c++") or "c++"
    standDown = stand_down(cxx, profile)
    if standDown is not None:
        return standDown
    committedProfile = profile
    profile = profile if arguments.plant_relative_keying else consumer_profile(profile, root)
    keyingFailure = prove_keying(root, cxx, profile, arguments.optimisation)
    if keyingFailure is not None:
        return keyingFailure
    newMass, newCompiled, newFailed, newUnparsed = massBySymbol(
        root, cxx, profile, both, arguments.optimisation, arguments.jobs)

    with BaselineWorktree(root, arguments.changed_from) as baseline:
        baseProfile = os.path.join(baseline, arguments.profile)
        # The base tree's own profile when it has one, else the current one:
        # the comparison needs the same counters on both sides, and the tracked
        # profile is the same blob on both sides of an ordinary branch.
        measureProfile = consumer_profile(baseProfile if os.path.isfile(baseProfile) else committedProfile, baseline)
        baseMass, baseCompiled, baseFailed, baseUnparsed = massBySymbol(
            baseline, cxx, measureProfile, both, arguments.optimisation, arguments.jobs)

    if both and (newCompiled == 0 or baseCompiled == 0):
        print(f"check-pgo-applies: REFUSED — {newCompiled} of {len(both)} changed unit(s) "
              f"compiled here, {baseCompiled} at the base; a zero from an empty side is "
              "not a delta", file=sys.stderr)
        for relative in (newFailed + baseFailed)[:5]:
            gate_report.finding(relative, GATE, "translation unit did not compile for PGO comparison")
        return gate_report.refused(
            GATE, f"{newCompiled} of {len(both)} changed units compiled here and "
            f"{baseCompiled} at the base")
    if newUnparsed or baseUnparsed:
        return gate_report.refused(
            GATE, f"{newUnparsed + baseUnparsed} discard diagnostics could not be parsed")

    grown = []
    for symbol, mass in sorted(newMass.items(), key=lambda item: item[1], reverse=True):
        delta = mass - baseMass.get(symbol, 0)
        if delta > 0:
            grown.append((symbol, delta, mass))
    newTotal = sum(delta for _, delta, _ in grown)

    print(f"check-pgo-applies: {len(both)} changed unit(s) compared against "
          f"{arguments.changed_from}'s merge base"
          + (f", {len(new)} new unit(s) not comparable" if new else ""))
    for unit in new:
        print(f"    new: {unit} (a new function has no counters to discard; its "
              "coverage arrives at the next retrain)")
    behind = commitsBehind(root, arguments.changed_from)
    print(f"  {len(grown)} function(s) newly detached, {newTotal} counter(s) of new "
          "discard mass")
    if behind >= staleBranchCommits:
        print(f"  NOTE: the baseline is {behind} commits behind {arguments.changed_from}. "
              "The mass above is measured against that fork point, so some of it is "
              "drift this branch did not cause. Rebase and re-run before reading it "
              "as a verdict on the change.")
    for symbol, delta, mass in grown[:arguments.top]:
        print(f"    {delta:>14}  {symbol} (now {mass})")

    if arguments.max_new_discarded is not None and newTotal > arguments.max_new_discarded:
        gate_report.finding(
            arguments.profile, GATE,
            f"{newTotal} newly discarded counters exceed budget {arguments.max_new_discarded}")
        if behind >= staleBranchCommits:
            print(f"  Before treating this as the branch's doing: the baseline is "
                  f"{behind} commits behind {arguments.changed_from}, and this mass is "
                  "measured against that fork point. Rebase and re-run — a stale "
                  "branch reads a mass its diff did not produce, and that reading has "
                  "cost a checkout a retrain it did not need.", file=sys.stderr)
        print("  If it survives a rebase, the branch detaches a hot function from the "
              "profile: sequence a retrain after it lands, or raise the budget with "
              "the number in the commit.", file=sys.stderr)
        return gate_report.failed(
            GATE, f"{newTotal} newly discarded counters exceed budget "
            f"{arguments.max_new_discarded}")
    return gate_report.ok(
        GATE, f"{newTotal} newly discarded counters"
        + (f" within budget {arguments.max_new_discarded}"
           if arguments.max_new_discarded is not None else " measured")
        + f" against {arguments.changed_from}")


def discarded_records(diagnostics: str) -> tuple[list[tuple[str, int]], int]:
    """Returns one unit's (symbol, counters discarded) pairs and its unparsed line count.

    - Returns: the records read, and how many lines announced a discard the
      pattern could not read. A nonzero second element means the parser and the
      compiler disagree about the format, which is reported rather than rounded
      down to no discards.
    """
    records: list[tuple[str, int]] = []
    unparsed = 0
    for line in diagnostics.splitlines():
        discard = DISCARD.search(line)
        if discard:
            records.append((discard.group("symbol"), int(discard.group("mass"))))
        elif MENTIONS_DISCARD.search(line):
            unparsed += 1
    return records, unparsed


def default_consumer_profile(root: str, compiler: str) -> str:
    """Selects the compiler target's profile, preserving relative worktree paths."""
    profile = subprocess.check_output(
        ["bash", "-c", '. "$1"; ubiquitty_pgo_profile "$2" "$3"', "pgo-profile",
         os.path.join(root, "Scripts", "pgo-flags.sh"), root, compiler],
        text=True).strip()
    # Diff-scoped checks resolve an in-tree profile in each worktree. Keeping
    # the current root here would make the base read the candidate's counters.
    prefix = os.path.join(root, "")
    return profile[len(prefix):] if profile.startswith(prefix) else profile


def main() -> int:
    root = repository_root()
    parser = argparse.ArgumentParser(description="Report how much of the PGO profile still applies.")
    parser.add_argument("--profile", default=None,
                        help="profile to apply, relative to the repository root; defaults to the compiler target's profile")
    # The machine's compile budget, as every build entry point reads it: the
    # probe is a compile of every portable-core unit, and the core count alone
    # ignores both UBQ_BUILD_JOBS and the memory each compiler holds.
    parser.add_argument("--jobs", type=int, default=build_jobs.build_jobs(),
                        help="compiles to run at once; defaults to Scripts/build_jobs.py's count, which UBQ_BUILD_JOBS sets")
    # The probe must run in the training regime's optimisation level or the
    # verdict is about a build nothing ships: pgo-train instruments at -O3
    # (INSTRUMENT_FLAGS), and a function whose CFG hash is O-level-sensitive
    # reads as wholly discarded at -O2 while the -O3 shipping build keeps every
    # counter. ThinLTO is absent here because the probe compiles single TUs;
    # the O-level is the half of the regime a per-TU probe can honour.
    parser.add_argument("--optimisation", default="-O3",
                        help="optimisation level for the probe compile")
    parser.add_argument("--max-discarded", type=int, default=None,
                        help="fail when the discarded total exceeds this; reports only when unset")
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--changed-from", default=None, metavar="REF",
                        help="diff-scoped mode: compare the changed portable-core "
                             "units against REF's merge base and report only the "
                             "discard mass this branch introduces")
    parser.add_argument("--max-new-discarded", type=int, default=None,
                        help="with --changed-from: fail when the NEW discard mass "
                             "exceeds this; reports only when unset")
    parser.add_argument("--plant-relative-keying", action="store_true",
                        help="probe with the committed, relative-keyed profile as a consumer "
                             "that skipped Scripts/pgo_localize.py would; the keying proof "
                             "must then fail")
    parser.add_argument("--recorded-budget", action="store_true",
                        help="compare the discarded total with the drift budget pgo-train.sh "
                             "recorded beside the profile; a profile without one stands down, "
                             "and one recorded for another training run is refused")
    parser.add_argument("--record-drift-budget", action="store_true",
                        help="after a passing run, write the discarded total plus the "
                             "allowance beside the profile as its drift budget (pgo-train.sh)")
    parser.add_argument("--print-drift-allowance", action="store_true",
                        help="print the allowance a retrain records above its baseline")
    parser.add_argument("--self-test", action="store_true",
                        help="prove the budget arithmetic, the sidecar handling and the "
                             "diagnostic parser against planted cases; compiles nothing")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test(root)
    if arguments.print_drift_allowance:
        print(DRIFT_ALLOWANCE)
        return 0
    import pgo_toolchain
    if pgo_toolchain.needs_image():
        ROOTED.cleanup()
        pgo_toolchain.enter_image(["python3", os.path.abspath(__file__), *sys.argv[1:]])
    pgo_toolchain.select_image_compiler()
    if arguments.profile is None:
        compiler = os.environ.get("CXX") or shutil.which("c++") or "c++"
        arguments.profile = default_consumer_profile(root, compiler)

    compiler = os.environ.get("CXX") or "c++"
    try:
        reason = pgo_toolchain.refusal(os.path.join(root, arguments.profile), compiler)
    except (OSError, subprocess.CalledProcessError) as error:
        reason = str(error)
    if reason:
        return gate_report.refused(GATE, reason)

    if arguments.changed_from is not None:
        return changed_from(root, arguments)

    profile = os.path.join(root, arguments.profile)
    if not os.path.isfile(profile):
        print("  Nothing was measured. A missing profile is not a healthy one.", file=sys.stderr)
        return gate_report.refused(
            GATE, f"no profile at {arguments.profile}", location=arguments.profile)
    committed = profile

    # The sidecar is read before anything compiles: a profile with no budget
    # beside it has nothing to be compared with, and saying so costs no build.
    recorded, standingDetail = None, ""
    if arguments.recorded_budget:
        standing, recorded, standingDetail = budget_standing(committed)
        if standing == ABSENT:
            if arguments.max_discarded is not None:
                return gate_report.refused(
                    GATE, f"{standingDetail}; an explicit discard limit requires a measurement")
            print(f"{GATE}: SKIPPED — {standingDetail}; Scripts/pgo-train.sh records one "
                  "at the next retrain, and the total-drift bound holds from then")
            return 0
        if standing != CURRENT:
            print("  A budget this cannot read, or one recorded for another training run, "
                  "is not a zero budget.", file=sys.stderr)
            print("  Retrain: Scripts/pgo-retrain-check.sh --retrain writes the profile and "
                  "its budget together.", file=sys.stderr)
            return gate_report.refused(
                GATE, standingDetail,
                location=os.path.relpath(sidecar(committed, BUDGET_SUFFIX), root))
    if arguments.record_drift_budget and trained_tree(committed) is None:
        print("  The budget is keyed to the Source/ tree the profile describes, and nothing "
              "names one.", file=sys.stderr)
        return gate_report.refused(
            GATE, f"no {TREE_SUFFIX} sidecar to key the drift budget to",
            location=os.path.relpath(sidecar(committed, TREE_SUFFIX), root))

    cxx = os.environ.get("CXX") or shutil.which("c++") or "c++"
    standDown = stand_down(cxx, profile)
    if standDown is not None:
        return standDown
    profile = profile if arguments.plant_relative_keying else consumer_profile(profile, root)
    keyingFailure = prove_keying(root, cxx, profile, arguments.optimisation)
    if keyingFailure is not None:
        return keyingFailure
    sources = source_list(root)
    if not sources:
        return gate_report.refused(
            GATE, "the portable-core source list is empty",
            location="Scripts/gen-portable-core-sources.sh")

    compiled = 0
    unparsed = 0
    failed: list[str] = []
    records: list[tuple[str, str, int]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=arguments.jobs) as pool:
        futures = [pool.submit(compile_one, root, cxx, profile, rel, arguments.optimisation)
                   for rel in sources]
        for future in concurrent.futures.as_completed(futures):
            relative, ok, diagnostics = future.result()
            if not ok:
                failed.append(relative)
                continue
            compiled += 1
            unit_records, unit_unparsed = discarded_records(diagnostics)
            unparsed += unit_unparsed
            for symbol, mass in unit_records:
                records.append((relative, symbol, mass))

    identity, standing = profile_identity(root, os.path.join(root, arguments.profile))

    # A run that compiled nothing cannot distinguish a profile that fits from a
    # probe that never ran, and those print the same zero.
    if compiled == 0:
        for relative in failed[:5]:
            gate_report.finding(relative, GATE, "translation unit did not compile for PGO probing")
        print("  A zero discarded total from an empty run says nothing about the profile.", file=sys.stderr)
        return gate_report.refused(
            GATE, f"0 of {len(sources)} portable-core translation units compiled")

    # A line that announced a discard the pattern could not read means the
    # compiler's format has moved. Reporting the parsed subset as the total
    # would understate it by exactly the part this parser stopped understanding.
    if unparsed:
        print("  The diagnostic's format has moved; the totals below it would be a subset.", file=sys.stderr)
        return gate_report.refused(
            GATE, f"{unparsed} discard diagnostics could not be parsed")

    total = sum(mass for _, _, mass in records)
    records.sort(key=lambda item: item[2], reverse=True)

    print(f"check-pgo-applies: profile {arguments.profile} ({identity}, {standing})")
    print(f"  {compiled} of {len(sources)} translation unit(s) compiled"
          + (f", {len(failed)} did not" if failed else ""))
    print(f"  {len(records)} function(s) lost their counters, {total} counter(s) discarded")
    for relative, symbol, mass in records[:arguments.top]:
        print(f"    {mass:>14}  {symbol}")
        print(f"    {'':>14}  {relative}")

    if arguments.max_discarded is not None and total > arguments.max_discarded:
        for relative, symbol, mass in records[:arguments.top]:
            gate_report.finding(
                relative, GATE,
                f"{symbol} discarded {mass} counters from the PGO profile")
        print("  Retrain (Scripts/pgo-train.sh) or raise the budget deliberately.", file=sys.stderr)
        return gate_report.failed(
            GATE, f"{total} discarded counters exceed budget {arguments.max_discarded}")

    what = f"{total} discarded counters"
    if arguments.max_discarded is not None:
        what += f" within budget {arguments.max_discarded}"
    if recorded is not None:
        bound = recorded["drift_budget"]
        baseline = recorded["baseline_counters"]
        verdict, share = judge_total(total, baseline, bound)
        print(f"  drift budget {bound}, {standingDetail}: baseline {baseline} counter(s) in "
              f"{recorded['baseline_functions']} function(s) measured at Source/ tree "
              f"{recorded['baseline_tree']}, allowance {bound - baseline}; {share}% of the "
              "allowance used")
        if verdict == EXCEEDED:
            for relative, symbol, mass in records[:arguments.top]:
                gate_report.finding(
                    relative, GATE,
                    f"{symbol} discarded {mass} counters from the PGO profile")
            print("  The profile has drifted off this tree by more than its budget: every "
                  "function above compiles cold, and the per-push bound reads only the "
                  "units a push changed, so it could not add this up.", file=sys.stderr)
            print("  Retrain (Scripts/pgo-retrain-check.sh --retrain leaves the profile and "
                  "its budget ready on a branch), or raise DRIFT_ALLOWANCE in this file "
                  "with the number in the commit.", file=sys.stderr)
            return gate_report.failed(
                GATE, f"{total} discarded counters exceed the recorded drift budget {bound}")
        if verdict == WARNING:
            # Twice, for two readers: the finding is what CI annotates, and the
            # warning line is what the pre-push runner repeats beside the pass.
            gate_report.finding(
                arguments.profile, GATE,
                f"{total} discarded counters use {share}% of the drift allowance (budget "
                f"{bound}); retrain before a push is refused", level="warning")
            print(f"{GATE}: WARNING — {total} discarded counters use {share}% of the drift "
                  f"allowance (budget {bound}); retrain (Scripts/pgo-retrain-check.sh "
                  "--retrain) before a push is refused")
        what += f" within the recorded drift budget {bound} ({share}% of the allowance used)"
    if arguments.max_discarded is None and recorded is None:
        what += " measured without a gating budget"

    if arguments.record_drift_budget:
        path = sidecar(committed, BUDGET_SUFFIX)
        bound = total + DRIFT_ALLOWANCE
        write_drift_budget(path, trained_tree(committed) or "", source_tree(root),
                           len(records), total, bound)
        print(f"  recorded drift budget {bound} in {os.path.relpath(path, root)}: baseline "
              f"{total} counter(s) in {len(records)} function(s), allowance {DRIFT_ALLOWANCE}")
        what += f"; drift budget {bound} recorded"
    return gate_report.ok(GATE, what)


def self_test(root: str) -> int:
    """Proves the budget arithmetic and the sidecar handling against planted cases.

    Nothing here compiles. The compiler's half of the verdict is what a real
    run measures; what this holds is everything around it: the allowance
    tracks the per-push figure in pre-push-gates.sh, a sidecar round-trips, a
    sidecar this cannot read or one recorded for another tree is told apart
    from a budget, the verdicts fall where the percentages say, and a
    diagnostic line parses while a moved format is counted rather than dropped.

    - Returns: 0 when every arm behaves as stated, 1 otherwise.
    """
    failures = []

    # The pre-push gate files are the ones that pass the figure: the family that
    # runs this gate is ubiquitty-core's, beside the file that lists the tiers.
    gateText = ""
    for gates in sorted(glob.glob(os.path.join(root, "Scripts", "pre-push-gates*.sh"))):
        with open(gates, encoding="utf-8") as stream:
            gateText += stream.read()
    perPush = re.search(r"--max-new-discarded\s+(\d+)", gateText)
    if perPush is None:
        failures.append("Scripts/pre-push-gates*.sh pass no --max-new-discarded figure for the "
                        "allowance to track")
    elif int(perPush.group(1)) != PER_PUSH_BUDGET:
        failures.append(f"PER_PUSH_BUDGET is {PER_PUSH_BUDGET} and Scripts/pre-push-gates*.sh pass "
                        f"{perPush.group(1)}; the allowance is ten pushes at the wrong bound")
    if DRIFT_ALLOWANCE != PER_PUSH_BUDGET * DRIFT_ALLOWANCE_PUSHES:
        failures.append("DRIFT_ALLOWANCE is not the per-push bound times the push count")

    with tempfile.TemporaryDirectory() as scratch:
        profile = os.path.join(scratch, "portable-core.profdata")
        open(profile, "wb").close()
        tree = sidecar(profile, TREE_SUFFIX)
        budget = sidecar(profile, BUDGET_SUFFIX)
        minted = "f6b2064e27b056aac86bc508ff5c1ad1e757619b"
        if budget != os.path.join(scratch, "portable-core" + BUDGET_SUFFIX):
            failures.append(f"the budget sidecar of {profile} is {budget}")

        standing, _, _ = budget_standing(profile)
        if standing != ABSENT:
            failures.append(f"a profile with no sidecar stands {standing}, not absent")

        with open(tree, "w", encoding="utf-8") as stream:
            stream.write(minted + "\n")
        write_drift_budget(budget, minted, minted, 0, 0, DRIFT_ALLOWANCE)
        standing, read, _ = budget_standing(profile)
        expected = {"trained_tree": minted, "baseline_tree": minted, "baseline_functions": 0,
                    "baseline_counters": 0, "drift_budget": DRIFT_ALLOWANCE}
        if standing != CURRENT or read != expected:
            failures.append(f"a sidecar written for the profile's tree reads back {standing} {read}")

        with open(tree, "w", encoding="utf-8") as stream:
            stream.write("0" * 40 + "\n")
        standing, _, _ = budget_standing(profile)
        if standing != STALE:
            failures.append(f"a sidecar recorded for another Source/ tree stands {standing}, not stale")

        os.remove(tree)
        standing, _, _ = budget_standing(profile)
        if standing != STALE:
            failures.append(f"a budget beside no {TREE_SUFFIX} sidecar stands {standing}, not stale")

        with open(budget, "w", encoding="utf-8") as stream:
            stream.write(f"trained_tree {minted}\nbaseline_tree {minted}\nbaseline_functions 0\n"
                         "baseline_counters 0\ndrift_budget lots\n")
        standing, _, _ = budget_standing(profile)
        if standing != MALFORMED:
            failures.append(f"a budget that is not a count stands {standing}, not malformed")

    # Against a baseline of zero, as a retrain records, and against one of a
    # million, as a budget recorded for a profile already in use might: the
    # verdicts turn at the same points of the allowance either way.
    for baseline in (0, 1_000_000):
        bound = baseline + DRIFT_ALLOWANCE
        warningEdge = baseline + (DRIFT_ALLOWANCE * DRIFT_WARNING_PERCENT) // 100
        for total, expected in ((0, WITHIN), (baseline, WITHIN), (warningEdge - 1, WITHIN),
                                (warningEdge, WARNING), (bound, WARNING), (bound + 1, EXCEEDED)):
            verdict, _ = judge_total(total, baseline, bound)
            if verdict != expected:
                failures.append(f"{total} between {baseline} and {bound} is judged {verdict}, "
                                f"not {expected}")
        _, share = judge_total(warningEdge, baseline, bound)
        if share != DRIFT_WARNING_PERCENT:
            failures.append(f"the warning edge above {baseline} reads as {share}% rather than "
                            f"{DRIFT_WARNING_PERCENT}%")
        _, share = judge_total(baseline, baseline, bound)
        if share != 0:
            failures.append(f"a total at the baseline {baseline} reads as {share}% used")

    # The compiler probe, against stand-ins that answer it the way the compilers
    # they model do: GCC, a clang older than the profile, and one that reads it.
    with tempfile.TemporaryDirectory() as scratch:
        profile = os.path.join(scratch, "portable-core.profdata")
        open(profile, "wb").close()
        compilers = {
            "gcc": 'case " $* " in *" -dM "*) echo "#define __GNUC__ 14" ;; esac\n',
            "oldclang": 'case " $* " in *" -dM "*) echo "#define __clang__ 1"; exit 0 ;; esac\n'
                        'echo "error: unsupported instrumentation profile format version" >&2; exit 1\n',
            "clang": 'case " $* " in *" -dM "*) echo "#define __clang__ 1" ;; esac\n',
        }
        for name, body in compilers.items():
            path = os.path.join(scratch, name)
            with open(path, "w", encoding="utf-8") as stream:
                stream.write("#!/bin/sh\n" + body)
            os.chmod(path, 0o755)
        for name, expected in (("gcc", "does not read LLVM's profile format"),
                               ("oldclang", "unsupported instrumentation profile format version"),
                               ("clang", None)):
            refusal = compiler_refusal(os.path.join(scratch, name), profile)
            if (refusal is None) != (expected is None) or (expected and expected not in refusal):
                failures.append(f"the compiler probe answered {refusal!r} for the {name} stand-in")

    symbol = "_ZN9Ubiquitty2VT8csiH_sgrERNS_8TerminalEc"
    parsed, unparsed = discarded_records(
        f"warning: Source/Core/VT/CSI.cpp: function control flow change detected (hash mismatch) "
        f"{symbol} Hash = 1058 up to 30099366 count discarded [-Wbackend-plugin]\n"
        "warning: Source/Core/VT/CSI.cpp: 12 count discarded in a shape this parser does not know\n")
    if parsed != [(symbol, 30099366)] or unparsed != 1:
        failures.append(f"the diagnostic parser read {parsed} with {unparsed} unparsed")

    for failure in failures:
        print(f"{GATE} --self-test: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"{GATE} --self-test: ok (the allowance is {DRIFT_ALLOWANCE_PUSHES} pushes at "
          "pre-push-gates.sh's bound; a budget sidecar round-trips; an absent, a stale and "
          f"a malformed sidecar are told apart; the verdicts turn at {DRIFT_WARNING_PERCENT}% "
          "of the allowance and at the budget, above a zero and a nonzero baseline; GCC and a "
          "clang older than the profile are told from one that reads it; a moved "
          "diagnostic format counts as unparsed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
