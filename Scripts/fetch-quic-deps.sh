#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# CMake runs this file through execute_process, which execs it directly with no
# shell. So the shebang has to stay on line 1: a `#!` below it is a comment, the
# kernel answers ENOEXEC, and nothing here retries under /bin/sh the way an
# interactive shell or a calling script would. Anything prepended goes below.
#
# Bound the compile fan-out. `cmake --build -j` with no number hands ninja its
# default of cores + 2, and several checkouts may build on one machine at
# once, so the unbounded form asks for far more processes than there are cores
# and the machine stops responding. UBQ_BUILD_JOBS overrides.
BUILD_JOBS="${UBQ_BUILD_JOBS:-$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) )}"

#
# fetch-quic-deps.sh — build the QUIC stack (ngtcp2 + AWS-LC) the daemon's QUIC
# transport links against, into a prefix the CMake `-DUBIQUITTY_QUIC=ON` build
# finds. Pinned to exact COMMITS, verified after checkout: a tag is a mutable ref,
# and this source is compiled into the notarised app and the release host binary,
# so whoever can move a tag upstream chooses what ships. The tags are kept beside
# the commits because a reader needs to know which release a commit is. Idempotent
# (skips when the libs are already present and reuses cached source clones when
# rebuilding). Run before
# configuring with UBIQUITTY_QUIC on:
#
#   ./Scripts/fetch-quic-deps.sh            # → <build root>/quic-deps
#   ./Scripts/fetch-quic-deps.sh <prefix>   # → custom prefix
#
# AWS-LC is the TLS crypto backend; ngtcp2's boringssl crypto backend natively
# detects it (HAVE_AWSLC). AWS-LC needs Go + Perl on PATH to build (it generates
# assembly + tables); both are present on the build hosts.
#
set -euo pipefail

# Every git command below names a dependency clone with `git -C`, and `-C` only
# sets the working directory — an inherited GIT_DIR skips discovery entirely and
# wins. CMake reaches this script from a configure step, and a configure step is
# reachable from the pre-push hook, where git exports GIT_DIR pointing at the
# pushing worktree. Measured there, in a fresh worktree:
#
#     git -C build/quic-deps/src/aws-lc config --get remote.origin.url
#       -> https://github.com/yaglo/ubiquitty
#     git -C build/quic-deps/src/aws-lc fetch origin refs/tags/v5.1.0:...
#       -> fatal: couldn't find remote ref refs/tags/v5.1.0
#
# The tag is missing because the command is asking the wrong repository. The
# `switch --detach` further down is the one that costs work rather than an error:
# aimed at the pushing worktree, it moves the developer's HEAD.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_NAMESPACE GIT_COMMON_DIR \
    GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"

# The deployment target has one home: a binary built for a newer floor than the
# app's Info.plist declares installs on a machine that cannot launch it.
UBQ_MACOS_MIN="$(sed -n 's/^cxx=//p' "$ROOT/Scripts/macos-deployment-target.txt")"
PREFIX="${1:-$UBQ_BUILD_ROOT/quic-deps}"
WORK="$PREFIX/src"

# Which Apple platform the archives are for. Empty (the default) is the host mac,
# which is what every caller before the iOS arm wanted.
#
# The prefix is the caller's to choose and it must differ per platform: the
# idempotency check below, and ubiquitty_ensure_quic_deps() in
# Scripts/cmake/UbiquittyToolchain.cmake, both ask only whether the four .a
# files EXIST. Neither can tell a macOS slice from an iOS one, so a shared
# prefix would hand an iOS link a macOS archive and the first error that names
# the mismatch would be from ld.
# The cmake to build the dependencies WITH.
#
# Not a bare `cmake`, because the caller that matters most cannot find one: an Xcode
# run-script phase runs with a restricted PATH that excludes /opt/homebrew/bin, so the very
# cmake running the configure is invisible to the script that configure spawns. It works
# from a terminal and fails from Xcode, which is the environment dependence that looks
# fine to whoever writes it.
#
# ubiquitty_ensure_quic_deps() passes ${CMAKE_COMMAND}, which is also more correct than
# finding *a* cmake: the dependencies are then built by the same one as everything else.
UBQ_CMAKE="${UBQ_CMAKE:-cmake}"
command -v "$UBQ_CMAKE" >/dev/null 2>&1 || {
    echo "quic-deps: no cmake at '$UBQ_CMAKE' — pass UBQ_CMAKE, or put cmake on PATH" >&2
    exit 127
}

