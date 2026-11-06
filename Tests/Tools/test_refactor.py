#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Memory-only clangd lifecycle tests: no child, pipe, index or filesystem edit."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("refactor_under_test", ROOT / "Scripts/refactor.py")
refactor = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(refactor)


class MemoryEvent:
    def __init__(self):
        self.ready = False
        self.waits = []

    def set(self):
        self.ready = True

    def wait(self, timeout):
        self.waits.append(timeout)
        return self.ready


class MemoryPipe:
    def __init__(self, name, events, data=b""):
        self.name, self.events = name, events
        self.data = io.BytesIO(data)
        self.closed = False

    def fileno(self):
        return 137

    def read(self, size):
        # Raw pipe reads may end before a complete LSP body has arrived.
        return self.data.read(min(size, 2))

    def close(self):
        self.closed = True
        self.events.append(self.name + ".close")


class MemoryReader:
    def __init__(self, events):
        self.events = events

    def start(self):
        self.events.append("reader.start")

    def join(self, timeout):
        self.events.append(("reader.join", timeout))

    def is_alive(self):
        return False


class MemoryProcess:
    """Only stdio/poll/wait exist; no real subprocess or signalling fallback."""

    def __init__(self, events, edit):
        self.pid = 424242
        self.returncode = None
        self.events = events
        self.stdin, self.stdout = MemoryPipe("stdin", events), MemoryPipe("stdout", events)
        self.client = None
        self.buffer = bytearray()
        self.messages = []
        self.edit = edit
        self.errors = {}
        self.unanswered = set()
        self.broken = set()
        self.remains_alive = False
        self.write_chunk_bytes = None

    def write(self, descriptor, packet):
        if descriptor != 137:
            raise AssertionError("unexpected mock descriptor")
        count = min(len(packet), self.write_chunk_bytes or len(packet))
        self.buffer.extend(packet[:count])
        while b"\r\n\r\n" in self.buffer:
            header, body = bytes(self.buffer).split(b"\r\n\r\n", 1)
            length = int(header.split(b":", 1)[1])
            if len(body) < length:
                break
            del self.buffer[:len(header) + 4 + length]
            message = json.loads(body[:length])
            method = message["method"]
            self.messages.append(message)
            self.events.append(method)
            if method in self.broken:
                raise BrokenPipeError("mock closed stdin")
            if "id" not in message or method in self.unanswered:
                continue
            response = {"id": message["id"]}
            if method in self.errors:
                response["error"] = {"message": self.errors[method]}
            else:
                response["result"] = self.edit if method == "textDocument/rename" else None
            pending = self.client._pending[message["id"]]
            pending[0] = response
            pending[1].set()
        return count

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        self.events.append(("process.wait", timeout))
        if self.remains_alive:
            raise subprocess.TimeoutExpired("memory-clangd", timeout)
        self.returncode = 0
        return self.returncode


class RefactorLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.context = contextlib.ExitStack()
        self.addCleanup(self.context.close)
        self.events = []
        self.edit = {"changes": {"file:///memory/Source.cpp": [{
            "range": {"start": {"line": 0, "character": 7}, "end": {"line": 0, "character": 10}},
            "newText": "NewName",
        }]}}
        self.process = MemoryProcess(self.events, self.edit)
        self.reader = MemoryReader(self.events)
        self.stdout, self.stderr = io.StringIO(), io.StringIO()
        self.context.enter_context(contextlib.redirect_stdout(self.stdout))
        self.context.enter_context(contextlib.redirect_stderr(self.stderr))
        self.popen = self.context.enter_context(mock.patch.object(refactor.subprocess, "Popen", return_value=self.process))
        self.context.enter_context(mock.patch.object(refactor.subprocess, "run", side_effect=AssertionError("no subprocess.run")))
        self.thread = self.context.enter_context(mock.patch.object(refactor.threading, "Thread", return_value=self.reader))
        self.context.enter_context(mock.patch.object(refactor.threading, "Event", side_effect=MemoryEvent))
        self.nonblocking = self.context.enter_context(mock.patch.object(refactor.os, "set_blocking"))
        self.context.enter_context(mock.patch.object(refactor.os, "write", side_effect=self.process.write))
        self.select = self.context.enter_context(mock.patch.object(refactor.select, "select", return_value=([], [137], [])))
        self.context.enter_context(mock.patch.object(refactor.time, "monotonic", return_value=1.0))
        self.context.enter_context(mock.patch.object(refactor.os.path, "isfile", return_value=True))
        self.open = self.context.enter_context(mock.patch.object(refactor, "open", mock.mock_open(read_data="struct Old {};\n"), create=True))
        self.database = self.context.enter_context(mock.patch.object(refactor, "ensure_worktree_db", return_value="/exact/source-db"))
        self.resolve = self.context.enter_context(mock.patch.object(refactor, "resolve_clangd", return_value="/owned/clangd"))
        self.apply = self.context.enter_context(mock.patch.object(refactor, "apply_workspace_edit"))
        actual_clangd = refactor.Clangd

        def make_client(path, database):
            self.client = actual_clangd(path, database)
            self.process.client = self.client
            return self.client

        self.context.enter_context(mock.patch.object(refactor, "Clangd", side_effect=make_client))
        self.arguments = SimpleNamespace(file="/memory/Source.cpp", symbol=None, line=0, col=7,
                                         new_name="NewName", db_dir="/requested/owned-db", apply=False)

    def assert_orderly_exit(self):
        self.assertEqual(self.events[-6:], ["shutdown", "exit", "stdin.close",
                                           ("process.wait", refactor.EXIT_TIMEOUT_SECONDS),
                                           ("reader.join", 1), "stdout.close"])
        self.assertEqual([message["method"] for message in self.process.messages][-2:], ["shutdown", "exit"])
        self.assertTrue(self.process.stdin.closed)
        self.assertTrue(self.process.stdout.closed)
        self.assertEqual(self.process.returncode, 0)
        self.assertEqual(self.client._pending, {})

    def test_dry_run_keeps_exact_edit_and_database_with_one_worker(self):
        self.process.write_chunk_bytes = 7
        refactor.cmd_rename(self.arguments)
        arguments, options = self.popen.call_args
        self.assertEqual(arguments[0], ["/owned/clangd", "--compile-commands-dir=/exact/source-db",
                                        "--background-index", "-j=1", "--limit-results=0", "--log=error"])
        self.assertEqual(options["bufsize"], 0)
        self.nonblocking.assert_called_once_with(137, False)
        self.database.assert_called_once_with(refactor.ROOT, "/requested/owned-db")
        self.resolve.assert_called_once_with("/exact/source-db")
        self.assertEqual(json.loads(self.stdout.getvalue()), self.edit)
        self.assertIn("dry run", self.stderr.getvalue())
        self.apply.assert_not_called()
        self.assert_orderly_exit()
        events = list(self.events)
        self.assertTrue(self.client.shutdown())
        self.assertEqual(self.events, events)

    def test_apply_uses_workspace_edit_only_after_orderly_exit(self):
        self.arguments.apply = True

        def apply(edit):
            self.assert_orderly_exit()
            self.assertIs(edit, self.edit)
            return {"/memory/Source.cpp": 1}

        self.apply.side_effect = apply
        refactor.cmd_rename(self.arguments)
        self.apply.assert_called_once_with(self.edit)
        self.assertIn("renamed", self.stdout.getvalue())

    def test_declined_rename_removes_pending_and_exits_orderly(self):
        self.process.errors["textDocument/rename"] = "cursor is not renameable"
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 3)
        self.assert_orderly_exit()
        self.apply.assert_not_called()

    def test_timed_out_rename_still_shuts_down_and_reaps(self):
        self.process.unanswered.add("textDocument/rename")
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("timed out on textDocument/rename", self.stderr.getvalue())
        self.assertTrue(all(0 < call.args[3] <= refactor.REQUEST_TIMEOUT_SECONDS for call in self.select.call_args_list))
        self.assert_orderly_exit()
        self.apply.assert_not_called()

    def test_initialize_failure_also_uses_the_same_cleanup(self):
        self.process.errors["initialize"] = "mock initialization refused"
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 3)
        self.assertNotIn("textDocument/rename", self.events)
        self.assert_orderly_exit()

    def test_open_write_failure_still_sends_shutdown_and_exit(self):
        self.process.broken.add("textDocument/didOpen")
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assert_orderly_exit()

    def test_write_timeout_is_bounded_and_cleans_pending_before_shutdown(self):
        self.select.side_effect = [([], [137], [])] * 3 + [([], [], [])] + [([], [137], [])] * 2
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("timed out writing textDocument/rename", self.stderr.getvalue())
        self.assert_orderly_exit()

    def test_shutdown_timeout_still_sends_exit_and_refuses_to_apply(self):
        self.arguments.apply = True
        self.process.unanswered.add("shutdown")
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("timed out on shutdown", self.stderr.getvalue())
        self.assert_orderly_exit()
        self.apply.assert_not_called()

    def test_child_that_stays_alive_is_reported_without_force_exit(self):
        self.arguments.apply = True
        self.process.remains_alive = True
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 2)
        self.assertIn("clangd PID 424242 is still running", self.stderr.getvalue())
        self.assertIn("left alive, no signal sent", self.stderr.getvalue())
        self.assertEqual(self.events[-4:], ["shutdown", "exit", "stdin.close", ("process.wait", refactor.EXIT_TIMEOUT_SECONDS)])
        self.assertIsNone(self.process.poll())
        self.assertFalse(self.process.stdout.closed)
        self.assertEqual(self.client._pending, {})
        self.apply.assert_not_called()

    def test_empty_workspace_edit_is_declined_only_after_cleanup(self):
        self.process.edit = {}
        with self.assertRaises(SystemExit) as failure:
            refactor.cmd_rename(self.arguments)
        self.assertEqual(failure.exception.code, 3)
        self.assert_orderly_exit()
        self.apply.assert_not_called()

    def test_reader_handles_partial_raw_bodies_and_wakes_unanswered_requests(self):
        self.client = refactor.Clangd("/owned/clangd", "/exact/source-db")
        response = json.dumps({"id": 0, "result": self.edit}).encode()
        self.process.stdout = MemoryPipe("stdout", self.events,
            f"Content-Length: {len(response)}\r\n\r\n".encode() + response)
        answered, abandoned = [None, MemoryEvent(), None], [None, MemoryEvent(), None]
        self.client._pending = {0: answered, 1: abandoned}
        self.client._read_loop()
        self.assertEqual(answered[0]["result"], self.edit)
        self.assertTrue(answered[1].ready)
        self.assertIsNone(answered[2])
        self.assertTrue(abandoned[1].ready)
        self.assertIn("closed its response stream", abandoned[2])



