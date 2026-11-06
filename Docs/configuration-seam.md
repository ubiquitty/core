<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The configuration seam

A frontend connects configuration to Core the way it connects a PTY: it hands
over bytes. Core parses the TOML document, validates it, owns the live values,
derives what every renderer and every terminal needs, and tells everything that
derived from the old document that a new one is live. A frontend never sets an
individual value and never derives a setting itself.

## The door

`Source/Core/Preferences/Configuration.h`:

```cpp
struct ConfigureResult {
    bool isApplied;                         // false only for Strict with a diagnostic
    std::vector<SkippedKey> diagnostics;    // every line not applied, document order
    std::uint64_t epoch;                    // the epoch after the call
};
ConfigureResult configure(const std::string& document, ParseMode mode);
ConfigureResult configureFromFile(const std::string& path, ParseMode mode);
const std::string& liveDocument();
std::uint64_t configurationEpoch();
class ConfigurationSubscription { explicit ConfigurationSubscription(std::function<void(const Values&)>); };
```

`Source/Core/Preferences/EffectiveValue.h`, beside the by-key writer:

```cpp
std::optional<std::string> configureKey(Key key, const Value& value);
```

- **The bytes are the primitive.** `configure` takes the document. An empty
  document is the default table, not an error. `configureFromFile` is a
  convenience: a file that does not exist is the empty document.
- **Wholesale replacement.** A key the new document omits reads as its default,
  whatever the previous document said.
- **A refused line keeps its key's value.** A key on a line the parse refuses
  keeps the value the previous document gave it, not its default. A line is
  refused when its value is the wrong kind or outside the key's vocabulary,
  when its value does not parse, when it opens an array that never closes, and
  when it sits under a section header that cannot be read. At launch there is
  no previous document, so such a key reads as its default. A bad line in a
  saved file costs that line's change and moves nothing else; the settings the
  file states correctly take effect.
- **Unknown keys are warnings.** A key no table names is listed and ignored, and
  the rest of the document applies: a file written for a newer build loads on
  an older one. A misspelled name takes the same path, so the setting it meant
  reads as its default. A number outside its key's range is clamped to the
  range, not refused.
- **A bad line costs only itself.** The keys under a header that cannot be read
  land in no section, rather than in the section before it. A multi-line array
  that reaches a header or an assignment before its closing bracket ends there,
  and the lines after it are read as written.
- **Validation** is the parser's, in two modes. `Permissive` applies what it
  could read and makes the document the live document, refused lines included,
  so a settings surface splicing `liveDocument()` edits the text the person
  saved. `Strict` refuses a document with any diagnostic and leaves the live
  configuration, the live document and the epoch untouched; a fixture or
  snapshot case wants a typo to fail rather than draw with the key missing. A
  live reload stays `Permissive`: a strict refusal would leave the live
  document behind the file, and the next settings write would save that older
  text over the person's edit.
- **Diagnostics are typed.** `SkippedKey` carries the 1-based line, the section,
  the key and one of three reasons: `UnknownKey`, `WrongValueKind` (with the
  kind the key takes) and `MalformedLine` (a line no grammar rule accepts; the
  record quotes the line, bounded to `malformedLineQuoteLimit` bytes). A known
  key written with something that is not a value is `WrongValueKind`. A value
  spanning lines is named at the line it starts on.
- **Diagnostics are shown in the window.** `liveDiagnosticSummary()` is the
  first diagnostic's sentence and how many follow. macOS shows it, after
  `config.toml`, in every window's title row; Linux in a bar under the panes.
  Each clears it when a document applies whole, and each keeps a line the
  person dismissed quiet until the document says something else.
- **Settings surfaces edit by key.** `configureKey` rewrites the live document
  through the by-key writer and comes back through `configure`, so the text and
  the values cannot disagree. It returns the new document for the frontend to
  persist. A value of the wrong kind changes nothing.
- **Round trip.** The rewrite keeps every comment, every unrelated line and the
  order of the document; it rewrites an existing assignment in place, keeping
  the indent and a trailing comment, uncomments a commented-out assignment of
  the key, appends a missing key at the end of its section and a missing
  section at the end of the document. It does not keep: the replaced value's
  own spelling (`12` written back as a double is `12.0`; a literal string
  becomes a basic string), CRLF line endings (the whole document comes back
  LF), a missing final newline (one is added), or a second assignment of the
  same key (only the first is rewritten). A key under a dotted header such as
  `[profile.font]` is not matched; the write lands in the plain section.
