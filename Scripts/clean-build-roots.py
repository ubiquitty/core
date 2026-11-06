#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""clean-build-roots.py — the build roots nobody needs, listed, and removed on request.

Every checkout builds into a root of its own, outside the checkout
(Scripts/build_root.py), and nothing removes that root when the checkout goes.
Worktrees are made and removed by the dozen, each leaving its build behind, and
the disks have filled that way more than once. Xcode's "Delete Derived Data"
clears every root, the main checkout's included; this clears only the ones no
checkout can still be using.

Each root the mapping can produce on this platform is listed: the
`Ubiquitty-<28 letters>` directories under build_root.derived_data(), which is
Xcode's DerivedData on macOS and $XDG_CACHE_HOME/ubiquitty/build elsewhere.
Each is mapped back to its worktree and given one class, the first that applies:

    KEEP    the main checkout's root; a build lane's root; a root the keep
            list names, or whose worktree it names; a root whose worktree has
            uncommitted changes;
            a root a running ninja, xcodebuild, swift-frontend, clang, cmake
            (or another compiler or linker) references, by its command line or
            its working directory; a root holding a worktree of its own.
    ORPHAN  the worktree is gone, or no checkout on this machine hashes to it.
    MERGED  the worktree is clean and its HEAD is an ancestor of its origin/main,
            as last fetched.
    STALE   no file in the root has been modified for --days days (7).
    KEEP    otherwise: unmerged work, built within those days.

    Scripts/clean-build-roots.py                    # dry run: the table and the reclaimable total
    Scripts/clean-build-roots.py --delete           # remove the ORPHAN and MERGED roots
    Scripts/clean-build-roots.py --delete --stale   # and the STALE ones
    Scripts/clean-build-roots.py --days 14
    Scripts/clean-build-roots.py --self-test

A root is mapped to its worktree two ways. By name: build_root.path_hash, the
function that named it, over every checkout this machine knows of — each
worktree of the main checkout's repository and of this one (--repo adds more),
and each checkout directly under ~/Projects, /tmp, $TMPDIR and the directories
those worktrees live in (--search adds more). Each is hashed under every
spelling it could have been built from, since /tmp/x and /private/tmp/x hash
apart. A worktree git still lists after its directory went is known, and gone.
By record: the source directory each CMakeCache.txt in the root names, and the
workspace an Xcode info.plist names. A hash cannot be reversed, so a root
nothing hashes to and that records no source is an ORPHAN: every checkout that
could own it was hashed.

A build lane (Scripts/build-lane.py) is a worktree named `lane-<n>` with its
lock file, `lane-<n>.lock`, beside it. Between runs a lane is detached and
clean at a commit main has, which is what MERGED describes, and its root is the
warm build every run in the lane reuses; so a lane's root is kept by name.

The keep list is ~/.config/ubiquitty/keep-build-roots ($XDG_CONFIG_HOME when
set), one entry per line, `#` starting a comment: a root's name or path, or a
worktree's path, which keeps the root that worktree builds into.

It never touches sources. A removal is refused for anything that is not a
directory directly under the build-root parent with a root's name, for a
symbolic link, and for a root holding a worktree (the merged-main check keeps
its merge in one). The running processes are read again just
before removing, and a root one has started using since is left in place.
Reading is read-only: git runs without optional locks, so not even an index
refresh is written.

