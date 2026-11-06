#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""build_root — where a checkout's build output goes.

Outside the checkout. A build tree inside a worktree makes `git status` slow,
makes every `rm -rf` of a worktree a decision about gigabytes, and leaves the
artefacts behind when the worktree is removed, or when its repository loses
track of it.

The layout is Xcode's, for the same reason Xcode uses it: a name a person can
read, and a hash that says which checkout it belongs to.

    ~/Library/Developer/Xcode/DerivedData/Ubiquitty-<Xcode's 28-letter path hash>

Xcode's own directory, so that Xcode's "Delete Derived Data" and the Storage
pane in System Settings clear these too -- a build tree somewhere of our own
invention is one more thing a person has to know about to reclaim. Xcode names
its own <Name>-<28 lowercase letters>; a hex digest cannot collide with that and
says at a glance which entries are ours.

On non-macOS hosts the parent is $XDG_CACHE_HOME/ubiquitty/build, defaulting
to ~/.cache/ubiquitty/build. Relative XDG_CACHE_HOME values are ignored.

UBIQUITTY_DERIVED_DATA moves the whole tree; UBQ_BUILD_ROOT names one build root
outright and skips the derivation, which is what a CI job with its own workspace
wants.

Scripts/build-root.sh derives the identical path for shell callers, and
Scripts/check-build-root.py holds the two to agreeing — a python gate
fingerprinting one directory while the shell builds into another would report a
verdict about bytes nobody wrote.
"""

import hashlib
import os
import struct
import subprocess
import sys

# The project, as CMakeLists names it. Constant across checkouts, exactly as
# Xcode is: the hash identifies the checkout, the name the product. Naming
# each root after its directory instead reads as a different product per
# worktree.
PROJECT = "Ubiquitty"

# Xcode's own encoding: md5 the absolute path, read the digest as two big-endian
# 64-bit values, and write each in base 26 as fourteen letters a-z. Following it
# exactly makes these directories indistinguishable from a project's, which is
# the point -- Xcode's "Delete Derived Data" and the Storage pane in System
# Settings then treat them as theirs.
HASH_LETTERS = 14


def checkout_root(start=None):
    """The top of the working tree, or the given directory when git cannot say.

    A submodule answers with the outermost repository's top (see `outermost`).

    A directory that does not exist is answered for rather than raised on: the
    derivation is pure path arithmetic, and a caller asking where a checkout
    would build before it exists is asking a fair question.
    """
    where = start or os.getcwd()
    if os.path.isdir(where):
        finished = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                  cwd=where, capture_output=True, text=True,
                                  check=False)
        found = finished.stdout.strip()
        if found:
            return outermost(found)
    return os.path.abspath(where)


def outermost(root):
    """The working tree of the repository that carries this one, however deep.

    A library checked out as a submodule builds into its consumer's root, since
    the consumer is the checkout a person builds and cleans; a tree that is no
    submodule is its own.
    """
    while True:
        finished = subprocess.run(["git", "rev-parse", "--show-superproject-working-tree"],
                                  cwd=root, capture_output=True, text=True, check=False)
        parent = finished.stdout.strip()
        if not parent:
            return root
        root = parent


def derived_data():
    """The directory holding every checkout's build tree."""
    override = os.environ.get("UBIQUITTY_DERIVED_DATA")
    if override:
        return os.path.abspath(os.path.expanduser(override))
    if sys.platform == "darwin":
        return os.path.expanduser("~/Library/Developer/Xcode/DerivedData")
    cache = os.environ.get("XDG_CACHE_HOME", "")
    if not os.path.isabs(cache):
        cache = os.path.expanduser("~/.cache")
    return os.path.join(cache, "ubiquitty", "build")


def build_root(start=None):
    """This checkout's build root.

    - Returns: an absolute path. Nothing is created here; a build script makes
      the directory it needs, so a read-only caller stays read-only.
    """
    named = os.environ.get("UBQ_BUILD_ROOT")
    if named:
        return os.path.abspath(os.path.expanduser(named))
    root = os.path.abspath(checkout_root(start))
    return os.path.join(derived_data(), f"{PROJECT}-{path_hash(root)}")


def path_hash(path):
    """Xcode's 28-letter encoding of an absolute path.

    - Returns: 28 characters, a to z.
    """
    digest = hashlib.md5(path.encode("utf-8")).digest()
    letters = []
    for value in struct.unpack(">QQ", digest):
        part = []
        for _ in range(HASH_LETTERS):
            part.append(chr(ord("a") + value % 26))
            value //= 26
        letters.append("".join(reversed(part)))
    return "".join(letters)


if __name__ == "__main__":
    print(build_root(sys.argv[1] if len(sys.argv) > 1 else None))
