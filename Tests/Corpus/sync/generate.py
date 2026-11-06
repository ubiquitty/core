#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""generate.py — the synthetic half of the Sync PTY corpus.

Every entry here is the byte stream a program writes to its PTY, cut into the
reads a host would make off it, with the instant each read became available and
whether the PTY then had nothing more to give (a would-block). Coherence is a
property of reads, not of bytes: a redraw that arrives as two reads with a
would-block between them is a frame a host can publish half of, and the same
bytes in one read are not.

The streams are synthesised rather than recorded wherever the shape is the
point and a recording would only add noise: a prompt and its echo, a flood, a
cluster table, a synchronized-output user, a cursor-hiding repaint split across
writes, an Ink-style spinner. Programs whose shape synthesis would misrepresent
(vim, less, a process monitor) are recorded instead; see
Scripts/record-pty-corpus.sh.

Deterministic: fixed seeds, no clock, no environment. Running it twice writes
identical files, which the corpus test relies on to mean "the corpus" rather
than "this run of it".

    python3 Tests/Corpus/sync/generate.py            # rewrite every synthetic entry
    python3 Tests/Corpus/sync/generate.py --check    # fail if a file would change
"""

import argparse
import os
import random
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

ESC = "\x1b"
CSI = ESC + "["
OSC = ESC + "]"
BEL = "\x07"
ST = ESC + "\\"


class Stream:
    """A program's output as a sequence of reads on a microsecond clock."""

    def __init__(self):
        self.data = bytearray()
        self.reads = []  # (atMicroseconds, length, ending)
        self.redraws = []  # (beginOffset, endOffset)
        self.inputs = []  # atMicroseconds of each key the user sent
        self.now = 0
        self._redraw_begin = None

    def read(self, text, ending="block"):
        """Appends one read available at the current instant."""
        payload = text.encode("utf-8") if isinstance(text, str) else bytes(text)
        if not payload:
            return
        self.reads.append((self.now, len(payload), ending))
        self.data += payload

    def key(self):
        """Marks a key the user sent at the current instant; the read that
        follows is its echo."""
        self.inputs.append(self.now)

    def wait(self, microseconds):
        self.now += int(microseconds)

    def begin_redraw(self):
        self._redraw_begin = len(self.data)

    def end_redraw(self):
        assert self._redraw_begin is not None
        if len(self.data) > self._redraw_begin:
            self.redraws.append((self._redraw_begin, len(self.data)))
        self._redraw_begin = None

    def reads_text(self, description):
        lines = [
            "# " + description,
            "# Each read: <microseconds> <length> <block|more>.",
            "#   block — the PTY had nothing more after this read (a would-block).",
            "#   more  — the next read was already pending; a host keeps draining.",
            "# Each redraw: redraw <begin> <end> — byte offsets of one logical repaint,",
            "#   so a frame published strictly inside it showed half a repaint.",
            "# Each key: input <microseconds> — the user sent a key then; the next read",
            "#   is its echo, so the frame that first shows it measures the hold after input.",
        ]
        for at, length, ending in self.reads:
            lines.append(f"{at} {length} {ending}")
        for begin, end in self.redraws:
            lines.append(f"redraw {begin} {end}")
        for at in self.inputs:
            lines.append(f"input {at}")
        return "\n".join(lines) + "\n"


def prompt():
    return (OSC + "133;A" + BEL + CSI + "1;32muser@host" + CSI + "0m:" + CSI + "1;34m~/project"
            + CSI + "0m$ " + OSC + "133;B" + BEL)


