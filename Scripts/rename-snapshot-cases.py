#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""rename-snapshot-cases.py — rename focused snapshot cases, their goldens, and every mention.

    Scripts/rename-snapshot-cases.py old_name=new_name...
    Scripts/rename-snapshot-cases.py --strip-prefix macos_ macos_twin=twin_named_for_what_differs
    Scripts/rename-snapshot-cases.py ... --dry-run

A case's name is more than a field of `Tests/Snapshots/cases.json`. It is the
file name of the case's golden on every tier, the stem of the golden of each
later frame (`<name>_<suffix>`), the middle of every `case:` identifier in the
waiver files, and a word in any prose that names the case. A rename moves all of
them together: `git mv` for each golden, then a whole-word rewrite of every
tracked text file for each old name and each old frame stem.

`--strip-prefix` names every case starting with the prefix by what follows it.
A pair given on the command line overrides that name. A stripped name that
already belongs to a case is refused, and the refusal lists every case that needs
a name of its own.

The catalogue is checked before and after with `Tests/Snapshots/run.py
--check-catalog`. That check refuses a missing golden, an orphaned one, and a
waiver line that names no case. A rename starts only from a catalogue that
passes it, and the script fails if the catalogue does not pass at the end. The
script also refuses each of these:

- a new name that is not a valid case name, or that is already a case;
- a new name that two old names share, or that is itself being renamed;
- a golden whose new path already exists;
- an approved golden, because `approved.tsv` names its path;
- an old name that is also a configuration file's stem or a showcase golden's
  tag, because its mentions could then mean that file as well as the case.

The working tree must be clean, so the rename is the only change in the commit
that records it. `--dry-run` prints the plan and changes nothing. The script
uses only the Python standard library.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SNAPSHOTS = ROOT / "Tests" / "Snapshots"
sys.path.insert(0, str(SNAPSHOTS))
import run as catalog  # noqa: E402 — the path above is what makes it importable

# One identifier character on either side of a name makes it part of a longer
# identifier: `sample_case` inside `sample_case_wide` or `test_sample_case` is
# not a mention of a case named `sample_case`.
IDENTIFIER_CHARACTER = "A-Za-z0-9_"


def git(*arguments, capture=True):
    """Runs git in the repository and returns its standard output.

    - Parameters:
      - arguments: The git arguments.
      - capture: Whether to capture standard output rather than pass it through.
    - Returns: Standard output as bytes, or empty bytes when not captured.
    """
    result = subprocess.run(["git", "-C", str(ROOT), *arguments],
                            capture_output=capture, check=True)
    return result.stdout if capture else b""


