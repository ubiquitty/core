#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-quic-mandatory.py — shipping native builds include QUIC.

QUIC is how every native frontend reaches a host, and the daemon that terminates
it is bundled into the shipped app. A build that quietly drops it produces
something that runs, answers, and cannot reach a host the way the product does —
which is not a failure any test of behaviour catches, because the binary is
correct for a transport nobody ships.

One target genuinely cannot have it. A browser exposes no raw UDP socket to
wasm, so ngtcp2 has nothing to send on, and AWS-LC wants a native platform
underneath; the web frontend reaches a host through the page's own socket
instead. That is a property of the sandbox rather than a preference, so the two
wasm configures pass -DUBIQUITTY_QUIC=OFF and are the only ones allowed to.

Three things are compared, because the guarantee needs all three and any one of
them can move without the others noticing:

  - the option still defaults ON, so a configure that says nothing gets QUIC;
    it is declared in the root CMakeLists.txt or a build module under
    Scripts/cmake, and both are read
  - the macOS Swift importer still defines it, in the Kit's Swift flags
    (Scripts/cmake/UbiquittyKit.cmake), and neither they nor the macOS app's
    list expand those flags ungrouped
  - the host still forces it ON, so the daemon in the bundle cannot lose it
  - only the wasm pair and the named in-memory regression gate turn it off

