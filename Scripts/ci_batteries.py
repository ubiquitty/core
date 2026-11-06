#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Runs CI's ordered, fail-fast batteries and reports every stage.

The stage manifest is the executable capability map:
  preflight: reach-independent style, capability, terminfo, and hygiene gates;
  linux: GCC suite/QUIC, portable snapshots, glibc/static-musl host builds,
         clang ASan/UBSan and fuzz, GTK/Vulkan, WASM, and web snapshots;
  apple: Swift format, macOS app/tidy/rendering/UI/snapshots, Apple test suite,
         conditional Xcode generation, and iOS build/smoke/UI tests.

The explicit snapshot stages own these tiers, including goldens-only changes.
SNAPSHOT=skip makes each full suite's snapshot entry report skipped through
CTest rather than repeat those tiers. GCC, clang, and native platform build
roots are distinct; platform-specific build roots retain their paths.
All stages in a battery share one checkout and toolchain setup.
Benchmark and coverage measurements are suspended outside this manifest.

A run's artifacts (stage logs, the machine record, events.jsonl) live under
one root per run, `UBQ_CI_ROOT/<battery>`. `run` passes the root to every
stage and `report` reads the same one, so the battery, the event writer and
the artifact reader agree whatever started them. Unset, the root is
`$RUNNER_TEMP/ubiquitty-ci`, which a CI job has to itself, and
`/tmp/ubiquitty-ci` for a battery run by hand on a machine without
RUNNER_TEMP; two batteries run by hand at once share that one, so a driver
that runs more than one at a time sets `UBQ_CI_ROOT` (the pre-push gate does).

`run --only <stage>` runs just the named stages, in manifest order, with the
same environment, commands and logs the whole battery gives them. A machine that
stands in for the CI runner for part of a push (Scripts/linux-host.sh) runs the
stages it covers this way, so the commands it runs are CI's own.

Usage: python3 Scripts/ci_batteries.py run|report|list preflight|linux|apple
       python3 Scripts/ci_batteries.py run linux --only linux-build --only linux-smokes
