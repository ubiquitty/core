#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Run every app-driven terminal regression through one strict harness.

The catalog combines the focused cases in cases.json with the machine-readable
showcase probes. Snapshot cases share one PNG decoder, comparison policy,
failure-artifact path, and update command. Reply probes use the same app binary
and run alongside the image regressions.

The portable tier has never been green, and a skip worded for the macOS
catalog hid that for weeks. The portable tier links Core and SoftwareRenderer
and needs no app build, so it can always run; gate the two tiers separately.

Do not run --update to clear the remainder. A golden committed without being
run records what the renderer did that day, or what someone expected it to do,
and nothing distinguishes the two afterwards. What is left is stale goldens
from a geometry change on 28 Jul that the diff called for — the renderer moved
and the goldens did not — plus a one-row dashed residual from the dashDrop term
added in the same commit. Regenerate one case at a time, each against a diff
someone has looked at, after the geometry question is settled — not in bulk to
make a summary line green.

What this case cost, because the detector is the lesson. A row-ink count with
no contiguity term was used to classify rules, and it manufactured three
defects that do not exist:

  "curly flattens to a solid rule"   false: 20 runs, a 2px gap every 8px — the
                                     6/8-duty wave SoftwareRenderer.cpp's curly
                                     decoration draws. Byte-identical to the
                                     golden on descender-free cells.
  "dashed flattens to two rows"      false as to solid: nineteen 5px runs with
                                     3px gaps, plus an 8px block that is the
                                     CURSOR overlay, present in both images.
  "the golden draws one rule for     false: the golden has two, y=69 (57px) and
   SGR 4:2"                          y=73 (152px). The counter missed y=69
                                     because skip-ink had cut it — a rendering
                                     bug hiding a rule from the detector
                                     investigating that same bug.

A detector without a longest-run test cannot tell a wave from a bar, and one
without a floor for cut rules cannot see a rule that skip-ink trimmed. Both
failures point the same way: toward a defect that is not there.

Claims refuted along the way — do not re-investigate:

  the rule moved 2px            artefact of matching the golden's only counted
                                rule against the second of the renderer's pair
  the renderer omits            refuted: buildCellInstances applies it,
  underlineDrop                 and underlineStrokeLift is 0 off macOS
  a null instance stream        refuted: a null stream can only push a glyph
  costs ink                     back onto the cell, never blank it
  the withinCell clamp never    WRONG. It does bind for the DOUBLE, where the
  binds at cellHeight 18        span is t+gap+t=3, so lowest = 72-3 = 69. True
                                only for the single.

A 2px rule shift was reported here earlier and it was an artefact. It came
from matching the golden's y=73 against the actual's y=71 because both were
152 ink wide — but those are different features: y=73 is the golden's only
rule and y=71 is the second of the actual's pair. On the single-underline row,
where the features genuinely correspond, the offset is 1px. Everything built
on the 2px is withdrawn: the row-start derivation, and the two-vertical-
derivations mechanism that was inferred from it.

A shift of any size is also ruled out independently. Comparing the images at
every offset from -3 to +3 rows, differing pixels are minimised at dy=0 and
nearly double at dy=+/-2 — so nothing outside the rule has moved, and no
uniform-offset explanation can survive that whatever its mechanism.

That sweep is the technique worth copying from this. Four candidate mechanisms
died to reading the code one at a time; the sweep killed an entire class
without naming a mechanism at all, and it is what exposed the artefact that
three of the four were chasing.

Bisecting this does not work and the attempt is recorded so it is not repeated.
There is no good endpoint in the range: the catalog was already red at the
goldens' own commit, so every step votes bad. The tell was the totals moving —
242 cases in one run against 256 in another — which is a catalog that changed
size and cannot be compared across the range at all.

The remaining step is a person looking at underline_styles.actual.png beside
its golden and saying which rule position is correct. It is the only part here
that cannot be automated, and --update before it makes whichever is wrong the
permanent reference.
"""

import argparse
import atexit
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent

# Where this checkout builds. Scripts/build_root.py is the one answer — the
# build output lives outside the checkout, so an arm looked for under ROOT/build
# is found only where a stale directory happens to survive, which resolves on a
# developer's machine and refuses on a fresh clone.
sys.path.insert(0, str(ROOT / "Scripts"))
import build_root as _build_root  # noqa: E402 — the path above is what makes it importable
import ubq_roots as _roots  # noqa: E402
from sources import SnapshotSources

BUILD_ROOT = Path(_build_root.build_root(str(ROOT)))
SUBJECT_ROOT = ROOT
SOURCE_FILES = None
GOLDEN_ROOT = HERE / "golden"
# The goldens checked by eye. A listed golden is never written here: a render
# that differs from it is the renderer's failure, not a reason to re-mint.
APPROVED_MANIFEST = HERE / "approved.tsv"
FAILURE_ROOT = Path(os.environ.get(
    "UBIQUITTY_SNAPSHOT_FAILURE_DIR",
    Path(tempfile.gettempdir()) / "ubiquitty-regression-fail",
))
SHOWCASE = ROOT / "Tools" / "showcase" / "showcase.py"

# A pixel differs when at least one RGB channel exceeds CHANNEL_TOLERANCE. The
# image fails when the differing-pixel fraction exceeds MAX_DIFFERING_FRACTION.
CHANNEL_TOLERANCE = 12
MAX_DIFFERING_FRACTION = 0.001

# A fraction alone cannot see a small subject. It scales with the image, so the
# larger the golden the more may change: on a 652x438 case one whole terminal
# cell — 144 pixels repainted, worst channel delta 246 — is 0.05% and passes,
# while two cells fail. Most of what this suite is about happens in one cell.
#
# So a second rule, on magnitude rather than area. Antialiasing across platforms
# moves many pixels a little; a wrong glyph, colour or attribute moves few
# pixels a lot. A pixel past REPAINT_DELTA is not a rasteriser disagreeing, and
# enough of them together is a feature that changed whatever the image size.
REPAINT_DELTA = 64
MAX_REPAINTED_PIXELS = 48

# Both rules above are budgets, and a budget is exactly what a small subject
# cannot afford. A prompt mark is a few dozen high-contrast pixels: move the
# bracket one pixel and 36 of them change by 200 codes, which is 0.01% of the
# image and 36 of the 48 allowed repaints — a geometry regression reported as
# "within tolerance". A case that says `strict` spends no repaint budget at all:
# any pixel past REPAINT_DELTA fails it. Sub-REPAINT_DELTA drift still passes,
# so the case does not break when a rasteriser updates its antialiasing; only
# ink that MOVED does, which is the whole of what such a case is asked to hold.
STRICT_MAX_REPAINTED_PIXELS = 0
SAFE_NAME = re.compile(r"^[a-z0-9][a-z0-9_]*$")

# Every arm a case may name. Listed so a run can say which ones it left out
# rather than reporting only what it reached.
ALL_BACKENDS = ("portable", "macos", "ios", "linux", "web", "swift")
CASE_KEYS = {
    "name", "columns", "rows",
    "feed", "feed_file", "showcase", "args", "stdout_contains",
    "stdout_excludes", "frames", "doc", "strict",
    "settings", "native", "native_stacks", "scale",
}
# The text stacks a native golden can span. A case names a subset through
# `native_stacks` when its face or its feature exists on only some of them:
# a face one platform alone ships resolves to a fallback on the other, and a
# golden of that fallback holds the case to a picture of a different font.
NATIVE_STACKS = ("coretext", "freetype")
# The flag each arm takes a device scale through. One case field, `scale`,
# rather than an argument spelled per arm: a case that renders at two
# pixels a point says so once, and every arm is asked the same thing.
SCALE_FLAG = {"portable": "--device-scale", "linux": "--device-scale",
              "macos": "--scale", "swift": "--scale", "ios": "--scale", "web": "--device-scale"}
# The keys of a [font] block that belong to a native profile rather than to a
# case's settings: a case says which face it wants through `native`, and a
# settings block naming one would make the tier ambiguous.
FONT_PROFILE_KEYS = {
    "family", "size", "weight", "bold_weight", "width", "variations", "features",
    "fallback", "symbol_fallback", "emoji_fallback", "cjk_fallback", "ligatures",
    "wide_glyph_boost", "wide_glyph_scale", "proportional_family",
}
NATIVE_PROFILE_ROOT = HERE / "configs" / "native"
# The unifont tier's face: GNU Unifont is baked into Core, so every arm draws
# it from the same bytes and every arm is held to one golden.
UNIFONT_FONT = {"family": "GNU Unifont", "size": 10}
# The native tiers: which arms share a text stack, and so a golden.
CORETEXT_BACKENDS = ("macos", "swift", "ios")
FREETYPE_BACKENDS = ("linux",)


def configure_sources(source=None, core=None, kit=None):
    """Bind catalog data and built products to the actual consuming checkout."""
    global ROOT, HERE, SUBJECT_ROOT, BUILD_ROOT, GOLDEN_ROOT, SHOWCASE, NATIVE_PROFILE_ROOT, SOURCE_FILES, _APPROVED, _CONFIG_DIRECTORY, FIXTURE_FONT_DIRECTORY, _fixture_fontconfig
    roots = _roots.source_roots(source, core, kit)
    files = SnapshotSources(roots, _roots.repositories)
    ROOT = roots.core
    HERE = ROOT / "Tests" / "Snapshots"
    SUBJECT_ROOT = roots.subject
    BUILD_ROOT = Path(_build_root.build_root(str(SUBJECT_ROOT)))
    GOLDEN_ROOT = HERE / "golden"
    SHOWCASE = ROOT / "Tools" / "showcase" / "showcase.py"
    NATIVE_PROFILE_ROOT = HERE / "configs" / "native"
    FIXTURE_FONT_DIRECTORY = HERE / "fixtures" / "fonts"
    _fixture_fontconfig = None
    SOURCE_FILES = files
    _APPROVED = None
    _CONFIG_DIRECTORY = None
    return roots


def snapshot_path(relative, *, required=False):
    if SOURCE_FILES:
        return SOURCE_FILES.path(relative, required=required)
    relative = Path(relative)
    if relative.parts[:1] == ("golden",):
        return GOLDEN_ROOT.joinpath(*relative.parts[1:])
    if relative.parts[:2] == ("configs", "native"):
        return NATIVE_PROFILE_ROOT.joinpath(*relative.parts[2:])
    return HERE / relative


def manifest_paths(pattern):
    return SOURCE_FILES.manifests(pattern) if SOURCE_FILES else sorted((ROOT / "Tests" / "Snapshots").glob(pattern))


def repository_file(relative):
    return SOURCE_FILES.repository_file(relative) if SOURCE_FILES else ROOT / relative


def display_path(path):
    """Keep diagnostics valid when a golden belongs to a different owner."""
    return path.relative_to(ROOT) if path.is_relative_to(ROOT) else path


@dataclass(frozen=True)
class Regression:
    identifier: str
    catalog: str
    section: str
    title: str
    kind: str
    backend: str
    golden: Optional[Path] = None
    # Held to pixel identity rather than the comparator's tolerance: the unifont
    # tier, where every arm draws the same pixels or one of them is wrong.
    exact: bool = False
    case: Optional[dict] = None
    sequence: Optional[bytes] = None
    expected_reply: Optional[str] = None
    # Set where the case selects a code path the default does not take. The arm
    # is otherwise identical — same binary, same case, same golden — so the
    # variable is the whole of what distinguishes the two results.
    environment: Optional[dict] = None
    # The builder this arm must report having used. Without it an arm that
    # selects a path through the environment cannot tell the selection working
    # from the selection being ignored: both render, both compare against the
    # same golden, and the one that silently fell back agrees with it.
    expected_builder: Optional[str] = None
    # The `--snapshot-backend` token, where it differs from the arm's identity.
    #
    # `backend` decides which golden this arm is held to and which
    # known-failing-*.txt records it; the token decides what the binary renders
    # with. They are the same for every arm but the Swift encoder one, which is
    # a macOS arm — macOS golden, macOS waiver file — that has to render through
    # the token `--backend swift` sets, because the app's snapshot command
    # (Core/App/SnapshotRequest.h) reads the token, not the
    # environment.
    snapshot_backend: Optional[str] = None
    # The feeds this arm applies after frame 1, each on the retained state, before
    # snapshotting the last onto its golden. Set only for a second-frame arm: the
    # base case arm leaves it None and snapshots frame 1. A retention defect — a
    # ghost that travels with the blit — is invisible on frame 1, which is why a
    # second frame needs a golden of its own rather than a build count.
    also_feeds: Optional[list] = None


def fail(message):
    print(f"regressions: {message}", file=sys.stderr)
    # A refusal has to be visible to the filter a verdict is read through.
    #
    # Every reason this is reached means no case was compared: a stale or
    # foreign binary, a manifest entry matching nothing, a filter selecting
    # nothing, orphaned goldens. The VERDICT line is printed because a reader
    # piping this through `grep -E "^SUMMARY|VERDICT"` would otherwise get no
    # output — and no output reads as nothing to report rather than as a stop.
    #
    # REFUSED rather than FAILED because they are different instructions. A
    # failure says look at the case; this says look at the build, and nothing
    # here is evidence about any case either way.
    print("VERDICT  REFUSED — no case was compared; see stderr")
    return 2


def sequence_bytes(sequence, encoding):
    if encoding not in ("latin-1", "utf-8"):
        raise ValueError(f"unsupported showcase sequence encoding: {encoding}")
    return sequence.encode(encoding)


def expected_reply_kind(expected):
    if expected is None:
        return None
    if "consumed, nothing printed" in expected:
        return "empty"
    return "regex"


def _reject_duplicate_keys(pairs):
    """Fail on a repeated JSON key rather than silently keeping the last one.

    A splice into cases.json can leave an object carrying two `agree` keys (or
    two of any field). json.loads accepts that and keeps whichever comes last,
    so every schema check downstream reads the collapsed object and never sees
    the malformed text it came from — a validity check passes over invalid
    input. This hook reads both keys before one is discarded, which is the only
    place the duplicate is still visible.
    """
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise ValueError(f"cases.json has a duplicate key: {key!r}")
        seen[key] = value
    return seen


_CONFIG_DIRECTORY = None


# What every case renders with unless it says otherwise. See
# `materialised_config` for what earns a place here.
HARNESS_BASELINE = {
    # Color fixtures compare requested RGB; contrast cases opt into adjustment.
    # sRGB because the shipped default is Display P3, and the arm that honours
    # it decodes a background image into P3: every image golden would move with
    # a default no case means to test.
    "appearance": {"text_contrast": False, "color_space": "srgb"},
    "cursor": {"shape": "block"},
}

# Sections whose keys are the reader's own text rather than setting names: a
# `[triggers]` entry maps a regex to an action, so its key is written quoted.
REGEX_KEYED_SECTIONS = frozenset({"triggers"})


def toml_literal(value):
    """One TOML value from a registry value: strings quoted, booleans and numbers bare."""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return repr(value) if isinstance(value, float) else str(value)
    return json.dumps(value, ensure_ascii=False)


def native_profile_text(profile):
    """The `[font]` block a native profile carries, as the file has it."""
    return snapshot_path(f"configs/native/{profile}.toml", required=True).read_text(encoding="utf-8")


def materialised_config(case, tier, font_text):
    """The configuration file one (case, tier) renders with, written once per process.

    A case carries its own settings and names a face through a tier; the
    binaries take one TOML file. So the file is assembled here — the tier's
    [font] block followed by the case's sections — into a directory that lives
    for the run, and the path is what the arm is launched with. Assembled
    rather than stored, so a case's settings are stated once, in the registry,
    and the tier is the only thing that differs between its renders.
    """
    global _CONFIG_DIRECTORY
    if _CONFIG_DIRECTORY is None:
        # Under this directory, not the system's temporary one: the Linux arm
        # renders inside a container that sees the repository and nothing
        # else, and its shim maps paths under the repository root. A config
        # written elsewhere is a path the container cannot open, and every
        # case then fails before it draws.
        _CONFIG_DIRECTORY = Path(tempfile.mkdtemp(prefix=".ubiquitty-configs-", dir=HERE))
        atexit.register(shutil.rmtree, _CONFIG_DIRECTORY, True)
    path = _CONFIG_DIRECTORY / f"{case['name']}.{tier}.toml"
    if not path.exists():
        # The harness's own baseline, under the case's settings so any case can
        # name something else. What belongs here is a preference whose SHIPPED
        # default a case does not mean to be testing: a suite that inherits one
        # re-renders every case the day the product changes it, and the diff
        # arrives as hundreds of unrelated failures rather than as the one
        # decision that moved.
        #
        # `[cursor] shape` is one of these. A bare model's zero-initialised shape
        # is Block, and the shipped default is not, so an arm that seeds the
        # shape from the reader's config would move every golden carrying a
        # cursor.
        sections = {section: dict(keys) for section, keys in HARNESS_BASELINE.items()}
        for section, keys in (case.get("settings") or {}).items():
            sections.setdefault(section, {}).update(keys)
        lines = [font_text.rstrip("\n"), ""]
        for section, keys in sections.items():
            lines.append(f"[{section}]")
            for key, value in keys.items():
                spelled = json.dumps(key, ensure_ascii=False) if section in REGEX_KEYED_SECTIONS else key
                lines.append(f"{spelled} = {toml_literal(value)}")
            lines.append("")
        path.write_text("\n".join(lines), encoding="utf-8")
    return path


def unifont_font_text():
    return "[font]\n" + "".join(f"{key} = {toml_literal(value)}\n" for key, value in UNIFONT_FONT.items())


def arm_variant(case, backend, config):
    """The case as one arm is asked to render it: its config, and its scale in the arm's spelling."""
    variant = dict(case)
    variant["backend"] = backend
    variant["config"] = str(config)
    if "scale" in case:
        variant["args"] = list(case.get("args", [])) + [SCALE_FLAG[backend], str(case["scale"])]
    return variant