A README or a comment may name the flag: prose describing how to configure a
Core-only tree is not a build that ships. Only an actual -D on a cmake line
counts, which is the difference between documenting the escape hatch and taking
it.
"""

import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots

# The configures that may turn QUIC off, and their nonshipping boundaries.
ALLOWED = {
    "Scripts/wasm-smoke.sh":
        "wasm: no UDP socket in a browser, and AWS-LC wants a native platform",
    "Scripts/build-web.sh":
        "wasm: the web frontend reaches a host through the page's own socket",
    "Scripts/build-core-wasm.sh":
        "wasm: the libraries the web frontend links, built without the page",
    "Scripts/check-sync-memory.py":
        "nonshipping Core tests: exact reviewed in-memory targets, no transport or Host execution",
}

# Files where the flag is INPUT to a test rather than a configure the build
# takes. A test proving the flag gets overridden has to name it, exactly as this
# gate has to name what it detects; reading either as a build reports the text
# rather than the tree. Membership here is not permission to ship QUIC off --
# these files configure nothing.
TEST_INPUTS = {
    "Scripts/test-ci-batteries.py":
        "feeds the flag in to assert test mode overrides a conflicting option",
}

# An actual flag on a configure line, not the flag named in prose.
FLAG_OFF = re.compile(r"-DUBIQUITTY_QUIC=OFF")
OPTION_ON = re.compile(r'^\s*option\(UBIQUITTY_QUIC\s+"[^"]*"\s+ON\s*\)', re.M)
HOST_FORCES_ON = re.compile(r"-DUBIQUITTY_QUIC=ON")


def repositoryRoot():
    """The checkout this gate is reading."""
    return ubq_roots.subject_root()


def scriptsThatTurnItOff(root):
    """Every build script passing -DUBIQUITTY_QUIC=OFF, as repo-relative paths."""
    found = {}
    scripts = os.path.join(root, "Scripts")
    for base, _, names in os.walk(scripts):
        for name in names:
            if not name.endswith((".sh", ".py")):
                continue
            path = os.path.join(base, name)
            # This gate spells the flag it looks for, in its pattern and in the
            # prose explaining it. Reading itself would report the detector as
            # the thing detected -- true of the text, false of the tree.
            if os.path.abspath(path) == os.path.abspath(__file__):
                continue
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    lines = handle.read().split("\n")
            except OSError:
                continue
            for number, line in enumerate(lines, start=1):
                stripped = line.lstrip()
                if stripped.startswith("#"):
                    continue  # prose naming the flag is not a build taking it
                if FLAG_OFF.search(line):
                    found.setdefault(os.path.relpath(path, root), []).append(number)
    return found


def problemsIn(root, kitRoot=None, coreRoot=None):
    """Every way this root fails the rule, as printable lines.

    Separated from main so the self-test can plant each violation in a tree of
    its own rather than against the checkout, which cannot be made to fail
    without editing the repository it is checking.

    - Parameter root: The tree to read.
    - Parameter kitRoot: Where the Kit's modules are; the Kit root the environment
      and the tree's Libraries/ name by default.
    - Parameter coreRoot: Where Core's build modules and host build are; the Core
      root the environment and the tree's Libraries/ name by default.
    - Returns: A list of problems, empty when the root satisfies the rule.
    """
    problems = []

    # The default is declared in Core's build modules, which a repository that
    # carries Core as a library includes from Core's checkout.
    coreRoot = coreRoot or ubq_roots.core_root(root)
    cmakeFiles = [os.path.join(root, "CMakeLists.txt")] + sorted(
        glob.glob(os.path.join(root, "Scripts", "cmake", "*.cmake")))
    if coreRoot != root:
        cmakeFiles += sorted(glob.glob(os.path.join(coreRoot, "Scripts", "cmake", "*.cmake")))
    cmake = ""
    for cmakeFile in cmakeFiles:
        try:
            with open(cmakeFile, "r", encoding="utf-8", errors="replace") as handle:
                cmake += handle.read() + "\n"
        except OSError:
            # A list, like every other outcome here: this returns problems, and an
            # int would reach len() in the self-test as a length rather than a count.
            return [os.path.relpath(cmakeFile, root) + " is unreadable, so the default cannot be read"]

    if not OPTION_ON.search(cmake):
        problems.append(
            "neither CMakeLists.txt nor a module under Scripts/cmake defaults UBIQUITTY_QUIC\n"
            "  to ON — a configure that says nothing about QUIC would build without it")

    host = os.path.join(coreRoot, "Scripts", "build-host.sh")
    try:
        with open(host, "r", encoding="utf-8", errors="replace") as handle:
            if not HOST_FORCES_ON.search(handle.read()):
                problems.append(
                    "Scripts/build-host.sh no longer passes -DUBIQUITTY_QUIC=ON — the daemon\n"
                    "  bundled into the app is what terminates QUIC, so it cannot inherit a default")
    except OSError:
        problems.append("Scripts/build-host.sh is unreadable")

    # The Kit's module computes the macOS Swift flags, and both it and the macOS
    # app's list compile with them. Parity is a fact about the app, which holds the
    # macOS list and reaches the Kit's module through the Kit's root; the other
    # repositories hold one half or neither, so it is the app's to check.
    swiftFlags = os.path.join(kitRoot or ubq_roots.kit_root(root), "Scripts", "cmake", "UbiquittyKit.cmake")
    macos = os.path.join(root, "Source", "Platform", "macOS", "CMakeLists.txt")
    native = {}
    for path in ((swiftFlags, macos) if "app" in ubq_roots.repositories(root) else ()):
        try:
            with open(path, "r", encoding="utf-8") as handle:
                native[path] = handle.read()
        except OSError:
            problems.append(os.path.relpath(path, root)
                            + " is unreadable; Swift transport parity cannot be checked")
    if swiftFlags in native and not re.search(
            r"list\(APPEND\s+UBIQUITTY_SWIFT_FLAGS\s+-Xcc\s+-DUBIQUITTY_QUIC=1(?=\s|\))", native[swiftFlags]):
        problems.append("macOS Swift C++ importer lacks UBIQUITTY_QUIC=1 in the Kit's Swift flags "
                        "(Scripts/cmake/UbiquittyKit.cmake); inline session factories would silently select SSH")
    for path, text in sorted(native.items()):
        if "SHELL:${UBIQUITTY_SWIFT_FLAGS}>" in text:
            problems.append(os.path.relpath(path, root)
                            + ": macOS Swift options expand an ungrouped CMake list; repeated -Xcc arguments are deduplicated")

    off = scriptsThatTurnItOff(root)
    unexpected = {path: lines for path, lines in off.items()
                  if path not in ALLOWED and path not in TEST_INPUTS}
    for path, lines in sorted(unexpected.items()):
        problems.append(
            "%s turns QUIC off at line %s\n"
            "  Only the named wasm and nonshipping in-memory test configures may."
            % (path, ", ".join(str(n) for n in lines)))

    return problems


def selfTest():
    """Prove each arm of the rule fires, against trees built to trip it.

    A gate that cannot be made to fail is indistinguishable from one that is
    not looking, and this one reads three separate things -- so each is planted
    separately rather than trusting that one failing proves the others would.

    - Returns: 0 when every planted violation was caught and the clean tree was
      not, 1 otherwise.
    """
    import shutil
    import tempfile

    def build(root, *, optionOn=True, optionInModule=False, hostForcesOn=True, swiftImporter=True, swiftFeature="1", swiftInclude=False, deduplicatedForwarding=None, extraOff=None):
        os.makedirs(os.path.join(root, "Scripts", "cmake"), exist_ok=True)
        option = ('option(UBIQUITTY_QUIC "Build the QUIC transport (ngtcp2 + AWS-LC)" %s)\n'
                  % ("ON" if optionOn else "OFF"))
        with open(os.path.join(root, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
            handle.write("" if optionInModule else option)
        with open(os.path.join(root, "Scripts", "cmake", "UbiquittyToolchain.cmake"), "w", encoding="utf-8") as handle:
            handle.write(option if optionInModule else "")
        with open(os.path.join(root, "Scripts", "build-host.sh"), "w", encoding="utf-8") as handle:
            handle.write("cmake -S . -B out %s\n" % ("-DUBIQUITTY_QUIC=ON" if hostForcesOn else ""))
        native = os.path.join(root, "Source", "Platform", "macOS")
        os.makedirs(native, exist_ok=True)
        ungrouped = 'target_compile_options(example PRIVATE "$<$<COMPILE_LANGUAGE:Swift>:SHELL:${UBIQUITTY_SWIFT_FLAGS}>")\n'
        with open(os.path.join(root, "Scripts", "cmake", "UbiquittyKit.cmake"), "w", encoding="utf-8") as handle:
            if swiftImporter:
                include = '\n    "-I${quicPrefix}/include"' if swiftInclude else ""
                handle.write(f"list(APPEND UBIQUITTY_SWIFT_FLAGS -Xcc -DUBIQUITTY_QUIC={swiftFeature}{include})\n")
            if deduplicatedForwarding == "kit":
                handle.write(ungrouped)
        with open(os.path.join(native, "CMakeLists.txt"), "w", encoding="utf-8") as handle:
            if deduplicatedForwarding == "app":
                handle.write(ungrouped)
        for name in ("wasm-smoke.sh", "build-web.sh", "build-core-wasm.sh"):
            with open(os.path.join(root, "Scripts", name), "w", encoding="utf-8") as handle:
                handle.write("emcmake cmake -S . -B out -DUBIQUITTY_QUIC=OFF\n")
        if extraOff:
            with open(os.path.join(root, "Scripts", extraOff), "w", encoding="utf-8") as handle:
                handle.write("cmake -S . -B out -DUBIQUITTY_QUIC=OFF\n")

    cases = [
        ("a tree that satisfies the rule", {}, 0),
        ("Swift flags with a trailing include directory", {"swiftInclude": True}, 0),
        ("Swift importer with a different feature value", {"swiftFeature": "10"}, 1),
        ("CMake deduplicating Clang forwarding in the Kit's module", {"deduplicatedForwarding": "kit"}, 1),
        ("CMake deduplicating Clang forwarding in the macOS app's list", {"deduplicatedForwarding": "app"}, 1),
        ("Swift importer omitting the native feature", {"swiftImporter": False}, 1),
        ("the option defaulting to OFF", {"optionOn": False}, 1),
        ("the option declared ON in a build module", {"optionInModule": True}, 0),
        ("the option declared OFF in a build module", {"optionInModule": True, "optionOn": False}, 1),
        ("the host no longer forcing it ON", {"hostForcesOn": False}, 1),
        ("another script turning it off", {"extraOff": "build-tests.sh"}, 1),
        ("the named in-memory gate", {"extraOff": "check-sync-memory.py"}, 0),
        # The named test file is passed over...
        ("the named test input", {"extraOff": "test-ci-batteries.py"}, 0),
        # ...and being a test is not itself the exemption, or a real configure
        # could be hidden behind the naming convention.
        ("an unnamed test file turning it off", {"extraOff": "test-other.py"}, 1),
    ]
    failures = 0
    for label, kwargs, expected in cases:
        root = tempfile.mkdtemp(prefix="ubq-quic-gate-")
        try:
            build(root, **kwargs)
            found = len(problemsIn(root, kitRoot=root, coreRoot=root))
            if (found > 0) != (expected > 0):
                print("check-quic-mandatory --self-test: %s -> %d problem(s), expected %s"
                      % (label, found, "one or more" if expected else "none"), file=sys.stderr)
                failures += 1
        finally:
            shutil.rmtree(root, ignore_errors=True)

    if failures:
        print("check-quic-mandatory --self-test: %d arm(s) did not behave" % failures,
              file=sys.stderr)
        return 1
    print("check-quic-mandatory --self-test: ok (%d arms, each planted and caught)"
          % (len(cases) - 1))
    return 0


def main(argv):
    if argv and argv[0] in ("--self-test", "--selftest"):
        return selfTest()

    problems = problemsIn(repositoryRoot())
    off = scriptsThatTurnItOff(repositoryRoot())
    if problems:
        for problem in problems:
            print("check-quic-mandatory: " + problem, file=sys.stderr)
        print("", file=sys.stderr)
        print("  QUIC is how a native frontend reaches a host, and a build without it", file=sys.stderr)
        print("  runs and answers and simply cannot — which no test of behaviour catches,", file=sys.stderr)
        print("  because the binary is correct for a transport nobody ships.", file=sys.stderr)
        print("  A nonshipping or platform-limited target needs a named ALLOWED entry", file=sys.stderr)
        print("  with its boundary; native application and host builds require QUIC.", file=sys.stderr)
        return 1

    # Counted apart, because one number covering both would say a test file is a
    # configure -- and a summary that miscounts its own exemptions is how
    # an exemption grows unnoticed.
    configures = sum(1 for path in off if path in ALLOWED)
    testInputs = sum(1 for path in off if path in TEST_INPUTS)
    print("check-quic-mandatory: ok (option defaults ON, the host forces it, "
          "%d named wasm or nonshipping test configure(s) turn it off, "
          "%d test file(s) name the flag as input)" % (configures, testInputs))
    return 0


if __name__ == "__main__":
    # argv without the program name, so argv[0] is the first flag -- which is
    # what main() reads, and what every other gate here passes.
    sys.exit(main(sys.argv[1:]))
