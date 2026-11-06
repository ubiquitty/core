#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Runs Scripts/run-tests.sh against a stanza that leaves something behind.

The `run_hygiene` stanza, given UBQ_HYGIENE_PLANT, plants what a leaking test
leaves: a process that detached into a session of its own, as a daemon started
with --ensure-daemon does, or a file in the state directory of whoever runs the
suite. The directory is a stand-in here (UBQ_HYGIENE_REAL_STATE_DIR); nothing
touches the real one. The claims: a run that leaves either fails and says what
it left; the process is ended whether the run passed, failed, was interrupted
or was killed outright; a stanza's own state directory is the run's scratch
one; and a clean run passes.

The state directory is also the person's own, and their daemons write into it
while a run is on. A file of a daemon outside the run, or one named for the
installed host, is a note and not a failure; a file of a daemon the run
started still fails it.
"""

import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "Scripts" / "run-tests.sh"
SLEEP = shutil.which("sleep") or "/bin/sleep"


def is_alive(pid):
    """Whether `pid` is running: an ended process nobody has reaped is not.

    A process the run detached is an orphan, and what reaps it is the machine's
    first process. A container started without one keeps it as a zombie, which
    still answers a signal, so the signal alone reads an ended process as left
    running. The tool under test asks the same way.
    """
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    state = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True, check=False).stdout.strip()
    return bool(state) and not state.startswith("Z")


def wait_gone(pid, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline and is_alive(pid):
        time.sleep(0.1)
    return not is_alive(pid)


class RunHygiene(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(dir="/tmp", prefix="ubq-hygiene-")
        self.real = Path(self.scratch.name) / "real-state"
        self.real.mkdir()
        (self.real / "tokens").write_text("")
        self.planted = []
        self.daemons = []

    def tearDown(self):
        for pid in self.planted:
            if is_alive(pid):
                os.kill(pid, signal.SIGKILL)
        for daemon in self.daemons:
            daemon.kill()
            daemon.wait()
        self.scratch.cleanup()

    def environment(self, plant):
        environment = dict(os.environ)
        environment.update({
            "UBQ_HYGIENE_PLANT": plant,
            "UBQ_HYGIENE_REAL_STATE_DIR": str(self.real),
            "NO_CTEST": "1",
            "UBQ_OUT": str(Path(self.scratch.name) / "out"),
        })
        return environment

    def start(self, plant):
        return subprocess.Popen([str(DRIVER), "run_hygiene"], env=self.environment(plant), stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, start_new_session=True)

    def run_driver(self, plant):
        process = self.start(plant)
        output, _ = process.communicate(timeout=120)
        return process.returncode, output

    def planted_process(self, output):
        match = re.search(r"planted process (\d+)", output)
        self.assertIsNotNone(match, output)
        self.planted.append(int(match.group(1)))
        return self.planted[-1]

    def daemon_outside_the_run(self, stem):
        """A ubiquitty-host of the person's own, serving `stem` in the stand-in
        directory since before the run, with the pid file it writes.

        A sleep under the host's name: what is asked of it is its name and
        that it carries no mark of the run, not anything a daemon does."""
        daemon = subprocess.Popen([str(Path(self.scratch.name) / "ubiquitty-host"), "600"], executable=SLEEP,
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  start_new_session=True)
        self.daemons.append(daemon)
        (self.real / f"{stem}.pid").write_text(f"{daemon.pid}\n")
        return daemon.pid

    def test_a_clean_run_passes(self):
        status, output = self.run_driver("none")
        self.assertEqual(status, 0, output)

    def test_a_process_left_running_fails_the_run_and_is_ended(self):
        status, output = self.run_driver("process")
        pid = self.planted_process(output)
        self.assertNotEqual(status, 0, output)
        self.assertRegex(output, rf"left running[\s\S]*\b{pid}\b")
        # By the time the run returns, not some time after: the next thing a
        # caller does may be to rebuild the binary the process was running.
        self.assertFalse(is_alive(pid), "the process a run left was still alive when the run returned")

    def test_a_file_left_in_the_real_state_directory_fails_the_run(self):
        status, output = self.run_driver("state")
        self.assertNotEqual(status, 0, output)
        self.assertRegex(output, r"state directory[\s\S]*host-planted\.pid")

    def test_a_file_of_a_daemon_outside_the_run_is_a_note(self):
        # The case that failed unrelated stanzas: a daemon a session over SSH
        # started wrote its layout record while they ran. The stem is drawn
        # fresh: the judge finds a daemon by its socket's leaf in any command
        # line, and a fixed stem can be one a real daemon on this machine
        # serves, which the judge would then name instead of this one.
        stem = "host-" + os.urandom(8).hex()
        pid = self.daemon_outside_the_run(stem)
        status, output = self.run_driver(f"entry:{stem}.sock.layouts")
        self.assertEqual(status, 0, output)
        self.assertRegex(output, rf"note — the state directory[^\n]*outside this run[\s\S]*{stem}\.sock\.layouts: its daemon, pid {pid}\b")
        self.assertNotRegex(output, r"FAILED — the state directory")

    def test_a_file_named_for_the_installed_host_is_a_note(self):
        # No daemon is running by then; the stem alone says whose it is.
        host = b"a stand-in for the installed host\n"
        (self.real / "ubiquitty-host").write_bytes(host)
        stem = "host-" + hashlib.sha256(host).hexdigest()[:16]
        status, output = self.run_driver(f"entry:{stem}.sock.layouts")
        self.assertEqual(status, 0, output)
        self.assertRegex(output, rf"{stem}\.sock\.layouts: it is named for \S*ubiquitty-host, the installed host")
        self.assertNotRegex(output, r"FAILED — the state directory")

    def test_a_file_of_the_runs_own_daemon_fails_the_run(self):
        # The daemon is the run's: its pid file names a process carrying the
        # run's mark. That the stem looks like any daemon's excuses nothing.
        stem = "host-0123456789abcdef"
        status, output = self.run_driver(f"daemon-entry:{stem}.sock.layouts")
        self.planted_process(output)
        self.assertNotEqual(status, 0, output)
        self.assertRegex(output, rf"FAILED — the state directory[^\n]*{stem}\.sock\.layouts")
        self.assertNotRegex(output, rf"  {stem}\.sock\.layouts: ")

    def test_a_stanza_is_given_the_runs_own_state_directory(self):
        status, output = self.run_driver("home")
        self.assertEqual(status, 0, output)
        match = re.search(r"stanza state directory (\S+)", output)
        self.assertIsNotNone(match, output)
        self.assertTrue(match.group(1).startswith("/tmp/ubq-run."), match.group(1))
        self.assertFalse(Path(match.group(1)).exists(), "the run's scratch state directory outlived a passing run")

    def interrupted_run_leaves_nothing(self, sent):
        process = self.start("process-then-hang")
        line = ""
        while "planted process" not in line:
            line = process.stdout.readline()
            self.assertTrue(line, "the run ended before planting")
        pid = self.planted_process(line)
        os.killpg(process.pid, sent)
        status = process.wait(timeout=30)
        process.stdout.close()
        if sent != signal.SIGKILL:
            self.assertEqual(status, 128 + int(sent), "the run answered the signal itself and said so in its status")
            self.assertFalse(is_alive(pid), "a detached process was still alive when the interrupted run returned")
        self.assertTrue(wait_gone(pid, 10), f"a detached process outlived a run ended by signal {int(sent)}")

    def test_an_interrupted_run_ends_what_it_started(self):
        self.interrupted_run_leaves_nothing(signal.SIGTERM)

    def test_a_run_killed_outright_still_ends_what_it_started(self):
        # What a job's own limit does: no trap of the run's gets to answer.
        self.interrupted_run_leaves_nothing(signal.SIGKILL)


if __name__ == "__main__":
    unittest.main()
