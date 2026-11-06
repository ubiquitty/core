#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# imgcat.sh — display an image inline using the iTerm2 OSC 1337 protocol.
#
# Usage:
#   ./Scripts/imgcat.sh <image-file> [width]
#
# width is an iTerm dimension: a cell count (e.g. 20), pixels (200px),
# a percentage of the window (50%), or "auto" (default). Ubiquitty decodes
# the container via ImageIO, so PNG / JPEG / GIF / HEIC all work.

set -euo pipefail

file="${1:?usage: imgcat.sh <image-file> [width]}"
width="${2:-auto}"

if [ ! -f "$file" ]; then
    echo "imgcat: no such file: $file" >&2
    exit 1
fi

b64=$(base64 < "$file" | tr -d '\n')
size=$(wc -c < "$file" | tr -d ' ')

printf '\033]1337;File=inline=1;size=%s;width=%s;preserveAspectRatio=1:%s\a\n' \
    "$size" "$width" "$b64"
