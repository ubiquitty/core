<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Underlines

How Ubiquitty draws the five SGR 4 underline shapes (single, double, curly,
dotted, dashed), a link's underline, and where glyph ink cuts them; and the
frames and circles drawn round text (SGR 51, SGR 52, U+20DD). Core decides
every rule's geometry and every cut, once, for every renderer; Metal, Vulkan,
WebGL and the CPU `SoftwareRenderer` draw the pieces Core emits and sample no
glyph for them. A change to the look is made and tested in one place, and a
rule cannot read one way on the Mac and another in a browser.

## Geometry

- **Weight.** The font's own underscore: every engine rasterises `_` and Core
  measures it (`measuredUnderscoreWeight`, `RuleGeometry.h`), so a face with a
  hairline underscore gets a hairline rule and a heavy face a heavy one. The Mac
  rasterises it with font smoothing off, as iOS and FreeType do: smoothing's
  stem darkening adds about 0.6 of a pixel of coverage that the glyph pass,
  mixing as CoreGraphics mixes text, never shows as weight, so a rule measured
  with it read a third heavier than the underscore beside it. The weight is
  drawn in whole device pixels, at least one (`ruleWeight`), as AppKit draws
  an underline: a partial last row reads as blur, not weight. `[underline]
  thickness` scales it before the rounding: 1.0 is the font's own weight.
- **Offset.** The rule's top sits the larger of 0.85 of its weight, in whole
  rows and never under one (`underlineGapInWeights`), and the face's declared
  underline position (`post.underlinePosition`, floored to its row) below the
  baseline (`underlineTopNoHigherThanDeclared`), so a face that asks for more
  room gets it and one that crowds its letters does not. At 12 points on a 2x
  display JetBrains Mono, declaring 3.72 pixels, gets three clear rows under
  its stems, as TextEdit draws it; SF Mono (2.34) and Monaspace Neon (0.84)
  keep the gap's two. `[underline] offset` moves the rule, in points, down for
  positive, and moves the declared position with it; 0 leaves it automatic.
- **Metrics.** `[underline] metrics` picks where the single rule's top and
  weight come from, and every shape below follows them. `ubiquitty`, the
  default, is the rule above: the underscore's weight and the larger gap.
  `font` draws the face's own declared underline as AppKit does: the rectangle
  from the declared position below the baseline (`post.underlinePosition`) down
  by the declared thickness (`post.underlineThickness`), snapped outward to
  whole device rows (`declaredUnderlineRows`), so SF Mono
  at 12 points on 2x sits on rows 2 and 3 below the baseline and JetBrains
  Mono on rows 3 and 4, two rows thick, as in TextEdit. `[underline]
  thickness` scales the declared thickness and `[underline] offset` moves the
  declared position. A face whose engine reports no declared position (the
  web arm, a bitmap face) keeps the `ubiquitty` rule. The skip-ink clearance
  left automatic stays the underscore's weight in either.
- **Double.** Two rules at the single's weight, 0.85 of a weight apart and
  never less than a point (`doubledRuleGap`, `pairedRuleTop`).
- **Dotted.** Round dots 1.15 times the single's weight, grid-fitted, two dot
  widths apart, laid from where the run starts (`dottedRuleWeightScale`).
- **Dashed.** Dashes 1.1 times the single's weight, three on to two off, on the
  single's rows (`dashedRuleWeightScale`, `dashedRuleOnShare`).
