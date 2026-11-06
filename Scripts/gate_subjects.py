#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Proves untracked discovery with absent, valid, and violating controls."""

import fcntl
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import uuid
from dataclasses import dataclass

import gate_report


class ProbeError(Exception):
    pass


class Interrupted(Exception):
    def __init__(self, number):
        self.status = 128 + number
        super().__init__(f"interrupted by signal {number}")


@dataclass(frozen=True)
class Row:
    name: str
    command: tuple
    selector: str
    suffix: str
    clean: str
    violation: str
    registered: bool = False
    swift: bool = False


def rows():
    clean_cpp = "int probeFunction()\n{\n    return 0;\n}\n"
    return (
        Row("check-slop.py", (sys.executable, "Scripts/check-slop.py"),
            "Scripts/check-slop.py", ".cpp", clean_cpp,
            "// We simply need to obviously just check this.\n" + clean_cpp),
        Row("check-comments.sh", ("bash", "Scripts/check-comments.sh"),
            "Scripts/check-comments.sh", ".cpp", clean_cpp,
            "// It used to return zero and now returns one.\n" + clean_cpp),
        Row("format-code.sh", ("bash", "Scripts/format-code.sh", "--check"),
            "Scripts/format-code.sh", ".cpp", clean_cpp,
            "int  probeFunction( ) {return    0;}\n"),
        Row("check-shell.sh", ("bash", "Scripts/check-shell.sh"),
            "Scripts/check-shell.sh", ".sh", "#!/usr/bin/env bash\ntrue\n",
            "#!/usr/bin/env bash\nunusedByDesign=1\n"),
        Row("check-gates-invoked.py", (sys.executable, "Scripts/check-gates-invoked.py"),
            "Scripts/check-gates-invoked.py", ".sh", "#!/usr/bin/env bash\ntrue\n",
            "#!/usr/bin/env bash\ntrue\n", registered=True),
        Row("check-personal-paths.py", (sys.executable, "Scripts/check-personal-paths.py"),
            "Scripts/check-personal-paths.py", ".cpp", clean_cpp,
            "// " + "/Users/" + "zzprobeaccount/notes.txt\n" + clean_cpp),
        Row("format-code.sh (Swift)", ("bash", "Scripts/format-code.sh", "--check"),
            "Scripts/format-code.sh", ".swift", "func probeFunction() -> Int {\n    0\n}\n",
            "func probeFunction( ) -> Int {\n              return 0\n}\n", swift=True),
    )


