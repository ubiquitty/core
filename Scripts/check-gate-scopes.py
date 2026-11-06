#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-gate-scopes.py — a scoped gate declares every input it reaches.

`run_scoped` in the Scripts/pre-push-gates*.sh files keys a gate's cached verdict on the
content of its declared paths and nothing else. That key is sound exactly as far
as the declaration is true: a gate that reads a file outside its scope keeps a
passing verdict after that file changes, and the gate reports `cached` while
covering nothing. The failure is silent by construction, which is why it gets a
gate of its own rather than a review convention.

So this walks each scoped gate's command, follows the scripts it invokes, and
reports every repository path they name that the declaration does not cover.

A CMake file the scope declares, by its own path or under a declared directory,
is followed through the modules it include()s, to any depth: the root
CMakeLists.txt reads Core's toolchain, optimisation and library modules from
Scripts/cmake, and Tests/CMakeLists.txt reads the test registration module
there, so an edit to one of them changes the build the gate configures as
surely as an edit to the file that includes it does.

The bound it reports is a lower one. It reads the gate's command and the scripts
that command names, one hop, plus literal driver lists. It sees a spelled path
and misses a computed one — `"$ROOT/$module/x.h"` is invisible here, as is anything a
file it does not open goes on to include. A gate can still go stale through a
path this cannot see; what it removes is the class that is merely unnoticed,
which is the one that has actually happened here. The stronger check is to
deny the reads outright: `sandbox-exec` can run a gate with every repository read
outside its scope refused, and an undeclared input then fails the run instead of
being cached. That costs a full build per gate to exercise, so it belongs in a
periodic audit rather than in the push path, and it is not wired here.
"""

import contextlib
import glob
import io
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots

ROOT = ubq_roots.subject_root()


def gate_files(root):
    """The files that list a repository's pre-push gates.

    The repository's own Scripts/pre-push-gates*.sh, and the gate files of the
    libraries it sources from their roots, each once.
    """
    found = []
    candidates = sorted(glob.glob(os.path.join(root, "Scripts", "pre-push-gates*.sh")))
    # A library's gate file lists gates its own repository runs, so it is read
    # where the checkout holds that repository.
    if "core" in ubq_roots.repositories(root):
        candidates.append(os.path.join(ubq_roots.core_root(root), "Scripts", "pre-push-gates-core.sh"))
    if "kit" in ubq_roots.repositories(root):
        candidates.append(os.path.join(ubq_roots.kit_root(root), "Scripts", "pre-push-gates-kit.sh"))
    for path in candidates:
        if os.path.isfile(path) and path not in found:
            found.append(path)
    return found


GATES = gate_files(ROOT)

# `run_scoped <name> <scope> <command...>`, the name and scope quoted, the scope
# possibly continued across lines with a trailing backslash.
INVOCATION = re.compile(
    r'run_scoped\s+"(?P<name>[^"]+)"\s*\\?\s*\n?\s*"(?P<scope>[^"]+)"\s*\\?\s*\n?\s*(?P<command>[^\n]+)')

# A repository path spelled in a script. The leading directories are the ones
# this tree keeps its inputs in; anything else is not a path a gate can read.
#
# The dot is absent from the guard on purpose. A gate command is written
# `./Scripts/build-host.sh`, and excluding a preceding `.` rejected every one of
# them — the checker then reached no script, found nothing, and reported that
# every scope was complete. A gate that cannot fail is the thing this file exists
# to prevent, so the guard admits the leading `./` and the match starts at the
# directory name.
REPOSITORY_PATH = re.compile(
    r'(?<![\w-])(?:\./)?(?:Scripts|Source|Tests|Tools|Docs|Support|Samples|Resources)/[A-Za-z0-9_./+-]+')


# A comment names a path the script does not read. Every gate script here opens
# with a prose header citing the design notes it implements, so counting those
# put twenty Docs/ entries under the Linux gate — none of which the compile can
# reach. A rule that fires on correct prose is one people learn to ignore.
COMMENT = re.compile(r'(^|\s)#.*$', re.M)


def roots():
    """The checkouts a gate's paths may live in: the subject's, then the libraries' it carries."""
    found = [ROOT]
    for library in (ubq_roots.core_root(ROOT), ubq_roots.kit_root(ROOT)):
        if library not in found:
            found.append(library)
    return found


def resolve(path):
    """The file or directory a repository-relative path names, in the checkout that holds it."""
    for root in roots():
        candidate = os.path.join(root, path)
        if os.path.exists(candidate):
            return candidate
    return os.path.join(ROOT, path)


def spelled_paths(text):
    """The repository paths named in a blob of script text, comments excluded.

    - Parameter text: the script's contents.
    - Returns: the set of paths, each relative to the repository root.
    """
    return {match[2:] if match.startswith("./") else match
            for match in REPOSITORY_PATH.findall(COMMENT.sub("", text))}

# A script the gate's command runs, so its own references are the gate's too.
SCRIPT_SUFFIXES = (".sh", ".py", ".cmake")


