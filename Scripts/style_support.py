#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""style_support — what every section of check-style.py is written against.

A section table (`style_sections_<repository>.py`) holds rows and handlers; this
holds the run they report into and the readers they share: the verdict-cached
gate runner, the branch-scoped file lists and the one-line scan summary. Tables
of every repository import it from ubiquitty-core's Scripts directory.
"""

import fnmatch
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_report
import gate_corpus
import gate_verdict
import ubq_roots
from gate_framework import Reporter

GATE = "check-style.py"


# A gate that stood down for a missing tool checked nothing, and its line says
# so in its own output. The run collects them so a green verdict can name what
# it does not cover.
#
# The name may carry a dot: format-code.sh is a gate, and `[a-z-]+` stopped at
# the dot and matched nothing, so the summary counted one stand-down while
# printing two — the tally that exists to name the gaps had a gap of its own.
STOOD_DOWN = re.compile(r"^[a-z][a-z0-9.-]*: SKIPPED", re.M)

# A gate whose subject is a build this job does not make reports where it does
# run. That is not a stand-down: nothing here waits for a tool to be installed,
# and a reader who takes it for one goes looking to fix a machine that is
# already right.
DEFERRED = re.compile(r"^[a-z][a-z0-9.-]*: DEFERRED", re.M)


class Run:
    """One pass over the gates: what failed, and what was written where."""

    def __init__(self, since):
        self.failed = []
        self.current = ""
        self.since = since
        self.outputs = []
        self.root = ubq_roots.subject_root()
        self.coreRoot = ubq_roots.core_root(self.root)
        self.kitRoot = ubq_roots.kit_root(self.root)
        self.repositories = ubq_roots.repositories(self.root)
        self.files = self._branch_files()
        self.cache = gate_verdict.Cache(self.root)
        self.reporter = Reporter(GATE, os.environ)

    def _branch_files(self):
        """The files this branch touched, for the sections that take a list.

        Empty is meaningful and is not the same as "everything": a branch that
        changed no file a gate reads has nothing for it to scan.
        """
        if not self.since:
            return []
        base = subprocess.run(["git", "merge-base", self.since, "HEAD"],
                              capture_output=True, text=True, check=False,
                              cwd=self.root).stdout.strip()
        if not base:
            return []
        listing = subprocess.run(
            ["git", "diff", "--name-only", "--diff-filter=ACMR",
             base + "...HEAD"],
            capture_output=True, text=True, check=False,
            cwd=self.root).stdout
        return [path for path in listing.split("\n") if path]

    def resolve(self, command):
        """The command with each `./Scripts/…` argument pointed at the checkout holding it.

        The subject's own script wins, then ubiquitty-core's, then ubiquitty-kit's.
        An argument no checkout holds is left as written, so the failure names
        the script that is missing.

        - Parameters:
          - command: the argument vector a section runs.
        - Returns: the argument vector with absolute paths where a script lives
          outside the subject.
        """
        resolved = []
        for argument in command:
            if argument.startswith("./Scripts/"):
                relative = argument[2:]
                for base in (self.root, self.coreRoot, self.kitRoot):
                    if os.path.exists(os.path.join(base, relative)):
                        if base != self.root:
                            argument = os.path.join(base, relative)
                        break
            resolved.append(argument)
        return resolved

    def section(self, title):
        self.current = title
        print()
        print(f"=== {title} ===")

    def fail(self, message=None):
        self.failed.append(message or self.current)

    def emit(self, output):
        """A gate's captured output, kept for the stood-down sweep."""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        self.outputs.append(output)
        if output:
            sys.stdout.write(output if output.endswith("\n") else output + "\n")

    def report(self, output, how):
        """Print a gate's output the way its section always has."""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        self.outputs.append(output)
        lines = output.split("\n")
        while lines and lines[-1] == "":
            lines.pop()
        if how == "ok":
            print("ok")
        elif how == "last":
            if lines:
                print(lines[-1])
        elif how == "last3":
            for line in lines[-3:]:
                print(line)
        else:
            if output:
                sys.stdout.write(output if output.endswith("\n")
                                 else output + "\n")

    def shell(self, command, **kwargs):
        """A command run for its output, with both streams joined."""
        finished = subprocess.run(self.resolve(command), cwd=self.root, check=False,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, **kwargs)
        return finished.returncode, finished.stdout.decode("utf-8", "replace")

    def gate(self, name, command, ok="all", bad="all", message=None):
        """Run one gate through the verdict cache and report it."""
        status, output = self.cache.run(name, self.resolve(command))
        self.report(output, ok if status == 0 else bad)
        if status != 0:
            self.fail(message or None)
        return status

    def scoped(self, pattern, name, command, ok="all", bad="all"):
        """Run a gate over this branch's files, or over the whole tree.

        A branch that touched none of the files a gate reads skips the scan
        rather than being handed a list the gate would reject.
        """
        if not self.since:
            return self.gate(name, command, ok, bad)
        subset = [path for path in self.files if re.search(pattern, path)]
        if not subset:
            # The skip is the gate's own output rather than a line of its own:
            # the section still reports through `ok`, so a section that prints
            # "ok" on success prints "ok" here too.
            self.report("ok (this branch changes no file this reads)", ok)
            return 0
        return self.gate(name, command + subset, ok, bad)


