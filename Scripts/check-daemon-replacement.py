#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Soak real --ensure-daemon handovers in a private home with two QUIC clients."""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time


specification = importlib.util.spec_from_file_location(
    "sync_transport_fixture", Path(__file__).with_name("check-sync-transport.py"))
fixture = importlib.util.module_from_spec(specification)
specification.loader.exec_module(fixture)


class DaemonControls:
    """Owns private supervisor channels, never permission to signal reported PIDs."""

    def __init__(self, path):
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(str(path))
        self.listener.listen(4)
        self.listener.settimeout(10)
        path.chmod(0o600)
        self.channels = []

    def accept(self):
        channel, _ = self.listener.accept()
        channel.settimeout(10)
        self.channels.append(channel)
        record = bytearray()
        while not record.endswith(b"\n"):
            chunk = channel.recv(1)
            if not chunk or len(record) >= 128:
                raise RuntimeError("The fixture daemon did not identify its supervising channel")
            record.extend(chunk)
        match = re.fullmatch(rb"([0-9]+) (host-[0-9a-f]{16}\.sock)\n", record)
        if not match or int(match[1]) <= 1:
            raise RuntimeError("Invalid fixture daemon registration")
        return channel, int(match[1]), match[2].decode()

    def retire(self, channel):
        if channel.recv(1) != b"":
            raise RuntimeError("The predecessor did not close its supervising channel")
        channel.close()
        self.channels.remove(channel)

    def close(self):
        stuck = False
        for channel in self.channels:
            try:
                channel.settimeout(3)
                channel.sendall(b"S")
                if channel.recv(1) != b"":
                    stuck = True
            except (BrokenPipeError, ConnectionResetError):
                pass
            except TimeoutError:
                stuck = True
            finally:
                channel.close()
        self.channels.clear()
        self.listener.close()
        if stuck:
            raise RuntimeError("A private daemon did not close its supervising channel during cleanup")


def resources(daemon):
    channel, pid, _ = daemon
    # A live private channel identifies the daemon while sampling; these PID
    # reads never authorize a signal, wait, or cleanup of another process.
    if select.select([channel], [], [], 0)[0]:
        raise RuntimeError("The successor exited before its resource sample")
    resident = subprocess.check_output(["/bin/ps", "-o", "rss=", "-p", str(pid)], timeout=5)
    if sys.platform == "linux":
        descriptors = len(list(Path(f"/proc/{pid}/fd").iterdir()))
    else:
        listing = subprocess.check_output(["/usr/sbin/lsof", "-nP", "-p", str(pid), "-Ff"], timeout=5)
        descriptors = sum(bool(re.fullmatch(rb"f[0-9]+", line)) for line in listing.splitlines())
    if descriptors <= 0:
        raise RuntimeError("No descriptors were sampled")
    return {"pid": pid, "fd_count": descriptors, "rss_kib": int(resident.strip())}


def install(source, destination, iteration):
    staged = destination.with_suffix(".next")
    shutil.copyfile(source, staged)
    with staged.open("ab") as output:
        output.write(f"\nreplacement-soak-{iteration}\n".encode())
    staged.chmod(0o700)
    staged.replace(destination)


def retire_after_handover(controls, predecessor, successor, directory):
    """Waits for retirement only after the successor confirms both sessions moved."""
    log = directory / Path(successor[2]).with_suffix(".log")
    with log.open("rb") as source:
        data = source.read(65537)
    if len(data) > 65536:
        raise RuntimeError("The successor's handover log exceeded its bound")
    text = data.decode(errors="replace")
    summaries = re.findall(r"^ubiquitty-host: took over ([0-9]+) sessions; ([0-9]+) older hosts kept theirs$",
                           text, re.MULTILINE)
    if len(summaries) != 1:
        raise RuntimeError("The successor did not record exactly one handover summary before listening")
    moved, kept = map(int, summaries[0])
    if kept:
        reasons = re.findall(r"^ubiquitty-host: (process [0-9]+ kept its sessions[^\n]*)$", text, re.MULTILINE)
        reason = "; ".join(reasons) or "no predecessor refusal reason was logged"
        raise RuntimeError(f"Handover refused: successor {successor[1]} took over {moved} sessions; "
                           f"{kept} older hosts kept theirs: {reason}")
    if moved != 2:
        raise RuntimeError(f"The successor took over {moved} sessions instead of both fixture sessions")
    controls.retire(predecessor[0])


