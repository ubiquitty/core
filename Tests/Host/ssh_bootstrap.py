#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Runs deployment, attach, discovery, and reattach through an isolated OpenSSH server."""

import os
import pathlib
import shlex
import socket
import subprocess
import sys
import tempfile
import time


def run(client, host):
    if os.geteuid() != 0 or not pathlib.Path('/.dockerenv').exists():
        raise RuntimeError('SSH smoke requires the disposable root Linux CI container')
    with tempfile.TemporaryDirectory(prefix='ubq-ssh-') as temporary:
        root = pathlib.Path(temporary)
        root.chmod(0o755)
        username = 'ubqssh' + str(os.getpid())
        user_directory = root / 'user'
        server = None
        created_user = False
        try:
            subprocess.run(['useradd', '-m', '-d', str(user_directory), '-s', '/bin/sh', '-p', '*', username], check=True)
            created_user = True
            for name in ['host-key', 'client-key']:
                subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(root / name)], check=True)
            ssh_directory = user_directory / '.ssh'
            ssh_directory.mkdir(mode=0o700)
            (ssh_directory / 'authorized_keys').write_bytes((root / 'client-key.pub').read_bytes())
            (ssh_directory / 'rc').write_text('printf "Startup banner before the bootstrap\\nunterminated login text"\n')
            subprocess.run(['chown', '-R', username + ':' + username, str(user_directory)], check=True)
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                port = listener.getsockname()[1]
            pathlib.Path('/run/sshd').mkdir(exist_ok=True)
            (root / 'sshd.conf').write_text(
                f'Port {port}\nListenAddress 127.0.0.1\nHostKey {root / "host-key"}\n'
                f'PidFile {root / "sshd.pid"}\nAllowUsers {username}\n'
                'PasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\n'
                'PermitRootLogin no\nAuthorizedKeysFile .ssh/authorized_keys\n')
            host_key = (root / 'host-key.pub').read_text().split()
            (root / 'known_hosts').write_text(f'[127.0.0.1]:{port} {host_key[0]} {host_key[1]}\n')
            (root / 'ssh.conf').write_text(
                f'Host fixture\n HostName 127.0.0.1\n Port {port}\n User {username}\n'
                f' IdentityFile {root / "client-key"}\n UserKnownHostsFile {root / "known_hosts"}\n'
                ' StrictHostKeyChecking yes\n IdentitiesOnly yes\n BatchMode yes\n ConnectTimeout 5\n LogLevel ERROR\n')
            server = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(root / 'sshd.conf')],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            deadline = time.monotonic() + 10
            while True:
                if server.poll() is not None:
                    raise RuntimeError('isolated sshd exited before accepting connections')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('isolated sshd did not start')
                    time.sleep(0.05)
            prefix = shlex.join(['ssh', '-F', str(root / 'ssh.conf'), 'fixture'])
            subprocess.run([client, '--ssh', prefix, host], check=True, timeout=120)
        finally:
            if created_user:
                subprocess.run(['pkill', '-KILL', '-u', username], check=False)
            if server is not None:
                server.terminate()
                server.wait(timeout=5)
            if created_user:
                subprocess.run(['userdel', '-r', username], check=False)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: ssh_bootstrap.py /path/to/remote_session_smoke /path/to/ubiquitty-host')
    run(*sys.argv[1:])
