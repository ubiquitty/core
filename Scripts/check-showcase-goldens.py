#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Check that every showcase entry and every golden name the other.

`Tools/showcase` is upstream of two checked artefacts owned by two different
checks: the PNG goldens the snapshot tier renders against, and the rows of
`Docs/feature_test_matrix.md` that `gen_matrix.py --check` compares. Each of
those gates reads one end and neither can see the source they share, so
removing an `Entry(...)` leaves a golden nothing renders and both gates stay
green — the removal is invisible to the only two things that look.

Both directions fail, because the two mistakes are different mistakes and a
check that catches one of them reads as covering both:

  * an entry naming a golden that is not on disk — the snapshot tier has
    nothing to compare against, and a tier that skips a missing file reports
    the same "no failures" as one that compared it,
  * a golden no entry names — the file survives the feature it recorded, and
    its pixels are a claim about a render path that no longer runs.

An entry with `snapshot=False` takes no picture: its feature draws nothing on
the grid, so it asserts its reply instead, and a golden on disk for it is one
nothing compares.

A blank golden is refused too: one that holds nothing but the background and
the cursor, the frame a sequence that drew nothing leaves. It is the same
picture for every such entry, and a comparison against it passes for any
feature that also draws nothing — including one that stopped working. A blank
entry either gets something visible to render or becomes a reply check.
BLANK_PENDING names the entries still waiting for one or the other, so the
count can only fall; an entry listed there whose golden is no longer blank
fails until it is removed.

BLANK_BY_CONTRACT names the entries whose expected result is that nothing
reaches the grid: a string the parser consumes, a write that answers nothing.
For them the blank frame is the assertion — a parser that printed the payload
would draw it — so they keep the picture beside their reply check. Their
goldens are the reference blank frame: each must draw nothing, and all must be
the same bytes.

A frame that is plain but different is a different case and passes: a hidden
cursor, a recoloured cursor, a swapped theme or reverse video each change the
one thing on screen. So outside BLANK_PENDING a golden counts as blank when it
draws nothing and is the reference frame or the same bytes as another golden;
inside it, drawing nothing is enough, so the last pending frame, or one that
stopped matching the others, still counts.

Stdlib only.

    python3 Scripts/check-showcase-goldens.py