def shell(rng):
    s = Stream()
    files = ["Makefile", "README.md", "src", "tests", "build.sh", "notes.txt", "LICENSE", "docs"]
    commands = [
        ("ls -la", "listing"), ("git status", "status"), ("cat notes.txt", "cat"),
        ("make -j8", "make"), ("ls", "listing"), ("echo $SHELL", "echo"),
        ("git log --oneline -20", "log"), ("make test", "make"), ("clear", "clear"),
        ("cat notes.txt", "cat"), ("ls -la", "listing"), ("history | tail", "log"),
        ("make -j8", "make"), ("git diff --stat", "status"), ("ls", "listing"),
    ]
    s.read(prompt())
    s.wait(400_000)
    for command, kind in commands:
        for character in command:
            s.key()
            s.read(character)
            s.wait(rng.randint(60_000, 180_000))
        s.key()
        s.read("\r\n" + OSC + "133;C" + BEL)
        s.wait(rng.randint(2_000, 9_000))
        if kind == "listing":
            out = []
            for i, name in enumerate(files * 2):
                colour = "1;34" if name in ("src", "tests", "docs") else "0;32" if name.endswith(".sh") else "0"
                out.append(f"-rw-r--r--  1 user  staff  {1000 + i * 137:6d} Oct  9 12:{i:02d} {CSI}{colour}m{name}{CSI}0m\r\n")
            s.read("".join(out))
        elif kind == "status":
            s.read("On branch main\r\nChanges not staged for commit:\r\n")
            s.wait(3_000)
            s.read("".join(f"\t{CSI}31mmodified:   src/file{i}.c{CSI}0m\r\n" for i in range(6)))
        elif kind == "cat":
            s.read("".join(f"{i:3d}  The quick brown fox jumps over the lazy dog, line {i}.\r\n" for i in range(60)))
        elif kind == "make":
            for i in range(30):
                s.read(f"{CSI}1;36m[{i * 100 // 30:3d}%]{CSI}0m Building CXX object src/module{i}.o\r\n")
                s.wait(rng.randint(5_000, 40_000))
            s.read(f"{CSI}1;32m[100%]{CSI}0m Built target app\r\n")
        elif kind == "log":
            s.read("".join(f"{CSI}33m{rng.getrandbits(28):07x}{CSI}0m Commit message number {i}\r\n" for i in range(20)))
        elif kind == "echo":
            s.read("/bin/zsh\r\n")
        elif kind == "clear":
            s.read(CSI + "H" + CSI + "2J" + CSI + "3J")
        s.wait(rng.randint(1_000, 4_000))
        s.read(OSC + "133;D;0" + BEL + prompt())
        s.wait(rng.randint(500_000, 1_500_000))
    return s


def cat_flood(rng):
    s = Stream()
    s.read(prompt() + "cat big.log\r\n" + OSC + "133;C" + BEL)
    s.wait(5_000)
    # A 400-line cycle fits zlib's window, so three megabytes check in small.
    cycle = [f"{i:07d} INFO worker-{i % 8} processed request id={i * 7:08x} in {i % 97} ms\r\n" for i in range(400)]
    body = "".join(cycle[i % len(cycle)] for i in range(50000)).encode()
    chunk = 16384
    offset = 0
    while offset < len(body):
        piece = body[offset:offset + chunk]
        offset += len(piece)
        s.read(piece, "more" if offset < len(body) else "block")
        # Paced like a local cat into a PTY, so the flood outlasts several
        # coalescing intervals rather than fitting inside one.
        s.wait(400)
    s.wait(2_000)
    s.read(OSC + "133;D;0" + BEL + prompt())
    return s


def emoji(rng):
    s = Stream()
    samples = [
        "family \U0001F468\u200D\U0001F469\u200D\U0001F467\u200D\U0001F466 done",
        "flags \U0001F1EF\U0001F1F5 \U0001F1FA\U0001F1F8 \U0001F1E9\U0001F1EA",
        "tones \U0001F44D\U0001F3FD \U0001F44B\U0001F3FF \U0001F9D1\U0001F3FB\u200D\U0001F4BB",
        "presentation \u2764\uFE0F \u263A\uFE0E 1\uFE0F\u20E3 \u00A9\uFE0F",
        "cjk \u6F22\u5B57\u304B\u306A\u30AB\u30CA \uD55C\uAD6D\uC5B4",
        "combining e\u0301 a\u0308 n\u0303 \u0915\u094D\u0937",
        "rainbow flag \U0001F3F3\uFE0F\u200D\U0001F308 and pirate \U0001F3F4\u200D\u2620\uFE0F",
    ]
    for line in range(48):
        text = samples[line % len(samples)]
        # A wide cluster at the right margin wraps; pad some lines to reach it.
        pad = "." * (rng.randint(0, 6) + (72 if line % 5 == 0 else 0))
        s.read(f"{line:02d} {pad}{text}\r\n")
        s.wait(rng.randint(20_000, 90_000))
        if line % 6 == 5:
            s.begin_redraw()
            s.read(CSI + "s" + CSI + "1;60H" + CSI + "7m status \u2705 \U0001F680 " + CSI + "0m" + CSI + "u")
            s.end_redraw()
            s.wait(30_000)
    return s


