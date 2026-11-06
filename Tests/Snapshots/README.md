<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Terminal regression harness

This is the single executable catalog for terminal images and protocol replies.
No single tier can be both byte-stable across platforms and a test of a
platform's own renderer, so there are two:

- `portable` links the platform-independent Core and `SoftwareRenderer`. It
  always rasterizes the ordinary configured font family `GNU Unifont` from the
  embedded bitmap data. No native font or GPU API participates, so showcase PNGs in
  `golden/portable/showcase/` must be byte-stable on macOS and Linux. Focused
  cases use the shared `golden/unifont/` pixel reference. Rich text blocks are
  set in the faces Core bundles, Noto Sans and GNU Unifont
  ([rich text](../../Docs/rich-text.md#rendering)): the showcase's rich text
  cards hold them, and the focused `rich_text_*` cases, whose unifont golden
  is the CoreText render, are exempt on this arm as on linux and web.
- Native catalogs (`macos`, and `linux` when its snapshot adapter is enabled)
  exercise the platform's actual renderer and font stack. macOS cases use the
  bundled JetBrains Mono and Symbols Nerd Font Mono faces plus CoreText/Metal;
  they cover color emoji, fractional advances, fallback inventories, and the
  font-backed/procedural box-drawing boundary. Their baselines are platform
  specific under `golden/native/coretext/` or `golden/native/freetype/`.

The macOS catalog's native baselines were captured on macOS 27 (27.0, build
26A428). The app's floor is macOS 26
(`app=` in `Scripts/macos-deployment-target.txt`); systems below that floor
cannot launch it. Renderer comparability is a separate requirement: CoreText
coverage can differ between OS generations. A baseline OS-generation change
requires running the full native catalog and inspecting every changed PNG.

`cases.json` is one list of focused cases, and every case renders on every
arm. A case says what it shows — `columns`, `rows`, a `feed` or `feed_file`,
its `settings` (the sections of a configuration that are not a font), and a
`doc` — and names a face through `native`, a profile under `configs/native/`.
The arms and the goldens follow from that, in two tiers:

- **unifont** — every arm, portable, macos, swift, ios, linux and web, draws
  the case in GNU Unifont, which is baked into Core, and all six are held to
  one golden, `golden/unifont/<name>.png`, pixel for pixel — every channel of
  every pixel, not the comparator's tolerance; the PNG bytes may differ by
  encoder. A bitmap face with no hinter and no antialiaser leaves nothing for
  a tolerance to absorb: a difference is one arm's layout or colour, and
  nothing else can hide in it.
- **native** — a case with a profile also renders in it, on the arms that
  share a text stack, held per stack: `golden/native/coretext/<name>.png` for
  macos, swift and ios, `golden/native/freetype/<name>.png` for linux. A case
  without a profile has no native tier and is not red for lacking it.

The macos and swift arms are one renderer, not two. Both run the app
binary, and its snapshot command (`Core/App/SnapshotRequest.h`) treats
the two `--snapshot-backend` tokens alike:
each draws through the Swift surface renderer and its Metal encode. They
differ only in what the runner asserts about the frame: the macos run
requires the `builder=core`, `compositor=metal` and `encoder=swift` render-path
markers, the swift run requires only the encoder marker. A picture both pass is
one renderer's picture checked twice, so their agreement is not independent
evidence, and the retained-frame regressions each runs are the same frames.
The independent arms are portable, macos (or swift), ios, linux and web.

No case reads a checked-in configuration file: the runner assembles the TOML
an arm is launched with from the tier's `[font]` block and the case's own
`settings`. So `configs/` holds the faces of `configs/native/`, each named by
a case through `native`, and the handful of configurations a tool names by
path — `showcase.toml` for the showcase probes, `macos_specimen.toml` for
`coretext_parity.py`, `macos_monaspace_healing.toml` for
`Scripts/ligature-determinism.py`, and `portable_solarized_light.toml` and
`macos_default.toml` for `Scripts/render-png.py`. A file there that nothing
names renders nowhere, and `Scripts/check-snapshot-faces.py` refuses it.

The catalog pins `appearance.text_contrast = false` to compare the application's
requested colors. The `perceptual_contrast_dark` and `perceptual_contrast_light`
cases enable it explicitly and compare the adjusted ink across renderers,
including indexed colors, truecolor, dim text, and explicit cell backgrounds.
Configuration and live UI tests cover the default-on setting and its toggle.

Every `Tools/showcase` entry also produces a portable snapshot, and entries
with an `expect` contract additionally check the terminal's outbound reply.
There is no manual-only category. Discovery fails on missing sequences,
duplicate names, missing PNGs, or orphaned PNGs.

An arm that disagrees with its tier's golden is red. The cases an arm has
never matched are listed in `known-failing-<arm>.txt`, one identifier per
line with the reason and the date it was recorded; a run prints those as
`KNOWN`, never `PASS`, the gate goes red on any new failure or on a listed
case that starts passing, and deleting a line is how a case is fixed. The
list only shrinks.

A comparison a tier cannot mean is not a failure owed. Colour emoji come from
a fallback face on every arm, and the bitmap reference has none, so the
unifont tier asks those cases for something no correct arm can give. Such a
case is listed in `exempt-<arm>.txt`, in the same reason-and-date grammar and
with no pixel ceiling: the arm does not run it on that tier at all, a run
prints it as `EXEMPT`, the verdict never turns on it, and an update naming it
is refused, since its golden is another arm's to mint. A case may not be both
exempt and known-failing on one arm; the catalogue check refuses that. An
exemption is a decision recorded, not a debt: it ends when the reference
gains what the arm draws, and the line is deleted then.

## Run

```sh
# Portable reference renderer (macOS or Linux)
./Scripts/build-core-snapshot.sh
python3 Tests/Snapshots/run.py --backend portable

# Native macOS renderer
./Scripts/build-macos.sh --no-helpers
python3 Tests/Snapshots/run.py --backend macos

# The Linux arm, drawn headlessly through its CPU adapter by the binary that
# also runs the Vulkan/lavapipe smokes; name a case, or none for the catalog
UBQ_BUILD_JOBS=4 ./Scripts/build-linux-gtk.sh Release
python3 Tests/Snapshots/run.py --backend linux linux_native_text   # or --backend linux alone

# Both rendering tiers
./Scripts/run-tests.sh snapshots

# Discovery and baseline integrity only
python3 Tests/Snapshots/run.py --check-catalog
```

The runner uses only the Python standard library. A mismatch writes the actual
image and an amplified diff image (when dimensions agree) under
`$TMPDIR/ubiquitty-regression-fail/`.

## Baseline policy

A golden is the reviewed visible contract, not an alternative implementation
and not an instruction to preserve a known bug. Production code, focused unit
tests, case inputs, configuration, and the resulting golden land in the same
change. Never make a showcase or feed less representative merely to recover an
old image.

A replacement names what it replaces, because a wider update rewrites images
nobody looked at:

- use an exact case name and one backend with `--update`;
- use `--update-missing` only while adding a new catalog entry;
- never use an unfiltered `--update` in a normal change;
- review the actual PNG and amplified diff before accepting a replacement;
- do not accept unrelated baseline changes together.

## Approved goldens

A golden that has been checked by eye is listed in `approved.tsv` with the
sha256 of its bytes, by `Tools/approve-golden.py <golden> --note "..."`. From
then on it is not re-minted: `run.py` refuses `--update`, `--update-missing`
and `--remint-passing` on it, and a render that differs from it is reported as
a failure of the renderer. `Scripts/check-approved-goldens.py` fails a push
when a listed golden's bytes differ from the hash, when the file is gone, or
when a line's hash is rewritten in place against `origin/main`.

To replace an approved picture: delete its line in one change and land that,
then approve the new picture in another. Both steps show in a diff.

Approval covers only the goldens `approved.tsv` lists, such as the FreeType
`osc66_fraction_alignment.png`. Every other golden under `golden/` is held to
its committed bytes by the run, but whether those bytes are right rests on the
review of the PNG and its diff in the change that wrote them. Count the two
with:

```sh
grep -cv '^#' Tests/Snapshots/approved.tsv
git ls-files 'Tests/Snapshots/golden/*.png' | wc -l
```


List the stable identifiers, titles, and golden names accepted as exact filters:

```sh
python3 Tests/Snapshots/run.py --list
python3 Tests/Snapshots/run.py --list | rg underline
```

## A case whose subject is too small for the budgets

The comparison passes an image whose differing pixels stay under 0.1% of its
area and whose pixels past `REPAINT_DELTA` stay under 48. Both are budgets, and
a small subject cannot afford one: a prompt-mark bracket is a few dozen
high-contrast pixels, so moving it a whole pixel spends 12 of the 48 repaints
and reports `within tolerance`.

A case whose point is where something sits, rather than what colour it came out,
sets `"strict": true` beside its name. That spends no repaint budget at all —
any pixel past `REPAINT_DELTA` fails it — while leaving smaller drift alone, so
the case still survives a rasteriser changing its antialiasing.

Use it for overlay geometry (marks, the cursor, badges, rules). Do not use it to
make an ordinary case stricter: a case covering a whole screen of text has
thousands of legitimately moving edge pixels, and strictness there is a request
to re-mint on every toolchain update.

## Fix an existing rendering bug

Use this workflow even when the committed golden faithfully records the current
but incorrect implementation:

1. Identify whether the defect belongs to the portable contract, a native
   renderer, or both. Run the smallest matching case before changing code:

   ```sh
   python3 Tests/Snapshots/run.py --backend portable case_name
   python3 Tests/Snapshots/run.py --backend macos case_name
   ```

   A pass only proves that the implementation still matches the committed
   image; it does not make a visibly incorrect image correct.
2. Add or strengthen a host unit test for the underlying model, geometry,
   configuration, or render-plan invariant whenever that surface is testable
   without a frontend. The test must fail against the buggy implementation.
3. Fix the owning production layer. Terminal semantics belong in Core;
   platform-independent layout, glyph selection, and geometry belong in
   `Source/Core/Rendering`; native font/GPU behavior belongs in the affected thin
   frontend.
4. Run the named snapshot again. The fix should fail against the old golden and
   write `<case>.actual.png` plus `<case>.diff.png` under
   `$TMPDIR/ubiquitty-regression-fail/`. If the case still passes, its input does
   not expose the fix; strengthen the case before replacing any baseline.
5. Inspect both artifacts, then accept only the corrected case and backend:

   ```sh
   python3 Tests/Snapshots/run.py --backend portable --update case_name
   python3 Tests/Snapshots/run.py --backend macos --update case_name
   ```

6. Rerun the named cases, the affected unit tests, the full snapshot catalogs,
   and catalog integrity. A unit test is a CTest case, which the `run-tests.sh`
   filter never selects; `CTEST_BUILD_DIR` is the test tree the top-level
   README's [Testing](../../README.md#testing) section configures. Commit the
   code fix, unit test, case change if any, and corrected golden together:

   ```sh
   ctest --test-dir "$CTEST_BUILD_DIR" -R '^test_<name>$' --output-on-failure
   ./Scripts/run-tests.sh snapshots
   python3 Tests/Snapshots/run.py --check-catalog
   ```

For a shared case, portable and native acceptance are two separate review
decisions. A native macOS baseline that differs on a developer machine must be
confirmed on a machine of the generation the baselines are minted on, macOS 27,
before it replaces the repository contract.

## Add coverage for existing behavior

Prefer an existing showcase entry when it already demonstrates the behavior:
showcase discovery automatically makes its sequence a portable snapshot and
its `expect` contract a reply check. Add a focused case when the regression
needs a minimal input, strict configuration, precise dimensions, or a native
renderer.

Append a focused object to `cases.json` with:

- a unique lowercase `name`;
- positive `columns` and `rows`;
- exactly one of `feed` or `feed_file`;
- a `doc` that says what the picture pins and what a wrong render would show;
- `settings`, the configuration sections the case needs other than a font —
  `appearance`, `underline`, `cursor`, `tek`, and the layout keys of `font`
  such as `line_spacing` — as an object of sections; a face is never named
  here;
- `native`, the profile under `configs/native/` the case's native tier draws
  with, when the case is about a real face at all;
- `native_stacks`, a subset of `coretext` and `freetype`, when the face or the
  behaviour exists on only some stacks: SF Pro ships with macOS alone, and a
  FreeType golden of the face Linux falls back to would hold the case to a
  picture of another font;
- `scale`, pixels per point, when the case is about rendering at two; the
  runner spells it in each arm's flag (`--device-scale` on portable, linux and
  web, `--scale` on macos, swift and ios);
