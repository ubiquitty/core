<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Procedural glyph coverage (`Rendering/BoxDrawing`)

Ubiquitty synthesizes a defined set of grid-aligned glyphs procedurally rather than
relying on the font, so they meet with no gap cell-to-cell at any size and never
depend on a Nerd Font being installed. Geometry lives in
`Source/Core/Rendering/BoxDrawing.{h,cpp}`; the Metal frontend's box pass
(`FrameEncoder.swift`) draws the primitives emitted by `BoxDrawCommands.cpp`.

## What is procedural

- **Block elements & shades** — quadrants, eighths, and the shade glyphs
  U+2591/2/3. Shades render as a **Stipple** primitive whose look is the
  `[appearance] shade_blocks` setting (`Rendering/ShadeStyle.h`):
  - `pattern` (the default) lights solid foreground on a quarter, half or three
    quarters of the device pixels, `((x + 2y + phase) & 3) < lit`, leaving the
    background untouched on the rest. The phase anchors the field to the grid's
    top-left pixel, so a run of shades is one continuous field at every cell
    size — a pattern restarted per cell would seam wherever the cell is not a
    multiple of the period (4 across, 2 down). Core writes the phase into the
    quad's `plA.x`; Metal, Vulkan, WebGL and the software renderer evaluate
    the same expression.
  - `fill` composites the foreground at 1/4, 1/2 or 3/4 over the cell: Core
    emits an ordinary rect at that alpha, so no frontend dithers it.

  The Linux Vulkan arm draws the same box-fill stream through
  `shaders/boxfill.frag`, so its pattern is the grid-anchored field too.
- **Symbols for Legacy Computing** — the geometric subsets of U+1FB00–1FBFF: sextants,
  smooth-mosaic wedges, triangular quarter/three-quarter blocks, vertical and
  horizontal eighths, medium-shade halves/inverse/checker, triangular medium
  shades, box-drawing light diagonals, and centred/fractional blocks. Pictographic
  gaps in this range remain font-rendered.
- **Octants** — U+1CD00–1CDE5 (Symbols for Legacy Computing Supplement): the 230
  2×4 sub-cell fill patterns, drawn as up to seven snapped sub-rects. The 26
  patterns that already exist as other glyphs (empty, full, the four halves, and
  the 2×2 quadrant-family patterns) are skipped, so the codePoints map to fill
  patterns in ascending order — the same remap the sextants use, one row taller.
- **Diagonals and corner triangles** — U+2571–2573 and U+25E2–25E5 reach
  the snapped cell corners. The corresponding legacy half-diagonal and chevron
  families follow the same rule.
- **Powerline separators** — U+E0B0–E0BF use cell-aligned analytic geometry.
  Other supported Powerline PUA glyphs have procedural fallback geometry but
  remain font-first. Filled separators reach the cell edges so prompt segments
  meet with no gap.

## Primitives

`BoxDrawing` emits a small primitive set the renderer rasterizes: strokes, fills,
**Triangle** (scan-converted per device row, honouring a sub-1 density for the
triangular shades), and **Stipple** (the shade field above). Dispatch routes
through `Rendering::BoxDrawing::covers`, `prefersProcedural`, and `decompose`.
`covers` describes available geometry; `prefersProcedural` identifies the subset
that takes priority over a font glyph.

## Policy: what stays font-first

- **Orthogonal box-drawing U+2500–257F** prefers the font's glyphs, centred on
  the cell and edge-stretched where appropriate. Dashed rules are not stretched,
  so their gaps remain open. A rasterizer that hints draws these from the
  unhinted outline, moved by one family offset (`boxFamilyPixelOffset` in
  `GlyphTiling.h`) that lands the reference `─` and `│` on whole pixels: hinting
  fits each glyph on its own and moves the same stroke differently in a tee
  than in the rule it continues. Diagonals U+2571–2573 are procedural. Missing
  non-dashed font glyphs can use the procedural fallback.
- **Pictographic legacy glyphs** (stick figures, arrows, circle slices, segmented
  digits, folders) stay font-first — they are illustrative, not grid-tiling.

The line between the two is "does it have to tile gaplessly with its neighbours on
the cell grid?" Area fills, diagonal joins, and analytic separators are
procedural; pictographs retain the font's design. Orthogonal rules use the
font's weight with cell-aligned placement.
