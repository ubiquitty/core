#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Agent/completions_smoke.sh — validate the shipped shell completions.
#
# No daemon needed: this checks the completion SCRIPTS, not a live session.
#   1. The static files (Resources/shell-integration/ubq.{bash,zsh})
#      parse with `bash -n` / `zsh -n`.
#   2. They mention every ubq verb (so a new verb can't silently
#      ship without a completion).
#   3. `ubq completions bash|zsh` emits non-empty, parseable output
#      that is byte-identical to the static file (single source of truth), and
#      an unknown shell name is rejected (exit 2).
#
# Env:
#   UBQ_BIN   path to the built ubq (else under UBQ_BUILD_ROOT)
#
# Exit 0 iff all checks pass. `zsh -n` checks are skipped (not failed) when zsh
# is unavailable on the runner.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
AGENT="${UBQ_BIN:-$UBQ_BUILD_ROOT/agent/ubq}"
SI="$ROOT/Resources/shell-integration"
BASH_FILE="$SI/ubq.bash"
ZSH_FILE="$SI/ubq.zsh"

fail() { echo "completions-smoke: FAIL: $*" >&2; exit 1; }
pass() { echo "completions-smoke: ok: $*"; }

[ -x "$AGENT" ]   || fail "ubq not built at $AGENT (run Scripts/build-agent.sh)"
[ -f "$BASH_FILE" ] || fail "missing $BASH_FILE"
[ -f "$ZSH_FILE" ]  || fail "missing $ZSH_FILE"

# Verbs the CLI dispatches (Source/Agent/main.cpp). Keep in sync with main().
VERBS=(ls new run send read split kill label status wait events api mcp \
       completions selftest imagetest)

# --- 1. static files parse -------------------------------------------------
bash -n "$BASH_FILE" || fail "bash -n rejected $BASH_FILE"
pass "bash -n $BASH_FILE"

if command -v zsh >/dev/null 2>&1; then
    zsh -n "$ZSH_FILE" || fail "zsh -n rejected $ZSH_FILE"
    pass "zsh -n $ZSH_FILE"
else
    echo "completions-smoke: SKIP zsh -n (zsh not installed)"
fi

# --- 2. every verb is mentioned in both scripts ---------------------------
for verb in "${VERBS[@]}"; do
    grep -qw -- "$verb" "$BASH_FILE" || fail "bash completion never mentions verb '$verb'"
    grep -qw -- "$verb" "$ZSH_FILE"  || fail "zsh completion never mentions verb '$verb'"
done
pass "all ${#VERBS[@]} verbs present in both completion scripts"

# --- 3. `completions` subcommand emits valid, identical output ------------
# Compare via process substitution (not "$(...)", which strips trailing
# newlines and would mask a real divergence). The emit is byte-exact.
for shell in bash zsh; do
    "$AGENT" completions "$shell" >/dev/null || fail "completions $shell exited non-zero"
    bytes="$("$AGENT" completions "$shell" | wc -c)"
    [ "$bytes" -gt 0 ] || fail "completions $shell produced empty output"
    diff <("$AGENT" completions "$shell") "$SI/ubq.$shell" >/dev/null \
        || fail "completions $shell output differs from the shipped $shell file"
done
pass "completions bash|zsh emit non-empty output identical to the static files"

"$AGENT" completions bash | bash -n - || fail "emitted bash completion fails bash -n"
pass "emitted bash completion parses (bash -n)"
if command -v zsh >/dev/null 2>&1; then
    "$AGENT" completions zsh | zsh -n - || fail "emitted zsh completion fails zsh -n"
    pass "emitted zsh completion parses (zsh -n)"
fi

# Unknown shell name must be rejected with the usage exit code.
if "$AGENT" completions fish >/dev/null 2>&1; then
    fail "completions accepted an unknown shell ('fish')"
fi
pass "completions rejects an unknown shell name"

# --- 4. the scripts complete, installed as the manual says -------------------
# bash: `eval "$(ubq completions bash)"`, which works in the bash 3.2 macOS
# ships, where `source <(...)` reads nothing. The completion function is called
# as bash calls it for a line, and must offer the verb the line begins.
COMPLETED="$(/bin/bash -c 'eval "$("$1" completions bash)"; COMP_WORDS=(ubq re); COMP_CWORD=1; COMPREPLY=(); _ubq; printf "%s " "${COMPREPLY[@]}"; COMP_WORDS=(ubq api list --surface s); COMP_CWORD=4; COMPREPLY=(); _ubq; printf "%s " "${COMPREPLY[@]}"' _ "$AGENT")"
[ "$COMPLETED" = "read sync " ] || fail "bash, installed with eval, completes 'ubq re' and '--surface s' as '$COMPLETED', not 'read sync '"
pass "bash completion installed with eval completes a verb and a surface"

# zsh: `ubq completions zsh > <fpath dir>/_ubq`, then compinit. compinit reads
# #compdef from a file's first line only, and the file runs as `_ubq`'s first
# call, so the first Tab must complete, for ubiquitty-host as well as ubq. An
# interactive zsh under zpty is given a line, a Tab, and a key that prints
# what the line became.
[ "$(head -1 "$ZSH_FILE")" = "#compdef ubq ubiquitty-host" ] || fail "the zsh completion's first line is not its #compdef tag"
if command -v zsh >/dev/null 2>&1 && zsh -fc 'zmodload zsh/zpty' 2>/dev/null; then
    ZSH_HOME="$(mktemp -d "/tmp/ubq-zsh-completion.XXXXXX")"
    mkdir -p "$ZSH_HOME/functions"
    "$AGENT" completions zsh >"$ZSH_HOME/functions/_ubq"
    COMPLETED="$(zsh -f -c '
zmodload zsh/zpty
zpty shell "env -i HOME=$1 PATH=/usr/bin:/bin TERM=dumb zsh -fi"
zpty -w shell "PS1=; fpath=($1/functions \$fpath); autoload -U compinit; compinit -u -d $1/zcompdump; zstyle \":completion:*\" menu no; shown() { print -r -- \"BUF<\$BUFFER>\"; BUFFER=; }; zle -N shown; bindkey \"^G\" shown; print READY"
out=; while zpty -r -t shell chunk; do out+=$chunk; [[ $out == *READY* ]] && break; done
for line in "ubq re" "ubiquitty-host tok" "ubq comp"; do
    zpty -w -n shell "$line"$'"'"'\t\a'"'"'
    out=
    for attempt in {1..100}; do
        zpty -r -t shell chunk && out+=$chunk
        [[ $out == *"BUF<"[^\$]* && ${out##*BUF<} == *">"* ]] && break
        sleep 0.1
    done
    buffer=${out##*BUF<}
    print -rn -- "${buffer%%>*}|"
done
zpty -d shell' _ "$ZSH_HOME")"
    rm -rf "$ZSH_HOME"
    [ "$COMPLETED" = "ubq read |ubiquitty-host token |ubq completions |" ] || fail "zsh, installed on fpath, completes the first Tab as '$COMPLETED'"
    pass "zsh completion installed on fpath completes from the first Tab, for ubq and ubiquitty-host"
else
    echo "completions-smoke: SKIP the zsh Tab check (zsh or its zpty module is not installed)"
fi

echo "completions-smoke: PASS"