"""

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gate_framework import Reporter


ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Stage:
    key: str
    label: str
    command: str
    condition: str = "always"
    toolchain: str = "native"


STYLE_SETUP = r'''
pin="$(tr -d '[:space:]' < Scripts/clang-format-pin)"
venv="$RUNNER_TOOL_CACHE/clang-format-$pin"
if ! ./Scripts/ci-toolchain.sh check "style-tools-$UBQ_CI_SETUP_KEY" clang-format python3; then
    python3 -m venv "$venv"
    "$venv/bin/pip" install --quiet --disable-pip-version-check "clang-format==$pin.*"
    clang-format --version
    ./Scripts/ci-toolchain.sh record "style-tools-$UBQ_CI_SETUP_KEY" clang-format python3
fi
if ! ./Scripts/ci-toolchain.sh check "shellcheck-$UBQ_CI_SETUP_KEY" shellcheck; then
    if ! command -v shellcheck >/dev/null 2>&1; then
        if command -v apt-get >/dev/null 2>&1; then
            sudo apt-get install -y shellcheck
        elif command -v dnf >/dev/null 2>&1; then
            sudo dnf install -y ShellCheck
        elif command -v pacman >/dev/null 2>&1; then
            sudo pacman -Sy --noconfirm shellcheck
        else
            echo "No supported package manager found for shellcheck" >&2
            exit 1
        fi
    fi
    shellcheck --version
    ./Scripts/ci-toolchain.sh record "shellcheck-$UBQ_CI_SETUP_KEY" shellcheck
fi
./Scripts/install-hooks.sh
'''

LINUX_SETUP = r'''
tools=(cmake ninja g++ make pkg-config)
if [ "$UBQ_CI_CODE" = true ]; then tools+=(clang llvm-profdata); fi
if ! ./Scripts/ci-toolchain.sh check "battery-linux-$UBQ_CI_SETUP_KEY" "${tools[@]}"; then
    if command -v apt-get >/dev/null 2>&1; then
        packages=(build-essential cmake ninja-build pkg-config)
        if [ "$UBQ_CI_CODE" = true ]; then packages+=(clang llvm); fi
        sudo apt-get update
        sudo apt-get install -y "${packages[@]}"
    elif command -v dnf >/dev/null 2>&1; then
        packages=(gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config)
        if [ "$UBQ_CI_CODE" = true ]; then packages+=(clang llvm); fi
        sudo dnf install -y "${packages[@]}"
    elif command -v pacman >/dev/null 2>&1; then
        packages=(base-devel cmake ninja pkgconf)
        if [ "$UBQ_CI_CODE" = true ]; then packages+=(clang llvm); fi
        sudo pacman -Sy --noconfirm "${packages[@]}"
    else
        echo "No supported package manager found for the Linux toolchain" >&2
        exit 1
    fi
    ./Scripts/ci-toolchain.sh record "battery-linux-$UBQ_CI_SETUP_KEY" "${tools[@]}"
fi
./Scripts/install-hooks.sh
'''

LINUX_FRONTEND_TOOLS = r'''
missing=0
for module in gtk4 vulkan freetype2 fontconfig harfbuzz; do
    pkg-config --exists "$module" || { echo "missing pkg-config module: $module" >&2; missing=1; }
done
for tool in cmake ninja c++ python3 glslc; do
    command -v "$tool" >/dev/null 2>&1 || { echo "missing tool: $tool" >&2; missing=1; }
done
[ "$missing" -eq 0 ] || { echo "Install the missing frontend tools on the runner" >&2; exit 1; }
glslc --version
ls -1 /usr/share/vulkan/icd.d/
'''

WASM_SETUP = r'''
if [ "$EMSDK_ENV" != /data/emsdk/emsdk_env.sh ]; then
    if [ ! -d "$RUNNER_TOOL_CACHE/emsdk-6.0.9/.git" ]; then
        git clone https://github.com/emscripten-core/emsdk.git "$RUNNER_TOOL_CACHE/emsdk-6.0.9"
    fi
    if [ ! -f "$RUNNER_TOOL_CACHE/emsdk-6.0.9/.ubq-6.0.9-ready" ]; then
        (cd "$RUNNER_TOOL_CACHE/emsdk-6.0.9" && ./emsdk install 6.0.9 && ./emsdk activate 6.0.9 && touch .ubq-6.0.9-ready)
    fi
fi
'''


def stages(platform):
    """Returns the ordered stages, including conditionally unexecuted ones."""
    if platform == "preflight":
        return (
            Stage("setup", "Style tools and hooks", STYLE_SETUP),
            Stage("orchestration-tests", "CI orchestration tests", r'''unset UBQ_BATTERY UBQ_CI_ANNOTATE UBQ_EVENT_LOG UBQ_RESULT_DIR UBQ_STAGE_KEY UBQ_STAGE_LABEL UBQ_STAGE_LOG_PATH UBQ_STAGE_JUNIT_PATH
python3 Scripts/test-gate-framework.py
python3 Scripts/test-ci-batteries.py --workflow .github/workflows/ci.yml'''),
            Stage("style", "Style and hygiene", "./Scripts/check-style.py"),
            Stage("capabilities", "Capability single source of truth", "./Scripts/check-capability-ssot.py"),
            Stage("terminfo", "Terminfo", "./Scripts/check-terminfo.sh"),
            Stage("workflows", "Workflow files", "./Scripts/check-workflows.sh"),
            Stage("build-dirs", "Build directory hygiene", "python3 ./Scripts/check-build-dirs.py ."),
            Stage("names", "Name collisions", "python3 ./Scripts/check-name-collisions.py ."),
            Stage("forward-names", "Forward-declaration collisions", "python3 ./Scripts/check-name-collisions.py . --forward"),
            Stage("alias-names", "Alias collisions", "python3 ./Scripts/check-name-collisions.py . --aliases"),
        )
    if platform == "linux":
        return (
            Stage("setup", "Linux tools and hooks", LINUX_SETUP),
            Stage("frontend-tools", "GTK/Vulkan toolchain", LINUX_FRONTEND_TOOLS),
            Stage("quic", "QUIC dependencies", './Scripts/ensure-quic-deps.sh "$UBIQUITTY_QUIC_PREFIX"', "code"),
            Stage("host-tests", "GCC host tests and QUIC integration", "SNAPSHOT=skip ./Scripts/run-tests.sh", "code", "gcc"),
            Stage("fuzz", "Required bounded libFuzzer harnesses", "FUZZ_REQUIRED=1 ./Scripts/run-tests.sh --fuzz-only", "code", "clang"),
            # REQUIRE: the gate stands down for a compiler that cannot read the
            # profile, and here that compiler is the one CI vouches applies it.
            # In the CI image that is the LLVM UBQ_PGO_LLVM names, which the
            # release AppImage is compiled with (Scripts/ci-linux-image.sh).
            Stage("pgo", "Linux release PGO profile",
                  'export PATH="${UBQ_PGO_LLVM:+$UBQ_PGO_LLVM:}$PATH"\n'
                  "python3 ./Scripts/check-profile-orphans.py\n"
                  "UBIQUITTY_PGO=REQUIRE python3 ./Scripts/check-pgo-applies.py --recorded-budget",
                  "code", "clang"),
            Stage("portable-build", "Portable snapshot build", "./Scripts/build-core-snapshot.sh"),
            Stage("portable-snapshots", "Portable reference catalog", "python3 Tests/Snapshots/run.py --backend portable --quiet"),
            Stage("host-glibc", "Linux host glibc-x86_64", "./Scripts/build-host.sh --release", "code"),
            Stage("agent-smokes", "Agent CLI, authentication, MCP, and completions",
                  './Scripts/build-agent.sh\nUBQ_REQUIRED_TOOLS="${UBQ_REQUIRED_TOOLS:-} ubiquitty-host ubq" ./Scripts/run-tests.sh agent_smoke agent_auth_smoke agent_mcp_smoke agent_completions_smoke ubq_smoke ubq_smoke_cleanup', "code"),
            Stage("host-musl", "Linux host static-musl-x86_64", "./Scripts/build-host-linux.sh x86_64", "code"),
            Stage("sanitizers", "Clang ASan and UBSan suite", "./Scripts/run-sanitized-tests.sh", "code", "clang"),
            Stage("linux-build", "Linux frontend Release", "./Scripts/build-linux-gtk.sh Release"),
            Stage("linux-smokes", "Linux headless smoke battery", "xvfb-run -a ./Scripts/run-linux-smokes.sh", "xvfb"),
            Stage("linux-snapshots", "Linux-native snapshots", "xvfb-run -a python3 Tests/Snapshots/run.py --backend linux --quiet", "xvfb"),
            Stage("linux-determinism", "Linux-native snapshot determinism", "xvfb-run -a python3 Tests/Snapshots/run.py --backend linux --twice linux_native_text", "xvfb"),
            Stage("wasm-tools", "Emscripten 6.0.9", WASM_SETUP, toolchain="wasm"),
            Stage("wasm-smoke", "WASM Core smoke", 'source "$EMSDK_ENV"\n./Scripts/wasm-smoke.sh', toolchain="wasm"),
            Stage("web-build", "Web frontend build", 'source "$EMSDK_ENV"\n./Scripts/build-web.sh', toolchain="wasm"),
            Stage("web-snapshots", "Web snapshot tier", 'source "$EMSDK_ENV"\npython3 Tests/Snapshots/run.py --backend web --quiet', toolchain="wasm"),
        )
    if platform == "apple":
        return (
            Stage("setup", "Apple tools and hooks", "command -v ninja >/dev/null 2>&1 || brew install ninja\ncommand -v go >/dev/null 2>&1 || brew install go\ncommand -v tmux >/dev/null 2>&1 || brew install tmux\ncommand -v chafa >/dev/null 2>&1 || brew install chafa\n./Scripts/install-hooks.sh"),
            Stage("swift-format", "Swift formatting", "./Scripts/format-code.sh --check --swift-only"),
            Stage("xcode", "Xcode workspace generation", "./Scripts/generate-xcode-workspace.sh", "cmake"),
            Stage("quic", "QUIC dependencies", './Scripts/ensure-quic-deps.sh "$UBIQUITTY_QUIC_PREFIX"', "code"),
            Stage("tidy", "Clang tidy", "./Scripts/check-tidy.sh --since origin/main"),
            # The build the README gives a contributor: the helpers bundled,
            # and neither the QUIC prefix nor the ad-hoc signing identity this
            # battery sets for its other stages. A hosted macOS runner has no
            # Docker for the static Linux hosts, which are built separately
            # and staged for the shipped bundle.
            Stage("macos-build", "macOS app",
                  "env -u UBIQUITTY_QUIC_PREFIX -u UBIQUITTY_SIGN_IDENTITY ./Scripts/build-macos.sh --no-linux-hosts"),
            # After the app, because the gate runs the built binary's own
            # dispatch modes. They drive a real shell through the ingest
            # thread, the drain signal's hop back to main, and the sync-frame
            # boundary — the pipeline legs a direct-feed test never crosses.
            Stage("macos-smokes", "macOS headless smokes", "python3 Scripts/check-macos-smokes.py"),
            # A hosted runner is a virtual Mac whose window server, display
            # timing and GPU differ from the machines the scenarios assert
            # about; the maintainers' Macs run them for every batch.
            Stage("ui-scenarios", "macOS UI scenarios", "python3 Scripts/check-ui-scenarios.py", "ui"),
            Stage("pgo", "Portable PGO profile",
                  "python3 ./Scripts/check-profile-orphans.py --profile Support/pgo/portable-core.profdata\n"
                  "UBIQUITTY_PGO=REQUIRE python3 ./Scripts/check-pgo-applies.py --profile Support/pgo/portable-core.profdata"),
            Stage("core-frames", "Core builder frames", "./Scripts/check-core-builder-frames.sh"),
            # Swift renderer drivers link the stamped portable Core archive.
            Stage("portable-build", "Portable snapshot build", "./Scripts/build-core-snapshot.sh"),
            Stage("swift-renderer", "Swift renderer drivers", "./Scripts/check-swift-renderer.sh"),
            Stage("forks", "Built symbol forks", './Scripts/check-forks.sh "$UBQ_BUILD_ROOT/macos"'),
            Stage("snapshots", "Portable and Apple snapshot regressions", "./Scripts/run-tests.sh snapshots"),
            Stage("host-tests", "Apple test suite", "SNAPSHOT=skip ./Scripts/run-tests.sh", "code"),
            Stage("ios-build", "iOS simulator app and UI bundle", "./Scripts/build-ios.sh --sdk iphonesimulator --ui-tests", "code"),
            Stage("ios-smokes", "iOS smokes", "./Scripts/run-ios-smokes.sh", "code"),
            Stage("ios-sync", "iOS Sync conformance", "./Scripts/run-ios-sync-conformance.sh", "code"),
            Stage("ios-protected-file", "iOS protected file", "./Scripts/run-ios-protected-file-tests.sh", "code"),
            Stage("ios-ui", "iOS UI tests", "./Scripts/run-ios-ui-tests.sh", "code"),
        )
    raise ValueError(f"unknown battery: {platform}")


def select_stages(manifest, keys):
    """The manifest's stages named by keys, in manifest order.

    An unknown key is an error rather than an empty selection: a stage renamed
    in the manifest would otherwise leave a caller running nothing and passing.
    """
    known = {stage.key for stage in manifest}
    unknown = [key for key in keys if key not in known]
    if unknown:
        raise ValueError(f"unknown stage: {', '.join(unknown)} (have: {', '.join(sorted(known))})")
    return tuple(stage for stage in manifest if stage.key in keys)


def condition_reason(stage, environment, which=shutil.which):
    if stage.condition == "code" and environment.get("UBQ_CI_CODE", "true") != "true":
        return "change contains only snapshot goldens"
    if stage.condition == "cmake" and (
            environment.get("UBQ_CI_CODE", "true") != "true"
            or environment.get("UBQ_CI_CMAKE", "true") != "true"):
        return "change does not reach the build configuration"
    if stage.condition == "ui" and environment.get("UBQ_CI_UI_SCENARIOS", "true") != "true":
        return "UI scenarios run by hand on a hosted runner; the maintainers' Macs run them per batch"
    if stage.condition == "xvfb" and not which("xvfb-run", path=environment.get("PATH")):
        return "UNGATED: xvfb-run is unavailable; the display capability was not exercised"
    return ""


def platform_environment(platform, environment):
    result = dict(environment)
    result.setdefault("UBQ_CI_CODE", "true")
    result.setdefault("UBQ_CI_CMAKE", "true")
    result.setdefault("UBQ_REPORT_MODE", "quiet" if result.get("GITHUB_ACTIONS") else "verbose")
    result["UBQ_CI_ROOT"] = str(result_root(result))
    result["UBQ_RESULT_DIR"] = str(result_directory(platform, result))
    result["UBQ_EVENT_LOG"] = str(Path(result["UBQ_RESULT_DIR"]) / "events.jsonl")
    result["UBQ_BATTERY"] = platform
    if platform == "preflight":
        pin = (ROOT / "Scripts/clang-format-pin").read_text(encoding="utf-8").strip()
        result["PATH"] = str(Path(result["RUNNER_TOOL_CACHE"]) / f"clang-format-{pin}/bin") + os.pathsep + result.get("PATH", "")
        return result
    result["UBQ_TEST_FAIL_FAST"] = "1"
    result["CCACHE_STATSLOG"] = str(Path(result["RUNNER_TEMP"]) / "ccache-stats.log")
    result["UBIQUITTY_SNAPSHOT_FAILURE_DIR"] = str(Path(result["RUNNER_TEMP"]) / "ubiquitty-regression-fail")
    if platform == "linux":
        result["UBQ_BUILD_ROOT"] = str(Path(result["RUNNER_WORKSPACE"]) / "build")
        result["CC"], result["CXX"] = "gcc", "g++"
        # Mesa's lavapipe, because a hosted runner has no GPU. A machine that
        # has one sets UBQ_CI_LINUX_VULKAN=native and runs its own driver:
        # lavapipe cannot export dma-buf, so the dma-buf smokes fail on it
        # whatever the tree does.
        if result.get("UBQ_CI_LINUX_VULKAN") != "native":
            result["VK_ICD_FILENAMES"] = "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"
            result["LIBGL_ALWAYS_SOFTWARE"] = "1"
        installed = Path("/data/emsdk/emsdk_env.sh")
        result["EMSDK_ENV"] = str(installed if installed.is_file() else Path(result["RUNNER_TOOL_CACHE"]) / "emsdk-6.0.9/emsdk_env.sh")
    else:
        result["UBQ_BUILD_ROOT"] = subprocess.check_output(
            [sys.executable, "Scripts/build_root.py"], cwd=ROOT, env=result, text=True).strip()
        result["UBIQUITTY_SIGN_IDENTITY"] = "ad-hoc"
    quic_root = Path(result["RUNNER_WORKSPACE"]) / "build-gcc" if platform == "linux" else Path(result["UBQ_BUILD_ROOT"])
    result["UBIQUITTY_QUIC_PREFIX"] = str(quic_root / "quic-deps")
    return result


def gcc_identity(compiler, language, environment):
    """Identifies the driver behind a name without confusing ccache with GCC."""
    try:
        def query(*arguments):
            return subprocess.check_output([compiler, *arguments], input="", text=True,
                                           stderr=subprocess.DEVNULL, env=environment, timeout=10).strip()
        macros = query("-dM", "-E", "-x", language, "-")
        if not re.search(r"^#define __GNUC__ ", macros, re.M) or re.search(r"^#define __clang__ ", macros, re.M):
            return None
        return (query("-dumpmachine"), query("-dumpfullversion"),
                hashlib.sha256(query("-dumpspecs").encode()).hexdigest(),
                tuple(sorted(macros.splitlines())))
    except (OSError, subprocess.SubprocessError):
        return None


def retained_gcc_drivers(environment, identify=gcc_identity):
    """Keeps a suite cache's driver spelling only for the same GCC toolchain."""
    defaults = {"CC": "gcc", "CXX": "g++"}
    cache = Path(environment["UBQ_BUILD_ROOT"]) / "BuildHostTests/ctest/CMakeCache.txt"
    try:
        content = cache.read_text(encoding="utf-8")
    except OSError:
        return defaults
    retained = {}
    for variable, language, cmake_name in (("CC", "c", "C"), ("CXX", "c++", "CXX")):
        match = re.search(rf"^CMAKE_{cmake_name}_COMPILER:[A-Z]+=(.+)$", content, re.M)
        if not match:
            return defaults
        cached = match.group(1)
        expected = identify(defaults[variable], language, environment)
        if expected is None or identify(cached, language, environment) != expected:
            return defaults
        retained[variable] = cached
    return retained


