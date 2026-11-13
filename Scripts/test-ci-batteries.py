#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises CI's command map, fail-fast accounting, and safe reach baselines.

This is called by the preflight battery; it is not an unregistered check-* gate.
The tests use fake executors/API replies and do not build or contact GitHub.
A run has an artifact root of its own (UBQ_CI_ROOT; see run_suite).
"""

from contextlib import redirect_stderr, redirect_stdout
from dataclasses import replace
from importlib import import_module
from importlib.util import module_from_spec, spec_from_file_location
import io
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import ci_batteries as batteries
import ci_reach as reach
import ctest_report as ctest_reporting
from ubq_roots import source_roots


WORKFLOW = None
if "--workflow" in sys.argv:
    index = sys.argv.index("--workflow")
    WORKFLOW = sys.argv[index + 1]
    del sys.argv[index:index + 2]

TEST_SOURCES = source_roots(source=Path(WORKFLOW).resolve().parents[2] if WORKFLOW else None)
# Core owns the smoke registry and host fixtures even when CI judges an App workflow.
CORE_TEST_SOURCES = source_roots(source=batteries.ROOT, core=batteries.ROOT, environment={})


def test_file(relative):
    return TEST_SOURCES.resolve_file(relative)


# The variables that name the library checkouts. A fixture of an app script
# binds Core through UBIQUITTY_CORE_SOURCE_DIR alone, so none of these may
# reach it from the run that started the test.
LIBRARY_ROOT_VARIABLES = ("UBQ_CORE_ROOT", "UBQ_KIT_ROOT", "UBIQUITTY_KIT_SOURCE_DIR")


def carry_library_resolver(scripts):
    """Copies the app's library resolver into a fixture's Scripts/ when the app has one.

    An app script that finds its libraries by sourcing Scripts/library-scripts.sh
    beside it needs that file beside its fixture copy.
    """
    try:
        resolver = test_file("Scripts/library-scripts.sh")
    except FileNotFoundError:
        return
    shutil.copyfile(resolver, scripts / "library-scripts.sh")


class BatteryTests(unittest.TestCase):
    def test_library_suites_have_one_repository_owner(self):
        core_only = {"fuzz", "pgo", "portable-build", "portable-snapshots",
                     "host-glibc", "agent-smokes", "host-musl", "sanitizers", "wasm-smoke"}
        linux = {stage.key: stage for stage in batteries.stages("linux")}
        self.assertTrue(core_only <= linux.keys())
        for key in core_only:
            self.assertEqual(linux[key].repositories, ("core",), key)
        self.assertEqual(linux["kit-build"].repositories, ("kit",))
        self.assertEqual(linux["host-tests"].repositories, ("core", "app"))
        apple = {stage.key: stage for stage in batteries.stages("apple")}
        for key in ("swift-renderer", "ios-protected-file"):
            self.assertEqual(apple[key].repositories, ("kit",), key)
        self.assertEqual(apple["pgo"].repositories, ("core",))
        for key in ("macos-build", "macos-smokes", "ios-build", "ios-smokes", "ios-ui"):
            self.assertEqual(apple[key].repositories, ("app",), key)

    def test_agent_smokes_are_required_after_the_host_build(self):
        stages = list(batteries.stages("linux"))
        keys = [stage.key for stage in stages]
        stage = stages[keys.index("agent-smokes")]
        self.assertGreater(keys.index("agent-smokes"), keys.index("host-glibc"))
        self.assertIn("./Scripts/build-agent.sh", stage.command)
        self.assertIn("ubiquitty-host ubq", stage.command)
        for name in ("agent_smoke", "agent_auth_smoke", "agent_mcp_smoke", "agent_completions_smoke", "ubq_smoke"):
            self.assertIn(name, stage.command)
        listed = subprocess.run(["bash", str(batteries.ROOT / "Scripts/run-tests.sh"), "--list-stanzas"],
                                cwd=CORE_TEST_SOURCES.subject, env=CORE_TEST_SOURCES.environment(os.environ),
                                capture_output=True, text=True, check=True)
        self.assertTrue(set(stage.command.split()[-4:]).issubset(set(listed.stdout.split())))

    def test_orchestration_negative_controls_do_not_publish_live_events(self):
        stage = next(stage for stage in batteries.stages("preflight")
                     if stage.key == "orchestration-tests")
        for variable in (
                "UBQ_BATTERY", "UBQ_CI_ANNOTATE", "UBQ_EVENT_LOG", "UBQ_RESULT_DIR",
                "UBQ_STAGE_KEY", "UBQ_STAGE_LABEL", "UBQ_STAGE_LOG_PATH",
                "UBQ_STAGE_JUNIT_PATH"):
            self.assertIn(variable, stage.command.splitlines()[0])

    def test_capability_map_preserves_enabled_commands(self):
        # Each row is an independently executable capability in the workflow.
        # Setup, cache diagnostics, and artifact publication are not tests.
        expected = {
            "preflight": {
                "style": "./Scripts/check-style.py",
                "capabilities": "./Scripts/check-capability-ssot.py",
                "terminfo": "./Scripts/check-terminfo.sh",
                "workflows": "./Scripts/check-workflows.sh",
                "build-dirs": "python3 ./Scripts/check-build-dirs.py .",
                "names": "python3 ./Scripts/check-name-collisions.py .",
                "forward-names": "python3 ./Scripts/check-name-collisions.py . --forward",
                "alias-names": "python3 ./Scripts/check-name-collisions.py . --aliases",
            },
            "linux": {
                "host-tests": "SNAPSHOT=skip ./Scripts/run-tests.sh",
                "portable-build": "./Scripts/build-core-snapshot.sh",
                "portable-snapshots": "python3 Tests/Snapshots/run.py --backend portable --quiet",
                "host-glibc": "./Scripts/build-host.sh --release",
                "host-musl": "./Scripts/build-host-linux.sh x86_64",
                "sanitizers": "./Scripts/run-sanitized-tests.sh",
                "fuzz": "FUZZ_REQUIRED=1 ./Scripts/run-tests.sh --fuzz-only",
                "pgo": 'export PATH="${UBQ_PGO_LLVM:+$UBQ_PGO_LLVM:}$PATH"\n'
                       "python3 ./Scripts/check-profile-orphans.py\n"
                       "UBIQUITTY_PGO=REQUIRE python3 ./Scripts/check-pgo-applies.py --recorded-budget",
                "kit-build": "./Scripts/build-linux.sh --test",
                "linux-build": "./Scripts/build-linux-gtk.sh Release",
                "linux-smokes": "xvfb-run -a ./Scripts/run-linux-smokes.sh",
                "linux-snapshots": "xvfb-run -a python3 Tests/Snapshots/run.py --backend linux --quiet",
                "linux-determinism": "xvfb-run -a python3 Tests/Snapshots/run.py --backend linux --twice linux_native_text",
                "wasm-smoke": 'source "$EMSDK_ENV"\n./Scripts/wasm-smoke.sh',
                "web-build": 'source "$EMSDK_ENV"\n./Scripts/build-web.sh',
                "web-snapshots": 'source "$EMSDK_ENV"\npython3 Tests/Snapshots/run.py --backend web --quiet',
            },
            "apple": {
                "macos-build": "env -u UBIQUITTY_QUIC_PREFIX -u UBIQUITTY_SIGN_IDENTITY ./Scripts/build-macos.sh --no-linux-hosts",
                "macos-smokes": "python3 Scripts/check-macos-smokes.py",
                "tidy": "./Scripts/check-tidy.sh --since origin/main",
                "swift-format": "./Scripts/format-code.sh --check --swift-only",
                "portable-build": "./Scripts/build-core-snapshot.sh",
                "snapshots": "./Scripts/run-tests.sh snapshots",
                "ui-scenarios": "python3 Scripts/check-ui-scenarios.py",
                "pgo": "python3 ./Scripts/check-profile-orphans.py --profile Support/pgo/portable-core.profdata\n"
                       "UBIQUITTY_PGO=REQUIRE python3 ./Scripts/check-pgo-applies.py --profile Support/pgo/portable-core.profdata",
                "core-frames": "./Scripts/check-core-builder-frames.sh",
                "metallib": "./Scripts/build-metallib.sh",
                "swift-renderer": "./Scripts/check-swift-renderer.sh",
                "forks": './Scripts/check-forks.sh "$UBQ_BUILD_ROOT/macos"',
                "host-tests": "SNAPSHOT=skip ./Scripts/run-tests.sh",
                "ios-build": "./Scripts/build-ios.sh --sdk iphonesimulator --ui-tests",
                "xcode": "./Scripts/generate-xcode-workspace.sh",
                "ios-smokes": "./Scripts/run-ios-smokes.sh",
                "ios-sync": "./Scripts/run-ios-sync-conformance.sh",
                "ios-protected-file": "./Scripts/run-ios-protected-file-tests.sh",
                "ios-ui": "./Scripts/run-ios-ui-tests.sh",
            },
        }
        for platform, commands in expected.items():
            actual = {stage.key: stage.command for stage in batteries.stages(platform)}
            for key, command in commands.items():
                with self.subTest(platform=platform, key=key):
                    self.assertEqual(actual[key], command)

    def test_stage_keys_and_commands_are_valid(self):
        for platform in ("preflight", "linux", "apple"):
            manifest = batteries.stages(platform)
            self.assertEqual(len(manifest), len({stage.key for stage in manifest}))
            self.assertEqual(sum(stage.key == "setup" for stage in manifest), 1)
            for stage in manifest:
                with self.subTest(platform=platform, stage=stage.key):
                    self.assertIn(stage.condition, ("always", "code", "cmake", "xvfb", "ui"))
                    self.assertIn(stage.toolchain, ("native", "gcc", "clang", "wasm"))
                    # The template describes all repositories. Required owner
                    # files are resolved/refused for the selected subject by
                    # the executable composition tests below.
                    self.assertTrue(stage.repositories)
                    self.assertTrue(set(stage.repositories) <= {"core", "kit", "app"})
                    checked = subprocess.run(["bash", "-n", "-c", stage.command], capture_output=True, text=True)
                    self.assertEqual(checked.returncode, 0, checked.stderr)

    def test_apple_battery_builds_the_app_as_the_readme_does(self):
        # Helpers on, and the battery's QUIC prefix and signing identity not
        # handed to the build, so the documented bare build runs somewhere.
        build = {stage.key: stage for stage in batteries.stages("apple")}["macos-build"]
        self.assertNotIn("--no-helpers", build.command)
        environment = batteries.platform_environment("apple", {
            "RUNNER_TEMP": "/runner/temp", "RUNNER_WORKSPACE": "/runner", "RUNNER_TOOL_CACHE": "/runner/tools",
            "UBQ_BUILD_ROOT": "/runner/build", "PATH": os.environ.get("PATH", "")})
        for preset in ("UBIQUITTY_QUIC_PREFIX", "UBIQUITTY_SIGN_IDENTITY"):
            self.assertIn(preset, environment)
            self.assertIn(f"-u {preset} ", build.command)
        keys = [stage.key for stage in batteries.stages("apple")]
        self.assertEqual(keys.index("macos-smokes"), keys.index("macos-build") + 1)

    def test_apple_renderer_builds_its_portable_core_prerequisite_first(self):
        keys = [stage.key for stage in batteries.stages("apple")]
        self.assertLess(keys.index("macos-build"), keys.index("swift-renderer"))
        self.assertLess(keys.index("portable-build"), keys.index("swift-renderer"))

    @unittest.skipUnless(shutil.which("shellcheck"), "shellcheck is installed by CI's preflight setup")
    def test_embedded_shell_commands_pass_shellcheck(self):
        for platform in ("preflight", "linux", "apple"):
            for stage in batteries.stages(platform):
                with self.subTest(platform=platform, stage=stage.key):
                    # Emscripten's environment is supplied by the runner, not
                    # this checkout; shellcheck cannot follow that source.
                    command = stage.command.replace('source "$EMSDK_ENV"', '# shellcheck source=/dev/null\nsource "$EMSDK_ENV"')
                    checked = subprocess.run(["shellcheck", "-s", "bash", "-S", "warning", "-"],
                                             input=command, capture_output=True, text=True)
                    self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_suspended_measurements_have_no_stage(self):
        commands = "\n".join(stage.command for platform in ("preflight", "linux", "apple")
                             for stage in batteries.stages(platform))
        self.assertNotIn("run-bench.sh", commands)
        self.assertNotIn("run-coverage.sh", commands)
        self.assertNotIn("check-coverage-floor.sh", commands)

    def test_goldens_keep_all_snapshot_tiers(self):
        environment = {"UBQ_CI_CODE": "false", "UBQ_CI_CMAKE": "false"}
        for platform, expected in (("linux", {"portable-snapshots", "linux-snapshots", "web-snapshots"}),
                                   ("apple", {"snapshots"})):
            active = {stage.key for stage in batteries.stages(platform)
                      if not batteries.condition_reason(stage, environment, which=lambda *args, **kwargs: "/xvfb-run")}
            self.assertTrue(expected <= active)
            self.assertNotIn("host-tests", active)
            self.assertNotIn("xcode", active)
            if platform == "linux":
                self.assertNotIn("pgo", active)
        script = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        self.assertIn('if wanted snapshots && [ "${SNAPSHOT:-}" != "skip" ]; then', script)

    def test_xcode_requires_code_and_cmake_reach(self):
        stage = next(stage for stage in batteries.stages("apple") if stage.key == "xcode")
        self.assertFalse(batteries.condition_reason(stage, {"UBQ_CI_CODE": "true", "UBQ_CI_CMAKE": "true"}))
        for code, cmake in (("false", "true"), ("true", "false"), ("false", "false")):
            self.assertTrue(batteries.condition_reason(stage, {"UBQ_CI_CODE": code, "UBQ_CI_CMAKE": cmake}))

    def test_ui_scenarios_run_unless_a_hosted_run_opts_out(self):
        stage = next(stage for stage in batteries.stages("apple") if stage.key == "ui-scenarios")
        self.assertFalse(batteries.condition_reason(stage, {}))
        self.assertFalse(batteries.condition_reason(stage, {"UBQ_CI_UI_SCENARIOS": "true"}))
        reason = batteries.condition_reason(stage, {"UBQ_CI_UI_SCENARIOS": "false"})
        self.assertTrue(reason)
        # A policy exclusion, not a missing capability: the run can still be
        # the full checkpoint later pushes measure their reach from.
        self.assertFalse(reason.startswith("UNGATED:"))

    def test_xvfb_absence_is_explicitly_ungated(self):
        for stage in batteries.stages("linux"):
            if stage.condition == "xvfb":
                self.assertTrue(batteries.condition_reason(stage, {}, which=lambda *args, **kwargs: None).startswith("UNGATED:"))

    def test_compiler_roots_remain_separate(self):
        environment = batteries.platform_environment("linux", {
            "RUNNER_WORKSPACE": "/runner", "RUNNER_TEMP": "/temporary", "RUNNER_TOOL_CACHE": "/tools", "PATH": "/bin"})
        by_key = {stage.key: stage for stage in batteries.stages("linux")}
        gcc = batteries.stage_environment(by_key["host-tests"], "linux", environment)
        clang = batteries.stage_environment(by_key["fuzz"], "linux", environment)
        sanitizers = batteries.stage_environment(by_key["sanitizers"], "linux", environment)
        pgo = batteries.stage_environment(by_key["pgo"], "linux", environment)
        native = batteries.stage_environment(by_key["portable-build"], "linux", environment)
        wasm = batteries.stage_environment(by_key["web-build"], "linux", environment)
        self.assertEqual((gcc["UBQ_BUILD_ROOT"], gcc["CC"], gcc["CXX"]), ("/runner/build-gcc", "gcc", "g++"))
        self.assertEqual((clang["UBQ_BUILD_ROOT"], clang["CC"], clang["CXX"]), ("/runner/build-clang", "clang", "clang++"))
        self.assertEqual((sanitizers["UBQ_BUILD_ROOT"], sanitizers["CC"], sanitizers["CXX"]),
                         ("/runner/build-clang", "clang", "clang++"))
        self.assertEqual((pgo["UBQ_BUILD_ROOT"], pgo["CC"], pgo["CXX"]),
                         ("/runner/build-clang", "clang", "clang++"))
        self.assertEqual(native["UBQ_BUILD_ROOT"], "/runner/build")
        self.assertEqual(wasm["UBQ_BUILD_ROOT"], "/runner/build")
        self.assertNotIn("CC", wasm)
        self.assertNotIn("CXX", wasm)
        self.assertEqual(gcc["UBIQUITTY_QUIC_PREFIX"], clang["UBIQUITTY_QUIC_PREFIX"])
        self.assertEqual(gcc["UBIQUITTY_QUIC_PREFIX"], "/runner/build-gcc/quic-deps")
        for stage in (gcc, native, clang, sanitizers, pgo, wasm):
            self.assertEqual(stage["UBIQUITTY_QUIC_PREFIX"], "/runner/build-gcc/quic-deps")
            self.assertEqual(stage["UBQ_TEST_FAIL_FAST"], "1")
        # run-tests sweeps immediate native children of its own build root.
        # None of the platform or WASM outputs lives in the clang root.
        for suffix in ("core", "host", "linux-gtk", "wasm", "web", "web-dist"):
            artifact = Path(native["UBQ_BUILD_ROOT"]) / suffix
            self.assertNotIn(Path(clang["UBQ_BUILD_ROOT"]), artifact.parents)
            self.assertNotIn(Path(gcc["UBQ_BUILD_ROOT"]), artifact.parents)
        with patch.object(batteries.subprocess, "check_output", return_value="/apple"):
            apple = batteries.platform_environment("apple", {"RUNNER_TEMP": "/temporary"})
        self.assertEqual(apple["UBQ_TEST_FAIL_FAST"], "1")
        # A hosted Mac takes the pinned clang-format from its tool cache; a
        # maintainer's Mac, with no tool cache, keeps its own PATH.
        self.assertEqual(apple.get("PATH"), None)
        pin = (batteries.ROOT / "Scripts/clang-format-pin").read_text(encoding="utf-8").strip()
        with patch.object(batteries.subprocess, "check_output", return_value="/apple"):
            hosted = batteries.platform_environment("apple", {"RUNNER_TEMP": "/temporary", "RUNNER_TOOL_CACHE": "/tools",
                                                              "PATH": "/bin"})
        self.assertEqual(hosted["PATH"], f"/tools/clang-format-{pin}/bin" + os.pathsep + "/bin")

    def test_linux_vulkan_is_lavapipe_unless_the_machine_names_its_own_driver(self):
        base = {"RUNNER_WORKSPACE": "/runner", "RUNNER_TEMP": "/temporary",
                "RUNNER_TOOL_CACHE": "/tools", "PATH": "/bin"}
        software = batteries.platform_environment("linux", dict(base))
        self.assertIn("lvp_icd", software["VK_ICD_FILENAMES"])
        self.assertEqual(software["LIBGL_ALWAYS_SOFTWARE"], "1")
        native = batteries.platform_environment("linux", dict(base, UBQ_CI_LINUX_VULKAN="native"))
        self.assertNotIn("VK_ICD_FILENAMES", native)
        self.assertNotIn("LIBGL_ALWAYS_SOFTWARE", native)

    def test_equivalent_cached_gcc_spelling_is_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "BuildHostTests/ctest/CMakeCache.txt"
            cache.parent.mkdir(parents=True)
            cache.write_text("CMAKE_C_COMPILER:FILEPATH=/cache/cc\n"
                             "CMAKE_CXX_COMPILER:FILEPATH=/cache/c++\n")
            identities = {"gcc": "gcc-c", "g++": "gcc-cxx", "/cache/cc": "gcc-c", "/cache/c++": "gcc-cxx"}
            actual = batteries.retained_gcc_drivers({"UBQ_BUILD_ROOT": directory},
                                                    lambda compiler, *args: identities[compiler])
            self.assertEqual(actual, {"CC": "/cache/cc", "CXX": "/cache/c++"})
            # CMake's recorded absolute spelling is what run-tests compares;
            # normalizing both wrappers to their shared ccache inode is unsafe.
            self.assertIn(f"CMAKE_CXX_COMPILER:FILEPATH={actual['CXX']}\n", cache.read_text())

    def test_nonmatching_or_unproven_cached_gcc_is_not_reused(self):
        defaults = {"CC": "gcc", "CXX": "g++"}
        reference = ("target", "version", "specs", "macros")
        for mismatch in (None, ("other-target", *reference[1:]),
                         (reference[0], "other-version", *reference[2:]),
                         (*reference[:2], "other-specs", reference[3]),
                         (*reference[:3], "other-macros")):
            with self.subTest(identity=mismatch), tempfile.TemporaryDirectory() as directory:
                cache = Path(directory) / "BuildHostTests/ctest/CMakeCache.txt"
                cache.parent.mkdir(parents=True)
                cache.write_text("CMAKE_C_COMPILER:FILEPATH=/cache/cc\n"
                                 "CMAKE_CXX_COMPILER:FILEPATH=/cache/c++\n")
                identify = lambda compiler, *args: mismatch if compiler == "/cache/c++" else reference
                self.assertEqual(batteries.retained_gcc_drivers({"UBQ_BUILD_ROOT": directory}, identify), defaults)
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(batteries.retained_gcc_drivers({"UBQ_BUILD_ROOT": directory},
                             lambda *args: self.fail("no cache needs no compiler probes")), defaults)

    def test_gcc_identity_rejects_clang_and_failed_probes(self):
        for macros in ("#define __GNUC__ 4\n#define __clang__ 1", "#define OTHER 1"):
            with self.subTest(macros=macros), patch.object(batteries.subprocess, "check_output", return_value=macros):
                self.assertIsNone(batteries.gcc_identity("/cache/c++", "c++", {}))
        for error in (OSError("missing compiler"), subprocess.TimeoutExpired("compiler", 10),
                      subprocess.CalledProcessError(1, "compiler")):
            with self.subTest(error=error), patch.object(batteries.subprocess, "check_output", side_effect=error):
                self.assertIsNone(batteries.gcc_identity("/cache/c++", "c++", {}))
        with patch.object(batteries.subprocess, "check_output", side_effect=(
                "#define __GNUC__ 16\n#define __GNUG__ 16", "target", "16.2.1", "specs")) as query:
            self.assertIsNotNone(batteries.gcc_identity("/cache/c++", "c++", {}))
            self.assertEqual(query.call_args_list[0].args[0], ["/cache/c++", "-dM", "-E", "-x", "c++", "-"])
            self.assertEqual(query.call_count, 4)

    def test_recipe_changes_invalidate_setup_stamp(self):
        stage = batteries.stages("linux")[0]
        first = batteries.stage_environment(stage, "linux", {})["UBQ_CI_SETUP_KEY"]
        second = batteries.stage_environment(replace(stage, command=stage.command + "\ntrue"), "linux", {})["UBQ_CI_SETUP_KEY"]
        self.assertNotEqual(first, second)

    def test_aggregate_case_gate_has_clean_and_structured_negative_controls(self):
        gate = import_module("check-aggregate-cases")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tests = root / "Tests"
            tests.mkdir()
            clean = tests / "clean.cpp"
            clean.write_text(
                "void helper() { CHECK(true); }\n"
                "TEST(one)\n{\n    helper();\n}\n")
            scanned, findings = gate.scan(str(root))
            self.assertEqual(scanned, 1)
            self.assertEqual(findings, [])
            self.assertEqual(gate.main(["check-aggregate-cases.py", str(root)]), 0)

            aggregate = tests / "aggregate.cpp"
            aggregate.write_text(
                "void first() { CHECK(true); }\n"
                "void second() { CHECK(true); }\n"
                "TEST(allCases)\n{\n    first();\n    second();\n}\n")
            errors = io.StringIO()
            with redirect_stderr(errors):
                self.assertEqual(gate.main(["check-aggregate-cases.py", str(root)]), 1)
            self.assertIn(
                "Tests/aggregate.cpp:3: check-aggregate-cases: TEST(allCases)",
                errors.getvalue())
            finding = batteries.Reporter("preflight", {}).finding_from_line(
                errors.getvalue().splitlines()[0])
            self.assertEqual(finding["file"], "Tests/aggregate.cpp")
            self.assertEqual(finding["line"], 3)
            self.assertEqual(finding["gate"], "check-aggregate-cases")

    def test_only_selects_named_stages_in_manifest_order_and_refuses_unknown_keys(self):
        manifest = batteries.stages("linux")
        selected = batteries.select_stages(manifest, ["linux-smokes", "linux-build"])
        self.assertEqual([stage.key for stage in selected], ["linux-build", "linux-smokes"])
        self.assertEqual(selected[0].command, next(
            stage.command for stage in manifest if stage.key == "linux-build"))
        with self.assertRaises(ValueError):
            batteries.select_stages(manifest, ["linux-build", "linux-bulid"])

    def test_only_refuses_a_stage_without_the_stage_whose_build_it_reads(self):
        manifest = batteries.stages("apple")
        with self.assertRaisesRegex(ValueError, "swift-renderer reads what portable-build builds"):
            batteries.select_stages(manifest, ["setup", "swift-renderer"], ("kit",))
        # The shader library comes from the Kit's own stage in a Kit checkout and
        # from the app's build in the app; each is required only where it applies.
        with self.assertRaisesRegex(ValueError, "swift-renderer reads what metallib builds"):
            batteries.select_stages(manifest, ["portable-build", "swift-renderer"], ("kit",))
        with self.assertRaisesRegex(ValueError, "swift-renderer reads what macos-build builds"):
            batteries.select_stages(manifest, ["portable-build", "swift-renderer"], ("app",))
        selected = batteries.select_stages(manifest, ["portable-build", "metallib", "swift-renderer"], ("kit",))
        self.assertEqual([stage.key for stage in selected], ["portable-build", "metallib", "swift-renderer"])
        selected = batteries.select_stages(manifest, ["macos-build", "portable-build", "swift-renderer"], ("app",))
        self.assertEqual([stage.key for stage in selected], ["macos-build", "portable-build", "swift-renderer"])
        # A battery runs its stages in manifest order, so what a stage reads is built before it.
        for platform in ("linux", "apple"):
            order = [stage.key for stage in batteries.stages(platform)]
            for stage in batteries.stages(platform):
                for required in stage.requires:
                    with self.subTest(stage=stage.key, required=required):
                        self.assertIn(required, order)
                        self.assertLess(order.index(required), order.index(stage.key))

    def test_lavapipe_manifest_is_the_one_mesa_installed(self):
        with tempfile.TemporaryDirectory(prefix="ubq-icd-") as scratch:
            directory = Path(scratch)
            self.assertEqual(batteries.lavapipe_manifest(directory), str(directory / "lvp_icd.json"))
            for name in ("lvp_icd.json", "lvp_icd.x86_64.json"):
                with self.subTest(name=name):
                    for stale in directory.iterdir():
                        stale.unlink()
                    (directory / "intel_icd.json").write_text("{}")
                    (directory / name).write_text("{}")
                    self.assertEqual(batteries.lavapipe_manifest(directory), str(directory / name))

    def test_first_failure_stops_and_records_remaining_stages(self):
        manifest = tuple(batteries.Stage(str(index), f"stage {index}", "true") for index in range(4))
        executed = []

        def execute(stage, environment):
            executed.append(stage.key)
            return 3 if stage.key == "1" else 0

        with tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()):
            path = Path(directory) / "records.json"
            status = batteries.run_battery("preflight", manifest, {"UBQ_CI_ROOT": directory}, path, execute)
            records = json.loads(path.read_text())
        self.assertEqual(status, 1)
        self.assertEqual(executed, ["0", "1"])
        self.assertEqual([record["state"] for record in records], ["completed", "failed", "unexecuted", "unexecuted"])

    def test_unreachable_code_is_not_mislabeled_as_blocked_by_failure(self):
        manifest = (batteries.Stage("first", "first", "false"), batteries.Stage("code", "code", "true", "code"))
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()):
            path = Path(directory) / "records.json"
            batteries.run_battery("preflight", manifest, {"UBQ_CI_CODE": "false", "UBQ_CI_ROOT": directory},
                                  path, lambda *args: 1)
            records = json.loads(path.read_text())
        self.assertEqual(records[1]["detail"], "change contains only snapshot goldens")

    def test_shell_stops_inside_multiline_and_pipeline_stages(self):
        for command in ("false\nprintf unexpected", "false | true\nprintf unexpected"):
            with patch.object(batteries, "ROOT", Path.cwd()):
                stage = batteries.Stage("failure", "failure", command)
                self.assertNotEqual(batteries.execute_stage(stage, {}), 0)

    def test_full_checkpoint_output_excludes_goldens_and_missing_capabilities(self):
        cases = (("true", "always", "true"), ("false", "always", "false"),
                 ("true", "xvfb", "false"))
        for code, condition, expected in cases:
            with self.subTest(code=code, condition=condition), tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()):
                path = Path(directory) / "records.json"
                output = Path(directory) / "output"
                stage = batteries.Stage("first", "first", "true", condition)
                environment = {"UBQ_CI_CODE": code, "UBQ_CI_ROOT": directory, "PATH": "/no-tools",
                               "GITHUB_OUTPUT": str(output)}
                self.assertEqual(batteries.run_battery("linux", (stage,), environment, path, lambda *args: 0), 0)
                self.assertEqual(output.read_text(), f"full_code={expected}\n")

    def test_missing_or_interrupted_state_reports_unexecuted(self):
        manifest = (batteries.Stage("first", "first", "true"),)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "records.json"
            for state in (None, "running"):
                if state:
                    batteries.save_records(path, [{"key": "first", "state": state, "detail": "interrupted before a verdict"}])
                output, errors = io.StringIO(), io.StringIO()
                with redirect_stdout(output), redirect_stderr(errors):
                    status = batteries.report("preflight", manifest, path, {"UBQ_CI_ROOT": directory})
                self.assertIn("| unexecuted |", output.getvalue())
                self.assertNotIn("| completed |", output.getvalue())
                # A run that wrote no events has no routing artifact to validate,
                # and the report says so about this run's root, not a shared one.
                self.assertEqual(status, 1)
                self.assertIn(f"routing artifact invalid — could not read {directory}/preflight/events.jsonl",
                              errors.getvalue())

    def test_interruption_keeps_the_in_progress_stage(self):
        stage = batteries.Stage("first", "first", "true")
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()):
            path = Path(directory) / "records.json"
            def interrupt(*args):
                raise KeyboardInterrupt()
            with self.assertRaises(KeyboardInterrupt):
                batteries.run_battery("preflight", (stage,), {"UBQ_CI_ROOT": directory}, path, interrupt)
            self.assertEqual(json.loads(path.read_text())[0]["state"], "running")

    def test_result_root_is_the_run_variable_then_the_runner_temp_then_tmp(self):
        self.assertEqual(batteries.result_root({"UBQ_CI_ROOT": "/run/one", "RUNNER_TEMP": "/temporary"}),
                         Path("/run/one"))
        self.assertEqual(batteries.result_directory("apple", {"UBQ_CI_ROOT": "/run/one"}),
                         Path("/run/one/apple"))
        self.assertEqual(batteries.result_directory("linux", {"RUNNER_TEMP": "/temporary"}),
                         Path("/temporary/ubiquitty-ci/linux"))
        self.assertEqual(batteries.result_directory("preflight", {}), Path("/tmp/ubiquitty-ci/preflight"))
        # An empty variable is unset, not a root at the working directory.
        self.assertEqual(batteries.result_root({"UBQ_CI_ROOT": "", "RUNNER_TEMP": "/temporary"}),
                         Path("/temporary/ubiquitty-ci"))
        # Every stage inherits the root the battery resolved, so a nested
        # report or battery lands in the same run.
        environment = batteries.platform_environment("preflight", {
            "RUNNER_TEMP": "/temporary", "RUNNER_TOOL_CACHE": "/tools", "PATH": "/bin"})
        self.assertEqual(environment["UBQ_CI_ROOT"], "/temporary/ubiquitty-ci")
        self.assertEqual(environment["UBQ_RESULT_DIR"], "/temporary/ubiquitty-ci/preflight")
        self.assertEqual(environment["UBQ_EVENT_LOG"], "/temporary/ubiquitty-ci/preflight/events.jsonl")

    def test_local_apple_battery_keeps_scratch_in_its_run_root_without_runner_temp(self):
        with patch.object(batteries.subprocess, "check_output", return_value="/apple"):
            environment = batteries.platform_environment("apple", {"UBQ_CI_ROOT": "/run/local"})
        self.assertEqual(environment["CCACHE_STATSLOG"], "/run/local/ccache-stats.log")
        self.assertEqual(environment["UBIQUITTY_SNAPSHOT_FAILURE_DIR"], "/run/local/ubiquitty-regression-fail")
        self.assertEqual(environment["UBQ_EVENT_LOG"], "/run/local/apple/events.jsonl")

    def test_batteries_under_different_roots_keep_their_artifacts_apart(self):
        # With one fixed root, two batteries on one machine share events.jsonl
        # and the stage logs, and the second's report reads whatever the first
        # left. Each gets its own root here, one passing and one failing, and
        # neither leaves a trace in the other's.
        manifest = (batteries.Stage("one", "one stage", 'printf "%s\\n" "$UBQ_BATTERY_MARK"; [ "$UBQ_BATTERY_MARK" = pass ]'),)
        with tempfile.TemporaryDirectory(prefix="ubiquitty-ci-") as first, \
                tempfile.TemporaryDirectory(prefix="ubiquitty-ci-") as second, \
                redirect_stdout(io.StringIO()):
            runs = {}
            for root, mark in ((first, "pass"), (second, "fail")):
                environment = batteries.platform_environment("preflight", {
                    "UBQ_CI_ROOT": root, "UBQ_BATTERY_MARK": mark, "RUNNER_TOOL_CACHE": root,
                    "PATH": os.environ.get("PATH", "")}, TEST_SOURCES)
                path = Path(root) / "preflight" / "battery-local-1.json"
                status = batteries.run_battery("preflight", manifest, environment, path)
                runs[mark] = (root, environment, path, status)
            self.assertEqual(runs["pass"][3], 0)
            self.assertEqual(runs["fail"][3], 1)
            for mark, (root, environment, path, _status) in runs.items():
                artifacts = sorted(entry.name for entry in (Path(root) / "preflight").iterdir())
                self.assertEqual(artifacts, ["01-one.log", "battery-local-1.json", "events.jsonl"], mark)
                self.assertEqual((Path(root) / "preflight/01-one.log").read_text(), f"{mark}\n")
                events = [json.loads(line) for line in (Path(root) / "preflight/events.jsonl").read_text().splitlines()]
                terminal = [event["state"] for event in events if event["kind"] == "stage"][-1]
                self.assertEqual(terminal, "passed" if mark == "pass" else "failed")
                for event in events:
                    for field in ("logPath", "junitPath"):
                        if event.get(field):
                            self.assertTrue(Path(event[field]).is_relative_to(root), (mark, event[field]))
                # The report reads this run's events, and validates them.
                with redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(batteries.report("preflight", manifest, path, environment), 0, errors.getvalue())
                self.assertEqual(batteries.validate_events(
                    "preflight", manifest, batteries.result_directory("preflight", environment) / "events.jsonl"), [])
            # The other run's root read through this run's environment is not this run's.
            self.assertNotEqual(batteries.result_directory("preflight", runs["pass"][1]),
                                batteries.result_directory("preflight", runs["fail"][1]))

    def test_reporter_signal_finalizes_failure_and_later_stages(self):
        manifest = (
            batteries.Stage("first", "first", "true"),
            batteries.Stage("later", "later", "true"),
        )
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(io.StringIO()):
            result_root = Path(directory) / "ubiquitty-ci" / "preflight"
            events = result_root / "events.jsonl"
            path = Path(directory) / "records.json"
            environment = {
                "RUNNER_TEMP": directory,
                "UBQ_RESULT_DIR": str(result_root),
                "UBQ_EVENT_LOG": str(events),
            }
            with patch.object(batteries.Reporter, "run_process", side_effect=SystemExit(143)), \
                    self.assertRaises(SystemExit) as stopped:
                batteries.run_battery("preflight", manifest, environment, path)
            self.assertEqual(stopped.exception.code, 143)
            records = json.loads(path.read_text())
            self.assertEqual([record["state"] for record in records],
                             ["failed", "unexecuted"])
            self.assertIn("blocked by failed stage", records[1]["detail"])
            self.assertEqual(batteries.validate_events("preflight", manifest, events), [])

    def test_real_quiet_stage_keeps_log_and_machine_record(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "battery.json"
            environment = {
                "GITHUB_ACTIONS": "true",
                "RUNNER_TEMP": directory,
                "UBQ_REPORT_MODE": "quiet",
                "UBQ_RETAIN_PASSED_LOGS": "1",
                "UBQ_RESULT_DIR": str(root / "ubiquitty-ci" / "linux"),
                "UBQ_EVENT_LOG": str(root / "events.jsonl"),
            }
            output = io.StringIO()
            with redirect_stdout(output), patch.object(batteries, "ROOT", Path.cwd()):
                status = batteries.run_battery(
                    "linux", (batteries.Stage("one", "one stage", "printf 'hidden detail\\n'"),),
                    environment, path)
            self.assertEqual(status, 0)
            self.assertNotIn("\nhidden detail\n", output.getvalue())
            record = json.loads(path.read_text())[0]
            self.assertEqual(record["state"], "completed")
            self.assertTrue(Path(record["logPath"]).is_file())
            self.assertEqual(Path(record["logPath"]).read_text(), "hidden detail\n")
            events = [json.loads(line) for line in (root / "events.jsonl").read_text().splitlines()]
            self.assertEqual([event["state"] for event in events if event["kind"] == "stage"],
                             ["unexecuted", "running", "passed"])
            self.assertEqual(events[-1]["logArtifactPath"], "01-one.log")

    def test_routing_artifact_requires_outer_stage_pr_identity_and_diagnostics(self):
        manifest = (batteries.Stage("tests", "Host tests", "./Scripts/run-tests.sh"),)
        complete = {
            "schema": "ubiquitty.ci-event.v1",
            "source": "linux-battery",
            "kind": "stage",
            "label": "Host tests",
            "state": "failed",
            "battery": "linux",
            "stageKey": "tests",
            "stageLabel": "Host tests",
            "reason": "compiler failure",
            "command": "./Scripts/run-tests.sh",
            "exitStatus": 1,
            "elapsedSeconds": 3.5,
            "logArtifactPath": "04-tests.log",
            "github": {
                "eventName": "pull_request",
                "repository": "owner/repository",
                "runId": "123",
                "runAttempt": "1",
                "pullRequest": 9,
                "headSha": "deadbeef",
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            (path.parent / complete["logArtifactPath"]).write_text("compiler failure\n")

            def validate(*events):
                path.write_text("".join(json.dumps(event) + "\n" for event in events))
                return batteries.validate_events("linux", manifest, path)

            self.assertEqual(validate(complete), [])
            nested = dict(complete, source="ctest", label="1 tests in ctest")
            self.assertTrue(any("no terminal event" in failure for failure in validate(nested)))
            missing_pr = dict(complete, github={"eventName": "pull_request"})
            self.assertTrue(any("GitHub PR routing" in failure for failure in validate(missing_pr)))
            incomplete = dict(complete)
            for field in ("reason", "command", "logArtifactPath"):
                incomplete.pop(field)
            failures = validate(incomplete)
            self.assertTrue(any("without reason" in failure for failure in failures))
            self.assertTrue(any("without command" in failure for failure in failures))
            self.assertTrue(any("diagnostic artifact" in failure for failure in failures))
            missing = dict(complete, logArtifactPath="missing.log")
            self.assertTrue(any("existing diagnostic artifact" in failure
                                for failure in validate(missing)))
            escaping = dict(complete, logArtifactPath="../outside.log")
            self.assertTrue(any("escaping logArtifactPath" in failure
                                for failure in validate(escaping)))
            path.write_text("not-json\n")
            self.assertTrue(any("invalid JSON" in failure
                                for failure in batteries.validate_events("linux", manifest, path)))

    def test_shared_host_trees_are_built_by_one_fixture_each(self):
        listing = subprocess.run(
            [str(batteries.ROOT / "Scripts/run-tests.sh"), "--list-serial-stanzas"],
            cwd=CORE_TEST_SOURCES.subject, env=CORE_TEST_SOURCES.environment(os.environ),
            capture_output=True, text=True, check=True).stdout.splitlines()
        by_tree = {}
        for line in listing:
            stanza, tree = line.split()
            by_tree.setdefault(tree, set()).add(stanza)
        self.assertIn("host_noquic_fixture", by_tree["host-noquic"])
        self.assertIn("host_quic_fixture", by_tree["host-quic"])
        stanzas = subprocess.run(
            [str(batteries.ROOT / "Scripts/run-tests.sh"), "--list-stanzas"],
            cwd=CORE_TEST_SOURCES.subject, env=CORE_TEST_SOURCES.environment(os.environ),
            capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertIn("cache_sweep_fixture", stanzas)
        self.assertIn("quic_deps_fixture", stanzas)
        self.assertGreaterEqual(len(by_tree["host-noquic"]), 40)
        self.assertGreaterEqual(len(by_tree["host-quic"]), 14)

        # run-tests.sh's own stanza graph is wired where its test tree calls
        # the scheduler, through the entries' shell_<stanza>_* settings.
        cmake = (batteries.ROOT / "Tests/CMakeLists.txt").read_text()
        for tree, variable in (("host-noquic", "UBQ_HOST_NOQUIC_PREBUILT"),
                               ("host-quic", "UBQ_HOST_QUIC_PREBUILT")):
            self.assertIn(f"list(REMOVE_ITEM ${{_entry}}_RESOURCE_LOCK \"ubq_build_{tree}\")", cmake)
            self.assertIn(f"list(APPEND ${{_entry}}_ENVIRONMENT {variable}=1)", cmake)
        self.assertIn("set(shell_cache_sweep_fixture_FIXTURES_SETUP ubq_cache_sweep_fixture)", cmake)
        self.assertIn("set(shell_quic_deps_fixture_FIXTURES_SETUP ubq_quic_deps_fixture)", cmake)
        self.assertRegex(cmake, r'_stanza STREQUAL "quic_deps_fixture"[\s\S]+?'
                                r'ubq_cache_sweep_fixture')
        self.assertRegex(cmake, r'_stanza STREQUAL "host_binary"[\s\S]+?'
                                r'ubq_quic_deps_fixture')
        self.assertIn("_stanza IN_LIST _quic_dependency_stanzas", cmake)
        self.assertRegex(cmake, r"list\(APPEND \$\{_entry\}_FIXTURES_REQUIRED\s+"
                                r"ubq_build_fixture\s+ubq_cache_sweep_fixture\)")
        self.assertRegex(cmake, r'_stanza STREQUAL "host_binary"[\s\S]+?'
                                r'UBQ_COVERAGE_BUILD_FIXTURE=1')
        self.assertRegex(cmake, r'_stanza STREQUAL "host_noquic_fixture"[\s\S]+?'
                                r'_stanza STREQUAL "host_quic_fixture"[\s\S]+?'
                                r'UBQ_COVERAGE_BUILD_FIXTURE=1')
        self.assertRegex(cmake, r'_stanza STREQUAL "cache_sweep_fixture"[\s\S]+?'
                                r'UBQ_COVERAGE_BUILD_FIXTURE=1')
        driver = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        # Joined at run time so this negative assertion is not itself the
        # in-checkout build path that check-build-root.py refuses.
        self.assertNotIn('HOST_BUILD="' + "/".join(("$ROOT", "build", "host-noquic")) + '"', driver)
        self.assertIn('if [ "${UBQ_CACHE_SWEEP_COMPLETE:-0}" != "1" ]; then', driver)
        self.assertIn('"$ROOT/Scripts/ensure-quic-deps.sh" "$QUIC_PREFIX"', driver)
        self.assertIn("quic_deps_present --quiet || build_broken", driver)
        self.assertIn('QUIC_PREFIX="${UBIQUITTY_QUIC_PREFIX:-$UBQ_BUILD_ROOT/quic-deps}"',
                      driver)
        self.assertIn('*) ubq_note_expected_profile ;;', driver)
        quic_consumers = subprocess.run(
            [str(batteries.ROOT / "Scripts/run-tests.sh"),
             "--list-quic-dependency-stanzas"],
            cwd=CORE_TEST_SOURCES.subject, env=CORE_TEST_SOURCES.environment(os.environ),
            capture_output=True, text=True, check=True).stdout.splitlines()
        for stanza in ("quic_loopback", "quic_channel_lifecycle", "quic_daemon_accept",
                       "quic_pin_gate", "quic_outbound_capacity"):
            self.assertIn(stanza, quic_consumers)

    @unittest.skipUnless(shutil.which("cc") and shutil.which("ar"),
                         "a C compiler and archiver are required")
    def test_quic_validator_rejects_one_foreign_archive_in_a_complete_set(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            library = prefix / "lib"
            library.mkdir()
            (prefix / "include/ngtcp2").mkdir(parents=True)
            (prefix / "include/ngtcp2/ngtcp2.h").touch()
            (prefix / "include/openssl").mkdir(parents=True)
            (prefix / "include/openssl/ssl.h").touch()
            source = prefix / "native.c"
            native_object = prefix / "native.o"
            source.write_text("int ubiquitty_quic_dependency_probe;\n")
            compile_command = ["cc"]
            if os.uname().sysname == "Darwin":
                deployment = (batteries.ROOT / "Scripts/macos-deployment-target.txt").read_text()
                target = re.search(r"^cxx=(.+)$", deployment, re.M).group(1)
                compile_command.append(f"-mmacosx-version-min={target}")
            subprocess.run([*compile_command, "-c", str(source), "-o", str(native_object)],
                           check=True)
            foreign_object = prefix / "foreign.o"
            foreign_bytes = bytearray(native_object.read_bytes())
            if os.uname().sysname == "Darwin":
                foreign_bytes[4:8] = bytes(4)
            elif foreign_bytes[:4] == b"\x7fELF":
                foreign_bytes[18:20] = bytes(2)
            else:
                self.skipTest("unknown native object format")
            foreign_object.write_bytes(foreign_bytes)
            archives = ("libngtcp2.a", "libngtcp2_crypto_boringssl.a",
                        "libssl.a", "libcrypto.a")
            for archive in archives:
                source = foreign_object if archive == "libssl.a" else native_object
                subprocess.run(["ar", "rcS", str(library / archive), str(source)], check=True)
            validator = batteries.ROOT / "Scripts/ensure-quic-deps.sh"
            checked = subprocess.run(
                [str(validator), "--check", directory], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
            (library / "libssl.a").unlink()
            subprocess.run(["ar", "rcS", str(library / "libssl.a"), str(native_object)],
                           check=True)
            checked = subprocess.run(
                [str(validator), "--check", directory], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    @unittest.skipUnless(os.uname().sysname == "Darwin"
                         and shutil.which("cc") and shutil.which("ar"),
                         "Apple archive metadata requires Darwin, a compiler, and an archiver")
    def test_quic_metadata_distinguishes_host_device_simulator_and_target(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory)
            library = prefix / "lib"
            library.mkdir()
            (prefix / "include/ngtcp2").mkdir(parents=True)
            (prefix / "include/ngtcp2/ngtcp2.h").touch()
            (prefix / "include/openssl").mkdir(parents=True)
            (prefix / "include/openssl/ssl.h").touch()
            source = prefix / "native.c"
            native_object = prefix / "native.o"
            source.write_text("int ubiquitty_quic_dependency_probe;\n")
            subprocess.run(["cc", "-c", str(source), "-o", str(native_object)], check=True)
            for archive in ("libngtcp2.a", "libngtcp2_crypto_boringssl.a",
                            "libssl.a", "libcrypto.a"):
                subprocess.run(["ar", "rcS", str(library / archive), str(native_object)],
                               check=True)

            tools = prefix / "tools"
            tools.mkdir()
            fake_xcrun = tools / "xcrun"
            fake_xcrun.write_text(
                "#!/usr/bin/env bash\n"
                "if [ \"$1\" = --sdk ]; then echo 18.2; exit 0; fi\n"
                "printf 'platform %s\\nminos %s\\nsdk %s\\n' \"$FAKE_PLATFORM\" "
                "\"$FAKE_MINIMUM\" \"$FAKE_SDK_VERSION\"\n")
            fake_xcrun.chmod(0o755)
            validator = batteries.ROOT / "Scripts/ensure-quic-deps.sh"
            environment = dict(
                os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"],
                FAKE_PLATFORM="IOSSIMULATOR", FAKE_MINIMUM="18.0",
                FAKE_SDK_VERSION="18.2")

            def check(sdk, target):
                return subprocess.run(
                    [str(validator), "--check", directory, sdk, target],
                    capture_output=True, text=True, env=environment).returncode

            self.assertEqual(check("iphonesimulator", "18.0"), 0)
            self.assertEqual(check("iphoneos", "18.0"), 1)
            self.assertEqual(check("iphonesimulator", "19.0"), 1)
            self.assertEqual(check("", "18.0"), 1)

    def test_quic_rebuild_clears_owned_archives_and_forwards_cross_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scripts = root / "Scripts"
            scripts.mkdir()
            validator = scripts / "ensure-quic-deps.sh"
            shutil.copy2(batteries.ROOT / "Scripts/ensure-quic-deps.sh", validator)
            (scripts / "build-root.sh").write_text(
                'UBQ_BUILD_ROOT="${UBQ_BUILD_ROOT:-$PWD/build}"\n'
                "export UBQ_BUILD_ROOT\n")
            capture = root / "fetch-arguments"
            fetcher = scripts / "fetch-quic-deps.sh"
            fetcher.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                'prefix="$1"\n'
                'remaining="$(find "$prefix/lib" -name "*.a" -print -quit)"\n'
                f"printf '%s\\n' \"$1\" \"$2\" \"$3\" \"remaining=$remaining\" >{shlex.quote(str(capture))}\n"
                "exit 23\n")
            fetcher.chmod(0o755)
            prefix = root / "prefix"
            library = prefix / "lib"
            library.mkdir(parents=True)
            (library / "libssl.a").write_bytes(b"stale")
            (library / "unowned.a").write_bytes(b"keep")

            checked = subprocess.run(
                [str(validator), str(prefix), "iphonesimulator", "18.0"],
                capture_output=True, text=True)
            self.assertEqual(checked.returncode, 23, checked.stdout + checked.stderr)
            self.assertEqual(capture.read_text().splitlines(), [
                str(prefix), "iphonesimulator", "18.0",
                f"remaining={library / 'unowned.a'}",
            ])

    def test_coverage_fixtures_have_no_marker_and_prebuilt_consumers_do(self):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()

        def function(name):
            start = source.index(f"\n{name}() {{") + 1
            return source[start:source.index("\n}\n", start) + 3]

        shell = "set -euo pipefail\n" + function("ubq_note_expected_profile") \
            + function("ubq_prebuilt_cmake") + function("cmake") \
            + function("ubq_host_binary") + r'''
UBQ_BUILD_ROOT="$1"
UBQ_COVERAGE=1
QUIC_PREFIX="$1/quic"
        for fixture in host_binary host_noquic_fixture host_quic_fixture quic_deps_fixture; do
    LLVM_PROFILE_FILE="$1/shell_${fixture}--%c%p.profraw"
    UBQ_COVERAGE_BUILD_FIXTURE=1
    export LLVM_PROFILE_FILE UBQ_COVERAGE_BUILD_FIXTURE
    ubq_note_expected_profile
    test ! -e "$1/shell_${fixture}.expects-profile"
done
UBQ_COVERAGE_BUILD_FIXTURE=0
LLVM_PROFILE_FILE="$1/shell_consumer--%c%p.profraw"
UBQ_HOST_NOQUIC_PREBUILT=1
export UBQ_COVERAGE_BUILD_FIXTURE LLVM_PROFILE_FILE UBQ_HOST_NOQUIC_PREBUILT
cmake -B "$1/host-noquic" -S "$1/source"
test -e "$1/shell_consumer.expects-profile"
HOST_TEST_BUILD="$1/host-tests"
mkdir -p "$HOST_TEST_BUILD"
touch "$HOST_TEST_BUILD/ubiquitty-host"
chmod +x "$HOST_TEST_BUILD/ubiquitty-host"
UBQ_HOST_PREBUILT=1
LLVM_PROFILE_FILE="$1/shell_host_consumer--%c%p.profraw"
export UBQ_HOST_PREBUILT LLVM_PROFILE_FILE
ubq_host_binary "$1/unused.log" >/dev/null
test -e "$1/shell_host_consumer.expects-profile"
'''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tree = root / "host-noquic"
            (tree / "CMakeFiles/ubiquitty-host.dir").mkdir(parents=True)
            binary = tree / "ubiquitty-host"
            binary.touch()
            binary.chmod(0o755)
            (tree / "CMakeCache.txt").write_text(
                "UBIQUITTY_FRONTEND:STRING=Host\nUBIQUITTY_QUIC:BOOL=ON\n")
            checked = subprocess.run(
                ["bash", "-c", shell, "test", directory], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)


def full_jobs():
    return [{"name": name, "conclusion": "success", "steps": [
        {"name": reach.FULL_MARKERS.get(name, "preflight"), "conclusion": "success"}]} for name in reach.JOBS]


class SnapshotSkipTests(unittest.TestCase):
    def fixture(self, directory, include_accounting=True):
        # Exercise the driver's real selection and accounting without its
        # compile/cache sweep or any renderer launch. Only the render body is
        # replaced by a controllable result.
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        report_start = source.index("ubq_report_unknown_filters() {")
        report = source[report_start:source.index("\n}\n", report_start) + 3]
        helpers = (batteries.ROOT / "Scripts/run-tests-lib.sh").read_text()
        wanted_start = helpers.index("wanted() {")
        wanted = helpers[wanted_start:helpers.index("\n}\n", wanted_start) + 3]
        accounting_start = source.index('if wanted snapshots && [ "${SNAPSHOT:-}" = "skip" ]; then')
        enabled_start = source.index('if wanted snapshots && [ "${SNAPSHOT:-}" != "skip" ]; then')
        accounting = source[accounting_start:enabled_start] if include_accounting else ""
        enabled = source[enabled_start:source.index("\n", enabled_start) + 1]
        sanity_start = source.rindex("ubq_unmatched=()")
        sanity = source[sanity_start:source.index("\n# The handoff's verdict", sanity_start)]
        fixture = Path(directory) / "snapshot-dispatch.sh"
        fixture.write_text("""set -eu
