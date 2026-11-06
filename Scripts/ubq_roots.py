#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""ubq_roots — which tree a script judges, and where the other repositories are.

The tree a script judges is its subject: the checkout git reports for the
directory the script runs in. Hooks, runners and the gate table change into
their own checkout before they start a gate, so a gate that reads the working
directory's repository reads the repository that started it.

Ubiquitty is three repositories, ubiquitty-core, ubiquitty-kit and the app,
and a script of one reaches the others through two roots:

    UBQ_CORE_ROOT   the ubiquitty-core checkout
    UBQ_KIT_ROOT    the ubiquitty-kit checkout

Neither needs setting. A root defaults to Libraries/ubiquitty-core or
Libraries/ubiquitty-kit under the subject when that path exists, and to the
subject itself otherwise, so inside one repository that holds everything, and
inside ubiquitty-core itself, every root is the same tree.

A checkout names the repository it is in with Scripts/repository.txt, holding
`core`, `kit` or `app`. A checkout with no such file holds every repository's
files and is judged as all of them.
"""

import fnmatch
import os
import subprocess
import sys

REPOSITORIES = ("core", "kit", "app")

CORE_SUBMODULE = os.path.join("Libraries", "ubiquitty-core")
KIT_SUBMODULE = os.path.join("Libraries", "ubiquitty-kit")
REPOSITORY_FILE = os.path.join("Scripts", "repository.txt")


# What a git hook is run with to tell git which repository, index and objects it
# is operating on. A command aimed at another repository must not inherit them:
# `git -C library ls-files` under them answers for the repository being committed.
REPOSITORY_VARIABLES = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX", "GIT_COMMON_DIR",
                        "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_QUARANTINE_PATH")


def foreign_git_environment():
    """The environment for a git command that reads a repository other than the one being operated on."""
    return {name: value for name, value in os.environ.items() if name not in REPOSITORY_VARIABLES}


def subject_root(start=None):
    """The top of the working tree a script judges, or the directory itself without git.

    - Parameters:
      - start: the directory to ask from; the working directory by default.
    - Returns: an absolute path.
    """
    where = os.path.abspath(start or os.getcwd())
    if os.path.isdir(where):
        finished = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                  cwd=where, capture_output=True, text=True,
                                  check=False)
        found = finished.stdout.strip()
        if found:
            return found
    return where


def subject_of(script):
    """The tree a gate script judges: the caller's repository, else the script's own.

    A gate run by a repository's runner, hook or developer is in that
    repository's directory, and judges it, whichever repository holds the gate's
    file (a library's copy judges the repository that carries it). A gate run
    from a directory that is no repository, such as ctest's build directory,
    judges the repository holding the script.

    - Parameters:
      - script: the gate's `__file__`.
    - Returns: an absolute path.
    """
    finished = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                              text=True, check=False)
    if finished.returncode == 0 and finished.stdout.strip():
        return finished.stdout.strip()
    own = os.path.dirname(os.path.abspath(script))
    finished = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=own,
                              capture_output=True, text=True, check=False)
    if finished.returncode == 0 and finished.stdout.strip():
        return finished.stdout.strip()
    return os.path.dirname(own)


def _root(variable, submodule, subject):
    """A sibling repository's root: the variable, the submodule, else the subject."""
    configured = os.environ.get(variable)
    if configured:
        return os.path.abspath(os.path.expanduser(configured))
    base = subject_root() if subject is None else subject
    # The submodule is chosen by existing, not by being populated: a checkout
    # that has not initialised it must fail on the missing files rather than
    # quietly judge its own tree as the library's.
    nested = os.path.join(base, submodule)
    return nested if os.path.lexists(nested) else base


def core_root(subject=None):
    """The ubiquitty-core checkout this subject reads.

    - Parameters:
      - subject: the subject's root; the working directory's repository by default.
    """
    return _root("UBQ_CORE_ROOT", CORE_SUBMODULE, subject)


def kit_root(subject=None):
    """The ubiquitty-kit checkout this subject reads.

    - Parameters:
      - subject: the subject's root; the working directory's repository by default.
    """
    return _root("UBQ_KIT_ROOT", KIT_SUBMODULE, subject)


def repositories(subject=None):
    """The repositories whose files the subject holds.

    - Returns: a tuple drawn from `REPOSITORIES`; all three for a checkout that
      names none, since that checkout is the whole product.
    """
    base = subject_root() if subject is None else subject
    try:
        with open(os.path.join(base, REPOSITORY_FILE), encoding="utf-8") as handle:
            name = "".join(line.strip() for line in handle if not line.startswith("#"))
    except OSError:
        return REPOSITORIES
    if name not in REPOSITORIES:
        raise ValueError(f"{REPOSITORY_FILE} names {name!r}, not one of {REPOSITORIES}")
    return (name,)


LEDGER_PATTERN = "gate-ledger.*.txt"


def ledger(subject=None):
    """What the gates of the repositories the subject holds are held to.

    Each `Scripts/gate-ledger.<repository>.txt` of the subject is read. A line
    `canary <gate>` names a gate whose self-test a stanza of the repository's
    driver depends on. A line `budget <name> <n>` is a count that must equal the
    tree's. Every count is over files and each file has one owner, so a subject
    holding several repositories sums their budgets. A line `tool <gate> <why>`
    names a gate that nothing invokes because a person runs it, and what invokes
    it instead.

    - Parameters:
      - subject: the subject's root; the working directory's repository by default.
    - Returns: (budgets, canaries, tools): a dict from budget name to its sum, the
      canaries of every ledger, and a dict from gate to the reason it is run by hand.
    - Raises: ValueError for a line it does not know, so a misspelt word cannot
      read as an absent budget.
    """
    base = subject_root() if subject is None else subject
    directory = os.path.join(base, "Scripts")
    budgets = {}
    canaries = []
    tools = {}
    for name in sorted(os.listdir(directory)) if os.path.isdir(directory) else []:
        if not fnmatch.fnmatch(name, LEDGER_PATTERN):
            continue
        with open(os.path.join(directory, name), encoding="utf-8") as handle:
            for line in handle:
                words = line.split("#", 1)[0].split()
                if len(words) >= 3 and words[0] == "tool":
                    tools[words[1]] = " ".join(words[2:])
                elif len(words) == 2 and words[0] == "canary":
                    canaries.append(words[1])
                elif len(words) == 3 and words[0] == "budget":
                    budgets[words[1]] = budgets.get(words[1], 0) + int(words[2])
                elif words:
                    raise ValueError("%s: unrecognised line %r" % (name, line.strip()))
    return budgets, tuple(canaries), tools


def budget(name, subject=None):
    """The count the subject's gates hold `name` to.

    - Returns: the sum over the ledgers the subject holds.
    - Raises: KeyError when no ledger sets it.
    """
    return ledger(subject)[0][name]


OTHER_REPOSITORIES_FILE = os.path.join("Docs", "other-repositories.txt")


def elsewhere(subject=None):
    """The paths and symbols the subject's documents name that another repository owns.

    Docs/other-repositories.txt lists them, one `path <repository> <path>` or
    `symbol <repository> <identifier>` per line. An entry for a repository the
    subject holds is not elsewhere and is left out, so a checkout holding every
    repository reads none.

    - Parameters:
      - subject: the subject's root; the working directory's repository by default.
    - Returns: (paths, symbols), two sets.
    """
    base = subject_root() if subject is None else subject
    held = repositories(base)
    paths, symbols = set(), set()
    try:
        with open(os.path.join(base, OTHER_REPOSITORIES_FILE), encoding="utf-8") as handle:
            lines = handle.read().splitlines()
    except OSError:
        return paths, symbols
    for line in lines:
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if len(words) != 3 or words[0] not in ("path", "symbol") or words[1] not in REPOSITORIES:
            raise ValueError("%s: unrecognised line %r" % (OTHER_REPOSITORIES_FILE, line))
        if words[1] in held:
            continue
        (paths if words[0] == "path" else symbols).add(words[2])
    return paths, symbols


def _git_lines(arguments, cwd, foreign=False):
    """The lines a git command prints, or None when it fails."""
    finished = subprocess.run(["git", *arguments], cwd=cwd, capture_output=True, text=True,
                              check=False, env=foreign_git_environment() if foreign else None)
    if finished.returncode != 0:
        return None
    return [line for line in finished.stdout.split("\n") if line]


def changed_paths(since="origin/main", subject=None, prefixes=(), mounted=False):
    """The files the branch changed from its merge base with `since`, library mounts read as files.

    A library a repository carries moves by its pinned commit, so the repository's
    own diff names the mount, not the files. Each mount that moved is replaced by
    the files between its two pins, read in its own checkout and spelled as that
    repository spells them. A mount whose pins cannot be read stays as its path,
    which a caller reads as "anything may have changed".

    - Parameters:
      - since: the ref the branch forked from.
      - subject: the repository; the working directory's by default.
      - prefixes: when given, only paths starting with one of them are returned
        (an unreadable mount is kept whatever the prefixes).
      - mounted: also spell each library file by its path through the mount, as a
        build of the repository records it.
    - Returns: a list of paths, or None when the merge base cannot be found.
    """
    base_root = subject_root() if subject is None else subject
    merged = _git_lines(["merge-base", "HEAD", since], base_root)
    if not merged:
        return None
    names = _git_lines(["diff", "--name-only", merged[0], "HEAD"], base_root)
    if names is None:
        return None
    expanded = []
    for name in names:
        if name not in (CORE_SUBMODULE, KIT_SUBMODULE):
            expanded.append(name)
            continue
        old = _git_lines(["rev-parse", "--verify", "--quiet", "%s:%s" % (merged[0], name)], base_root)
        new = _git_lines(["rev-parse", "--verify", "--quiet", "HEAD:%s" % name], base_root)
        between = None
        if old and new:
            between = _git_lines(["diff", "--name-only", old[0], new[0]],
                                 os.path.join(base_root, name), foreign=True)
        if not between:
            expanded.append(name)
            continue
        expanded.extend(between)
        if mounted:
            expanded.extend(os.path.join(name, path) for path in between)
    if not prefixes:
        return expanded
    return [name for name in expanded
            if name.startswith(tuple(prefixes)) or name in (CORE_SUBMODULE, KIT_SUBMODULE)]


def scripts_of(root):
    """The Scripts directory of a repository root."""
    return os.path.join(root, "Scripts")


def main(argv):
    """Prints one answer for a shell caller: a root, the repositories, or a budget."""
    if len(argv) == 2 and argv[0] == "budget":
        try:
            print(budget(argv[1]))
        except KeyError:
            print("ubq_roots: no ledger of this checkout sets budget %s" % argv[1], file=sys.stderr)
            return 1
        return 0
    if len(argv) >= 2 and argv[0] == "changed":
        changed = changed_paths(argv[1], prefixes=argv[2:])
        if changed is None:
            return 1
        print("\n".join(changed))
        return 0
    if len(argv) != 1 or argv[0] not in ("subject", "core", "kit", "repositories"):
        print("usage: ubq_roots.py subject|core|kit|repositories|budget <name>|changed <since> [prefix...]",
              file=sys.stderr)
        return 2
    if argv[0] == "subject":
        print(subject_root())
    elif argv[0] == "core":
        print(core_root())
    elif argv[0] == "kit":
        print(kit_root())
    else:
        print(" ".join(repositories()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
