#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-standalone-tools-compile.py — a tool with its own runner still parses.

`Tests/CMakeLists.txt` builds every `bench_*`/`perf_*` under `Tests/`, so one that
stops compiling fails a push. Its sibling rule — "a file nothing names is a file
nothing checks. The naming convention is the registration" — does not reach
`Tools/` subdirectories: that glob is `file(GLOB "Tools/*.cpp")`, not recursive.

Three tools sat behind that edge and did not compile:

    Tools/reflow-bench/reflow_bench.cpp        3 errors
    Tools/reflow-bench/throughput_bench.cpp    1 error
    Tools/sgr-profile/sgr_bench.cpp            1 error

Two Core changes they never tracked, stacked so the first hid the second.
`Ubiquitty::Terminal` stopped arriving transitively through
`Core/TerminalOperations.h`, and under that, `Ubiquitty::resize` had moved from
`Terminal*` to `Terminal&`. Each tool has a `run.sh` that compiles and runs it, so
nothing was orphaned — but a runner nobody runs between API changes is a check
that arrives when someone reaches for a bench and finds it broken.

Syntax-only, because a full build of these costs minutes: each runner builds a
portable-core archive to link against. `-fsyntax-only` needs no archive, no link
and no PAL stub, and it catches every break of this kind — a moved declaration, a
changed signature, a renamed type. What it cannot catch is a link error, and that
is stated rather than implied.

Subjects are derived, not listed: every tracked `.cpp` under `Tools/` that no
CMakeLists names. A tool CMake builds is already covered by the build.

Stdlib only.

    python3 Scripts/check-standalone-tools-compile.py
    python3 Scripts/check-standalone-tools-compile.py --self-test
