#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercise the shell test driver's public namespace without building targets."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class TestDriverTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ubiquitty-driver-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.repository = self.directory / "checkout with spaces"
        self.repository.mkdir()
        for name in ("Scripts", "Tests"):
            destination = self.repository / name
            destination.mkdir()
            for source in (ROOT / name).iterdir():
                if source.name not in ("run-tests.sh", "test-gate-report.sh"):
                    (destination / source.name).symlink_to(source, target_is_directory=source.is_dir())
        (self.repository / "Source").symlink_to(ROOT / "Source", target_is_directory=True)
        self.driver = self.repository / "Scripts/run-tests.sh"
        shutil.copyfile(ROOT / "Scripts/run-tests.sh", self.driver)
        self.stanzaTrace = self.directory / "stanza-ran"
        stanza = self.repository / "Tests/test-gate-report.sh"
        stanza.write_text('#!/bin/bash\nprintf "gate_report\\n" >> "$DRIVER_STANZA_TRACE"\n')
        stanza.chmod(0o755)

        self.tools = self.directory / "tools"
        self.tools.mkdir()
        self.toolTrace = self.directory / "tools-ran"
        for name in ("cmake", "ctest", "cc", "c++", "clang", "clang++", "ninja", "ccache"):
            tool = self.tools / name
            tool.write_text(
                f"#!{sys.executable}\n"
                "import json, os, sys\n"
                "with open(os.environ['DRIVER_TOOL_TRACE'], 'a') as output:\n"
                "    output.write(json.dumps(sys.argv) + '\\n')\n"
                "sys.exit(0 if os.environ.get('DRIVER_DIAGNOSTIC_ONLY') == '1' else 97)\n")
            tool.chmod(0o755)

        self.buildRoot = self.directory / "external build"
        self.output = self.directory / "recipe output"
        self.cache = self.buildRoot / "existing/CMakeCache.txt"
        self.cache.parent.mkdir(parents=True)
        self.cache.write_text("CMAKE_CACHEFILE_DIR:INTERNAL=/another/checkout\n")
        self.environment = dict(os.environ)
        for name in ("CTEST_BUILD_DIR", "UBQ_OUT", "NO_CTEST", "SKIP_CTEST_COVERED", "FUZZ_REQUIRED",
                     "UBQ_HOST_TEST_TREE", "UBQ_TEST_TARGETS_PREBUILT", "UBQ_LIBCORE_PREBUILT",
                     "UBQ_SHARED_BINARIES_FROM_FIXTURES", "UBQ_HOST_PREBUILT", "UBQ_AGENT_PREBUILT",
                     "UBQ_HOST_TEST_BUILD", "UBQ_BIN", "UBIQUITTY_HOST_BIN", "UBQ_SNAPSHOT_RENDERER_PREBUILT"):
            self.environment.pop(name, None)
        self.environment.update(
            PATH=str(self.tools) + os.pathsep + os.environ.get("PATH", ""),
            UBQ_BUILD_ROOT=str(self.buildRoot), UBQ_OUT=str(self.output),
            UBQ_BUILD_JOBS="1", UBQ_COMPILER_LAUNCHER="", UBQ_HOOKS_REPORTED="1",
            DRIVER_TOOL_TRACE=str(self.toolTrace), DRIVER_STANZA_TRACE=str(self.stanzaTrace),
            CC="cc", CXX="c++", PYTHONDONTWRITEBYTECODE="1")

    def runDriver(self, *arguments):
        return subprocess.run(
            ["/bin/bash", str(self.driver), *arguments], cwd=self.repository,
            env=self.environment, capture_output=True, text=True, timeout=20, check=False)

    def assertReadOnly(self, result):
        self.assertTrue(self.cache.is_file(), result.stdout + result.stderr)
        self.assertEqual(self.cache.read_text(), "CMAKE_CACHEFILE_DIR:INTERNAL=/another/checkout\n")
        self.assertFalse(self.output.exists(), "the driver generated recipe output")
        self.assertFalse(self.toolTrace.exists(), "the driver invoked a build or test tool")
        self.assertFalse(self.stanzaTrace.exists(), "the driver executed a stanza")
        self.assertNotIn("=== ", result.stdout)

    def sharedTarget(self, status=0):
        tree = self.directory / "shared test tree"
        binary = tree / "Tests/test_display"
        binary.parent.mkdir(parents=True)
        binary.write_text(f'#!/bin/bash\nprintf "shared-target\\n" >> "$DRIVER_STANZA_TRACE"\nexit {status}\n')
        binary.chmod(0o755)
        self.environment.update(UBQ_HOST_TEST_TREE=str(tree), UBQ_CACHE_SWEEP_COMPLETE="1")
        return binary

    def testPrebuiltSharedTargetRunsWithoutBuildTools(self):
        binary = self.sharedTarget()
        self.environment["UBQ_TEST_TARGETS_PREBUILT"] = "1"
        result = self.runDriver("test_display")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.stanzaTrace.read_text(), "shared-target\n")
        self.assertEqual((self.output / "test_display").resolve(), binary.resolve())
        self.assertFalse(self.toolTrace.exists(), "the prebuilt stanza invoked a build tool")

    def testMissingSharedTargetCannotRunAnOldScratchBinary(self):
        binary = self.sharedTarget()
        binary.unlink()
        self.output.mkdir()
        stale = self.output / "test_display"
        stale.write_text('#!/bin/bash\nprintf "stale\\n" >> "$DRIVER_STANZA_TRACE"\n')
        stale.chmod(0o755)
        self.environment["UBQ_TEST_TARGETS_PREBUILT"] = "1"
        result = self.runDriver("test_display")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("shared test target is missing", result.stderr)
        self.assertFalse(self.stanzaTrace.exists(), "the stanza ran its stale scratch binary")
        self.assertFalse(self.toolTrace.exists(), "the stanza ignored the fixture's failed promise")

    def testSharedTreePathAloneDoesNotPromiseABuiltTarget(self):
        binary = self.sharedTarget()
        tree = binary.parent.parent
        (tree / "CMakeCache.txt").write_text("fixture cache\n")
        result = self.runDriver("test_display")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = [json.loads(line)[1:] for line in self.toolTrace.read_text().splitlines()]
        self.assertIn(["--build", str(tree), "--target", "test_display", "-j1"], calls)
        self.assertFalse(self.stanzaTrace.exists(), "the stanza ran after its build failed")

    def testPrebuiltSharedTargetStillPropagatesTestFailure(self):
        self.sharedTarget(status=13)
        self.environment["UBQ_TEST_TARGETS_PREBUILT"] = "1"
        result = self.runDriver("test_display")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.stanzaTrace.read_text(), "shared-target\n")
        self.assertFalse(self.toolTrace.exists())

    def testHelpDescribesSeparateNamespacesAndRealStanzaExamples(self):
        result = self.runDriver("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertReadOnly(result)
        listing = self.runDriver("--list-stanzas")
        self.assertEqual(listing.returncode, 0, listing.stderr)
        names = set(listing.stdout.splitlines())
        examples = []
        for line in result.stdout.splitlines():
            command = re.split(r"\s{2,}", line.strip(), maxsplit=1)[0]
            words = command.split()
            if words and words[0] == "./Scripts/run-tests.sh" and len(words) > 1 and not words[1].startswith("-"):
                examples.append(words[1:])
                self.assertTrue(set(words[1:]).issubset(names), command)
        self.assertGreaterEqual(len(examples), 2)
        self.assertIn("different namespaces", result.stdout)
        self.assertIn("--list-stanzas", result.stdout)
        self.assertIn('CTEST_BUILD_DIR="$(python3 Scripts/build_root.py .)/BuildHostTests/ctest"', result.stdout)
        self.assertIn('-DUBIQUITTY_FRONTEND=Core -DUBIQUITTY_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug', result.stdout)
        self.assertIn('ctest --test-dir "$CTEST_BUILD_DIR" -N', result.stdout)
        self.assertIn("-R '^test_selection$'", result.stdout)
        self.assertNotIn("test_selection", names)
        self.assertTrue((ROOT / "Tests/Core/Selection/test_selection.cpp").is_file())
        self.assertIn("SKIP", result.stdout)
        self.assertIn("FUZZ_REQUIRED=1", result.stdout)
        self.assertNotIn("silently skipped", result.stdout)

    def testEveryDiscoveryModeLeavesCachesAndOutputUntouched(self):
        for argument in ("--help", "-h", "--list-stanzas", "--list-serial-stanzas", "--list-host-binary-stanzas",
                         "--list-agent-binary-stanzas"):
            with self.subTest(argument=argument):
                result = self.runDriver(argument)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.strip())
                self.assertReadOnly(result)

    def testDiscoveryReadsNestedStanzasFromTheDispatcher(self):
        source = self.driver.read_text()
        self.driver.write_text(source + "\nif false; then\n    if wanted driver_discovery_probe; then :; fi\nfi\n")
        result = self.runDriver("--list-stanzas")
        self.assertEqual(result.returncode, 0, result.stderr)
        names = result.stdout.splitlines()
        self.assertIn("driver_discovery_probe", names)
        self.assertEqual(names, sorted(set(names)))
        self.assertReadOnly(result)

    def usePlatform(self, platform):
        uname = self.tools / "uname"
        uname.write_text(f'#!/bin/bash\nif [ "$1" = "-s" ]; then echo "{platform}"; else /usr/bin/uname "$@"; fi\n')
        uname.chmod(0o755)

    def testGuardedNamesAreDiscoveredOnlyOnTheirPlatform(self):
        source = self.driver.read_text()
        guarded = {line.split("wanted ", 1)[1].split(";", 1)[0]
                   for line in source.splitlines()
                   if line.startswith('if [ "$(uname -s)" = "Darwin" ] && wanted ')}
        # The floor is what the driver carries, and it is here because a guard
        # spelled differently would match nothing and leave every assertion
        # below vacuously true. A stanza that becomes portable, or moves into
        # the CMake suite, lowers it; a Darwin-only one raises it.
        self.assertGreaterEqual(len(guarded), 9)
        self.driver.write_text(source + '\nif false; then\n'
                               '    if [ "$(uname -s)" = "Darwin" ] && wanted driver_guarded_probe; then :; fi\nfi\n')
        guarded.add("driver_guarded_probe")
        for platform in ("Darwin", "Linux"):
            with self.subTest(platform=platform):
                self.usePlatform(platform)
                result = self.runDriver("--list-stanzas")
                self.assertEqual(result.returncode, 0, result.stderr)
                names = set(result.stdout.splitlines())
                self.assertIn("gate_report", names)
                self.assertEqual(guarded & names, guarded if platform == "Darwin" else set())
                self.assertReadOnly(result)

    def testGuardedStanzaPassesPreflightAndExecutes(self):
        self.usePlatform("Darwin")
        source = self.driver.read_text()
        declaration = "if wanted gate_report; then"
        self.assertEqual(source.count(declaration), 1)
        self.driver.write_text(source.replace(
            declaration, 'if [ "$(uname -s)" = "Darwin" ] && wanted driver_guarded_probe; then'))
        result = self.runDriver("driver_guarded_probe")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.stanzaTrace.read_text(), "gate_report\n")
        self.assertIn("PASSED — 1 ran, 0 skipped", result.stdout)
        self.assertFalse(self.toolTrace.exists(), result.stdout + result.stderr)

    def testGuardedDeclarationsOwnTheirBuildLocksAndHostDependencies(self):
        self.driver.write_text(self.driver.read_text() + '''
if false; then
    if wanted driver_previous_probe; then :; fi
    if [ "$(uname -s)" = "Darwin" ] && wanted driver_guarded_probe; then
        : "$UBQ_BUILD_ROOT/guarded-probe"
        : "$(ubq_host_binary)"
    fi
fi
''')
        serial = self.runDriver("--list-serial-stanzas")
        self.assertEqual(serial.returncode, 0, serial.stderr)
        self.assertIn("driver_guarded_probe guarded-probe", serial.stdout.splitlines())
        self.assertNotIn("driver_previous_probe guarded-probe", serial.stdout.splitlines())
        host = self.runDriver("--list-host-binary-stanzas")
        self.assertEqual(host.returncode, 0, host.stderr)
        self.assertIn("driver_guarded_probe", host.stdout.splitlines())
        self.assertNotIn("driver_previous_probe", host.stdout.splitlines())
        self.assertReadOnly(serial)
        self.assertReadOnly(host)

    # How the dispatcher opens a stanza, as Scripts/stanza-declarations.awk reads it.
    STANZA_OPENING = re.compile(
        r'^[ \t]*(?:build_and_run|if (?:\[ "\$\(uname -s\)" = "Darwin" \] && )?wanted) ([A-Za-z0-9_-]+)')

    # The variables a stanza holds the agent and the Host frontend in, by the
    # inventory that orders it after the fixture building each.
    SHARED_BINARY_USES = {
        "--list-agent-binary-stanzas": re.compile(r"\$\{?AGENT_BINARY\b"),
        "--list-host-binary-stanzas": re.compile(r"\$\{?(?:AGENT_HOST|HOST_BINARY|HOST_TEST_BUILD)\b"),
    }

    def testEveryStanzaUsingASharedBinaryIsOrderedAfterItsFixture(self):
        # A stanza reading one of these without asking for the binary in its
        # own body was handed it by an enclosing block or an earlier stanza.
        # The inventories read the call, so such a stanza is not ordered after
        # the fixture, and under ctest it would build the binary under the
        # stanzas executing it.
        bodies, current = {}, None
        for line in self.driver.read_text().splitlines():
            opening = self.STANZA_OPENING.match(line)
            if opening:
                current = opening.group(1)
            if current is not None and not line.lstrip().startswith("#"):
                bodies.setdefault(current, []).append(line)
        for inventory, use in self.SHARED_BINARY_USES.items():
            with self.subTest(inventory=inventory):
                listed = self.runDriver(inventory)
                self.assertEqual(listed.returncode, 0, listed.stderr)
                self.assertReadOnly(listed)
                users = {name for name, body in bodies.items() if any(use.search(line) for line in body)}
                # Not vacuous: a stanza the driver carries uses both binaries.
                self.assertIn("ubq_smoke_cleanup", users)
                self.assertEqual(sorted(users - set(listed.stdout.splitlines())), [])

    # Each shared binary: the helper a stanza asks through, the promise ctest
    # makes to the stanzas it orders after the fixture, where the fixture puts
    # the binary, the fixture, and what the helper's own build configures.
    SHARED_BINARIES = (
        ("ubq_agent_binary", "UBQ_AGENT_PREBUILT", "agent/ubq", "agent_binary",
         "-DUBIQUITTY_FRONTEND=Agent"),
        ("ubq_host_binary", "UBQ_HOST_PREBUILT", "host-tests/ubiquitty-host", "host_binary",
         "-DUBIQUITTY_FRONTEND=Host"),
    )

    def runBinaryProbe(self, helper, **environment):
        """Runs a stanza that asks `helper` for its binary and records the path.

        The stanza goes ahead of one the driver carries, so the filter check
        after the last stanza sees it answer.
        """
        source = (ROOT / "Scripts/run-tests.sh").read_text()
        declaration = "if wanted gate_report; then"
        self.assertEqual(source.count(declaration), 1)
        probe = ('if wanted driver_binary_probe; then\n'
                 '    echo "=== driver_binary_probe ==="\n'
                 '    RAN=$((RAN + 1))\n'
                 f'    driver_probe_binary="$({helper} "$OUT/driver-binary-probe.log")" || exit 3\n'
                 '    printf \'%s\\n\' "$driver_probe_binary" >> "$DRIVER_STANZA_TRACE"\n'
                 'fi\n\n')
        self.driver.write_text(source.replace(declaration, probe + declaration))
        for trace in (self.toolTrace, self.stanzaTrace):
            trace.unlink(missing_ok=True)
        probeEnvironment = {**self.environment, **environment}
        for name in ("LLVM_PROFILE_FILE", "UBQ_COVERAGE", "UBQ_COVERAGE_BUILD_FIXTURE"):
            probeEnvironment.pop(name, None)
        return subprocess.run(
            ["/bin/bash", str(self.driver), "driver_binary_probe"], cwd=self.repository,
            env=probeEnvironment, capture_output=True, text=True, timeout=20, check=False)

    def testAStanzaCTestDidNotOrderRefusesToBuildASharedBinary(self):
        for helper, _, binary, fixture, _ in self.SHARED_BINARIES:
            with self.subTest(helper=helper):
                result = self.runBinaryProbe(helper, UBQ_SHARED_BINARIES_FROM_FIXTURES="1")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"the {fixture} fixture builds {self.buildRoot / binary}", result.stderr)
                self.assertFalse(self.toolTrace.exists(), "the unordered stanza started a build")
                self.assertFalse(self.stanzaTrace.exists(), "the unordered stanza ran without its binary")

    def testAStanzaCTestOrderedReadsTheFixturesBinary(self):
        for helper, promise, binary, _, _ in self.SHARED_BINARIES:
            with self.subTest(helper=helper):
                built = self.buildRoot / binary
                built.parent.mkdir(parents=True, exist_ok=True)
                built.write_text("#!/bin/bash\n")
                built.chmod(0o755)
                result = self.runBinaryProbe(helper, UBQ_SHARED_BINARIES_FROM_FIXTURES="1", **{promise: "1"})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(self.stanzaTrace.read_text(), f"{built}\n")
                self.assertFalse(self.toolTrace.exists(), "the ordered stanza started a build")

    def testAStanzaRunAloneBuildsItsSharedBinary(self):
        for helper, _, _, _, configure in self.SHARED_BINARIES:
            with self.subTest(helper=helper):
                result = self.runBinaryProbe(helper)
                # The stand-in cmake fails, so the stanza stops at its build.
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(self.toolTrace.exists(), "the stanza run alone did not build")
                calls = [json.loads(line)[1:] for line in self.toolTrace.read_text().splitlines()]
                self.assertTrue(any(configure in call for call in calls), calls)
                self.assertFalse(self.stanzaTrace.exists(), "the stanza ran without its binary")

    def standInSnapshotRenderer(self, current):
        """Replaces the portable renderer's freshness check, build, and render.

        Each records itself in one trace, in order. The check answers from a
        marker file, the build writes that marker, and a render records whether
        the marker was there, so a stanza that rendered a stale renderer, built
        one, or refused leaves a different trace.

        - Parameter current: Whether the renderer starts out current.
        - Returns: The trace's path.
        """
        trace = self.directory / "renderer-ran"
        marker = self.directory / "renderer-current"
        if current:
            marker.touch()
        scripts = self.repository / "Scripts"
        for name in ("snapshot-binary-is-current.py", "build-core-snapshot.sh",
                     "check-snapshot-prerequisites.py"):
            (scripts / name).unlink()
        (scripts / "snapshot-binary-is-current.py").write_text(
            "import os, sys\n"
            "with open(os.environ['DRIVER_RENDERER_TRACE'], 'a') as trace:\n"
            "    trace.write('check\\n')\n"
            "if os.path.exists(os.environ['DRIVER_RENDERER_CURRENT']):\n"
            "    sys.exit(0)\n"
            "print('Source/Core/Probe.cpp is newer than ubiquitty-snapshot')\n"
            "sys.exit(1)\n")
        build = scripts / "build-core-snapshot.sh"
        build.write_text('#!/bin/bash\nprintf "build\\n" >> "$DRIVER_RENDERER_TRACE"\n'
                         ': > "$DRIVER_RENDERER_CURRENT"\n')
        build.chmod(0o755)
        prerequisites = scripts / "check-snapshot-prerequisites.py"
        prerequisites.write_text(f"#!{sys.executable}\nimport sys\nsys.exit(0)\n")
        prerequisites.chmod(0o755)
        snapshots = self.repository / "Tests/Snapshots"
        snapshots.unlink()
        snapshots.mkdir()
        (snapshots / "run.py").write_text(
            "import os, sys\n"
            "backend = sys.argv[sys.argv.index('--backend') + 1]\n"
            "state = '' if os.path.exists(os.environ['DRIVER_RENDERER_CURRENT']) else ' stale'\n"
            "with open(os.environ['DRIVER_RENDERER_TRACE'], 'a') as trace:\n"
            "    trace.write(f'render {backend}{state}\\n')\n")
        self.environment.pop("SNAPSHOT", None)
        self.environment.update(DRIVER_RENDERER_TRACE=str(trace), DRIVER_RENDERER_CURRENT=str(marker),
                                UBQ_CACHE_SWEEP_COMPLETE="1")
        return trace

    def rendererTrace(self, trace):
        return trace.read_text().splitlines() if trace.exists() else []

    def testSnapshotRendererFixtureBuildsAStaleRenderer(self):
        trace = self.standInSnapshotRenderer(current=False)
        result = self.runDriver("snapshot_renderer_fixture")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.rendererTrace(trace), ["check", "build"])
        self.assertIn("PASSED — 1 ran, 0 skipped", result.stdout)
        self.assertFalse(self.toolTrace.exists(), result.stdout + result.stderr)

    def testSnapshotRendererFixtureLeavesACurrentRendererAlone(self):
        trace = self.standInSnapshotRenderer(current=True)
        result = self.runDriver("snapshot_renderer_fixture")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.rendererTrace(trace), ["check"])

    def testSnapshotRendererFixtureDeclinesWhenSnapshotsAreDisabled(self):
        trace = self.standInSnapshotRenderer(current=False)
        self.environment["SNAPSHOT"] = "skip"
        result = self.runDriver("snapshot_renderer_fixture")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # The line ctest's decline expression reads as Skipped.
        self.assertIn("PASSED — 0 ran, 1 skipped", result.stdout)
        self.assertEqual(self.rendererTrace(trace), [])

    def testSnapshotsOrderedAfterTheFixtureRefusesAStaleRendererWithoutBuilding(self):
        trace = self.standInSnapshotRenderer(current=False)
        self.environment["UBQ_SNAPSHOT_RENDERER_PREBUILT"] = "1"
        result = self.runDriver("snapshots")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(self.rendererTrace(trace), ["check"])
        self.assertIn("BUILD-BROKEN: snapshots:", result.stdout)
        self.assertIn("snapshot_renderer_fixture", result.stderr)
        self.assertIn("Source/Core/Probe.cpp is newer", result.stderr)
        self.assertFalse(self.toolTrace.exists(), result.stdout + result.stderr)

    def testSnapshotsOrderedAfterTheFixtureRendersACurrentRenderer(self):
        trace = self.standInSnapshotRenderer(current=True)
        self.environment["UBQ_SNAPSHOT_RENDERER_PREBUILT"] = "1"
        result = self.runDriver("snapshots")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.rendererTrace(trace), ["check", "render portable"])
        self.assertFalse(self.toolTrace.exists(), result.stdout + result.stderr)

    def testSnapshotsRunAloneRebuildsAStaleRendererBeforeRendering(self):
        trace = self.standInSnapshotRenderer(current=False)
        result = self.runDriver("snapshots")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.rendererTrace(trace), ["check", "build", "render portable"])

    def testCTestOrdersSnapshotsAfterTheRendererFixture(self):
        # A plain full run has no ctest ordering, so the declaration order is
        # what builds the renderer before the stanza renders with it.
        driver = self.driver.read_text()
        self.assertLess(driver.index("if wanted snapshot_renderer_fixture; then"),
                        driver.index('if wanted snapshots && [ "${SNAPSHOT:-}" != "skip" ]; then'))
        # The wiring is run-tests.sh's, set where its test tree schedules it;
        # the timeout every other stanza gets is the module's.
        cmake = (ROOT / "Tests/CMakeLists.txt").read_text()
        module = (ROOT / "Scripts/cmake/UbiquittyTests.cmake").read_text()
        self.assertRegex(cmake, r'_stanza STREQUAL "snapshot_renderer_fixture"\)\s+'
                                r'set\(\$\{_entry\}_JOBS \$\{UBIQUITTY_HOST_FIXTURE_JOBS\}\)')
        self.assertRegex(cmake, r'set\(_snapshot_renderer_stanzas snapshots\)')
        self.assertRegex(cmake, r'elseif\(_stanza IN_LIST _snapshot_renderer_stanzas\)\s+'
                                r'list\(APPEND \$\{_entry\}_ENVIRONMENT UBQ_SNAPSHOT_RENDERER_PREBUILT=1\)')
        self.assertRegex(cmake, r'if\(_stanza IN_LIST _snapshot_renderer_stanzas\)\s+'
                                r'list\(APPEND \$\{_entry\}_FIXTURES_REQUIRED ubq_snapshot_renderer_fixture\)')
        self.assertIn("set(shell_snapshot_renderer_fixture_FIXTURES_SETUP ubq_snapshot_renderer_fixture)", cmake)
        setup = re.search(r'set\(shell_snapshot_renderer_fixture_TIMEOUT (\d+)\)', cmake)
        self.assertIsNotNone(setup)
        stanza = re.search(r'set\(_ubq_stanza_timeout (\d+)\)', module)
        self.assertIsNotNone(stanza)
        self.assertGreater(int(setup.group(1)), int(stanza.group(1)))

    def testUnknownAndCoreFiltersRefuseBeforeAnyExecution(self):
        for name in ("not_a_real_stanza", "test_selection", "test_width", "test_vt100_cursor"):
            with self.subTest(name=name):
                result = self.runDriver(name)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("not a stanza name:", result.stderr)
                self.assertIn(name, result.stderr)
                self.assertIn("--list-stanzas", result.stderr)
                self.assertNotIn("PASSED", result.stdout)
                self.assertReadOnly(result)

    def testMixedUnknownFiltersCannotExecuteTheValidPortion(self):
        for arguments in (("gate_report", "test_selection"), ("not_a_stanza", "gate_report"),
                          ("gate_report", "not_a_stanza", "test_width")):
            with self.subTest(arguments=arguments):
                result = self.runDriver(*arguments)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertReadOnly(result)

    def testMalformedFiltersAndFlagsDoNotExecute(self):
        for name, status in (("gate_report\nnot_a_stanza", 1), ("", 1), ("--not-an-option", 2)):
            with self.subTest(name=name):
                result = self.runDriver(name)
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                self.assertReadOnly(result)

    def testAValidStanzaStillExecutesAloneWithoutCTest(self):
        result = self.runDriver("gate_report")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.stanzaTrace.read_text(), "gate_report\n")
        self.assertIn("PASSED — 1 ran, 0 skipped", result.stdout)
        self.assertFalse(self.toolTrace.exists(), result.stdout + result.stderr)

    def testLargeInventoryAcceptsAnEarlyValidNameAndRefusesUnknownNames(self):
        declarations = "".join(f"    if wanted zz_inventory_probe_{index:05d}; then :; fi\n"
                               for index in range(65536))
        self.driver.write_text(self.driver.read_text() + "\nif false; then\n" + declarations + "fi\n")

        result = self.runDriver("not_a_stanza")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("not a stanza name:", result.stderr)
        self.assertReadOnly(result)

        result = self.runDriver("gate_report")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.stanzaTrace.read_text(), "gate_report\n")
        self.assertIn("PASSED — 1 ran, 0 skipped", result.stdout)
        self.assertFalse(self.toolTrace.exists(), result.stdout + result.stderr)

    def diagnosticArguments(self, result):
        commands = [line.strip() for line in result.stderr.splitlines()
                    if line.startswith("    cmake ") or line.startswith("    ctest ")]
        self.assertEqual(len(commands), 3, result.stderr)
        environment = dict(self.environment, DRIVER_DIAGNOSTIC_ONLY="1")
        for command in commands:
            invoked = subprocess.run(["/bin/bash", "-c", command], cwd=self.repository,
                                     env=environment, capture_output=True, text=True, timeout=10, check=False)
            self.assertEqual(invoked.returncode, 0, invoked.stderr)
        return [json.loads(line)[1:] for line in self.toolTrace.read_text().splitlines()]

    def assertDiagnosticBuildTree(self, expected):
        result = self.runDriver("test_selection")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertReadOnly(result)
        configure, discover, select = self.diagnosticArguments(result)
        self.assertEqual(configure, ["-S", str(self.repository), "-B", str(expected), "-G", "Ninja",
                                    "-DUBIQUITTY_FRONTEND=Core", "-DUBIQUITTY_BUILD_TESTS=ON", "-DCMAKE_BUILD_TYPE=Debug"])
        self.assertEqual(discover, ["--test-dir", str(expected), "-N"])
        self.assertEqual(select, ["--test-dir", str(expected), "-R", "^<ctest-name>$", "--output-on-failure"])

    def testDiagnosticsUseTheConfiguredOutputDirectory(self):
        self.assertDiagnosticBuildTree(self.output / "ctest")

    def testDiagnosticsUseTheExternalDefaultBuildRoot(self):
        self.environment.pop("UBQ_OUT")
        self.assertDiagnosticBuildTree(self.buildRoot / "BuildHostTests/ctest")

    def testCustomBuildTreeIsSafelyQuotedAndOverridesOutput(self):
        custom = self.directory / "tree ' quote $(touch injected) ; $value"
        self.environment["CTEST_BUILD_DIR"] = str(custom)
        self.assertDiagnosticBuildTree(custom)
        self.assertFalse((self.repository / "injected").exists())
        self.assertFalse(custom.exists())


if __name__ == "__main__":
    unittest.main()
