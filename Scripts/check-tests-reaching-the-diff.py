#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-tests-reaching-the-diff.py — run the tests this branch can break.

The whole suite is not in the push tier: several checkouts can share one
machine, and each of them running every entry before every push is the largest
single load on it. Running nothing instead lets a break through — a change adds
a row to the `Key.h` descriptor table without the row `test_preferences_keys`
hand-copies beside it, that test asserts the two tables are the same length,
and the count is one short on every platform while every gate that ran is
green, because `Tests/ compiles` compiles and does not run.

So neither all of them nor none: the ones the change reaches.

    ./Scripts/check-tests-reaching-the-diff.py <build-dir> [--changed-from REF]

Ninja already holds the answer, and `check-build-reached.py` already reads it
for a different question. Its dependency database records, per object, every
file the compiler actually OPENED — headers and the source alike, not the
includes a file declares and not what a grep of include lines would guess. So a
changed file names the objects that read it, an object names its target through
the `CMakeFiles/<target>.dir/` path CMake gives it, and a target that ctest
registers is a test to run. A settings change selects five tests where the suite
runs three hundred, and `test_preferences_keys` is one of the five.

This is the reason the step is cheap enough to be unconditional: the tier
already builds this directory for `Tests/ compiles`, so the database is warm and
the objects are current. What is added is the RUN, and only of what was named.

Three ways a selector like this passes without running, all closed here.

A missing or unreadable dependency database selects nothing, and nothing is what
a green run looks like. That REFUSES instead — the database is the whole
instrument, and a gate that cannot read its instrument has not measured
anything.

A name ctest does not know silently narrows `-R` to the names it does know, so a
target renamed out from under this would quietly stop being covered. Every
selected name is checked against `ctest -N` first and an unknown one refuses.

And a `-R` pattern can match fewer entries than it names — several names
collapsing to one is exactly how a runner reports a green over a set it never
ran. So the check is not on ctest's total: every selected name must appear in
the run's own per-test lines, and one that does not refuses whatever the rest
of them said. The total cannot serve, because ctest pulls in the fixtures a
selected test requires (`ubq_build` is a FIXTURES_SETUP entry every test in
this directory needs) and legitimately runs more entries than were asked for.

A fourth: a source only another frontend compiles. This directory builds the
Core frontend, so `Source/Platform/Host/Daemon.cpp` is read by no object here,
and the database alone reports it as reaching nothing and passes. The host
stanzas that link it are ctest entries all the same
(`shell_foreground_probe_budget` and its siblings build the host themselves),
and any of them can be red behind that report. For a file no object here reads,
the coverage index
(`Tests/coverage/covering-tests.tsv`, which records the tests whose run
executed each file) names the entries to run. Only for those files: the index
lists dozens of host stanzas under every Core header, and running them for a
header the dependency database already answers for is the whole suite again.

A fifth: a stanza that builds a CMake test's own source from a hand-written
list (`shell_test_pty` beside `test_pty`). The database names the CMake target,
which links the Core library and passes, while the stanza's list can fall
behind a case that reaches into Core and stops at the link. A selected test
therefore brings the stanza of the same name.

A sixth: a stanza whose subject is not in this directory at all. The agent
smokes run `ubq`, built in a tree of its own from a trimmed Core, and are shell
scripts. No object here is theirs, and the coverage index has no line for them
either, since the binary they run is not the instrumented one. `ubq_smoke`
asserts the list of Sync methods `ubq api list` prints, and a method joining
`MethodRegistry.cpp`, which only the library's object reads, selects nothing
from the database. So the driver's own recipes
are read: a changed file under Tests/ that a stanza's recipe names selects the
stanza, and a changed file the agent's build read selects the stanzas that run
the agent. The agent's tree alone: the host links all of Core, and every host
stanza for every Core change is the suite again.

A seventh: a stanza that runs a gate script over the tree. Its subject is
whatever the script reads, which is neither an object nor a file under Tests/:
`test_only_operations` runs `Scripts/check-test-only-operations.py`, which reads
`Source/Core/TerminalOperations.h` and searches the shipping directories for a
caller of each operation it declares. An operation added to that header with
no caller selects the tests that compile the header, and not the one stanza
that would say so. So a script a recipe runs is read as
`check-gate-scopes.py` reads a gate's: the repository paths it spells, comments
excluded, are the stanza's inputs, and a changed file that is one of them, or
lies under a directory that is, selects the stanza. The script itself is an
input too. The same lower bound applies as there: a spelled path is seen and a
computed one is not. Only a stanza that is nothing but gate scripts is read
this way. One that also compiles a test or runs a product is answered for by
the rules above, and the scripts it runs on the way are its tooling: the
snapshot stanza rebuilds its renderer through a build script that spells half
the tree, and running every arm for each of those paths is the suite again.

An eighth: a stanza that builds tests this directory also builds, another way.
`sanitized_sync` builds the Sync, Session and Runtime unit tests under
AddressSanitizer and UndefinedBehaviorSanitizer in a tree of its own, where a
read of freed memory that the plain runs of those same tests pass is caught. Its
recipe names its tests by glob, so a selected test whose source a recipe's
glob matches brings that stanza, whatever the change reached the test through.

An eighth, the other way round: a stanza selected where it only declines. The
sanitizer stanza runs on macOS alone and answers with a skip anywhere else, and
the index names it under most of Core, so a Linux run would select it for
nearly every change to print that it will not run. A stanza whose opening line
says it declines off macOS is left out of the selection there, and said to be.