def referenced_paths(path):
    """The repository paths this file names directly.

    One hop, not a transitive walk. Scripts/run-linux.sh is a container wrapper
    that runs whatever it is handed, so following it reaches every build script
    in the tree and, through their prose headers, most of Docs/ — forty findings
    for a gate that compiles one frontend. Unbounded reach through a wrapper says
    nothing about what the gate read, and a gate whose output has to be filtered
    by hand is one nobody runs.

    A literal script list dispatched through `$ROOT/Scripts/$variable` is also
    followed: the list names the inputs even though the invocation computes
    their paths. Other computed paths remain outside this check.

    - Parameter path: a repository-relative file to read.
    - Returns: the set of repository-relative paths it names.
    """
    absolute = resolve(path)
    if not os.path.isfile(absolute):
        return set()
    try:
        with open(absolute, "r", errors="replace") as handle:
            source = COMMENT.sub("", handle.read())
            paths = spelled_paths(source)
            # A literal script list executed through $ROOT/Scripts/$variable.
            if re.search(r'\$ROOT/Scripts/\$(?:\{)?[A-Za-z_]', source):
                for body in re.findall(r'\b[A-Za-z_][A-Za-z0-9_]*=\((.*?)\)', source, re.S):
                    for driver in re.findall(r'(?<![\w/.-])([\w-]+\.sh)\b', body):
                        relative = "Scripts/" + driver
                        paths.add(relative)
                        absolute_driver = resolve(relative)
                        if os.path.isfile(absolute_driver):
                            paths |= spelled_paths(Path(absolute_driver).read_text())
            return paths
    except OSError:
        return set()


# `include(<path>.cmake)`, quoted or not. A module named by bare name, such as
# CheckIPOSupported, is CMake's own and not an input of this tree.
CMAKE_INCLUDE = re.compile(r'\binclude\s*\(\s*"?([^")\s]+\.cmake)"?\s*\)')

# Every source root defaults to this checkout, so a module spelled under one is
# this checkout's file.
CMAKE_ROOT_VARIABLES = ("${UBIQUITTY_CORE_SOURCE_DIR}", "${UBIQUITTY_KIT_SOURCE_DIR}",
                        "${CMAKE_SOURCE_DIR}")


def cmake_includes(path, closure):
    """Adds the CMake modules a CMake file includes, and theirs, to a closure.

    - Parameters:
      - path: a repository-relative CMake file.
      - closure: the repository-relative modules found so far; extended in place.
    """
    absolute = resolve(path)
    if not os.path.isfile(absolute):
        return
    try:
        with open(absolute, "r", errors="replace") as handle:
            source = COMMENT.sub("", handle.read())
    except OSError:
        return
    directory = os.path.dirname(path)
    for spelled in CMAKE_INCLUDE.findall(source):
        for variable in CMAKE_ROOT_VARIABLES:
            spelled = spelled.replace(variable, ".")
        for variable in ("${CMAKE_CURRENT_LIST_DIR}", "${CMAKE_CURRENT_SOURCE_DIR}"):
            spelled = spelled.replace(variable, directory or ".")
        if "${" in spelled:
            continue
        module = os.path.normpath(spelled)
        if module not in closure:
            closure.add(module)
            cmake_includes(module, closure)


def declared_cmake_files(declared):
    """The CMake files a declared pathspec holds.

    - Parameter declared: a repository-relative pathspec from a gate's scope.
    - Returns: the pathspec itself when it is a CMakeLists.txt or a .cmake file,
      or every such file under it when it is a directory, each one
      repository-relative.
    """
    mounts = [(root, os.path.join(root, declared)) for root in roots()
            if os.path.isdir(os.path.join(root, declared))]
    if not mounts:
        if os.path.basename(declared) == "CMakeLists.txt" or declared.endswith(".cmake"):
            yield declared
        return
    for base, absolute in mounts:
        # A checkout's files are reported as it spells them, whichever holds them.
        for directory, subdirectories, files in os.walk(absolute):
            subdirectories[:] = sorted(name for name in subdirectories if not name.startswith("."))
            for name in sorted(files):
                if name == "CMakeLists.txt" or name.endswith(".cmake"):
                    yield os.path.relpath(os.path.join(directory, name), base)


def held(path):
    """Whether a declared pathspec names something this checkout can read.

    A gate is named by its path in the repository that owns it, so a scope may
    spell a path that only a library the checkout carries holds. Such a path
    keys nothing here: the verdict cache's environment already carries the
    library's commit and every uncommitted change in it.

    - Parameter path: a repository-relative pathspec from a gate's scope.
    """
    return any(os.path.exists(os.path.join(root, path))
               for root in (ROOT, ubq_roots.core_root(ROOT), ubq_roots.kit_root(ROOT)))


def covers(scope, path):
    """Whether a declared scope contains a path.

    - Parameters:
      - scope: the declared pathspecs, already split.
      - path: a repository-relative path.
    - Returns: True when some pathspec is the path or a directory above it.
    """
    for spec in scope:
        if path == spec or path.startswith(spec.rstrip("/") + "/"):
            return True
    return False