"""

import os
import re
import shlex
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402
REPO = ubq_roots.subject_of(__file__)

# The flags the runners use, minus everything that needs a link. Kept in one
# place so a runner gaining a flag that changes what parses is a visible edit.
FLAGS = ["-std=c++23", "-fsyntax-only", "-DNDEBUG", "-fno-exceptions",
         "-fno-rtti", "-Wno-deprecated-declarations", "-I", "Source"]

# The compiler the runners use: each `run.sh` compiles with `${CXX:-c++}`. A bare
# `c++` older than Source/Base/ToolchainFloor.h allows fails every tool on the
# floor while the runner, handed CXX, builds them, and that verdict would be about
# the machine rather than the tools.
#
# Split into words the way CMake reads CXX, where a launcher or a flag may ride
# ahead of the compiler (`ccache g++`). Taken whole, that string is looked up as
# one executable, found nowhere, and the gate stands down on a machine that can
# compile.
COMPILER = shlex.split(os.environ.get("CXX") or "") or ["c++"]


def libraryIncludes():
    """The Source roots of the libraries this repository carries, which its tools include."""
    roots = [root for root in dict.fromkeys((ubq_roots.kit_root(REPO), ubq_roots.core_root(REPO)))
             if os.path.realpath(root) != os.path.realpath(REPO)]
    return [argument for root in roots for argument in ("-I", os.path.join(root, "Source"))]


def tracked(pattern):
    listing = subprocess.run(["git", "-C", REPO, "ls-files", pattern],
                             capture_output=True, text=True)
    return [p for p in listing.stdout.split("\n") if p]


def cmakeText():
    """Every CMakeLists in the tree, concatenated."""
    out = []
    for path in tracked("*CMakeLists.txt"):
        try:
            with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
                out.append(handle.read())
        except OSError:
            pass
    return "\n".join(out)


def subjects():
    """Tools/ sources no CMakeLists names, as repo-relative paths."""
    cmake = cmakeText()
    found = []
    for path in tracked("Tools/*.cpp") + tracked("Tools/**/*.cpp"):
        if path in found:
            continue
        base = os.path.basename(path)
        # A bare `Tools/*.cpp` glob covers the top level; a named source covers
        # itself. Either way CMake compiles it and the build is the check.
        if base in cmake or path in cmake:
            continue
        if re.search(r'Tools/\*\.cpp', cmake) and path.count("/") == 1:
            continue
        found.append(path)
    return sorted(found)


def compiles(path):
    """Whether one source parses. Returns (ok, first error line or '')."""
    result = subprocess.run(COMPILER + FLAGS + libraryIncludes() + [path], cwd=REPO,
                            capture_output=True, text=True)
    if result.returncode == 0:
        return True, ""
    for line in result.stderr.split("\n"):
        if "error:" in line:
            return False, line.strip()
    return False, result.stderr.strip().split("\n")[0] if result.stderr else "failed"


def selfTest():
    ok = True

    # A source CMake names is not a subject; one it does not is.
    cmake = 'add_executable(snap "Tools/snapshot/main.cpp")\nfile(GLOB X "Tools/*.cpp")'
    if "main.cpp" not in cmake:
        print("self-test: fixture wrong", file=sys.stderr)
        ok = False

    # The compiler probe reports a real failure rather than swallowing it.
    import tempfile
    with tempfile.TemporaryDirectory() as scratch:
        bad = os.path.join(scratch, "bad.cpp")
        with open(bad, "w", encoding="utf-8") as handle:
            handle.write("int main() { return notADeclaredThing; }\n")
        good, message = compiles(bad)
        if good or "error:" not in message:
            print("self-test: a file that cannot compile was reported as compiling",
                  file=sys.stderr)
            ok = False

        fine = os.path.join(scratch, "fine.cpp")
        with open(fine, "w", encoding="utf-8") as handle:
            handle.write("int main() { return 0; }\n")
        if not compiles(fine)[0]:
            print("self-test: a valid file was reported as broken", file=sys.stderr)
            ok = False

        # CXX names the compiler asked, as it does for the runners: one that
        # refuses everything fails the file the default compiler just parsed.
        refusing = os.path.join(scratch, "refusing-compiler")
        with open(refusing, "w", encoding="utf-8") as handle:
            handle.write("#!/bin/sh\necho 'error: refused by the stand-in' >&2\nexit 1\n")
        os.chmod(refusing, 0o755)
        answer = subprocess.run(
            [sys.executable, "-c",
             "import sys; sys.path.insert(0, %r); import importlib; "
             "gate = importlib.import_module('check-standalone-tools-compile'); "
             "print(*gate.COMPILER); print(gate.compiles(%r)[0])" % (HERE, fine)],
            env=dict(os.environ, CXX=refusing), cwd=REPO, capture_output=True, text=True)
        if answer.stdout.split() != [refusing, "False"]:
            print("self-test: CXX did not choose the compiler; the stand-in answered %r"
                  % answer.stdout.strip(), file=sys.stderr)
            ok = False

        # A CXX of several words is a launcher ahead of the compiler: the first
        # word is what must be on PATH, and every word reaches the command line.
        launcher = os.path.join(scratch, "launcher")
        with open(launcher, "w", encoding="utf-8") as handle:
            handle.write('#!/bin/sh\nexec "$@"\n')
        os.chmod(launcher, 0o755)
        answer = subprocess.run(
            [sys.executable, "-c",
             "import shutil, sys; sys.path.insert(0, %r); import importlib; "
             "gate = importlib.import_module('check-standalone-tools-compile'); "
             "print(len(gate.COMPILER)); print(shutil.which(gate.COMPILER[0]) is not None); "
             "print(gate.compiles(%r)[0])" % (HERE, fine)],
            env=dict(os.environ, CXX=shlex.join([launcher] + COMPILER)), cwd=REPO,
            capture_output=True, text=True)
        if answer.stdout.split() != [str(1 + len(COMPILER)), "True", "True"]:
            print("self-test: a launcher in CXX was not split from the compiler; the "
                  "gate answered %r" % (answer.stdout.strip() or answer.stderr.strip()),
                  file=sys.stderr)
            ok = False

    print("check-standalone-tools-compile: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    # shutil.which, not `command -v`: that is a shell builtin, so running it as
    # an executable fails on every machine and the gate stands down, reporting
    # SKIPPED, on the ones that can compile.
    if shutil.which(COMPILER[0]) is None:
        print("check-standalone-tools-compile: SKIPPED — no %s on PATH, so "
              "nothing was parsed" % COMPILER[0])
        return 0

    sources = subjects()
    if not sources and "core" not in ubq_roots.repositories(REPO):
        print("check-standalone-tools-compile: ok (this repository keeps no Tools/ source outside CMake)")
        return 0
    if not sources:
        print("check-standalone-tools-compile: FAILED — no Tools/ source is "
              "outside CMake, so this checked nothing. If the tools moved under "
              "CMake, delete this gate; the build covers them.", file=sys.stderr)
        return 1

    broken = []
    for path in sources:
        ok, message = compiles(path)
        if not ok:
            broken.append("%s — %s" % (path, message))

    if broken:
        status = gate_report.failed("check-standalone-tools-compile",
                                    "standalone tool(s) that do not parse", broken)
        print("\n  These have their own run.sh and no CMake target, so a Core API\n"
              "  change reaches them only when someone runs a bench. Parsed with\n"
              "  the runners' own compiler and flags: %s %s" % (shlex.join(COMPILER), " ".join(FLAGS)),
              file=sys.stderr)
        return status

    print("check-standalone-tools-compile: ok (%d tool(s) outside CMake parse "
          "against Source/; syntax only, so a link error is not covered)" % len(sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())