UBQ_QUIC_SDK="${2:-}"
# The deployment floor, passed in rather than read here. UBIQUITTY_IOS_MIN lives
# once, in Scripts/cmake/UbiquittyPreProject.cmake, and the call that passes it
# (ubiquitty_ensure_quic_deps in Scripts/cmake/UbiquittyToolchain.cmake) says
# so; a copy in this script would be the second reader that comment promises
# does not exist.
UBQ_QUIC_DEPLOYMENT_TARGET="${3:-$UBQ_MACOS_MIN}"

# The cross flags both dependencies take. Empty for the host build, so the
# command lines below are byte-identical to what they were — expanded through the
# `+alternate` form because macOS ships bash 3.2, where an empty array's `[@]`
# counts as unbound under `set -u` and aborts the host build that passes no SDK.
CROSS_FLAGS=()
# Apple's ranlib warns "has no symbols" for every object a library compiled to
# nothing on this platform — AWS-LC carries a dozen (the Windows, threadless
# and FIPS shims), so a mac or iOS build of it prints the line twelve times per
# archive. The recipe Scripts/cmake/UbiquittyToolchain.cmake gives the tree's
# own archives: ar skips the symbol table (S) and ranlib builds it without the
# remark.
ARCHIVE_FLAGS=()
if [ "$(uname -s)" = "Darwin" ]; then
    ARCHIVE_FLAGS=(
        "-DCMAKE_C_ARCHIVE_CREATE=<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>"
        "-DCMAKE_C_ARCHIVE_FINISH=<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>"
        "-DCMAKE_CXX_ARCHIVE_CREATE=<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>"
        "-DCMAKE_CXX_ARCHIVE_FINISH=<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>"
    )
fi
# The prefix the sources and build trees sit under, rewritten to a relative
# name in everything the compilers write. Both libraries pass __FILE__ to their
# error and assertion paths (OPENSSL_PUT_ERROR in AWS-LC), so without it every
# binary linking them carries this machine's build root, and two builds of one
# commit at different paths differ in bytes. One map covers the sources, the
# build trees inside them and the headers installed under include/.
# Scripts/ensure-quic-deps.sh refuses an archive set that still holds the path.
PATH_MAP_FLAGS="-ffile-prefix-map=$PREFIX=quic-deps"

if [ -n "$UBQ_QUIC_SDK" ]; then
    sdk_path="$(xcrun --sdk "$UBQ_QUIC_SDK" --show-sdk-path 2>/dev/null)" || {
        echo "quic-deps: no SDK named $UBQ_QUIC_SDK" >&2; exit 2; }
    # arm64 alone, for the reason the iOS build gives: the simulator runs arm64 on
    # every Mac this tree builds on, and x86_64 would be a slice nobody can run.
    #
    # CMAKE_SYSTEM_NAME is what makes this a cross build rather than a mac build
    # with an odd sysroot. Without it CMake compiles for the host and the archive
    # answers a question about macOS while being named for iOS — the same trap
    # the iOS build documents for the main build.
    CROSS_FLAGS=(
        -DCMAKE_SYSTEM_NAME=iOS
        -DCMAKE_OSX_SYSROOT="$sdk_path"
        -DCMAKE_OSX_ARCHITECTURES=arm64
    )
fi

# The tag names the release; the commit is what is actually required. Resolved
# with `git ls-remote <url> 'refs/tags/<tag>*'` and cross-checked
# against the already-built clones under build/quic-deps/src. ngtcp2's tag is
# annotated, so its commit is the peeled `^{}` ref rather than the tag object.
#
# To move a version: change both lines together. The verification below refuses a
# checkout whose commit does not match, which is what makes the pair a pin rather
# than a note.
AWSLC_TAG="v5.1.0"
AWSLC_COMMIT="6283365b1d43abadfa9b997812cef06b095f7f04"
NGTCP2_TAG="v1.23.0"
NGTCP2_COMMIT="454390edaf6c954a101aa77961a939dd9e338ad2"

if [ -f "$PREFIX/lib/libngtcp2.a" ] \
    && [ -f "$PREFIX/lib/libngtcp2_crypto_boringssl.a" ] \
    && [ -f "$PREFIX/lib/libssl.a" ] \
    && [ -f "$PREFIX/lib/libcrypto.a" ]; then
    echo "quic-deps: already built at $PREFIX (libngtcp2 + crypto_boringssl + AWS-LC)"
    exit 0
fi

