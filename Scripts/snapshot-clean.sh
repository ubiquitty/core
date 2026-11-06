#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# Validates a source export from committed HEAD. With --output, writes the
# validated files to a new directory. It never changes branches or contacts a
# remote.
#
#   Scripts/snapshot-clean.sh
#   Scripts/snapshot-clean.sh --output /path/to/new-directory
#
# UBIQUITTY_SNAPSHOT_SKIP_BUILD=1 skips the build smoke for a documentation-only
# check. UBQ_BUILD_JOBS defaults to 1. Release validation runs the build smoke.
set -euo pipefail

unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
    GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE
SRC="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output)
            [ "$#" -ge 2 ] && [ -n "$2" ] || { echo "ERROR: --output needs a new directory" >&2; exit 2; }
            case "$2" in -*) echo "ERROR: --output needs a directory, not an option" >&2; exit 2 ;; esac
            [ -z "$OUTPUT" ] || { echo "ERROR: --output specified twice" >&2; exit 2; }
            OUTPUT="$2"; shift 2 ;;
        --help|-h) sed -n '2,/^set -euo pipefail$/p' "$0" | sed '$d'; exit 0 ;;
        *) echo "ERROR: unsupported option: $1 (export only; no commit or push mode)" >&2; exit 2 ;;
    esac
done
if [ -n "$OUTPUT" ]; then
    [ ! -e "$OUTPUT" ] && [ ! -L "$OUTPUT" ] \
        || { echo "ERROR: output already exists: $OUTPUT" >&2; exit 1; }
    [ -d "$(dirname "$OUTPUT")" ] \
        || { echo "ERROR: output parent must exist" >&2; exit 1; }
fi

MANIFEST=(
    Source Tests Scripts Resources Tools Samples Support cmake
    CMakeLists.txt README.md AGENTS.md LICENSE LICENSES NOTICE TRADEMARKS.md VERSION third-party-notices.md license-scope.json
    .github .gitignore .gitleaks.toml .clang-format .clang-tidy .swift-format .clangd .editorconfig
)
REQUIRED=(CMakeLists.txt README.md LICENSE LICENSES/Apache-2.0.txt NOTICE TRADEMARKS.md VERSION third-party-notices.md license-scope.json)
missing=()
for p in "${MANIFEST[@]}" Docs; do
    git -C "$SRC" cat-file -e "HEAD:$p" 2>/dev/null || missing+=("$p")
done
[ "${#missing[@]}" -eq 0 ] \
    || { echo "ERROR: manifest paths absent from HEAD: ${missing[*]}" >&2; exit 1; }
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/ubiquitty-snapshot.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
git -C "$SRC" archive HEAD "${MANIFEST[@]}" Docs | tar -x -C "$STAGE"

# ---- 3. validate required legal/release files --------------------------------
for f in "${REQUIRED[@]}"; do
    [ -s "$STAGE/$f" ] || { echo "ERROR: required file missing/empty in snapshot: $f" >&2; exit 1; }
done
grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' "$STAGE/VERSION" \
    || { echo "ERROR: VERSION is not x.y.z" >&2; exit 1; }

# ---- 3b. third-party license gate (release mode) on the STAGED tree ----------
# The public cut must ship a complete, pinned license inventory: every redistributed
# dependency inventoried + notice-covered, no moving pins or blocking-risk licenses.
# Run --release from the staged tree so we gate exactly what publishes — this catches
# a license text or inventory row the manifest dropped, not just what's in this repo.
echo "snapshot: third-party license gate (check-licenses --release) ..."
( cd "$STAGE" && ./Scripts/check-licenses.py --release ) \
    || { echo "ERROR: third-party license gate failed on the staged snapshot" >&2; exit 1; }

# ---- 4. validate local links in every staged Markdown file -------------------
# A link the manifest dropped (a legal file left off the manifest) must fail
# here, not ship as a 404. Every document is read, not the README alone:
# CONTRIBUTING, SECURITY, the code of conduct and the notices are what a
# newcomer opens first, and they sit in different directories.
#
# A link is anything GitHub renders as one: an inline link or image, a reference
# definition, and an HTML `href` or `src`. Fenced code, inline code and HTML
# comments render none, so an example link in them is not read. A target
# resolves against the directory of the file that holds it, as GitHub resolves
# it, or against the tree root when it starts with a slash, and a target that
# climbs out of the tree is broken. The anchor is cut before the check, so a link
# into a section of a missing file is still caught; a target with a scheme
# (https:, mailto:) is not a path and is not checked.
python3 - "$STAGE" <<'LINKS' || exit 1
import os
import re
import sys
from urllib.parse import unquote

