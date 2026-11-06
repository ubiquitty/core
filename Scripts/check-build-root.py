#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-build-root.py — one build root, spelled the same by both languages.

Scripts/build-root.sh and Scripts/build_root.py derive where a checkout builds.
They must agree exactly. A python gate that fingerprints one directory while the
shell builds into another reports a verdict about bytes nobody wrote, and the
run stays green while checking nothing — the same silence a gate whose directory
moved produces.

They must also stay outside the checkout. A build tree inside a worktree makes
every removal of that worktree a decision about gigabytes, and leaves the
artefacts behind when it goes.

    Scripts/check-build-root.py
    Scripts/check-build-root.py --self-test

Exit 0 = the two agree and no script builds into the checkout, 1 = they do not.
"""

import os
import re
import subprocess
import sys
import tempfile
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_root
import gate_corpus

GATE = "check-build-root"

# A build output spelled relative to the checkout. This is what the change
# exists to remove, so it is what the gate refuses.
IN_CHECKOUT = re.compile(
    r'\$\{?ROOT\}?/(build|BuildHostTests|BuildBench|BuildPGOTrain)\b')

# The scripts allowed to name one: this gate, its helpers, and the doc that
# explains the layout.
EXEMPT = ("Scripts/build-root.sh", "Scripts/build_root.py",
          "Scripts/check-build-root.py")


def shell_build_root(checkout):
    """What build-root.sh derives, asked of the shell itself."""
    scripts = os.path.dirname(os.path.abspath(__file__))
    finished = subprocess.run(
        ["bash", "-c",
         f'ROOT="{checkout}"; . "{scripts}/build-root.sh"; printf "%s" "$UBQ_BUILD_ROOT"'],
        capture_output=True, text=True, check=False,
        env=dict(os.environ, PWD=checkout))
    return finished.stdout.strip()


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())
    if argv and argv[0] in ("--self-test", "--selftest"):
        return _self_test()

    failures = 0

    checkout = gate_corpus.root()
    fromPython = build_root.build_root(checkout)
    fromShell = shell_build_root(checkout)
    if fromPython != fromShell:
        print(f"{GATE}: the two derivations disagree about where this checkout "
              f"builds:", file=sys.stderr)
        print(f"  python: {fromPython}", file=sys.stderr)
        print(f"  shell : {fromShell}", file=sys.stderr)
        failures += 1

    if os.path.commonpath([fromPython, checkout]) == checkout:
        print(f"{GATE}: the build root is inside the checkout: {fromPython}",
              file=sys.stderr)
        failures += 1

    inside = []
    for path, text in gate_corpus.texts(
            sorted(p for p in gate_corpus.tracked_and_untracked()
                   if p.startswith("Scripts/") or p == "CMakeLists.txt")):
        if path in EXEMPT:
            continue
        for number, line in enumerate(text.split("\n"), start=1):
            if line.lstrip().startswith("#"):
                continue
            if IN_CHECKOUT.search(line):
                inside.append(f"{path}:{number}:{line.strip()[:96]}")
    if inside:
        print(f"{GATE}: {len(inside)} site(s) build into the checkout. Source "
              f"Scripts/build-root.sh and use $UBQ_BUILD_ROOT:", file=sys.stderr)
        for hit in inside[:20]:
            print(f"  {hit}", file=sys.stderr)
        if len(inside) > 20:
            print(f"  … and {len(inside) - 20} more.", file=sys.stderr)
        failures += 1

    if failures:
        return 1
    print(f"{GATE}: ok (both derivations agree on {fromPython}, and no script "
          f"builds into the checkout)")
    return 0


def _self_test():
    """The derivation is stable, distinguishing, and overridable."""
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    # UBQ_BUILD_ROOT names a root outright and so makes the derivation
    # path-independent — correct for the override, and fatal to every claim
    # below about two paths differing. A caller that has already sourced
    # build-root.sh exports it, which is exactly how the push gate runs this.
    environment = dict(os.environ)
    os.environ.pop("UBQ_BUILD_ROOT", None)
    os.environ.pop("UBIQUITTY_DERIVED_DATA", None)

    with patch.object(build_root.sys, "platform", "linux"):
        with patch.dict(os.environ, {"XDG_CACHE_HOME": "/tmp/ubq-cache"}):
            check("Linux uses the XDG cache directory",
                  build_root.derived_data() == "/tmp/ubq-cache/ubiquitty/build")
        with patch.dict(os.environ, {"XDG_CACHE_HOME": "relative"}):
            check("a relative XDG cache directory is ignored",
                  build_root.derived_data() == os.path.expanduser("~/.cache/ubiquitty/build"))
    with patch.object(build_root.sys, "platform", "darwin"):
        check("macOS uses DerivedData",
              build_root.derived_data() == os.path.expanduser("~/Library/Developer/Xcode/DerivedData"))

    here = gate_corpus.root()
    check("the two languages agree",
          build_root.build_root(here) == shell_build_root(here))
    check("the same checkout gives the same root",
          build_root.build_root(here) == build_root.build_root(here))
    check("two checkouts do not collide",
          build_root.build_root("/tmp/one") != build_root.build_root("/tmp/two"))
    # The PROJECT's name, not the checkout's: Xcode names a derived-data entry
    # after the project and lets the hash say which checkout, so every worktree
    # of this product reads as one product.
    check("the root carries the project's name",
          os.path.basename(build_root.build_root(here)).startswith(
              build_root.PROJECT + "-"))
    check("the hash is Xcode's 28 letters",
          len(build_root.path_hash(here)) == 28
          and build_root.path_hash(here).isalpha()
          and build_root.path_hash(here).islower())
    check("the root is outside the checkout",
          os.path.commonpath([build_root.build_root(here), here]) != here)

    # Two checkouts named alike must still differ, which is what the hash buys.
    left = build_root.build_root("/tmp/alpha/ubiquitty")
    right = build_root.build_root("/tmp/beta/ubiquitty")
    check("same basename, different path, different root", left != right)

    # A library checked out as a submodule builds into its consumer's root: the
    # consumer is the checkout a person builds and cleans.
    with tempfile.TemporaryDirectory(prefix="ubq-build-root-") as scratch:
        scratch = os.path.realpath(scratch)
        library, consumer = os.path.join(scratch, "library"), os.path.join(scratch, "consumer")
        identity = ["-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "protocol.file.allow=always"]
        for repository in (library, consumer):
            subprocess.run(["git", "init", "-q", repository], check=True)
        subprocess.run(["git", *identity, "commit", "-q", "--allow-empty", "-m", "library"],
                       cwd=library, check=True, capture_output=True)
        subprocess.run(["git", *identity, "submodule", "add", "-q", library, "Libraries/library"],
                       cwd=consumer, check=True, capture_output=True)
        nested = os.path.join(consumer, "Libraries", "library")
        check("a submodule builds into its consumer's root",
              build_root.build_root(nested) == build_root.build_root(consumer))
        check("the shell agrees for a submodule",
              shell_build_root(nested) == build_root.build_root(consumer))

    try:
        os.environ["UBQ_BUILD_ROOT"] = "/tmp/named-outright"
        check("UBQ_BUILD_ROOT names the root outright",
              build_root.build_root(here) == "/tmp/named-outright")
        del os.environ["UBQ_BUILD_ROOT"]
        os.environ["UBIQUITTY_DERIVED_DATA"] = "/tmp/elsewhere"
        check("UBIQUITTY_DERIVED_DATA moves the whole tree",
              build_root.build_root(here).startswith("/tmp/elsewhere/"))
    finally:
        os.environ.clear()
        os.environ.update(environment)

    if failures:
        print(f"{GATE} --self-test: {len(failures)} claim(s) the derivation "
              f"does not keep", file=sys.stderr)
        return 1
    print(f"{GATE} --self-test: ok (both languages agree, and the root is "
          f"named, hashed and outside the checkout)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
