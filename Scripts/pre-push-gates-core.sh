#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# Scripts/pre-push-gates-core.sh — the pre-push gates ubiquitty-core answers for.
#
# Sourced after Scripts/pre-push-gates-lib.sh by the Scripts/pre-push-gates.sh of
# a repository that holds ubiquitty-core's files: ubiquitty-core itself, and a
# repository that holds everything. Each `core_gate_*` function is one family of
# gates. A gate whose subject is any repository's tree is `run_for "core kit app"`,
# so the repositories that carry ubiquitty-core as a library run it over their own
# files; a gate that judges only what ubiquitty-core holds is `run_for "core"`.
#
# `core_tier` is the list ubiquitty-core runs on its own, by tier. A repository
# that holds more calls the families itself, beside its own.

# Every gate pre-commit runs, run again here.
#
# A rule enforced at commit alone holds only as long as every commit runs its
# hook. `git commit --no-verify` is one flag, and pre-commit documents it as
# turning off all of them: such a rule is one that flag retires, with the tree
# then carrying the violation under a full row of passing push gates.
#
# Push is where work stops being local, so it is the boundary that has to hold
# independently of what any commit did. check-gate-coverage.py keeps the two
# lists nested from the other side: a gate added to pre-commit and not to here
# fails Scripts/check-gate-coverage.py.
#
# No argument is passed where pre-commit passes --staged or a root. Those gates
# read the whole tree when given none, which is what a push wants — the question
# here is whether the tree is clean, not which lines this branch touched.
core_gate_commit_rules() {
    run_for "core kit app" "build dirs" python3 ./Scripts/check-build-dirs.py
    run_for "core" "conformance claims" python3 ./Scripts/check-conformance-claims.py
    run_for "core" "core subset paths" bash ./Scripts/check-core-subset-paths.sh
    run_for "core kit app" "empty array expansion" python3 ./Scripts/check-empty-array-expansion.py
    run_for "core kit app" "enum switches" python3 ./Scripts/check-enum-switches.py
    run_for "core kit app" "glyph backend claims" python3 ./Scripts/check-glyph-backend-claims.py
    run_for "core kit app" "literal qualifiers" python3 ./Scripts/check-literal-qualifiers.py
    run_for "core kit app" "memcmp fold" python3 ./Scripts/check-memcmp-fold.py
    run_for "core kit app" "name collisions" python3 ./Scripts/check-name-collisions.py
    run_for "core" "pgo commit shape" python3 ./Scripts/check-pgo-commit-shape.py
    run_for "core" "profile orphans" python3 ./Scripts/check-profile-orphans.py
    run_for "core app" "quic mandatory" python3 ./Scripts/check-quic-mandatory.py
    run_for "core" "showcase goldens" python3 ./Scripts/check-showcase-goldens.py
    run_for "core" "snapshot harness" python3 ./Scripts/check-snapshot-harness.py
    run_for "core kit app" "stale cmake cache" python3 ./Scripts/check-stale-cmake-cache.py
}

# ci.yml: style — check-style.py and check-capability-ssot.py.
#
# test-ci-batteries.py is here because ci.yml's preflight runs it, and a verdict
# only CI produces reaches only whoever opens the run. It parses the workflow
# and the CMake test registrations, both of which drift under ordinary edits — a
# set_tests_properties wrapped across lines can read as the property having been
# deleted, and the preflight then stops there for every push.
# It reads ci.yml as the preflight does: without --workflow the workflow
# contract test skips, and an edit to ci.yml reaches the preflight untested.
#
# The style checks, then the one step that compiles; core_gate_style_checks
# alone is what the quick tier runs.
core_gate_style() {
    core_gate_style_checks
    core_gate_bench_compiles
}

