#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""build-lane.py — builds in a few long-lived worktrees whose build roots stay warm.

A checkout's build root is named after the checkout's path (Scripts/build_root.py),
so a new worktree starts from an empty root and builds everything: the Swift,
the PGO-instrumented C++, ThinLTO and the link. ccache answers for the C++
compiles and for nothing after them. Ten agents each building in a worktree of
their own are ten such builds on one machine.

A lane is a detached worktree at a fixed path, so its root is the same root
from one user to the next. `run` takes a free lane, brings it to the caller's
tree, runs the command there, and lets the lane go when the command exits:

    python3 Scripts/build-lane.py run --wait 3600 --from "$PWD" -- Scripts/run-tests.sh <test>
    python3 Scripts/build-lane.py run --from origin/main -- Scripts/build-tests.sh
    python3 Scripts/build-lane.py status
    python3 Scripts/build-lane.py root --lane 2
    python3 Scripts/build-lane.py --self-test

The lanes are `lane-1` to `lane-N` in the lanes directory: `--lanes-dir`, else
$UBQ_LANES_DIR, else `ubq-lanes` beside the repository's main checkout. N is
`--lanes`, else two on macOS, where at most two heavy builds share a machine,
and three elsewhere. A lane is created on first use, as a worktree of the
repository the other lanes belong to, or of the one this script is in.

Taking a lane. Each lane has a lock file beside it, `lane-N.lock`, held with
flock(2) for the whole run; the kernel releases it when the process ends,
however it ends. `lane-N.json` records the last taker: process, source, command
and start. A run tries the lane its source used last, then the others in
order; `--lane N` asks for one lane only. While every lane is held it prints
the holders and waits up to `--wait` seconds (0 by default), then exits 75. A
lane whose lock is free while a build process (ninja, a compiler, cmake, a
linker) still names it is passed over: its holder was killed and the build it
started was not.

Bringing the lane to the source. `--from` names a worktree (the current
directory by default) or a commit. A worktree contributes its HEAD and, on top,
its uncommitted changes: staged and unstaged through `git diff` and `git apply`,
and its untracked files unless `--no-untracked` is given. An untracked file over
64 MiB is left out and named. A worktree of another repository has its HEAD
fetched into the lanes' repository first. The lane's working tree then becomes
that tree exactly: what the last user changed, added or left mid-merge goes,
and ignored files stay, since build products are among them. Only files whose
content differs are written, so an unchanged file keeps its modification time
and ninja rebuilds what changed. HEAD is detached at the commit with the
changes staged, so a gate reading the index sees them.

Running. The command runs in the lane with UBQ_BUILD_ROOT set to the lane's
root and git's per-repository variables (GIT_DIR and the rest) removed, in a
session and process group of its own. SIGINT, SIGTERM and SIGHUP are passed on
to that group; a third signal sends SIGKILL. The lock is released when the command
ends, whatever its status, and the lane stays at that tree until the next run
replaces it.

Scripts/clean-build-roots.py keeps the root of every lane. Between runs a lane
is detached and clean at a commit main has, which is what a MERGED root looks
like.

Exit status: the command's, or 128 + N when signal N ended it; 2 = usage;
70 = the lane could not be created or brought to the source; 75 = no lane came
free within --wait.
"""

import argparse
import concurrent.futures
import fcntl
import importlib.util
import json
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import build_root  # noqa: E402 — the path above is what makes it importable


def _load_cleaner():
    """Loads Scripts/clean-build-roots.py, which owns what a lane is to the cleaner."""
    specification = importlib.util.spec_from_file_location(
        "clean_build_roots", os.path.join(HERE, "clean-build-roots.py"))
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


CLEANER = _load_cleaner()

TOOL = "build-lane"

DEFAULT_LANES = 2 if sys.platform == "darwin" else 3
LANES_DIRECTORY = "ubq-lanes"

EXIT_PREPARE = 70
EXIT_BUSY = 75

# Seconds between attempts at a lane while waiting, and between the reports of
# who holds them.
POLL = 0.5
REPORT_EVERY = 300

# An untracked file larger than this is a dump or a build product nobody
# ignored, and copying it into the object store costs more than it carries.
UNTRACKED_LIMIT = 64 * 1024 * 1024

# The states a git operation leaves when it stops half done, each with the
# command that forgets it without touching the working tree.
UNFINISHED = (("rebase-merge", ("rebase", "--quit")),
              ("rebase-apply", ("rebase", "--quit")),
              ("MERGE_HEAD", ("merge", "--quit")),
              ("CHERRY_PICK_HEAD", ("cherry-pick", "--quit")),
              ("REVERT_HEAD", ("revert", "--quit")),
              ("sequencer", ("cherry-pick", "--quit")))

# Hooks stay off for every git command here: a lane switch is plumbing, and a
# post-checkout hook from the shared repository has no business in it.
GIT_OPTIONS = ("-c", "core.hooksPath=/dev/null", "-c", "gc.auto=0",
               "-c", "advice.detachedHead=false")


class LaneError(Exception):
    """A lane that could not be created, found, or brought to a source."""


def environment(extra=None):
    """The environment for git and for the command, with git's per-repository variables removed.

    - Parameters:
      - extra: Variables to set on top.
    - Returns: A new dictionary.
    """
    found = {k: v for k, v in os.environ.items()
             if k not in CLEANER.GIT_LOCAL_VARIABLES}
    found.update(extra or {})
    return found


def git(where, *args, data=None, extra=None, check=True):
    """Runs git in a directory, hooks off.

    - Parameters:
      - where: The directory git runs in, or None for the current one.
      - args: The git arguments.
      - data: Bytes for its standard input.
      - extra: Environment variables to set for this call.
      - check: Whether a failure raises.
    - Returns: (status, stdout as bytes).
    - Throws: LaneError naming the command and git's message, when check is set
      and git fails.
    """
    command = ["git", *GIT_OPTIONS] + (["-C", where] if where else []) + list(args)
    try:
        finished = subprocess.run(command, input=data, capture_output=True,
                                  env=environment(extra), check=False)
    except OSError as error:
        raise LaneError(f"git could not run: {error}") from error
    if check and finished.returncode != 0:
        message = finished.stderr.decode("utf-8", "replace").strip()
        raise LaneError(f"git {' '.join(args[:3])} failed in {shown(where or os.getcwd())}: "
                        f"{message or 'status ' + str(finished.returncode)}")
    return finished.returncode, finished.stdout


def git_text(where, *args, **keywords):
    """Runs git and returns its standard output as stripped text.

    - Parameters:
      - where: The directory git runs in.
      - args: The git arguments.
      - keywords: As git() takes them.
    - Returns: The output, or "" when git failed and check is off.
    """
    status, out = git(where, *args, **keywords)
    return out.decode("utf-8", "replace").strip() if status == 0 else ""


def split_nul(data):
    """The entries of a NUL-separated git listing, as text."""
    return [entry.decode("utf-8", "surrogateescape")
            for entry in data.split(b"\0") if entry]


def shown(path):
    """A path with the home directory spelled ~."""
    return CLEANER.shown(path)


def toplevel(directory):
    """The top of the working tree holding a directory, or None when it is not in one."""
    if not os.path.isdir(directory):
        return None
    return git_text(directory, "rev-parse", "--show-toplevel", check=False) or None


def common_directory(checkout):
    """The repository a checkout belongs to, as its absolute, resolved common git directory."""
    found = git_text(checkout, "rev-parse", "--path-format=absolute",
                     "--git-common-dir", check=False)
    return os.path.realpath(found) if found else None


def main_checkout(checkout):
    """The first worktree git lists for a checkout's repository, which is its main checkout."""
    listing = git_text(checkout, "worktree", "list", "--porcelain", check=False)
    for line in listing.splitlines():
        if line.startswith("worktree "):
            return line[len("worktree "):]
    return checkout


