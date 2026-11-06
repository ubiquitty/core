#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Refuse a kept translation unit that needs a symbol only a trimmed one defines.

Each frontend takes a subset of portable Core through `ubiquitty_core_subset(…
EXCLUDE …)`, dropping the translation units that shell does not use. Nothing
checks the other side of that cut: a Core commit can add a call from a kept file
into a trimmed one, and the only report is an undefined symbol for whoever builds
that arm next: a function defined in a source two subsets trim and called from a
source both keep leaves main unlinkable on that arm.

`check-core-srcs-complete.py` enforces exactly this partition for the test
harness; this is its equivalent for the frontend subsets.

## Latent, not always live — and the difference is the linker

A reference that survives is not a reference that is safe. A kept source can
call readers in a source the agent trims and the agent still links: ld64
discards code no entry point reaches, so the undefined references go with it.
The same pair on a linker that does not strip is the break above. So this
refuses the reference itself, not the subset of references that happen to be
fatal on one linker.

## Why symbols rather than includes or grep

The question is which definitions a link needs, and only the object files answer
it. The host test build compiles every canonical Core translation unit, so one
`nm` pass over that build gives both halves — what each file defines and what
each references — without configuring a single frontend. That matters for Wscons
in particular, which no Linux or macOS machine can configure at all.

Three bounds:

    a kept TU referencing a trimmed-only symbol -> FAIL, the cut is broken
    a subset naming a file not in the canonical
    source list                                 -> FAIL, a stale name
    no objects, or no subsets parsed            -> REFUSE, an empty scan and a
                                                   clean tree print alike
    --self-test with no object tree at all      -> SKIPPED, naming the build
                                                   that supplies one
    --if-built with no object tree at all       -> SKIPPED, for check-style on
                                                   a machine that builds nothing

What this cannot see, stated so nobody quotes it for more: the symbol tables are
the host build's. A definition or reference that exists only under another
platform's conditional compilation is invisible here, and so is anything a
frontend's own Platform sources define.

