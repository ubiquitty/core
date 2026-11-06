#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""refactor.py — semantic rename driven by clangd.

`clang-rename` is no longer shipped by Apple's toolchain or brew LLVM, so the
sanctioned semantic rename runs through clangd's LSP `textDocument/rename`, which
resolves the symbol by its Unified Symbol Resolution across the AST and rewrites
every reference — never a substring, comment, or unrelated token (the failure
mode `sed` has, which Docs/coding-conventions.md §2.3 rules out for renames).

Engine choice: clangd from the same toolchain that produced the compile database
parses our Objective-C++/Metal TUs cleanly; a version-mismatched standalone LLVM
tool drifts on the SDK/resource-dir headers. This picks the clangd next to the
compiler in compile_commands.json, falling back to Apple's /usr/bin/clangd.

    Scripts/refactor.py rename --file <path> --line <L> --col <C> --new-name <n>
    Scripts/refactor.py rename --file <path> --symbol <old> --new-name <n>
        (--symbol finds the first `old` occurrence in the file as the cursor)

--apply writes the edits; without it the WorkspaceEdit is printed as a dry run.
Cross-file references resolve through clangd's background index (give it a moment
on a cold tree). Exit 0 on a clean rename, 2 on a usage/environment error, 3 when
clangd declines the rename (e.g. cursor not on a renameable symbol).

"""

import argparse
import json
import math
import os
import re
import select
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import build_root

ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
REQUEST_TIMEOUT_SECONDS = 120
SHUTDOWN_TIMEOUT_SECONDS = 10
EXIT_TIMEOUT_SECONDS = 10


def die(message, code=2):
    sys.stderr.write(f"refactor: {message}\n")
    sys.exit(code)


def resolve_clangd(db_dir):
    """Prefer the clangd whose version matches the compile DB's compiler.

    compile_commands.json records the exact `clang` binary; clangd sits next to
    it in the same toolchain. Apple's /usr/bin/clangd matches an Apple-clang DB.
    """
    database = os.path.join(db_dir, "compile_commands.json")
    if os.path.isfile(database):
        try:
            entries = json.load(open(database, encoding="utf-8"))
            compiler = (entries[0].get("arguments") or entries[0]["command"].split())[0]
            sibling = os.path.join(os.path.dirname(compiler), "clangd")
            if os.path.isfile(sibling):
                return sibling
        except (OSError, ValueError, KeyError, IndexError):
            pass
    for candidate in ("/usr/bin/clangd", "clangd"):
        found = candidate if os.path.isabs(candidate) else None
        if found and os.path.isfile(found):
            return found
    from shutil import which
    return which("clangd") or die("no clangd on PATH")


def git(root, *args):
    return subprocess.run(["git", "-C", root, *args],
                          capture_output=True, text=True).stdout.strip()


def describes(database, checkout):
    """True when the database's first entry compiles a file of this checkout."""
    try:
        first = os.path.realpath(json.load(open(database, encoding="utf-8"))[0]["file"])
    except (OSError, ValueError, KeyError, IndexError, TypeError):
        return False
    return first.startswith(checkout + os.sep)


def build_databases(checkout):
    """The compile databases a build of this checkout leaves, in the order read.

    The build writes under the build root, outside the checkout. The link the
    macOS and Linux app builds make in the checkout's root names the one clangd
    reads; the default macOS build's own follows it, for a checkout
    whose name was already taken.
    """
    return (os.path.join(checkout, "compile_commands.json"),
            os.path.join(build_root.build_root(checkout), "macos", "compile_commands.json"))


def canonical_db(root):
    """(db_dir, db_root) for the compile DB whose file paths we trust: this
    checkout's own build DB if it matches, else the main worktree's."""
    for database in build_databases(root):
        if describes(database, root):
            return os.path.dirname(database), root
    for line in git(root, "worktree", "list", "--porcelain").splitlines():
        if line.startswith("worktree "):
            main_root = os.path.realpath(line[len("worktree "):])
            for database in build_databases(main_root):
                if describes(database, main_root):
                    return os.path.dirname(database), main_root
            break
    return None, None