def links(rng):
    s = Stream()
    for i in range(40):
        name = f"file{i:02d}.c"
        url = f"file:///project/src/{name}"
        s.read(f"{OSC}8;id=f{i};{url}{ST}{name}{OSC}8;;{ST}  {OSC}8;;https://example.com/docs/{i}{ST}docs{OSC}8;;{ST}\r\n")
        s.wait(rng.randint(10_000, 60_000))
    # A link that wraps across the margin, then one overwritten in place.
    s.read(f"{OSC}8;;https://example.com/{'a' * 90}{ST}{'long link ' * 10}{OSC}8;;{ST}\r\n")
    s.wait(40_000)
    for i in range(10):
        s.begin_redraw()
        s.read(f"{CSI}5;1H{OSC}8;;https://example.com/replaced/{i}{ST}replaced {i}{OSC}8;;{ST}{CSI}K{CSI}24;1H")
        s.end_redraw()
        s.wait(50_000)
    return s


def double_width(rng):
    s = Stream()
    for i in range(30):
        kind = i % 4
        if kind == 0:
            s.read(f"{ESC}#6double width line {i}\r\n")
        elif kind == 1:
            s.read(f"{ESC}#3double height {i}\r\n{ESC}#4double height {i}\r\n")
        elif kind == 2:
            s.read(f"{ESC}#5single width line {i}\r\n")
        else:
            s.read(f"plain line {i}\r\n")
        s.wait(rng.randint(30_000, 80_000))
    # Rows that change attribute in place, without new text.
    for i in range(8):
        s.begin_redraw()
        s.read(f"{CSI}{3 + i};1H{ESC}#{6 if i % 2 == 0 else 5}{CSI}24;1H")
        s.end_redraw()
        s.wait(40_000)
    return s


