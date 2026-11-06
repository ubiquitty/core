#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""What a test run leaves behind: processes it started, and files in the state
directory of the person running it.

A run marks itself in the environment (UBQ_TEST_RUN=<id>), and everything it
starts inherits the mark, including a daemon that detached into a session of
its own, which no process-group kill reaches. The mark is how such a process
is found again: by what started it, not by where its binary lives, so two runs
from one build root never take each other's daemons, and a developer's own
daemon started from the same binary is never touched.

A nested run appends its id to its parent's (outer/inner), so the outer run
also finds what the inner one started.

What it sees is what the system shows of an environment. Darwin shows none for
its own protected binaries (/bin/sleep, /bin/sh), so a leaked system shell is
not found; a host daemon, a bench or a test binary built from this tree is.

  survivors   print the marked processes outside the caller's own tree
  marked      print every marked process, the caller's own tree included
  reap        end them: SIGTERM, a grace period, SIGKILL
  watch       wait for a process to go, then reap its run; survives the
              run being killed outright, which no trap of its own does
  snapshot    record a directory's listing
  added       print the entries a directory gained since a snapshot, each
              as the run's or as another process's, with the reason
  plant       leave something behind on purpose, for the gate's own test

The id is read from UBQ_TEST_RUN, or from UBQ_WATCHED_RUN by `watch`, whose
own environment must not carry the mark it sweeps for.

The state directory is shared with everything else the person runs: their app
starts a daemon there, and so does a session they open over SSH with the
installed host. A daemon names its files after its socket leaf, `host-<16
hex>`, which is a hash of its binary, so an entry with that stem is put down to
the daemon serving it. It is another process's when a live ubiquitty-host
serving the stem carries no mark of this run, or when the stem is the leaf of
the directory's installed host, which no test runs. Any other entry, and one
whose daemon a marked process is, counts against the run: the run cannot be
told apart from a writer nothing names.
"""

from __future__ import annotations

import hashlib
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

MARK = "UBQ_TEST_RUN"


def process_table() -> dict[int, tuple[int, str]]:
    """pid -> (parent pid, command line) for every process."""
    listing = subprocess.run(["ps", "-axww", "-o", "pid=,ppid=,command="], capture_output=True, text=True, check=False).stdout
    table: dict[int, tuple[int, str]] = {}
    for line in listing.splitlines():
        parts = line.split(None, 2)
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
            table[int(parts[0])] = (int(parts[1]), parts[2] if len(parts) > 2 else "")
    return table


def marked(run: str) -> set[int]:
    """The processes whose environment carries the run's mark, or a nested run's."""
    wanted = re.compile(r"(?:^|[\s\0])" + re.escape(MARK) + "=" + re.escape(run) + r"(?:/[^\s\0]*)?(?:$|[\s\0])")
    found: set[int] = set()
    proc = Path("/proc")
    if (proc / "self" / "environ").exists():
        for entry in proc.iterdir():
            if not entry.name.isdigit():
                continue
            try:
                environment = (entry / "environ").read_bytes().decode("utf-8", "replace")
            except OSError:
                continue
            if wanted.search(environment):
                found.add(int(entry.name))
        return found
    # Darwin: the environment follows the command line, for the caller's own
    # processes, which is every process a run can have started.
    # The ps that answers is marked too, and sees itself.
    asked = subprocess.Popen(["ps", "-axEww", "-o", "pid=,command="], stdout=subprocess.PIPE, text=True)
    listing, _ = asked.communicate()
    for line in listing.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0].isdigit() and wanted.search(parts[1]):
            found.add(int(parts[0]))
    found.discard(asked.pid)
    return found


def own_tree(table: dict[int, tuple[int, str]], root: int) -> set[int]:
    """`root`, its ancestors, its descendants, and this process with its ancestors."""
    tree: set[int] = set()
    for start in (root, os.getpid()):
        pid = start
        while pid > 1 and pid not in tree:
            tree.add(pid)
            pid = table.get(pid, (0, ""))[0]
    grew = True
    descendants = {root}
    while grew:
        grew = False
        for pid, (parent, _command) in table.items():
            if parent in descendants and pid not in descendants:
                descendants.add(pid)
                grew = True
    return tree | descendants


def survivors(run: str, root: int) -> dict[int, str]:
    found = marked(run)
    table = process_table()
    # One that went between the two questions is not a survivor.
    outside = {pid for pid in found if pid in table} - own_tree(table, root)
    return {pid: table[pid][1] for pid in sorted(outside)}


def started(pid: int) -> str:
    return subprocess.run(["ps", "-o", "lstart=", "-p", str(pid)], capture_output=True, text=True, check=False).stdout.strip()


def is_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    state = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True, check=False).stdout.strip()
    return bool(state) and not state.startswith("Z")


def reap(run: str, root: int, grace: float = 2.0) -> list[int]:
    """Ends the run's survivors, and whatever they start while being ended."""
    ended: list[int] = []
    for sent in (signal.SIGTERM, signal.SIGKILL):
        found = survivors(run, root)
        for pid in found:
            try:
                os.kill(pid, sent)
            except (ProcessLookupError, PermissionError):
                continue
            if pid not in ended:
                ended.append(pid)
        deadline = time.monotonic() + grace
        while time.monotonic() < deadline and any(is_alive(pid) for pid in found):
            time.sleep(0.05)
    return ended


