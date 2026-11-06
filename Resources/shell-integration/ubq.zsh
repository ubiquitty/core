#compdef ubq ubiquitty-host
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Ubiquitty zsh completion for ubq (and ubiquitty-host). compinit reads the
# #compdef tag from a file's first line only, so it stays above the header.
#
# Install (one of):
#   source "$UBIQUITTY_SHELL_INTEGRATION_DIR/ubq.zsh"
#   eval "$(ubq completions zsh)"
#   cp ubq.zsh "${fpath[1]}/_ubq"   # then `compinit`
#
# Completes verbs, flags, and, where it can reach a daemon, live session IDs
# and labels via `ubq ls`.

# Session IDs and labels from the daemon (best-effort; empty if unreachable).
__ubq_sessions() {
    local -a ids
    ids=( here ${(f)"$(ubq ls 2>/dev/null | awk -F'\t' 'NF { print $1; if ($2 != "-") print $2 }')"} )
    _describe -t sessions 'session' ids
}

__ubq_panes() {
    local -a ids
    ids=( here ${(f)"$(ubq ls 2>/dev/null | awk -F'\t' 'NF { print $3 }')"} )
    _describe -t panes 'pane' ids
}

_ubq() {
    local context state state_descr line
    typeset -A opt_args

    local -a verbs
    verbs=(
        'ls:list the sessions with their labels and panes'
        'tree:print the workspaces, windows, tabs and panes as an outline'
        'new:create a session, optionally labelled and running a command'
        'run:run a command and return its exit status and output'
        'send:type text into a session'
        'read:read the screen, the last command, or scrollback'
        'search:find a pattern in all the history the host keeps of a pane'
        'split:split a pane right or down'
        'kill:end a session'
        'label:label a session, or a workspace, window, tab or pane named with -w, -W, -t or -p'
        'tag:replace the tags on a session, or on a workspace, window, tab or pane'
        'status:show the OSC 7501 status records'
        'wait:wait for exit, idle, blocked, or text'
        'events:print the event stream'
        'api:list, describe, or call an operation'
        'mcp:run the MCP server over standard input and output'
        'completions:print a shell completion script (bash|zsh)'
        'selftest:attach, run a command, read it back'
        'imagetest:check that an inline image reaches the client'
    )

    local -a common
    common=(
        '(-H --host)'{-H,--host}'[reach a daemon over ssh]:user@host:_hosts'
        '--json[print the result as JSON]'
        '--yes[approve a confirm policy decision]'
        '(-s --session)'{-s,--session}'[target session ID or label]:session:__ubq_sessions'
        '(-p --pane)'{-p,--pane}'[target pane ID or label]:pane:__ubq_panes'
    )

    _arguments -C \
        '1: :->verb' \
        '*:: :->args' && return 0

    case "$state" in
        verb)
            _describe -t verbs 'ubq verb' verbs
            ;;
        args)
            case "$line[1]" in
                completions)
                    _values 'shell' bash zsh
                    ;;
                api)
                    if (( CURRENT == 2 )); then
                        _values 'api verb' list describe call validate
                    elif [[ $line[2] == list ]]; then
                        _arguments $common '--surface[which surface to list]:surface:(command-line control mcp sync all)'
                    fi
                    ;;
                wait)
                    _arguments $common \
                        '--for[what to wait for]:condition:(exit idle blocked text\:)' \
                        '--since[event cursor]:cursor' \
                        '--timeout[seconds]:seconds'
                    ;;
                events)
                    _arguments $common '--since[event cursor]:cursor' '--limit[events]:count' '--timeout[seconds]:seconds'
                    ;;
                read)
                    _arguments $common '--screen[the visible screen]' '--last-command[the last command'"'"'s output]' '--scrollback[lines]:lines'
                    ;;
                search)
                    _arguments $common '--from[first line searched]:line' '--to[line past the last searched]:line' '--case-sensitive[match case]' '*:pattern:'
                    ;;
                split)
                    _arguments $common '--right' '--down' '*:direction:(right down)'
                    ;;
                send)
                    _arguments $common '--enter[press Return after the text]' '*:text:'
                    ;;
                new)
                    _arguments $common '--label[label]:label' '--columns[columns]:columns' '--rows[rows]:rows' '*:command:'
                    ;;
                mcp|ls|selftest|imagetest)
                    _arguments $common
                    ;;
                tree)
                    _arguments $common '*:words:'
                    ;;
                label|tag)
                    _arguments $common \
                        '(-w --workspace)'{-w,--workspace}'[target workspace ID or label]:workspace:' \
                        '(-W --window)'{-W,--window}'[target window ID or label]:window:' \
                        '(-t --tab)'{-t,--tab}'[target tab ID or label]:tab:' \
                        '*: :'
                    ;;
                *)
                    _arguments $common '*: :'
                    ;;
            esac
            ;;
    esac
}

_ubiquitty_host() {
    local context state line
    typeset -A opt_args

    local -a scopes
    scopes=(observe drive control region new all)

    if (( CURRENT > 1 )) && [[ "$words[2]" == token ]]; then
        if (( CURRENT == 3 )); then
            _values 'token subcommand' issue list revoke
            return
        fi
        case "$words[3]" in
            issue)
                _arguments \
                    "--scope[scope list]:scope:_values -s , scope $scopes" \
                    '--expires[lifetime: N seconds, or Nm, Nh, Nd]:lifetime' \
                    '--label[human label]:label'
                ;;
        esac
        return
    fi

    _arguments \
        '--daemon[run the long-lived server]' \
        '--attach[bridge stdio to the daemon]' \
        '--ensure-daemon[start the daemon detached]' \
        '--columns[columns]:columns' \
        '--rows[rows]:rows' \
        '--quic-port[QUIC UDP port (0 = ephemeral, -1 = off)]:port' \
        '--quic-version[print the linked QUIC stack]' \
        '--quic-bootstrap[ensure a QUIC daemon, print port + token]' \
        '--quic-listen[bring up the QUIC listener, print the port]' \
        '--socket[pin a specific socket leaf]:socket name' \
        '(-h --help)'{-h,--help}'[show usage]' \
        '1:mode:(token)'
}

# Sourced or evaluated, the file binds the functions to the commands. Autoloaded
# from fpath as `_ubq`, it runs as that function's first call, for either
# command: it completes the line it was called for, and binds ubiquitty-host to
# its own function, which `#compdef` alone would send to `_ubq`.
case "$zsh_eval_context[-1]" in
    eval | file)
        compdef _ubq ubq 2>/dev/null || true
        compdef _ubiquitty_host ubiquitty-host 2>/dev/null || true
        ;;
    *)
        compdef _ubiquitty_host ubiquitty-host
        if [[ $service == ubiquitty-host ]]; then
            _ubiquitty_host "$@"
        else
            _ubq "$@"
        fi
        ;;
esac