core_gate_style_checks() {
    # Whole tree, not the branch diff. This tier is the floor because it is the
    # one an author can rely on: it runs before the push, on the machine making
    # it, whatever the remote is doing. A rule narrowed here is narrowed for
    # every push regardless of whether anything downstream is watching.
    # --since stays available for a fast local pass; it is not what gates a push.
    run_for "core kit app" "style (check-style.py)" ./Scripts/check-style.py
    run_for "core kit app" "workflow lint (actionlint)" ./Scripts/check-workflows.sh
    run_for "core kit app" "capability SSOT" ./Scripts/check-capability-ssot.py
    run_for "core kit app" "gate coverage" python3 ./Scripts/check-gate-coverage.py
    run_whole "CI orchestration tests" python3 ./Scripts/test-ci-batteries.py --workflow .github/workflows/ci.yml
    run_for "core" "terminfo infocmp gate" ./Scripts/check-terminfo.sh
    # Here rather than in pre-commit: it scans the whole tree twice, once here
    # and once in a throwaway worktree at origin/main, which is a push's budget
    # and not a commit's. The reference is computed rather than recorded because
    # a number several checkouts share goes stale the moment any of them lands
    # anything, and then blocks all of them.
    run_for "core kit app" "backlog growth" python3 ./Scripts/check-backlog-growth.py
}

# Tests/Performance is built by scripts that each name the benchmarks they
# want, and several benchmarks are named by none of them. Nothing compiles
# those, so a tree-wide edit rots them with every gate green. It compiles every
# benchmark whatever the push changed, so it is CI's and the full tier's.
core_gate_bench_compiles() {
    run_for "core" "benchmarks compile" ./Scripts/check-bench-compiles.sh
}

# gate_style minus the one step in it that compiles.
#
# check-bench-compiles.sh builds the benchmarks Tests/Performance holds, which is
# the whole of what gate_style spends a toolchain on. Everything else there reads
# the tree: check-style.py, the capability SSOT, and the backlog ratchet against a
# throwaway worktree at origin/main.
#
# The skipped gate is announced through the same SKIPPED channel a missing tool
# uses, so the summary says a build gate stood down instead of reporting that
# everything ran. A tier that quietly narrowed what it checked would be the
# bypass this ladder refuses to have.
core_gate_style_without_builds() {
    run_for "core kit app" "style (check-style.py)" ./Scripts/check-style.py
    run_for "core kit app" "workflow lint (actionlint)" ./Scripts/check-workflows.sh
    run_for "core kit app" "capability SSOT" ./Scripts/check-capability-ssot.py
    run_for "core kit app" "gate coverage" python3 ./Scripts/check-gate-coverage.py
    run_whole "CI orchestration tests" python3 ./Scripts/test-ci-batteries.py --workflow .github/workflows/ci.yml
    run_for "core kit app" "backlog growth" python3 ./Scripts/check-backlog-growth.py
    SKIPPED+=("benchmarks compile (tier lint builds nothing; CI compiles them, and so does the full tier)")
}

# The host's builds and the tests the push reaches. A repository that carries
# Core calls this whole; CI calls the halves, since the whole suite runs there
# and a subset of it first would run those tests twice.
core_gate_host() {
    core_gate_host_builds
    core_gate_reached_tests
}

# ci.yml: host-linux (glibc). The host's platform branches are invisible to a
# frontend-only build, and its source subset is trimmed, so a kept translation
# unit calling into a trimmed one is caught at link or not at all.
core_gate_host_builds() {
    run_scoped "ubiquitty-host build" \
        "Source/Platform/Host Source/Platform/POSIX Source/Agent Source/Core Source/Base Source/PAL CMakeLists.txt Scripts/cmake cmake Scripts/portable-core-sources.txt Scripts/check-core-layer-partition.sh Scripts/build-host.sh Scripts/build-provenance.sh Scripts/build-root.sh Scripts/build_root.py" \
        ./Scripts/build-host.sh
    # ubq trims Core through its own ubiquitty_core_subset(), so a
    # kept translation unit calling into an excluded one fails here and nowhere
    # else — the host subset excludes a different set and links fine. A source
    # entering the SSOT flows into both subsets, and can call into a translation
    # unit only one of them trims.
    #
    # One second warm, which is what makes a second subset worth gating.
    run_scoped "ubq build" \
        "Source/Agent Source/Platform/Host Source/Platform/POSIX Source/Core Source/Base Source/PAL CMakeLists.txt Scripts/cmake cmake Scripts/portable-core-sources.txt Scripts/check-core-layer-partition.sh Scripts/build-agent.sh Scripts/build-provenance.sh Scripts/build-root.sh Scripts/build_root.py" \
        ./Scripts/build-agent.sh
    # Tests/ COMPILES. Not the suite — the suite is retired from this tier for
    # the load reason below, and it stays retired.
    #
    # No frontend build compiles Tests/, so a namespace move can pass host,
    # agent, Linux and macOS and leave a test file unable to compile: a
    # using-directive and a namespace alias are the two spellings that carry a
    # name with no trailing `::`, and every frontend is green because no
    # frontend includes them.
    #
    # Building is the whole of what this adds. Running is what costs, and a file
    # that does not compile fails here for the same reason it would fail there,
    # sooner and for a tenth of the machine.
    run_scoped "Tests/ compiles" \
        "Source Tests CMakeLists.txt Scripts/cmake cmake Scripts/build-tests.sh Scripts/ubq_roots.py Scripts/check-build-reached.py Scripts/portable-core-sources.txt Scripts/build-root.sh Scripts/build_root.py Scripts/build-jobs.sh Scripts/build_jobs.py" \
        ./Scripts/build-tests.sh
}