"""

import json
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SHOWCASE = os.path.join(REPO, "Tools", "showcase", "showcase.py")
GOLDEN_DIR = os.path.join(REPO, "Tests", "Snapshots", "golden", "portable", "showcase")


# Entries whose golden is still the blank frame. Each draws nothing the portable
# renderer shows and answers nothing a reply check can read: a bell, a progress
# report or a tab status is presented by the frontend's chrome, and a window
# operation acts on a window the headless renderer does not have.
BLANK_PENDING = frozenset((
    "bell_bel", "bell_decps", "bell_notify",
    "color_special_osc113_118",
    "progress_determinate_40", "progress_error", "progress_indeterminate",
    "progress_paused_60", "progress_progress_clear_osc_9_4_0",
    "progress_tab_dot_clear_osc_21337",
    "screens_erase",
    "shell_osc1337_subcommands", "shell_working_directory_and_document_osc_7_osc_6",
    "tab_status_busy", "tab_status_idle", "tab_status_waiting",
    "window_title_csi_2_t_1_t_iconify_deiconify", "window_title_full_screen_csi_10_t",
    "window_title_maximize_csi_9_t", "window_title_move_csi_3_t",
    "window_title_repaint_csi_7_t", "window_title_resize_in_cells_csi_8_t",
    "window_title_resize_in_pixels_csi_4_t", "window_title_stacking_order_csi_6_t_csi_5_t",
))

# Entries whose contract is that nothing reaches the grid, so their blank golden
# is what they assert.
BLANK_BY_CONTRACT = frozenset((
    "clipboard_read_clipboard_osc_52", "clipboard_set_clipboard_osc_52",
    "conformance_conformance_level_decscl",
    "control_strings_application_program_command_apc",
    "control_strings_osc_terminators_bel_st",
    "control_strings_unknown_and_over_long_sequences",
    "multiplexing_tmux_dcs_passthrough",
))

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


def paeth(left, up, upperLeft):
    estimate = left + up - upperLeft
    distances = (abs(estimate - left), abs(estimate - up), abs(estimate - upperLeft))
    return (left, up, upperLeft)[distances.index(min(distances))]


def pixelRows(path):
    """(width, channels, rows) of an 8-bit, non-interlaced, non-palette PNG.

    The snapshot tier writes every row unfiltered, which is a slice; a row with
    a filter is undone byte by byte, which is slower and correct.

    - Parameter path: The PNG.
    - Returns: The decoded rows as bytes, or None for a file this cannot read.
    """
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(PNG_SIGNATURE):
        return None
    offset, header, compressed = len(PNG_SIGNATURE), None, bytearray()
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            compressed += body
        offset += 12 + length
    if header is None:
        return None
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or interlace or colour not in CHANNELS:
        return None
    stride = CHANNELS[colour]
    rowBytes = width * stride
    raw = zlib.decompress(bytes(compressed))
    rows = []
    previous = bytes(rowBytes)
    for row in range(height):
        start = row * (rowBytes + 1)
        kind, line = raw[start], raw[start + 1:start + 1 + rowBytes]
        if kind:
            line = bytearray(line)
            for index in range(rowBytes):
                left = line[index - stride] if index >= stride else 0
                up = previous[index]
                upperLeft = previous[index - stride] if index >= stride else 0
                predictor = (left if kind == 1 else up if kind == 2 else (left + up) >> 1 if kind == 3
                             else paeth(left, up, upperLeft))
                line[index] = (line[index] + predictor) & 0xFF
            line = bytes(line)
        rows.append(line)
        previous = line
    return width, stride, rows


def drawsNothing(path):
    """Whether a frame holds nothing but the background and the cursor.

    Decoded rather than compared to a known file, because the blank frame's
    bytes change whenever the renderer's background or cursor does, and a check
    keyed to the old bytes would then pass every blank frame minted after it.
    The cursor is the one mark a sequence that drew nothing still leaves, so a
    frame passes as blank when everything off the background fits in two cells
    each way of an 80 x 24 grid.

    - Parameter path: The golden.
    - Returns: True for a blank frame.
    """
    decoded = pixelRows(path)
    if decoded is None:
        return False
    width, stride, rows = decoded
    background = rows[-1][-stride:]
    blankRow = background * width
    maximumWidth, maximumHeight = 2 * width // 80, 2 * len(rows) // 24
    top = left = None
    right = bottom = -1
    for number, line in enumerate(rows):
        if line == blankRow:
            continue
        columns = [column for column in range(width)
                   if line[column * stride:(column + 1) * stride] != background]
        top = number if top is None else top
        bottom = number
        left = columns[0] if left is None else min(left, columns[0])
        right = max(right, columns[-1])
        if right - left >= maximumWidth or bottom - top >= maximumHeight:
            return False
    return True


def probes():
    """Returns the showcase's probe contract, or exits naming what refused."""
    try:
        out = subprocess.run([sys.executable, SHOWCASE, "--probes"],
                             capture_output=True, text=True, check=True, timeout=30)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
        stderr = (e.stderr or "").strip()
        raise SystemExit("check-showcase-goldens: %s --probes failed\n%s" % (SHOWCASE, stderr))
    return json.loads(out.stdout)