FUZZ_ONLY=0
SKIP_CTEST_COVERED=0
FILTERS=("$@")
FILTERS_MATCHED=" "
ROOT=/fixture
CORE_ROOT=/fixture
RAN=0
SKIPPED=0
ubq_stanzas="snapshots"
ubq_skip() { printf 'SKIP: %s\\n' "$1"; }
""" + report + wanted + accounting + enabled + """
    RAN=$((RAN + 1))
    echo "snapshot fixture executed"
    if [ "${UBQ_TEST_SNAPSHOT_STATUS:-0}" != 0 ]; then
        echo "SKIP: snapshots: unavailable renderer"
        exit "$UBQ_TEST_SNAPSHOT_STATUS"
    fi
fi
""" + sanity + 'printf "%s ran, %s skipped\\n" "$RAN" "$SKIPPED"\n')
        return fixture

    def test_filtered_explicit_skip_answers_without_executing(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = self.fixture(directory)
            checked = subprocess.run(["bash", str(fixture), "snapshots"], capture_output=True, text=True,
                                     env=dict(os.environ, SNAPSHOT="skip"))
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertIn("0 ran, 1 skipped", checked.stdout)
            self.assertNotIn("snapshot fixture executed", checked.stdout)

    def test_old_skip_contract_reproduces_unmatched_filter_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = self.fixture(directory, include_accounting=False)
            checked = subprocess.run(["bash", str(fixture), "snapshots"], capture_output=True, text=True,
                                     env=dict(os.environ, SNAPSHOT="skip"))
            self.assertEqual(checked.returncode, 1)
            self.assertIn("no tests matched filter: snapshots", checked.stderr)

    def test_enabled_and_unknown_filters_keep_their_verdicts(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = self.fixture(directory)
            for filters, expected in ((("snapshots",), 0), (("unknown",), 1), (("snapshots", "unknown"), 1)):
                with self.subTest(filters=filters):
                    checked = subprocess.run(["bash", str(fixture), *filters], capture_output=True, text=True,
                                             env=dict(os.environ, SNAPSHOT=""))
                    self.assertEqual(checked.returncode, expected, checked.stdout + checked.stderr)
                    if expected == 0:
                        self.assertIn("1 ran, 0 skipped", checked.stdout)
                    else:
                        self.assertIn("not a stanza name: unknown", checked.stderr)

    @unittest.skipUnless(shutil.which("ctest"), "CTest is needed to verify its explicit-skip reporting")
    def test_ctest_inherits_skip_and_does_not_mask_enabled_failures(self):
        cmake = (batteries.ROOT / "Tests/CMakeLists.txt").read_text()
        # Whitespace-tolerant after the setting's name. CMake wraps this
        # setting across lines, so a literal space here reads a reformatting
        # as the setting having been deleted.
        property_match = re.search(r'set\(shell_snapshots_SKIP_REGULAR_EXPRESSION\s+"([^"]+)"\)', cmake)
        self.assertIsNotNone(property_match)
        with tempfile.TemporaryDirectory() as directory:
            fixture = self.fixture(directory)
            Path(directory, "CTestTestfile.cmake").write_text(
                f'add_test(shell_snapshots "bash" "{fixture}" "snapshots")\n'
                f'set_tests_properties(shell_snapshots PROPERTIES SKIP_REGULAR_EXPRESSION "{property_match.group(1)}")\n')
            for skip, status, result, code in (("skip", "0", "Skipped", 0), ("", "0", "Passed", 0), ("", "7", "Failed", 8)):
                with self.subTest(skip=skip, status=status):
                    checked = subprocess.run(["ctest", "--test-dir", directory, "--output-on-failure"],
                                             capture_output=True, text=True,
                                             env=dict(os.environ, SNAPSHOT=skip, UBQ_TEST_SNAPSHOT_STATUS=status))
                    self.assertEqual(checked.returncode, code, checked.stdout + checked.stderr)
                    self.assertIn(result, checked.stdout)
                    if skip:
                        self.assertIn("shell_snapshots (Skipped)", checked.stdout)
                    elif status != "0":
                        self.assertNotIn("(Skipped)", checked.stdout)


class StanzaDeclineTests(unittest.TestCase):
    @staticmethod
    def generatedShellEntries(tests, command):
        # CMake serializes test names with quotes or bracket arguments,
        # depending on its version. Both name the same registered entry.
        return dict(re.findall(
            rf'^{command}\((?:\[=*\[|")?(shell_\w+)(?:\]=*\]|")? (.*)\)$', tests, re.M))

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"),
                         "CMake and CTest are needed to schedule a stanza and report it")
    def test_a_stanza_declined_by_any_driver_is_reported_skipped(self):
        # Scheduled through the module itself, for a driver that is not
        # run-tests.sh: a decline is read from the closing line by the name the
        # driver gives itself there, which is its file name.
        with tempfile.TemporaryDirectory() as directory:
            driver = Path(directory, "run-kit-tests.sh")
            driver.write_text(
                "#!/bin/bash\n"
                'case "$1" in\n'
                "    --list-stanzas) echo demo ;;\n"
                "    --list-*) ;;\n"
                '    *) echo "$UBQ_TEST_STANZA_DRIVER: PASSED — $UBQ_TEST_STANZA_RAN ran, 1 skipped" ;;\n'
                "esac\n")
            driver.chmod(0o755)
            project = Path(directory, "project")
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(Decline LANGUAGES NONE)\n"
                "enable_testing()\n"
                f'set(UBIQUITTY_CORE_SOURCE_DIR "{batteries.ROOT}")\n'
                f'include("{batteries.ROOT}/Scripts/cmake/UbiquittyTests.cmake")\n'
                f'ubiquitty_schedule_shell_stanzas(DRIVER "{driver}")\n')
            build = Path(directory, "build")
            configured = subprocess.run(["cmake", "-S", str(project), "-B", str(build)],
                                        capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            # The last row is a line naming another file: the dot in the
            # driver's name matches only a dot.
            for name, ran, skipped in (("run-kit-tests.sh", "0", True),
                                       ("run-kit-tests.sh", "2", False),
                                       ("run-kit-tests_sh", "0", False)):
                with self.subTest(name=name, ran=ran):
                    # The fixtures the entry requires build the real tree; none
                    # is set up here, and the entry runs alone.
                    checked = subprocess.run(["ctest", "--test-dir", str(build), "-R", "^shell_demo$",
                                              "-FA", ".*", "--output-on-failure"],
                                             capture_output=True, text=True,
                                             env=dict(os.environ, UBQ_TEST_STANZA_DRIVER=name,
                                                      UBQ_TEST_STANZA_RAN=ran))
                    self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                    if skipped:
                        self.assertIn("shell_demo (Skipped)", checked.stdout)
                    else:
                        self.assertIn("Passed", checked.stdout)
                        self.assertNotIn("(Skipped)", checked.stdout)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is needed to schedule a driver's stanzas")
    def test_another_driver_gets_only_what_its_own_tree_wires(self):
        # A driver that answers --list-stanzas and nothing else, listing names
        # run-tests.sh also uses. The module gives them the build fixture and
        # the driver's own decline, and none of run-tests.sh's fixtures, its
        # cache sweep or its shared-binary refusal; what the tree sets for an
        # entry is what that entry gets. The deferred tests are left to the
        # driver the tree scheduled.
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory, "project")
            (project / "Scripts").mkdir(parents=True)
            driver = project / "Scripts" / "run-app-tests.sh"
            driver.write_text(
                "#!/bin/bash\n"
                'case "$1" in\n'
                "    --list-stanzas) printf 'snapshots\\nhost_binary\\nweb_outline\\n' ;;\n"
                "    --*) exit 2 ;;\n"
                "esac\n")
            driver.chmod(0o755)
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(AppDriver LANGUAGES NONE)\n"
                "enable_testing()\n"
                f'set(UBIQUITTY_CORE_SOURCE_DIR "{batteries.ROOT}")\n'
                f'include("{batteries.ROOT}/Scripts/cmake/UbiquittyTests.cmake")\n'
                "set(shell_web_outline_ENVIRONMENT UBQ_WEB_OUTLINE=1)\n"
                "set(shell_web_outline_TIMEOUT 900)\n"
                "ubiquitty_schedule_shell_stanzas(DRIVER ${CMAKE_SOURCE_DIR}/Scripts/run-app-tests.sh)\n"
                "ubiquitty_finish_host_tests()\n")
            build = Path(directory, "build")
            configured = subprocess.run(["cmake", "-S", str(project), "-B", str(build)],
                                        capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            self.assertIn("more are left to Scripts/run-app-tests.sh", configured.stdout)
            self.assertNotIn("run-tests.sh", configured.stdout)
            tests = (build / "CTestTestfile.cmake").read_text()
            commands = self.generatedShellEntries(tests, "add_test")
            properties = self.generatedShellEntries(tests, "set_tests_properties")
            self.assertEqual(sorted(commands), ["shell_host_binary", "shell_snapshots", "shell_web_outline"])
            for entry in commands:
                with self.subTest(entry=entry):
                    environment = [word for word in shlex.split(commands[entry]) if "=" in word]
                    names = [word.split("=", 1)[0] for word in environment]
                    self.assertEqual(names[:6], ["NO_CTEST", "SKIP_CTEST_COVERED", "UBQ_OUT", "UBQ_HOST_TEST_TREE",
                                                 "UBIQUITTY_QUIC_PREFIX", "UBQ_BUILD_JOBS"])
                    self.assertEqual(environment[6], "UBQ_TEST_TARGETS_PREBUILT=1")
                    own = ["UBQ_WEB_OUTLINE=1"] if entry == "shell_web_outline" else []
                    self.assertEqual(environment[7:], own + [f"UBQ_TESTS_ROOT={project}",
                                                            f"UBQ_CORE_ROOT={batteries.ROOT}",
                                                            f"UBQ_KIT_ROOT={project}"])
                    self.assertIn(f'WORKING_DIRECTORY "{project}"', properties[entry])
                    self.assertIn('FIXTURES_REQUIRED "ubq_build_fixture" ', properties[entry])
                    self.assertNotIn("FIXTURES_SETUP", properties[entry])
                    self.assertNotIn("RESOURCE_LOCK", properties[entry])
                    self.assertTrue(any(value in properties[entry] for value in (
                        r"[=[run-app-tests\.sh: PASSED .+ 0 ran, [1-9]]=]",
                        r'"run-app-tests\\.sh: PASSED .+ 0 ran, [1-9]"')), properties[entry])
                    timeout = '"900"' if entry == "shell_web_outline" else '"600"'
                    self.assertIn(f"TIMEOUT {timeout}", properties[entry])

    def subjectFixture(self, ownDriver, sharedBuilds=False, lockSharedBuilds=True, productionPolicy=False):
        scratch = tempfile.TemporaryDirectory(prefix="ubq-ctest-subject-")
        self.addCleanup(scratch.cleanup)
        directory = Path(scratch.name).resolve()
        project = directory / "consumer with spaces"
        core = directory / "core"
        kit = directory / "selected Kit"
        for checkout in (project, core, kit):
            (checkout / "Scripts").mkdir(parents=True)
        (core / "Scripts/ctest-shell-lane-census.sh").symlink_to(
            batteries.ROOT / "Scripts/ctest-shell-lane-census.sh")
        expected = {"UBQ_TESTS_ROOT": str(project), "UBQ_CORE_ROOT": str(core), "UBQ_KIT_ROOT": str(kit)}

        def writeDriver(path, stanza):
            serial = "web_proxy_frames web\nweb_proxy_frames web-dist" if sharedBuilds else ""
            hosts = "host_a\nhost_b" if sharedBuilds else ""
            path.write_text(f"#!{sys.executable}\n"
                            "import json, os, sys\n"
                            f"expected = {expected!r}\n"
                            "if any(os.environ.get(key) != value for key, value in expected.items()):\n"
                            "    sys.exit(73)\n"
                            "if os.getcwd() != expected['UBQ_TESTS_ROOT']:\n"
                            "    sys.exit(74)\n"
                            "if sys.argv[1] == '--list-stanzas':\n"
                            f"    print({stanza!r})\n"
                            "elif sys.argv[1] == '--list-serial-stanzas':\n"
                            f"    print({serial!r})\n"
                            "elif sys.argv[1] == '--list-host-binary-stanzas':\n"
                            f"    print({hosts!r})\n"
                            "elif sys.argv[1].startswith('--list-'):\n"
                            "    pass\n"
                            f"elif sys.argv[1] in {stanza!r}.splitlines():\n"
                            "    with open(os.environ['UBQ_DRIVER_RUNTIME_TRACE'], 'w') as output:\n"
                            "        json.dump(expected, output)\n"
                            "    print('run-tests.sh: PASSED — 1 ran, 0 skipped')\n"
                            "else:\n"
                            "    sys.exit(75)\n")
            path.chmod(0o755)

        stanzas = "app_only\nhost_a\nhost_b\nweb_proxy_frames" if sharedBuilds else "app_only"
        writeDriver(core / "Scripts/run-tests.sh", stanzas if productionPolicy else "core_only")
        if ownDriver:
            writeDriver(project / "Scripts/run-tests.sh", stanzas)
        locks = 'set(shell_host_a_RESOURCE_LOCK existing_resource)\n' if sharedBuilds else ""
        if sharedBuilds and lockSharedBuilds:
            locks += 'ubiquitty_lock_shell_build_trees(DRIVER "${driver}")\n'
        scheduling = 'ubiquitty_schedule_shell_stanzas(DRIVER "${driver}")\n'
        if productionPolicy:
            # Exercise the actual top-level dispatch, without configuring or
            # compiling Core. This callback marks a mistaken Core-policy
            # selection; its prebuilt promise has no producer in the App.
            source = (batteries.ROOT / "Tests/CMakeLists.txt").read_text()
            start = source.index("ubiquitty_default_shell_driver(_ubq_shell_driver)")
            finish = source.index("\nendif()", start) + len("\nendif()")
            scheduling = (
                'function(ubq_schedule_run_tests_stanzas selected)\n'
                '    set(shell_host_a_ENVIRONMENT UBQ_HOST_PREBUILT=1)\n'
                '    set(shell_host_a_FIXTURES_REQUIRED ubq_host_binary_fixture)\n'
                '    ubiquitty_schedule_shell_stanzas(DRIVER "${selected}")\n'
                'endfunction()\n' + source[start:finish] + '\n')
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(Subject LANGUAGES NONE)\n"
            "enable_testing()\n"
            f'set(UBIQUITTY_CORE_SOURCE_DIR "{core}")\n'
            f'set(UBIQUITTY_KIT_SOURCE_DIR "{kit}")\n'
            f'include("{batteries.ROOT}/Scripts/cmake/UbiquittyTests.cmake")\n'
            "ubiquitty_default_shell_driver(driver)\n"
            'file(WRITE "${CMAKE_BINARY_DIR}/selected-driver" "${driver}")\n'
            + locks + scheduling)
        build = directory / "build"
        environment = dict(os.environ, UBQ_TESTS_ROOT=str(directory / "wrong subject"),
                           UBQ_CORE_ROOT=str(directory / "wrong Core"), UBQ_KIT_ROOT=str(directory / "wrong Kit"),
                           UBQ_DRIVER_RUNTIME_TRACE=str(directory / "ran.json"))
        configured = subprocess.run(["cmake", "-S", str(project), "-B", str(build)], cwd=directory,
                                    env=environment, capture_output=True, text=True)
        self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
        return project, core, build, environment

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "CMake and CTest are needed")
    def test_consumer_driver_keeps_its_subject_and_library_roots_in_bare_ctest(self):
        project, core, build, environment = self.subjectFixture(ownDriver=True)
        self.assertEqual((build / "selected-driver").read_text(), str(project / "Scripts/run-tests.sh"))
        tests = (build / "CTestTestfile.cmake").read_text()
        self.assertIn("shell_app_only", tests)
        self.assertNotIn("shell_core_only", tests)
        # A direct driver invocation demonstrates the wrong-subject oracle;
        # CTest must replace these inherited roots with the configured roots.
        refused = subprocess.run([str(project / "Scripts/run-tests.sh"), "app_only"], cwd=project,
                                 env=environment, capture_output=True, text=True)
        self.assertEqual(refused.returncode, 73)
        checked = subprocess.run(["ctest", "--test-dir", str(build), "-R", "^shell_app_only$",
                                  "-FA", ".*", "--output-on-failure"], env=environment,
                                 capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("Passed", checked.stdout)
        recorded = json.loads(Path(environment["UBQ_DRIVER_RUNTIME_TRACE"]).read_text())
        self.assertEqual(recorded["UBQ_TESTS_ROOT"], str(project))
        self.assertEqual(recorded["UBQ_CORE_ROOT"], str(core))

    @unittest.skipUnless(shutil.which("cmake"), "CMake is needed")
    def test_checkout_without_a_driver_uses_core_without_inventing_consumer_stanzas(self):
        project, core, build, environment = self.subjectFixture(ownDriver=False)
        self.assertFalse((project / "Scripts/run-tests.sh").exists())
        self.assertEqual((build / "selected-driver").read_text(), str(core / "Scripts/run-tests.sh"))
        tests = (build / "CTestTestfile.cmake").read_text()
        self.assertIn("shell_core_only", tests)
        self.assertNotIn("shell_app_only", tests)

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ctest"), "CMake and CTest are needed")
    def test_standalone_core_ctest_clears_both_inherited_kit_roots_before_snapshot_checks(self):
        with tempfile.TemporaryDirectory(prefix="ubq-core-ctest-") as directory:
            project = Path(directory, "core")
            (project / "Scripts").mkdir(parents=True)
            (project / "Scripts/repository.txt").write_text("core\n")
            shutil.copytree(batteries.ROOT / "Tests/Snapshots", project / "Tests/Snapshots",
                            ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copytree(batteries.ROOT / "Tools/showcase", project / "Tools/showcase",
                            ignore=shutil.ignore_patterns("__pycache__"))
            shutil.copy2(batteries.ROOT / "Scripts/ctest-shell-lane-census.sh",
                         project / "Scripts/ctest-shell-lane-census.sh")
            driver = project / "Scripts/run-core-checks.sh"
            snapshot = batteries.ROOT / "Tests/Snapshots/run.py"
            driver.write_text(
                "#!/bin/bash\n"
                'case "$1" in\n'
                "    --list-stanzas) echo snapshot_catalog ;;\n"
                "    --list-*) ;;\n"
                f"    snapshot_catalog) exec {shlex.quote(sys.executable)} {shlex.quote(str(snapshot))} "
                "--backend portable --check-catalog ;;\n"
                "esac\n")
            driver.chmod(0o755)
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(CoreCatalog LANGUAGES NONE)\n"
                "enable_testing()\n"
                f'set(UBIQUITTY_CORE_SOURCE_DIR "{project}")\n'
                f'include("{batteries.ROOT}/Scripts/cmake/UbiquittyTests.cmake")\n'
                'ubiquitty_schedule_shell_stanzas(DRIVER "${CMAKE_SOURCE_DIR}/Scripts/run-core-checks.sh")\n')
            environment = dict(os.environ, UBQ_TESTS_ROOT=str(project), UBQ_CORE_ROOT=str(batteries.ROOT),
                               UBQ_KIT_ROOT=str(batteries.ROOT), UBIQUITTY_KIT_SOURCE_DIR=str(batteries.ROOT))
            refused = subprocess.run([str(driver), "snapshot_catalog"], cwd=project,
                                     env=environment, capture_output=True, text=True)
            self.assertNotEqual(refused.returncode, 0)
            self.assertIn("kit source root names core, not kit", refused.stderr)
            build = Path(directory, "build")
            configured = subprocess.run(["cmake", "-S", str(project), "-B", str(build)],
                                        env=environment, capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            checked = subprocess.run(["ctest", "--test-dir", str(build), "-R", "^shell_snapshot_catalog$",
                                      "-FA", ".*", "--no-tests=error", "--output-on-failure"],
                                     env=environment, capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertIn("Passed", checked.stdout)

    def assertSharedBuildLocks(self, tests):
        properties = self.generatedShellEntries(tests, "set_tests_properties")
        locks = {}
        for entry, settings in properties.items():
            match = re.search(r'RESOURCE_LOCK "([^"]*)"', settings)
            locks[entry] = set(match.group(1).split(";")) if match else set()
        for entry in ("shell_host_a", "shell_host_b"):
            self.assertIn("ubq_build_host-tests", locks[entry], entry)
        self.assertIn("existing_resource", locks["shell_host_a"])
        self.assertEqual(locks["shell_web_proxy_frames"], {"ubq_build_web", "ubq_build_web-dist"})
        self.assertFalse(locks["shell_app_only"])

    @unittest.skipUnless(shutil.which("cmake"), "CMake is needed")
    def test_opted_in_driver_locks_each_shared_build_tree_for_the_entire_stanza(self):
        project, core, build, environment = self.subjectFixture(ownDriver=True, sharedBuilds=True)
        tests = (build / "CTestTestfile.cmake").read_text()
        self.assertSharedBuildLocks(tests)
        # Removing one host consumer's lock recreates the unsafe registration:
        # its build and daemon could overlap another consumer's configure/link.
        line = next(line for line in tests.splitlines()
                    if "shell_host_b" in self.generatedShellEntries(line, "set_tests_properties"))
        unlocked = re.sub(r'RESOURCE_LOCK "[^"]*" ', "", line)
        self.assertNotEqual(unlocked, line)
        with self.assertRaisesRegex(AssertionError, "shell_host_b"):
            self.assertSharedBuildLocks(tests.replace(line, unlocked))

    @unittest.skipUnless(shutil.which("cmake"), "CMake is needed")
    def test_registration_without_inventory_locks_is_rejected_by_the_shared_build_oracle(self):
        project, core, build, environment = self.subjectFixture(ownDriver=True, sharedBuilds=True,
                                                               lockSharedBuilds=False)
        with self.assertRaisesRegex(AssertionError, "shell_host_a"):
            self.assertSharedBuildLocks((build / "CTestTestfile.cmake").read_text())

    @unittest.skipUnless(shutil.which("cmake"), "CMake is needed")
    def test_core_implementation_keeps_consumer_policy_without_a_facade(self):
        project, core, build, _ = self.subjectFixture(ownDriver=False, sharedBuilds=True,
                                                     productionPolicy=True)
        self.assertEqual((build / "selected-driver").read_text(), str(core / "Scripts/run-tests.sh"))
        tests = (build / "CTestTestfile.cmake").read_text()
        self.assertSharedBuildLocks(tests)
        commands = self.generatedShellEntries(tests, "add_test")
        properties = self.generatedShellEntries(tests, "set_tests_properties")
        self.assertEqual(set(commands), {"shell_app_only", "shell_host_a", "shell_host_b", "shell_web_proxy_frames"})
        for name, command in commands.items():
            with self.subTest(name=name):
                self.assertIn(f"UBQ_TESTS_ROOT={project}", command)
                self.assertNotIn("UBQ_HOST_PREBUILT=1", command)
                self.assertNotIn("UBQ_SHARED_BINARIES_FROM_FIXTURES=1", command)
                self.assertIn('FIXTURES_REQUIRED "ubq_build_fixture"', properties[name])
                self.assertNotIn("ubq_host_binary_fixture", properties[name])


class SuiteFailFastTests(unittest.TestCase):
    def test_failed_ctest_stops_before_shell_stanzas_in_ci_only(self):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        start = source.index('if [ "$USE_CTEST" = "1" ]; then')
        handoff = source[start:source.index("\n#", start)]
        script = """set -eu