# The tests this branch can break, against the tree core_gate_host_builds built.
core_gate_reached_tests() {
    # And RUNS the ones this branch can break. Compiling is not running: the
    # rows `test_preferences_keys` hand-copies from the `Key.h` descriptor table
    # can go one short, red on every platform, while every build gate here is
    # green.
    #
    # Not the suite, which stays retired for the load reason above — the tests
    # whose objects actually READ a file the branch changed, which ninja's
    # dependency database already knows per object. A settings change selects
    # five; a rendering change selects about twenty and runs them in under
    # thirty seconds. The directory is warm from the step above, so what this
    # adds is the run.
    #
    # It needs that build: asked against an unbuilt directory it refuses rather
    # than select nothing, because nothing selected is what a green run looks
    # like.
    #
    # Scoped to the build's paths and to everything a gate script can read. A
    # stanza that is nothing but gate scripts is selected by the paths those
    # scripts spell, and the scripts are read to learn them, so all of Scripts
    # and each directory a script may name is an input: a verdict cached across
    # a change to a script would be one for a selection that script no longer
    # makes.
    run_scoped "tests reaching the diff" \
        "Source Tests CMakeLists.txt cmake Scripts Tools Docs Support Samples Resources" \
        ./Scripts/check-tests-reaching-the-diff.py "$(python3 "$UBQ_CORE_ROOT/Scripts/build_root.py")/BuildHostTests/ctest" --jobs "$JOBS"
    # A run of the tests leaves no process running and adds nothing to the
    # state directory of whoever ran it. Every run-tests.sh invocation judges
    # itself on the way out, so the tiers that run stanzas are covered by the
    # stanzas; this proves the judge still fails, against a stanza that leaks
    # on purpose. Five seconds, and it builds nothing; unscoped, because what
    # it reads is the driver itself, whose inputs are the whole test tree.
    core_gate_test_hygiene
}

core_gate_test_hygiene() {
    run_for "core" "test runs leave nothing behind" ./Scripts/run-tests.sh run_hygiene
}

# The complete suite is selected by the ctest and full tiers. Quick and default
# run the focused dependency-based selection in gate_host instead.
core_gate_ctest() {
    local build="BuildHostTests/ctest"
    run "ctest configure" cmake -S . -B "$build" -G Ninja \
        -DUBIQUITTY_BUILD_TESTS=ON -DUBIQUITTY_FRONTEND=Core -DCMAKE_BUILD_TYPE=Debug
    # A failed configure is already reported; ctest against it adds only noise.
    [ -f "$build/CMakeCache.txt" ] || { SKIPPED+=("ctest (no configure)"); return; }
    run "ctest suite" env -C "$build" ctest -j"$JOBS" --output-on-failure
}