def stage_environment(stage, platform, environment):
    result = dict(environment)
    result["UBQ_STAGE_KEY"] = stage.key
    result["UBQ_STAGE_LABEL"] = stage.label
    # Package recipes live in this manifest rather than the workflow. Their
    # digest joins the tool-version stamp so changing a recipe reinstalls it.
    result["UBQ_CI_SETUP_KEY"] = hashlib.sha256(stage.command.encode()).hexdigest()[:16]
    if platform == "linux" and stage.toolchain != "native":
        if stage.toolchain == "gcc":
            result["UBQ_BUILD_ROOT"] = str(Path(result["RUNNER_WORKSPACE"]) / "build-gcc")
            result.update(retained_gcc_drivers(result))
        elif stage.toolchain == "clang":
            # run-tests sweeps native CMake caches before it parses --fuzz-only.
            # A sibling root keeps that sweep out of the platform builds.
            result["UBQ_BUILD_ROOT"] = str(Path(result["RUNNER_WORKSPACE"]) / "build-clang")
            result["CC"], result["CXX"] = "clang", "clang++"
        else:
            result.pop("CC", None)
            result.pop("CXX", None)
    return result


def initial_records(manifest):
    return [{"key": stage.key, "label": stage.label, "state": "unexecuted",
             "detail": "battery did not reach this stage"} for stage in manifest]


