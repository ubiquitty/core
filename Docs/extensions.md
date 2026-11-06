<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ubiquitty extensions

Sequences Ubiquitty introduces. Everything else the emulator accepts comes from a
published specification (ECMA-48, ECMA-35, ISO 8613-6/ITU-T T.416, DEC STD 070) or
from a documented de-facto protocol, and is not repeated here.

The private modes, OSCs and reports defined here use the 74xx block. SGR extensions are
either colon sub-parameters of an existing selector (`1:n`), so a parser that does not
know them drops the sub-parameters rather than mis-reading the tail, or a selector no
standard assigns (`76`), which such a parser drops.

## Feature detection

Every private mode answers DECRQM (`CSI ? Pd $ p`) with its real state, so detection
never needs a version string:

```
CSI ? 7403 $ p   ->   CSI ? 7403 ; 1 $ y    set
                      CSI ? 7403 ; 2 $ y    reset (supported, currently off)
                      CSI ? 7403 ; 0 $ y    not recognised — not Ubiquitty, or too old
```

`0` is the only answer that means "do not use this feature". Terminals that ignore
unknown DECRQM entirely will not reply at all, so read with a timeout, or pair the
query with a DA1 request and treat "DA1 arrived, DECRQM did not" as unsupported.

SGR extensions are not queryable. They are designed to degrade: on a terminal without
them, `1:7` renders as bold and an unknown selector is dropped. The sound extension is
not a mode either; DECRQSS `,~` reports its bounds, see [Sound](#sound).

## Private modes

Modes 7401–7403 share the block with 7400, the Tektronix pen override.
Feature detection uses each mode's own DECRQM report; programs must not infer
one private mode's availability from another.

### 7401 — password input

```
CSI ? 7401 h     enter    CSI ? 7401 l     leave
```

Marks the run as secret input. The frontend shows a lock indicator and masks local
echo. A shell or application may assert it directly around a prompt; Ubiquitty's own
prompt heuristic sets the same flag, so the two paths cannot disagree, and DECRQM
reports set while either holds it.

Not cleared by DECSTR (soft reset) — a soft reset issued by a program running inside a
password prompt must not silently unmask it.

### 7402 — fullscreen content

```
CSI ? 7402 h     enter    CSI ? 7402 l     leave
```

Edge-to-edge display for image and graphics output that should not be framed by terminal
chrome. With a colour letterbox, content is drawn from origin 0 and filled to the
drawable rather than inset by the usual padding; with `fill=pass` the grid keeps its
padding around the fitted image. Mouse reports under SGR-pixel mode (`CSI ? 1016 h`)
are measured from the drawable's corner, so pointer positions land exactly on the pixels
the client drew, and are clamped to the extent `CSI 14 t` advertises, the drawable's,
so a client can reconstruct the drawable it is painting into.

Cell geometry is unchanged — this is presentation, not a resize, and no reflow is
triggered on entry or exit. The mode bit and its DECRQM answer are kept regardless; the
frontend applies the mode and OSC 7402 only where the window-mutation capability is
granted.

Fit and letterbox are controlled by **OSC 7402**, which is meaningful while the mode is
active:

```
OSC 7402 [ ; key=value ]* ST
```

| key | values | meaning |
|---|---|---|
| `fit` | `contain` (default), `stretch`, `native`, `scale` | how content maps onto the drawable |
| `ar` | `W:H` | target aspect for `stretch` (required there); positive integers, ratio within [1/8, 8] |
| `scale` | `N` | source-px to device-px integer multiplier for `scale` (required there); clamped to 64 |
| `fill` | `RRGGBB` (default `000000`) or `pass` | letterbox fill colour, or `pass` to leave terminal content visible around the image |

Each OSC fully re-specifies the display state: named keys override, unnamed keys return
to their defaults, so a bare `OSC 7402 ST` resets to contain-fit on black. Unknown keys
are ignored for forward compatibility, but a malformed value, a token without `=`,
`stretch` without a valid `ar`, or `scale` without a valid `scale` rejects the entire
command and leaves the current selection intact — these bytes are attacker-reachable, so
the parse fails safe rather than applying half of a request.

The OSC carries the mode's own number: `OSC 7402` configures mode 7402.

Not cleared by DECSTR.

### OSC 7404 — rich text blocks

**Version 0.1 — Initial proposal**

```
OSC 7404 ; a=p : w=<cells> : h=<rows> [: i=<id>] [: o=w|c|e] [: j=l|c|r] [: p=<N>|<T,R,B,L>] [: C=1] ; {\rtf1 ...} ST
OSC 7404 ; a=q : w=<cells|0> : h=<rows|0> [: i=<id>] [: o=w|c|e] [: j=l|c|r] [: p=<N>|<T,R,B,L>] ; {\rtf1 ...} ST
OSC 7404 ; a=d [: i=<id>] ST
OSC 7404 ; a=s : i=<id> : N=<0|1> ST
OSC 7404 ; a=c : i=<request> [: e=1] ST
```

Places a block of rich text — a documented subset of RTF, with families, sizes, weight,
slant, colour, underline and strikethrough, line and paragraph spacing, and indents — into
a rectangle of cells, inset by `p` points of padding, and answers what fits before it is
placed. A query's zero width or height leaves that dimension open, and the reply's `m`
is the narrowest width that breaks no word:

```
OSC 7404 ; a=r : i=<id> : f=<0|1> : w=<cells> : h=<rows> : n=<chars> : c=<chars> : l=<lines> : m=<cells> : x=<0|1> : v=3 : ... ST
```

The reply's remaining fields carry the typography (cell size, baseline, font size, line
height, epoch) and the line ends. `a=s` subscribes to typography changes, reported as
`a=n`; `M=1` with nonzero `D` reports revision-tagged source positions through
OSC 7405. `E` guards measure-then-place against typography changes, and `v`
anchors a viewport to source text across different renderers. `K`, `T` and named
`Z` fields advertise rendering support, the measurement deadline and resource bounds. An empty query is the
feature test: a terminal that answers knows the code. Keys are strict, as OSC 66's are:
an unknown or repeated key, a malformed value, or a body over a cap refuses the whole
code, which then changes nothing and answers nothing unless `e=1` asked for a status
reply. Selection inside a block is per character, and a copy yields its text. The full
contract — every key, the subset, the box model, the measurement model and the caps — is
in `Docs/rich-text.md`.