def tracked(run, *patterns):
    """The tracked files matching `patterns`, narrowed to the branch's own.

    A whole-tree list handed to a per-file section spawns a process for every
    file in the tree on every push, to re-answer a question about files the
    branch never touched. `run.files` is the same narrowing the table-driven
    sections take, so a section reading this agrees with them about what a
    scoped run covers.

    - Parameters:
      - run: the run state, which carries the scope.
      - patterns: git pathspecs the section reads.
    - Returns: the paths to examine, in git's order.
    """
    listing = subprocess.run(["git", "ls-files"] + list(patterns),
                             capture_output=True, text=True, check=False,
                             cwd=run.root).stdout
    paths = [path for path in listing.split("\n") if path]
    if not run.since:
        return paths
    branch = set(run.files)
    return [path for path in paths if path in branch]



def scanned(run, kind, passed, subjects, scoped=False):
    """Reports a per-file scan as one line rather than one line per file.

    A reader — an agent, most of the time — acts on what failed, and a run that
    names every file it was happy with buries that under hundreds of lines it
    cannot use.

    An empty scan is read by where the files came from. A branch-scoped section
    legitimately has nothing to read when the branch touched none of its files,
    and says so. A whole-tree section that matched nothing has lost its corpus:
    finding no files is not evidence that the files are clean, so it refuses
    rather than printing the `ok` a clean scan prints.

    - Parameters:
      - run: the run state, for recording a refusal.
      - kind: what was scanned, for the one line.
      - passed: how many files passed.
      - subjects: the files examined.
      - scoped: whether the corpus is this branch's files rather than the tree.
    """
    if not subjects:
        if scoped:
            print("ok (this branch changes no file this reads)")
        else:
            print(f"FAIL: {kind} matched no file — a scan of nothing is not a pass")
            run.fail()
        return
    print(f"ok ({passed} {kind})")


def grep_lines(run, pattern, roots, includes):
    """Every matching line under roots, as `path:line:text`."""
    compiled = re.compile(pattern)
    prefixes = tuple(root.rstrip("/") + "/" for root in roots)
    paths = [path for path in gate_corpus.eligible_under(run.root) or []
             if path.startswith(prefixes)
             and any(fnmatch.fnmatch(path, include) for include in includes)]
    return [f"{path}:{number}:{line}"
            for path, number, line in gate_corpus.lines(sorted(paths))
            if compiled.search(line)]


# A finding inside a comment is prose about the rule, not a use of it.
IN_COMMENT = re.compile(r":[0-9]+:[ \t]*(//|\*)")

CAST_PATTERN = r'\((const +)?[A-Za-z_][A-Za-z0-9_:<> ]*\*+\)[A-Za-z_&(]'