Exit 0 = listed (and removed, with --delete), 1 = a removal failed or was
refused, 2 = usage.
"""

import argparse
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build_root  # noqa: E402 — the path above is what makes it importable

TOOL = "clean-build-roots"

# A root the mapping can produce: the project's name and Xcode's 28 letters.
ROOT_NAME = re.compile(r"^" + re.escape(build_root.PROJECT) + r"-([a-z]{28})$")

KEEP, ORPHAN, MERGED, STALE = "KEEP", "ORPHAN", "MERGED", "STALE"
ORDER = (ORPHAN, MERGED, STALE, KEEP)

DEFAULT_DAYS = 7
DAY = 86400

# At most four of anything at once: the machines this runs on are shared with
# builds, and a walk over every root is disk-bound anyway.
WORKERS = 4

# The processes whose use of a root makes it busy. The named five, and the
# other compilers, linkers and drivers a build of this tree starts: GCC on
# Linux, and the drivers above swift-frontend.
BUILD_PROCESS = re.compile(
    r"^(ninja|make|gmake|cmake|ctest|xcodebuild|swift-frontend|swift-driver"
    r"|swiftc|swift-build|clang(\+\+)?(-\d+)?|cc|c\+\+|gcc(-\d+)?|g\+\+(-\d+)?"
    r"|cc1|cc1plus|ld|ld64|ld\.lld|lld|ccache)$")

# A build lane's worktree, as Scripts/build-lane.py names it.
LANE_NAME = re.compile(r"^lane-[1-9][0-9]*$")

# Directories that are one directory under two names, so a path built from one
# hashes apart from the same path built from the other.
ALIASES = (("/tmp", "/private/tmp"), ("/var", "/private/var"),
           ("/etc", "/private/etc"))

# The environment a hook leaves behind names the pushing worktree, and git
# obeys it over `-C`; every git command here asks about a directory it names.
GIT_LOCAL_VARIABLES = ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
                       "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY", "GIT_PREFIX",
                       "GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_NAMESPACE")


def git(where, *args):
    """Runs git in a directory, read-only, and returns (status, stdout).

    - Parameters:
      - where: The directory git runs in.
      - args: The git arguments.
    - Returns: The exit status and the standard output, stripped.
    """
    environment = {k: v for k, v in os.environ.items()
                   if k not in GIT_LOCAL_VARIABLES}
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    try:
        finished = subprocess.run(["git", "-C", where, *args],
                                  capture_output=True, text=True, check=False,
                                  env=environment)
    except OSError:
        return 1, ""
    return finished.returncode, finished.stdout.strip()


def spellings(path):
    """Every spelling a build of this path could have hashed.

    - Parameters:
      - path: An absolute path, existing or not.
    - Returns: A set holding the path, its resolved form, and each form with a
      /tmp, /var or /etc prefix swapped for its /private alias and back.
    """
    found = {os.path.abspath(path), os.path.realpath(path)}
    for spelling in list(found):
        for short, long in ALIASES:
            if spelling == short or spelling.startswith(short + "/"):
                found.add(long + spelling[len(short):])
            elif spelling == long or spelling.startswith(long + "/"):
                found.add(short + spelling[len(long):])
    return found


def worktrees(repository):
    """The worktrees git lists for a repository, gone ones included.

    - Parameters:
      - repository: Any checkout of the repository.
    - Returns: [(path, present)], where present is False for a worktree whose
      directory is gone (git calls it prunable).
    """
    status, out = git(repository, "worktree", "list", "--porcelain")
    if status != 0:
        return []
    listed = []
    path, present = None, True
    for line in out.splitlines() + [""]:
        if line.startswith("worktree "):
            path, present = line[len("worktree "):], True
        elif line.startswith("prunable"):
            present = False
        elif not line and path:
            listed.append((path, present and os.path.isdir(path)))
            path, present = None, True
    return listed


def common_dir(checkout):
    """The repository a checkout belongs to, as its absolute common git dir, or None."""
    status, out = git(checkout, "rev-parse", "--path-format=absolute",
                      "--git-common-dir")
    return os.path.realpath(out) if status == 0 and out else None


def default_searches():
    """The directories whose children are checked for checkouts by default."""
    found = [os.path.expanduser("~/Projects"), "/tmp"]
    if os.environ.get("TMPDIR"):
        found.append(os.environ["TMPDIR"])
    return found


def default_mains():
    """The main checkouts: ~/Projects/ubiquitty, and this script's repository's first worktree."""
    found = [os.path.expanduser("~/Projects/ubiquitty")]
    listed = worktrees(os.path.dirname(HERE))
    if listed:
        found.append(listed[0][0])
    return found


def known_checkouts(repositories, searches):
    """Every checkout this machine knows of, keyed by path.

    - Parameters:
      - repositories: Checkouts whose repositories' worktrees are all listed.
      - searches: Directories whose immediate children are checked for a .git.
    - Returns: {path: present}, where present is False for a worktree git
      still lists after its directory went.
    """
    found = {}
    seen = set()

    def add_repository(checkout):
        repository = common_dir(checkout)
        if not repository or repository in seen:
            return
        seen.add(repository)
        for path, present in worktrees(checkout):
            found[path] = found.get(path, False) or present

    for checkout in repositories:
        if os.path.isdir(checkout):
            add_repository(checkout)
    directories = list(searches)
    directories += sorted({os.path.dirname(path) for path in found})
    searched = set()
    for directory in directories:
        directory = os.path.realpath(directory)
        if directory in searched or not os.path.isdir(directory):
            continue
        searched.add(directory)
        try:
            children = sorted(os.scandir(directory), key=lambda e: e.name)
        except OSError:
            continue
        for child in children:
            try:
                if not child.is_dir(follow_symlinks=False):
                    continue
            except OSError:
                continue
            if not os.path.lexists(os.path.join(child.path, ".git")):
                continue
            found.setdefault(child.path, True)
            add_repository(child.path)
    return found


def hash_index(checkouts):
    """Maps each hash a checkout could have built under to that checkout.

    - Parameters:
      - checkouts: {path: present}, as known_checkouts returns.
    - Returns: {hash: path}. A present checkout wins a hash over a gone one.
    """
    index = {}
    for path in sorted(checkouts, key=lambda p: checkouts[p]):
        for spelling in spellings(path):
            index[build_root.path_hash(spelling)] = path
    return index


def recorded_sources(root):
    """The source directories a root records.

    - Parameters:
      - root: A build root.
    - Returns: The sorted, distinct CMAKE_HOME_DIRECTORY of every CMakeCache.txt
      up to two directories down, and the directory of the workspace an Xcode
      info.plist at the top names.
    """
    found = set()
    caches = [os.path.join(root, "CMakeCache.txt")]
    for first in _subdirectories(root):
        caches.append(os.path.join(first, "CMakeCache.txt"))
        for second in _subdirectories(first):
            caches.append(os.path.join(second, "CMakeCache.txt"))
    for cache in caches:
        try:
            with open(cache, encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    if line.startswith("CMAKE_HOME_DIRECTORY:"):
                        found.add(line.split("=", 1)[1].strip())
                        break
        except OSError:
            continue
    try:
        with open(os.path.join(root, "info.plist"), "rb") as handle:
            workspace = plistlib.load(handle).get("WorkspacePath")
        if workspace:
            found.add(os.path.dirname(workspace))
    except (OSError, plistlib.InvalidFileException, AttributeError, ValueError):
        pass
    return sorted(path for path in found if os.path.isabs(path))


def _subdirectories(directory):
    """The real subdirectories of a directory, symbolic links excluded."""
    try:
        return [entry.path for entry in os.scandir(directory)
                if entry.is_dir(follow_symlinks=False)]
    except OSError:
        return []


def checkout_of(source, wanted_hash):
    """The checkout a recorded source directory belongs to.

    - Parameters:
      - source: A directory a root records.
      - wanted_hash: The root's hash; a gone source's ancestor hashing to it is
        the checkout it was.
    - Returns: (path, present). A present source answers with its git top
      level; a gone one with its ancestor that hashes to the root, or itself.
    """
    if os.path.isdir(source):
        status, top = git(source, "rev-parse", "--show-toplevel")
        return (top if status == 0 and top else source), True
    candidate = source
    while candidate and candidate != os.path.dirname(candidate):
        if any(build_root.path_hash(s) == wanted_hash for s in spellings(candidate)):
            return candidate, os.path.isdir(candidate)
        candidate = os.path.dirname(candidate)
    return source, False


def footprint(root):
    """What a root occupies, when it last changed, and any worktree inside it.

    - Parameters:
      - root: A build root.
    - Returns: (bytes, newest, nested). Bytes counts allocated blocks, a file
      linked twice once; newest is the latest modification time of anything in
      it; nested lists the directories holding a `.git` file, which is a linked
      worktree and so sources. A `.git` directory is a fetched dependency's
      clone, and is a build's.
    """
    total, newest, nested = 0, 0.0, []
    seen = set()
    stack = [root]
    while stack:
        directory = stack.pop()
        try:
            entries = list(os.scandir(directory))
        except OSError:
            continue
        for entry in entries:
            try:
                info = entry.stat(follow_symlinks=False)
            except OSError:
                continue
            newest = max(newest, info.st_mtime)
            if entry.name == ".git" and entry.is_file(follow_symlinks=False):
                nested.append(directory)
            if entry.is_dir(follow_symlinks=False):
                stack.append(entry.path)
                total += getattr(info, "st_blocks", 0) * 512
                continue
            key = (info.st_dev, info.st_ino)
            if info.st_nlink > 1:
                if key in seen:
                    continue
                seen.add(key)
            total += getattr(info, "st_blocks", 0) * 512 or info.st_size
    return total, newest, sorted(nested)


def build_processes():
    """The running build processes, with what each names and where it works.

    - Returns: [(pid, name, args, cwd)], cwd being "" where it cannot be read.
    """
    try:
        finished = subprocess.run(["ps", "axww", "-o", "pid=,comm=,args="],
                                  capture_output=True, text=True, check=False)
    except OSError:
        return []
    found = []
    for line in finished.stdout.splitlines():
        parts = line.strip().split(None, 2)
        if len(parts) < 2 or not parts[0].isdigit():
            continue
        # macOS prints comm as the executable's path, which may hold spaces;
        # args then repeats it. The program's name is the first word of args.
        args = parts[2] if len(parts) > 2 else parts[1]
        name = os.path.basename(args.split()[0]) if args.split() else ""
        if not BUILD_PROCESS.match(name):
            name = os.path.basename(parts[1])
            if not BUILD_PROCESS.match(name):
                continue
        found.append((int(parts[0]), name, args))
    directories = working_directories([pid for pid, _, _ in found])
    return [(pid, name, args, directories.get(pid, ""))
            for pid, name, args in found]


def working_directories(pids):
    """Each process's working directory, from /proc where there is one, else lsof."""
    found = {}
    if not pids:
        return found
    if os.path.isdir("/proc/self"):
        for pid in pids:
            try:
                found[pid] = os.readlink(f"/proc/{pid}/cwd")
            except OSError:
                pass
        return found
    try:
        finished = subprocess.run(
            ["lsof", "-a", "-d", "cwd", "-Fpn", "-p", ",".join(map(str, pids))],
            capture_output=True, text=True, check=False)
    except OSError:
        return found
    pid = None
    for line in finished.stdout.splitlines():
        if line.startswith("p") and line[1:].isdigit():
            pid = int(line[1:])
        elif line.startswith("n") and pid is not None:
            found[pid] = line[1:]
    return found


