#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo

# SPDX-License-Identifier: Apache-2.0

"""check-git-isolation.py — a destructive git command names its tree and gets it.

git exports `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE` and their relatives into
a hook's environment, and they OVERRIDE the repository a git command discovers
from its working directory. `git -C <tree>` does not escape that: `-C` sets the
working directory, and the working directory is only the discovery input, so an
explicit `GIT_DIR` skips discovery entirely.

Measured, in two throwaway repositories:

    GIT_DIR=<victim> git -C <target> checkout --orphan x
      -> victim gets the orphan branch, target is untouched

**The tree the command names is the one that does not move.** That is what makes
`-C` read as the safe spelling while being the ignored one.

What this requires. A script that runs one of the verbs below must clear those
variables first. The verbs are the ones that rewrite a repository rather than
read it, so aiming one at the wrong tree costs work rather than a wrong answer:

    git worktree add / remove      git checkout --orphan
    git reset --hard               git init
    git clean -fd                  git checkout -B / -b / --detach

A script operating on the repository it belongs to is not exempt and does not
need to be. Under a hook its GIT_DIR already points there, so clearing changes
nothing; the cost is one line and the benefit is that moving the script, or
pointing it at a fixture later, cannot quietly re-arm it.

The source exporter and test fixtures use isolated temporary repositories.
This gate makes their Git environment isolation explicit and checks that it
survives changes to the surrounding scripts.
"""

import os
import re
import sys
import tempfile

# Any global option may stand between `git` and the verb, not only `-C`. A
# prefix that enumerates one option reads as covering the command form while
# matching a single spelling of it: `git -C d -c advice.detachedHead=false switch
# --detach` is the shape that got through, and `git --no-pager -C d reset --hard`
# is the same hole. The value token is optional because `-c k=v` and `--git-dir=d`
# carry theirs inline while `-C` and `--work-tree` take a separate one.
GLOBAL_OPTIONS = r'(?:-\S+\s+(?:[^-\s]\S*\s+)?)*'

# Rewrites a repository rather than reading one. `checkout -- <path>` and a plain
# `git checkout <branch>` are absent: they move one tree's HEAD or restore a
# file, which is recoverable, and including them would flag the many scripts that
# legitimately drive the repository they live in.
#
# `switch` carries the same qualifiers as `checkout` and for the same reason. It
# is not a synonym to be listed once: `checkout` was split into `switch` and
# `restore` precisely so that each does one job, so a codebase moving to the newer
# spelling leaves a verb list written against the older one behind, silently and
# one call site at a time. `restore` is absent for the reason `checkout -- <path>`
# is.
DESTRUCTIVE = re.compile(
    r'\bgit\s+' + GLOBAL_OPTIONS + r'(?:'
    r'worktree\s+(?:add|remove)'
    r'|checkout\s+(?:-q\s+)?(?:--orphan|--detach|-B\b|-b\b)'
    r'|switch\s+(?:-q\s+)?(?:--detach|-C\b|-c\b)'
    r'|reset\s+(?:-q\s+)?--hard'
    r'|clean\s+-[a-z]*[fd]'
    r'|init\b'
    r')')

# Either spelling counts: the shared helper, or the unset written out.
ISOLATION = re.compile(r'\bubq_isolate_git_environment\b|\bunset\s+GIT_DIR\b'
                       r'|GIT_DIR.*\bpop\(|del\s+\w+\[.GIT_DIR.\]')

# Everywhere a script lives, not everywhere one is expected to. Beside Scripts/
# and Tests/, Tools/ holds more script directories and so does
# Resources/shell-integration, and a `git worktree add` landing in one of them
# is invisible to a walk rooted at the first two. The roots a sweep chooses are
# its most confident assumption and the one its output cannot report on.
SEARCH_ROOTS = ("Scripts", "Tests", "Tools", "Resources")
SUFFIXES = (".sh", ".py")


TRIPLE = re.compile(r"('''|\"\"\")")
HEREDOC = re.compile(r"<<-?\s*'?\"?([A-Za-z_][A-Za-z0-9_]*)'?\"?")


