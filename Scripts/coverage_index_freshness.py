#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Reports structural index drift without building, measuring, or rewriting coverage."""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys

# How far the index's commit may trail HEAD before it is reported stale. Paths
# and test names can all still resolve while the code under them has moved on:
# the index then names the tests that reached that code as it was. At this
# tree's pace this is a few days of work.
MAX_COMMITS_BEHIND = 500


@dataclass(frozen=True)
class Index:
    header: str
    revision: str | None
    paths: frozenset[str]
    entries: frozenset[str]
    problems: tuple[str, ...]


def isLocalPath(path):
    components = path.split("/")
    return (len(components) > 1 and components[0] in ("Source", "Tests")
            and all(component not in ("", ".", "..") for component in components)
            and "\\" not in path and ":" not in path
            and not any(ord(character) < 32 or ord(character) == 127
                        or 0xD800 <= ord(character) <= 0xDFFF for character in path))


def parseIndex(text):
    lines = text.split("\n")
    header = lines[0] if lines else ""
    match = re.fullmatch(r"# ([0-9a-fA-F]{4,64}|unknown), [0-9]+ tests", header)
    problems = [] if match else ["The provenance header is malformed."]
    revision = match[1] if match and match[1] != "unknown" else None
    paths, entries = set(), set()
    for number, line in enumerate(lines[1:], 2):
        if line.startswith("#") or (not line and number == len(lines)):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not fields[1] or not all(
                re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*", entry)
                for entry in fields[1].split(" ")):
            problems.append(f"Row {number} is not a file-to-test mapping.")
            continue
        path, tests = fields
        if not isLocalPath(path):
            problems.append(f"Row {number} has an unsafe indexed path: {ascii(path)}.")
            continue
        if path in paths:
            problems.append(f"Row {number} repeats an indexed path: {ascii(path)}.")
        paths.add(path)
        entries.update(tests.split(" "))
    if not paths:
        problems.append("The index contains no valid file mappings.")
    return Index(header.removeprefix("# "), revision, frozenset(paths),
                 frozenset(entries), tuple(problems))


def indexedFileProblem(root, path):
    # Validate before joining: an index may not inspect a path outside its checkout.
    if not isLocalPath(path):
        return "Unsafe indexed path"
    try:
        resolved = (root / path).resolve()
        if not resolved.is_relative_to(root):
            return "Indexed path resolves outside the checkout"
        if not resolved.is_file():
            return "Indexed file is missing or is not a regular file"
    except (OSError, RuntimeError):
        return "Indexed file cannot be inspected"
    return None


def isCoverageTest(path):
    name = PurePosixPath(path).name
    return (path.startswith("Tests/") and name.startswith(("test_", "fuzz_"))
            and name.endswith((".cpp", ".sh")))


def parseChangedPaths(data):
    if not data:
        return set()
    fields = data.split(b"\0")
    if fields.pop() != b"":
        raise ValueError("Unterminated Git path record")
    paths = set()
    offset = 0
    while offset < len(fields):
        status = fields[offset]
        count = 1 if status == b"A" else 2 if re.fullmatch(rb"R[0-9]{1,3}", status) else 0
        if (not count or len(fields) - offset - 1 < count
                or any(not path for path in fields[offset + 1:offset + count + 1])):
            raise ValueError("Malformed Git change record")
        paths.add(os.fsdecode(fields[offset + count]))
        offset += count + 1
    return paths


def parseUntrackedPaths(data):
    if not data:
        return set()
    fields = data.split(b"\0")
    if fields.pop() != b"" or any(not field for field in fields):
        raise ValueError("Malformed Git untracked path record")
    return {os.fsdecode(field) for field in fields}


def readGitMetadata(root, revision, run):
    def git(*arguments):
        return run(["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C", str(root),
                    *arguments], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)

    problems = []
    base = "HEAD"
    try:
        if revision and git("rev-parse", "--verify", "--quiet", f"{revision}^{{commit}}").returncode == 0:
            base = revision
        else:
            problems.append("The provenance commit is unavailable here; committed test drift is unmeasurable.")
    except OSError:
        problems.append("Git cannot verify the provenance commit.")

    paths = set()
    for arguments, parse, message in (
            (("diff", "--no-ext-diff", "--no-textconv", "--name-status", "-z", "--find-renames",
              "--diff-filter=AR", base, "--", "Tests"), parseChangedPaths,
             "Git cannot list added or renamed test files."),
            (("ls-files", "--others", "--exclude-standard", "-z", "--", "Tests"), parseUntrackedPaths,
             "Git cannot list untracked test files.")):
        try:
            result = git(*arguments)
            if result.returncode:
                problems.append(message)
            else:
                paths.update(parse(result.stdout))
        except (OSError, ValueError):
            problems.append(message)
    return paths, tuple(problems)


