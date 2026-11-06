<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Rich text blocks

**Version 0.1 — Initial proposal**

OSC 7404 defines retained styled source and cell placements; OSC 7405 reports
interaction with that source. This proposal is intended for independent
implementations. The proposal version is distinct from the `v=3` reply schema
and the Sync transport version. The requirements below apply to the OSC
interface; sections naming Ubiquitty or Sync describe its implementation.

A program places rich text into a rectangle of cells: text in more than one
family, size, weight, slant and colour, wrapped or cut to the rectangle, and
selected and copied as text. Before it places anything it can ask what fits:
how many characters per line and how many lines a style gets in a rectangle,
or whether a given body fits and what extent it needs.

The body uses the Rich Text Format subset specified below. The escape's keys
carry the rectangle, overflow policy, default alignment, and block identifier;
character and paragraph styles belong to the RTF body.

The sizing protocol (OSC 66, `Docs/proportional-text.md`) sets one run of one
face inside a row; a block is the shape for anything with more than one style
or more than one line.

## The sequence

```
OSC 7404 ; <keys> ; <rtf> ST
```

The 75xx numbers belong to another terminal's protocol that Ubiquitty honours;
Ubiquitty allocates in 74xx.

`<keys>` is a colon-separated list of `key=value` pairs; `<rtf>` is everything
after the second semicolon, up to the terminator, and may itself contain
semicolons. BEL terminates the code as ST does.

Numeric values use unsigned decimal notation. Identifier domains (`i`, `D`,
`E`), flag values and enumerated choices are fixed by this draft. The workload
ranges for `w`, `h`, `k` and `v` instead use the terminal's advertised `Z`
profile; the numbers in parentheses below are Ubiquitty's current profile.
Source offsets must also be no greater than the document length. Advertising
a profile does not enlarge this draft's packet or geometry-assembly envelopes.

| key | values | default | meaning |
|---|---|---|---|
| `a` | `p`, `q`, `d`, `s`, `c`, `f` | `p` | Action: place, measure, delete, subscribe to typography changes, cancel a query, or enumerate font families. |
| `w` | 0–`Z.width` (1024) | — | Width in cells. Required by `p` and `q`. 0 leaves the width open and is accepted only by `q`. |
| `h` | 0–`Z.height` (1024) | — | Height in rows. Required by `p` and `q`. 0 leaves the height open and is accepted only by `q`. |
| `i` | 0–65535 | 0 | The block's id for `p` and `d`; the request id a reply echoes for `q` and `f`. Explicitly required by `f`. |
| `b` | 0–4294967295 | 0 | Font catalog only: zero-based family offset. A nonzero offset requires `E`. |
| `n` | 1–`Z.catalog-count` (255) | 255 | Font catalog only: maximum family count requested in this page. |
| `o` | `w`, `c`, `e` | `w` | Overflow: wrap, clip, or wrap and end with an ellipsis. |
| `j` | `l`, `c`, `r` | `l` | Alignment of a paragraph that names none in the body. |
| `C` | 0, 1 | 0 | 1 leaves the cursor where it was after a placement. |
| `g` | 0, 1 | 0 | 1 uses the terminal row height and baseline; `\fs24` follows the configured font size for its family, including zoom. Proportional families use the proportional size; `\fmodern` uses the grid font's size. |
| `M` | 0, 1 | 0 | 1 reports shaped character positions through OSC 7405; requires nonzero `i` and `D`. |
| `P` | 0, 1 | 0 | 1 retains an application overlay across ordinary cell writes. Full screen erase and leaving the alternate screen remove it. |
| `k` | 0–`Z.scalars` (4096) | absent | Logical scalar insertion offset of the native terminal caret inside the block. |
| `v` | 0–`Z.scalars` (4096) | 0 | Placement only: Unicode scalar source anchor for the first visible line, no greater than the document length. A nonzero anchor requires `g=1`. |
| `D` | 0–4294967295 | 0 | Placement only: application document revision, echoed in pointer reports. Required and nonzero with `M=1`. |
| `p` | `N` or `T,R,B,L`, each 0–255 | 0 | Padding in points: the content insets of `p` and `q`, one value for every side or four in the order top, right, bottom, left. Any other count, or a value past 255, refuses the code. |
| `y` | `t`, `c`, `b` | `t` | Where content that needs fewer rows than a placement covers sits in its content box: top, centre or bottom. A query accepts it and answers as it would without it. |
| `r` | `x,y,width,height,...` | full rectangle | Up to 32 disjoint visible rectangles relative to the placement, in cells. Placement only; each must be positive and inside `w`×`h`. |
| `G` | 0, 1 | 0 | Measurement only: include native grapheme caret geometry in bounded reply chunks. |
| `N` | 0, 1 | — | Subscription only: enable or disable font-change reports. Requires a nonzero `i`. |
| `e` | 0, 1 | 0 | Request status replies, including refusals. Requires an explicit `i`. |
| `E` | 0–4294967295 | absent | Measurement, font catalog and placement: require the current typography epoch to equal this value. A mismatch refuses the operation with `stale` when `e=1`. |

The keys follow the sizing protocol's strictness: an unknown key, a pair
without `=`, an empty or out-of-range value, a repeated key, a missing
required key, a body over the caps, or an invalid RTF body for a placement or
nonempty measurement refuses the whole code. A font catalog request requires an
empty body. A refused code changes nothing. Without `e=1` it answers nothing. With `e=1`
and an unambiguous `i`, it returns the status described below. A terminal that
does not implement the protocol remains silent.

There is no key-value form for styles. A plain block is `{\rtf1 text}`, which
costs eight bytes over the text; a second vocabulary for the common case would
be one more thing a program must learn and the terminal must keep in step
with the first.

### Place

`a=p` places a block whose top-left cell is the cursor, `w` cells wide and `h`
rows tall. The rectangle must fit the screen's width from the cursor's column,
and `h` must not exceed the screen's rows. When the rectangle reaches past the
bottom row, the screen scrolls up by the excess first, as that many line feeds
at the bottom would; with a scroll region narrower than the screen the code is
refused instead. A row with a double-width or double-height line attribute
refuses the placement.

A nonzero `i` names the block. Placing a block with the id of a live block on
the same screen removes the old one first, so a program updates a block by
placing it again. Validation and storage admission finish before the old block or
any cells change. Id 0 is anonymous: the block is placed and can be removed
only by what removes any block.

Unless `C=1`, the cursor then moves to the block's left column on the row
below the block, scrolling as a line feed would.

`r` clips presentation without changing paragraph layout. The complete source
still shapes at width `w`, including text behind a menu. Only visible rectangles
cover cells, draw pixels, receive pointers or contribute to visible screen text.
Two blocks may share a bounding box when their visible rectangles do not overlap.
Clip edges are rounded once to device pixels; every fragment samples the same
full paragraph raster. Splitting source at a clip edge would break contextual
shaping, bidi ordering and ligatures, so callers must not do that.

`D` identifies the styled source, independently of placement serial and typography
`E`. `D=0` is unversioned and cannot be used with `M=1`. An application
using nonzero revisions must change `D` when it changes that source and must not reuse
an `(i,D)` pair while input for its earlier source can still be in flight.
Repositioning, clipping, caret and viewport updates may retain `D`. Replacing a
live block with different source under the same nonzero `(i,D)` is refused with
`conflict`; deletion does not establish an unbounded revision history.