def save_records(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(records), encoding="utf-8")
    temporary.replace(path)


def validate_events(platform, manifest, path):
    """Validates the routing artifact independently of console success."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        return [f"could not read {path}: {error}"]
    if not lines:
        return [f"{path} is empty"]
    events = []
    for line_number, line in enumerate(lines, 1):
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            return [f"{path}:{line_number}: invalid JSON: {error}"]
        if event.get("schema") != "ubiquitty.ci-event.v1":
            return [f"{path}:{line_number}: unknown event schema"]
        events.append(event)
    failures = []
    artifact_root = path.parent.resolve()
    terminal_states = {"passed", "failed", "unexecuted"}
    for stage in manifest:
        matching = [event for event in events
                    if event.get("kind") == "stage"
                    and event.get("source") == f"{platform}-battery"
                    and event.get("stageKey") == stage.key
                    and event.get("stageLabel") == stage.label
                    and event.get("label") == stage.label]
        terminal = next((event for event in reversed(matching)
                         if event.get("state") in terminal_states), None)
        if terminal is None:
            failures.append(f"stage {stage.key} has no terminal event")
            continue
        for field in ("battery", "stageKey", "stageLabel", "command"):
            if not terminal.get(field):
                failures.append(f"stage {stage.key} terminal event lacks {field}")
        if terminal.get("state") in ("passed", "failed"):
            for field in ("elapsedSeconds", "exitStatus", "logArtifactPath"):
                if field not in terminal or terminal.get(field) in (None, ""):
                    failures.append(f"stage {stage.key} executed without {field}")
    for line_number, event in enumerate(events, 1):
        existing_artifacts = []
        for field in ("logArtifactPath", "junitArtifactPath"):
            value = event.get(field)
            if value in (None, ""):
                continue
            if not isinstance(value, str) or Path(value).is_absolute():
                failures.append(f"event {line_number} has invalid {field}")
                continue
            resolved = (artifact_root / value).resolve()
            try:
                resolved.relative_to(artifact_root)
            except ValueError:
                failures.append(f"event {line_number} has escaping {field}")
                continue
            if resolved.is_file():
                existing_artifacts.append(field)
        github = event.get("github", {})
        if github.get("eventName") == "pull_request":
            for field in ("repository", "runId", "runAttempt", "pullRequest", "headSha"):
                if not github.get(field):
                    failures.append(f"event {line_number} lacks GitHub PR routing field {field}")
        is_failure = event.get("state") == "failed" or (
            event.get("kind") == "finding"
            and event.get("state") in ("fatal error", "error"))
        if not is_failure or event.get("kind") not in (
                "stage", "suite", "test", "gate", "finding"):
            continue
        for field in ("reason", "command"):
            if not event.get(field):
                failures.append(f"event {line_number} failed without {field}")
        if "elapsedSeconds" not in event:
            failures.append(f"event {line_number} failed without elapsedSeconds")
        if "exitStatus" not in event and "suiteExitStatus" not in event:
            failures.append(f"event {line_number} failed without an exit status")
        if not event.get("logArtifactPath") and not event.get("junitArtifactPath"):
            failures.append(f"event {line_number} failed without a diagnostic artifact path")
        elif not existing_artifacts:
            failures.append(f"event {line_number} failed without an existing diagnostic artifact")
    return failures


def execute_stage(stage, environment):
    return subprocess.run(["bash", "-e", "-o", "pipefail", "-c", stage.command],
                          cwd=ROOT, env=environment, check=False).returncode


def result_root(environment):
    """The run's artifact root: UBQ_CI_ROOT, else $RUNNER_TEMP/ubiquitty-ci.

    One directory per run is what keeps two runs on one machine apart. The
    module docstring says who sets the variable and what the default is for.
    """
    configured = environment.get("UBQ_CI_ROOT")
    if configured:
        return Path(configured)
    return Path(environment.get("RUNNER_TEMP", "/tmp")) / "ubiquitty-ci"


def result_directory(platform, environment):
    """The one artifact directory a battery and its nested tools share."""
    return result_root(environment) / platform


def run_battery(platform, manifest, environment, path, execute=None):
    records = initial_records(manifest)
    reporter = Reporter(f"{platform}-battery", environment)
    result_root = result_directory(platform, environment)
    result_root.mkdir(parents=True, exist_ok=True)
    save_records(path, records)
    # Seed a terminal record for every stage before setup can receive a signal.
    # Later running/passed/failed records supersede it in append order.
    for stage in manifest:
        reporter.record("stage", label=stage.label, state="unexecuted",
                        reason="battery did not reach this stage", command=stage.command,
                        battery=platform, stageKey=stage.key, stageLabel=stage.label)
    # Classify capability/reach exclusions before execution, so a failed first
    # stage does not disguise them as tests blocked by that failure.
    for stage, record in zip(manifest, records):
        reason = condition_reason(stage, environment)
        if reason:
            record["detail"] = reason
            record["state"] = "unexecuted"
            reporter.record("stage", label=stage.label, state="unexecuted", reason=reason,
                            command=stage.command, battery=platform,
                            stageKey=stage.key, stageLabel=stage.label)
    save_records(path, records)

    def block_later_stages(start_index, failed_label):
        for later_stage, later in zip(manifest[start_index:], records[start_index:]):
            if later["state"] != "unexecuted" \
                    or later["detail"] != "battery did not reach this stage":
                continue
            later["detail"] = f"blocked by failed stage: {failed_label}"
            reporter.record("stage", label=later["label"], state="unexecuted",
                            reason=f"blocked by {failed_label}", battery=platform,
                            stageKey=later["key"], stageLabel=later["label"],
                            command=later_stage.command)

    for stage_index, (stage, record) in enumerate(zip(manifest, records), 1):
        if record["detail"] != "battery did not reach this stage":
            if record["detail"].startswith("UNGATED:"):
                print(f"::warning title={stage.label}::{record['detail']}", flush=True)
            continue
        record.update(state="running", detail="interrupted before a verdict")
        save_records(path, records)
        stage_environment_values = stage_environment(stage, platform, environment)
        stage_reporter = Reporter(f"{platform}-battery", stage_environment_values)
        log_path = result_root / f"{stage_index:02d}-{stage.key}.log"
        if execute is None:
            started = time.monotonic()
            try:
                result = stage_reporter.run_process(
                    ["bash", "-e", "-o", "pipefail", "-c", stage.command],
                    label=stage.label, cwd=ROOT, environment=stage_environment_values,
                    log_path=str(log_path), command=stage.command)
            except SystemExit as interruption:
                status = interruption.code if isinstance(interruption.code, int) else 1
                elapsed = time.monotonic() - started
                if not log_path.is_file():
                    log_path.write_text(
                        f"{stage.label}: interrupted with exit {status}\n", encoding="utf-8")
                stage_reporter.record(
                    "stage", label=stage.label, state="failed",
                    elapsed_seconds=elapsed, reason=f"interrupted with exit {status}",
                    command=stage.command, exitStatus=status, logPath=str(log_path),
                    battery=platform, stageKey=stage.key, stageLabel=stage.label)
                record.update(state="failed", detail=f"interrupted; exit {status}; {elapsed:.1f}s",
                              elapsedSeconds=round(elapsed, 4), command=stage.command,
                              logPath=str(log_path))
                block_later_stages(stage_index, stage.label)
                save_records(path, records)
                raise
            status = result.status
            elapsed = result.elapsed_seconds
        else:
            stage_reporter.start(stage.label, command=stage.command)
            started = time.monotonic()
            try:
                status = execute(stage, stage_environment_values)
            except OSError as error:
                print(f"Could not run {stage.label}: {error}", file=sys.stderr)
                status = 127
            elapsed = time.monotonic() - started
            stage_reporter.finish(stage.label, status, elapsed, command=stage.command)
        record.update(state="completed" if status == 0 else "failed",
                      detail=f"exit {status}; {elapsed:.1f}s",
                      elapsedSeconds=round(elapsed, 4), command=stage.command,
                      logPath=str(log_path) if execute is None else "")
        save_records(path, records)
        if status == 0 and environment.get("GITHUB_ACTIONS") \
                and environment.get("UBQ_RETAIN_PASSED_LOGS") != "1":
            try:
                log_path.unlink()
            except OSError:
                pass
        if status:
            block_later_stages(stage_index, stage.label)
            save_records(path, records)
            return 1
    # A capability exclusion permits success under that capability's policy,
    # but cannot prove a full checkpoint for future reach decisions.
    full_code = environment.get("UBQ_CI_CODE", "true") == "true" and not any(
        record["detail"].startswith("UNGATED:") for record in records)
    if environment.get("GITHUB_OUTPUT"):
        with open(environment["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            output.write(f"full_code={str(full_code).lower()}\n")
    return 0


def report(platform, manifest, path, environment):
    records = initial_records(manifest)
    if path.is_file():
        recorded = {entry["key"]: entry for entry in json.loads(path.read_text(encoding="utf-8"))}
        for record in records:
            record.update(recorded.get(record["key"], {}))
    lines = [f"### {platform.title()} battery stages", "", "| Stage | Result | Detail |", "| --- | --- | --- |"]
    for record in records:
        state = record["state"] if record["state"] != "running" else "unexecuted"
        lines.append(f"| {record['label']} | {state} | {record['detail']} |")
    lines += ["", "Completed means the command succeeded; any internal harness skips remain in its annotations.",
              "Benchmark and coverage measurements are suspended.", ""]
    rendered = "\n".join(lines)
    print(rendered)
    if environment.get("GITHUB_STEP_SUMMARY"):
        with open(environment["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write(rendered)
    tally = Path(environment.get("RUNNER_TEMP", "/tmp")) / "ccache-stats.log"
    if platform != "preflight" and tally.is_file() and shutil.which("ccache"):
        subprocess.run(["ccache", "--show-log-stats"], env=dict(environment, CCACHE_STATSLOG=str(tally)), check=False)
    failures = validate_events(
        platform, manifest, result_directory(platform, environment) / "events.jsonl")
    if failures:
        for failure in failures:
            print(f"ci-batteries: routing artifact invalid — {failure}", file=sys.stderr)
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("run", "report", "list"))
    parser.add_argument("platform", choices=("preflight", "linux", "apple"))
    parser.add_argument("--only", action="append", default=[], metavar="STAGE",
                        help="with run: execute only this stage (repeatable)")
    arguments = parser.parse_args()
    manifest = stages(arguments.platform)
    if arguments.only:
        if arguments.operation != "run":
            parser.error("--only applies to run")
        try:
            manifest = select_stages(manifest, arguments.only)
        except ValueError as error:
            parser.error(str(error))
    if arguments.operation == "list":
        for stage in manifest:
            print(f"{stage.key}: {stage.label} [{stage.condition}; {stage.toolchain}]")
        return 0
    environment = dict(os.environ)
    run_key = environment.get("GITHUB_RUN_ID", "local") + "-" + environment.get("GITHUB_RUN_ATTEMPT", "1")
    path = result_directory(arguments.platform, environment) / f"battery-{run_key}.json"
    if arguments.operation == "report":
        return report(arguments.platform, manifest, path, environment)
    environment = platform_environment(arguments.platform, environment)
    try:
        Path(environment["UBQ_EVENT_LOG"]).unlink()
    except FileNotFoundError:
        pass
    save_records(path, initial_records(manifest))
    return run_battery(arguments.platform, manifest, environment, path)


if __name__ == "__main__":
    sys.exit(main())
