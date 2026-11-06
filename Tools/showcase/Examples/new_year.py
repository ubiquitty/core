#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""A New Year celebration: countdown, fireworks, and a greeting."""

import random
import shutil
import sys
import time
from datetime import datetime, timedelta

RESET = "\033[0m"
BOLD = "\033[1m"
HIDE_CURSOR = "\033[?25l"
SHOW_CURSOR = "\033[?25h"
CLEAR = "\033[2J\033[H"

COLORS = [
    "\033[91m", "\033[92m", "\033[93m", "\033[94m",
    "\033[95m", "\033[96m", "\033[97m",
]

SPARKS = "*+.x⋆✦✧·"


def move(row: int, col: int) -> str:
    return f"\033[{row};{col}H"


def center(text: str, width: int) -> str:
    pad = max(0, (width - len(text)) // 2)
    return " " * pad + text


def countdown(seconds: int) -> None:
    cols, rows = shutil.get_terminal_size((80, 24))
    for n in range(seconds, 0, -1):
        sys.stdout.write(CLEAR)
        big = f"{BOLD}{random.choice(COLORS)}{n}{RESET}"
        sys.stdout.write(move(rows // 2, 1))
        sys.stdout.write(center(big, cols + len(big) - 1))
        sys.stdout.write("\n")
        sys.stdout.write(center("Get ready...", cols))
        sys.stdout.flush()
        time.sleep(1)


def firework(cols: int, rows: int) -> None:
    cx = random.randint(8, max(9, cols - 8))
    cy = random.randint(3, max(4, rows // 2))
    color = random.choice(COLORS)

    for r in range(1, 6):
        sys.stdout.write(CLEAR)
        for angle in range(0, 360, 20):
            import math
            x = int(cx + r * math.cos(math.radians(angle)) * 2)
            y = int(cy + r * math.sin(math.radians(angle)))
            if 1 <= x <= cols and 1 <= y <= rows:
                sys.stdout.write(move(y, x) + color + random.choice(SPARKS) + RESET)
        sys.stdout.flush()
        time.sleep(0.08)


def finale() -> None:
    cols, rows = shutil.get_terminal_size((80, 24))
    year = datetime.now().year + 1
    msg = f"HAPPY NEW YEAR {year}!"
    sys.stdout.write(CLEAR)
    for _ in range(20):
        color = random.choice(COLORS)
        sys.stdout.write(move(rows // 2, 1))
        sys.stdout.write(center(f"{BOLD}{color}{msg}{RESET}", cols + len(color) + len(RESET) + len(BOLD)))
        sys.stdout.flush()
        time.sleep(0.15)
    sys.stdout.write("\n\n")


def main() -> None:
    sys.stdout.write(HIDE_CURSOR)
    try:
        countdown(5)
        cols, rows = shutil.get_terminal_size((80, 24))
        for _ in range(8):
            firework(cols, rows)
        finale()
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write(SHOW_CURSOR + RESET + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