USE_CTEST=1
CTEST_FAILED=0
run_ctest_suite() { return "$UBQ_TEST_CTEST_STATUS"; }
""" + handoff + '\necho "later shell stanza executed"\nexit "$CTEST_FAILED"\n'
        for fail_fast, status, later, expected in (("1", "8", False, 1), ("0", "8", True, 1), ("1", "0", True, 0)):
            with self.subTest(fail_fast=fail_fast, status=status):
                checked = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                                         env=dict(os.environ, UBQ_TEST_FAIL_FAST=fail_fast, UBQ_TEST_CTEST_STATUS=status))
                self.assertEqual(checked.returncode, expected, checked.stdout + checked.stderr)
                self.assertEqual("later shell stanza executed" in checked.stdout, later)

    @unittest.skipUnless(shutil.which("ctest"), "CTest is needed to verify fail-fast scheduling")
    def test_common_ctest_driver_stops_scheduling_but_local_diagnostics_continue(self):
        with tempfile.TemporaryDirectory() as directory:
            events = Path(directory) / "events"
            body = Path(directory) / "body.sh"
            body.write_text('printf "%s\\n" "$1" >> "$2"\nexit "$3"\n')
            Path(directory, "CTestTestfile.cmake").write_text(
                f'add_test(first "bash" "{body}" "first" "{events}" "1")\n'
                f'add_test(later "bash" "{body}" "later" "{events}" "0")\n'
                'set_tests_properties(first PROPERTIES COST 100)\n'
                'set_tests_properties(later PROPERTIES COST 1)\n')
            driver = batteries.ROOT / "Scripts/run-ctest.sh"
            for fail_fast, expected in (("1", ["first"]), ("0", ["first", "later"])):
                with self.subTest(fail_fast=fail_fast):
                    events.write_text("")
                    checked = subprocess.run([
                        "bash", str(driver), directory, "-j1", "--output-on-failure"],
                        capture_output=True, text=True,
                        env=dict(os.environ, UBQ_TEST_FAIL_FAST=fail_fast, UBQ_BUILD_ROOT=directory,
                                 UBQ_CI_ANNOTATE="", GITHUB_STEP_SUMMARY=""))
                    self.assertEqual(checked.returncode, 8, checked.stdout + checked.stderr)
                    self.assertEqual(events.read_text().splitlines(), expected)
                    self.assertEqual("did not start after fail-fast" in checked.stdout, fail_fast == "1")
        sanitizer = (batteries.ROOT / "Scripts/run-sanitized-tests.sh").read_text()
        self.assertIn('"$CORE_ROOT/Scripts/run-ctest.sh" "$BUILD"', sanitizer)


class CTestReportingTests(unittest.TestCase):
    def test_build_progress_is_live_bounded_and_only_shown_for_the_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            log = build / "Testing/Temporary/ubq-build.log"
            log.parent.mkdir(parents=True)
            log.write_text("old diagnostic\n" * 2000 + "[71/900] Building CXX object VT.cpp.o\n")
            activity = ctest_reporting.CTestActivity(2, build)
            self.assertNotIn("Building CXX", activity.finish())
            self.assertIn("[71/900]", activity("Start 1: ubq_build"))
            log.write_text("[84/900] Linking CXX executable test_vt\n")
            self.assertIn("[84/900]", activity.finish())
            self.assertNotIn("old diagnostic", activity.finish())
            self.assertNotIn("build:", activity("1/2 Test #1: ubq_build ... Passed 3.00 sec"))

    def test_file_only_gate_failure_has_no_invented_line(self):
        reporter = ctest_reporting.Reporter("ctest", {})
        self.assertEqual(
            ctest_reporting.failure_location(
                "Source/Core/Foo.cpp: check-style: generated file is stale", reporter),
            ("Source/Core/Foo.cpp", None, "generated file is stale"))

    def test_native_progress_tracks_active_entries(self):
        activity = ctest_reporting.CTestActivity(3)
        self.assertIn("active first", activity("    Start 1: first"))
        self.assertIn("active first, second", activity("    Start 2: second"))
        state = activity("1/3 Test #1: first ........................ Passed 0.01 sec")
        self.assertIn("1/3 completed", state)
        self.assertIn("active second", state)
        state = activity("2/3 Test #2: second ................ ***Exception: SegFault 0.01 sec")
        self.assertIn("2/3 completed", state)
        self.assertNotIn("active second", state)

    def test_discovery_keeps_only_ctest_selection_arguments(self):
        arguments = ["-j8", "--output-on-failure", "-R", "selection", "-LE", "slow",
                     "--label-regex=fast", "--rerun-failed", "--union",
                     "--tests-from-file", "selected.txt",
                     "--exclude-from-file=excluded.txt", "--stop-on-failure"]
        self.assertEqual(ctest_reporting.selection_arguments(arguments),
                         ["-R", "selection", "-LE", "slow", "--label-regex=fast",
                          "--rerun-failed", "--union", "--tests-from-file",
                          "selected.txt", "--exclude-from-file=excluded.txt"])

    def test_discovery_resolves_relative_selector_files_from_the_build_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            completed = subprocess.CompletedProcess(
                [], 0, stdout=json.dumps({"tests": [{"name": "selected"}]}), stderr="")
            with patch.object(ctest_reporting.subprocess, "run", return_value=completed) as run:
                self.assertEqual(ctest_reporting.discover(
                    build, ["--tests-from-file", "selected.txt"], {}), ["selected"])
            self.assertEqual(run.call_args.kwargs["cwd"], build)

    def test_runtime_arguments_replace_a_caller_owned_junit_path(self):
        self.assertEqual(ctest_reporting.runtime_arguments([
            "-j8", "--output-junit", "/tmp/old.xml", "--output-junit=other.xml",
            "--test-dir", "/tmp/wrong", "--test-dir=other",
            "--stop-on-failure"]), ["-j8", "--stop-on-failure"])

    def test_junit_results_emit_exact_test_failures_and_unexecuted_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit_path = root / "results.xml"
            log_path = root / "ctest.log"
            event_path = root / "events.jsonl"
            junit_path.write_text("""<testsuites><testsuite>