if { [ ! -f "$PREFIX/lib/libssl.a" ] || [ ! -f "$PREFIX/lib/libcrypto.a" ]; } && ! command -v go >/dev/null 2>&1; then
    echo "quic-deps: Go is required to build AWS-LC; install Go (macOS: brew install go) and put go on PATH" >&2
    exit 127
fi

mkdir -p "$WORK"

# Fetches the pinned commit, or stops saying what a fresh checkout needs.
#
# A first build downloads both sources once; a git error alone ("unable to
# access", "couldn't find remote ref") surfaced from inside a CMake configure
# reads as a broken tree rather than a missing network or a moved upstream.
fetch_pinned()
{
    local name="$1"
    local directory="$2"
    local want_commit="$3"
    local url
    url="$(git -C "$directory" config --get remote.origin.url)"
    local attempt
    for attempt in 1 2 3; do
        if git -C "$directory" fetch --depth 1 origin "$want_commit"; then
            return 0
        fi
        if [ "$attempt" -lt 3 ]; then
            echo "quic-deps: $name fetch attempt $attempt failed; retrying the pinned commit" >&2
            sleep "$((attempt * 2))"
        fi
    done
    echo "quic-deps: could not fetch $name $want_commit from $url after 3 attempts" >&2
    echo "  A fresh checkout downloads the pinned QUIC sources once, so its first" >&2
    echo "  build needs network access to $url. Retry once it is reachable, or" >&2
    echo "  point UBIQUITTY_QUIC_PREFIX at a prefix that already holds the libraries." >&2
    exit 1
}

checkout_source()
{
    local name="$1"
    local url="$2"
    local tag="$3"
    local directory="$4"
    local want_commit="$5"

    if [ -d "$directory/.git" ]; then
        echo "quic-deps: reusing $name source at $directory"
        # The pinned commit is fetched by its own name. Fetching the tag's ref
        # into a shallow clone stores the annotated tag object the ref names
        # and warns that it is not a commit, on every reuse; and the tag is
        # the mutable half of the pin — the commit below is what was reviewed,
        # so it is what is asked for. The verification after this still
        # compares the two, so a tag that moved is still refused.
        if ! git -C "$directory" rev-parse --verify --quiet "$want_commit^{commit}" >/dev/null; then
            fetch_pinned "$name" "$directory" "$want_commit"
        fi
        git -C "$directory" -c advice.detachedHead=false switch --detach "$want_commit"
    else
        if [ -e "$directory" ]; then
            rm -rf "$directory"
        fi
        # By commit, as the reuse path above fetches it: a shallow clone of an
        # annotated tag's ref stores the tag object and warns that it is not a
        # commit.
        git init -q "$directory"
        git -C "$directory" remote add origin "$url"
        fetch_pinned "$name" "$directory" "$want_commit"
        git -C "$directory" -c advice.detachedHead=false switch --detach "$want_commit"
    fi

    # The pin. Everything above resolves a TAG, and a tag is a mutable ref: the
    # upstream owner can move it, and so can anyone who takes the repository. This
    # is the line that decides whether what was fetched is what was reviewed.
    #
    # Refusing rather than warning, because the caller is a CMake configure step
    # whose next act is to compile these sources into a signed binary — a warning
    # scrolls past and the build succeeds anyway.
    local got_commit
    got_commit="$(git -C "$directory" rev-parse HEAD)"
    if [ "$got_commit" != "$want_commit" ]; then
        echo "quic-deps: $name $tag is not the pinned commit" >&2
        echo "  expected $want_commit" >&2
        echo "  got      $got_commit" >&2
        echo "  The tag has moved, or the source directory is not what it claims." >&2
        echo "  Verify upstream before changing the pin: a moved release tag on a" >&2
        echo "  TLS library is the shape a supply-chain attack takes." >&2
        exit 1
    fi
    echo "quic-deps: $name $tag verified at $got_commit"
}