- **Curly.** Circular arcs, alternately cresting and dipping, meeting on the
  centre line with matching tangents, seated on the single's top row
  (`curlArcs`, `curlCentreY`). `[underline] curly` picks the proportions:
  `compact` (two thirds of a cell, two thirds of a weight's swing), `medium`
  (a cell, three quarters; the default) or `wide` (a cell, a weight). The
  wavelength never falls under 8 device pixels.
- **Links.** A link's cells count as underlined neighbours of one another, so
  its rule runs on across them and a cut beside one is judged as the line's,
  not as a line's end.

## Skip-ink

`[underline] skip_ink` cuts a rule where glyph ink crosses it. The gap kept
beside the ink is `[underline] skip_ink_pad` points; unset, it is the
underline's own weight, so it grows with the font and the line; 0 is none.

Each shape is cut the way that reads best for it:

- **Single and double: the stroke-following cut** (`markSkipInkCuts`,
  `SkipInk.cpp`). The strokes that touch the rule — ink within a pixel of its
  rows — are followed away from it, and each row of the rule is cleared a gap's
  distance from them, measured as a distance, so a slant or a curl leaning over
  the line is cleared as far as a straight stem and the cut's ends follow the
  stroke. A glyph whose ink stays clear of the line — `o`, `e`, a flat-bottomed
  letter on the baseline — never bites it. What the cut leaves is judged as
  pieces of line: a piece shorter than two weights between two close
  descenders reads as a stray dot and goes, and a fragment that is not the
  line's full height anywhere goes too.
- **Dashed: cut square.** Every row takes the cut of all of them, so a dash
  beside a slanting comma's tail is never left a row tall, and a dash the cut
  ends in keeps what is left of it only where that is two weights or more.
- **Dotted: whole dots, fitted across the run** (`DottedRuleRun`). A dot is whole
  or absent. A cell sees only its own glyph and its neighbours', so a dotted
  run is held until it ends and its dots are laid across the whole of it: each
  stretch between two cuts holds every whole dot that fits, centred on the
  rhythm's pixel phase, so the gap beside the ink is even on both sides and a
  stretch never loses a dot to where the rhythm happens to fall. The stretch
  before the first cut keeps the run's own rhythm, so a run starts with a dot
  at its edge as an uncut run does.
- **Curly: a soft mask** (`markSkipInkMask`). A wave cut hard falls apart into
  ticks. Instead each pixel of the curl keeps one less the coverage of the
  glyphs beside it, their ink grown by a disc of the gap in every direction and
  read smoothly at the pixel's centre: the wave keeps its rhythm, fades where
  ink is near, and the letter stays clean. Every glyph cuts it, so an
  underscore resting on the line is cut around rather than merged into a heavy
  bar. A stretch of the curl's stroke the mask leaves less than one and a half
  stroke widths of, between two cut places or a cut place and the line's own
  end, reads as a speck and goes, judged along where the stroke is drawn.

## Frames and circles

SGR 51 and 52 and U+20DD COMBINING ENCLOSING CIRCLE put a line round text
rather than under it. Their stroke is two thirds of the underline's weight in
whole pixels, never under one (`enclosureWeight`, `Enclosure.h`): a line round
text reads heavier than one under it.

- **Framed (SGR 51)** is one box round each run of framed cells. Its top and
  bottom run on across the run, and its sides stand only at the run's ends, a
  stroke outside it in the cells beside, because a monospaced letter reaches
  its cell's edge and a side on that edge runs into it. A run against the
  grid's edge keeps that side inside.
- **Encircled (SGR 52) and U+20DD** are one circle round each character, two
  cells across whatever the character, centred on its cells and its row, and
  the row's height across rather than cropped on a row shorter than two cells
  (`enclosingCircle`). A circle in the first or last column is cut at the
  grid's edge, which nothing is drawn past. One space between two enclosed letters keeps their
  circles apart. The character inside is drawn at seven eighths of its size,
  its cell box shrunk about the box's centre so an `e` and a `k` keep one
  baseline, by the path an OSC 66 fraction takes (`enclosedTextSize`). A
  bitmap face such as Unifont keeps its own size: its glyphs are whole pixels,
  and a smaller size would resample them. A character carrying other marks
  keeps them: it is drawn from its own cluster's raster mapped into the
  smaller box.
- **U+20DD is Core's to draw, not the font's.** The face is handed the cluster
  without the mark, so a letter whose only mark it was is the plain letter's
  glyph. Drawn by the font, a face lacking the mark sent the whole cluster to
  a fallback face, letter and all; the mark sat on each letter's own ink, so a
  `c`'s circle was lower than an `A`'s; and the fallback's circle, wider and
  deeper than a cell, was cropped by the cluster's slot.

The circle is an `OverlayInstance` on the box-fill stream drawn as Braille's
outline dot is (`enclosingCircleShapeKind`): a circle in its ink box, hollow
at its stroke, which every renderer's box-fill pass draws the same way.

## Pieces

Core emits each rule as `DecorationInstance` pieces: the rectangle drawn, the
whole rule as the pattern's frame (so a dot or a crest lands in the same place
whichever piece draws it), which ends a cut made (drawn in part, so the gap
beside a stroke is the gap exactly at any phase), and, for a soft cut, the
share of the rule's opacity the piece keeps. A curly rule's soft cut is merged
before it is emitted: shares are kept in steps of a 64th, a pixel the stroke
does not pass through joins the stretch beside it, and rows keeping the same
are one piece.

## Cost

Cutting in Core costs the CPU what a shader cost the GPU before, so the build
avoids the work wherever the answer is already known:

- A plain rule, or any rule when skip-ink is off, is a handful of stores; the
  cut, the mask and the dotted run are out of line.
- Each tap carries the rows its glyph's slot holds ink in, learned once a
  build, so a cell whose glyphs' ink cannot reach its rule is drawn whole
  without building a query: most of a page of text.
- Per build, a glyph is cut once under each rule shape (`SkipInkSourceMemo`), a
  rule beside the same glyphs is cut once (`SkipInkCutMemo`), a glyph's grown
  coverage for a soft cut is worked out once (`SkipInkGrownMemo`), and a cell
  beside the same glyphs as one already drawn copies its pieces. Every memo is
  forgotten when the next build begins, in constant time: a slot one frame
  read may hold another glyph in the next.

`bench_render_plan --scene underlined|dotted|curly --skip-ink` measures the
worst case, a screen of underlined text with a descender beside nearly every
cell; `--scene` alone measures it with skip-ink off.

## Where

| What | Where |
|---|---|
| Rule geometry, the underscore measure, the curl's arcs | `Source/Core/Rendering/RuleGeometry.h` |
| Shape proportions, emission, memos of pieces | `Source/Core/Rendering/CellInstanceBuilder.{h,cpp}` |
| The cut, the soft mask, the memos | `Source/Core/Rendering/SkipInk.{h,cpp}` |
| Dots fitted across a run | `Source/Core/Rendering/DottedRuleRun.{h,cpp}` |
| Frames' and circles' weight, the circle, the enclosed size | `Source/Core/Rendering/Enclosure.h` |
| The CPU renderer's shapes | `Source/Core/Rendering/SoftwareRenderer.cpp` |
| Rich text's rules | `Source/Core/RichText/Drawing.{h,cpp}` |
| Tests | `Tests/Core/Rendering/test_skip_ink.cpp`, `test_cell_instances.cpp`, `test_rule_geometry.cpp` |
