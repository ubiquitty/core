#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Validates cached QUIC archives before delegating only an incomplete or
# platform-incompatible prefix to the pinned dependency builder.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_NAMESPACE GIT_COMMON_DIR \
    GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES

CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
    shift
fi
PREFIX="${1:-$UBQ_BUILD_ROOT/quic-deps}"
SDK="${2:-}"
DEPLOYMENT_TARGET="${3:-}"
FETCH_SCRIPT="$ROOT/Scripts/fetch-quic-deps.sh"
REQUIRED_ARCHIVES=(
    libngtcp2.a
    libngtcp2_crypto_boringssl.a
    libssl.a
    libcrypto.a
)

if [ -n "$SDK" ] && [ -z "$DEPLOYMENT_TARGET" ]; then
    echo "quic-deps: a cross-SDK cache requires its deployment target" >&2
    exit 2
fi

EFFECTIVE_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
EXPECTED_PLATFORM=""
EXPECTED_SDK_VERSION=""
if [ "$(uname -s)" = "Darwin" ]; then
    requested_sdk="${SDK:-macosx}"
    case "$requested_sdk" in
        macosx) EXPECTED_PLATFORM="MACOS" ;;
        iphoneos) EXPECTED_PLATFORM="IOS" ;;
        iphonesimulator) EXPECTED_PLATFORM="IOSSIMULATOR" ;;
        *) echo "quic-deps: unsupported Apple SDK $requested_sdk" >&2; exit 2 ;;
    esac
    if [ -z "$EFFECTIVE_DEPLOYMENT_TARGET" ]; then
        EFFECTIVE_DEPLOYMENT_TARGET="$(sed -n 's/^cxx=//p' \
            "$ROOT/Scripts/macos-deployment-target.txt")"
    fi
    EXPECTED_SDK_VERSION="$(xcrun --sdk "$requested_sdk" --show-sdk-version)"
fi

archives_complete()
{
    local archive
    for archive in "${REQUIRED_ARCHIVES[@]}"; do
        [ -f "$PREFIX/lib/$archive" ] || return 1
    done
    [ -f "$PREFIX/include/ngtcp2/ngtcp2.h" ] || return 1
    [ -f "$PREFIX/include/openssl/ssl.h" ] || return 1
}

first_object()
{
    ar t "$1" 2>/dev/null | sed -n '/\.o$/{p;q;}'
}

archive_is_native()
{
    local archive="$1"
    local member header
    member="$(first_object "$archive")"
    [ -n "$member" ] || return 1
    header="$(ar p "$archive" "$member" 2>/dev/null | od -An -tx1 -N20 | tr -d ' \n')"
    case "$(uname -s):$(uname -m)" in
        Darwin:arm64) [ "${header:0:16}" = "cffaedfe0c000001" ] ;;
        Darwin:x86_64) [ "${header:0:16}" = "cffaedfe07000001" ] ;;
        Linux:aarch64 | Linux:arm64)
            [ "${header:0:8}" = "7f454c46" ] && [ "${header:36:4}" = "b700" ] ;;
        Linux:x86_64)
            [ "${header:0:8}" = "7f454c46" ] && [ "${header:36:4}" = "3e00" ] ;;
        *) return 1 ;;
    esac
}

archive_matches_apple_target()
{
    local archive="$1"
    local member metadata platform minimum sdk_version temporary
    member="$(first_object "$archive")"
    [ -n "$member" ] || return 1
    temporary="$(mktemp "${TMPDIR:-/tmp}/ubiquitty-quic-object.XXXXXX")"
    if ! ar p "$archive" "$member" >"$temporary"; then
        rm -f "$temporary"
        return 1
    fi
    if ! metadata="$(xcrun vtool -show-build "$temporary" 2>/dev/null)"; then
        rm -f "$temporary"
        return 1
    fi
    rm -f "$temporary"
    platform="$(printf '%s\n' "$metadata" | sed -n 's/^ *platform //p' | sed -n '1p')"
    minimum="$(printf '%s\n' "$metadata" | sed -n 's/^ *minos //p' | sed -n '1p')"
    sdk_version="$(printf '%s\n' "$metadata" | sed -n 's/^ *sdk //p' | sed -n '1p')"
    [ "$platform" = "$EXPECTED_PLATFORM" ] \
        && [ "$minimum" = "$EFFECTIVE_DEPLOYMENT_TARGET" ] \
        && [ "$sdk_version" = "$EXPECTED_SDK_VERSION" ]
}

# Whether an archive names the prefix it was built under. fetch-quic-deps.sh
# maps that path away; an archive that still holds it was built without the map
# and puts this machine's build root into every binary that links it.
archive_holds_build_path()
{
    LC_ALL=C grep -a -F -q "$PREFIX/" "$1"
}

archives_match_request()
{
    archives_complete || return 1
    local archive
    for archive in "${REQUIRED_ARCHIVES[@]}"; do
        archive_is_native "$PREFIX/lib/$archive" || return 1
        ! archive_holds_build_path "$PREFIX/lib/$archive" || return 1
        if [ "$(uname -s)" = "Darwin" ]; then
            archive_matches_apple_target "$PREFIX/lib/$archive" || return 1
        fi
    done
}

if archives_match_request; then
    [ "$CHECK_ONLY" -eq 1 ] \
        || echo "quic-deps: cached archives match the requested SDK at $PREFIX"
    exit 0
fi

[ "$CHECK_ONLY" -eq 0 ] || exit 1

echo "quic-deps: replacing an incomplete, SDK-incompatible or path-carrying archive set"
for archive in "${REQUIRED_ARCHIVES[@]}"; do
    rm -f "$PREFIX/lib/$archive"
done

"$FETCH_SCRIPT" "$PREFIX" "$SDK" "$DEPLOYMENT_TARGET"
if ! archives_match_request; then
    echo "quic-deps: dependency builder did not produce the requested archive set" >&2
    exit 1
fi
