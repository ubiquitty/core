#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# pgo-flags.sh — who may be handed the training profile, decided in one place.
#
# Sourced, not run. `ubiquitty_pgo_flags <root> <compiler>` echoes the flags to
# add, or nothing, and says on stderr which it did and why.
#
# The profile in Support/pgo is LLVM's IR-instrumentation format, and only clang
# reads it. GCC's -fprofile-use takes a DIRECTORY of .gcda files, so handed a
# .profdata path it resolves `<profile-path>/<object>.gcda`, finds nothing, and
# COMPILES ANYWAY -- exit 0, one -Wmissing-profile warning inside a build log
# that already scrolls past. The binary is a plain -O3 build that every status
# line above it called profile-guided. That is the failure this file exists to
# prevent: not a build that fails, but one that succeeds while quietly dropping
# the thing it advertised.
#
# So the consumer is established before the flag is handed over, by asking the
# compiler what it is rather than by reading its name -- a launcher (ccache,
# distcc), a versioned symlink, or a $CXX carrying flags all defeat a name test,
# and the repo's other compiler probes are written the same way for the same
# reason. __clang__ is the exact question: the format is LLVM's, so the reader
# must be clang.
#
# UBIQUITTY_PGO=OFF disables it everywhere. UBIQUITTY_PGO=REQUIRE turns a
# compiler that cannot consume the profile into a build failure, which is what a
# release that means to ship a profiled binary wants -- silence is the whole
# hazard, and a release is where it costs the most.
#
# UBQ_PGO_PROFILE picks the profile, as it does for run-bench.sh and
# build-perf-bench.sh: an A/B that swaps the tracked artifact in and out of
# Support/pgo/ loses an arm to the next branch switch.
#
#   ./Scripts/pgo-flags.sh --self-test

# True when the compiler reads LLVM's profile format.
#
# Probes for __clang__ rather than matching a name. A compiler that cannot be
# run at all answers no, which is the safe direction: the flag is withheld and
# the reason is printed, instead of a silent -O3 build wearing a PGO label.
ubiquitty_compiler_reads_llvm_profile() {
    local compiler="$1"
    # shellcheck disable=SC2086
    # Read the whole macro stream: grep -q can close it early and make a
    # valid compiler fail with SIGPIPE under the builders' pipefail setting.
    $compiler -dM -E -x c++ /dev/null 2>/dev/null | grep '__clang__' >/dev/null
}

# Select by the compiler's target rather than the machine running this script:
# a cross-compiler on Linux must not give an ARM or Apple build x86 counters.
ubiquitty_default_pgo_profile() {
    local root="$1" compiler="$2" macros system="unknown" processor="unknown"
    # shellcheck disable=SC2086
    macros="$($compiler -dM -E -x c++ /dev/null 2>/dev/null)" || macros=""
    case "$macros" in *'#define __linux__ '*) system="Linux" ;; esac
    case "$macros" in *'#define __x86_64__ '*) processor="x86_64" ;; esac
    case "$macros" in *'#define __aarch64__ '*) processor="aarch64" ;; esac
    python3 "$root/Scripts/pgo_localize.py" --default-profile "$root" "$system" "$processor"
}

# Consumer overrides do not change the producer's default output destination.
ubiquitty_pgo_profile() {
    if [ -n "${UBQ_PGO_PROFILE:-}" ]; then
        printf '%s\n' "$UBQ_PGO_PROFILE"
        return 0
    fi
    ubiquitty_default_pgo_profile "$1" "$2"
}

# Echoes the compiler's complaint about this profile, or nothing when it reads it.
#
# Being clang is necessary and not sufficient. An indexed profile carries the
# format version of the llvm-profdata that wrote it, and a clang older than that
# refuses the file with "unsupported instrumentation profile format version" on
# every translation unit -- a build that fails on each file and names nothing a
# reader can act on. One empty translation unit compiled with the flag asks the
# compiler itself, which is the same question Scripts/cmake/ToolchainFloor.cmake
# puts for the CMake builds. Returns non-zero when the compiler refused.
ubiquitty_compiler_profile_refusal() {
    local compiler="$1" profile="$2" output
    # shellcheck disable=SC2086
    if output="$($compiler -x c++ -c /dev/null -o /dev/null "-fprofile-use=$profile" \
            -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin 2>&1)"; then
        return 0
    fi
    printf '%s\n' "$output" | grep -m1 -i 'profile' || printf '%s\n' "$output" | head -n1
    return 1
}