def load_case_regressions():
    raw_cases = json.loads((HERE / "cases.json").read_text(encoding="utf-8"),
                           object_pairs_hook=_reject_duplicate_keys)
    if not isinstance(raw_cases, list) or not raw_cases:
        raise ValueError("cases.json must contain a non-empty list")
    regressions = []
    names = set()
    showcase_sequences = None
    for index, case in enumerate(raw_cases):
        validate_case(case, index, names)
        name = case["name"]
        # A case fed by a showcase entry renders that entry's exact sequence on
        # every arm, so the probe the portable catalog holds to one picture is
        # also held to each arm's own; the bytes are the probe's, never a copy.
        if "showcase" in case:
            if showcase_sequences is None:
                showcase_sequences = showcase_sequences_by_golden()
            if case["showcase"] not in showcase_sequences:
                raise ValueError(f"case {name}.showcase names no showcase probe: {case['showcase']}")
            case = dict(case, feed_bytes=showcase_sequences[case["showcase"]])
        # Every case renders in two tiers. The unifont tier is every arm at
        # once: GNU Unifont is baked into Core, a bitmap face with no hinter
        # and no antialiaser, so every renderer rasterising a code point
        # produces the same bytes and the six arms are held to one golden,
        # byte for byte. Any difference is a real defect in one arm's layout
        # or colour, and nothing else can hide in it.
        unifont_config = materialised_config(case, "unifont", unifont_font_text())
        for backend in ALL_BACKENDS:
            variant = arm_variant(case, backend, unifont_config)
            # A stdout contract names what the native face resolved to; in the
            # unifont tier every arm draws GNU Unifont, so the contract is the
            # native tier's and is not asked here.
            variant.pop("stdout_contains", None)
            variant.pop("stdout_excludes", None)
            regressions.append(Regression(
                identifier=f"case:{name}:unifont:{backend}",
                catalog="cases",
                section="Unifont agreement",
                title=name,
                kind="snapshot",
                backend=backend,
                golden=snapshot_path(f"golden/unifont/{name}.png"),
                case=variant,
                expected_builder="core" if backend == "macos" else None,
                exact=True,
            ))
        # The native tier is the arms that share a text stack, held to one
        # golden per stack: CoreText on macOS, Swift and iOS; FreeType on
        # Linux. Core Text and FreeType disagree on the edge pixels of every
        # glyph, which is why a native golden spans one stack and not all six.
        # A case names its face through a profile; a case without one has no
        # native tier and is not red for lacking it.
        native = case.get("native")
        if native is None:
            continue
        native_config = materialised_config(case, "native", native_profile_text(native))
        stacks = case.get("native_stacks", NATIVE_STACKS)
        for stack, backends in (("coretext", CORETEXT_BACKENDS), ("freetype", FREETYPE_BACKENDS)):
            if stack not in stacks:
                continue
            golden = snapshot_path(f"golden/native/{stack}/{name}.png")
            for backend in backends:
                variant = arm_variant(case, backend, native_config)
                # A stdout contract quotes the macOS driver's own report of
                # the face CoreText resolved. The Linux and iOS drivers print
                # no such line, so on those arms the case is its pixels.
                if stack == "freetype" or backend == "ios":
                    variant.pop("stdout_contains", None)
                    variant.pop("stdout_excludes", None)
                regressions.append(Regression(
                    identifier=f"case:{name}:{stack}:{backend}",
                    catalog="cases",
                    section=f"Native agreement ({stack})",
                    title=name,
                    kind="snapshot",
                    backend=backend,
                    golden=golden,
                    case=variant,
                    expected_builder="core" if backend == "macos" else None,
                ))
                # The macos and swift arms are one binary and one render path
                # (SnapshotRequest.h reads both tokens alike); they differ only in
                # the markers asserted above, so these frames are one
                # renderer's, checked under each arm's markers. The surface is
                # retained between frames: the same case fed one frame further
                # on the retained state, held to a golden of its own. A retention
                # defect — a ghost that travels with the blit — is invisible on
                # frame 1, which is why the later frame needs a golden rather
                # than a build count.
                if backend not in ("macos", "swift"):
                    continue
                for depth, frame in enumerate(variant.get("frames") or []):
                    suffix = frame["suffix"]
                    feeds = [step["feed"] for step in variant["frames"][:depth + 1]]
                    regressions.append(Regression(
                        identifier=f"case:{name}_{suffix}:{stack}:{backend}",
                        catalog="cases",
                        section=f"Native agreement ({stack})",
                        title=f"{name}_{suffix}",
                        kind="snapshot",
                        backend=backend,
                        golden=snapshot_path(f"golden/native/{stack}/{name}_{suffix}.png"),
                        case=variant,
                        also_feeds=feeds,
                        expected_builder="core",
                    ))
    return regressions


# One frame encoder. The second-encoder arm rendered every macOS case twice —
# once per encoder, both against one shared golden — because two encoders in
# one binary could disagree and a per-encoder golden would have let them. The
# ObjC++ encoder is deleted, so the arm's subject is gone: the macOS backend
# renders through the swift encoder and EXPECTED_ENCODER refuses anything else.
def second_encoder_arm(name, backend, golden, variant):
    """No second encoder exists; the macOS arm is the swift-encode arm.

    - Returns: An empty list, on every backend.
    """
    return []


def agreement_exclusions(raw_cases):
    """Backends that render a case no group of which holds them to agreement.

    Such a backend still renders the case; it is sent to a golden of its own
    instead of a shared one. Both arms report a pass, and the pass means two
    different things — one says the arms drew the same pixels, the other says
    this arm drew what it drew last time.

    Reported because the difference is invisible in the result. A case counted
    under cross-platform agreement can carry an arm that agreed with nothing,
    and the arm most likely to be in that position is the one with a second
    renderer to diverge from.

    - Parameter rawCases: The case records as read from `cases.json`.
    - Returns: Backend name to the number of cases it renders without being
      party to any agreement in them.
    """
    excluded = {}
    for case in raw_cases:
        groups = case.get("groups") or []
        if not any(group.get("agree") for group in groups):
            continue
        agreeing = {backend for group in groups if group.get("agree")
                    for backend in group["backends"]}
        rendering = {backend for group in groups for backend in group["backends"]}
        for backend in rendering - agreeing:
            excluded[backend] = excluded.get(backend, 0) + 1
    return excluded


