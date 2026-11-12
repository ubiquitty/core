#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Builds and installs the Core/Kit package, then consumes only its relocated
# install prefix. The nondefault library directory and current umbrella/header
# assertions cover package topology independently of source-tree compilation.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=Scripts/build-jobs.sh
. "$ROOT/Scripts/build-jobs.sh"
BUILD_JOBS="$(ubq_build_jobs)"
WORK="$(mktemp -d -t ubq-core-pkg.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
PREFIX="$WORK/prefix"
LIBDIR=lib/ubiquitty-package-test

echo "== build + install libUbiquittyCore to a prefix =="
cmake -S "$ROOT" -B "$WORK/build" -DUBIQUITTY_FRONTEND=Core \
    -DCMAKE_INSTALL_LIBDIR="$LIBDIR" \
    -DCMAKE_INSTALL_INCLUDEDIR=include -DCMAKE_INSTALL_DOCDIR=share/doc/Ubiquitty \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/build" --target ubiquitty-core ubiquitty-kit --parallel "$BUILD_JOBS" >/dev/null
cmake --install "$WORK/build" >/dev/null

echo "== assert the installed package layout =="
for config in UbiquittyCoreConfig.cmake UbiquittyCoreTargets.cmake UbiquittyCoreConfigVersion.cmake; do
    test -f "$PREFIX/$LIBDIR/cmake/UbiquittyCore/$config" || { echo "FAIL: no $config"; exit 1; }
done
ARCHIVES=()
for target in ubiquitty-core ubiquitty-core-model ubiquitty-behaviour ubiquitty-kit; do
    archive="$PREFIX/$LIBDIR/lib$target.a"
    test -f "$archive" || { echo "FAIL: no $target archive"; exit 1; }
    ARCHIVES+=("$archive")
done
HEADER_ROOTS=("$PREFIX"/include/UbiquittyCore-*)
if [ "${#HEADER_ROOTS[@]}" -ne 1 ] || [ ! -d "${HEADER_ROOTS[0]}" ]; then
    echo "FAIL: expected one versioned public include directory"
    exit 1
fi
for header in UbiquittyCore/UbiquittyCore.h UbiquittyKit/UbiquittyKit.h \
        Core/Terminal.h Core/Session/RemoteSessionCore.h Base/Types.h PAL/PAL.h Forward.h; do
    test -f "${HEADER_ROOTS[0]}/$header" || { echo "FAIL: no installed $header"; exit 1; }
done
for private_directory in Platform ThirdParty; do
    test ! -e "${HEADER_ROOTS[0]}/$private_directory" || { echo "FAIL: published $private_directory implementation headers"; exit 1; }
done
python3 "$ROOT/Scripts/license_notices.py" --source "$ROOT" --list > "$WORK/notices.txt"
while IFS= read -r notice; do
    cmp "$ROOT/$notice" "$PREFIX/share/doc/Ubiquitty/$notice" \
        || { echo "FAIL: $notice missing or changed after installation"; exit 1; }
done < "$WORK/notices.txt"

echo "== link graph: no QUIC/TLS dependency in the installed archives =="
if ! nm ${ARCHIVES[@]+"${ARCHIVES[@]}"} >"$WORK/symbols.txt" 2>"$WORK/nm-errors.txt"; then
    cat "$WORK/nm-errors.txt" >&2
    echo "FAIL: could not inspect the installed archives" >&2
    exit 1
fi
if grep -iE ' U .*(ngtcp2|wolfssl|aws_lc_|SSL_new|quiche_)' "$WORK/symbols.txt"; then
    echo "FAIL: QUIC/TLS undefined symbol in the installed package" >&2
    exit 1
fi

echo "== consume via find_package against a relocated prefix ONLY =="
mv "$PREFIX" "$WORK/relocated"
PREFIX="$WORK/relocated"
# Do not let an ambient source-header search path mask a missing installed file.
unset CPATH CPLUS_INCLUDE_PATH
cmake -S "$ROOT/Samples/find-package-consumer" -B "$WORK/consumer" \
    -DUbiquittyCore_DIR="$PREFIX/$LIBDIR/cmake/UbiquittyCore" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF >/dev/null