# What this branch detaches from the PGO profile, diff-scoped, and what the
# whole tree has detached since the retrain. Drift found weeks later is hard to
# place; per branch the number is small and names its function. The absolute
# budget at retrain lives in pgo-train.sh (--max-discarded 0), which also
# records the total-drift budget beside the profile; the first bound here is on
# the new mass only, and the value is the performance owner's knob.
# The budget is far below buildCellInstances-class detachment — a change to
# that function's shape discards its whole profile — so an edit there always
# refuses the push and lands as a pair: the change plus a retrain on the same branch, retrain
# second, driven by whoever owns the performance work. A branch that changes no portable-core TU
# prints zero and passes.
#
# It is its own gate because the per-branch bound and the test suite answer
# different questions, and a bound that only runs when someone asks for the
# suite bounds nothing: a profile bounded only beside the suite can drift far
# enough to lose whole lanes against a retrained one. It compiles only the
# portable-core units the branch changed, which is seconds.
#
# The per-branch bound is the quick tier's; the total, which compiles every
# portable-core unit whatever the push changed, is CI's and the full tier's.
core_gate_pgo_drift() {
    core_gate_pgo_diff
    core_gate_pgo_total
}

core_gate_pgo_diff() {
    # 30000, set from the tree rather than picked: a budget far above anything
    # this tree produces runs on every push and cannot fail. An ordinary branch
    # reads zero; the budget sits above the drift a long run of commits
    # accumulates, so it tolerates a branch that rewrites that much and fails
    # one that goes further.
    #
    # Raise it with the number the failure prints, and say what the branch did
    # to earn it. A profile retrain is the other answer, and the better one when
    # the drift is real rather than a one-off.
    run "pgo diff-applies" python3 ./Scripts/check-pgo-applies.py \
        --changed-from origin/main --max-new-discarded 30000
}

core_gate_pgo_total() {
    # The total, against the budget pgo-train.sh recorded beside the profile.
    # The bound above reads only the units a push changed: a header edit that
    # reshapes every function inlining it compiles nothing there and reads
    # zero, and pushes each under 30000 can leave the profile far off the tree
    # within days of a retrain. This compiles every portable-core unit across
    # the cores, and refuses once the total exceeds the
    # recorded budget; from 60% of it the gate prints a warning line that the
    # runner repeats beside the pass. A profile with no budget beside it stands
    # down here, by name, until a retrain records one.
    run "pgo total drift" python3 ./Scripts/check-pgo-applies.py --recorded-budget
}

# ci.yml: host-linux + linux-frontend. Needs Docker (OrbStack or Docker Desktop); absent is SKIPPED.
# The container runtime takes a whole core for the length of both builds, so a
# loaded machine may switch it off.
#
# A switched-off run names it in the skip list rather than passing over it,
# because what it covers is not covered elsewhere: no other gate compiles
# Source/Platform/Linux at all, and the container is the only arm that reads
# the toolchain floor. Without it, a branch that touches the GTK frontend or a
# Core header it includes reaches main with nobody having compiled it.
#
#   UBIQUITTY_LINUX_GATE=off ./Scripts/pre-push-gates.sh   (to skip it)
#
core_gate_linux() {
    if [ "${UBIQUITTY_LINUX_GATE:-on}" = "off" ]; then
        SKIPPED+=("linux build (paused; UBIQUITTY_LINUX_GATE=off pauses it)")
        printf '  %-34s %s\n' "linux build" "PAUSED (UBIQUITTY_LINUX_GATE=off)"
        return
    fi
    if ! command -v docker >/dev/null 2>&1; then
        SKIPPED+=("linux build (no docker; install OrbStack or Docker Desktop)")
        printf '  %-34s %s\n' "linux build" "SKIPPED (no docker)"
        return
    fi
    run "Core + host on Linux" ./Scripts/run-linux.sh test
}

# One implementation, checked against the symbol tables the build just produced.
#
# Docs/coding-conventions.md §10 says portable policy lives in Core and every
# frontend routes through it. A forked copy of that policy is fixed in one place
# and tested in another: with two byte-identical copies of an ownership gate,
# the test suite can link the copy that lacks a fix the other one carries.
#
# After gate_host, because it reads objects rather than sources and a build
# directory this push did not refresh answers about an older tree. Only the
# directories this tier actually built are passed.
core_gate_forks() {
    local dirs=""
    [ -d "$UBQ_BUILD_ROOT/host" ] && dirs="$dirs $UBQ_BUILD_ROOT/host"
    [ "$(uname -s)" = "Darwin" ] && [ -d "$UBQ_BUILD_ROOT/macos" ] \
        && dirs="$dirs $UBQ_BUILD_ROOT/macos"
    if [ -z "$dirs" ]; then
        SKIPPED+=("one implementation (no built directory to read symbol tables from)")
        return 0
    fi
    # shellcheck disable=SC2086
    run "one implementation" ./Scripts/check-forks.sh $dirs
}