def lanes_directory(option):
    """Where the lanes live.

    - Parameters:
      - option: The --lanes-dir value, or None.
    - Returns: --lanes-dir, else $UBQ_LANES_DIR, else `ubq-lanes` beside the
      main checkout of the repository this script is in, as an absolute path.
    """
    chosen = option or os.environ.get("UBQ_LANES_DIR")
    if chosen:
        return os.path.abspath(os.path.expanduser(chosen))
    home = main_checkout(os.path.dirname(HERE))
    return os.path.join(os.path.dirname(os.path.realpath(home)), LANES_DIRECTORY)


def lane_path(lanes, number):
    """A lane's worktree."""
    return os.path.join(lanes, f"lane-{number}")


def record_path(lanes, number):
    """The file naming a lane's last taker."""
    return os.path.join(lanes, f"lane-{number}.json")


def lane_root(path):
    """A lane's build root, derived as the lane's own build scripts derive it.

    UBQ_BUILD_ROOT is not consulted: a caller's environment naming the caller's
    root is the case this exists to route around.

    - Parameters:
      - path: The lane's worktree, existing or not.
    - Returns: An absolute path under build_root.derived_data().
    """
    top = toplevel(path) or os.path.realpath(path)
    return os.path.join(build_root.derived_data(),
                        f"{build_root.PROJECT}-{build_root.path_hash(os.path.abspath(top))}")


def is_lane_worktree(path):
    """Whether a lane's directory is a working worktree whose top is the directory itself."""
    if not os.path.isfile(os.path.join(path, ".git")):
        return False
    top = toplevel(path)
    return bool(top) and os.path.realpath(top) == os.path.realpath(path)


def lane_numbers(lanes, count, beyond=False):
    """The lanes to consider.

    - Parameters:
      - lanes: The lanes directory.
      - count: How many lanes there are.
      - beyond: Whether a higher-numbered lane that has a lock file counts too,
        as one taken with --lane does.
    - Returns: The lane numbers, ascending.
    """
    numbers = set(range(1, count + 1))
    if not beyond:
        return sorted(numbers)
    try:
        for name in os.listdir(lanes):
            stem = name[:-len(".lock")] if name.endswith(".lock") else ""
            if CLEANER.LANE_NAME.match(stem):
                numbers.add(int(stem.split("-", 1)[1]))
    except OSError:
        pass
    return sorted(numbers)


def lanes_repository(lanes, count):
    """The repository lanes are worktrees of.

    - Parameters:
      - lanes: The lanes directory.
      - count: How many lanes there are.
    - Returns: The common git directory of the first lane that is a worktree,
      else of the repository this script is in.
    """
    for number in lane_numbers(lanes, count, beyond=True):
        path = lane_path(lanes, number)
        if is_lane_worktree(path):
            found = common_directory(path)
            if found:
                return found
    found = common_directory(os.path.dirname(HERE))
    if not found:
        raise LaneError(f"{shown(os.path.dirname(HERE))} is not a git checkout")
    return found


# --- Records and locks -------------------------------------------------------


def read_record(lanes, number):
    """A lane's record, or an empty dictionary when there is none or it does not parse."""
    try:
        with open(record_path(lanes, number), encoding="utf-8") as handle:
            found = json.load(handle)
        return found if isinstance(found, dict) else {}
    except (OSError, ValueError):
        return {}


def write_record(lanes, number, record):
    """Replaces a lane's record in one rename, so a reader sees the old one or the new one."""
    handle, temporary = tempfile.mkstemp(prefix=f".lane-{number}.", dir=lanes)
    with os.fdopen(handle, "w", encoding="utf-8") as out:
        json.dump(record, out, indent=1, sort_keys=True)
        out.write("\n")
    os.replace(temporary, record_path(lanes, number))


