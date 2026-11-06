#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks terminal_list through a private Sync listing-only host.

The client speaks canonical envelopes from its first byte: a Greet asking
for an unbound Control connection, then the subscription Call. The small
records spell the same positional grammar as Messages.cpp, SessionTreeWire.cpp
and Records.cpp; the wire version comes from Envelope.h.
Run with the matching build: python3 Tests/Agent/mcp_tree_list.py /path/to/ubq.
"""

import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Host"))
from wire_version import wire_fingerprint, wire_version  # noqa: E402

GREET, WELCOME, FACTS, ACK, PROBE, ECHO, BYE, TYPOGRAPHY_FACTS = 1, 2, 3, 4, 5, 6, 10, 11
CALL, RESULT, WORKSPACE_STRUCTURE, PANE_LISTED = 13, 14, 16, 17
PANE_FACTS, STATUS_RECORDS = 48, 49
CONTROL_TARGET = 2
AGENT_ROLE = 3
MAX_FRAME_BODY_BYTES = 65535


def number(format, *values):
    return struct.pack("<" + format, *values)


def varint(value):
    output = bytearray()
    while value >= 128:
        output.append((value & 127) | 128)
        value >>= 7
    output.append(value)
    return bytes(output)


def read_varint(body, offset):
    value = 0
    for shift in range(0, 35, 7):
        if offset == len(body):
            return None
        byte = body[offset]
        offset += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            return value, offset
    raise AssertionError("unbounded fixture varint")


def text(value):
    encoded = value.encode("utf-8")
    return varint(len(encoded)) + encoded


def envelope(kind, body=b"", channel=0):
    return bytes([kind]) + varint(channel) + varint(len(body)) + body


def welcome(target=CONTROL_TARGET, session_id=0):
    # Granted Agent role on the requested target, no idle probes requested,
    # and room for a Call.
    body = number("H", wire_version()) + text("") + wire_fingerprint()
    body += number("QQQQQQ", 1, 2, 3, 4, 5, 6) + bytes([AGENT_ROLE])
    body += bytes([target]) + number("IIQ", session_id, 0, 7) + text("host-0123456789abcdef.sock") + b"\x00" + text("")
    body += number("Q", 0)  # restoredFromDaemonInstanceID: a live selection
    body += b"\x01" + number("II", 0, 0) + text("fixture")
    body += number("HIH", 16, 1048576, 16) + varint(0) + number("Q", 1)
    body += b"\x00" + varint(0) + b"\x00"
    return envelope(WELCOME, body)


def requested_target(greet):
    """The admission target a Greet names, after its wire version, build and
    identity."""
    offset = 2
    length, offset = read_varint(greet, offset)
    offset += length + 32 + 16 + 2
    target, session_id, workspace_id = struct.unpack_from("<BII", greet, offset)
    return target, session_id, workspace_id


def workspace(workspace_id, session_id, columns, rows, directory):
    # One window/tab/leaf. A restorable leaf has no live pane/session identity
    # and carries its saved title/directory in the structure itself.
    is_restorable = session_id == 0
    pane_id = (workspace_id, 1) if session_id else (0, 0)
    record = number("QBBI", 1, 2 if is_restorable else 0, 0, workspace_id)
    record += text("workspace") + text("") + b"\x00\x01\x00"
    record += number("I", workspace_id + 100) + text("") + b"\x00\x00\x01\x00"
    record += number("I", workspace_id + 200) + text("") + b"\x00" + text("") + b"\x01\x00"
    record += b"\x00" + number("QQIIHHHHQ", *pane_id, workspace_id + 300 if session_id else 0,
                               session_id, rows, columns, 0, 0, 1)
    record += text("") + b"\x00" + text("") + b"\x00\x00"
    if is_restorable:
        record += text("saved title") + text(directory) + text("")
    return envelope(WORKSPACE_STRUCTURE, number("IQBB", workspace_id, 1, 0, 0) + record)


def listed_records(workspace_id, handle, columns, rows, program, directory, is_idle):
    listed = envelope(PANE_LISTED, varint(handle) + number("QQ", workspace_id, 1))
    facts = number("I", 1)
    for value in ("title", directory, "", "", "", "", "", "", ""):
        facts += text(value)
    facts += number("HHBBI", rows, columns, 0, 0, 1) + text(program)
    facts += number("IBBB", 1, int(is_idle), 0, 0) + text("") + b"\x00"
    facts += number("HHHHBQQQB", 0, 0, 0, 0, 0, 0, 0, 0, 0) + text("") + text("")
    return listed + envelope(PANE_FACTS, facts, handle) + envelope(STATUS_RECORDS, number("IBBB", 1, 0, 0, 0), handle)


class Answer:
    """The subscription's Result, correlated to the Call's own id, and the
    records that follow it."""

    def __init__(self, workspaces, panes=(), records=b""):
        # Every structure here is at revision 1, and every pane's facts too.
        self.result = {"hostNowUnixMilliseconds": 1, "workspaces": workspaces,
                       "structureRevisions": ["1" for _ in workspaces],
                       "paneRevisions": [{"session": session, "revision": 1} for session in panes]}
        self.records = records

    def __add__(self, records):
        return Answer(self.result["workspaces"], [pane["session"] for pane in self.result["paneRevisions"]],
                      self.records + records)

    def to(self, request_id):
        response = {"jsonrpc": "2.0", "id": request_id, "result": self.result}
        return envelope(RESULT, varint(0) + json.dumps(response).encode()) + self.records


def answer(workspaces, panes=()):
    return Answer(workspaces, panes)


def receive(connection, count):
    body = bytearray()
    while len(body) < count:
        part = connection.recv(count - len(body))
        if not part:
            if not body:
                return None
            raise AssertionError("client ended a partial frame")
        body.extend(part)
    return bytes(body)


def serve(listener, response, failures, observed):
    try:
        with listener.accept()[0] as connection:
            connection.settimeout(20)
            pending = bytearray()
            is_greeted = False
            while True:
                part = connection.recv(65536)
                if not part:
                    return
                pending.extend(part)
                assert len(pending) <= 2 * MAX_FRAME_BODY_BYTES, "oversized retained fixture stream"
                # A read may hold several envelopes or only part of one;
                # retain the incomplete suffix.
                while pending:
                    kind = pending[0]
                    decoded = read_varint(pending, 1)
                    if decoded is None:
                        break
                    channel, offset = decoded
                    decoded = read_varint(pending, offset)
                    if decoded is None:
                        break
                    length, offset = decoded
                    assert length <= MAX_FRAME_BODY_BYTES, "oversized client fixture frame"
                    if len(pending) - offset < length:
                        break
                    carried = bytes(pending[offset:offset + length])
                    del pending[:offset + length]
                    assert channel == 0, f"listing addressed pane handle {channel}"
                    assert kind in (GREET, FACTS, ACK, PROBE, ECHO, BYE, TYPOGRAPHY_FACTS, CALL), \
                        f"listing sent unexpected envelope {kind}"
                    assert is_greeted or kind == GREET, "the first envelope is not a Greet"
                    if kind == GREET:
                        assert not is_greeted, "listing greeted twice"
                        is_greeted = True
                        assert requested_target(carried) == (CONTROL_TARGET, 0, 0), \
                            "listing tried to create or attach a session"
                        connection.sendall(welcome())
                    elif kind == PROBE:
                        connection.sendall(envelope(ECHO, carried + number("I", 0)))
                    elif kind == CALL:
                        request = json.loads(carried)
                        method = request.get("method")
                        assert isinstance(request.get("id"), str) and request == {
                            "jsonrpc": "2.0", "id": request["id"], "method": method, "params": {}}, \
                            f"listing called {request}"
                        if method == "host.sessions.unsubscribe":
                            # The daemon answers the end of a listing with an
                            # empty result; a client that has its rows may
                            # close before that answer arrives.
                            assert len(observed) == 1, "listing unsubscribed before it subscribed"
                            try:
                                connection.sendall(envelope(RESULT, varint(0) + json.dumps(
                                    {"jsonrpc": "2.0", "id": request["id"], "result": {}}).encode()))
                            except (BrokenPipeError, ConnectionResetError):
                                return
                            continue
                        assert method == "host.sessions.subscribe", f"listing called {request}"
                        observed.append(request)
                        assert len(observed) == 1, "listing subscribed more than once"
                        if response is None:
                            return
                        connection.sendall(response.to(request["id"]))
                        # Keep the connection live for an incomplete listing.
                        # Timeout must not turn missing records into rows.
                    elif kind == BYE:
                        return
    except Exception as error:
        failures.append(error)


def check(agent, name, response, expected_text, is_error=False):
    with tempfile.TemporaryDirectory(prefix="ubq-mcp-tree-", dir="/tmp") as directory:
        control = Path(directory) / "control"
        control.mkdir(mode=0o700)
        socket_path = control / "host-0123456789abcdef.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(socket_path))
            listener.listen(1)
            listener.settimeout(20)
            failures = []
            observed = []
            worker = threading.Thread(target=serve, args=(listener, response, failures, observed))
            worker.start()
            environment = os.environ.copy()
            for key in ("UBIQUITTY_HOST_BIN", "UBQ_REMOTE_TOKEN", "UBIQUITTY_AGENT_TOKEN",
                        "UBIQUITTY_AGENT_POLICY", "UBIQUITTY_SESSION", "UBIQUITTY_PANE"):
                environment.pop(key, None)
            environment.update(UBIQUITTY_HOME=str(control), UBIQUITTY_SOCKET=str(socket_path))
            request = {"jsonrpc": "2.0", "id": 11, "method": "tools/call",
                       "params": {"name": "terminal_list", "arguments": {}}}
            try:
                process = subprocess.run([agent, "mcp"], input=json.dumps(request) + "\n",
                                         text=True, capture_output=True, env=environment, timeout=18)
            finally:
                worker.join(timeout=21)
            assert not worker.is_alive(), f"{name}: fixture did not finish"
            assert not failures, f"{name}: {failures}"
            assert len(observed) == 1, f"{name}: no Sync subscription"
            assert process.returncode == 0, f"{name}: {process.stderr}"
            result = {"content": [{"type": "text", "text": expected_text}]}
            if is_error:
                result["isError"] = True
            expected = {"jsonrpc": "2.0", "id": 11, "result": result}
            assert json.loads(process.stdout) == expected, f"{name}: {process.stdout!r}"
            print(f"mcp-tree-list: {name}: ok")


def main():
    assert len(sys.argv) == 2, "usage: mcp_tree_list.py /path/to/ubq"
    agent = str(Path(sys.argv[1]).resolve())
    # Arrival/workspace order differs from row order. Restorable rows retain
    # their workspace IDs and follow all live sessions, even lower IDs.
    rows = [(80, 9, 132, 43, "make", "/work/quote\"\\tab\tline\né", False),
            (7, 0, 90, 30, "", "/saved", False),
            (5, 4, 80, 24, "zsh", "/home/user", True),
            (40, 0, 100, 35, "", "/" + "x" * 600, False)]
    records = b""
    sessions = []
    expected_rows = []
    for index, (workspace_id, session_id, columns, height, program, directory, is_idle) in enumerate(rows):
        records += workspace(workspace_id, session_id, columns, height, directory)
        if session_id:
            sessions.append(session_id)
            records += listed_records(workspace_id, 0x40000000 + index, columns, height, program, directory, is_idle)
        row_id = session_id or workspace_id
        line = f"{row_id}\t{columns}x{height}\t{'idle' if is_idle else 'busy'}\t{program}\t{directory}\n"
        # Preserve the MCP surface's bounded snprintf row, including its
        # 511-byte truncation, rather than changing the text API in migration.
        expected_rows.append((not bool(session_id), row_id, line.encode()[:511].decode()))
    expected = "".join(line for _, _, line in sorted(expected_rows))
    check(agent, "live/restorable identity, order, escaping and row bound",
          answer([row[0] for row in rows], sessions) + records, expected)
    check(agent, "empty complete host", answer([]), "(no sessions)")
    check(agent, "missing workspace", answer([80]), "no response", True)
    check(agent, "missing pane facts", answer([80], [9]) + workspace(80, 9, 80, 24, "/work"), "no response", True)
    check(agent, "malformed structure", answer([80]) + envelope(WORKSPACE_STRUCTURE, number("IQBB", 80, 1, 0, 0) + b"\x00"), "no response", True)
    check(agent, "malformed subscription answer", answer(["not-an-id"]), "no response", True)
    check(agent, "no subscription answer", None, "no response", True)


if __name__ == "__main__":
    main()
