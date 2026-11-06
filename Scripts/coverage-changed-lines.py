#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""coverage-changed-lines.py — every changed line of product code is covered.

Given an llvm-cov lcov file (from `Scripts/run-coverage.sh`, which emits
BuildHostTests/coverage/coverage.lcov) and a git diff range, fail if any
changed line of instrumented product code is uncovered. A genuinely-untestable
line is spared only by an explicit reviewer-audited marker on that exact line:

    // coverage-exempt(<gate>): <specific reason>

<gate> names the reviewer identity that sanctioned it; the marker + reason are
logged so the exemption is auditable, never a silent skip. The tool is
deterministic and self-contained: same tree + same lcov => same verdict.

Usage:
    coverage-changed-lines.py --lcov <path> --base <ref> [--head <ref>]
                              [--root <dir>] [--json <path>]

Exit 0 when every changed instrumented line is covered or exempt; exit 1 with a
report otherwise; exit 2 on a usage/environment error.
"""

import argparse
import json
import os
import re
import subprocess
import sys

# The exemption marker. <gate> is a reviewer identity, <reason> is free text;
# both are required so the ledger never records a bare skip.
EXEMPT_RE = re.compile(r"//\s*coverage-exempt\(([^)]+)\):\s*(\S.*\S|\S)\s*$")

# A changed line is gated only for compilable translation units — a .cpp/.mm/.c
# without coverage is a real gap; headers carry inline/template lines llvm-cov
# attributes to the including TU, so a header absent from the lcov is not by
# itself a gap.
TU_SUFFIXES = (".cpp", ".mm", ".c", ".cc", ".cxx")

# Paths run-tests.sh's cov_ignore drops from the coverage denominator — vendored
# ThirdParty, test code, compat shims, host-test build artifacts. A changed line
# under any of these is not our coverage responsibility, so the gate skips it
# (mirrors -ignore-filename-regex; without this a changed vendored .cc, absent
# from the lcov, would wrongly count as an uninstrumented gap).
EXCLUDE_RE = re.compile(r"(^|/)(ThirdParty|Tests|BuildHostTests)/|/compat/")

# The frontend implementation trees the host coverage build never compiles —
# run-coverage.sh names this boundary itself ("63 frontend implementation
# file(s) … are absent from the denominator"). Consulted only for a changed
# TU the lcov does not know: a frontend TU that a host test links (the
# typed pasteboard bridge) is in the lcov and gated line by line like any
# Core file, and a Core TU absent from the lcov still fails — an untested
# Core file is exactly what this gate exists to catch.
FRONTEND_RE = re.compile(r"(^|/)Source/Platform/(macOS|Linux|Web|Wasm|Emacs|Wscons)/")


def run(cmd, root):
    result = subprocess.run(
        cmd, cwd=root, capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        sys.stderr.write(f"error: {' '.join(cmd)}\n{result.stderr}")
        sys.exit(2)
    return result.stdout


def changed_lines(base, head, root):
    """Map each changed file -> the set of NEW-side line numbers added/modified.

    --unified=0 keeps only the touched lines; rename-aware via --find-renames.
    Deletions never need coverage, so only the '+' hunk lines count.
    """
    diff_range = f"{base}..{head}" if head else base
    out = run(
        ["git", "diff", "--unified=0", "--find-renames", "--no-color", diff_range],
        root,
    )
    changed = {}
    current = None
    new_line = 0
    for line in out.splitlines():
        if line.startswith("+++ "):
            path = line[4:].strip()
            current = None if path == "/dev/null" else path[2:]  # strip 'b/'
        elif line.startswith("@@"):
            # @@ -old,cnt +new,cnt @@  — new-side start is the '+' group.
            match = re.search(r"\+(\d+)(?:,(\d+))?", line)
            new_line = int(match.group(1)) if match else 0
        elif current is not None and line.startswith("+") and not line.startswith("+++"):
            changed.setdefault(current, set()).add(new_line)
            new_line += 1
    return changed


def parse_lcov(path):
    """Parse lcov into {repo_relative_file: {line: hit_count}}.

    llvm-cov emits SF: as an absolute or build-relative path; the caller
    normalizes against --root so the keys match git's repo-relative paths.
    """
    coverage = {}
    current = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("SF:"):
                current = line[3:].strip()
                coverage.setdefault(current, {})
            elif line.startswith("DA:") and current is not None:
                number, _, count = line[3:].strip().partition(",")
                coverage[current][int(number)] = int(count or 0)
            elif line.startswith("end_of_record"):
                current = None
    return coverage


def normalize(coverage, root):
    root = os.path.realpath(root)
    normalized = {}
    for path, lines in coverage.items():
        absolute = os.path.realpath(os.path.join(root, path))
        if absolute.startswith(root + os.sep):
            normalized[os.path.relpath(absolute, root)] = lines
    return normalized


def source_line(root, path, number):
    try:
        with open(os.path.join(root, path), encoding="utf-8", errors="replace") as handle:
            for index, text in enumerate(handle, start=1):
                if index == number:
                    return text
    except OSError:
        return ""
    return ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lcov", required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", default="")
    parser.add_argument("--root", default=".")
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    if not os.path.isfile(args.lcov):
        sys.stderr.write(f"error: no lcov file at {args.lcov}\n")
        sys.exit(2)

    root = os.path.realpath(args.root)
    coverage = normalize(parse_lcov(args.lcov), root)
    changed = changed_lines(args.base, args.head, root)

    uncovered = []   # covered-line-count 0 in an instrumented file
    exempt = []      # spared by an audited marker
    uninstrumented = []  # a changed .cpp/.mm with no coverage at all
    frontend_only = []  # absent from the lcov and outside the host build's denominator

    for path in sorted(changed):
        if EXCLUDE_RE.search(path):
            continue  # vendored/test/compat — not in the coverage denominator
        if not path.endswith(TU_SUFFIXES):
            continue  # header/non-TU: attributed to its including TU, not gated here
        file_cov = coverage.get(path)
        if file_cov is None:
            if FRONTEND_RE.search(path):
                frontend_only.append(path)
            else:
                uninstrumented.append(path)
            continue
        for number in sorted(changed[path]):
            hits = file_cov.get(number)
            if hits is None:
                continue  # not a coverable line (brace/decl/blank per llvm-cov)
            if hits > 0:
                continue
            marker = EXEMPT_RE.search(source_line(root, path, number))
            if marker:
                exempt.append(
                    {"file": path, "line": number, "gate": marker.group(1).strip(),
                     "reason": marker.group(2).strip()}
                )
            else:
                uncovered.append({"file": path, "line": number})

    report = {
        "base": args.base,
        "head": args.head or "WORKTREE",
        "uncovered": uncovered,
        "exempt": exempt,
        "uninstrumented_changed_tus": uninstrumented,
        "frontend_only_changed_tus": frontend_only,
    }
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)

    for entry in exempt:
        print(f"EXEMPT {entry['file']}:{entry['line']}  ({entry['gate']}: {entry['reason']})")
    for path in frontend_only:
        print(f"OUT-OF-DENOMINATOR (frontend TU the host coverage build never compiles): {path}")
    for path in uninstrumented:
        print(f"NO-COVERAGE (changed TU absent from the instrumented run): {path}")
    for entry in uncovered:
        print(f"UNCOVERED {entry['file']}:{entry['line']}")

    if uncovered or uninstrumented:
        print(
            f"\ncoverage gate FAILED: {len(uncovered)} uncovered changed line(s), "
            f"{len(uninstrumented)} changed TU(s) with no coverage."
        )
        sys.exit(1)
    print(
        f"coverage gate OK ({len(exempt)} audited exemption(s), "
        f"{len(frontend_only)} frontend TU(s) outside the denominator)."
    )


if __name__ == "__main__":
    main()
