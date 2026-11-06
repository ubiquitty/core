#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# check-core-subset-paths.sh — every path a target trims must name a real SSOT entry.
#
# ubiquitty_core_subset() already rejects a name absent from
# Scripts/portable-core-sources.txt, but only while CMake configures the target
# that calls it. A target nobody configures locally — ubiquitty-host is the
# standing example — carries a stale path indefinitely, and every other gate
# stays green because none of them read CMakeLists' per-target lists.
#
# Scoped to the lists a subset is cut by, the arguments of ubiquitty_core_subset()
# and the EXCLUDE list of ubiquitty_add_core_ios_library(), rather than every
# Source/ literal in the file: a target may legitimately add a TU that sits
# outside the SSOT, and only the subset lists are required to draw from it.
#
#   check-core-subset-paths.sh [--cmake <file>]... [--canonical <list>]
#
# --cmake names a CMake file holding subset calls, the checkout's CMakeLists.txt
# when none is named; --canonical names the SSOT, this checkout's
# Scripts/portable-core-sources.txt by default. A project that builds Core from
# another checkout names its own CMake files and that checkout's list.
#

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_FILES=()
SSOT="$ROOT/Scripts/portable-core-sources.txt"
while [ $# -gt 0 ]; do
    case "$1" in
        --cmake) CMAKE_FILES+=("$2"); shift 2 ;;
        --canonical) SSOT="$2"; shift 2 ;;
        *) echo "usage: check-core-subset-paths.sh [--cmake <file>]... [--canonical <list>]" >&2; exit 2 ;;
    esac
done
[ "${#CMAKE_FILES[@]}" -gt 0 ] || CMAKE_FILES=("$ROOT/CMakeLists.txt")

[ -f "$SSOT" ] || { echo "check-core-subset-paths: missing $SSOT" >&2; exit 1; }

python3 - "$ROOT" "$SSOT" "${CMAKE_FILES[@]}" <<'PY'
import os, re, sys

root, ssot_path, cmake_files = sys.argv[1], sys.argv[2], sys.argv[3:]

canonical = {
    line.strip() for line in open(ssot_path)
    if line.strip() and not line.lstrip().startswith('#')
}

# A mention of the helper in prose is not a call. Two comments name it above the
# definitions, and counting them would read six where four targets call it --
# the number this file's own refusal below is reasoned about. Worse, they alone
# would keep that refusal quiet: rename the helper and the four real calls
# stop matching while the two comments still do, so `calls` is non-empty, the
# guard passes, and the gate walks two empty bodies and certifies every stale
# path in the real lists.
#
# Blanked rather than removed so every offset still lines up: the body extraction
# and the line numbers below both index into the original text.
def codeOnly(source):
    """The text with comment bodies blanked, one line at a time.

    - Parameter source: the CMakeLists text.
    - Returns: the same text and the same length, comments replaced by spaces.
    """
    out = []
    for line in source.split('\n'):
        comment = line.find('#')
        out.append(line if comment == -1 else line[:comment] + ' ' * (len(line) - comment))
    return '\n'.join(out)


# Each subset call, matched to its closing paren. The body is scanned line by
# line so a path inside a comment is not mistaken for an argument.
SUBSET_CALL = re.compile(r'\b(?:ubiquitty_core_subset|ubiquitty_add_core_ios_library)\s*\(')
calls = []
for cmakelists in cmake_files:
    text = open(cmakelists).read()
    for match in SUBSET_CALL.finditer(codeOnly(text)):
        depth, index = 1, match.end()
        while depth and index < len(text):
            depth += (text[index] == '(') - (text[index] == ')')
            index += 1
        line = text.count('\n', 0, match.start()) + 1
        name = cmakelists[len(root) + 1:] if cmakelists.startswith(root + os.sep) else cmakelists
        calls.append((name, line, text[match.end():index - 1]))

# The whole check is scoped to ubiquitty_core_subset() arguments, so a zero-call
# population is not a clean tree — it is the helper name having drifted out from
# under the regex. CMakeLists always calls it (four standing targets do), and a
# gate that walked zero lists would certify every stale path in them as ok, which
# is the exact silence this check exists to close. Refuse rather than pass.
if not calls:
    print("check-core-subset-paths: REFUSED — no subset call in " + ", ".join(cmake_files),
          file=sys.stderr)
    print("  The helper was renamed, or its calls moved, and this gate walked zero subset",
          file=sys.stderr)
    print("  lists — a stale path in any of them would have read as ok. Re-point",
          file=sys.stderr)
    print("  SUBSET_CALL to the new name, or --cmake to the file the calls moved to.", file=sys.stderr)
    sys.exit(2)

failures = []
for name, start_line, body in calls:
    for offset, raw in enumerate(body.split('\n')):
        code = raw.split('#', 1)[0].strip()
        for token in re.findall(r'\bSource/[A-Za-z0-9_./-]+\.(?:cpp|mm|c)\b', code):
            if token not in canonical:
                failures.append((name, start_line + offset, token))

if failures:
    print("=== ubiquitty_core_subset paths must exist in the canonical SSOT ===")
    for name, line, token in failures:
        print(f"  {name}:{line}: '{token}' is not in {ssot_path}")
    print()
    print("  A file moved and the subset list kept the old name. Update the path,")
    print("  or drop the entry if the target no longer needs to trim that TU.")
    print(f"check-core-subset-paths: {len(failures)} stale path(s).")
    sys.exit(1)

print(f"check-core-subset-paths: ok ({len(calls)} subset call(s) checked)")
PY
