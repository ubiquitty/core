#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Release-image dispatch and profile provenance checks, without a compiler."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import pgo_toolchain


class ToolchainTests(unittest.TestCase):
    def test_release_check_refuses_a_missing_recorded_budget(self):
        script = Path(__file__).with_name("check-pgo-applies.py")
        spec = importlib.util.spec_from_file_location("check_pgo_applies", script)
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        with tempfile.TemporaryDirectory() as directory:
            profile = Path(directory) / "linux-x86_64-core.profdata"
            profile.write_bytes(b"profile")
            arguments = [str(script), "--profile", str(profile), "--recorded-budget", "--max-discarded", "0"]
            with patch.object(check, "repository_root", return_value=directory), \
                    patch.object(pgo_toolchain, "needs_image", return_value=False), \
                    patch.object(pgo_toolchain, "select_image_compiler"), \
                    patch.object(pgo_toolchain, "refusal", return_value=None), \
                    patch.object(sys, "argv", arguments), contextlib.redirect_stdout(io.StringIO()):
                self.assertNotEqual(check.main(), 0)
        check.ROOTED.cleanup()

    def test_cold_image_is_built_before_its_identity_is_read(self):
        root = Path(__file__).resolve().parent.parent
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            docker = temporary / "docker"
            docker.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
state = Path(os.environ["DOCKER_TEST_STATE"])
args = sys.argv[1:]
if args == ["info"]:
    pass
elif args[:2] == ["image", "inspect"]:
    if not state.exists():
        sys.exit(1)
    if "--format" in args:
        print("sha256:" + "1" * 64)
elif args[0] == "build":
    state.touch()
elif args[0] == "run":
    assert "UBQ_CI_LINUX_IMAGE_ID=sha256:" + "1" * 64 in args
    assert args[args.index("--user") + 1] == "1234:5678"
else:
    sys.exit("unexpected Docker command: " + repr(args))
''')
            docker.chmod(0o755)
            environment = {**os.environ, "PATH": str(temporary) + os.pathsep + os.environ["PATH"],
                           "DOCKER_TEST_STATE": str(temporary / "built"), "UBQ_CI_LINUX_IMAGE": "true",
                           "UBQ_CI_LINUX_USER": "1234:5678",
                           "RUNNER_WORKSPACE": str(root), "RUNNER_TEMP": str(temporary / "runner"),
                           "RUNNER_TOOL_CACHE": str(temporary / "tools")}
            environment.pop("UBQ_CI_IMAGE_ARCHIVE", None)
            result = subprocess.run(["bash", str(root / "Scripts/ci-linux-image.sh"), "true"],
                                    cwd=root, env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((temporary / "built").exists())

    def test_native_linux_enters_image(self):
        with patch.dict(os.environ, {}, clear=True), patch("platform.system", return_value="Linux"):
            self.assertTrue(pgo_toolchain.needs_image())

    def test_apple_stays_native_unless_linux_is_requested(self):
        with patch.dict(os.environ, {}, clear=True), patch("platform.system", return_value="Darwin"):
            self.assertFalse(pgo_toolchain.needs_image())
            os.environ["UBQ_PGO_PLATFORM"] = "Linux"
            self.assertTrue(pgo_toolchain.needs_image())

    def test_image_does_not_reenter(self):
        with patch.dict(os.environ, {"UBQ_PGO_PLATFORM": "Linux", "UBQ_CI_LINUX_IMAGE_ID": "sha256:" + "1" * 64}):
            self.assertFalse(pgo_toolchain.needs_image())

    def test_gate_replaces_an_inherited_nonrelease_compiler(self):
        environment = {"UBQ_CI_LINUX_IMAGE_ID": "sha256:" + "1" * 64,
                       "UBQ_PGO_LLVM": "/release/llvm/bin", "CXX": "g++", "PATH": "/usr/bin"}
        with patch.dict(os.environ, environment):
            pgo_toolchain.select_image_compiler()
            self.assertEqual(os.environ["CXX"], "clang++")
            self.assertEqual(os.environ["PATH"], "/release/llvm/bin:/usr/bin")

    def test_linux_profile_requires_its_image_compiler_and_bytes(self):
        environment = {"UBQ_CI_LINUX_IMAGE_ID": "sha256:" + "1" * 64, "UBQ_CI_LINUX_RECIPE": "recipe"}
        with tempfile.TemporaryDirectory() as directory, patch.dict(os.environ, environment), \
                patch.object(pgo_toolchain, "is_linux_compiler", return_value=True), \
                patch.object(pgo_toolchain, "compiler_output", return_value="clang 22") as compiler:
            profile = str(Path(directory) / "linux-aarch64-core.profdata")
            Path(profile).write_bytes(b"profile")
            self.assertIsNotNone(pgo_toolchain.refusal(profile, "clang++"))
            pgo_toolchain.record(profile, "clang++")
            self.assertIsNone(pgo_toolchain.refusal(profile, "clang++"))
            compiler.return_value = "clang 23"
            self.assertIsNotNone(pgo_toolchain.refusal(profile, "clang++"))
            compiler.return_value = "clang 22"
            os.environ["UBQ_CI_LINUX_IMAGE_ID"] = "sha256:" + "2" * 64
            self.assertIsNotNone(pgo_toolchain.refusal(profile, "clang++"))
            os.environ["UBQ_CI_LINUX_IMAGE_ID"] = environment["UBQ_CI_LINUX_IMAGE_ID"]
            Path(profile).write_bytes(b"another profile")
            self.assertIsNotNone(pgo_toolchain.refusal(profile, "clang++"))

    def test_native_linux_refuses_shipping_profile(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(pgo_toolchain, "is_linux_compiler", return_value=True):
            self.assertIn("ci-linux-image.sh", pgo_toolchain.refusal("profile.profdata", "clang++"))

    def test_apple_does_not_require_linux_metadata(self):
        with patch.object(pgo_toolchain, "is_linux_compiler", return_value=False):
            self.assertIsNone(pgo_toolchain.refusal("portable-core.profdata", "clang++"))


if __name__ == "__main__":
    unittest.main()