def listing(directory: Path) -> list[str]:
    try:
        return sorted(entry.name for entry in directory.iterdir())
    except OSError:
        return []


# A file a daemon keeps beside its socket: the stem is the socket leaf without
# `.sock`, and what follows says which file it is (.pid, .log, .sock.layouts).
DAEMON_FILE = re.compile(r"^(host-[0-9a-f]{16})\.")


def binary_stem(binary: Path) -> str:
    """The stem a daemon of `binary` names its files with.

    Core's socketLeafForBinary without `.sock`: the first eight bytes of the
    SHA-256 of the binary's contents, in hex, after `host-`.

    - Parameter binary: The ubiquitty-host binary.
    - Returns: The stem, or empty when the binary cannot be read or is empty,
      as socketLeafForBinary answers then.
    """
    digest = hashlib.sha256()
    total = 0
    try:
        with binary.open("rb") as stream:
            for chunk in iter(lambda: stream.read(65536), b""):
                digest.update(chunk)
                total += len(chunk)
    except OSError:
        return ""
    return "host-" + digest.hexdigest()[:16] if total else ""


def daemon_elsewhere(directory: Path, stem: str, table: dict[int, tuple[int, str]], own: set[int], installed: list[str]) -> str:
    """Why the daemon whose files share `stem` is not this run's.

    The daemon is found two ways: the process its pid file names, and any
    process whose command line names its socket. One of them carrying the
    run's mark makes the files the run's. Otherwise a live ubiquitty-host
    among them is another's daemon; the name is asked for so that a pid the
    system has since handed to something else is not taken for one. With no
    live daemon, the stem is compared with the installed host's.

    - Parameters:
      - directory: The state directory.
      - stem: The daemon's socket leaf without `.sock`.
      - table: The process table.
      - own: The processes carrying the run's mark.
      - installed: The installed host's stem, once asked for; empty until then.
    - Returns: The reason, or empty when the files may be the run's.
    """
    candidates: set[int] = set()
    try:
        named = int((directory / f"{stem}.pid").read_text().strip())
    except (OSError, ValueError):
        named = 0
    if named in table:
        candidates.add(named)
    socket = stem + ".sock"
    candidates |= {pid for pid, (_parent, command) in table.items() if socket in command}
    candidates.discard(os.getpid())
    if candidates & own:
        return ""
    for pid in sorted(candidates):
        command = table[pid][1]
        if "ubiquitty-host" in command:
            return f"its daemon, pid {pid}, carries no mark of this run: {command[:160]}"
    host = directory / "ubiquitty-host"
    if not installed:
        installed.append(binary_stem(host))
    if installed[0] == stem:
        return f"it is named for {host}, the installed host, which no test runs"
    return ""


def gained(directory: Path, record: Path, run: str) -> list[tuple[str, str]]:
    """The entries a directory gained since a snapshot, each with whose it is.

    - Parameters:
      - directory: The state directory.
      - record: The listing `snapshot` wrote.
      - run: The run's mark.
    - Returns: (name, reason) pairs; the reason is empty for an entry that
      counts against the run, and says whose it is otherwise.
    """
    before = set(record.read_text().splitlines()) if record.exists() else set()
    names = [name for name in listing(directory) if name not in before]
    if not names:
        return []
    table = process_table()
    own = marked(run)
    installed: list[str] = []
    entries = []
    for name in names:
        match = DAEMON_FILE.match(name)
        entries.append((name, daemon_elsewhere(directory, match.group(1), table, own, installed) if match else ""))
    return entries


