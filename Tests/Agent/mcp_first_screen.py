#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks MCP readiness against a private host that sends no screen.

A tool that needs the screen must report its absence before acting. Creating
an admitted session needs its acknowledged identity, not a binding or checkpoint.
The fixture uses the canonical envelope and handshake encoders shared with the
listing test.
"""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time

from mcp_tree_list import (BYE, CALL, ECHO, GREET, MAX_FRAME_BODY_BYTES, PROBE,
                           envelope, number, read_varint, requested_target, text, varint, welcome)

SESSION_TARGET = 0
PANE_BIND = 7
RAW = 69
SESSION_ID = 7
TOKEN_INVALID = 12
NO_CONSENT = 17
HOST_FULL = 20


def serve(listener, binds_pane, closes_after_binding, closes_socket, fails_writes, refusal, observed, failures, finished):
    """Admits one session without publishing a checkpoint or answering Calls."""
    try:
        with listener.accept()[0] as connection:
            connection.settimeout(15)
            pending = bytearray()
            greeted = False
            did_close_output = False
            while True:
                part = connection.recv(65536)
                if not part:
                    return
                pending.extend(part)
                assert len(pending) <= 2 * MAX_FRAME_BODY_BYTES, "fixture input exceeded its bound"
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
                    assert length <= MAX_FRAME_BODY_BYTES, "oversized fixture frame"
                    if len(pending) - offset < length:
                        break
                    body = bytes(pending[offset:offset + length])
                    del pending[:offset + length]
                    observed.append(kind)
                    assert len(observed) <= 256, "fixture frame count exceeded its bound"
                    if kind == GREET:
                        assert not greeted and channel == 0, "invalid greeting"
                        greeted = True
                        target, requested_session, workspace = requested_target(body)
                        assert target == SESSION_TARGET and requested_session in (0, SESSION_ID) and workspace == 0
                        if refusal is not None:
                            connection.sendall(envelope(BYE, bytes([refusal]) + text("") + b"\x00"))
                            return
                        response = welcome(SESSION_TARGET, SESSION_ID)
                        if binds_pane:
                            response += envelope(PANE_BIND, varint(1) + number("QQ", SESSION_ID, 7))
                        if fails_writes:
                            # Linux rejects writes after the peer shuts down
                            # reading; Darwin/BSD Unix streams do not.
                            connection.shutdown(socket.SHUT_RD)
                        connection.sendall(response)
                        if closes_socket:
                            return
                        if fails_writes:
                            assert finished.wait(timeout=15), "client did not report the failed write"
                            return
                        if closes_after_binding:
                            # EOF must not race EPIPE from the client's final
                            # acknowledgements: keep accepting its writes.
                            connection.shutdown(socket.SHUT_WR)
                            did_close_output = True
                        elif not binds_pane:
                            return
                    elif kind == PROBE:
                        if not did_close_output:
                            connection.sendall(envelope(ECHO, body + number("I", 0)))
                    elif kind in (BYE, CALL, RAW):
                        return
    except Exception as error:  # noqa: BLE001 — surfaced by the fixture owner
        failures.append(error)


def check(agent, tool, arguments, closes_after_binding=False, closes_socket=False, fails_writes=False, refusal=None):
    with tempfile.TemporaryDirectory(prefix="ubq-mcp-first-screen-", dir="/tmp") as directory:
        control = Path(directory) / "control"
        control.mkdir(mode=0o700)
        policy = control / "allow.conf"
        policy.write_text("default allow\n")
        socket_path = control / "host-0123456789abcdef.sock"
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(str(socket_path))
            listener.listen(1)
            listener.settimeout(15)
            observed, failures = [], []
            finished = threading.Event()
            worker = threading.Thread(target=serve,
                                      args=(listener, tool != "terminal_new", closes_after_binding, closes_socket, fails_writes,
                                            refusal, observed, failures, finished))
            worker.start()
            environment = os.environ.copy()
            for key in ("UBIQUITTY_HOST_BIN", "UBQ_REMOTE_TOKEN", "UBIQUITTY_AGENT_TOKEN",
                        "UBIQUITTY_AGENT_LABEL", "UBIQUITTY_SESSION", "UBIQUITTY_PANE"):
                environment.pop(key, None)
            environment.update(UBIQUITTY_HOME=str(control), UBIQUITTY_SOCKET=str(socket_path),
                               UBIQUITTY_AGENT_POLICY=str(policy),
                               UBIQUITTY_AGENT_AUDIT=str(control / "agent-audit.log"))
            request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": tool, "arguments": arguments}}
            started = time.monotonic()
            try:
                process = subprocess.run([agent, "mcp"], input=json.dumps(request) + "\n",
                                         text=True, capture_output=True, env=environment, timeout=17)
            finally:
                finished.set()
                worker.join(timeout=16)
            elapsed = time.monotonic() - started
            assert not worker.is_alive(), "fixture host did not finish"
            assert not failures, failures
            assert process.returncode == 0, process.stderr
            assert GREET in observed, "fixture never received a greeting"
            assert CALL not in observed and RAW not in observed, "a tool acted without its first screen"
            response = json.loads(process.stdout)
            assert response["id"] == 1
            result = response["result"]
            if tool == "terminal_new":
                assert result == {"content": [{"type": "text", "text": "session 7"}]}, result
            else:
                assert result.get("isError") is True, result
                assert all(item["type"] == "text" for item in result["content"]), result
                message = "\n".join(item["text"] for item in result["content"])
                if refusal is not None:
                    reason = {TOKEN_INVALID: "token not recognized", NO_CONSENT: "no live human", HOST_FULL: "as many sessions"}
                    assert reason[refusal] in message, message
                else:
                    assert "first screen" in message, message
                    if fails_writes:
                        expected, outcome = "writing to the host failed", "failed write before checkpoint"
                    elif closes_socket:
                        expected, outcome = "did not arrive", "closed socket before checkpoint"
                    elif closes_after_binding:
                        expected, outcome = "did not arrive", "EOF before checkpoint"
                    else:
                        expected, outcome = "timed out", "checkpoint deadline"
                    assert expected in message, f"expected {outcome}, got {message!r} after {elapsed:.3f}s"
                    if closes_socket:
                        # A full close may be observed by the read or write
                        # first. Either must report the actual connection end.
                        reasons = ("writing to the host failed", "the connection to the host was lost",
                                   "the host closed the connection without saying why")
                        assert any(reason in message for reason in reasons), message
                if tool == "terminal_run":
                    audit = (control / "agent-audit.log").read_text()
                    if refusal in (TOKEN_INVALID, NO_CONSENT):
                        assert "run deny: [attach-denied]" in audit, audit
                        assert "incomplete:" not in audit, audit
                    else:
                        assert "run incomplete: [attach-incomplete]" in audit, audit
                        assert "deny:" not in audit and "[attach-denied]" not in audit, audit
                    assert message in audit, (message, audit)
            return elapsed


def main():
    assert len(sys.argv) == 2, "usage: mcp_first_screen.py /path/to/ubq"
    agent = str(Path(sys.argv[1]).resolve())
    run_arguments = {"session": SESSION_ID, "command": "echo never-written"}
    cases = [
        ("checkpoint deadline", "terminal_screenshot", {"session": SESSION_ID}, {}),
        ("identity without checkpoint", "terminal_new", {}, {}),
        ("EOF before checkpoint", "terminal_run", run_arguments, {"closes_after_binding": True}),
        ("closed socket before checkpoint", "terminal_run", run_arguments, {"closes_socket": True}),
        ("failed write before checkpoint", "terminal_run", run_arguments, {"fails_writes": True}),
        ("invalid token", "terminal_run", run_arguments, {"refusal": TOKEN_INVALID}),
        ("missing consent", "terminal_run", run_arguments, {"refusal": NO_CONSENT}),
        ("host full", "terminal_run", run_arguments, {"refusal": HOST_FULL}),
    ]
    passed, failed, skipped = 0, 0, 0
    for name, tool, arguments, options in cases:
        label = f"mcp-first-screen: {tool} ({name})"
        if options.get("fails_writes") and sys.platform != "linux":
            print(f"{label}: SKIP (Linux-only trigger: peer SHUT_RD does not force "
                  "a write failure on Darwin/BSD Unix streams)", flush=True)
            skipped += 1
            continue
        try:
            elapsed = check(agent, tool, arguments, **options)
        except Exception as error:  # noqa: BLE001 — report every case before failing the suite
            print(f"{label}: FAIL ({type(error).__name__}: {error})", flush=True)
            failed += 1
        else:
            print(f"{label}: ok ({elapsed:.3f}s)", flush=True)
            passed += 1
    print(f"mcp-first-screen: {passed} passed, {failed} failed, {skipped} skipped", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