# Echoes the -fprofile-use flag set for this compiler, or nothing.
#
# The stamp rides along as an inert define because ninja rebuilds an object when
# its command line changes, and -fprofile-use names only the profile's PATH:
# after a retrain the cached objects still carry the old counts, and ThinLTO
# then refuses to link the mix on conflicting ProfileSummary module flags.
ubiquitty_pgo_flags() {
    local root="$1" compiler="$2"

    if [ "${UBIQUITTY_PGO:-ON}" = "OFF" ]; then
        echo "PGO:        off (UBIQUITTY_PGO=OFF)" >&2
        return 0
    fi
    local profile
    profile="$(ubiquitty_pgo_profile "$root" "$compiler")" || return 1
    if [ ! -f "$profile" ]; then
        echo "PGO:        no profile at $profile — building without one" >&2
        [ "${UBIQUITTY_PGO:-ON}" != REQUIRE ] || return 1
        return 0
    fi
    if ! ubiquitty_compiler_reads_llvm_profile "$compiler"; then
        echo "PGO:        NOT APPLIED — '$compiler' does not read LLVM's profile format." >&2
        echo "            Support/pgo carries a .profdata (clang, IR instrumentation); GCC's" >&2
        echo "            -fprofile-use wants a directory of .gcda and would compile clean" >&2
        echo "            without it. This build is plain -O3, and says so rather than" >&2
        echo "            claiming a profile it did not use." >&2
        if [ "${UBIQUITTY_PGO:-ON}" = "REQUIRE" ]; then
            echo "PGO:        UBIQUITTY_PGO=REQUIRE — refusing to build unprofiled." >&2
            return 1
        fi
        return 0
    fi
    local toolchain_refusal
    if ! toolchain_refusal="$(python3 "$root/Scripts/pgo_toolchain.py" verify "$profile" "$compiler" 2>&1)"; then
        echo "PGO:        NOT APPLIED — $toolchain_refusal" >&2
        [ "${UBIQUITTY_PGO:-ON}" != REQUIRE ] || return 1
        return 0
    fi
    local refusal
    if ! refusal="$(ubiquitty_compiler_profile_refusal "$compiler" "$profile")"; then
        echo "PGO:        NOT APPLIED — '$compiler' cannot read $profile:" >&2
        echo "            ${refusal:-the probe compile failed}" >&2
        echo "            This build is plain -O3. A clang at least as new as the LLVM that" >&2
        echo "            wrote the profile applies it." >&2
        if [ "${UBIQUITTY_PGO:-ON}" = "REQUIRE" ]; then
            echo "PGO:        UBIQUITTY_PGO=REQUIRE — refusing to build unprofiled." >&2
            return 1
        fi
        return 0
    fi

    local stamp rooted cache
    stamp="$(shasum -a 256 "$profile" | cut -c1-16)"
    # The copy keyed for this checkout's absolute compile paths, so internal-
    # linkage functions find their counters; Scripts/pgo_localize.py says why.
    cache="${UBQ_BUILD_ROOT:-$(python3 "$root/Scripts/build_root.py" "$root")}/pgo"
    rooted="$(python3 "$root/Scripts/pgo_localize.py" "$profile" "$root" "$cache")" || rooted="$profile"
    echo "PGO:        using $profile as $rooted (UBIQUITTY_PGO=OFF to disable)" >&2
    printf '%s' "-fprofile-use=$rooted -DUBIQUITTY_PGO_STAMP=$stamp -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin"
}