def tui_frame(rng, frame, rows, columns, title):
    lines = [CSI + "H" + CSI + "1;44;37m" + f" {title} frame {frame:03d} ".ljust(columns) + CSI + "0m"]
    for row in range(2, rows):
        value = (frame * 7 + row * 13) % 100
        bar = "\u2588" * (value * (columns - 30) // 100)
        lines.append(f"{CSI}{row};1H{CSI}2Kitem {row:02d} {value:3d}% {CSI}32m{bar}{CSI}0m")
    return lines


def synchronized(rng):
    s = Stream()
    rows, columns = 24, 80
    s.read(CSI + "?1049h" + CSI + "H" + CSI + "2J")
    s.wait(20_000)
    for frame in range(60):
        body = tui_frame(rng, frame, rows, columns, "mode2026")
        s.begin_redraw()
        if frame % 3 == 0:
            # The block spans a would-block: the writer flushed mid-frame.
            half = len(body) // 2
            s.read(CSI + "?2026h" + "".join(body[:half]))
            s.wait(rng.randint(2_000, 6_000))
            s.read("".join(body[half:]) + CSI + "?2026l")
        else:
            s.read(CSI + "?2026h" + "".join(body) + CSI + "?2026l")
        s.end_redraw()
        s.wait(rng.randint(28_000, 38_000))
    s.read(CSI + "?1049l")
    return s


def cursor_hidden(rng):
    s = Stream()
    rows, columns = 24, 80
    s.read(CSI + "?1049h" + CSI + "H" + CSI + "2J")
    s.wait(20_000)
    typed = ""
    for frame in range(50):
        body = tui_frame(rng, frame, rows - 1, columns, "repaint")
        thirds = [body[: len(body) // 3], body[len(body) // 3: 2 * len(body) // 3], body[2 * len(body) // 3:]]
        s.begin_redraw()
        s.read(CSI + "?25l" + "".join(thirds[0]))
        for part in thirds[1:]:
            s.wait(rng.randint(1_500, 6_000))
            s.read("".join(part))
        s.wait(rng.randint(500, 2_000))
        s.read(f"{CSI}{rows};1H{CSI}2K> {typed}" + CSI + "?25h")
        s.end_redraw()
        s.wait(rng.randint(15_000, 40_000))
        # Input echo between repaints, with the cursor shown.
        if frame % 4 == 1:
            character = "abcdefghij"[frame % 10]
            typed += character
            s.key()
            s.read(character)
            s.wait(rng.randint(20_000, 60_000))
    s.read(CSI + "?1049l")
    return s


SPINNER = "\u280B\u2819\u2839\u2838\u283C\u2834\u2826\u2827\u2807\u280F"


def ink_frame(tick, typed, columns):
    width = columns - 2
    lines = [
        f"{CSI}36m{SPINNER[tick % len(SPINNER)]}{CSI}0m Thinking\u2026 ({tick * 80 // 1000}s \u00B7 \u2193 {tick * 37} tokens)",
        "",
        "\u256D" + "\u2500" * (width - 2) + "\u256E",
        "\u2502 > " + typed.ljust(width - 5)[: width - 5] + "\u2502",
        "\u2570" + "\u2500" * (width - 2) + "\u256F",
        f"{CSI}2m  ? for shortcuts{CSI}0m",
    ]
    return lines


def ink_erase(count):
    # Ink's eraseLines: clear each line and move up, then return to column 1.
    if count == 0:
        return ""
    return (CSI + "2K" + CSI + "1A") * (count - 1) + CSI + "2K" + CSI + "G"


def ink(rng):
    s = Stream()
    columns = 80
    s.read(CSI + "?25l")
    previous = 0
    typed = ""
    message = "Refactor the publish gate so it reads the parser state"
    for tick in range(120):
        logs = ""
        if tick % 25 == 24:
            logs = f"{CSI}32m\u25CF{CSI}0m Read Source/Core/file{tick}.cpp (120 lines)\n" * 3
        if 20 <= tick < 20 + len(message):
            typed = message[: tick - 19]
        frame = ink_frame(tick, typed, columns)
        # The terminal driver's ONLCR turns each newline Ink writes into CR LF.
        payload = (ink_erase(previous) + logs + "\n".join(frame) + "\n").replace("\n", "\r\n")
        previous = len(frame) + 1
        s.begin_redraw()
        if len(payload) > 900 or tick % 7 == 3:
            # A frame bigger than one read arrives as two.
            cut = len(payload) // 2
            s.read(payload[:cut])
            s.wait(rng.randint(300, 1_500))
            s.read(payload[cut:])
        else:
            s.read(payload)
        s.end_redraw()
        s.wait(80_000)
    s.read(ink_erase(previous) + "Done.\r\n" + CSI + "?25h")
    return s


def echo_burst(rng):
    s = Stream()
    rows, columns = 24, 80
    s.read(CSI + "?1049h" + CSI + "H" + CSI + "2J" + CSI + "7m" + " composer ".ljust(columns) + CSI + "0m"
           + f"{CSI}{rows};1H> ")
    s.wait(300_000)
    text = "the persistent bracket keeps an input burst from paying the deadline on every key "
    for burst in range(2):
        typed = ""
        for index in range(120):
            character = text[(burst * 120 + index) % len(text)]
            typed += character
            # The program hides the cursor on the burst's first key and keeps it
            # hidden until the burst is over: one bracket, open for seconds.
            hide = CSI + "?25l" if index == 0 else ""
            s.key()
            s.read(hide + f"{CSI}{rows};1H{CSI}2K> {typed[-(columns - 3):]}")
            s.wait(rng.randint(40_000, 90_000))
        s.wait(250_000)
        s.read(f"{CSI}{rows};{3 + min(len(typed), columns - 3)}H" + CSI + "?25h")
        s.wait(rng.randint(400_000, 700_000))
    s.read(CSI + "?1049l")
    return s


def line_trickle(shortest_gap, longest_gap):
    """A build log written a line at a time, each line its own write and its
    own read, with gaps drawn between the two bounds: a writer that never fills
    a drain pass and, below the quiet window, never goes quiet either."""
    def generate(rng):
        s = Stream()
        steps = ["compiling", "linking", "checking", "generating"]
        line = 0
        while s.now < 1_500_000:
            step = steps[line % len(steps)]
            s.read(f"[{line:05d}] {step} Source/Core/Module{line % 97:02d}.cpp " + CSI + "32mok" + CSI + "0m\r\n")
            s.wait(rng.randint(shortest_gap, longest_gap))
            line += 1
        return s
    return generate


ENTRIES = [
    # name, rows, columns, generator, description, compress
    ("shell", 24, 80, shell, "an interactive shell: prompts with OSC 133, typed commands echoed per key, output that scrolls into history", False),
    ("cat-flood", 24, 80, cat_flood, "cat of a 3 MB log: reads that never would-block until the end, so drain passes saturate", True),
    ("emoji-zwj", 24, 80, emoji, "emoji with ZWJ sequences, flags, skin tones, presentation selectors, CJK and combining marks, scrolled into history", False),
    ("osc8-links", 24, 80, links, "OSC 8 hyperlinks, one wrapping the margin, links overwritten in place", False),
    ("decdwl-decdhl", 24, 80, double_width, "DECDWL and DECDHL rows written, scrolled into history, and switched in place", False),
    ("synchronized-output", 24, 80, synchronized, "a DEC 2026 user repainting the alternate screen every ~33 ms; one block in three spans a would-block", False),
    ("cursor-hidden-repaint", 24, 80, cursor_hidden, "a non-2026 TUI: ?25l, a repaint in three writes with would-blocks between, ?25h; key echo between repaints", False),
    ("ink-spinner", 24, 80, ink, "an Ink-style agent UI: cursor hidden throughout, a spinner region erased and redrawn every 80 ms, some frames split across two reads", False),
    ("echo-burst", 24, 80, echo_burst, "a TUI that hides the cursor for the length of an input burst: two bursts of 120 echoed keys inside one ?25l bracket each", False),
    ("line-trickle-fast", 24, 80, line_trickle(200, 1_000), "a build log a line per write, 0.2 to 1 ms apart for 1.5 s: never fills a pass and never goes quiet", False),
    ("line-trickle-slow", 24, 80, line_trickle(2_500, 3_500), "a build log a line per write, 2.5 to 3.5 ms apart for 1.5 s: quiet after every line, just past the window", False),
]


def write_entry(name, stream, description, compress, check):
    changed = []
    stream_name = name + (".stream.z" if compress else ".stream")
    payload = zlib.compress(bytes(stream.data), 9) if compress else bytes(stream.data)
    files = {stream_name: payload, name + ".reads": stream.reads_text(description).encode()}
    for file_name, content in files.items():
        path = os.path.join(HERE, file_name)
        existing = open(path, "rb").read() if os.path.exists(path) else None
        if existing != content:
            changed.append(file_name)
            if not check:
                with open(path, "wb") as handle:
                    handle.write(content)
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report files that would change and exit 1")
    arguments = parser.parse_args()
    changed = []
    for index, (name, rows, columns, generator, description, compress) in enumerate(ENTRIES):
        stream = generator(random.Random(0x5EED + index))
        changed += write_entry(name, stream, description, compress, arguments.check)
    if arguments.check and changed:
        print("generate.py: out of date: " + ", ".join(changed), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
