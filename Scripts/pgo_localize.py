#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""pgo_localize.py — the training profile, keyed for the checkout that compiles it.

LLVM keys an internal-linkage function — anything in an anonymous namespace, a
file-static, every lambda — as "<source path as compiled>;<name>". The trainer
compiles from the repository root with relative paths, so the committed profile
names those functions `./Source/...;name`, which keeps this machine's paths out
of a tracked artefact. Every consumer compiles absolute paths — CMake hands the
compiler `<root>/Source/...`, and so do the bench scripts — and looks each one
up as `<root>/Source/...;name`. Nothing matches, nothing says so: clang reports
a function whose counters no longer fit its body, never one it found no
counters for, and every internal function in the shipping app compiled as
though no profile existed. No compiler flag remaps the key — the file, debug
and coverage prefix maps all leave it alone, which was tested — so the
profile is rewritten instead.

This writes a copy of the profile whose relative keys carry the consumer's
absolute root, into the consumer's build root, keyed by the profile's content
and the root, and prints its path. The copy is derived, local and never
committed; the next retrain or another checkout writes a new one beside it.

    pgo_localize.py <profile> <source-root> <cache-directory>
    pgo_localize.py --default-profile <source-root> <target-system> <target-processor>
    pgo_localize.py --self-test

When llvm-profdata cannot be found the committed profile's own path is printed,
with the reason on stderr: the build then behaves exactly as it did before,
external-linkage counters only, and says so.

A configure runs this with the Python CMake finds, which on a machine with Xcode
alone, Xcode Cloud's among them, is Xcode's 3.9. Annotations are kept as text so
the `X | None` spelling below never evaluates there.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile


def default_profile(root: str, system: str, processor: str) -> str:
    """Returns the training profile for the target, without probing this host.

    Linux targets have architecture-specific release-toolchain counters. Other
    targets retain the portable profile. Explicit profile overrides
    belong to the caller and must be resolved before asking for this default.
    """
    name = "portable-core.profdata"
    if system.lower() == "linux":
        if processor.lower() in ("x86_64", "amd64"):
            name = "linux-x86_64-core.profdata"
        elif processor.lower() in ("aarch64", "arm64"):
            name = "linux-aarch64-core.profdata"
    return os.path.join(root, "Support", "pgo", name)


def llvm_profdata_candidates() -> list[list[str]]:
    """Returns the command of every llvm-profdata on this host, in the order tried.

    An indexed profile carries the format version of the llvm-profdata that
    wrote it, and an older one refuses the file. A host with several LLVMs has
    whichever its packages chose as the unversioned name, which can be older than
    a versioned one beside it, so each is tried in turn by `localized_profile`.
    """
    found = []
    if shutil.which("xcrun"):
        located = subprocess.run(["xcrun", "--find", "llvm-profdata"], capture_output=True, text=True)
        if located.returncode == 0 and located.stdout.strip():
            found.append([located.stdout.strip()])
    for name in ("llvm-profdata", *(f"llvm-profdata-{version}" for version in range(30, 13, -1))):
        path = shutil.which(name)
        if path and [path] not in found:
            found.append([path])
    return found


def llvm_profdata() -> list[str] | None:
    """Returns the first llvm-profdata `llvm_profdata_candidates` names, or None."""
    candidates = llvm_profdata_candidates()
    return candidates[0] if candidates else None


