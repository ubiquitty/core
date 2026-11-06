#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# record-stream.py — record what a program writes to its terminal, with timing.
#
#   record-stream.py --columns 140 --rows 36 --seconds 60 \
#       --out /tmp/ubqstreams/agent_tmux_140x36.stream -- tmux attach -r
#
# The program runs in a pty of the given size and its output lands in timed
# records, so replay-stream.py hands every terminal the same bytes at the same
# pace. A live `tmux attach -r` is a different workload every run — the session
# behind it moves on — and two terminals measured a minute apart were measured
# against two workloads.
#
# Format: the magic line, then records of <u32 delay_us><u32 length><bytes>,
# little-endian, the delay counted from the previous record. The program's
# input is never written: a read-only client records what it shows and sends
# nothing back.
#
#   record-stream.py --census /tmp/ubqstreams/agent_tmux_140x36.stream
#
# reports what a recording holds — records, bytes, span, synchronized-output
# frames, bytes per second — which is the check to run before measuring on it.

import argparse
import fcntl
import os
import select
import signal
import struct
import sys
import termios
import time

MAGIC = b"UBQTSTREAM1\n"


def census(path):
    payload = open(path, "rb").read()
    if not payload.startswith(MAGIC):
        sys.exit(f"{path}: not a timed stream (no {MAGIC.strip().decode()} magic)")
    offset = len(MAGIC)
    records = total = sync_frames = 0
    span = 0.0
    while offset + 8 <= len(payload):
        delay_us, length = struct.unpack_from("<II", payload, offset)
        offset += 8
        chunk = payload[offset:offset + length]
        offset += length
        records += 1
        total += length
        span += delay_us / 1_000_000.0
        sync_frames += chunk.count(b"\x1b[?2026h")
    rate = total / span if span > 0 else 0.0
    print(f"{path}: {records} records, {total} bytes over {span:.1f} s, {sync_frames} synchronized-output frames, {rate:.0f} bytes/s")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--census", help="report what a recording holds and exit")
    parser.add_argument("--columns", type=int)
    parser.add_argument("--rows", type=int)
    parser.add_argument("--seconds", type=float)
    parser.add_argument("--out")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.census:
        census(args.census)
        return
    if not (args.columns and args.rows and args.seconds and args.out):
        sys.exit("record-stream.py: --columns, --rows, --seconds and --out are required to record")
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        sys.exit("record-stream.py: a command is required after --")

    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", args.rows, args.columns, 0, 0))
    env = dict(os.environ)
    # A tmux client started from inside a session refuses to nest.
    env.pop("TMUX", None)
    env["TERM"] = env.get("TERM") or "xterm-256color"
    env["COLUMNS"] = str(args.columns)
    env["LINES"] = str(args.rows)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        os.dup2(slave, 0)
        os.dup2(slave, 1)
        os.dup2(slave, 2)
        os.close(master)
        os.close(slave)
        os.execvpe(command[0], command, env)
    os.close(slave)

    deadline = time.monotonic() + args.seconds
    last = time.monotonic()
    total = 0
    records = 0
    with open(args.out, "wb") as out:
        out.write(MAGIC)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            ready, _, _ = select.select([master], [], [], min(remaining, 0.25))
            if not ready:
                continue
            try:
                data = os.read(master, 1 << 16)
            except OSError:
                break
            if not data:
                break
            now = time.monotonic()
            out.write(struct.pack("<II", int((now - last) * 1_000_000), len(data)))
            out.write(data)
            last = now
            total += len(data)
            records += 1
    # The child is its own process group; a hangup detaches a tmux client.
    os.killpg(pid, signal.SIGHUP)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    os.close(master)
    print(f"{args.out}: {records} records, {total} bytes over {args.seconds:.0f} s", file=sys.stderr)


if __name__ == "__main__":
    main()