<testcase name="passed" time="0.1"/>
<testcase name="failed" time="0.2"><failure message="case failed"/>
<system-out>Tests/Core/test_example.cpp:42: test-harness: assertion failed — got 1, want 2
test-case: FAILED — preservesTail (0.1s; 1 assertion)</system-out>
<system-err>bounded stderr context</system-err>
</testcase>
<testcase name="skipped" status="notrun" time="0"/>
</testsuite></testsuites>""")
            reporter = ctest_reporting.Reporter(
                "ctest", {"UBQ_RESULT_DIR": directory}, event_path=event_path)
            failures, not_run = ctest_reporting.record_results(
                reporter, root / "build", junit_path,
                ["passed", "failed", "skipped", "later"], 8, log_path,
                {"UBQ_BUILD_ROOT": str(root)})
            self.assertEqual([name for name, _ in failures], ["failed"])
            self.assertEqual(not_run, ["later"])
            events = [json.loads(line) for line in event_path.read_text().splitlines()]
            by_name = {event["test"]: event for event in events}
            self.assertEqual(by_name["passed"]["exitStatus"], 0)
            self.assertEqual(by_name["failed"]["file"], "Tests/Core/test_example.cpp")
            self.assertEqual(by_name["failed"]["line"], 42)
            self.assertEqual(by_name["failed"]["reason"], "got 1, want 2")
            self.assertEqual(by_name["failed"]["testCases"], ["preservesTail"])
            self.assertNotIn("exitStatus", by_name["failed"])
            self.assertEqual(by_name["failed"]["suiteExitStatus"], 8)
            self.assertEqual(by_name["skipped"]["state"], "skipped")
            self.assertEqual(by_name["later"]["state"], "unexecuted")
            self.assertIn("^later$", by_name["later"]["command"])
            self.assertIn("$(python3 Scripts/build_root.py .)", by_name["later"]["command"])

    def test_junit_rejects_results_outside_the_discovered_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            junit_path = root / "results.xml"
            junit_path.write_text('<testsuite><testcase name="stale"/></testsuite>')
            reporter = ctest_reporting.Reporter("ctest", {})
            with self.assertRaisesRegex(ValueError, "unselected test: stale"):
                ctest_reporting.record_results(
                    reporter, root / "build", junit_path, ["fresh"], 0,
                    root / "ctest.log", {})

    def test_shell_test_reproduction_uses_the_direct_stanza(self):
        command = ctest_reporting.reproduce_command(
            Path("/runner/build/ctest"), "shell_fuzz_quic_preamble",
            {"UBQ_BUILD_ROOT": "/runner/build"})
        self.assertEqual(command, "./Scripts/run-tests.sh fuzz_quic_preamble")

    def test_stale_junit_is_removed_before_a_start_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            build.mkdir()
            environment = {"UBQ_RESULT_DIR": str(root), "UBQ_REPORT_MODE": "quiet"}
            _, junit_path = ctest_reporting.result_paths(build, environment)
            junit_path.write_text('<testsuite><testcase name="stale"/></testsuite>')
            result = type("Result", (), {
                "status": 127, "elapsed_seconds": 0.01, "tail": ("could not start",)})()
            with patch.dict(os.environ, environment, clear=False), \
                    patch.object(ctest_reporting, "discover", return_value=["fresh"]), \
                    patch.object(ctest_reporting, "supports_junit", return_value=True), \
                    patch.object(ctest_reporting.Reporter, "run_process", return_value=result), \
                    redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                self.assertEqual(ctest_reporting.main(["--build", str(build)]), 127)
            self.assertFalse(junit_path.exists())

    @unittest.skipUnless(shutil.which("c++"), "a C++ compiler is required")
    def test_legacy_summary_never_wraps_failed_assertions_to_success(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "summary.cpp"
            binary = root / "summary"
            source.write_text(
                '#include "Assertions.h"\nint main() { failures = 256; '
                'return UBIQUITTY_TEST_SUMMARY(); }\n')
            built = subprocess.run([
                "c++", "-std=c++23", "-I", str(batteries.ROOT / "Tests"),
                str(source), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            checked = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
            self.assertIn("FAILED", checked.stdout)


class FuzzFailFastTests(unittest.TestCase):
    def test_source_path_preflight_matches_complete_cc_suffix(self):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        start = source.index("verify_source_paths() {")
        checker = source[start:source.index("\nverify_source_paths\n", start)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Source").mkdir()
            source_file = root / "Source/probe.cc"
            source_file.touch()
            runner = root / "check.sh"
            runner.write_text('set -euo pipefail\nROOT="$1"\n' + checker
                              + '\n# Source/probe.cc\nverify_source_paths\n')
            checked = subprocess.run(["bash", str(runner), directory], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            source_file.unlink()
            (root / "Source/probe.c").touch()
            checked = subprocess.run(["bash", str(runner), directory], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
            self.assertIn("source that does not exist: Source/probe.cc", checked.stderr)

    def test_host_tests_fail_before_fuzz_and_fuzz_precedes_long_platform_builds(self):
        keys = [stage.key for stage in batteries.stages("linux")]
        self.assertLess(keys.index("quic"), keys.index("fuzz"))
        self.assertLess(keys.index("host-tests"), keys.index("fuzz"))
        for later in ("host-glibc", "host-musl", "sanitizers"):
            self.assertLess(keys.index("fuzz"), keys.index(later))

    def run_quic_preamble_build(self, status="0"):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        start = source.index("ubq_fuzz_run() {")
        launch = source[start:source.index("\n}\n", start) + 3]
        start = source.index("build_broken() {")
        broken = source[start:source.index("\nbuild_and_run()", start)]
        # The stanza compiles yyjson through this helper, defined once for every
        # fuzz stanza; with FUZZ_CC faked it answers through fake_compile.
        start = source.index("    ubq_yyjson_fuzz_object() {")
        yyjson = source[start:source.index("\n    }\n", start) + 7]
        start = source.index('    echo "=== fuzz_quic_preamble ==="')
        stanza = source[start:source.index("\n    # fuzz_workspace", start)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Source").symlink_to(batteries.ROOT / "Source", target_is_directory=True)
            (root / "Scripts").symlink_to(batteries.ROOT / "Scripts", target_is_directory=True)
            # The stanza reads the daemon's translation units from ubiquitty-host's
            # list in the root CMakeLists.txt, and refuses a list it reads as empty.
            (root / "CMakeLists.txt").symlink_to(batteries.ROOT / "CMakeLists.txt")
            (root / "Tests/Host").mkdir(parents=True)
            (root / "output").mkdir()
            script = r'''set -euo pipefail
ROOT="$1"
OUT="$2"
FUZZ_OBJDIR="$OUT/objects"
QUIC_PREFIX="$OUT/cache with spaces"
QUIC_FRAMEWORKS=unrelated-scalar
THEMES_GEN="$OUT/themes.cpp"
FUZZ_CC=fake_compile
FUZZ_CXX=fake_compile
FUZZ_FLAGS=(-std=c++23 -fsanitize=fuzzer,address,undefined -I"$ROOT/Source")
SKIPPED=0
quic_deps_present() { return 0; }
ubq_fuzz_links() { return 0; }
ubq_fuzz_scratch() { :; }
ubq_skip() { echo "SKIP: $1"; }
fake_compile() {
    local output="" previous="" argument
    for argument in "$@"; do
        if [ "$previous" = -o ]; then output="$argument"; fi
        previous="$argument"
    done
    printf '#!/bin/bash\necho "fuzzer ran" >> "%s/events"\n' "$OUT" > "$output"
    chmod +x "$output"
}
ubq_compile_parallel() {
    printf '%s\0' "$@" > "$OUT/arguments"
    echo "parallel compile attempted" >> "$OUT/events"
    if [ "$UBQ_TEST_BUILD_STATUS" != 0 ]; then
        echo 'undefined reference: injected missing dependency' > "$3"
        return "$UBQ_TEST_BUILD_STATUS"
    fi
    fake_compile -o "$1"
}
''' + launch + broken + yyjson + stanza + '''
if [ "${QUIC_FRAMEWORKS:-}" != unrelated-scalar ]; then
    echo "QUIC fuzzer changed another stanza's framework flags" >&2
    exit 29
fi
echo "later fuzzer" >> "$OUT/events"
'''
            checked = subprocess.run(["bash", "-c", script, "test", directory, str(root / "output")],
                capture_output=True, text=True, env=dict(os.environ,
                    UBQ_TEST_FAIL_FAST="1", FUZZ_REQUIRED="1", UBQ_TEST_BUILD_STATUS=status))
            arguments = root / "output/arguments"
            return checked, (root / "output/events").read_text().splitlines(), (
                arguments.read_bytes().decode().replace(directory + "/Source/", str(batteries.ROOT) + "/Source/")
                .rstrip("\0").split("\0") if arguments.exists() else [])

    def test_quic_fuzzer_uses_cached_parallel_build_with_complete_search_dependencies(self):
        checked, events, arguments = self.run_quic_preamble_build()
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(events, ["parallel compile attempted", "fuzzer ran", "later fuzzer"])
        first = arguments.index("--")
        second = arguments.index("--", first + 1)
        sources = arguments[first + 1:second]
        self.assertIn(str(batteries.ROOT / "Source/Core/Screen/ScrollSearch.cpp"), sources)
        self.assertIn(str(batteries.ROOT / "Source/Core/Rendering/OverlaySpanGather.cpp"), sources)
        # A daemon translation unit read from ubiquitty-host's list, spelled under
        # Core's source root there; the host's entry point is the binary's alone.
        self.assertIn(str(batteries.ROOT / "Source/Platform/Host/HandoverAdmission.cpp"), sources)
        self.assertNotIn(str(batteries.ROOT / "Source/Platform/Host/main.cpp"), sources)
        for source in (batteries.ROOT / "Source/ThirdParty/re2/re2").glob("*.cc"):
            self.assertIn(str(source), sources)
        for name in ("rune.cc", "strutil.cc"):
            self.assertIn(str(batteries.ROOT / "Source/ThirdParty/re2/util" / name), sources)
        self.assertEqual(len(sources), len(set(sources)))
        self.assertIn("-DUBIQUITTY_QUIC=1", arguments[:first])
        self.assertIn("-fsanitize=fuzzer,address,undefined", arguments[:first])
        self.assertEqual(sum("cache with spaces/include" in value for value in arguments[:first]), 1)
        self.assertFalse(any(value.endswith((".a", ".o")) for value in sources))
        self.assertEqual(sum(value.endswith(".a") for value in arguments[second + 1:]), 4)

    def test_required_quic_link_failure_stops_without_running_or_skipping(self):
        checked, events, _ = self.run_quic_preamble_build(status="17")
        self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
        self.assertEqual(events, ["parallel compile attempted"])
        self.assertIn("BUILD-BROKEN: fuzz_quic_preamble", checked.stdout)
        self.assertIn("undefined reference: injected missing dependency", checked.stderr)
        self.assertNotIn("SKIP:", checked.stdout)

    def test_syntax_failure_stops_later_compiles_only_in_fail_fast_mode(self):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        start = source.index("if wanted fuzz_harnesses_compile; then")
        stanza = source[start:source.index("\n# host_binary", start)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Tests").mkdir()
            for name in ("first", "later"):
                (root / "Tests" / f"fuzz_{name}.cpp").write_text("")
            script = """set -euo pipefail
ROOT="$1"
OUT="$1"
CXX_TEST=fake_compile
RAN=0
wanted() { return 0; }
fake_compile() {
    local argument
    for argument in "$@"; do :; done
    printf '%s\\n' "$(basename "$argument")" >> "$OUT/events"
    if [ "$UBQ_TEST_BUILD_STATUS" != 0 ]; then
        echo 'error: injected compiler failure' >&2
        return "$UBQ_TEST_BUILD_STATUS"
    fi
}
""" + stanza + '\necho "later stanza" >> "$OUT/events"\n'
            for fail_fast, status, expected in (("1", "1", ["fuzz_first.cpp"]),
                                                ("0", "1", ["fuzz_first.cpp", "fuzz_later.cpp"]),
                                                ("1", "0", ["fuzz_first.cpp", "fuzz_later.cpp", "later stanza"])):
                with self.subTest(fail_fast=fail_fast, status=status):
                    (root / "events").write_text("")
                    checked = subprocess.run(["bash", "-c", script, "test", directory],
                        capture_output=True, text=True, env=dict(
                            os.environ, UBQ_TEST_FAIL_FAST=fail_fast, UBQ_TEST_BUILD_STATUS=status))
                    self.assertEqual(checked.returncode, int(status), checked.stdout + checked.stderr)
                    self.assertEqual((root / "events").read_text().splitlines(), expected)
                    if status != "0":
                        self.assertIn("error: injected compiler failure", checked.stderr)

    def run_fuzzer(self, *, fail_fast="1", required="1", status="1", compiler="true", runtime="0", exit_status="0"):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        start = source.index("ubq_fuzz_run() {")
        launch = source[start:source.index("\n}\n", start) + 3]
        start = source.index("build_broken() {")
        broken = source[start:source.index("\nbuild_and_run()", start)]
        start = source.index("    ubq_fuzz_build() {")
        stanza = source[start:source.index("\n    # fuzz_kitty", start)]
        start = source.index('    if [ "${FUZZ_REQUIRED:-0}" = "1" ]; then')
        required_check = source[start:source.index('\n    if [ "$(ubq_fuzz_corpus_state)', start)]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "Scripts").symlink_to(batteries.ROOT / "Scripts", target_is_directory=True)
            (root / "Tests").mkdir()
            (root / "Tests" / "fuzz_vt100.cpp").write_text("")
            binary = root / "fake-fuzzer"
            binary.write_text('#!/bin/sh\necho "fuzzer ran" >> "$OUT/events"\nexit "$UBQ_TEST_FUZZ_EXIT"\n')
            binary.chmod(0o755)
            (root / "build.log").write_text("error: injected fuzzer build failure\n")
            (root / "events").write_text("")
            script = """set -euo pipefail
ROOT="$1"
export OUT="$1"
FUZZ_CXX=unused
FUZZ_OBJDIR="$OUT/objects"
FUZZ_FLAGS=(unused)
CORE_SRCS=(unused)
FUZZ_LOG="$OUT/build.log"
SKIPPED=0
ubq_skip() { echo "SKIP: $1"; }
ubq_fuzz_links() { return "$UBQ_TEST_RUNTIME_STATUS"; }
quic_deps_present() { return 0; }
ubq_compile_parallel() {
    echo "compile attempted" >> "$OUT/events"
    if [ "$UBQ_TEST_BUILD_STATUS" != 0 ]; then
        return "$UBQ_TEST_BUILD_STATUS"
    fi
    cp "$OUT/fake-fuzzer" "$1"
}
""" + launch + broken + stanza + '\necho "later fuzzer" >> "$OUT/events"\n' + required_check
            checked = subprocess.run(["bash", "-c", script, "test", directory],
                capture_output=True, text=True, env=dict(
                    os.environ, UBQ_TEST_FAIL_FAST=fail_fast, FUZZ_REQUIRED=required,
                    UBQ_TEST_BUILD_STATUS=status, FUZZ_CC=compiler, UBQ_TEST_RUNTIME_STATUS=runtime,
                    UBQ_TEST_FUZZ_EXIT=exit_status))
            return checked, (root / "events").read_text().splitlines()

    def test_required_fuzzer_compile_failure_stops_without_calling_it_a_skip(self):
        checked, events = self.run_fuzzer()
        self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
        self.assertEqual(events, ["compile attempted"])
        self.assertIn("BUILD-BROKEN: fuzz_vt100", checked.stdout)
        self.assertIn("error: injected fuzzer build failure", checked.stdout + checked.stderr)
        self.assertNotIn("SKIP:", checked.stdout)

    def test_a_harness_that_does_not_build_fails_the_lane_whatever_the_run_requires(self):
        # A compile or link error past the toolchain probes is a broken tree:
        # it is BUILD-BROKEN with the compiler's words, never a SKIP that
        # exits 0, on a developer's machine as in a required run.
        for options in ({"fail_fast": "0"}, {"required": "0"}, {"fail_fast": "0", "required": "0"}):
            with self.subTest(options=options):
                checked, events = self.run_fuzzer(**options)
                self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
                self.assertEqual(events, ["compile attempted"])
                self.assertIn("BUILD-BROKEN: fuzz_vt100", checked.stdout)
                self.assertIn("error: injected fuzzer build failure", checked.stdout + checked.stderr)
                self.assertNotIn("SKIP:", checked.stdout)

    def test_no_harness_declines_for_a_build_that_failed(self):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        self.assertNotIn("the harness did not build", source)
        self.assertNotIn("! ubq_fuzz_build", source)

    def test_optional_compiler_and_runtime_skips_remain_skips(self):
        for options in ({"compiler": "ubq-missing-fuzzer-compiler"}, {"runtime": "1"}):
            with self.subTest(options=options):
                checked, events = self.run_fuzzer(required="0", **options)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                self.assertEqual(events[-1], "later fuzzer")
                self.assertNotIn("fuzzer ran", events)
                self.assertIn("SKIP:", checked.stdout)
                self.assertNotIn("BUILD-BROKEN:", checked.stdout)

    def test_required_success_builds_and_runs_the_fuzzer(self):
        checked, events = self.run_fuzzer(status="0")
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(events, ["compile attempted", "fuzzer ran", "later fuzzer"])
        self.assertIn("1 harness(es) declared", checked.stdout)

    def test_fuzzer_runtime_failure_keeps_exact_status_and_stops_later_harnesses(self):
        checked, events = self.run_fuzzer(status="0", exit_status="17")
        self.assertEqual(checked.returncode, 17, checked.stdout + checked.stderr)
        self.assertEqual(events, ["compile attempted", "fuzzer ran"])
        self.assertIn("exit=17", checked.stdout)
        self.assertNotIn("SKIP:", checked.stdout)


class FuzzLoggingTests(unittest.TestCase):
    begin = b"###### Recommended dictionary. ######\n"
    end = b"###### End of recommended dictionary. ######\n"
    entry = b'"protocol\\000\\\\\\\"" # Uses: 13\n'
    stats = (b"stat::number_of_executed_units: 10000\n"
             b"stat::average_exec_per_sec:     500\n"
             b"stat::new_units_added:          25\n"
             b"stat::slowest_unit_time_sec:    0\n"
             b"stat::peak_rss_mb:              44\n")

    def run_fuzzer(self, output, *, status=0, signal_number=0, arguments=(), output_fd=2):
        with tempfile.TemporaryDirectory(prefix="fuzz logs ") as directory:
            root = Path(directory)
            binary = root / "fake fuzzer"
            binary.write_text(f"""#!{sys.executable}
import os
import signal
import sys
expected = ['-runs=10000', *{arguments!r}, '-verbosity=0', '-print_funcs=0', '-print_pcs=0', '-print_final_stats=1']
assert sys.argv[1:] == expected, sys.argv
os.write({output_fd}, {output!r})
if {signal_number}:
    os.kill(os.getpid(), {signal_number})
sys.exit({status})
""")
            binary.chmod(0o755)
            checked = subprocess.run([sys.executable, str(batteries.ROOT / "Scripts/run-fuzzer.py"),
                "--log-dir", str(root / "logs"), "--", str(binary), "-runs=10000", *arguments],
                capture_output=True, timeout=10)
            logs = sorted((root / "logs").glob("*/output.log"))
            dictionaries = sorted((root / "logs").glob("*/recommended-dictionary.txt"))
            return checked, [path.read_bytes() for path in logs], [path.read_bytes() for path in dictionaries]

    def test_quiet_success_saves_dictionary_and_summarizes_real_statistics(self):
        output = self.begin + self.entry + self.end + self.stats
        checked, logs, dictionaries = self.run_fuzzer(output)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(logs, [output])
        self.assertEqual(dictionaries, [self.begin + self.entry + self.end])
        self.assertEqual(checked.stderr, b"")
        self.assertNotIn(self.entry, checked.stdout)
        self.assertNotIn(b"stat::", checked.stdout)
        self.assertIn(b"exit=0", checked.stdout)
        self.assertIn(b"units=10000", checked.stdout)
        self.assertIn(b"added=25", checked.stdout)
        self.assertIn(b"rss=44MB", checked.stdout)
        self.assertIn(b"recommended-dictionary.txt", checked.stdout)

    def test_unknown_output_inside_dictionary_remains_visible(self):
        diagnostics = (b"WARNING: suspicious dictionary input\n"
                       b"runtime error: signed integer overflow\n"
                       b"    #0 sanitizer frame\n"
                       b"SUMMARY: UndefinedBehaviorSanitizer: failure\n"
                       b"ALARM: timed out\n"
                       b"artifact_prefix='test'; Test unit written to crash-input\n"
                       b'"not a dictionary entry" # Uses: 1 WARNING\n'
                       b"stat::unknown_future_counter: 9\n"
                       b"stat::peak_rss_mb: 4 WARNING\n"
                       b"arbitrary target output: \xff\x00\n")
        output = self.begin + self.entry + diagnostics + self.end + self.stats
        checked, logs, dictionaries = self.run_fuzzer(output)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(checked.stderr, diagnostics)
        self.assertEqual(logs, [output])
        self.assertEqual(dictionaries, [self.begin + self.entry + diagnostics + self.end])

    def test_dictionary_like_output_outside_envelope_is_not_hidden(self):
        output = self.entry + self.end + b"stat::peak_rss_mb: 4"
        checked, logs, _ = self.run_fuzzer(output)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(checked.stderr, output)
        self.assertEqual(logs, [output])

    def test_target_standard_output_is_also_preserved(self):
        output = b"target stdout diagnostic\n"
        checked, logs, _ = self.run_fuzzer(output, output_fd=1)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(checked.stderr, output)
        self.assertEqual(logs, [output])

    def test_crash_timeout_and_other_failure_statuses_are_preserved(self):
        diagnostics = b"ERROR: AddressSanitizer: heap-buffer-overflow\n    #0 frame\nSUMMARY: crash\n"
        for status in (1, 17, 70, 124, 255):
            with self.subTest(status=status):
                checked, logs, _ = self.run_fuzzer(diagnostics + self.stats, status=status)
                self.assertEqual(checked.returncode, status, checked.stdout + checked.stderr)
                self.assertEqual(checked.stderr, diagnostics)
                self.assertEqual(logs, [diagnostics + self.stats])
                self.assertIn(f"exit={status}".encode(), checked.stdout)

    def test_signal_exit_remains_nonzero_with_visible_diagnostics(self):
        checked, logs, _ = self.run_fuzzer(b"last diagnostic\n", signal_number=9)
        self.assertEqual(checked.returncode, 137, checked.stdout + checked.stderr)
        self.assertEqual(checked.stderr, b"last diagnostic\n")
        self.assertEqual(logs, [b"last diagnostic\n"])

    def test_long_and_unterminated_output_is_drained_without_truncation(self):
        output = b"WARNING: " + b"x" * (1024 * 1024) + self.entry + b"unterminated diagnostic"
        checked, logs, _ = self.run_fuzzer(output)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(checked.stderr, output)
        self.assertEqual(logs, [output])

    def test_long_dictionary_is_fully_saved_without_console_noise(self):
        output = self.begin + self.entry * 30000 + self.end
        checked, logs, dictionaries = self.run_fuzzer(output)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertEqual(checked.stderr, b"")
        self.assertEqual(logs, [output])
        self.assertEqual(dictionaries, [output])

    def test_every_real_fuzzer_launch_uses_the_quiet_helper(self):
        source = (batteries.ROOT / "Scripts/run-tests.sh").read_text()
        direct = re.findall(r'^\s*"\$OUT/fuzz_[a-z0-9_]+"\s+-artifact_prefix=', source, re.MULTILINE)
        self.assertEqual(direct, [])
        launches = re.findall(r'ubq_fuzz_run "\$OUT/(fuzz_[a-z0-9_]+)"', source)
        self.assertEqual(len(launches), 45)
        self.assertEqual(len(set(launches)), 20)

    def test_quiet_flags_take_precedence_without_dropping_other_arguments(self):
        checked, _, _ = self.run_fuzzer(b"", arguments=("-verbosity=9", "-print_final_stats=0", "a seed path"))
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertNotIn(b"units=", checked.stdout)

    def test_missing_binary_and_unwritable_log_destination_cannot_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            helper = str(batteries.ROOT / "Scripts/run-fuzzer.py")
            checked = subprocess.run([sys.executable, helper, "--log-dir", str(root / "logs"),
                                      "--", str(root / "missing binary")], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 127, checked.stdout + checked.stderr)
            self.assertIn(b"No such file", checked.stderr)
            self.assertNotIn(b"units=", checked.stdout)
            destination = root / "not a directory"
            destination.write_bytes(b"preserve existing data")
            checked = subprocess.run([sys.executable, helper, "--log-dir", str(destination),
                                      "--", "/usr/bin/true"], capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
            self.assertIn(b"cannot reserve logs", checked.stderr)
            self.assertEqual(destination.read_bytes(), b"preserve existing data")

    def test_concurrent_invocations_preserve_existing_logs_and_symlink_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log_root = root / "logs"
            log_root.mkdir()
            existing = log_root / "fuzz-probe-existing"
            existing.mkdir()
            (existing / "output.log").write_bytes(b"existing log")
            target = root / "original"
            target.write_bytes(b"original dictionary")
            (existing / "recommended-dictionary.txt").symlink_to(target)
            binary = root / "fuzz-probe"
            binary.write_text(f"#!{sys.executable}\nimport os\nos.write(2, {self.begin + self.entry + self.end!r})\n")
            binary.chmod(0o755)
            processes = [subprocess.Popen([sys.executable, str(batteries.ROOT / "Scripts/run-fuzzer.py"),
                "--log-dir", str(log_root), "--", str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                for _ in range(2)]
            try:
                for process in processes:
                    output, errors = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 0, output + errors)
                    self.assertEqual(errors, b"")
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=5)
            self.assertEqual(len(list(log_root.iterdir())), 3)
            self.assertEqual((existing / "output.log").read_bytes(), b"existing log")
            self.assertTrue((existing / "recommended-dictionary.txt").is_symlink())
            self.assertEqual(target.read_bytes(), b"original dictionary")
            new_logs = [path for path in log_root.iterdir() if path != existing]
            for path in new_logs:
                self.assertEqual((path / "output.log").read_bytes(), self.begin + self.entry + self.end)
                self.assertEqual((path / "recommended-dictionary.txt").read_bytes(), self.begin + self.entry + self.end)

    def test_logging_failure_cannot_turn_green_or_leave_the_target_running(self):
        helper = import_module("run-fuzzer")
        native_popen = subprocess.Popen
        processes = []

        def launch(*arguments, **options):
            process = native_popen(*arguments, **options)
            processes.append(process)
            return process

        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory, redirect_stdout(output), redirect_stderr(output), \
                patch.object(helper, "route_output", side_effect=OSError("injected log write failure")), \
                patch.object(helper.subprocess, "Popen", side_effect=launch):
            status = helper.run([sys.executable, "-c", "import time; time.sleep(30)"], Path(directory))
        self.assertEqual(status, 2, output.getvalue())
        self.assertIn("injected log write failure", output.getvalue())
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].poll())
        with self.assertRaises(ProcessLookupError):
            os.kill(processes[0].pid, 0)

    def test_cancellation_kills_only_the_owned_process_group_and_is_not_green(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "fuzz-probe"
            binary.write_text(f"""#!{sys.executable}
import os
import signal
import time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
with open({str(root / 'pid')!r}, 'w') as output:
    output.write(str(os.getpid()))
while True:
    time.sleep(1)
