#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""gate_verdict — a gate's verdict is a fact about the tree it read.

The same cache Scripts/gate-verdict-cache.sh implements, for callers that are
python. Both must key alike: a python runner and a shell runner that disagree
about the key would each replay the other's misses and neither would be wrong
about anything a reader could see.

What goes in the key, and why each part has to:

  the tree's content     tracked and untracked alike. A file the tree does not
                         track yet can carry the violation, and a key blind to
                         it replays a pass taken before the file existed.
  the build's state      some gates answer about compiled output rather than
                         source — nm over BuildHostTests, an executable under
                         build/ — and those bytes are gitignored, so the tree
                         fingerprint cannot see them.
  the fork point         a gate given a ref asks what this branch changed, which
                         is the merge base and not the ref's tip, so a teammate
                         landing on main leaves these verdicts standing and a
                         rebase retires them.
  the command            the same script asked a different question is a
                         different verdict.
  the toolchain          shellcheck, clang-format and python differ between
                         machines and versions.

Only a pass is remembered. A red verdict is re-asked on the next run, because a
retry after a failure is someone asking whether it is fixed, and answering that
from a cache answers a different question.

A stand-down is not a pass. A gate that found no tool or no build says so with a
"<gate>: SKIPPED" line and exits zero, having checked nothing. What it looked for
is often outside the key: the smokes read an app or a GTK binary under build
roots the build digest does not walk. Kept, the stand-down was replayed after the
build appeared, and the smokes read SKIPPED with both builds present.

The fingerprint is taken one time, before any gate runs.
Scripts/check-gate-subjects plants an untracked file and removes it again, and a
key that moved underneath the run would store verdicts under keys that never
recur.
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import ubq_roots

# The toolchains whose version changes what a gate says.
TOOLCHAIN = ("git", "cmake", "c++", "swiftc", "python3", "emcc")

# Variables that steer this cache rather than any gate. They change where a
# verdict is stored and whether it is consulted, never what a gate would say,
# and keying on them makes turning the log on invalidate every entry the log was
# meant to explain.
STEERING_PREFIX = "UBIQUITTY_GATE_CACHE"

# A gate's own line saying it checked nothing, at any depth: a section of
# check-style runs gates that run gates, and indents what they print.
# Scripts/gate-verdict-cache.sh matches the same line.
STOOD_DOWN = re.compile(rb"^[ \t]*[a-z][a-z0-9.-]*: SKIPPED", re.M)


def stood_down(output):
    """Whether a gate's output says some part of it checked nothing.

    - Parameter output: the bytes the gate wrote.
    - Returns: True when a line reports a stand-down.
    """
    return STOOD_DOWN.search(output) is not None


def _run(arguments, **kwargs):
    return subprocess.run(arguments, capture_output=True, check=False, **kwargs)


