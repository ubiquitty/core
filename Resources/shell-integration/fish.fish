# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Ubiquitty fish shell integration — OSC 7 location reports and OSC 133
# marks.
#
# Opt-in. Add to ~/.config/fish/config.fish:
#   if test -n "$UBIQUITTY_INTEGRATION_DIR"
#       source "$UBIQUITTY_INTEGRATION_DIR/fish.fish"
#   end
#
# On a REMOTE host, copy this file across and source it unconditionally:
#   source ~/.ubiquitty/fish.fish
# ssh forwards TERM_PROGRAM only when both ends configure SendEnv and AcceptEnv,
# so a guard on it switches the integration off exactly where the location
# report is the only way to learn which machine a session is on.

# Once per shell: sourcing twice would double every report.
set -q UBIQUITTY_SHELL_INTEGRATION_DONE; and exit 0
set -g UBIQUITTY_SHELL_INTEGRATION_DONE 1

# Neither half is gated on the terminal being Ubiquitty. OSC 133 is read by
# kitty, WezTerm, VS Code, iTerm2 and Windows Terminal as well, and a terminal
# that knows neither sequence ignores both — so a guard would buy nothing and
# cost the marks in every other terminal.

# The machine this shell is on, resolved once. A prompt runs the report on every
# command, and forking `hostname` each time would be paid on all of them.
set -g __ubiquitty_host (hostname 2>/dev/null)

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
#
# fish's `string escape --style=url` is the percent-encoder, and it escapes the
# separators too, so the slashes are put back: a path is a series of encoded
# segments, not one encoded string.
function __ubiquitty_report_location
    set -l encoded (string escape --style=url -- $PWD | string replace -a '%2F' '/')
    printf '\e]7;file://%s%s\a' $__ubiquitty_host $encoded
    printf '\e]1337;RemoteHost=%s@%s\a' $USER $__ubiquitty_host
end

function __ubiquitty_prompt_start --on-event fish_prompt
    printf '\e]133;D;%s\a' $status
    # DECSCUSR 7 restores the terminal's configured cursor.
    printf '\e[7 q'
    printf '\e]133;A\a'
    __ubiquitty_report_location
end

function __ubiquitty_cmd_start --on-event fish_preexec
    printf '\e]133;C\a'
end
