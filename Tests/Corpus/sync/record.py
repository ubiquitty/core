#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""record.py — the recorded half of the Sync PTY corpus.

Runs a real program on a PTY of a fixed size, types a fixed script of keys at
fixed instants, and records every read the way a host makes it: the instant the
bytes became readable, how many came back, and whether the PTY then had nothing
more (a zero-timeout poll after the read found it empty — a would-block).

Recorded only where synthesis would misrepresent the program's shape: an
editor's scroll regions, a pager's repaint, a process monitor's refresh. The
rest of the corpus is synthetic (generate.py).

The program runs in a scratch directory with an empty HOME and a fixed
environment, so the stream names nobody: no user, host, path or configuration
of the machine that recorded it. Each entry is trimmed to stay under 200 KB.

Invoked by Scripts/record-pty-corpus.sh, which says what each entry runs.

The bytes recorded are the kernel's. A host reads through PTYStream, which
puts one cancel byte (0x18) ahead of the echo of an interrupt it wrote to a terminal in
cooked mode (Docs/architecture.md, section 4). An entry whose key script sent
^C, ^\\ or ^Z to such a program would differ from what a host parses by that
byte; the entries here all run programs that take those keys themselves.
"""

import argparse
import fcntl
import os
import pty
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE_LIMIT = 200 * 1024


def corpus_text(lines):
    words = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho "
             "sigma tau upsilon phi chi psi omega").split()
    out = []
    for i in range(lines):
        out.append(f"{i + 1:4d} " + " ".join(words[(i * 7 + k) % len(words)] for k in range(6 + i % 7)))
    return "\n".join(out) + "\n"


# name: (argv, rows, columns, [(delay seconds before key, key bytes)], description)
SCRIPTS = {
    "vim-margins": (
        ["vim", "-u", "NONE", "-i", "NONE", "-N", "-n", "corpus.txt"], 24, 80,
        [(0.8, b":set number\r"), (0.3, b":split\r"), (0.4, b"\x05")] + [(0.12, b"\x05")] * 20
        + [(0.3, b"\x04"), (0.3, b"\x04"), (0.3, b"\x17w")] + [(0.12, b"\x19")] * 10
        + [(0.3, b"Gzz"), (0.3, b"\x15"), (0.3, b"ggOinserted line\x1b"), (0.4, b"u"),
           (0.4, b":q!\r"), (0.3, b":q!\r")],
        "vim with no initialization file and a horizontal split, scrolled line by line in each window (DECSTBM margins)",
    ),
    "less": (
        ["less", "-R", "corpus.txt"], 24, 80,
        [(0.6, b" "), (0.3, b" ")] + [(0.1, b"j")] * 15
        + [(0.3, b"/omega\r"), (0.3, b"n"), (0.3, b"n"), (0.3, b"b"), (0.3, b"G"), (0.3, b"g"), (0.4, b"q")],
        "less paging, line scrolling, a search with highlighted matches, end and start of file",
    ),
    "top": (
        ["top", "-s", "1", "-stats", "pid,command,cpu,mem,time,state", "-pid", "{sleeper}"], 24, 80,
        [(10.5, b"q")],
        "top refreshing every second, filtered to one scratch process (stands in for htop, which this host lacks)",
    ),
}


def set_window_size(fd, rows, columns):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))


def record(name, argv, rows, columns, keys):
    scratch = tempfile.mkdtemp(prefix="ubq-corpus-")
    home = os.path.join(scratch, "home")
    os.mkdir(home)
    with open(os.path.join(scratch, "corpus.txt"), "w") as handle:
        handle.write(corpus_text(400))
    sleeper = subprocess.Popen(["sleep", "60"])
    argv = [part.replace("{sleeper}", str(sleeper.pid)) for part in argv]
    environment = {
        "TERM": "xterm-256color",
        "HOME": home,
        "PATH": "/usr/bin:/bin",
        "LANG": "en_US.UTF-8",
        "LC_ALL": "en_US.UTF-8",
        "LESSHISTFILE": "-",
        "USER": "user",
        "LOGNAME": "user",
    }
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(scratch)
        os.execve(shutil.which(argv[0], path=environment["PATH"]), argv, environment)
    set_window_size(fd, rows, columns)
    start = time.monotonic()
    data = bytearray()
    reads = []
    schedule = []
    at = 0.0
    for delay, key in keys:
        at += delay
        schedule.append((at, key))
    deadline = at + 3.0
    while True:
        now = time.monotonic() - start
        if schedule and now >= schedule[0][0]:
            os.write(fd, schedule.pop(0)[1])
            continue
        if now > deadline:
            break
        timeout = (schedule[0][0] - now) if schedule else 0.1
        ready, _, _ = select.select([fd], [], [], max(0.0, min(timeout, 0.1)))
        if not ready:
            continue
        stamp = time.monotonic() - start
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        more, _, _ = select.select([fd], [], [], 0)
        reads.append((int(stamp * 1_000_000), len(chunk), "more" if more else "block"))
        data += chunk
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    sleeper.kill()
    sleeper.wait()
    shutil.rmtree(scratch)
    # Trim from the end, at a read boundary, to the size limit.
    while len(data) > SIZE_LIMIT and reads:
        length = reads.pop()[1]
        del data[len(data) - length:]
    if reads:
        origin = reads[0][0]
        reads = [(at - origin, length, ending) for at, length, ending in reads]
    return bytes(data), reads


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="entries to record; all when none are named")
    arguments = parser.parse_args()
    for name in arguments.names or SCRIPTS:
        argv, rows, columns, keys, description = SCRIPTS[name]
        if not shutil.which(argv[0], path="/usr/bin:/bin"):
            print(f"record.py: {argv[0]} is not installed; {name} not recorded", file=sys.stderr)
            return 1
        data, reads = record(name, argv, rows, columns, keys)
        with open(os.path.join(HERE, name + ".stream"), "wb") as handle:
            handle.write(data)
        lines = [
            "# " + description + f" (recorded at {rows}x{columns})",
            "# Each read: <microseconds> <length> <block|more>.",
            "#   block — a zero-timeout poll after the read found the PTY empty.",
            "#   more  — bytes were already waiting; a host keeps draining.",
        ]
        lines += [f"{at} {length} {ending}" for at, length, ending in reads]
        with open(os.path.join(HERE, name + ".reads"), "w") as handle:
            handle.write("\n".join(lines) + "\n")
        print(f"record.py: {name}: {len(data)} bytes in {len(reads)} reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