def try_lock(lanes, number):
    """Takes a lane's lock without waiting.

    The lock file is opened and never replaced: flock(2) locks the open file,
    and a file renamed over it would be a second lock nobody holds.

    - Parameters:
      - lanes: The lanes directory.
      - number: The lane.
    - Returns: The open descriptor holding the lock, or None when another
      process holds it.
    """
    descriptor = os.open(CLEANER.lane_lock(lane_path(lanes, number)),
                         os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(descriptor)
        return None
    return descriptor


def is_held(lanes, number):
    """Whether some process holds a lane's lock, asked without blocking a taker for long."""
    try:
        descriptor = os.open(CLEANER.lane_lock(lane_path(lanes, number)), os.O_RDONLY)
    except OSError:
        return False
    try:
        fcntl.flock(descriptor, fcntl.LOCK_SH | fcntl.LOCK_NB)
    except BlockingIOError:
        return True
    finally:
        os.close(descriptor)
    return False


def lingering(lane, root):
    """The build processes still naming a lane or its root.

    Read from the process table's command lines, which is where a compiler
    names its source and its output, and where `ninja -C` names its directory.

    - Parameters:
      - lane: The lane's worktree.
      - root: The lane's build root.
    - Returns: ["name pid"] for each, or an empty list.
    """
    try:
        finished = subprocess.run(["ps", "axww", "-o", "pid=,args="],
                                  capture_output=True, text=True, check=False)
    except OSError:
        return []
    forms = [form + "/" for form in CLEANER.spellings(lane)]
    forms += list(CLEANER.spellings(root))
    found = []
    for line in finished.stdout.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) < 2 or not parts[0].isdigit() or int(parts[0]) == os.getpid():
            continue
        name = os.path.basename(parts[1].split()[0])
        if CLEANER.BUILD_PROCESS.match(name) and any(form in parts[1] for form in forms):
            found.append(f"{name} {parts[0]}")
    return found


def when(seconds):
    """A time as the local date and time, to the second."""
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(seconds))


def duration(seconds):
    """A span of seconds as hours, minutes and seconds."""
    whole = max(0, int(seconds))
    hours, rest = divmod(whole, 3600)
    minutes, seconds = divmod(rest, 60)
    if hours:
        return f"{hours}h {minutes:02d}m"
    return f"{minutes}m {seconds:02d}s" if minutes else f"{seconds}s"


def describe_holder(record, now):
    """One line naming a lane's taker: process, start, source and command."""
    if not record:
        return "held; no record of by whom"
    started = record.get("started", 0)
    command = " ".join(record.get("command", []))
    return (f"held by pid {record.get('pid', '?')} on {record.get('host', '?')} since "
            f"{when(started)} ({duration(now - started)}), from {record.get('from', '?')}: "
            f"{command}")


def holders(lanes, numbers, out):
    """Prints who holds each lane.

    - Parameters:
      - lanes: The lanes directory.
      - numbers: The lanes to describe.
      - out: Where to print.
    """
    now = time.time()
    for number in numbers:
        if is_held(lanes, number):
            state = describe_holder(read_record(lanes, number), now)
        elif os.path.isdir(lane_path(lanes, number)):
            state = "free"
        else:
            state = "free; not created"
        print(f"  lane-{number}  {state}", file=out)


def preference(lanes, numbers, key):
    """The order to try lanes in: the ones whose last source was this one first.

    A lane that last held the same worktree or commit has the fewest files to
    rewrite and the most of its build still current.
    """
    return sorted(numbers, key=lambda n: (read_record(lanes, n).get("key") != key, n))


def acquire(lanes, numbers, key, wait, out):
    """Takes a free lane, waiting for one up to a limit.

    - Parameters:
      - lanes: The lanes directory.
      - numbers: The lanes that may be taken.
      - key: The source's key, matched against each lane's last.
      - wait: Seconds to wait for a lane; 0 tries once.
      - out: Where the holders are printed while waiting.
    - Returns: (number, descriptor) for the lane taken, or None.
    """
    deadline = time.monotonic() + wait
    report = time.monotonic()
    announced = False
    while True:
        passed = []
        for number in preference(lanes, numbers, key):
            descriptor = try_lock(lanes, number)
            if descriptor is None:
                continue
            path = lane_path(lanes, number)
            users = lingering(path, lane_root(path))
            if not users:
                return number, descriptor
            os.close(descriptor)
            passed.append(f"lane-{number} is unlocked but still built in by "
                          + ", ".join(users[:3]))
        now = time.monotonic()
        if now >= report:
            if not announced:
                print(f"{TOOL}: no lane free in {shown(lanes)}:", file=out)
                announced = True
            else:
                print(f"{TOOL}: still waiting:", file=out)
            holders(lanes, numbers, out)
            for line in passed:
                print(f"  {line}", file=out)
            if now < deadline:
                print(f"{TOOL}: waiting up to {int(deadline - now)} s more", file=out)
            out.flush()
            report = now + REPORT_EVERY
        if now >= deadline:
            return None
        time.sleep(POLL)


# --- Sources -----------------------------------------------------------------


class Source:
    """What a lane is brought to: a commit, and the worktree whose changes go on top of it."""

    def __init__(self, commit, worktree=None, repository=None):
        self.commit = commit
        self.worktree = worktree
        self.repository = repository

    @property
    def key(self):
        """What two runs from the same source share."""
        return self.worktree if self.worktree else "commit " + self.commit

    @property
    def label(self):
        """The source as a person reads it."""
        return shown(self.worktree) if self.worktree else "commit " + self.commit[:12]


def resolve_source(specification, repository):
    """The source a --from value names.

    - Parameters:
      - specification: A directory inside a worktree, or anything git resolves
        to a commit; None means the current directory.
      - repository: The lanes' repository, where a commit name is resolved.
    - Returns: A Source.
    - Throws: LaneError when it names neither.
    """
    specification = specification or os.getcwd()
    if os.path.isdir(specification):
        top = toplevel(specification)
        if not top:
            raise LaneError(f"{shown(specification)} is not inside a git worktree")
        commit = git_text(top, "rev-parse", "--verify", "--quiet", "HEAD^{commit}",
                          check=False)
        if not commit:
            raise LaneError(f"{shown(top)} has no commit checked out")
        return Source(commit, os.path.realpath(top), common_directory(top))
    commit = git_text(None, f"--git-dir={repository}", "rev-parse", "--verify",
                      "--quiet", specification + "^{commit}", check=False)
    if not commit:
        raise LaneError(f"{specification!r} is neither a directory nor a commit")
    return Source(commit)


def untracked(worktree):
    """A worktree's untracked files, ignored ones excluded, as paths relative to it."""
    _, out = git(worktree, "ls-files", "-z", "--others", "--exclude-standard",
                 extra={"GIT_OPTIONAL_LOCKS": "0"})
    return split_nul(out)