# The rule is shown to catch its case and to leave the near-miss alone.
#
# Each stand-in answers the probes the way the real compiler it models does --
# the point is the decision, and a fixture that hardcoded the answer would test
# nothing. `oldclang` is a clang older than the LLVM that wrote the profile: it
# says __clang__ and refuses the file.
if [ "${1:-}" = "--self-test" ]; then
    set -uo pipefail
    _pgoSelfTestRoot="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    _pgoSelfTestFixture="$(mktemp -d)"
    trap 'rm -rf "$_pgoSelfTestFixture"' EXIT
    _pgoSelfTestStatus=0

    mkdir -p "$_pgoSelfTestFixture/Support/pgo" "$_pgoSelfTestFixture/Scripts"
    cp "$_pgoSelfTestRoot/Scripts/pgo_localize.py" "$_pgoSelfTestFixture/Scripts/"
    cp "$_pgoSelfTestRoot/Scripts/pgo_toolchain.py" "$_pgoSelfTestFixture/Scripts/"
    printf 'not a real profile\n' > "$_pgoSelfTestFixture/Support/pgo/portable-core.profdata"

    printf '#!/bin/sh\necho "#define __clang__ 1"\n' > "$_pgoSelfTestFixture/clanglike"
    printf '#!/bin/sh\necho "#define __GNUC__ 15"\n' > "$_pgoSelfTestFixture/gcclike"
    printf '#!/bin/sh\nexit 127\n' > "$_pgoSelfTestFixture/broken"
    cat > "$_pgoSelfTestFixture/oldclang" <<'OLDCLANG'
#!/bin/sh
for argument in "$@"; do
    case "$argument" in
        -fprofile-use=*)
            echo "error: Error in reading profile: unsupported instrumentation profile format version" >&2
            exit 1
            ;;
    esac
done
echo "#define __clang__ 1"
OLDCLANG
    chmod +x "$_pgoSelfTestFixture/clanglike" "$_pgoSelfTestFixture/gcclike" "$_pgoSelfTestFixture/broken" \
        "$_pgoSelfTestFixture/oldclang"

    _pgoSelfTestFlags="$(UBIQUITTY_PGO=ON ubiquitty_pgo_flags "$_pgoSelfTestFixture" "$_pgoSelfTestFixture/clanglike" 2>/dev/null)"
    case "$_pgoSelfTestFlags" in
        *-fprofile-use=*) ;;
        *) echo "self-test: a clang consumer was not given the profile" >&2; _pgoSelfTestStatus=1 ;;
    esac

    for _pgoSelfTestCompiler in gcclike broken oldclang; do
        _pgoSelfTestFlags="$(UBIQUITTY_PGO=ON ubiquitty_pgo_flags "$_pgoSelfTestFixture" "$_pgoSelfTestFixture/$_pgoSelfTestCompiler" 2>/dev/null)"
        if [ -n "$_pgoSelfTestFlags" ]; then
            echo "self-test: $_pgoSelfTestCompiler was handed a profile it cannot read: $_pgoSelfTestFlags" >&2
            _pgoSelfTestStatus=1
        fi
        if UBIQUITTY_PGO=REQUIRE ubiquitty_pgo_flags "$_pgoSelfTestFixture" "$_pgoSelfTestFixture/$_pgoSelfTestCompiler" >/dev/null 2>&1; then
            echo "self-test: REQUIRE did not fail the build for $_pgoSelfTestCompiler" >&2
            _pgoSelfTestStatus=1
        fi
    done

    _pgoSelfTestFlags="$(UBIQUITTY_PGO=OFF ubiquitty_pgo_flags "$_pgoSelfTestFixture" "$_pgoSelfTestFixture/clanglike" 2>/dev/null)"
    if [ -n "$_pgoSelfTestFlags" ]; then
        echo "self-test: UBIQUITTY_PGO=OFF still produced flags: $_pgoSelfTestFlags" >&2
        _pgoSelfTestStatus=1
    fi

    if [ "$_pgoSelfTestStatus" = 0 ]; then
        echo "pgo-flags: self-test ok (a clang consumer is given the profile; a GCC, an unrunnable, or a too-old clang is not, and REQUIRE fails)"
    fi
    exit "$_pgoSelfTestStatus"
fi