- optional `args` only for terminal actions that are not configuration.

The runner assembles the configuration an arm is launched with from the tier's
`[font]` block and the case's `settings`, so a case states its settings once
and the tier is the only thing that differs between its renders.

Create only the named missing baselines, inspect them, and rerun without update:

```sh
python3 Tests/Snapshots/run.py --backend portable --update-missing case_name
python3 Tests/Snapshots/run.py --backend macos --update-missing case_name
python3 Tests/Snapshots/run.py --backend portable case_name
python3 Tests/Snapshots/run.py --backend macos case_name
```

The new case must fail or be absent before the change it protects. A case that
passes unchanged on the parent revision is not regression coverage for new
behavior.

## Rename a case

A case's name is also the file name of each of its goldens, the middle of every
`case:` identifier in the waiver files, and a word in any prose that mentions
it. Rename all of these in one change:

```sh
python3 Scripts/rename-snapshot-cases.py --dry-run old_name=new_name
python3 Scripts/rename-snapshot-cases.py old_name=new_name
```

The script moves each golden with `git mv` and rewrites every mention. It
checks the catalogue before and after the rename. A renamed golden keeps its
bytes, so nothing is re-minted.

## Introduce a renderer feature

1. Put the feature in the lowest reusable owning layer. Keep terminal state and
   observable semantics in Core, reusable rendering policy and geometry in
   `Source/Core/Rendering`, and only API submission, platform font access, and GPU
   mechanics in the thin frontend. Do not implement the same policy separately
   in Metal and Vulkan.
