#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Tests host tooling with controlled subprocess output and build arguments.

`Tools/host-tests.py` and `Tools/remint-fingerprint.py` are the loop an agent
runs in every worktree, and the parts that can be wrong quietly are the parts
that read another program's output: the test list ctest prints, a CMake cache's
home line, the fingerprint test's own report, and the one line in a test source
the re-mint may rewrite. Each is driven here with the program's output held
constant and the outcome read back, so a tool that misreads is a failing case
rather than a wrong build root or a silently unrecorded digest.

Stdlib only.
"""
from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name: str):
    """Imports a Tools/ script as a module, without running its main.

    - Parameter name: the script's file name.
    - Returns: the module.
    """
    spec = importlib.util.spec_from_file_location(name.replace("-", "_").replace(".py", ""), ROOT / "Tools" / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Completed:
    def __init__(self, stdout: str = "", returncode: int = 0):
        self.stdout = stdout
        self.stderr = ""
        self.returncode = returncode


class BuildPrerequisiteTests(unittest.TestCase):
    def test_quic_requires_go_before_fetching_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executables = root / "bin"
            executables.mkdir()
            for tool in ("dirname", "sed", "uname", "cmake"):
                executable = shutil.which(tool)
                if executable is None:
                    self.skipTest(f"{tool} is unavailable")
                (executables / tool).symlink_to(executable)
            environment = {**os.environ, "PATH": str(executables), "UBQ_BUILD_ROOT": str(root / "build")}
            prefix = root / "quic"
            command = ["/bin/bash", str(ROOT / "Scripts/fetch-quic-deps.sh"), str(prefix)]
            checked = subprocess.run(command, env=environment, text=True, capture_output=True)
            self.assertEqual(checked.returncode, 127, checked.stdout + checked.stderr)
            self.assertIn("Go is required", checked.stderr)
            self.assertFalse((prefix / "src").exists())
            (prefix / "lib").mkdir(parents=True)
            for archive in ("libngtcp2.a", "libngtcp2_crypto_boringssl.a", "libssl.a", "libcrypto.a"):
                (prefix / "lib" / archive).touch()
            checked = subprocess.run(command, env=environment, text=True, capture_output=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_agent_smokes_resolve_build_root_and_explicit_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            # The first binary each smoke checks: the daemon for the three that
            # start one, the ubq CLI for the one that reads only completions.
            # Each row is the build directory, the file the build names, and the
            # variable that overrides it.
            host = ("host", "ubiquitty-host", "UBIQUITTY_HOST_BIN")
            cli = ("agent", "ubq", "UBQ_BIN")
            for script in ("smoke", "auth_smoke", "mcp_smoke", "completions_smoke"):
                buildDirectory, binary, variable = cli if script == "completions_smoke" else host
                for override in (False, True):
                    with self.subTest(script=script, override=override):
                        environment = {**os.environ, "UBQ_BUILD_ROOT": directory}
                        environment.pop("UBIQUITTY_HOST_BIN", None)
                        environment.pop("UBQ_BIN", None)
                        expected = str(Path(directory) / buildDirectory / binary)
                        if override:
                            expected = str(Path(directory) / "explicit binary")
                            environment[variable] = expected
                        checked = subprocess.run(["bash", str(ROOT / f"Tests/Agent/{script}.sh")],
                                                 env=environment, text=True, capture_output=True)
                        self.assertNotEqual(checked.returncode, 0)
                        self.assertIn("not built at " + expected, checked.stdout + checked.stderr)

    def test_font_deferrals_survive_later_platform_deferrals(self):
        if shutil.which("cmake") is None:
            self.skipTest("cmake is unavailable")
        source = (ROOT / "Tests/CMakeLists.txt").read_text()
        source = re.sub(r"#[^\n]*", "", source)
        declarations = re.findall(r"(?:set\(UBIQUITTY_(?:DEFERRED|UNAVAILABLE)_TESTS|list\(APPEND UBIQUITTY_(?:DEFERRED|UNAVAILABLE)_TESTS)\b[^)]*\)", source)
        # The registration as Tests/CMakeLists.txt calls it, with every command
        # that would build a target recording the target's name instead.
        stubs = "".join(f"function({command} name)\n"
                        f'    file(APPEND "${{CMAKE_BINARY_DIR}}/compiled" "${{name}}\\n")\nendfunction()\n'
                        for command in ("add_executable", "add_library"))
        stubs += "".join(f"function({command})\nendfunction()\n"
                         for command in ("target_include_directories", "target_compile_features",
                                         "target_compile_options", "target_link_libraries",
                                         "target_compile_definitions", "target_precompile_headers"))
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory) / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(Deferrals LANGUAGES NONE)\n"
                "enable_testing()\n"
                f'set(UBIQUITTY_CORE_SOURCE_DIR "{ROOT}")\n'
                'set(UBIQUITTY_TEST_PCH OFF CACHE BOOL "")\n'
                + stubs
                + f'include("{ROOT}/Scripts/cmake/UbiquittyTests.cmake")\n'
                + "\n".join(declarations) + "\n"
                'file(WRITE "${CMAKE_BINARY_DIR}/deferred" "${UBIQUITTY_DEFERRED_TESTS}")\n'
                'file(WRITE "${CMAKE_BINARY_DIR}/unavailable" "${UBIQUITTY_UNAVAILABLE_TESTS}")\n'
                "set(paths ${UBIQUITTY_UNAVAILABLE_TESTS} test_serial)\n"
                'list(TRANSFORM paths PREPEND "Platform/")\n'
                'list(TRANSFORM paths APPEND ".cpp")\n'
                f'ubiquitty_add_host_tests(ROOT "{directory}" SOURCES ${{paths}}\n'
                f'    INCLUDE_DIRECTORIES "{directory}"\n'
                "    DEFERRED ${UBIQUITTY_DEFERRED_TESTS}\n"
                "    UNAVAILABLE ${UBIQUITTY_UNAVAILABLE_TESTS})\n"
                f'ubiquitty_finish_host_tests(DRIVER "{ROOT}/Scripts/run-tests.sh")\n')
            build = Path(directory) / "build"
            configured = subprocess.run(["cmake", "-S", str(project), "-B", str(build)],
                                        capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            deferred = (build / "deferred").read_text().split(";")
            # The Linux font tests ship with the repository that holds the
            # Linux renderers; a checkout without one has nothing to defer.
            for name in ("test_linux_fontinventory", "test_linux_color_emoji", "test_linux_color_glyph_painter", "test_linux_fallback_face", "test_serial"):
                if name.startswith("test_linux_") and not any(ROOT.glob(f"Tests/**/{name}.cpp")):
                    continue
                self.assertIn(name, deferred)
            # A deferred test still compiles; an unavailable one builds nothing,
            # though it is deferred as well. Both are counted as left out.
            self.assertEqual((build / "compiled").read_text().split(), ["ubq-deferred-test_serial"])
            unavailable = [name for name in (build / "unavailable").read_text().split(";") if name]
            self.assertIn(f"CTest runs 0 (one at a time with ctest -R <name>); {len(unavailable) + 1} more",
                          configured.stdout)


class PublishedLintConfigurationTests(unittest.TestCase):
    def test_public_manifest_carries_the_formatter_and_linter_rules(self):
        script = (ROOT / "Scripts" / "snapshot-clean.sh").read_text()
        manifest = script.split("MANIFEST=(", 1)[1].split(")", 1)[0].split()
        for name in (".clang-format", ".clang-tidy", ".swift-format"):
            self.assertIn(name, manifest)
            # The Swift rules ship with the repositories that hold Swift.
            if name == ".swift-format" and not (ROOT / name).exists():
                continue
            self.assertTrue((ROOT / name).is_file())

    def test_tidy_refuses_a_missing_configuration_before_reporting_rules(self):
        with tempfile.TemporaryDirectory() as directory:
            scripts = Path(directory) / "Scripts"
            scripts.mkdir()
            script = scripts / "check-tidy.sh"
            shutil.copy2(ROOT / "Scripts" / script.name, script)
            result = subprocess.run(["bash", str(script), "--rule-count"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("missing", result.stderr)
            self.assertIn(".clang-tidy", result.stderr)
            self.assertEqual(result.stdout, "")


class HostBuildEntrypointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.repository = self.directory / "checkout with spaces"
        scripts = self.repository / "Scripts"
        scripts.mkdir(parents=True)
        for name in ("build-host.sh", "build-root.sh", "ccache-env.sh", "build-provenance.sh"):
            shutil.copy2(ROOT / "Scripts" / name, scripts / name)
        self.buildRoot = self.directory / "external build output"
        self.trace = self.directory / "cmake-arguments.jsonl"
        executables = self.directory / "executables"
        executables.mkdir()
        cmake = executables / "cmake"
        cmake.write_text(
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            "with open(os.environ['HOST_BUILD_TEST_TRACE'], 'a') as trace:\n"
            "    trace.write(json.dumps(sys.argv[1:]) + '\\n')\n"
        )
        cmake.chmod(0o755)
        self.environment = dict(os.environ)
        self.environment.pop("UBIQUITTY_QUIC_PREFIX", None)
        self.environment.update({
            "PATH": str(executables) + os.pathsep + os.environ.get("PATH", os.defpath),
            "HOST_BUILD_TEST_TRACE": str(self.trace),
            "UBQ_BUILD_ROOT": str(self.buildRoot),
            "UBQ_BUILD_JOBS": "1",
        })

    def assertSelectedPrefix(self, expected):
        result = subprocess.run(
            ["bash", str(self.repository / "Scripts" / "build-host.sh")],
            env=self.environment, capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line) for line in self.trace.read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertIn("-DUBIQUITTY_QUIC=ON", calls[0])
        self.assertEqual(
            [argument for argument in calls[0] if argument.startswith("-DUBIQUITTY_QUIC_PREFIX=")],
            ["-DUBIQUITTY_QUIC_PREFIX=" + str(expected)],
        )
        self.assertEqual(calls[1], ["--build", str(self.buildRoot / "host"), "-j1"])

    def testDefaultUsesTheExternalBuildRoot(self):
        self.assertSelectedPrefix(self.buildRoot / "quic-deps")

    def testEmptyOverrideUsesTheExternalBuildRoot(self):
        self.environment["UBIQUITTY_QUIC_PREFIX"] = ""
        self.assertSelectedPrefix(self.buildRoot / "quic-deps")

    def testOverrideUsesTheSelectedCache(self):
        prefix = self.directory / "runner-workspace" / "build-gcc" / "quic-deps"
        self.environment["UBIQUITTY_QUIC_PREFIX"] = str(prefix)
        self.assertSelectedPrefix(prefix)

    def testOverridePreservesSpacesAndLiteralShellCharacters(self):
        prefix = self.directory / "cache with 'quotes' and $literal" / "quic-deps"
        self.environment["UBIQUITTY_QUIC_PREFIX"] = str(prefix)
        self.assertSelectedPrefix(prefix)


class CompileDatabaseLinkTests(unittest.TestCase):
    """The checkout's compile_commands.json link, for clangd, claimed with care.

    The build root is outside the checkout, where clangd does not look, so the
    build scripts link the database into the checkout. A container build's tree
    sits under the checkout and must not take the name, and a name a person or
    an earlier build put there must survive.
    """

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.checkout = self.directory / "checkout with spaces"
        self.checkout.mkdir()
        self.link = self.checkout / "compile_commands.json"

    def build(self, name, database=True):
        directory = self.directory / name
        directory.mkdir(parents=True, exist_ok=True)
        if database:
            (directory / "compile_commands.json").write_text("[]")
        return directory

    def linkTo(self, build):
        result = subprocess.run(
            ["bash", "-c", '. "$1"; ubq_link_compile_database "$2" "$3"', "link",
             str(ROOT / "Scripts" / "build-root.sh"), str(build), str(self.checkout)],
            env=dict(os.environ, UBQ_BUILD_ROOT=str(self.directory / "root")),
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def testABuildWithADatabaseClaimsTheName(self):
        build = self.build("macos")
        self.linkTo(build)
        self.assertEqual(os.readlink(self.link), str(build / "compile_commands.json"))

    def testABuildWithoutADatabaseLinksNothing(self):
        self.linkTo(self.build("macos", database=False))
        self.assertFalse(os.path.lexists(self.link))

    def testALinkThatResolvesIsKept(self):
        first = self.build("macos")
        self.linkTo(first)
        self.linkTo(self.build("linux"))
        self.assertEqual(os.readlink(self.link), str(first / "compile_commands.json"))

    def testADanglingLinkIsClaimedAgain(self):
        self.linkTo(self.build("gone"))
        (self.directory / "gone" / "compile_commands.json").unlink()
        second = self.build("macos")
        self.linkTo(second)
        self.assertEqual(os.readlink(self.link), str(second / "compile_commands.json"))

    def testAFileOfTheSameNameIsKept(self):
        self.link.write_text("mine")
        self.linkTo(self.build("macos"))
        self.assertEqual(self.link.read_text(), "mine")

    def testABuildUnderTheCheckoutClaimsNothing(self):
        inside = self.checkout / "build" / "linux"
        inside.mkdir(parents=True)
        (inside / "compile_commands.json").write_text("[]")
        self.linkTo(inside)
        self.assertFalse(os.path.lexists(self.link))


class CTestCoveredListTests(unittest.TestCase):
    """Every shell stanza ctest schedules is on the list run-tests.sh skips after it.

    A full run-tests.sh runs ctest and then its own stanza loop, which skips
    the names in ctest-covered.txt. A stanza ctest runs as shell_<name> that is
    missing from the list runs a second time in that loop.
    """

    def test_every_scheduled_shell_stanza_is_covered(self):
        tree = Path(os.environ.get("UBQ_HOST_TEST_TREE", ""))
        tests = tree / "Tests" / "CTestTestfile.cmake"
        if not os.environ.get("UBQ_HOST_TEST_TREE") or not tests.is_file():
            self.skipTest("reads the configured test tree ctest runs this stanza from")
        scheduled = set(re.findall(r"^add_test\(shell_([A-Za-z0-9_-]+) ", tests.read_text(), re.M))
        if not scheduled:
            self.skipTest("this tree schedules no shell stanzas (UBIQUITTY_CTEST_SHELL_REMAINDER=OFF)")
        covered = set((tree / "ctest-covered.txt").read_text().split())
        self.assertEqual(sorted(scheduled - covered), [])


class GateVerdictCacheTests(unittest.TestCase):
    """A replayed verdict must be one the gate reached about this tree.

    A gate that stands down checked nothing: it found no tool or no build, and
    says so with a `<gate>: SKIPPED` line while exiting zero. The key covers
    the tree and some build roots, not every artefact a gate looks for, so a
    stand-down remembered as a pass was replayed after the build it missed
    appeared. The macOS and GTK smokes stayed SKIPPED through a full check-style
    run with both builds present.
    """

    def setUp(self):
        sys.path.insert(0, str(ROOT / "Scripts"))
        import gate_verdict
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.cache = gate_verdict.Cache(root=str(ROOT), enabled=False)
        self.cache.enabled = True
        self.cache.directory = self.temporary.name
        self.cache._environment, self.cache._tree, self.cache._build = "environment", "tree", "build"

    def remembered(self):
        return [name for name in os.listdir(self.temporary.name) if name.endswith(".pass")]

    def testAPassIsRememberedAndReplayed(self):
        command = ["sh", "-c", "echo 'probe-gate: ok'"]
        self.assertEqual(self.cache.run("probe-gate", command)[0], 0)
        self.assertEqual(len(self.remembered()), 1)
        self.assertEqual(self.cache.run("probe-gate", command), (0, b"probe-gate: ok\n"))
        self.assertEqual(self.cache.reused, 1)

    def testAStandDownIsNotRemembered(self):
        command = ["sh", "-c", "echo 'probe-gate: SKIPPED \u2014 no build in this checkout'"]
        status, output = self.cache.run("probe-gate", command)
        self.assertEqual(status, 0)
        self.assertIn(b"SKIPPED", output)
        self.assertEqual(self.remembered(), [], "a gate that checked nothing has no verdict to replay")
        self.cache.run("probe-gate", command)
        self.assertEqual(self.cache.reused, 0, "so the next run asks again")

    def testAStandDownInsideAnotherGatesOutputIsNotRemembered(self):
        # check-style's own sections run gates that run gates: a stand-down two
        # levels down still means that part checked nothing.
        command = ["sh", "-c", "echo 'section: ok'; echo '  check-macos-smokes: SKIPPED \u2014 no app'"]
        self.cache.run("probe-gate", command)
        self.assertEqual(self.remembered(), [])


class HostTests(unittest.TestCase):
    def setUp(self):
        self.tool = load("host-tests.py")

    def testSelectionReadsCtestNamesAndDropsTheFixture(self):
        listing = "Test project /x\n  Test  #21: test_sixel_lifecycle\n  Test #469: ubq_build\n  Test #501: shell_emacs_snapshot_decode\n\nTotal Tests: 3\n"
        with mock.patch.object(subprocess, "run", return_value=Completed(listing)):
            names = self.tool.selectedTests(Path("/x"), "sixel|emacs")
        self.assertEqual(names, ["test_sixel_lifecycle", "shell_emacs_snapshot_decode"])

    def testSelectionOfNothingIsEmpty(self):
        with mock.patch.object(subprocess, "run", return_value=Completed("Test project /x\nTotal Tests: 0\n")):
            self.assertEqual(self.tool.selectedTests(Path("/x"), "zzz"), [])

    def testACacheBelongsHereOnlyWhenItsHomeIsThisCheckout(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            self.assertFalse(self.tool.cacheBelongsHere(cache))  # absent
            cache.write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/somewhere/else\n")
            self.assertFalse(self.tool.cacheBelongsHere(cache))  # another checkout's
            cache.write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n")
            self.assertTrue(self.tool.cacheBelongsHere(cache))

    def testTheQuicPrefixIsThisCheckoutsBuildRoot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "build root"
            environment = {key: value for key, value in os.environ.items() if key != "UBIQUITTY_QUIC_PREFIX"}
            with mock.patch.dict(os.environ, environment, clear=True), \
                 mock.patch.object(self.tool, "buildRoot", return_value=root):
                self.assertEqual(self.tool.quicPrefix(), root / "quic-deps")

    def testTheQuicPrefixNeverReadsAnotherWorktree(self):
        # A fresh checkout handed the main worktree's in-tree copy would build
        # or not depending on what another checkout held.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "build root"
            environment = {key: value for key, value in os.environ.items() if key != "UBIQUITTY_QUIC_PREFIX"}
            with mock.patch.dict(os.environ, environment, clear=True), \
                 mock.patch.object(self.tool, "buildRoot", return_value=root), \
                 mock.patch.object(subprocess, "run", side_effect=AssertionError("asked git for other worktrees")):
                self.assertEqual(self.tool.quicPrefix(), root / "quic-deps")

    def testAnExplicitQuicPrefixWins(self):
        with mock.patch.dict(os.environ, {"UBIQUITTY_QUIC_PREFIX": "/elsewhere/quic-deps"}):
            self.assertEqual(self.tool.quicPrefix(), Path("/elsewhere/quic-deps"))


class FreshCheckoutQUICTests(unittest.TestCase):
    """A fresh checkout builds its QUIC dependencies once, outside the tree.

    CMake's default prefix sat inside the source tree while the build scripts
    named the build root, so a fresh checkout built AWS-LC and ngtcp2 twice and
    wrote one copy into the tree.
    """

    def configure(self, buildRoot, *arguments):
        if shutil.which("cmake") is None or shutil.which("ninja") is None:
            self.skipTest("cmake and ninja are required")
        build = Path(buildRoot) / "core"
        environment = {**os.environ, "UBQ_BUILD_ROOT": str(buildRoot)}
        environment.pop("UBIQUITTY_QUIC_PREFIX", None)
        result = subprocess.run(
            ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja", "-DUBIQUITTY_FRONTEND=Core",
             "-DUBIQUITTY_QUIC=OFF", *arguments],
            env=environment, capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-2000:])
        for line in (build / "CMakeCache.txt").read_text().splitlines():
            if line.startswith("UBIQUITTY_QUIC_PREFIX:"):
                return line.split("=", 1)[1]
        self.fail("the cache names no UBIQUITTY_QUIC_PREFIX")

    def testTheDefaultPrefixIsTheBuildRoots(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(self.configure(directory), str(Path(directory) / "quic-deps"))

    def testAnExplicitPrefixIsKept(self):
        with tempfile.TemporaryDirectory() as directory:
            chosen = str(Path(directory) / "chosen prefix")
            self.assertEqual(self.configure(directory, f"-DUBIQUITTY_QUIC_PREFIX={chosen}"), chosen)

    def testEveryBuildScriptLeavesThePrefixToTheBuildRoot(self):
        # A script that hard-codes a prefix in the tree reintroduces the second
        # copy; one that names nothing takes CMake's default, which is right.
        for script in sorted((ROOT / "Scripts").glob("*.sh")):
            text = script.read_text()
            with self.subTest(script=script.name):
                self.assertNotRegex(text, r"-DUBIQUITTY_QUIC_PREFIX=[\"']?\$\{?ROOT\}?/build/")

    def testAFailedFetchSaysWhatAFreshCheckoutNeeds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            executables = root / "bin"
            executables.mkdir()
            for tool in ("dirname", "sed", "uname", "cmake", "rm", "mkdir", "sysctl", "xcrun"):
                found = shutil.which(tool)
                if found:
                    (executables / tool).symlink_to(found)
            if not (executables / "cmake").exists():
                self.skipTest("cmake is unavailable")
            (executables / "go").write_text("#!/bin/sh\nexit 0\n")
            (executables / "go").chmod(0o755)
            realGit = shutil.which("git")
            # Every git command works except reaching the network.
            (executables / "git").write_text(
                "#!/bin/sh\n"
                "for argument in \"$@\"; do [ \"$argument\" = fetch ] && { echo 'fatal: unable to access' >&2; exit 128; }; done\n"
                f"exec {realGit} \"$@\"\n")
            (executables / "git").chmod(0o755)
            environment = {**os.environ, "PATH": str(executables), "UBQ_BUILD_ROOT": str(root / "build")}
            prefix = root / "quic-deps"
            checked = subprocess.run(["/bin/bash", str(ROOT / "Scripts/fetch-quic-deps.sh"), str(prefix)],
                                     env=environment, text=True, capture_output=True, timeout=60)
            self.assertNotEqual(checked.returncode, 0)
            self.assertIn("could not fetch AWS-LC", checked.stderr)
            self.assertIn("needs network access", checked.stderr)
            self.assertIn("UBIQUITTY_QUIC_PREFIX", checked.stderr)


class RemintFingerprint(unittest.TestCase):
    def setUp(self):
        self.tool = load("remint-fingerprint.py")

    def testAMovedBodyIsReadWithItsNumbers(self):
        report = "snapshotVersion: moved 16 -> 17; re-mint with length=4252 digest=0xbe1b796b3af52950\n"
        self.assertEqual(self.tool.MOVED.findall(report), [("snapshotVersion", "16", "17", "4252", "0xbe1b796b3af52950")])
        self.assertEqual(self.tool.STOOD_STILL.findall(report), [])

    def testAStandingVersionIsReadAsARefusal(self):
        report = "imageBodyVersion: the body changed but the version is still 2.\n  recorded: length=140 digest=0x8e1f\n"
        self.assertEqual(self.tool.STOOD_STILL.findall(report), [("imageBodyVersion", "2")])
        self.assertEqual(self.tool.MOVED.findall(report), [])

    def testTheRecordLineMatchesItsOwnBodyOnly(self):
        source = ('    checkFingerprint({ "snapshotVersion", 16, 4252, 0x65e1074ae1523b4full }, snapshotVersion, first);\n'
                  '    checkFingerprint({ "imageBodyVersion", 2, 149, 0xb8480677da3d9d16ull }, imageBodyVersion, first);\n')
        line = self.tool.recordLine("snapshotVersion")
        self.assertEqual(len(line.findall(source)), 1)
        rewritten = line.sub('checkFingerprint({ "snapshotVersion", 17, 4252, 0xbe1b796b3af52950ull }', source)
        self.assertIn('"snapshotVersion", 17, 4252, 0xbe1b796b3af52950ull', rewritten)
        self.assertIn('"imageBodyVersion", 2, 149, 0xb8480677da3d9d16ull', rewritten)  # the neighbour is untouched
        self.assertEqual(len(self.tool.recordLine("scrollbackVersion").findall(source)), 0)

    def testAMovedBodyIsRecordedAndAStandingOneRefused(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test_sync_wire_fingerprint.cpp"
            source.write_text('    checkFingerprint({ "snapshotVersion", 16, 4252, 0x65e1074ae1523b4full }, snapshotVersion, first);\n')
            reports = iter([
                (1, "snapshotVersion: moved 16 -> 17; re-mint with length=4260 digest=0x1234\n"
                    "configVersion: the body changed but the version is still 1.\n"),
                (0, ""),
            ])
            with mock.patch.object(self.tool, "TEST_SOURCE", source), \
                 mock.patch.object(self.tool, "buildAndRun", side_effect=lambda: next(reports)), \
                 mock.patch.object(sys, "argv", ["remint-fingerprint.py"]):
                status = self.tool.main()
            self.assertEqual(status, 1)  # the standing body keeps the run red
            self.assertIn('"snapshotVersion", 17, 4260, 0x1234ull', source.read_text())

    def testNothingMovedIsNothingToDo(self):
        with mock.patch.object(self.tool, "buildAndRun", return_value=(0, "")), mock.patch.object(sys, "argv", ["remint-fingerprint.py"]):
            self.assertEqual(self.tool.main(), 0)

    def testAnUnnamedFailureIsNotSwallowed(self):
        with mock.patch.object(self.tool, "buildAndRun", return_value=(1, "FAIL something unrelated\n")), mock.patch.object(sys, "argv", ["remint-fingerprint.py"]):
            self.assertEqual(self.tool.main(), 1)


if __name__ == "__main__":
    os.chdir(ROOT)
    unittest.main()
