#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""The evolution of the terminal, played on the terminal.

A title sequence, nine chapters, a credits roll, and a hand-off to the
shell. One thing travels through every era: HELLO, WORLD, typed on paper in
1963, drawn by a storage tube's beam, looped on a phosphor screen, surviving
a selective erase, printed in colour, set in eight scripts, built by a
shell that talks back, and at last running in a session that outlives the
window watching it. Each chapter is drawn only with the sequences its era
brought and the terminal still honours. Every cut is a wipe or a dissolve
under synchronized output, every card counts its year up in double height,
and the score is a tracker's piece played by DECPS, the VT520's play-sound
sequence with Ubiquitty's voices and channels, cued on a timeline under the
action.

    python3 evolution.py                       the director's cut, about two minutes
    python3 evolution.py --cut short           the first-launch cut, under a minute
    python3 evolution.py --chapter tektronix   one chapter (names: --list)
    python3 evolution.py --reduced-motion      cuts for dissolves, stills for fades
    python3 evolution.py --silent              no score
    python3 evolution.py --dump DIR            one .ans per chapter, for --snapshot
    python3 evolution.py --bake FILE           a POSIX sh player with no Python in it

Any key skips a chapter and q leaves. The persistence chapter starts a
private ubiquitty-host of its own, in a directory of its own with a token
of its own, and stops it afterwards; where no host can be found it plays
the same scene from a script and says so. Stdlib only, Python 3.8+. The
stage is 80x24 cells, centred in a larger window; a smaller window gets
the short cut with reduced motion.
"""

import argparse
import base64
import math
import os
import random
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ESC = "\x1b"
CSI = ESC + "["
OSC = ESC + "]"
ST = ESC + "\\"
DCS = ESC + "P"
APC = ESC + "_"
BEL = "\x07"
SO = "\x0e"
SI = "\x0f"

RESET = CSI + "0m"
BOLD = CSI + "1m"
DIM = CSI + "2m"
ITALIC = CSI + "3m"
HIDE = CSI + "?25l"
SHOW = CSI + "?25h"
CLEAR = CSI + "2J"
SYNC_ON = CSI + "?2026h"
SYNC_OFF = CSI + "?2026l"

STAGE_W = 80
STAGE_H = 24

MOUSE_RE = re.compile(rb"\x1b\[<(\d+);(\d+);(\d+)([mM])")
KEY_RE = re.compile(rb"\x1b\[(\d+)(?:;(\d+)(?::\d+)?)?u")
# Replies the terminal sends on its own initiative, which are not keys.
REPLY_RE = re.compile(rb"\x1b\[\?[\d;]*[cyu]|\x1b\[[\d;]*R|\x1b\](?:66|1[01]);[^\x07\x1b]*(?:\x07|\x1b\\)|\x1b_G[^\x1b]*\x1b\\")
SGR_RE = re.compile("\x1b(\\[[0-9;:?]*[a-zA-Z]|\\][^\x07\x1b]*(?:\x07|\x1b\\\\)|[#()][0-9A-Za-z ]|.)")


def sgr(*codes):
    return CSI + ";".join(str(c) for c in codes) + "m"


def fg(r, g, b):
    return CSI + "38;2;%d;%d;%dm" % (r, g, b)


def bg(r, g, b):
    return CSI + "48;2;%d;%d;%dm" % (r, g, b)


def rgb_spec(r, g, b):
    return "rgb:%02x/%02x/%02x" % (r, g, b)


class Era:
    """The colours a chapter is lit in: a phosphor, a paper, or the theme."""

    def __init__(self, background, foreground):
        self.background = background
        self.foreground = foreground


THEME = None  # the era whose colours are the terminal's own theme

PAPER = Era((232, 224, 200), (44, 40, 34))
PHOSPHOR = Era((6, 16, 8), (72, 255, 128))
AMBER = Era((22, 14, 4), (255, 178, 52))
INK = Era((0, 0, 0), (235, 235, 235))
ACCENT = (136, 136, 190)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def hsv(h, s, v):
    i = int(h * 6) % 6
    f = h * 6 - int(h * 6)
    p, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return int(r * 255), int(g * 255), int(b * 255)


def visible_width(text):
    return len(SGR_RE.sub("", text))


def link(uri, text):
    return OSC + "8;;" + uri + ST + text + OSC + "8;;" + ST


# ---------------------------------------------------------------------------
# The score: DECPS, notes 1..25 from C5, one duration per sequence
# ---------------------------------------------------------------------------
#
# Every DECPS request opens its own voice on the terminal, so sequences
# written at the same moment sound together: an arpeggio, a melody and a
# pad, each a sine, is the whole of the arrangement available. A sequence
# holds sixteen notes at most and one duration for all of them, so a bar of
# eighths is one sequence, a phrase with rhythm is cut into runs of equal
# duration, and every run is cued at the moment it starts; the cue list is
# played out during the show's pauses, so the music runs under the action.

C5, D5, E5, F5, G5, GS5, A5, B5 = 1, 3, 5, 6, 8, 9, 10, 12
C6, D6, E6, F6, G6, A6, B6, C7 = 13, 15, 17, 18, 20, 22, 24, 25
REST = 0
S, E, Q, DQ, H, DH, W = 4, 8, 16, 24, 32, 48, 64  # sixteenth to whole, in 1/32 s: 120 to the minute
BAR = W / 32.0

# The piece is a tracker's: the terminal's DECPS carries, as colon
# sub-parameters, a voice, a channel, an envelope and a transposition
# (Docs/extensions.md), so the score has a kick on the drum voice, a hat and a
# clap on noise, a square bass three octaves down, sawtooth stabs on the
# off-beats, a triangle arpeggio in sixteenths, a sine melody and a sine pad.
# Each part is its own channel, and a channel plays its requests in turn
# without a gap, so every bar is cued a moment early and joins the last. In
# C major, at a hundred and twenty to the minute; the volumes stay low.
CHORDS = {
    "Am": (C5, E5, A5), "F": (C5, F5, A5), "C": (C5, E5, G5), "G": (D5, G5, B5),
    "Dm": (D5, F5, A5), "E": (E5, GS5, B5), "Em": (E5, G5, B5),
}
ROOTS = {"Am": A5, "F": F5, "C": C5, "G": G5, "Dm": D5, "E": E5, "Em": E5}

# Beethoven's public-domain "Ode to Joy" theme, transposed to C major.
# Four-bar questions and answers keep their dotted-quarter/eighth cadences;
# the bridge leads back to the opening, and the coda resolves on the tonic.
# Source score: https://www.carnegiehall.org/Education/Programs/Link-Up/National-Program/The-Orchestra-Sings/Ode-to-Joy
THEME_A = [
    ("C", [(E6, Q), (E6, Q), (F6, Q), (G6, Q)]),
    ("G", [(G6, Q), (F6, Q), (E6, Q), (D6, Q)]),
    ("C", [(C6, Q), (C6, Q), (D6, Q), (E6, Q)]),
    ("G", [(E6, DQ), (D6, E), (D6, H)]),
    ("C", [(E6, Q), (E6, Q), (F6, Q), (G6, Q)]),
    ("G", [(G6, Q), (F6, Q), (E6, Q), (D6, Q)]),
    ("C", [(C6, Q), (C6, Q), (D6, Q), (E6, Q)]),
    ("C", [(D6, DQ), (C6, E), (C6, H)]),
]
THEME_B = [
    ("G", [(D6, Q), (D6, Q), (E6, Q), (C6, Q)]),
    ("G", [(D6, Q), (E6, E), (F6, E), (E6, Q), (C6, Q)]),
    ("G", [(D6, Q), (E6, E), (F6, E), (E6, Q), (D6, Q)]),
    ("G", [(C6, Q), (D6, Q), (G5, H)]),
    ("C", [(E6, Q), (E6, Q), (F6, Q), (G6, Q)]),
    ("G", [(G6, Q), (F6, Q), (E6, Q), (D6, Q)]),
    ("C", [(C6, Q), (C6, Q), (D6, Q), (E6, Q)]),
    ("C", [(D6, DQ), (C6, E), (C6, H)]),
]
FINAL = [("G", [(D6, H), (B5, H)]), ("C", [(C6, DH), (REST, Q)]), ("C", [(REST, W)])]
SONG = THEME_A + THEME_B
# How far ahead of the clock a bar is cued. Short, so a chapter's change of
# parts lands on the next bar or the one after; long enough that a picture
# being written does not hold a bar past its time.
LOOKAHEAD = 0.6

# The drum kit is sampled: seven short sounds synthesized here at the show's
# start and downloaded into the terminal's sample bank the way a soft font is
# downloaded (DCS + s, Docs/extensions.md), one slot per note number, so a
# DECPS pattern on the sample voice plays them like a tracker plays its
# instruments. The rest of the kit is the synth's own voices.
DRUM_RATE = 22050
DRUM_SLOTS = {"kick": 1, "snare": 2, "hat": 3, "ohat": 4, "clap": 5, "crash": 6, "rim": 7}


def drum_frames(kind):
    """One drum, as frames in -1..1 at DRUM_RATE, from a seeded generator so
    every run downloads the same bytes."""
    rnd = random.Random(7)
    env = lambda t, tau: math.exp(-t / tau)
    noise = lambda: rnd.random() * 2.0 - 1.0
    frames = []
    if kind == "kick":
        phase = 0.0
        for i in range(int(DRUM_RATE * 0.18)):
            t = i / DRUM_RATE
            phase += 2.0 * math.pi * (45.0 + 110.0 * env(t, 0.025)) / DRUM_RATE
            v = math.sin(phase) * env(t, 0.09) + (noise() * 0.5 if t < 0.003 else 0.0)
            frames.append(math.tanh(v * 2.2) * 0.9)
    elif kind == "snare":
        for i in range(int(DRUM_RATE * 0.16)):
            t = i / DRUM_RATE
            v = noise() * env(t, 0.05) * 0.8 + math.sin(2.0 * math.pi * 190.0 * t) * env(t, 0.04) * 0.6
            frames.append(math.tanh(v * 1.6) * 0.9)
    elif kind in ("hat", "ohat", "crash"):
        length, tau, level = {"hat": (0.045, 0.012, 0.7), "ohat": (0.22, 0.07, 0.5), "crash": (0.9, 0.3, 0.5)}[kind]
        previous = 0.0
        for i in range(int(DRUM_RATE * length)):
            t = i / DRUM_RATE
            current = noise()
            frames.append((current - previous) * 0.5 * env(t, tau) * level)
            previous = current
    elif kind == "clap":
        for i in range(int(DRUM_RATE * 0.15)):
            t = i / DRUM_RATE
            burst = any(start <= t < start + 0.008 for start in (0.0, 0.011, 0.022))
            frames.append(noise() * (0.8 if burst else (env(t - 0.03, 0.04) * 0.6 if t > 0.03 else 0.15)))
    elif kind == "rim":
        for i in range(int(DRUM_RATE * 0.02)):
            t = i / DRUM_RATE
            frames.append(math.sin(2.0 * math.pi * 800.0 * t) * env(t, 0.006) * 0.8)
    return frames


def wav_bytes(frames, rate):
    """A mono 16-bit RIFF/WAVE file of `frames`."""
    samples = [max(-32767, min(32767, int(round(f * 32767)))) for f in frames]
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 36 + 2 * len(samples), b"WAVE", b"fmt ", 16, 1, 1, rate, rate * 2, 2, 16, b"data", 2 * len(samples))
    return header + struct.pack("<%dh" % len(samples), *samples)


def download_drums(s):
    """The kit into the terminal's sample bank, one DCS + s per slot."""
    for kind, slot in DRUM_SLOTS.items():
        s.write(DCS + "%d;0+s" % slot + base64.b64encode(wav_bytes(drum_frames(kind), DRUM_RATE)).decode("ascii") + ST)


# The parts, each a voice on a channel of its own: keyword arguments to
# decps. Waves: 0 square, 1 sine, 2 triangle, 3 sawtooth, 4 noise, 5 drum,
# 6 sample. The drums are samples, so a note in their patterns is the slot
# it plays.
# Every part is cued on the bar's grid (Pgrid, a bar of 64 units), so the
# channels start together and stay together; each has its filters, the saws
# softened by a low-pass and the hats brightened by a high-pass, and a send
# to the hall.
KICK = dict(wave=6, channel=1, grid=W)
HAT = dict(wave=6, channel=2, highpass=2500, reverb=1, grid=W)
SNARE = dict(wave=6, channel=3, reverb=3, grid=W)
BASS = dict(wave=0, channel=4, down=36, gap=1, decay=60, sustain=4, lowpass=700, grid=W)
ARP = dict(wave=3, channel=5, down=12, gap=1, release=20, decay=40, sustain=5, lowpass=3200, reverb=2, grid=W)
STAB = dict(wave=3, channel=5, down=12, release=60, gap=2, decay=90, sustain=3, lowpass=2600, reverb=3, grid=W)
LEAD = dict(wave=1, channel=6, attack=6, release=40, reverb=4, grid=W)
OHAT = dict(wave=6, channel=7, highpass=1800, reverb=2, grid=W)
CRASH = dict(wave=6, channel=7, highpass=1200, reverb=4, grid=W)
PAD = dict(wave=3, channel=8, down=12, attack=300, release=300, lowpass=1400, reverb=5, grid=W)

# Patterns, sixteen steps a bar; a drum's step is its slot, 0 a rest.
KICK_A = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
KICK_B = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1]
KICK_FILL = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 1]
HAT_A = [0, 0, 3, 0] * 4
HAT_B = [(3, 1), (3, 1), (3, 3), (3, 1)] * 4
OPEN_HAT = [0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 4, 0]
SNARE_A = [0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0]
SNARE_FILL = [0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 2]
SNARE_ROLL = [0] * 8 + [(2, 1), (2, 1), (2, 2), (2, 2), (2, 3), (2, 3), (2, 4), (2, 5)]
FULL = ("crash", "kick", "hat", "ohat", "snare", "bass", "arp", "lead", "pad", "fills")
CLIMAX = ("crash", "kick", "hat16", "ohat", "snare", "bass", "arp", "lead", "pad", "fills")
STOP_ALL = CSI + "0,~"


def decps(notes, duration, volume, wave=0, channel=0, attack=None, release=None, down=0, gap=0, decay=0, sustain=7, lowpass=0, highpass=0, reverb=0, grid=0):
    """A DECPS request. Plain when the voice is the speaker's; otherwise the
    voice, channel, envelope, transposition, filters, reverb send and grid
    ride as sub-parameters, each group cut after its last piece that is not
    the default. A note given as (note, volume) carries its own level, and
    as (note, volume, effect, parameter) a tracker effect too, in the
    FastTracker numbering."""
    ramp = 0 if wave in (0, 4, 5, 6) else 4
    head = [volume, channel, wave, down, ramp if attack is None else attack, decay, sustain, ramp if release is None else release, lowpass, highpass, reverb]
    defaults = [None, 0, 0, 0, ramp, 0, 7, ramp, 0, 0, 0]
    hold = [duration, gap, grid]
    trim = lambda pieces, plain: pieces[:max(index + 1 for index, (piece, default) in enumerate(zip(pieces, plain)) if piece != default)] if any(piece != default for piece, default in zip(pieces, plain)) else pieces[:1]
    body = ";".join(":".join("%d" % piece for piece in note) if isinstance(note, tuple) else "%d" % note for note in notes)
    return CSI + ":".join("%d" % piece for piece in trim(head, defaults)) + ";" + ":".join("%d" % piece for piece in trim(hold, [None, 0, 0])) + ";" + body + ",~"


ARPEGGIO, PORTAMENTO_UP, TONE_PORTAMENTO, VIBRATO, EXTENDED = 0, 1, 3, 4, 14


def sound(s, notes, duration=3, volume=3):
    """An incidental blip, off unless asked for: a beep on every line and click
    is the sound of an alarm, and the show is scored instead."""
    if s.quiet or not s.blips:
        return
    s.write(decps(notes, duration, volume))


def runs_of(phrase):
    """A phrase cut into (duration, notes) runs a sequence can carry."""
    runs = []
    for note, duration in phrase:
        if runs and runs[-1][0] == duration and len(runs[-1][1]) < 16:
            runs[-1][1].append(note)
        else:
            runs.append((duration, [note]))
    return runs


def score(s, melody, volume=2, delay=0.0, voice=None):
    """Cues a phrase from `delay` seconds from now on the lead's channel, a
    moment early so its runs queue and join; a note a half or longer takes a
    vibrato. Without the tracker the phrase is the plain form on the speaker,
    each run on time, since a speaker has no queue. Returns its length in
    seconds."""
    at = delay
    for duration, notes in runs_of(melody):
        if not s.quiet:
            if s.tracker:
                sung = [(note, 0, VIBRATO, 0x24) if duration >= H and note else note for note in notes]
                cue_voice(s, s.now() + at, sung, duration, volume, **(voice or LEAD))
            else:
                s.cue(max(0.0, at), decps(notes, duration, volume))
        at += duration * len(notes) / 32.0
    return at


def bass_line(root, variation):
    """Sixteen steps of the bass: the root, its fifth, and octave jumps that
    slide up from the root the way a tracker's tone portamento does."""
    fifth, octave = root + 7, root + 12
    glide = (octave, 0, TONE_PORTAMENTO, 48)
    if variation:
        return [root, root, 0, root, 0, root, root, 0, root, 0, root, glide, 0, octave, root, fifth]
    return [root, 0, root, root, 0, root, 0, glide, root, 0, root, root, 0, root, fifth, octave]