A ninth: the fourth way, reopened where the daemon is compiled here. A Linux
test tree builds an object from `Source/Platform/Host/Daemon.cpp`, so the file
reaches an object and the coverage index would not be consulted for it, so a
revocation change would select none of the `revoke_*` stanzas that exercise the
daemon. A file under the host's own tree (HOST_OWNED) therefore always brings
the index's entries as well, whatever objects read it. The host's tree only:
under a Core header the index names dozens of host stanzas, and the database
already answers for those.
"""

import argparse
import fnmatch
import glob
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402

# `path/to.o: #deps 243, deps mtime … (VALID)`
DEPS_HEADER = re.compile(r"^(\S+): #deps")
# CMake's own object layout: Tests/CMakeFiles/<target>.dir/<source path>.o
OBJECT_TARGET = re.compile(r"CMakeFiles/([^/]+)\.dir/")
# How ctest names a Scripts/run-tests.sh stanza: `shell_<stanza>`.
STANZA_PREFIX = "shell_"
# Sources and headers a compiler opens. A change outside this set (a script, a
# golden, a document) reaches no object and is reported as reaching none.
# The daemon's own sources: a change here reaches the host stanzas through the
# coverage index even where an object of this directory reads it.
HOST_OWNED = ("Source/Platform/Host/",)
COMPILED_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".m", ".mm", ".inc")

# ctest's per-test line, which is what says an entry actually ran. It reads
# `<index>/<total> Test #<number>: <name> ......   Passed    0.07 sec`, and the
# name is the only field this needs.
TEST_RAN = re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)")


def changedFiles(reference):
    """Repo-relative paths this branch changed, committed and not."""
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True)
    if root.returncode != 0:
        return None, "not a git checkout"
    paths = set()
    # The branch, the working tree, the index, and the files git is not
    # tracking yet. `git diff` cannot see an untracked file, and a new test
    # source is untracked exactly while it is being written — the moment its
    # author most wants to know whether it runs.
    # A library mount that moved is read as the files between its pins, spelled as
    # the library spells them and as the build records them through the mount.
    branch = ubq_roots.changed_paths(reference, root.stdout.strip(), mounted=True)
    if branch is None:
        return None, f"no merge base with {reference}"
    paths.update(branch)
    for arguments in (["git", "diff", "--name-only", "HEAD"],
                      ["git", "diff", "--name-only", "--cached", "HEAD"],
                      ["git", "ls-files", "--others", "--exclude-standard"]):
        result = subprocess.run(arguments, capture_output=True, text=True)
        if result.returncode != 0:
            return None, f"`{' '.join(arguments)}` failed: {result.stderr.strip()}"
        paths.update(line for line in result.stdout.split("\n") if line)
    return paths, None


def dependencyMap(buildDirectory):
    """Repo-relative file -> the objects whose compile actually read it."""
    result = subprocess.run(["ninja", "-t", "deps"], cwd=buildDirectory,
                            capture_output=True, text=True)
    if result.returncode != 0 or not result.stdout.strip():
        return None
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True).stdout.strip()
    byFile, current = {}, None
    for line in result.stdout.split("\n"):
        header = DEPS_HEADER.match(line)
        if header:
            current = header.group(1)
            continue
        if not current or not line.startswith("    "):
            continue
        # Ninja spells its dependencies relative to the build directory, which
        # sits outside the checkout, so they resolve through several `..` to an
        # absolute path this makes repo-relative again.
        absolute = os.path.normpath(os.path.join(buildDirectory, line.strip()))
        try:
            relative = os.path.relpath(absolute, root)
        except ValueError:
            continue
        if relative.startswith(".."):
            continue
        byFile.setdefault(relative, set()).add(current)
    return byFile


# How Scripts/run-tests.sh opens a stanza (Scripts/stanza-declarations.awk).
STANZA_START = re.compile(r'^[ \t]*(?:if (?:\[ "\$\(uname -s\)" = "Darwin" \] && )?wanted|build_and_run) ([A-Za-z0-9_-]+)')
# A file under Tests/ a recipe names: the script it runs, the source it
# compiles. A `*` in the name is the recipe's own glob, and names every file it
# matches.
RECIPE_PATH = re.compile(r'\$ROOT/(Tests/[A-Za-z0-9_./*-]+\.[A-Za-z0-9]+)')
# A test target's source, from its object: CMake lays the object out under the
# target's directory at the source's own path below Tests/.
TEST_OBJECT_SOURCE = re.compile(r"^Tests/CMakeFiles/[^/]+\.dir/(.+)\.o$")
# A product binary a recipe runs, by the build tree under the build root that
# produces it, and the spellings of the binary in a recipe: its path, and the
# driver's helper that a stanza asks for it through. The agent's only; see the
# module comment for why not the host's.
PRODUCT_BINARIES = {"agent": ("$UBQ_BUILD_ROOT/agent/ubq", "ubq_agent_binary")}
# A gate script a recipe runs.
RECIPE_SCRIPT = re.compile(r'\$ROOT/(Scripts/[A-Za-z0-9_./-]+\.(?:py|sh))')
# A repository path spelled in a script, as Scripts/check-gate-scopes.py reads
# one: a file, or a directory the script walks.
SPELLED_PATH = re.compile(
    r'(?<![\w-])(?:\./)?((?:Scripts|Source|Tests|Tools|Docs|Support|Samples|Resources)/[A-Za-z0-9_./+-]+)')
SCRIPT_COMMENT = re.compile(r'(^|\s)#.*$', re.M)


def scriptsRun(driverLines):
    """The gate scripts each stanza's recipe runs.

    - Parameter driverLines: The lines of Scripts/run-tests.sh.
    - Returns: Stanza name -> the Scripts/ files its recipe names. A recipe
      runs to the next stanza's opening, and a comment names nothing.
    """
    scripts, current = {}, None
    for line in driverLines:
        opening = STANZA_START.match(line)
        if opening:
            current = opening.group(1)
        if current is None or line.lstrip().startswith("#"):
            continue
        found = RECIPE_SCRIPT.findall(line)
        if found:
            scripts.setdefault(current, set()).update(found)
    return scripts


def gateStanzas(scripts, recipes):
    """The stanzas that are nothing but gate scripts.

    - Parameter scripts: What `scriptsRun` answered.
    - Parameter recipes: What `stanzaRecipes` answered.
    - Returns: `scripts` without each stanza whose recipe also names a file
      under Tests/ or runs a product binary.
    """
    return {stanza: names for stanza, names in scripts.items()
            if not any(recipes.get(stanza, ((), ())))}


def spelledPaths(scriptText):
    """The repository paths a script's text spells, comments excluded.

    - Parameter scriptText: The script.
    - Returns: The paths, without a trailing slash or sentence punctuation.
    """
    return {path.rstrip("/.") for path in SPELLED_PATH.findall(SCRIPT_COMMENT.sub("", scriptText))}


def gateInputs(scripts, readScript):
    """What each stanza's gate scripts read.

    - Parameter scripts: What `scriptsRun` answered.
    - Parameter readScript: Repo-relative script path -> its text, or None
      when it cannot be read.
    - Returns: Stanza name -> the scripts it runs and the paths they spell.
    """
    inputs = {}
    for stanza, names in scripts.items():
        reads = set(names)
        for name in names:
            text = readScript(name)
            if text is not None:
                reads |= spelledPaths(text)
        inputs[stanza] = reads
    return inputs


def isUnder(path, spelled):
    """Whether a changed file is a spelled path or lies under one."""
    return path == spelled or path.startswith(spelled + "/")


# A stanza that opens by declining anywhere but macOS.
DECLINES_OFF_DARWIN = re.compile(r'^[ \t]*if wanted ([A-Za-z0-9_-]+) && \[ "\$\(uname -s\)" != "Darwin" \]')


def stanzasDecliningOn(driverLines, platform):
    """The stanzas that decline on a platform by their opening line.

    - Parameter driverLines: The lines of Scripts/run-tests.sh.
    - Parameter platform: What `uname -s` answers on this host.
    - Returns: The names; none on Darwin, where such a stanza runs.
    """
    if platform == "Darwin":
        return set()
    return {match.group(1) for line in driverLines if (match := DECLINES_OFF_DARWIN.match(line))}


def stanzaRecipes(driverLines):
    """What each stanza's recipe names.

    - Parameter driverLines: The lines of Scripts/run-tests.sh.
    - Returns: Stanza name -> (the Tests/ files its recipe names, the product
      trees whose binary it runs). A recipe runs to the next stanza's opening.
    """
    recipes, current = {}, None
    for line in driverLines:
        opening = STANZA_START.match(line)
        if opening:
            current = opening.group(1)
            recipes.setdefault(current, (set(), set()))
        if current is None or line.lstrip().startswith("#"):
            continue
        files, trees = recipes[current]
        files.update(RECIPE_PATH.findall(line))
        trees.update(tree for tree, spellings in PRODUCT_BINARIES.items()
                     if any(spelling in line for spelling in spellings))
    return recipes


def sharedTargetStanzas(driverLines):
    """Map each shared executable target to every shell entry that runs it."""
    targets, current = {}, None
    for line in driverLines:
        opening = STANZA_START.match(line)
        if opening:
            current = opening.group(1)
        if current is None or line.lstrip().startswith("#"):
            continue
        target = re.search(r"\bubq_build_test\s+([A-Za-z0-9_-]+)\s", line)
        if target:
            targets.setdefault(target.group(1), set()).add(STANZA_PREFIX + current)
    return targets


def stanzasReaching(path, recipes, productReads, inputs=None):
    """The ctest entries of the stanzas a changed file reaches through a recipe.

    - Parameter path: A repo-relative changed file.
    - Parameter recipes: What `stanzaRecipes` answered.
    - Parameter productReads: Product tree -> the repo-relative files its build read.
    - Parameter inputs: What `gateInputs` answered.
    - Returns: `shell_<stanza>` for each stanza that names the file, runs a
      binary built from it, or runs a gate script that reads it.
    """
    reached = {STANZA_PREFIX + stanza for stanza, (files, trees) in recipes.items()
               if any(recipeNames(named, path) for named in files)
               or any(path in productReads.get(tree, ()) for tree in trees)}
    reached |= {STANZA_PREFIX + stanza for stanza, reads in (inputs or {}).items()
                if any(isUnder(path, spelled) for spelled in reads)}
    return reached


def recipeNames(named, path):
    """Whether a path a recipe names is a file, or a glob the file matches.

    - Parameter named: A path as the recipe spells it.
    - Parameter path: A repo-relative file.
    - Returns: Whether the recipe names the file. A `*` stays inside one
      directory, as the shell's does.
    """
    if "*" not in named:
        return named == path
    return os.path.dirname(named) == os.path.dirname(path) and fnmatch.fnmatchcase(path, named)


def coveringTests(indexPath):
    """Repo-relative file -> the tests whose run executed it, from the index.

    An absent index answers an empty map: the files it would have named are
    then reported as reaching nothing, as the dependency database alone says.

    - Parameter indexPath: `Tests/coverage/covering-tests.tsv`.
    - Returns: The map.
    """
    covering = {}
    try:
        with open(indexPath, encoding="utf-8") as index:
            for line in index:
                if line.startswith("#") or "\t" not in line:
                    continue
                path, names = line.rstrip("\n").split("\t", 1)
                covering[path] = set(names.split())
    except OSError:
        return {}
    return covering


def selectTests(changedPaths, byFile, known, covering=None, recipes=None, productReads=None, inputs=None, targetStanzas=None):
    """The tests to run for `changedPaths`, and the paths that reached nothing.

    The rule, on its own so a self-test can plant a dependency map against it:
    a changed file names the objects whose compile read it, an object names its
    target through CMake's `CMakeFiles/<target>.dir/` path, and a target ctest
    registers is a test. Intersecting with `known` is what stops a library
    target — `ubiquitty-behaviour` reads most of these headers — being handed to
    `ctest -R`, where an unknown name narrows the pattern in silence.

    - Parameter changedPaths: Repo-relative sources and headers the branch moved.
    - Parameter byFile: Repo-relative file -> objects that read it.
    - Parameter known: The test names ctest registers.
    A test whose target is selected brings the stanza of its name, where ctest
    registers one. The shell wrapper's setup and invocation remain part of
    the contract even when it runs the same executable.

    - Parameter covering: Repo-relative file -> tests the coverage index names,
      consulted only for a path no object read.
    - Parameter recipes: Stanza -> what its recipe names, from `stanzaRecipes`.
      Whatever else a path reaches, it also reaches the stanzas whose recipe
      names it, and those that run a product binary built from it.
    - Parameter productReads: Product tree -> the files its build read.
    - Parameter inputs: Stanza -> what its gate scripts read, from
      `gateInputs`. A path one of them reads reaches that stanza as well.
    - Parameter targetStanzas: Shared executable target -> the shell entries
      that run it, from `sharedTargetStanzas`.
    - Returns: The selected test names, and the changed paths that reached
      neither an object nor an indexed test.
    """
    selected, reachedNothing = set(), []
    for path in changedPaths:
        throughRecipe = stanzasReaching(path, recipes or {}, productReads or {}, inputs) & known
        selected |= throughRecipe
        objects = byFile.get(path)
        if not objects:
            indexed = (covering or {}).get(path, set()) & known
            if indexed:
                selected |= indexed
            elif not throughRecipe:
                reachedNothing.append(path)
            continue
        if path.startswith(HOST_OWNED):
            selected |= (covering or {}).get(path, set()) & known
        targets = {match.group(1) for o in objects
                   if (match := OBJECT_TARGET.search(o))}
        selected |= targets & known
        for target in targets:
            selected |= (targetStanzas or {}).get(target, set()) & known
        # The matching shell stanza exercises its own setup and invocation.
        selected |= {STANZA_PREFIX + target for target in targets} & known
        # A stanza whose recipe names a selected test's source builds that
        # test another way, under a sanitizer for one, and is a further test
        # of whatever the change reached the test through.
        for o in objects:
            target, source = OBJECT_TARGET.search(o), TEST_OBJECT_SOURCE.match(o)
            if target and source and target.group(1) in known:
                selected |= stanzasReaching("Tests/" + source.group(1), recipes or {}, {}) & known
    return selected, reachedNothing


def selfTest():
    """The rule against maps it must read one way and not the other."""
    failures = []

    def check(claim, got, want):
        if got != want:
            failures.append(f"{claim}: got {got!r}, want {want!r}")

    # A header two test objects and one library object read. The library target
    # is not a test and must not reach `ctest -R`, where it would narrow the
    # pattern to the names ctest does know without saying so.
    byFile = {
        "Source/Core/Preferences/Key.h": {
            "Tests/CMakeFiles/test_preferences_keys.dir/Core/Preferences/test_preferences_keys.cpp.o",
            "Tests/CMakeFiles/test_effective_value.dir/Core/Preferences/test_effective_value.cpp.o",
            "CMakeFiles/ubiquitty-behaviour.dir/Source/Core/Preferences/Preferences.cpp.o",
        },
        # Read only by a library, which is the shape of a header no test covers.
        "Source/Core/Uncovered.h": {
            "CMakeFiles/ubiquitty-behaviour.dir/Source/Core/Uncovered.cpp.o",
        },
    }
    known = {"test_preferences_keys", "test_effective_value", "test_runtime"}

    selected, nothing = selectTests(["Source/Core/Preferences/Key.h"], byFile, known)
    check("a changed header selects the tests that read it",
          sorted(selected), ["test_effective_value", "test_preferences_keys"])
    check("and reports no path as unreached", nothing, [])

    selected, nothing = selectTests(["Source/Core/Uncovered.h"], byFile, known)
    check("a header no test reads selects nothing", sorted(selected), [])
    check("and is not reported as unreached — an object did read it", nothing, [])

    selected, nothing = selectTests(["Source/Core/Absent.h"], byFile, known)
    check("a header no object reads at all is reported", nothing, ["Source/Core/Absent.h"])
    check("and selects nothing", sorted(selected), [])

    # The rename case: the target exists in the map and not in the registry.
    selected, _ = selectTests(["Source/Core/Preferences/Key.h"], byFile,
                              {"test_effective_value"})
    check("a target ctest does not register is dropped rather than passed to -R",
          sorted(selected), ["test_effective_value"])

    # A source only another frontend compiles: no object here reads it, and the
    # coverage index names the host stanzas that do. An indexed name ctest does
    # not register is dropped, as a renamed target is.
    covering = {"Source/Platform/Host/Daemon.cpp": {"shell_foreground_probe_budget", "shell_retired_stanza"},
                "Source/Core/Preferences/Key.h": {"shell_host_cli"}}
    hostKnown = known | {"shell_foreground_probe_budget", "shell_host_cli"}
    selected, nothing = selectTests(["Source/Platform/Host/Daemon.cpp"], byFile, hostKnown, covering)
    check("a source no object reads selects the stanzas the index names",
          sorted(selected), ["shell_foreground_probe_budget"])
    check("and is no longer reported as reaching nothing", nothing, [])
    selected, _ = selectTests(["Source/Core/Preferences/Key.h"], byFile, hostKnown, covering)
    check("a header the database answers for takes nothing from the index",
          sorted(selected), ["test_effective_value", "test_preferences_keys"])
    linuxByFile = dict(byFile)
    linuxByFile["Source/Platform/Host/Daemon.cpp"] = {"Tests/CMakeFiles/test_host_daemon.dir/x.o"}
    selected, _ = selectTests(["Source/Platform/Host/Daemon.cpp"], linuxByFile, hostKnown | {"test_host_daemon"}, covering)
    check("a host source an object here also reads still brings the index's stanzas",
          sorted(selected), ["shell_foreground_probe_budget", "test_host_daemon"])
    keyByFile = dict(byFile)
    keyByFile["Source/Core/Preferences/Key.h"] = {"Tests/CMakeFiles/test_runtime.dir/x.o"}
    selected, _ = selectTests(["Source/Core/Preferences/Key.h"], keyByFile, hostKnown | {"test_runtime"}, covering)
    check("and a Core header an object reads does not", sorted(selected), ["test_runtime"])
    selected, nothing = selectTests(["Source/Platform/Host/Daemon.cpp"], byFile, hostKnown, {})
    check("without the index the source is reported as reaching nothing",
          (sorted(selected), nothing), ([], ["Source/Platform/Host/Daemon.cpp"]))

    # A stanza that builds a CMake test's source from a hand-written list is a
    # second link of the same test. The database answers for the file with the
    # CMake target alone, and the stanza is what breaks when the list falls
    # behind the source: `test_pty` linked and passed while `shell_test_pty`
    # stopped at undefined Core symbols.
    twinFile = {"Tests/Platform/test_pty.cpp": {"Tests/CMakeFiles/test_pty.dir/Platform/test_pty.cpp.o"}}
    selected, _ = selectTests(["Tests/Platform/test_pty.cpp"], twinFile, {"test_pty", "shell_test_pty", "shell_host_cli"})
    check("a selected test brings the stanza that links the same source by hand",
          sorted(selected), ["shell_test_pty", "test_pty"])
    selected, _ = selectTests(["Tests/Platform/test_pty.cpp"], twinFile, {"test_pty", "shell_host_cli"})
    check("and brings none where ctest registers no such stanza", sorted(selected), ["test_pty"])

    shared = sharedTargetStanzas([
        'if wanted session_directory; then\n',
        '    ubq_build_test ubq-shell-session_directory "$OUT/session_directory" "$log"\n',
        'fi\n',
        '# ubq_build_test ignored "$out" "$log"\n',
        'if wanted directory_watch_silence; then\n',
        '    ubq_build_test ubq-shell-session_directory "$OUT/session_directory" "$log"\n',
        'fi\n',
        'if wanted unrelated; then\n',
        '    ubq_build_test another-target "$out" "$log"\n',
        'fi\n',
    ])
    sharedFile = {"Tests/Host/session_directory.cpp": {
        "Tests/CMakeFiles/ubq-shell-session_directory.dir/Host/session_directory.cpp.o"}}
    sharedKnown = {"shell_session_directory", "shell_directory_watch_silence", "shell_unrelated"}
    selected, nothing = selectTests(list(sharedFile), sharedFile, sharedKnown, targetStanzas=shared)
    check("a shared executable selects every shell entry that runs it",
          (sorted(selected), nothing), (["shell_directory_watch_silence", "shell_session_directory"], []))
    selected, _ = selectTests(list(sharedFile), sharedFile, {"shell_session_directory"}, targetStanzas=shared)
    check("a shared executable never selects an unregistered shell entry",
          sorted(selected), ["shell_session_directory"])
    check("shared target discovery ignores comments and keeps stanza ownership",
          {target: sorted(stanzas) for target, stanzas in shared.items()},
          {"ubq-shell-session_directory": ["shell_directory_watch_silence", "shell_session_directory"],
           "another-target": ["shell_unrelated"]})

    # A stanza whose subject is outside this directory: a script the recipe
    # runs, and the agent binary, built in its own tree from a source that here
    # only the library's object reads. Neither the database nor the index names
    # the stanza. A recipe ends at the next stanza, and a comment names nothing.
    recipes = stanzaRecipes([
        'if wanted ubq_smoke; then\n',
        '    if [ ! -x "${UBQ_BIN:-$UBQ_BUILD_ROOT/agent/ubq}" ]; then\n',
        '        bash "$ROOT/Tests/Agent/ubq_smoke.sh" || exit 1\n',
        'fi\n',
        '# $ROOT/Tests/Agent/commented.sh and $UBQ_BUILD_ROOT/agent/ubq in a comment\n',
        'if [ "$(uname -s)" = "Darwin" ] && wanted host_cli; then\n',
        '    "$ROOT/Tests/Host/host_cli.cpp" \\\n',
        'fi\n',
        'if wanted ubq_smoke_cleanup; then\n',
        '    AGENT_BINARY="$(ubq_agent_binary "$OUT/agent-build.log")"\n',
        'fi\n',
    ])
    check("a recipe names the files and the product binary of its own stanza only",
          {name: (sorted(files), sorted(trees)) for name, (files, trees) in recipes.items()},
          {"ubq_smoke": (["Tests/Agent/ubq_smoke.sh"], ["agent"]), "host_cli": (["Tests/Host/host_cli.cpp"], []),
           "ubq_smoke_cleanup": ([], ["agent"])})
    registry = "Source/Core/Protocol/Control/MethodRegistry.cpp"
    libraryOnly = {registry: {"CMakeFiles/ubiquitty-behaviour.dir/Source/Core/Protocol/Control/MethodRegistry.cpp.o"}}
    smokeKnown = {"shell_ubq_smoke", "shell_host_cli", "test_runtime"}
    selected, nothing = selectTests([registry], libraryOnly, smokeKnown, {}, recipes, {"agent": {registry}})
    check("a source the agent's build read selects the stanzas that run the agent",
          (sorted(selected), nothing), (["shell_ubq_smoke"], []))
    selected, _ = selectTests([registry], libraryOnly, smokeKnown, {}, recipes, {"agent": set()})
    check("and selects none where the agent's build did not read it", sorted(selected), [])
    selected, nothing = selectTests(["Tests/Agent/ubq_smoke.sh"], {}, smokeKnown, {}, recipes, {})
    check("a script a recipe runs selects its stanza and is not reported as unreached",
          (sorted(selected), nothing), (["shell_ubq_smoke"], []))
    selected, nothing = selectTests(["Tests/Agent/commented.sh"], {}, smokeKnown, {}, recipes, {})
    check("a file only a comment names reaches nothing",
          (sorted(selected), nothing), ([], ["Tests/Agent/commented.sh"]))

    # A stanza that runs a gate script over the tree: the header the script
    # reads and a source under a directory it searches select the stanza, as
    # the script itself does; a path only its comment names does not, and a
    # script that cannot be read still selects its stanza when it changes.
    scripts = scriptsRun([
        'if wanted test_only_operations; then\n',
        '    python3 "$ROOT/Scripts/check-test-only-operations.py" --self-test || exit 1\n',
        'fi\n',
        '# python3 "$ROOT/Scripts/commented.py"\n',
        'if wanted retired_gate; then\n',
        '    "$ROOT/Scripts/check-retired.sh" || exit 1\n',
        'fi\n',
    ])
    check("a recipe names the gate scripts of its own stanza only",
          {name: sorted(names) for name, names in scripts.items()},
          {"test_only_operations": ["Scripts/check-test-only-operations.py"], "retired_gate": ["Scripts/check-retired.sh"]})
    builds = ['if wanted snapshots; then\n',
              '    "$ROOT/Scripts/build-core-snapshot.sh" || exit 1\n',
              '    python3 "$ROOT/Tests/Snapshots/run.py" --backend portable\n',
              'fi\n']
    check("a stanza that also runs a file under Tests/ is not read as a gate",
          sorted(gateStanzas(scriptsRun(builds), stanzaRecipes(builds))), [])
    check("and one that runs gate scripts alone is",
          sorted(gateStanzas(scripts, stanzaRecipes([]))), ["retired_gate", "test_only_operations"])
    gateText = {"Scripts/check-test-only-operations.py":
                'HEADER = str(REPO / "Source/Core/TerminalOperations.h")\n'
                'SHIPPING = ("Source/Platform", "Source/Agent/")  # not Source/Core/Commented.h\n'}
    inputs = gateInputs(scripts, gateText.get)
    check("a gate's inputs are its script and the paths the script spells outside a comment",
          sorted(inputs["test_only_operations"]),
          ["Scripts/check-test-only-operations.py", "Source/Agent", "Source/Core/TerminalOperations.h", "Source/Platform"])
    check("and a script that cannot be read is still its stanza's input",
          sorted(inputs["retired_gate"]), ["Scripts/check-retired.sh"])
    gateKnown = {"shell_test_only_operations", "shell_retired_gate", "test_runtime"}
    operations = "Source/Core/TerminalOperations.h"
    operationsReadBy = {operations: {"Tests/CMakeFiles/test_runtime.dir/Core/Runtime/test_runtime.cpp.o"}}
    selected, nothing = selectTests([operations], operationsReadBy, gateKnown, {}, {}, {}, inputs)
    check("a header a gate script reads selects the gate's stanza beside the tests that compile it",
          (sorted(selected), nothing), (["shell_test_only_operations", "test_runtime"], []))
    selected, _ = selectTests([operations], operationsReadBy, gateKnown, {}, {}, {}, {})
    check("and without the script's inputs the stanza is not selected, which is how it was missed",
          sorted(selected), ["test_runtime"])
    selected, nothing = selectTests(["Source/Platform/Host/Daemon.cpp"], {}, gateKnown, {}, {}, {}, inputs)
    check("a source under a directory the script searches selects the stanza",
          (sorted(selected), nothing), (["shell_test_only_operations"], []))
    selected, nothing = selectTests(["Source/PlatformOther/File.cpp", "Source/Core/Commented.h"], {}, gateKnown, {}, {}, {}, inputs)
    check("a sibling directory with the same prefix and a path only a comment names reach nothing",
          (sorted(selected), nothing), ([], ["Source/PlatformOther/File.cpp", "Source/Core/Commented.h"]))
    selected, _ = selectTests(["Scripts/check-test-only-operations.py", "Scripts/check-retired.sh"], {}, gateKnown, {}, {}, {}, inputs)
    check("a changed gate script selects the stanza that runs it",
          sorted(selected), ["shell_retired_gate", "shell_test_only_operations"])

    # A stanza that builds tests by a glob. A
    # change that reaches one of those tests through a header selects the lane
    # with it; a test the glob does not match, and a file in a directory below
    # the glob's, do not.
    laneRecipes = stanzaRecipes([
        'if wanted sanitized_sync && command -v cmake >/dev/null 2>&1; then\n',
        '            "$ROOT/Tests/Core/Protocol/Sync/test_sync_*.cpp"\n',
        'fi\n',
    ])
    check("a recipe's glob is kept as the recipe spells it",
          {name: sorted(files) for name, (files, _) in laneRecipes.items()},
          {"sanitized_sync": ["Tests/Core/Protocol/Sync/test_sync_*.cpp"]})
    laneFile = {
        "Source/Core/Protocol/Sync/Connection.h": {
            "Tests/CMakeFiles/test_sync_probe.dir/Core/Protocol/Sync/test_sync_probe.cpp.o",
            "Tests/CMakeFiles/test_keys.dir/Core/Keys/test_keys.cpp.o",
            "CMakeFiles/ubiquitty-behaviour.dir/Source/Core/Protocol/Sync/Connection.cpp.o",
        },
        "Source/Core/Keys/Keymap.h": {"Tests/CMakeFiles/test_keys.dir/Core/Keys/test_keys.cpp.o"},
    }
    laneKnown = {"test_sync_probe", "test_keys", "shell_sanitized_sync"}
    selected, _ = selectTests(["Source/Core/Protocol/Sync/Connection.h"], laneFile, laneKnown, {}, laneRecipes, {})
    check("a header one of a lane's tests reads selects the lane with the test",
          sorted(selected), ["shell_sanitized_sync", "test_keys", "test_sync_probe"])
    selected, _ = selectTests(["Source/Core/Keys/Keymap.h"], laneFile, laneKnown, {}, laneRecipes, {})
    check("a header only a test outside the lane's glob reads selects no lane",
          sorted(selected), ["test_keys"])
    selected, _ = selectTests(["Tests/Core/Protocol/Sync/test_sync_new.cpp"], {}, laneKnown, {}, laneRecipes, {})
    check("a new test source the glob matches selects the lane before it has an object",
          sorted(selected), ["shell_sanitized_sync"])
    selected, nothing = selectTests(["Tests/Core/Protocol/Sync/Deeper/test_sync_x.cpp"], {}, laneKnown, {}, laneRecipes, {})
    check("a glob does not reach below its own directory",
          (sorted(selected), nothing), ([], ["Tests/Core/Protocol/Sync/Deeper/test_sync_x.cpp"]))

    # A stanza that opens by declining off macOS is one the selection leaves
    # out there and keeps on macOS; a stanza that opens any other way is in
    # neither answer.
    declining = [
        'if wanted sanitized_sync && [ "$(uname -s)" != "Darwin" ]; then\n',
        '    ubq_skip "not run here"\n',
        'elif wanted sanitized_sync && command -v cmake >/dev/null 2>&1; then\n',
        'fi\n',
        'if wanted host_cli; then\n',
        'fi\n',
    ]
    check("a stanza that opens by declining off macOS is named on Linux",
          sorted(stanzasDecliningOn(declining, "Linux")), ["sanitized_sync"])
    check("and none is on macOS, where it runs", sorted(stanzasDecliningOn(declining, "Darwin")), [])
    check("and it is still one stanza with one recipe",
          sorted(stanzaRecipes(declining)), ["host_cli", "sanitized_sync"])

    # The instrument-absent case, which is the one that reads as a green run: a
    # directory with no ninja database must answer None, so main refuses on it
    # rather than selecting nothing from it.
    with tempfile.TemporaryDirectory() as empty:
        check("a directory with no ninja database answers None",
              dependencyMap(empty), None)

    if failures:
        for failure in failures:
            print(f"check-tests-reaching-the-diff --self-test: {failure}", file=sys.stderr)
        print(f"check-tests-reaching-the-diff --self-test: {len(failures)} case(s) failed",
              file=sys.stderr)
        return 1
    print("check-tests-reaching-the-diff --self-test: ok (36 case(s))", file=sys.stderr)
    return 0


def registeredTests(buildDirectory):
    """The test names ctest knows in this directory."""
    result = subprocess.run(["ctest", "-N"], cwd=buildDirectory,
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    found = set()
    for line in result.stdout.split("\n"):
        match = re.match(r"\s*Test\s+#\d+:\s+(\S+)", line)
        if match:
            found.add(match.group(1))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", nargs="?")
    parser.add_argument("--changed-from", default="origin/main")
    parser.add_argument("--jobs", default="4")
    parser.add_argument("--agent-build",
                        help="the agent's build tree (default: agent/ beside the build "
                             "directory's parent, where Scripts/build-agent.sh puts it)")
    parser.add_argument("--list", action="store_true",
                        help="name the selected tests and exit without running them")
    parser.add_argument("--self-test", action="store_true",
                        help="run the selection rule against planted maps and exit")
    options = parser.parse_args()

    if options.self_test:
        return selfTest()

    if not options.build:
        print("check-tests-reaching-the-diff: name a build directory, or --self-test",
              file=sys.stderr)
        return 1
    if not os.path.isdir(options.build):
        print(f"check-tests-reaching-the-diff: no build directory at {options.build}", file=sys.stderr)
        return 1

    changed, failure = changedFiles(options.changed_from)
    if changed is None:
        print(f"check-tests-reaching-the-diff: {failure}", file=sys.stderr)
        return 1
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True).stdout.strip()
    # The driver and the stanza files it sources hold the stanzas between them.
    driverLines = []
    for name in ["run-tests.sh"] + sorted(
            os.path.basename(path) for path in glob.glob(os.path.join(root, "Scripts", "run-tests-stanzas-*.sh"))):
        try:
            with open(os.path.join(root, "Scripts", name), encoding="utf-8") as driver:
                driverLines += driver.readlines()
        except OSError:
            continue
    recipes = stanzaRecipes(driverLines)

    def readScript(name):
        try:
            with open(os.path.join(root, name), encoding="utf-8") as script:
                return script.read()
        except OSError:
            return None

    inputs = gateInputs(gateStanzas(scriptsRun(driverLines), recipes), readScript)
    # A source or header a compiler opens, a file a stanza's recipe names, or
    # one a stanza's gate script reads: a script reaches no object and is still
    # that stanza's subject.
    named = set().union(*(files for files, _ in recipes.values())) if recipes else set()
    compiled = sorted(path for path in changed if path.endswith(COMPILED_SUFFIXES) or path in named
                      or any(isUnder(path, spelled) for reads in inputs.values() for spelled in reads))
    if not compiled:
        print(f"check-tests-reaching-the-diff: ok (this branch changes no source or header "
              f"the tests could read, and no file a stanza's recipe names; {len(changed)} changed path(s))",
              file=sys.stderr)
        return 0

    byFile = dependencyMap(options.build)
    if byFile is None:
        print("check-tests-reaching-the-diff: REFUSED — `ninja -t deps` returned nothing in "
              f"{options.build}.", file=sys.stderr)
        print("  The dependency database is the whole instrument here. Selecting nothing "
              "from an absent one is indistinguishable from a green run, so this refuses "
              "rather than report one. Build the directory first (Scripts/build-tests.sh).",
              file=sys.stderr)
        return 1

    known = registeredTests(options.build)
    if known is None:
        print(f"check-tests-reaching-the-diff: REFUSED — `ctest -N` failed in {options.build}",
              file=sys.stderr)
        return 1

    declined = {STANZA_PREFIX + name for name in stanzasDecliningOn(driverLines, os.uname().sysname)} & known
    if declined:
        print(f"check-tests-reaching-the-diff: {len(declined)} stanza(s) decline on this host by "
              f"their opening line and are not selected here: {', '.join(sorted(declined))}", file=sys.stderr)
        known -= declined

    covering = coveringTests(os.path.join(root, "Tests", "coverage", "covering-tests.tsv"))
    # What the agent's build read, from its own database. A tree that is not
    # built answers for nothing, and the stanzas that run its binary would
    # decline for want of it anyway; said, so that nothing selected for them
    # is not read as nothing reaching them.
    agentBuild = options.agent_build or os.path.normpath(os.path.join(options.build, "..", "..", "agent"))
    agentReads = dependencyMap(agentBuild) if os.path.isdir(agentBuild) else None
    if agentReads is None:
        print(f"check-tests-reaching-the-diff: the agent's build tree at {agentBuild} has no "
              f"dependency database, so 0 of the stanzas that run `ubq` are selected by what "
              f"its build reads (Scripts/build-agent.sh builds it)", file=sys.stderr)
    productReads = {"agent": set(agentReads or ())}
    selected, reachedNothing = selectTests(
        compiled, byFile, known, covering, recipes, productReads, inputs, sharedTargetStanzas(driverLines))

    for path in reachedNothing:
        print(f"check-tests-reaching-the-diff: {path} reaches no object in this build and no "
              f"test in the coverage index — new, unused, or covered by nothing recorded",
              file=sys.stderr)

    if not selected:
        print(f"check-tests-reaching-the-diff: ok (no test in this build reads any of the "
              f"{len(compiled)} changed source(s) or header(s))", file=sys.stderr)
        return 0

    names = sorted(selected)
    print(f"check-tests-reaching-the-diff: {len(names)} of {len(known)} test(s) read what this "
          f"branch changed", file=sys.stderr)
    for name in names:
        print(f"    {name}", file=sys.stderr)
    if options.list:
        for name in names:
            print(name)
        return 0

    pattern = "^(" + "|".join(re.escape(name) for name in names) + ")$"
    result = subprocess.run(["ctest", "-R", pattern, "-j", str(options.jobs),
                             "--output-on-failure"],
                            cwd=options.build, capture_output=True, text=True)
    sys.stderr.write(result.stdout)
    sys.stderr.write(result.stderr)

    # Which entries ctest actually RAN, read from its per-test lines rather than
    # from its total. A `-R` alternation that matched fewer entries than it
    # named would otherwise report a green over a set this never ran, and the
    # total cannot tell that apart from the fixtures ctest adds on its own.
    ran = {match.group(1) for line in result.stdout.split("\n")
           if (match := TEST_RAN.match(line))}
    if not ran:
        print("check-tests-reaching-the-diff: REFUSED — ctest printed no per-test line, so "
              "what it ran cannot be read", file=sys.stderr)
        return 1
    missing = sorted(set(names) - ran)
    if missing:
        print(f"check-tests-reaching-the-diff: REFUSED — {len(missing)} selected test(s) never "
              f"ran, so the verdict does not describe the selection:", file=sys.stderr)
        for name in missing:
            print(f"    {name}", file=sys.stderr)
        return 1
    # Everything else ctest ran is a fixture it required, named so the count in
    # its summary does not read as a selection this did not make.
    fixtures = sorted(ran - set(names))
    if fixtures:
        print(f"check-tests-reaching-the-diff: ctest also ran {len(fixtures)} required "
              f"fixture(s): {', '.join(fixtures)}", file=sys.stderr)
    if result.returncode != 0:
        print(f"check-tests-reaching-the-diff: FAILED — {len(names)} test(s) reaching this "
              f"branch's changes, and ctest reports a failure above", file=sys.stderr)
        return 1
    print(f"check-tests-reaching-the-diff: ok (all {len(names)} test(s) reaching this branch's "
          f"changes ran and passed)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
