#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""A QUIC request cannot replace a live owner or signal a PID-file bystander."""

import fcntl
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


def end_detached_daemon(lock, pid_file, children):
    """Ends a daemon an ensure started detached, which no wait of this process reaches.

    Only while the socket's startup lock is held: a daemon holds it for its
    whole life and writes the PID file after taking it, so a file read while
    nobody holds the lock may name a number the system has given another
    process. A process of this fixture's own is left to it.
    """
    try:
        with lock.open('rb') as handle:
            fcntl.flock(handle, fcntl.LOCK_SH | fcntl.LOCK_NB)
        return
    except BlockingIOError:
        pass
    except FileNotFoundError:
        return
    try:
        daemon = int(pid_file.read_text().strip())
    except (OSError, ValueError):
        return
    if daemon <= 1 or daemon in children:
        return
    for sent in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.kill(daemon, sent)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                os.kill(daemon, 0)
            except ProcessLookupError:
                return
            time.sleep(.05)


def check(binary):
    with tempfile.TemporaryDirectory(prefix='ubq-ensure-owner-', dir='/tmp') as temporary:
        root = Path(temporary)
        environment = os.environ.copy()
        environment['UBIQUITTY_HOME'] = temporary
        environment['XDG_CONFIG_HOME'] = str(root / 'config')
        leaf = 'host-0000000000000000.sock'
        path = root / leaf
        pid_file = root / leaf.replace('.sock', '.pid')
        with (root / 'host.log').open('wb') as log:
            owner = subprocess.Popen([binary, '--daemon', '--socket', leaf, '--quic-port', '-1'], env=environment, stdout=log, stderr=log)
            bystander = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'], env=environment)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if owner.poll() is not None:
                        raise AssertionError('test owner exited before listening')
                    try:
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.connect(str(path))
                        break
                    except OSError:
                        time.sleep(.01)
                else:
                    raise AssertionError('test owner did not listen')

                # A stale PID file can name a completely unrelated process.
                # Both processes are this fixture's children, never user jobs.
                pid_file.write_text(str(bystander.pid) + '\n')
                result = subprocess.run([binary, '--ensure-daemon', '--socket', leaf, '--quic-port', '0'], env=environment,
                                        capture_output=True, timeout=6)
                failures = []
                if bystander.poll() is not None:
                    failures.append('ensuring QUIC signalled the PID-file bystander')
                if owner.poll() is not None:
                    failures.append('ensuring QUIC replaced the live owner')
                if result.returncode != 0:
                    failures.append('ensuring an already-live owner did not succeed')
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.connect(str(path))
                if pid_file.read_text().strip() != str(bystander.pid):
                    failures.append('ensuring an already-live owner rewrote its PID file')
                assert not failures, '; '.join(failures)

                bootstrap = subprocess.run([binary, '--quic-bootstrap', '--socket', leaf], env=environment,
                                           capture_output=True, timeout=6)
                assert bootstrap.returncode != 0, 'a disabled listener must not offer QUIC'
                assert owner.poll() is None and bystander.poll() is None, 'a declined bootstrap must preserve both processes'
            finally:
                for child in (owner, bystander):
                    if child.poll() is None:
                        child.terminate()
                    try:
                        child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait(timeout=3)
                # An owner that was gone before the ensure leaves the ensure
                # to start a daemon of its own, in a session of its own.
                end_detached_daemon(root / (leaf + '.lock'), pid_file, {owner.pid, bystander.pid})
    print('ensure_preserves_owner: live owner and PID-file bystander preserved')


if __name__ == '__main__':
    check(str(Path(sys.argv[1]).resolve()))