def detach() -> None:
    """Leaves the caller's session and process group for good."""
    if os.fork() > 0:
        os._exit(0)
    os.setsid()
    if os.fork() > 0:
        os._exit(0)
    devnull = os.open(os.devnull, os.O_RDWR)
    for descriptor in (0, 1, 2):
        os.dup2(devnull, descriptor)


def main(arguments: list[str]) -> int:
    if not arguments:
        print(__doc__, file=sys.stderr)
        return 2
    command, rest = arguments[0], arguments[1:]
    run = os.environ.get(MARK, "")

    if command == "survivors":
        root = int(rest[0])
        for pid, line in survivors(run, root).items():
            print(f"{pid}\t{started(pid)}\t{line[:200]}")
        return 0

    if command == "marked":
        # Everything the run started that is still alive, its own tree included,
        # for a caller that ends some of it between steps.
        table = process_table()
        for pid in sorted(marked(run)):
            if pid in table and pid != os.getpid():
                print(f"{pid}\t{table[pid][1]}")
        return 0

    if command == "reap":
        root = int(rest[0])
        reap(run, root)
        left = survivors(run, root)
        for pid, line in left.items():
            print(f"test-run-hygiene: could not end {pid}: {line[:200]}", file=sys.stderr)
        return 1 if left else 0

    if command == "watch":
        # Not marked itself, and out of reach of whatever ends the run.
        watched_run = os.environ.get("UBQ_WATCHED_RUN", "")
        parent = int(rest[0])
        scratch = rest[1:]
        if not watched_run or MARK in os.environ:
            print("watch: UBQ_WATCHED_RUN names the run, and UBQ_TEST_RUN must be unset", file=sys.stderr)
            return 2
        detach()
        while is_alive(parent):
            time.sleep(0.5)
        reap(watched_run, os.getpid())
        for path in scratch:
            subprocess.run(["rm", "-rf", "--", path], check=False)
        return 0

    if command == "snapshot":
        directory, record = Path(rest[0]), Path(rest[1])
        record.write_text("\n".join(listing(directory)) + "\n")
        return 0

    if command == "added":
        # Tab-separated: `made` and the name, or `outside`, the name and why.
        directory, record = Path(rest[0]), Path(rest[1])
        for name, reason in gained(directory, record, run):
            print(f"outside\t{name}\t{reason}" if reason else f"made\t{name}")
        return 0

    if command == "plant":
        kind, _, entry = rest[0].partition(":")
        if kind in ("process", "process-then-hang"):
            sleeper = subprocess.Popen(
                [sys.executable, "-c", "import os,time\nif os.fork(): os._exit(0)\nos.setsid()\nif os.fork(): os._exit(0)\nprint(os.getpid(), flush=True)\ntime.sleep(600)"],
                stdout=subprocess.PIPE, text=True)
            print("planted process", sleeper.stdout.readline().strip(), flush=True)
            if kind == "process-then-hang":
                time.sleep(600)
        elif kind == "state":
            target = Path(rest[1])
            target.mkdir(parents=True, exist_ok=True)
            (target / "host-planted.pid").write_text("1\n")
            print("planted state", flush=True)
        elif kind == "entry":
            # The file alone, as a daemon outside the run would write it.
            (Path(rest[1]) / entry).write_text("")
            print("planted entry", entry, flush=True)
        elif kind == "daemon-entry":
            # A daemon of the run's own that wrote into this directory: a
            # detached process, named in the stem's pid file, and the file.
            target = Path(rest[1])
            stem = DAEMON_FILE.match(entry)
            if not stem:
                print(f"plant: {entry} is not a daemon's file", file=sys.stderr)
                return 2
            sleeper = subprocess.Popen(
                [sys.executable, "-c", "import os,time\nif os.fork(): os._exit(0)\nos.setsid()\nif os.fork(): os._exit(0)\nprint(os.getpid(), flush=True)\ntime.sleep(600)"],
                stdout=subprocess.PIPE, text=True)
            pid = sleeper.stdout.readline().strip()
            (target / f"{stem.group(1)}.pid").write_text(pid + "\n")
            (target / entry).write_text("")
            print("planted process", pid, flush=True)
        elif kind == "home":
            print("stanza state directory", os.environ.get("UBIQUITTY_HOME", "(unset)"), flush=True)
        elif kind != "none":
            print(f"plant: unknown kind {kind}", file=sys.stderr)
            return 2
        return 0

    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
