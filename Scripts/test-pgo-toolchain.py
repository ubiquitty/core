#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Release-image dispatch and profile provenance checks, without a compiler."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import pgo_toolchain


class BenchmarkDriverTests(unittest.TestCase):
    def test_only_core_and_the_link_receive_the_selected_profile(self):
        source = Path(__file__).resolve().parent
        for builder in ("build-perf-bench.sh", "run-bench.sh"):
            with self.subTest(builder=builder), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                scripts = root / "Scripts"
                scripts.mkdir()
                shutil.copy2(source / builder, scripts / builder)
                (scripts / "build-root.sh").write_text('export UBQ_BUILD_ROOT\n')
                (scripts / "pgo-flags.sh").write_text('''ubiquitty_pgo_profile() { printf '%s' "$UBQ_PGO_PROFILE"; }
ubiquitty_pgo_flags() { printf '%s' "-fprofile-use=$UBQ_PGO_PROFILE"; }
''')
                (scripts / "pgo-staleness.sh").write_text('pgo_staleness_warning() { :; }\n')
                (scripts / "portable-core-lib.sh").write_text('''ubiquitty_lto_flag() { printf '%s' '-flto=thin'; }
build_portable_core_archive() {
    printf '%s' "$2" > "$UBQ_TEST_ARCHIVE_FLAGS"
    PORTABLE_CORE_ARCHIVE="$1/libubiquitty-core.a"
    touch "$PORTABLE_CORE_ARCHIVE"
}
ubiquitty_core_pal_stub_source() { touch "$1"; }
ubiquitty_bench_include_flags() { printf '%s\\n' "-I$1/Source" "-I$1/Tests"; }
''')
                compiler = root / "compiler"
                compiler.write_text("#!" + sys.executable + '''
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
with open(os.environ["UBQ_TEST_COMPILER_LOG"], "a") as output:
    output.write(json.dumps(args) + "\\n")
if "-c" not in args:
    sys.exit(42)
Path(args[args.index("-o") + 1]).touch()
''')
                compiler.chmod(0o755)
                log = root / "compiler.jsonl"
                archive = root / "archive-flags"
                drivers = []
                for profile in ("baseline", "candidate"):
                    log.write_text("")
                    profile_path = str(root / (profile + ".profdata"))
                    environment = {**os.environ, "CXX": str(compiler), "UBQ_COMPILER_LAUNCHER": "",
                                   "UBQ_BUILD_ROOT": str(root / "build"), "UBQ_PGO_PROFILE": profile_path,
                                   "UBQ_TEST_COMPILER_LOG": str(log), "UBQ_TEST_ARCHIVE_FLAGS": str(archive)}
                    result = subprocess.run(["bash", str(scripts / builder)], env=environment,
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 42, result.stderr)
                    calls = [json.loads(line) for line in log.read_text().splitlines()]
                    self.assertEqual(len(calls), 2)
                    driver, link = calls
                    self.assertIn("-c", driver)
                    self.assertFalse(any(flag.startswith("-fprofile") for flag in driver))
                    self.assertIn("-O3", driver)
                    self.assertIn("-flto=thin", driver)
                    self.assertIn("-fprofile-use=" + profile_path, link)
                    self.assertIn("-fprofile-use=" + profile_path, archive.read_text())
                    drivers.append(driver)
                self.assertEqual(drivers[0], drivers[1])