def localized_profile(profile: str, root: str, cache: str) -> str:
    """Returns the path of `profile` with its relative keys rooted at `root`.

    - Parameters:
      - profile: The committed, relative-keyed profile.
      - root: The absolute source root the consumer compiles under, exactly as
        its compile lines spell it.
      - cache: The directory the derived profile is written to.
    - Returns: The derived profile's path, or `profile` itself when no
      llvm-profdata is available or it cannot read the profile.
    """
    tools = llvm_profdata_candidates()
    if not tools:
        print(f"pgo-localize: no llvm-profdata; {profile} is used as committed, so "
              "internal-linkage functions compile without counters", file=sys.stderr)
        return profile
    root = root.rstrip("/")
    with open(profile, "rb") as stream:
        content = hashlib.sha256(stream.read()).hexdigest()[:16]
    rooted = hashlib.sha256(root.encode()).hexdigest()[:8]
    target = os.path.join(cache, f"portable-core.{content}.{rooted}.profdata")
    if os.path.isfile(target):
        return target
    os.makedirs(cache, exist_ok=True)
    failures = []
    for tool in tools:
        try:
            return derive(tool, profile, root, cache, target)
        except subprocess.CalledProcessError as failure:
            failures.append(f"{tool[0]}: {(failure.stderr or b'').decode(errors='replace').strip() or failure}")
    print(f"pgo-localize: no llvm-profdata could derive a rooted copy of {profile} ({'; '.join(failures)}); "
          "it is used as committed, so internal-linkage functions compile without counters", file=sys.stderr)
    return profile


def derive(tool: list[str], profile: str, root: str, cache: str, target: str) -> str:
    """Writes the rooted copy of `profile` to `target` and returns `target`."""
    with tempfile.TemporaryDirectory(dir=cache) as scratch:
        text = os.path.join(scratch, "profile.proftext")
        subprocess.run([*tool, "merge", "--text", profile, "-o", text], check=True, capture_output=True)
        rewritten = os.path.join(scratch, "rooted.proftext")
        with open(text, encoding="utf-8", errors="surrogateescape") as source, \
                open(rewritten, "w", encoding="utf-8", errors="surrogateescape") as sink:
            for line in source:
                sink.write(root + line[1:] if line.startswith("./") else line)
        built = os.path.join(scratch, "rooted.profdata")
        subprocess.run([*tool, "merge", rewritten, "-o", built], check=True, capture_output=True)
        os.replace(built, target)
    return target


def self_test() -> int:
    """Trains a static function under a relative path and looks it up rooted."""
    tool = llvm_profdata()
    compiler = os.environ.get("CXX") or shutil.which("c++")
    if tool is None or compiler is None:
        print("pgo-localize: self-test needs clang and llvm-profdata", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as scratch:
        scratch = os.path.realpath(scratch)
        with open(os.path.join(scratch, "unit.cpp"), "w") as unit:
            unit.write("static int __attribute__((noinline)) scaled(int x) { return x * 3; }\n"
                       "int main(int count, char**) { return scaled(count) == 3 ? 0 : 1; }\n")
        raw = os.path.join(scratch, "raw")
        subprocess.run([compiler, "-O1", f"-fprofile-generate={raw}", "./unit.cpp", "-o", "unit"],
                       cwd=scratch, check=True)
        subprocess.run([os.path.join(scratch, "unit")], cwd=scratch, check=True)
        profiles = [os.path.join(raw, name) for name in os.listdir(raw)]
        trained = os.path.join(scratch, "trained.profdata")
        subprocess.run([*tool, "merge", "-o", trained, *profiles], check=True)
        local = localized_profile(trained, scratch, os.path.join(scratch, "cache"))
        names = subprocess.run([*tool, "show", "--all-functions", local],
                               capture_output=True, text=True, check=True).stdout
        if f"{scratch}/unit.cpp;_ZL6scaledi" not in names:
            print(f"pgo-localize: self-test FAILED — {scratch}/unit.cpp;_ZL6scaledi is not in the "
                  "rooted profile", file=sys.stderr)
            return 1
        if "./unit.cpp;" in names:
            print("pgo-localize: self-test FAILED — a relative key survived", file=sys.stderr)
            return 1
    print("pgo-localize: self-test ok (a relative internal key is rooted at the consumer's path)")
    return 0


def main() -> int:
    if sys.argv[1:2] == ["--default-profile"]:
        if len(sys.argv) != 5:
            print(__doc__, file=sys.stderr)
            return 2
        print(default_profile(*sys.argv[2:]))
        return 0
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    print(localized_profile(*sys.argv[1:]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
