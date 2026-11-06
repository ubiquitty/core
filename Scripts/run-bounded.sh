#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/run-bounded.sh — the one bound every stanza runs under, sourced by
# run-tests.sh and by the tests that prove what it leaves behind.

# ubq_run_bounded SECONDS BINARY [ARGUMENT…]
#
# Runs a stanza's binary in a process group of its own and leaves nothing of
# that group behind: when the binary exits, when it is still running after
# SECONDS, and when this script is interrupted or terminated, the whole group
# is killed. A stanza that hangs would otherwise hold its runner until the job's own
# limit, and whatever it had started would outlive both. A program the binary runs
# under a PTY is in a session of its own, outside the group; it is hung up
# when the binary's end of the PTY closes, which killing the binary does.
#
# A stanza that runs out of time, or a runner that is interrupted, sends the
# group SIGTERM first and SIGKILL only after UBQ_STANZA_GRACE_SECONDS (20):
# a daemon a stanza started is in a session of its own, so killing the group
# cannot reach it, and only the stanza's own trap can stop it. A SIGKILL at
# once leaves that trap no chance to run.
#
# The binary runs with a HOME of its own under /tmp, made for the run and
# removed after it, and no shell history: a daemon it starts, and the shell a
# session starts, never read or write the home of the person running the
# suite. The home holds an empty .zshrc: a zsh with no startup file runs its
# new-user menu, which would take what a test types as its answers. A test
# that needs a particular HOME sets its own. A build a stanza
# runs keeps the compiler cache it had, which ccache would otherwise look for
# in the new home.
#
# Exits with the binary's status, or 124 when the limit ended it.
ubq_run_bounded() {
    python3 -c '
import os, shutil, signal, subprocess, sys, tempfile

seconds = float(sys.argv[1])
limit = seconds if seconds > 0 else None
grace = float(os.environ.get("UBQ_STANZA_GRACE_SECONDS", "20"))
home = tempfile.mkdtemp(prefix="ubq-home.", dir="/tmp")
open(os.path.join(home, ".zshrc"), "w").close()
environment = dict(os.environ, HOME=home, HISTFILE="/dev/null")
if "CCACHE_DIR" not in os.environ and shutil.which("ccache"):
    cache = subprocess.run(["ccache", "--get-config", "cache_dir"], capture_output=True, text=True)
    if cache.returncode == 0 and cache.stdout.strip():
        environment["CCACHE_DIR"] = cache.stdout.strip()
child = subprocess.Popen(sys.argv[2:], start_new_session=True, env=environment)


def leave(status):
    shutil.rmtree(home, ignore_errors=True)
    sys.exit(status)


def end_group():
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def stop_group():
    """Asks the group to stop, waits for the leader to run its trap, then kills what is left."""
    try:
        os.killpg(child.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        child.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        pass
    end_group()


def interrupted(number, _frame):
    stop_group()
    leave(128 + number)


for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
    signal.signal(number, interrupted)
try:
    status = child.wait(timeout=limit)
except subprocess.TimeoutExpired:
    stop_group()
    child.wait()
    name = os.path.basename(sys.argv[2])
    print("run-tests.sh: FAILED — 1 stanza binary (%s) still ran after %d s; its process group was killed" % (name, seconds), flush=True)
    leave(124)
end_group()
leave(status if status >= 0 else 128 - status)
' "$@"
}

# ubq_run_private BINARY [ARGUMENT…]
#
# Runs a stanza's binary as ubq_run_bounded does, in a group of its own and
# with a HOME of its own, but with no time limit, for a stanza that bounds its
# own waits.
ubq_run_private() {
    ubq_run_bounded 0 "$@"
}