def users(processes, root):
    """The build processes referencing a root, as "name pid" strings."""
    forms = spellings(root)
    found = []
    for pid, name, args, cwd in processes:
        named = any(form in args for form in forms)
        inside = any(cwd == form or cwd.startswith(form + "/") for form in forms)
        if named or inside:
            found.append(f"{name} {pid}")
    return found


def lane_lock(worktree):
    """The lock file Scripts/build-lane.py holds beside a lane's worktree."""
    return worktree.rstrip("/") + ".lock"


def is_build_lane(worktree):
    """Whether a worktree is a build lane.

    - Parameters:
      - worktree: A directory a root was mapped to.
    - Returns: True for an existing directory named `lane-<n>` with
      `lane-<n>.lock` beside it.
    """
    return (bool(LANE_NAME.match(os.path.basename(worktree.rstrip("/"))))
            and os.path.isdir(worktree) and os.path.isfile(lane_lock(worktree)))


def keep_file():
    """Where the keep list lives."""
    config = os.environ.get("XDG_CONFIG_HOME", "")
    if not os.path.isabs(config):
        config = os.path.expanduser("~/.config")
    return os.path.join(config, "ubiquitty", "keep-build-roots")


def read_keep(path):
    """The keep list's entries, each in every spelling, or an empty set when there is none."""
    entries = set()
    try:
        with open(path, encoding="utf-8") as handle:
            for line in handle:
                entry = line.split("#", 1)[0].strip()
                if not entry:
                    continue
                entry = os.path.expanduser(entry).rstrip("/") or "/"
                if os.path.isabs(entry):
                    entries |= spellings(entry)
                else:
                    entries.add(entry)
    except OSError:
        pass
    return entries