# The wasm arm's premise, proven freshly: wasm32's toolchain rejects a shape a
# 64-bit host accepts, so a Core that builds under emscripten is a claim about a
# different standard library. The build itself is the web frontend's gate; this
# is Core's half, and it reads a fixture, not the tree. Selected on the same
# terms as the build: a branch touching neither Source/Core nor Source/Base
# cannot break it. A machine without emsdk reports SKIPPED, counted as having
# checked nothing.
core_gate_wasm_oracle() {
    run "wasm arm disagrees with host" ./Scripts/check-wasm-oracle.sh
}

# The wasm32 arm of Core. Proves the arm still disagrees with the host before
# trusting its build: a build that compiles everything and says ok is
# indistinguishable from one whose toolchain quietly stopped rejecting what the
# host accepts. Selected by a change to what it compiles, unless every gate is.
core_gate_wasm() {
    if ! ubq_select_all; then
        [ -n "$(git -C "$ROOT" diff --name-only origin/main...HEAD -- Source/Core Source/Base Source/PAL Scripts/cmake CMakeLists.txt Scripts/portable-core-sources.txt 2>/dev/null)" ] || return 0
    fi
    if ! command -v emcmake >/dev/null 2>&1; then
        SKIPPED+=("wasm build (no emscripten; emsdk provides emcmake)")
        printf '  %-34s %s\n' "wasm build" "SKIPPED (no emscripten)"
        return
    fi
    core_gate_wasm_oracle
    run "portable Core on wasm" ./Scripts/build-core-wasm.sh
}

# The suite under AddressSanitizer and UndefinedBehaviorSanitizer, and the host
# built and driven with the same instrumentation (Scripts/run-sanitized-tests.sh).
# CI's Linux battery runs it with clang; here it is the full tier's.
core_gate_sanitizers() {
    run_for "core" "sanitized suite" ./Scripts/run-sanitized-tests.sh
}

# Clang's thread-safety and function-effect analyses, proved to have run: the
# host, ubq and the test tree built with the configured compiler, then every
# tree this build root holds checked to have compiled with
# UBIQUITTY_OWNERSHIP_ANALYSIS on. The option defaults on only for a Clang that
# has both analyses, and off without a word in the build for any other, so a
# green build is evidence of the analysis only once its cache says it ran.
core_gate_ownership() {
    core_gate_host_builds
    run_for "core" "ownership analysis on" python3 ./Scripts/check-ownership-analysis-on.py "$UBQ_BUILD_ROOT"
}

# The tiers of a checkout that holds ubiquitty-core and nothing above it. The
# frontends' arms and the snapshot regressions are the gates of the repositories
# that hold those frontends; the wasm arm and its oracle are Core's own.
core_tier() {
    case "$TIER" in
        lint) core_gate_style_without_builds; core_gate_commit_rules ;;
        quick | default) core_gate_pgo_drift; core_gate_style; core_gate_commit_rules; core_gate_host; core_gate_forks; core_gate_wasm ;;
        ctest) core_gate_pgo_drift; core_gate_style; core_gate_commit_rules; core_gate_host; core_gate_forks; core_gate_wasm; core_gate_ctest ;;
        linux) core_gate_pgo_drift; core_gate_style; core_gate_commit_rules; core_gate_host; core_gate_forks; core_gate_wasm; core_gate_linux ;;
        full)
            core_gate_pgo_drift; core_gate_style; core_gate_commit_rules; core_gate_host; core_gate_forks
            core_gate_wasm; core_gate_ctest; core_gate_sanitizers; core_gate_ownership; core_gate_linux
            ;;
        families) ubq_run_families ;;
        *)
            echo "pre-push gates: unknown tier '$TIER' (want lint|quick|ctest|linux|full|families)" >&2
            exit 1
            ;;
    esac
}