class Probes:
    def __init__(self, root, environment=None):
        self.root = Path(root).resolve()
        self.environment = dict(os.environ if environment is None else environment)
        # A hook's alternate index or git directory must not redirect this tree's probes.
        for key in tuple(self.environment):
            if key.startswith("GIT_"):
                del self.environment[key]
        self.owned = {}
        self.lock_descriptor = None

    def command(self, arguments, timeout=300):
        process = subprocess.Popen(arguments, cwd=self.root, env=self.environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, errors="replace", start_new_session=True)
        try:
            output, _ = process.communicate(timeout=timeout)
        except BaseException:
            # Only the subprocess group created for this invocation is ours to stop.
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.communicate(timeout=2)
            except ProcessLookupError:
                process.communicate()
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()
            raise
        return process.returncode, output

    def lock(self):
        status, output = self.command(("git", "rev-parse", "--absolute-git-dir"))
        if status:
            raise ProbeError(f"cannot resolve worktree git directory: {output.strip()}")
        lock_path = Path(output.strip()) / "ubiquitty-gate-subjects.lock"
        descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        self.lock_descriptor = descriptor
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise ProbeError(f"lock is not a regular file: {lock_path}")
        # The lock inode persists: unlinking it lets a third process bypass a waiter.
        fcntl.flock(descriptor, fcntl.LOCK_EX)

    def changed(self):
        if self.environment.get("UBIQUITTY_CHECK_GATE_SUBJECTS_ALL") == "1":
            return None
        changed = set()
        for arguments in (
            ("git", "diff", "--name-only", "-z", "origin/main...HEAD", "--", "Scripts/"),
            ("git", "diff", "--name-only", "-z", "--", "Scripts/"),
            ("git", "diff", "--cached", "--name-only", "-z", "--", "Scripts/"),
            ("git", "ls-files", "--others", "--exclude-standard", "-z", "--", "Scripts/"),
        ):
            status, output = self.command(arguments)
            if status:
                print("check-gate-subjects: scope unavailable; probing every selector: "
                      + output.strip(), file=sys.stderr)
                return None
            changed.update(output.split("\0"))
        shared = {
            "Scripts/check-gate-subjects.sh", "Scripts/gate_subjects.py",
            "Scripts/test-gate-subjects.py", "Scripts/gate_corpus.py",
            "Scripts/gate-report-lib.sh", "Scripts/gate_report.py", "Scripts/check-style.py",
            "Scripts/ubq_roots.py", "Scripts/style_support.py",
        }
        return None if changed & shared else changed

    def create(self, relative, content):
        path = self.root / relative
        if path.parent.resolve() != path.parent or not path.parent.is_relative_to(self.root):
            raise ProbeError(f"probe directory is not an owned tree directory: {path.parent}")
        descriptor = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o700)
        self.owned[path] = descriptor
        self.write(path, content)
        return path

    def verify_owned(self, path):
        actual = path.lstat()
        owned = os.fstat(self.owned[path])
        if not stat.S_ISREG(actual.st_mode) or (actual.st_dev, actual.st_ino) != (owned.st_dev, owned.st_ino):
            raise ProbeError(f"probe ownership changed; preserving {path}")

    def write(self, path, content):
        self.verify_owned(path)
        descriptor = self.owned[path]
        os.lseek(descriptor, 0, os.SEEK_SET)
        os.ftruncate(descriptor, 0)
        with os.fdopen(os.dup(descriptor), "w", encoding="utf-8") as stream:
            stream.write(content)

    def remove(self, path):
        self.verify_owned(path)
        path.unlink()
        os.close(self.owned.pop(path))

    def cleanup(self):
        failures = []
        for path in tuple(self.owned):
            try:
                self.remove(path)
            except (OSError, ProbeError) as error:
                failures.append(str(error))
                os.close(self.owned.pop(path))
        if self.lock_descriptor is not None:
            os.close(self.lock_descriptor)
            self.lock_descriptor = None
        return failures

    def swift_available(self):
        if shutil.which("xcrun", path=self.environment.get("PATH")):
            status, output = self.command(("xcrun", "--find", "swift-format"))
            if status == 0:
                if not output.strip() or not os.access(output.strip(), os.X_OK):
                    raise ProbeError("xcrun resolved an unusable swift-format")
                return True
            missing_utility = re.search(r"unable to find utility [\"']swift-format[\"']", output)
            if status not in (1, 72) or not missing_utility:
                raise ProbeError(f"xcrun failed resolving swift-format ({status}): {output.strip()}")
        return shutil.which("swift-format", path=self.environment.get("PATH")) is not None

    def measure(self, row, path, phase, expected):
        status, output = self.command(row.command)
        if status not in (0, 1):
            raise ProbeError(f"{row.name} {phase} exited {status}:\n{output[-4000:]}")
        named = path.name in output
        if named != expected or (expected and status != 1):
            raise ProbeError(f"{row.name} {phase}: expected probe "
                             f"{'diagnosed with exit 1' if expected else 'absent from findings'}, "
                             f"got named={named}, exit={status}:\n{output[-4000:]}")

    def probe(self, row):
        token = uuid.uuid4().hex
        directory = "Scripts" if row.suffix == ".sh" else "Tests/Core"
        prefix = "check-" if row.registered else ""
        relative = f"{directory}/{prefix}zz-gate-subject-{token}{row.suffix}"
        path = self.root / relative
        if os.path.lexists(path):
            raise ProbeError(f"probe path already exists; preserving {path}")
        self.measure(row, path, "absent", False)
        self.create(relative, row.clean)
        caller = None
        if row.registered:
            caller = self.create(f"Scripts/zz-gate-subject-caller-{token}.sh", f"#!/usr/bin/env bash\nbash {relative}\n")
        self.measure(row, path, "valid", False)
        if caller is not None:
            self.remove(caller)
        self.write(path, row.violation)
        self.measure(row, path, "violating", True)
        self.remove(path)

    def run(self):
        self.lock()
        changed = self.changed()
        checked = skipped = 0
        print("=== tree-scoped gates discover untracked violations ===", flush=True)
        for row in rows():
            if changed is not None and row.selector not in changed:
                print(f"  --       {row.name}: selector unchanged")
                skipped += 1
                continue
            if row.swift and not self.swift_available():
                print("check-gate-subjects: SKIPPED — format-code.sh (Swift): swift-format is not installed")
                skipped += 1
                continue
            self.probe(row)
            checked += 1
            print(f"  ok       {row.name}: absent, valid, and violating controls", flush=True)
        print(f"check-gate-subjects: ok ({checked} selector(s) proved, {skipped} explicitly skipped)")
        return 0


def execute(probes):
    previous = {}
    status = 2

    def interrupted(number, _frame):
        raise Interrupted(number)

    try:
        for number in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, interrupted)
        status = probes.run()
    except Interrupted as error:
        status = error.status
        print(f"check-gate-subjects: FAILED — {error}", file=sys.stderr)
    except (OSError, ProbeError, subprocess.SubprocessError) as error:
        print(f"check-gate-subjects: FAILED — {error}", file=sys.stderr)
    finally:
        for number in previous:
            signal.signal(number, signal.SIG_IGN)
        try:
            failures = probes.cleanup()
            if failures:
                gate_report.failed("check-gate-subjects", "cleanup failure(s)", failures)
                status = status or 2
        finally:
            for number, handler in previous.items():
                signal.signal(number, handler)
    return status


def main():
    root = Path(__file__).resolve().parent.parent
    if sys.argv[1:] == ["--self-test"]:
        return subprocess.call([sys.executable, str(root / "Scripts/test-gate-subjects.py")])
    if sys.argv[1:] in (["--help"], ["-h"]):
        print(__doc__ + "\nUsage: Scripts/check-gate-subjects.sh [--self-test]")
        return 0
    if sys.argv[1:]:
        print("check-gate-subjects: unknown arguments", file=sys.stderr)
        return 2
    return execute(Probes(root))


if __name__ == "__main__":
    sys.exit(main())