The cuts are the EXCLUDE lists of `ubiquitty_core_subset()` and of
`ubiquitty_add_core_ios_library()`, read from each --cmake file (this
checkout's CMakeLists.txt when none is named) and judged against --canonical
(this checkout's Scripts/portable-core-sources.txt when not named).
"""

import argparse
import collections
import re
import subprocess
import os
import sys

import os.path as _p, sys as _s
_s.path.insert(0, _p.dirname(_p.abspath(__file__)))
import build_root
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CANONICAL = REPO / "Scripts" / "portable-core-sources.txt"
CMAKE = REPO / "CMakeLists.txt"
HOST_TESTS = Path(build_root.build_root(str(REPO))) / "BuildHostTests"


def objectTree(sources):
    """Returns the BuildHostTests subtree that best covers `sources`.

    The tree is not always `ctest`. The coverage job builds its own subtree and
    runs the same stanzas over it, so a gate that reads one hardcoded sibling
    reads whatever an earlier run happened to leave there — objects whose
    freshness is accidental, and on a runner that keeps its workspace, missing
    the sources added since. `$UBQ_HOST_TEST_TREE` names one explicitly;
    otherwise the subtree holding the most of the canonical set wins, which is
    the one the current run built. Refusing when even the best is incomplete is
    unchanged: a partial scan still reads as a clean cut.
    """
    named = os.environ.get("UBQ_HOST_TEST_TREE")
    if named:
        return Path(named)
    best, bestFound = HOST_TESTS / "ctest", -1
    for tree in sorted(p for p in HOST_TESTS.glob("*") if p.is_dir()):
        index = objectIndex(tree)
        found = sum(1 for source in sources if objectFor(index, source) is not None)
        if found > bestFound:
            best, bestFound = tree, found
    return best


def objectIndex(tree):
    """Returns the objects under `tree`, keyed by the source path each compiles.

    A basename is not a key: `Source/Core/DragDrop/Session.cpp`,
    `Source/Core/Protocol/Tmux/Session.cpp` and `Source/UbiquittyKit/Session.cpp`
    all compile to `Session.cpp.o`, and whichever the walk met first would be
    read for all three. CMake places an object at `<target>.dir/<source>.o`, so
    the part after `.dir/` names its source. A fixture that flattens the path
    into the file name, each `/` spelled `_`, is keyed by that name.
    """
    index = {}
    for path in tree.rglob("*.cpp.o"):
        relative = path.relative_to(tree).as_posix()
        marker = relative.find(".dir/")
        if marker >= 0:
            index.setdefault(relative[marker + len(".dir/"):-len(".o")], path)
        index.setdefault(path.name, path)
    return index


def objectFor(index, source):
    """Returns the object compiled from `source`, or None when the tree has none."""
    return index.get(source) or index.get(source.replace("/", "_") + ".o")

# nm letters that mean "this object defines it". Weak and common definitions
# count: a symbol several objects define is not one a trim can take away.
DEFINING = set("TDBSRWVC")


def canonicalSources(canonical=CANONICAL):
    """Returns the canonical Core translation units."""
    if not canonical.is_file():
        return []
    return [line.strip() for line in canonical.read_text().splitlines()
            if line.strip() and not line.strip().startswith("#")]


SUBSET_CUT = re.compile(r"(?:ubiquitty_core_subset|ubiquitty_add_core_ios_library)\(\s*([\w-]+)\s+"
                        r"(?:ABSOLUTE\s+)?EXCLUDE(.*?)\)\s*\n", re.S)


def subsets(cmakeFiles=(CMAKE,)):
    """Returns each frontend's excluded translation units, by subset name."""
    found = {}
    for cmakeFile in cmakeFiles:
        if cmakeFile.is_file():
            found.update(subsetsIn(cmakeFile.read_text()))
    return found


def subsetsIn(text):
    """Returns the excluded translation units of each cut in one CMake text."""
    found = {}
    for match in SUBSET_CUT.finditer(text):
        # A comment inside the block is prose about the cut, not part of it.
        # Read as data it can only ever ADD a path, which is the direction that
        # hides a defect: a sentence naming the file a trimmed one derives from
        # puts that file in the gate's model of the exclusion list, and the
        # reference the cut actually broke then reads as trimmed-to-trimmed and
        # passes. Nothing here spells an entry inside a comment, so dropping
        # them subtracts nothing.
        body = re.sub(r"#[^\n]*", "", match.group(2))
        found[match.group(1)] = re.findall(r"Source/\S+?\.cpp", body)
    return found


def symbolTables(sources):
    """Returns (defines, references) per source, read from the host build.

    - Parameter sources: the canonical translation units.
    - Returns: two dicts of source path to symbol set, the list of sources whose
      object was not found, and the tree they were read from — a caller that
      reports the tree must name the one that was scanned, not resolve it again.
    """
    tree = objectTree(sources)
    objects = objectIndex(tree)
    defines = collections.defaultdict(set)
    references = collections.defaultdict(set)
    absent = []
    for source in sources:
        obj = objectFor(objects, source)
        if obj is None:
            absent.append(source)
            continue
        listing = subprocess.run(["nm", "-g", str(obj)], capture_output=True, text=True).stdout
        for line in listing.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                references[source].add(parts[1])
            elif len(parts) == 3 and parts[1] in DEFINING:
                defines[source].add(parts[2])
    return defines, references, absent, tree


def judge(sources, excluded, defines, references):
    """Returns the kept TUs referencing symbols only trimmed TUs define."""
    trimmed = set(excluded)
    kept = [source for source in sources if source not in trimmed]
    keptDefines = set().union(*[defines[source] for source in kept]) if kept else set()
    trimmedDefines = set().union(*[defines[source] for source in trimmed]) if trimmed else set()
    onlyTrimmed = trimmedDefines - keptDefines
    findings = []
    for source in kept:
        for symbol in sorted(references[source] & onlyTrimmed):
            findings.append((source, symbol))
    return findings


def demangled(symbol):
    """Returns a readable spelling of a mangled name, or the name itself."""
    result = subprocess.run(["c++filt", symbol], capture_output=True, text=True)
    return result.stdout.strip() or symbol


def reportFailures(failures):
    """Prints each self-test failure and returns the failing status."""
    for failure in failures:
        print("check-core-subset-symbols --self-test: " + failure, file=sys.stderr)
    return 1


def selfTest(canonical=CANONICAL, cmakeFiles=(CMAKE,)):
    """Proves the rule fires, and that a clean verdict is not an empty scan."""
    failures = []
    sources = canonicalSources(canonical)
    parsed = subsets(cmakeFiles)
    if not sources:
        failures.append("the canonical source list is empty — a clean verdict would be its own")
    if not parsed:
        failures.append("no subset cut parsed out of " + ", ".join(str(f) for f in cmakeFiles)
                        + ", so every subset would report clean by having no members")

    # Every spelling of a cut is read, and a comment inside one adds nothing.
    for spelling, name in (("ubiquitty_core_subset(UBIQUITTY_X_CORE EXCLUDE", "UBIQUITTY_X_CORE"),
                           ("ubiquitty_core_subset(UBIQUITTY_X_CORE ABSOLUTE EXCLUDE", "UBIQUITTY_X_CORE"),
                           ("ubiquitty_add_core_ios_library(ubiquitty-x EXCLUDE", "ubiquitty-x")):
        read = subsetsIn(spelling + "\n    # Source/Core/Commented.cpp\n    Source/Core/Cut.cpp\n"
                         "    INCLUDE_DIRECTORIES ${ROOTS})\n")
        if read != {name: ["Source/Core/Cut.cpp"]}:
            failures.append(f"`{spelling}` read as {read}, not one cut of Source/Core/Cut.cpp")

    # No object tree at all is an input that does not exist here, not a rule
    # that stopped catching its planted defect: the self-test stands down and
    # names the build that supplies it. A tree with some objects missing is a
    # partial scan, and that stays a failure below.
    tree = objectTree(sources)
    if not any(tree.rglob("*.cpp.o")):
        if failures:
            return reportFailures(failures)
        print(f"check-core-subset-symbols: SKIPPED — no host test objects under {tree}, so the "
              "self-test read nothing; ./Scripts/build-tests.sh builds them")
        return 0

    defines, references, absent, tree = symbolTables(sources)
    if absent and not os.environ.get("UBQ_HOST_TEST_TREE"):
        # An unnamed tree is whatever an earlier run left under BuildHostTests,
        # and one that lacks canonical sources was built before they existed:
        # on a runner that keeps its workspace, the style job read a coverage
        # tree four days old and reported the rule broken, while the rule had
        # only never been asked. That is an input this run does not have, the
        # same as no tree at all, and it stands down the same way — naming
        # what is missing, so the reader can tell a stale tree from a trimmed
        # build. A caller that built a tree names it, and a named tree missing
        # sources is a build that dropped them, which stays a failure below.
        if failures:
            return reportFailures(failures)
        newest = sorted(absent)[-1]
        print(f"check-core-subset-symbols: SKIPPED — {tree.name} under BuildHostTests predates "
              f"{len(absent)} canonical source(s) ({newest} among them), so it is not this "
              "run's build; ./Scripts/build-tests.sh builds one that is")
        return 0
    if absent:
        failures.append(f"{len(absent)} canonical source(s) have no object in {tree.name} — build "
                        "the test tree first; a partial scan reads as a clean cut")
    if sources and not any(defines.values()):
        failures.append("no object defined a single symbol, so the nm pass is reading nothing")

    # Two sources that share a file name must read two objects, or one of them
    # is judged by the other's symbols.
    index = objectIndex(tree)
    byName = collections.defaultdict(list)
    for source in sources:
        byName[Path(source).name].append(source)
    for name, sharing in sorted(byName.items()):
        found = [objectFor(index, source) for source in sharing]
        if len(sharing) > 1 and None not in found and len(set(found)) < len(sharing):
            failures.append(f"{len(sharing)} sources named {name} read {len(set(found))} object(s)")

    # The rule has to fire. Trimming a file the rest of Core calls into is the
    # planted defect, and CellGrid.cpp is reached from most of the tree.
    if sources and defines:
        planted = "Source/Core/CellGrid.cpp"
        if planted not in sources:
            planted = sources[0]
        if not judge(sources, [planted], defines, references):
            failures.append(f"trimming {planted} produced no finding, so the comparison is not "
                            "comparing")
        # And it must not fire on a trim of nothing.
        if judge(sources, [], defines, references):
            failures.append("an empty trim list produced a finding, so the rule fires on the tree "
                            "rather than on the cut")

    for name, excluded in sorted(parsed.items()):
        stale = [f for f in excluded if f not in sources]
        if stale:
            failures.append(f"{name} names {len(stale)} file(s) outside the canonical list")

    if failures:
        return reportFailures(failures)
    print(f"check-core-subset-symbols --self-test: ok ({len(parsed)} subset(s) over {len(sources)} "
          "canonical source(s); trimming a depended-on file fires and trimming nothing does not)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true",
                        help="prove the rule fires, and that a clean verdict is not an empty scan")
    parser.add_argument("--if-built", action="store_true",
                        help="stand down, saying so, when this machine has no host test objects "
                             "at all; a partial tree still refuses")
    parser.add_argument("--cmake", action="append", type=Path, default=[],
                        help="a CMake file holding subset cuts; this checkout's CMakeLists.txt "
                             "when none is named")
    parser.add_argument("--canonical", type=Path, default=CANONICAL,
                        help="the canonical Core source list the cuts are made from")
    arguments = parser.parse_args()
    cmakeFiles = tuple(arguments.cmake) or (CMAKE,)

    if arguments.self_test:
        return selfTest(arguments.canonical, cmakeFiles)

    sources = canonicalSources(arguments.canonical)
    parsed = subsets(cmakeFiles)
    if not sources or not parsed:
        print("check-core-subset-symbols: REFUSED — no canonical sources or no subset blocks "
              "parsed. An empty scan and an intact cut print the same verdict.", file=sys.stderr)
        return 2

    if arguments.if_built:
        # check-style runs on machines that build nothing; there the question
        # has no input, which is a stand-down rather than a verdict. A tree
        # with some objects is a build of this checkout, and a partial one
        # still refuses below.
        tree = objectTree(sources)
        if not any(tree.rglob("*.cpp.o")):
            print(f"check-core-subset-symbols: SKIPPED — no host test objects under {tree}; "
                  "./Scripts/build-tests.sh builds them")
            return 0

    defines, references, absent, tree = symbolTables(sources)
    if absent:
        print(f"check-core-subset-symbols: REFUSED — {len(absent)} canonical source(s) have no "
              f"object under {tree}. Run Scripts/build-tests.sh first; a partial symbol table "
              "reads as an intact cut.", file=sys.stderr)
        return 2

    failed = False
    for name, excluded in sorted(parsed.items()):
        findings = judge(sources, excluded, defines, references)
        if not findings:
            continue
        failed = True
        print(f"check-core-subset-symbols: {name} keeps a file that needs a trimmed one:",
              file=sys.stderr)
        for source, symbol in findings:
            print(f"    {source}", file=sys.stderr)
            print(f"        needs {demangled(symbol)}", file=sys.stderr)
    if failed:
        print("  Trim the caller too, or keep the callee. A reference across the cut is an "
              "undefined symbol on any arm whose linker does not discard the caller — and one "
              "that does discard it today is one entry point away from not.", file=sys.stderr)
        return 1

    total = sum(len(v) for v in parsed.values())
    print(f"check-core-subset-symbols: ok ({len(parsed)} subset(s), {total} trimmed translation "
          f"unit(s) across them, no kept file referencing a symbol only a trimmed file defines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