def validate_case(case, index, names):
    if not isinstance(case, dict):
        raise ValueError(f"cases.json[{index}] must be an object")
    unknown = set(case) - CASE_KEYS
    if unknown:
        raise ValueError(
            f"cases.json[{index}] has unknown fields: {', '.join(sorted(unknown))}")
    name = case.get("name")
    if not isinstance(name, str) or not SAFE_NAME.fullmatch(name):
        raise ValueError(f"cases.json[{index}].name must be lowercase snake_case")
    if name in names:
        raise ValueError(f"duplicate focused case name: {name}")
    names.add(name)
    for field in ("columns", "rows"):
        value = case.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
            raise ValueError(f"case {name}.{field} must be a positive integer")
    settings = case.get("settings", {})
    if not isinstance(settings, dict):
        raise ValueError(f"case {name}.settings must be an object of sections")
    for section, keys in settings.items():
        if not isinstance(section, str) or not SAFE_NAME.fullmatch(section):
            raise ValueError(f"case {name}.settings section names are lowercase snake_case")
        if not isinstance(keys, dict) or not keys:
            raise ValueError(f"case {name}.settings.{section} must be a non-empty object")
        for key, value in keys.items():
            if section in REGEX_KEYED_SECTIONS:
                # The key is the reader's own regex and the value an action
                # spec, so the setting-name rule has nothing to say about the
                # key; a line break is the one thing a TOML key cannot carry.
                if not isinstance(key, str) or not key or "\n" in key:
                    raise ValueError(f"case {name}.settings.{section} keys are single-line regexes")
                if not isinstance(value, str) or not value:
                    raise ValueError(f"case {name}.settings.{section}.{key} must be an action spec string")
                continue
            if not isinstance(key, str) or not SAFE_NAME.fullmatch(key):
                raise ValueError(f"case {name}.settings.{section} keys are lowercase snake_case")
            if isinstance(value, bool) or isinstance(value, (str, int, float)):
                continue
            raise ValueError(f"case {name}.settings.{section}.{key} must be a string, number or boolean")
        # A face is named through `native`, never through settings: the tier
        # supplies the font, and a settings block carrying one would make two
        # tiers of one case draw the same face.
        if section == "font" and set(keys) & FONT_PROFILE_KEYS:
            raise ValueError(f"case {name}.settings.font names a face; use native instead")
    native = case.get("native")
    if native is not None:
        if not isinstance(native, str) or not SAFE_NAME.fullmatch(native):
            raise ValueError(f"case {name}.native must be a lowercase snake_case profile name")
        if not snapshot_path(f"configs/native/{native}.toml").is_file():
            raise ValueError(f"case {name}.native names no profile under configs/native: {native}")
    if "native_stacks" in case:
        stacks = case["native_stacks"]
        if native is None:
            raise ValueError(f"case {name}.native_stacks needs a native profile")
        if (not isinstance(stacks, list) or not stacks or len(set(stacks)) != len(stacks)
                or not set(stacks) <= set(NATIVE_STACKS)):
            raise ValueError(f"case {name}.native_stacks must be a non-empty list of distinct stacks from "
                             f"{', '.join(NATIVE_STACKS)}")
    if "doc" in case and not isinstance(case["doc"], str):
        raise ValueError(f"case {name}.doc must be a string")
    if "scale" in case:
        scale = case["scale"]
        if isinstance(scale, bool) or not isinstance(scale, int) or scale < 1 or scale > 4:
            raise ValueError(f"case {name}.scale must be an integer from 1 to 4")
    has_feed = "feed" in case
    has_feed_file = "feed_file" in case
    if has_feed + has_feed_file + ("showcase" in case) != 1:
        raise ValueError(f"case {name} needs exactly one of feed, feed_file or showcase")
    if "showcase" in case and (not isinstance(case["showcase"], str) or not SAFE_NAME.fullmatch(case["showcase"])):
        raise ValueError(f"case {name}.showcase must name a showcase probe's golden")
    if has_feed and not isinstance(case["feed"], str):
        raise ValueError(f"case {name}.feed must be a string")
    if has_feed_file:
        feed_file = case["feed_file"]
        if not isinstance(feed_file, str) or not feed_file:
            raise ValueError(f"case {name}.feed_file must be a relative path")
        resolved = snapshot_path(feed_file, required=True).resolve()
        if (not SOURCE_FILES and HERE.resolve() not in resolved.parents) or not resolved.is_file():
            raise ValueError(f"case {name}.feed_file must name a file under {HERE}")
    if "args" in case and not isinstance(case["args"], list):
        raise ValueError(f"case {name}.args must be a list")
    for field in ("stdout_contains", "stdout_excludes"):
        if field in case and (not isinstance(case[field], str) or not case[field]):
            raise ValueError(f"case {name}.{field} must be a non-empty string")
    if "frames" in case:
        # A second-frame arm drives the app's --also-feed, which only the
        # macOS binary parses, so a frames case is macOS-only until the portable
        # one grows the flag. Each entry snapshots one later frame to a golden of
        # its own, keyed by suffix; an empty feed is legal — it renders the
        # retained scene again, the pure grid-cache-retention probe.
        if native is None:
            raise ValueError(
                f"case {name}.frames needs a native profile: the later frames render on the coretext macOS arm")
        frames = case["frames"]
        if not isinstance(frames, list) or not frames:
            raise ValueError(f"case {name}.frames must be a non-empty list")
        seen = set()
        for entry in frames:
            if not isinstance(entry, dict) or set(entry) - {"suffix", "feed"}:
                raise ValueError(f"case {name}.frames entries take exactly suffix and feed")
            suffix = entry.get("suffix")
            if not isinstance(suffix, str) or not SAFE_NAME.fullmatch(suffix):
                raise ValueError(f"case {name}.frames suffix must be lowercase snake_case")
            if suffix in seen:
                raise ValueError(f"case {name}.frames has a duplicate suffix: {suffix}")
            seen.add(suffix)
            if not isinstance(entry.get("feed"), str):
                raise ValueError(f"case {name}.frames[{suffix}].feed must be a string")


def showcase_probes():
    result = subprocess.run(
        [sys.executable, str(SHOWCASE), "--probes"],
        capture_output=True,
        text=True,
        timeout=10,
    )
    if result.returncode:
        raise RuntimeError(f"showcase probe discovery failed: {result.stderr.strip()}")
    return json.loads(result.stdout)


def showcase_sequences_by_golden():
    """Each showcase probe's exact byte sequence, by its golden tag."""
    sequences = {}
    for index, probe in enumerate(showcase_probes()):
        validate_showcase_probe(probe, index)
        sequences[probe["golden"]] = sequence_bytes(probe["sequence"], probe["sequence_encoding"])
    return sequences


def load_showcase_regressions():
    regressions = []
    for index, probe in enumerate(showcase_probes()):
        validate_showcase_probe(probe, index)
        expected = probe["expect"]
        golden_name = probe["golden"]
        module = probe.get("module") or "probe"
        stable_name = golden_name or f"{module}-{index}"
        sequence = probe["sequence"]
        if sequence is None or golden_name is None:
            raise RuntimeError(
                f"showcase entry is not snapshot-testable: {probe['section']} / "
                f"{probe['title']}")
        encoded_sequence = sequence_bytes(sequence, probe["sequence_encoding"])
        # An entry whose feature draws nothing asserts its reply and takes no
        # picture: its frame would be the blank one, which proves nothing.
        if probe.get("snapshot", True):
            regressions.append(Regression(
                identifier=f"showcase:{stable_name}",
                catalog="showcase",
                section=probe["section"],
                title=probe["title"],
                kind="snapshot",
                backend="portable",
                golden=snapshot_path(f"golden/portable/showcase/{golden_name}.png"),
                sequence=encoded_sequence,
            ))
        if expected is not None:
            regressions.append(Regression(
                identifier=f"showcase-reply:{stable_name}",
                catalog="showcase",
                section=probe["section"],
                title=f"{probe['title']} [reply]",
                kind="reply",
                backend="portable",
                sequence=encoded_sequence,
                expected_reply=expected,
            ))
    return regressions


def validate_showcase_probe(probe, index):
    if not isinstance(probe, dict):
        raise ValueError(f"showcase probe {index} must be an object")
    for field in ("section", "module", "title", "sequence", "golden"):
        if not isinstance(probe.get(field), str) or not probe[field]:
            raise ValueError(f"showcase probe {index}.{field} must be a non-empty string")
    if not SAFE_NAME.fullmatch(probe["golden"]):
        raise ValueError(f"showcase probe {index}.golden must be lowercase snake_case")
    if probe.get("sequence_encoding") not in ("latin-1", "utf-8"):
        raise ValueError(f"showcase probe {index} has an invalid sequence encoding")
    if probe.get("expect") is not None and not isinstance(probe["expect"], str):
        raise ValueError(f"showcase probe {index}.expect must be a string or null")
    if not isinstance(probe.get("snapshot", True), bool):
        raise ValueError(f"showcase probe {index}.snapshot must be a bool")
    if not probe.get("snapshot", True) and probe.get("expect") is None:
        raise ValueError(f"showcase probe {index} takes no picture and asserts no reply")


def load_regressions(catalog="all"):
    regressions = []
    if catalog in ("all", "cases"):
        regressions.extend(load_case_regressions())
    if catalog in ("all", "showcase"):
        regressions.extend(load_showcase_regressions())

    identifiers = [regression.identifier for regression in regressions]
    if len(identifiers) != len(set(identifiers)):
        raise RuntimeError("regression identifiers must be unique")
    # Two regressions may share a golden only when they are the same case on
    # different backends — which is what an agreement case is, and the only
    # sharing that is meant. Any other collision is two cases writing over each
    # other, so what must be unique is the case claiming a golden rather than
    # the golden appearing once.
    shared = {}
    for regression in regressions:
        if regression.golden is None:
            continue
        shared.setdefault(regression.golden, set()).add(regression.title)
    for golden, titles in shared.items():
        if len(titles) > 1:
            raise RuntimeError(
                f"golden {golden.name} is claimed by {len(titles)} cases: "
                f"{', '.join(sorted(titles))}")
    return regressions


def baseline_problems(regressions):
    """The goldens the catalog expects but lacks, and the files no case names.

    A golden is expected where at least one AUDITED arm holding it is not on
    that arm's known-failing list: a picture every such arm is known unable to
    produce (a freetype golden for a case whose flags the Linux driver does not
    parse) is the known line's gap, not a missing baseline. An arm is audited
    when it carries a known-failing manifest, because that file is where an arm
    states its gaps; an arm that states none has not been held to the catalog
    and cannot be the thing that certifies a picture.

    - Parameter regressions: The regressions in play.
    - Returns: (missing, orphaned), each a sorted list of paths.
    """
    # An arm certifies a golden only if it has declared what it cannot draw. A
    # backend with no manifest has never been audited against this catalog, and
    # counting it as able to reproduce every picture is what lets a golden no
    # arm renders sit unchallenged: the one arm nobody runs covers for it, and
    # the picture drifts with nothing to contradict it.
    audited = {backend for backend in {regression.backend for regression in regressions}
               if known_failing_identifiers(backend) is not None}
    known = set()
    for backend in audited:
        known.update(known_failing_identifiers(backend) or set())
    expected = {regression.golden.resolve() for regression in regressions
                if regression.golden is not None and regression.backend in audited
                and regression.identifier not in known}
    existing = set()
    directories = {regression.golden.parent for regression in regressions
                   if regression.golden is not None}
    for directory in directories:
        if directory.exists():
            existing.update(path.resolve() for path in directory.glob("*.png"))
    return sorted(expected - existing), sorted(existing - expected)


# The environment variable that names each arm's binary when no argument does.
BINARY_ENVIRONMENT_NAMES = {
    "portable": "UBIQUITTY_SNAPSHOT_BIN",
    "swift": "UBIQUITTY_MACOS_BIN",
    "macos": "UBIQUITTY_MACOS_BIN",
    "ios": "UBIQUITTY_IOS_SNAPSHOT_BIN",
    "linux": "UBIQUITTY_LINUX_BIN",
    "web": "UBIQUITTY_WEB_BIN",
}


def find_binary(argument, backend):
    named = os.environ.get(BINARY_ENVIRONMENT_NAMES.get(backend, ""), "")
    if argument:
        candidate = Path(argument)
    elif named:
        candidate = Path(named)
    else:
        portable = BUILD_ROOT / "core" / "ubiquitty-snapshot"
        macos = (BUILD_ROOT / "macos" / "Source" / "Platform" /
                 "macOS" / "Ubiquitty.app" / "Contents" / "MacOS" /
                 "Ubiquitty")
        linux = BUILD_ROOT / "linux" / "ubiquitty-linux"
        # The web frontend is a wasm module rather than a program, so its arm is
        # the script that loads one. It ships in the tree instead of a build
        # directory, and answers --print-render-binary with the module the
        # freshness guards should be measuring.
        # The Swift arm is the macOS binary with its renderer knob selected by
        # the backend name, so it resolves to the same artefact.
        if backend in ("web", "ios"):
            candidate = repository_file(f"Scripts/{backend}-snapshot-shim." + ("mjs" if backend == "web" else "sh"))
        else:
            candidate = {"portable": portable, "macos": macos, "linux": linux, "swift": macos}[backend]
    # The web arm is a launcher, not the thing it renders with. Naming the
    # module here is the reachable mistake — it is the artefact every other arm
    # names, it exists, and it is what the freshness guards report on — and
    # without this it lands on the message below, which says a file that is
    # sitting there was not built.
    if candidate.suffix == ".wasm":
        raise RuntimeError(
            f"{backend} arm was given the wasm module at {candidate}, which is what it "
            f"renders with rather than what it runs.\n"
            f"  Pass the launcher instead: Scripts/web-snapshot-shim.mjs. It loads the "
            f"module and answers --print-render-binary with it, so the freshness guards "
            f"still measure the module you meant.")
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        raise RuntimeError(
            f"{backend} snapshot binary not found at {candidate}; build it first")
    rendering = _rendering_binary(candidate)
    _refuse_if_stale(rendering, backend)
    _refuse_if_foreign(rendering, backend)
    return str(candidate)


