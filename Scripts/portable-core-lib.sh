#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# portable-core-lib.sh — shared helper: build the libUbiquittyCore static
# archive from the canonical source list (Scripts/portable-core-sources.txt).
#
# Sourced by the bash perf drivers (run-bench.sh / run-profile.sh) so they
# DERIVE their portable-Core link set from the one canonical list the CMake
# `ubiquitty-core` target also reads — the link list can never drift again.
# A driver links its own test main + a tiny PAL stub
# (the system-services half: ubiquitty_core_pal_stub_source) against the
# archive; the static archive pulls only the TUs the driver references, so
# the superset list costs nothing it doesn't use.
#
# Usage (from a driver, after setting ROOT + CXX + an -O level):
#   source "$ROOT/Scripts/portable-core-lib.sh"
#   build_portable_core_archive "$BUILD" "-O3 $(ubiquitty_lto_flag) -DNDEBUG"   # -> $PORTABLE_CORE_ARCHIVE (shipping flags)
#   ubiquitty_core_pal_stub_source "$BUILD/pal_stub.cpp"
#   "$CXX" ... main.cpp "$BUILD/pal_stub.cpp" "$PORTABLE_CORE_ARCHIVE" -o out
#

# build_portable_core_archive <build-dir> <extra-cxxflags>
#   Compiles the canonical list (+ the embedded built-in-themes TU Themes.cpp
#   needs) into <build-dir>/portable-core-obj/ and archives them into
#   <build-dir>/libubiquitty-core.a. Sets PORTABLE_CORE_ARCHIVE to that path.
#   Honors $CXX (default c++) and $ROOT (repo root).
# The LTO spelling this compiler accepts: clang's ThinLTO where it exists,
# monolithic -flto elsewhere (g++, the Linux container's and the Linux
# runner's only compiler). Probed by compiling a translation unit that has a
# function in it: g++ accepts -flto=thin under -fsyntax-only and on an empty
# unit, and refuses it only once there is something to stream, so a cheaper
# probe answers yes and the real build then fails. Honors $CXX and
# $UBQ_COMPILER_LAUNCHER as build_portable_core_archive does.
ubiquitty_lto_flag() {
    local launcher="${UBQ_COMPILER_LAUNCHER-}"
    local cxx="${CXX:-c++}"
    local probe
    probe="$(mktemp "${TMPDIR:-/tmp}/ubq-lto-probe.XXXXXX")"
    # Unquoted launcher, so an empty one contributes no argument.
    # shellcheck disable=SC2086
    if printf 'int ubiquittyLtoProbe() { return 1; }\n' \
        | $launcher "$cxx" -x c++ -flto=thin -c -o "$probe" - >/dev/null 2>&1; then
        printf '%s' "-flto=thin"
    # GCC: =auto hands the link-time compilation every core the jobserver or
    # the machine offers. Bare -flto ran it serially and said so on every
    # link — "using serial compilation of 33 LTRANS jobs".
    elif printf 'int ubiquittyLtoProbe() { return 1; }\n' \
        | $launcher "$cxx" -x c++ -flto=auto -c -o "$probe" - >/dev/null 2>&1; then
        printf '%s' "-flto=auto"
    else
        printf '%s' "-flto"
    fi
    rm -f "$probe"
}