# ---- AWS-LC (QUIC TLS crypto backend; BoringSSL API) ---------------------
if [ ! -f "$PREFIX/lib/libssl.a" ] || [ ! -f "$PREFIX/lib/libcrypto.a" ]; then
    echo "quic-deps: building AWS-LC $AWSLC_TAG"
    checkout_source "AWS-LC" https://github.com/aws/aws-lc "$AWSLC_TAG" "$WORK/aws-lc" "$AWSLC_COMMIT"
    rm -rf "$WORK/aws-lc/build"
    # Static libs into lib/ (not the multilib lib64/ GNUInstallDirs picks on
    # Fedora/RHEL) so the CMake build (ubiquitty_ensure_quic_deps and
    # ubiquitty_link_quic_archives under Scripts/cmake) and the idempotency
    # check find them.
    # FIPS=OFF: the non-FIPS module; FIPS pins a heavier validated build we don't
    # need. AWS-LC ships the BoringSSL QUIC TLS API ngtcp2's boringssl backend uses.
    "$UBQ_CMAKE" -S "$WORK/aws-lc" -B "$WORK/aws-lc/build" \
        -DCMAKE_BUILD_TYPE=Release \
        ${CROSS_FLAGS[@]+"${CROSS_FLAGS[@]}"} \
        ${ARCHIVE_FLAGS[@]+"${ARCHIVE_FLAGS[@]}"} \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$UBQ_QUIC_DEPLOYMENT_TARGET" \
        -DCMAKE_C_FLAGS="$PATH_MAP_FLAGS" \
        -DCMAKE_CXX_FLAGS="$PATH_MAP_FLAGS" \
        -DCMAKE_ASM_FLAGS="$PATH_MAP_FLAGS" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_TOOL=OFF \
        -DFIPS=OFF
    "$UBQ_CMAKE" --build "$WORK/aws-lc/build" -j"$BUILD_JOBS"
    "$UBQ_CMAKE" --install "$WORK/aws-lc/build" --prefix "$PREFIX"
fi

# ---- ngtcp2 (+ its boringssl crypto backend, built against AWS-LC) -----------
if [ ! -f "$PREFIX/lib/libngtcp2.a" ] \
    || [ ! -f "$PREFIX/lib/libngtcp2_crypto_boringssl.a" ]; then
    echo "quic-deps: building ngtcp2 $NGTCP2_TAG (AWS-LC / boringssl backend)"
    checkout_source "ngtcp2" https://github.com/ngtcp2/ngtcp2 "$NGTCP2_TAG" "$WORK/ngtcp2" "$NGTCP2_COMMIT"
    rm -rf "$WORK/ngtcp2/build"
    # Static-link order matters: libssl before libcrypto. ngtcp2 checks the lib
    # for AWS-LC (HAVE_AWSLC), then builds libngtcp2_crypto_boringssl.
    #
    # ENABLE_LIB_ONLY: the libraries and their crypto backend, no examples. Left
    # off, ngtcp2 looks for libev, nghttp3 and brotli wherever the machine
    # keeps them and builds its example clients when it finds them, so what a
    # fresh checkout compiled depended on what else the machine had installed.
    "$UBQ_CMAKE" -S "$WORK/ngtcp2" -B "$WORK/ngtcp2/build" \
        -DCMAKE_BUILD_TYPE=Release \
        ${CROSS_FLAGS[@]+"${CROSS_FLAGS[@]}"} \
        ${ARCHIVE_FLAGS[@]+"${ARCHIVE_FLAGS[@]}"} \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$UBQ_QUIC_DEPLOYMENT_TARGET" \
        -DCMAKE_C_FLAGS="$PATH_MAP_FLAGS" \
        -DCMAKE_CXX_FLAGS="$PATH_MAP_FLAGS" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DENABLE_BORINGSSL=ON \
        -DBORINGSSL_INCLUDE_DIR="$PREFIX/include" \
        -DBORINGSSL_LIBRARIES="$PREFIX/lib/libssl.a;$PREFIX/lib/libcrypto.a" \
        -DENABLE_OPENSSL=OFF \
        -DENABLE_GNUTLS=OFF \
        -DENABLE_STATIC_LIB=ON \
        -DENABLE_SHARED_LIB=OFF \
        -DENABLE_LIB_ONLY=ON \
        -DBUILD_TESTING=OFF
    "$UBQ_CMAKE" --build "$WORK/ngtcp2/build" -j"$BUILD_JOBS"
    "$UBQ_CMAKE" --install "$WORK/ngtcp2/build" --prefix "$PREFIX"
fi

required_libraries=(
    libngtcp2_crypto_boringssl.a
    libngtcp2.a
    libssl.a
    libcrypto.a
)
for required_library in "${required_libraries[@]}"; do
    if [ ! -f "$PREFIX/lib/$required_library" ]; then
        echo "quic-deps: missing $PREFIX/lib/$required_library after build" >&2
        exit 1
    fi
done

echo "quic-deps: ready at $PREFIX"
ls -1 "$PREFIX/lib"/libngtcp2*.a "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a"