def _rendering_binary(candidate):
    """The executable that actually renders, when `candidate` only launches it.

    A backend that cannot run on this host — the Linux frontend on a Mac —
    arrives as a shell script that runs the real binary inside a container. Both
    guards below ask a question about the artefact that draws the pixels, and a
    script's mtime and provenance answer neither: the script is edited once and
    then never again while the binary it launches is rebuilt every day, so the
    staleness check fires forever and no rebuild can clear it.

    A launcher says which binary it stands for. Anything that does not answer is
    measured as itself, so a real binary needs no cooperation.
    """
    try:
        with candidate.open("rb") as handle:
            if handle.read(2) != b"#!":
                return candidate
    except OSError:
        return candidate
    try:
        answer = subprocess.run([str(candidate), "--print-render-binary"],
                                capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return candidate
    named = Path(answer.stdout.strip()) if answer.returncode == 0 else None
    return named if named and named.is_file() else candidate


def _refuse_if_stale(binary, backend):
    """Refuse a binary older than the sources it was built from.

    Nothing here builds. The tier renders with whatever executable is on disk
    and reports the result as a fact about the tree, so a binary from before the
    last render change produces a diff against a renderer nobody is running —
    and the failure reads as a regression in the goldens rather than as a stale
    artefact. A case that has already been FIXED reports as failing, which is the
    most expensive possible direction to be wrong in: it sends someone to
    diagnose a bug that is not there.

    Scoped to the portable Core's own sources, from the same SSOT the archives
    are built from, because a touched Platform file is not an input to this
    binary and would make the refusal unclearable.
    """
    ssot = ROOT / "Scripts" / "portable-core-sources.txt"
    directories = set()
    if ssot.exists():
        for line in ssot.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line.startswith("Source/"):
                directories.add(ROOT / os.path.dirname(line))
    if not directories:
        return  # no SSOT to measure against; say nothing rather than guess

    newest, newest_path = 0.0, None
    for directory in directories:
        if not directory.is_dir():
            continue
        for entry in directory.iterdir():
            if entry.is_file() and entry.suffix in (".h", ".hpp", ".cpp", ".mm", ".m"):
                stamp = entry.stat().st_mtime
                if stamp > newest:
                    newest, newest_path = stamp, entry
    # The macos and swift arms render with the app, which also compiles its own
    # Swift, Objective-C++ and Metal. A link that fails leaves the previous
    # executable in place, and without these the arm renders with it and reads
    # every difference the branch made as a golden regression.
    if backend in ("macos", "swift"):
        for entry in _app_compiled_sources():
            stamp = entry.stat().st_mtime
            if stamp > newest:
                newest, newest_path = stamp, entry

    built = binary.stat().st_mtime
    if newest <= built:
        return
    raise RuntimeError(
        f"{backend} snapshot binary at {binary} is older than the portable Core "
        f"sources it renders with.\n"
        f"  newest source: {newest_path}\n"
        f"  Rebuild it before reading any verdict from this run: a diff against a "
        f"renderer nobody is running reads as a golden regression, and a case that "
        f"has already been fixed reports as failing.")


def _app_compiled_sources():
    """The files the macOS app compiles beyond the portable Core, as CMake lists them.

    Read from the lists rather than from a directory walk, because a file the
    app does not compile would make the refusal unclearable: touched, it stays
    newer than any binary a rebuild can produce. Headers are left out for the
    same reason — one no source includes changes nothing a link rewrites — and
    so are the UI-test scenarios, which a separate build compiles.

    Returns:
        The listed sources that exist, as paths.
    """
    roots = SOURCE_FILES.roots if SOURCE_FILES else (ROOT,)
    listings = [root / relative for root in roots for relative in
                ("Source/Platform/macOS/CMakeLists.txt", "Scripts/cmake/AppleSharedSwift.cmake",
                 "Scripts/cmake/UbiquittyKit.cmake")]
    pattern = re.compile(r"Source/[\w/+.\-]+\.(?:swift|mm|m|cpp|metal)\b")
    found = set()
    for listing in listings:
        if listing.is_file():
            found.update(pattern.findall(listing.read_text(encoding="utf-8")))
    return [root / path for path in sorted(found) for root in roots
            if "/UITest/" not in path and (root / path).is_file()]


def _portable_core_paths():
    """The repo-relative directories the portable snapshot binary compiles from.

    Read from the same SSOT `_refuse_if_stale` measures mtimes against, so the
    two halves of this check agree about what the binary is built out of rather
    than each carrying its own idea.

    - Returns: A sorted list of repo-relative directory paths, empty when the
      SSOT is absent.
    """
    ssot = ROOT / "Scripts" / "portable-core-sources.txt"
    if not ssot.exists():
        return []
    directories = set()
    for line in ssot.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("Source/"):
            directories.add(os.path.dirname(line))
    return sorted(directories)


def _renders_the_same_sources(recorded, current, source_root=None):
    """Whether two commits hold identical portable-core sources.

    The stamp records a commit, and a commit moves for reasons that never reach
    this binary: a workflow edit, a script, a document. Comparing the identifier
    answers "was this built somewhere else", which is not the question the
    refusal asks -- it asks whether the binary renders code the tree does not
    contain, and that is a question about the sources.

    A commit this checkout does not have cannot be compared, so it stays
    refused: an unknown pre-image is exactly the case the identity check exists
    for, and guessing in the permissive direction there would restore the
    confident-false-verdict this whole function prevents.

    - Parameters:
      - recorded: The commit the binary's provenance stamp names.
      - current: The commit checked out now.
    - Returns: True only when both commits are present and no path the binary
      compiles from differs between them.
    """
    root = source_root or ROOT
    paths = _portable_core_paths()
    if SOURCE_FILES and root != ROOT:
        # The app/Kit stamp names its producing repository, whose gitlinks
        # bind the libraries. A changed library pin must not be treated as a
        # documentation-only commit in the consumer.
        paths = ["Source", "CMakeLists.txt"]
        paths.extend(str(owner.relative_to(root)) for owner in SOURCE_FILES.roots
                     if owner != root and owner.is_relative_to(root))
    if not paths:
        return False
    for commit in (recorded, current):
        known = subprocess.run(["git", "-C", str(root), "cat-file", "-e", commit + "^{commit}"],
                               capture_output=True, env=_roots.foreign_git_environment())
        if known.returncode != 0:
            return False
    diff = subprocess.run(
        ["git", "-C", str(root), "diff", "--quiet", recorded, current, "--"] + paths,
        capture_output=True, env=_roots.foreign_git_environment())
    return diff.returncode == 0


def _refuse_if_foreign(binary, backend):
    """Refuse a binary built from a different commit than the tree being judged.

    Freshness and identity are different questions, and only the first is an
    mtime comparison. A binary built from ANOTHER BRANCH is newer than the
    sources it does not match, so it passes every staleness test while rendering
    code this tree does not contain. The run then produces a detailed, confident,
    entirely false verdict: named cases, per-case pixel deltas, all describing a
    renderer nobody checked out. That is worse than refusing, because a reader
    believes it and goes looking for the bug in whatever the goldens last moved
    for.

    Read from the same stamp the CMake frontends write (Scripts/build-provenance.sh)
    rather than a second mechanism, so a build directory has one recorded identity
    and the tier cannot disagree with the build about what it holds.

    Only the commit is compared, not the worktree delta the stamp also carries.
    An uncommitted edit to a source this binary renders with already moves that
    source's mtime, so `_refuse_if_stale` covers it; extending the refusal to the
    delta would block the tier on any dirty tree, including a tree dirty only in
    files this binary never compiled.
    """
    for directory in binary.parents:
        stamp = directory / ".ubiquitty-build-provenance"
        if not stamp.is_file():
            continue
        try:
            recorded = stamp.read_text(encoding="utf-8").split()[0]
        except (OSError, IndexError):
            return  # an unreadable stamp makes no claim to contradict
        source_root = ROOT if backend == "portable" else SUBJECT_ROOT
        current = subprocess.run(
            ["git", "-C", str(source_root), "rev-parse", "HEAD"],
            capture_output=True, text=True, env=_roots.foreign_git_environment()).stdout.strip()
        if not current or not recorded or recorded == current:
            return
        if _renders_the_same_sources(recorded, current, source_root):
            # A different commit is not by itself a different renderer. The
            # claim this refusal makes is that the binary "renders code this
            # tree does not contain", and between these two commits it does
            # not: nothing under the portable-core SSOT moved. Refusing anyway
            # blocks the tier on any commit that touched only scripts or
            # workflows -- which is most of them, and leaves a red that no
            # rebuild clears until some unrelated C++ edit happens along.
            return
        raise RuntimeError(
            f"{backend} snapshot binary at {binary} was built from "
            f"{recorded[:9]}, and this tree is {current[:9]}.\n"
            f"  Rebuild it before reading any verdict from this run: it renders "
            f"code this tree does not contain, so every case it reports — "
            f"including the ones it reports as failing — describes a different "
            f"commit.")
        return
    # No stamp anywhere above the binary. Only the CMake frontend scripts write
    # one, so an arm configured by a bare cmake rather than through its script
    # arrives here. Said out loud rather than passed over:
    # an arm whose identity was never checked reads exactly like one that passed
    # the check, and that equivalence is the whole defect this function exists to
    # remove.
    print(f"note: {backend} arm carries no build stamp, so its commit was not "
          f"verified — only its mtime. A binary built from another branch is "
          f"newer than the sources it does not match.", file=sys.stderr)


# The compositor each arm is NAMED for. An arm that falls through to another
# implementation renders a correct-looking image and reports as the arm that was
# asked for, so this is asserted from the frame's own marker rather than assumed
# from the flag that was passed. The macOS arm needs it most: it builds cell
# instances through Core and composites in Metal, and a frontend copy of the
# instance geometry can sit unrendered by any arm while reading as covered.
#
# The BUILDER is reported, not asserted. It is a live property with an
# environment override, so both of its values are legitimate; what a reader
# needs is to know which one produced the image in front of them.
#
# Three of the four values here are "software", and that is the map earning its
# keep rather than a mistake. The linux arm snapshots through the CPU rasteriser
# and not the Vulkan pipeline its frontend ships; the web arm renders in wasm
# through the same Core rasteriser and not the canvas or WebGL compositors the
# page uses. Only macOS composites through the GPU pipeline it ships. An arm is
# named for a frontend, and a frontend is not one renderer.
EXPECTED_COMPOSITOR = {
    "portable": "software",
    "macos": "metal",
    "ios": "metal",
    "linux": "software",
    "web": "software",
}

# Which frame encode each macOS arm must report, keyed on the `--snapshot-backend`
# token rather than on the arm's identity: the token is what the app's snapshot
# command reads, and the encoder arm is a macOS arm — macOS golden, macOS waivers —
# whose token says swift.
#
# Both arms composite through Metal, so the compositor axis cannot separate them.
# Without this an arm that fell back to the other encode renders, compares against
# the same golden, and agrees with it — a default that flips under an arm whose
# environment request is overwritten reports its waivers as fixed by an arm that
# has stopped looking. The builder arm cannot fail that way because
# `expected_builder` asserts what it drew with.
#
# Absent on the other arms because their binaries do not report the axis; a missing
# encoder there is not evidence of anything. On these two it is a refusal, since
# an unattributable frame is exactly the state this exists to reject.
# Both macOS arms expect the swift encoder: the native encode is deleted, and
# the marker check's job is to refuse a frame from any path that is not
# the one shipping encode.
EXPECTED_ENCODER = {
    "ios": "swift",
    "macos": "swift",
    "swift": "swift",
}

# Arms whose absence of a marker has already been reported. An unwired arm is
# named once per run: repeating it per case trains the reader to skip it, which
# is the failure mode this whole check exists to remove.
_PATHLESS_ARMS_REPORTED = set()


def _check_render_path(backend, stdout, expected_builder=None, token=None):
    """Refuse a frame produced by a path other than the one the arm names.

    The marker is `ubiquitty-render-path: builder=… compositor=…`, written by
    the frontend that rendered. Its absence is reported rather than passed over:
    an arm that never emitted one reads exactly like an arm that emitted the
    right one, and that equivalence is what lets a fall-through pass.
    """
    marker = None
    for line in stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("ubiquitty-render-path:"):
            marker = line
    expected = EXPECTED_COMPOSITOR.get(backend)
    if marker is None:
        if expected is not None:
            raise RuntimeError(
                f"the {backend} arm emitted no ubiquitty-render-path marker, and "
                f"one is expected: the frame cannot be attributed to a renderer, "
                f"so a fall-through to another path would read as a pass.")
        if backend not in _PATHLESS_ARMS_REPORTED:
            _PATHLESS_ARMS_REPORTED.add(backend)
            print(f"note: the {backend} arm emits no ubiquitty-render-path marker, "
                  f"so which renderer produced its frames was not verified.",
                  file=sys.stderr)
        return
    if expected_builder is not None and f"builder={expected_builder}" not in marker:
        raise RuntimeError(
            f"the {backend} arm rendered through the wrong cell builder: expected "
            f"builder={expected_builder}, got \"{marker}\". Both builders draw "
            f"this case against one golden, so an arm that fell back to the other "
            f"agrees with it and reports a comparison it did not make.")
    encoder = EXPECTED_ENCODER.get(token or backend)
    if encoder is not None:
        if "encoder=" not in marker:
            raise RuntimeError(
                f"the {backend} arm emitted no encoder axis: \"{marker}\". Both "
                f"macOS arms composite through Metal, so without it a frame cannot "
                f"be attributed to an encode and an arm that fell back to the other "
                f"one agrees with the same golden.")
        if f"encoder={encoder}" not in marker:
            raise RuntimeError(
                f"the {backend} arm rendered through the wrong frame encode: "
                f"expected encoder={encoder}, got \"{marker}\". The image is a "
                f"valid frame from the encode this arm exists to differ from, so it "
                f"agrees with a golden it never tested.")
    if expected is not None and f"compositor={expected}" not in marker:
        raise RuntimeError(
            f"the {backend} arm rendered through the wrong path: expected "
            f"compositor={expected}, got \"{marker}\". The image is a valid "
            f"frame from a renderer this arm is not named for.")


# The faces a case may name that no platform ships: test fonts kept beside the
# goldens, each renamed from its upstream so it cannot shadow an installed
# family. CoreText arms take none of them; the Linux arm resolves families
# through fontconfig, which reads this directory beside the system's own.
FIXTURE_FONT_DIRECTORY = HERE / "fixtures" / "fonts"
_fixture_fontconfig = None


def fixture_fontconfig():
    """Returns a fontconfig file that adds the fixture fonts to the system's."""
    global _fixture_fontconfig
    if _fixture_fontconfig is None:
        directory = Path(tempfile.mkdtemp(prefix="ubiquitty-fontconfig-"))
        atexit.register(shutil.rmtree, directory, True)
        configuration = directory / "fonts.conf"
        configuration.write_text(
            '<?xml version="1.0"?>\n<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">\n<fontconfig>\n'
            '  <include ignore_missing="yes">/etc/fonts/fonts.conf</include>\n'
            f"  <dir>{FIXTURE_FONT_DIRECTORY}</dir>\n"
            f"  <cachedir>{directory / 'cache'}</cachedir>\n"
            "</fontconfig>\n")
        _fixture_fontconfig = str(configuration)
    return _fixture_fontconfig


def render_snapshot(binary, regression, output_path):
    if regression.case is not None:
        case = regression.case
        # A second-frame arm renders its frame on top of the retained scene: the
        # base feed paints frame one to a throwaway, then each --also-feed drives
        # one frame further without resetting, and the last --also-snapshot is
        # the frame this arm is held to. Without also_feeds the base is the frame,
        # so it snapshots straight to output_path.
        also_feeds = regression.also_feeds
        with tempfile.TemporaryDirectory(dir=HERE) as scratch:
            base_target = Path(scratch) / "frame1.png" if also_feeds else output_path
            arguments = [
                binary, "--snapshot", str(base_target),
                "--snapshot-backend", regression.snapshot_backend or regression.backend,
                "--columns", str(case["columns"]),
                "--rows", str(case["rows"]),
            ]
            config = Path(case["config"])
            arguments.extend(("--config", str(config if config.is_absolute() else snapshot_path(config, required=True))))
            if case.get("feed_bytes") is not None:
                feed_path = Path(scratch) / "feed.bin"
                feed_path.write_bytes(case["feed_bytes"])
                arguments.extend(("--feed-file", str(feed_path)))
            elif case.get("feed_file"):
                arguments.extend(("--feed-file", str(snapshot_path(case["feed_file"], required=True))))
            else:
                arguments.extend(("--feed", case["feed"]))
            # A case argument that names a file cannot be relative to the runner's
            # working directory, which is whatever the invoker had. {SNAPSHOTS}
            # anchors it to this directory the way feed_file and config already are.
            arguments.extend(
                str(snapshot_path(str(value)[len("{SNAPSHOTS}/"):], required=True))
                if str(value).startswith("{SNAPSHOTS}/") else str(value)
                for value in case.get("args", []))
            for depth, feed in enumerate(also_feeds or []):
                last = depth == len(also_feeds) - 1
                target = output_path if last else Path(scratch) / f"frame{depth + 2}.png"
                arguments.extend(("--also-feed", feed, "--also-snapshot", str(target)))
            environment = regression.environment
            if regression.backend == "linux":
                environment = {**(environment or {}), "FONTCONFIG_FILE": fixture_fontconfig()}
            stdout = run_app(arguments, output_path, environment,
                             timeout=APP_LAUNCH_SECONDS.get(regression.backend, 15))
            _check_render_path(regression.backend, stdout, regression.expected_builder,
                               token=regression.snapshot_backend or regression.backend)
            if expected := case.get("stdout_contains"):
                if expected.encode() not in stdout:
                    # The terminal's replies are the usual subject of a contract,
                    # and the line that holds them says what it answered instead.
                    replies = [line for line in stdout.decode(errors="replace").splitlines()
                               if line.startswith("snapshot: replies")]
                    raise RuntimeError(f"stdout does not contain required text: {expected}"
                                       + (f"; {replies[0]}" if replies else ""))
            if rejected := case.get("stdout_excludes"):
                if rejected.encode() in stdout:
                    raise RuntimeError(f"stdout contains forbidden text: {rejected}")
            return

    if regression.sequence is None:
        raise RuntimeError("snapshot probe has no sequence")
    with tempfile.NamedTemporaryFile(dir=HERE) as feed:
        feed.write(regression.sequence)
        feed.flush()
        arguments = [
            binary, "--snapshot", str(output_path),
            "--snapshot-backend", "portable",
            "--feed-file", feed.name,
            "--columns", "80", "--rows", "24",
            "--config", str(snapshot_path("configs/showcase.toml", required=True)),
        ]
        _check_render_path("portable", run_app(arguments, output_path))


# How long one render may take, by arm. The iOS arm's shim reinstalls the app
# and launches it through simctl for every render; on a loaded machine that
# alone outlasts the default, and a render cut off there fails as a timeout
# with nothing drawn.
APP_LAUNCH_SECONDS = {"ios": 90}


def run_app(arguments, expected_output=None, environment=None, timeout=15):
    child = None
    if environment:
        child = dict(os.environ)
        child.update(environment)
    result = subprocess.run(arguments, capture_output=True, timeout=timeout, env=child)
    # A diagnostic run asks the arm to print the numbers a frame is built from
    # (UBIQUITTY_METRICS_LOG); those lines are the point of the run, and a
    # successful exit would otherwise discard them with the rest of stderr.
    if os.environ.get("UBIQUITTY_METRICS_LOG") and result.stderr:
        sys.stderr.write(result.stderr.decode("utf-8", "replace"))
    if result.returncode:
        stderr = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"app exited {result.returncode}: {stderr}")
    if expected_output is not None and not expected_output.exists():
        raise RuntimeError("app exited successfully without writing a snapshot")
    return result.stdout