stage = os.path.realpath(sys.argv[1])
opening_fence = re.compile(r"^\s*(`{3,}|~{3,})")
closing_fence = re.compile(r"^\s*(`{3,}|~{3,})\s*$")
comment = re.compile(r"<!--.*?-->", re.S)
code_span = re.compile(r"(`+).+?\1")
link_patterns = (
    re.compile(r"\]\(\s*(<[^>]*>|[^)\s]+)"),
    re.compile(r"^ {0,3}\[(?!\^)[^\]]+\]:\s*(<[^>]*>|\S+)"),
    re.compile(r"""<(?:a|img)\b[^>]*?\b(?:href|src)\s*=\s*["']([^"']*)["']""", re.I),
)
scheme = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


def rendered_lines(text):
    """The lines GitHub reads links from: no comment, fenced block, or code span."""
    fence = None
    for line in comment.sub("", text).split("\n"):
        if fence:
            closing = closing_fence.match(line)
            if closing and closing.group(1)[0] == fence[0] and len(closing.group(1)) >= len(fence):
                fence = None
            continue
        opening = opening_fence.match(line)
        if opening:
            fence = opening.group(1)
            continue
        yield code_span.sub("", line)


broken = 0
for directory, subdirectories, names in os.walk(stage):
    subdirectories.sort()
    for name in sorted(names):
        if not name.endswith(".md"):
            continue
        path = os.path.join(directory, name)
        with open(path, encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        for line in rendered_lines(text):
            for pattern in link_patterns:
                for link in pattern.findall(line):
                    target = link[1:-1] if link.startswith("<") and link.endswith(">") else link
                    target = unquote(re.split(r"[#?]", target, maxsplit=1)[0])
                    if not target or target.startswith("//") or scheme.match(target):
                        continue
                    if target.startswith("/"):
                        resolved = os.path.normpath(os.path.join(stage, target.lstrip("/")))
                    else:
                        resolved = os.path.normpath(os.path.join(directory, target))
                    inside = resolved == stage or resolved.startswith(stage + os.sep)
                    if not inside or not os.path.exists(resolved):
                        print("BROKEN LINK: %s -> %s" % (os.path.relpath(path, stage), link),
                              file=sys.stderr)
                        broken += 1
if broken:
    print("ERROR: %d broken Markdown link(s) — fix the manifest or the document." % broken,
          file=sys.stderr)
    sys.exit(1)
LINKS

# ---- 5. clean configure + build smoke against the STAGED tree ----------------
# Proves the source export builds standalone and catches a source/header the
# manifest dropped. Core = portable heart, Host = daemon (both headless, quick);
# the native frontend gets a configure-only smoke (a full GUI build is SDK-heavy).
if [ "${UBIQUITTY_SNAPSHOT_SKIP_BUILD:-0}" != 1 ]; then
    command -v cmake >/dev/null || { echo "ERROR: cmake is required for build validation" >&2; exit 1; }
    gen=(); command -v ninja >/dev/null && gen=(-G Ninja)
    # Reuse a pre-built QUIC deps prefix if given (fast CI/dev), else each smoke
    # builds AWS-LC + ngtcp2 from the snapshot (proving fetch-quic-deps ships).
    qp=(); [ -n "${UBIQUITTY_QUIC_PREFIX:-}" ] && qp=(-DUBIQUITTY_QUIC_PREFIX="$UBIQUITTY_QUIC_PREFIX")
    for fe in Core Host; do
        bld="$STAGE/.smoke-$fe"
        echo "snapshot: build-smoke $fe ..."
        cmake -S "$STAGE" -B "$bld" ${gen[@]+"${gen[@]}"} ${qp[@]+"${qp[@]}"} -DUBIQUITTY_FRONTEND="$fe" \
            -DCMAKE_BUILD_TYPE=Release >"$bld.log" 2>&1 \
            || { echo "ERROR: $fe configure failed against the snapshot (see $bld.log)" >&2; exit 1; }
        cmake --build "$bld" --parallel "${UBQ_BUILD_JOBS:-1}" >>"$bld.log" 2>&1 \
            || { echo "ERROR: $fe build failed against the snapshot (see $bld.log)" >&2; exit 1; }
        rm -rf "$bld" "$bld.log"
    done
    fe=$([ "$(uname)" = Darwin ] && echo macOS || echo Linux)
    echo "snapshot: configure-smoke $fe ..."
    cmake -S "$STAGE" -B "$STAGE/.smoke-fe" ${gen[@]+"${gen[@]}"} ${qp[@]+"${qp[@]}"} -DUBIQUITTY_FRONTEND="$fe" \
        >"$STAGE/.smoke-fe.log" 2>&1 \
        || { echo "ERROR: $fe configure failed against the snapshot (see $STAGE/.smoke-fe.log)" >&2; exit 1; }
    rm -rf "$STAGE/.smoke-fe" "$STAGE/.smoke-fe.log"
else
    echo "snapshot: build-smoke SKIPPED (UBIQUITTY_SNAPSHOT_SKIP_BUILD=1)"
fi

# ---- 6. secret + personal-path scans on the STAGED bytes ---------------------
# Don't trust that main's CI ran on exactly what's about to publish: re-scan the
# staged tree itself for secrets and machine-specific personal identifiers.
if command -v gitleaks >/dev/null; then
    echo "snapshot: gitleaks scan ..."
    # -v prints each finding's rule, file and line; --redact replaces the value
    # it matched, so the failure can be read without the log carrying the thing
    # it complains about. Without -v gitleaks reports a count and nothing else,
    # which is the whole of "leaks found: 2". Discarding the output leaves the
    # one job that has gitleaks saying a secret was flagged and naming neither
    # rule nor file nor line, about a stage that is a mktemp tree already
    # deleted by the time anyone reads the message.
    if ! gitleaks_report="$(gitleaks dir "$STAGE" --config "$STAGE/.gitleaks.toml" \
            --no-banner --no-color -v --redact 2>&1)"; then
        echo "ERROR: gitleaks flagged a secret in the staged snapshot" >&2
        # The stage is a temp directory, so its prefix names nothing a reader
        # can open. Strip it and the finding names the file as the repository
        # spells it, which is also how .gitleaks.toml's paths are written.
        # Matched through the mktemp basename rather than through "$STAGE":
        # TMPDIR carries a trailing slash on macOS, so the variable holds a
        # doubled one that the scanner's own output has already collapsed, and
        # a literal comparison silently strips nothing.
        printf '%s\n' "$gitleaks_report" | sed "s#[^ ]*/${STAGE##*/}/##g" >&2
        exit 1
    fi
else
    echo "ERROR: gitleaks is required to validate an export" >&2
    exit 1
fi
# check-personal-paths.py scans a git index, so init a throwaway one in the stage,
# scan --staged, then drop it before the export (.git must never ship).
echo "snapshot: personal-path scan ..."
. "$SRC/Tests/git-fixture.sh"
ubq_make_fixture_repo "$STAGE" || exit 1
git -C "$STAGE" add -A
if ! ( cd "$STAGE" && ./Scripts/check-personal-paths.py --staged ); then
    rm -rf "$STAGE/.git"
    echo "ERROR: personal identifiers found in the staged snapshot" >&2
    exit 1
fi
rm -rf "$STAGE/.git"

echo "snapshot: validated — $(find "$STAGE" -type f | wc -l | tr -d ' ') files staged, all checks passed."

# The destination is created only after validation. mkdir refuses an existing
# path even if another process creates it while validation is running.
if [ -z "$OUTPUT" ]; then
    echo "Dry run complete; no export directory written."
    exit 0
fi
mkdir "$OUTPUT" || { echo "ERROR: cannot create new output: $OUTPUT" >&2; exit 1; }
( cd "$STAGE" && tar -cf - . ) | tar -xf - -C "$OUTPUT"
echo "Export written to $OUTPUT. No commit, push, or repository visibility change performed."
