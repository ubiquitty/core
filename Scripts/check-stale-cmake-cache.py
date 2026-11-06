#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Check that no build directory holds a cached default the tree has moved past.

A `set(X "v" CACHE ...)` guarded by `if(NOT DEFINED X)` writes its value once,
when the directory is first configured. Raising the default afterwards changes
nothing for any directory that already exists: CMake keeps what it has and says
nothing about it, forever.

That is invisible in the worst way, because the symptom is a compile error in a
header nobody edited. Raising CMAKE_OSX_DEPLOYMENT_TARGET from 13.0 to 13.3 so
`std::print` could be used produced, in one hour, across four checkouts:

    'to_chars' is unavailable: introduced in macOS 13.3   in a libc++ header
    "Failed test dependencies" on 17 ctest cases          from the build that failed
    a shell_snapshots STALENESS message                   the binary was old because
                                                          its build kept failing

Four checkouts, one cause, four symptoms, and not one of them named the cache. Two
of us ran source-level attribution controls — rebuild clean origin/main, see the
same failure, conclude the source is not the difference — which is a correct
experiment that cannot reach this answer: both arms use the same build
directory, so holding it fixed is holding the cause fixed.

This compares what a fresh configure would choose against what each existing
directory holds, and names the variable and the directory rather than whatever
the compiler happened to complain about.

The defaults are read from the root CMakeLists.txt and the build modules under
Scripts/cmake, where the configure-time defaults Core's builds share live, or
from the files --cmake names.

Stdlib only.

    python3 Scripts/check-stale-cmake-cache.py [--cmake <file>]...
    python3 Scripts/check-stale-cmake-cache.py --fix   print the commands, run nothing
