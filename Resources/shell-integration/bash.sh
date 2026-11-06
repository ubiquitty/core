# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# shellcheck shell=bash
# Ubiquitty bash shell integration — OSC 7 location reports and OSC 133
# marks.
#
# Opt-in. Add to ~/.bashrc:
#   [[ -n "$UBIQUITTY_INTEGRATION_DIR" ]] \
#       && source "$UBIQUITTY_INTEGRATION_DIR/bash.sh"
#
# On a REMOTE host, copy this file across and source it unconditionally:
#   source ~/.ubiquitty/bash.sh
# ssh forwards TERM_PROGRAM only when both ends configure SendEnv and AcceptEnv,
# so a guard on it switches the integration off exactly where the location
# report is the only way to learn which machine a session is on.

# Once per shell: sourcing twice would double every report.
[[ -n "$UBIQUITTY_SHELL_INTEGRATION_DONE" ]] && return 0
UBIQUITTY_SHELL_INTEGRATION_DONE=1

# Neither half is gated on the terminal being Ubiquitty. OSC 133 is read by
# kitty, WezTerm, VS Code, iTerm2 and Windows Terminal as well, and a terminal
# that knows neither sequence ignores both — so a guard would buy nothing and
# cost the marks in every other terminal. It would also switch them off over
# ssh, since TERM_PROGRAM is not forwarded unless both ends configure SendEnv
# and AcceptEnv, and a long remote session is where jumping between prompts
# earns the most.

# The machine this shell is on, resolved once. A prompt runs the report on every
# command, and forking `hostname` each time would be paid on all of them.
__ubiquitty_host="${HOSTNAME:-$(hostname 2>/dev/null)}"

# Percent-encodes a path for a file:// URL.
#
# Almost every path is already safe byte for byte, so that is tested whole
# before any per-byte work.
__ubiquitty_urlencode() {
    local LC_ALL=C
    local text="$1"
    if [[ "$text" != *[^A-Za-z0-9/._~-]* ]]; then
        printf '%s' "$text"
        return
    fi
    local out="" index character ordinal escaped
    for (( index = 0; index < ${#text}; index++ )); do
        character="${text:index:1}"
        case "$character" in
            [A-Za-z0-9/._~-]) out+="$character" ;;
            *)
                printf -v ordinal '%d' "'$character"
                printf -v escaped '%%%02X' "$((ordinal & 255))"
                out+="$escaped"
                ;;
        esac
    done
    printf '%s' "$out"
}

# OSC 7 — the working directory together with the machine it is on, as
# file://host/path.
#
# The host is the half nothing else can supply. A terminal's process tree holds
# one ssh however many the reader has opened, so a terminal looking at `ssh bastion`
# cannot see the `ssh example-host` beyond it; the innermost shell naming itself here is
# the only thing that can. It is also what lets a proxy icon stand for a path on
# another machine rather than being dropped as unresolvable.
#
# OSC 1337 RemoteHost states the same fact in iTerm2's spelling, for a reader
# whose terminal takes that one. Both halves are known already, so it is free.
__ubiquitty_report_location() {
    printf '\e]7;file://%s%s\a' "$__ubiquitty_host" "$(__ubiquitty_urlencode "$PWD")"
    printf '\e]1337;RemoteHost=%s@%s\a' "${USER:-$LOGNAME}" "$__ubiquitty_host"
}

# The DEBUG trap fires before every simple command, and a line holds many: a
# pipeline, a list, a loop body. The C mark opens the line's output, so it is
# written once, before the line's first command, and only once the prompt has
# been drawn; a mark before each command would move the start of the output to
# the last one, and `run`, `tail` and Select Last Command Output would return
# that command's output alone. PROMPT_COMMAND's own commands run disarmed.
__ubiquitty_armed=
__ubiquitty_preexec() {
    [[ -n "$COMP_LINE" ]] && return                # completion, not a command
    [[ -z "$__ubiquitty_armed" ]] && return
    # An empty line runs nothing, and the next prompt's report is no command.
    [[ "$BASH_COMMAND" == __ubiquitty_precmd ]] && return
    __ubiquitty_armed=
    printf '\e]133;C\a'
}

__ubiquitty_precmd() {
    local exit_code=$?
    __ubiquitty_armed=
    printf '\e]133;D;%s\a' "$exit_code"
    # DECSCUSR 7 restores the terminal's configured cursor.
    printf '\e[7 q'
    printf '\e]133;A\a'
    __ubiquitty_report_location
}

# Last in PROMPT_COMMAND, so the commands before it never open a line.
__ubiquitty_arm() {
    __ubiquitty_armed=1
}

trap '__ubiquitty_preexec' DEBUG
# Chain rather than overwrite — preserve a user's PROMPT_COMMAND.
PROMPT_COMMAND="__ubiquitty_precmd${PROMPT_COMMAND:+; $PROMPT_COMMAND}; __ubiquitty_arm"