cmake --build "$WORK/consumer" --parallel "$BUILD_JOBS" >/dev/null
"$WORK/consumer/package-consumer"

echo "== public Core and Kit version guards against the relocated prefix =="
mkdir "$WORK/version-probes"
cat > "$WORK/version-probes/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.13)
project(ubiquitty-core-version-probes LANGUAGES CXX)
find_package(UbiquittyCore REQUIRED)
foreach(probe IN ITEMS compatible equal too-new kit-compatible kit-too-new)
    add_library(version-${probe} OBJECT ${probe}.cpp)
    if(probe MATCHES "^kit-")
        target_link_libraries(version-${probe} PRIVATE Ubiquitty::ubiquitty-kit)
    else()
        target_link_libraries(version-${probe} PRIVATE Ubiquitty::ubiquitty-core)
    endif()
    set_target_properties(version-${probe} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(version-${probe} PRIVATE -fno-exceptions -fno-rtti)
endforeach()
CMAKE
cat > "$WORK/version-probes/compatible.cpp" <<'CPP'
#define UBIQUITTY_CORE_REQUIRE 0x010300
#include "UbiquittyCore/UbiquittyCore.h"
static_assert(UBIQUITTY_CORE_VERSION_HEX >= UBIQUITTY_CORE_REQUIRE);
CPP
cat > "$WORK/version-probes/equal.cpp" <<'CPP'
#define UBIQUITTY_CORE_REQUIRE UBIQUITTY_CORE_VERSION_HEX
#include "UbiquittyCore/UbiquittyCore.h"
static_assert(UBIQUITTY_CORE_VERSION_HEX == UBIQUITTY_CORE_REQUIRE);
CPP
cat > "$WORK/version-probes/too-new.cpp" <<'CPP'
#define UBIQUITTY_CORE_REQUIRE (UBIQUITTY_CORE_VERSION_HEX + 1)
#include "UbiquittyCore/UbiquittyCore.h"
CPP
cat > "$WORK/version-probes/kit-compatible.cpp" <<'CPP'
#include "UbiquittyKit/UbiquittyKit.h"
UBIQUITTY_KIT_REQUIRE(2, 0, 0);
CPP
cat > "$WORK/version-probes/kit-too-new.cpp" <<'CPP'
#include "UbiquittyKit/UbiquittyKit.h"
UBIQUITTY_KIT_REQUIRE(::UbiquittyKit::kitVersionMajor, ::UbiquittyKit::kitVersionMinor,
    ::UbiquittyKit::kitVersionPatch + 1);
CPP
cmake -S "$WORK/version-probes" -B "$WORK/version-probes-build" \
    -DUbiquittyCore_DIR="$PREFIX/$LIBDIR/cmake/UbiquittyCore" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF >/dev/null
cmake --build "$WORK/version-probes-build" --target version-compatible version-equal version-kit-compatible \
    --parallel "$BUILD_JOBS" >/dev/null
if cmake --build "$WORK/version-probes-build" --target version-too-new \
        >"$WORK/version-too-new.log" 2>&1; then
    echo "FAIL: Core accepted a consumer requiring a newer public API" >&2
    exit 1
fi
if ! grep -Fq "libUbiquittyCore is older than the version this consumer requires" "$WORK/version-too-new.log"; then
    cat "$WORK/version-too-new.log" >&2
    echo "FAIL: the newer-version probe failed for a reason other than the public guard" >&2
    exit 1
fi
if cmake --build "$WORK/version-probes-build" --target version-kit-too-new \
        >"$WORK/version-kit-too-new.log" 2>&1; then
    echo "FAIL: Kit accepted a consumer requiring a newer public API" >&2
    exit 1
fi
if ! grep -Fq "UbiquittyKit is older than the version this consumer requires" "$WORK/version-kit-too-new.log"; then
    cat "$WORK/version-kit-too-new.log" >&2
    echo "FAIL: the newer-version probe failed for a reason other than the Kit guard" >&2
    exit 1
fi

echo "package test: PASS — relocated Core/Kit package, current headers, consumer runtime, version guard, no QUIC/TLS"