- **One notification.** Every replacement of the live values — `configure`,
  `configureKey`, and the internal value replacement — moves
  `configurationEpoch()` and calls each live `ConfigurationSubscription` once,
  after the replacement, on the main thread. A subscription is RAII; a callback
  may destroy any subscription, its own included, and a destroyed one is never
  called. A consumer that pulls rather than subscribes compares the epoch.
- Main-thread only, like `values()`.

## The derivations

### Renderers: `RenderConfiguration`

`Source/Core/Rendering/RenderConfiguration.h` is the struct;
`Source/Core/Preferences/RenderConfigurationFold.h` is the one function that
fills it:

```cpp
Rendering::RenderConfiguration Preferences::Effective::renderConfiguration(const Values&);
Rendering::RenderConfiguration Preferences::Effective::liveRenderConfiguration();
```

The fold lives in Preferences rather than in Rendering because Rendering is a
model-layer module (L1) and Preferences is a behaviour-layer module (L2); the
model layer cannot depend on the parser. The struct is Rendering's; filling it
is Preferences' work.

- **Trivially copyable.** It crosses to render threads and to Swift by value,
  and `==` decides whether anything moved. `hasSameTypography` separates a
  change that cuts new glyph sheets from one that only restyles the frame. Text
  is held in fixed buffers (`FamilyText` 128 bytes, `FontSpecificationText`
  256, `PathText` 1024, `TokenText` 36); a longer value is not stored and reads
  as empty, which is its default, rather than being truncated into a different
  family or path. At most 16 rules of each fallback kind are kept.
- **Points, not pixels.** One configuration serves surfaces on every display a
  window spans, and the backing scale belongs to the surface and the frame, not
  to the configuration. A renderer multiplies once, by the scale it is drawing
  at.
- **Default-constructed is not the default table.** The member initialisers are
  structural placeholders, not Preferences defaults. Rendering cannot name
  those defaults; the default table is `renderConfiguration(Values {})`.