def main():
    source = ""
    for gates in GATES:
        with open(gates, "r", errors="replace") as handle:
            source += handle.read() + "\n"

    invocations = list(INVOCATION.finditer(source))
    if not invocations:
        print("check-gate-scopes: REFUSED — no run_scoped invocation parsed from "
              "Scripts/pre-push-gates*.sh. A driver that stopped using scopes and one "
              "this parser stopped reading print the same verdict, so this is not a "
              "pass.", file=sys.stderr)
        return 2

    findings = []
    for match in invocations:
        name = match.group("name")
        scope = match.group("scope").split()
        command = match.group("command")

        entries = [p for p in spelled_paths(command)
                   if p.endswith(SCRIPT_SUFFIXES)]
        invalid = [p for p in scope if not held(p)]
        if invalid:
            findings.append((name + " (scope paths do not exist)", invalid))
        reached = set()
        for entry in entries:
            reached |= {entry}
            reached |= referenced_paths(entry)
        modules = set()
        for declared in scope:
            for cmake in declared_cmake_files(declared):
                cmake_includes(cmake, modules)
        reached |= modules

        # Only paths that exist: a spelled string that names nothing is not an input.
        # A FILE, not a directory: a bare `Source/Platform` match is the head of
        # a path the script builds at runtime, and the file it finally opens is
        # the thing this cannot see either way.
        missing = sorted(p for p in reached
                         if os.path.isfile(resolve(p)) and not covers(scope, p))
        if missing:
            findings.append((name, missing))

    if findings:
        print("check-gate-scopes: a scoped gate reaches inputs its declaration "
              "does not cover:", file=sys.stderr)
        for name, missing in findings:
            print("  %s" % name, file=sys.stderr)
            for path in missing:
                print("      %s" % path, file=sys.stderr)
        print("\n  Add each to that gate's scope in Scripts/pre-push-gates.sh, or the "
              "gate\n  reports `cached` after the file changes and covers nothing.",
              file=sys.stderr)
        return 1

    print("check-gate-scopes: ok (%d scoped gate(s), every reachable input declared; "
          "spelled paths only — a computed path is not visible here)" % len(invocations))
    return 0


def self_test():
    global ROOT, GATES
    old_root, old_gates = ROOT, GATES
    with tempfile.TemporaryDirectory() as directory:
        ROOT = directory
        GATES = [os.path.join(directory, "Scripts/pre-push-gates.sh")]
        scripts = Path(directory) / "Scripts"
        scripts.mkdir()
        (scripts / "runner.sh").write_text('CHECKS=(check-driver.sh)\n"$ROOT/Scripts/$check"\n')
        (scripts / "check-driver.sh").write_text('cat "$ROOT/Source/Renderer.swift"\n')
        (Path(directory) / "Source").mkdir()
        (Path(directory) / "Source/Renderer.swift").touch()
        (Path(directory) / "CMakeLists.txt").write_text(
            'include("${UBIQUITTY_CORE_SOURCE_DIR}/cmake/Module.cmake")\ninclude(CheckIPOSupported)\n')
        (Path(directory) / "cmake").mkdir()
        (Path(directory) / "cmake/Module.cmake").write_text('include("${CMAKE_CURRENT_LIST_DIR}/Nested.cmake")\n')
        (Path(directory) / "cmake/Nested.cmake").touch()
        (Path(directory) / "Tests").mkdir()
        (Path(directory) / "Tests/CMakeLists.txt").write_text(
            'include(${UBIQUITTY_CORE_SOURCE_DIR}/cmake/Tests.cmake)\n')
        (Path(directory) / "cmake/Tests.cmake").touch()
        (Path(directory) / "Libraries/ubiquitty-core/Resources/fonts").mkdir(parents=True)
        try:
            environment = {name: os.environ.pop(name) for name in ("UBQ_CORE_ROOT", "UBQ_KIT_ROOT") if name in os.environ}
            for scope, expected in (("Scripts/runner.sh", 1),
                                    ("Scripts Source Missing", 1),
                                    ("Scripts Source", 0),
                                    ("Scripts Source CMakeLists.txt", 1),
                                    ("Scripts Source CMakeLists.txt cmake/Module.cmake", 1),
                                    ("Scripts Source CMakeLists.txt cmake", 0),
                                    ("Scripts Source Tests", 1),
                                    ("Scripts Source Tests cmake/Tests.cmake", 0),
                                    ("Scripts Source Resources/fonts", 0),
                                    ("Scripts Source Resources/missing", 1)):
                Path(GATES[0]).write_text(f'run_scoped "fixture" "{scope}" ./Scripts/runner.sh\n')
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    actual = main()
                if actual != expected:
                    print(f"check-gate-scopes: self-test failed for {scope}", file=sys.stderr)
                    return 1
        finally:
            ROOT, GATES = old_root, old_gates
            os.environ.update(environment)
    print("check-gate-scopes: self-test ok (driver lists, nonexistent scope paths, paths a library holds "
          "and CMake includes, from a declared file or directory)")
    return 0


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv else main())