""")
            binary.chmod(0o755)
            process = subprocess.Popen([sys.executable, str(batteries.ROOT / "Scripts/run-fuzzer.py"),
                "--log-dir", str(root / "logs"), "--", str(binary)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            child_pid = None
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        child_pid = int((root / "pid").read_text())
                        break
                    except (FileNotFoundError, ValueError):
                        pass
                    time.sleep(0.01)
                self.assertIsNotNone(child_pid)
                process.send_signal(signal.SIGTERM)
                output, errors = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 143, output + errors)
                self.assertIn(b"interrupted by signal 15", errors)
                with self.assertRaises(ProcessLookupError):
                    os.kill(child_pid, 0)
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=5)
                if child_pid is not None:
                    try:
                        os.kill(child_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass


class WebBuildCacheTests(unittest.TestCase):
    source_repositories = ("app",)

    def fixture(self, directory, cached_path):
        root = Path(directory) / "web fixture"
        scripts = root / "Scripts"
        scripts.mkdir(parents=True)
        shutil.copyfile(test_file("Scripts/build-web.sh"), scripts / "build-web.sh")
        carry_library_resolver(scripts)
        core_scripts = root / "Libraries/ubiquitty-core/Scripts"
        core_scripts.mkdir(parents=True)
        (core_scripts / "build-root.sh").write_text(': "${UBQ_BUILD_ROOT:?}"\n')
        (core_scripts / "ubq-roots.sh").write_text('UBQ_CORE_ROOT="$CORE_ROOT"\nUBQ_KIT_ROOT="$KIT_ROOT"\n')
        build = root / "build/web"
        (build / "CMakeFiles").mkdir(parents=True)
        (build / "CMakeCache.txt").write_text(
            f"CMAKE_TOOLCHAIN_FILE:FILEPATH={cached_path}\n")
        (build / "CMakeFiles/kept").write_text("generated\n")
        tools = root / "tools"
        tools.mkdir()
        emcmake = tools / "emcmake"
        emcmake.write_text("""#!/bin/bash
printf '%s\\n' "$@" > "$WEB_CONFIGURE_ARGUMENTS"
exit "$WEB_CONFIGURE_STATUS"
""")
        emcmake.chmod(0o755)
        environment = dict(
            os.environ,
            UBQ_BUILD_ROOT=str(root / "build"),
            WEB_CONFIGURE_ARGUMENTS=str(root / "configure-arguments"),
            WEB_CONFIGURE_STATUS="17",
            UBIQUITTY_CORE_SOURCE_DIR=str(core_scripts.parent),
            PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
        )
        for name in LIBRARY_ROOT_VARIABLES:
            environment.pop(name, None)
        return root, environment, build

    def test_removed_emscripten_toolchain_invalidates_only_generated_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            missing_toolchain = Path(directory) / "removed emscripten/Emscripten.cmake"
            root, environment, build = self.fixture(directory, missing_toolchain)

            checked = subprocess.run(
                ["bash", str(root / "Scripts/build-web.sh")],
                capture_output=True, text=True, env=environment, timeout=10)

            self.assertEqual(checked.returncode, 17, checked.stdout + checked.stderr)
            self.assertFalse((build / "CMakeCache.txt").exists())
            self.assertFalse((build / "CMakeFiles").exists())
            self.assertIn("discarding stale CMake toolchain cache", checked.stderr)

    def test_existing_emscripten_toolchain_keeps_incremental_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment, build = self.fixture(directory, Path("/bin/sh"))

            checked = subprocess.run(
                ["bash", str(root / "Scripts/build-web.sh")],
                capture_output=True, text=True, env=environment, timeout=10)

            self.assertEqual(checked.returncode, 17, checked.stdout + checked.stderr)
            self.assertTrue((build / "CMakeCache.txt").is_file())
            self.assertTrue((build / "CMakeFiles/kept").is_file())
            self.assertNotIn("discarding stale CMake toolchain cache", checked.stderr)


class LinuxSmokeFailFastTests(unittest.TestCase):
    source_repositories = ("app",)

    def run_smokes(self, directory, fail_fast, status="17", failure_token=False, *,
                   modes=("a", "b", "c"), failing="b", vulkan_driver=None):
        root = Path(directory)
        scripts = root / "Scripts"
        scripts.mkdir()
        source = root / "Source/Platform/Linux"
        source.mkdir(parents=True)
        (source / "main.cpp").write_text('\n'.join(f'if (mode == "--smoke-{name}") {{}}' for name in modes))
        shutil.copyfile(test_file("Scripts/run-linux-smokes.sh"), scripts / "run-linux-smokes.sh")
        carry_library_resolver(scripts)
        core_scripts = root / "Libraries/ubiquitty-core/Scripts"
        core_scripts.mkdir(parents=True)
        (core_scripts / "build-root.sh").write_text(': "${UBQ_BUILD_ROOT:?}"\n')
        tools = root / "tools"
        tools.mkdir()
        timeout = tools / "timeout"
        timeout.write_text('#!/bin/bash\nshift\nexec "$@"\n')
        timeout.chmod(0o755)
        binary = root / "fake binary"
        binary.write_text("""#!/bin/bash
printf '%s\\n' "$1" >> "$SMOKE_EVENTS"
if [ "$1" = "--smoke-$SMOKE_FAILING_MODE" ]; then
    echo "diagnostic from the selected smoke"
    [ "$SMOKE_FAILURE_TOKEN" = 1 ] && echo "FAIL: reported by the smoke"
    exit "$SMOKE_EXIT_STATUS"
fi
echo "PASS: smoke completed"
""")
        binary.chmod(0o755)
        events = root / "events"
        environment = dict(os.environ, UBQ_BUILD_ROOT=str(root / "build"),
                           UBIQUITTY_CORE_SOURCE_DIR=str(core_scripts.parent),
                           PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
                           SMOKE_EVENTS=str(events), SMOKE_EXIT_STATUS=status, SMOKE_FAILING_MODE=failing,
                           SMOKE_FAILURE_TOKEN="1" if failure_token else "0", SMOKE_TIMEOUT="9")
        environment.pop("UBQ_TEST_FAIL_FAST", None)
        environment.pop("VK_ICD_FILENAMES", None)
        if vulkan_driver is not None:
            environment["VK_ICD_FILENAMES"] = vulkan_driver
        for name in LIBRARY_ROOT_VARIABLES:
            environment.pop(name, None)
        if fail_fast is not None:
            environment["UBQ_TEST_FAIL_FAST"] = fail_fast
        checked = subprocess.run(["bash", str(scripts / "run-linux-smokes.sh"), str(binary)],
                                 capture_output=True, text=True, env=environment)
        return checked, events.read_text().splitlines(), binary

    def test_ci_stops_after_failure_and_preserves_diagnostic_and_command(self):
        for status in ("17", "124"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                checked, events, binary = self.run_smokes(directory, "1", status)
                self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
                self.assertEqual(events, ["--smoke-a", "--smoke-b"])
                self.assertIn(f"--smoke-b (rc={status})", checked.stdout)
                self.assertIn("diagnostic from the selected smoke", checked.stdout)
                self.assertIn("1 passed, 1 failed, 1 unexecuted", checked.stdout)
                command = next(line.split("reproduce: ", 1)[1] for line in checked.stderr.splitlines() if "reproduce:" in line)
                self.assertEqual(shlex.split(command), ["timeout", "9", str(binary), "--smoke-b"])

    def test_local_default_keeps_collecting(self):
        for fail_fast in (None, "0"):
            with self.subTest(fail_fast=fail_fast), tempfile.TemporaryDirectory() as directory:
                checked, events, binary = self.run_smokes(directory, fail_fast)
                self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
                self.assertEqual(events, ["--smoke-a", "--smoke-b", "--smoke-c"])
                self.assertIn("2 passed, 1 failed, 0 unexecuted", checked.stdout)

    def test_failure_token_with_zero_exit_still_stops_ci(self):
        with tempfile.TemporaryDirectory() as directory:
            checked, events, binary = self.run_smokes(directory, "1", "0", failure_token=True)
            self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
            self.assertEqual(events, ["--smoke-a", "--smoke-b"])

    def test_green_ci_catalog_executes_every_smoke(self):
        with tempfile.TemporaryDirectory() as directory:
            checked, events, binary = self.run_smokes(directory, "1", "0")
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertEqual(events, ["--smoke-a", "--smoke-b", "--smoke-c"])
            self.assertIn("3 passed, 0 failed, 0 unexecuted", checked.stdout)

    LAVAPIPE_MANIFESTS = ("/usr/share/vulkan/icd.d/lvp_icd.json", "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json")

    def run_exporter_smoke(self, directory, mode, status, vulkan_driver):
        return self.run_smokes(directory, "1", status, modes=("a", mode, "z"), failing=mode,
                               vulkan_driver=vulkan_driver)

    def test_listed_mode_without_an_exporter_is_skipped_and_counted_under_lavapipe(self):
        for manifest in self.LAVAPIPE_MANIFESTS:
            with self.subTest(manifest=manifest), tempfile.TemporaryDirectory() as directory:
                checked, events, binary = self.run_exporter_smoke(directory, "dmabuf-import-failure", "77", manifest)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                self.assertEqual(events, ["--smoke-a", "--smoke-dmabuf-import-failure", "--smoke-z"])
                self.assertIn("SKIPPED --smoke-dmabuf-import-failure (lavapipe cannot export dma-buf;"
                              " runs where UBQ_CI_LINUX_VULKAN=native)", checked.stdout)
                self.assertIn("2 passed, 0 failed, 0 unexecuted, 1 skipped on lavapipe", checked.stdout)

    def test_skip_is_refused_off_lavapipe_for_unlisted_modes_and_for_other_statuses(self):
        for mode, status, manifest in (("dmabuf-import-failure", "77", None),
                                       ("dmabuf-import-failure", "77", "/usr/share/vulkan/icd.d/radeon_icd.x86_64.json"),
                                       ("b", "77", self.LAVAPIPE_MANIFESTS[0]),
                                       ("dmabuf-import-failure", "1", self.LAVAPIPE_MANIFESTS[0])):
            with self.subTest(mode=mode, status=status, manifest=manifest), tempfile.TemporaryDirectory() as directory:
                checked, events, binary = self.run_exporter_smoke(directory, mode, status, manifest)
                self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
                self.assertEqual(events, ["--smoke-a", f"--smoke-{mode}"])
                self.assertIn(f"FAIL  --smoke-{mode} (rc={status})", checked.stdout)
                self.assertNotIn("SKIPPED", checked.stdout)
                self.assertIn("1 passed, 1 failed, 1 unexecuted, 0 skipped on lavapipe", checked.stdout)


class UIScenarioResultTests(unittest.TestCase):
    source_repositories = ("app",)

    def setUp(self):
        specification = spec_from_file_location("ui_result_gate_fixture", test_file("Scripts/check-ui-scenarios.py"))
        self.gate = module_from_spec(specification)
        specification.loader.exec_module(self.gate)
        for name, values in (("KNOWN_FAILING", {"known_failure": "owned-failure"}),
                             ("KNOWN_FLAKY", {"flaky": "owned-flake"})):
            replacement = patch.object(self.gate, name, values)
            replacement.start()
            self.addCleanup(replacement.stop)

    def output(self, failed=("known_failure",), passed=("good", "flaky"), totals=None):
        counts = totals if totals is not None else (len(passed), len(failed) + len(passed))
        return "\n".join([*(f"FAIL {name}" for name in failed),
                          *(f"PASS {name}" for name in passed),
                          f"{counts[0]}/{counts[1]} scenarios passed"])

    def verdict(self, output, *, expected=("known_failure", "flaky"), returncode=1):
        captured = io.StringIO()
        with redirect_stdout(captured), redirect_stderr(captured):
            status = self.gate.verdict(*self.gate.parse(output), expected=expected, returncode=returncode)
        return status, captured.getvalue()

    def test_named_failures_and_flakes_preserve_their_policy(self):
        for failed, passed in ((("known_failure",), ("good", "flaky")),
                               (("known_failure", "flaky"), ("good",))):
            with self.subTest(failed=failed):
                status, output = self.verdict(self.output(failed, passed))
                self.assertEqual(status, 0, output)
                self.assertIn("UNGATED", output)
        status, output = self.verdict(self.output(failed=(), passed=("known_failure", "flaky", "good")),
                                      returncode=0)
        self.assertEqual(status, 1)
        self.assertIn("now passing", output)

    def test_empty_or_mismatched_totals_cannot_pass(self):
        for output, message in (("0/0 scenarios passed", "did not execute any"),
                                (self.output(totals=(99, 3)), "passing scenarios but named"),
                                (self.output(totals=(2, 99)), "scenarios but named")):
            with self.subTest(output=output):
                status, diagnostic = self.verdict(output)
                self.assertEqual(status, 1)
                self.assertIn(message, diagnostic)

    def test_missing_expected_names_fail_without_expanding_filtered_scope(self):
        output = self.output(failed=(), passed=("good",))
        status, diagnostic = self.verdict(output, returncode=0)
        self.assertEqual(status, 1)
        self.assertIn("expected scenario(s) did not execute", diagnostic)
        self.assertIn("known_failure", diagnostic)
        status, diagnostic = self.verdict(output, expected=("good",), returncode=0)
        self.assertEqual(status, 0, diagnostic)

    def test_duplicate_results_and_totals_are_not_deduplicated_into_success(self):
        for prefix in ("PASS good\n", "FAIL flaky\n", "FAIL known_failure\n", "2/3 scenarios passed\n"):
            with self.subTest(prefix=prefix):
                status, output = self.verdict(prefix + self.output())
                self.assertEqual(status, 1)
                self.assertIn("invalid suite result accounting", output)
        status, output = self.verdict(self.output() + "\nPASS late")
        self.assertEqual(status, 1)
        self.assertIn("reported after the suite total", output)

    def test_a_child_application_prints_into_a_log_not_into_the_suite(self):
        # A scenario that starts a second process of the app hands it this
        # run's stdout unless it routes it elsewhere, and the child's result
        # and tally then stand among the suite's, which the accounting above
        # rejects for the whole run. Every such launch goes through the one
        # helper that writes the child's output to a log of the scenario's.
        sources = TEST_SOURCES.subject / "Source/Platform/macOS/UITest/Swift"
        launchers = sorted(path.name for path in sources.glob("*.swift")
                           if "Bundle.main.executableURL" in path.read_text(encoding="utf-8"))
        self.assertEqual(launchers, ["NativeTestContext.swift"])
        helper = (sources / "NativeTestContext.swift").read_text(encoding="utf-8")
        self.assertEqual(helper.count("Bundle.main.executableURL"), 1)
        start = helper.index("func runApplicationPhase(")
        body = helper[start:helper.index("\n    }\n", start)]
        self.assertIn("Bundle.main.executableURL", body)
        for stream in ("standardOutput", "standardError"):
            self.assertRegex(body, rf"\bchild\.{stream} = log\n")

    def test_contradictory_sets_are_rejected_even_without_parser_diagnostics(self):
        output = io.StringIO()
        with redirect_stdout(output), redirect_stderr(output):
            status = self.gate.verdict({"flaky"}, {"flaky"}, (1, 2))
        self.assertEqual(status, 1)
        self.assertIn("both PASS and FAIL", output.getvalue())

    def test_refusal_and_abnormal_exit_cannot_hide_behind_valid_totals(self):
        status, output = self.verdict("REFUSED refused\n" + self.output(), returncode=2)
        self.assertEqual(status, 1)
        self.assertIn("refused; it did not execute", output)
        for returncode in (2, 127, -9, -11):
            with self.subTest(returncode=returncode):
                status, output = self.verdict(self.output(), returncode=returncode)
                self.assertEqual(status, 1)
                self.assertIn(f"exit {returncode}", output)

    def test_exit_code_must_agree_with_reported_results(self):
        status, output = self.verdict(self.output(), returncode=0)
        self.assertEqual(status, 1)
        self.assertIn("disagrees with its reported results", output)
        status, output = self.verdict(self.output(failed=(), passed=("good",)),
                                      expected=("good",), returncode=1)
        self.assertEqual(status, 1)
        self.assertIn("disagrees with its reported results", output)

    def test_entry_point_preserves_exit_and_full_suite_name_checks(self):
        cases = ((self.output(), 1, 0), (self.output(), 2, 1),
                 (self.output(failed=(), passed=("good",)), 0, 1))
        for output, returncode, expected_status in cases:
            with self.subTest(returncode=returncode, output=output):
                completed = subprocess.CompletedProcess([], returncode, output, "")
                with patch.object(self.gate.sys, "argv", ["check-ui-scenarios.py"]), \
                        patch.object(self.gate, "check_entries", return_value=0), \
                        patch.object(self.gate.os, "access", return_value=True), \
                        patch.object(self.gate.subprocess, "run", return_value=completed) as run, \
                        redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    self.assertEqual(self.gate.main(), expected_status)
                self.assertEqual(run.call_count, 1)
                self.assertEqual(Path(run.call_args.args[0][0]).name, "run-ui-tests.sh")

    def test_ungated_results_warn_but_actual_regressions_remain_errors(self):
        report = self.gate.gate_report
        with patch.dict(os.environ, {"UBQ_CI_ANNOTATE": "1"}), \
                patch.object(report, "_annotationsEmitted", 0), \
                patch.object(report, "summary_row"):
            status, output = self.verdict(self.output())
            self.assertEqual(status, 0, output)
            self.assertIn("::warning title=check-ui-scenarios::", output)
            self.assertNotIn("::error", output)
            status, output = self.verdict(self.output(failed=("known_failure", "regression")))
            self.assertEqual(status, 1)
            self.assertIn("::error title=check-ui-scenarios::", output)
            self.assertIn("regression", output)


UI_TEST_BUNDLE_IDENTIFIER = "org.ubiquitty.Ubiquitty.uitest"


class UITestLaunchTests(unittest.TestCase):
    source_repositories = ("app",)

    def fixture(self, directory, bundle_identifier=UI_TEST_BUNDLE_IDENTIFIER, plist_format=plistlib.FMT_XML):
        root = Path(directory) / "UI fixture"
        scripts = root / "Scripts"
        scripts.mkdir(parents=True)
        # The script and what it sources: the run's hygiene is part of what
        # launches and ends a scenario, so the fixture carries the real one.
        shutil.copyfile(test_file("Scripts/run-ui-tests.sh"), scripts / "run-ui-tests.sh")
        carry_library_resolver(scripts)
        core_scripts = root / "Libraries/ubiquitty-core/Scripts"
        core_scripts.mkdir(parents=True)
        for name in ("test-run-hygiene.sh", "test-run-hygiene.py"):
            shutil.copyfile(test_file("Scripts/" + name), core_scripts / name)
        (core_scripts / "build-root.sh").write_text(': "${UBQ_BUILD_ROOT:?}"\n')

        def program(path, content):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#!/bin/bash\nset -eu\n" + content)
            path.chmod(0o755)

        program(scripts / "build-macos.sh", """
printf '%s\\n' "$UBIQUITTY_CMAKE_EXTRA" > "$UI_FLAGS"
printf '%s\\n' "$UBIQUITTY_QUIC_PREFIX" > "$UI_QUIC_PREFIX"
printf 'build-macos\\n' >> "$UI_EVENTS"
[ "$UBIQUITTY_HELPERS" = 1 ]
[ "$UBIQUITTY_LINUX_HOSTS" = "${UI_EXPECT_LINUX_HOSTS:-0}" ]
exit "${UI_BUILD_STATUS:-0}"
""")
        program(scripts / "build-host.sh", "printf 'build-host\\n' >> \"$UI_EVENTS\"\n")
        program(root / "tools/xcrun", """
[ "$1 $2" = 'nm -gU' ]
[ -x "$3" ]
printf 'inspect-binary\\n' >> "$UI_EVENTS"
printf '%s\\n' "$UI_SYMBOLS"
exit "${UI_SYMBOL_STATUS:-0}"
""")
        program(root / "tools/codesign", """
printf 'verify-signature\\n' >> "$UI_EVENTS"
exit "${UI_SIGN_STATUS:-0}"
""")
        program(root / "tools/defaults", "printf '%s\\n' \"$*\" >> \"$UI_DEFAULTS\"\n")
        build = root / "build"
        program(build / "host/ubiquitty-host", "exit 0\n")
        bundle = build / "macos-uitest/Source/Platform/macOS/Ubiquitty.app"
        program(bundle / "Contents/Helpers/ubiquitty-host", "exit 0\n")
        program(bundle / "Contents/MacOS/Ubiquitty", """
printf 'launch:%s:%s\\n' "$1" "$2" >> "$UI_EVENTS"
printf '%s\\n' "$XDG_CONFIG_HOME" > "$UI_CONFIG"
""")
        if bundle_identifier is not None:
            with open(bundle / "Contents/Info.plist", "wb") as plist:
                plistlib.dump({"CFBundleIdentifier": bundle_identifier}, plist, fmt=plist_format)
        home = root / "home"
        home.mkdir()
        environment = dict(os.environ, UBQ_BUILD_ROOT=str(build), UI_EVENTS=str(root / "events"),
                           UBIQUITTY_CORE_SOURCE_DIR=str(core_scripts.parent),
                           HOME=str(home), UI_DEFAULTS=str(root / "defaults-calls"),
                           UI_FLAGS=str(root / "flags"), UI_CONFIG=str(root / "config"),
                           UI_QUIC_PREFIX=str(root / "quic-prefix"),
                           UI_SYMBOLS="00000001 S _OBJC_CLASS_$_UBUITestRunner",
                           PATH=str(root / "tools") + os.pathsep + os.environ.get("PATH", ""))
        environment.pop("UBIQUITTY_BUILD_DIR", None)
        environment.pop("UBIQUITTY_CMAKE_EXTRA", None)
        environment.pop("UBIQUITTY_QUIC_PREFIX", None)
        environment.pop("UBIQUITTY_LINUX_HOSTS", None)
        for name in LIBRARY_ROOT_VARIABLES:
            environment.pop(name, None)
        return root, environment

    def run_ui_fixture(self, root, environment, *arguments):
        # The bound is against the build-lock wait, which has no other end; it
        # is not a budget. A run that reaches the launch spends its time in the
        # hygiene sweep's `ps -axEww` passes over every process, which another
        # push's compilers slow badly, so a tight bound times these out when
        # two pushes gate at once.
        checked = subprocess.run(["bash", str(root / "Scripts/run-ui-tests.sh"), *arguments],
                                 capture_output=True, text=True, env=environment, timeout=120)
        return checked, (root / "events").read_text().splitlines()

    def test_missing_build_root_is_created_before_the_lock_is_acquired(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            populated_build = Path(environment["UBQ_BUILD_ROOT"])
            fresh_build = root / "fresh build"
            environment["UBIQUITTY_BUILD_DIR"] = str(populated_build / "macos-uitest")
            environment["UBQ_BUILD_ROOT"] = str(fresh_build)

            checked, events = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertEqual(events, ["build-macos", "inspect-binary", "verify-signature", "launch:--ui-test:all"])
            self.assertTrue(fresh_build.is_dir())
            self.assertFalse((fresh_build / ".run-ui-tests-build.lock").exists())
            self.assertEqual((root / "quic-prefix").read_text().strip(), str(fresh_build / "quic-deps"))

    def test_invalid_signature_never_launches(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            environment["UI_SIGN_STATUS"] = "1"
            checked, events = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 2)
            self.assertIn("signature is invalid", checked.stderr)
            self.assertEqual(events, ["build-macos", "inspect-binary", "verify-signature"])
            self.assertFalse((root / "defaults-calls").exists())

    def test_missing_sealed_helper_never_launches(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            helper = Path(environment["UBQ_BUILD_ROOT"]) / "macos-uitest/Source/Platform/macOS/Ubiquitty.app/Contents/Helpers/ubiquitty-host"
            helper.unlink()
            checked, events = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 1)
            self.assertIn("no host helper", checked.stderr)
            self.assertEqual(events, ["build-macos", "inspect-binary"])

    def test_explicit_quic_cache_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            explicit_prefix = root / "shared quic"
            environment["UBIQUITTY_QUIC_PREFIX"] = str(explicit_prefix)

            checked, _ = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertEqual((root / "quic-prefix").read_text().strip(), str(explicit_prefix))

    def test_linux_deployment_helpers_are_built_only_when_requested(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            environment.update(UBIQUITTY_LINUX_HOSTS="1", UI_EXPECT_LINUX_HOSTS="1")
            checked, _ = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_test_mode_wins_over_conflicting_options_and_keeps_other_flags(self):
        for extra in ("", "-DUBIQUITTY_UI_TESTS=OFF", "-DUBIQUITTY_UI_TESTS:BOOL=OFF -DUBIQUITTY_QUIC=OFF"):
            with self.subTest(extra=extra), tempfile.TemporaryDirectory() as directory:
                root, environment = self.fixture(directory)
                environment["UBIQUITTY_CMAKE_EXTRA"] = extra
                checked, events = self.run_ui_fixture(root, environment)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                self.assertEqual(shlex.split((root / "flags").read_text()),
                                 shlex.split(extra) + ["-DUBIQUITTY_UI_TESTS=ON"])
                self.assertEqual(events, ["build-macos", "inspect-binary", "verify-signature", "launch:--ui-test:all"])
                self.assertFalse(Path((root / "config").read_text().strip()).exists())

    def test_absent_undefined_or_similar_entry_point_never_launches(self):
        for symbols in ("", "         U _OBJC_CLASS_$_UBUITestRunner",
                        "00000001 S _OBJC_CLASS_$_UBUITestRunner.extra"):
            with self.subTest(symbols=symbols), tempfile.TemporaryDirectory() as directory:
                root, environment = self.fixture(directory)
                environment["UI_SYMBOLS"] = symbols
                checked, events = self.run_ui_fixture(root, environment)
                self.assertEqual(checked.returncode, 2)
                self.assertIn("refusing to launch", checked.stderr)
                self.assertEqual(events, ["build-macos", "inspect-binary"])

    def test_a_bundle_without_a_ui_test_identifier_never_launches(self):
        # The installed app's identifier, an absent Info.plist, and one that
        # only contains the suffix: each would run under a domain the reader
        # owns, so none reaches the host build, the domain clear or the launch.
        for identifier in ("org.ubiquitty.Ubiquitty", None, "org.ubiquitty.Ubiquitty.uitest.copy"):
            with self.subTest(identifier=identifier), tempfile.TemporaryDirectory() as directory:
                root, environment = self.fixture(directory, bundle_identifier=identifier)
                checked, events = self.run_ui_fixture(root, environment)
                self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
                self.assertIn("not a UI-test one", checked.stderr)
                self.assertIn("refusing to launch", checked.stderr)
                self.assertEqual(events, ["build-macos", "inspect-binary"])
                self.assertFalse((root / "defaults-calls").exists())

    def test_the_ui_test_domain_and_its_saved_state_are_cleared_before_launch(self):
        # A binary Info.plist reads the same as the XML one CMake writes.
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory, plist_format=plistlib.FMT_BINARY)
            saved_state = root / "home/Library/Saved Application State"
            test_state = saved_state / f"{UI_TEST_BUNDLE_IDENTIFIER}.savedState"
            readers_state = saved_state / "org.ubiquitty.Ubiquitty.savedState"
            test_state.mkdir(parents=True)
            readers_state.mkdir(parents=True)

            checked, events = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertEqual((root / "defaults-calls").read_text().splitlines(),
                             [f"delete {UI_TEST_BUNDLE_IDENTIFIER}"])
            self.assertFalse(test_state.exists())
            self.assertTrue(readers_state.exists())
            self.assertEqual(events, ["build-macos", "inspect-binary", "verify-signature", "launch:--ui-test:all"])

    def test_symbol_tool_failure_is_not_masked_by_a_matching_line(self):
        for status in ("17", "127"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                root, environment = self.fixture(directory)
                environment["UI_SYMBOL_STATUS"] = status
                checked, events = self.run_ui_fixture(root, environment)
                self.assertEqual(checked.returncode, 2)
                self.assertIn("refusing to launch", checked.stderr)
                self.assertEqual(events, ["build-macos", "inspect-binary"])

    def test_failed_build_cannot_launch_an_existing_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            environment["UI_BUILD_STATUS"] = "13"
            checked, events = self.run_ui_fixture(root, environment)
            self.assertEqual(checked.returncode, 13)
            self.assertEqual(events, ["build-macos"])

    def test_only_explicit_scenario_arguments_filter_the_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            root, environment = self.fixture(directory)
            environment["SCENARIO"] = "unrequested"
            checked, events = self.run_ui_fixture(root, environment, "first", "second")
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertEqual(events[-1], "launch:--ui-test:first,second")


class VerdictProbeIsolationTests(unittest.TestCase):
    def git(self, root, environment, *arguments, input=None):
        return subprocess.check_output(["git", "-C", str(root), *arguments],
                                       env=environment, input=input, text=True).strip()

    def fixture(self, directory, body):
        root = Path(directory) / "repository"
        scripts = root / "Scripts"
        scripts.mkdir(parents=True)
        (root / "Source").mkdir()
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        self.git(root, environment, "init", "-q")
        tree = self.git(root, environment, "mktree", input="")
        arguments = ("-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid", "commit-tree", tree)
        tip = self.git(root, environment, *arguments, "-m", "first")
        other = self.git(root, environment, *arguments, "-p", tip, "-m", "second")
        self.git(root, environment, "update-ref", "HEAD", tip)
        self.git(root, environment, "update-ref", "refs/heads/ubq-verdict-cache-probe", tip)
        (root / "Source/zz_verdict_cache_probe.h").write_text("operator data\n")
        source = (batteries.ROOT / "Scripts/check-gate-verdict-cache.sh").read_text()
        prefix = source[:source.index("\nfailures=0\n")]
        (scripts / "gate-verdict-cache.sh").write_text("")
        script = scripts / "probe.sh"
        script.write_text(prefix + body)
        return root, environment, script, tip, other

    def test_concurrent_runs_own_distinct_refs_and_files(self):
        body = """