Blocks are grid content: DECSTR keeps them and RIS removes them.

### 7403 — relative pointer lock (mouselook)

```
CSI ? 7403 h     lock      CSI ? 7403 l     release
```

Captures the pointer for camera-style relative input: the OS cursor is hidden and
warped, and motion is delivered as unbounded deltas instead of cell or pixel positions.
This is what a TUI game, a 3D viewer, or a map pans with; without it, motion reports
stop at the window edge. The frontend holds the grab only while the surface has focus,
and takes it back on return. Holding Control+Command alone for 500 ms releases the grab
and clears the mode whatever the program does, and sends an unsolicited
`CSI ? 7403 ; 2 $ y` so the program learns the lock is gone.

Enable SGR-pixel mouse reporting (`CSI ? 1016 h`) alongside it. Motion reports then
carry deltas in the pixel fields, biased by a fixed `0x8000`, and are sent without
`CSI ? 1003 h`:

```
delta_x = px - 0x8000
delta_y = py - 0x8000
```

The bias exists because a literal minus sign in an SGR body breaks conforming parsers,
and because a fast flick must not underflow the field. Button events report zero delta.

Cleared by DECSTR, unlike 7401 and 7402: a program that soft-resets while holding the
pointer would otherwise strand a hidden system cursor with no way to recover it.

### 7410, 7411 — sound reports

