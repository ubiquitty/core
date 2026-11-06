# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Ubiquitty bash completion for ubq (and ubiquitty-host).
#
# Install (one of):
#   source "$UBIQUITTY_SHELL_INTEGRATION_DIR/ubq.bash"
#   eval "$(ubq completions bash)"
#   cp ubq.bash /usr/local/etc/bash_completion.d/  # or distro path
#
# Completes verbs, flags, and, where it can reach a daemon, live session IDs
# and labels via `ubq ls`.

# The daemon's session IDs and labels. Best-effort: a missing or unreachable
# daemon yields no completions rather than an error.
__ubq_sessions() {
    local ubq="${COMP_WORDS[0]}"
    "$ubq" ls 2>/dev/null | awk -F'\t' 'NF { print $1; if ($2 != "-") print $2 }'
}

__ubq_panes() {
    local ubq="${COMP_WORDS[0]}"
    "$ubq" ls 2>/dev/null | awk -F'\t' 'NF { print $3 }'
}

_ubq() {
    local cur prev
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    local verbs="ls tree new run send read search split kill label tag status wait events api mcp \
completions selftest imagetest"
    local global_flags="-H --host --json --yes"
    local target_flags="-s --session -p --pane"

    if [[ $COMP_CWORD -eq 1 ]]; then
        COMPREPLY=( $(compgen -W "$verbs $global_flags" -- "$cur") )
        return 0
    fi

    local verb="${COMP_WORDS[1]}"

    case "$prev" in
        -s|--session)
            COMPREPLY=( $(compgen -W "here $(__ubq_sessions)" -- "$cur") )
            return 0 ;;
        -p|--pane)
            COMPREPLY=( $(compgen -W "here $(__ubq_panes)" -- "$cur") )
            return 0 ;;
        --for)
            COMPREPLY=( $(compgen -W "exit idle blocked text:" -- "$cur") )
            return 0 ;;
        -H|--host|--label|--timeout|--since|--limit|--scrollback|--from|--to|--columns|--rows|-w|--workspace|-W|--window|-t|--tab)
            return 0 ;;
    esac

    case "$verb" in
        completions)
            COMPREPLY=( $(compgen -W "bash zsh" -- "$cur") )
            return 0 ;;
        api)
            if [[ $COMP_CWORD -eq 2 ]]; then
                COMPREPLY=( $(compgen -W "list describe call validate" -- "$cur") )
            elif [[ "${COMP_WORDS[2]}" == list ]]; then
                if [[ "$prev" == --surface ]]; then
                    COMPREPLY=( $(compgen -W "command-line control mcp sync all" -- "$cur") )
                else
                    COMPREPLY=( $(compgen -W "--surface --json" -- "$cur") )
                fi
            elif [[ $COMP_CWORD -eq 3 ]]; then
                COMPREPLY=( $(compgen -W "$verbs" -- "$cur") )
            fi
            return 0 ;;
        split)
            if [[ "$cur" != -* ]]; then
                COMPREPLY=( $(compgen -W "right down" -- "$cur") )
                return 0
            fi ;;
    esac

    local flags="$global_flags $target_flags"
    case "$verb" in
        ls|tree|mcp|selftest|imagetest) flags="$global_flags" ;;
        label|tag) flags="$flags -w --workspace -W --window -t --tab" ;;
        new) flags="$global_flags --label --columns --rows" ;;
        run) flags="$flags --timeout" ;;
        send) flags="$flags --enter" ;;
        read) flags="$flags --screen --last-command --scrollback" ;;
        search) flags="$flags --from --to --case-sensitive" ;;
        split) flags="$flags --right --down" ;;
        wait) flags="$flags --for --since --timeout" ;;
        events) flags="$flags --since --limit --timeout" ;;
    esac
    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$flags" -- "$cur") )
    fi
    return 0
}

# ubiquitty-host completion: top-level modes + the `token` subcommand.
_ubiquitty_host() {
    local cur prev
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    local modes="--daemon --attach --ensure-daemon --quic-port --quic-version \
--quic-bootstrap --quic-listen --socket --columns --rows --help token"
    local scopes="observe drive control region new all"

    if [[ "${COMP_WORDS[1]}" == "token" ]]; then
        if [[ $COMP_CWORD -eq 2 ]]; then
            COMPREPLY=( $(compgen -W "issue list revoke" -- "$cur") )
            return 0
        fi
        case "$prev" in
            --scope)   COMPREPLY=( $(compgen -W "$scopes" -- "$cur") ); return 0 ;;
            --expires|--label) return 0 ;;
        esac
        if [[ "${COMP_WORDS[2]}" == "issue" && "$cur" == -* ]]; then
            COMPREPLY=( $(compgen -W "--scope --expires --label" -- "$cur") )
        fi
        return 0
    fi

    case "$prev" in
        --columns|--rows|--quic-port|--socket) return 0 ;;
    esac
    COMPREPLY=( $(compgen -W "$modes" -- "$cur") )
    return 0
}

complete -F _ubq ubq
complete -F _ubiquitty_host ubiquitty-host