def ensure_worktree_db(root, requested_db_dir):
    """clangd needs a compile DB whose file paths match this checkout. In a git
    worktree the shared DB points at the main checkout, so remap it: source
    paths -> this worktree; build-tree paths (generated headers + objects, which
    exist only in the real build and are identical for a rename) stay put. The
    remapped DB is written under this worktree's build root, where only the
    clangd this tool starts reads it: an editor's clangd looks beside the
    source, at the link a build of this worktree makes. No rebuild needed."""
    root = os.path.realpath(root)
    if requested_db_dir is not None and describes(
            os.path.join(requested_db_dir, "compile_commands.json"), root):
        return requested_db_dir
    db_dir, db_root = canonical_db(root)
    if not db_dir:
        die("no compile_commands.json for this checkout or its main worktree; "
            "build once (Scripts/build-macos.sh) or pass --db-dir")
    if db_root == root:
        return db_dir
    build_prefix = os.path.join(db_root, "build") + os.sep

    def remap(token):  # keep build-tree tokens at the real build; remap the rest
        return token if build_prefix in token else token.replace(db_root + os.sep, root + os.sep)

    entries = json.load(open(os.path.join(db_dir, "compile_commands.json")))
    for entry in entries:
        if "file" in entry:
            entry["file"] = remap(entry["file"])
        if "arguments" in entry:
            entry["arguments"] = [remap(a) for a in entry["arguments"]]
        if "command" in entry:
            entry["command"] = " ".join(remap(t) for t in entry["command"].split())
    target = os.path.join(build_root.build_root(root), "refactor-compile-database")
    os.makedirs(target, exist_ok=True)
    with open(os.path.join(target, "compile_commands.json"), "w", encoding="utf-8") as handle:
        json.dump(entries, handle, indent=1)
    sys.stderr.write(f"refactor: remapped compile DB {db_root} -> {root} "
                     f"({len(entries)} entries) into {target}\n")
    return target


