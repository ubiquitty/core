#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-bench-harness-reach.py — every bench times and reports through the
shared harness.

A bench outside Ubiquitty::Bench carries none of its guarantees: no
repeat-to-clear-the-clock, no optimiser barrier, no control lane — while sitting
in the same directory and the same report as lanes that do.

  Scripts/check-bench-harness-reach.py

Exit 0 = clean, 1 = a bench outside the harness, or an allowlist entry kept past
its need.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus

GATE = "check-bench-harness-reach"

# Benches that still time and report on their own. Every entry here is a bench
# whose numbers carry none of the harness's guarantees. Shrink it; never extend
# it.
UNMIGRATED = (
    "bench_cellops.cpp",
    "bench_core.cpp",
    "bench_drain.cpp",
    "bench_ingest_corpora.cpp",
    "bench_linestore.cpp",
    "bench_unicode_parse.cpp",
    "bench_utf8.cpp",
)

HARNESS_HEADER = re.compile(r'#include *"(\./)?BenchHarness\.h"')

# The qualifier is OPTIONAL, because `using namespace Ubiquitty::Bench` makes the
# call bare and a pattern anchored on `Bench::` then reports zero for a bench
# calling it three times. That direction is the harmful one: a migrated bench
# reads as unmigrated and the reader is pushed toward an allowlist entry it does
# not need, which is a pre-excused slot for the next real regression.
#
# Kept precise by also requiring the harness header, so a bare `measure(` that
# belongs to something else cannot satisfy this.
TIMES = re.compile(r"\b(Ubiquitty::)?(Bench::)?measure(Paired|Samples)?[ \t]*\(")
REPORTS = re.compile(r"\b(Ubiquitty::)?(Bench::)?emitJSON[ \t]*\(")


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())

    benches = sorted(path for path in gate_corpus.tracked_and_untracked()
                     if path.startswith("Tests/Performance/bench_")
                     and path.endswith(".cpp"))

    failures = 0
    migrated = 0
    unmigratedFound = []

    for path, text in gate_corpus.texts(benches):
        name = os.path.basename(path)
        listed = name in UNMIGRATED

        # Timing through the harness and reporting through it are separate
        # questions: a bench can call measure() and then print its own table,
        # which keeps the repeat and the barrier but loses the control lane.
        times = reports = 0
        if HARNESS_HEADER.search(text):
            times = len(TIMES.findall(text))
            reports = len(REPORTS.findall(text))

        if times and reports:
            if listed:
                print(f"FAIL {name} routes through the harness but is still "
                      f"listed as unmigrated.", file=sys.stderr)
                print("     Remove it from UNMIGRATED in this script — an "
                      "exemption kept past its", file=sys.stderr)
                print("     need excuses the next bench that regresses into "
                      "the same shape.", file=sys.stderr)
                failures += 1
            else:
                migrated += 1
            continue

        unmigratedFound.append(name)
        if not listed:
            print(f"FAIL {name} times or reports outside the shared harness "
                  f"and is not listed.", file=sys.stderr)
            print(f"     times through harness: {times}, reports through "
                  f"harness: {reports}", file=sys.stderr)
            print("     Route it through Ubiquitty::Bench::measure + emitJSON. "
                  "Its numbers otherwise", file=sys.stderr)
            print("     carry no repeat-to-clear-the-clock, no optimiser "
                  "barrier, and no control lane,", file=sys.stderr)
            print("     while sitting in the same directory and the same "
                  "report as lanes that do.", file=sys.stderr)
            failures += 1

    # The lower bound. Without it the list is a budget with slack: a bench could
    # be migrated, its entry left behind, and the next bench to regress would
    # land in a slot that was already excused.
    if len(unmigratedFound) != len(UNMIGRATED):
        print(f"FAIL the allowlist has {len(UNMIGRATED)} entries but "
              f"{len(unmigratedFound)} bench(es) need one.", file=sys.stderr)
        failures += 1

    if failures:
        print(f"{GATE}: {failures} failure(s)", file=sys.stderr)
        return 1

    print(f"{GATE}: ok ({migrated} through the harness, {len(UNMIGRATED)} not "
          f"yet)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