`v` selects the locally wrapped line containing that scalar; an anchor at a line
boundary selects the following line, and an anchor at the document end selects
the last line. Each renderer shapes the complete document first, then resolves
this anchor in its own line map. The height still reserves exactly `h` terminal
rows, with padding inside them. This keeps the same source location in view
across operating systems without importing another renderer's line numbers.
`k` identifies a source caret and does not override the explicit viewport; a
caret outside the local visible region is clipped. An editor can choose `v=k`
to bring the caret's source line to the top on each renderer; an earlier anchor
keeps context but cannot guarantee identical visible context on other displays.
Vertical alignment `y`
applies only to an unscrolled (`v=0`) placement. Selection and copying from the
visible viewport do not include its hidden prefix.

### Delete

`a=d:i=N` removes the block named `N` on the active screen. `a=d` alone
removes every block on the active screen. The body is ignored. A removed
block's cells stay as the blank cells they already are.

### Query

`a=q` measures the body in a `w`×`h` rectangle and replies; it places nothing
and moves nothing.

A query proposes the rectangle, the way a layout asks a view for its size.
`w=0` leaves the width open: nothing wraps at an edge, and the reply's `w` is
the ideal width, the widest paragraph. `h=0` leaves the height open: the body
wraps at `w`, every line is shown, and the reply's `h` is the rows those lines
need. Both may be open at once. An open dimension is measured up to the
implementation's advertised open-query bounds, currently 1024 cells wide and
5120 rows tall, and
then resolves to the reply's value, or to 1 for an empty body; `f`, `n`, `c`,
`l` and `R` describe that resolved rectangle. A placement must name both
dimensions, and `a=p` with `w=0` or `h=0` is refused. A query with an empty body measures nothing and replies
anyway, which makes it the feature test: a terminal that answers knows the
protocol. It does not prove that every attached viewer can render blocks.

```
OSC 7404 ; a=r : i=<id> : f=<0|1> : w=<cells> : h=<rows> : n=<chars> : c=<chars> : l=<lines> : m=<cells> : x=<0|1> : v=3 : W=<cell-width> : H=<cell-height> : B=<baseline> : S=<font-size> : E=<epoch> : L=<line-height> : A=<line-baseline> : Q=<0|1> : K=<0|1> : F=<0|1|2> : V=1 : N=<caret-count> : T=<milliseconds> : Z=<limits> : R=<line-ends> ST
```

| key | meaning |
|---|---|
| `i` | The query's `i`. |
| `f` | 1 when the whole body is shown under the query's overflow policy: no line is dropped, and no character is cut or replaced by an ellipsis. |
| `w` | The cells the widest laid-out line and the horizontal padding need, rounded up. Under `o=c`, the widest paragraph; under `o=w` and `o=e`, the widest line after wrapping at the query's `w`. |
| `h` | The rows every line, its paragraphs' space before and after, and the vertical padding need, rounded up, at the query's `w`. |
| `n` | How many characters of the body are shown: all of them when `f=1`. |
| `c` | Characters per line in the style of the body's first character: the width the first line's indents leave in the rectangle, over that face's average advance at that size, rounded down. |
| `l` | Lines that fit in the style of the body's first character: the rectangle's height over that face's line height at that size in the first paragraph's line spacing, rounded down. |
| `m` | The narrowest width in cells that breaks no word: the widest run of text between two break opportunities and the horizontal padding, rounded up. A break opportunity follows a space, a tab or a line break, and falls on either side of a wide character. It does not depend on the query's `w` or `h`. |
| `x` | 1 when every character was measured from a published table (exact); 0 when any character, or the whole answer, is an estimate. |

The version 3 fields `W`, `H`, `B`, `S`, `L` and `A` use 1/64 point.
`E` identifies the measurement authority and typography publication. `Q=1` means a native shaper
measured the complete styled source. For an empty probe, `Q` instead reports
the selected authority's published native capability without invoking its shaper.
`R` lists exclusive Unicode scalar line
end offsets, separated by commas, including explicit line feeds. It is empty
for an empty feature probe. With `g=1`, each entry reserves exactly one row.
`c` remains an average-width estimate and is zero for native shaping; it must
not be used to wrap a particular string. Use `R` and `h` instead.
`K=1` means the selected authority has an active block renderer; it is independent
of native shaping `Q`. A headless pane or one with no eligible rendering attachment
reports `K=0`. Neither field describes every attached viewer. `T` is the maximum
native measurement wait in milliseconds, measured from admission, currently 2000.
`F` advertises the selected renderer's font catalog: 0 unavailable, 1 exposed
families only, or 2 system-font enumeration under platform policy, as specified below.
`V=1` advertises placement clipping with `r`.

A fit result describes one measurement authority at epoch `E`. It does not
promise identical wrapping on every attached display. A program that depends
on a measurement submits its placement with that `E` and `e=1`; a changed
authority or typography refuses the placement before any block, cell or cursor
mutation. It then measures again. Omitting `E` accepts the current
typography. This is an admission guard, not a reservation: subsequent font
changes can relayout an accepted block inside its existing cell rectangle.

A character is a code point of the body's text after the RTF is read, `\par`
and `\line` counting one each. A program sizes a block by asking with a
generous `h`, reading `h` back, and placing with that. It sizes a label from the measured `w`; the average `c` is not a string measurement.