def commitsBehind(root, revision, run):
    """Counts the commits HEAD has that the index's commit lacks.

    - Parameters:
      - root: The checkout.
      - revision: The index's provenance commit, already verified, or None.
      - run: `subprocess.run`, or a stand-in.
    - Returns: The count, or None when it cannot be measured here.
    """
    if not revision:
        return None
    try:
        result = run(["git", "--no-optional-locks", "-c", "core.fsmonitor=false", "-C", str(root),
                      "rev-list", "--count", f"{revision}..HEAD"],
                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    except OSError:
        return None
    text = result.stdout.decode("ascii", "replace").strip() if isinstance(result.stdout, bytes) else str(result.stdout).strip()
    return int(text) if result.returncode == 0 and text.isdigit() else None


def assessIndex(index, fileProblem, changedPaths=(), metadataProblems=(), behind=None):
    problems = list(index.problems)
    unchecked = list(metadataProblems)
    if behind is not None and behind > MAX_COMMITS_BEHIND:
        problems.append(f"The index was built {behind} commits behind HEAD, past the {MAX_COMMITS_BEHIND} it may trail.")
    for path in sorted(index.paths):
        problem = fileProblem(path)
        if problem:
            problems.append(f"{problem}: {ascii(path)}.")
    # CTest shards share their binary's entry; Python/tool tests do not emit LLVM profiles.
    named = {re.sub(r"\.[0-9]+$", "", entry) for entry in index.entries}
    for path in sorted(set(changedPaths)):
        if not isLocalPath(path) or not path.startswith("Tests/"):
            problems.append(f"Git reports an unsafe test path: {ascii(path)}.")
        elif path in index.paths:
            continue
        elif isCoverageTest(path):
            if PurePosixPath(path).stem not in named:
                problems.append(f"Added or renamed test is not named by the index: {ascii(path)}.")
        elif PurePosixPath(path).suffix in (".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".swift", ".sh"):
            # Shell stanzas need not share a source basename; only an indexed row proves their attribution.
            unchecked.append(f"Added or renamed test source has no indexed attribution: {ascii(path)}.")
    if not index.revision and not metadataProblems:
        unchecked.append("The provenance commit is unavailable here; committed test drift is unmeasurable.")
    if problems:
        return "stale", tuple(problems) + tuple(unchecked)
    if unchecked:
        return "UNCHECKED", tuple(unchecked)
    return "fresh", ()


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--index", type=Path, required=True)
    options = parser.parse_args(arguments)
    if not options.index.is_file():
        print("coverage-index: absent — run ./Scripts/run-coverage.sh to measure an index", file=sys.stderr)
        return 1
    try:
        text = options.index.read_text(encoding="utf-8")
    except FileNotFoundError:
        print("coverage-index: absent — run ./Scripts/run-coverage.sh to measure an index", file=sys.stderr)
        return 1
    except (OSError, UnicodeError):
        print("coverage-index: UNCHECKED — the existing index cannot be read", file=sys.stderr)
        return 0

    root = options.root.resolve()
    index = parseIndex(text)
    paths, metadataProblems = readGitMetadata(root, index.revision, subprocess.run)
    behind = None if metadataProblems else commitsBehind(root, index.revision, subprocess.run)
    verdict, problems = assessIndex(index, lambda path: indexedFileProblem(root, path), paths, metadataProblems, behind)
    print(f"coverage-index: {options.index}")
    print(f"  built from: {index.header if not index.problems else ascii(index.header)}")
    if behind is not None:
        print(f"  behind:     {behind} commits (at most {MAX_COMMITS_BEHIND})")
    print(f"  files:      {len(index.paths)}")
    if verdict == "fresh":
        print("  verdict:    fresh — indexed files exist; added or renamed tests are named")
        print("              This checks paths and test names, not coverage after source edits.")
    else:
        print(f"  verdict:    {verdict}")
        for problem in problems[:6]:
            print(f"              {problem}")
        if len(problems) > 6:
            print(f"              … and {len(problems) - 6} more finding(s).")
        print("              An unnamed file is unknown, not proof that no test reaches it.")
        print("              ./Scripts/run-coverage.sh builds and runs the instrumented suite")
        print("              to measure a fresh index; --check does not regenerate coverage.")
    # Existing indexes are diagnostic-only; only an absent index changes the exit status.
    return 0


if __name__ == "__main__":
    sys.exit(main())