2. Add host unit tests for the portable state, configuration processing,
   geometry, batching, and fallback decisions. Add performance coverage or a
   baseline when the feature changes per-cell or per-frame work.
3. Choose snapshot tiers by what the feature promises:

   - terminal semantics and portable rendering behavior require a `portable`
     GNU Unifont case;
   - native font metrics, shaping, emoji, fallback, fractional advances, and GPU
     composition require platform-native cases;
   - a setting or behavior implemented by both paths should be one case with
     common `settings`, an optional `native` font profile, and `native_stacks`
     when the behavior applies only to selected text stacks.

4. Add or extend the showcase entry when the feature is user-demonstrable. The
   showcase sequence is then executable regression input, not separate manual
   documentation. Use a focused case as well only when it adds a smaller
   invariant or native coverage.
5. Generate only the new named baselines with `--update-missing`. If the feature
   changes existing output, review and replace each affected
   existing case separately with filtered `--update` commands.
6. Run the affected unit tests, every relevant backend, the full snapshot suite,
   catalog integrity, and the frontend builds. Regenerate the showcase matrix
   when showcase discovery changes:

   ```sh
   python3 Tests/Snapshots/gen_matrix.py
   python3 Tests/Snapshots/run.py --check-catalog
   ./Scripts/run-tests.sh snapshots
   ```

Missing baselines never appear implicitly. Review PNG changes as source changes.
The common comparator treats a pixel as different when any RGBA channel differs
by more than 12 and fails when more than 0.1% of pixels differ. It also permits
at most 48 pixels with a channel delta greater than 64; strict cases permit
none at that magnitude. The unifont agreement tier requires exact pixels.

The generated showcase coverage index is `Docs/feature_test_matrix.md`; regenerate
it with `python3 Tests/Snapshots/gen_matrix.py`. The harness unit test fails when
the committed matrix is stale.