class DatabaseLookupTests(unittest.TestCase):
    """Where the rename finds a compile database, asked of patched lookups only."""

    def test_a_worktree_without_a_build_reads_its_main_worktrees_build_root(self):
        derived = {"/work/tree": "/derived/tree", "/src/main": "/derived/main"}
        with mock.patch.object(refactor.build_root, "build_root", side_effect=derived.__getitem__):
            self.assertEqual(refactor.build_databases("/work/tree"),
                             ("/work/tree/compile_commands.json", "/derived/tree/macos/compile_commands.json"))
            owners = {"/derived/main/macos/compile_commands.json": "/src/main"}
            with mock.patch.object(refactor, "describes",
                                   side_effect=lambda database, checkout: owners.get(database) == checkout), \
                    mock.patch.object(refactor, "git", return_value="worktree /src/main\nHEAD 0123\n"), \
                    mock.patch.object(refactor.os.path, "realpath", side_effect=lambda path: path):
                self.assertEqual(refactor.canonical_db("/work/tree"), ("/derived/main/macos", "/src/main"))
                owners["/work/tree/compile_commands.json"] = "/work/tree"
                self.assertEqual(refactor.canonical_db("/work/tree"), ("/work/tree", "/work/tree"))


if __name__ == "__main__":
    unittest.main()