def main():
    entries = probes()

    # An empty contract is the shape that makes this pass without judging
    # anything: a showcase that fails to import its sections still prints `[]`,
    # and every set comparison below is then vacuously satisfied.
    if not entries:
        raise SystemExit("check-showcase-goldens: the showcase reported no entries at all")

    if not os.path.isdir(GOLDEN_DIR):
        raise SystemExit("check-showcase-goldens: no golden directory at %s" % GOLDEN_DIR)

    named = {}
    unnamed = []
    replyOnly = {}
    for entry in entries:
        golden = entry.get("golden")
        if not entry.get("snapshot", True):
            replyOnly[golden] = entry["title"]
        elif golden:
            named.setdefault(golden, []).append(entry["title"])
        else:
            unnamed.append(entry["title"])

    onDisk = {f[:-4] for f in os.listdir(GOLDEN_DIR) if f.endswith(".png")}

    failures = []
    for golden in sorted(set(named) - onDisk):
        failures.append("  %s.png — named by %s, not on disk"
                        % (golden, ", ".join(named[golden])))
    for golden in sorted(onDisk - set(named)):
        if golden in replyOnly:
            failures.append("  %s.png — on disk, and %r asserts a reply and takes no "
                            "picture" % (golden, replyOnly[golden]))
        else:
            failures.append("  %s.png — on disk, no showcase entry names it" % golden)
    pictures = {}
    for golden in onDisk & set(named):
        with open(os.path.join(GOLDEN_DIR, golden + ".png"), "rb") as handle:
            pictures[golden] = handle.read()
    byBytes = {}
    for golden, picture in pictures.items():
        byBytes.setdefault(picture, []).append(golden)
    for golden in sorted(BLANK_BY_CONTRACT - set(pictures)):
        failures.append("  %s — listed in BLANK_BY_CONTRACT and no entry takes its picture; "
                        "remove it from the list" % golden)
    reference = set()
    for golden in sorted(BLANK_BY_CONTRACT & set(pictures)):
        if not drawsNothing(os.path.join(GOLDEN_DIR, golden + ".png")):
            failures.append("  %s.png — blank by contract and draws something: nothing the "
                            "sequence carries may reach the grid" % golden)
        else:
            reference.add(pictures[golden])
    if len(reference) > 1:
        failures.append("  BLANK_BY_CONTRACT goldens differ: %d distinct blank frames, where "
                        "the renderer leaves one" % len(reference))
    blank = set()
    for golden, picture in pictures.items():
        if golden in BLANK_BY_CONTRACT:
            continue
        if golden not in BLANK_PENDING and picture not in reference and len(byBytes[picture]) < 2:
            continue
        if drawsNothing(os.path.join(GOLDEN_DIR, golden + ".png")):
            blank.add(golden)
    for golden in sorted(blank - BLANK_PENDING):
        failures.append("  %s.png — the blank frame other entries share: the sequence drew "
                        "nothing, so the picture proves nothing; draw something or assert "
                        "the reply (snapshot=False)" % golden)
    for golden in sorted(BLANK_PENDING - blank):
        failures.append("  %s — listed in BLANK_PENDING and no longer a blank golden; "
                        "remove it from the list" % golden)
    # Two entries sharing a golden means one of them is compared against the
    # other's pixels, which fails as a render bug in whichever renders second.
    for golden, titles in sorted(named.items()):
        if len(titles) > 1:
            failures.append("  %s.png — claimed by %d entries: %s"
                            % (golden, len(titles), ", ".join(titles)))
    for title in unnamed:
        failures.append("  (no golden) — entry %r names none, so nothing renders it" % title)

    if failures:
        print("check-showcase-goldens: the showcase and its goldens disagree", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("\nA showcase entry, its golden, and its row in "
              "Docs/feature_test_matrix.md move together. After changing "
              "Tools/showcase, delete or add the golden and re-run "
              "python3 Tests/Snapshots/gen_matrix.py.", file=sys.stderr)
        return 1

    print("check-showcase-goldens: ok (%d entries, %d goldens, paired both ways; "
          "%d reply-only, %d blank by contract, %d blank golden(s) pending)"
          % (len(entries), len(onDisk), len(replyOnly), len(BLANK_BY_CONTRACT), len(blank)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
