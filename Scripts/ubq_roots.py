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

Neither needs setting. The Kit's root defaults to Libraries/ubiquitty-kit
under the subject when that path exists. Core's defaults to the Kit's own
Libraries/ubiquitty-core when the subject carries the Kit, so the app builds the
Core its Kit pins, and to Libraries/ubiquitty-core under the subject when the
subject carries Core and no Kit, as ubiquitty-kit does. Each defaults to the
subject itself otherwise, so inside one repository that holds everything, and
inside ubiquitty-core itself, every root is the same tree.

A checkout names the repository it is in with Scripts/repository.txt, holding
`core`, `kit` or `app`. A checkout with no such file holds every repository's
files and is judged as all of them.
"""

import fnmatch
from dataclasses import dataclass
import os
from pathlib import Path
from typing import Optional
import subprocess
import sys

REPOSITORIES = ("core", "kit", "app")

CORE_SUBMODULE = os.path.join("Libraries", "ubiquitty-core")
KIT_SUBMODULE = os.path.join("Libraries", "ubiquitty-kit")
# Core as a repository that carries the Kit holds it: the Kit's own submodule.
CORE_THROUGH_KIT = os.path.join(KIT_SUBMODULE, CORE_SUBMODULE)
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


def core_submodule(subject):
    """Where the subject carries Core, relative to it.

    A subject that carries the Kit reads the Kit's own Core, the one that Kit
    pins, whatever else it holds; a subject that carries Core and no Kit reads
    its own submodule. A mount is chosen by existing, not by being populated: a
    checkout that has not initialised it must fail on the missing files rather
    than quietly judge its own tree as the library's.

    - Parameters:
      - subject: the subject's root.
    - Returns: `CORE_THROUGH_KIT`, `CORE_SUBMODULE`, or None when it carries neither.
    """
    if os.path.lexists(os.path.join(subject, KIT_SUBMODULE)):
        return CORE_THROUGH_KIT
    if os.path.lexists(os.path.join(subject, CORE_SUBMODULE)):
        return CORE_SUBMODULE
    return None


def kit_submodule(subject):
    """Where the subject carries the Kit, relative to it, or None when it does not."""
    return KIT_SUBMODULE if os.path.lexists(os.path.join(subject, KIT_SUBMODULE)) else None


def _root(variable, mount, subject):
    """A sibling repository's root: the variable, the mount the subject carries, else the subject."""
    configured = os.environ.get(variable)
    if configured:
        return os.path.abspath(os.path.expanduser(configured))
    base = subject_root() if subject is None else subject
    mounted = mount(base)
    return os.path.join(base, mounted) if mounted else base


def core_root(subject=None):
    """The ubiquitty-core checkout this subject reads.

    - Parameters:
      - subject: the subject's root; the working directory's repository by default.
    """
    return _root("UBQ_CORE_ROOT", core_submodule, subject)


def kit_root(subject=None):
    """The ubiquitty-kit checkout this subject reads.

    - Parameters:
      - subject: the subject's root; the working directory's repository by default.
    """
    return _root("UBQ_KIT_ROOT", kit_submodule, subject)


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
    it instead. A line `raised <name> <from> <to> <reason>` records why a budget
    rose past the merge base's number; Scripts/check-ledger-ratchet.py judges it,
    and the budget it names is read from its own `budget` line.

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
                elif len(words) >= 5 and words[0] == "raised":
                    continue
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