def run_reply(binary, regression):
    if regression.sequence is None:
        raise RuntimeError("reply probe has no sequence")
    with tempfile.NamedTemporaryFile(dir=HERE) as feed:
        feed.write(regression.sequence)
        feed.flush()
        stdout = run_app([binary, "--probe-reply", "--feed-file", feed.name])
    for line in stdout.splitlines():
        if line.startswith(b"REPLY-HEX"):
            encoded = line[len(b"REPLY-HEX"):].strip()
            return bytes.fromhex(encoded.decode("ascii")) if encoded else b""
    raise RuntimeError("app output has no REPLY-HEX line")


def read_png_rgb(path):
    """Decodes a PNG to packed RGB, the alpha dropped.

    The comparison reads read_png_rgba; this stays for the harness's own
    round-trip and for any caller that wants the colour alone."""
    width, height, rgba = read_png_rgba(path)
    rgb = bytearray()
    for offset in range(0, len(rgba), 4):
        rgb.extend(rgba[offset:offset + 3])
    return width, height, bytes(rgb)


def read_png_rgba(path):
    """Decodes a PNG to packed RGBA; an RGB file reads as opaque.

    The alpha is compared, not dropped: a translucent frame's premultiplied
    colour can be right while its alpha is wrong — an opaque clear under a
    background at 0.6 lands the same RGB as the translucent frame at alpha 1
    — and a comparison over the colour alone cannot see that."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    position = 8
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    while position < len(data):
        if position + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        length = struct.unpack(">I", data[position:position + 4])[0]
        chunk_type = data[position + 4:position + 8]
        chunk = data[position + 8:position + 8 + length]
        position += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", chunk)
        elif chunk_type == b"IDAT":
            compressed.extend(chunk)
        elif chunk_type == b"IEND":
            break

    if (width is None or height is None or bit_depth != 8 or
            color_type not in (2, 6) or interlace != 0):
        raise ValueError("unsupported PNG format")
    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    expected_length = height * (stride + 1)
    if len(raw) != expected_length:
        raise ValueError("unexpected decompressed PNG length")

    rgba = bytearray()
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        filter_type = raw[position]
        position += 1
        row = bytearray(raw[position:position + stride])
        position += stride
        unfilter(row, previous, filter_type, channels)
        for column in range(width):
            offset = column * channels
            rgba.extend(row[offset:offset + 3])
            rgba.append(row[offset + 3] if channels == 4 else 255)
        previous = row
    return width, height, bytes(rgba)


def unfilter(row, previous, filter_type, bytes_per_pixel):
    if filter_type == 0:
        return
    if filter_type not in (1, 2, 3, 4):
        raise ValueError(f"unsupported PNG filter {filter_type}")
    for index, value in enumerate(row):
        left = row[index - bytes_per_pixel] if index >= bytes_per_pixel else 0
        above = previous[index]
        upper_left = (previous[index - bytes_per_pixel]
                      if index >= bytes_per_pixel else 0)
        if filter_type == 1:
            predictor = left
        elif filter_type == 2:
            predictor = above
        elif filter_type == 3:
            predictor = (left + above) // 2
        else:
            predictor = paeth(left, above, upper_left)
        row[index] = (value + predictor) & 0xFF


def paeth(left, above, upper_left):
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def is_strict(regression):
    """Whether this case spends no repaint budget.

    Read from the case rather than the golden because it is a property of what
    the case is asked to hold, not of the pixels it happened to record.
    """
    return bool((regression.case or {}).get("strict"))


def compare_png(actual_path, golden_path, strict=False):
    # Identical bytes are identical pixels, and decoding them to discover that
    # is what this tier spends its time on: read_png_rgb unfilters row by row in
    # Python, so a 652x438 pair is about 1.7 million interpreted iterations, and
    # the arm renders 260 cases. A passing case against an unchanged renderer
    # produces the same file the golden holds, so the comparison it needs is a
    # byte one.
    #
    # Sound in the direction that matters and not the other: different bytes can
    # still be identical pixels — an encoder difference does that, and web
    # against linux is one — so a mismatch here decides nothing and falls
    # through to the pixel comparison below.
    if actual_path.read_bytes() == golden_path.read_bytes():
        return True, "identical", None, 0

    actual_width, actual_height, actual = read_png_rgba(actual_path)
    golden_width, golden_height, golden = read_png_rgba(golden_path)
    if (actual_width, actual_height) != (golden_width, golden_height):
        # No count is possible across two shapes, and None says so rather than
        # standing in with a zero a budget would read as an improvement.
        return False, (f"size {(actual_width, actual_height)} != golden "
                       f"{(golden_width, golden_height)}"), None, None

    differing = 0
    repainted = 0
    worst = 0
    first_difference = None
    heatmap = bytearray(actual_width * actual_height * 3)
    for offset in range(0, len(actual), 4):
        differences = [abs(actual[offset + channel] - golden[offset + channel])
                       for channel in range(4)]
        pixel_worst = max(differences)
        worst = max(worst, pixel_worst)
        if pixel_worst > CHANNEL_TOLERANCE:
            differing += 1
            if first_difference is None:
                pixel = offset // 4
                first_difference = (
                    f"; first differing pixel ({pixel % actual_width},{pixel // actual_width}) "
                    f"expected RGBA {tuple(golden[offset:offset + 4])}, "
                    f"actual RGBA {tuple(actual[offset:offset + 4])}"
                )
        if pixel_worst > REPAINT_DELTA:
            repainted += 1
        # The heat map stays RGB; an alpha difference heats every channel so
        # it cannot hide behind a matching colour.
        heat = offset // 4 * 3
        for channel in range(3):
            heatmap[heat + channel] = min(255, max(differences[channel], differences[3]) * 8)

    total = actual_width * actual_height
    fraction = differing / total
    repaint_budget = STRICT_MAX_REPAINTED_PIXELS if strict else MAX_REPAINTED_PIXELS
    if repainted > repaint_budget:
        return False, (f"{repainted}px repainted (delta > {REPAINT_DELTA}), "
                       f"{differing}px differ ({fraction:.4%}), worst channel "
                       f"delta {worst}{first_difference or ''}"
                       + (f"; strict case, budget {repaint_budget}" if strict else "")
                       ), (actual_width, actual_height, heatmap), differing
    if fraction <= MAX_DIFFERING_FRACTION:
        # "identical" is every channel of every pixel, not a count that the
        # tolerance rounded to zero: the unifont tier reads this word.
        detail = "identical" if worst == 0 else (
            f"{differing}px differ ({fraction:.4%}), worst channel delta "
            f"{worst}; within tolerance")
        return True, detail, None, differing
    return False, (f"{differing}px differ ({fraction:.4%}), worst channel "
                   f"delta {worst}{first_difference or ''}"), (actual_width, actual_height, heatmap), differing


def write_rgb_png(path, width, height, pixels):
    scanlines = bytearray()
    stride = width * 3
    for row in range(height):
        scanlines.append(0)
        start = row * stride
        scanlines.extend(pixels[start:start + stride])

    def chunk(kind, payload):
        body = kind + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
        chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def save_failure(regression, actual_path, heatmap):
    FAILURE_ROOT.mkdir(parents=True, exist_ok=True)
    safe_name = regression.identifier.replace(":", "-")
    actual_destination = FAILURE_ROOT / f"{safe_name}.actual.png"
    shutil.copyfile(actual_path, actual_destination)
    if heatmap is not None:
        width, height, pixels = heatmap
        write_rgb_png(FAILURE_ROOT / f"{safe_name}.diff.png",
                      width, height, pixels)
    return actual_destination


def matches_filter(regression, filters):
    if not filters:
        return True
    aliases = {regression.identifier, regression.title}
    if regression.golden is not None:
        aliases.add(regression.golden.stem)
    return any(value in aliases for value in filters)


def write_known_budgets(measured):
    """Records what each waived case measured, as the ceiling excusing it.

    Rewrites only the ceiling token on the lines whose identifier this run
    measured, so an arm that renders a hundred cases cannot touch the entries
    of an arm it did not run — the Linux entries are minted on Linux and a Mac
    must leave them alone.

    - Parameter measured: Identifier -> differing pixels this run.
    - Returns: How many entries were written.
    """
    written = 0
    for path in manifest_paths("known-failing-*.txt"):
        lines = path.read_text().splitlines(keepends=True)
        changed = False
        for index, line in enumerate(lines):
            text = line.strip()
            if not text or text.startswith("#"):
                continue
            parts = text.split(None, 1)
            if len(parts) != 2 or parts[0] not in measured:
                continue
            identifier, reason = parts[0], parts[1].strip()
            existing = KNOWN_BUDGET.match(reason)
            if existing:
                reason = existing.group(2).strip()
            ending = "\n" if line.endswith("\n") else ""
            lines[index] = f"{identifier}  [max {measured[identifier]}px] {reason}{ending}"
            changed = True
            written += 1
        if changed:
            path.write_text("".join(lines))
    return written


def failure_label(identifier, known):
    """`KNOWN` for a case the manifest already lists, `FAIL` for one it does not.

    A case that has never matched prints the same word as one that just broke,
    so a reader using these goldens as an oracle sees six failures and takes
    them for live ones. That cost a wrong claim in a merged pull request: an
    injected defect was credited with two failures that were already there.
    Splitting the label means a `FAIL` line always says something changed, and
    the set comparison below stays the verdict either way.
    """
    return "KNOWN" if identifier in known else "FAIL"


def print_result(status, regression, detail=None, quiet=False):
    if quiet and status in ("PASS", "UPDATE"):
        return
    suffix = f"  ({detail})" if detail else ""
    approval = approved_goldens().get(regression.golden.resolve()) if regression.golden else None
    if approval:
        suffix += f"  [approved {approval['on']}]"
    if status == "KNOWN" and regression.identifier in KNOWN_REASONS:
        suffix += f"  [known: {KNOWN_REASONS[regression.identifier]}]"
    print(f"  {status:<4} {regression.section} / {regression.title}{suffix}")


def load_approved(manifest=None):
    """The approved goldens, by absolute path.

    Each maps to the manifest fields that matter to a reader of a result line:
    the date and the note. Comment and blank lines are skipped, and a manifest
    that does not exist approves nothing.
    """
    approved = {}
    manifests = [manifest] if manifest is not None else manifest_paths("approved.tsv")
    for path in manifests:
        if not path.exists():
            continue
        for line in path.read_text(encoding="utf-8").split("\n"):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            fields = line.split("\t")
            while len(fields) < 4:
                fields.append("")
            relative = Path(fields[0])
            if SOURCE_FILES:
                if relative.parts[:2] != ("Tests", "Snapshots"):
                    raise ValueError(f"{path}: approved golden is outside the snapshot catalog: {relative}")
                golden = snapshot_path(Path(*relative.parts[2:]))
            else:
                golden = ROOT / relative
            approved[golden.resolve()] = {"sha256": fields[1], "on": fields[2], "note": fields[3]}
    return approved


_APPROVED = None


def approved_goldens():
    """`load_approved()`, read once per process."""
    global _APPROVED
    if _APPROVED is None:
        _APPROVED = load_approved()
    return _APPROVED


def approval_action(is_approved, byte_identical):
    """What an approval says about writing one case's golden.

    `None` when the golden is not approved, so `update_action` decides. `noop`
    when it is approved and the render is byte-identical: there is nothing to
    write and nothing to refuse. `refuse` otherwise — an approved golden is not
    re-minted by any flag, because the approval records that a person looked at
    this picture and said it is right, and a render that disagrees is a change
    in the renderer that the picture exists to catch.
    """
    if not is_approved:
        return None
    return "noop" if byte_identical else "refuse"


def update_action(golden_exists, passing, byte_identical, remint_passing):
    """What `--update` should do with one case's freshly rendered frame.

    `write` re-mints the golden, `noop` leaves a byte-identical one untouched,
    and `launder` is the refusal: the case already passes, but the render is
    not the bytes on disk, so rewriting the golden would fold that drift into
    the commit. That is the class that silently rebaselined five passing
    goldens (underline_colored, sgr_stack, sgr_underline_color, text_cjk_align,
    text_combining_marks) when a doubled-row `--update` swept the whole portable
    set: bulk --update launders any drift a case still passes over into whatever
    commit runs it.

    The identity that decides `noop` is byte identity, not the comparator's
    "identical" verdict — which it also returns for a render that differs from
    the golden by no more than the channel tolerance. Such a render still passes,
    but its bytes have drifted, so it is `launder`, not `noop`: `noop` is only
    for a golden there is genuinely nothing to write.

    A missing golden or an explicit `--remint-passing` always writes; a failing
    case always writes (the legitimate re-mint); a passing case never writes.
    """
    if byte_identical:
        return "noop"
    if not golden_exists or remint_passing:
        return "write"
    if not passing:
        return "write"
    return "launder"


def run_regressions(binaries, regressions, update, quiet=False, twice=False,
                    remint_passing=False, update_budgets=False, require_ceilings=False,
                    *, fail_fast=False):
    failed_identifiers = set()
    executed = []
    update_refusals = []
    # A waived case past the distance its entry was written for, a waived entry
    # carrying no distance at all, and what each waived case measured this run.
    overrun = []
    unbudgeted = []
    unmeasurable = []
    refused = []
    measured = {}
    # One manifest per run: every regression here shares a backend, and a mixed
    # set falls back to an empty manifest so nothing is mislabelled as known.
    backends = {regression.backend for regression in regressions}
    known = (known_failing_identifiers(next(iter(backends))) or set()) if len(backends) == 1 else set()
    # Set aside before anything runs: an exempt case is not compared on this
    # backend, so it is neither executed nor judged, and it is reported apart
    # from the tallies a verdict turns on. Its golden is another arm's to
    # mint, so an update naming it is refused rather than written from an
    # arm the tier does not compare.
    exempt = exempt_identifiers(next(iter(backends))) if len(backends) == 1 else set()
    exempted = [regression for regression in regressions if regression.identifier in exempt]
    regressions = [regression for regression in regressions if regression.identifier not in exempt]
    counts = {"PASS": 0, "FAIL": 0, "KNOWN": 0, "EXEMPT": len(exempted), "UPDATE": 0}
    for regression in exempted:
        print(f"  EXEMPT {regression.section} / {regression.title}"
              f"  [exempt: {EXEMPT_REASONS.get(regression.identifier, '')}]")
        if update:
            update_refusals.append(f"{regression.identifier} (exempt on this arm: nothing is compared, so nothing is updated)")
    temporary_parent = GOLDEN_ROOT if update else None
    # Under the goldens on an update and under this directory otherwise, never
    # the system's temporary one: the Linux arm's container sees only the
    # repository, and a frames case writes its held frame straight to the
    # actual path.
    with tempfile.TemporaryDirectory(
            prefix=".ubiquitty-regressions-", dir=temporary_parent or HERE) as directory:
        actual_root = Path(directory)
        for regression in regressions:
            # Only an executed case can recover from its waiver. Known failures
            # within their ceilings do not fail the gate and must not stop it.
            if fail_fast and executed:
                previous = executed[-1].identifier
                if (counts["FAIL"] or (previous in known and previous not in failed_identifiers)
                        or (require_ceilings and unbudgeted)):
                    break
            executed.append(regression)
            if regression.kind == "reply":
                try:
                    reply = run_reply(binaries[regression.backend], regression)
                    kind = expected_reply_kind(regression.expected_reply)
                    if kind == "empty":
                        matches = not reply
                        expectation = "empty reply"
                    else:
                        matches = re.search(
                            regression.expected_reply,
                            reply.decode("latin-1"),
                        ) is not None
                        expectation = regression.expected_reply
                    if matches:
                        counts["PASS"] += 1
                        print_result("PASS", regression, quiet=quiet)
                    else:
                        label = failure_label(regression.identifier, known)
                        counts[label] += 1
                        failed_identifiers.add(regression.identifier)
                        print_result(
                            label, regression,
                            f"expected {expectation!r}, got {reply!r}")
                except Exception as error:
                    counts[failure_label(regression.identifier, known)] += 1
                    failed_identifiers.add(regression.identifier)
                    print_result(failure_label(regression.identifier, known), regression, str(error))
                continue

            actual = actual_root / f"{regression.identifier.replace(':', '-')}.png"
            try:
                render_snapshot(binaries[regression.backend], regression, actual)
                if twice or update:
                    # Determinism guard: a second render must be byte-identical.
                    # A difference means the render depends on non-input state
                    # (SDK-gated CoreText font resolution, uninitialised memory,
                    # ordering) — the class of bug that silently poisons a golden.
                    #
                    # Always on for --update, whether or not --twice was asked
                    # for. Writing the reference is the moment the guard is worth
                    # the most: a comparison against a poisoned golden fails and
                    # gets looked at, while the golden itself is believed. A
                    # render that cannot reproduce itself has no reference to
                    # offer, and the second render costs one case.
                    again = actual_root / f"{regression.identifier.replace(':', '-')}.again.png"
                    render_snapshot(binaries[regression.backend], regression, again)
                    if actual.read_bytes() != again.read_bytes():
                        counts[failure_label(regression.identifier, known)] += 1
                        failed_identifiers.add(regression.identifier)
                        print_result(failure_label(regression.identifier, known), regression,
                                     "NONDETERMINISTIC: two consecutive renders differ"
                                     + (" — refusing to write a golden from a render "
                                        "that does not reproduce itself" if update else ""))
                        continue
                if update:
                    regression.golden.parent.mkdir(parents=True, exist_ok=True)
                    exists = regression.golden.exists()
                    byte_identical = exists and (
                        actual.read_bytes() == regression.golden.read_bytes())
                    if exists and not byte_identical:
                        passing, detail, _, _ = compare_png(
                            actual, regression.golden, strict=is_strict(regression))
                    else:
                        passing, detail = byte_identical, ("identical" if byte_identical else "missing")
                    approval = approved_goldens().get(regression.golden.resolve()) if regression.golden else None
                    if approval_action(approval is not None, byte_identical) == "refuse":
                        counts["FAIL"] += 1
                        failed_identifiers.add(regression.identifier)
                        update_refusals.append(regression.identifier)
                        print_result("FAIL", regression,
                                     f"REFUSED to re-mint an approved golden (checked by eye on "
                                     f"{approval['on']}); no flag writes it. The render is not "
                                     f"the approved bytes (comparator: {detail}), so the renderer "
                                     f"is what has to change. Compare: Scripts/compare-snapshot.py "
                                     f"{regression.golden} {actual}")
                        continue
                    action = update_action(exists, passing, byte_identical, remint_passing)
                    if action == "noop":
                        # Byte-identical: rewriting only bumps the mtime and
                        # would mislead a freshness guard reading it.
                        counts["PASS"] += 1
                        print_result("PASS", regression, "identical; golden already current", quiet=quiet)
                        continue
                    if action == "launder":
                        counts["FAIL"] += 1
                        failed_identifiers.add(regression.identifier)
                        update_refusals.append(regression.identifier)
                        print_result("FAIL", regression,
                                     f"REFUSED to launder a passing golden ({detail}); --update "
                                     f"rewrites only failing or missing goldens. Review the drift "
                                     f"(Scripts/compare-snapshot.py {regression.golden} <render>), "
                                     f"and pass --remint-passing to rebaseline it deliberately")
                        continue
                    os.replace(actual, regression.golden)
                    counts["UPDATE"] += 1
                    print_result("UPDATE", regression, quiet=quiet)
                    continue
                if not regression.golden.exists():
                    counts[failure_label(regression.identifier, known)] += 1
                    failed_identifiers.add(regression.identifier)
                    print_result(failure_label(regression.identifier, known), regression, "missing golden; run --update")
                    continue
                matches, detail, heatmap, differing = compare_png(
                    actual, regression.golden, strict=is_strict(regression))
                if matches and regression.exact and read_png_rgba(actual) != read_png_rgba(regression.golden):
                    # Within the comparator's tolerance and still not the
                    # golden: a difference the tolerance would absorb on a
                    # native tier is a real one here, because there is no
                    # antialiaser to explain it.
                    matches = False
                    detail = f"{detail}; the unifont tier is held to pixel identity and this render is not"
                if matches:
                    counts["PASS"] += 1
                    print_result("PASS", regression, detail, quiet=quiet)
                else:
                    label = failure_label(regression.identifier, known)
                    # A waived case is excused for the distance it was measured
                    # at, not for any distance. Past its ceiling it is a
                    # regression that happens to carry an excuse, so it reds
                    # under its own name rather than counting as KNOWN.
                    ceiling = KNOWN_BUDGETS.get(regression.identifier)
                    if (label == "KNOWN" and ceiling is not None
                            and differing is not None and differing > ceiling):
                        label = "FAIL"
                        detail = (f"{detail}; {differing}px is past the {ceiling}px this "
                                  f"entry was excused at, so it is worse than what is known")
                        overrun.append((regression.identifier, differing, ceiling))
                    counts[label] += 1
                    failed_identifiers.add(regression.identifier)
                    artifact = save_failure(regression, actual, heatmap)
                    print_result(label, regression, f"{detail}; wrote {artifact}")
                    if label == "KNOWN":
                        # Measured whether or not an entry already carries a
                        # ceiling: --update-budgets mints the first one from
                        # this and lowers a slack one to it, and both need the
                        # number even when the comparison stayed within bounds.
                        if differing is None:
                            # Two different shapes have no pixels in common to
                            # count, so there is no distance a ceiling could
                            # bound. Its own bucket, or it would read as an
                            # entry someone forgot to mint.
                            unmeasurable.append(regression.identifier)
                        else:
                            measured[regression.identifier] = differing
                            if ceiling is None:
                                unbudgeted.append(regression.identifier)
                    # The command that says what the difference is, offered where
                    # it is needed rather than written down somewhere. A
                    # hand-rolled pixel measurement is easy to get wrong — a
                    # predicate that counts every pixel as lit — and a correction
                    # kept as prose in a manifest goes unread until someone is
                    # already looking at the case it describes.
                    print(f"         Scripts/compare-snapshot.py "
                          f"{regression.golden} {artifact}")
            except Exception as error:
                label = failure_label(regression.identifier, known)
                counts[label] += 1
                failed_identifiers.add(regression.identifier)
                print_result(label, regression, str(error))
                # A waiver whose case never reached a comparison — the tool
                # refused the flag, the shim declined the setting — has no pixel
                # distance to bound, and a ceiling on it would be a number about
                # nothing. Counted apart so the report can say which of the two
                # kinds of silence each waiver is in.
                if label == "KNOWN":
                    refused.append(regression.identifier)

    print()
    print("=" * 72)
    unexecuted = regressions[len(executed):]
    print("SUMMARY  " + "  ".join(f"{key}={value}" for key, value in counts.items()) +
          f"  UNEXECUTED={len(unexecuted)}  (executed {len(executed)} of {len(regressions)})")
    for regression in unexecuted:
        print(f"UNEXECUTED  {regression.identifier}")

    # What this verdict covers. A green over one backend reads as the tier, and
    # a change to shared machinery — the comparator, the macOS lane, a golden
    # three arms share — can land on the strength of whichever arm happens to be
    # configured. The comparison policy, the manifests and the case list are
    # common to every arm, so a change to any of them is answered by none of the
    # arms that did not run.
    ran = {regression.backend for regression in executed}
    absent = sorted(backend for backend in ALL_BACKENDS if backend not in ran)
    if absent:
        print(f"         scope: {', '.join(sorted(ran))} only — this run says "
              f"nothing about {', '.join(absent)}")
    cases = json.loads((HERE / "cases.json").read_text(encoding="utf-8"))
    excluded = {backend: count for backend, count
                in agreement_exclusions(cases).items() if backend in ran}
    for backend in sorted(excluded):
        print(f"         {backend} catalog contains {excluded[backend]} agreement case(s) "
              f"outside the agreeing set, against its own golden; only executed cases are judged")
    if overrun:
        print(f"         {len(overrun)} waived case(s) are worse than the entry excusing "
              f"them, and red on their own account:")
        for identifier, differing, ceiling in sorted(overrun):
            print(f"           {identifier}  {differing}px against a {ceiling}px ceiling")
        print("         Fix it, or raise the number in the entry and say in the reason "
              "what made the picture worse.")
    if unbudgeted:
        print(f"         {len(unbudgeted)} waived case(s) carry no pixel ceiling, so nothing "
              f"reports them getting worse — run --update-budgets on an arm that can "
              f"render them")
        for identifier in sorted(unbudgeted):
            print(f"           {identifier}")
    if unmeasurable:
        print(f"         {len(unmeasurable)} waived case(s) differ in frame SIZE, so no ceiling "
              f"applies: there is no pixel the two shapes share to count")
    if refused:
        print(f"         {len(refused)} waived case(s) never reached a comparison, so no "
              f"ceiling applies: the arm refused the case rather than drawing it differently")
    # A ceiling far above what the case now measures excuses a re-regression all
    # the way back up to it. Reported rather than failed: the number moves a
    # little with a toolchain, and a gate that reds on an improvement is one
    # every contributor learns to work around.
    slack = {identifier: (value, KNOWN_BUDGETS[identifier])
             for identifier, value in measured.items()
             if identifier in KNOWN_BUDGETS and KNOWN_BUDGETS[identifier] > 0
             and value * 2 <= KNOWN_BUDGETS[identifier]}
    if slack:
        print(f"         {len(slack)} waived case(s) now measure less than half their "
              f"ceiling, which excuses a return to it — lower them with --update-budgets:")
        for identifier in sorted(slack):
            value, ceiling = slack[identifier]
            print(f"           {identifier}  {value}px against a {ceiling}px ceiling")
    if update_budgets:
        written = write_known_budgets(measured)
        print(f"         wrote {written} pixel ceiling(s) into the manifest(s)")
    status, verdict_reason = judge_against_known_failures(
        failed_identifiers, executed, overrun, backends=backends)
    # A rendering waiver cannot authorize a protected golden update.
    if update_refusals and status == 0:
        status = 1
        verdict_reason = "golden update refused: " + ", ".join(sorted(update_refusals))
    # A waiver for a difference nobody bounded is the state this whole mechanism
    # exists to end, so an arm that can measure one refuses to call the run
    # green while any remain. Off by default: an arm cannot mint the ceilings of
    # an arm it did not run, and the Linux entries are minted on Linux.
    if require_ceilings and unbudgeted and status == 0:
        status = 1
        verdict_reason = (f"{len(unbudgeted)} waived case(s) measured a pixel distance and "
                          f"name no ceiling: " + ", ".join(sorted(unbudgeted)))
    # The tallies are not the verdict. A run whose every case passes still fails
    # when the manifest excuses a case that no longer needs excusing, and that
    # reason goes to stderr — so stdout alone can read `PASS=35 FAIL=0 KNOWN=0` on
    # a run that exits 1. Anything capturing one stream sees a green suite.
    # Exactly one verdict line, on every path, so that a filter selecting for it
    # cannot come back empty. A pass printing tallies alone and a refusal
    # printing neither meant three output shapes for three outcomes, and the one
    # a reader is least likely to have filtered for is the one that says stop.
    if status != 0:
        # The offenders ride the verdict line itself. "see stderr" is a pointer,
        # and a pointer is worth nothing to a reader who kept only this line —
        # which every reader does, because this is the line that says stop.
        print(f"VERDICT  FAILED — {verdict_reason or 'see stderr'}")
    else:
        print("VERDICT  PASSED")
    print("=" * 72)
    return status


def known_failing_identifiers(backend):
    """The cases this backend has never matched, from the manifest beside this file.

    Absent manifest means every failure counts, which is the right default for a
    backend nobody has had to write one for.
    """
    path = snapshot_path(f"known-failing-{backend}.txt") if SOURCE_FILES else ROOT / "Tests" / "Snapshots" / f"known-failing-{backend}.txt"
    if not path.exists():
        return None
    return set(known_failing_entries(path))


def exempt_identifiers(backend):
    """The cases this backend does not compare, by decision rather than by defect.

    A comparison the tier cannot mean is not a failure owed. Colour emoji come
    from a fallback face on every arm and the bitmap reference has none, so
    the unifont tier asks those cases for something no correct arm can give;
    listed as known failures they sit beside the entries a fix would clear and
    train the reader to skip both. An exemption records the decision in
    `exempt-<backend>.txt`, in the reason-and-date grammar of a known failure
    and without a pixel ceiling, because nothing is measured: the case is not
    run on that backend at all, and a run reports it as EXEMPT. An absent file
    exempts nothing.

    A runner that lacks what a case needs names further exemptions, in the same
    grammar, in the file UBQ_SNAPSHOT_RUNNER_EXEMPTIONS names: Scripts/
    ci_batteries.py writes it for a hosted Mac, which has no SF Pro.
    """
    identifiers = set()
    path = snapshot_path(f"exempt-{backend}.txt") if SOURCE_FILES else ROOT / "Tests" / "Snapshots" / f"exempt-{backend}.txt"
    if path.exists():
        identifiers |= set(exempt_entries(path))
    runner = os.environ.get("UBQ_SNAPSHOT_RUNNER_EXEMPTIONS")
    if runner:
        identifiers |= set(exempt_entries(Path(runner)))
    return identifiers


EXEMPT_REASONS = {}


def exempt_entries(path):
    """The identifiers an exemption list names, each with its reason and date."""
    entries = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        parts = text.split(None, 1)
        if len(parts) != 2 or not parts[1].strip():
            raise ValueError(f"{path.name}:{number}: an exemption needs a reason and a date after its identifier")
        identifier, reason = parts[0], parts[1].strip()
        if KNOWN_BUDGET.match(reason):
            raise ValueError(f"{path.name}:{number}: an exemption measures nothing, so it carries no pixel ceiling")
        if not re.search(r"\(\d{4}-\d{2}-\d{2}\)\s*$", reason):
            raise ValueError(f"{path.name}:{number}: the reason ends with the date it was recorded, (YYYY-MM-DD)")
        entries.append(identifier)
        EXEMPT_REASONS[identifier] = reason
    return entries


KNOWN_REASONS = {}
KNOWN_BUDGETS = {}

# `[max 16212px]` at the head of a reason: how far apart this pair is allowed to
# be while it stays excused.
KNOWN_BUDGET = re.compile(r"^\[max (\d+)px\]\s*(.*)$", re.DOTALL)


def known_failing_entries(path):
    """The identifiers a manifest lists, each with the reason and date beside it.

    A line is `identifier  [max Npx] reason (YYYY-MM-DD)`. The reason is
    required: a bare identifier says a case is red and nothing about why or
    since when, which is how a list of known failures turns into a list nobody
    reads. The reasons are kept for the result lines, so a KNOWN case says what
    is known about it.

    The ceiling is what stops the entry excusing more than it was written for.
    The two rules around it — a case not listed reds, a listed case that passes
    reds — compare the failing SET, and a set cannot see a waived case getting
    WORSE. `search_highlight` was excused at 2324px; nothing in this file
    would have reported it at twenty thousand. So the number a case measured
    when it was excused rides the entry, and exceeding it reds as a regression
    even though the identifier is listed.

    Optional, because it cannot be minted from a machine that cannot run the
    arm: the Linux entries are minted on Linux and this is a Mac. An entry without
    one is reported and ratcheted by name, so the remainder shrinks and can
    never grow.
    """
    entries = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        parts = text.split(None, 1)
        if len(parts) != 2 or not parts[1].strip():
            raise ValueError(f"{path.name}:{number}: a known failure needs a reason and a date after its identifier")
        identifier, reason = parts[0], parts[1].strip()
        budget = KNOWN_BUDGET.match(reason)
        if budget:
            KNOWN_BUDGETS[identifier] = int(budget.group(1))
            reason = budget.group(2).strip()
            if not reason:
                raise ValueError(f"{path.name}:{number}: a ceiling is not a reason — say what is known too")
        if not re.search(r"\(\d{4}-\d{2}-\d{2}\)\s*$", reason):
            raise ValueError(f"{path.name}:{number}: the reason ends with the date it was recorded, (YYYY-MM-DD)")
        entries.append(identifier)
        KNOWN_REASONS[identifier] = reason
    return entries


def unmatched_manifest_entries(catalog_regressions):
    """Manifest lines naming no case in the catalogue, per backend.

    An entry that matches nothing fires neither rule: it is never in the failing
    set, so it is never a repaired case, and it can never be an unexpected one.
    A typo is therefore indistinguishable from a correct line until the case it
    names happens to exist, and a case that is renamed or deleted leaves its
    entry behind as an excuse for nothing.

    Checked against the whole catalogue rather than the filtered run, because a
    run narrowed to one case would otherwise report every other entry as dead.

    - Parameters:
      - catalogRegressions: Every regression the catalogue defines.
    - Returns: Sorted `(backend, identifier)` pairs the catalogue does not
      define, empty when every entry names a case.
    """
    known_identifiers = {regression.identifier for regression in catalog_regressions}
    unmatched = []
    for path in manifest_paths("known-failing-*.txt"):
        backend = path.stem[len("known-failing-"):]
        for entry in known_failing_entries(path):
            if entry not in known_identifiers:
                unmatched.append((backend, entry))
    for path in manifest_paths("exempt-*.txt"):
        backend = path.stem[len("exempt-"):]
        for entry in exempt_entries(path):
            if entry not in known_identifiers:
                unmatched.append((backend, entry))
    return sorted(unmatched)


def contradictory_manifest_entries():
    """Identifiers a backend lists both as a known failure and as exempt.

    The two lists answer opposite questions — one says the case is compared
    and red, the other that it is not compared at all — so one identifier in
    both is a decision nobody made. Reported by name so the reader deletes the
    line that is wrong rather than the one that is convenient.

    - Returns: Sorted `(backend, identifier)` pairs, empty when the lists agree.
    """
    contradictory = []
    for path in manifest_paths("exempt-*.txt"):
        backend = path.stem[len("exempt-"):]
        known = known_failing_identifiers(backend) or set()
        for entry in exempt_entries(path):
            if entry in known:
                contradictory.append((backend, entry))
    return sorted(contradictory)


def judge_against_known_failures(failed, regressions, overrun=(), *, backends=None):
    """Compare the failing SET to the manifest, not the count.

    Seventeen permanently-red cases mean a new regression is invisible, and a
    red entry nobody can act on trains people to stop reading it. Comparing sets
    keeps the tier able to detect change while it is unable to be green.

    A listed case that PASSES is also a failure. That is what stops the manifest
    rotting into an allowlist: a fix cannot leave a stale entry behind, because
    the stale entry is itself the thing that fails.

    And a listed case past its pixel ceiling is a failure, which the SET cannot
    see: the identifier is listed, so it is neither unexpected nor repaired, and
    the comparison below reports no change however much worse the picture got.
    `overrun` carries those, and one of them decides the verdict — otherwise the
    tallies read `FAIL=1` while the verdict reads PASSED, and a reader who
    filtered for the verdict is told nothing happened.

    - Returns: `(status, reason)`. The reason names the offenders and is what the
      verdict line carries, so a reader who filtered for the verdict has the
      answer on the line they kept rather than in a stream they dropped.
    """
    over = [f"WORSE THAN KNOWN {identifier} ({differing}px past {ceiling}px)"
            for identifier, differing, ceiling in sorted(overrun)]
    # The planned backend set owns waiver policy. An early exit from a mixed
    # run must not acquire a waiver merely because only one backend executed.
    if backends is None:
        backends = {regression.backend for regression in regressions}
    if len(backends) != 1:
        if failed or over:
            return 1, "; ".join(over) or f"{len(failed)} case(s) failed"
        return 0, ""
    known = known_failing_identifiers(next(iter(backends)))
    if known is None:
        if failed or over:
            return 1, "; ".join(over) or f"{len(failed)} case(s) failed"
        return 0, ""

    ran = {regression.identifier for regression in regressions}
    unexpected = sorted(failed - known)
    # Only cases this run actually exercised can be judged as fixed; a filtered
    # run must not report every case it did not touch as newly passing.
    repaired = sorted((known & ran) - failed)

    for identifier in unexpected:
        print(f"NEW FAILURE  {identifier}", file=sys.stderr)
    for identifier in repaired:
        print(f"NOW PASSING  {identifier} — delete it from "
              f"Tests/Snapshots/known-failing-*.txt", file=sys.stderr)

    if unexpected or repaired or over:
        print("", file=sys.stderr)
        print(f"snapshot: the failing set changed ({len(unexpected)} new, "
              f"{len(repaired)} fixed, {len(over)} worse than known)", file=sys.stderr)
        # Named, not counted. A verdict saying "1 fixed" sends the reader to
        # another stream to learn which, and a reader who filtered for the
        # verdict never arrives — the identifier has to travel ON the line.
        parts = [f"NEW {identifier}" for identifier in unexpected]
        parts += [f"NOW PASSING {identifier}" for identifier in repaired]
        parts += over
        return 1, "; ".join(parts)
    if failed:
        print(f"snapshot: {len(failed)} known-failing case(s), set unchanged — "
              f"see the docstring in run.py")
    return 0, ""


def _main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", help="consuming checkout whose snapshot products are tested")
    parser.add_argument("--core-source-root", help="explicit Core definitions/source checkout")
    parser.add_argument("--kit-source-root", help="explicit Kit native data/source checkout")
    parser.add_argument("filters", nargs="*", help="exact test ID, title, or golden name")
    parser.add_argument("--catalog", choices=("all", "cases", "showcase"),
                        default="all")
    parser.add_argument("--portable-binary",
                        help="path to the cross-platform ubiquitty-snapshot executable")
    parser.add_argument("--swift-binary",
                        help="binary for the swift backend (defaults to the macOS app binary)")
    parser.add_argument("--macos-binary",
                        help="path to the macOS Ubiquitty executable")
    parser.add_argument("--ios-binary",
                        help="the iOS simulator snapshot launcher")
    parser.add_argument("--web-binary",
                        help="the web arm's launcher (default Scripts/web-snapshot-shim.mjs)")
    parser.add_argument("--linux-binary",
                        help="the Linux frontend, ubiquitty-linux, as Scripts/build-linux-gtk.sh builds it")
    parser.add_argument("--backend", choices=("all", "portable", "macos", "ios", "linux", "web", "swift"),
                        default="all" if sys.platform == "darwin" else "portable",
                        help="renderer catalog to run")
    parser.add_argument("--update", action="store_true",
                        help="replace selected snapshot goldens")
    parser.add_argument("--update-missing", action="store_true",
                        help="create only missing snapshot goldens")
    parser.add_argument("--check-catalog", action="store_true",
                        help="validate discovery and baseline coverage without the app")
    parser.add_argument("--list", action="store_true", help="list discovered tests")
    parser.add_argument("--quiet", action="store_true",
                        help="print only failures and the summary")
    parser.add_argument("--fail-fast", action="store_true",
                        default=os.environ.get("UBQ_TEST_FAIL_FAST") == "1",
                        help="stop after the first gating failure or stale waiver recovery "
                             "(also enabled by UBQ_TEST_FAIL_FAST=1)")
    parser.add_argument("--twice", action="store_true",
                        help="render each snapshot twice and fail if the two renders "
                             "differ (determinism guard: catches toolchain/SDK-gated "
                             "font resolution flips before they land as a golden)")
    parser.add_argument("--update-budgets", action="store_true",
                        help="record what each waived case measured as the pixel ceiling "
                             "excusing it, for the arm(s) this run rendered")
    parser.add_argument("--require-ceilings", action="store_true",
                        help="fail when a waived case measured a pixel distance and its "
                             "entry names no ceiling to bound it")
    parser.add_argument("--remint-passing", action="store_true",
                        help="with --update, also rewrite goldens whose case already "
                             "passes within tolerance (deliberately rebaseline "
                             "sub-tolerance drift). Without it, --update refuses to "
                             "rewrite a passing golden and fails, so a bulk --update "
                             "cannot launder drift into a commit unnoticed.")
    args = parser.parse_args(argv)
    if args.update and args.update_missing:
        return fail("--update and --update-missing are mutually exclusive")
    if args.remint_passing and not args.update:
        return fail("--remint-passing only applies with --update")

    try:
        configure_sources(args.source_root, args.core_source_root, args.kit_source_root)
        if not args.list:
            selected_backends = ALL_BACKENDS if args.backend == "all" else (args.backend,)
            for backend in selected_backends:
                # A backend whose owner is absent must not look like an
                # unaudited arm and silently certify an incomplete catalog.
                snapshot_path(f"exempt-{backend}.txt", required=True)
        catalog_regressions = load_regressions(args.catalog)
    except (KeyError, OSError, TypeError, ValueError, RuntimeError,
            json.JSONDecodeError) as error:
        return fail(str(error))
    # Before anything runs: a manifest entry naming no case is an excuse for
    # nothing, and it stays silent for as long as the name is wrong.
    if args.catalog == "all":
        unmatched = unmatched_manifest_entries(catalog_regressions)
        for backend, entry in unmatched:
            print(f"known-failing-{backend}.txt names no such case: {entry}",
                  file=sys.stderr)
        if unmatched:
            return fail(f"{len(unmatched)} manifest entry/entries match no case; "
                        f"fix the name or delete the line")
        contradictory = contradictory_manifest_entries()
        for backend, entry in contradictory:
            print(f"known-failing-{backend}.txt and exempt-{backend}.txt both name: {entry}",
                  file=sys.stderr)
        if contradictory:
            return fail(f"{len(contradictory)} case(s) listed as both a known failure and exempt; "
                        f"delete the line that is wrong")

    regressions = [regression for regression in catalog_regressions
                   if matches_filter(regression, args.filters) and
                   (args.backend == "all" or regression.backend == args.backend)]
    if not regressions:
        return fail(f"no regressions match: {' '.join(args.filters)}")

    if args.list:
        for regression in regressions:
            print(f"{regression.identifier}\t{regression.kind}\t"
                  f"{regression.section} / {regression.title}")
        return 0

    validation_regressions = catalog_regressions if args.check_catalog else regressions
    missing, orphaned = baseline_problems(validation_regressions)
    if args.check_catalog:
        for path in missing:
            print(f"MISSING {display_path(path)}")
        for path in orphaned:
            print(f"ORPHAN {display_path(path)}")
        if missing or orphaned:
            return 1
        snapshots = sum(regression.kind == "snapshot"
                        for regression in validation_regressions)
        replies = sum(regression.kind == "reply"
                      for regression in validation_regressions)
        print(f"catalog: {snapshots} snapshots and {replies} reply checks; "
              "all baselines accounted for")
        return 0

    if args.update_missing:
        missing_set = set(missing)
        regressions = [regression for regression in regressions
                       if regression.golden is not None and
                       regression.golden.resolve() in missing_set]
        if not regressions:
            print("regressions: no missing baselines")
            return 0
    # A golden is orphaned when no case at all claims it, which is a question about the
    # catalog and not about the cases this run selected. Asked of the selection,
    # every golden belonging to a case the run skipped reads as orphaned — and a
    # backend that renders two of thirty cases then reports twenty-eight of them
    # for deletion.
    _, orphaned_in_catalog = baseline_problems(catalog_regressions)
    if orphaned_in_catalog and not args.filters:
        for path in orphaned_in_catalog:
            print(f"ORPHAN golden: {display_path(path)}", file=sys.stderr)
        return fail("remove orphaned goldens before running the suite")
    if missing and not (args.update or args.update_missing):
        for path in missing:
            print(f"MISSING golden: {display_path(path)}", file=sys.stderr)
        return fail("snapshot baselines are incomplete; use --update with explicit filters")

    # A golden written by two arms in one --update run keeps whichever wrote last.
    #
    # `matches_filter` accepts a bare case name through the golden's stem, and the
    # macOS arms share one golden because that is what the cell-builder and
    # encoder arms are: three renderers held to a single image, so a difference
    # between them is one of them drawing what the others do not. So
    # `--update search_highlight` resolves to three regressions writing one
    # path, and the file ends up holding whichever arm the loop reached last
    # rather than the one the caller meant.
    #
    # 67 of 286 goldens have more than one writer, so this is a quarter of the
    # suite rather than one case. A warning in the prose of a known-failing list
    # does not prevent it: someone re-minting a golden reads the runner's output,
    # not a manifest comment.
    #
    # Refusing rather than picking a winner, because there is no correct winner
    # to pick — the arms disagree by construction, and which of them should own
    # the image is the judgement the caller has to make. The identifier is the
    # spelling that makes it, and the message hands it over verbatim.
    update = args.update or args.update_missing
    if update:
        by_golden = {}
        for regression in regressions:
            if regression.golden is not None:
                by_golden.setdefault(regression.golden, []).append(regression)
        contested = {path: group for path, group in by_golden.items() if len(group) > 1}
        if contested:
            print("regressions: REFUSED — --update would write one golden from more than one arm",
                  file=sys.stderr)
            for path, group in sorted(contested.items()):
                print(f"  {path.name}", file=sys.stderr)
                for regression in group:
                    print(f"      {regression.identifier}", file=sys.stderr)
            print("  The last arm to run would win, silently, and the others' output would be",
                  file=sys.stderr)
            print("  discarded. Name the arm you mean, by identifier:", file=sys.stderr)
            example = next(iter(sorted(contested.items())))[1][0]
            print(f"      python3 Tests/Snapshots/run.py --update {example.identifier}", file=sys.stderr)
            return 2

    binaries = {}
    binary_arguments = {
        "portable": args.portable_binary,
        "macos": args.macos_binary,
        "ios": args.ios_binary,
        "linux": args.linux_binary,
        "web": args.web_binary,
        "swift": args.swift_binary or args.macos_binary,
    }
    try:
        for backend in {regression.backend for regression in regressions}:
            binaries[backend] = find_binary(binary_arguments[backend], backend)
    except (OSError, ValueError, RuntimeError) as error:
        return fail(str(error))
    # The iOS arm renders every case through Scripts/ios-snapshot-shim.sh, and
    # each invocation would otherwise boot the simulator and shut it down again.
    # The batch boots it once here and shuts down only what it booted; a device
    # somebody else had up stays up.
    bootedIOSHere = False
    if "ios" in binaries:
        boot = subprocess.run([str(repository_file("Scripts/ios-simulator.sh")), "--boot"],
                              capture_output=True, text=True)
        if boot.returncode:
            return fail(f"ios arm: no simulator for this checkout: {boot.stderr.strip()}")
        bootedIOSHere = boot.stdout.strip().endswith(" booted")
        os.environ["UBQ_IOS_KEEP_BOOTED"] = "1"
    try:
        return run_regressions(binaries, regressions, update, args.quiet, args.twice,
                               args.remint_passing, args.update_budgets,
                               args.require_ceilings, fail_fast=args.fail_fast)
    finally:
        if bootedIOSHere:
            subprocess.run([str(repository_file("Scripts/ios-simulator.sh")), "--shutdown"],
                           capture_output=True)


def main(argv=None):
    # The CLI may also be embedded by the harness tests. One invocation's
    # caller/data roots must never change a later invocation's subject.
    names = ("ROOT", "HERE", "SUBJECT_ROOT", "BUILD_ROOT", "GOLDEN_ROOT", "SHOWCASE",
             "NATIVE_PROFILE_ROOT", "SOURCE_FILES", "_APPROVED", "_CONFIG_DIRECTORY",
             "FIXTURE_FONT_DIRECTORY", "_fixture_fontconfig")
    previous = {name: globals()[name] for name in names}
    try:
        return _main(argv)
    finally:
        globals().update(previous)


if __name__ == "__main__":
    sys.exit(main())