setProbeRef HEAD || exit 2
createProbeFile || exit 2
printf '%s\\n' "$PROBE_REF" "$PROBE_FILE"
read -r -t 10 signal
"""
        with tempfile.TemporaryDirectory() as directory:
            root, environment, script, tip, other = self.fixture(directory, body)
            processes, identities = [], []
            try:
                for _ in range(2):
                    process = subprocess.Popen(["bash", str(script)], env=environment, text=True,
                                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    processes.append(process)
                    identities.append((process.stdout.readline().strip(), process.stdout.readline().strip()))
                self.assertNotEqual(identities[0][0], identities[1][0])
                self.assertNotEqual(identities[0][1], identities[1][1])
                for ref, probe in identities:
                    self.assertTrue(ref.startswith("refs/ubiquitty/verdict-probes/"), ref)
                    self.assertEqual(self.git(root, environment, "rev-parse", ref), tip)
                    self.assertEqual((root / probe).read_text(), "// probe\n")
                for process in processes:
                    stdout, stderr = process.communicate("continue\n", timeout=10)
                    self.assertEqual(process.returncode, 0, stdout + stderr)
                self.assertEqual(self.git(root, environment, "for-each-ref", "--format=%(refname)",
                                          "refs/ubiquitty/verdict-probes/"), "")
                self.assertTrue(all(not (root / probe).exists() for _, probe in identities))
                self.assertEqual(self.git(root, environment, "rev-parse", "refs/heads/ubq-verdict-cache-probe"), tip)
                self.assertEqual((root / "Source/zz_verdict_cache_probe.h").read_text(), "operator data\n")
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=10)

    def test_ref_replacement_is_neither_overwritten_nor_deleted(self):
        body = """
setProbeRef HEAD || exit 2
printf '%s\\n' "$PROBE_REF"
read -r -t 10 signal
setProbeRef HEAD || exit 2
"""
        with tempfile.TemporaryDirectory() as directory:
            root, environment, script, tip, other = self.fixture(directory, body)
            with subprocess.Popen(["bash", str(script)], env=environment, text=True,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
                ref = process.stdout.readline().strip()
                self.assertTrue(ref.startswith("refs/ubiquitty/verdict-probes/"), ref)
                self.git(root, environment, "update-ref", ref, other, tip)
                stdout, stderr = process.communicate("continue\n", timeout=10)
                self.assertEqual(process.returncode, 2, stdout + stderr)
                self.assertEqual(self.git(root, environment, "rev-parse", ref), other)

    def test_existing_probe_file_is_not_overwritten_or_removed(self):
        body = """
printf 'operator data\\n' > "$PROBE_FILE"
printf '%s\\n' "$PROBE_FILE"
createProbeFile
"""
        with tempfile.TemporaryDirectory() as directory:
            root, environment, script, tip, other = self.fixture(directory, body)
            checked = subprocess.run(["bash", str(script)], env=environment, text=True,
                                     capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 2)
            self.assertIn("cannot create owned probe", checked.stderr)
            self.assertEqual((root / checked.stdout.strip()).read_text(), "operator data\n")


class TidyGateTests(unittest.TestCase):
    def run_tidy(self, changed_paths, diagnostic="", status=0, diagnostic_stream="stdout"):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            scripts = root / "Scripts"
            scripts.mkdir()
            for name in ("check-tidy.sh", "gate-report-lib.sh", "build-root.sh", "ccache-env.sh"):
                shutil.copyfile(batteries.ROOT / "Scripts" / name, scripts / name)
            shutil.copyfile(batteries.ROOT / ".clang-tidy", root / ".clang-tidy")
            source = root / "Source/Probe.cpp"
            source.parent.mkdir()
            source.write_text("int value;\n")
            build = root / "build/macos"
            build.mkdir(parents=True)
            (build / "compile_commands.json").write_text(json.dumps([{
                "directory": str(root), "file": str(source),
                "command": "c++ -std=c++23 -c " + shlex.quote(str(source)),
            }]))
            (root / "changed-paths").write_text(changed_paths)
            tools = root / "tools"
            tools.mkdir()
            git = tools / "git"
            git.write_text(
                "#!/usr/bin/env python3\n"
                "from pathlib import Path\nimport os, signal, sys\n"
                "signal.signal(signal.SIGPIPE, signal.SIG_DFL)\n"
                "arguments = sys.argv[1:]\n"
                "if arguments[0] == 'rev-parse':\n    print('fixture-head')\n"
                "elif arguments[:2] == ['diff', '--name-only']:\n"
                "    sys.stdout.write((Path(os.environ['UBQ_TIDY_FIXTURE']) / 'changed-paths').read_text())\n"
                "elif arguments[0] == 'diff':\n"
                "    print('+++ b/Source/Probe.cpp\\n@@ -0,0 +1 @@\\n+int value;')\n"
                "elif arguments[0] == 'ls-files':\n    print('Source/Probe.cpp')\n"
                "else:\n    sys.exit(1)\n")
            git.chmod(0o755)
            brew = tools / "brew"
            brew.write_text('#!/bin/sh\nprintf "%s\\n" "$UBQ_TIDY_FIXTURE/llvm"\n')
            brew.chmod(0o755)
            analyzer = root / "llvm/bin/clang-tidy"
            analyzer.parent.mkdir(parents=True)
            analyzer.write_text(
                "#!/usr/bin/env python3\n"
                "from pathlib import Path\nimport json, os, sys\n"
                "root = Path(os.environ['UBQ_TIDY_FIXTURE'])\n"
                "(root / 'analyzer-arguments').write_text(json.dumps(sys.argv[1:]))\n"
                "message = os.environ['UBQ_TIDY_DIAGNOSTIC'].replace('{source}', str(root / 'Source/Probe.cpp'))\n"
                "if message:\n    print(message, file=getattr(sys, os.environ['UBQ_TIDY_STREAM']))\n"
                "sys.exit(int(os.environ['UBQ_TIDY_STATUS']))\n")
            analyzer.chmod(0o755)
            environment = {**os.environ, "PATH": str(tools) + os.pathsep + os.environ["PATH"],
                           "UBQ_BUILD_ROOT": str(root / "build"), "UBQ_TIDY_FIXTURE": str(root),
                           "UBQ_TIDY_DIAGNOSTIC": diagnostic, "UBQ_TIDY_STATUS": str(status),
                           "UBQ_TIDY_STREAM": diagnostic_stream, "UBQ_CI_ANNOTATE": ""}
            checked = subprocess.run(["bash", str(scripts / "check-tidy.sh"), "--since", "fixture-base"],
                                     cwd=root, env=environment, capture_output=True, text=True, timeout=30)
            arguments_path = root / "analyzer-arguments"
            arguments = json.loads(arguments_path.read_text()) if arguments_path.exists() else []
            return checked, arguments

    def test_large_diff_with_an_early_source_file_runs_the_analyzer(self):
        # The producer must still be writing when quiet grep finds the first
        # source; its SIGPIPE under pipefail must not become a clean no-op.
        changed = "Source/Probe.cpp\n" + "".join(
            "Docs/" + "long-path-" * 16 + str(index) + ".md\n" for index in range(16384))
        checked, arguments = self.run_tidy(changed)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("Source/Probe.cpp", arguments)
        self.assertNotIn("no compilable file changed", checked.stdout)

    def test_documentation_only_and_empty_diffs_skip_the_analyzer(self):
        for changed in ("", "Docs/architecture.md\nScripts/check-tidy.sh\n"):
            with self.subTest(changed=changed):
                checked, arguments = self.run_tidy(changed)
                self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                self.assertEqual(arguments, [])
                self.assertIn("no compilable file changed", checked.stdout)

    def test_frontend_errors_outside_changed_lines_fail_the_audit(self):
        message = "{source}:7:1: error: unknown type name 'Broken' [clang-diagnostic-error]"
        for status, stream in ((1, "stdout"), (0, "stdout"), (1, "stderr")):
            with self.subTest(status=status, stream=stream):
                checked, arguments = self.run_tidy("Source/Probe.cpp\n", message, status, stream)
                self.assertTrue(arguments)
                self.assertNotEqual(checked.returncode, 0, checked.stdout + checked.stderr)
                self.assertIn("unknown type name 'Broken'", checked.stderr)
                self.assertIn("analysis incomplete", checked.stderr)
                self.assertNotIn("check-tidy: ok", checked.stdout)

    def test_analyzer_failure_without_a_diagnostic_fails_the_audit(self):
        checked, arguments = self.run_tidy("Source/Probe.cpp\n", status=3)
        self.assertTrue(arguments)
        self.assertNotEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("analysis incomplete", checked.stderr)

    def test_ignored_attribute_warnings_on_changed_lines_still_fail(self):
        message = "{source}:1:1: warning: attribute is ignored [clang-diagnostic-ignored-attributes]"
        checked, arguments = self.run_tidy("Source/Probe.cpp\n", message)
        self.assertIn("-extra-arg=-Wno-error=ignored-attributes", arguments)
        self.assertEqual(checked.returncode, 1, checked.stdout + checked.stderr)
        self.assertIn("clang-diagnostic-ignored-attributes", checked.stderr)
        listed = subprocess.run(["bash", str(batteries.ROOT / "Scripts/check-tidy.sh"), "--list"],
                                capture_output=True, text=True, check=True)
        self.assertIn("clang-diagnostic-ignored-attributes", listed.stdout.splitlines())


class PythonProseTests(unittest.TestCase):
    def test_generated_shell_fixture_does_not_open_a_python_docstring(self):
        source = (
            '"""See Source/Core/CellGrid.h."""\n'
            'script = """set -eu\n'
            '# generated-body.sh\n'
            '""" + "exit 0"\n'
            'body = Path(directory) / "body.sh"\n'
            '# See Scripts/run-tests.sh.\n'
        )
        gate = import_module("check-comment-file-references")
        self.assertEqual(gate.references("Scripts/fixture.py", source),
                         [(1, "Source/Core/CellGrid.h"), (6, "Scripts/run-tests.sh")])
        prose = gate.slop.prose("Scripts/fixture.py", source)
        self.assertEqual(len(prose), len(source))
        self.assertEqual([index for index, char in enumerate(prose) if char == "\n"],
                         [index for index, char in enumerate(source) if char == "\n"])

    def test_real_python_docstrings_and_comments_are_kept_not_runtime_strings(self):
        source = (
            '"""See Source/Core/CellGrid.h."""\n'
            'class Example:\n'
            '    "See Source/Core/CellGrid.cpp."\n'
            '    async def method(self):\n'
            '        r"""See Scripts/run-tests.sh.\n'
            '        Source/Core/VT/VT.cpp is its parser.\n'
            '        """\n'
            '        value = "# phantom.py"  # See Scripts/build-tests.sh.\n'
            '        return f"{value} generated.py"\n'
        )
        gate = import_module("check-comment-file-references")
        self.assertEqual(gate.references("Scripts/fixture.py", source), [
            (1, "Source/Core/CellGrid.h"), (3, "Source/Core/CellGrid.cpp"),
            (5, "Scripts/run-tests.sh"), (6, "Source/Core/VT/VT.cpp"),
            (8, "Scripts/build-tests.sh"),
        ])

    def test_unicode_docstring_offsets_do_not_move_references(self):
        source = ('"""é Source/Core/CellGrid.h"""; value = "phantom.py"\n'
                  '# é Scripts/run-tests.sh\n')
        gate = import_module("check-comment-file-references")
        self.assertEqual(gate.references("Scripts/fixture.py", source),
                         [(1, "Source/Core/CellGrid.h"), (2, "Scripts/run-tests.sh")])
        prose = gate.slop.prose("Scripts/fixture.py", source)
        self.assertEqual(prose.index("Source/"), source.index("Source/"))
        self.assertEqual(len(prose), len(source))

    def test_malformed_python_is_not_silently_exempted(self):
        source = 'def broken(:\n    # missing-source.py\n'
        gate = import_module("check-comment-file-references")
        self.assertIn((2, "missing-source.py"), gate.references("Scripts/fixture.py", source))

    def test_a_path_through_any_library_mount_names_the_librarys_own_file(self):
        gate = import_module("check-comment-file-references")
        for mounted in ("Libraries/ubiquitty-core/Scripts/run-tests.sh",
                        "Libraries/ubiquitty-kit/Scripts/run-tests.sh",
                        "Libraries/ubiquitty-kit/Libraries/ubiquitty-core/Scripts/run-tests.sh"):
            self.assertEqual(gate.LIBRARY_MOUNT.sub("", mounted), "Scripts/run-tests.sh", mounted)
        for unmounted in ("Libraries/ubiquitty-core-other/Scripts/run-tests.sh", "Scripts/Libraries/ubiquitty-core/x.sh"):
            self.assertEqual(gate.LIBRARY_MOUNT.sub("", unmounted), unmounted)

    def test_unicode_separator_inside_data_is_not_a_source_line_break(self):
        source = ('value = "\u2028"\n'
                  '# Source/Core/CellGrid.h\n'
                  'class Example:\n'
                  '    """Scripts/run-tests.sh"""\n')
        gate = import_module("check-comment-file-references")
        self.assertEqual(gate.references("Scripts/fixture.py", source),
                         [(2, "Source/Core/CellGrid.h"), (4, "Scripts/run-tests.sh")])
        self.assertEqual(len(gate.slop.prose("Scripts/fixture.py", source)), len(source))


class ReachTests(unittest.TestCase):
    def test_path_filters_preserve_platform_and_golden_reach(self):
        cases = (
            (["Docs/architecture.md", "README.md", "Scripts/check-bundled-fonts.py"], (False, False, False, False, False)),
            (["README.rst"], (False, False, False, False, False)),
            (["Source/Core/CellGrid.cpp"], (True, True, True, False, True)),
            (["Source/Platform/macOS/UBTerminalView.mm"], (True, False, True, False, True)),
            (["Source/Platform/macOS/UBRuntimeBridge.h"], (True, True, True, False, True)),
            (["Source/UbiquittyKit/Swift/TerminalSurface.swift"], (True, False, True, False, True)),
            (["Source/Platform/Linux/main.cpp"], (True, True, False, False, True)),
            (["Tests/Web/page-smoke.mjs"], (True, True, False, False, True)),
            (["Tests/Snapshots/golden/native/coretext/frame.png"], (True, False, True, False, False)),
            (["Tests/Snapshots/golden/native/freetype/frame.png"], (True, True, False, False, False)),
            (["Tests/Snapshots/golden/portable/frame.png"], (True, True, True, False, False)),
            (["Scripts/build-tests.sh"], (True, True, True, False, True)),
            (["CMakeLists.txt"], (True, True, True, True, True)),
            ([".github/workflows/ci.yml"], (True, True, True, False, True)),
            ([], (True, True, True, True, True)),
        )
        for paths, expected in cases:
            with self.subTest(paths=paths):
                self.assertEqual(tuple(reach.changed_reach(paths).values()), expected)

    def test_checkpoint_requires_both_successful_full_code_batteries(self):
        self.assertTrue(reach.is_full_checkpoint(full_jobs()))
        for conclusion in ("failure", "cancelled", "skipped", None):
            for index in range(3):
                jobs = full_jobs()
                jobs[index]["conclusion"] = conclusion
                self.assertFalse(reach.is_full_checkpoint(jobs))
        for index in (1, 2):
            jobs = full_jobs()
            jobs[index]["steps"] = []
            self.assertFalse(reach.is_full_checkpoint(jobs))
        self.assertFalse(reach.is_full_checkpoint(full_jobs() + [full_jobs()[0]]))
        self.assertFalse(reach.is_full_checkpoint([{"name": "Host tests", "conclusion": "success"}]))

    def test_extra_required_jobs_cannot_be_hidden_by_successful_batteries(self):
        for name in ("Coverage floor", "Linux host build (glibc-arm64)"):
            for conclusion in ("failure", "cancelled", "timed_out", None):
                with self.subTest(name=name, conclusion=conclusion):
                    jobs = full_jobs() + [{"name": name, "conclusion": conclusion}]
                    self.assertFalse(reach.is_full_checkpoint(jobs))
                    self.assertEqual(reach.tip_verdict(jobs), reach.FAILED if conclusion == "failure" else "")
            for conclusion in ("success", "skipped"):
                jobs = full_jobs() + [{"name": name, "conclusion": conclusion}]
                self.assertTrue(reach.is_full_checkpoint(jobs))
                self.assertEqual(reach.tip_verdict(jobs), reach.PASSED)

    def test_a_battery_split_into_groups_is_complete_only_when_every_group_is(self):
        def split():
            jobs = [job for job in full_jobs() if job["name"] != "Linux battery"]
            for group in ("gcc", "clang", "wasm"):
                jobs.append({"name": f"Linux battery ({group})", "conclusion": "success", "steps": [
                    {"name": reach.FULL_MARKERS["Linux battery"], "conclusion": "success"}]})
            return jobs
        self.assertTrue(reach.is_full_checkpoint(split()))
        self.assertEqual(reach.tip_verdict(split()), reach.PASSED)
        for conclusion in ("failure", "cancelled", "skipped", None):
            jobs = split()
            jobs[-1]["conclusion"] = conclusion
            with self.subTest(conclusion=conclusion):
                self.assertFalse(reach.is_full_checkpoint(jobs))
        markerless = split()
        markerless[-2]["steps"] = []
        self.assertFalse(reach.is_full_checkpoint(markerless))
        failed = split()
        failed[-1]["conclusion"] = "failure"
        self.assertEqual(reach.tip_verdict(failed), reach.FAILED)
        # A group named twice is ambiguous, and a preflight cannot be a group.
        self.assertFalse(reach.is_full_checkpoint(split() + [split()[-1]]))
        renamed = split()
        renamed[0]["name"] = "CI preflight (extra)"
        self.assertFalse(reach.is_full_checkpoint(renamed))
        for name in ("Linux battery ()", "Linux battery (gcc", "Linux batteries (gcc)"):
            self.assertIsNone(reach.battery_of(name))

    def test_last_checkpoint_walks_past_partial_or_cancelled_runs(self):
        runs = [{"id": 1, "head_sha": "cancelled", "conclusion": "cancelled"},
                {"id": 2, "head_sha": "goldens", "conclusion": "success"},
                {"id": 3, "head_sha": "complete", "conclusion": "success"}]
        partial = full_jobs()
        partial[1]["steps"] = []
        reads = []
        def jobs(run):
            reads.append(run["id"])
            return partial if run["id"] == 2 else full_jobs()
        self.assertEqual(reach.select_history(runs, "current", jobs), ("complete", ""))
        self.assertEqual(reads, [2, 3])

    def test_sweep_distinguishes_known_failure_from_complete_coverage(self):
        failed = full_jobs()
        failed[1]["conclusion"] = "failure"
        failed[2]["conclusion"] = "cancelled"
        verdict = reach.tip_verdict(failed)
        self.assertIn("failed gate", verdict)
        self.assertIn("unexecuted", verdict)
        cancelled = full_jobs()
        cancelled[1]["conclusion"] = "cancelled"
        self.assertFalse(reach.tip_verdict(cancelled))
        run = {"id": 1, "head_sha": "current", "conclusion": "failure"}
        self.assertEqual(reach.select_history([run], "current", lambda run: failed, sweep=True), ("", verdict))

    def test_a_sweep_that_stands_down_skips_preflight_too(self):
        class Verdict:
            def __init__(self, environment):
                pass
            def runs(self, branch):
                return [{"id": 1, "head_sha": "current", "conclusion": "success"}]
            def jobs(self, run):
                return full_jobs()
        sweep = {"GITHUB_EVENT_NAME": "schedule", "GITHUB_REF_NAME": "main", "GITHUB_SHA": "current"}
        result, reason = reach.decide(sweep, Verdict, git=lambda *args: self.fail("no diff needed"))
        self.assertIn("stands down", reason)
        self.assertIs(result["preflight"], False)
        self.assertEqual(result["standing"], "success")
        self.assertFalse(any(result[key] for key in reach.OUTPUTS))
        pushed, _ = reach.decide({"GITHUB_EVENT_NAME": "workflow_dispatch"})
        self.assertNotIn("preflight", pushed)
        for decided, expected in (((result, reason), "preflight=false\nstanding=success\n"),
                                  ((pushed, "pushed"), "preflight=true\nstanding=\n")):
            with tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "output"
                with patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}, clear=True), \
                        patch.object(reach, "decide", return_value=decided), redirect_stdout(io.StringIO()):
                    reach.main()
                self.assertTrue(output.read_text().endswith(expected), output.read_text())

    def test_a_sweep_that_stands_down_for_a_failure_stays_a_failure(self):
        # The run that judged the tip: Linux failed. The sweep after it stands
        # down and fails in its own step, running neither battery; the sweep
        # after that reads the newest run, the stood-down one, first.
        judged = full_jobs()
        judged[1]["conclusion"] = "failure"
        stood_down = [{"name": "CI preflight", "conclusion": "failure", "steps": [
                          {"name": "Keep this tip's failed verdict", "conclusion": "failure"}]},
                      {"name": "Linux battery", "conclusion": "skipped", "steps": []},
                      {"name": "Apple battery", "conclusion": "skipped", "steps": []}]
        history = {1: judged, 2: stood_down}
        reads = []
        class History:
            def __init__(self, environment):
                pass
            def runs(self, branch):
                return [{"id": 2, "head_sha": "current", "conclusion": "failure"},
                        {"id": 1, "head_sha": "current", "conclusion": "failure"}]
            def jobs(self, run):
                reads.append(run["id"])
                return history[run["id"]]
        sweep = {"GITHUB_EVENT_NAME": "schedule", "GITHUB_REF_NAME": "main", "GITHUB_SHA": "current"}
        result, reason = reach.decide(sweep, History, git=lambda *args: self.fail("no diff needed"))
        self.assertEqual(result["standing"], "failure")
        self.assertIn(reach.FAILED, reason)
        self.assertEqual(reads, [2])
        self.assertEqual(reach.tip_verdict(stood_down), reach.FAILED)
        # A stood-down sweep of a passing tip concludes green, and reads back
        # as the same pass.
        passed = [dict(job, conclusion="success" if job["name"] == "CI preflight" else "skipped",
                       steps=[]) for job in stood_down]
        self.assertEqual(reach.tip_verdict(passed), reach.PASSED)

    def test_sweep_accepts_successful_out_of_reach_batteries(self):
        jobs = full_jobs()
        jobs[1]["conclusion"] = "skipped"
        self.assertTrue(reach.tip_verdict(jobs))
        self.assertFalse(reach.is_full_checkpoint(jobs))

    def test_no_checkpoint_never_uses_a_code_push_parent_as_a_checkpoint(self):
        class EmptyGitHub:
            def __init__(self, environment):
                pass
            def runs(self, branch):
                return []
            def jobs(self, run):
                raise AssertionError("no run exists")
        environment = {"GITHUB_EVENT_NAME": "push", "GITHUB_REF_NAME": "main", "GITHUB_SHA": "current", "BEFORE_SHA": "docs-parent"}
        calls = []
        def git(*args):
            calls.append(args)
            return "Source/Core/CellGrid.cpp\0" if args[0] == "diff" else ""
        result, reason = reach.decide(environment, EmptyGitHub, git=git)
        self.assertEqual(result, reach.all_reach())
        self.assertIn("no fully successful checkpoint", reason)
        self.assertFalse(any(call[0] == "diff" and "docs-parent" in call and "--no-renames" not in call
                             for call in calls))

    def test_documentation_push_skips_batteries_without_history_or_api(self):
        for paths in (["README.md"], ["Docs/guide.md", "Docs/images/example.svg"],
                      ["Docs/guide.rst", "Tests/README.md"]):
            with self.subTest(paths=paths):
                calls = []
                def git(*args):
                    calls.append(args)
                    return "\0".join(paths) + "\0" if args[0] == "diff" else ""
                result, reason = reach.decide(
                    {"GITHUB_EVENT_NAME": "push", "BEFORE_SHA": "whole-push-base"},
                    lambda *args: self.fail("documentation needs no history API"), git)
                self.assertEqual({key: result[key] for key in reach.OUTPUTS}, reach.all_reach(False))
                self.assertTrue(result["documentation"])
                self.assertNotIn("preflight", result)
                self.assertIn("documentation-only push", reason)
                self.assertEqual(calls[-1], ("diff", "--no-renames", "--name-only", "-z", "whole-push-base", "HEAD"))

    def test_documentation_shortcut_rejects_code_pins_gates_and_unknown_ranges(self):
        class NoHistory:
            def __init__(self, environment):
                pass
            def runs(self, branch):
                return []
            def jobs(self, run):
                raise AssertionError("no run exists")
        environment = {"GITHUB_EVENT_NAME": "push", "GITHUB_REF_NAME": "main",
                       "GITHUB_SHA": "current", "BEFORE_SHA": "whole-push-base"}
        for paths in ([], ["README.md", "Source/Core/CellGrid.cpp"],
                      ["README.md", "Libraries/ubiquitty-core"],
                      ["README.md", "Libraries/ubiquitty-kit"],
                      ["README.md", "Scripts/check-docs.py"], ["Docs/example.py"],
                      ["Docs/budget.json"], [".github/workflows/ci.yml"],
                      ["CMakeLists.txt"], ["Source/old.cpp", "README.md"]):
            with self.subTest(paths=paths):
                result, _ = reach.decide(environment, NoHistory,
                                        git=lambda *args: "\0".join(paths) + ("\0" if paths else ""))
                self.assertEqual(result, reach.all_reach())
        for before in ("", "0" * 40):
            result, _ = reach.decide(dict(environment, BEFORE_SHA=before), NoHistory,
                                    git=lambda *args: self.fail("no usable push range"))
            self.assertEqual(result, reach.all_reach())
        result, _ = reach.decide(environment, NoHistory,
                                git=lambda *args: (_ for _ in ()).throw(subprocess.CalledProcessError(1, "git")))
        self.assertEqual(result, reach.all_reach())

    def test_documentation_push_uses_the_entire_real_git_range_and_both_rename_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def git(*args):
                return reach.git_output(*args, cwd=root)
            git("init", "-q")
            git("config", "user.name", "Fixture")
            git("config", "user.email", "fixture@example.invalid")
            (root / "README.md").write_text("guide\n")
            (root / "source.cpp").write_text("int value = 1;\n")
            git("add", "."); git("commit", "-qm", "base")
            before = git("rev-parse", "HEAD").strip()
            (root / "README.md").write_text("updated guide\n")
            git("add", "."); git("commit", "-qm", "docs")
            result = reach.documentation_push({"GITHUB_EVENT_NAME": "push", "BEFORE_SHA": before}, git)
            self.assertTrue(result)
            (root / "source.cpp").rename(root / "renamed.md")
            git("add", "-A"); git("commit", "-qm", "rename")
            self.assertIsNone(reach.documentation_push(
                {"GITHUB_EVENT_NAME": "push", "BEFORE_SHA": before}, git))
            (root / "README.md").write_text("another docs-only last commit\n")
            git("add", "."); git("commit", "-qm", "more docs")
            self.assertIsNone(reach.documentation_push(
                {"GITHUB_EVENT_NAME": "push", "BEFORE_SHA": before}, git))

    def test_documentation_success_does_not_claim_platform_coverage_or_stop_a_sweep(self):
        jobs = full_jobs()
        jobs[0]["steps"] = [{"name": reach.DOCUMENTATION_MARKER, "conclusion": "success"}]
        for job in jobs[1:]:
            job.update(conclusion="skipped", steps=[])
        self.assertFalse(reach.is_full_checkpoint(jobs))
        self.assertEqual(reach.tip_verdict(jobs), "")
        run = {"id": 1, "head_sha": "current", "conclusion": "success"}
        self.assertEqual(reach.select_history([run], "current", lambda run: jobs, sweep=True), ("", ""))
        jobs[0]["conclusion"] = "failure"
        self.assertEqual(reach.tip_verdict(jobs), reach.FAILED)

    def test_api_failure_is_conservative_and_visible(self):
        def unavailable(environment):
            raise OSError("offline")
        result, reason = reach.decide({"GITHUB_EVENT_NAME": "push"}, unavailable)
        self.assertEqual(result, reach.all_reach())
        self.assertIn("history unavailable", reason)

    def test_dispatch_runs_everything_without_api_or_git(self):
        result, reason = reach.decide({"GITHUB_EVENT_NAME": "workflow_dispatch"},
                                     lambda *args: self.fail("no API needed"), lambda *args: self.fail("no diff needed"))
        self.assertEqual(result, reach.all_reach())

    def test_pull_request_uses_its_supplied_base(self):
        calls = []
        def git(*args):
            calls.append(args)
            return "README.md\n" if args[0] == "diff" else ""
        result, reason = reach.decide({"GITHUB_EVENT_NAME": "pull_request", "BASE_SHA": "base"}, git=git)
        self.assertEqual(result, reach.all_reach(False))
        self.assertEqual(calls[-1], ("diff", "--name-only", "base", "HEAD"))