class BaselineWorktreeTests(unittest.TestCase):
    def test_cleanup_preserves_an_unavailable_siblings_index_and_branch(self):
        script = Path(__file__).with_name("check-pgo-applies.py")
        spec = importlib.util.spec_from_file_location("check_pgo_applies", script)
        check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(check)
        self.addCleanup(check.ROOTED.cleanup)
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        for should_raise in (False, True):
            with self.subTest(probe_fails=should_raise), tempfile.TemporaryDirectory() as directory, \
                    patch.dict(os.environ, environment, clear=True):
                directory = Path(directory).resolve()
                root = directory / "repo"
                root.mkdir()

                def git(tree, *arguments):
                    return subprocess.run(["git", "-C", str(tree), *arguments],
                                          check=True, capture_output=True, text=True).stdout.strip()

                git(root, "init", "--quiet")
                self.assertEqual(Path(git(root, "rev-parse", "--absolute-git-dir")), root / ".git")
                git(root, "config", "core.hooksPath", os.devnull)
                git(root, "config", "user.name", "Test")
                git(root, "config", "user.email", "test@example.com")
                (root / "tracked").write_text("committed\n")
                git(root, "add", "tracked")
                git(root, "commit", "--quiet", "-m", "fixture")
                sibling = Path(directory) / "sibling"
                git(root, "worktree", "add", "-b", "sibling", str(sibling))
                (sibling / "tracked").write_text("staged\n")
                git(sibling, "add", "tracked")
                (sibling / "tracked").write_text("unstaged\n")
                (sibling / "untracked").write_text("untracked\n")
                before_status = git(sibling, "status", "--porcelain=v1")
                index = Path(git(sibling, "rev-parse", "--git-path", "index"))
                before_index = index.read_bytes()
                before_branch = git(sibling, "symbolic-ref", "HEAD")
                before_head = git(sibling, "rev-parse", "HEAD")
                unavailable = Path(directory) / "unmounted-sibling"
                sibling.rename(unavailable)
                try:
                    expectation = self.assertRaisesRegex(RuntimeError, "probe failed") if should_raise else contextlib.nullcontext()
                    with expectation:
                        with check.BaselineWorktree(str(root), "HEAD") as baseline:
                            baseline_admin = Path(git(baseline, "rev-parse", "--absolute-git-dir"))
                            (Path(baseline) / "probe-output").write_text("temporary\n")
                            if should_raise:
                                raise RuntimeError("probe failed")
                finally:
                    unavailable.rename(sibling)
                self.assertFalse(Path(baseline).exists())
                self.assertFalse(baseline_admin.exists())
                self.assertTrue(index.exists(), "baseline cleanup pruned the unavailable sibling")
                self.assertEqual(index.read_bytes(), before_index)
                self.assertEqual(git(sibling, "status", "--porcelain=v1"), before_status)
                self.assertEqual(git(sibling, "symbolic-ref", "HEAD"), before_branch)
                self.assertEqual(git(sibling, "rev-parse", "HEAD"), before_head)
                self.assertEqual((sibling / "tracked").read_text(), "unstaged\n")
                self.assertEqual((sibling / "untracked").read_text(), "untracked\n")


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
if args[0] == "info":
    print("[]")
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
elif args[:2] == ["container", "inspect"]:
    sys.exit("Error: No such container: fixture")
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

    def test_codegen_policy_uses_the_compiler_target(self):
        linux_x86 = "#define __clang__ 1\n#define __linux__ 1\n#define __x86_64__ 1\n"
        with patch.object(pgo_toolchain, "compiler_output", return_value=linux_x86):
            self.assertEqual(pgo_toolchain.codegen_flags("compiler"), ["-falign-functions=64"])
        for macros in (linux_x86.replace("__x86_64__", "__aarch64__"),
                       linux_x86.replace("__linux__", "__APPLE__"),
                       linux_x86.replace("__clang__", "__GNUC__")):
            with self.subTest(macros=macros), patch.object(pgo_toolchain, "compiler_output", return_value=macros):
                self.assertEqual(pgo_toolchain.codegen_flags("compiler"), [])

    def test_codegen_query_does_not_require_a_profile_or_container(self):
        with patch.object(pgo_toolchain, "codegen_flags", return_value=["-falign-functions=64"]), \
                patch.object(sys, "argv", ["pgo_toolchain.py", "codegen-flags", "compiler"]), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(pgo_toolchain.main(), 0)
            self.assertEqual(output.getvalue(), "-falign-functions=64\n")

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
