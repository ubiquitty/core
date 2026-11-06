#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Relays private agent connections, stopping the daemon before marked input."""

import os
from pathlib import Path
import selectors
import signal
import socket
import sys


class Relay:
    def __init__(self, selector, marker, freeze):
        self.selector = selector
        self.marker = marker
        self.freeze = freeze
        self.peers = {}
        self.seen = {}
        self.frozen_client = None

    def add_pair(self, client, daemon):
        self.peers[client], self.peers[daemon] = daemon, client
        self.seen[client] = b""
        self.selector.register(client, selectors.EVENT_READ)
        self.selector.register(daemon, selectors.EVENT_READ)

    def retire_pair(self, source):
        other = self.peers.pop(source, None)
        if other is None:
            return False
        self.peers.pop(other)
        for end in (source, other):
            self.seen.pop(end, None)
            self.selector.unregister(end)
            end.close()
        return self.frozen_client in (source, other)

    def transfer(self, source):
        # A selected pair can already have retired on its other end's event.
        if source not in self.peers:
            return False
        try:
            data = source.recv(65536)
        except (BrokenPipeError, ConnectionResetError):
            return self.retire_pair(source)
        if not data:
            return self.retire_pair(source)
        if source in self.seen and self.frozen_client is None:
            self.seen[source] = (self.seen[source] + data)[-4096:]
            if self.marker in self.seen[source]:
                self.frozen_client = source
                self.freeze()
        try:
            self.peers[source].sendall(data)
        except (BrokenPipeError, ConnectionResetError):
            # A completed control client may close before its final reply.
            # Its pair owns that failure, not the listener or the next attach.
            return self.retire_pair(source)
        return False

    def close(self):
        for source in list(self.peers):
            self.retire_pair(source)


def main():
    listen_path, daemon_path, daemon_pid, marker, frozen_path, ready_path = sys.argv[1:]
    daemon_pid = int(daemon_pid)

    def freeze():
        os.kill(daemon_pid, signal.SIGSTOP)
        Path(frozen_path).touch()

    def finish(*_):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, finish)
    signal.signal(signal.SIGALRM, finish)
    signal.alarm(60)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server, selectors.DefaultSelector() as selector:
        relay = Relay(selector, marker.encode(), freeze)
        try:
            server.bind(listen_path)
            server.listen(8)
            selector.register(server, selectors.EVENT_READ)
            Path(ready_path).touch()
            while True:
                for key, _ in selector.select():
                    if key.fileobj is server:
                        client, _ = server.accept()
                        daemon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        try:
                            daemon.connect(daemon_path)
                        except BaseException:
                            client.close()
                            daemon.close()
                            raise
                        relay.add_pair(client, daemon)
                    elif relay.transfer(key.fileobj):
                        return
        finally:
            if relay.frozen_client is not None:
                try:
                    os.kill(daemon_pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            relay.close()
            signal.alarm(0)


if __name__ == "__main__":
    main()