| Field | Source |
|---|---|
| `fontFamily` | `[font] family` (empty: the house family list, `Defaults::houseFontFamilies`) |
| `fontSizePoints` | `[font] size` |
| `proportionalFontFamily` | `[font] proportional_family` |
| `proportionalFontSizePoints` | `[font] proportional_size` |
| `symbolFallbackFamily` | `[font] symbol_fallback`, else `[font] fallback`, else the bundled symbol family |
| `shouldMatchFallbackSize` | `[font] match_fallback_size` (default on); range rules carry their own optional `match_size` override |
| `cjkFallbackFamily` | `[font] cjk_fallback` |
| `emojiFallbackFamily` | `[font] emoji_fallback` |
| `languageFallbacks`, `languageFallbackCount` | `[[font.language_fallbacks]]` |
| `rangeMaps`, `rangeMapCount` | `[[font.range_map]]` |
| `fontFeatures` | `[font] features` |
| `fontVariations` | `[font] variations` |
| `hasLigatures` | `[font] ligatures` |
| `fontWeight` | `[font] weight` |
| `boldFontWeight` | `[font] bold_weight` |
| `fontWidthPercent` | `[font] width` |
| `lineSpacingPoints` | `[font] line_spacing` |
| `characterSpacingPoints` | `[font] character_spacing` |
| `baselineOffsetPoints` | `[font] baseline_offset` |
| `shouldBoostWideGlyphs` | `[font] wide_glyph_boost` |
| `wideGlyphScale` | `[font] wide_glyph_scale` |
| `shouldDrawBrailleGrid` | `[font] braille_grid` |
| `brailleStyle` | `[font] braille_style` |
| `shadeStyle` | `[appearance] shade_blocks` |
| `shouldBrightenBold` | `[text] brighten_bold` |
| `selectionStyle` | `[selection] style` |
| `underlineThicknessScale` | `[underline] thickness` |
| `underlineOffsetPoints` | `[underline] offset` |
| `shouldUnderlineSkipInk` | `[underline] skip_ink` |
| `shouldUnderlineFollowInk` | `[underline] follows_ink` |
| `underlineSkipInkPaddingPoints` | `[underline] skip_ink_pad` |
| `shouldUnderlineLinks` | `[underline] links` |
| `linkUnderlineStyle` | `[underline] link_style` |
| `shouldStyleLinksPerKind` | `[underline] link_style_per_kind` |
| `shouldUseSystemBlueLinkColor` | `[underline] link_color_blue` |
| `paddingLeftPoints` … `paddingBottomPoints` | `[appearance] padding_left/right/top/bottom`, else `padding`, else the built-in edges |
| `titlelessPaddingTopPoints` | `[appearance] titleless_padding_top` |
| `usesDisplayP3` | `[appearance] color_space` |
| `colorblindMode` | `[appearance] colorblind_mode` |
| `backgroundOpacity` | `[appearance] background_opacity` (theme appearance, then 1.0, when unset) |
| `defaultBackgroundOpacity` | `[appearance] default_background_opacity` |
| `blurRadiusPoints` | `[appearance] blur_radius`, else the theme's appearance, then 20 |
| `shouldEnhanceTextContrast` | `[appearance] text_contrast`, else the theme's appearance |
| `shouldEnhanceTextOnBlur` | `[appearance] text_contrast_on_blur`, else the theme's appearance |
| `usesBackgroundVibrancy` | `[appearance] background_vibrancy`, else the theme's appearance |
| `backgroundImagePath` | `[appearance] background_image` |
| `backgroundImageOpacity` | `[appearance] background_image_opacity` |
| `backgroundImageMode` | `[appearance] background_image_mode` |
| `cursorAlpha` | `[cursor] alpha`, as 0-1 |
| `shouldCursorInvertText` | `[cursor] invert_text` |
| `shouldFadeBlink` | `[appearance] blink_animation` |
| `isBellVisual` | `[bell] visual` |
| `bellFlashPeakAlpha` | `[bell] visual_intensity` |
| `usesCustomTekColors` | `[tek] color_mode` |
| `tekBeamRGB` | `[tek] beam_color` |
| `tekBackgroundRGB` | `[tek] background_color` |
| `drawsTekWritingSpot` | `[tek] bright_pen` |
| `drawsTekGlowVectors` | `[tek] glow_vectors` |
| `maxFramesPerSecond` | `[render] max_fps` |
| `isPresentSynchronized` | `[render] vsync` |

The cursor's shape, blink, blink rate, linger and glide are not here: they are
terminal state — what DECSCUSR and DECRQSS answer against — and are seeded onto
each terminal by `Preferences::Effective::applyCursorPreferences`. The window
chrome keys (title bar, folder icon, corners, snap resize, UI font scale) are
the frontend's own seam and are not renderer input.

`Preferences::Effective::reachOf(Key)` places every `Key` as `Renderer`,
`Terminal` or `Frontend`. The switch is exhaustive with no default, so a new
key does not compile until it is placed; a key placed in `Renderer` that moves
no field fails `test_render_configuration`.

### Sessions: no second struct

Per-terminal derivation has one owner:
`Runtime::seedTerminalConfiguration` at launch and
`Runtime::reseedTerminalConfiguration` on a live change, both in
`Source/Core/Runtime/TerminalLaunchSeed.h`. They read `Values` through
`Effective::`. The launch seed applies the scrollback limit; live reseeding
changes only the documented live subset. A remote replica keeps the encoding
chosen by its host.

## The default rule

An unset key behaves exactly as its default value, on every frontend, whether
the document came from a file, from a page, or was empty.
`test_preferences_configure` states every key at its default and requires every
key-addressed read, the renderer fold and the seeded terminal to be identical to
an empty document's.

The following defaults have explicit semantics:

- **Cursor blink and mode 12.** The person's blink preference, default on,
  governs the default-shape cursor. Mode 12 remains recorded and reported by
  DECRQM but does not override that preference. A program's explicit DECSCUSR
  style takes precedence while it is active.
- **`[font] width = 0`** selects the face's own width. Zero is a sentinel,
  not a value to clamp to the ordinary width range.
- **`[window] initial_columns = 0` and `initial_rows = 0`** mean no pinned
  grid dimension. An explicit zero and an omitted key have the same effect.

The parser's sentinel-default table preserves those stated sentinel values.
Every consumer must use the effective-value accessor rather than duplicate a
default in a frontend.

