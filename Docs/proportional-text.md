<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Proportional text runs

An extension of the kitty text-sizing protocol (OSC 66) that lets an
application set a run of text in a proportional face inside the cell grid,
and learn how many cells it needs before it does.

## Measurement and placement

A terminal's model must lay text out without a renderer: it is replayed
headless, mirrored over the Sync wire to a client that may have a different
font, and rendered by a bitmap backend that has no proportional face at all.
So the cells a run occupies cannot depend on what a font measures. The
protocol therefore splits the question in two:

- **Measure** — `OSC 66 ; p=1 : q=1 : i=<id> ; <text> ST` prints nothing and
  answers `OSC 66 ; i=<id> : c=<cells> ST`: the cells the text needs in the
  terminal's proportional face at the current type size. The renderer in front
  of the terminal answers; a host, which runs a pane and draws nothing, answers
  in the faces the pane's clients draw runs in (see "A pane a host runs"); a
  terminal that draws runs but has no measurer answers with the monospace
  width, which is what the text would take set in the grid's own face. A
  terminal whose renderer does not draw runs does not answer (see "Whether a
  terminal draws runs").
- **Print** — `OSC 66 ; p=1 : c=<cells> [: h=<0|1|2|3>] ; <text> ST` writes the
  run into exactly `c` cells and advances the cursor by `c`, whatever the
  face. The claim fits as a sized block does: it moves whole to the next row
  when the right edge, or the last visible column of a doubled row, arrives
  inside it or the wrap is pending, and one wrapping past the scroll region's
  last row scrolls the region a line, as the text would. A claim ending at the
  right edge leaves the cursor on its last cell with the wrap pending, so the
  text after it starts on the next row. The renderer fits the text into those cells: at its natural advances
  when they fit, aligned by `h` (left, right, centred, justified), and with the glyph
  overflow clipped to the run when they do not — a renderer never changes the
  cells a run occupies, because every other consumer of the model already
  agreed on them.

The measure is the text's advance divided by the cell width, rounded up, so
grid text after a run printed at its measure starts on the next cell
boundary: the space between the run's last glyph and that cell is under one
cell, and nothing else is added. A space the application writes after the run
is a grid cell of its own, so a sentence around a run shows that remainder
plus one cell; the run's text carries no trailing space for it to double.
A query is answered by the measurer of the face the frame draws, which a pane
adopts before its program runs; an answer in the monospace width is for a
terminal with no renderer at all.

## A pane a host runs

A host runs a pane's program and draws nothing, so it has no face of its own
to measure in. Every client that draws runs publishes the faces it sets them
in, in a `ProportionalFaces` frame (Docs/sync.md): its own measurer's advance
for each code point in U+0020–U+024F, U+0300–U+036F, U+0370–U+03FF
and U+0400–U+04FF,
shaped alone, in the regular,
bold, italic and bold italic faces, and the grid cell those advances are
counted against, in 1/64 of the measurer's pixel, the advances rounded up and
the cell down. Weight zero keeps its measured advance; weights one through seven
share the largest advance measured at those steps, rounded up in sixteenths
of a cell. The two weight classes keep the frame bounded while a variable-weight
run cannot receive less than its individual glyph advances. Core measures them
through the measurer the frontend installs
(`OSC::TextSizing::measureProportionalFaces`). A client offers
`PublishesFaces` in its Sync `Greet` only when the pane's renderer draws
proportional runs. It publishes after a hosted pane that it may drive
attaches, and again when the faces it draws
proportional runs in change. The pane must actually draw proportional runs;
a grid-only pane, a local pane and a viewer publish no run faces.

Before it reads a pane's output, the host gives the pane the faces of every
person viewing it who may drive it (`OSC::TextSizing::PublishedFacesMeasurer`):
an observer or an agent draws the pane without deciding its layout, so its
faces are not counted. A query is answered from the upper envelope of their
glyph advances, so a program that lays out by it clips on no
such client, and a client with a narrower face shows the slack. Repeats count
once. For every tabled code point and weight, the host keeps the maximum
advance over all current offers, so a narrow regular face with a wide bold
glyph cannot be discarded by an unrelated ranking. Removing an offer rebuilds
that envelope from the remaining offers. Outside handover, faces leave with the client that
published them: a pane no driver views answers in the grid's width, as one
never viewed does. A rewrap on the host measures its pieces the same way. A
client that attaches after a program measured changes no answer already
given. A handover carries each attachment's published faces; the successor
counts them until that attachment's client republishes on resume. At most
64 faces may be offered to one pane: 824,064 face-body bytes and one 34,304-byte
envelope, or 858,368 bytes. At most 64
current-client bodies and 256 inherited attachment bodies may be retained by
one daemon: 4,120,320 face-body bytes and at most 64 envelopes, or 6,315,776
bytes. The pane shares these bodies rather
than copying them; these counts exclude the shared-pointer bookkeeping.

When an attached driver offered `PublishesFaces` but has not yet published
faces, the host holds that
pane's OSC 66 measure queries for at most 250 ms from its first held query.
It answers them in the face envelope as soon as the owed publication arrives.
At the deadline it answers in grid width. It holds at most eight measure
queries, 32 total ordered replies and 64 KiB of later literal reply bytes per
pane; reaching any bound answers the held queries in grid width and subsequent
queries use grid width until the owed publication arrives. Replies sent after
a held query remain behind it in the order the program requested them. A pane
with no attached driver offering `PublishesFaces` owes no publication and
answers immediately in the faces already held, or in grid width when it has
none.

An untabled code point uses the face's widest tabled advance for the run's
weight. One Core classifies as two grid cells uses at least two cells instead.
This bounds a real glyph when that face's widest glyph is among the tabled
scripts, without a single CJK character or emoji claiming the entire row.
The renderer clips text to the claim when the pane is narrower.

A face whose cell is under a pixel, or which gives a tabled code point more
than eight cells, is refused.

The count is the sum of the published advances over the cell, rounded up: a
local pane's measure without what shaping does between letters. Pair kerning
and ligatures close a Latin run up, so a hosted answer is wider than a local
one by what they closed, which leaves slack, and narrower only where shaping
widens the text. Once every attached driver has published, the answer depends
on the faces and the text alone. A query during the bounded publication
interval can take the grid fallback if the deadline passes.

## Whether a terminal draws runs

An application detects the extension by measuring: it sends a query and waits
for the reply, and no reply means no support. So the answer depends on the
renderer, not on the parser: Core answers only where the frontend has said its
renderer draws runs, through `Terminal::drawsProportionalText`. That fact is
independent of `Terminal::drawsSizedText`, since a renderer can crop sized
bands without shaping a run and the reverse.

With the fact clear, the terminal looks like one that never learned the
extension: a measure query goes unanswered, a placed run prints its text as
ordinary grid text with the cursor advancing by the text rather than by `c`,
and a Sync replica flattens any run a peer sends to its anchor's text. No
other report names the extension: no terminfo capability, XTGETTCAP name,
device attribute or private mode advertises it.

The fact is off by default wherever an embedder brings its own renderer:

| Surface | The fact | Default |
|---|---|---|
| libUbiquittyCore | `Ubiquitty::Terminal::drawsProportionalText` | off |
| Session runtime | `Runtime::DrawnText` at `TerminalSession::start` and `WorkspaceSession::make` | `GridOnly` |
| `ubiquitty.js` module | `ubiquittyWebSetDrawsProportionalText(int)`, read back by `ubiquittyWebDrawsProportionalText()` | off |
| The bundled web page (`terminal.js`) | turned on after `ubiquittyWebCreate`, since its proportional atlas draws runs | on |
| macOS, iOS, Linux, the host daemon | `DrawnText::SizedAndProportional` or set directly | on |

A host that drives the module for its own grid renderer leaves it off and
needs to do nothing; a user of the bundled page gets proportional runs with
nothing to set. The snapshot drivers' `--draws-sized-text` declares both
facts, since every snapshot renderer draws both.

An application that wants a laid-out interface measures each label once,
chooses its columns, and prints with `c`. The same stream replays identically
on every terminal that knows the protocol, and a terminal that does not know
it drops the code whole, which is the text-sizing protocol's own degradation.

## The keys

| key | range | meaning |
|---|---|---|
| `p` | 1 | the run is proportional; required for every key below |
| `c` | 1–1024 | cells the run occupies; required to print, ignored by a query |
| `q` | 1 | measure only: reply, print nothing |
| `i` | 0–255 | request id, echoed in the reply; default 0 |
| `h` | 0–3 | horizontal alignment inside the `c` cells: left, right, centred, justified — 3 opens the run's word gaps evenly until the text spans its cells |

Of the sizing protocol's own keys a run takes one: `s`, 1–7, which sets it
`s` rows tall with each of its `c` cells `s` columns wide (see "A run at a
scale"). `w`, `n`, `d` and `v` refuse with `p`: `w` would be a second width
beside `c`, and `n`, `d` and `v` set a glyph smaller inside its block, where
a run's cells are what its text is measured and laid out in, so there is no
reading of them a later revision would not have to keep. Undefined
combinations are rejected. The payload is at most 4,096 UTF-8 bytes per run. The run-text store
holds at most 262,144 live code points across both screens and retained history.
A terminal holds at most 15 size specifications at once, and each distinct
specification a cell still names takes one: a sized block's keys, or a run's
`c`, `h` and `s`. A run the specification table or the run-text store has no
room for, after a reference census, still takes its claim: its text is set in
the grid face inside the `c` cells, each grapheme at the cells the grid's
print path gives it from the claim's first, right or centred by `h` when
narrower and cut at the last whole grapheme the claim holds, the claim's other
cells erased, and the cursor advances by `c`. The cells are written as the
run's own are, so the text joins no cluster before the claim, follows no
right-to-left writing and stays inside the claim. That is the run as a
renderer with no proportional face draws it, so the program's layout holds.
Reclamation retries within 64 placements. A line of text is one run: runs snap to whole cells, so a sentence
split into runs opens a gap of up to a cell at every seam, which is not a
word space. Text longer than a run is the application's to split at line
breaks, each line with its own width. `c` runs to a row's width, 1024 cells; a renderer that
draws a run glyph by glyph draws all of it. A strip renderer uses bounded
horizontal tiles of the complete shaped run, so the atlas slot's width does
not limit the text it can draw.

## A run at a scale

`OSC 66 ; p=1 : c=<cells> : s=<scale> ; <text> ST` sets the run at the
sizing protocol's scale: `s` rows tall, and each of its `c` cells `s`
columns wide, so the run claims `c × s` columns of each of `s` rows, as a
sized block of the same scale does. The text is laid out in the run's own
`c` cells exactly as at scale 1 and drawn `s` times the size, so a run and
its scaled copy wrap their letters at the same places.

- **Measure.** A query takes `s` too and answers in the run's own cells,
  which the scale enlarges with the text: `p=1:q=1:s=3` answers what
  `p=1:q=1` answers, and printing that `c` with the same `s` fits the text.
- **Placement.** The run is a block: every one of its `s` rows holds an
  anchor and continuations over the `c × s` columns, every cell stamped with
  the spec and its band, and every band's anchor names the run's text, so a
  row draws its slice without walking to the block's top. Every band row is
  flowed. It fits and refuses as a sized block does: it wraps whole to the
  next row when the margin arrives mid-run or the wrap is pending, and a run
  wider than the writable columns, or taller than the rows left in the scroll
  region, prints as ordinary text. The cursor advances `c × s` columns on the
  block's top row.
- **Editing and reflow.** The block's bands follow the sized-block rules: an
  edit that tears a band erases the block. A rewrap moves each band whole
  with its row and never breaks it at its spaces or clamps it to a narrower
  row, since a band changed by its own row would no longer stack on the
  others; a band wider than the new row drops whole, and a rewrap that sets
  the bands apart leaves a block the scrub erases.
- **Selection and copy.** A scaled run is selected letter by letter, as
  kitty selects scaled characters: each letter is a glyph `s` rows tall over
  its columns — the cell its ink begins in, and the cells after it inside the
  text that no letter's ink begins in, up to the next letter's; the last
  letters' columns run through the last cell the text's advance reaches,
  where their ink runs on past the cell it begins in — and a selection that
  holds any of those columns on any band row (top, bottom or between) holds
  the letter. The letters held are lit the same on every band
  row, whatever that row's own span reached, cut inside a packed cell where
  a caret at the selection's ends stands between two letters; a drag across
  part of the run, or across its edge, holds exactly the letters whose
  columns it reaches on the rows it passes through; word and line presses
  read the run's text as they read a run of one row; and a caret stands at a
  letter boundary on the row the pointer is on. The copy carries each held
  letter once, in the text's order, where the selection first reaches the
  run, and a band row that adds nothing else adds no line. The cells before
  an aligned text and the claim's tail after the text's last cell are no
  letter's: they light, on every band row, only where a band row's span
  reaches them, and the tail is the row's void as a run of one row's is. The
  text's last cell is the one its advance ends in, for a run of one row as
  for a scaled one: a pointer on the ink a letter runs into the next cell is
  on the letter, not in the void.
  Each piece of a band row's span names the letters it holds
  (`SelectionRowSpan::firstLetter` and `endLetter`), and the renderer lights
  those letters wherever its own layout draws them, so a renderer that puts
  a letter a cell from where the measurer put it still lights it. A
  terminal with no measurer — the headless host, or an embedder of
  libUbiquittyCore that installs none — lays the whole text in the anchor's
  cell, so its runs select whole; the macOS, iOS, Linux and web frontends
  and the snapshot runner each install one.
- **Accessibility.** The text reads once, on the block's top row, each
  letter at the columns of the cell its ink begins in, and a position on a
  lower band is the letter boundary above it, so a selection made on any
  band names the letters it holds.
- **Sync.** The spec's scale already travels in the text-size table, so the
  wire is unchanged. A replica whose renderer draws runs but no sized bands
  flattens a scaled run as it flattens a sized glyph.
- **Without sized bands.** A terminal whose renderer does not crop sized
  bands (`Terminal::drawsSizedText` clear) places the run at scale 1, one
  row of its `c` cells, which is how a terminal that never learned `s` reads
  the code.
- **Drawing.** The renderer draws the run as one picture `s` times the size
  standing on the block's top row, of which each band row draws the part
  inside its row: the generalisation of the DECDHL pair, whose run is the
  picture at scale 2 cut at the midline. No arm stretches it. On the
  per-glyph route each glyph is drawn from slot-sized windows of a raster `s`
  times its size (`GlyphBackend::rasteriseSizedProportionalGlyph` with the
  tile's scale, band and window, the master `selectSizedMaster` selects), one
  atlas pixel to a screen pixel, and a row faults only the slot heights that
  reach into it. The strip route asks `GlyphBackend::proportionalStrip` for
  the strip at the same size, a slot height at a time: the run's layout taken
  `s` times across, drawn from a master on a canvas `s` slots tall, of which
  each band row draws the slot heights that reach into it. The DECDWL row's
  and the DECDHL pair's runs take both routes the same way. The master is
  each arm's own text at that size: CoreText under a scaled CTM, FreeType at
  the scaled em, the browser at the scaled pixel size, and on the bitmap
  tier the strike that size is at 12 and 24 px and the face's outlines at
  every other size, so every stroke is drawn at the size rather than a
  strike's pixels magnified. A backend with no
  master at all leaves the builder its stretched quad.

## What a frontend supplies

One thing: a shaper. `TextMeasureClient::shapeProportionalRun` answers the
glyphs, advances and clusters its proportional face sets a run in, and the
renderer's `GlyphBackend::shapeProportionalRun` answers the same for the
frame. `GlyphBackend::bindProportionalAtlas` supplies the independent sheet's
Core cache and geometry; `rasteriseProportionalGlyph` draws one glyph into the
slot Core assigns and records its measured ink through
`recordProportionalGlyphInk`. Everything placed from those answers is
Core's, once: alignment and justification, which cell each code point's ink
begins in, the source cluster a pixel falls on, and where each glyph's quad goes.
Selection reads that same layout. A frontend reports where the pointer stands
along its row, in cells and unrounded, and Core resolves it to a caret between
two graphemes of the run, so a drag selects and copies single characters inside
a cell that holds several letters' ink (see "Selecting inside a run"). A
frontend without a shaper draws the run in the grid face, one code point per
cell.

The renderer draws a run glyph by glyph where the backend offers the route —
slots keyed by the face's repertoire, which is bounded where text is not, and
no strip cap on the run's width. Core chooses a strip for a run whose glyph
ink overlaps, so weaker antialiased edges cannot erase earlier strokes;
missing identities, failed rasters, and exhausted caches also take that
atomic fallback. Cache ink records belong to the current slot identity and
are invalidated on eviction, reset, and failed rasterisation. The strip uses
the same `layoutProportionalRun` placements, including justification, and
does not change caret, selection, or model capacity. Each tile rasterises the
complete shaped run at an integral translated pen, with guard pixels outside
its disjoint visible interior; crossing glyphs keep their coverage without
being composited twice at a tile boundary. The native shape is cached across
the tiles, and the tile index participates in the verified raster key.

The proportional cache is independent of mono, wide, and ligature resources.
Core keys all 32 glyph-identity bits plus face, weight, phase, and raster tier;
font, variation, or scale changes replace the sheet. Its nominal 128-by-64
slot grid is bounded by the device texture limit. Linux and Web upload the
`ProportionalGlyphSheet` mirror; Metal supplies native raster storage over
the same cache and placement contract. Frontends consume separate glyph and
strip streams without reconstructing Core's choice.

A row holding a run carries `RowFlag::Flowed`, latched by the run's
writer and by the wire's row decoders, so a row without one costs the
renderer's walk one byte test and every reader that maps pixels to text asks
the run's layout only where a run is.

## What the model stores

The run is one anchor cell followed by `c − 1` continuation cells, the shape
a wide glyph and a sized block already take, so every editing control —
erase, scroll, reflow, selection, the wire — treats it by the rules it
already has. The anchor's cluster field names the run's whole text in
`RunTextStore`; continuation cells carry only the claim. The proportional spec
distinguishes this id from a grapheme-cluster id. Every cell carries the spec id
of a `TextSizeSpec` whose `isProportional` is set and whose `runWidthCells` is `c`.
Every cell also carries the pen the run was printed under, so the run is bold,
faint, slanted, ruled, inverted and coloured as grid text printed under the
same SGR would be, and a Sync replica receives that pen with the cells.

A rewrap reflows a run as it reflows text. A run whose claim fits what is
left of its row stays whole. One that does not breaks after a space into
pieces: each piece ends at the last space whose words fit what is left of
its row, measured as a query measures them, and the next piece opens the
next row. A piece is a chain of its own whose anchor holds its share of the
text, the break's space included, so a copy across the wrap carries every
letter and the space between the words; the space is outside the piece's
cells. Every piece's spec keeps the `c` the program asked for in
`claimedRunWidthCells`, and every piece but the last sets `isContinued`. A
later rewrap finds the pieces cell after cell — erased cells closing a
wrapped row between them do not count — joins their text, and sets the run
again, so widening back gives the run its claim and its own spec.

A run set right, centred or justified, a run on a doubled row, and a run
with no space to break at move whole. One wider than its row is clamped to
the row, its text intact, under a spec that keeps its claim, and a word
wider than a row is a piece clamped the same way. A rewrap that cannot
intern a piece's text or spec moves the run whole. The cursor keeps its
place in the text: after a run, it moves by the cells the run gained or
lost.

`Rendering/ProportionalRun.h` maps shaped code points to cells for selection.
Without a shaper, the anchor owns the whole run. Copy reads the stored text
without duplicating continuation cells. A torn run — a continuation whose
anchor is gone — is erased, which is the sized-block policy.

## Selecting inside a run

A selection caret is a cell boundary plus a count of code points into the cell
to its right — `Selection::CaretResolution::subCaret`, and on the bounds a copy
is cut by, `SelectionBounds::subColumn0` and `subColumn1`. The count is nonzero
only inside a proportional run: everywhere else it is zero, and a zero at both
ends is a plain cell selection. One field, and nothing
else in the grid gains a sub-cell coordinate.

`Selection::resolveSelectionCaret` is where a pixel becomes that caret.
It lays out the one run under the point, through the frontend's measurer and
`Rendering::proportionalRunCaretAt`, and answers the gap between letters the
pixel is nearest; the cell is the owner of the letter the caret stands before,
by the same `proportionalRunCellOwners` map a copy of that run reads, so the
cell a drag names and the cell a letter is copied from cannot disagree. A caret
never lands inside a grapheme: a combining mark or an emoji sequence moves it to
the cluster edge on the side the pixel rounded towards. A point on a cell that
holds no run shapes nothing.

Two things the caret is not. It is not the host's cursor: the host addresses
cells, a cell of a run holds only the letters whose ink begins there, and no
position inside a cell is addressable — a host edits a run by printing the run
again at its anchor. And it is not what an application's mouse protocol
receives: a `PointerFact` carries `row` and `column`, the cell under the
pointer, and every report rung reads those; `caretSubCaret` travels to the
selection model and stops there.

The highlight is drawn in whole cells. A selection cut inside a packed cell
paints that cell entire and copies the letters the caret named, so the copy is
finer than the band drawn over it.

## What a renderer draws

The complete text across the anchor and continuations is shaped as one line
in the proportional face. Core aligns it by `h`, clips overflow at the run's
claim, and emits either independent glyphs or nonoverlapping strip tiles.
The terminal's cell grid keeps its fixed pitch. The proportional face is the
platform's UI face (San Francisco on macOS, the fontconfig sans on Linux,
and CSS sans-serif on Web). The bitmap
families have a proportional face of their own: Noto Sans, Regular and Bold,
pre-rendered into Core at 12 px and 24 px (`Source/Core/Rendering/
ProportionalStrike.h`, baked by `Tools/gen-proportional-strike`, which runs
FreeType and HarfBuzz), one strike for each of the bitmap tier's first two
scales; at every other size — a grid upscaled past two, a run at an OSC 66
scale, on a DECDHL pair or a DECDWL row — the face is drawn from its own
outlines (`Source/Core/Rendering/ProportionalOutline.h`, baked by
`Tools/gen-proportional-outlines`) at that size, by the area of each pixel
the glyph covers, unhinted, as FreeType's smooth rasterizer draws an
unhinted outline, and never a strike magnified. Unifont, the tier's grid
face, has one design of 16 pixel rows, and past it the only drawing true to
it is whole-texel magnification, which is what its glyphs get. The strike records
what the Linux arm draws: advances and pair kerning from HarfBuzz in 1/64 px,
and a light-hinted raster of each glyph at every subpixel phase Core places
at. A run in the bitmap tier walks a fractional pen and draws each glyph's
raster for the phase the pen lands on, so it is FreeType's text at that size,
kerned as HarfBuzz kerns it, and the same bytes on the portable renderer, the
Linux CPU and Vulkan paths, the web arm and Metal: the Metal sheet
is cut to the plan the reference sheet cuts for the same cell
(`unifontProportionalGlyphSlotPlan`) and asks Core to shape the run and draw
each glyph into its slot (`shapeUnifontProportionalRun`,
`rasteriseUnifontProportionalGlyph`, sharing the reference sheet's own raster)
rather than asking CoreText; the strip fallback's slot is likewise Core-drawn
(`ProportionalStripRaster.h`). Unifont's cell has fewer rows below the
baseline than the face's descenders need, so a proportional slot in the
bitmap tier is cut taller than the cell by the strike's overhang on both
sides and its quad reaches into the rows beside it, the way the native tiers'
slot margins let a glyph's ink reach past its cell. A renderer whose face has
no glyph for a code point sets that code point in the grid's face across the
run's cells, clipped: the run still occupies what the model said.

A rule under a run drawn glyph by glyph is cut per cell: each cell of the
claim emits its own rule, tapping the slot of the glyph that inks it most
(the dedicated sheet is the fourth sheet a rule's taps may name) and its
neighbours theirs, so a descender breaks the underline as it does under grid
text. One rule over the claim could name one slot, and the glyphs are in as
many; cell by cell rather than glyph by glyph because a patterned rule's
phase is the cell's. A strip's run keeps its one tap over the whole claim.

The measurement is the same shaping: `ceil(advance / cellWidth)` for the
text in that face, with a floor of one.

Inside a strip, where two glyphs' coverage overlaps, each arm joins it the
way its own text stack joins it, so the strip is that stack's text. CoreText's
join depends on the face object: a face registered for the process takes the
per-pixel maximum of the two coverages, and a system face or one built from a
file's descriptor lays each glyph over the last, source-over. The CoreText
arms follow that per face. FreeType rasterises one glyph at a time and has no
join of its own; the FreeType arm takes the maximum.

## What the macOS renderer does

CoreText supplies glyphs and metrics from the system UI face or the
configured family, including the bold and italic variants. Each style is the
family's own member, asked for by its symbolic trait: a weight asked of the
regular face moves only a variable face's axis, so a family of static faces,
such as SF Pro Text, would otherwise set its bold in its regular. A family
with no bold member but a weight axis takes the axis. Core places independent
glyphs in the dedicated atlas or chooses the tiled strip route for overlapping ink and native
fallback runs. A strip retains the complete line and key regardless of the
slot width; every tile uses Core's translated pen, clip, and visible interior.
A run whose text fits keeps the margin's overhang each side; one that
overflows is cut at its last cell.

The face is built at its resolved point size and scaled to device pixels by a
font matrix. It is not requested at the device-pixel size: optical sizing and
tracking depend on point size, independently of display scale.

The measurement is the same CoreText line: `ceil(advance / cell)`, answered
on the reader thread through the model's client, shaped once per text and
face and remembered. The first frame cuts the renderer that answers; a query
that arrives before any frame is answered in the grid face.

## What the Linux renderer does

The Core strip sheet shapes the run with HarfBuzz in fontconfig's
`sans-serif` face — `sans-serif:bold` under SGR 1 — at the grid's pixel
size, through the same `proportionalStripLayout` the CoreText arms place by,
and draws it on the grid's baseline with FreeType. Fractional pens are passed
through FreeType's transform before rasterisation, and the matching residual
participates in the cache key. The measurement shapes the same way and answers
through the renderer. A backend without native sized-glyph hooks uses the
builder's grid-glyph fallback stretched over the declared cells.

## A proportional face as the grid face

The runs above are one channel; the other is a proportional family chosen as
the grid's own `font.family`. Such a face is not a grid of its letters. Core
decides it from the face's advances (`FontMetrics::facePitch`, read from
unhinted printable-ASCII advances) and every arm follows one rule
(`Source/Core/Rendering/LetterLayout.h`):

- **The cell** is the advance of `0` (`FontMetrics::cellAdvance`): text faces
  carry their figures at one width so that numbers align, and the cell serves
  what still counts columns — the window's size and the winsize, a cell nothing
  has written, a glyph that keeps its cells. A face without `0` takes the mean
  lowercase advance.
- **Every row is laid out as proportional runs.** The cells of one pen that
  hold text are shaped together in the primary face
  (`GlyphBackend::shapePrimaryRun`: the face's advances and kerning, every
  ligature feature off) and each letter stands at the shaper's own advance
  from the one before it, packed as the typeface intends; a written space is
  the face's space. A run breaks where the pen changes. The builder draws each
  letter from the mono sheet at the letter's pen and phase, so the glyphs are
  the ones every row draws, placed where the face puts them.
- **Cell-bound glyphs keep their cells inside the row.** Box drawing, blocks
  and shades, braille, an emoji, a wide glyph, an OSC 66 claim and a downloaded
  bitmap advance the pen by their cells at the pitch, and a cell nothing has
  written keeps one cell's pitch, so a column a program addressed past its
  text stands where the grid put it.
- **The program's contract does not change.** Columns, rows, the winsize and
  the cursor column are the grid's; what moves is where a column stands on
  screen. Column alignment across rows is therefore not preserved, by design:
  `ls -l` lines up by cells and a proportional face has none to line up by.
- **Presentation follows the letters.** The cursor stands at its letter's pen
  with the letter's advance, every shape, still the inverting overlay tier; the
  selection highlights the letters' extents; a press, a drag and a report
  resolve a pixel to the letter under it through the frame
  (`FrameCapture::columnAt`, `xInCellsAt`, `letterExtent`), and the caret the
  ladder rounds to is a letter boundary; the background, an underline, a
  strikethrough and a skip-ink tap span the letter. The frame keeps every
  row's letters (`InstanceBuffers::letters`) and no frontend recomputes a
  layout: the Swift surface, the GTK pane and the page each ask the renderer
  that drew the frame.
- **A doubled row** sets its letters at twice their advances and draws each
  from the doubled master Core selects for sized text (`selectSizedMaster`):
  on a DECDWL row the face widened on its `wdth` axis toward twice where it
  stands and scaled across by only what the axis left, on a DECDHL pair the
  face at its own width drawn twice its size, each half from its band. An
  arm that keeps its doubled glyphs on a sheet of their own (the CoreText
  arms) draws each from that sheet at the letter's pen; an arm that cuts
  sized windows on its mono sheet (the software and Vulkan renderers, the
  web arm) draws the windows of the doubled master there, stepped from the
  letter's pen, the master drawn at its pen rather than centred so the
  letter stands where the row's layout put it and its cell carries
  background alone. Only an arm with neither keeps a doubled row on the
  pitch, one letter a doubled cell.
- **A backend that cannot shape** — one with no primary-face shaper, or whose
  shaper refuses a run — keeps that run on the pitch, one cell a letter. The
  cell-baked path keeps the grid too: a letter is placed by its own pen, and a
  glyph baked into its cell quad has none.

Symbol fallback and every fallback face keep their own placement. The
CoreText and FreeType arms shape through their own engines
(`PrimaryRunShaper.swift`, `FreeTypeRasterizer::shapePrimaryRun`); the web
arm measures each letter in the primary family, which is what a canvas can
answer, and lays the run out at those advances without kerning. The bitmap
tier is fixed pitch and never takes this path. The snapshot cases
`proportional_runs_sf_pro` and `proportional_runs_noto_sans` hold the
specimen rows, the latter in the test face `Ubiquitty Wdth Test` (Noto Sans
pinned at `wght` 400, subset and renamed, under
`Tests/Snapshots/fixtures/fonts`).

## The face

A run is set in one proportional face for the whole grid: the platform's UI
face, which is what the platform's own dialogs are set in, or the family
`font.proportional_family` names. Its size is `font.proportional_size` when
that is set; unset, the face is sized one optical step over the grid's em —
the grid size times 1.08, rounded to a quarter point and never under the grid
size. This is a fixed optical-sizing policy, not a font-metric match. An
explicit `font.proportional_size` overrides it. The arithmetic is Core's
`proportionalFacePointSize`
(`Source/Core/Rendering/CellDecisions.h`), used by the CoreText renderer;
the FreeType sheet above shapes at the grid's pixel size. Weight
and slant follow the grid face the run carries — SGR bold takes the family's
bold — and the run is drawn on the row's baseline pen, so a label shares its
baseline with the grid text beside it. Both keys are Settings' Proportional
Family and Proportional Size, and the Fonts preview's "Mixed faces" sample
sets a grid line and a run side by side.

## Bounds

- At most 4,096 payload bytes per run and 262,144 live code points per terminal.
  A run wider than the available row cannot be placed as a proportional block
  and prints as ordinary text.
- At most 15 size specifications live at once. A run past either bound is set
  in the grid face inside its claim, and the cursor still advances by `c`.
- A host counts a pane's runs in at most eight distinct published faces, each
  with a cell of at least a pixel and no code point wider than eight cells.
- Text that exceeds its declared cell claim is clipped at the claim, not at
  an atlas tile boundary.