def catalogueVerdict(when):
    """Runs the catalogue check and reports whether it passed.

    - Parameter when: The word printed before the check's output: "before" or "after".
    - Returns: Whether `run.py --check-catalog` exited zero.
    """
    result = subprocess.run([sys.executable, str(SNAPSHOTS / "run.py"), "--check-catalog"],
                            capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip().splitlines()
    for line in output[-20:]:
        print(f"catalogue {when}: {line}")
    return result.returncode == 0


def requestedNames(pairs, stripPrefix, caseNames):
    """The new name for each case being renamed.

    - Parameters:
      - pairs: `old=new` arguments, which take precedence over the prefix.
      - stripPrefix: The prefix to strip from every case carrying it, or None.
      - caseNames: Every case name in the catalogue.
    - Returns: A dict from old name to new name, and a list of refusals.
    """
    renames = {}
    refusals = []
    explicit = {}
    for pair in pairs:
        old, separator, new = pair.partition("=")
        if not separator or not old or not new:
            refusals.append(f"{pair}: write a rename as old_name=new_name")
            continue
        explicit[old] = new
    if stripPrefix:
        for name in caseNames:
            if name.startswith(stripPrefix) and name not in explicit:
                renames[name] = name[len(stripPrefix):]
    renames.update(explicit)
    return renames, refusals


def validatedRenames(renames, caseNames):
    """Refusals for names the catalogue cannot hold after the rename.

    - Parameters:
      - renames: Old name to new name.
      - caseNames: Every case name in the catalogue.
    - Returns: A list of refusals, empty when every new name is usable.
    """
    refusals = []
    staying = set(caseNames) - set(renames)
    claimedBy = {}
    for old, new in sorted(renames.items()):
        if old not in caseNames:
            refusals.append(f"{old}: no case has this name")
        if not catalog.SAFE_NAME.fullmatch(new):
            refusals.append(f"{old} -> {new}: a case name is lowercase snake_case")
        if new in staying:
            refusals.append(f"{old} -> {new}: {new} is already a case; name {old} for what "
                            f"differs from it with old_name=new_name")
        if new in renames:
            refusals.append(f"{old} -> {new}: {new} is itself being renamed; rename in two runs")
        claimedBy.setdefault(new, []).append(old)
    for new, olds in sorted(claimedBy.items()):
        if len(olds) > 1:
            refusals.append(f"{', '.join(olds)} would all become {new}")
    return refusals


def goldenMoves(renames):
    """Every golden path the renamed cases claim, and where each one goes.

    The paths are the catalogue's own: each regression of a renamed case names
    its golden, frame goldens included, so nothing here derives a path the
    runner would not.

    - Parameter renames: Old name to new name.
    - Returns: A dict from old golden path to new golden path, and a dict from
      old frame stem to new frame stem.
    """
    moves = {}
    frameStems = {}
    for regression in catalog.load_case_regressions():
        name = regression.case["name"]
        if name not in renames or regression.golden is None:
            continue
        stem = regression.golden.stem
        newStem = renames[name] + stem[len(name):]
        moves[regression.golden] = regression.golden.with_name(newStem + regression.golden.suffix)
        if stem != name:
            frameStems[stem] = newStem
    return moves, frameStems


def ambiguousNames(tokens):
    """Old names that also name a configuration file or a showcase golden.

    - Parameter tokens: The old names and frame stems to be rewritten.
    - Returns: A list of refusals.
    """
    others = {path.stem for path in (SNAPSHOTS / "configs").rglob("*.toml")}
    others |= set(catalog.showcase_sequences_by_golden())
    return [f"{token}: also names a configuration file or a showcase golden"
            for token in sorted(set(tokens) & others)]


def approvedGoldens():
    """The repository paths `approved.tsv` lists.

    - Returns: A set of absolute paths.
    """
    paths = set()
    manifest = SNAPSHOTS / "approved.tsv"
    if not manifest.exists():
        return paths
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            paths.add((ROOT / line.split("\t")[0]).resolve())
    return paths


def trackedTextFiles():
    """Every tracked file outside the golden directory that decodes as UTF-8.

    - Returns: A list of (path, text) pairs.
    """
    files = []
    goldenRoot = SNAPSHOTS / "golden"
    for raw in git("ls-files", "-z").split(b"\0"):
        if not raw:
            continue
        path = ROOT / raw.decode("utf-8")
        if goldenRoot in path.parents or not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue
        try:
            files.append((path, data.decode("utf-8")))
        except UnicodeDecodeError:
            continue
    return files


def mentionPattern(tokens):
    """A pattern matching any of the tokens as a whole identifier.

    - Parameter tokens: The names to match.
    - Returns: A compiled pattern whose first group is the matched name.
    """
    alternatives = "|".join(re.escape(token) for token in sorted(tokens, key=len, reverse=True))
    return re.compile(f"(?<![{IDENTIFIER_CHARACTER}])({alternatives})(?![{IDENTIFIER_CHARACTER}])")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("renames", nargs="*", metavar="old_name=new_name",
                        help="a case and the name it takes")
    parser.add_argument("--strip-prefix", metavar="PREFIX",
                        help="rename every case starting with PREFIX to what follows it")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the plan without changing anything")
    arguments = parser.parse_args(argv)
    if not arguments.renames and not arguments.strip_prefix:
        parser.error("name at least one old_name=new_name, or pass --strip-prefix")

    if not arguments.dry_run and git("status", "--porcelain").strip():
        print("rename-snapshot-cases: the working tree has changes; commit or stash them first",
              file=sys.stderr)
        return 2
    if not catalogueVerdict("before"):
        print("rename-snapshot-cases: the catalogue is inconsistent before the rename; "
              "fix that first", file=sys.stderr)
        return 2

    caseNames = [case["name"] for case in json.loads(
        (SNAPSHOTS / "cases.json").read_text(encoding="utf-8"))]
    renames, refusals = requestedNames(arguments.renames, arguments.strip_prefix, caseNames)
    refusals += validatedRenames(renames, caseNames)
    if not renames and not refusals:
        refusals.append("no case matches; nothing to rename")
    moves, frameStems = goldenMoves(renames) if not refusals else ({}, {})
    tokens = {**renames, **frameStems}
    refusals += ambiguousNames(tokens)
    approved = approvedGoldens()
    for old, new in sorted(moves.items()):
        if old.resolve() in approved:
            refusals.append(f"{old.relative_to(ROOT)}: approved; its line in approved.tsv "
                            f"names this path")
        if new.exists():
            refusals.append(f"{new.relative_to(ROOT)}: already exists")
    if len(set(moves.values())) != len(moves):
        refusals.append("two renamed goldens would take one path")
    if refusals:
        for refusal in refusals:
            print(f"refused: {refusal}", file=sys.stderr)
        return 2

    existing = {old: new for old, new in moves.items() if old.exists()}
    pattern = mentionPattern(tokens)
    rewrites = []
    for path, text in trackedTextFiles():
        count = len(pattern.findall(text))
        if count:
            rewrites.append((path, text, count))

    print(f"{len(renames)} case(s), {len(existing)} golden(s), "
          f"{sum(count for _, _, count in rewrites)} mention(s) in {len(rewrites)} file(s)")
    for old, new in sorted(renames.items()):
        print(f"  case    {old} -> {new}")
    for old, new in sorted(existing.items()):
        print(f"  golden  {old.relative_to(ROOT)} -> {new.name}")
    for path, _, count in rewrites:
        print(f"  text    {path.relative_to(ROOT)}: {count}")
    if arguments.dry_run:
        return 0

    for old, new in sorted(existing.items()):
        git("mv", "--", str(old.relative_to(ROOT)), str(new.relative_to(ROOT)), capture=False)
    for path, text, _ in rewrites:
        path.write_bytes(pattern.sub(lambda match: tokens[match.group(1)], text).encode("utf-8"))

    leftovers = [str(path.relative_to(ROOT)) for path, text in trackedTextFiles()
                 if pattern.search(text)]
    if leftovers:
        print(f"rename-snapshot-cases: old names remain in {', '.join(leftovers)}", file=sys.stderr)
        return 1
    if not catalogueVerdict("after"):
        print("rename-snapshot-cases: the catalogue is inconsistent after the rename",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