Some defaults depend on another key and therefore have no single literal
spelling: per-edge padding falls back to `padding`; `titleless_padding_top`
defaults to `padding_bottom`;
`blur_radius` falls back to the selected theme's appearance; the ground and
contrast keys fall back to the selected theme's appearance; `[title] pane_*`
inherit the resolved `tab_*`. Each is a Core rule applied identically to an
empty document and to a file.

## Live apply

`UbiquittyKit::Session` subscribes on creation. On every replacement it derives
the `RenderConfiguration` on the main thread, posts to its Dispatcher, and on
the session thread re-seeds its `Terminal` (`reseedTerminalConfiguration`;
a remote session keeps its encoding, since the host decodes) and calls the
Observer hook `onConfiguration(const RenderConfiguration&)`, followed by
`onFrame`. A reload is one `configure` call, observed by every Kit session.
A Kit session is created and destroyed on the main thread, where the
configuration lives; on iOS that is also its session thread.

The Swift renderer reads the struct through one adapter,
`Source/UbiquittyKit/Swift/SurfaceRenderer+Configuration.swift`:
`SurfaceTypography(configuration:)`, `SurfaceStyle(configuration:systemTransparency:)`,
`SurfaceTekSettings(configuration:)` and
`TerminalSurface.makeRenderer(device:configuration:systemTransparency:)`, plus
`TerminalSurface.preferredFontFamily()`, which asks CoreText for the first
family of Core's preference list that it finds, on either shell. The ground
goes through Core's `backgroundEffects`; the host reports only the system's
Reduce Transparency.

On macOS the window owner holds the one `ConfigurationSubscription`
(`TerminalWindowController+Configuration.swift`) and re-applies
`liveRenderConfiguration()` to every open window; a settings write configures
its key (`configureOwnKey`, or `configureOwnDocument` over a splice of
`liveDocument()` for a block) and broadcasts nothing.
`Scripts/check-settings-live-apply.py` asserts both.

## Frontend integration

Each frontend and tool finds its configuration bytes and hands them to Core;
none mutates a setting's backing storage.

| Consumer | Load | Live apply | Renderer reads |
|---|---|---|---|
| macOS | `configureFromFile`, the watcher calls the same; the scripting hook's output through `configureScriptOutput` | the window owner's subscription | `RenderConfiguration`, a profile folded in by `renderConfiguration(values, profile)` |
| iOS | `configureFromStoredDocument` | `TerminalSessionModel`'s subscription adopts every pane | `TerminalSurface.adopt(configuration:systemTransparency:)` |
| Snapshot tools | `configure(document, Strict)`; a case with a diagnostic fails | — | `RenderStyle.h` appliers over `RenderConfiguration` |
| Linux | `configureFromStoredDocument`; `--config` through `configureFromFile` | the app's subscription | `Raster` and `VulkanRenderer` hold the struct |
| Web | `configureFromStoredDocument` over the page's blob | the terminal re-derives when `configurationEpoch()` moves | `applyConfiguredFrameStyle` |

`replaceValues`, `mutableValues` and the file loader are private to
`Preferences.cpp`; `Tests/Harness.h` pins the defaults with
`configure("", Permissive)`. The scrollback depth is part of
`seedTerminalConfiguration`. A `[[font.language_fallbacks]]` or
`[[font.range_map]]` entry the router cannot use is a `WrongValueKind`
diagnostic at its header line. `Effective::LaunchSettings` carries only window
facts (grid, title row, resize snap, startup host). The key table's coverage
test writes each descriptor's default into a document and reads it back.

## Consumer requirements

A frontend must derive defaults and live settings through this same seam,
including font size, line spacing, color space, bell presentation, background
images, and frame limits. A page control or native settings widget must not
quietly introduce a second value that disagrees with the document.

Settings views must refresh from the authoritative configured values.
Renderer and terminal updates must respect their owning threads. Replacing
configuration must not change a hosted program's identity, restart a session,
or replace a host-selected encoding on a remote replica.

Validation includes `test_preferences_configure`, `test_render_configuration`,
the per-key descriptor coverage tests, and
`Scripts/check-settings-live-apply.py`. A renderer's coverage is established
by its own adapter and rendering tests, not by the existence of a field in
`RenderConfiguration`.
