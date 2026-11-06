#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Exercise build wrappers with stand-in tools; no compiler or SDK is used."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class BuildWrapperTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="ubq-build-wrappers-")
        self.addCleanup(scratch.cleanup)
        self.directory = Path(scratch.name).resolve()
        self.root = self.directory / "checkout with spaces"
        self.scripts = self.root / "Scripts"
        self.scripts.mkdir(parents=True)
        original = Path(__file__).resolve().parent
        for name in ("pre-push-gates.sh", "pre-push-gates-lib.sh", "build-root.sh", "build_root.py",
                     "ccache-env.sh", "portable-core-lib.sh"):
            shutil.copy2(original / name, self.scripts / name)
        self.buildRoot = self.directory / "external build root"
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        # A closed PATH prevents accidental use of a real compiler. These are
        # the only external utilities used by the wrappers and shared stub.
        for name in ("dirname", "mkdir", "cat"):
            (self.bin / name).symlink_to(shutil.which(name))
        self.bash = shutil.which("bash")
        self.log = self.directory / "tool.json"
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith(("GIT_", "UBIQUITTY_", "UBQ_"))
                            and key not in ("BASH_ENV", "ENV", "SHELLOPTS", "BASHOPTS")}
        self.environment.update(PATH=str(self.bin), UBQ_BUILD_ROOT=str(self.buildRoot),
                                UBQ_TEST_LOG=str(self.log))

    def writeTool(self, path, body):
        path.write_text(f"#!{sys.executable}\n" + body, encoding="utf-8")
        path.chmod(0o755)

    def runScript(self, name, *arguments):
        return subprocess.run([self.bash, str(self.scripts / name), *arguments],
                              env=self.environment, text=True, capture_output=True)

    def testPrePushDoesNotChangeSchedulingPriority(self):
        self.writeTool(self.bin / "renice", """import os
from pathlib import Path
Path(os.environ['UBQ_TEST_LOG']).write_text('renice called')
""")
        # Stop at the first sourced helper, before any gate or repository
        # operation. This executes the real scheduling prefix, not its text.
        (self.scripts / "gate-report-lib.sh").write_text("exit 83\n", encoding="utf-8")
        result = self.runScript("pre-push-gates.sh")
        self.assertEqual(result.returncode, 83, result.stderr)
        self.assertFalse(self.log.exists(), "pre-push changed the inherited priority")


if __name__ == "__main__":
    unittest.main()
