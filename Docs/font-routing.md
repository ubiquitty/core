<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Ubiquitty Font Routing Contract

This document defines the boundary for Ubiquitty's font-selection,
fallback, diagnostics, and glyph-routing pipeline, including CJK, language, and
script preferences.

## 1. Layer ownership

Font routing is human-visible presentation policy. It is not part of the
program-observable terminal model. The terminal Core decides bytes, characters,
grapheme clusters, cell attributes, and terminal cell widths; font routing
decides which local platform capability renders those already-decided cells.

The ownership boundary is:

```text
Source/Core/
  Program-observable terminal model only. No platform font APIs. No route policy.

Source/Core/Rendering/Font/
  Portable, host-testable route policy: requests, routes, script classes,
  preference normalization, route ordering, cache rules, diagnostics schemas,
  and performance counters. No platform font APIs or GPU objects.

Source/UbiquittyRenderKit/
  Apple CoreText-backed font inventory and raster capability. Owns CTFont,
  CTFontCreateForString, language fallback, exact family validation, and
  CoreText shaping.

Source/Platform/Linux/
  fontconfig / FreeType / HarfBuzz-backed font inventory and raster capability.
  Owns local font discovery, glyph probing, shaping, and platform font handles.

Platform rendering backends
  Renderer-specific rasterization, atlas allocation, texture upload, shaders,
  and draw submission. A renderer consumes GlyphRoute decisions; it does not own
  language preference parsing, Unified Han policy, generic fallback policy, or
  route diagnostics policy.
```

A renderer is therefore a route consumer. It may choose between mono, wide,
color, ligature, double-size, procedural box, and shaped-run draw paths based on
route flags. It must not own Unified Han detection, CJK language
token parsing, fallback-cache ordering, or native-cascade policy.

## 2. Preferences intent and local capability

Configuration carries user intent. Local clients resolve that intent against
installed fonts and platform text APIs. A config file may name a preferred
family, language, script, or range; it never carries platform pointers,
rasterized pixels, glyph cache slots, or GPU state.

The configuration surface is:

```toml
[font]
family = "JetBrains Mono"
fallback = ""                 # existing symbol fallback alias; empty = bundled symbols
symbol_fallback = ""          # canonical symbol fallback, empty = bundled symbols
match_fallback_size = true    # match fallback text to the primary font's visible size
emoji_fallback = "auto"       # auto | system | custom family
cjk_fallback = "auto"         # Han shorthand/default: auto | system | zh-Hans | zh-Hant | ja | ko | custom family
wide_glyph_boost = true
wide_glyph_scale = 1.2

[[font.language_fallbacks]]
script = "han"                # script token from the shared ScriptClass set
language = "ja"               # BCP-47-style token or "*" for script default
mode = "custom"               # auto | system | regional | custom
family = "Hiragino Sans"      # required when mode == custom

[[font.range_map]]
range = "U+4E00-U+9FFF"
mode = "custom"
family = "Hiragino Sans"
match_size = false           # optional: true = match primary, false = original size; omitted = global default
```

`font.cjk_fallback` is the shorthand/default for Unified Han. It remains useful
for common CJK regional control, while `font.language_fallbacks` handles the
full script/language preference table and `font.range_map` handles explicit
Unicode range overrides. Range maps are more specific than script/language
fallbacks and must be diagnosable.

## 3. Route request and route result

The portable route policy receives facts that the terminal model and renderer
already know:

```text
FontRequest
  codepoint or cluster codepoints
  script class
  presentation request: text | emoji | default
  font style: regular | bold | italic | boldItalic | variable weight
  width class: narrow | wide | zero | control | cluster
  current font preference generation
```

The policy returns a platform-neutral route:

```text
GlyphRoute
  source: Primary | RangeMap | ExplicitHan | AutoHanCascade | SymbolFallback |
          EmojiCascade | ScriptFallback | GenericCache | NativeCascade | Missing
  platform font handle
  glyph id or shaped-run token
  route flags: mono | wide | color | ligature | shapedRun | procedural | missing
  diagnostics fields when collection is enabled
```

The platform font handle is opaque outside the platform adapter. Portable
Rendering code may store and compare handles by value/generation, but it cannot
dereference platform font objects.

## 4. Selection order

The route policy applies this exact order:

1. Select the styled primary font and try direct glyph lookup.
2. Apply an explicit `font.range_map` override when one matches the codepoint or
   cluster lead, then the built-in ranges (`Font::defaultRangeMaps`): the media
   controls U+23E9–23FA, Geometric Shapes U+25A0–25FF and the large shapes
   U+2B12–2B2F route to the bundled Monaspace Neon, which draws all of them, so
   a primary face's gaps among `● ○ ◯ ⏺ ⬤ ◐` come from one face instead of
   whichever system face the later stages reach. A built-in range yields to
   emoji presentation, and a code point its family lacks goes on to the next
   stage.
3. For Unified Han, use the Han path only:
   1. exact `font.language_fallbacks` script/language preference,
   2. script-default Han preference,
   3. `font.cjk_fallback` shorthand/default,
   4. `HanFallbackCache`,
   5. native Han cascade through the platform inventory,
   6. missing glyph.