def music(s, parts=FULL, coda=None):
    """The arrangement from the next bar on: the named parts play, the drums
    from their samples, a bass that jumps its octave, saw stabs or a saw
    arpeggio, the melody and a pad. With "fills" the fourth bar of every
    phrase is a drum fill and the bar after opens on a crash; "build" rolls
    the snare there instead. The piece starts with the first call, goes round
    the song for as long as the show runs, and ends with a `coda`. Returns the
    length of a bar in seconds."""
    if s.next_bar is None and s.coda is None:
        s.next_bar = s.now() + 0.3
    s.arrangement = tuple(parts)
    s.is_entering = True
    if coda is not None:
        s.coda = list(coda)
    s.conduct()
    return BAR


def cue_voice(s, at, notes, duration, volume, **voice):
    """A request for `at` on the clock, sent a moment early: a channel still
    playing queues it behind the bar playing, and an idle one holds it for
    the grid, so either way it starts on the bar."""
    s.cue(max(0.0, at - 0.05 - s.now()), decps(notes, duration, volume, **voice))


def bar_of_music(s, chord, tune, index, at):
    """One bar of the arrangement in force, starting at `at` on the clock.
    Without the tracker only the melody plays, in the plain form and on
    time, since a terminal whose DECPS is the speaker alone would play every
    channel's part on that one voice."""
    parts = s.arrangement
    low, mid, high = CHORDS[chord]
    root = ROOTS[chord]
    if not s.tracker:
        if "lead" in parts and tune:
            score(s, tune, 2, at - s.now())
        return
    is_fill = ("fills" in parts or "build" in parts) and index % 4 == 3
    opens_on_crash = "crash" in parts and (s.is_entering or ("fills" in parts and index % 4 == 0))
    cue = lambda notes, duration, volume, **voice: cue_voice(s, at, notes, duration, volume, **voice)
    if opens_on_crash:
        cue([6], 0, 3, **CRASH)
    elif "ohat" in parts:
        cue(OPEN_HAT, S, 2, **OHAT)
    if "kick" in parts:
        cue(KICK_FILL if is_fill else (KICK_B if index % 2 else KICK_A), S, 5, **KICK)
    if "hat" in parts:
        cue(HAT_A, S, 2, **HAT)
    if "hat16" in parts:
        cue(HAT_B, S, 2, **HAT)
    if "build" in parts and is_fill:
        cue(SNARE_ROLL, S, 4, **SNARE)
    elif "snare" in parts:
        cue(SNARE_FILL if is_fill else SNARE_A, S, 4, **SNARE)
    if "bass" in parts:
        cue(bass_line(root, index % 2 == 1), S, 3, **BASS)
    if "stab" in parts:
        # One voice, a chord: the arpeggio effect runs the stab's note
        # through the chord's third and fifth fifty times a second.
        third, fifth = mid - low, high - low
        stab = (low, 0, ARPEGGIO, (third << 4) | fifth)
        cue([0, 0, stab, 0, 0, 0, stab, 0, 0, 0, stab, 0, 0, 0, stab, 0], S, 2, **STAB)
    if "arp" in parts:
        cue([low, mid, high, mid + 12] * 4, S, 2, **ARP)
    if "lead" in parts and tune:
        score(s, tune, 2, at - s.now())
    if "pad" in parts:
        cue([high], W, 1, **PAD)


class Skip(Exception):
    """A key during a chapter: on to the next one."""


class Quit(Exception):
    """q, Escape or Ctrl-C: leave the show."""


# ---------------------------------------------------------------------------
# The channel a chapter writes to
# ---------------------------------------------------------------------------


