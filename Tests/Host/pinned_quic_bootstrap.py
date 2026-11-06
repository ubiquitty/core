#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks explicit-socket QUIC bootstrap against one private daemon without starting a replacement."""

import os
import pathlib
import subprocess
import sys
import tempfile
import time

def run(binary):
    with tempfile.TemporaryDirectory(prefix='ubpin-', dir='/tmp') as temporary:
        root = pathlib.Path(temporary)
        environment = os.environ.copy()
        environment.update(UBIQUITTY_HOME=temporary, ZDOTDIR=temporary, SHELL='/bin/sh')
        log = open(root / 'daemon.log', 'wb')
        name = 'host-1111111111111111.sock'
        daemon = subprocess.Popen([binary, '--daemon', '--socket', name, '--quic-port', '0'], env=environment, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while not list(root.rglob(name)):
                assert daemon.poll() is None, 'isolated QUIC daemon failed'
                assert time.monotonic() < deadline, 'isolated daemon socket not published'
                time.sleep(0.02)
            before = {str(p.relative_to(root)) for p in root.rglob('*.sock')}
            answers = []
            for arguments in [['--quic-bootstrap', '--socket', name], ['--socket', name, '--quic-bootstrap']]:
                result = subprocess.run([binary, *arguments], env=environment, capture_output=True, timeout=10)
                assert result.returncode == 0, 'pinned bootstrap failed: ' + result.stderr.decode()
                fields = dict((item.split('=', 1) for item in result.stdout.decode().split()))
                assert int(fields['quic_port']) > 0 and fields['quic_token'], 'missing QUIC endpoint/token'
                answers.append(fields)
                assert {str(p.relative_to(root)) for p in root.rglob('*.sock')} == before, 'bootstrap created another daemon socket'
                assert daemon.poll() is None, 'bootstrap replaced the live daemon'
            assert answers[0]['quic_port'] == answers[1]['quic_port'], 'argument order changed target daemon'
            assert answers[0]['quic_token'] != answers[1]['quic_token'], 'bootstrap reused one-time token'
            for arguments in [['--quic-bootstrap', '--socket', 'host-missing.sock'], ['--socket', 'host-missing.sock', '--quic-bootstrap']]:
                result = subprocess.run([binary, *arguments], env=environment, capture_output=True, timeout=10)
                assert result.returncode != 0 and (not result.stdout), 'missing pin silently bootstrapped another daemon'
                assert {str(p.relative_to(root)) for p in root.rglob('*.sock')} == before, 'missing pin created a daemon'
            print('PASS: pinned QUIC bootstrap succeeds in both argument orders, same live daemon, unique tokens; missing pin fails without spawning or replacing')
        finally:
            if daemon.poll() is None:
                daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
            log.close()
if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: pinned_quic_bootstrap.py /path/to/ubiquitty-host')
    run(sys.argv[1])
