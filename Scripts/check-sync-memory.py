#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Builds and runs the reviewed, in-memory Sync regression binaries only.

    python3 Scripts/check-sync-memory.py
    python3 Scripts/check-sync-memory.py --self-test
"""

import argparse
import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import build_root
import gate_report


ROOT = Path(__file__).resolve().parents[1]
GATE = "check-sync-memory"
# Each entry and its transitive fixtures require an in-memory-only review.
# Do not derive this set from a glob, label, prefix, or coverage index.
TEST_SOURCES = (
    "Core/Protocol/Sync/test_sync_admission.cpp",
    "Core/Protocol/Sync/test_sync_envelope.cpp",
    "Core/Protocol/Sync/test_sync_resume.cpp",
    "Core/Protocol/Sync/test_sync_checkpoint_storage_admission.cpp",
    "Core/Protocol/Sync/test_sync_rich_text.cpp",
    "Core/Protocol/Sync/test_sync_wire_fingerprint.cpp",
    "Core/Session/test_adopt_transaction.cpp",
    "Core/Session/test_reclaim_transaction.cpp",
    "Platform/test_owned_child.cpp",
    "Core/Protocol/Sync/test_sync_checkpoint_budget.cpp",
    "Core/Protocol/Sync/test_sync_checkpoint_restore_budget.cpp",
    "Base/test_fallible_storage.cpp",
    "Core/Protocol/Sync/test_sync_checkpoint_full_budget.cpp",
    "Core/Protocol/Sync/test_sync_checkpoint_admission.cpp",
    "Core/Protocol/Sync/test_sync_handover_fallible_owners.cpp",
    "Core/Session/test_remotesessioncore.cpp",
    "Core/Session/test_background_refresh_state.cpp",
    "Core/Protocol/Sync/test_sync_images.cpp",
    "Core/Protocol/Sync/test_sync_lanes.cpp",
    "Core/Protocol/Sync/test_sync_input.cpp",
    "Core/Protocol/Sync/test_sync_history_cache.cpp",
    "Core/Protocol/Sync/test_sync_memory_owner.cpp",
    "Core/Protocol/Sync/test_sync_checkpoint_ceiling.cpp",
)
CASE_SUMMARY = re.compile(r"^test-harness: ok \(([1-9][0-9]*) cases?; [^\n]*\)$", re.MULTILINE)
TEST_TIMEOUT_SECONDS = 60


def positive_integer(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def cache_source(build):
    """Reads the configured source owner without executing or deleting a cache."""
    cache = build / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            return Path(line.split("=", 1)[1]).resolve()
    raise ValueError("The existing CMake cache does not identify its source checkout")


def run_gate(build, jobs):
    """Builds the exact allowlist before running any test; no CTest fixtures run."""
    targets = tuple(Path(source).stem for source in TEST_SOURCES)
    if not targets or len(set(targets)) != len(targets):
        return gate_report.refused(GATE, "The reviewed allowlist must be nonempty and contain unique targets")
    build = build.expanduser().resolve()
    if build == ROOT or ROOT in build.parents:
        return gate_report.refused(GATE, "The build directory must be outside the checkout")
    try:
        owner = cache_source(build)
    except (OSError, ValueError) as error:
        return gate_report.refused(GATE, str(error))
    if owner is not None and owner != ROOT:
        return gate_report.refused(GATE, "The build directory belongs to another checkout; choose an unused directory")
    missing = [source for source in TEST_SOURCES if not (ROOT / "Tests" / source).is_file()]
    if missing:
        return gate_report.failed(GATE, "allowlisted sources missing", missing)

    environment = dict(os.environ, CCACHE_HASHDIR="true")
    configure = [
        "cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
        "-DUBIQUITTY_FRONTEND=Core", "-DUBIQUITTY_BUILD_TESTS=ON",
        "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DUBIQUITTY_QUIC=OFF", "-DUBIQUITTY_BUILD_SSH_PACKAGE=OFF",
        "-DUBIQUITTY_TEST_PCH=OFF", "-DUBIQUITTY_CTEST_SHELL_REMAINDER=OFF",
        "-DUBIQUITTY_COVERAGE=OFF",
    ]
    print(f"{GATE}: Building {len(targets)} allowlisted binaries in {build}", flush=True)
    try:
        for phase, command in (
            ("configure", configure),
            ("build", ["cmake", "--build", str(build), "--parallel", str(jobs), "--target", *targets]),
        ):
            finished = subprocess.run(command, cwd=ROOT, env=environment, check=False)
            if finished.returncode:
                return gate_report.failed(GATE, f"{phase} failed; no test binaries were run")

        binaries = tuple(build / "Tests" / target for target in targets)
        for binary in binaries:
            if binary.resolve() != binary or not binary.is_file() or not os.access(binary, os.X_OK):
                return gate_report.failed(GATE, "allowlisted executable missing or redirected", [binary.name])

        cases = 0
        # The Harness pins defaults after its first Preferences lookup. Give
        # that lookup an empty task-owned configuration directory as well.
        with tempfile.TemporaryDirectory(prefix="ubq-sync-memory-config-") as config_directory:
            test_environment = dict(environment, XDG_CONFIG_HOME=str(Path(config_directory).resolve()))
            for binary in binaries:
                print(f"{GATE}: Running {binary.name}", flush=True)
                try:
                    finished = subprocess.run([str(binary)], cwd=build, env=test_environment,
                                              capture_output=True, text=True, timeout=TEST_TIMEOUT_SECONDS, check=False)
                except subprocess.TimeoutExpired:
                    return gate_report.failed(GATE, f"test exceeded {TEST_TIMEOUT_SECONDS}s", [binary.name])
                print(finished.stdout, end="", flush=True)
                print(finished.stderr, end="", file=sys.stderr, flush=True)
                if finished.returncode:
                    return gate_report.failed(GATE, "test binaries failed", [binary.name])
                summaries = CASE_SUMMARY.findall(finished.stdout)
                if len(summaries) != 1:
                    return gate_report.failed(GATE, "test binary did not report one nonempty passing suite", [binary.name])
                cases += int(summaries[0])
    except OSError as error:
        return gate_report.refused(GATE, str(error))
    return gate_report.ok(GATE, f"{len(targets)} in-memory binaries; {cases} cases")


def self_test():
    """Plants the refusals this gate owes before it builds anything, and checks each lands.

    Nothing here configures or compiles: every planted defect is one the gate
    must catch while reading its own arguments and the tree, which is where a
    gate that silently builds the wrong thing goes wrong.
    """
    global TEST_SOURCES
    problems = []
    quiet = io.StringIO()
    with tempfile.TemporaryDirectory() as scratch:
        outside = Path(scratch) / "sync-memory"
        with contextlib.redirect_stdout(quiet), contextlib.redirect_stderr(quiet):
            if run_gate(ROOT / "build" / "sync-memory-self-test", 1) != 2:
                problems.append("a build directory inside the checkout was not refused")
            foreign = Path(scratch) / "foreign"
            foreign.mkdir()
            (foreign / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/elsewhere/checkout\n", encoding="utf-8")
            if run_gate(foreign, 1) != 2:
                problems.append("a build directory configured for another checkout was not refused")
            reviewed = TEST_SOURCES
            try:
                TEST_SOURCES = reviewed + ("Core/Protocol/Sync/test_sync_planted_absent.cpp",)
                if run_gate(outside, 1) != 1:
                    problems.append("an allowlisted source missing from the tree did not fail the gate")
                TEST_SOURCES = reviewed + (reviewed[0],)
                if run_gate(outside, 1) != 2:
                    problems.append("a duplicated allowlist entry was not refused")
            finally:
                TEST_SOURCES = reviewed
        if outside.exists() and any(outside.iterdir()):
            problems.append("a refused run left files in its build directory")
    for problem in problems:
        print("self-test: %s" % problem, file=sys.stderr)
    print("%s: self-test %s" % (GATE, "FAILED" if problems else "ok"))
    return 1 if problems else 0


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="plant the refusals this gate owes and check each lands, without building")
    parser.add_argument("--list", action="store_true", help="list the reviewed test sources without building or running")
    parser.add_argument("--build-dir", type=Path, help="dedicated external build directory (default: derived build root/BuildHostTests/sync-memory)")
    parser.add_argument("--jobs", type=positive_integer, default=min(os.cpu_count() or 2, 4), help="parallel compile jobs (default: up to 4)")
    options = parser.parse_args(arguments)
    if options.self_test:
        return self_test()
    if options.list:
        print("\n".join("Tests/" + source for source in TEST_SOURCES))
        return 0
    build = options.build_dir or Path(build_root.build_root(str(ROOT))) / "BuildHostTests" / "sync-memory"
    return run_gate(build, options.jobs)


if __name__ == "__main__":
    sys.exit(main())