def run(arguments):
    binary = fixture.executable(arguments.binary)
    samples = []
    with tempfile.TemporaryDirectory(prefix="ubq-dr-", dir="/tmp") as temporary:
        root = Path(temporary)
        for name in ("host", "config", "cache", "bin"):
            (root / name).mkdir(mode=0o700)
        executable = root / "bin/ubiquitty-host"
        install(binary, executable, 0)
        environment = fixture.isolated_environment(root, root / "peers.sock")
        environment.update(HOME=str(root), SHELL=str(executable), ENV="/dev/null", HISTFILE="/dev/null",
                           UBQ_REPLACEMENT_CONTROL=str(root / "daemons.sock"))
        fixture.run_command([fixture.command_tool("openssl"), "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                             "-days", "1", "-subj", "/CN=localhost", "-keyout", root / "host/quic-key.pem",
                             "-out", root / "host/quic-cert.pem"], environment)
        (root / "host/quic-key.pem").chmod(0o600)
        controls = DaemonControls(root / "daemons.sock")
        peers = fixture.PeerControls(root / "peers.sock", None)
        phase = "initial startup"
        try:
            fixture.run_command([executable, "--ensure-daemon", "--quic-port", "0", "--columns", "80", "--rows", "24"], environment, timeout=35)
            daemon = controls.accept()
            bootstraps = []
            for index in range(2):
                bootstrap = root / f"bootstrap-{index}"
                bootstrap.write_bytes(fixture.run_command([executable, "--quic-bootstrap", "--socket", daemon[2]], environment))
                bootstrap.chmod(0o600)
                bootstraps.append(bootstrap)
            command = [str(binary), "--replacement-soak", *map(str, bootstraps), str(arguments.iterations), str(arguments.seed)]
            with fixture.OwnedProcess(command, env=environment, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as client:
                pending = bytearray()
                total = 0
                replacements = 0
                success = False
                deadline = time.monotonic() + 60 + arguments.iterations * 45
                while time.monotonic() < deadline:
                    if not select.select([client.process.stdout], [], [], 0.2)[0]:
                        continue
                    chunk = os.read(client.process.stdout.fileno(), 8192)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > 4 * 1024 * 1024:
                        raise RuntimeError("Replacement diagnostics exceeded their bound")
                    pending.extend(chunk)
                    while b"\n" in pending:
                        line, _, remainder = pending.partition(b"\n")
                        pending[:] = remainder
                        text = line.decode(errors="replace")
                        print(text, flush=True)
                        if text.startswith("replacement-soak: replace "):
                            fields = dict(field.split("=", 1) for field in text.split()[2:])
                            iteration = int(fields["iteration"])
                            if iteration != replacements + 1:
                                raise RuntimeError("Replacement iteration skipped or repeated")
                            install(binary, executable, iteration)
                            phase = f"iteration {iteration}: ensure successor"
                            fixture.run_command([executable, "--ensure-daemon", "--quic-port", "-1"], environment, timeout=35)
                            phase = f"iteration {iteration}: register successor"
                            successor = controls.accept()
                            if successor[1] == daemon[1] or successor[2] == daemon[2]:
                                raise RuntimeError("The replacement did not get a new process and socket identity")
                            phase = f"iteration {iteration}: handover from predecessor {daemon[1]} to successor {successor[1]} ({successor[2]})"
                            retire_after_handover(controls, daemon, successor, root / "host")
                            daemon = successor
                            replacements += 1
                            client.process.stdin.write((daemon[2] + "\n").encode())
                            client.process.stdin.flush()
                            phase = f"iteration {iteration}: both clients resume"
                        elif text.startswith("replacement-soak: complete "):
                            fields = dict(field.split("=", 1) for field in text.split()[2:])
                            iteration = int(fields["iteration"])
                            if iteration != len(samples) + 1 or iteration != replacements:
                                raise RuntimeError("A completion did not match its replacement")
                            sample = {"iteration": iteration, "seed": int(fields["seed"]),
                                      "resume_ms": int(fields["resume_ms"]), **resources(daemon)}
                            samples.append(sample)
                            print("replacement-resources: " + json.dumps(sample, sort_keys=True), flush=True)
                        elif text == f"replacement-soak: ok iterations={arguments.iterations} seed={arguments.seed}":
                            success = True
                client.wait(5)
                if client.process.returncode or not success or len(samples) != arguments.iterations:
                    raise RuntimeError(f"Replacement soak seed {arguments.seed} failed after {len(samples)} cycles")
            if len(samples) > 10:
                warmed = samples[9:]
                if max(row["fd_count"] for row in warmed) > warmed[0]["fd_count"] + 2:
                    raise RuntimeError("Surviving daemon descriptors grew past the allowance")
                if max(row["rss_kib"] for row in warmed) > warmed[0]["rss_kib"] + arguments.max_rss_growth_kib:
                    raise RuntimeError("Surviving daemon RSS grew past the allowance")
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            if arguments.report:
                logs = arguments.report.with_suffix(".logs")
                logs.mkdir(parents=True, exist_ok=True)
                for log in (root / "host").glob("*.log"):
                    shutil.copyfile(log, logs / log.name)
                arguments.report.write_text(json.dumps({"seed": arguments.seed, "phase": phase, "samples": samples}, indent=2) + "\n")
            raise RuntimeError(f"{phase}: {error}") from error
        finally:
            peers.close()
            controls.close()
    return samples


def self_test():
    import threading
    import unittest
    from unittest import mock

    class ControlsTests(unittest.TestCase):
        def setUp(self):
            self.directory = tempfile.TemporaryDirectory(prefix="ubq-drc-", dir="/tmp")
            self.addCleanup(self.directory.cleanup)
            self.path = Path(self.directory.name) / "control.sock"
            self.controls = DaemonControls(self.path)
            self.addCleanup(self.controls.close)
            self.peers = []
            self.addCleanup(lambda: [peer.close() for peer in self.peers])

        def connect(self, record):
            peer = socket.socket(socket.AF_UNIX)
            self.peers.append(peer)
            peer.connect(str(self.path))
            for byte in record:
                peer.sendall(bytes([byte]))
            return peer

        def test_fragmented_registration_and_retirement(self):
            peer = self.connect(b"42 host-0123456789abcdef.sock\n")
            channel, pid, leaf = self.controls.accept()
            self.assertEqual((pid, leaf), (42, "host-0123456789abcdef.sock"))
            peer.close()
            self.controls.retire(channel)
            self.assertEqual(self.controls.channels, [])

        def test_invalid_and_unbounded_registrations(self):
            for record in (b"1 host-0123456789abcdef.sock\n", b"42 ../host.sock\n", b"9" * 129 + b"\n", b"42"):
                with self.subTest(record=record[:40]):
                    peer = self.connect(record)
                    peer.shutdown(socket.SHUT_WR)
                    with self.assertRaises(RuntimeError):
                        self.controls.accept()
                    peer.close()

        def cleanup_exchange(self, reply):
            received = []
            workers = []

            def serve(peer, response):
                with peer:
                    received.append(peer.recv(1))
                    if response:
                        peer.sendall(response)

            for response in (reply, b""):
                peer = self.connect(b"42 host-0123456789abcdef.sock\n")
                self.controls.accept()
                worker = threading.Thread(target=serve, args=(peer, response), daemon=True)
                workers.append(worker)
                worker.start()
            try:
                if reply:
                    with self.assertRaisesRegex(RuntimeError, "during cleanup"):
                        self.controls.close()
                else:
                    self.controls.close()
            finally:
                for worker in workers:
                    worker.join(2)
            self.assertFalse(any(worker.is_alive() for worker in workers))
            self.assertEqual(received, [b"S", b"S"])
            self.assertEqual(self.controls.channels, [])

        def test_cleanup_requests_owned_exit(self):
            self.cleanup_exchange(b"")

        def test_cleanup_refusal_does_not_skip_other_peers(self):
            self.cleanup_exchange(b"?")

        def test_closed_channel_cannot_sample_a_reused_pid(self):
            channel, peer = socket.socketpair()
            peer.close()
            with channel, mock.patch.object(subprocess, "check_output") as query:
                with self.assertRaisesRegex(RuntimeError, "exited before"):
                    resources((channel, os.getpid(), "host-0123456789abcdef.sock"))
                query.assert_not_called()

        def test_replacement_keeps_the_source_and_changes_installed_bytes(self):
            source = Path(self.directory.name) / "source"
            destination = Path(self.directory.name) / "installed"
            source.write_bytes(b"fixture executable")
            install(source, destination, 0)
            first = destination.read_bytes()
            install(source, destination, 1)
            self.assertNotEqual(first, destination.read_bytes())
            self.assertEqual(source.read_bytes(), b"fixture executable")
            self.assertEqual(destination.stat().st_mode & 0o777, 0o700)
            self.assertFalse(destination.with_suffix(".next").exists())

        def handover_fixture(self, text):
            successor = (None, 43, "host-0123456789abcdef.sock")
            directory = Path(self.directory.name)
            (directory / "host-0123456789abcdef.log").write_text(text)
            return mock.Mock(spec=["retire"]), (object(), 42, "host-fedcba9876543210.sock"), successor, directory

        def test_refused_handover_never_waits_for_retirement(self):
            arguments = self.handover_fixture(
                "ubiquitty-host: process 42 kept its sessions (a client has not yet been sent all it is owed)\n"
                "ubiquitty-host: took over 0 sessions; 1 older hosts kept theirs\n")
            arguments[0].retire.side_effect = AssertionError("waited for a predecessor that kept serving")
            with self.assertRaisesRegex(RuntimeError, r"process 42 kept its sessions \(a client has not yet been sent all it is owed\)"):
                retire_after_handover(*arguments)
            arguments[0].retire.assert_not_called()

        def test_accepted_handover_waits_for_predecessor(self):
            arguments = self.handover_fixture("ubiquitty-host: took over 2 sessions; 0 older hosts kept theirs\n")
            retire_after_handover(*arguments)
            arguments[0].retire.assert_called_once_with(arguments[1][0])

        def test_unproven_handover_never_waits_for_retirement(self):
            for text in ("", "ubiquitty-host: took over 1 sessions; 0 older hosts kept theirs\n",
                         "ubiquitty-host: took over 2 sessions; 0 older hosts kept theirs\n" * 2):
                with self.subTest(text=text):
                    arguments = self.handover_fixture(text)
                    arguments[0].retire.side_effect = AssertionError("waited without an accepted handover")
                    with self.assertRaises(RuntimeError):
                        retire_after_handover(*arguments)
                    arguments[0].retire.assert_not_called()

        def test_handover_log_is_bounded(self):
            arguments = self.handover_fixture("x" * 65537)
            with self.assertRaisesRegex(RuntimeError, "exceeded its bound"):
                retire_after_handover(*arguments)
            arguments[0].retire.assert_not_called()

    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ControlsTests))
    return 0 if result.wasSuccessful() else 1


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--iterations", type=int, default=50)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--max-rss-growth-kib", type=int, default=16384)
    parser.add_argument("--report", type=Path)
    arguments = parser.parse_args()
    if not 1 <= arguments.iterations <= 1000 or not 0 <= arguments.seed < 2**32 or arguments.max_rss_growth_kib < 0:
        parser.error("iterations must be 1..1000, seed a uint32, and the RSS allowance nonnegative")
    try:
        samples = run(arguments)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"replacement-soak: {error}", file=sys.stderr)
        return 1
    if arguments.report:
        arguments.report.write_text(json.dumps({"seed": arguments.seed, "samples": samples}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