4. Try terminal symbol fallback for Powerline, Nerd Font, private-use, box-like,
   and technical symbol ranges that require stable terminal UI glyphs.
5. Route emoji and color clusters through the color/cluster path.
6. Try script/language-specific fallback preferences and `ScriptFallbackCache`.
7. Try `GenericFallbackCache`.
8. Ask native cascade for non-Han fallback.
9. Return a cacheable missing route when all sources fail or resolve to a
   LastResort-like font.

Unified Han never consults or populates the generic fallback cache. A kana
fallback font that also contains Han glyphs cannot answer Han, and a Chinese Han
fallback cannot poison Japanese text through the generic path.

## 5. Width and geometry invariants

Text fallback faces match the primary face's x-height, or cap height
and line height when the preceding measurement is unavailable. Measurements
are normalized to em units and the resulting scale is bounded to 0.5–2.
The primary baseline and terminal cell width stay authoritative. Color emoji
keep their separate cell-fitting path. Private-use icons retain their original
cell-based sizing unless a range explicitly selects size matching.

Each `[[font.range_map]]` naming a family may override its size with `match_size = true` or
`false`; omitting it inherits the global setting. The font settings expose
Use Default, Match Primary Font, and Original Size for each configured range.
`font.match_fallback_size = false` keeps the original point size; changing it invalidates font routes and glyph sheets.

Font routing never changes terminal width. Core decoding, grapheme clustering,
and width classification happen before route selection. If the request says a
cell is wide, the renderer uses a wide route and a true two-cell atlas slot. If
the request says a cell is narrow, font metrics do not promote it to wide.

Fullwidth glyphs render into a wide atlas slot whose physical width is two cell
boxes plus horizontal overhang allowance, and whose height is one cell box plus
vertical overhang allowance. They are not drawn into a one-cell mono atlas and
stretched to two cells.

A proportional primary face changes where a cell stands, never a cell's
classification. Core cuts its cell to the advance of `0`, and lays each row
out as proportional runs: the letters of one pen stand at the face's own
advances, and a cell-bound glyph — box drawing, a block, an emoji, a wide
glyph from a fallback face — keeps its cells at the pitch inside the row. The
route is unchanged: a letter is drawn from the primary face, and fallback
faces keep their placement. Docs/proportional-text.md states the rule.

Complex scripts and emoji clusters may require shaped-run routes. The policy
marks those routes explicitly so a renderer does not silently rasterize Arabic,
Indic, emoji ZWJ sequences, flags, or variation-selector clusters as unrelated
single codepoints.

## 6. Cache taxonomy

The route policy owns cache rules; platform adapters own platform font objects.
Every cache key carries enough generation data to avoid stale routes after font
or preference changes.

```text
HanFallbackCache
  Unified Han only. Keyed by primary font identity, size/metrics generation,
  style, Han preference, language token, and platform font catalog generation.

ScriptFallbackCache
  Non-Han script/language fallback results. Keyed by script, language, style,
  primary font identity, preference generation, and platform catalog generation.

GenericFallbackCache
  Small MRU cache for non-Han, non-symbol, non-emoji platform fallback fonts.

EmojiFallbackCache
  Color emoji and emoji-cluster route decisions.

SymbolFallbackCache
  Configured or bundled terminal symbol fallback decisions.

RangeMapCache
  Resolved explicit range-map routes.

NegativeRouteCache
  Missing glyphs, rejected LastResort-like fonts, and failed custom families.
```

Caches clear or miss when primary font, size, style generation, symbol fallback,
emoji fallback, CJK fallback, language fallback table, range map, or platform
font catalog generation changes.

## 7. Diagnostics contract

Every visible glyph or shaped run must be diagnosable when diagnostics are
enabled. Diagnostics are off in the hot path unless explicitly requested; cheap
aggregate counters may remain active under existing performance gates.

Required diagnostic fields:

```text
codepoint(s)
script class
language preference
terminal width class
route source
selected font family and PostScript name
glyph id(s) or shaped-run token
cache state: hit | miss | stored | negative
atlas family: mono | wide | color | ligature | double | procedural | shaped
shaped-run flag
color route flag
missing or LastResort rejection reason
cascade-call count for the route or frame
```

The diagnostic surface must support a headless developer command or harness and
a Settings preview. A user choosing a custom font for Japanese Han, Arabic, an
emoji family, or a symbol range must be able to see whether that choice is used,
rejected, or bypassed by a higher-priority range map.

## 8. Performance contract

After warm-up, route lookups are served from the §6 caches; the §7 counters
report cascade calls and cache hits.

## 9. Implementation guardrails

- No platform font API appears in terminal `Source/Core`.
- No platform font API appears in portable Rendering headers.
- Renderers do not parse language tokens, own Unified Han detection, or own
  fallback-cache ordering.
- Platform adapters validate custom family names because platform constructors
  may silently substitute a different family.
- LastResort-like fonts are treated as missing unless diagnostics explicitly ask
  to inspect them.
- Route diagnostics are stable enough for tests; exact glyph IDs are asserted
  only with pinned fake inventories or controlled test fonts.