class WorkflowScriptPathTests(unittest.TestCase):
    """Every script a workflow names exists in the tree.

    A workflow step that calls a renamed script fails only when that workflow
    runs, and the release workflow runs once per tag, so the rename goes
    unnoticed until the one run that matters. The check reads every workflow
    file and needs no flag.
    """

    SCRIPT_PATH = re.compile(r"(?<![\w/.-])(?:\./)?((?:Libraries/ubiquitty-kit/Libraries/ubiquitty-core/|"
                             r"Libraries/(?:ubiquitty-core|ubiquitty-kit)/)?(?:Scripts|Tests|Tools)/[\w./-]+\.(?:sh|py|mjs))\b")

    def test_owned_arm_and_coverage_jobs_are_required_and_cached(self):
        jobs = workflow_jobs((TEST_SOURCES.subject / ".github/workflows/ci.yml").read_text())
        if TEST_SOURCES.repositories == ("app",):
            self.assertNotIn("coverage", jobs)
            self.assertNotIn("host-linux-arm64", jobs)
            return
        for name in ("host-linux-arm64", "coverage"):
            job = jobs[name]
            self.assertIn("if: needs.preflight.outputs.linux == 'true'", job)
            self.assertNotIn("if: ${{ false }}", job)
            self.assertNotIn("continue-on-error: true\n    steps:", job)
            self.assertIn("ccache-v2-${{ runner.os }}-${{ runner.arch }}-", job)
            self.assertIn("ci-linux-image-${{ runner.arch }}-", job)
            self.assertIn(name, jobs["ci"].split("needs:", 1)[1].splitlines()[0])
        self.assertIn("runs-on: ubuntu-24.04-arm", jobs["host-linux-arm64"])
        self.assertIn("--check-file-floors", jobs["coverage"])
        for name in ("linux", "apple", "coverage", "host-linux-arm64"):
            self.assertIn("Compiler cache hit rate", jobs[name])

    def test_every_script_a_workflow_names_exists(self):
        root = TEST_SOURCES.subject
        workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
        self.assertTrue(workflows, "no workflow files found")
        missing = []
        for workflow in workflows:
            for number, line in enumerate(workflow.read_text().splitlines(), 1):
                for match in self.SCRIPT_PATH.finditer(line):
                    if not (root / match.group(1)).exists():
                        missing.append(f"{workflow.relative_to(root)}:{number}: {match.group(1)}")
        self.assertEqual(missing, [])

    def test_every_repository_file_a_packaging_script_names_exists(self):
        """The release packaging scripts run once per tag, like the workflow."""
        root = TEST_SOURCES.subject
        scripts = sorted((root / "Scripts").glob("package-*.sh")) + sorted((root / "Scripts").glob("release-*.sh"))
        if "app" in TEST_SOURCES.repositories:
            self.assertTrue(scripts, "no packaging scripts found")
        literal = re.compile(r'"\$ROOT/([\w./-]+)"')
        missing = []
        for script in scripts:
            for number, line in enumerate(script.read_text().splitlines(), 1):
                for match in literal.finditer(line):
                    if not (root / match.group(1)).exists():
                        missing.append(f"{script.relative_to(root)}:{number}: {match.group(1)}")
        self.assertEqual(missing, [])

    def test_a_missing_script_is_reported(self):
        line = "          ./Scripts/check-docs.sh && python3 Scripts/ci_batteries.py run linux"
        named = [match.group(1) for match in self.SCRIPT_PATH.finditer(line)]
        self.assertEqual(named, ["Scripts/check-docs.sh", "Scripts/ci_batteries.py"])


class ReleaseFloorTests(unittest.TestCase):
    """The README's release matrix names the macOS floor the app bundle declares.

    The app floor lives in Scripts/macos-deployment-target.txt and reaches the
    bundle's LSMinimumSystemVersion; the README states it in prose. A user reads
    the prose and installs the bundle, so the two are compared here.
    """

    source_repositories = ("app",)

    def test_readme_states_the_app_bundle_floor(self):
        root = TEST_SOURCES.subject
        floors = dict(re.findall(r"(?m)^(cxx|app)=(\S+)$",
                                 test_file("Scripts/macos-deployment-target.txt").read_text()))
        self.assertIn("app", floors)

        def normalised(version):
            parts = version.split(".")
            while len(parts) > 1 and parts[-1] == "0":
                parts.pop()
            return ".".join(parts)

        stated = re.findall(r"Apple-silicon\s+macOS\s+(\d+(?:\.\d+)*)\+",
                            (root / "README.md").read_text())
        self.assertTrue(stated, "the README states no Apple-silicon macOS floor")
        for version in stated:
            self.assertEqual(normalised(version), normalised(floors["app"]))


WORKFLOWS = TEST_SOURCES.subject / ".github" / "workflows"
# The GitHub-hosted images the workflows name. A self-hosted label can take the
# same shape (`ubuntu-builder`), so a pattern would count it as hosted.
HOSTED_RUNNERS = frozenset({"ubuntu-24.04", "ubuntu-24.04-arm", "xcode-27"})
REPOSITORY = "owner/ubiquitty"
FORK_ORIGINS = ("fork", "deleted fork", "fork through pull_request_target")


def draft_workflows():
    """Loads the workflow reader and expression evaluator the draft-policy tests use."""
    spec = spec_from_file_location("draft_workflows", batteries.ROOT / "Tests/Tools/test_draft_workflows.py")
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def runner_field(text):
    """Returns a runs-on value without the comment lines the job reader keeps beneath it."""
    return "\n".join(line for line in text.splitlines() if not line.strip().startswith("#")).strip()


def runner_contexts():
    """Yields each origin a run can have."""
    fork = {"repo": {"full_name": "contributor/ubiquitty"}}
    heads = (("fork", "pull_request", fork),
             ("deleted fork", "pull_request", {"repo": None}),
             ("fork through pull_request_target", "pull_request_target", fork),
             ("same repository", "pull_request", {"repo": {"full_name": REPOSITORY}}))
    invocation = {"vars": {}, "inputs": {}}
    for origin, event, head in heads:
        github = {"event_name": event, "repository": REPOSITORY,
                  "event": {"pull_request": {"head": head}}}
        yield origin, {**invocation, "github": github}
    for event in ("push", "schedule", "workflow_dispatch"):
        github = {"event_name": event, "repository": REPOSITORY, "event": {}}
        yield event, {**invocation, "github": github}


def github_hosted(runner):
    """Returns whether a resolved runs-on names a single GitHub-hosted image."""
    return isinstance(runner, str) and runner in HOSTED_RUNNERS


def fork_runners(evaluate, runs_on):
    """Returns every fork pull request context whose runs-on resolves to a runner GitHub does not host."""
    if not (runs_on.startswith("${{") and runs_on.endswith("}}")):
        return [] if github_hosted(runs_on) else [("any origin", runs_on)]
    reached = []
    for origin, context in runner_contexts():
        if origin in FORK_ORIGINS:
            runner = evaluate(runs_on, context)
            if not github_hosted(runner):
                reached.append((origin, runner))
    return reached


class WorkflowTests(unittest.TestCase):
    source_repositories = ("app",)

    def test_apple_runner_provides_the_required_xcode_generation(self):
        drafts = draft_workflows()
        workflow = drafts.Workflow((WORKFLOWS / "ci.yml").read_text())
        self.assertEqual(runner_field(workflow.jobs["apple"]["runs-on"]), "xcode-27")

    def test_linux_cleanup_is_hosted_only_and_preserves_build_dependencies(self):
        drafts = draft_workflows()
        workflow = drafts.Workflow((WORKFLOWS / "ci.yml").read_text())
        steps = workflow.jobs["linux"]["steps"]
        cleanup = re.search(r"(?ms)^      - name: Free unused hosted SDK space\n(.*?)(?=^      - |\Z)", steps)
        self.assertIsNotNone(cleanup)
        fields = drafts.mapping(cleanup.group(1), 8)
        condition = drafts.scalar(fields["if"])
        for environment in ("github-hosted", "self-hosted", ""):
            for operating_system in ("Linux", "macOS", "Windows", ""):
                with self.subTest(environment=environment, operating_system=operating_system):
                    context = {"runner": {"environment": environment, "os": operating_system}}
                    self.assertEqual(bool(drafts.expression(condition, context)),
                                     environment == "github-hosted" and operating_system == "Linux")
        # Inspect rather than execute the destructive command. Only the three
        # unused SDK installations may be removed, never the workspace,
        # Docker storage, compiler cache, or shared Actions tool cache.
        self.assertEqual(drafts.scalar(fields["run"]), "|")
        commands = [shlex.split(line, comments=True) for line in fields["run"].splitlines()[1:] if line.strip()]
        self.assertEqual(commands, [
            ["df", "-h", "/"],
            ["sudo", "rm", "-rf", "--", "/usr/local/lib/android", "/usr/share/dotnet", "/usr/local/.ghcup"],
            ["df", "-h", "/"],
        ])
        for cache in ("Cache QUIC dependencies", "Restore the compiler cache", "Restore the CI image"):
            self.assertLess(cleanup.start(), steps.index(f"- name: {cache}"))
        for name, job in workflow.jobs.items():
            if name != "linux":
                self.assertNotIn("Free unused hosted SDK space", job.get("steps", ""))

    def test_fork_pull_requests_never_reach_a_self_hosted_runner(self):
        # Repository registration is the security boundary; the checked-in
        # workflows must also keep every pull request on hosted infrastructure.
        drafts = draft_workflows()
        inspected = 0
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            workflow = drafts.Workflow(path.read_text())
            forkable = any(event.startswith("pull_request") for event in workflow.events)
            for job, fields in workflow.jobs.items():
                runs_on = runner_field(fields.get("runs-on", ""))
                self.assertNotIn("self-hosted", runs_on, (path.name, job))
                if not runs_on or not forkable:
                    continue
                inspected += 1
                with self.subTest(workflow=path.name, job=job):
                    self.assertEqual(fork_runners(drafts.expression, runs_on), [])
        self.assertGreater(inspected, 0)
        # Hosted only for pushes, so every pull request origin reaches the self-hosted label.
        unguarded = ("${{ github.event_name == 'push' && 'ubuntu-24.04' || "
                     "fromJSON('[\"self-hosted\",\"Linux\",\"X64\",\"build-host\"]') }}")
        self.assertEqual({origin for origin, *_ in fork_runners(drafts.expression, unguarded)}, set(FORK_ORIGINS))
        for literal in ("build-host", "[self-hosted, build-host]", "- self-hosted\n      - build-host"):
            self.assertTrue(fork_runners(drafts.expression, literal), literal)

    @unittest.skipUnless(WORKFLOW, "pass --workflow to validate the live workflow contract")
    def test_workflow_runs_every_app_battery_without_library_jobs(self):
        text = Path(WORKFLOW).read_text()
        jobs = workflow_jobs(text)
        self.assertEqual(set(jobs), {"preflight", "linux", "apple", "ci"})
        prefix = "Libraries/ubiquitty-kit/Libraries/ubiquitty-core/" if TEST_SOURCES.repositories == ("app",) else ""
        source = ' --source-root "$GITHUB_WORKSPACE"' if prefix else ""
        for platform in ("preflight", "linux", "apple"):
            job = jobs[platform]
            self.assertEqual(job.count("uses: actions/checkout@"), 1)
            battery = f"python3 {prefix}Scripts/ci_batteries.py run {platform}{source}"
            if platform != "apple":
                battery = f"./{prefix}Scripts/ci-linux-image.sh{source} {battery}"
            # A battery split into matrix groups runs, and reports, the stages
            # its group names.
            stages = "" if platform == "preflight" else " $STAGES"
            self.assertIn(f"{battery}{stages}\n", job)
            # Preflight's battery stands down with a sweep that has this tip's
            # verdict, and so does its report, which would find no stages.
            reported = "always() && steps.reach.outputs.preflight != 'false'" if platform == "preflight" else "always()"
            report = f"python3 {prefix}Scripts/ci_batteries.py report {platform}{source}{stages}"
            self.assertRegex(job, rf"        if: {re.escape(reported)}\n        run: (\|\n(?:          #.*\n)*          )?{re.escape(report)}\n")
            if platform != "apple":
                # The image comes from the Actions cache under its recipe's
                # digest, and is saved back whether or not a gate failed.
                self.assertIn(f'run: echo "digest=$(./{prefix}Scripts/ci-linux-image.sh{source} --recipe-digest)" >> "$GITHUB_OUTPUT"', job)
                self.assertIn("UBQ_CI_IMAGE_ARCHIVE: ${{ runner.temp }}/ubq-ci-linux-image.tar", job)
                self.assertEqual(job.count("key: ci-linux-image-${{ runner.arch }}-${{ steps.image.outputs.digest }}"), 2)
                self.assertIn("name: Save the CI image\n        if: always() &&", job)
            self.assertIn("name: Print the failed stage logs\n        if: failure()", job)
            self.assertIn(f"$RUNNER_TEMP/ubiquitty-ci/{platform}", job)
        self.assertNotIn("actions/upload-artifact@", text)
        self.assertIn(f"run: python3 {prefix}Scripts/ci_reach.py{source}", jobs["preflight"])
        if prefix:
            for job in jobs.values():
                self.assertEqual(job.count("submodules: recursive"), job.count("uses: actions/checkout@"))
            self.assertEqual(text.count(f"hashFiles('{prefix}Scripts/fetch-quic-deps.sh')"), 1)
        # A sweep that stands down for a failed tip fails too, so the failure
        # is what the next sweep reads back from it.
        self.assertRegex(jobs["preflight"], r"name: Keep this tip's failed verdict\n"
                         r"        if: steps\.reach\.outputs\.standing == 'failure'\n"
                         r"        run: \|\n(?:          .*\n)*?          exit 1\n")
        self.assertIn("name: CI preflight", jobs["preflight"])
        for name in ("contents", "actions", "checks"):
            self.assertIn(f"      {name}: read", jobs["preflight"])
        # The workflow's own grant is read-only, so a job that declares none
        # cannot inherit a broader repository default.
        self.assertRegex(text, r"(?m)^permissions:\n  contents: read\n(?!  )")
        # The batteries run on hosted runners behind the preflight, each where
        # change reach says it is needed, and every group carries the marker
        # the history reader requires of a full checkpoint.
        for platform in ("linux", "apple"):
            job = jobs[platform]
            self.assertIn("needs: preflight", job)
            self.assertIn(f"if: needs.preflight.outputs.{platform} == 'true'", job)
            self.assertNotIn("if: ${{ false }}", job)
            self.assertIn(f"name: {platform.title()} battery (${{{{ matrix.group }}}})", job)
            self.assertIn('UBQ_CI_PREFLIGHT_COMPLETE: "1"', job)
            marker = reach.FULL_MARKERS[f"{platform.title()} battery"]
            self.assertIn(f"name: {marker}\n        if: steps.battery.outputs.full_code == 'true'", job)
            self.assertNotIn("continue-on-error: true\n        run:", job)
        # The UI scenarios run on a hosted runner only when a run by hand asks.
        self.assertIn("UBQ_CI_UI_SCENARIOS: ${{ github.event_name == 'workflow_dispatch' && inputs.ui_scenarios && 'true' || 'false' }}",
                      jobs["apple"])


def workflow_jobs(text):
    """Each job's name and the text of its body."""
    body = text.split("\njobs:\n", 1)[1]
    starts = list(re.finditer(r"^  ([a-z][a-z0-9-]*):\n", body, re.M))
    return {match.group(1): body[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(body)]
            for index, match in enumerate(starts)}