class Root:
    """One build root: where it is, whose it is, what it holds, and its class."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.hash = ROOT_NAME.match(self.name).group(1)
        self.worktree = None
        self.present = False
        self.size = 0
        self.newest = 0.0
        self.nested = []
        self.cls = KEEP
        self.why = ""


def roots_under(parent):
    """The build roots directly under the parent, symbolic links excluded."""
    try:
        entries = sorted(os.scandir(parent), key=lambda e: e.name)
    except OSError:
        return []
    return [Root(entry.path) for entry in entries
            if ROOT_NAME.match(entry.name) and entry.is_dir(follow_symlinks=False)]


def worktree_state(path):
    """A worktree's state, for classifying its root.

    - Parameters:
      - path: A directory a root was mapped to.
    - Returns: (is_checkout, dirty, merged). merged is whether HEAD is an
      ancestor of the checkout's own origin/main as last fetched.
    """
    status, _ = git(path, "rev-parse", "--verify", "--quiet", "HEAD")
    if status != 0:
        return False, False, False
    status, out = git(path, "status", "--porcelain", "--untracked-files=normal",
                      "--ignore-submodules=dirty")
    dirty = status != 0 or bool(out)
    status, _ = git(path, "merge-base", "--is-ancestor", "HEAD",
                    "refs/remotes/origin/main")
    return True, dirty, status == 0


def survey(parent, mains, repositories, searches, keep, processes, days,
           now=None):
    """Every root under the parent, mapped to its worktree and classified.

    - Parameters:
      - parent: The directory holding the roots.
      - mains: The main checkouts, whose roots are always kept.
      - repositories: Checkouts whose repositories' worktrees are hashed.
      - searches: Directories whose children are hashed when they are checkouts.
      - keep: The keep list's entries, as read_keep returns them.
      - processes: The running build processes, as build_processes returns them.
      - days: How long untouched makes a root STALE.
      - now: The time to measure staleness from; the current time by default.
    - Returns: [Root], each with its worktree, size and class filled in.
    """
    now = time.time() if now is None else now
    roots = roots_under(parent)
    checkouts = known_checkouts(list(mains) + list(repositories), searches)
    index = hash_index(checkouts)
    main_forms = set()
    for main in mains:
        main_forms |= spellings(main)

    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        sizes = list(pool.map(lambda r: footprint(r.path), roots))
    for root, (size, newest, nested) in zip(roots, sizes):
        root.size, root.newest, root.nested = size, newest, nested
        if root.hash in index:
            root.worktree = index[root.hash]
            root.present = os.path.isdir(root.worktree)
            continue
        for source in recorded_sources(root.path):
            path, present = checkout_of(source, root.hash)
            if root.worktree is None or any(
                    build_root.path_hash(s) == root.hash for s in spellings(path)):
                root.worktree, root.present = path, present

    owners = sorted({r.worktree for r in roots if r.worktree and r.present})
    with ThreadPoolExecutor(max_workers=WORKERS) as pool:
        states = dict(zip(owners, pool.map(worktree_state, owners)))

    for root in roots:
        root.cls, root.why = classify(root, main_forms, keep, processes, states,
                                      days, now)
    return roots


def classify(root, main_forms, keep, processes, states, days, now):
    """A root's class and the reason for it; see the module's header for the rules."""
    owner_forms = spellings(root.worktree) if root.worktree else set()
    if owner_forms & main_forms:
        return KEEP, "the main checkout"
    if root.worktree and is_build_lane(root.worktree):
        return KEEP, "a build lane (Scripts/build-lane.py)"
    if root.name in keep or spellings(root.path) & keep:
        return KEEP, "named in the keep list"
    if owner_forms & keep:
        return KEEP, "its worktree is in the keep list"
    busy = users(processes, root.path)
    if busy:
        return KEEP, "in use by " + ", ".join(busy[:3])
    if root.nested:
        return KEEP, "holds a worktree: " + os.path.relpath(root.nested[0], root.path)
    if root.worktree is None:
        return ORPHAN, "no checkout on this machine hashes to it"
    if not root.present:
        return ORPHAN, "its worktree is gone"
    is_checkout, dirty, merged = states.get(root.worktree, (False, False, False))
    if dirty:
        return KEEP, "its worktree has uncommitted changes"
    if is_checkout and merged:
        return MERGED, "HEAD is in origin/main and the tree is clean"
    idle = (now - root.newest) / DAY
    if idle >= days:
        return STALE, f"untouched for {int(idle)} days"
    if not is_checkout:
        return KEEP, f"not a git checkout; built {ago(idle)}"
    return KEEP, f"unmerged; built {ago(idle)}"


def ago(days):
    """A whole number of days in the past, in words."""
    whole = int(days)
    return "today" if whole == 0 else "1 day ago" if whole == 1 else f"{whole} days ago"


def removable(roots, stale):
    """The roots --delete removes: ORPHAN and MERGED, and STALE when asked."""
    wanted = {ORPHAN, MERGED} | ({STALE} if stale else set())
    return [root for root in roots if root.cls in wanted]


def human(size):
    """A byte count as a short figure in binary units."""
    value = float(size)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if value < 1024 or unit == "GiB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{value:.1f} GiB"


def shown(path):
    """A path with the home directory spelled ~."""
    if not path:
        return "-"
    home = os.path.expanduser("~")
    return "~" + path[len(home):] if path == home or path.startswith(home + "/") else path


def report(roots, parent, days, out=sys.stdout):
    """Prints the table and the totals.

    - Parameters:
      - roots: The surveyed roots.
      - parent: The directory holding them, named in the heading.
      - days: The staleness threshold, named in the totals.
      - out: Where to print.
    """
    ordered = sorted(roots, key=lambda r: (ORDER.index(r.cls), -r.size, r.name))
    print(f"{TOOL}: {len(roots)} build root(s) under {shown(parent)}", file=out)
    if roots:
        width = max(len(r.name) for r in roots)
        tree = min(60, max(8, max(len(shown(r.worktree)) for r in roots)))
        print(f"  {'ROOT':<{width}}  {'WORKTREE':<{tree}}  {'CLASS':<6}  {'SIZE':>10}  WHY",
              file=out)
        for r in ordered:
            print(f"  {r.name:<{width}}  {shown(r.worktree):<{tree}}  {r.cls:<6}  "
                  f"{human(r.size):>10}  {r.why}", file=out)
    totals = {cls: [0, 0] for cls in ORDER}
    for r in roots:
        totals[r.cls][0] += 1
        totals[r.cls][1] += r.size
    print("  " + "; ".join(f"{cls} {totals[cls][0]} ({human(totals[cls][1])})"
                           for cls in ORDER), file=out)
    plain = sum(r.size for r in removable(roots, False))
    every = sum(r.size for r in removable(roots, True))
    print(f"  reclaimable: {human(plain)} with --delete; {human(every)} with "
          f"--delete --stale (STALE = untouched for {days} days)", file=out)


def remove(root, parent, processes):
    """Removes one root, after checking again that it is a root and is idle.

    - Parameters:
      - root: The root to remove.
      - parent: The only directory a root may be removed from.
      - processes: The build processes running now.
    - Returns: None when removed, else the reason it was not.
    """
    path = root.path
    if os.path.dirname(os.path.abspath(path)) != os.path.abspath(parent):
        return "not directly under the build-root parent"
    if not ROOT_NAME.match(os.path.basename(path)):
        return "not named as a build root"
    if os.path.islink(path) or not os.path.isdir(path):
        return "not a directory"
    busy = users(processes, path)
    if busy:
        return "in use by " + ", ".join(busy[:3])
    for directory in [path] + _subdirectories(path):
        if os.path.isfile(os.path.join(directory, ".git")):
            return "holds a worktree"
    failures = []
    shutil.rmtree(path, onerror=lambda _f, p, e: failures.append(f"{p}: {e[1]}"))
    if failures:
        return f"{len(failures)} entr(ies) could not be removed, first {failures[0]}"
    return None


def delete(roots, parent, stale, processes, out=sys.stdout):
    """Removes the removable roots and reports each.

    - Parameters:
      - roots: The surveyed roots.
      - parent: The directory holding them.
      - stale: Whether STALE roots go too.
      - processes: The build processes running now, read after the survey.
      - out: Where to print.
    - Returns: 0 when every removal succeeded, 1 otherwise.
    """
    status, freed = 0, 0
    for root in removable(roots, stale):
        refused = remove(root, parent, processes)
        if refused:
            print(f"  kept     {root.name}: {refused}", file=out)
            status = 1
            continue
        freed += root.size
        print(f"  removed  {root.name}  {root.cls:<6}  {human(root.size):>10}  "
              f"{shown(root.worktree)}", file=out)
    print(f"{TOOL}: removed {human(freed)}", file=out)
    return status


def main(argv):
    """Parses the arguments, then lists the roots and removes them when asked."""
    parser = argparse.ArgumentParser(
        prog="clean-build-roots.py",
        description="List build roots no checkout needs, and remove them with --delete.")
    parser.add_argument("--delete", action="store_true",
                        help="remove the ORPHAN and MERGED roots")
    parser.add_argument("--stale", action="store_true",
                        help="with --delete, remove the STALE roots too")
    parser.add_argument("--days", type=int, default=DEFAULT_DAYS,
                        help=f"untouched days that make a root STALE ({DEFAULT_DAYS})")
    parser.add_argument("--repo", action="append", default=[],
                        help="a checkout whose repository's worktrees are hashed too")
    parser.add_argument("--search", action="append", default=[],
                        help="a directory whose children are hashed when they are checkouts")
    parser.add_argument("--main", action="append", default=[],
                        help="a main checkout, whose root is always kept")
    parser.add_argument("--keep-file", default=None,
                        help=f"the keep list ({shown(keep_file())})")
    parser.add_argument("--self-test", action="store_true",
                        help="check the classification against a planted tree")
    options = parser.parse_args(argv)
    if options.self_test:
        return _self_test()
    if options.stale and not options.delete:
        parser.error("--stale only widens --delete")
    if options.days < 0:
        parser.error("--days must not be negative")

    parent = build_root.derived_data()
    if not os.path.isdir(parent):
        print(f"{TOOL}: no build-root parent at {shown(parent)}; nothing to list")
        return 0
    mains = default_mains() + options.main
    roots = survey(parent, mains, [os.path.dirname(HERE)] + options.repo,
                   default_searches() + options.search,
                   read_keep(options.keep_file or keep_file()),
                   build_processes(), options.days)
    report(roots, parent, options.days)
    if not options.delete:
        return 0
    return delete(roots, parent, options.stale, build_processes())


def _self_test():
    """The classes, the mapping and the removals, against a planted tree."""
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    saved = dict(os.environ)
    scratch = tempfile.mkdtemp(prefix="clean-build-roots-")
    try:
        # A home, a git configuration and an author of its own, so nothing here
        # reads or writes the person's.
        os.environ.update({
            "HOME": os.path.join(scratch, "home"),
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "user@example.com",
            "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "user@example.com",
        })
        for variable in ("UBQ_BUILD_ROOT", "UBIQUITTY_DERIVED_DATA", "XDG_CONFIG_HOME"):
            os.environ.pop(variable, None)
        os.makedirs(os.environ["HOME"])
        _planted(scratch, check)
    finally:
        os.environ.clear()
        os.environ.update(saved)
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print(f"{TOOL} --self-test: {len(failures)} claim(s) the cleaner does not keep",
              file=sys.stderr)
        return 1
    print(f"{TOOL} --self-test: ok (each class is found, and --delete removes "
          f"only roots, and only the ones it lists)")
    return 0


def _planted(scratch, check):
    """Plants checkouts and roots under scratch and checks what the cleaner makes of them."""
    def run(where, *args):
        subprocess.run(["git", "-C", where, *args], check=True,
                       capture_output=True, text=True)

    def commit(where, name):
        with open(os.path.join(where, name), "w", encoding="utf-8") as handle:
            handle.write(name + "\n")
        run(where, "add", name)
        run(where, "commit", "--quiet", "--no-verify", "-m", name)

    parent = os.path.join(scratch, "DerivedData")
    os.makedirs(parent)
    origin = os.path.join(scratch, "origin.git")
    main = os.path.join(scratch, "main")
    subprocess.run(["git", "init", "--quiet", "--bare", "-b", "main", origin],
                   check=True, capture_output=True)
    subprocess.run(["git", "clone", "--quiet", origin, main], check=True,
                   capture_output=True)
    commit(main, "first")
    run(main, "push", "--quiet", "origin", "HEAD:main")
    run(main, "fetch", "--quiet", "origin")

    def worktree(name, branch_commit=None):
        path = os.path.join(scratch, name)
        run(main, "worktree", "add", "--quiet", "--detach", path, "origin/main")
        if branch_commit:
            commit(path, branch_commit)
        return path

    merged = worktree("merged")
    dirty = worktree("dirty")
    with open(os.path.join(dirty, "unsaved"), "w", encoding="utf-8") as handle:
        handle.write("work\n")
    ahead = worktree("ahead", "ahead-work")
    fresh = worktree("fresh", "fresh-work")
    gone = worktree("gone")
    kept = worktree("kept")
    busy = worktree("busy")

    def plant(checkout=None, name=None, files=("build/out.o",)):
        root = (build_root.build_root(checkout) if checkout
                else os.path.join(parent, name))
        root = os.path.join(parent, os.path.basename(root))
        for relative in files:
            path = os.path.join(root, relative)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as handle:
                handle.write(b"\0" * 8192)
        return root

    # Built while the checkout existed, as a build would: the hash is taken of
    # the path git reports, and only then does the directory go.
    roots = {name: plant(path) for name, path in (
        ("main", main), ("merged", merged), ("dirty", dirty), ("ahead", ahead),
        ("fresh", fresh), ("gone", gone), ("kept", kept), ("busy", busy))}
    shutil.rmtree(gone)
    roots["unknown"] = plant(name="Ubiquitty-" + "q" * 28)
    roots["recorded"] = plant(name="Ubiquitty-" + "r" * 28, files=("macos/CMakeCache.txt",))
    with open(os.path.join(roots["recorded"], "macos", "CMakeCache.txt"), "w",
              encoding="utf-8") as handle:
        handle.write("CMAKE_HOME_DIRECTORY:INTERNAL=" + os.path.join(scratch, "vanished")
                     + "\n")
    roots["nested"] = plant(name="Ubiquitty-" + "n" * 28)
    run(main, "worktree", "add", "--quiet", "--detach",
        os.path.join(roots["nested"], "merged-main-src"), "origin/main")
    other = plant(name="Ubiquitty-short")
    os.symlink(roots["unknown"], os.path.join(parent, "Ubiquitty-" + "s" * 28))

    old = time.time() - 30 * DAY
    for directory, subdirectories, files in os.walk(roots["ahead"]):
        for entry in subdirectories + files:
            os.utime(os.path.join(directory, entry), (old, old))
    os.utime(roots["ahead"], (old, old))

    keep_list = os.path.join(scratch, "keep-build-roots")
    with open(keep_list, "w", encoding="utf-8") as handle:
        handle.write(f"# planted\n{kept}  # a worktree's path\n")
    processes = [(4242, "ninja", "ninja -C " + os.path.join(roots["busy"], "build"), "")]

    surveyed = survey(parent, [main], [], [scratch], read_keep(keep_list), processes,
                      DEFAULT_DAYS)
    by_path = {r.path: r for r in surveyed}

    def cls(name):
        root = by_path.get(roots[name])
        return root.cls if root else None

    check("only the roots the mapping can produce are listed",
          set(by_path) == set(roots.values()))
    check("a name that is not Xcode's 28 letters is not a root", other not in by_path)
    check("the main checkout's root is KEEP", cls("main") == KEEP)
    check("a clean worktree whose HEAD is in origin/main is MERGED", cls("merged") == MERGED)
    check("a worktree with uncommitted changes is KEEP", cls("dirty") == KEEP)
    check("unmerged work untouched for 30 days is STALE", cls("ahead") == STALE)
    check("unmerged work built today is KEEP", cls("fresh") == KEEP)
    check("a worktree git still lists after its directory went is ORPHAN",
          cls("gone") == ORPHAN)
    check("a root no checkout hashes to is ORPHAN", cls("unknown") == ORPHAN)
    check("a root whose recorded source is gone is ORPHAN, mapped to that source",
          cls("recorded") == ORPHAN
          and by_path[roots["recorded"]].worktree == os.path.join(scratch, "vanished"))
    check("a worktree in the keep list keeps its root", cls("kept") == KEEP)
    check("a root a running build names is KEEP", cls("busy") == KEEP)
    check("a root holding a worktree is KEEP", cls("nested") == KEEP)
    check("each root is mapped back to the worktree that built it",
          all(os.path.realpath(by_path[roots[n]].worktree) == os.path.realpath(p)
              for n, p in (("merged", merged), ("ahead", ahead), ("fresh", fresh))))
    if sys.platform == "darwin":
        check("/tmp and /private/tmp are one directory to the hash",
              "/private/tmp/x" in spellings("/tmp/x") and "/tmp/x" in spellings("/private/tmp/x"))

    listing = _Capture()
    report(surveyed, parent, DEFAULT_DAYS, out=listing)
    check("the dry run names every root and the reclaimable total",
          all(os.path.basename(p) in listing.text for p in roots.values())
          and "reclaimable:" in listing.text)
    check("a dry run removes nothing", all(os.path.isdir(p) for p in roots.values()))

    refused = remove(by_path[roots["nested"]], parent, [])
    check("a root holding a worktree is refused even when asked", refused is not None
          and os.path.isdir(roots["nested"]))
    stranger = Root(os.path.join(scratch, "Ubiquitty-" + "t" * 28))
    os.makedirs(stranger.path)
    check("a directory outside the parent is refused",
          remove(stranger, parent, []) is not None and os.path.isdir(stranger.path))
    check("a root a build started using since the survey is refused",
          remove(by_path[roots["merged"]], parent,
                 [(7, "clang", "clang -o " + roots["merged"] + "/a.o", "")]) is not None)

    status = delete(surveyed, parent, False, [], out=_Capture())
    left = {name for name, path in roots.items() if os.path.isdir(path)}
    check("--delete removes ORPHAN and MERGED and leaves STALE and KEEP",
          status == 0 and left == {"main", "dirty", "ahead", "fresh", "kept", "busy",
                                   "nested"})
    delete(surveyed, parent, True, [], out=_Capture())
    check("--delete --stale removes STALE too", not os.path.isdir(roots["ahead"]))
    check("no source is touched",
          all(os.path.isfile(os.path.join(p, "first"))
              for p in (main, merged, dirty, ahead, fresh, kept, busy))
          and os.path.isfile(os.path.join(dirty, "unsaved"))
          and os.path.isfile(os.path.join(roots["nested"], "merged-main-src", "first")))
    check("a symbolic link named as a root is left alone",
          os.path.islink(os.path.join(parent, "Ubiquitty-" + "s" * 28)))


class _Capture:
    """A writable stand-in for a stream that keeps what is printed to it."""

    def __init__(self):
        self.text = ""

    def write(self, text):
        """Keeps the text."""
        self.text += text

    def flush(self):
        """Nothing is buffered."""


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