def source_tree(lane, source, carry_untracked, out):
    """Writes the tree a lane is brought to, into the lanes' object store.

    A commit is its own tree. A worktree's is its HEAD with its uncommitted
    changes applied in a temporary index: the diff against HEAD, binary files
    included, and the untracked files hashed in, so the lane receives the bytes
    the worktree holds. The worktree's own index is not written.

    - Parameters:
      - lane: The lane, whose repository receives the objects.
      - source: The Source.
      - carry_untracked: Whether untracked files are carried.
      - out: Where skipped files are named.
    - Returns: (tree, changed) with the tree's object name and how many paths
      differ from the commit.
    """
    if not source.worktree:
        return git_text(lane, "rev-parse", source.commit + "^{tree}"), 0
    scratch = tempfile.mkdtemp(prefix="build-lane-index-")
    index = {"GIT_INDEX_FILE": os.path.join(scratch, "index")}
    try:
        git(lane, "read-tree", source.commit, extra=index)
        _, patch = git(source.worktree, "diff", "--binary", "--full-index", "--no-color",
                       "--no-ext-diff", "--no-textconv", "--no-renames", "--no-relative",
                       "--src-prefix=a/", "--dst-prefix=b/", source.commit,
                       extra={"GIT_OPTIONAL_LOCKS": "0"})
        if patch:
            git(lane, "apply", "--cached", "--binary", "--whitespace=nowarn", "-",
                data=patch, extra=index)
        if carry_untracked:
            entries = untracked_entries(lane, source.worktree, out)
            if entries:
                git(lane, "update-index", "-z", "--add", "--index-info",
                    data=b"".join(entries), extra=index)
        tree = git_text(lane, "write-tree", extra=index)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    _, listing = git(lane, "diff-tree", "-r", "-z", "--name-only", "--no-renames",
                     source.commit, tree)
    return tree, len(split_nul(listing))


def untracked_entries(lane, worktree, out):
    """Hashes a worktree's untracked files into the lanes' object store.

    - Parameters:
      - lane: The lane, whose repository receives the objects.
      - worktree: The worktree holding the files.
      - out: Where a file left out is named.
    - Returns: `git update-index --index-info` lines, NUL-terminated, as bytes.
    """
    regular, entries = [], []
    for relative in untracked(worktree):
        path = os.path.join(worktree, relative)
        if relative.endswith("/"):
            print(f"{TOOL}: left out {relative}: a repository of its own", file=out)
            continue
        try:
            info = os.lstat(path)
        except OSError:
            continue
        if stat.S_ISLNK(info.st_mode):
            target = os.fsencode(os.readlink(path))
            blob = git_text(lane, "hash-object", "-w", "--stdin", data=target)
            entries.append(index_line("120000", blob, relative))
        elif not stat.S_ISREG(info.st_mode):
            continue
        elif info.st_size > UNTRACKED_LIMIT:
            print(f"{TOOL}: left out {relative}: {CLEANER.human(info.st_size)}, over "
                  f"{CLEANER.human(UNTRACKED_LIMIT)}", file=out)
        elif "\n" in relative:
            print(f"{TOOL}: left out {relative!r}: a newline in its name", file=out)
        else:
            mode = "100755" if info.st_mode & stat.S_IXUSR else "100644"
            regular.append((relative, mode))
    if regular:
        listing = "".join(os.path.join(worktree, relative) + "\n"
                          for relative, _ in regular)
        blobs = git_text(lane, "hash-object", "-w", "--no-filters", "--stdin-paths",
                         data=os.fsencode(listing)).split()
        entries += [index_line(mode, blob, relative)
                    for (relative, mode), blob in zip(regular, blobs)]
    return entries


def index_line(mode, blob, relative):
    """One `--index-info -z` entry."""
    return f"{mode} {blob}\t".encode() + os.fsencode(relative) + b"\0"


# --- Lanes -------------------------------------------------------------------


def ensure_lane(path, repository, commit):
    """Creates a lane's worktree when it does not exist.

    - Parameters:
      - path: The lane's worktree.
      - repository: The common git directory it is created in.
      - commit: What it is created at.
    - Throws: LaneError when something else is at the path, or the lane
      belongs to another repository.
    """
    if is_lane_worktree(path):
        if common_directory(path) != repository:
            raise LaneError(f"{shown(path)} is a worktree of {shown(common_directory(path))}, "
                            f"not of {shown(repository)}")
        return
    if os.path.lexists(path):
        raise LaneError(f"{shown(path)} exists and is not a worktree; move it out of the way")
    # --force: a registration whose directory is gone would refuse the path.
    git(None, f"--git-dir={repository}", "worktree", "add", "--quiet", "--force",
        "--detach", path, commit)


def fetch_commit(lane, source):
    """Fetches a source's commit into the lanes' repository when it is not there."""
    status, _ = git(lane, "cat-file", "-e", source.commit + "^{commit}", check=False)
    if status == 0:
        return
    git(lane, "fetch", "--quiet", "--no-tags", "--no-write-fetch-head",
        source.worktree, "HEAD")
    status, _ = git(lane, "cat-file", "-e", source.commit + "^{commit}", check=False)
    if status != 0:
        raise LaneError(f"{source.commit[:12]} is not in {shown(source.worktree)}'s HEAD "
                        "any more; run again")


def switch(lane, commit, tree):
    """Makes a lane's working tree the given tree, writing only the files that differ.

    The lane's tree as it stands is staged whole, the previous user's leftovers
    included, and a reset read of the target tree then writes each path whose
    content differs and deletes each path the target lacks. Untracked files the
    target lacks are deleted first, so nothing large is hashed on its way out.

    - Parameters:
      - lane: The lane's worktree.
      - commit: The commit HEAD is detached at.
      - tree: The tree the working tree and index become.
    - Returns: How many paths were written or deleted.
    """
    directory = git_text(lane, "rev-parse", "--absolute-git-dir")
    lock = os.path.join(directory, "index.lock")
    if os.path.exists(lock):
        os.unlink(lock)
    for state, command in UNFINISHED:
        if os.path.exists(os.path.join(directory, state)):
            git(lane, *command, check=False)
    _, listing = git(lane, "ls-tree", "-r", "-z", "--name-only", tree)
    wanted = set(split_nul(listing))
    top = os.path.realpath(lane)
    for relative in untracked(lane):
        if relative.rstrip("/") in wanted:
            continue
        path = os.path.realpath(os.path.join(lane, relative.rstrip("/")))
        if not path.startswith(top + os.sep):
            continue
        if os.path.isdir(path) and not os.path.islink(path):
            shutil.rmtree(path, ignore_errors=True)
        elif os.path.lexists(path):
            os.unlink(path)
    git(lane, "add", "--all")
    before = git_text(lane, "write-tree")
    git(lane, "read-tree", "--reset", "-u", tree)
    git(lane, "clean", "-ffdq")
    git(lane, "update-ref", "--no-deref", "HEAD", commit)
    _, status = git(lane, "status", "--porcelain", "-z", "--no-renames",
                    "--untracked-files=normal")
    stray = [entry for entry in split_nul(status)
             if entry.startswith("??") or (len(entry) > 1 and entry[1] != " ")]
    if stray:
        raise LaneError(f"{shown(lane)} still differs from the tree it was given: "
                        + ", ".join(stray[:5]))
    _, written = git(lane, "diff-tree", "-r", "-z", "--name-only", "--no-renames",
                     before, tree)
    return len(split_nul(written))


