# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# shellcheck-skip: zsh dialect — shellcheck implements sh, bash, dash and ksh,
# so there is no shell directive that describes this file.
# Ubiquitty zsh shell integration — OSC 7 location reports and OSC 133
# marks.
#
# Opt-in. Add to ~/.zshrc:
#   [[ -n "$UBIQUITTY_INTEGRATION_DIR" ]] \
#       && source "$UBIQUITTY_INTEGRATION_DIR/zsh.sh"
#
# On a REMOTE host, copy this file across and source it unconditionally:
#   source ~/.ubiquitty/zsh.sh
# ssh forwards TERM_PROGRAM only when both ends configure SendEnv and AcceptEnv,
# so a guard on it switches the integration off exactly where the location
# report is the only way to learn which machine a session is on.

# Once per shell: sourcing twice would double every report.
[[ -n "$UBIQUITTY_SHELL_INTEGRATION_DONE" ]] && return 0
typeset -g UBIQUITTY_SHELL_INTEGRATION_DONE=1

# Neither half is gated on the terminal being Ubiquitty. OSC 133 is read by
# kitty, WezTerm, VS Code, iTerm2 and Windows Terminal as well, and a terminal
# that knows neither sequence ignores both — so a guard would buy nothing and
# cost the marks in every other terminal. It would also switch them off over
# ssh, since TERM_PROGRAM is not forwarded unless both ends configure SendEnv
# and AcceptEnv, and a long remote session is where jumping between prompts
# earns the most.

# The machine this shell is on, resolved once. A prompt runs the report on every
# command, and forking `hostname` each time would be paid on all of them.
typeset -g __ubiquitty_host="${HOST:-${HOSTNAME:-$(hostname 2>/dev/null)}}"

# Percent-encodes a path for a file:// URL.
#
# Almost every path is already safe byte for byte, so that is tested whole
# before any per-byte work.
__ubiquitty_urlencode() {
    local LC_ALL=C
    setopt local_options no_multibyte
    local text="$1"
    if [[ "$text" != *[^A-Za-z0-9/._~-]* ]]; then
        printf '%s' "$text"
        return
    fi
    local out="" index character ordinal escaped
    for (( index = 0; index < ${#text}; index++ )); do
        character="${text:$index:1}"
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

# Fired right before zsh draws the prompt.
__ubiquitty_precmd() {
    local exit_code=$?
    # D;<exit> closes the prior command; safe even on the first prompt
    # (exit_code = 0 from the shell's own startup).
    printf '\e]133;D;%s\a' "$exit_code"
    # DECSCUSR 7 restores the terminal's configured cursor.
    printf '\e[7 q'
    printf '\e]133;A\a'
    __ubiquitty_report_location
}

# OSC 133 C — command output start. Fired after the user hits Enter,
# before the command runs.
__ubiquitty_preexec() {
    printf '\e]133;C\a'
}

# Restore the cursor before other prompt hooks customize it.
typeset -ga precmd_functions preexec_functions
precmd_functions=(__ubiquitty_precmd "${precmd_functions[@]}")
preexec_functions+=(__ubiquitty_preexec)
