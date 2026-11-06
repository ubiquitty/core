#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# check-terminfo.sh — the terminfo entry compiles and names what ships.
#
# `Source/Core/Terminfo.cpp` carries the xterm-ubiquitty
# entry as a C string literal, and nothing between that literal and a user's
# `infocmp xterm-ubiquitty` checks it: a typo fails only when the app next
# installs it, and a capability that lands without its terminfo line is one an
# application gating on terminfo never uses. This compiles the entry with
# `tic -x`, reads it back with `infocmp -x`, and requires the capabilities the
# terminal advertises to be present.
#
# Usage:
#   ./Scripts/check-terminfo.sh              # gate, exit 1 on a violation
#   ./Scripts/check-terminfo.sh --verbose    # also print the infocmp excerpt
#   ./Scripts/check-terminfo.sh --self-test  # the gate reddens on an entry
#                                            # missing a required capability

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERBOSE=0
SELF_TEST=0
case "${1:-}" in
--verbose) VERBOSE=1 ;;
--self-test) SELF_TEST=1 ;;
esac

# The system tic first: it writes the legacy database format the host ncurses
# reads (UbiquittyTerminfo.cpp:findTic makes the same choice at install time).
find_tic() {
    local candidate
    for candidate in /usr/bin/tic /usr/local/bin/tic /home/linuxbrew/.linuxbrew/opt/ncurses/bin/tic; do
        if [[ -x "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    if command -v tic >/dev/null 2>&1; then
        command -v tic
        return 0
    fi
    return 1
}

TIC="$(find_tic || true)"
if [[ -z "${TIC:-}" ]]; then
    echo "check-terminfo: SKIPPED — tic not found (install ncurses)" >&2
    exit 0
fi

SOURCE_CPP="$ROOT/Source/Core/Terminfo.cpp"
if [[ ! -f "$SOURCE_CPP" ]]; then
    echo "check-terminfo: FAIL — $SOURCE_CPP not found" >&2
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ubiquitty-terminfo.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# The gate against the entry it ships, and against that entry with one
# required capability removed: a rule that stops matching prints the same ok
# as a clean entry, so the planted defect is what makes the ok evidence.
if [[ $SELF_TEST -eq 1 ]]; then
    ok=0
    if ! "$0" >/dev/null 2>&1; then
        echo "self-test: the shipped entry failed the gate" >&2
        ok=1
    fi
    PLANTED="$WORK/Planted.cpp"
    sed 's/E3=\\\\E\[3J, //' "$SOURCE_CPP" >"$PLANTED"
    if grep -q 'E3=' "$PLANTED"; then
        echo "self-test: the fixture still carries E3, the plant did not take" >&2
        ok=1
    elif UBIQUITTY_TERMINFO_SOURCE="$PLANTED" "$0" >/dev/null 2>&1; then
        echo "self-test: an entry without E3 passed the gate" >&2
        ok=1
    fi
    [[ $ok -eq 0 ]] && echo "check-terminfo: self-test ok (the shipped entry passes, an entry missing E3 fails)"
    exit "$ok"
fi
SOURCE_CPP="${UBIQUITTY_TERMINFO_SOURCE:-$SOURCE_CPP}"

SOURCE_FILE="$WORK/xterm-ubiquitty.src"
DATABASE_DIR="$WORK/tinfo"
mkdir -p "$DATABASE_DIR"

# Unescape the C string literal the way the compiler does, so the file tic sees
# is the file the app installs. Only the escapes the literal uses are handled;
# `\E` stays as the two characters terminfo spells ESC with.
python3 - "$SOURCE_CPP" "$SOURCE_FILE" <<'PY'
import re
import sys

source_cpp, out = sys.argv[1], sys.argv[2]
text = open(source_cpp, encoding="utf-8").read()
match = re.search(r'const char source\[\] =', text)
if not match:
    print(f"check-terminfo: FAIL — source[] not found in {source_cpp}", file=sys.stderr)
    sys.exit(1)
tail = text[match.end():]
parts = []
current = []
in_string = False
escaped = False
i = 0
while i < len(tail):
    c = tail[i]
    if in_string:
        if escaped:
            current.append({'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'"}.get(c, '\\' + c))
            escaped = False
        elif c == '\\':
            escaped = True
        elif c == '"':
            in_string = False
            parts.append(''.join(current))
            current = []
        else:
            current.append(c)
    elif c == '"':
        in_string = True
        current = []
    elif c == ';':
        break
    elif c == '/' and tail[i + 1:i + 2] == '/':
        while i < len(tail) and tail[i] != '\n':
            i += 1
        continue
    i += 1

with open(out, 'w', encoding='utf-8') as f:
    f.write(''.join(parts))
PY

if [[ ! -s "$SOURCE_FILE" ]]; then
    echo "check-terminfo: FAIL — extracted source is empty" >&2
    exit 1
fi

if ! "$TIC" -x -o "$DATABASE_DIR" "$SOURCE_FILE" 2>"$WORK/tic.err"; then
    echo "check-terminfo: FAIL — tic -x refused the entry" >&2
    cat "$WORK/tic.err" >&2
    exit 1
fi

if ! infocmp -x -A "$DATABASE_DIR" xterm-ubiquitty >"$WORK/infocmp.txt" 2>"$WORK/infocmp.err"; then
    echo "check-terminfo: FAIL — infocmp -x could not read the compiled entry" >&2
    cat "$WORK/infocmp.err" >&2
    exit 1
fi

# One capability per entry, matched as a whole terminfo field (a name followed
# by `=`, `#` or a separator), so `Tc` cannot be satisfied by `Tc` inside another
# field's value.
has_capability() {
    grep -Eq "(^|[[:space:],])$1([=#,]|$)" "$WORK/infocmp.txt"
}

required=(
    "E3"    # ED 3 erases scrollback (CSI 3 J)
    "hpa"   # CSI Ps `
    "vpa"   # CSI Ps d
    "rep"   # CSI Ps b
    "Ms"    # OSC 52 clipboard
    "Smulx" # styled underline, SGR 4:0-5
    "Sync"  # synchronized output, mode 2026
    "Tc"    # truecolor
    "AX"    # default colours through SGR 39/49
    "smcup" # alternate screen (DA1 18)
    "rmcup" # alternate screen (DA1 18)
    "csr"   # scroll region (DA1 21/28)
    "colors"
    "pairs"
    "smam"  # DECAWM
    "XR"    # XTVERSION
    "kf63"  # the modified function keys reach the end of the pcfkeys set
    "kUP7"  # and the modified navigation keys
)

fail=0
if ! grep -q "^xterm-ubiquitty" "$WORK/infocmp.txt"; then
    echo "check-terminfo: FAIL — infocmp did not read back xterm-ubiquitty" >&2
    fail=1
fi
for capability in "${required[@]}"; do
    if ! has_capability "$capability"; then
        echo "check-terminfo: FAIL — infocmp shows no $capability capability" >&2
        fail=1
    fi
done

if [[ $fail -ne 0 ]]; then
    echo "--- infocmp excerpt ---" >&2
    head -n 40 "$WORK/infocmp.txt" >&2
    exit 1
fi

echo "check-terminfo: ok — tic -x compiled the entry, infocmp reads it back, every required capability present"
if [[ $VERBOSE -eq 1 ]]; then
    head -n 30 "$WORK/infocmp.txt" >&2
fi
