#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks that the name an MCP client gives in `initialize` labels its attach.

The consent prompt shows the label an agent's Greet carries as that agent's
own claim, so the label has to be the client's: a prompt that called every
MCP client by one product's name would misname every other. A private socket
stands in for the host. It reads each Greet the agent sends, keeps the label,
and closes the connection, so the tool call fails after the label is seen and
no daemon is needed. The Greet's layout is putGreet's and putRequest's in
Messages.cpp.

Run with the matching build: python3 Tests/Agent/mcp_client_label.py /path/to/ubq.
"""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcp_tree_list import GREET, MAX_FRAME_BODY_BYTES, read_varint  # noqa: E402

# u16 wire version, then the app version, then the engine fingerprint, the
# client instance, the client kind and the requested role; then the request's
# target, session, workspace and expected daemon instance.
FINGERPRINT_BYTES = 32
CLIENT_INSTANCE_BYTES = 16
REQUEST_FIXED_BYTES = 1 + 4 + 4 + 8


def greeted_label(greet):
    """The agentLabel a Greet body carries, after the workspace name."""
    length, offset = read_varint(greet, 2)
    offset += length + FINGERPRINT_BYTES + CLIENT_INSTANCE_BYTES + 2 + REQUEST_FIXED_BYTES
    length, offset = read_varint(greet, offset)
    length, offset = read_varint(greet, offset + length)
    return greet[offset:offset + length].decode("utf-8")


def first_greet(connection):
    """The body of the first envelope on a connection, which must be a Greet."""
    pending = bytearray()
    while True:
        part = connection.recv(65536)
        if not part:
            return None
        pending.extend(part)
        assert len(pending) <= 2 * MAX_FRAME_BODY_BYTES, "oversized fixture stream"
        decoded = read_varint(pending, 1)
        if decoded is None:
            continue
        decoded = read_varint(pending, decoded[1])
        if decoded is None:
            continue
        length, offset = decoded
        if len(pending) - offset < length:
            continue
        assert pending[0] == GREET, f"the first envelope is {pending[0]}, not a Greet"
        return bytes(pending[offset:offset + length])


def serve(listener, labels, failures, finished):
    while not finished.is_set():
        try:
            connection, _ = listener.accept()
        except socket.timeout:
            continue
        except OSError as error:
            failures.append(error)
            return
        with connection:
            try:
                connection.settimeout(10)
                greet = first_greet(connection)
                if greet is not None:
                    labels.append(greeted_label(greet))
            except Exception as error:  # noqa: BLE001 — reported by the caller
                failures.append(error)


def labels_for(agent, messages):
    """Every label the agent greets with while it answers these messages."""
    with tempfile.TemporaryDirectory(prefix="ubq-mcp-label-", dir="/tmp") as directory:
        control = Path(directory) / "control"
        control.mkdir(mode=0o700)
        socket_path = control / "host-0123456789abcdef.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(socket_path))
            listener.listen(4)
            listener.settimeout(0.2)
            labels, failures, finished = [], [], threading.Event()
            worker = threading.Thread(target=serve, args=(listener, labels, failures, finished))
            worker.start()
            environment = os.environ.copy()
            for key in ("UBIQUITTY_HOST_BIN", "UBQ_REMOTE_TOKEN", "UBIQUITTY_AGENT_TOKEN",
                        "UBIQUITTY_AGENT_LABEL", "UBIQUITTY_SESSION", "UBIQUITTY_PANE"):
                environment.pop(key, None)
            policy = control / "allow.conf"
            policy.write_text("default allow\n")
            environment.update(UBIQUITTY_HOME=str(control), UBIQUITTY_SOCKET=str(socket_path),
                               UBIQUITTY_AGENT_POLICY=str(policy))
            try:
                process = subprocess.run([agent, "mcp"], text=True, capture_output=True, env=environment,
                                         input="".join(json.dumps(message) + "\n" for message in messages),
                                         timeout=30)
            finally:
                finished.set()
                worker.join(timeout=5)
            assert not worker.is_alive(), "the fixture host did not stop"
            assert not failures, failures
            assert process.returncode == 0, process.stderr
            return labels


def initialize(client_info):
    parameters = {"protocolVersion": "2024-11-05", "capabilities": {}}
    if client_info is not None:
        parameters["clientInfo"] = client_info
    return {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": parameters}


def call(tool, arguments, request_id=2):
    return {"jsonrpc": "2.0", "id": request_id, "method": "tools/call",
            "params": {"name": tool, "arguments": arguments}}


def check(agent, name, messages, expected):
    labels = labels_for(agent, messages)
    assert labels and set(labels) == {expected}, f"{name}: greeted as {labels}, not {expected!r}"
    print(f"mcp-client-label: {name}: ok")


def main():
    assert len(sys.argv) == 2, "usage: mcp_client_label.py /path/to/ubq"
    agent = str(Path(sys.argv[1]).resolve())
    screen = call("terminal_screen", {"session": 7})
    check(agent, "a client's own name labels an attach",
          [initialize({"name": "zed-smoke", "version": "1"}), screen], "zed-smoke")
    check(agent, "and a control connection",
          [initialize({"name": "zed-smoke", "version": "1"}), call("terminal_list", {})], "zed-smoke")
    check(agent, "a client that gives no name is an MCP client",
          [initialize(None), screen], "mcp-client")
    check(agent, "and so is one that never initializes", [screen], "mcp-client")
    check(agent, "a name with a control byte is not shown",
          [initialize({"name": "zed\u001b[2Jsmoke", "version": "1"}), screen], "mcp-client")
    check(agent, "a blank name is not shown",
          [initialize({"name": "   ", "version": "1"}), screen], "mcp-client")
    check(agent, "a long name is cut to 32 characters",
          [initialize({"name": "x" * 40, "version": "1"}), screen], "x" * 32)


if __name__ == "__main__":
    main()
