<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Arabic joining and bidirectional display

Terminal cells remain in logical input order. Joining and right-to-left display
are row projections; they must not rewrite the terminal's stored text or change
the column addresses observed by programs.

This document specifies the portable joining interface and the constraints on
font-based shaping. The relevant code is
`Source/Core/Unicode/Joining.{h,cpp}` and
`Source/Core/Screen/Addressing.cpp`.

## Text model

UTF-8 decoding, cell width, and grapheme segmentation belong to portable Core.
A cell holds a cluster base and combining data, with overflow in the terminal's
cluster pool. Wide cells and their continuations represent one display unit.

The grid and scrollback retain logical cells. A display-row fetch can produce
a visual projection with shaped base code points while preserving each source
cell's pen, combining data, links, and other attributes. Rendering, selection,
copy, and cursor code must distinguish logical coordinates from that projection.

Do not add platform font types to `Cell` or change its fixed representation
to cache a renderer-specific result.

## Joining contract

`joiningType` classifies code points as non-joining, transparent,
right-joining, left-joining, dual-joining, or join-causing. Transparent marks
do not interrupt the neighbor search. ZWJ causes a join; ZWNJ breaks it.

`resolveForm` chooses isolated, initial, medial, or final form from the
nearest non-transparent neighbors. A run boundary acts as a non-joining
neighbor. `presentationForm` maps that result to a Unicode presentation
form and preserves the original code point when no form exists.

`shapeRun` processes logical order and permits input/output aliasing.
A lam followed by a ligating alef produces a ligature carrier and a collapsed
cell. The display projection marks the resulting pair as a wide lead and
continuation so the glyph spans two terminal columns. The logical store still
contains both source letters.

Presentation-form mapping is a code-point transform. It is not a font GSUB
substitution pass and does not supply GPOS mark positioning. Rendering these
forms depends on the resolved font's coverage.

## Visual-row contract

`shapeVisualRow(logical, outCodePoints, outLogical, count)` first shapes in
logical order, then reverses contiguous strong-RTL runs. Its output is indexed
by visual column:

- `outCodePoints[visual]` is the base code point to paint.
- `outLogical[visual]` identifies the logical source cell whose attributes
  accompany that code point.

Output arrays must not alias the logical input. A row needing no joining or
RTL transform returns identity outputs and reports no transformation.

The algorithm absorbs interior transparent marks, collapsed ligature cells,
and SPACE, TAB, NBSP, or NNBSP only when another strong-RTL cell follows.
Leading and trailing neutral cells remain outside the reversed run.
Arabic-Indic digit ranges are not strong-RTL anchors.

This is a row-local ordering rule, not full UAX #9. It does not define
embedding levels, isolates, bracket pairing, paragraph-direction resolution,
or the complete weak-character rules. Digits between RTL words can therefore
separate runs. A caller must not treat its permutation as proof of general
Unicode bidirectional conformance.

## Display caching

`Screen::getDisplayRow` carries source attributes through the permutation,
then applies the lam-alef wide-cell representation to the projected cells.

Live-row shaping can be cached by row identity and content generation.
A scrollback ring index can be reused for different content after eviction;
it is not a durable cache key. Reflow and geometry changes require deriving
the projection from the resulting logical row, not reusing an old permutation.

The row's shaping hint is a conservative gate. A false positive may cost an
extra pass; a false negative must not omit shaping. Plain ASCII must not pay
for a full joining pass.

## Font-based directional shaping

A directional run-shaping API must preserve the original logical code points,
source-cell mapping, font identities, style boundaries, direction, and full
required context. The font engine owns glyph substitution, cursive metrics,
ligatures, and mark positioning. Portable Core owns terminal columns and the
logical-to-visual relationship.

Shape the logical run before placing glyphs. Do not reverse raster pixels or
apply bidirectional reordering twice. A ligature spanning several source cells
must preserve their allocated column extent, paint ownership, and cursor and
selection behavior.

Glyph identity is scoped to an actual font face. Fallback, variation settings,
and a mixed-style run cannot be represented by copying numeric glyph IDs
between unrelated fonts. Ambiguous cluster boundaries require a conservative
larger shaping group, not a guessed split.

## Required checks

`Tests/Core/Unicode/test_joining.cpp` covers the joining and visual-row APIs.
A change to this contract needs cases for:

- Isolated, initial, medial, and final forms.
- Transparent marks, ZWJ, ZWNJ, and run boundaries.
- Lam-alef forms and preservation of two logical columns.
- Mixed Latin, Arabic, Hebrew, whitespace, and digit runs.
- Logical source attributes after visual permutation.
- Row edits, scrollback eviction, resize, and reflow.
- Selection, copy, and cursor mapping independent of paint order.

Font-shaping changes also require pixel comparisons across actual fallback
faces, combining marks, style boundaries, and contextual ligatures. A correct
row permutation alone does not establish correct glyph shaping.