def _mount_changes(checkout, mount, old, new, foreign):
    """The files a library mount changed between two commits of the checkout holding it.

    The Kit's own mount of Core is read the same way inside the Kit's checkout,
    so a repository that carries Core through its Kit sees Core's files. A
    mount whose pins cannot be read stays as its path.

    - Parameters:
      - checkout: the checkout holding the mount.
      - mount: `CORE_SUBMODULE` or `KIT_SUBMODULE`.
      - old, new: the commits of `checkout` to read the pins from.
      - foreign: whether `checkout` is a repository other than the one being operated on.
    - Returns: (as the library spells it, as `checkout` spells it through the mount)
      pairs, or None when either pin, or the diff between them, cannot be read.
    """
    pins = [_git_lines(["rev-parse", "--verify", "--quiet", "%s:%s" % (commit, mount)], checkout, foreign)
            for commit in (old, new)]
    if not pins[0] or not pins[1]:
        return None
    library = os.path.join(checkout, mount)
    between = _git_lines(["diff", "--name-only", pins[0][0], pins[1][0]], library, foreign=True)
    if not between:
        return None
    changes = []
    for path in between:
        nested = None
        if mount == KIT_SUBMODULE and path == CORE_SUBMODULE:
            nested = _mount_changes(library, CORE_SUBMODULE, pins[0][0], pins[1][0], True)
        for spelled, through in nested if nested is not None else ((path, path),):
            changes.append((spelled, os.path.join(mount, through)))
    return changes


