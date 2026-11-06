#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Tools/showcase/shades-demo.sh — show off the shade (░▒▓) + block-element
# rendering: the square-dot shade dither (SF Mono / Monaspace style) and smooth
# sub-cell progress bars built from eighth-blocks. Run it inside Ubiquitty.
#
#   ./Tools/showcase/shades-demo.sh            full demo
#   ./Tools/showcase/shades-demo.sh --no-anim  skip the animated section (static only)
set -u

ESC=$'\e'
csi() { printf '%s[%s' "$ESC" "$1"; }
sgr() { printf '%s[%sm' "$ESC" "$1"; }
reset() { printf '%s[0m' "$ESC"; }
hide_cursor() { printf '%s[?25l' "$ESC"; }
show_cursor() { printf '%s[?25h' "$ESC"; }
clear_screen() { printf '%s[2J%s[H' "$ESC" "$ESC"; }

ANIM=1
[ "${1:-}" = "--no-anim" ] && ANIM=0

trap 'show_cursor; reset; printf "\n"' EXIT INT

# Eighth-blocks for smooth (1/8-cell) horizontal progress.
EIGHTHS=(' ' '▏' '▎' '▍' '▌' '▋' '▊' '▉' '█')
# Shades, light → dark.
SHADES=('░' '▒' '▓' '█')

clear_screen
hide_cursor

# ---- 1. Shade ramp -------------------------------------------------------
sgr '1'; printf '  Shade dither — ░ ▒ ▓ █\n\n'; reset
for label_pair in "░:light (25%%)" "▒:medium (50%%)" "▓:dark (75%%)" "█:full (100%%)"; do
    g="${label_pair%%:*}"; desc="${label_pair#*:}"
    printf '   '
    sgr '38;5;31'
    for _ in $(seq 1 36); do printf '%s' "$g"; done
    reset
    printf '   %s\n' "$desc"
done
printf '\n'

# ---- 2. A two-colour gradient wash made of shades ------------------------
sgr '1'; printf '  Gradient wash (shades over a 256-colour ramp)\n\n'; reset
for _ in 0 1 2; do
    printf '   '
    for col in $(seq 0 47); do
        idx=$(( 17 + col / 2 ))
        g="${SHADES[$(( (col / 12) % 4 ))]}"
        printf '%s[38;5;%dm%s' "$ESC" "$idx" "$g"
    done
    reset; printf '\n'
done
printf '\n'

# ---- 3. Block-element showcase -------------------------------------------
sgr '1'; printf '  Block elements (quadrants · eighths · halves)\n\n'; reset
printf '   ▀▁▂▃▄▅▆▇█  ▏▎▍▌▋▊▉█  ▖▗▘▙▚▛▜▝▞▟  ▌▐▀▄\n\n'

if [ "$ANIM" -eq 0 ]; then
    reset; show_cursor; exit 0
fi

# ---- 4. Animated progress bars -------------------------------------------
sgr '1'; printf '  Animated progress\n\n'; reset

BARW=40   # cells

# Smooth eighth-block bar: 1/8-cell resolution so the leading edge
# advances sub-character, not in whole-cell jumps.
draw_eighth_bar() {
    local pct=$1 color=$2
    local total_eighths=$(( BARW * 8 ))
    local filled=$(( pct * total_eighths / 100 ))
    local full=$(( filled / 8 ))
    local rem=$(( filled % 8 ))
    printf '   '
    sgr "38;5;${color}"
    local i
    for ((i = 0; i < full; i++)); do printf '█'; done
    if [ "$rem" -gt 0 ] && [ "$full" -lt "$BARW" ]; then
        printf '%s' "${EIGHTHS[$rem]}"; full=$(( full + 1 ))
    fi
    reset
    sgr '38;5;238'
    for ((i = full; i < BARW; i++)); do printf '░'; done
    reset
    printf ' %3d%%' "$pct"
}

# Shade "pulse" bar: the fill cycles ░▒▓█ to suggest indeterminate work.
draw_pulse() {
    local phase=$1
    printf '   '
    local i
    for ((i = 0; i < BARW; i++)); do
        local s="${SHADES[$(( (i + phase) % 4 ))]}"
        printf '%s[38;5;31m%s' "$ESC" "$s"
    done
    reset
}

# Reserve two rows for the bars, then redraw them in place each tick:
# move the cursor up 2 lines, clear + redraw each row, drop back down 2.
printf '\n\n'

for pct in $(seq 0 2 100); do
    printf '%s[2A\r' "$ESC"               # up to the smooth-bar row
    csi '2K'; draw_eighth_bar "$pct" 40; printf '\n'
    csi '2K'; draw_pulse "$pct";         printf '\n'
    sleep 0.03
done

# Land the smooth bar full + green; leave the pulse on its last phase.
printf '%s[2A\r' "$ESC"
csi '2K'; draw_eighth_bar 100 40; printf '\n'
csi '2K'; printf '   '; sgr '38;5;31'
for _ in $(seq 1 "$BARW"); do printf '█'; done; reset; printf '\n'

printf '\n'
reset
show_cursor