class EveryWorkflowTests(unittest.TestCase):
    """What every repository's ci.yml holds to, whichever repository it is."""

    def jobs(self):
        return workflow_jobs(Path(WORKFLOW).read_text())

    @unittest.skipUnless(WORKFLOW, "pass --workflow to validate the live workflow contract")
    def test_documentation_shortcut_receives_the_push_range_and_marks_partial_coverage(self):
        preflight = self.jobs()["preflight"]
        self.assertIn("BEFORE_SHA: ${{ github.event.before }}", preflight)
        self.assertIn(f"name: {reach.DOCUMENTATION_MARKER}\n"
                      "        if: steps.reach.outputs.documentation == 'true'\n", preflight)

    @unittest.skipUnless(WORKFLOW, "pass --workflow to validate the live workflow contract")
    def test_the_one_required_check_needs_every_other_job(self):
        jobs = self.jobs()
        self.assertIn("ci", jobs)
        aggregate = jobs["ci"]
        self.assertIn("    name: CI\n", aggregate)
        # always(): a job that failed or was cancelled leaves its dependents
        # skipped, and a skipped required check reads as a pass.
        self.assertIn("    if: ${{ always() &&", aggregate)
        needs = re.search(r"(?m)^    needs: \[(.*)\]$", aggregate)
        self.assertIsNotNone(needs)
        self.assertEqual(sorted(name.strip() for name in needs.group(1).split(",")),
                         sorted(name for name in jobs if name != "ci"))
        self.assertIn('if job == "preflight" and conclusion != "success":', aggregate)
        self.assertIn('elif conclusion not in ("success", "skipped"):', aggregate)

    @unittest.skipUnless(WORKFLOW, "pass --workflow to validate the live workflow contract")
    def test_push_gate_steps_run_strict_and_select_every_gate(self):
        text = Path(WORKFLOW).read_text()
        steps = re.findall(r"(?ms)^      - name: Push gates.*?(?=^      - |\Z)", text)
        self.assertTrue(steps, "no step runs the push gates")
        for step in steps:
            with self.subTest(step=step.splitlines()[0]):
                for setting in ("UBIQUITTY_PREPUSH: families", "UBQ_PREPUSH_SELECT: all",
                                'UBQ_PREPUSH_STRICT: "1"', 'UBIQUITTY_PREPUSH_CACHE: "off"'):
                    self.assertIn(f"          {setting}\n", step)
                self.assertRegex(step, r"          UBQ_PREPUSH_FAMILIES: \S")
                self.assertIn("Scripts/pre-push-gates.sh", step)

    @unittest.skipUnless(WORKFLOW, "pass --workflow to validate the live workflow contract")
    def test_a_hosted_mac_exempts_the_cases_it_cannot_draw_and_names_why(self):
        stage = next(stage for stage in batteries.stages("apple") if stage.key == "snapshots")
        with tempfile.TemporaryDirectory() as directory:
            base = {"RUNNER_TEMP": directory, "UBQ_RESULT_DIR": directory}
            self.assertNotIn("UBQ_SNAPSHOT_RUNNER_EXEMPTIONS", batteries.stage_environment(stage, "apple", base))
            hosted = batteries.stage_environment(stage, "apple", {**base, "RUNNER_ENVIRONMENT": "github-hosted"})
            lines = Path(hosted["UBQ_SNAPSHOT_RUNNER_EXEMPTIONS"]).read_text(encoding="utf-8").splitlines()
        self.assertEqual({line.split(None, 1)[0] for line in lines}, set(batteries.HOSTED_SNAPSHOT_EXEMPTIONS))
        for line in lines:
            # The grammar of Tests/Snapshots/exempt-<backend>.txt: an identifier,
            # then a reason that ends with the date it was recorded.
            self.assertRegex(line, r"^case:\S+:coretext:(macos|swift)  \S.* \(\d{4}-\d{2}-\d{2}\)$")
        self.assertTrue(all("sf_pro" in identifier for identifier in batteries.HOSTED_SNAPSHOT_EXEMPTIONS))

    def test_a_battery_split_into_groups_runs_every_stage_of_its_manifest(self):
        for name, job in self.jobs().items():
            for platform in ("linux", "apple"):
                if f"name: {platform.title()} battery (" not in job:
                    continue
                with self.subTest(job=name):
                    groups = re.findall(r"(?m)^            stages: (.*)$", job)
                    self.assertTrue(groups, "a battery named by group lists no group's stages")
                    covered = set()
                    for group in groups:
                        words = group.split()
                        self.assertEqual(words[::2], ["--only"] * (len(words) // 2))
                        self.assertIn("setup", words[1::2])
                        covered.update(words[1::2])
                    expected = {stage.key for stage in batteries.source_stages(platform, TEST_SOURCES) if stage.hosted}
                    self.assertEqual(covered, expected)

    @unittest.skipUnless(WORKFLOW, "pass --workflow to validate the live workflow contract")
    def test_every_stage_list_selects_the_stages_whose_build_it_reads(self):
        text = Path(WORKFLOW).read_text()
        lists = []
        for name, job in self.jobs().items():
            for platform in ("linux", "apple"):
                if f"name: {platform.title()} battery (" in job:
                    lists += [(name, platform, group.split()[1::2], TEST_SOURCES.repositories)
                              for group in re.findall(r"(?m)^            stages: (.*)$", job)]
                elif f"name: {platform.title()} battery\n" in job:
                    lists.append((name, platform,
                                  [stage.key for stage in batteries.source_stages(platform, TEST_SOURCES)],
                                  TEST_SOURCES.repositories))
        # A step names the checkout it judges; a cross-repository job's is the Kit's or the app's.
        subjects = {"$KIT_CHECKOUT": ("kit",), "$APP_CHECKOUT": ("app",)}
        for platform, rest in re.findall(r"ci_batteries\.py\"? (?:run|report) (linux|apple)\b([^\n']*)", text):
            keys = re.findall(r"--only (\S+)", rest)
            source = re.search(r'--source-root "([^"]+)"', rest)
            if keys:
                lists.append(("step", platform, keys,
                              subjects.get(source.group(1), TEST_SOURCES.repositories) if source else TEST_SOURCES.repositories))
        self.assertTrue(lists, "no battery stage list found")
        for job, platform, keys, repositories in lists:
            with self.subTest(job=job, platform=platform, keys=" ".join(keys)):
                batteries.select_stages(batteries.stages(platform), keys, repositories)

def run_suite(run_tests):
    """Runs the tests under an artifact root no other run has; returns their status.

    A test that hands a battery a root of its own keeps its artifacts there.
    One whose environment starts from os.environ inherits UBQ_CI_ROOT, which
    inside the preflight battery is that battery's own live root, and without
    it and RUNNER_TEMP falls back to the /tmp/ubiquitty-ci every hand run on
    the machine shares. So the suite claims a fresh root for the whole run:
    removed when the tests passed or left nothing in it, kept and named when
    they failed.
    """
    root = Path(tempfile.mkdtemp(prefix="ubiquitty-ci-"))
    os.environ["UBQ_CI_ROOT"] = str(root)
    status = 1
    try:
        status = run_tests()
    finally:
        if not status or not any(root.iterdir()):
            shutil.rmtree(root, ignore_errors=True)
        else:
            print(f"test-ci-batteries: the failed run's artifacts are kept in {root}", file=sys.stderr)
    return status


class SourceCompositionBatteryTests(unittest.TestCase):
    def setUp(self):
        # The fake SELinux daemon rejects system trees. macOS's default
        # temporary directory is under /private/var, so use project data in
        # /tmp to model the writable Linux workspace these fixtures exercise.
        scratch = tempfile.TemporaryDirectory(prefix="ubq-battery-composition-", dir="/tmp")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        self.app = self.root / "app with spaces"
        self.kit = self.app / "Libraries/ubiquitty-kit"
        self.core = self.kit / "Libraries/ubiquitty-core"
        for path, role in ((self.app, "app"), (self.core, "core"), (self.kit, "kit")):
            (path / "Scripts").mkdir(parents=True)
            (path / "CMakeLists.txt").write_text("# source composition fixture\n")
            (path / "Scripts/repository.txt").write_text(role + "\n")
        self.log = self.root / "invocations.jsonl"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.bin / "xvfb-run").write_text('#!/bin/sh\nshift\nexec "$@"\n')
        (self.bin / "xvfb-run").chmod(0o755)
        self.tool(self.app, "Scripts/build-linux-gtk.sh", "app-build")
        self.tool(self.core, "Scripts/ensure-quic-deps.sh", "core-quic")
        self.tool(self.core, "Scripts/run-tests.sh", "core-tests")
        self.tool(self.kit, "Scripts/run-tests.sh", "kit-tests")
        self.tool(self.core, "Tests/Snapshots/run.py", "core-snapshot")
        self.tool(self.kit, "Scripts/check-linux-build-prerequisites.sh", "kit-prerequisite")

    def tool(self, owner, relative, label):
        path = owner / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#!/usr/bin/env python3\nimport json, os, sys\n'
                        'from pathlib import Path\n'
                        f'row = dict(label={label!r}, arguments=sys.argv[1:], cwd=os.getcwd(), '
                        'subject=os.environ.get("UBQ_TESTS_ROOT"), core=os.environ.get("UBQ_CORE_ROOT"), '
                        'kit=os.environ.get("UBQ_KIT_ROOT"), build_root=os.environ.get("UBQ_BUILD_ROOT"))\n'
                        'with open(os.environ["UBQ_ROUTING_LOG"], "a") as stream: '
                        'stream.write(json.dumps(row)+"\\n")\n')
        path.chmod(0o755)

    def environment(self, context):
        return batteries.platform_environment("linux", {
            "PATH": str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
            "RUNNER_WORKSPACE": str(self.root / "workspace"),
            "RUNNER_TEMP": str(self.root / "temporary"),
            "RUNNER_TOOL_CACHE": str(self.root / "tools"),
            "UBQ_CI_ROOT": str(self.root / "results"), "UBQ_ROUTING_LOG": str(self.log),
        }, context)

    def test_gitless_app_routes_real_stage_processes_to_each_owner(self):
        context = source_roots(self.app, environment={})
        environment = self.environment(context)
        manifest = batteries.source_stages("linux", context, ("quic", "host-tests", "linux-build", "linux-snapshots"))
        path = self.root / "battery.json"
        self.assertEqual(batteries.run_battery("linux", manifest, environment, path), 0)
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([row["label"] for row in rows], ["core-quic", "core-tests", "app-build", "core-snapshot"])
        gcc_root = Path(environment["RUNNER_WORKSPACE"]) / "build-gcc"
        self.assertEqual(rows[1]["build_root"], str(gcc_root / "subject"))
        for row in rows:
            self.assertEqual(row["cwd"], str(self.app))
            self.assertEqual((row["subject"], row["core"], row["kit"]), tuple(map(str, (self.app, self.core, self.kit))))
        self.assertIn("--source-root", rows[-1]["arguments"])
        self.assertIn(str(self.app), rows[-1]["arguments"])
        self.assertIn(str(self.kit), rows[-1]["arguments"])
        self.assertEqual({row["state"] for row in json.loads(path.read_text())}, {"completed"})
        self.assertEqual(batteries.validate_events("linux", manifest, Path(environment["UBQ_EVENT_LOG"])), [])

    def test_substantive_kit_driver_remains_the_kit_subject_entry(self):
        context = source_roots(self.kit, core=self.core, environment={})
        stage = batteries.source_stages("apple", context, ("host-tests",))[0]
        self.assertEqual(batteries.execute_stage(stage, self.environment(context)), 0)
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([row["label"] for row in rows], ["kit-tests"])
        self.assertEqual(rows[0]["arguments"], [])
        self.assertEqual(rows[0]["subject"], str(self.kit))
        self.assertEqual(rows[0]["kit"], str(self.kit))

    def test_core_sanitizers_cannot_be_executed_as_a_consumer_suite(self):
        self.tool(self.core, "Scripts/run-sanitized-tests.sh", "core-sanitizers")
        for subject, kit in ((self.kit, self.kit), (self.app, self.kit)):
            with self.subTest(subject=subject.name):
                context = source_roots(subject, core=self.core, kit=kit, environment={})
                stage = batteries.source_stages("linux", context, ("sanitizers",))[0]
                self.assertEqual(batteries.execute_stage(stage, self.environment(context)), 2)
                self.assertFalse(self.log.exists())
        context = source_roots(self.core, environment={})
        stage = batteries.source_stages("linux", context, ("sanitizers",))[0]
        self.assertEqual(batteries.execute_stage(stage, self.environment(context)), 0)
        self.assertEqual(json.loads(self.log.read_text())["label"], "core-sanitizers")

    def test_consumer_owned_driver_is_not_replaced_with_the_shared_implementation(self):
        self.tool(self.app, "Scripts/run-tests.sh", "app-tests")
        context = source_roots(self.app, environment={})
        stage = batteries.source_stages("linux", context, ("host-tests",))[0]
        self.assertEqual(batteries.execute_stage(stage, self.environment(context)), 0)
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([row["label"] for row in rows], ["app-tests"])
        self.assertEqual(rows[0]["subject"], str(self.app))

    def test_consumer_driver_cannot_escape_its_owner_before_either_pass_starts(self):
        outside = self.root / "outside-driver.sh"
        outside.write_text("exit 0\n")
        (self.app / "Scripts/run-tests.sh").symlink_to(outside)
        context = source_roots(self.app, environment={})
        stage = batteries.source_stages("linux", context, ("host-tests",))[0]
        self.assertEqual(batteries.execute_stage(stage, self.environment(context)), 2)
        self.assertFalse(self.log.exists())

    def test_required_kit_tool_is_executed_without_a_copied_app_shim(self):
        context = source_roots(self.app, environment={})
        stage = batteries.route_stage(batteries.Stage("kit-prerequisite", "Kit prerequisite", "./Scripts/check-linux-build-prerequisites.sh"), context)
        self.assertEqual(batteries.execute_stage(stage, self.environment(context)), 0)
        self.assertEqual(json.loads(self.log.read_text())["label"], "kit-prerequisite")

    def test_app_snapshot_stage_checks_its_native_frontend_and_optional_web(self):
        context = source_roots(self.app, environment={})
        stage = batteries.source_stages("apple", context, ("snapshots",))[0]
        environment = self.environment(context)
        self.assertEqual(batteries.execute_stage(stage, environment), 0)
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([row["arguments"][row["arguments"].index("--backend") + 1] for row in rows], ["macos"])
        module = Path(environment["UBQ_BUILD_ROOT"]) / "web-dist/ubiquitty.wasm"
        module.parent.mkdir(parents=True)
        module.write_text("built Web module fixture")
        self.log.unlink()
        self.assertEqual(batteries.execute_stage(stage, environment), 0)
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual([row["arguments"][row["arguments"].index("--backend") + 1] for row in rows], ["macos", "web"])

    def test_explicit_reach_root_uses_app_git_and_library_pins_require_full_reach(self):
        output = self.root / "reach-output"
        def decide(environment, git):
            git("diff", "--name-only", "base", "HEAD")
            return reach.all_reach(), "fixture"
        with patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}), \
                patch.object(reach, "decide", side_effect=decide), \
                patch.object(reach.subprocess, "check_output", return_value="README.md\n") as invoked:
            self.assertEqual(reach.main(["--source-root", str(self.app)]), 0)
        self.assertEqual(invoked.call_args.kwargs["cwd"], self.app)
        for mount in ("Libraries/ubiquitty-core", "Libraries/ubiquitty-kit"):
            self.assertEqual(reach.changed_reach([mount]), reach.all_reach())

    def install_shared_shell_helper(self, name):
        path = self.core / "Scripts" / name
        shutil.copyfile(batteries.ROOT / "Scripts" / name, path)
        shutil.copyfile(batteries.ROOT / "Scripts/ubq_roots.py", self.core / "Scripts/ubq_roots.py")
        return path

    def ci_image_fixture(self):
        script = self.install_shared_shell_helper("ci-linux-image.sh")
        # A container installs the pinned clang-format, read from the Core's pin file.
        shutil.copyfile(batteries.ROOT / "Scripts/clang-format-pin", self.core / "Scripts/clang-format-pin")
        recipe = self.core / "Scripts/run-linux.sh"
        recipe.write_text('#!/bin/sh\n[ "$UBIQUITTY_LINUX_ROOT" = "$UBQ_TESTS_ROOT" ] || exit 93\nprintf "FROM fixture\\n"\n')
        recipe.chmod(0o755)
        docker = self.bin / "docker"
        docker.write_text('#!/usr/bin/env python3\nimport json, os, sys\n'
                          'with open(os.environ["UBQ_ROUTING_LOG"], "a") as log: log.write(json.dumps(sys.argv[1:])+"\\n")\n'
                          'if sys.argv[1:2] == ["info"]: print(os.environ.get("UBQ_DOCKER_SECURITY_OPTIONS", "[]"))\n'
                          'elif "--format" in sys.argv: print("sha256:fixture")\n')
        docker.chmod(0o755)
        base = {key: value for key, value in os.environ.items() if not key.startswith(("UBQ_", "UBIQUITTY_", "CCACHE_"))}
        environment = dict(base, PATH=str(self.bin) + os.pathsep + os.environ.get("PATH", ""),
                           UBQ_ROUTING_LOG=str(self.log), UBQ_CI_LINUX_IMAGE="true",
                           RUNNER_WORKSPACE=str(self.root / "workspace"), RUNNER_TEMP=str(self.root / "temporary"),
                           RUNNER_TOOL_CACHE=str(self.root / "tools"))
        for name in ("UBQ_TESTS_ROOT", "UBQ_CORE_ROOT", "UBQ_KIT_ROOT", "UBIQUITTY_CORE_SOURCE_DIR", "UBIQUITTY_KIT_SOURCE_DIR"):
            environment.pop(name, None)
        return script, environment

    def ci_image_run(self, script, environment):
        self.log.unlink(missing_ok=True)
        result = subprocess.run(["bash", str(script), "--source-root", str(self.app), "true"],
                                cwd=self.core, env=environment, capture_output=True, text=True)
        calls = [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []
        return result, calls

    def test_ci_image_mounts_gitless_app_and_explicit_libraries_at_their_real_paths(self):
        script, environment = self.ci_image_fixture()
        for workspace in (str(self.root / "workspace"), None, str(self.app)):
            with self.subTest(workspace=workspace):
                configured = dict(environment)
                if workspace is None:
                    configured.pop("RUNNER_WORKSPACE", None)
                else:
                    configured["RUNNER_WORKSPACE"] = workspace
                result, calls = self.ci_image_run(script, configured)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                run = next(row for row in calls if row[0] == "run")
                for path in (self.app, self.core, self.kit):
                    self.assertIn(f"{path}:{path}", run)
                targets = [run[index + 1].split(":")[1] for index, argument in enumerate(run) if argument == "-v"]
                self.assertEqual(len(targets), len(set(targets)), run)
                self.assertIn(f"{self.app}:/src:ro", run)
                self.assertEqual(run[run.index("-w") + 1], str(self.app))
                self.assertFalse(any("/.git" in argument for argument in run))
                self.assertFalse(any(argument.endswith((":z", ",z", ":Z", ",Z")) for argument in run))

    def test_ci_image_selinux_labels_only_scoped_shared_data_and_keeps_read_only_alias(self):
        script, environment = self.ci_image_fixture()
        subprocess.run(["git", "init", "-q", str(self.core)], check=True)
        configured = dict(environment, UBQ_DOCKER_SECURITY_OPTIONS='["name=seccomp,profile=builtin", "name=selinux"]',
                          UBQ_BUILD_ROOT=str(self.root / "build"), CCACHE_DIR=str(self.root / "cache"),
                          UBQ_PGO_TRAIN_OUT=str(self.root / "profiles/core.profdata"),
                          UBQ_PGO_RETRAIN_WORKTREE=str(self.root / "retraining/worktree"))
        for workspace in (str(self.root / "workspace"), str(self.app)):
            with self.subTest(workspace=workspace):
                result, calls = self.ci_image_run(script, dict(configured, RUNNER_WORKSPACE=workspace))
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                run = next(row for row in calls if row[0] == "run")
                for path in (self.app, self.core, self.kit, self.core / ".git", self.root / "temporary",
                             self.root / "build", self.root / "cache", self.root / "profiles", self.root / "retraining"):
                    self.assertIn(f"{path}:{path}:z", run)
                self.assertIn(f"{self.app}:/src:ro,z", run)
                self.assertIn(f"{self.root / 'tools'}:{self.root / 'tools'}", run)
                if workspace != str(self.app):
                    self.assertIn(f"{workspace}:{workspace}", run)
                self.assertIn("/var/run/docker.sock:/var/run/docker.sock", run)
                self.assertIn(f"{self.bin / 'docker'}:/usr/local/bin/docker:ro", run)
                targets = [run[index + 1].split(":")[1] for index, argument in enumerate(run) if argument == "-v"]
                self.assertEqual(len(targets), len(set(targets)), run)
                self.assertFalse(any(argument.endswith((":Z", ",Z")) for argument in run))
                self.assertNotIn("--privileged", run)
                self.assertNotIn("--security-opt", run)

    def test_ci_image_selinux_mounts_scoped_default_cache_without_relabeling_workspace(self):
        script, environment = self.ci_image_fixture()
        result, calls = self.ci_image_run(script, dict(environment, UBQ_DOCKER_SECURITY_OPTIONS='["name=selinux"]'))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        run = next(row for row in calls if row[0] == "run")
        workspace = self.root / "workspace"
        self.assertIn(f"{workspace}:{workspace}", run)
        self.assertIn(f"{workspace / 'ccache'}:{workspace / 'ccache'}:z", run)

    def test_ci_image_selinux_accepts_a_scoped_variable_temporary_build_directory(self):
        script, environment = self.ci_image_fixture()
        with tempfile.TemporaryDirectory(prefix="ubq-image-output-", dir="/var/tmp") as temporary:
            build = Path(temporary).resolve() / "build"
            result, calls = self.ci_image_run(script, dict(environment, UBQ_DOCKER_SECURITY_OPTIONS='["name=selinux"]',
                                                        UBQ_BUILD_ROOT=str(build)))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            run = next(row for row in calls if row[0] == "run")
            self.assertIn(f"{build}:{build}:z", run)
            self.assertTrue(build.is_dir())

    def test_ci_image_selinux_refuses_broad_system_and_symlink_targets_before_container_mutations(self):
        script, environment = self.ci_image_fixture()
        alias = self.root / "system-alias"
        alias.symlink_to("/usr", target_is_directory=True)
        cases = (("RUNNER_TEMP", "/tmp"), ("UBQ_BUILD_ROOT", "/"), ("CCACHE_DIR", str(Path.home())),
                 ("UBQ_PGO_TRAIN_OUT", "/tmp/core.profdata"), ("UBQ_PGO_RETRAIN_WORKTREE", "/tmp/worktree"),
                 ("UBQ_BUILD_ROOT", str(self.root)), ("UBQ_BUILD_ROOT", str(alias)),
                 ("CCACHE_DIR", str(self.bin)), ("UBQ_BUILD_ROOT", "/var/log"),
                 ("UBQ_BUILD_ROOT", "/var/lib/docker"), ("UBQ_BUILD_ROOT", "/var/lib/containers"),
                 ("UBQ_BUILD_ROOT", "/var/cache"), ("UBQ_PGO_TRAIN_OUT", "/var/tmp/core.profdata"))
        for name, value in cases:
            with self.subTest(name=name, value=value):
                result, calls = self.ci_image_run(script, dict(environment, UBQ_DOCKER_SECURITY_OPTIONS='["name=selinux"]',
                                                            **{name: value}))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("refusing to relabel", result.stderr)
                self.assertEqual([row[0] for row in calls], ["info"])

    def test_ci_image_refuses_unknown_daemon_security_metadata_and_mount_collisions(self):
        script, environment = self.ci_image_fixture()
        for value in ("not-json", "null", '["name=selinux", 1]'):
            with self.subTest(security_options=value):
                result, calls = self.ci_image_run(script, dict(environment, UBQ_DOCKER_SECURITY_OPTIONS=value))
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual([row[0] for row in calls], ["info"])
        result, calls = self.ci_image_run(script, dict(environment, RUNNER_WORKSPACE="/src"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("conflicting sources or modes", result.stderr)
        self.assertEqual([row[0] for row in calls], ["info"])
        for name in ("RUNNER_WORKSPACE", "RUNNER_TOOL_CACHE"):
            with self.subTest(broad_runner_mount=name):
                result, calls = self.ci_image_run(script, dict(environment, UBQ_DOCKER_SECURITY_OPTIONS='["name=selinux"]',
                                                            **{name: "/"}))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("refusing a broad runner mount", result.stderr)
                self.assertEqual([row[0] for row in calls], ["info"])
        for name in ("RUNNER_WORKSPACE", "RUNNER_TOOL_CACHE"):
            for suffix in ("colon:path", "new\nline"):
                with self.subTest(invalid_runner_mount=name, suffix=suffix):
                    path = self.root / suffix
                    result, calls = self.ci_image_run(script, dict(environment, UBQ_DOCKER_SECURITY_OPTIONS='["name=selinux"]',
                                                                **{name: str(path)}))
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("invalid bind mount path", result.stderr)
                    self.assertEqual([row[0] for row in calls], ["info"])
                    self.assertFalse(path.exists())

    def test_source_collection_keeps_all_app_dependency_checks_without_inventing_upper_sources(self):
        class CoreFixture(unittest.TestCase):
            source_repositories = ("core",)
        class AppFixture(unittest.TestCase):
            source_repositories = ("app",)
        core, app = CoreFixture(), AppFixture()
        suite = unittest.TestSuite((core, app))
        self.assertEqual(source_tests(suite, ("core",)), [core])
        self.assertEqual(source_tests(suite, ("app",)), [core, app])

    def test_sanitizer_configures_app_tests_but_core_host_and_host_filters(self):
        script = self.install_shared_shell_helper("run-sanitized-tests.sh")
        (self.core / "Scripts/build-root.sh").write_text(': "${UBQ_BUILD_ROOT:?}"\n')
        (self.core / "Scripts/build-jobs.sh").write_text('ubq_build_jobs() { echo 1; }\n')
        self.tool(self.core, "Scripts/run-ctest.sh", "core-ctest")
        self.tool(self.bin, "cmake", "cmake")
        for name in ("clang", "clang++"):
            (self.bin / name).write_text('#!/bin/sh\necho "clang fixture"\n')
            (self.bin / name).chmod(0o755)
        context = source_roots(self.app, environment={})
        environment = dict(self.environment(context), CC="clang", CXX="clang++")
        checked = subprocess.run(["bash", str(script), "--source-root", str(self.app)],
                                 env=environment, cwd=self.core, capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        configured = [row for row in rows if row["label"] == "cmake" and "-S" in row["arguments"]]
        self.assertEqual([row["arguments"][row["arguments"].index("-S") + 1] for row in configured], [str(self.app), str(self.core)])
        native = configured[0]["arguments"]
        self.assertIn("-DUBIQUITTY_SANITIZERS=address,undefined", native)
        self.assertIn(f"-DUBIQUITTY_CORE_SOURCE_DIR={self.core}", native)
        host = [row for row in rows if row["label"] == "core-tests"]
        self.assertEqual([row["arguments"] for row in host], [["host_smoke", "host_cli"], ["host_soak"]])
        self.assertTrue(all(row["subject"] == str(self.core) and row["kit"] == "" for row in host))

    def test_coverage_configures_the_actual_app_or_standalone_core_subject(self):
        script = self.install_shared_shell_helper("run-coverage.sh")
        (self.core / "Scripts/build-root.sh").write_text(': "${UBQ_BUILD_ROOT:?}"\n')
        (self.core / "Scripts/build-jobs.sh").write_text('ubq_build_jobs() { echo 1; }\n')
        self.tool(self.bin, "cmake", "coverage-configure")
        with open(self.bin / "cmake", "a") as output:
            output.write("raise SystemExit(27)\n")
        for subject in (self.app, self.core, self.kit):
            with self.subTest(subject=subject):
                self.log.unlink(missing_ok=True)
                context = source_roots(subject, environment={})
                checked = subprocess.run(["bash", str(script), "--source-root", str(subject)],
                                         env=self.environment(context), cwd=self.core,
                                         capture_output=True, text=True)
                self.assertEqual(checked.returncode, 27, checked.stdout + checked.stderr)
                row = json.loads(self.log.read_text())
                self.assertEqual(row["subject"], str(subject))
                self.assertEqual(row["arguments"][row["arguments"].index("-S") + 1], str(subject))
                self.assertIn("-DUBIQUITTY_COVERAGE=ON", row["arguments"])
                self.assertIn("-DUBIQUITTY_TEST_PCH=OFF", row["arguments"])
                frontend = "Linux" if subject == self.kit else "Core"
                self.assertIn(f"-DUBIQUITTY_FRONTEND={frontend}", row["arguments"])
                self.assertIn(f"-DUBIQUITTY_CORE_SOURCE_DIR={self.core}", row["arguments"])

    def test_coverage_floor_refuses_stale_app_or_core_inputs_without_changing_floors(self):
        script = self.install_shared_shell_helper("check-coverage-floor.sh")
        (self.core / "Scripts/build-root.sh").write_text(': "${UBQ_BUILD_ROOT:?}"\n')
        for subject in (self.app, self.core):
            with self.subTest(subject=subject):
                context = source_roots(subject, environment={})
                environment = self.environment(context)
                summary = Path(environment["UBQ_BUILD_ROOT"]) / "BuildHostTests/coverage/summary.txt"
                summary.parent.mkdir(parents=True, exist_ok=True)
                summary.write_text("stale summary must not reach any percentage comparison\n")
                source = subject / "Source/fresh.cpp"
                source.parent.mkdir(exist_ok=True)
                source.write_text("// newer product source\n")
                os.utime(summary, (1, 1))
                os.utime(source, (2, 2))
                checked = subprocess.run(["bash", str(script), "--source-root", str(subject)],
                                         env=environment, capture_output=True, text=True)
                self.assertEqual(checked.returncode, 2, checked.stdout + checked.stderr)
                self.assertIn("older than the sources it scores", checked.stderr)
                self.assertIn("fresh.cpp", checked.stderr)

    def test_missing_required_tool_fails_and_records_later_capabilities(self):
        (self.app / "Scripts/build-linux-gtk.sh").unlink()
        context = source_roots(self.app, environment={})
        environment = self.environment(context)
        manifest = batteries.source_stages("linux", context, ("quic", "linux-build", "linux-snapshots"))
        path = self.root / "battery.json"
        self.assertEqual(batteries.run_battery("linux", manifest, environment, path), 1)
        records = json.loads(path.read_text())
        self.assertEqual([row["state"] for row in records], ["completed", "failed", "unexecuted"])
        self.assertIn("required source file", records[1]["command"])
        self.assertIn("blocked by failed stage", records[2]["detail"])
        self.assertEqual(batteries.validate_events("linux", manifest, Path(environment["UBQ_EVENT_LOG"])), [])

    def test_gitless_caller_discovery_promotes_nested_core_to_app(self):
        context = source_roots(start=self.core / "Scripts", environment={})
        self.assertEqual((context.subject, context.core, context.kit), (self.app, self.core, self.kit))
        (self.kit / "Scripts/repository.txt").write_text("core\n")
        with self.assertRaisesRegex(ValueError, "kit source root names core"):
            source_roots(self.app, environment={})

    def test_arbitrary_library_worktree_or_foreign_consumer_is_not_the_subject(self):
        renamed = self.app / "Libraries/my-core-worktree"
        self.core.rename(renamed)
        context = source_roots(start=renamed / "Scripts", environment={})
        self.assertEqual(context.subject, renamed)
        self.assertIsNone(context.kit)
        renamed.rename(self.core)
        # Core promotes to the Kit that carries it, and the Kit to no consumer
        # but an app.
        (self.app / "Scripts/repository.txt").write_text("core\n")
        context = source_roots(start=self.core / "Scripts", environment={})
        self.assertEqual((context.subject, context.core, context.kit), (self.kit, self.core, self.kit))

    def test_uninitialized_app_dependency_refuses_instead_of_falling_back(self):
        (self.core / "CMakeLists.txt").unlink()
        with self.assertRaisesRegex(ValueError, "core source root is not populated"):
            source_roots(self.app, environment={})

    def test_required_tool_symlink_cannot_escape_its_source_owner(self):
        path = self.app / "Scripts/build-linux-gtk.sh"
        path.unlink()
        outside = self.root / "outside.sh"
        outside.write_text("exit 0\n")
        path.symlink_to(outside)
        context = source_roots(self.app, environment={})
        stage = batteries.source_stages("linux", context, ("linux-build",))[0]
        self.assertIn("escapes its owner", stage.command)
        environment = context.environment({"GIT_DIR": "/foreign/.git", "GIT_INDEX_FILE": "/foreign/index"})
        self.assertNotIn("GIT_DIR", environment)
        self.assertNotIn("GIT_INDEX_FILE", environment)

    def test_standalone_core_has_no_upper_dependency_or_app_capabilities(self):
        context = source_roots(self.core, environment={})
        self.assertIsNone(context.kit)
        manifest = batteries.source_stages("linux", context)
        self.assertNotIn("linux-build", {stage.key for stage in manifest})
        self.assertIn("host-tests", {stage.key for stage in manifest})
        selected = batteries.source_stages("linux", context, ("linux-build",))
        self.assertIn("requires app sources", selected[0].command)
        with self.assertRaisesRegex(ValueError, "kit source root names core, not kit"):
            source_roots(self.core, kit=self.core, environment={})
        for variable in ("UBQ_KIT_ROOT", "UBIQUITTY_KIT_SOURCE_DIR"):
            with self.subTest(variable=variable), self.assertRaisesRegex(ValueError, "kit source root names core, not kit"):
                source_roots(self.core, environment={variable: str(self.core)})

    def test_shell_context_removes_both_previous_kit_roots_when_core_has_no_kit(self):
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("UBQ_TESTS_ROOT", "UBQ_CORE_ROOT", "UBQ_KIT_ROOT",
                                      "UBIQUITTY_CORE_SOURCE_DIR", "UBIQUITTY_KIT_SOURCE_DIR")}
        context = subprocess.run([
            sys.executable, str(batteries.ROOT / "Scripts/ubq_roots.py"), "shell-context",
            "--source-root", str(self.core)], env=environment, capture_output=True, text=True)
        self.assertEqual(context.returncode, 0, context.stdout + context.stderr)
        environment.update(UBQ_KIT_ROOT=str(self.kit), UBIQUITTY_KIT_SOURCE_DIR=str(self.core),
                           UBQ_TEST_SOURCE_CONTEXT=context.stdout)
        checked = subprocess.run([
            "bash", "-c", 'eval "$UBQ_TEST_SOURCE_CONTEXT"\n"$1" -c '
            + shlex.quote('import os; assert "UBQ_KIT_ROOT" not in os.environ; '
                          'assert "UBIQUITTY_KIT_SOURCE_DIR" not in os.environ'),
            "core-context", sys.executable], env=environment, capture_output=True, text=True)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)

    def test_missing_selected_conditional_capability_cannot_become_a_successful_skip(self):
        context = source_roots(self.core, environment={})
        environment = dict(self.environment(context), UBQ_CI_CODE="false")
        manifest = batteries.source_stages("apple", context, ("ios-build", "ios-smokes"))
        path = self.root / "missing-apple.json"
        self.assertEqual(batteries.run_battery("apple", manifest, environment, path), 1)
        self.assertEqual([row["state"] for row in json.loads(path.read_text())], ["failed", "unexecuted"])

    def test_combined_tree_retains_its_single_owner_and_full_manifest(self):
        (self.app / "Scripts/repository.txt").unlink()
        context = source_roots(self.app, environment={})
        self.assertEqual((context.core, context.kit), (self.app, self.app))
        self.assertEqual({stage.key for stage in batteries.source_stages("linux", context)},
                         {stage.key for stage in batteries.stages("linux")})


class SuiteRootTests(unittest.TestCase):
    def test_the_suite_runs_under_a_root_of_its_own_and_keeps_it_only_when_it_failed(self):
        roots = []
        for status, leaves, kept in ((0, True, False), (1, True, True), (1, False, False)):
            with self.subTest(status=status, leaves=leaves), \
                    patch.dict(os.environ, {"UBQ_CI_ROOT": "/tmp/ubiquitty-ci"}), \
                    redirect_stderr(io.StringIO()) as errors:
                def run_tests():
                    root = Path(os.environ["UBQ_CI_ROOT"])
                    roots.append(root)
                    if leaves:
                        (root / "preflight").mkdir()
                        (root / "preflight/events.jsonl").write_text("{}\n", encoding="utf-8")
                    return status
                self.assertEqual(run_suite(run_tests), status)
                root = roots[-1]
                self.addCleanup(shutil.rmtree, root, True)
                # An inherited root is replaced, not shared.
                self.assertTrue(root.name.startswith("ubiquitty-ci-"), root)
                self.assertEqual(root.exists(), kept)
                self.assertEqual(str(root) in errors.getvalue(), kept)
        self.assertEqual(len(set(roots)), len(roots))


def source_tests(suite, repositories):
    """Collects tests for files the subject owns; App preflight retains all owners."""
    available = set(repositories)
    if "app" in available:
        available.update(("core", "kit"))
    elif "kit" in available:
        available.add("core")
    selected = []
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            selected.extend(source_tests(test, repositories))
        elif set(getattr(type(test), "source_repositories", ("core", "kit", "app"))).intersection(available):
            selected.append(test)
    return selected


def load_tests(loader, tests, pattern):
    return unittest.TestSuite(source_tests(tests, TEST_SOURCES.repositories))


if __name__ == "__main__":
    def run_tests():
        try:
            unittest.main()
        except SystemExit as exit_request:
            return exit_request.code
        return 0
    sys.exit(run_suite(run_tests))