class Show:
    """Bytes out, pauses, replies in, and the cues due along the way.

    The stage is 80x24 cells at an offset inside the real window, so every
    coordinate a chapter uses is a stage coordinate; ``at`` places the cursor
    and ``trow``/``tcol`` translate for the controls that take absolute
    positions (margins, rectangles). The clock is the show's own timeline:
    the cues a score leaves are written when the timeline reaches them.
    """

    is_live = False

    def __init__(self, columns, rows):
        self.columns = columns
        self.rows = rows
        self.row0 = max(0, (rows - STAGE_H) // 2)
        self.col0 = max(0, (columns - STAGE_W) // 2)
        self.in_tek = False
        self.wide = False
        self.window_columns = columns
        self.is_ubiquitty = False
        self.quiet = False
        self.blips = False
        self.sized_text = False
        # Whether DECPS here is the tracker, asked before the curtain; the
        # score is written for it, and a speaker gets the melody alone.
        self.tracker = False
        self.arrangement = ()
        self.bar = 0
        # When the next bar starts, on the clock; None before the music
        # begins and once the coda has been cued whole.
        self.next_bar = None
        self.coda = None
        self.is_entering = False
        self.short = False
        self.reduced = False
        self.session_mode = "auto"
        self.jump_to_finale = False
        self.cues = []
        self.clock = 0.0
        self.on_exit = []
        # The theme's own colours, learned by OSC 10/11 queries when live;
        # a dissolve into a theme-lit chapter needs the ground it lands on.
        self.theme = Era((26, 27, 38), (192, 202, 245))
        self.current_background = self.theme.background

    def now(self):
        return self.clock

    def cue(self, delay, text):
        when = self.now() + max(0.0, delay)
        index = len(self.cues)
        while index > 0 and self.cues[index - 1][0] > when:
            index -= 1
        self.cues.insert(index, (when, text))

    def conduct(self, horizon=None):
        """Keeps the score a moment ahead of the clock: every bar starting
        before `horizon` (the lookahead from now, unless given) is cued on
        the arrangement in force, so a chapter's change of parts lands on
        the next bar and the music never stops between chapters."""
        if self.quiet or self.next_bar is None or self.in_tek:
            return
        if horizon is None:
            horizon = self.now() + LOOKAHEAD
        while self.next_bar < horizon:
            if self.coda is not None:
                if not self.coda:
                    self.next_bar = None
                    return
                chord, tune = self.coda.pop(0)
                if not self.coda:
                    # The coda's last bar is the chord ringing: the band stops.
                    self.arrangement = tuple(part for part in self.arrangement if part in ("lead", "pad"))
            else:
                chord, tune = SONG[self.bar % len(SONG)]
            bar_of_music(self, chord, tune, self.bar, self.next_bar)
            self.is_entering = False
            self.bar += 1
            self.next_bar += BAR

    def queue_ahead(self, chapter):
        """Sends now every bar a chapter spends in Tek mode, since a storage
        tube cannot hear a CSI. The span is measured by a dry run of the
        chapter, and each channel plays its queue in turn, on the grid, so
        the bars keep their places without the clock."""
        if self.quiet or self.next_bar is None:
            return
        span = Meter(self).tek_span(chapter)
        self.conduct(self.now() + span + LOOKAHEAD)
        while self.cues:
            self.write(self.cues.pop(0)[1])

    def rewind(self):
        """After a skip: the bars queued for the chapter cut short are
        dropped, and the next bar keeps the beat."""
        if self.next_bar is None or self.next_bar <= self.now() + LOOKAHEAD:
            return
        self.write(STOP_ALL)
        self.cues = []
        ahead = (self.next_bar - self.now()) % BAR
        self.next_bar = self.now() + (ahead if ahead > 0.2 else ahead + BAR)

    def era_colours(self, era):
        return era if era is not None else self.theme

    def trow(self, row):
        return row + self.row0

    def tcol(self, col):
        return col + self.col0

    def at(self, row, col):
        return CSI + "%d;%dH" % (self.trow(row), self.tcol(col))

    def reframe(self, columns):
        """The stage centred again after the terminal changed its width."""
        self.columns = columns
        self.col0 = max(0, (columns - STAGE_W) // 2)

    def restore_width(self):
        """Back from DECCOLM. Core is eighty columns at once and the window
        follows it; the window is then asked for the width it had (XTWINOPS
        8, the host escape DECCOLM resizes by too), and the stage is framed
        on what the terminal reports once it has settled."""
        self.write(CSI + "?3l" + CSI + "?40l")
        self.wide = False
        window = self.window_columns
        if self.is_live:
            self.write(CSI + "8;%d;%dt" % (self.rows, window))
            reported = None
            for _ in range(8):
                reply = self.query(CSI + "18t", rb"\x1b\[8;\d+;\d+t", timeout=0.25)
                numbers = re.findall(r"\d+", reply or "")
                if len(numbers) == 3:
                    reported = int(numbers[2])
                if reported == window:
                    break
                self.pause(0.1)
            if reported is not None:
                window = reported
        self.reframe(window)

    def line_start(self, row):
        """The window's first column of a stage row: where a DECDWL or DECDHL
        line is begun, since the line's cells are double and the stage
        offset is paid in the padding instead."""
        return CSI + "%d;1H" % self.trow(row)

    def dwl_pad(self, text):
        """Leading spaces that centre ``text`` on a double-width line.

        A DECDWL/DECDHL line addresses double cells, so the stage offset and
        the slack are both halved.
        """
        return " " * (self.col0 // 2 + max(0, STAGE_W // 2 - visible_width(text)) // 2)

    def centred(self, row, text, col=1, width=STAGE_W):
        pad = max(0, (width - visible_width(text)) // 2)
        return self.at(row, col + pad) + text

    def write(self, *parts):
        raise NotImplementedError

    def flush(self):
        pass

    def pause(self, seconds):
        raise NotImplementedError

    def query(self, sequence, pattern, timeout=0.5):
        return None

    def events(self, seconds, keys=False):
        self.pause(seconds)
        return iter(())


class Meter(Show):
    """Runs a chapter against no terminal at all, to learn how long it is:
    the clock is the sum of its pauses, and the Tek mode's span is noted
    from the bytes that enter and leave it."""

    def __init__(self, like):
        super().__init__(like.columns, like.rows)
        for name in ("blips", "sized_text", "short", "reduced", "is_ubiquitty", "theme", "current_background", "tracker", "session_mode"):
            setattr(self, name, getattr(like, name))
        self.quiet = True
        self.entered = None
        self.exited = None

    def write(self, *parts):
        text = "".join(parts)
        if TEK_ENTER in text and self.entered is None:
            self.entered = self.clock
        if TEK_EXIT in text and self.entered is not None and self.exited is None:
            self.exited = self.clock

    def pause(self, seconds):
        self.clock += seconds

    def queue_ahead(self, chapter):
        pass

    def tek_span(self, chapter):
        try:
            chapter(self)
        except Skip:
            pass
        if self.entered is None:
            return 0.0
        return (self.exited if self.exited is not None else self.clock) - self.entered


class Live(Show):
    """Plays on the controlling terminal; stdin is read for skips and replies."""

    is_live = True

    def __init__(self):
        columns, rows = shutil.get_terminal_size((80, 24))
        super().__init__(columns, rows)
        self.out = sys.stdout.buffer
        self.fd = sys.stdin.fileno() if sys.stdin.isatty() else None
        self.pending = b""
        self.origin = time.monotonic()

    def now(self):
        return time.monotonic() - self.origin

    def write(self, *parts):
        self.out.write("".join(parts).encode("utf-8"))

    def flush(self):
        self.out.flush()

    def _emit_due(self):
        self.conduct()
        while self.cues and self.cues[0][0] <= self.now():
            self.write(self.cues.pop(0)[1])
        self.flush()

    def _wait(self, deadline):
        """Time to sleep before the deadline, the next cue, or the next bar
        coming within the conductor's lookahead."""
        remaining = deadline - self.now()
        if self.cues:
            remaining = min(remaining, self.cues[0][0] - self.now())
        if self.next_bar is not None:
            remaining = min(remaining, self.next_bar - LOOKAHEAD - self.now())
        return max(0.0, remaining)

    def _read(self, timeout):
        if self.fd is None:
            time.sleep(max(0.0, timeout))
            return b""
        ready, _, _ = select.select([self.fd], [], [], max(0.0, timeout))
        if not ready:
            return b""
        return os.read(self.fd, 4096)

    def _keys(self, data):
        text = REPLY_RE.sub(b"", MOUSE_RE.sub(b"", data))
        if not text:
            return
        if b"q" in text or b"Q" in text or b"\x03" in text or text == b"\x1b":
            raise Quit()
        raise Skip()

    def pause(self, seconds):
        self.flush()
        if self.pending:
            data, self.pending = self.pending, b""
            self._keys(data)
        deadline = self.now() + seconds
        while True:
            self._emit_due()
            if self.now() >= deadline:
                return
            data = self._read(self._wait(deadline))
            if data:
                self._keys(data)

    def query(self, sequence, pattern, timeout=0.5):
        self.write(sequence)
        self.flush()
        expected = re.compile(pattern)
        buffer, self.pending = self.pending, b""
        deadline = self.now() + timeout
        while True:
            match = expected.search(buffer)
            if match:
                self.pending = buffer[:match.start()] + buffer[match.end():]
                return match.group(0).decode("ascii", "replace")
            self._emit_due()
            if self.now() >= deadline:
                self.pending = buffer
                return None
            buffer += self._read(self._wait(deadline))

    def events(self, seconds, keys=False):
        """Mouse reports, and key reports when asked, for up to ``seconds``.

        Yields ("mouse", button, column, row, pressed) and ("key", code,
        modifiers). Without `keys`, a key still skips the chapter; with them,
        only q leaves, and every other key is an event.
        """
        self.flush()
        buffer, self.pending = self.pending, b""
        deadline = self.now() + seconds
        while True:
            while True:
                mouse = MOUSE_RE.search(buffer)
                key = KEY_RE.search(buffer) if keys else None
                match = min((m for m in (mouse, key) if m), key=lambda m: m.start(), default=None)
                if not match:
                    break
                before, buffer = buffer[:match.start()], buffer[match.end():]
                for event in self._plain_keys(before, keys):
                    yield event
                if match is mouse:
                    yield ("mouse", int(match.group(1)), int(match.group(2)), int(match.group(3)), match.group(4) == b"M")
                else:
                    code, modifiers = int(match.group(1)), int(match.group(2) or 1)
                    if code == 113 and modifiers == 1:
                        raise Quit()
                    yield ("key", code, modifiers)
            if buffer and not buffer.startswith(b"\x1b["):
                data, buffer = buffer, b""
                for event in self._plain_keys(data, keys):
                    yield event
            self._emit_due()
            if self.now() >= deadline:
                return
            buffer += self._read(self._wait(deadline))

    def _plain_keys(self, data, keys):
        """Bytes that are not reports: a terminal without the kitty protocol sends these."""
        data = REPLY_RE.sub(b"", data)
        if not data:
            return
        if not keys:
            self._keys(data)
            return
        text = data.decode("utf-8", "replace")
        if text in ("q", "Q", "\x03"):
            raise Quit()
        yield ("key", ord(text[0]), 1)


class Recorder(Show):
    """Collects what a chapter writes, per chapter, for --dump and --bake.

    The clock is the sum of the pauses recorded, and a cue due inside a
    pause splits it, so the baked player writes the sequence at the same
    moment the live show would.
    """

    def __init__(self, columns, rows, assume_ubiquitty):
        super().__init__(columns, rows)
        self.is_ubiquitty = assume_ubiquitty
        self.chapters = []

    def begin(self, name):
        self.chapters.append((name, []))

    def write(self, *parts):
        self.chapters[-1][1].append(("bytes", "".join(parts)))

    def pause(self, seconds):
        end = self.clock + seconds
        events = self.chapters[-1][1]
        self.conduct(end + LOOKAHEAD)
        while self.cues and self.cues[0][0] <= end:
            when, text = self.cues.pop(0)
            if when > self.clock:
                events.append(("sleep", when - self.clock))
                self.clock = when
            events.append(("bytes", text))
        if end > self.clock:
            events.append(("sleep", end - self.clock))
            self.clock = end


# ---------------------------------------------------------------------------
# Pictures, built here so the script carries no files
# ---------------------------------------------------------------------------


def png_bytes(width, height, rgba):
    def chunk(tag, payload):
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def kitty_chunks(control, payload):
    encoded = base64.b64encode(payload).decode("ascii")
    parts = [encoded[i:i + 4096] for i in range(0, len(encoded), 4096)] or [""]
    out = []
    for index, part in enumerate(parts):
        more = 1 if index < len(parts) - 1 else 0
        continuation = "a=f," if "a=f" in control.split(",") else ""
        head = "%s,m=%d" % (control, more) if index == 0 else "%sm=%d" % (continuation, more)
        out.append(APC + "G" + head + ";" + part + ST)
    return "".join(out)


# Kitty ids well above the small integers: an image that arrives without an
# id, an OSC 1337 file for one, is numbered by the terminal, and a terminal
# that numbers from one collides with a client's first `i=`.
KITTY_JULIA, KITTY_SPINNER, KITTY_FULLSCREEN = 7001, 7002, 7003


def kitty_transmit(image_id, width, height, rgba):
    """Transmit raw RGBA under an id and place nothing yet."""
    return kitty_chunks("a=t,f=32,s=%d,v=%d,i=%d,q=2" % (width, height, image_id), rgba)


def kitty_place(image_id, columns, rows, source=None, placement=1):
    """Place a transmitted image at the cursor into a cell box.

    `source` is an (x, y, w, h) rectangle of the image's pixels; the same
    placement id replaces the previous placement, which is what a wipe is.
    """
    control = "a=p,i=%d,p=%d,c=%d,r=%d,q=2" % (image_id, placement, columns, rows)
    if source:
        control += ",x=%d,y=%d,w=%d,h=%d" % source
    return APC + "G" + control + ST


def kitty_image(image_id, width, height, rgba, columns, rows):
    """Transmit and place raw RGBA at the cursor, scaled into a cell box."""
    control = "a=T,f=32,s=%d,v=%d,i=%d,c=%d,r=%d,q=2" % (width, height, image_id, columns, rows)
    return kitty_chunks(control, rgba)


def kitty_animation(image_id, width, height, frames, columns, rows, delay_ms):
    out = [kitty_chunks("a=T,f=32,s=%d,v=%d,i=%d,c=%d,r=%d,q=2" % (width, height, image_id, columns, rows), frames[0])]
    for frame in frames[1:]:
        out.append(kitty_chunks("a=f,f=32,s=%d,v=%d,i=%d,z=%d,q=2" % (width, height, image_id, delay_ms), frame))
    out.append(APC + "Ga=a,i=%d,s=3,r=1,z=%d,q=2" % (image_id, delay_ms) + ST)
    return "".join(out)


def iterm2_image(width, height, rgba, columns, rows, name="picture.png"):
    png = png_bytes(width, height, rgba)
    args = "name=%s;size=%d;inline=1;width=%d;height=%d;preserveAspectRatio=1" % (
        base64.b64encode(name.encode()).decode("ascii"), len(png), columns, rows)
    return OSC + "1337;File=" + args + ":" + base64.b64encode(png).decode("ascii") + BEL


def sixel_image(width, height, rgba, levels=4, bands=None):
    """DCS q: one colour pass per band, run-length compressed.

    `bands` limits the picture to its first so many six-row bands, which is
    how it develops on screen a band at a time, the way a picture arrived
    down a serial line.
    """
    step = 256 // levels
    palette = {}
    indexed = bytearray(width * height)
    for p in range(width * height):
        key = tuple(min(levels - 1, rgba[p * 4 + c] // step) * 100 // (levels - 1) for c in range(3))
        index = palette.get(key)
        if index is None:
            index = len(palette)
            palette[key] = index
        indexed[p] = index
    out = [DCS + "0;0;0q"]
    for (r, g, b), index in palette.items():
        out.append("#%d;2;%d;%d;%d" % (index, r, g, b))
    limit = height if bands is None else min(height, bands * 6)
    for band in range(0, limit, 6):
        present = []
        seen = set()
        for y in range(band, min(band + 6, height)):
            for x in range(width):
                index = indexed[y * width + x]
                if index not in seen:
                    seen.add(index)
                    present.append(index)
        for index in present:
            run = []
            for x in range(width):
                bits = 0
                for dy in range(6):
                    y = band + dy
                    if y < height and indexed[y * width + x] == index:
                        bits |= 1 << dy
                run.append(chr(0x3F + bits))
            encoded = []
            i = 0
            while i < len(run):
                j = i
                while j < len(run) and run[j] == run[i]:
                    j += 1
                encoded.append("!%d%s" % (j - i, run[i]) if j - i >= 3 else run[i] * (j - i))
                i = j
            out.append("#%d" % index + "".join(encoded) + "$")
        out.append("-")
    out.append(ST)
    return "".join(out)


def picture_sunset(width, height):
    """A sunset for the Sixel era: sky, sun, two ridges and water."""
    buf = bytearray(width * height * 4)
    horizon = int(height * 0.62)
    sun_x, sun_y, sun_r = width * 0.68, height * 0.44, height * 0.15
    for y in range(height):
        for x in range(width):
            if y < horizon:
                colour = lerp((40, 20, 70), (255, 150, 60), y / horizon)
                if math.hypot(x - sun_x, y - sun_y) < sun_r:
                    colour = (255, 236, 150)
            else:
                t = (y - horizon) / max(1, height - horizon)
                colour = lerp((70, 40, 110), (20, 10, 40), t)
                if abs(x - sun_x) < sun_r * (1.2 - t) and (y // 2) % 2 == 0:
                    colour = lerp(colour, (255, 200, 120), 0.6 - t * 0.5)
            ridge_far = height * 0.48 + height * 0.07 * math.sin(x / 19.0) + height * 0.04 * math.sin(x / 7.0 + 1)
            ridge_near = height * 0.58 + height * 0.05 * math.sin(x / 13.0 + 2) + height * 0.03 * math.sin(x / 5.0)
            if y >= ridge_near and y < horizon:
                colour = (35, 18, 50)
            elif y >= ridge_far and y < horizon:
                colour = (80, 40, 100)
            i = (y * width + x) * 4
            buf[i:i + 4] = bytes(colour + (255,))
    return bytes(buf)


def picture_sphere(size):
    """A lit sphere on a transparent ground, for the inline-PNG era."""
    buf = bytearray(size * size * 4)
    radius = size * 0.46
    centre = size / 2.0
    light = (-0.5, -0.6, 0.62)
    for y in range(size):
        for x in range(size):
            dx, dy = (x + 0.5 - centre) / radius, (y + 0.5 - centre) / radius
            d2 = dx * dx + dy * dy
            i = (y * size + x) * 4
            if d2 > 1.0:
                continue
            dz = math.sqrt(1.0 - d2)
            lambert = max(0.0, dx * light[0] + dy * light[1] + dz * light[2])
            spec = max(0.0, lambert) ** 24
            base = (70, 130, 255)
            colour = tuple(min(255, int(c * (0.18 + 0.82 * lambert) + 200 * spec)) for c in base)
            edge = min(1.0, (1.0 - d2) * radius * 0.5)
            buf[i:i + 4] = bytes(colour + (int(255 * edge),))
    return bytes(buf)


def picture_julia(width, height, limit=40):
    """A Julia set, c = -0.8 + 0.156i, for the kitty-graphics era."""
    buf = bytearray(width * height * 4)
    cr, ci = -0.8, 0.156
    for y in range(height):
        zi0 = (y / height - 0.5) * 2.4
        for x in range(width):
            zr, zi = (x / width - 0.5) * 3.6, zi0
            n = 0
            while n < limit and zr * zr + zi * zi < 16.0:
                zr, zi = zr * zr - zi * zi + cr, 2 * zr * zi + ci
                n += 1
            i = (y * width + x) * 4
            if n >= limit:
                colour = (8, 6, 24)
            else:
                t = (n + 1 - math.log(math.log(zr * zr + zi * zi + 1e-9) / 2 + 1e-9) / math.log(2)) / limit
                t = max(0.0, min(1.0, t))
                colour = tuple(int(255 * (0.5 + 0.5 * math.cos(6.2832 * (t * 1.6 + phase)))) for phase in (0.0, 0.12, 0.28))
            buf[i:i + 4] = bytes(colour + (255,))
    return bytes(buf)


def picture_spinner(size, frames):
    """Frames of a rotating arc on a transparent ground, for kitty animation."""
    out = []
    centre = size / 2.0
    for frame in range(frames):
        buf = bytearray(size * size * 4)
        start = frame * 6.2832 / frames
        for y in range(size):
            for x in range(size):
                dx, dy = x + 0.5 - centre, y + 0.5 - centre
                radius = math.hypot(dx, dy)
                if radius < size * 0.28 or radius > size * 0.42:
                    continue
                angle = (math.atan2(dy, dx) - start) % 6.2832
                if angle > 4.9:
                    continue
                tone = angle / 4.9
                colour = lerp((70, 130, 255), (200, 240, 255), tone)
                i = (y * size + x) * 4
                buf[i:i + 4] = bytes(colour + (int(60 + 195 * tone),))
        out.append(bytes(buf))
    return out


class Pictures:
    """Every raster the show places, computed once before the curtain."""

    def __init__(self):
        self.sunset = (200, 96, picture_sunset(200, 96))
        self.sphere = (96, 96, picture_sphere(96))
        self.julia = (240, 120, picture_julia(240, 120))
        self.spinner = (48, 48, picture_spinner(48, 8))


# ---------------------------------------------------------------------------
# Staging: wipes, fades, the leader, the roll
# ---------------------------------------------------------------------------


def light(s, era):
    """OSC 10/11/12: the era's colours, or the theme's own back."""
    s.current_background = s.era_colours(era).background
    if era is None:
        return OSC + "111" + ST + OSC + "110" + ST + OSC + "112" + ST
    return (OSC + "11;" + rgb_spec(*era.background) + ST + OSC + "10;" + rgb_spec(*era.foreground) + ST
            + OSC + "12;" + rgb_spec(*era.foreground) + ST)


def cut(s, era, kind="dissolve", hold=0.05):
    """A cut between eras: the next ground takes the stage, then the window.

    `kind` names the wipe: a dissolve sweeps in on a diagonal from the top
    left, a curtain closes from both edges, an iris shrinks the old picture
    to a point at the centre, and a slide sweeps down from the top. Crossfade
    blends the two grounds after the outgoing title fades away. All run under
    synchronized output; with reduced motion the cut is a cut. Inline pictures
    float above the cells, so they go first.
    """
    colour = s.era_colours(era).background
    s.write(APC + "Ga=d,d=A" + ST)
    if s.reduced:
        s.write(light(s, era), CLEAR)
        return
    if kind == "crossfade":
        previous = s.current_background
        s.write(RESET, CLEAR)
        for step in range(1, 17):
            t = step / 16
            t = t * t * (3 - 2 * t)
            s.write(SYNC_ON, OSC + "11;" + rgb_spec(*lerp(previous, colour, t)) + ST, SYNC_OFF)
            s.pause(hold)
        s.write(light(s, era), CLEAR)
        return
    frames = []
    if kind == "dissolve":
        reach = STAGE_W + STAGE_H * 2.5
        for step in range(1, 9):
            limit = reach * step / 8.0
            frames.append([(row, col, 1) for row in range(1, STAGE_H + 1) for col in range(1, STAGE_W + 1)
                           if col + row * 2.5 <= limit])
    elif kind == "curtain":
        for step in range(1, 9):
            width = step * 5
            frames.append([(row, col, width) for row in range(1, STAGE_H + 1) for col in (1, STAGE_W - width + 1)])
    elif kind == "iris":
        for step in range(1, 9):
            radius = 20.0 * (1.0 - step / 8.0)
            frames.append([(row, col, 1) for row in range(1, STAGE_H + 1) for col in range(1, STAGE_W + 1)
                           if math.hypot((col - 40.5) / 2.5, row - 12.5) > radius])
    else:
        for step in range(1, 9):
            frames.append([(row, 1, STAGE_W) for row in range((step - 1) * 3 + 1, min(STAGE_H, step * 3) + 1)])
    painted = set()
    for frame in frames:
        fresh = {}
        for row, col, width in frame:
            for column in range(col, col + width):
                if (row, column) not in painted:
                    fresh.setdefault(row, []).append(column)
                    painted.add((row, column))
        s.write(SYNC_ON, RESET)
        for row, columns in sorted(fresh.items()):
            columns.sort()
            start = previous = columns[0]
            for column in columns[1:] + [STAGE_W + 2]:
                if column == previous + 1:
                    previous = column
                    continue
                width = previous - start + 1
                # ECH leaves the cursor in place. Its default erase background
                # repairs an entire text block without spreading the new colour.
                s.write(s.at(row, start), CSI + "49m", CSI + "%dX" % width, bg(*colour), " " * width)
                start = previous = column
        s.write(RESET, SYNC_OFF)
        s.pause(hold)
    s.write(light(s, era), CLEAR)


def fade_in(s, era, lines, steps=6, hold=0.06):
    """Text that brightens from the ground up: (row, col, text, weight) tuples.

    Weight 1 is the era's foreground, 0.5 a muted line. Each step redraws the
    lines in a colour between the ground and the target, under synchronized
    output, so the card comes up like a lamp rather than a print.
    """
    colours = s.era_colours(era)
    if s.reduced:
        steps = 1
    for step in range(1, steps + 1):
        t = step / steps
        s.write(SYNC_ON)
        for row, col, text, weight in lines:
            target = lerp(colours.background, colours.foreground, weight)
            s.write(s.at(row, col), fg(*lerp(colours.background, target, t)), text, RESET)
        s.write(SYNC_OFF)
        s.pause(hold)


def dec_headline(s, row, text, colour):
    """A DECDHL pair, used in the DEC type specimen."""
    pad = s.dwl_pad(text)
    return (s.line_start(row) + ESC + "#3" + pad + fg(*colour) + BOLD + text + RESET
            + s.line_start(row + 1) + ESC + "#4" + pad + fg(*colour) + BOLD + text + RESET)


def proportional_heading(s, row, text, colour, bold=False, scale=1):
    """Centred type at its natural proportions, inside the stage's measure."""
    cells = (STAGE_W - 4) // scale
    return (s.at(row, 3) + fg(*colour) + (BOLD if bold else "")
            + OSC + "66;p=1:c=%d:s=%d:h=2;" % (cells, scale) + text + BEL + RESET)


def headline(s, row, text, colour):
    if s.sized_text:
        return proportional_heading(s, row, text, colour, bold=True, scale=2)
    return dec_headline(s, row, text, colour)


def card(s, era, numeral, year, title, subtitle, from_year=None):
    """A chapter card: a large year, a smaller title, and a quiet subtitle."""
    s.write(HIDE, CLEAR)
    mid = STAGE_H // 2 - 2
    colours = s.era_colours(era)
    def paint(weight, year_weight=None):
        if year_weight is None:
            year_weight = weight
        colour = lerp(colours.background, colours.foreground, weight)
        s.write(SYNC_ON, s.centred(mid - 2, fg(*lerp(colours.background, colours.foreground, 0.55 * weight)) + "CHAPTER " + numeral + RESET),
                headline(s, mid, year, lerp(colours.background, colours.foreground, year_weight)))
        if s.sized_text:
            s.write(proportional_heading(s, mid + 3, title, colour, bold=True))
            s.write(proportional_heading(s, mid + 5, subtitle, lerp(colours.background, colours.foreground, 0.6 * weight)))
        else:
            s.write(s.line_start(mid + 3), ESC + "#6", s.dwl_pad(title), fg(*colour), BOLD, title, RESET)
            s.write(s.centred(mid + 5, fg(*lerp(colours.background, colours.foreground, 0.6 * weight)) + subtitle + RESET))
        s.write(SYNC_OFF)
    s.write(s.centred(mid - 2, fg(*lerp(colours.background, colours.foreground, 0.55)) + "CHAPTER " + numeral + RESET))
    if from_year is not None and not s.reduced:
        span = int(year) - int(from_year)
        for step in range(1, 9):
            shown = int(from_year) + round(span * (step / 8.0) ** 1.6)
            s.write(SYNC_ON, headline(s, mid, str(shown), lerp(colours.background, colours.foreground, 0.35 + 0.65 * step / 8)), SYNC_OFF)
            s.pause(0.05 + 0.02 * step)
    for step in range(1, (1 if s.reduced else 6) + 1):
        paint(1.0 if s.reduced else step / 6, year_weight=1.0)
        s.pause(0.07)
    s.pause(0.9 if s.short else 1.5)
    if not s.reduced:
        for step in range(7, -1, -1):
            paint(step / 8)
            s.pause(0.05)
    s.write(CLEAR)


def crt_power_on(s, era):
    """A tube coming up: a bright line at the middle that opens into the frame."""
    if s.reduced:
        return
    colours = s.era_colours(era)
    mid = STAGE_H // 2
    for half in (0, 1, 2, 4, 7, 11):
        s.write(SYNC_ON)
        for row in range(max(1, mid - half), min(STAGE_H, mid + half) + 1):
            distance = abs(row - mid) / max(1, half + 1)
            glow = lerp(colours.foreground, colours.background, 0.35 + 0.65 * distance)
            s.write(s.at(row, 1), bg(*glow), " " * STAGE_W)
        s.write(RESET, SYNC_OFF)
        s.pause(0.05)
    s.pause(0.1)
    s.write(CLEAR)


LEADER_DIGITS = {
    "3": ["11111", "....1", "....1", "01111", "....1", "....1", "11111"],
    "2": ["11111", "....1", "....1", "11111", "1....", "1....", "11111"],
    "1": ["..1..", ".11..", "..1..", "..1..", "..1..", "..1..", "11111"],
}


def leader_frame(s, digit, sweep):
    """One frame of a film leader: the clock-hand sweep through a ringed count."""
    cx, cy = 40.5, 12.5
    dark, lit, ring, ink = (34, 34, 34), (92, 92, 92), (150, 150, 150), (240, 240, 240)
    glyph = LEADER_DIGITS[digit]
    s.write(SYNC_ON)
    for row in range(1, STAGE_H + 1):
        line = [s.at(row, 1)]
        current = None
        for col in range(1, STAGE_W + 1):
            dx, dy = (col - cx) / 2.5, row - cy
            radius = math.hypot(dx, dy)
            angle = (math.atan2(dx, -dy)) % 6.2832
            colour = (0, 0, 0)
            if radius <= 9.0:
                colour = lit if angle <= sweep else dark
                if abs(radius - 9.0) < 0.6 or (abs(dx) < 0.3 and radius > 2.5) or (abs(dy) < 0.3 and radius > 2.5):
                    colour = ring
            gx, gy = (col - 36) // 2, row - 9
            if 0 <= gx < 5 and 0 <= gy < 7 and glyph[gy][gx] == "1":
                colour = ink
            if colour != current:
                line.append(bg(*colour))
                current = colour
            line.append(" ")
        s.write("".join(line))
    s.write(RESET, SYNC_OFF)


def chapter_title(s):
    """The opening: the name in double height, the title, and a leader counting down."""
    s.write(light(s, INK), HIDE, CLEAR)
    if s.tracker and not s.quiet:
        download_drums(s)
    s.pause(0.3)
    music(s, ("lead", "pad"))
    steps = 1 if s.reduced else 8
    for step in range(1, steps + 1):
        t = step / steps
        s.write(SYNC_ON, headline(s, 10, "UBIQUITTY", lerp(INK.background, ACCENT, t)))
        s.write(s.centred(13, fg(*lerp(INK.background, INK.foreground, 0.45 * t)) + "p r e s e n t s" + RESET), SYNC_OFF)
        s.pause(0.08)
    s.pause(2.2 if not s.short else 1.4)
    if not s.reduced:
        for step in range(7, -1, -1):
            t = step / 8
            s.write(SYNC_ON, headline(s, 10, "UBIQUITTY", lerp(INK.background, ACCENT, t)),
                    s.centred(13, fg(*lerp(INK.background, INK.foreground, 0.45 * t)) + "p r e s e n t s" + RESET), SYNC_OFF)
            s.pause(0.05)
    s.write(CLEAR)
    for step in range(1, steps + 1):
        t = step / steps
        s.write(SYNC_ON, headline(s, 9, "THE TERMINAL", lerp(INK.background, INK.foreground, t)))
        colour = fg(*lerp(INK.background, INK.foreground, 0.6 * t))
        if s.sized_text:
            s.write(proportional_heading(s, 13, "sixty years, one screen", lerp(INK.background, INK.foreground, 0.6 * t)), SYNC_OFF)
        else:
            s.write(s.centred(13, colour + "sixty years, one screen" + RESET), SYNC_OFF)
        s.pause(0.08)
    s.pause(1.4)
    if not s.reduced:
        for step in range(7, -1, -1):
            t = step / 8
            s.write(SYNC_ON, headline(s, 9, "THE TERMINAL", lerp(INK.background, INK.foreground, t)))
            s.write(proportional_heading(s, 13, "sixty years, one screen", lerp(INK.background, INK.foreground, 0.6 * t))
                    if s.sized_text else s.centred(13, fg(*lerp(INK.background, INK.foreground, 0.6 * t)) + "sixty years, one screen" + RESET))
            s.write(SYNC_OFF)
            s.pause(0.05)
    s.write(CLEAR)
    if s.short or s.reduced:
        return
    for digit in "321":
        sound(s, [A5], 2, 3)
        for step in range(1, 13):
            leader_frame(s, digit, 6.2832 * step / 12)
            s.pause(0.07)
    s.write(CLEAR)
    sound(s, [A6], 3, 3)
    s.pause(0.3)


def type_out(s, row, col, text, cps, jitter=0.35, clatter=False):
    """Types `text` at `cps` characters a second.

    With `clatter` the machine is heard: the print hammer's tick on each
    character, a short low note on one of two pitches so it clatters rather
    than sings, softer for the space bar, and no more than a dozen or so a
    second however fast the line comes.
    """
    s.write(s.at(row, col))
    every = max(1, round(cps / 12.0))
    for index, ch in enumerate(text):
        s.write(ch)
        if clatter and not s.quiet and index % every == 0:
            s.write(decps([C5] if ch == " " else [(C5, D5)[index % 2]], 1, 1 if ch == " " else 2))
        s.pause((1.0 / cps) * (1 + random.uniform(-jitter, jitter)))


def scroll_region(s, top, bottom, left=1, right=STAGE_W):
    """A four-sided scroll region in stage coordinates, cursor at its foot."""
    return (CSI + "?69h" + CSI + "%d;%ds" % (s.tcol(left), s.tcol(right)) + CSI + "%d;%dr" % (s.trow(top), s.trow(bottom))
            + s.at(bottom, left))


NO_REGION = CSI + "r" + CSI + "?69l"


# ---------------------------------------------------------------------------
# The chapters
# ---------------------------------------------------------------------------


def chapter_teletype(s):
    cut(s, PAPER, "crossfade")
    card(s, PAPER, "I", "1963", "THE TELETYPE", "a typewriter with a wire")
    s.write(HIDE)
    row = 3
    lines = [
        ("TELETYPE MODEL 33 ASR        110 BAUD        1963", 90),
        ("", 0),
        ("TEN CHARACTERS A SECOND. UPPER CASE ONLY.", 28),
        ("NO SCREEN AND NO CURSOR: THE OUTPUT IS PAPER,", 28),
        ("AND PAPER DOES NOT ERASE.", 28),
        ("", 0),
        ("READY", 40),
        (">PRINT \"HELLO, WORLD\"", 11),
        ("HELLO, WORLD", 30),
        (">", 11),
    ]
    if s.short:
        lines = lines[:1] + lines[5:]
    for text, cps in lines:
        if text:
            type_out(s, row, 6, text, cps, clatter=True)
            if not s.quiet:
                # The carriage flies back: a longer, lower thump than a key.
                s.write(decps([C5], 4, 2))
        if text == "HELLO, WORLD":
            s.write(BEL)
        s.pause(0.22)
        row += 1
    s.pause(1.0)
    # The tear-off: the page rides up and out of the frame.
    if not s.reduced:
        s.write(scroll_region(s, 1, STAGE_H))
        for _ in range(STAGE_H):
            s.write(ESC + "D")
            s.pause(0.03)
        s.write(NO_REGION)
    s.pause(0.3)


TEK_ENTER = CSI + "?38h"
TEK_EXIT = ESC + "\x03"
TEK_PAGE = ESC + "\x0c"
GS = "\x1d"
US = "\x1f"
TEK_SOLID, TEK_DOTTED, TEK_DASHDOT, TEK_SHORT, TEK_LONG = (ESC + c for c in "`abcd")
TEK_LARGE, TEK_MEDIUM, TEK_SMALL = ESC + "8", ESC + "9", ESC + ";"


def tek_addr(x, y):
    x, y = max(0, min(4095, int(x))), max(0, min(3119, int(y)))
    return "".join(chr(b) for b in (0x20 | ((y >> 7) & 0x1F), 0x60 | ((y >> 2) & 0x1F), 0x20 | ((x >> 7) & 0x1F), 0x40 | ((x >> 2) & 0x1F)))


def tek_poly(points):
    return GS + "".join(tek_addr(x, y) for x, y in points)


def tek_text(x, y, text):
    """Alpha-mode text with its cell's bottom-left at (x, y): the tube letters it."""
    return GS + tek_addr(x, y) + US + text


# A stroke alphabet on a four-by-six grid, y up, for the title: plotted as
# vectors, letter by letter, the way a plotter drew a drawing's title block.
STROKES = {
    "E": [[(4, 0), (0, 0), (0, 6), (4, 6)], [(0, 3), (3, 3)]],
    "I": [[(1, 6), (3, 6)], [(2, 6), (2, 0)], [(1, 0), (3, 0)]],
    "K": [[(0, 0), (0, 6)], [(4, 6), (0, 2)], [(1.4, 3.4), (4, 0)]],
    "N": [[(0, 0), (0, 6), (4, 0), (4, 6)]],
    "O": [[(1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0)]],
    "R": [[(0, 0), (0, 6), (3, 6), (4, 5), (4, 4), (3, 3), (0, 3)], [(2, 3), (4, 0)]],
    "T": [[(0, 6), (4, 6)], [(2, 6), (2, 0)]],
    "X": [[(0, 0), (4, 6)], [(0, 6), (4, 0)]],
    "0": [[(1, 0), (3, 0), (4, 1), (4, 5), (3, 6), (1, 6), (0, 5), (0, 1), (1, 0)], [(1, 1), (3, 5)]],
    "1": [[(1, 5), (2, 6), (2, 0)], [(1, 0), (3, 0)]],
    "4": [[(3, 0), (3, 6), (0, 2), (4, 2)]],
    " ": [],
}


def tek_lettering(x, y, text, unit):
    """Vector strokes for ``text`` with its baseline at (x, y), one glyph per element."""
    glyphs = []
    for index, ch in enumerate(text):
        left = x + index * 6 * unit
        glyphs.append("".join(tek_poly([(left + px * unit, y + py * unit) for px, py in stroke]) for stroke in STROKES.get(ch, [])))
    return glyphs


def globe_curves(cx, cy, radius, tilt, spin=0.0):
    """Latitude and longitude polylines of a sphere, front hemisphere only, turned by `spin`."""
    ct, st = math.cos(tilt), math.sin(tilt)

    def project(lat, lon):
        x, y, z = math.cos(lat) * math.sin(lon + spin), math.sin(lat), math.cos(lat) * math.cos(lon + spin)
        y, z = y * ct - z * st, y * st + z * ct
        return (cx + radius * x, cy + radius * y, z)

    curves = []
    for lat in (-1.05, -0.7, -0.35, 0.0, 0.35, 0.7, 1.05):
        curves.append([project(lat, 6.2832 * i / 48) for i in range(49)])
    for lon in range(0, 12):
        curves.append([project(-1.5708 + 3.1416 * i / 32, 3.1416 * lon / 6) for i in range(33)])
    out = []
    for curve in curves:
        run = []
        for x, y, z in curve:
            if z >= -0.02:
                run.append((x, y))
            elif run:
                out.append(run)
                run = []
        if run:
            out.append(run)
    out.append([(cx + radius * math.cos(6.2832 * i / 64), cy + radius * math.sin(6.2832 * i / 64)) for i in range(65)])
    return out


def cube_edges(cx, cy, size, yaw=0.55, pitch=0.38):
    ca, sa, cb, sb = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    corners = []
    for i in range(8):
        x, y, z = (1 if i & 1 else -1), (1 if i & 2 else -1), (1 if i & 4 else -1)
        x, z = x * ca + z * sa, -x * sa + z * ca
        y, z = y * cb - z * sb, y * sb + z * cb
        corners.append((cx + size * x, cy + size * y, z))
    back = min(range(8), key=lambda i: corners[i][2])
    edges = []
    for i in range(8):
        for bit in (1, 2, 4):
            j = i | bit
            if j != i:
                edges.append((i, j, back in (i, j)))
    return corners, edges


def tek_cube(cx, cy, size, yaw):
    """The cube's edges as one Tek string, hidden ones dotted."""
    corners, edges = cube_edges(cx, cy, size, yaw)
    out = []
    style = None
    for i, j, hidden in edges:
        wanted = TEK_DOTTED if hidden else TEK_SOLID
        if wanted != style:
            out.append(wanted)
            style = wanted
        out.append(tek_poly([corners[i][:2], corners[j][:2]]))
    out.append(TEK_SOLID)
    return "".join(out)


GLOBE = (1150, 1350, 780, 0.42)
CUBE = (3000, 1350, 580)


def tek_scene(spin, yaw):
    """Everything on the tube for one frame: the titles, the globe turned by `spin`, the cube at `yaw`, the plot."""
    out = [TEK_SOLID]
    out.extend(tek_lettering(380, 2760, "TEKTRONIX 4014", 40))
    out.append(TEK_LARGE + tek_text(380, 2500, "HELLO, WORLD"))
    out.append(TEK_MEDIUM + tek_text(380, 2380, "STORAGE TUBE - 1974 - THE PICTURE PERSISTS UNTIL THE PAGE IS ERASED"))
    out.append(TEK_SHORT + tek_poly([(300, 2300), (3820, 2300)]) + TEK_SOLID)
    out.extend(tek_poly(curve) for curve in globe_curves(*GLOBE, spin=spin))
    out.append(TEK_LARGE + tek_text(900, 380, "GLOBE"))
    out.append(tek_cube(*CUBE, yaw))
    out.append(tek_text(2820, 380, "CUBE"))
    out.append(TEK_DASHDOT + tek_poly([(300, 220), (3820, 220)]) + tek_poly([(300, 60), (300, 380)]))
    out.append(TEK_SOLID + tek_poly([(300 + 3520 * i / 140, 220 + 130 * math.sin(i / 140 * 18.85)) for i in range(141)]))
    out.append(TEK_SMALL + tek_text(3500, 280, "SINE"))
    return "".join(out)


def chapter_tektronix(s):
    cut(s, PHOSPHOR, "iris")
    # The tube is lit in the same phosphor as the window around it.
    s.write(OSC + "16;" + rgb_spec(*PHOSPHOR.background) + ST, OSC + "15;" + rgb_spec(*PHOSPHOR.foreground) + ST)
    card(s, PHOSPHOR, "II", "1974", "TEKTRONIX 4014", "a storage tube draws in vectors, and remembers them", from_year="1963")
    s.queue_ahead(chapter_tektronix)
    s.write(TEK_ENTER, TEK_PAGE, TEK_SOLID)
    s.in_tek = True
    music(s, ("hat", "ohat", "bass"))
    sound(s, [C6, A5, G5, E5, D5, C5], 1, 2)
    for glyph in tek_lettering(380, 2760, "TEKTRONIX 4014", 40):
        s.write(glyph)
        s.pause(0.04 if not s.short else 0.02)
    # The greeting and the caption are the tube's own alpha mode, in its
    # largest and its smallest character sizes; the beam letters them.
    s.write(TEK_LARGE, tek_text(380, 2500, "HELLO, WORLD"))
    sound(s, [E6, A6], 2, 2)
    s.write(TEK_MEDIUM, tek_text(380, 2380, "STORAGE TUBE - 1974 - THE PICTURE PERSISTS UNTIL THE PAGE IS ERASED"))
    s.write(TEK_SHORT, tek_poly([(300, 2300), (3820, 2300)]))
    s.pause(0.4)

    s.write(TEK_SOLID)
    for index, curve in enumerate(globe_curves(*GLOBE)):
        s.write(tek_poly(curve))
        if index % 4 == 0:
            sound(s, [E5 + (index % 5)], 1, 1)
        s.pause(0.05 if not s.short else 0.02)
    s.write(TEK_LARGE, tek_text(900, 380, "GLOBE"))
    sound(s, [G5], 2, 2)
    s.pause(0.3)

    corners, edges = cube_edges(*CUBE)
    for i, j, hidden in edges:
        s.write(TEK_DOTTED if hidden else TEK_SOLID)
        s.write(tek_poly([corners[i][:2], corners[j][:2]]))
        s.pause(0.07 if not s.short else 0.03)
    s.write(TEK_SOLID, tek_text(2820, 380, "CUBE"))
    sound(s, [A5], 2, 2)
    s.pause(0.3)

    s.write(TEK_DASHDOT, tek_poly([(300, 220), (3820, 220)]), tek_poly([(300, 60), (300, 380)]))
    s.write(TEK_SOLID, tek_poly([(300 + 3520 * i / 140, 220 + 130 * math.sin(i / 140 * 18.85)) for i in range(141)]))
    s.write(TEK_SMALL, tek_text(3500, 280, "SINE"))
    sound(s, [C6], 2, 2)
    s.pause(1.2)

    # The page is erased and rewritten twenty-four times over: each PAGE
    # commits the frame drawn since the last one and clears the tube for
    # the next, so the globe and the cube turn while everything around
    # them is redrawn in place.
    frames = 8 if s.reduced else (14 if s.short else 24)
    for frame in range(1, frames + 1):
        s.write(TEK_PAGE, tek_scene(6.2832 * frame / frames / 4, 0.55 + 6.2832 * frame / frames))
        s.pause(0.08)
    s.write(TEK_PAGE)
    s.pause(1.6)
    # The page is erased, the way a storage tube ends a picture, and the
    # tube is left before the next era's curtain closes over the grid.
    s.write(TEK_PAGE)
    s.pause(0.4)
    s.write(TEK_EXIT, OSC + "115" + ST, OSC + "116" + ST)
    s.in_tek = False


def dec_box(s, top, left, bottom, right, title=""):
    """A frame from the DEC special graphics set, the VT100's line drawing."""
    width = right - left - 1
    s.write(ESC + "(0")
    s.write(s.at(top, left), "l" + "q" * width + "k")
    for row in range(top + 1, bottom):
        s.write(s.at(row, left), "x", s.at(row, right), "x")
    s.write(s.at(bottom, left), "m" + "q" * width + "j")
    s.write(ESC + "(B")
    if title:
        s.write(s.at(top, left + 2), title)


DA1_FEATURES = {
    1: "132 columns", 2: "a printer port", 4: "sixel graphics", 6: "selective erase",
    9: "national character sets", 15: "the technical set", 18: "user windows",
    21: "horizontal scrolling", 22: "ANSI colour", 28: "rectangular editing",
}
DA1_LEVELS = {1: "VT100", 6: "VT102", 62: "VT200", 63: "VT300", 64: "VT400", 65: "VT500"}


def describe_da1(reply):
    numbers = [int(n) for n in re.findall(r"\d+", reply)]
    if not numbers:
        return "something that did not say"
    level = DA1_LEVELS.get(numbers[0], "a VT%d" % numbers[0])
    features = [DA1_FEATURES[n] for n in numbers[1:] if n in DA1_FEATURES]
    if len(features) > 4:
        features = features[:4] + ["%d more" % (len(features) - 4)]
    if not features:
        return "a " + level
    if len(features) == 1:
        return "a %s with %s" % (level, features[0])
    return "a %s with %s and %s" % (level, ", ".join(features[:-1]), features[-1])


def wrap(text, width):
    words, lines, line = text.split(), [], ""
    for word in words:
        if len(line) + len(word) + 1 > width and line:
            lines.append(line)
            line = word
        else:
            line = (line + " " + word).strip()
    if line:
        lines.append(line)
    return lines


def chapter_vt100(s):
    cut(s, PHOSPHOR, "curtain")
    card(s, PHOSPHOR, "III", "1978", "VT100", "the screen learns to erase", from_year="1974")
    sound(s, [C5], 12, 1)
    crt_power_on(s, PHOSPHOR)
    music(s, ("kick", "hat", "snare", "bass"))
    s.write(SHOW, CSI + "1 q")
    title = "DIGITAL VT100"
    s.write(s.line_start(2), ESC + "#6", s.dwl_pad(title), BOLD, title, RESET)
    s.write(s.at(5, 4), DIM, "ATTRIBUTES", RESET, "   ", BOLD, "BOLD", RESET, "   ", sgr(4), "UNDERLINE", RESET,
            "   ", sgr(5), "BLINK", RESET, "   ", sgr(7), " REVERSE ", RESET, "   ", sgr(1, 4, 7), " ALL AT ONCE ", RESET)
    s.pause(0.6)

    # The same program as the paper's, in a scroll region the frame stays
    # around: the screen erases what the paper could not.
    dec_box(s, 7, 4, 15, 77, " BASIC ")
    s.write(s.at(7, 60), DIM, " DECSTBM 8;14 ", RESET)
    s.write(scroll_region(s, 8, 14, 6, 75))
    program = ["10 PRINT \"HELLO, WORLD\"", "20 GOTO 10", "RUN"]
    for line in program:
        type_out(s, 14, 6, line, 30 if not s.short else 60)
        s.write(ESC + "D", "\r", CSI + "%dG" % s.tcol(6))
        sound(s, [G6], 1, 2)
        s.pause(0.2)
    for _ in range(14 if not s.short else 8):
        s.write("HELLO, WORLD", ESC + "D", "\r", CSI + "%dG" % s.tcol(6))
        sound(s, [G6], 1, 1)
        s.pause(0.12)
    s.write("^C", ESC + "D", "\r", CSI + "%dG" % s.tcol(6), "READY", NO_REGION, s.at(14, 11))
    s.pause(0.6)

    s.write(s.at(17, 4), DIM, "WHO ARE YOU?   ESC [ c", RESET)
    reply = s.query(CSI + "c", rb"\x1b\[\?[\d;]*c") if s.is_live else CSI + "?64;1;2;4;6;9;15;18;21;22;28c"
    answer = describe_da1(reply) if reply else "no answer came back"
    sound(s, [G5, C6], 2, 2)
    for index, line in enumerate(wrap("THE TERMINAL ANSWERS: IT IS " + answer.upper() + ".", 70)[:2]):
        s.write(s.at(18 + index, 6), line)
    s.pause(0.8)
    s.write(s.at(20, 4), DIM, "WHERE IS THE CURSOR?   ESC [ 6 n", RESET)
    s.write(s.at(21, 6))
    reply = s.query(CSI + "6n", rb"\x1b\[\d+;\d+R") if s.is_live else CSI + "%d;%dR" % (s.trow(21), s.tcol(6))
    numbers = re.findall(r"\d+", reply or "")
    s.write("ROW %s, COLUMN %s." % (numbers[0], numbers[1]) if len(numbers) == 2 else "NO ANSWER CAME BACK.")
    s.pause(0.8)
    if s.short:
        return

    s.write(s.at(23, 4), DIM, "CURSOR SHAPES", RESET, "   ")
    for code, name in ((2, "BLOCK"), (4, "UNDERLINE"), (6, "BAR"), (5, "BLINKING BAR"), (3, "BLINKING UNDERLINE"), (1, "BLINKING BLOCK")):
        s.write(s.at(23, 21), CSI + "K", name, "  ", CSI + "%d q" % code)
        s.pause(0.45)
    s.pause(1.0)
    if s.reduced:
        return
    # The VT100's other width. DECCOLM makes the page 132 columns wide and
    # clears it, so the moment is the chapter's last: a ruler across every
    # column, then the page comes back to eighty.
    s.write(s.at(23, 4), CSI + "K", DIM, "132 COLUMNS", RESET, "   ESC [ ? 3 h")
    s.pause(0.9)
    # DECCOLM is ignored until mode 40 allows it; restore_width resets 40.
    s.write(HIDE, CSI + "?40h" + CSI + "?3h")
    s.wide = True
    s.window_columns = s.columns
    s.reframe(132)
    ruler = "".join(("%d" % (col // 10 * 10)).rjust(10) for col in range(10, 140, 10))[:132]
    s.write(CSI + "%d;1H" % s.trow(10), fg(*lerp(PHOSPHOR.background, PHOSPHOR.foreground, 0.6)), ruler, RESET)
    s.write(CSI + "%d;1H" % s.trow(11), "┈┈┈┈┈┈┈┈┈┼" * 13 + "┈┈")
    s.write(s.centred(13, BOLD + "ONE HUNDRED AND THIRTY-TWO COLUMNS" + RESET, col=1 - s.col0, width=132))
    s.write(s.centred(14, DIM + "the VT100's other width, DECCOLM: the same window, the cells narrowed to fit" + RESET, col=1 - s.col0, width=132))
    s.pause(2.4)
    s.restore_width()
    s.pause(0.3)


SOFT_GLYPHS = {
    "heart": ["........", ".##..##.", "########", "########", "########", ".######.", "..####..", "...##...", "........", "........"],
    "smile": ["..####..", ".#....#.", "#.#..#.#", "#......#", "#.#..#.#", "#..##..#", ".#....#.", "..####..", "........", "........"],
    "screen": ["########", "#......#", "#.#.#..#", "#......#", "#......#", "########", "...##...", ".######.", "........", "........"],
}


def decdld(glyphs):
    """DECDLD: a soft set of 8x10 bitmaps, two sixel bands per glyph.

    Pcn is 1 so the first bitmap lands on ``!``, and the set's name carries
    the SP intermediate the designator will use.
    """
    encoded = []
    for rows in glyphs:
        columns = [0] * 8
        for r, row in enumerate(rows):
            for c, cell in enumerate(row):
                if cell == "#":
                    columns[c] |= 1 << r
        bands = []
        for band in range(2):
            bands.append("".join(chr(0x3F + ((col >> (band * 6)) & 0x3F)) for col in columns))
        encoded.append("/".join(bands))
    return DCS + "1;1;2;8;0;0;10;0{ A" + ";".join(encoded) + ST


def chapter_vt220(s, pictures):
    cut(s, AMBER, "slide")
    card(s, AMBER, "IV", "1983", "VT220 · VT240 · VT320", "soft fonts, pictures, a status line, and erase that spares", from_year="1978")
    s.write(HIDE)
    music(s, ("kick", "hat", "snare", "bass", "stab"))
    s.write(CSI + "2$~", CSI + "1$}", sgr(7), "  VT320   HOST-WRITABLE STATUS LINE   DECSSDT 2   ".ljust(s.columns), RESET, CSI + "0$}")
    s.pause(1.2)

    s.write(s.at(2, 4), DIM, "SOFT FONT", RESET, "     ", decdld(list(SOFT_GLYPHS.values())), ESC + ") A", SO, "!  \"  #", SI,
            "     three bitmaps downloaded as a soft set, DECDLD")
    sound(s, [F5, F5, F5], 1, 2)
    s.pause(0.8)

    s.write(s.at(4, 4), DIM, "SELECTIVE ERASE", RESET, "   DECSCA marks the cells that survive an erase")
    protect, plain = CSI + '1"q', CSI + '0"q'
    s.write(s.at(6, 4), "the program said   ", protect, "HELLO", plain, "   to the   ", protect, "WORLD", plain, "   and the words")
    s.write(s.at(7, 4), "were marked; when the rest of the line is erased, the   ", protect, "ERASE", plain)
    s.write(s.at(8, 4), "cannot touch them, and what   ", protect, "SURVIVES", plain, "   is exactly the greeting.")
    s.pause(1.2)
    for row in (6, 7, 8):
        s.write(s.at(row, 1), CSI + "?2K")
        sound(s, [D5], 2, 2)
        s.pause(0.3)
    s.pause(0.4)

    # The picture develops a band at a time, the way one arrived down a
    # serial line: the same Sixel, sent again with more of itself each time.
    width, height, rgba = pictures.sunset
    s.write(s.at(11, 46), DIM, "SIXEL   VT240, 1983", RESET)
    # Six passes rather than one per band: every pass is an image of its
    # own in the terminal's store, and the store holds a bounded number.
    band_count = (height + 5) // 6
    steps = (3, 6, 9, 12, 15, band_count) if not s.reduced else (band_count,)
    for bands in steps:
        # Each pass is a fresh image at the same anchor; the cells under the
        # last one are cleared first so the newer picture is the only one there.
        for row in range(12, 22):
            s.write(s.at(row, 44), " " * 36)
        s.write(s.at(12, 44), sixel_image(width, height, rgba, bands=bands))
        if bands % 4 == 1:
            sound(s, [C5 + bands], 1, 1)
        s.pause(0.06)
    sound(s, [C5, E5, G5, C6], 2, 2)
    s.pause(0.6)

    s.write(s.at(11, 4), DIM, "RECTANGLES", RESET, "   VT420: fill, copy, restyle")
    s.write(CSI + "2*x", sgr(7))
    for left, top in ((5, 20), (10, 17), (15, 14)):
        # Each bar rises: the fill grows from the baseline in three steps.
        for step in (3, 2, 1):
            row = 22 - (22 - top) // step
            s.write(CSI + "35;%d;%d;%d;%d$x" % (s.trow(row), s.tcol(left), s.trow(22), s.tcol(left + 3)))
            s.pause(0.05)
        sound(s, [D6], 1, 2)
        s.pause(0.15)
    s.write(RESET)
    s.pause(0.4)
    s.write(CSI + "%d;%d;%d;%d;1;%d;%d;1$v" % (s.trow(13), s.tcol(4), s.trow(22), s.tcol(19), s.trow(13), s.tcol(24)))
    s.write(s.at(23, 24), DIM, "copied with DECCRA", RESET)
    s.pause(0.7)
    s.write(CSI + "%d;%d;%d;%d;27$r" % (s.trow(13), s.tcol(24), s.trow(22), s.tcol(39)))
    s.write(s.at(23, 24), CSI + "K", DIM, "restyled with DECCARA", RESET)
    s.pause(2.0)
    s.write(CSI + "0$~", ESC + ")B")


def chapter_xterm(s):
    cut(s, THEME, "slide")
    card(s, THEME, "V", "1996", "XTERM", "colour, a title, and a mouse on the Unix desktop", from_year="1983")
    music(s, ("kick", "hat", "ohat", "snare", "bass", "arp"))
    s.write(HIDE, CSI + "22;0t", OSC + "2;xterm - the evolution of the terminal" + ST)
    s.write(s.at(2, 4), DIM, "16 COLOURS ", RESET, "    ")
    for code in list(range(40, 48)) + list(range(100, 108)):
        s.write(sgr(code), "   ", RESET)
        s.pause(0.02)
    s.write(s.at(4, 4), DIM, "256 COLOURS", RESET, "    1999: a 6x6x6 cube and 24 greys")
    for g in range(6):
        s.write(s.at(5 + g, 8))
        for r in range(6):
            for b in range(6):
                s.write(CSI + "48;5;%dm  " % (16 + 36 * r + 6 * g + b))
        s.write(RESET)
        sound(s, [C5 + g * 2], 1, 1)
        s.pause(0.06)
    s.write(s.at(11, 8))
    for grey in range(24):
        s.write(CSI + "48;5;%dm   " % (232 + grey))
    s.write(RESET)
    s.pause(0.8)

    s.write(s.at(13, 4), BOLD, "$ ", RESET)
    type_out(s, 13, 6, "ls -l --color", 40)
    s.write(s.at(14, 4), "-rw-r--r--  ", "hello.c", "   ", "Makefile", "   ", "README")
    s.write(s.at(15, 4), "-rwxr-xr-x  ", sgr(32), "hello", RESET, "*   ", sgr(32), "build.sh", RESET, "*")
    s.write(s.at(16, 4), "drwxr-xr-x  ", sgr(1, 34), "src", RESET, "/   ", sgr(1, 34), "docs", RESET, "/   ", sgr(36), "latest", RESET, " -> releases/1.0")
    s.pause(0.5)
    s.write(s.at(17, 4), BOLD, "$ ", RESET)
    type_out(s, 17, 6, "./hello", 40)
    s.write(s.at(18, 4), sgr(1, 32), "hello, world", RESET)
    sound(s, [E6, A6], 2, 2)
    s.pause(0.8)

    s.write(s.at(20, 4), DIM, "MOUSE", RESET, "   click anywhere: the terminal says where  ", DIM, "xterm mouse 1000 · SGR 1006", RESET)
    s.write(CSI + "?1000h", CSI + "?1006h")
    clicks = 0
    for event in s.events(6.0):
        if event[0] != "mouse":
            continue
        button, col, row, pressed = event[1:]
        if not pressed or button >= 64:
            continue
        clicks += 1
        sound(s, [C6], 2, 2)
        s.write(CSI + "%d;%dH" % (row, col), sgr(1, 33), "✦︎", RESET)
        s.write(s.at(22, 4), CSI + "K", "button %d at row %d, column %d" % (button, row - s.row0, col - s.col0))
    if s.is_live and not clicks:
        s.write(s.at(22, 4), DIM, "no click; the mouse is yours again", RESET)
    s.write(CSI + "?1006l", CSI + "?1000l")
    s.pause(1.0)
    s.write(CSI + "23;0t")


BRAILLE_BITS = ((0x01, 0x08), (0x02, 0x10), (0x04, 0x20), (0x40, 0x80))


def braille_frame(columns, rows, phase):
    """A wave plotted two dots wide and four high per cell, coloured by column."""
    width, height = columns * 2, rows * 4
    cells = [[0] * columns for _ in range(rows)]
    for x in range(width):
        centre = height / 2 + (height / 2 - 1.5) * math.sin(x / 9.0 + phase) * math.cos(x / 41.0 - phase / 3)
        for y in (int(centre), int(centre) + 1):
            if 0 <= y < height:
                cells[y // 4][x // 2] |= BRAILLE_BITS[y % 4][x % 2]
    lines = []
    for row in range(rows):
        line = []
        for col in range(columns):
            line.append(fg(*hsv((col / columns + phase / 12) % 1.0, 0.7, 1.0)) + chr(0x2800 + cells[row][col]))
        lines.append("".join(line))
    return lines


HELLO_SCRIPTS = [
    "hello, world  ·  こんにちは、世界  ·  안녕, 세상  ·  Привет, мир",
    "Γεια σου, κόσμε  ·  नमस्ते दुनिया  ·  مرحبا بالعالم  ·  สวัสดีชาวโลก",
]


def chapter_unicode(s):
    cut(s, THEME, "curtain")
    card(s, THEME, "VI", "2003", "UNICODE", "hello, world in every script; the grid still counts cells", from_year="1996")
    s.write(HIDE)
    label = lambda row, text: s.at(row, 3) + DIM + text.ljust(10) + RESET
    music(s, ("kick", "hat", "ohat", "snare", "bass", "arp", "lead", "fills"))
    fade_in(s, THEME, [(2, 13, HELLO_SCRIPTS[0], 1.0), (3, 13, HELLO_SCRIPTS[1], 1.0)], steps=6, hold=0.05)
    s.write(label(2, "hello"))
    s.pause(0.6)
    rows = [
        (5, "emoji", "😀 🚀 🎨 🦉 🌊   👩‍💻 👨‍👩‍👧   🇨🇦 🇧🇷 🇯🇵 🇮🇸   👍🏽 ✌🏿   ☂️ ☂︎"),
        (7, "marks", "é ñ ộ ǚ ą̈ ậ   Z̤̈a̘̼l̯̗g̙̙o   ∑ ∫ √ ∞ ≠ ≤ ≥ ⌈x⌉"),
    ]
    for row, name, text in rows:
        fade_in(s, THEME, [(row, 13, text, 1.0)], steps=4, hold=0.04)
        s.write(label(row, name))
    if s.short:
        s.pause(1.2)
        return
    s.write(label(9, "boxes"), "┌──┐ ┏━━┓ ╔══╗ ╭──╮   ░▒▓█  ▁▂▃▄▅▆▇█  ▏▎▍▌▋▊▉  🬀🬁🬂🬃🬄🬅  ⣿⣷⣯⣟")
    s.write(s.at(10, 13), "│  │ ┃  ┃ ║  ║ │  │", DIM, s.at(10, 35), "shades", s.at(10, 42), "blocks", s.at(10, 51), "eighths",
            s.at(10, 60), "sextant", s.at(10, 68), "braille", RESET)
    s.write(s.at(11, 13), "└──┘ ┗━━┛ ╚══╝ ╰──╯")
    s.write(label(13, "rules"), sgr("4:1"), "single", RESET, "  ", sgr("4:2"), "double", RESET, "  ", sgr("4:3"), "curly", RESET, "  ",
            sgr("4:4"), "dotted", RESET, "  ", sgr("4:5"), "dashed", RESET, "  ", sgr("4:3", "58;2;255;90;90"), "coloured", RESET, "  ",
            sgr(53), "overline", RESET)
    s.write(label(14, "styles"), BOLD, "bold", RESET, "  ", DIM, "faint", RESET, "  ", ITALIC, "italic", RESET, "  ", sgr(9), "struck", RESET,
            "  ", sgr(7), " inverse ", RESET, "  x", sgr(73), "2", sgr(75), "  H", sgr(74), "2", sgr(75), "O")
    arrow = chr(0xE0B0)
    s.write(label(15, "powerline"), bg(40, 40, 60), fg(220, 225, 240), " ~/hello ",
            bg(90, 110, 200), fg(40, 40, 60), arrow, fg(255, 255, 255), " main ",
            bg(60, 160, 100), fg(90, 110, 200), arrow, fg(15, 35, 22), " ✓ ",
            RESET, fg(60, 160, 100), arrow, RESET, "   ", DIM, "wedges the terminal draws itself", RESET)
    s.pause(1.0)
    s.write(label(18, "braille"))
    frames = 12 if s.reduced else 48
    for step in range(frames):
        frame = braille_frame(64, 4, step * 0.22)
        s.write(SYNC_ON)
        for row, line in enumerate(frame):
            s.write(s.at(17 + row, 13), line)
        s.write(RESET, SYNC_OFF)
        if step % 12 == 0:
            sound(s, [C6 + (step // 12) * 2], 2, 1)
        s.pause(0.04)
    s.write(s.at(22, 13), DIM, "two by four dots per cell, drawn under synchronized output (2026h)", RESET)
    s.pause(1.4)


def chapter_truecolor(s):
    """A plasma on the whole stage, each cell two pixels tall."""
    cut(s, THEME, "iris")
    card(s, THEME, "VII", "2012", "TRUECOLOR", "sixteen million colours, two to a cell", from_year="2003")
    s.write(HIDE)
    palette = [hsv(i / 256.0, 0.85, 1.0) for i in range(256)]
    music(s)
    frames = 8 if s.reduced else (16 if s.short else 40)
    for step in range(frames):
        t = step * 0.12
        s.write(SYNC_ON)
        for row in range(1, STAGE_H + 1):
            line = [s.at(row, 1)]
            for col in range(1, STAGE_W + 1):
                colours = []
                for half in (0, 1):
                    x, y = col, row * 2 + half
                    v = (math.sin(x / 9.0 + t) + math.sin(y / 7.0 - t * 0.7) + math.sin((x + y) / 11.0 + t * 0.5)
                         + math.sin(math.hypot(x - 40, y - 24) / 6.0 - t))
                    colours.append(palette[int((v + 4) / 8 * 255) & 255])
                line.append(fg(*colours[0]) + bg(*colours[1]) + "▀")
            s.write("".join(line))
        s.write(RESET, s.centred(12, bg(0, 0, 0) + BOLD + fg(255, 255, 255) + "   HELLO, WORLD  ·  16,777,216 COLOURS   " + RESET))
        s.write(s.centred(13, bg(0, 0, 0) + DIM + fg(255, 255, 255) + "  truecolor SGR 38;2 / 48;2, 2012  " + RESET), SYNC_OFF)
        if step == 13:
            sound(s, list(range(D6, C7 + 1)), 1, 2)
        s.pause(0.045)
    s.pause(1.2)


def chapter_pictures(s, pictures):
    cut(s, THEME, "slide")
    card(s, THEME, "VIII", "2013", "PICTURES IN THE GRID", "inline images, animation, and a shell that talks back", from_year="2012")
    music(s, ("pad", "arp", "lead", "build"))
    s.write(HIDE)
    width, height, rgba = pictures.sphere
    s.write(s.at(2, 4), DIM, "OSC 1337   iTerm2, 2013", RESET, s.at(3, 4), iterm2_image(width, height, rgba, 12, 6))
    sound(s, [G6, REST, G6], 1, 2)
    s.pause(0.6)
    width, height, rgba = pictures.julia
    s.write(s.at(2, 30), DIM, "KITTY GRAPHICS   2018, a Julia set as raw RGBA", RESET, kitty_transmit(KITTY_JULIA, width, height, rgba))
    # The picture wipes in from the left: the same placement, re-placed with
    # a wider source rectangle each frame.
    steps = [12] if s.reduced else range(1, 13)
    for step in steps:
        s.write(s.at(3, 30), kitty_place(KITTY_JULIA, 48 * step // 12, 12, (0, 0, width * step // 12, height)))
        s.pause(0.05)
    sound(s, [C6, E6, G6], 1, 2)
    s.pause(0.6)
    width, height, frames = pictures.spinner
    s.write(s.at(10, 4), kitty_animation(KITTY_SPINNER, width, height, frames, 6, 3, 90))
    s.write(s.at(13, 4), DIM, "frames the terminal", RESET, s.at(14, 4), DIM, "advances itself", RESET)
    s.pause(1.0)

    prompt = lambda row, command: (s.at(row, 4) + OSC + "133;A" + ST + fg(120, 200, 255) + "❯ " + RESET + OSC + "133;B" + ST + command + OSC + "133;C" + ST)
    s.write(s.at(16, 4), DIM, "SHELL INTEGRATION   OSC 133 marks commands; 9;4 and 21337 talk to the tab", RESET)
    s.write(prompt(17, "make hello"))
    s.write(OSC + "21337;indicator=#ff9500;status=Building;status-color=#ff9500" + BEL)
    s.pause(0.4)
    s.write(s.at(18, 4), "compiling hello.c  ")
    for percent in range(0, 101, 5):
        filled = percent * 30 // 100
        s.write(s.at(18, 23), fg(90, 200, 120), "█" * filled, DIM, "░" * (30 - filled), RESET, " %3d%%" % percent)
        s.write(OSC + "9;4;1;%d" % percent + BEL)
        if percent % 25 == 0:
            sound(s, [A5 + percent // 25], 1, 2)
        s.pause(0.05)
    s.write(OSC + "9;4;0;0" + BEL, OSC + "133;D;0" + ST)
    s.write(OSC + "21337;indicator=#00d75f;status=Idle;status-color=#888888" + BEL)
    s.write(prompt(19, "./hello"))
    s.write(s.at(20, 4), sgr(1, 32), "hello, world", RESET, OSC + "133;D;0" + ST)
    s.pause(1.0)
    s.write(prompt(21, "make test"))
    s.pause(0.4)
    s.write(s.at(22, 4), fg(255, 100, 100), "1 of 40 tests failed", RESET, OSC + "133;D;2" + ST)
    s.pause(0.5)
    s.write(s.at(23, 4), DIM, "the gutter marks each prompt, red where it failed; the tab showed progress", RESET)
    s.pause(1.8)
    s.write(OSC + "21337;indicator=;status=;status-color=" + BEL)


def proportional(text, cells, align=0):
    return OSC + "66;p=1:c=%d:h=%d;" % (cells, align) + text + BEL


def sized(text, scale):
    return OSC + "66;s=%d;" % scale + text + BEL


SUP, SUB, SOFF = CSI + "73m", CSI + "74m", CSI + "75m"


def masthead(s, title, folio, colours):
    """A plate's head, the way the snapshot folio sets it: the title in spaced
    capitals, the plate's number at the right, and a rule under both."""
    spaced, number = " ".join(title), " ".join(folio)
    return (s.at(1, 3) + fg(*lerp(colours.background, colours.foreground, 0.85)) + spaced
            + s.at(1, 79 - len(number)) + number + RESET
            + s.at(2, 3) + fg(*lerp(colours.background, colours.foreground, 0.5)) + "─" * 76 + RESET)


def reveal(s, rows, hold):
    """The plate's rows, one at a time, each under synchronized output."""
    for text in rows:
        s.write(SYNC_ON, text, SYNC_OFF)
        s.pause(hold)


def chapter_typography(s):
    """The type specimen: a folio of plates on the eighty-column stage.

    The plates follow the snapshot suite's specimen folio. Plate I is sizes,
    weights and mathematics: DEC's double height and double width, OSC 66
    sized text where the terminal draws it, the SGR weights and the variable
    axis, then display settings a foundry would show. Plate II is styles,
    rules and colour; Plate III, where sized text is drawn, is proportional
    runs; Plate IV is boxes, blocks and separators. The short cut shows the
    first plate.
    """
    cut(s, THEME, "curtain")
    card(s, THEME, "IX", "TYPE", "TYPOGRAPHY", "a specimen folio, on the same eighty columns")
    s.write(HIDE)
    music(s)
    colours = s.era_colours(THEME)
    fade = lambda weight: fg(*lerp(colours.background, colours.foreground, weight))
    note = lambda text: fade(0.6) + text + RESET
    label = lambda row, text: s.at(row, 4) + DIM + text.ljust(11) + RESET
    it = lambda text: ITALIC + text + RESET
    hold = 0.06 if s.short else 0.1
    reading = 2.6 if s.short else 4.0

    # ---- Plate I: sizes, weights and mathematics ----------------------------
    s.write(masthead(s, "SIZES, WEIGHTS & MATH", "PLATE I", colours))
    s.pause(0.3)
    rows = [dec_headline(s, 4, "#3 #4  double height, Aa Bb Gg 0123 &", colours.foreground)]
    dwl = "#6  double width, two cells a glyph"
    rows.append(s.line_start(6) + ESC + "#6" + s.dwl_pad(dwl) + fade(0.7) + dwl + RESET)
    row = 8
    if s.sized_text:
        rows.append(label(row, "sized") + sized("Aa", 3) + "   " + sized("Bb", 2) + "   " + note("s=3 and s=2: as wide and tall as the scale"))
        row += 3
    rows.append(label(row, "weight") + "Regular   " + BOLD + "Bold" + RESET + "   " + DIM + "Faint" + RESET + "   " + ITALIC + "Italic" + RESET
                + "   " + BOLD + ITALIC + "Bold Italic" + RESET + "   " + note("SGR 1, 2, 3, 1;3"))
    rows.append(label(row + 1, "axis") + "".join(CSI + "1:%dm" % n + "Weight " + sgr(22) for n in range(1, 8)) + "  " + note("SGR 1:1 to 1:7"))
    row += 3
    rows.append(s.at(row, 16) + "⎛ " + it("n") + " ⎞         " + it("n") + "!         " + it("n") + "                    ⌠∞"
                + label(row + 1, "display") + s.at(row + 1, 16) + "⎜   ⎟  =  ────────       ∑ " + it("i") + "  =  ½ " + it("n") + "(" + it("n") + "+1)     ⎮ "
                + it("e") + SUP + "−" + it("x") + "²" + SOFF + " d" + it("x") + " = √π ⁄ 2"
                + s.at(row + 2, 16) + "⎝ " + it("k") + " ⎠     " + it("k") + "!(" + it("n") + "−" + it("k") + ")!      " + it("i") + "=1                   ⌡0")
    row += 4
    rows.append(s.at(row, 16) + "⎡ " + it("a") + "  " + it("b") + " ⎤" + s.at(row, 33) + "d" + s.at(row, 57) + "⎧ " + it("x") + ",   " + it("x") + " > 0"
                + label(row + 1, "matrix") + s.at(row + 1, 16) + "⎢      ⎥" + s.at(row + 1, 32) + "─── " + it("f") + "(" + it("x") + ")  =  " + it("f") + "′(" + it("x") + ")"
                + s.at(row + 1, 51) + "|" + it("x") + "| = ⎨"
                + s.at(row + 2, 16) + "⎣ " + it("c") + "  " + it("d") + " ⎦" + s.at(row + 2, 32) + "d" + it("x") + s.at(row + 2, 57) + "⎩ −" + it("x") + ",  " + it("x") + " ≤ 0")
    row += 4
    rows.append(label(row, "sup/sub") + "E = " + it("m") + it("c") + SUP + "2" + SOFF + "   H" + SUB + "2" + SOFF + "O   " + it("x") + SUP + "2" + SUB + "1" + SOFF
                + "   " + it("a") + SUP + it("n") + "+1" + SOFF + " = " + it("a") + SUB + it("n") + SOFF + " + " + it("d") + "   ∆" + it("t") + " → 0   " + it("λ") + " = " + it("c") + " ⁄ " + it("ν"))
    rows.append(label(row + 1, "symbols") + "∀ ∃ ∈ ∉ ∩ ∪ ⊂ ⊃   ≈ ≠ ≡ ≤ ≥ ± × ÷ ∝   ⊕ ⊗ ⋅   ⌈x⌉ ⌊x⌋")
    rows.append(label(row + 2, "ligatures") + "=> -> === <= >= :: |> <| ++ -- ... ///   " + note("fused by the face"))
    reveal(s, rows, hold)
    s.pause(reading)
    if s.short:
        return

    # ---- Plate II: styles, rules and colour ---------------------------------
    cut(s, THEME, "slide", hold=0.03)
    s.write(HIDE, masthead(s, "STYLES, RULES & COLOUR", "PLATE II", colours))
    s.pause(0.3)
    rows = [label(4, "manner") + sgr(7) + " Inverse " + RESET + "   " + sgr(9) + "Struck" + RESET + "   " + sgr(53) + "Overlined" + RESET
            + "   " + sgr(8) + "Concealed" + sgr(28) + note(" ← concealed")]
    rows.append(label(6, "underline") + sgr("4:1") + "single" + RESET + "   " + sgr(21) + "double" + RESET + "   " + sgr("4:3") + "curly" + RESET
                + "   " + sgr("4:4") + "dotted" + RESET + "   " + sgr("4:5") + "dashed" + RESET + "   " + sgr(4, "58;5;160") + "carmine" + RESET
                + "   " + sgr("4:3", "58;5;27") + "azure curl" + RESET)
    rows.append(label(8, "ideogram") + sgr(60) + "下線" + sgr(65) + " under   " + sgr(61) + "二重" + sgr(65) + " double   " + sgr(62) + "上線" + sgr(65)
                + " over   " + sgr(64) + "強調" + sgr(65) + " stress   " + note("SGR 60 to 64"))
    rows.append(label(10, "link") + link("https://github.com/ubiquitty/ubiquitty", "an OSC 8 hyperlink, quietly ruled"))
    ansi = "".join(CSI + "4%dm  " % i + CSI + "49m" for i in range(8)) + "  " + "".join(CSI + "10%dm  " % i + CSI + "49m" for i in range(8))
    rows.append(label(12, "ansi") + ansi + "  " + note("SGR 40 to 47, 100 to 107"))
    rows.append(label(13, "cube 16-79") + "".join(CSI + "48;5;%dm " % (16 + i) + CSI + "49m" for i in range(64)))
    rows.append(label(14, "greys") + "".join(CSI + "48;5;%dm  " % (232 + i) + CSI + "49m" for i in range(24)))
    rows.append(label(15, "truecolor") + "".join(bg(30 + i * 3, 50 + i * 2, 235 - i * 3) + " " for i in range(64)) + RESET)
    rows.append(label(17, "blink") + sgr(5) + "slow" + sgr(25) + "   " + sgr(6) + "rapid" + sgr(25) + "   " + note("SGR 5 and 6, where the window blinks"))
    rows.append(label(19, "colour") + "".join(CSI + "3%dm" % i + "text " for i in range(8)) + RESET + "  " + note("SGR 30 to 37"))
    reveal(s, rows, hold)
    s.pause(reading)

    # ---- Plate III: proportional runs, where they are drawn -----------------
    if s.sized_text:
        cut(s, THEME, "slide", hold=0.03)
        s.write(HIDE, masthead(s, "PROPORTIONAL RUNS", "PLATE III", colours))
        s.pause(0.3)
        rows = [label(4, "heading") + BOLD + proportional("bold, one run", 13) + RESET]
        rows.append(label(6, "body") + proportional("the run claims 23 cells", 23)
                    + s.at(7, 15) + proportional("text may be less", 17) + "   " + note("the run still spans 17"))
        rows.append(label(9, "buttons") + sgr(7) + " " + proportional("Cancel", 8, 2) + " " + RESET + "   " + bg(60, 100, 220) + fg(255, 255, 255)
                    + " " + proportional("Install", 8, 2) + " " + RESET + "   " + note("centred in their 8 cells (h=2)"))
        rows.append(label(11, "ladder") + proportional("iiii", 4) + "│ " + proportional("WWWW", 8) + "│ " + note("four narrow in four cells; four wide in eight"))
        rows.append(label(12, "align") + proportional("left (h=0)", 14, 0) + "│" + proportional("right (h=1)", 14, 1) + "│"
                    + proportional("centred (h=2)", 14, 2) + "│ " + note("three ways"))
        rows.append(label(13, "justified") + proportional("word gaps open", 14, 3) + "│" + proportional("to the measure", 14, 3) + "│"
                    + proportional("of each run", 14, 3) + "│ " + note("h=3"))
        rows.append(label(15, "styled") + ITALIC + proportional("italic", 9) + RESET + "  " + BOLD + ITALIC + proportional("bold italic", 12) + RESET
                    + "  " + sgr("4:3", "58;5;27") + proportional("curly ruled", 13) + RESET + "  " + fg(230, 80, 80) + proportional("coloured", 8) + RESET)
        rows.append(label(17, "mixed") + "grid text, then " + proportional("an aside", 10) + ", then grid: one baseline")
        reveal(s, rows, hold)
        s.pause(reading)

    # ---- Plate IV: boxes, blocks and separators -----------------------------
    cut(s, THEME, "slide", hold=0.03)
    s.write(HIDE, masthead(s, "BOXES, BLOCKS & SEPARATORS", "PLATE IV", colours))
    s.pause(0.3)
    rows = [s.at(4, 4) + "┌─────┬─────┐   ┏━━━━━┳━━━━━┓   ╔═════╦═════╗   ╭─────╮   ╌╌╌╌ ┄┄┄┄ ┈┈┈┈"
            + s.at(5, 4) + "│ pty │ tty │   ┃ raw ┃ cmd ┃   ║ ssh ║ mux ║   │ tab │   ╍╍╍╍ ┅┅┅┅ ┉┉┉┉"
            + s.at(6, 4) + "├─────┼─────┤   ┣━━━━━╋━━━━━┫   ╠═════╬═════╣   ╰─────╯   ╱╲╱╲ ╳╳ ╲╱╲╱"
            + s.at(7, 4) + "└─────┴─────┘   ┗━━━━━┻━━━━━┛   ╚═════╩═════╝"]
    rows.append(label(9, "mixed") + "┍━┑ ┎─┒ ┏┭┓ ┗┵┛  ╒═╕ ╓─╖  ┽ ╀ ╁ ╂ ┿ ╊ ╉   " + note("light meets heavy"))
    rows.append(label(11, "linedraw") + ESC + "(0" + "lqqwqqk" + ESC + "(B" + "  SCS G0 line drawing  " + ESC + "(0" + "x" + ESC + "(B" + " pipes "
                + ESC + "(0" + "x" + ESC + "(B" + "  " + ESC + "(0" + "mqqvqqj" + ESC + "(B")
    rows.append(label(13, "scan") + "⎺⎺⎻⎻⎼⎼⎽⎽   corners ⎡ ⎤ ⎣ ⎦   brackets ⎴ ⎵")
    rows.append(label(15, "blocks") + "▁▂▃▄▅▆▇█   ▏▎▍▌▋▊▉   ▀▄ ▖▗▘▝   ▙▚▛▜   ░░░▒▒▒▓▓▓")
    rows.append(label(16, "sextants") + "🬀🬁🬂🬇🬎🬞🬺🬻   " + note("sextant fills tile with no seam between cells"))
    separators = "".join(chr(cp) + (" " if cp % 4 == 3 else "") for cp in range(0xE0B0, 0xE0C0))
    rows.append(label(18, "separators") + separators + "  " + note("these tile edge to edge"))
    rows.append(label(19, "segments") + CSI + "48;5;214;38;5;235m" + chr(0xE0B0) + CSI + "38;5;231m SRC " + CSI + "48;5;235;38;5;214m" + chr(0xE0B0)
                + CSI + "38;5;245m " + chr(0xE0B1) + " BUILD " + CSI + "49;38;5;235m" + chr(0xE0B0) + RESET + "   "
                + CSI + "38;5;235m" + chr(0xE0B2) + CSI + "48;5;235;38;5;245m LOG " + chr(0xE0B3) + " RUN " + CSI + "38;5;214m" + chr(0xE0B2)
                + CSI + "48;5;214;38;5;231m OUT " + RESET + "   " + note("both ways, no seam"))
    artwork = " ".join(chr(c) for c in (0xE0A0, 0xE0A1, 0xE0A2, 0xE0A3)) + "   " + "".join(chr(c) for c in (0xE0C0, 0xE0C2, 0xE0C8, 0xE0CA)) \
        + "   " + "".join(chr(c) for c in (0xE0CC, 0xE0CD, 0xE0CE, 0xE0CF)) + "   " + "".join(chr(c) for c in (0xE0D0, 0xE0D1, 0xE0D2, 0xE0D4))
    rows.append(label(20, "artwork") + artwork + "   " + note("routed to the symbols face"))
    rows.append(s.at(22, 4) + note("these tables tile by construction: a seam or a drift shows first here."))
    reveal(s, rows, hold)
    s.pause(reading)


RAIN_GLYPHS = [chr(c) for c in range(0xFF66, 0xFF9E)] + list("0123456789")


def rain_frames(count, seed=2026):
    """Frames of falling glyphs: every column a stream with a head, a tail and a speed."""
    rng = random.Random(seed)
    heads = [rng.uniform(-30, STAGE_H) for _ in range(STAGE_W)]
    speeds = [rng.uniform(0.6, 1.6) for _ in range(STAGE_W)]
    tails = [rng.randint(5, 14) for _ in range(STAGE_W)]
    frames = []
    for _ in range(count):
        for col in range(STAGE_W):
            heads[col] += speeds[col]
            if heads[col] - tails[col] > STAGE_H + 2:
                heads[col] = rng.uniform(-12, 0)
                speeds[col] = rng.uniform(0.6, 1.6)
                tails[col] = rng.randint(5, 14)
        rows = []
        for row in range(1, STAGE_H + 1):
            line = []
            current = None
            for col in range(STAGE_W):
                depth = heads[col] - row
                if depth < 0 or depth > tails[col]:
                    if current is not None:
                        line.append(RESET)
                        current = None
                    line.append(" ")
                    continue
                if depth < 1:
                    colour = (220, 255, 230)
                else:
                    colour = lerp((120, 255, 140), (10, 60, 20), depth / tails[col])
                if colour != current:
                    line.append(fg(*colour))
                    current = colour
                line.append(rng.choice(RAIN_GLYPHS))
            rows.append("".join(line) + RESET)
        frames.append(rows)
    return frames


# ---------------------------------------------------------------------------
# The persistence scene: a session on ubiquitty-host outlives its clients
# ---------------------------------------------------------------------------


def find_helpers():
    """The daemon and the agent, from the app that hosts this shell or from the usual places.

    The process tree is walked upward looking for an executable inside a
    Ubiquitty.app bundle, so the show reaches the very build it runs in.
    """
    candidates = []
    host = os.environ.get("UBIQUITTY_HOST_BIN")
    if host:
        candidates.append(os.path.dirname(host))
    pid = os.getppid()
    for _ in range(12):
        try:
            out = subprocess.run(["ps", "-o", "ppid=,comm=", "-p", str(pid)], capture_output=True, text=True, timeout=2).stdout.split(None, 1)
        except (OSError, subprocess.SubprocessError):
            break
        if len(out) < 2:
            break
        parent, command = int(out[0]), out[1].strip()
        marker = ".app/Contents/MacOS/"
        if marker in command:
            candidates.append(command[:command.index(marker)] + ".app/Contents/Helpers")
        if parent <= 1:
            break
        pid = parent
    candidates.append("/Applications/Ubiquitty.app/Contents/Helpers")
    for directory in candidates:
        agent = os.path.join(directory, "ubq")
        host = os.path.join(directory, "ubiquitty-host")
        if os.access(agent, os.X_OK) and os.access(host, os.X_OK):
            return host, agent
    agent = shutil.which("ubq")
    host = shutil.which("ubiquitty-host")
    if agent and host:
        return host, agent
    return None


class PrivateHost:
    """A ubiquitty-host of the show's own: its own directory, token and session.

    The user's daemon is never touched: this one lives in a short-named
    temporary directory (a socket path has a length limit), holds one token
    minted for the show, runs one session, and is stopped and removed at the
    end, or on the way out if the show is left early.
    """

    BUILD = ("clear; for i in 1 2 3 4 5 6 7 8 9 10 11 12; do printf 'compiling %2d/12  hello.c\\n' $i; sleep 0.35; done; "
             "echo 'linking hello'; sleep 0.3; echo 'hello: build complete'; ./hello 2>/dev/null || echo 'hello, world'\n")

    def __init__(self, host, agent):
        self.host, self.agent = host, agent
        self.home = tempfile.mkdtemp(prefix="ubq-", dir="/tmp")
        self.env = dict(os.environ, UBIQUITTY_HOME=self.home, UBIQUITTY_HOST_BIN=host)
        self.session = None

    def run(self, *arguments, timeout=6.0):
        return subprocess.run(list(arguments), capture_output=True, text=True, timeout=timeout, env=self.env)

    def start(self):
        minted = self.run(self.host, "token", "issue", "--scope", "all", "--label", "evolution")
        token = minted.stdout.strip().split("\n")[-1].split()[-1] if minted.stdout.strip() else ""
        if minted.returncode or len(token) < 32:
            raise RuntimeError("no token")
        self.env["UBIQUITTY_AGENT_TOKEN"] = token
        created = self.run(self.agent, "--yes", "new", "--columns", "70", "--rows", "12", timeout=12.0)
        if created.returncode or not created.stdout.strip().startswith("session:"):
            raise RuntimeError("no session")
        self.session = created.stdout.strip()

    def build(self):
        typed = self.run(self.agent, "--yes", "send", "-s", self.session, self.BUILD)
        if typed.returncode:
            raise RuntimeError("no send")

    def screen(self):
        """The session's screen as the lines a client sees, blank tail dropped."""
        shown = self.run(self.agent, "read", "-s", self.session, "--screen")
        if shown.returncode:
            raise RuntimeError("no screen")
        lines = [line.rstrip() for line in shown.stdout.split("\n")]
        while lines and not lines[-1]:
            lines.pop()
        return lines

    def stop(self):
        if self.session:
            try:
                self.run(self.agent, "--yes", "kill", "-s", self.session)
            except (OSError, subprocess.SubprocessError):
                pass
            self.session = None
        for name in os.listdir(self.home) if os.path.isdir(self.home) else []:
            if name.startswith("host-") and name.endswith(".pid"):
                try:
                    os.kill(int(open(os.path.join(self.home, name)).read().strip()), 15)
                except (OSError, ValueError):
                    pass
        shutil.rmtree(self.home, ignore_errors=True)


SCRIPTED_BUILD = ["compiling %2d/12  hello.c" % i for i in range(1, 13)] + ["linking hello", "hello: build complete", "hello, world"]


def scene_persistence(s):
    """The build starts, the client drops, another client finds it running.

    Live inside a Ubiquitty that ships its helpers, a private daemon runs the
    build for real and two agent clients read its screen; anywhere else the
    same scene plays from a script and the caption says so.
    """
    box_top, box_left, box_width, box_height = 6, 4, 73, 9
    inner = box_width - 2
    private = None
    if s.is_live and s.session_mode != "sim":
        helpers = find_helpers()
        if helpers:
            try:
                private = PrivateHost(*helpers)
                s.on_exit.append(private.stop)
                private.start()
            except (OSError, RuntimeError, subprocess.SubprocessError):
                private.stop()
                private = None
        if s.session_mode == "real" and private is None:
            raise RuntimeError("no ubiquitty-host to run the session on")
    started = time.monotonic()
    started_clock = s.clock

    def elapsed():
        return time.monotonic() - started if s.is_live else s.clock - started_clock

    def lines_now():
        """What a client attached this moment would see: the real screen, or the script's clock."""
        if private:
            lines = private.screen()
            return [line for line in lines if "compiling" in line or "linking" in line or "hello" in line.lower()][-6:]
        return SCRIPTED_BUILD[:min(len(SCRIPTED_BUILD), int(elapsed() / 0.35))][-6:]

    def frame(title, colour, body, note):
        s.write(SYNC_ON, s.at(box_top, box_left), fg(*colour), "╭─ ", title, " ", "─" * max(0, inner - len(title) - 3), "╮", RESET)
        for index in range(box_height - 2):
            text = body[index] if index < len(body) else ""
            s.write(s.at(box_top + 1 + index, box_left), fg(*colour), "│", RESET, " ", text[:inner - 1].ljust(inner - 1), fg(*colour), "│", RESET)
        s.write(s.at(box_top + box_height - 1, box_left), fg(*colour), "╰", "─" * inner, "╯", RESET)
        s.write(s.at(box_top + box_height, box_left), CSI + "K", DIM, note, RESET, SYNC_OFF)

    where = "a private ubiquitty-host, started for this scene" if private else "scripted: no ubiquitty-host was found here"
    s.write(s.at(5, 4), DIM, "PERSISTENCE", RESET, "   the session lives on ubiquitty-host; clients come and go")
    prompt = "❯ ubq send " + ("-s %s " % private.session if private else "") + "'make hello'"
    frame("client A · attached", (120, 200, 255), [prompt], "")
    if private:
        private.build()
    started = time.monotonic()
    started_clock = s.clock
    sound(s, [E5, A5], 2, 2)
    while elapsed() < 1.8:
        frame("client A · attached", (120, 200, 255), [prompt] + lines_now(), "the build runs in the session, and client A watches it")
        s.pause(0.25)
    # The drop: nobody is attached, and the build goes on regardless.
    sound(s, [C5, REST, C5], 3, 3)
    dropped = [prompt] + lines_now()
    for blink in range(3):
        frame("client A · connection dropped", (255, 100, 100) if blink % 2 == 0 else (120, 60, 60), dropped,
              "✂  the window is gone; nothing is watching; the host keeps building")
        s.pause(0.4)
    s.pause(1.2)
    # Another client: the same session, everything that happened meanwhile.
    sound(s, [A5, E6, A6], 2, 2)
    seen_done = False
    while elapsed() < 9.0:
        lines = lines_now()
        frame("client B · attached to the same session", (120, 255, 160), [prompt] + lines,
              "client B reattached: the gap's lines are all here, and the build ends")
        if any("hello, world" in line for line in lines):
            seen_done = True
            break
        s.pause(0.25)
    if not seen_done:
        frame("client B · attached to the same session", (120, 255, 160), [prompt] + lines_now(),
              "client B reattached: the gap's lines are all here")
    s.pause(0.3)
    s.write(s.at(box_top + box_height + 1, box_left), BOLD, "same session, same scrollback, nothing lost.", RESET)
    s.write(s.at(box_top + box_height + 2, box_left), DIM, "ubiquitty-host keeps the Core; windows and agents are its clients", RESET)
    s.write(s.at(box_top + box_height + 3, box_left), DIM, where, RESET)
    s.pause(2.0)
    if private:
        private.stop()
        s.on_exit.remove(private.stop)
    return where


def dialog(s, top, left):
    """The welcome dialog at a column: rounded frame, proportional text, buttons.

    A proportional run is placed whole or on the next row, whatever autowrap
    says, so while the dialog is still off the right edge only the pieces
    that fit are drawn; the frame itself is clipped by the edge.
    """
    bottom, right = top + 7, left + 35
    inner = right - left - 1
    s.write(s.at(top, left), "╭" + "─" * inner + "╮")
    for row in range(top + 1, bottom):
        s.write(s.at(row, left), "│", " " * inner, "│")
    s.write(s.at(bottom, left), "╰" + "─" * inner + "╯")
    fits = lambda col, cells: col + cells - 1 <= STAGE_W
    if s.sized_text:
        run = proportional
        body = ("Text set in the system face, inside", "the cell grid, measured and placed.")
    else:
        run = lambda text, cells, align=0: (text.center(cells) if align == 2 else text.ljust(cells))[:cells]
        body = ("The tour has a minute to go, or", "the window is yours right now.")
    if fits(left + 2, 20):
        s.write(s.at(top + 1, left + 2), BOLD, run("Welcome to Ubiquitty", 20), RESET)
    if fits(left + 2, 32):
        s.write(s.at(top + 3, left + 2), run(body[0], 32))
        s.write(s.at(top + 4, left + 2), run(body[1], 32))
    if fits(left + 5, 26):
        s.write(s.at(top + 6, left + 5), sgr(7), " ", run("Not now", 9, 2), " ", RESET, "    ",
                bg(60, 100, 220), fg(255, 255, 255), " ", run("Continue", 9, 2), " ", RESET)


def dialog_answer(s, top, left, seconds):
    """The dialog's buttons, for real: a click on one, Return, or Escape.

    Return and a click on Continue go on; Escape and a click on Not now jump
    to the finale. Keys arrive through the kitty keyboard protocol where the
    terminal speaks it, so Return is CSI 13 u and Escape is CSI 27 u rather
    than bytes that could begin anything.
    """
    row = top + 6
    not_now = range(left + 5, left + 16)
    go_on = range(left + 20, left + 31)
    s.write(s.at(top + 8, left), DIM, "Enter / click · Esc to skip", RESET)
    s.write(CSI + "?1000h", CSI + "?1006h", CSI + ">9u")
    answer = None
    for event in s.events(seconds, keys=True):
        if event[0] == "mouse":
            button, col, click_row, pressed = event[1:]
            if not pressed or button >= 64 or click_row - s.row0 != row:
                continue
            column = col - s.col0
            if column in go_on:
                answer = "continue"
            elif column in not_now:
                answer = "not now"
        else:
            code = event[1]
            if code in (13, 10):
                answer = "continue"
            elif code == 27:
                answer = "not now"
        if answer:
            break
    s.write(CSI + "<u", CSI + "?1006l", CSI + "?1000l")
    s.write(s.at(top + 8, left), CSI + "K")
    return answer


def chapter_ubiquitty(s, pictures):
    frames = rain_frames(76 if not s.short else 56)
    cut(s, THEME, "slide")
    card(s, THEME, "X", "2026", "UBIQUITTY", "one terminal session, wherever the work runs", from_year="2013")
    s.write(HIDE)
    music(s, CLIMAX)
    # The rain, paced: frames the eye can follow.
    paced = 6 if s.reduced else (16 if s.short else 36)
    for rows in frames[:paced]:
        s.write(SYNC_ON)
        for index, line in enumerate(rows):
            s.write(s.at(index + 1, 1), line)
        s.write(SYNC_OFF)
        s.pause(0.06)
    # Then the burst: forty frames handed over at once, and a cursor report
    # asked for behind them. The reply comes back only after the terminal
    # has drained every byte before it, which is what the clock measures.
    burst_frames = frames[paced:]
    burst = "".join(SYNC_ON + "".join(s.at(index + 1, 1) + line for index, line in enumerate(rows)) + SYNC_OFF for rows in burst_frames)
    drained = None
    if s.is_live:
        started = time.monotonic()
        s.write(burst)
        if s.query(CSI + "6n", rb"\x1b\[\d+;\d+R", timeout=5.0):
            drained = time.monotonic() - started
    else:
        s.write(SYNC_ON, "".join(s.at(index + 1, 1) + line for index, line in enumerate(burst_frames[-1])), SYNC_OFF)
    # The reveal: the last frame stays, and the numbers land on it.
    cells = len(burst_frames) * STAGE_W * STAGE_H
    measure = "drained in %d ms" % round(drained * 1000) if drained else "synchronized output"
    plate = bg(0, 0, 0) + fg(235, 235, 235)
    if s.is_live:
        s.write(SYNC_ON)
        s.write(s.centred(11, plate + BOLD + "   %s cells  ·  %s bytes  ·  %s   " % (format(cells, ","), format(len(burst.encode("utf-8")), ","), measure) + RESET))
        s.write(s.centred(12, plate + DIM + "  %d frames handed over at once, drawn faster than one could be seen  " % len(burst_frames) + RESET))
        s.write(SYNC_OFF)
    sound(s, [A6, E6, A5], 2, 2)
    s.pause(1.8)
    last = burst_frames[-1]
    s.write(SYNC_ON, s.at(11, 1), last[10], s.at(12, 1), last[11], SYNC_OFF)
    cut(s, THEME, "dissolve", hold=0.04)

    steps = 1 if s.reduced else 6
    for step in range(1, steps + 1):
        t = step / steps
        s.write(SYNC_ON, headline(s, 1, "UBIQUITTY", lerp(s.theme.background, ACCENT, t)))
        s.write(s.centred(3, fg(*lerp(s.theme.background, s.theme.foreground, 0.6 * t)) + "one terminal session, wherever the work runs" + RESET), SYNC_OFF)
        s.pause(0.06)
    s.pause(0.4)

    where = scene_persistence(s)
    for row in range(5, 20):
        s.write(s.at(row, 1), CSI + "2K")

    # The dialog slides in from the right edge, clipped there rather than
    # wrapped, and then it is a dialog: its buttons answer.
    s.write(CSI + "?7l")
    for left in ((62, 54, 48, 44, 43) if not s.reduced else (43,)):
        s.write(SYNC_ON)
        for row in range(8, 16):
            s.write(s.at(row, 43), CSI + "K")
        dialog(s, 8, left)
        s.write(SYNC_OFF)
        s.pause(0.05)
    s.write(CSI + "?7h")
    if s.is_live:
        s.write(s.at(5, 4), DIM, "throughput", RESET, "   %s cells in %d frames, %s" % (format(cells, ","), len(burst_frames), measure))
    if s.sized_text:
        s.write(s.at(6, 4), DIM, "sized text", RESET, "   ", sized("2x", 2), " ", sized("3x", 3), "   ", DIM, "OSC 66 s=2 and s=3", RESET)
    s.write(s.at(10, 4), DIM, "presentation", RESET, "   ", CSI + "76:1m", "☂ ☀ ✈ ❤ ⭐", CSI + "76m", "  colour")
    s.write(s.at(11, 19), CSI + "76:2m", "☂ ☀ ✈ ❤ ⭐", CSI + "76m", "  text  ", DIM, "SGR 76", RESET)
    s.write(s.at(13, 4), DIM, "password", RESET, "       password: ", CSI + "?7401h", "●●●●●●●●")
    s.write(s.at(14, 19), DIM, "mode 7401 shows a lock", RESET)
    s.write(s.at(17, 4), DIM, "session", RESET, "      ", DIM, where, RESET)
    sound(s, [E5], 1, 2)
    answer = dialog_answer(s, 8, 43, 6.0)
    if not s.is_live:
        s.pause(0.4)
    s.write(CSI + "?7401l")
    if answer == "not now":
        s.jump_to_finale = True
        return
    if answer == "continue":
        sound(s, [E6, A6], 2, 2)

    reply = s.query(CSI + "?7402$p", rb"\x1b\[\?7402;\d\$y") if s.is_live else (CSI + "?7402;2$y" if s.is_ubiquitty else CSI + "?7402;0$y")
    state = {"1": "set", "2": "reset, so the mode is understood", "0": "not recognised: this is not Ubiquitty"}
    numbers = re.findall(r";(\d)", reply or "")
    verdict = state.get(numbers[0], "unclear") if numbers else "no answer, so the mode is unknown"
    s.write(s.at(20, 4), DIM, "detection", RESET, "    DECRQM ?7402 → ", verdict)
    sound(s, [C6, G6], 3, 2)
    s.write(s.at(21, 4), DIM, "hyperlinks", RESET, "   ", link("https://github.com/ubiquitty/ubiquitty", sgr(4) + "github.com/ubiquitty/ubiquitty" + RESET),
            "   ", DIM, "OSC 8: ⌘-click opens it", RESET)
    if s.short:
        s.pause(1.6)
        return
    s.write(s.at(23, 4), DIM, "fullscreen", RESET, "   mode 7402 next: content edge to edge, no padding, for a moment")
    s.pause(2.2)


def chapter_fullscreen(s, pictures):
    """Mode 7402: the picture reaches the window's edge; settle leaves it."""
    width, height, rgba = pictures.julia
    s.write(CLEAR, CSI + "?7402h", OSC + "7402;fit=contain;fill=000000" + ST)
    music(s, ("pad",))
    s.write(CSI + "1;1H", kitty_image(KITTY_FULLSCREEN, width, height, rgba, s.columns, s.rows))
    sound(s, [E5, A5, D6, G6, C7], 1, 2)
    s.pause(2.4)


CREDITS = [
    ("THE TERMINAL", 1.0), ("sixty years, one screen", 0.6), ("", 0), ("", 0),
    ("CAST, IN ORDER OF APPEARANCE", 0.5), ("", 0),
    ("Teletype Model 33 ASR  ·  1963", 1.0), ("Tektronix 4014  ·  1974", 1.0), ("DEC VT100  ·  1978", 1.0),
    ("DEC VT220, VT240, VT320  ·  1983", 1.0), ("xterm  ·  1996", 1.0), ("Unicode and UTF-8  ·  2003", 1.0),
    ("truecolor  ·  2012", 1.0), ("inline pictures  ·  2013", 1.0), ("Ubiquitty and ubiquitty-host  ·  2026", 1.0), ("", 0), ("", 0),
    ("SEQUENCES", 0.5), ("", 0),
    ("OSC 10, 11, 12  ·  DECDHL, DECDWL  ·  SGR 1, 4, 5, 7  ·  BEL", 0.8),
    ("DECSET 38  ·  GS vectors  ·  US alpha text  ·  PAGE  ·  line styles", 0.8),
    ("DEC special graphics  ·  DECSTBM, DECSLRM  ·  DA1, CPR  ·  DECSCUSR", 0.8),
    ("DECDLD  ·  DECSCA, DECSEL  ·  DECSSDT, DECSASD  ·  Sixel", 0.8),
    ("DECFRA, DECCRA, DECCARA, DECSACE", 0.8),
    ("SGR 38;5  ·  OSC 2  ·  XTWINOPS 22, 23  ·  DECSET 1000, 1006", 0.8),
    ("UAX-29 clusters  ·  braille  ·  box drawing  ·  SGR 4:3, 58  ·  DECSET 2026", 0.8),
    ("SGR 38;2  ·  OSC 1337  ·  kitty graphics and animation", 0.8),
    ("OSC 133  ·  OSC 9;4  ·  OSC 21337  ·  OSC 8  ·  DSR", 0.8),
    ("the Sync protocol  ·  ubiquitty-host  ·  ubq new, send, read", 0.8),
    ("OSC 66  ·  SGR 1:n  ·  SGR 76  ·  DECRQM", 0.8),
    ("DECSET 7401, 7402  ·  OSC 7402  ·  CSI > 9 u", 0.8),
    ("", 0), ("", 0),
    ("MUSIC", 0.5), ("", 0),
    ("Beethoven: Ode to Joy, arranged for DECPS voices and a sampled kit,", 0.8),
    ("a square bass, saw arpeggios, a sine lead, eight channels of DECPS", 0.8),
    ("", 0), ("", 0), ("", 0),
    ("A  U B I Q U I T T Y  P R O D U C T I O N", 1.0),
    ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0), ("", 0),
]


def chapter_credits(s):
    """The roll: lines rise through a four-sided region, the track under them."""
    cut(s, INK, "iris")
    s.write(HIDE, CLEAR, scroll_region(s, 1, STAGE_H))
    colours = s.era_colours(INK)
    music(s)
    credits = CREDITS
    if s.quiet:
        start = next(i for i, (text, _) in enumerate(credits) if text == "MUSIC")
        credits = credits[:start] + credits[start + 4:]
    step = 7 * BAR / len(credits)
    for text, weight in credits:
        s.write(s.at(STAGE_H, 1), CSI + "K")
        if text:
            s.write(s.centred(STAGE_H, fg(*lerp(colours.background, colours.foreground, weight)) + (BOLD if weight == 1.0 else "") + text + RESET))
        s.write(s.at(STAGE_H, 1), ESC + "D")
        s.pause(step)
    s.write(NO_REGION)
    s.pause(0.8)


def chapter_finale(s):
    """A closing card, with playback controls left to the player."""
    s.write(light(s, THEME), CSI + "?1049l", HIDE, CSI + "H", CLEAR)
    s.write(headline(s, 10, "UBIQUITTY", (136, 136, 190)))
    s.write(s.centred(15, DIM + "ubiquitty.org" + RESET))
    music(s, ("crash", "kick", "hat", "snare", "bass", "arp", "lead", "pad"), coda=FINAL)
    s.pause(4.0 if not s.quiet else 0.5)
    # The coda is sent whole before the hand-off; it rings out after.
    while not s.quiet and s.next_bar is not None:
        s.pause(0.5)


# ---------------------------------------------------------------------------
# Running the show
# ---------------------------------------------------------------------------


def settle(s):
    """Leaves whatever a chapter left on, whether it ended or was skipped.
    The cues stay: they are the music's, and it plays on."""
    if s.in_tek:
        s.write(TEK_EXIT, OSC + "115" + ST, OSC + "116" + ST)
        s.in_tek = False
    if s.wide:
        s.restore_width()
    s.write(SYNC_OFF, RESET, SI, ESC + "(B", ESC + ")B", NO_REGION, CSI + '0"q', CSI + "0$~", CSI + "0$}",
            CSI + "<u", CSI + "?1006l", CSI + "?1000l", CSI + "?7h", CSI + "?7401l", CSI + "?7402l",
            OSC + "9;4;0;0" + BEL, OSC + "21337;indicator=;status=;status-color=" + BEL, CSI + "23;0t", HIDE)


CLEANUP = (TEK_EXIT + SYNC_OFF + RESET + SI + ESC + "(B" + ESC + ")B" + NO_REGION + CSI + '0"q' + CSI + "0$~"
           + CSI + "0$}" + CSI + "<u" + CSI + "?1006l" + CSI + "?1000l" + CSI + "?7h" + CSI + "?7401l" + CSI + "?7402l"
           + OSC + "9;4;0;0" + BEL + OSC + "21337;indicator=;status=;status-color=" + BEL + CSI + "23;0t"
           + OSC + "111" + ST + OSC + "110" + ST + OSC + "112" + ST + OSC + "115" + ST + OSC + "116" + ST
           + APC + "Ga=d,d=A,q=2" + ST + CSI + "?1049l" + CSI + "0 q" + SHOW)

SHORT_CUT = ("title", "teletype", "tektronix", "vt100", "unicode", "truecolor", "typography", "ubiquitty", "finale")


def chapters(pictures):
    return [
        ("title", chapter_title),
        ("teletype", chapter_teletype),
        ("tektronix", chapter_tektronix),
        ("vt100", chapter_vt100),
        ("vt220", lambda s: chapter_vt220(s, pictures)),
        ("xterm", chapter_xterm),
        ("unicode", chapter_unicode),
        ("truecolor", chapter_truecolor),
        ("pictures", lambda s: chapter_pictures(s, pictures)),
        ("typography", chapter_typography),
        ("ubiquitty", lambda s: chapter_ubiquitty(s, pictures)),
        ("fullscreen", lambda s: chapter_fullscreen(s, pictures)),
        ("credits", chapter_credits),
        ("finale", chapter_finale),
    ]


def probe_sized_text(s):
    """Whether OSC 66 sized text is drawn here.

    A proportional run the terminal prints plainly advances the cursor by its
    text; one it draws advances by the run's cell budget. The probe is made
    on the alternate screen and wiped.
    """
    s.write(CSI + "H", OSC + "66;p=1:c=12;ab" + BEL)
    reply = s.query(CSI + "6n", rb"\x1b\[\d+;\d+R", timeout=0.4)
    numbers = re.findall(r"\d+", reply or "")
    s.sized_text = len(numbers) == 2 and int(numbers[1]) == 13
    s.write(CSI + "H", CSI + "2K")


def probe_sound(s):
    """Whether DECPS here is the tracker.

    DECRQSS of the control's own final answers the extension's bounds; a
    terminal without it answers the invalid form, or nothing, and is given
    the melody alone in the plain form: one voice playing every channel's
    part at once is noise, not music.
    """
    reply = s.query(DCS + "$q,~" + ST, rb"\x1bP[01]\$r[^\x1b]*\x1b\\", timeout=0.4)
    s.tracker = bool(reply) and reply.startswith(ESC + "P1$r")


PREFIX = CSI + "?1049h" + HIDE + CLEAR


def run(s, selected):
    if s.is_live:
        s.write(PREFIX)
        probe_sized_text(s)
        probe_sound(s)
    for name, chapter in selected:
        if s.jump_to_finale and name != "finale":
            continue
        if isinstance(s, Recorder):
            s.begin(name)
            # Each dumped chapter stands alone, so it carries the prefix; the
            # baked player writes it once, so its wipes start from the last
            # picture rather than from a cleared screen.
            s.chapters[-1][1].append(("prefix", PREFIX))
        settle(s)
        try:
            chapter(s)
        except Skip:
            s.rewind()
    s.flush()


def parse_colour(reply):
    """An OSC 10/11 answer, rgb:rrrr/gggg/bbbb at any channel width, to 8 bits."""
    match = re.search(r"rgb:([0-9a-fA-F]+)/([0-9a-fA-F]+)/([0-9a-fA-F]+)", reply or "")
    if not match:
        return None
    return tuple(int(channel, 16) * 255 // (16 ** len(channel) - 1) for channel in match.groups())


def detect(s):
    """What the terminal is and what it is lit in, asked before the curtain."""
    reply = s.query(CSI + "?7402$p", rb"\x1b\[\?7402;\d\$y", timeout=0.4)
    s.is_ubiquitty = bool(reply) and reply[-3] in "12"
    background = parse_colour(s.query(OSC + "11;?" + ST, rb"\x1b\]11;[^\x07\x1b]*(?:\x07|\x1b\\)", timeout=0.4))
    foreground = parse_colour(s.query(OSC + "10;?" + ST, rb"\x1b\]10;[^\x07\x1b]*(?:\x07|\x1b\\)", timeout=0.4))
    if background and foreground:
        s.theme = Era(background, foreground)
        s.current_background = background


def configure(s, args):
    s.quiet = args.silent
    s.blips = args.blips
    s.sized_text = getattr(args, "assume_sized_text", False)
    s.tracker = getattr(args, "assume_ubiquitty", False)
    s.short = args.cut == "short"
    s.reduced = args.reduced_motion or os.environ.get("UBIQUITTY_REDUCED_MOTION", "") not in ("", "0")
    s.session_mode = args.session
    if s.columns < STAGE_W or s.rows < STAGE_H:
        # A window smaller than the stage: the short cut, without the
        # motion that needs the whole stage to read.
        s.short = True
        s.reduced = True


def play(selected, pictures, args):
    s = Live()
    configure(s, args)
    if s.short:
        selected = [(name, fn) for name, fn in selected if name in SHORT_CUT]
    saved = None
    if s.fd is not None:
        import termios
        import tty
        saved = termios.tcgetattr(s.fd)
        tty.setraw(s.fd)
    try:
        detect(s)
        run(s, selected)
    except Quit:
        s.write(STOP_ALL)
    finally:
        for hook in list(s.on_exit):
            try:
                hook()
            except Exception:
                pass
        s.write(CLEANUP)
        s.flush()
        if saved is not None:
            termios.tcsetattr(s.fd, termios.TCSADRAIN, saved)
    return 0


def record(selected, pictures, args):
    s = Recorder(args.columns, args.rows, args.assume_ubiquitty)
    configure(s, args)
    if s.short:
        selected = [(name, fn) for name, fn in selected if name in SHORT_CUT]
    run(s, selected)
    return s


def dump(selected, pictures, args):
    s = record(selected, pictures, args)
    os.makedirs(args.dump, exist_ok=True)
    for index, (name, events) in enumerate(s.chapters, 1):
        path = os.path.join(args.dump, "%02d-%s.ans" % (index, name))
        with open(path, "wb") as out:
            out.write("".join(text for kind, text in events if kind in ("prefix", "bytes")).encode("utf-8"))
        print(path)
    return 0


def sh_quoted(text):
    """A printf format in single quotes: octal for every byte sh could mangle."""
    out = []
    for byte in text.encode("utf-8"):
        if byte == 0x27:
            out.append("'\\''")
        elif byte == 0x5C:
            out.append("\\\\")
        elif byte == 0x25:
            out.append("%%")
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append("\\%03o" % byte)
    return "'" + "".join(out) + "'"


def bake(selected, pictures, args):
    s = record(selected, pictures, args)
    lines = ["#!/bin/sh", "# The evolution of the terminal, baked by Tools/showcase/Examples/evolution.py",
             "# for a %dx%d window. Plays straight through; Ctrl-C restores the terminal." % (args.columns, args.rows),
             "restore() { printf %s; }" % sh_quoted(CLEANUP), "trap 'restore; exit 130' INT TERM", ""]
    for index, (name, events) in enumerate(s.chapters):
        lines.append("# " + name)
        text, sleep = "", 0.0
        for kind, value in events:
            if kind == "prefix":
                if index == 0:
                    text += value
            elif kind == "bytes":
                if sleep >= 0.01:
                    lines.append("printf %s" % sh_quoted(text))
                    lines.append("sleep %.2f" % sleep)
                    text = ""
                    sleep = 0.0
                text += value
            else:
                sleep += value
        if text:
            lines.append("printf %s" % sh_quoted(text))
        if sleep >= 0.01:
            lines.append("sleep %.2f" % sleep)
        lines.append("")
    with open(args.bake, "w") as out:
        out.write("\n".join(lines))
    os.chmod(args.bake, 0o755)
    print(args.bake)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cut", choices=("full", "short"), default="full", help="the director's cut, or the first-launch cut")
    parser.add_argument("--chapter", action="append", help="play only this chapter (repeatable; see --list)")
    parser.add_argument("--reduced-motion", action="store_true", help="cuts instead of wipes, stills instead of fades")
    parser.add_argument("--session", choices=("auto", "real", "sim"), default="auto",
                        help="the persistence scene on a private ubiquitty-host, or from a script")
    parser.add_argument("--dump", metavar="DIR", help="write one .ans per chapter instead of playing")
    parser.add_argument("--bake", metavar="FILE", help="write a POSIX sh player instead of playing")
    parser.add_argument("--columns", type=int, default=80, help="window width for --dump/--bake")
    parser.add_argument("--rows", type=int, default=24, help="window height for --dump/--bake")
    parser.add_argument("--assume-ubiquitty", action="store_true", help="for --dump/--bake: the terminal is Ubiquitty")
    parser.add_argument("--assume-sized-text", action="store_true", help="for --dump/--bake: the terminal draws OSC 66 sized text")
    parser.add_argument("--silent", action="store_true", help="no DECPS score")
    parser.add_argument("--blips", action="store_true", help="the incidental beeps too: keyclicks, ticks and pips")
    parser.add_argument("--list", action="store_true", help="print the chapter names")
    args = parser.parse_args(argv)
    random.seed(1963)
    pictures = Pictures()
    all_chapters = chapters(pictures)
    if args.list:
        print("\n".join(name for name, _ in all_chapters))
        return 0
    selected = all_chapters
    if args.chapter:
        unknown = [name for name in args.chapter if name not in dict(all_chapters)]
        if unknown:
            parser.error("unknown chapter: " + ", ".join(unknown))
        selected = [(name, fn) for name, fn in all_chapters if name in args.chapter]
    if args.dump:
        return dump(selected, pictures, args)
    if args.bake:
        return bake(selected, pictures, args)
    return play(selected, pictures, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