class Clangd:
    """A minimal LSP client: Content-Length framing over clangd's stdio."""

    def __init__(self, clangd_path, db_dir):
        self._proc = subprocess.Popen(
            [clangd_path, f"--compile-commands-dir={db_dir}",
             "--background-index", "-j=1", "--limit-results=0", "--log=error"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            cwd=ROOT, bufsize=0,
        )
        self._next_id = 0
        self._pending = {}
        self._lock = threading.Lock()
        self._reader_failure = None
        self._reader = None
        self._reader_started = False
        self._stdin_is_nonblocking = False
        self._shutdown_started = False
        self._shutdown_complete = False
        try:
            # A stalled server must not block a request before its response
            # deadline even starts. Unbuffered close never flushes a backlog.
            os.set_blocking(self._proc.stdin.fileno(), False)
            self._stdin_is_nonblocking = True
            self._reader = threading.Thread(target=self._read_loop, daemon=True)
            self._reader.start()
            self._reader_started = True
        except BaseException:
            self.shutdown()
            raise

    def _fail_pending(self, failure):
        with self._lock:
            self._reader_failure = failure
            for pending in self._pending.values():
                if pending[0] is None:
                    pending[2] = failure
                    pending[1].set()

    def _read_loop(self):
        stream = self._proc.stdout
        try:
            while True:
                header = b""
                while b"\r\n\r\n" not in header:
                    chunk = stream.read(1)
                    if not chunk:
                        return
                    header += chunk
                length = int(re.search(rb"Content-Length:\s*(\d+)", header).group(1))
                body = bytearray()
                while len(body) < length:
                    chunk = stream.read(length - len(body))
                    if not chunk:
                        return
                    body.extend(chunk)
                message = json.loads(body)
                if "id" in message and ("result" in message or "error" in message):
                    with self._lock:
                        pending = self._pending.get(message["id"])
                        if pending:
                            pending[0] = message
                            pending[1].set()
        except (OSError, ValueError, AttributeError, TypeError) as error:
            self._fail_pending(f"invalid clangd response: {error}")
        finally:
            self._fail_pending(self._reader_failure or "clangd closed its response stream")

    def _send(self, payload, deadline):
        data = json.dumps(payload).encode("utf-8")
        packet = memoryview(f"Content-Length: {len(data)}\r\n\r\n".encode("ascii") + data)
        try:
            descriptor = self._proc.stdin.fileno()
            while packet:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not select.select([], [descriptor], [], remaining)[1]:
                    die(f"clangd timed out writing {payload['method']}")
                try:
                    written = os.write(descriptor, packet)
                except (BlockingIOError, InterruptedError):
                    continue
                if not written:
                    die(f"clangd closed its request stream during {payload['method']}")
                packet = packet[written:]
        except (OSError, ValueError) as error:
            die(f"clangd write failed on {payload['method']}: {error}")

    def request(self, method, params, timeout=REQUEST_TIMEOUT_SECONDS):
        if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 < timeout <= REQUEST_TIMEOUT_SECONDS:
            die("clangd request timeout must be finite and within the request bound")
        deadline = time.monotonic() + timeout
        with self._lock:
            request_id = self._next_id
            self._next_id += 1
            event = [None, threading.Event(), self._reader_failure]
            self._pending[request_id] = event
            if event[2]:
                event[1].set()
        try:
            self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}, deadline)
            remaining = max(0, deadline - time.monotonic())
            if not event[1].wait(remaining):
                die(f"clangd timed out on {method}", 2)
            if event[2]:
                die(f"clangd failed on {method}: {event[2]}", 2)
            message = event[0]
            if "error" in message:
                die(f"clangd error on {method}: {message['error'].get('message')}", 3)
            return message["result"]
        finally:
            with self._lock:
                self._pending.pop(request_id, None)

    def notify(self, method, params, timeout=REQUEST_TIMEOUT_SECONDS):
        if not isinstance(timeout, (int, float)) or not math.isfinite(timeout) or not 0 < timeout <= REQUEST_TIMEOUT_SECONDS:
            die("clangd notification timeout must be finite and within the request bound")
        self._send({"jsonrpc": "2.0", "method": method, "params": params}, time.monotonic() + timeout)

    def shutdown(self):
        """Attempt LSP shutdown/exit once, then reap without ever signalling.

        A child that ignores exit remains observable under its exact PID. Its
        response pipe stays open; this method never substitutes forced exit.
        """
        if self._shutdown_started:
            return self._shutdown_complete
        self._shutdown_started = True
        orderly = True
        if self._stdin_is_nonblocking and self._proc.poll() is None:
            try:
                self.request("shutdown", None, timeout=SHUTDOWN_TIMEOUT_SECONDS)
            except BaseException as error:
                if not isinstance(error, SystemExit):
                    sys.stderr.write(f"refactor: clangd shutdown request failed: {error}\n")
                orderly = False
            try:
                self.notify("exit", None, timeout=SHUTDOWN_TIMEOUT_SECONDS)
            except BaseException as error:
                if not isinstance(error, SystemExit):
                    sys.stderr.write(f"refactor: clangd exit notification failed: {error}\n")
                orderly = False
        elif not self._stdin_is_nonblocking:
            orderly = False
        try:
            self._proc.stdin.close()
        except (OSError, ValueError) as error:
            sys.stderr.write(f"refactor: clangd stdin close failed: {error}\n")
            orderly = False
        try:
            status = self._proc.wait(timeout=EXIT_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            status = self._proc.poll()
            if status is None:
                sys.stderr.write(f"refactor: clangd PID {self._proc.pid} is still running after orderly exit; left alive, no signal sent\n")
                return False
        if self._reader_started:
            self._reader.join(timeout=1)
        if not self._reader_started or not self._reader.is_alive():
            self._proc.stdout.close()
        else:
            sys.stderr.write(f"refactor: clangd response reader did not finish after PID {self._proc.pid} exited\n")
            orderly = False
        if status:
            sys.stderr.write(f"refactor: clangd exited with status {status}\n")
            orderly = False
        self._shutdown_complete = orderly
        return orderly


def uri(path):
    return "file://" + os.path.realpath(path)


def path_of(file_uri):
    return file_uri[len("file://"):] if file_uri.startswith("file://") else file_uri


def find_symbol_position(path, symbol):
    """First occurrence of `symbol` as a whole word → (line, col) 0-based."""
    pattern = re.compile(rf"\b{re.escape(symbol)}\b")
    with open(path, encoding="utf-8") as handle:
        for line_index, text in enumerate(handle):
            match = pattern.search(text)
            if match:
                return line_index, match.start()
    die(f"symbol {symbol!r} not found in {path}", 2)


def apply_workspace_edit(edit):
    """Apply an LSP WorkspaceEdit; edits per file are applied bottom-up so earlier
    ranges keep their offsets. Returns {path: edit_count}."""
    changes = edit.get("changes")
    if changes is None:
        changes = {doc["textDocument"]["uri"]: doc["edits"]
                   for doc in edit.get("documentChanges", [])}
    touched = {}
    for file_uri, edits in changes.items():
        path = path_of(file_uri)
        lines = open(path, encoding="utf-8").read().split("\n")
        ordered = sorted(
            edits,
            key=lambda e: (e["range"]["start"]["line"], e["range"]["start"]["character"]),
            reverse=True,
        )
        for change in ordered:
            start, end = change["range"]["start"], change["range"]["end"]
            if start["line"] == end["line"]:
                row = lines[start["line"]]
                lines[start["line"]] = row[:start["character"]] + change["newText"] + row[end["character"]:]
            else:
                head = lines[start["line"]][:start["character"]]
                tail = lines[end["line"]][end["character"]:]
                lines[start["line"]:end["line"] + 1] = [head + change["newText"] + tail]
        open(path, "w", encoding="utf-8").write("\n".join(lines))
        touched[path] = len(edits)
    return touched


def cmd_rename(args):
    path = os.path.realpath(args.file)
    if not os.path.isfile(path):
        die(f"no file {path}")
    if args.symbol is not None:
        line, col = find_symbol_position(path, args.symbol)
    else:
        line, col = args.line, args.col

    db_dir = ensure_worktree_db(ROOT, args.db_dir)
    clangd = Clangd(resolve_clangd(db_dir), db_dir)
    try:
        clangd.request("initialize", {
            "processId": os.getpid(),
            "rootUri": uri(ROOT),
            "capabilities": {"workspace": {"workspaceEdit": {"documentChanges": True}}},
        })
        clangd.notify("initialized", {})
        text = open(path, encoding="utf-8").read()
        clangd.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri(path), "languageId": "cpp", "version": 1, "text": text}})
        edit = clangd.request("textDocument/rename", {
            "textDocument": {"uri": uri(path)},
            "position": {"line": line, "character": col},
            "newName": args.new_name,
        })
    finally:
        clean_exit = clangd.shutdown()
    if not clean_exit:
        die("clangd did not complete orderly shutdown; no rename applied", 2)
    if not edit or not (edit.get("changes") or edit.get("documentChanges")):
        die(f"clangd returned no rename for {args.symbol or (line, col)} — "
            f"cursor may not be on a renameable symbol", 3)

    if args.apply:
        touched = apply_workspace_edit(edit)
        for path, count in sorted(touched.items()):
            print(f"renamed {count:3d} ref(s)  {os.path.relpath(path, ROOT)}")
    else:
        print(json.dumps(edit, indent=2))
        print("\n(dry run — pass --apply to write)", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    rename = sub.add_parser("rename", help="semantic rename via clangd")
    rename.add_argument("--file", required=True)
    rename.add_argument("--line", type=int, help="0-based line of the cursor")
    rename.add_argument("--col", type=int, help="0-based column of the cursor")
    rename.add_argument("--symbol", help="find this whole-word symbol as the cursor")
    rename.add_argument("--new-name", required=True)
    rename.add_argument("--db-dir",
                        help="dir holding compile_commands.json (default: this checkout's build, "
                             "else its main worktree's, remapped)")
    rename.add_argument("--apply", action="store_true", help="write edits (default: dry run)")
    rename.set_defaults(func=cmd_rename)
    args = parser.parse_args()
    if args.command == "rename" and args.symbol is None and (args.line is None or args.col is None):
        die("rename needs --symbol OR both --line and --col")
    args.func(args)


if __name__ == "__main__":
    main()