def prepare(lanes, number, repository, source, carry_untracked, out):
    """Creates a lane if needed and brings it to a source.

    A switch that fails is retried once after a hard reset and clean, which
    is what a lane left in a state the switch does not expect needs.

    - Parameters:
      - lanes: The lanes directory.
      - number: The lane.
      - repository: The lanes' common git directory.
      - source: The Source.
      - carry_untracked: Whether a worktree's untracked files are carried.
      - out: Where progress is printed.
    - Returns: (changed, written): the paths the source changes against its
      commit, and the paths the switch wrote or deleted.
    """
    path = lane_path(lanes, number)
    ensure_lane(path, repository, source.commit)
    if source.worktree and source.repository != repository:
        fetch_commit(path, source)
    tree, changed = source_tree(path, source, carry_untracked, out)
    try:
        written = switch(path, source.commit, tree)
    except LaneError as error:
        print(f"{TOOL}: {error}; resetting lane-{number} and trying once more", file=out)
        git(path, "reset", "--hard", "--quiet", check=False)
        git(path, "clean", "-ffdq", check=False)
        written = switch(path, source.commit, tree)
    return changed, written


def execute(command, lane, root):
    """Runs a command in a lane and returns its exit status.

    - Parameters:
      - command: The argument vector.
      - lane: The directory it runs in.
      - root: The lane's build root, exported as UBQ_BUILD_ROOT.
    - Returns: The command's status, 128 + N when signal N ended it, or 127
      when it could not be started.
    """
    received = []
    child = None

    def forward(signum, _frame):
        received.append(signum)
        if child is not None and child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGKILL if len(received) >= 3 else signum)
            except ProcessLookupError:
                pass

    caught = (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    saved = {signum: signal.signal(signum, forward) for signum in caught}
    try:
        try:
            # A session of its own, and so a process group whose ID is the
            # child's: a forwarded signal reaches the compilers under the
            # script, and a terminal's SIGINT reaches them once, through here.
            child = subprocess.Popen(command, cwd=lane, start_new_session=True,
                                     env=environment({"UBQ_BUILD_ROOT": root}))
        except OSError as error:
            print(f"{TOOL}: {command[0]}: {error.strerror}", file=sys.stderr)
            return 127
        for signum in list(received):
            os.killpg(child.pid, signum)
        status = child.wait()
    finally:
        for signum, handler in saved.items():
            signal.signal(signum, handler)
    return 128 - status if status < 0 else status


def run(options, out=sys.stderr):
    """Takes a lane, brings it to the source, runs the command in it, and lets it go.

    - Parameters:
      - options: The parsed `run` arguments.
      - out: Where this tool's own lines go; the command's output is its own.
    - Returns: The exit status.
    """
    command = list(options.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        print(f"{TOOL}: run needs a command after --", file=out)
        return 2
    lanes = lanes_directory(options.lanes_dir)
    os.makedirs(lanes, exist_ok=True)
    try:
        repository = lanes_repository(lanes, options.lanes)
        source = resolve_source(options.source, repository)
    except LaneError as error:
        print(f"{TOOL}: {error}", file=out)
        return EXIT_PREPARE
    numbers = [options.lane] if options.lane else lane_numbers(lanes, options.lanes)
    taken = acquire(lanes, numbers, source.key, options.wait, out)
    if taken is None:
        print(f"{TOOL}: no lane came free within {options.wait} s", file=out)
        return EXIT_BUSY
    number, descriptor = taken
    path = lane_path(lanes, number)
    root = lane_root(path)
    record = {"pid": os.getpid(), "host": socket.gethostname(), "from": source.label,
              "key": source.key, "commit": source.commit, "command": command,
              "started": time.time()}
    try:
        write_record(lanes, number, record)
        print(f"{TOOL}: lane-{number} ({shown(path)}), build root {shown(root)}", file=out)
        try:
            changed, written = prepare(lanes, number, repository, source,
                                       options.untracked, out)
        except LaneError as error:
            print(f"{TOOL}: lane-{number}: {error}", file=out)
            return EXIT_PREPARE
        subject = git_text(path, "log", "-1", "--format=%h %s", source.commit)
        extra = f" with {changed} uncommitted path(s)" if changed else ""
        print(f"{TOOL}: lane-{number} at {subject}{extra}; {written} path(s) written",
              file=out)
        out.flush()
        status = execute(command, path, root)
        record.update({"finished": time.time(), "status": status})
        write_record(lanes, number, record)
        return status
    finally:
        os.close(descriptor)


# --- status and root ---------------------------------------------------------


def status(options, out=sys.stdout):
    """Prints each lane's holder, commit, and build root's size and last build.

    - Parameters:
      - options: The parsed `status` arguments.
      - out: Where to print.
    - Returns: 0.
    """
    lanes = lanes_directory(options.lanes_dir)
    numbers = lane_numbers(lanes, options.lanes, beyond=True)
    paths = {n: lane_path(lanes, n) for n in numbers}
    roots = {n: lane_root(p) for n, p in paths.items()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=CLEANER.WORKERS) as pool:
        sizes = dict(zip(numbers, pool.map(lambda n: CLEANER.footprint(roots[n]), numbers)))
    print(f"{TOOL}: {len(numbers)} lane(s) in {shown(lanes)}", file=out)
    now = time.time()
    for number in numbers:
        path = paths[number]
        record = read_record(lanes, number)
        if is_held(lanes, number):
            holder = describe_holder(record, now)
        elif record:
            ended = record.get("finished")
            holder = (f"free; last run from {record.get('from', '?')}"
                      + (f", status {record.get('status')} at {when(ended)}" if ended
                         else ", which did not finish"))
        else:
            holder = "free"
        print(f"  lane-{number}  {holder}", file=out)
        if is_lane_worktree(path):
            commit = git_text(path, "log", "-1", "--format=%h %s", "HEAD", check=False)
            changed, _ = git(path, "diff-index", "--cached", "--quiet", "HEAD", "--",
                             check=False, extra={"GIT_OPTIONAL_LOCKS": "0"})
            print(f"          at {commit or '?'}"
                  + (" with uncommitted changes" if changed else ""), file=out)
        else:
            print(f"          not created: {shown(path)}", file=out)
        size, newest, _ = sizes[number]
        built = when(newest) if newest else "never"
        print(f"          root {shown(roots[number])}: {CLEANER.human(size)}, "
              f"last written {built}", file=out)
    return 0


def root(options, out=sys.stdout):
    """Prints a lane's build root: --lane's, else the lane the current worktree ran in last, else lane 1.

    - Parameters:
      - options: The parsed `root` arguments.
      - out: Where to print.
    - Returns: 0.
    """
    lanes = lanes_directory(options.lanes_dir)
    number = options.lane
    if not number:
        top = toplevel(os.getcwd())
        key = os.path.realpath(top) if top else None
        last = [(read_record(lanes, n).get("started", 0), n)
                for n in lane_numbers(lanes, options.lanes, beyond=True)
                if key and read_record(lanes, n).get("key") == key]
        number = max(last)[1] if last else 1
    print(lane_root(lane_path(lanes, number)), file=out)
    return 0


def positive(text):
    """An argparse type: an integer of at least 1."""
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def seconds(text):
    """An argparse type: a whole number of seconds, 0 or more."""
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return value


def main(argv):
    """Parses the arguments and runs the subcommand."""
    shared = argparse.ArgumentParser(add_help=False)
    shared.add_argument("--lanes-dir", default=None,
                        help="the lanes directory ($UBQ_LANES_DIR, else ubq-lanes "
                             "beside the main checkout)")
    shared.add_argument("--lanes", type=positive, default=DEFAULT_LANES,
                        help=f"how many lanes ({DEFAULT_LANES} here)")
    parser = argparse.ArgumentParser(
        prog="build-lane.py",
        description="Build in long-lived worktrees whose build roots stay warm.")
    parser.add_argument("--self-test", action="store_true",
                        help="check lanes, locks and switches against a throwaway repository")
    commands = parser.add_subparsers(dest="subcommand")
    running = commands.add_parser("run", parents=[shared],
                                  help="run a command in a free lane")
    running.add_argument("--lane", type=positive, default=None,
                         help="take this lane and no other")
    running.add_argument("--wait", type=seconds, default=0,
                         help="seconds to wait for a free lane (0)")
    running.add_argument("--from", dest="source", default=None,
                         help="a worktree, whose uncommitted changes come too, or a "
                              "commit (the current directory)")
    running.add_argument("--no-untracked", dest="untracked", action="store_false",
                         help="leave the worktree's untracked files out")
    running.add_argument("command", nargs=argparse.REMAINDER,
                         help="-- and the command to run in the lane")
    commands.add_parser("status", parents=[shared],
                        help="each lane's holder, commit, and build root")
    rooting = commands.add_parser("root", parents=[shared], help="a lane's build root")
    rooting.add_argument("--lane", type=positive, default=None,
                         help="the lane (default: the one this worktree ran in last, else 1)")
    options = parser.parse_args(argv)
    if options.self_test:
        return _self_test()
    if options.subcommand == "run":
        return run(options)
    if options.subcommand == "status":
        return status(options)
    if options.subcommand == "root":
        return root(options)
    parser.print_usage(sys.stderr)
    return 2


# --- Self-test ---------------------------------------------------------------


def _self_test():
    """Lanes, locks, switches and the cleaner's view of them, against a throwaway repository."""
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    saved = dict(os.environ)
    scratch = os.path.realpath(tempfile.mkdtemp(prefix="build-lane-"))
    try:
        # A home, a git configuration, an author and a build-root parent of its
        # own, so nothing here reads or writes the person's.
        os.environ.update({
            "HOME": os.path.join(scratch, "home"),
            "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "user@example.com",
            "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "user@example.com",
            "UBIQUITTY_DERIVED_DATA": os.path.join(scratch, "DerivedData"),
        })
        for variable in ("UBQ_BUILD_ROOT", "UBQ_LANES_DIR", "XDG_CONFIG_HOME",
                         *CLEANER.GIT_LOCAL_VARIABLES):
            os.environ.pop(variable, None)
        os.makedirs(os.environ["HOME"])
        _exercise(scratch, check)
    finally:
        os.environ.clear()
        os.environ.update(saved)
        shutil.rmtree(scratch, ignore_errors=True)

    if failures:
        print(f"{TOOL} --self-test: {len(failures)} claim(s) the lanes do not keep",
              file=sys.stderr)
        return 1
    print(f"{TOOL} --self-test: ok (a held lane makes the next run wait, a lane becomes "
          "its source exactly and writes only what differs, and its root is kept)")
    return 0


def _exercise(scratch, check):
    """Builds a repository and a worktree under scratch and runs lanes against them."""
    def sh(where, *args):
        subprocess.run(["git", "-C", where, *args], check=True, capture_output=True)

    def write(path, content, mode=None):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(content if isinstance(content, bytes) else content.encode())
        if mode is not None:
            os.chmod(path, mode)

    def read(path):
        with open(path, "rb") as handle:
            return handle.read()

    origin = os.path.join(scratch, "origin.git")
    main = os.path.join(scratch, "main")
    subprocess.run(["git", "init", "--quiet", "--bare", "-b", "main", origin],
                   check=True, capture_output=True)
    subprocess.run(["git", "clone", "--quiet", origin, main], check=True,
                   capture_output=True)
    # The tool runs from a copy inside the throwaway repository, so the lanes
    # it creates are that repository's and sit beside its checkout.
    for name in ("build-lane.py", "build_root.py", "clean-build-roots.py"):
        write(os.path.join(main, "Scripts", name), read(os.path.join(HERE, name)))
    files = {"a.c": "int a;\n", "b.swift": "let b = 1\n", "dir/c.h": "#define C 1\n",
             "gone.txt": "going\n", "tool.sh": "echo tool\n", "untouched.txt": "same\n",
             "blob.bin": b"\0\1\2binary\0", ".gitignore": "*.o\n"}
    for relative, content in files.items():
        write(os.path.join(main, relative), content)
    sh(main, "add", "--all")
    sh(main, "commit", "--quiet", "--no-verify", "-m", "first")
    sh(main, "push", "--quiet", "origin", "HEAD:main")
    sh(main, "fetch", "--quiet", "origin")
    base = subprocess.run(["git", "-C", main, "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()

    work = os.path.join(scratch, "work")
    sh(main, "worktree", "add", "--quiet", "--detach", work, "origin/main")
    write(os.path.join(work, "committed.txt"), "local commit\n")
    sh(work, "add", "committed.txt")
    sh(work, "commit", "--quiet", "--no-verify", "-m", "local")
    head = subprocess.run(["git", "-C", work, "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()
    write(os.path.join(work, "a.c"), "int a = 2;\n")
    write(os.path.join(work, "dir/c.h"), "#define C 2\n")
    sh(work, "add", "dir/c.h")
    os.unlink(os.path.join(work, "gone.txt"))
    os.chmod(os.path.join(work, "tool.sh"), 0o755)
    write(os.path.join(work, "blob.bin"), b"\0\3\4changed\0")
    write(os.path.join(work, "new/untracked.swift"), "let fresh = true\n")
    os.symlink("a.c", os.path.join(work, "link"))
    write(os.path.join(work, "out.o"), "ignored\n")

    tool = os.path.join(main, "Scripts", "build-lane.py")
    lanes = os.path.join(scratch, LANES_DIRECTORY)
    lane = os.path.join(lanes, "lane-1")
    marks = os.path.join(scratch, "marks")
    os.makedirs(marks)

    def start(*args, command=("true",)):
        return subprocess.Popen([sys.executable, tool, *args, "--", *command],
                                cwd=work, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)

    def finish(*args, command=("true",)):
        process = start(*args, command=command)
        output, _ = process.communicate(timeout=60)
        return process.returncode, output

    def waited(path, limit=30):
        deadline = time.monotonic() + limit
        while not os.path.exists(path) and time.monotonic() < deadline:
            time.sleep(0.05)
        return os.path.exists(path)

    def program(text):
        return (sys.executable, "-c", text)

    hold = program(
        "import os, sys, time\n"
        f"m = {marks!r}\n"
        "open(os.path.join(m, sys.argv[1] + '-started'), 'w').close()\n"
        "deadline = time.time() + 60\n"
        "while not os.path.exists(os.path.join(m, sys.argv[1] + '-release')):\n"
        "    if time.time() > deadline: sys.exit(1)\n"
        "    time.sleep(0.05)\n"
        "open(os.path.join(m, sys.argv[1] + '-done'), 'w').close()\n")

    # The worktree's tree, uncommitted changes and all.
    status, output = finish("run", "--lanes", "1", "--from", work)
    check("a run creates its lane beside the main checkout and succeeds",
          status == 0 and is_lane_worktree(lane))
    lane_head = git_text(lane, "rev-parse", "HEAD")
    detached = git(lane, "symbolic-ref", "--quiet", "HEAD", check=False)[0] != 0
    check("the lane's HEAD is the worktree's HEAD, detached", lane_head == head and detached)
    same = all(read(os.path.join(lane, p)) == read(os.path.join(work, p))
               for p in ("a.c", "dir/c.h", "blob.bin", "committed.txt",
                         "new/untracked.swift", "untouched.txt"))
    check("unstaged, staged, binary and untracked changes reach the lane", same)
    check("a deleted file is gone from the lane",
          not os.path.lexists(os.path.join(lane, "gone.txt")))
    check("an executable bit and a symbolic link arrive as they are",
          os.access(os.path.join(lane, "tool.sh"), os.X_OK)
          and os.path.islink(os.path.join(lane, "link"))
          and os.readlink(os.path.join(lane, "link")) == "a.c")
    check("an ignored file in the worktree stays out of the lane",
          not os.path.exists(os.path.join(lane, "out.o")))
    check("the changes are staged in the lane, so a gate reading the index sees them",
          "dir/c.h" in git_text(lane, "diff", "--cached", "--name-only", "HEAD")
          and "new/untracked.swift" in git_text(lane, "diff", "--cached", "--name-only",
                                                "HEAD"))

    status, output = finish("run", "--lanes", "1", "--from", work, "--no-untracked")
    check("--no-untracked leaves untracked files out",
          status == 0 and not os.path.exists(os.path.join(lane, "new/untracked.swift"))
          and read(os.path.join(lane, "a.c")) == read(os.path.join(work, "a.c")))

    # Two runs on one lane: the second waits for the first.
    first = start("run", "--lanes", "1", "--from", work, command=hold + ("first",))
    check("the first run starts its command", waited(os.path.join(marks, "first-started")))
    status, output = finish("run", "--lanes", "1", "--wait", "0", "--from", work)
    check("with the lane held and --wait 0, a run exits 75 and names the holder",
          status == EXIT_BUSY and f"pid {first.pid}" in output and work in output.replace(
              "~", os.environ["HOME"]))
    report = subprocess.run([sys.executable, tool, "status", "--lanes", "2"], cwd=work,
                            capture_output=True, text=True).stdout
    lines = {line.split()[0]: line for line in report.splitlines()
             if line.strip().startswith("lane-")}
    check("status names the holder's process and source, and the lane's commit",
          f"pid {first.pid}" in lines.get("lane-1", "") and head[:7] in report
          and "uncommitted changes" in report)
    check("status lists a lane that is not created as free",
          "free" in lines.get("lane-2", "") and "not created" in report)
    lane_root_path = subprocess.run([sys.executable, tool, "root", "--lane", "1"],
                                    cwd=work, capture_output=True, text=True).stdout.strip()
    check("root prints the root the lane's own build scripts derive",
          lane_root_path == build_root.build_root(lane))
    write(os.path.join(lane_root_path, "macos", "build.ninja"), b"\0" * 8192)
    report = subprocess.run([sys.executable, tool, "status", "--lanes", "1"], cwd=work,
                            capture_output=True, text=True).stdout
    written_line = [line for line in report.splitlines()
                    if shown(lane_root_path) in line or lane_root_path in line]
    check("status gives the root's size and when it was last written",
          len(written_line) == 1 and ": 0 B," not in written_line[0]
          and "last written " + time.strftime("%Y-%m-%d") in written_line[0])
    second = start("run", "--lanes", "1", "--wait", "30", "--from", work,
                   command=program("import os\n"
                                   f"m = {marks!r}\n"
                                   "ran = 'after' if os.path.exists(os.path.join(m, "
                                   "'first-done')) else 'during'\n"
                                   "open(os.path.join(m, 'second-' + ran), 'w').close()\n"))
    time.sleep(1.5)
    check("a second run waits while the lane is held",
          second.poll() is None
          and not any(name.startswith("second-") for name in os.listdir(marks)))
    write(os.path.join(marks, "first-release"), "")
    first.communicate(timeout=60)
    output, _ = second.communicate(timeout=60)
    check("the second run takes the lane once the first lets it go",
          second.returncode == 0 and os.path.exists(os.path.join(marks, "second-after"))
          and f"pid {first.pid}" in output)

    # Release on failure and on a signal.
    status, _ = finish("run", "--lanes", "1", "--from", work,
                       command=program("import sys; sys.exit(7)"))
    check("a failing command's status is the run's", status == 7)
    status, _ = finish("run", "--lanes", "1", "--wait", "0", "--from", work)
    check("the lane is free again after a failure", status == 0)
    child_pid = os.path.join(marks, "child-pid")
    killed = start("run", "--lanes", "1", "--from", work,
                   command=program("import os, time\n"
                                   f"open({child_pid!r}, 'w').write(str(os.getpid()))\n"
                                   "time.sleep(60)\n"))
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline and not (
            os.path.exists(child_pid) and read(child_pid).strip()):
        time.sleep(0.05)
    killed.send_signal(signal.SIGTERM)
    try:
        killed.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        killed.kill()
        killed.communicate()
    gone = True
    try:
        os.kill(int(read(child_pid)), 0)
        gone = False
    except (OSError, ValueError):
        pass
    check("SIGTERM reaches the command, and the run exits 128 + 15",
          killed.returncode == 128 + signal.SIGTERM and gone)
    status, _ = finish("run", "--lanes", "1", "--wait", "0", "--from", work)
    check("the lane is free again after a signal", status == 0)

    # A dirty lane, reset for the next user, rewriting only what differs.
    write(os.path.join(lane, "b.swift"), "let b = 999\n")
    write(os.path.join(lane, "junk.txt"), "left behind\n")
    write(os.path.join(lane, "keep.o"), "a build product\n")
    sh(lane, "add", "junk.txt")
    directory = git_text(lane, "rev-parse", "--absolute-git-dir")
    write(os.path.join(directory, "MERGE_HEAD"), base + "\n")
    write(os.path.join(directory, "index.lock"), "")
    old = time.time() - 30 * 86400
    for relative in ("untouched.txt", "a.c"):
        os.utime(os.path.join(lane, relative), (old, old))
    status, output = finish("run", "--lanes", "1", "--from", "origin/main")
    clean = git_text(lane, "status", "--porcelain", "--untracked-files=normal")
    check("a dirty lane is reset to the commit: edits, junk and a merge in progress go",
          status == 0 and clean == "" and git_text(lane, "rev-parse", "HEAD") == base
          and read(os.path.join(lane, "b.swift")) == b"let b = 1\n"
          and not os.path.exists(os.path.join(lane, "junk.txt"))
          and not os.path.exists(os.path.join(directory, "MERGE_HEAD")))
    check("an ignored file in the lane stays", os.path.exists(os.path.join(lane, "keep.o")))
    check("a file the switch does not change keeps its modification time",
          abs(os.stat(os.path.join(lane, "untouched.txt")).st_mtime - old) < 2)
    check("a file the switch changes is written",
          os.stat(os.path.join(lane, "a.c")).st_mtime > old + 3600)
    os.utime(os.path.join(lane, "a.c"), (old, old))
    status, output = finish("run", "--lanes", "1", "--from", "origin/main")
    check("a second run from the same source writes nothing",
          status == 0 and "0 path(s) written" in output
          and abs(os.stat(os.path.join(lane, "a.c")).st_mtime - old) < 2)

    # The cleaner keeps lane roots, which look merged.
    status, _ = finish("run", "--lanes", "2", "--lane", "2", "--from", "origin/main")
    merged = os.path.join(scratch, "merged")
    sh(main, "worktree", "add", "--quiet", "--detach", merged, "origin/main")
    planted = {}
    for name, checkout in (("lane-1", lane), ("lane-2", lane_path(lanes, 2)),
                           ("merged", merged)):
        planted[name] = build_root.build_root(checkout)
        write(os.path.join(planted[name], "build", "out.o"), b"\0" * 8192)
    parent = build_root.derived_data()

    def classes():
        surveyed = CLEANER.survey(parent, [main], [], [scratch], set(), [], 7)
        return {r.path: (r.cls, r.why) for r in surveyed}

    found = classes()
    check("clean-build-roots keeps both lanes' roots",
          all(found.get(planted[n], ("",))[0] == CLEANER.KEEP for n in ("lane-1", "lane-2"))
          and "build lane" in found.get(planted["lane-1"], ("", ""))[1])
    check("a clean detached worktree on main beside them is MERGED",
          found.get(planted["merged"], ("",))[0] == CLEANER.MERGED)
    lock = CLEANER.lane_lock(lane)
    os.rename(lock, lock + ".aside")
    found = classes()
    os.rename(lock + ".aside", lock)
    check("without its lock file a lane's root would be MERGED, so the lane is what keeps it",
          found.get(planted["lane-1"], ("",))[0] == CLEANER.MERGED)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