class Cache:
    """The verdicts one run may replay."""

    def __init__(self, root=None, enabled=True):
        self.root = root or gate_corpus.root()
        self.enabled = False
        self.reused = 0
        self.asked = 0
        self.directory = None
        self._environment = ""
        self._tree = ""
        self._build = ""
        if not enabled or os.environ.get("UBIQUITTY_GATE_CACHE", "on") == "off":
            return
        self.directory = self._store()
        if not self.directory:
            return
        self._environment = self._environment_digest() + self._library_digest()
        self._tree = self._tree_digest()
        self._build = gate_corpus.build_state()
        if not (self._environment and self._tree and self._build):
            return
        self.enabled = True

    def _store(self):
        finished = _run(["git", "-C", self.root, "rev-parse", "--git-path",
                         "ubiquitty-gate-verdicts"])
        path = finished.stdout.decode().strip()
        if not path:
            return None
        if not os.path.isabs(path):
            path = os.path.join(self.root, path)
        try:
            os.makedirs(path, exist_ok=True)
        except OSError:
            return None
        return path

    def _environment_digest(self):
        digest = hashlib.sha256()
        digest.update(b"tier=gate-verdict\n")
        digest.update(("path=%s\n" % os.environ.get("PATH", "")).encode())
        for name in ("CC", "CXX", "SDKROOT"):
            digest.update(("%s=%s\n" % (name.lower(),
                                        os.environ.get(name, ""))).encode())
        for name in sorted(os.environ):
            if name.startswith("UBIQUITTY_") and not name.startswith(STEERING_PREFIX):
                digest.update(("%s=%s\n" % (name, os.environ[name])).encode())
        digest.update(os.uname().sysname.encode() + b" " +
                      os.uname().release.encode() + b" " +
                      os.uname().machine.encode() + b"\n")
        for tool in TOOLCHAIN:
            found = shutil.which(tool) or ""
            digest.update(("%s=%s\n" % (tool, found)).encode())
            if found:
                version = _run([found, "--version"]).stdout.split(b"\n", 1)[0]
                digest.update(version + b"\n")
        return digest.hexdigest()

    def _library_digest(self):
        """What the libraries a repository carries hold, by their commit and their edits.

        A gate of ubiquitty-core run from a consumer reads the consumer's tree
        under the library's rules, so the library's state is part of the question
        the verdict answers. The consumer's own digest sees a submodule only as
        the commit it pins; an edit inside the checkout is seen here.
        """
        digest = hashlib.sha256()
        seen = {os.path.realpath(self.root)}
        for library in (ubq_roots.core_root(self.root), ubq_roots.kit_root(self.root)):
            if os.path.realpath(library) in seen:
                continue
            seen.add(os.path.realpath(library))
            digest.update(("library=%s\n" % library).encode())
            for arguments in (["rev-parse", "HEAD"], ["status", "--porcelain"], ["diff", "HEAD"]):
                digest.update(_run(["git", "-C", library] + arguments,
                                    env=ubq_roots.foreign_git_environment()).stdout)
        return digest.hexdigest() if len(seen) > 1 else ""

    def _tree_digest(self):
        """Everything the tree carries, tracked and untracked."""
        digest = hashlib.sha256()
        for arguments in (["ls-tree", "-r", "HEAD"],
                          ["diff", "--no-ext-diff", "--binary", "HEAD"]):
            digest.update(_run(["git", "-C", self.root] + arguments).stdout)
        listing = _run(["git", "-C", self.root, "ls-files", "--others",
                        "--exclude-standard", "-z"]).stdout
        for path in listing.split(b"\0"):
            if not path:
                continue
            digest.update(b"untracked:" + path + b"\0")
            digest.update(_run(["git", "-C", self.root, "hash-object",
                                "--no-filters", path.decode("utf-8", "replace")]).stdout)
        return digest.hexdigest()

    def _fork_points(self, command):
        """Any argument that names a commit, recorded as its merge base.

        A gate told to check "since origin/main" reads a ref, and the name in
        the command stays the same when the ref moves. What it asks about is the
        fork point.
        """
        parts = []
        for argument in command:
            # The same arguments the shell key passes over: flags and paths.
            # A slash alone does not make a path — origin/main is the ref this
            # exists for, and skipping every slash kept it out of the key.
            if argument.startswith(("-", "./", "/")) or argument == ".":
                continue
            resolved = _run(["git", "-C", self.root, "rev-parse", "--verify",
                             "--quiet", argument + "^{commit}"]).stdout.decode().strip()
            if not resolved:
                continue
            base = _run(["git", "-C", self.root, "merge-base", resolved,
                         "HEAD"]).stdout.decode().strip() or resolved
            parts.append("forkpoint:%s=%s" % (argument, base))
        return parts

    def key(self, command):
        digest = hashlib.sha256()
        for part in [self._environment, self._tree, self._build] + list(command):
            digest.update(part.encode("utf-8", "replace") + b"\0")
        for part in self._fork_points(command):
            digest.update(part.encode() + b"\0")
        return digest.hexdigest()

    def run(self, name, command, **kwargs):
        """A gate's output and status, replayed when this tree already knows it.

        - Returns: (status, output bytes). The output is what the gate wrote on
          both streams, so a caller sees what it would have seen.
        """
        if not self.enabled:
            finished = _run(command, cwd=self.root, **kwargs)
            self.asked += 1
            return finished.returncode, finished.stdout + finished.stderr

        entry = os.path.join(self.directory, self.key(command) + ".pass")
        if os.path.exists(entry):
            try:
                with open(entry, "rb") as handle:
                    self.reused += 1
                    return 0, handle.read()
            except OSError:
                pass

        self.asked += 1
        finished = subprocess.run(command, cwd=self.root, check=False,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, **kwargs)
        output = finished.stdout
        if finished.returncode == 0 and not stood_down(output):
            # Written through a temporary and moved, so a run interrupted
            # mid-write leaves no half a verdict for the next one to replay.
            temporary = entry + ".partial"
            try:
                with open(temporary, "wb") as handle:
                    handle.write(output)
                os.replace(temporary, entry)
            except OSError:
                if os.path.exists(temporary):
                    os.remove(temporary)
        return finished.returncode, output

    def summary(self):
        if not self.enabled:
            return "verdict cache off"
        return "%d reused, %d asked" % (self.reused, self.asked)