`Z` is a comma-separated list of named decimal resource fields, described under
[Resource bounds](#resource-bounds). Receivers must ignore unknown reply fields
and unknown `Z` names, reject duplicate known fields, and bound packet assembly
before allocating. Request keys remain strict. Limits describe supported work;
`retained-used` is a snapshot, not reserved capacity.

### Font catalog

Programs can enumerate usable family names from the same selected per-pane
renderer that answers measurements. Native clients share one process-wide
installed and bundled family inventory; each renderer adds the class faces it
actually draws. The resulting names may differ from another attached viewer.
The native inventory is built off the main and render threads on first rich-text
use. A query while it is being built returns `s=stale` for a retry. Named runs
use their class fallback until publication; Core then invalidates their layout
and requests one replacement frame.

```
OSC 7404 ; a=f:i=<id>:b=<offset>:n=<count>:E=<epoch>:e=1 ST
OSC 7404 ; a=f:i=<id>:v=3:E=<epoch>:F=<scope>:b=<offset>:n=<returned>:t=<total>:s=<status> ; <hex-name>,<hex-name>,... ST
```

The request has an empty body and accepts only `a`, `i`, `b`, `n`, `E` and `e`.
`b` defaults to zero and `n` to 255. The first page may omit `E`; every nonzero
offset must name the epoch returned by the first page. The reply echoes the
requested offset, reports its actual count and the total number of selectable
families in this snapshot. Names are sorted uniquely by their UTF-8 byte strings.
Advance by `b+n`, using the returned count; a page may stop early at its byte
budget. Every successful nonfinal page makes progress. `b=t` returns a successful
empty final page; `b>t` returns `s=invalid`, never a fabricated empty catalog.

Each name is its exact UTF-8 byte string encoded as lowercase hexadecimal, two
digits per byte. Commas separate names and do not occur inside their encoding.
The decoded names contain no C0, C1 or DEL controls, are nonempty, have no ASCII
space at either edge, and occupy at most 255 bytes. Escape `\`, `{`, `}` and `;`
when inserting a name in the RTF font table. Consumers must bound the packet,
count and decoded bytes before allocating, validate UTF-8, and reject inconsistent
offsets, totals, duplicate names or a different epoch. Programs must use the
returned UTF-8 name verbatim for portable selection. The protocol does not require
case-folding, normalization or alias matching; a renderer may accept additional
spellings, but aliases absent from its catalog are not portable guarantees.

Inventory availability is independent of native shaping. A program selecting
arbitrary catalog families for RTF must require `Q=1` to obtain matching native
measurement and drawing. With `Q=0`, only names represented in the published
metrics table can be resolved exactly; every other name must use the same generic
class fallback for measurement and rendering. A catalog can also serve font
selection outside RTF, such as choosing the terminal's grid font.

`F=1` means only fonts exposed to that renderer. In a browser this includes loaded
page fonts; it does not claim to enumerate the operating system. A browser may
publish `F=2` after the user explicitly grants local-font access through its UI.
The terminal query itself never prompts or grants permission. Native renderers
publish `F=2` when they enumerate usable system families. Operating-system or
browser policy may restrict that inventory; `F=2` does not promise unrestricted
access to every installed font.
Unrepresentable names are omitted from both the page and its total. A ready but
empty exposed catalog is consequently different from unavailable system access.

Every admitted catalog result replies without requiring `e=1`. `s=ok` carries
names and a total. `s=unavailable`, `denied`, `stale` or `invalid` carries no names
and `t=0`; this zero is an error field, not a claim that the system has no fonts.
`e=1` additionally requests the shared grammar, admission and lifecycle errors
described below. `F=0` discovery allows a program to avoid an unsupported query.

Catalog pages and measurements share the same request-ID namespace, pending
credits, admission deadline, cancellation and atomic output queue. Reusing an
outstanding ID across either action retires both meanings with `duplicate`.
Font installation, removal, loading, permission changes and authority changes
advance `E`; a continuation under the previous epoch is refused. Programs discard
the partial inventory and restart at zero. An inventory is not a reservation:
subsequent measurement and placement should also carry that `E`.

### Measurement lifetime and cancellation

Request IDs are scoped to the terminal's query namespace, separately from block
and subscription IDs. A client must have at most one outstanding query per ID,
including the default `i=0`. Reusing a pending ID retires the old query and refuses
the new one; `duplicate` is the single terminal outcome if either request opted
into status. No native reply for either query can later become a successful result.

A native query's deadline starts when Core admits it, including time waiting to
send it to a renderer. Reply arrival at or after the deadline is late. Neither
retransmission nor a change of authority extends the deadline: a font/authority
change yields `stale`, and expiration yields `timeout`, when status was requested.
The application decides whether to submit a new query.

```
OSC 7404 ; a=c:i=<request>:e=1 ST
```

Cancellation requires an explicit ID (including 0); only `a`, `i` and `e` are
accepted. It cancels an admitted query and any unpublished result, releases its
native correlation and partial assembly, and returns `cancelled`. A request
already delivered to a synchronous native shaper may finish, but its result is
discarded. If no query is outstanding it returns
`absent`. These are barriers: after that status, no result for the retired token
can follow. A result already queued before cancellation stays before the barrier.
Cancellation does not remove a block with the same ID.

Admitted queries occupy a bounded credit until their complete result or opted-in
terminal status is queued. Output backpressure retains that outcome; it never
publishes a partial geometry result or silently drops an admitted query. A full
queue refuses new work with `busy`. Immediate refusals to unadmitted or malformed
requests are best effort when the terminal's output queue is already full;
applications must also have their own timeout. `T` bounds computation wait, not
transport or application input delivery. Screen switches and reset cancel pending
work; stale replies cannot enter a later screen or reuse of the same ID.

### Caret geometry

A query with `G=1` sends zero or more geometry packets before its final `a=r`:

```
OSC 7404 ; a=g:i=<request>:v=3:E=<epoch>:s=<span-start>:G=<first,end,row,leading,trailing,...> ST
```

Each packet holds at most 128 spans and starts at the next expected span index.
The final `N` is the complete span count; it is zero when geometry was not
requested or native shaping is unavailable. The entire result is queued
atomically. Each individual OSC is below 32 KiB, including the final line-end
list. Clients assemble at most 4096 spans for an outstanding request, require a
single epoch and contiguous chunk indices, then commit on the final result.

Offsets count Unicode scalars; `[first,end)` is one grapheme, never a UTF-8 byte
range or a UTF-16 range. `row` indexes `R`. Leading and trailing caret positions
are signed-direction edges expressed as nonnegative coordinates in 1/64 point
from the block's left edge; leading can exceed trailing in RTL text. They include
alignment. Trailing whitespace placed left of the block by native shaping keeps
its source span, with out-of-block caret coordinates clipped to zero.
A client maps validated grapheme boundaries to its own string indices
and preserves the visual x when navigating vertically. Measurement and pointer
reports come from the same shaped geometry.

### Typography subscription

```
OSC 7404 ; a=s:i=<subscription>:N=1 ST
OSC 7404 ; a=n:i=<subscription>:v=3:E=<epoch>:W=<cell-width>:H=<cell-height>:B=<baseline>:S=<font-size>:Q=<native>:K=<renders>:F=<catalog>:T=<milliseconds> ST
OSC 7404 ; a=s:i=<subscription>:N=0 ST
```

A nonzero subscription id replaces the terminal's one active subscription. An
initial `a=n` reports the current metrics; subsequent publications report only
changed epochs, even if the dense metrics and terminal dimensions stay unchanged.
Native renderer identity, font fallback changes and authority changes all advance
`E`. Under output pressure, notifications coalesce to the latest epoch and are
retried when output space becomes available.
The application invalidates cached measurements on a changed `E` and requests
fresh layouts asynchronously. No keyboard reader waits for a measurement.
Screen switches and terminal reset cancel the subscription. Full screen erasure
does not. Clients unsubscribe on teardown. Host handover carries the subscription;
replicas never originate application reports. Every report checks `TextMeasureRead`.

### Request status

With `e=1`, a failed request returns:

```
OSC 7404 ; a=e:i=<request>:v=3:s=<status> ST
```

Statuses are `invalid` (malformed keys or body), `unsupported` (unknown key or unsupported RTF control),
`limit` (document or retained-storage bound), `geometry` (placement cannot fit),
`denied` (rendering permission), `busy` (native measurement queue full), or
`stale` (the expected epoch or an in-flight measurement is no longer current),
`timeout` (native deadline elapsed), `duplicate` (pending query ID reused),
`conflict` (live document revision reused for different source), `cancelled`, or
`absent` (no matching query to cancel). Successful place,
delete, and subscription requests return `ok`; a query's final `a=r` is its
success response. Status reports themselves require `TextMeasureRead`, so a
denied read or a passive replica stays silent. Duplicate request ids within one
header or ambiguous error opt-ins stay silent. Clients release a refused query
rather than leaving it pending indefinitely.

The reply is a terminal report: it goes through the reply disposition every
report goes through and is gated by `TextMeasureRead`. A terminal that is a
replica of another answers nothing, because the host it mirrors answers.

## The RTF subset

OSC 7404 defines a **UTF-8-only RTF profile**. The body is exactly one complete,
balanced group opening with `{\rtf1`; this marker identifies the structure,
not a legacy character encoding. Raw text and font-family names use strict,
shortest-form UTF-8. Unicode scalar values, combining marks, variation selectors
and ZWJ sequences are preserved without normalization. Only ASCII space, tab,
CR and LF may follow the root's closing brace.

Every mandatory control must belong to this subset. An unknown control refuses
the whole body with status `unsupported`. An optional destination begins its
group with `\*` followed by a control word, for example
`{\*\producer note}`. Its contents have no document effect, but UTF-8, group
balance, escapes, numeric syntax and resource bounds are still checked.
Encoding controls remain forbidden even inside optional destinations.

| Control words | Meaning |
|---|---|
| `\fonttbl` with `{\fN \fnil\|\froman\|\fswiss\|\fmodern\|\fscript\|\fdecor\|\ftech\|\fbidi name;}` | Font entries: a unique nonnegative number, family class, and complete UTF-8 family name. Each entry ends in `;`. ASCII spaces at the name's edges are trimmed; internal spaces are preserved. |
| `\deffN` | The declared default font of text that names none; it may precede the font table. |
| `\fN` | Select an already declared font entry. An undefined number refuses the body. |
| `\fsN` | Size in half-points, clamped to 2–1000. Default 24 (12 points). |
| `\b`, `\b0`, `\i`, `\i0` | Bold and italic; a missing or nonzero parameter enables the style. |
| `\ul`, `\ul0`, `\ulnone`, `\strike`, `\strike0` | Single underline and strikethrough, with the same toggle rule. |
| `\colortbl` with `\redN\greenN\blueN;` entries | Colours, with components clamped to 0–255. Every entry ends in `;`; an empty entry is the automatic colour. |
| `\cfN` | Foreground colour by table index. 0 or a declared empty entry is automatic; an undefined positive or negative index refuses the body. |
| `\highlightN` | Character background from the colour table, with the same index rules; 0 is transparent. |
| `\plain` | Character formatting back to the defaults: default font, 12 points, regular, automatic colour. |
| `\pard`, `\ql`, `\qc`, `\qr`, `\qj` | Paragraph alignment; `\qj` is left-aligned. `\pard` also resets spacing and indents. |
| `\slN`, `\slmultN` | Line spacing. With `\slmult0`, N in twips, −32767–32767: N > 0 at least N, N < 0 exactly −N. With `\slmult1`, N/240 of the natural line, 1–2400. `\sl0` restores natural spacing in either mode. |
| `\sbN`, `\saN` | Space before and after the paragraph, 0–32767 twips. |
| `\liN`, `\riN`, `\fiN` | Left and right indents, 0–32767 twips, and first-line indent, −32767–32767 twips. A negative `\fi` hangs the first line. |
| `\par`, `\line` | Paragraph and line breaks: U+000A and U+2028 respectively. Copy, capture and accessibility read either as a line feed. |
| `\tab` | Advance to the next multiple of eight cell widths from the content box's left edge; the first tab of a hanging first line stops at the left indent. |
| `\\`, `\{`, `\}` | Literal backslash and braces, in text or a font-family name. |
| `\;` | Literal semicolon within a font-family name; the terminating semicolon remains unescaped. |
| `\~`, `\_`, `\-` | No-break space, non-breaking hyphen, and an ignored optional hyphen. |
| `\emdash`, `\endash`, `\bullet`, `\lquote`, `\rquote`, `\ldblquote`, `\rdblquote` | The Unicode characters they name. |

Control words are case-sensitive ASCII letters and, where specified, a decimal
integer with an optional leading minus sign, in the range −2147483648 through
2147483647. Overflow is an error;
numbers are never saturated during parsing. One ASCII space after a control
word or parameter is a delimiter and does not become text. Controls shown with
`N` require a number; controls without a parameter reject one, except the style
toggles above. Groups restore the formatting in effect when they opened.
Font and colour tables occupy their own groups before body text, and each table
may appear at most once. Duplicate font numbers and unresolved explicit default
fonts refuse the body. With no explicit default, font number 0 is used when present;
otherwise the renderer's default face applies. Colour index 0 is always valid,
including when there is no colour table.

Unescaped CR and LF are formatting whitespace inside the root and produce no
text. An escaped CR or LF is a paragraph break. All other raw C0 controls,
DEL, and U+0080–U+009F are forbidden. Use `\tab`, `\par` and `\line` for document
control characters. UTF-8 sequences cannot cross an RTF escape or group boundary.

There is no byte code page, UTF-16 escape or fallback-text interpretation.
`\ansi`, `\ansicpgN`, `\mac`, `\pc`, `\pca`, `\cpgN`, `\fcharsetN`, `\uN`,
`\ucN` and `\'hh` refuse the whole body with status `unsupported`. A producer
writes `café 😀` directly. General-purpose clipboard RTF must be converted to
this profile before it is sent through OSC 7404.

Malformed or truncated UTF-8, unfinished groups or escapes, trailing content,
invalid parameters and forbidden raw controls refuse the whole body with status
`invalid`. Exceeding an advertised resource allowance also refuses the complete
body. No prefix is placed, and a failed replacement leaves the previous block
and cursor intact.

## The box model

A block covers the `w`×`h` rectangle of cells it was placed in, on the screen
it was placed on. Each covered cell holds a written space in the pen the block
was placed with, marked as covered. The grid under a block is therefore
ordinary cells: the pen's background paints the rectangle, a search finds
nothing there, a renderer that does not draw blocks shows a blank rectangle,
and the wire carries the cells as it carries every cell.

With the default `P=0`, anything that changes a covered cell removes its block: printing into it,
erasing it (ED, EL, ECH, DECERA, DECSERA), inserting or deleting characters
across it (ICH, DCH), inserting or deleting lines inside it (IL, DL), a scroll
that moves some of its rows and not others, and a reflow that separates its
cells. The
check runs once after each parse pass, against the covered mark: a block
whose rectangle no longer holds its own covered cells is removed, and the
covered mark is cleared from its cells that remain, which leaves them as
spaces in the block's pen. The block's text goes with it, as an image's
pixels go when the cells under the image are erased.

A scroll that feeds the scrollback moves every block with its cells into it,
and a block stays selectable and copyable there until its top row leaves the
scrollback. A scroll or line edit that moves rows in place moves a block it
moves whole and keeps inside the moved rows; it removes a block it moves in
part or carries past the edge of the moved rows. A resize moves a block with its rows, and removes it when the
reflow separates its cells. Switching screens keeps each screen's blocks with
that screen. RIS removes every block; DECSTR keeps them; ED 3 removes the
blocks in the scrollback.

### Alternate-screen application pointers

`M=1` replaces an otherwise eligible mouse report inside that block with:

```
OSC 7405 ; a=p:i=<block>:D=<document-revision>:e=<p|r|m>:b=<button>:m=<modifiers>:x=<column>:y=<row>:c=<caret>:k=<character> ST
```

`D` is the revision of the source actually hit on that viewer. The application
must discard a report whose `(i,D)` no longer names its source; an event delayed
across replacement cannot be applied to unrelated text. Source offsets remain
valid across different font choices and wrapping, while coordinates describe
only the reporting viewer. Coordinates are one-based physical terminal cells. `c` is the nearest logical
scalar insertion boundary and `k` is the grapheme under the pointer. They use
the original full document even with `v`. The modifier/button values use SGR
mouse conventions; `e` supplies press, release or motion separately. Motion
is not deduplicated by cell, since a cell may cover several shaped characters.
The ordinary mouse modes still control eligibility; Shift retains terminal
selection. `M=0` retains ordinary cell reports.

### Selection and copy

Selection inside a block is per character, never per cell. The contract:

1. **Pointer to character.** Core maps a pointer position inside a block's
   rectangle to a character index from the block's laid-out advances: the
   line is the last line whose top is at or above the point, and the
   character is the first one on that line whose right edge is past the
   point. A point left of a line's first character is on it; a point right of
   its last is after it. The caret a press opens stands on the nearer edge of
   that character. A caret never stands inside a grapheme: a combining mark,
   a variation selector, an emoji sequence or a wide glyph selects as one.
2. **One layout.** The hit-test, the highlight and the drawn glyphs read the
   same shaped layout for that frontend. The renderer draws each complete
   line through the same typesetter used to measure it.
3. **Highlight shaped as a text view's.** Each line the selection reaches is
   highlighted by one band. A band starts at the press, or at the block's
   leading edge on a line the selection came into from before, and ends at
   the release, or at the block's trailing edge on a line the selection goes
   on past; a line crossed whole fills the block's width. The bands tile the
   block: each runs from its line's top to the next line's, the first from
   the block's shown top and the last to its bottom where the selection goes
   on past them, so the block's highlight meets the fill of the grid rows
   around it with no gap. A line selected only between the press and the
   release keeps one band per visual interval, which bidirectional text can
   split. Covered cells are never highlighted as cells. Background edges are rounded to device pixels, including adjacent shared
   edges, with fill antialiasing disabled. Glyph positions retain fractional
   advances. This applies to terminal selection and RTF `\highlight` in an
   application's alternate screen.
4. **Words.** A double click selects the word around the character under the
   pointer, by the word rules a double click uses everywhere else, applied to
   the block's text and bounded by the block.
5. **Reading order.** In the grid's reading order a block is read at its
   top-left cell, whole: the rows above it, then the cells left of it on its
   first row, then its text, then the cells right of it on its first row,
   then the cells outside it on the rows below. A drag that leaves a block
   continues into the surrounding grid in that order, and one that enters a
   block takes its characters up to the pointer.
6. **Copy.** A copy yields the selected characters in reading order as plain
   text. `\par` and `\line` copy as a line feed and `\tab` as a tab; a line
   the layout wrapped copies with no break. Styles are not copied.
   The copyable text runs through the block's last visible line; an
   ellipsis is drawn, not copied.

The caret a selection holds inside a block is the block's top-left cell and a
character index into the block's text, which is the sub-cell caret the
selection model already speaks for proportional runs. Nothing else in the
selection model gains a coordinate.

## Layout

Core owns the layout and hit testing. A frontend may install a native shaper
that receives the complete styled source and returns lines and grapheme caret
edges. Apple frontends use CoreText for fallback fonts, contextual shaping,
ligatures, bidirectional order and language-aware line breaks. Unshaped
frontends retain the table-derived fallback described below.

**Units.** Lengths are integers in layout units of 1/4096 point. A face's
advances are in 1/2048 em, and a size is in half-points, so an advance at a
size is the product of the two, exactly. Sums of products are exact, on every
arm and every compiler; nothing in the layout rounds until a frontend maps it
to pixels.

**Lines.** The text breaks into lines at every `\par` and `\line`. Under `o=w`
and `o=e`, a line also breaks after the last space that keeps it within the
width its indents leave, and a word wider than that breaks between graphemes.
Spaces at a wrapped line's end hang past the edge and take no width. Under
`o=c` a line breaks only where the body says, and what passes the right edge
is clipped there.

**Height.** A line's natural height combines the largest ascent with the
largest descent plus nonnegative leading among its characters' faces at their
sizes. Its baseline sits the largest ascent below its top, so faces with
different proportions fit above and below their shared baseline. The
paragraph's line spacing sets the line box from that: at
least `\sl` twips, exactly `-\sl` twips, or the natural height times
`\sl`/240 with `\slmult1`; the baseline keeps the share of the box it has at
natural height. A paragraph's `\sb` sits above its first line inside that
line's box and its `\sa` below its last line inside that box, so the boxes tile
the block with no gap, the first paragraph's and the last's included. With
`g=1` every line keeps one row and the spacing and paragraph spaces do not
apply. Lines stack from the block's top. Only whole lines are shown; the first
line is shown even when it is taller than the block, clipped at the block's
bottom.

**Indents.** Every line of a paragraph starts `\li` twips in from the content
box's left edge, and its first line `\fi` further, which a negative `\fi`
turns into a hanging indent; no line starts left of the box. A line wraps at
the width that leaves less `\ri`, and aligns within it. A paragraph takes its
spacing and indents from its first character, as it takes its alignment.

**Alignment.** A line takes the alignment of its paragraph, which is the one
in force at the paragraph's first character, or `j` when the body names none.
Centred and right-aligned lines are offset by the free width, rounded down to
a layout unit.

**Padding.** `p` insets a content box from the block's rectangle, in points on
each side. The text flows into the content box: lines wrap at its width, are
shown while they fit its height, are clipped and cut for an ellipsis at its
edges, and align within it. Padding that leaves no width puts every grapheme
on a line of its own. The padding shows the covered cells' background, as the
rest of the block does; a block has no background or border of its own. Every
size a fit reply names is the rectangle's, its padding included, so a program
places what it was told, and caret geometry is measured from the rectangle's
top-left. Without a typography table the layout counts in cells and padding
does not apply.

**Vertical placement.** When the lines and their paragraph spaces need less
height than the content box, `y` places them in the slack: `t` leaves it all
below, `c` splits it with the odd layout unit below, and `b` puts it all
above. Content that fills or overflows the box sits at its top whatever `y`
says, so clipping and the ellipsis do not change, and a block scrolled with
`v` is not placed. The text, its selection highlight and a pointer's hit test
move with the placement; the content box and its padding do not.

**Ellipsis.** Under `o=e`, when text remains after the last line that fits,
that line is cut at the last grapheme that leaves room for U+2026 in the style
of the character before it, and the ellipsis is drawn there.

**Fallback storage.** A table-derived layout is one allocation sized at layout: a line array
(first character, end, top, height, baseline, origin) and one array of the
prefix-summed right edge of every laid-out character, relative to its line's
origin. The layout is derived state: Core builds it when the block is placed
and again only when the typography table's epoch moves — a resize of the cell,
a change of font or size, a newly published face.

## The measurement model

The answer is Core's and the metrics are the frontend's. A program observes
the reply and the selection, so Core decides both; only the frontend knows its
fonts, so it publishes the facts Core decides from, as it publishes its
window's size.

A native shaped layout also keeps logical grapheme ranges and their two
visual caret positions. Leading may be to the right of trailing in RTL text.
The same source and line ranges are passed to CoreText for drawing; text is
never rasterized one character at a time on the shaped path.

### The typography facts table

| field | meaning |
|---|---|
| cell width, cell height | The grid's cell, in 1/64 point, as the frontend draws it: its pixel cell over its pixels per point. |
| faces, at most 16 | One entry per face the frontend publishes. |

Each face entry:

| field | meaning |
|---|---|
| family | The name RTF's font table uses for it, or empty for a class face. |
| class | Monospace, sans-serif or serif. |
| bold, italic | The style the entry measures. |
| ascent, descent, line height | In 1/2048 em. |
| advances | U+0020–U+024F, 560 advances in 1/2048 em, one u16 each: 1120 bytes. |
| average | The advance an estimate uses for a narrow character outside the range. |

A table is scale-free: an advance at a size is the em advance times the size.
A face's advances are not exactly linear in its size where it has optical
sizes or hinting; the renderer places glyphs at Core's advances anyway, so the
layout, the hit-test and the pixels agree by construction, and an optical
difference shows as spacing a glyph's own design did not choose, never as
disagreement.

The class-face table has three families in four styles each: the grid's face
for monospace, its proportional face for sans-serif, and the platform's serif:
the system face's serif design on Apple (New York, reached through the system
descriptor, since its family name is hidden from a lookup by name) and
fontconfig's `serif` on Linux. Core's software renderer has no font library
and publishes the faces Core bundles: Noto Sans for sans-serif, drawn from
the outlines its proportional runs use, and GNU Unifont, the grid's face, for
monospace. It publishes no serif face, so a serif entry resolves to the
sans-serif class face, and its italics are the upright faces drawn oblique.

**Class faces share the em.** A run is set at its own point size in whichever
class face it resolves to: every class face takes the primary face's em at
that size and keeps its own ascent, descent, line height and advances. No
class face is scaled to match another's line box, cap height or x-height, so
a face whose letters are drawn smaller within its em looks smaller, and the
platform serif is chosen for proportions that match the sans-serif face's. A
line whose class face's ascent and descent exceed the row it sits in takes
the taller line box the "Height" rule gives it, and what passes the block's
bottom is clipped there, as any sized text is; it is never shrunk to fit.
The table format also permits named families. Native shaping resolves names
against the renderer's actual catalog and uses the same family for measurement
and drawing. The compact table remains the deterministic fallback for renderers
without native shaping; it does not enumerate all installed fonts.

### Resolving a font entry to a face

With native shaping, an RTF entry requests the exact family identifier returned
by `a=f`. Bold and italic select an available face within that family. The native
engine may synthesize a missing style; otherwise it keeps an available face of
that family. It must not switch to an unrelated family solely to satisfy a style.
A family unavailable on another viewer falls
back to the entry's generic class. Native font matching can differ across
operating systems, so portable applications use the exact catalog spelling and
verify geometry under the same epoch.

Without native shaping, an RTF font entry resolves to the published face whose family matches its
name (ASCII case-insensitively) in the requested bold and italic; failing
that, to the class face of its family class (`\fmodern` monospace, `\froman`
serif, every other class sans-serif) in that style; failing that, to the
sans-serif class face in that style; failing that, to the regular sans-serif
class face, then to the first published face. The same resolution serves the query, the layout and the
renderer when using this table fallback, so a face the frontend never published
is drawn in the face it was measured in.

### What the answer promises

With `Q=1`, the query uses the frontend's native shaping service at the actual
font size. The full Unicode source participates in shaping, including text
outside the legacy dense advance table. Without a native shaper, `Q=0` and
the table fallback promises the following:

A character in U+0020–U+024F measured in a published face is exact: the
layout places it where the renderer draws it. A character beyond the range is
an estimate: a narrow character takes the face's average advance, a wide one
(UAX #11 W and F) one em, and a combining mark none. The renderer still draws
it at Core's position, so its glyph may be wider or narrower than the room
the layout gave it. `x=0` says an estimate is in the answer.

With no table published at all — a headless terminal with no renderer —
Core answers in cells: each character takes its cell width, every line takes
one row, and the reply says `x=0`. This is the sizing protocol's answer
without a measurer.

### Local and remote panes

A local pane's frontend publishes its table to the pane's Core directly, once
when its first frame is cut and again whenever its cell, font or size changes;
macOS and iOS derive it in the shared surface factory, once.

A remote pane is answered by its host, which runs the program. Typography is
published per pane binding, never as connection-global state. Each publication
contains cell and face metrics, `rendersBlocks`, native shaping support, and a
nonzero renderer revision. The revision changes when the native renderer or its
font environment changes, including fallback font loading with unchanged dense
advances. A new renderer must not reuse a retired revision on that attachment.

Ubiquitty keeps the current measurement authority while it is eligible. Eligibility
requires a live human attachment, Drive permission, a current live pane view, and
an active renderer. Observers, agent connections, historical views and withdrawn
renderers cannot become authorities. When the owner becomes ineligible, the host
chooses the eligible attachment with the lowest connection ID; with none it uses
headless fallback. A later typography publication from another viewer does not
steal ownership. Authority transitions advance `E` even when metrics compare equal.
Independent terminal multiplexers may choose another stable owner policy, but must
invalidate `E` on every owner transition and must not mix replies from owners.

The authoritative cell rectangle and styled source are replicated. Each viewer
shapes, paints, hits, selects and copies with its own renderer and local typography.
It never installs another viewer's line map as its local layout. The selected
measurement result promises fit only in that authority's environment; other
viewers clip or wrap locally inside the same rectangle. Applications use source
anchors and revision-tagged input rather than assuming identical visual rows.
A typography change never changes shared placement dimensions implicitly.

## Native measurement across a connection

The host forwards a nonempty query only to the selected authority for that pane.
Equal advance tables do not identify equal shaping engines. `RichTextMeasure`
(55) carries the pane binding, request token, expected renderer revision, shape
request and parsed document. `RichTextMeasured` (76) returns that revision and
bounded geometry. The client refuses a request for its previous renderer; the
host validates the sender, pane, token, current authority, epoch, renderer
revision, deadline, complete source ranges and geometry before constructing OSC
bytes itself. No client-supplied terminal bytes are forwarded to the program.

The same typed exchange carries a font catalog mode: a request contains an
offset, count and expected renderer revision; a response contains status, scope,
offset, total and bounded UTF-8 family identifiers. The host also checks that the
scope matches that pane's publication and constructs the hex-encoded OSC reply
itself. Catalog responses have their own smaller assembly bound, derived from
their page byte and count limits; a catalog request cannot admit a geometry-sized
assembly. Handover preserves catalog capability and renderer identity. Native
renderers borrow their process-wide inventory and add their own class faces.
Pending queries and completed outcomes waiting for output space block handover.
The host can transfer after their outcome has entered the carried PTY output, or
refuse handover when its quiescence deadline expires; it cannot discard the query
to manufacture a transferable state.

The largest page has 255 family slots and at most 16212 name bytes. Hex encoding
doubles those bytes; the maximum 88-byte OSC header and 254 separators still keep
the whole packet below 32768 bytes. The host accepts at most 16742 serialized
bytes for a catalog response: 20 fixed bytes, at most two length bytes per family,
and the bounded name bytes. These transport bounds are independent of the
retained-document quota and the size of a renderer's native inventory.

At most 32 measurements and catalog queries occupy a terminal or connection queue. Native assemblies
are ordered, at most 512 KiB per request, and share the original admission deadline.
Expired, cancelled, replaced and stale replies release their assembly and cannot
be adopted, including replies received before the next host service pass.
An empty probe uses published capabilities without a round trip. Block geometry
requested for capture uses the same authority and validation; expiry can be retried
by a later explicit capture, never by extending an application's query.
No UI-thread dispatch is allowed inside a shaper callback: its caller holds the
terminal model lock.

## Colour and SGR

A block's text takes the pen it was placed with. The automatic colour is the
pen's foreground, which follows the theme as any cell's does; `\cfN` overrides
it with the table's colour, as a direct colour that the theme does not move.
The block's background is the covered cells' background: the pen's. Bold and
italic in the body are the block's own; the pen's SGR bold and italic do not
apply inside a block, and neither do its underline and strikethrough, which
the body says itself. A selection highlight is drawn in the tone of a
selected cell beside the block: where the selection is filled behind the
text, the selection colour washed over the block's background by the rule a
selected cell's background takes, opaque, under the glyphs; where it is a
tint, the selection's tint over them. Glyphs keep their colours.

## Rendering

Core hands each visible block to the frontend through the frame capture: its
rectangle in pixels, a content key that moves only when the block, its layout,
the palette or the scale changes, and its draw list — runs of glyphs with a
face, a size, a colour, decorations and each glyph's position in layout units.
Decorations come as stretches: the longest run of touching graphemes on one
line that share a style. A frontend strokes one underline and one
strikethrough across each stretch, never one per grapheme, so a rule is
continuous across letter boundaries that fall inside a device pixel and
breaks only where the style or the line does. Core places a stretch's rows
as the cell builder places grid text's rules: the weight and drop from the
run face's underline weight, descent and leading at the run's size, the
strike at half its x-height, the frame's underline thickness multiplier and
offset applied, each weight a whole number of device rows and each top a
whole device pixel. The frame's rule settings ride on the block's quad and
move its key. The macOS raster strokes the rows Core places, under the
glyphs, and sets every grapheme on a whole-pixel baseline as the grid sets
its own. With skip-ink on, Core's ink test cuts an underline as the grid's
shader cuts a cell's: each pixel loses the strongest glyph coverage within
the grid's reach of it, looking no higher than the rule's top row, so a
descender opens a gap and a letter sitting on the baseline leaves the rule
whole. On Linux, Pango draws its own run-level decorations, which meet the
continuity rule.
Raster backends rasterise the block once per key into a retained layer and
composite it as they composite an image, over the covered cells; a frame whose
blocks did not change does no block work. The layer is rasterised at the
drawable's device pixels and composited at exactly one texel per device
pixel, never resampled: a change of backing scale or of the table's epoch
moves the key and rasterises it again. The layer is premultiplied, with a
transparent ground, so the selection highlights — separate rectangles drawn
beneath it — show through between and around the glyphs of every run,
coloured or not.

- **Swift and Metal (macOS and iOS).** CoreText draws complete shaped lines
  using the same faces and ranges used for measurement, into a
  linear half-float raster with premultiplied alpha, drawn over the blitted
  grid every frame and redrawn only when the content key moves. The table is
  measured from the same faces, in the one derivation both shells share
  (`RichTextFaces`). On the bitmap tier the blocks are still CoreText's.
- **Portable (Core's software renderer).** With no shaper, Core lays blocks
  out from the table of the faces it bundles (`BundledRichText`), and the
  renderer sets every grapheme in its style's face at the position Core gave
  it: Noto Sans from its outlines, GNU Unifont as texel squares at the run's
  size, which at the grid's size are the grid's own glyphs. Each block is a
  retained premultiplied raster redrawn only when its content key moves,
  composited over the grid and its images and under the cursor and the
  overlays, inside its visible rectangles and outside any chrome; its
  backgrounds and selection share rounded device-pixel edges. A snapshot
  driver publishes the table before its feed; the browser's CPU raster draws
  no blocks, since its DOM layer does.
- **Linux (GTK, CPU and Vulkan).** Pango lays out complete paragraphs through
  HarfBuzz, FriBidi and fontconfig fallback. Its line boundaries and grapheme
  carets feed the same Core layout, OSC replies, pointer reports and agent
  projection. PangoCairo paints retained transparent paragraph rasters onto the
  existing RGBA presentation path for both grid renderers. Native chrome clips
  the rich layer; character backgrounds, selection and the caret share rounded
  device-pixel boundaries. GTK zoom and DPI changes republish font metrics.
- **Web (browser DOM over the canvas).** Browser paragraph layout supplies
  line breaks and grapheme carets. A retained DOM layer paints complete styled
  lines at Core's captured positions above the WebGL or CPU canvas, with Core's
  clipping, selection and caret geometry. Each pane owns its shaping adapter;
  font loading and configuration changes invalidate its metrics and advance its
  typography publication. Measurement remains available while a pane is hidden.
  Canvas-only pixel snapshots exclude this DOM layer.
- **Backend fallback.** A backend without block rendering shows the
  covered cells in the placing pen's background. Core still owns selection
  and copy. A backend that draws blocks must use the faces represented
  by its published table and place their glyphs at Core's positions.

## Performance

Layouts and rasters are retained until source, typography or placement changes.
Rows are located by binary search; shaped grapheme spans on that row determine
pointer hits and selections. Backgrounds use shared rounded device-pixel edges.
The original document, rather than raster data or per-frame glyph streams,
crosses Sync. Queries may require one round trip to the font-owning client;
applications should cache results by source, width and typography revision and
keep the previous proportional frame visible while a new measurement arrives.

## Synchronization

| State | Carriage |
|---|---|
| The blocks' placements: screen, serial, id, document revision, rectangle, source anchor, caret, visible rectangles, overflow, alignment, padding, vertical placement | Digested, in the `RichText` state block (Sync wire 25) |
| The blocks' documents: text, runs, font table, colour table | Defined once per attachment by a `DefineRichText` op naming the block's serial |
| A block's layout and drawing | Derived |
| The typography table | Host-only: it arrives from the client in `TypographyFacts` and never travels in a checkpoint |

The covered cells travel as cells. A serial names one placement for the life
of the host terminal — a hard reset continues the count — so an update that
moves or removes blocks carries their placements and no document, and a
checkpoint defines every document it places. The digest covers the
placements; the loopback oracle compares every block, placement and
document, between host and client after every frame.

## Capabilities

Placing and deleting blocks is grid content, like an image: `RichTextRender`,
in the Rendering tier, allowed for every origin. The query is
`TextMeasureRead`, in the Benign tier: replies reveal metrics and, for `a=f`, the
font families exposed by the selected viewer. Both are checked at the
operation, before the code touches anything.

## Resource bounds

The OSC grammar defines numeric notation and fixed field domains; an
implementation advertises its accepted workload in `Z`. Programs use the smaller
of their own bounds and the advertised values, while still enforcing this draft's
fixed wire envelopes. They must handle `limit` without assuming that a successful
probe reserves memory. `Z` reports admission policy; it does not negotiate a new
wire format. The current Ubiquitty profile is:

| `Z` field | Value | Resource bounded |
|---|---:|---|
| `body` | 16384 | RTF input bytes |
| `scalars` | 4096 | Unicode scalars in one parsed document |
| `runs` | 512 | Style runs in one document |
| `fonts`, `colors` | 16 each | Document table entries |
| `depth` | 32 | RTF parser group stack |
| `family` | 255 | UTF-8 bytes per font family name, represented by one length octet; longer names refuse the whole body |
| `catalog-count` | 255 | Families per catalog page, represented by one count octet |
| `catalog-bytes` | 16212 | Decoded name bytes per page, derived from the OSC packet envelope after hex expansion, the largest header and separators |
| `blocks` | 256 | Placements on either screen |
| `rectangles` | 32 | Disjoint presentation clips per block |
| `pending` | 32 | Outstanding measurements and catalog queries, including retained unpublished outcomes |
| `carets` | 128 | Grapheme spans per OSC geometry chunk |
| `width`, `height` | 1024 each | Explicit rectangle dimensions |
| `open-width`, `open-height` | 1024, 5120 | Query search extents for an open dimension |
| `retained-bytes` | at most 1048576 | Host source storage allowance across both screens and history |
| `retained-used` | current usage | Actual retained source capacity in bytes |

These are implementation resource policies, not universal typographic constants.
They remain constrained by the version 0.1 reply contract: at most 4096 assembled
caret spans, at most 128 spans per geometry packet, and each OSC reply sequence
below 32 KiB, including its header, terminator and final line-end list. A larger
advertised workload must still satisfy those bounds for every admitted result;
raising a `Z` value alone cannot authorize larger packets or assemblies. Changing
these fixed envelopes requires a protocol revision. The Sync frame and assembly
bounds described below additionally constrain implementations using that transport.

The source owner charges allocated block slots (including inline colour tables),
scalar, run and font-entry capacities, and clip arrays. Thus many styled short documents and a few
long documents consume the same actual byte allowance without an unrelated
aggregate scalar quota. A host may lower this allowance; handover and a hard
reset (RIS) preserve it, while client replica decoding uses its own bounded envelope. Layouts, raster
caches and pending measurement documents have separate bounded owners; the source
allowance is not a claim about total process memory.

Admission is transactional, including allocation failure. It first stages all
required source storage. If needed, it reclaims only the oldest complete primary
scrollback prefix, extending the prefix through intersecting historical blocks so
no block is cut in half. Active or partly visible content is never evicted to admit
a new block. If enough historical storage cannot be reclaimed, it returns `limit`
with the old blocks, cells and cursor intact. Replacing or deleting content frees
capacity; a smaller update may succeed. Replicas reproduce accepted state rather
than independently evicting it.

The bounds compose: 4096 scalars, 512 runs and the bounded tables keep a decoded
measurement request below the 65535-byte Sync frame body. Complete source and
ellipsis geometry (each at most 4096 lines and spans), plus line endings, fit the
512 KiB assembly envelope. OSC replies chunk geometry into 128 spans and keep each
sequence below 32 KiB. Terminal-row viewports need at most 4096 preceding rows plus
1024 visible rows, explaining the 5120-row internal extent. At the supported
100-point cell bound, these coordinates fit signed 32-bit layout units (4096 per
point). Parser depth, queue credits and cache/source allowances bound different
resources and are checked independently; raising one requires checking its
composed envelopes, not changing a magic number in isolation.

## A worked example

A program shows a heading and a paragraph in a 40×6 block. It asks first:

```
printf '\033]7404;a=q:i=1:w=40:h=6;{\\rtf1{\\fonttbl{\\f0\\fswiss ;}}{\\colortbl;\\red0\\green102\\blue204;}\\f0\\fs36\\b\\cf1 Release notes\\par\\fs24\\b0\\cf0 The build now signs every artefact and verifies the chain before upload.}\033\\'
```

One possible table-based reply begins with the fields below. This is an
abbreviation: the version 3 metadata and line-end list are omitted here, but
remain required in the actual reply.

```
ESC ] 7404 ; a=r:i=1:f=1:w=39:h=5:n=88:c=60:l=4:m=9:x=1:...
```

The body fits (`f=1`) in 39 columns and 5 rows, and no word is wider than 9
columns (`m=9`). A program that does not know the width it wants asks with
`w=0` for the ideal width, or with `h=0` for the rows a fixed width needs.
The program places it:

```
printf '\033]7404;a=p:i=1:w=40:h=5;{\\rtf1{\\fonttbl{\\f0\\fswiss ;}}{\\colortbl;\\red0\\green102\\blue204;}\\f0\\fs36\\b\\cf1 Release notes\\par\\fs24\\b0\\cf0 The build now signs every artefact and verifies the chain before upload.}\033\\'
```

The cursor lands on the row below the block. To update the paragraph, the
program places `i=1` again; to remove it, it sends `printf '\033]7404;a=d:i=1\033\\'`.

The numbers in the reply depend on the viewer's fonts; that is the point of
asking.

## Scope

- Native shaping covers wrap, clip and grapheme-safe ellipsis when a native renderer is available.
- A family unavailable to a renderer falls back to the entry's generic class.
- `\qj` is left-aligned; tabs stop every eight cells, and only the hanging first line has a custom stop; underline is single.
- `\highlight` is supported. Controls for `\cb`, superscript, subscript, tables,
  lists, links and pictures are unsupported and refuse the body unless enclosed
  in an explicit optional destination beginning with `\*`.
- Styles are not copied.
- A double-width or double-height row refuses a block.

## Agent access

`pane.capture` with `source=screen` returns the same visible text projection as
accessibility: grid labels and visible rich-text ranges, in screen-row order.
The `richText` member carries the original UTF-8 text of each intersecting block,
style runs (including named and generic font families), and the visible source ranges. Hidden text remains in the block's
source for shaping context; only `visible` ranges contribute to the flat `text`.
All source offsets count Unicode scalar values, with half-open `[first,end)`
ranges. They never count glyphs, UTF-8 bytes, or UTF-16 code units.

Blocks are ordered by top row then left column. `id` is the application's OSC
identifier; `documentRevision` carries `D`, `sourceAnchor` carries `v`, and
`serial` identifies this placement. The pane, serial and
`typographyEpoch` together identify the geometry. Replacement or font changes
invalidate that identity. Cell rectangles are zero-based. Native `lines` and
`graphemes` use points relative to the unscrolled block content, with a top-left
origin. The capture's `viewportTop` is the resolved local row offset, distinct from the
OSC source anchor `v`. Subtract `viewportTop * cellHeight` from a line's top to locate it inside
the visible rectangle; add the block's cell origin to locate it in the pane.
`visibleRects` carries the optional placement clips. Each visible source range
also names its screen `column`, so grid labels inside clip holes retain their
correct position in the flat text projection.
Line baselines are relative to the line top. Leading and trailing carets may
run right-to-left. A grapheme range can contain several source scalars.

On a capture request, the font-owning attachment supplies the host's geometry over Sync. Requests
are bounded to 32 pending measurements; replies are assembled from ordered
chunks, at most 512 KiB per request, and expire with the request's two-second
deadline. Block replacement and typography changes prevent stale adoption.
Until native geometry arrives, `richText.layoutPending` is true and `complete`
is false. `AgentSession` retries within the original capture deadline. A host
without an attached shaper cannot invent native geometry. Table fallback
layouts report `shaped=false` and omit native lines and grapheme edges.

## Architecture choices

The terminal retains styled source and placement, not a stream of individually
positioned characters. Core owns lifetime, bounds, permission checks, the wire,
and validated layout geometry. The font-owning frontend owns paragraph shaping
and rasterization: CoreText on Apple platforms and Pango on Linux. Measurement,
hit testing and paint must use the same complete source, font configuration and
line ranges. A scalar width table is an explicitly approximate fallback, never
an exact paragraph typesetter.

This division follows CoreText's typesetter/line model and Pango's paragraph
layout model. HarfBuzz alone supplies glyph shaping, but does not provide the
paragraph breaking, bidi resolution or font fallback that this protocol needs.
The shaping callback never dispatches synchronously to a UI thread while Core
holds its model lock. Native font state is owned per renderer and synchronized
at that boundary.

Placement clips (`r`) and a scrolled content origin (`v`) are presentation
operations. They must not split the source into fragments before shaping.
Application menus can cover part of a paragraph without changing its wrapping
or exposing the monospace backing grid. Colour changes invalidate painted
content; unchanged geometry and unrelated typing do not require measurement.
OSC 66 remains the bounded grid-run protocol; OSC 7404 is the retained paragraph
protocol, and OSC 7405 reports logical character positions without assuming
that a glyph occupies a terminal cell.

Native ellipsis (`o=e`) shapes a grapheme-safe source prefix with U+2026 attached.
Core uses a bounded search of native layouts rather than summing character widths;
the complete styled prefix participates in bidi resolution and contextual shaping.
Paint, hit testing and selection use that presentation; the synthetic ellipsis is
not copied. Query extents, `R`, and optional `G` describe the complete unelided
source, while `n` reports the visible source prefix. Remote measurement carries
both geometries, so the host and font-owning frontend use the same cut.

References: [CoreText typesetters](https://developer.apple.com/documentation/coretext/cttypesetter),
[Pango layout](https://docs.gtk.org/Pango/class.Layout.html),
and [HarfBuzz's scope](https://harfbuzz.github.io/what-harfbuzz-doesnt-do.html).

`ubq read --json` exposes this snapshot under `screen`. MCP callers can request
`terminal_screen` with `format="structured"` for the capture JSON, or omit the
format for visible text. Both use the existing region-read permission. Capturing
text grants no input or selection authority. Screenshots remain useful for
visual verification.