def changed_paths(since="origin/main", subject=None, prefixes=(), mounted=False):
    """The files the branch changed from its merge base with `since`, library mounts read as files.

    A library a repository carries moves by its pinned commit, so the repository's
    own diff names the mount, not the files. Each mount that moved is replaced by
    the files between its two pins, read in its own checkout and spelled as that
    repository spells them, and Core moved inside a Kit that moved is read the
    same way. A mount whose pins cannot be read stays as its path, which a caller
    reads as "anything may have changed".

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
        changes = _mount_changes(base_root, name, merged[0], "HEAD", False) \
            if name in (CORE_SUBMODULE, KIT_SUBMODULE) else None
        if not changes:
            expanded.append(name)
            continue
        expanded.extend(spelled for spelled, _ in changes)
        if mounted:
            expanded.extend(through for _, through in changes)
    if not prefixes:
        return expanded
    return [name for name in expanded
            if name.startswith(tuple(prefixes)) or name in (CORE_SUBMODULE, KIT_SUBMODULE)]


def scripts_of(root):
    """The Scripts directory of a repository root."""
    return os.path.join(root, "Scripts")


@dataclass(frozen=True)
class SourceRoots:
    """The explicit source composition of a build/check, including gitless exports."""

    subject: Path
    core: Path
    kit: Optional[Path]
    repositories: tuple[str, ...]

    def resolve_file(self, relative):
        """Finds a required tool/data file in its actual owner; absence refuses."""
        if Path(relative).is_absolute() or ".." in Path(relative).parts:
            raise ValueError(f"source-relative path required: {relative}")
        for root in dict.fromkeys((self.subject, self.kit, self.core)):
            if root is not None and (root / relative).is_file():
                try:
                    (root / relative).resolve().relative_to(root)
                except ValueError as error:
                    raise ValueError(f"required source file escapes its owner: {root / relative}") from error
                return root / relative
        raise FileNotFoundError(f"required source file is absent from the composition: {relative}")

    def environment(self, original):
        """Binds nested tools to this subject without relying on git or cwd discovery."""
        result = {key: value for key, value in original.items() if key not in REPOSITORY_VARIABLES}
        result.update(UBQ_TESTS_ROOT=str(self.subject), UBQ_CORE_ROOT=str(self.core),
                      UBIQUITTY_CORE_SOURCE_DIR=str(self.core))
        if self.kit is not None:
            result.update(UBQ_KIT_ROOT=str(self.kit), UBIQUITTY_KIT_SOURCE_DIR=str(self.kit))
        else:
            result.pop("UBQ_KIT_ROOT", None)
            result.pop("UBIQUITTY_KIT_SOURCE_DIR", None)
        return result


def _source_subject(start, promote_libraries=True):
    """Finds the nearest caller, optionally promoting nested libraries to their consumer."""
    where = Path(start).expanduser().resolve()
    found = next((parent for parent in (where, *where.parents)
                  if (parent / "CMakeLists.txt").is_file() and (parent / "Scripts").is_dir()), None)
    if not promote_libraries:
        return found
    while found is not None and found.parent.name == "Libraries":
        if found.name not in (Path(CORE_SUBMODULE).name, Path(KIT_SUBMODULE).name):
            break
        consumer = found.parent.parent
        if not (consumer / "CMakeLists.txt").is_file() or not (consumer / REPOSITORY_FILE).is_file():
            break
        roles = repositories(str(found))
        parent_roles = repositories(str(consumer))
        if found.name == Path(CORE_SUBMODULE).name:
            if "core" not in roles or not set(parent_roles).intersection(("kit", "app")):
                break
        elif "kit" not in roles or "app" not in parent_roles:
            break
        found = consumer
    return found


def source_roots(source=None, core=None, kit=None, environment=None, start=None, promote_libraries=True):
    """Resolves a caller's immutable source composition, without requiring git.

    Explicit roots win, then the UBQ_TESTS_ROOT/UBQ_CORE_ROOT/UBQ_KIT_ROOT
    environment. A marked App requires both populated public libraries; Core
    alone never requires a repository above it. Markerless combined trees
    retain their one-root composition. A test entry point can keep the nearest
    checkout as its subject instead of promoting an embedded library to its
    consumer; explicit subjects always win.
    """
    environment = os.environ if environment is None else environment
    selected = source or environment.get("UBQ_TESTS_ROOT")
    subject = Path(selected).expanduser().resolve() if selected else _source_subject(start or os.getcwd(), promote_libraries)
    if subject is None:
        subject = Path(__file__).resolve().parent.parent
    held = repositories(str(subject))

    def checked(value, role):
        root = Path(value).expanduser().resolve()
        if not (root / "CMakeLists.txt").is_file() or not (root / "Scripts").is_dir():
            raise ValueError(f"{role} source root is not populated: {root}")
        roles = repositories(str(root))
        if role not in roles:
            raise ValueError(f"{role} source root names {'/'.join(roles)}, not {role}: {root}")
        return root

    if not (subject / "CMakeLists.txt").is_file() or not (subject / "Scripts").is_dir():
        raise ValueError(f"source root is not populated: {subject}")
    core_value = core or environment.get("UBQ_CORE_ROOT") or environment.get("UBIQUITTY_CORE_SOURCE_DIR")
    if not core_value:
        core_value = subject if "core" in held else subject / (core_submodule(str(subject)) or CORE_SUBMODULE)
    core_path = checked(core_value, "core")
    kit_value = kit or environment.get("UBQ_KIT_ROOT") or environment.get("UBIQUITTY_KIT_SOURCE_DIR")
    if not kit_value and "kit" in held:
        kit_value = subject
    if not kit_value and "app" in held:
        kit_value = subject / KIT_SUBMODULE
    kit_path = checked(kit_value, "kit") if kit_value else None
    return SourceRoots(subject, core_path, kit_path, held)


def main(argv):
    """Prints one answer for a shell caller: a root, the repositories, or a budget."""
    if argv[:1] == ["shell-context"]:
        import argparse
        import shlex
        parser = argparse.ArgumentParser()
        parser.add_argument("--source-root")
        parser.add_argument("--core-source-root")
        parser.add_argument("--kit-source-root")
        parser.add_argument("--no-promote-libraries", action="store_true")
        arguments = parser.parse_args(argv[1:])
        try:
            roots = source_roots(arguments.source_root, arguments.core_source_root, arguments.kit_source_root,
                                 promote_libraries=not arguments.no_promote_libraries)
        except ValueError as error:
            parser.error(str(error))
        print("unset " + " ".join((*REPOSITORY_VARIABLES, "UBQ_KIT_ROOT", "UBIQUITTY_KIT_SOURCE_DIR")))
        for key, value in (("ROOT", str(roots.subject)), ("CORE_ROOT", str(roots.core)),
                           ("KIT_ROOT", str(roots.kit) if roots.kit else "")):
            print(f"{key}={shlex.quote(value)}")
        for key, value in roots.environment({}).items():
            print(f"export {key}={shlex.quote(value)}")
        return 0
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