"""

import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ubq_roots  # noqa: E402

# The build directories judged are those configured from the repository run
# from; its build modules are its own, then the libraries' it carries.
REPO = ubq_roots.subject_of(__file__)


def defaultCMakeFiles():
    """The root CMakeLists.txt and every build module of the repository and the libraries it carries."""
    files = []
    for root in dict.fromkeys((REPO, ubq_roots.core_root(REPO), ubq_roots.kit_root(REPO))):
        files += [os.path.join(root, "CMakeLists.txt")]
        files += sorted(glob.glob(os.path.join(root, "Scripts", "cmake", "*.cmake")))
    return [path for path in files if os.path.isfile(path)]


def namedCMakeFiles(argv):
    """The files each --cmake names, or the default set when none is named."""
    named = [argv[index + 1] for index, argument in enumerate(argv[:-1]) if argument == "--cmake"]
    return named or defaultCMakeFiles()


def guardedDefaults(text):
    """Returns {variable: value} for each CACHE default the tree can move.

    Only the `if(NOT DEFINED X)` shape: an unguarded `set(... CACHE ... FORCE)`
    rewrites the cache on every configure and cannot go stale, and a plain
    `option()` the caller set on purpose is theirs to keep.
    """
    found = {}
    unresolved = []
    opener = re.compile(r"if\s*\(\s*NOT DEFINED (\w+)\s*\)")
    inner_if = re.compile(r"\bif\s*\(")
    inner_endif = re.compile(r"\bendif\s*\(")
    for match in opener.finditer(text):
        name = match.group(1)
        # Scan forward for the MATCHING endif(), counting nested if()/endif() so a
        # nested block — a diagnostic message() guarded on a flag, say — before the
        # set() line does not truncate the body. `(.*?)endif` stopped at the first
        # endif(), dropping the set() below a nested block and the variable with it:
        # a member of the population silently absent, with other defaults keeping
        # `found` non-empty so the whole-population refusal never fires.
        depth, pos, end = 1, match.end(), None
        while pos < len(text):
            nxt_end = inner_endif.search(text, pos)
            if nxt_end is None:
                break
            nxt_if = inner_if.search(text, pos)
            if nxt_if is not None and nxt_if.start() < nxt_end.start():
                depth += 1
                pos = nxt_if.end()
            else:
                depth -= 1
                pos = nxt_end.end()
                if depth == 0:
                    end = nxt_end.start()
                    break
        if end is None:
            continue
        body = text[match.end():end]
        value = re.search(r'set\(\s*%s\s+"?([^"\s)]+)"?\s+CACHE' % re.escape(name), body)
        if not value:
            continue
        literal = value.group(1)
        # A default written as ${other} is computed at configure time — read from
        # a file, derived from a probe — and this script does not evaluate
        # CMake. Comparing a cache against the unexpanded text reports every
        # directory as stale forever, which is the always-fires gate that teaches
        # people to skip the whole hook. Naming it as uncovered says what this
        # script did not evaluate; comparing it silently would report a verdict
        # on a value nobody computed.
        if "${" in literal or "$CACHE{" in literal:
            unresolved.append((name, literal))
            continue
        found[name] = literal
    return found, unresolved


def cachedValue(cachePath, name):
    """The value a configured directory holds, or None when it names none."""
    try:
        with open(cachePath, encoding="utf-8", errors="ignore") as f:
            for line in f:
                if line.startswith(name + ":"):
                    return line.split("=", 1)[1].strip() if "=" in line else None
    except OSError:
        return None
    return None


def buildDirectories():
    """Build directories configured from this repo's own CMakeLists.

    CMAKE_HOME_DIRECTORY is what discriminates, and it has to: a vendored
    dependency configures its own tree (build/quic-deps/src/aws-lc names
    aws-lc's CMakeLists), and the Linux container configures ours through a
    bind mount that calls it /src. Neither takes its defaults from the file
    this check reads, so comparing them reports a difference that means
    nothing — and a check that names ten directories nobody must touch is one
    people stop reading before they reach the one that matters.
    """
    found = []
    for root, dirs, files in os.walk(REPO):
        dirs[:] = [d for d in dirs if d not in (".git", "node_modules", "ThirdParty")]
        if "CMakeCache.txt" not in files:
            continue
        dirs[:] = []
        home = cachedValue(os.path.join(root, "CMakeCache.txt"), "CMAKE_HOME_DIRECTORY")
        if home and os.path.realpath(home) == os.path.realpath(REPO):
            found.append(root)
    return sorted(found)


def selfTest():
    """Exercise the comparison on a synthetic tree.

    The real tree compares nothing today: its one guarded default is computed
    from a file, and the hazard for that variable is handled upstream by making
    the value self-healing. So without this, the code path that reports a stale
    directory never runs — and a check whose failing arm is never taken is one
    nobody can trust the day it finally matters.
    """
    import shutil
    import tempfile
    root = tempfile.mkdtemp()
    try:
        with open(os.path.join(root, "CMakeLists.txt"), "w") as f:
            f.write('if(NOT DEFINED UBQ_PROBE)\n'
                    '    set(UBQ_PROBE "2.0" CACHE STRING "probe")\n'
                    'endif()\n')
        stale = os.path.join(root, "build-stale")
        current = os.path.join(root, "build-current")
        for directory, value in ((stale, "1.0"), (current, "2.0")):
            os.makedirs(directory)
            with open(os.path.join(directory, "CMakeCache.txt"), "w") as f:
                f.write("CMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % root)
                f.write("UBQ_PROBE:STRING=%s\n" % value)

        defaults, unresolved = guardedDefaults(open(os.path.join(root, "CMakeLists.txt")).read())
        if defaults != {"UBQ_PROBE": "2.0"} or unresolved:
            print("  FAIL: a literal guarded default was not read as one: %r" % (defaults,))
            return 1
        staleHeld = cachedValue(os.path.join(stale, "CMakeCache.txt"), "UBQ_PROBE")
        currentHeld = cachedValue(os.path.join(current, "CMakeCache.txt"), "UBQ_PROBE")
        if staleHeld != "1.0" or currentHeld != "2.0":
            print("  FAIL: a cache value was misread: stale=%r current=%r" % (staleHeld, currentHeld))
            return 1
        if staleHeld == defaults["UBQ_PROBE"]:
            print("  FAIL: the stale directory compared equal")
            return 1
        if currentHeld != defaults["UBQ_PROBE"]:
            print("  FAIL: the current directory compared unequal")
            return 1

        # And the computed-default arm, which is what the real tree exercises:
        # a ${...} default must be named rather than compared, or every
        # directory reads as stale forever.
        computed, computedUnresolved = guardedDefaults(
            'if(NOT DEFINED UBQ_PROBE)\n    set(UBQ_PROBE "${OTHER}" CACHE STRING "p")\nendif()\n')
        if computed or computedUnresolved != [("UBQ_PROBE", "${OTHER}")]:
            print("  FAIL: a computed default was compared rather than named")
            return 1

        # A nested if()/endif() before the set() line. The first-endif match
        # truncated the body here and dropped the variable with no message, while
        # other defaults kept the population non-empty so nothing refused.
        nested, nestedUnresolved = guardedDefaults(
            'if(NOT DEFINED UBQ_PROBE)\n'
            '    if(SOME_FLAG)\n        message(STATUS "configuring")\n    endif()\n'
            '    set(UBQ_PROBE "2.0" CACHE STRING "p")\nendif()\n')
        if nested != {"UBQ_PROBE": "2.0"} or nestedUnresolved:
            print("  FAIL: a default below a nested if()/endif() was dropped: %r" % (nested,))
            return 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("check-stale-cmake-cache: self-test ok (stale reported, current clean, computed named)")
    return 0


def main():
    printFix = "--fix" in sys.argv
    if "--self-test" in sys.argv:
        return selfTest()

    cmakeFiles = namedCMakeFiles(sys.argv)
    defaults, unresolved = {}, []
    for cmakeFile in cmakeFiles:
        with open(cmakeFile, encoding="utf-8") as handle:
            found, computed = guardedDefaults(handle.read())
        defaults.update(found)
        unresolved += computed
    for name, literal in unresolved:
        print("check-stale-cmake-cache: %s defaults to %s, which this check cannot "
              "evaluate — not compared" % (name, literal))
    if not defaults and not unresolved:
        # An empty population passes every comparison below, so it is an answer
        # rather than a silence: either the parse broke or the shape is gone,
        # and both want a person rather than a green line.
        print("check-stale-cmake-cache: no guarded CACHE defaults found in "
              + ", ".join(os.path.relpath(f, REPO) for f in cmakeFiles)
              + " — the parse or the convention changed", file=sys.stderr)
        return 1

    directories = buildDirectories()
    stale = []
    for directory in directories:
        cache = os.path.join(directory, "CMakeCache.txt")
        for name, expected in sorted(defaults.items()):
            held = cachedValue(cache, name)
            if held is not None and held != expected:
                stale.append((os.path.relpath(directory, REPO), name, held, expected))

    if stale:
        print("check-stale-cmake-cache: a build directory holds a default the tree has moved past",
              file=sys.stderr)
        for directory, name, held, expected in stale:
            print("  %s\n    %s: cached %s, CMakeLists now chooses %s"
                  % (directory, name, held, expected), file=sys.stderr)
        print("\nCMake keeps a cached value forever and never mentions it, so the failure "
              "surfaces as\nsomething downstream — a compile error in a header nobody edited, a "
              "test binary that\nlooks stale, a dependency that 'failed'. Reconfigure the "
              "directories above:", file=sys.stderr)
        for directory, _, _, _ in dict.fromkeys((d, n, h, e) for d, n, h, e in stale):
            print("  rm -rf %s" % directory, file=sys.stderr)
        if not printFix:
            print("\n  (--fix prints these commands only; it never deletes anything)", file=sys.stderr)
        return 1

    print("check-stale-cmake-cache: ok (%d directory/ies, %d compared, %d uncomparable)"
          % (len(directories), len(defaults), len(unresolved)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