def readable_lines(text):
    """The file with prose blanked, one entry per original line.

    A docstring describing this very check names every verb it looks for, and a
    heredoc quoting a command is a string rather than a command. Blanking rather
    than dropping keeps each index on its own line number, so a report still
    points at the right place.
    """
    lines = text.split("\n")
    out = []
    inside_triple = None
    heredoc_terminator = None
    for raw in lines:
        if heredoc_terminator is not None:
            if raw.strip() == heredoc_terminator:
                heredoc_terminator = None
            out.append("")
            continue
        if inside_triple is not None:
            if inside_triple in raw:
                inside_triple = None
            out.append("")
            continue

        line = raw
        opener = TRIPLE.search(line)
        if opener:
            # A one-line docstring opens and shuts on the same line.
            rest = line[opener.end():]
            if opener.group(1) not in rest:
                inside_triple = opener.group(1)
            line = line[:opener.start()]

        # A trailing `#`, respecting quotes so a `#` inside a string stays.
        quote = None
        for i, c in enumerate(line):
            if quote:
                if c == quote and line[i - 1] != '\\':
                    quote = None
            elif c in '"\'':
                quote = c
            elif c == '#':
                line = line[:i]
                break

        here = HEREDOC.search(line)
        if here:
            heredoc_terminator = here.group(1)
        out.append(line)
    return out


def examine(path, root):
    """Returns [(line number, the offending command)] for one file."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        lines = readable_lines(handle.read())

    # Isolation anywhere above the command counts. A shell function that unsets
    # runs in the caller's shell, so the effect outlives the call — which is
    # precisely why relying on one called for another reason is fragile, and why
    # the report names the command rather than the file.
    isolated_by = None
    findings = []
    aliases = set()
    assignment = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)=(?:\(\s*|[\"'])git(?:\s|[\"')])")
    for number, line in enumerate(lines, 1):
        if isolated_by is None and ISOLATION.search(line):
            isolated_by = number
            continue
        aliases.update(assignment.findall(line))
        expanded = line
        for alias in aliases:
            reference = re.compile(r'"?\$(?:\{' + re.escape(alias)
                                   + r'(?:\[@\])?\}|' + re.escape(alias) + r'\b)"?')
            expanded = reference.sub("git", expanded)
        match = DESTRUCTIVE.search(expanded)
        if match and isolated_by is None:
            findings.append((number, line.strip()))
    return findings


def sources(root):
    for name in SEARCH_ROOTS:
        base = os.path.join(root, name)
        if not os.path.isdir(base):
            continue
        for directory, subdirectories, files in os.walk(base):
            subdirectories[:] = [d for d in subdirectories if d != "hooks"]
            for entry in sorted(files):
                if entry.endswith(SUFFIXES):
                    yield os.path.join(directory, entry)


def self_test():
    commands = (
        'GIT=(git -c x=y)\n"${GIT[@]}" -C "$d" init -q',
        'runner="git -c x=y"\n$runner -C "$d" reset --hard',
        "runner='git -c x=y'\n${runner} worktree add \"$d\"",
    )
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "fixture.sh")
        for command in commands:
            for isolated in (False, True):
                with open(path, "w") as handle:
                    handle.write(("ubq_isolate_git_environment\n" if isolated else "") + command)
                if bool(examine(path, directory)) == isolated:
                    print("check-git-isolation: self-test failed", file=sys.stderr)
                    return 1
    print("check-git-isolation: self-test ok (array and scalar commands, isolated and unisolated)")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    root = os.path.abspath(argv[1] if len(argv) > 1 and not argv[1].startswith("--") else ".")

    scanned, findings = 0, []
    for path in sources(root):
        scanned += 1
        for number, command in examine(path, root):
            findings.append((os.path.relpath(path, root), number, command))

    if not scanned:
        print("check-git-isolation: no scripts found under %s — the walk matched"
              % " or ".join(SEARCH_ROOTS), file=sys.stderr)
        print("  nothing, which is not the same as nothing being wrong.", file=sys.stderr)
        return 2

    for relative, number, command in sorted(findings):
        print("%s:%d: rewrites a repository with no GIT_DIR isolation above it"
              % (relative, number), file=sys.stderr)
        print("    %s" % command[:100], file=sys.stderr)

    if findings:
        print("", file=sys.stderr)
        print("check-git-isolation: %d command(s) can be aimed at the wrong repository."
              % len(findings), file=sys.stderr)
        print("  Under a hook, GIT_DIR names the pushing worktree and overrides the", file=sys.stderr)
        print("  directory the command runs in — `git -C` included. Call", file=sys.stderr)
        print("  ubq_isolate_git_environment (Tests/git-fixture.sh), or unset GIT_DIR", file=sys.stderr)
        print("  GIT_WORK_TREE GIT_INDEX_FILE and friends, above the command.", file=sys.stderr)
        return 1

    print("check-git-isolation: ok (%d scripts, every repository-rewriting command "
          "isolated first)" % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