These are DSR parameters rather than modes: `CSI ? 7410 n` asks whether sound is audible
and which sample slots are filled, and `CSI ? 7411 ; Ptag n` asks which slot holds a
tagged sample. Both are described under [Sound](#sound), where the rest of their
vocabulary lives.

### 7400 — Tektronix bright pen

```
CSI ? 7400 h     force bright    CSI ? 7400 l     force normal
```

Overrides the configured Tek 4014 pen brightness for the session, until a reset clears
it back to the configured default. DECRQM reports set only while bright is forced. Only
meaningful in Tek mode (`CSI ? 38 h`).

## Private SGR

### `1:n` — variable weight

```
CSI 1 : n m      n = 0..7, clamped
```

Selects a variable-font weight: `1:n` puts the `wght` axis at 100 + 100·n, and `1:0`
returns to the face's own weight; a face or rasterizer without the axis keeps its own
weight. It does not set bold, and SGR 22 and SGR 0 clear it. Bare `CSI 1 m` remains
exactly bold, so this is a strict extension of the standard selector.

### `76` — emoji presentation

```
CSI 76 : 1 m     force colour      CSI 76 : 2 m     force text
CSI 76 : 0 m     auto (default)    CSI 76 m         auto
```

Pins presentation for the run, overriding both the codepoint's default and any VS-15 /
VS-16 that follows — for aligned columns of symbols that must not shift width when a
font decides one of them is emoji.

## Sound

### DECPS sub-parameters — voices, channels, envelopes

The VT520's play-sound control is

```
CSI Pvolume ; Pduration ; Pnote ; ... , ~
```

with `Pvolume` 0 to 7, `Pduration` in 1/32 s (0 to 255), and the `Pnote`s, 0 a rest
and 1 to 25 the chromatic scale from C5 (523 Hz) to C7 (2093 Hz), played one after
another for the same duration. Ubiquitty plays up to sixteen notes and drops the rest.
It takes the scale on to 49, C9 (8372 Hz), and `Pdown` below carries it four octaves
under C5, so the range is eight octaves. Ubiquitty plays it as the VT520 did, through a
square wave switched on and off, and hangs its extension off the same parameters as
colon sub-parameters, so a parser that takes the first piece of each still plays the
plain sequence:

```
CSI Pvolume : Pchannel : Pwave : Pdown : Pattack : Pdecay : Psustain : Prelease
    : Plowpass : Phighpass : Preverb : Ppan
    ; Pduration : Pgap : Pgrid : Prepeat
    ; Pnote : Pnotevolume : Peffect : Pparameter ; ... , ~
```

The head reads as a tracker's row does, and as a signal chain: the volume, then where
(the channel), what (the voice and its register), the envelope in its order, the filters,
and the send.

| sub-parameter | range | meaning |
|---|---|---|
| `Pchannel` | 0..8 | 0 the speaker, 1 to 8 the tracker's channels |
| `Pwave` | 0..6 | the voice: 0 square (the speaker), 1 sine, 2 triangle, 3 sawtooth, 4 noise, 5 drum, 6 a downloaded sample |
| `Pdown` | 0..48 | semitones to transpose the sequence down, so a bass line can sit under the VT520's range |
| `Pattack`, `Prelease` | 0..5000 | the note's ramps in milliseconds; absent, the voice's own (0 for square, noise, drum and sample, 4 for the rest) |
| `Pdecay`, `Psustain` | 0..5000, 0..7 | the rest of the envelope: after the attack the level falls over `Pdecay` milliseconds to `Psustain` sevenths of the peak and holds there until the release; absent, no decay and a full sustain |
| `Plowpass`, `Phighpass` | 0..20000 | a filter's cutoff in hertz, a Butterworth low-pass keeping what lies below it and a high-pass what lies above; 0 leaves the voice unfiltered; see Filters below |
| `Preverb` | 0..7 | the request's send to the hall, in sevenths; see Reverb below |
| `Ppan` | 0..64 | where the request sits across the stereo field: 0 hard left, 64 hard right, 32 the centre a request that names no pan keeps |
| `Pgap` | 0..`Pduration` | silence at the end of each note, in 1/32 s, for a staccato line |
| `Pgrid` | 0..255 | the grid the request starts on, in 1/32 s: a bar; see Together below |
| `Prepeat` | 0..255 | how many more times the request plays after its first pass; 255 repeats until the channel is stopped. See Repeating below |
| `Pnotevolume` | 1..7 | this note's own volume, in place of `Pvolume` |
| `Peffect`, `Pparameter` | 0..15, 0..255 | a tracker effect on this note and its byte, below |

Every value is clamped into its range, so a wave past 6 is the sample voice; a note is
the exception, and one past 49 is a rest.

**Effects.** A note's effect is the FastTracker II number of the command and its
parameter byte, written in decimal (`4:36` is `424`, a vibrato at speed 2 and depth 4);
effect 0 with byte 0 is none. The effect steps at a tracker's tick, fifty times a second,
for as long as its note sounds, a slide from the second tick since the first is the note
itself, and its state starts fresh with the next note.

| effect | tracker | meaning |
|---|---|---|
| 0 | `0xy` arpeggio | the note, then `x` semitones up, then `y`, a tick each, round again |
| 1, 2 | `1xx`, `2xx` portamento | the pitch slides up or down `xx` sixteenths of a semitone a tick |
| 3 | `3xx` tone portamento | the note starts at the pitch before it and slides to its own, `xx` sixteenths a tick |
| 4 | `4xy` vibrato | the pitch swings `y` eighths of a semitone either way, `x` sixteenths of a cycle a tick |
| 7 | `7xy` tremolo | the level swings `y` sixteenths either way, `x` sixteenths of a cycle a tick |
| 9 | `9xx` sample offset | the sample voice starts `xx` × 256 frames in |
| 10 | `Axy` volume slide | the level rises `x` and falls `y` sixty-fourths a tick |
| 12 | `Cxx` set volume | the level is `xx` of 64 |
| 14 | `E9x` retrigger, `ECx` note cut, `EDx` note delay | the note restarts every `x` ticks; is silenced after `x` ticks; begins `x` ticks late |

The rest of a tracker, its patterns, its speed and its jumps, is the program's: it sends
one request a bar and the channels join them.

**Voices.** The square is on or off, the way the speaker was. The sine, triangle and
sawtooth are the usual shapes. The noise voice ignores the note's pitch and is the
tracker's hat and snare, shaped by `Prelease`. The drum is a sine whose pitch falls two
octaves within the first tens of milliseconds of the note and whose level decays through
it: a kick, at `Pdown` 24 from C5.

**Channels and queues.** A channel plays one request at a time and holds thirty-two, the
playing one among them; a request sent while the channel is busy starts the sample after
the ones ahead of it end, so a line built from one request per bar has no seam, and a
request sent a little early costs nothing. Requests on different channels sound together.
A request past the depth is dropped. Channel 0 is the speaker: a plain DECPS queues there,
as the VT520's one tone generator did.

**Together.** Each channel chains its requests from its own start, so two parts started
a few milliseconds apart stay a few milliseconds apart, and a part entering on an idle
channel starts as its request arrives. `Pgrid` names the bar, in 1/32 s, and a request
that names it starts on the bar: the first such request sets the grid's origin and starts
at once, and every later one starts at the grid's next point or, if the last point
passed less than a tenth of the grid ago, in step with it, as though it had started
there. So requests sent together start together to the sample, an entering part sent
early waits for the bar, and one sent a little late joins it. The grid places a request
that finds its channel idle; one queued behind another follows it the sample after it
ends, whatever the grid says, so a bar cut into runs of different note lengths stays one
bar. The grid is forgotten when every channel falls silent or the synth is stopped, and
a request without `Pgrid` starts as before.

**Repeating.** `Prepeat` plays the request again when it ends: the next pass begins on
the sample after the last one, so a loop is joined the way two requests on one channel
are, with no silence between them. 255 repeats until something stops the channel, which is what makes a
background track one request rather than a program that has to stay alive feeding the
channel and gaps whenever it is busy. A repeating request holds its channel: anything
queued behind it waits, and stopping the channel empties the queue with it, so a part that
must change puts the change on another channel or stops this one first.

**Stopping.** A request at volume 0 stops its channel and empties its queue; on channel 0
it stops every channel. `CSI 0 , ~` is the whole synth silenced. RIS (`ESC c`) does the
same. A request that stops is not gated the way one that sounds is (see Consent below):
losing permission to make noise is not a reason to be unable to stop making it.

**Loudness.** Each note's amplitude is weighted by the inverse of the ear's sensitivity at
its pitch, after the A-weighting curve at half strength, normalised at C5 and bounded to
one octave of gain either way, so a sequence that climbs does not get louder as it climbs.
Each voice is scaled to the loudness of a sine at the same volume. The channels are summed
through a soft limit, so a chord cannot clip.

**Filters.** `Plowpass` and `Phighpass` put a second-order Butterworth section on the
request's output, after its envelope and before its send: the low-pass keeps what lies
below its cutoff and the high-pass what lies above, each falling twelve decibels an
octave past it. A pad softened by a low-pass at a kilohertz, a hat brightened by a
high-pass at three: the usual tracker treatments, one cutoff each.

**Reverb.** `Preverb` sends a share of the request, in sevenths, to a hall the channels
share: Schroeder's design as Freeverb tuned it, four damped combs into two allpasses,
about a second of decay. The dry signal is untouched, and the hall is added before the
limiter. The hall is mono and reaches both sides equally, which is what a chime or a
jingle wants and costs one filter chain rather than two.

**Stereo.** The mix has two sides and `Ppan` places a request across them. The centre is
both sides at full gain rather than the 3 dB down a constant-power law would give, so a
request that names no pan is as loud as one that names the centre and no caller pays for
the parameter by leaving it out. Each side falls linearly to silence at the far edge, and
the soft limit is per side, so a hard-panned part cannot clip the side it sits on.

### DCS + s — downloadable samples

A sample is downloaded the way a soft font is:

```
DCS Pslot ; Pformat ; Ptag + s <base64 of a WAV file> ST
```

`Pslot` is 1 to 25, the number a note names, or **0 to ask the bank for a free one**;
`Pformat` is 0, a RIFF/WAVE file with a PCM or IEEE-float `fmt ` chunk (plain or
extensible), 8, 16, 24 or 32 bits (float at 32), one to eight channels mixed to mono, any
rate from 1 kHz to 384 kHz resampled to the synth's 44.1 kHz, cut at ten seconds. Anything else is refused and the slot keeps what
it had. The bank is dropped by RIS, as the soft font is, and DECSTR leaves it. The
`dcs_allow` and `dcs_deny` lists name it `sample`, and it is gated by the sound capability
below: a download is the bytes a later request makes audible.

**Slots belong to nobody.** The twenty-five numbers are the terminal's, not a program's,
and a program naming a slot can replace its contents. `Pslot` 0 asks the
bank to choose instead, and the terminal answers with the number it filled:

```
DCS 0 ; 0 ; 4660 + s <base64> ST     ->   DCS 7 ; 0 ; 4660 + s ST
```

Slot 0 in the answer means it filled none — the bank was full, the file did not decode, or
the capability was refused. A download that names its own slot and no tag is answered with
nothing: an acknowledgement is returned only when the request asks the bank
to choose a slot or supplies a tag. A `Pslot` past 25 or a `Pformat` other than 0 is
ignored and answered with nothing.

**`Ptag` is the program's own name for its sample**, 1 to 65535 (0 is "no name"). It exists
because a slot number cannot answer the question a program actually has. A session outlives
the program that filled the bank: reattach to a coding session an hour later, find slot 7
occupied, and nothing tells you whether it is still your chime or something another program
put there while you were away. Ask by tag instead:

```
CSI ? 7411 ; 4660 n     ->   CSI ? 7411 ; 4660 ; 7 n    still in slot 7
                             CSI ? 7411 ; 4660 ; 0 n    gone; upload it again
```

Uploading with `Pslot` 0 a tag the bank already holds lands on the same slot rather than
spending a second one, and a download to a named slot clears any other slot holding its tag, so re-uploading after a "gone" answer is safe, and so is uploading unconditionally if
a program would rather not ask. Derive the tag from the sample — a hash of the file — and
a program can recognize its own sample without retaining a slot number.
The 16-bit tag is not collision-resistant identity or an ownership boundary.

**Giving a slot back.** An empty payload clears a slot, the way a zero-length DECUDK value
clears a key. Name the slot, or name 0 and the tag when the program never learned its
number:

```
DCS 7 ; 0 + s ST            clears slot 7
DCS 0 ; 0 ; 4660 + s ST     clears whichever slot holds that tag
```

A clear is answered as a download is, with the slot it emptied.

The sample voice (`Pwave` 6) plays a slot: the first note of the request from 1 to 25 names
it, every sounding note replays it, a rest is a rest. A `Pduration` of 0 holds each note
for the sample's length; any other cuts or pads the sample to the note. `Pdown` slows the
sample by the same ratio it lowers a tone, so one drum sample is a kit, and `Pnotevolume`
accents a hit. A slot's level is its volume's alone; the loudness weighting is for pitched
voices.

A kick, a snare and a hat, downloaded once and then played as a bar:

```
DCS 1;0 + s <kick.wav in base64> ST
DCS 2;0 + s <snare.wav in base64> ST
DCS 3;0 + s <hat.wav in base64> ST
CSI 5:1:6 ; 4 ; 1;0;0;0;1;0;0;0;1;0;0;0;1;0;0;0 , ~
CSI 4:3:6 ; 4 ; 0;0;0;0;2;0;0;0;0;0;0;0;2;0;0;0 , ~
CSI 2:2:6 ; 4 ; 0;0;3;0;0;0;3;0;0;0;3;0;0;0;3:3;0 , ~
```

A program that only wants a notification, an agent harness at the end of a task, downloads
its chime at startup with `Pslot` 0 and a tag, and plays `CSI 5:8:6 ; 0 ; Pslot , ~` with
the slot the bank answered when it is done.

### Consent, and asking before you play

Sound is the one thing here that leaves the screen. It reaches a person who is not looking
at it, and the people beside them, and until a program is stopped it can hold eight channels
and ten-second samples. So it is a named capability — `PlaySound` in the one `Capability`
vocabulary (`Source/PAL/Capability.h`) — in the same tier as the clipboard and the window
controls: by default allowed for a local program, prompted for an attached agent, denied for
a remote peer. Downloading a sample asks the same question, since it is the same effect one step
earlier. Stopping does not ask: a program that has lost permission to make noise must still
be able to stop the noise it made.

The user has two settings under `[sound]` — `audible` and `volume` — and the volume applies
to the mix rather than to each request, so moving it changes what is already sounding.

None of that is visible to a program, which is the problem `CSI ? 7410 n` solves: a terminal
with no synth and one whose user turned sound off both consume a request in silence, and a
chime that made no noise is worse than one that was never sent, because the program believes
it told somebody.

```
CSI ? 7410 n   ->   CSI ? 7410 ; Pstate ; Pvolume ; Pslots n
```

| field | meaning |
|---|---|
| `Pstate` | 0 the terminal has no synth, 1 sound is audible, 2 the user turned it off or set the volume to 0 |
| `Pvolume` | the user's master volume, 0 to 100 |
| `Pslots` | which sample slots hold something, as a bitmask with bit 0 for slot 1 |

A program that gets 0 or 2 should show something instead. `Pslots` rides along because it is
the same question one step later and costs no second round trip; `CSI ? 7411` above answers
the sharper form of it.

### Remote sessions

A session hosted by `ubiquitty-host` is heard by the windows attached to it, on this
machine or another: the host carries each admitted DECPS request as a sound effect in
the pane's effect log, and ahead of a sample-voice request whose slot the window does not
hold at that content, a `SampleHead` frame with the exact sample the request named when it
was parsed. The window plays through its own synth, from the samples it was sent. The
daemon itself makes no sound and has no synth, so `CSI ? 7410 n` in a hosted session
answers `Pstate` 0.

**Detection.** DECRQSS of the control's own final, `DCS $ q , ~ ST`, answers with the
extension's bounds: the request's grammar with every parameter at its top, then a
fourth group naming the sample bank's slots and the seconds a slot holds.

```
DCS $ q , ~ ST   ->   DCS 1 $ r 7:8:6:48:5000:5000:7:5000:20000:20000:7:64 ; 255:255:255:255 ; 49:7:15:255 ; 25:10 , ~ ST
```

A terminal without the extension answers DECRQSS's invalid form, `DCS 0 $ r ST`, or
nothing at all; pair the query with DA1 for those. Ask before sending a score: a
terminal whose DECPS is the speaker alone, and which takes the first piece of each
sub-parameter, plays every channel's part on that one voice at once, and mintty reads a
colon piece after the volume as its own tone style, which persists until changed or
reset. The plain form is safe everywhere, since every terminal with DECPS plays it as
the VT520 did.

An example: a bar of four-on-the-floor on the drum voice, an off-beat hat on the noise
voice, and a bass line on the square an octave and a fifth down, all at 120 to the minute
(a sixteenth is 4/32 s):

```
CSI 5:1:5:24 ; 4 ; 1;0;0;0;1;0;0;0;1;0;0;0;1;0;0;0 , ~
CSI 2:2:4:0:0:0:7:20 ; 4 ; 0;0;10;0;0;0;10;0;0;0;10;0;0;0;10;0 , ~
CSI 3:3:0:19 ; 4:1 ; 10;0;10;10;0;10;0;10;10;0;10;10;0;13;0;8 , ~
```

## Reset behaviour

| | DECSTR (`CSI ! p`) | RIS (`ESC c`) |
|---|---|---|
| 7401 password, 7402 fullscreen | kept | cleared |
| 7403 pointer lock | **cleared** | cleared |
| OSC 7404 blocks | kept | cleared |
| 7400 Tek pen | cleared (back to the configured default) | cleared |
| SGR extensions | cleared with the pen | cleared |
| DECPS channels | kept | **stopped** |
| DCS + s samples and their tags | kept | cleared |

7401 and 7402 survive a soft reset because a program that resets its own rendering state
should not drop the session out of a password prompt or out of fullscreen. 7403 does not,
for the stranded-cursor reason above.

## Related

`Docs/tab-status.md` covers OSC 21337 and OSC 7501, third-party protocols Ubiquitty
consumes rather than defines, beside the OSC 9, 99 and 777 notifications. Renderer-side
detail for the extensions above lives in `Docs/variable-weight.md` and
`Docs/wide-gamut.md`.