build_portable_core_archive() {
    local build="$1"; shift
    local extra="${1:-}"
    # Unquoted below, so an empty launcher contributes no argument.
    local launcher="${UBQ_COMPILER_LAUNCHER-}"
    local cxx="${CXX:-c++}"
    # The C translation unit shares the C++ flags, so it must come from the
    # same compiler family: GCC refuses clang's -flto=thin and -fprofile-*.
    local cc="${CC:-}"
    if [[ -z "$cc" ]]; then
        case "$(basename "$cxx")" in
            clang++*) cc="$(dirname "$cxx")/clang${cxx##*clang++}" ;;
            g++*) cc="$(dirname "$cxx")/gcc${cxx##*g++}" ;;
            *) cc="cc" ;;
        esac
        [[ "$cc" == ./* && "$cxx" != ./* ]] && cc="${cc#./}"
    fi
    local root="${ROOT:?ROOT must be set before sourcing portable-core-lib.sh}"
    local objdir="$build/portable-core-obj"
    mkdir -p "$objdir"

    # Core includes simdutf without a guard — Codec/Base64.cpp for both
    # directions of base64, Unicode/UTF8Decoder.cpp for the multibyte fast path
    # — so this archive does not compile without the include path, and every
    # multibyte parse lane measures the decoder the app ships.
    local simdutf_dir="$root/Source/ThirdParty/simdutf"
    extra="$extra -I$simdutf_dir"

    # ScrollSearch.cpp compiles against the vendored RE2 (same include-root
    # convention as the app targets).
    local re2_dir="$root/Source/ThirdParty/re2"
    extra="$extra -I$re2_dir"

    # Themes.cpp parses the embedded built-in [[themes]] from a generated TU.
    local themes_gen="$objdir/BuiltinThemesData.gen.cpp"
    python3 "$root/Scripts/embed-builtin-themes.py" \
        "$root/Source/Core/Themes/builtin-themes.toml" "$themes_gen"

    # Incremental: each TU emits a depfile, and a TU recompiles only when its
    # object is older than its source or any header the depfile names. The
    # flags stamp keeps it sound across callers — a different flag set in the
    # same build dir (pgo-train's instrumented pass, a sanitizer sweep) wipes
    # the objects rather than mixing two configurations in one archive.
    # UBQ_CORE_ARCHIVE_FULL=1 forces the historical full rebuild.
    local flagsStamp="$objdir/.flags"
    # The stamp carries the profile's CONTENT, not just its path: a retrain
    # leaves -fprofile-use=<same path> in the flag string while every counter
    # behind it changes, and objects from two profiles in one archive abort
    # ThinLTO on conflicting ProfileSummary metadata.
    local profileArgument profileStamp=""
    for profileArgument in $extra; do
        case "$profileArgument" in
            -fprofile-use=*)
                profileStamp="$(shasum -a 256 "${profileArgument#-fprofile-use=}" 2>/dev/null | cut -d' ' -f1)"
                ;;
        esac
    done
    if [ "${UBQ_CORE_ARCHIVE_FULL:-}" = "1" ] || [ ! -f "$flagsStamp" ] || [ "$(cat "$flagsStamp")" != "$cxx $extra $profileStamp" ]; then
        rm -f "$objdir"/*.o "$objdir"/*.d
        printf '%s' "$cxx $extra $profileStamp" > "$flagsStamp"
    fi
    local staleList
    staleList="$("$root/Scripts/gen-portable-core-sources.sh" --print | python3 -c "
import os
import sys
objdir = sys.argv[1]
root = sys.argv[2]
for rel in sys.stdin.read().split():
    obj = os.path.join(objdir, rel.replace('/', '_') + '.o')
    dep = obj + '.d'
    if not (os.path.exists(obj) and os.path.exists(dep)):
        print(rel)
        continue
    objTime = os.path.getmtime(obj)
    words = open(dep).read().replace('\\\\\n', ' ').split()
    stale = False
    for word in words[1:]:
        try:
            if os.path.getmtime(word if os.path.isabs(word) else os.path.join(root, word)) > objTime:
                stale = True
                break
        except OSError:
            stale = True
            break
    if stale:
        print(rel)
" "$objdir" "$root")"
    local objs=()
    local rel obj
    while IFS= read -r rel; do
        obj="$objdir/$(echo "$rel" | tr '/' '_').o"
        # A here-string, not a pipe: grep -q closes its input on the first
        # match, and a printf feeding a pipe then takes EPIPE, which pipefail
        # reports as "not stale" — so a long stale list left objects unbuilt.
        if grep -qxF "$rel" <<<"$staleList"; then
            # shellcheck disable=SC2086
            $launcher "$cxx" -std=c++23 $extra -fno-exceptions -fno-rtti -pthread \
                -MMD -MF "$obj.d" -I"$root/Source" -c "$root/$rel" -o "$obj"
        fi
        objs+=("$obj")
    done < <("$root/Scripts/gen-portable-core-sources.sh" --print)

    # shellcheck disable=SC2086
    $launcher "$cxx" -std=c++23 $extra -fno-exceptions -fno-rtti \
        -I"$root/Source" -c "$themes_gen" -o "$objdir/themes_gen.o"
    objs+=("$objdir/themes_gen.o")

    # simdutf amalgamation TU — UTF8Decoder.o and Base64.o both reference it
    # (mirrors ub_add_thirdparty_source in the app target; third-party TU, so
    # no -Wall/-Wextra).
    # shellcheck disable=SC2086
    $launcher "$cxx" -std=c++23 $extra -fno-exceptions -fno-rtti -w \
        -c "$simdutf_dir/simdutf.cpp" -o "$objdir/simdutf.o"
    objs+=("$objdir/simdutf.o")

    # RE2 TUs — ScrollSearch.o resolves against these (third-party TUs, so
    # no -Wall/-Wextra, exceptions/RTTI at the language defaults).
    local re2_src
    for re2_src in "$re2_dir"/re2/*.cc "$re2_dir/util/rune.cc" "$re2_dir/util/strutil.cc"; do
        obj="$objdir/re2_$(basename "$re2_src").o"
        # shellcheck disable=SC2086
        $launcher "$cxx" -std=c++23 $extra -pthread -w -I"$re2_dir" \
            -c "$re2_src" -o "$obj"
        objs+=("$obj")
    done

    # yyjson TU — Codec/JSON.o resolves against it. It is C and wants
    # -std=c99, so the C compiler builds it (third-party TU, so no -Wall/-Wextra).
    # shellcheck disable=SC2086
    $launcher "$cc" -std=c99 $extra -w \
        -c "$root/Source/ThirdParty/yyjson/yyjson.c" -o "$objdir/yyjson.o"
    objs+=("$objdir/yyjson.o")

    PORTABLE_CORE_ARCHIVE="$build/libubiquitty-core.a"
    rm -f "$PORTABLE_CORE_ARCHIVE"
    # stderr stays visible: an archive that fails to write (a full disk, a
    # missing object) has to stop the build here, not surface later as a
    # driver with nothing to link.
    ar rcs "$PORTABLE_CORE_ARCHIVE" "${objs[@]}"
}

# ubiquitty_core_pal_stub_source <out.cpp>
#   Writes a trivial PAL "system services" backend — the half the bundled
#   POSIX backend (in the archive) does not provide. A headless perf driver
#   needs no real clock / clipboard / bell, so these are no-ops.
# The include set every benchmark compiles under, emitted one flag per line.
#
# One definition, because the gate that says the benchmarks compile and the
# script that builds one to run it have to ask the same question. Held apart,
# check-bench-compiles.sh passed on `-ISource -ITests` while run-bench.sh
# carried `-ISource` alone and could not compile bench_stack at all: a gate
# proving compilability under flags no runner uses is a green light for a build
# nobody can produce, and it reports the failure as absent rather than as
# unchecked.
ubiquitty_bench_include_flags() {
    local root="$1"
    printf '%s\n' -I"$root/Source" -I"$root/Tests" -I"$root/Source/ThirdParty/simdutf"
}

# `--with-pty-stream` leaves openPTY out: a driver that links the real
# Source/Platform/POSIX/PTYStream.cpp beside this stub — the benches, whose
# drain lane launches a flood child through the production path — would
# otherwise carry two definitions of it, and the linker refuses the pair.
ubiquitty_core_pal_stub_source() { # <path> [--with-pty-stream]
    local withPTYStream=0
    [ "${2:-}" = "--with-pty-stream" ] && withPTYStream=1
    cat > "$1" <<'STUB'
// Auto-generated by Scripts/portable-core-lib.sh — the PAL system-services
// half a headless libUbiquittyCore consumer must supply (no-op for benches).
#include "PAL/PAL.h"
#include "PAL/Stream.h"
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <string_view>
namespace Ubiquitty {
namespace PAL {
// Real clocks, not constants: a zero monotonicMicros() turns any
// wait-until-elapsed loop over it into a side-effect-free infinite loop,
// which is UB the optimizer deletes — a bench built that way measures a
// different program than the app ships (and time-gated paths like the
// foreground-probe interval degenerate).
uint32_t monotonicMicros()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint32_t)(ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL);
}
Ticks monotonicTicks() { return Ticks { monotonicMicros() / 16667 }; } // ~60 Hz
void diagWrite(std::string_view) { }
void bellRing(BellMode) { }
void clipboardSetText(std::string_view) { }
int clipboardGetText(char*, int) { return -1; }
int preferencesLoad(PreferencesBlobKey, void*, int) { return -1; }
int preferencesSaveAtomic(PreferencesBlobKey, std::span<const std::byte>) { return 0; }
STUB
    if [ "$withPTYStream" -eq 0 ]; then
        cat >> "$1" <<'STUB'
// No child process: a headless consumer drives the model from bytes it already
// has. Failure is the honest answer, and the contract's one for it.
std::unique_ptr<Stream> openPTY(std::span<const std::string>, const PTYLaunchOptions&) { return nullptr; }
STUB
    fi
    cat >> "$1" <<'STUB'
} // namespace PAL
} // namespace Ubiquitty
STUB
}
