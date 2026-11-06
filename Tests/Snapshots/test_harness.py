#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run
import gen_matrix


class CatalogTests(unittest.TestCase):
    def test_every_showcase_entry_has_snapshot_and_baseline(self):
        probes = json.loads(subprocess.check_output(
            [sys.executable, str(run.SHOWCASE), "--probes"], text=True))
        regressions = run.load_regressions()
        snapshots = [regression for regression in regressions
                     if regression.kind == "snapshot"]
        replies = [regression for regression in regressions
                   if regression.kind == "reply"]
        focused_cases = json.loads(
            (run.HERE / "cases.json").read_text(encoding="utf-8"))

        # Every backend renders once; the Swift frame drive is the sole macOS
        # encoder. The per-backend count is written out rather than obtained by
        # calling the loader's own arm functions: an expectation computed from
        # the code it checks agrees with that code however wrong both are. A
        # backend that grows a second arm fails here until someone counts it,
        # which is the trade.
        # Every case renders once per arm in the unifont tier, and a case with
        # a native profile once more per arm of its two stacks: three CoreText
        # arms and one FreeType arm. Written out rather than obtained from the
        # loader's own tables: an expectation computed from the code it checks
        # agrees with that code however wrong both are.
        UNIFONT_ARMS = 6
        NATIVE_ARMS_PER_STACK = {"coretext": 3, "freetype": 1}
        focused_variants = sum(
            UNIFONT_ARMS + (sum(NATIVE_ARMS_PER_STACK[stack]
                                for stack in case.get("native_stacks", NATIVE_ARMS_PER_STACK))
                            if case.get("native") else 0)
            for case in focused_cases
        )
        # Each retained frame has one golden shared by the macOS entry points.
        focused_variants += sum(
            2 * len(case.get("frames", ())) for case in focused_cases)
        # A showcase entry whose frame shows nothing of its feature is a reply
        # check and takes no picture; every entry proves its feature one way
        # or the other.
        self.assertEqual(len(snapshots), sum(probe["snapshot"] for probe in probes) + focused_variants)
        self.assertTrue(all(probe["snapshot"] or probe["expect"] is not None for probe in probes))
        self.assertEqual(len(replies), sum(probe["expect"] is not None
                                           for probe in probes))
        self.assertTrue(all(probe["sequence"] is not None for probe in probes))
        self.assertTrue(all(probe["sequence_encoding"] in ("latin-1", "utf-8")
                            for probe in probes))
        self.assertTrue(all(probe["golden"] is not None for probe in probes))
        self.assertEqual(run.baseline_problems(regressions), ([], []))

    def test_showcase_contract_is_deterministic(self):
        command = [sys.executable, str(run.SHOWCASE), "--probes"]
        self.assertEqual(subprocess.check_output(command),
                         subprocess.check_output(command))

    def test_showcase_rejects_unknown_arguments(self):
        result = subprocess.run(
            [sys.executable, str(run.SHOWCASE), "--not-a-real-option"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("unrecognized arguments", result.stderr)

    def test_sequence_encoding_is_explicit(self):
        self.assertEqual(run.sequence_bytes("é", "utf-8"), b"\xc3\xa9")
        self.assertEqual(run.sequence_bytes("é", "latin-1"), b"\xe9")

    def test_focused_case_schema_rejects_unknown_fields(self):
        case = {
            "name": "example",
            "columns": 80,
            "rows": 24,
            "feed": "hello",
            "colums": 80,
        }
        with self.assertRaisesRegex(ValueError, "unknown fields: colums"):
            run.validate_case(case, 0, set())

    def test_focused_case_schema_rejects_a_face_in_settings(self):
        """A face is named through `native`, never through a settings block.

        The tier supplies the font: the unifont tier draws GNU Unifont, the
        native tier draws the profile. A settings block carrying a family would
        make both tiers of one case draw that face, and the unifont golden would
        stop being the arms' agreement on one bitmap face.
        """
        case = {
            "name": "example",
            "columns": 80,
            "rows": 24,
            "feed": "hello",
            "settings": {"font": {"family": "JetBrains Mono"}},
        }
        with self.assertRaisesRegex(ValueError, "names a face; use native"):
            run.validate_case(case, 0, set())

    def test_focused_case_schema_rejects_an_unknown_native_profile(self):
        case = {
            "name": "example",
            "columns": 80,
            "rows": 24,
            "feed": "hello",
            "native": "no_such_profile",
        }
        with self.assertRaisesRegex(ValueError, "names no profile"):
            run.validate_case(case, 0, set())

    def test_focused_case_schema_rejects_stacks_it_cannot_hold(self):
        base = {"name": "example", "columns": 80, "rows": 24, "feed": "hello"}
        with self.assertRaisesRegex(ValueError, "needs a native profile"):
            run.validate_case({**base, "native_stacks": ["coretext"]}, 0, set())
        for stacks in ([], ["coretext", "coretext"], ["directwrite"], "coretext"):
            with self.subTest(stacks=stacks), self.assertRaisesRegex(ValueError, "non-empty list of distinct stacks"):
                run.validate_case({**base, "native": "jetbrains_mono_10", "native_stacks": stacks}, 0, set())
        run.validate_case({**base, "native": "jetbrains_mono_10", "native_stacks": ["coretext"]}, 0, set())

    def test_a_case_held_to_one_stack_has_no_native_golden_on_the_other(self):
        """A face one platform ships is compared where it resolves and nowhere else.

        SF Pro resolves on macOS alone; on Linux it would fall back, and a
        FreeType golden would hold the case to a picture of another font.
        """
        cases = json.loads((run.HERE / "cases.json").read_text(encoding="utf-8"))
        held = next(entry for entry in cases if entry.get("native_stacks") == ["coretext"])
        native = [regression for regression in run.load_regressions()
                  if regression.title == held["name"] and regression.section.startswith("Native agreement")]
        self.assertEqual({regression.backend for regression in native}, set(run.CORETEXT_BACKENDS))
        self.assertEqual({regression.golden for regression in native},
                         {run.GOLDEN_ROOT / "native" / "coretext" / f"{held['name']}.png"})

    def test_focused_case_schema_rejects_a_structured_setting(self):
        case = {
            "name": "example",
            "columns": 80,
            "rows": 24,
            "feed": "hello",
            "settings": {"appearance": {"padding": [1, 2]}},
        }
        with self.assertRaisesRegex(ValueError, "string, number or boolean"):
            run.validate_case(case, 0, set())

    def test_loader_rejects_a_duplicate_json_key(self):
        """A repeated key is a malformed splice, and json.loads hides it.

        Two same-named keys collapse to the last, so a case that already carried
        `agree` and had a second `agree` spliced onto it parses to one clean-
        looking object — every schema check above runs on the survivor and never
        sees the duplicate. The loader parses through a hook that reads the pairs
        before one is dropped, so the malformed text fails at the door.
        """
        malformed = '{"name": "example", "agree": true, "agree": true}'
        with self.assertRaisesRegex(ValueError, "duplicate key: 'agree'"):
            json.loads(malformed, object_pairs_hook=run._reject_duplicate_keys)
        # A clean object still parses to the same dict json.loads would return.
        clean = '{"name": "example", "agree": true}'
        self.assertEqual(
            json.loads(clean, object_pairs_hook=run._reject_duplicate_keys),
            {"name": "example", "agree": True})

    def test_cases_json_file_has_no_duplicate_keys(self):
        """The catalog itself, parsed strictly. Guards the file, not just the hook.

        A test proving the hook fires is vacuous if nothing parses the real file
        through it. This does — a duplicate key spliced anywhere into cases.json
        fails here rather than being silently accepted and misclassified as a
        pixel drift when one arm's golden is minted from the survivor.
        """
        json.loads((Path(run.HERE) / "cases.json").read_text(encoding="utf-8"),
                   object_pairs_hook=run._reject_duplicate_keys)

    def test_a_case_expands_to_every_arm_in_the_unifont_tier_and_its_stacks_in_the_native_one(self):
        """Six unifont arms held to one golden; the native arms held per stack.

        Read from the case rather than written down beside it. A case with a
        native profile renders on every arm twice: once in the unifont tier,
        where all six answer to golden/unifont/<name>.png byte for byte, and
        once in its stack's native tier — CoreText for macos, swift and ios,
        FreeType for linux — each stack to one golden. A case without a profile
        has the unifont tier alone.
        """
        cases = json.loads((Path(run.HERE) / "cases.json").read_text(encoding="utf-8"))
        with_native = next(entry for entry in cases if entry.get("native"))
        without = next(entry for entry in cases if not entry.get("native") and "frames" not in entry)
        regressions = run.load_case_regressions()
        for case in (with_native, without):
            mine = [regression for regression in regressions if regression.title == case["name"]]
            unifont = [regression for regression in mine if ":unifont:" in regression.identifier]
            self.assertEqual({regression.backend for regression in unifont}, set(run.ALL_BACKENDS))
            self.assertEqual({regression.golden for regression in unifont},
                             {run.GOLDEN_ROOT / "unifont" / f"{case['name']}.png"})
            self.assertTrue(all(regression.exact for regression in unifont))
            coretext = [regression for regression in mine if ":coretext:" in regression.identifier]
            freetype = [regression for regression in mine if ":freetype:" in regression.identifier]
            if case.get("native"):
                self.assertEqual({regression.backend for regression in coretext}, set(run.CORETEXT_BACKENDS))
                self.assertEqual({regression.golden for regression in coretext},
                                 {run.GOLDEN_ROOT / "native" / "coretext" / f"{case['name']}.png"})
                self.assertEqual({regression.backend for regression in freetype}, set(run.FREETYPE_BACKENDS))
                self.assertFalse(any(regression.exact for regression in coretext + freetype))
            else:
                self.assertEqual(coretext, [])
                self.assertEqual(freetype, [])

    def test_a_materialised_config_carries_the_tier_font_and_the_case_settings(self):
        """The file an arm is launched with is the tier's [font], the harness baseline, and the case's sections."""
        case = {"name": "example_config", "settings": {"appearance": {"theme": "Noir Clair", "padding": 4},
                                                       "cursor": {"invert_text": True}}}
        path = run.materialised_config(case, "unifont", run.unifont_font_text())
        text = path.read_text(encoding="utf-8")
        self.assertIn('family = "GNU Unifont"', text)
        self.assertIn("size = 10", text)
        self.assertIn('[appearance]\ntext_contrast = false\ncolor_space = "srgb"\ntheme = "Noir Clair"\npadding = 4', text)
        # One [cursor] block carrying both, not two blocks: a section named by
        # the baseline and by the case has to merge, because a second [cursor]
        # header is a duplicate key the parser is entitled to reject.
        self.assertIn('[cursor]\nshape = "block"\ninvert_text = true', text)
        self.assertEqual(text.count("[cursor]"), 1)

    def test_a_case_overrides_the_harness_baseline_it_names(self):
        """A case naming a baseline key renders with its own value, not the baseline's.

        The baseline exists so no case inherits a shipped default it did not
        choose. That is only true if a case can still choose: a baseline the
        case cannot override is a value nothing in the catalogue can vary, and
        the preference would be untestable through this harness.
        """
        case = {"name": "example_override", "settings": {"cursor": {"shape": "bar"},
                                                       "appearance": {"text_contrast": True}}}
        path = run.materialised_config(case, "unifont", run.unifont_font_text())
        text = path.read_text(encoding="utf-8")
        self.assertIn('shape = "bar"', text)
        self.assertNotIn('shape = "block"', text)
        self.assertEqual(text.count("[cursor]"), 1)
        self.assertIn('text_contrast = true', text)
        self.assertNotIn('text_contrast = false', text)
        self.assertEqual(text.count("[appearance]"), 1)

    def test_the_harness_baseline_reaches_a_case_that_names_no_settings(self):
        """A case with no settings of its own still renders on the baseline."""
        path = run.materialised_config({"name": "example_bare"}, "unifont", run.unifont_font_text())
        text = path.read_text(encoding="utf-8")
        self.assertIn('[cursor]\nshape = "block"', text)

    def test_second_frame_renders_native_entry_points_on_retained_state(self):
        """Both entry points accumulate feeds over the same retained surface."""
        name = "second_frame_overwrite"
        case = next(entry for entry in json.loads(
            (Path(run.HERE) / "cases.json").read_text(encoding="utf-8"))
            if entry["name"] == name)
        frames = case["frames"]

        frame_regressions = [regression for regression in run.load_case_regressions()
                             if regression.also_feeds is not None
                             and regression.title.startswith(name + "_")]
        self.assertEqual(len(frame_regressions), 2 * len(frames))
        for depth, frame in enumerate(frames):
            arms = [regression for regression in frame_regressions
                    if regression.title == f"{name}_{frame['suffix']}"]
            self.assertEqual({arm.backend for arm in arms}, {"macos", "swift"})
            for arm in arms:
                self.assertEqual(arm.golden.name, f"{name}_{frame['suffix']}.png")
                self.assertEqual(arm.also_feeds,
                                 [step["feed"] for step in frames[:depth + 1]])

    def test_second_frame_schema_needs_a_native_profile(self):
        """The later frames render on the coretext macOS arm, so a frames case names a face."""
        case = {
            "name": "example",
            "columns": 20,
            "rows": 3,
            "feed": "hello",
            "frames": [{"suffix": "edit", "feed": "world"}],
        }
        with self.assertRaisesRegex(ValueError, "needs a native profile"):
            run.validate_case(case, 0, set())

    def test_second_frame_schema_rejects_duplicate_suffix(self):
        case = {
            "name": "example",
            "columns": 20,
            "rows": 3,
            "feed": "hello",
            "native": "jetbrains_mono_10",
            "frames": [{"suffix": "edit", "feed": "a"},
                       {"suffix": "edit", "feed": "b"}],
        }
        with self.assertRaisesRegex(ValueError, "duplicate suffix"):
            run.validate_case(case, 0, set())


class PNGTests(unittest.TestCase):
    def test_round_trip_and_comparison_threshold(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / "golden.png"
            within = root / "within.png"
            outside = root / "outside.png"
            run.write_rgb_png(golden, 2, 1, bytes((10, 20, 30, 40, 50, 60)))
            run.write_rgb_png(within, 2, 1, bytes((22, 20, 30, 40, 50, 60)))
            run.write_rgb_png(outside, 2, 1, bytes((23, 20, 30, 40, 50, 60)))

            self.assertEqual(run.read_png_rgb(golden),
                             (2, 1, bytes((10, 20, 30, 40, 50, 60))))
            self.assertTrue(run.compare_png(within, golden)[0])
            matches, detail, _, _ = run.compare_png(outside, golden)
            self.assertFalse(matches)
            self.assertIn(
                "first differing pixel (0,0) expected RGBA (10, 20, 30, 255), actual RGBA (23, 20, 30, 255)",
                detail,
            )

    def test_flattened_opacity_names_the_alpha_loss(self):
        golden = run.GOLDEN_ROOT / "unifont" / "background_opacity.png"
        width, height, rgba = run.read_png_rgba(golden)
        rgb = bytes(channel for offset in range(0, len(rgba), 4)
                    for channel in rgba[offset:offset + 3])
        with tempfile.TemporaryDirectory() as directory:
            actual = Path(directory) / "opaque.png"
            run.write_rgb_png(actual, width, height, rgb)
            matches, detail, _, differing = run.compare_png(actual, golden)
        self.assertFalse(matches)
        self.assertEqual(differing, 74792)
        self.assertIn("worst channel delta 102", detail)
        self.assertIn("first differing pixel (0,0) expected RGBA (16, 16, 20, 153), actual RGBA (16, 16, 20, 255)", detail)

    def test_strict_case_catches_a_shift_the_default_budget_allows(self):
        """A small subject moved one pixel is what `strict` exists to catch.

        The default rules are budgets — 0.1% of the image may differ, and 48
        pixels may move past REPAINT_DELTA. A prompt-mark bracket is a few dozen
        high-contrast pixels, so shifting it a whole pixel spends a fraction of
        both and reports "within tolerance". This builds that exact shape: a
        652x438 ground with one 6x14 bar of ink, and the same image with the bar
        one row lower. The default comparison must pass it — that is the blind
        spot, asserted rather than described — and a strict comparison must not.
        """
        width, height = 652, 438
        bar_left, bar_top, bar_width, bar_height = 40, 100, 6, 14

        def field(top):
            pixels = bytearray(width * height * 3)
            for row in range(top, top + bar_height):
                for column in range(bar_left, bar_left + bar_width):
                    offset = (row * width + column) * 3
                    pixels[offset:offset + 3] = b"\xff\xff\xff"
            return bytes(pixels)

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / "golden.png"
            shifted = root / "shifted.png"
            run.write_rgb_png(golden, width, height, field(bar_top))
            run.write_rgb_png(shifted, width, height, field(bar_top + 1))

            # Only the bar's first and last rows change: 2 * bar_width pixels,
            # each by the full 255. Under the area rule that is 0.004% of the
            # image, and under the magnitude rule 12 of the 48 allowed repaints.
            lenient, detail, _, _ = run.compare_png(shifted, golden)
            self.assertTrue(lenient, detail)
            self.assertIn("within tolerance", detail)

            strict, detail, _, _ = run.compare_png(shifted, golden, strict=True)
            self.assertFalse(strict, detail)
            self.assertIn("repainted", detail)
            self.assertIn("strict case", detail)

    def test_strict_case_still_allows_antialiasing_drift(self):
        """Strict spends no repaint budget, and that is all it changes.

        A rasteriser that moves edge pixels a little must not fail a strict
        case, or the flag would be a request to re-mint on every toolchain
        update rather than a geometry check. The separator is REPAINT_DELTA:
        this moves every pixel by just under it, over the whole image.
        """
        width, height = 64, 64
        ground = bytes(bytearray(b"\x40" * (width * height * 3)))
        drifted = bytes(bytearray(b"\x7f" * (width * height * 3)))  # +63, under 64
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / "golden.png"
            actual = root / "actual.png"
            run.write_rgb_png(golden, width, height, ground)
            run.write_rgb_png(actual, width, height, drifted)
            # Every pixel differs, so the AREA rule fails it either way; what is
            # asserted is that strictness did not add a second reason.
            _, lenient_detail, _, _ = run.compare_png(actual, golden)
            _, strict_detail, _, _ = run.compare_png(actual, golden, strict=True)
            self.assertNotIn("repainted", lenient_detail)
            self.assertNotIn("repainted", strict_detail)

    def test_dimension_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.png"
            second = root / "second.png"
            run.write_rgb_png(first, 1, 1, bytes((0, 0, 0)))
            run.write_rgb_png(second, 2, 1, bytes((0, 0, 0, 0, 0, 0)))
            matches, detail, _, _ = run.compare_png(first, second)
            self.assertFalse(matches)
            self.assertIn("size", detail)


class ExemptionTests(unittest.TestCase):
    """An exempt case is set aside, reported apart, and never mistaken for a fix."""

    def run_one(self, *, exempt, update=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "cases.json").write_text("[]")
            regression = run.Regression(
                identifier="case:exempt:unifont:testarm", catalog="cases", section="Unifont agreement",
                title="exempt", kind="snapshot", backend="testarm", expected_reply=None,
                golden=root / "missing.png")
            rendered = []

            def render(binary, selected, target):
                rendered.append(target)
                target.write_bytes(b"\x00")

            output, errors = io.StringIO(), io.StringIO()
            with patch.object(run, "HERE", root), patch.object(run, "approved_goldens", return_value={}), \
                    patch.object(run, "known_failing_identifiers", return_value=set()), \
                    patch.object(run, "exempt_identifiers", return_value={regression.identifier} if exempt else set()), \
                    patch.dict(run.EXEMPT_REASONS, {regression.identifier: "the tier cannot mean it (2026-09-18)"}), \
                    patch.object(run, "render_snapshot", side_effect=render), \
                    contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                status = run.run_regressions({"testarm": Path("unused")}, [regression], update)
            return status, output.getvalue(), errors.getvalue(), rendered

    def test_an_exempt_case_is_not_run_and_the_verdict_passes(self):
        status, output, errors, rendered = self.run_one(exempt=True)
        self.assertEqual(status, 0)
        self.assertEqual(rendered, [], "an exempt case renders nothing on this arm")
        self.assertIn("EXEMPT Unifont agreement / exempt", output)
        self.assertIn("EXEMPT=1", output)
        self.assertNotIn("NOW PASSING", errors)

    def test_the_same_case_unexempted_is_compared(self):
        status, output, errors, rendered = self.run_one(exempt=False)
        self.assertEqual(len(rendered), 1)
        self.assertIn("EXEMPT=0", output)
        self.assertEqual(status, 1, "a missing golden fails when the case is compared")

    def test_an_update_naming_an_exempt_case_is_refused(self):
        status, output, errors, rendered = self.run_one(exempt=True, update=True)
        self.assertEqual(status, 1)
        self.assertEqual(rendered, [])
        self.assertIn("golden update refused", errors + output)

    def test_an_exemption_line_carries_a_reason_and_a_date_and_no_ceiling(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "exempt-testarm.txt"
            path.write_text("# comment\ncase:a:unifont:testarm  the reason (2026-09-18)\n")
            self.assertEqual(run.exempt_entries(path), ["case:a:unifont:testarm"])
            path.write_text("case:a:unifont:testarm  [max 5px] a ceiling on nothing (2026-09-18)\n")
            with self.assertRaises(ValueError):
                run.exempt_entries(path)
            path.write_text("case:a:unifont:testarm  no date\n")
            with self.assertRaises(ValueError):
                run.exempt_entries(path)
            path.write_text("case:a:unifont:testarm\n")
            with self.assertRaises(ValueError):
                run.exempt_entries(path)

    def test_a_case_in_both_lists_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            snapshots = root / "Tests" / "Snapshots"
            snapshots.mkdir(parents=True)
            (snapshots / "known-failing-testarm.txt").write_text("case:a:unifont:testarm  red (2026-09-18)\n")
            (snapshots / "exempt-testarm.txt").write_text("case:a:unifont:testarm  not compared (2026-09-18)\n")
            with patch.object(run, "ROOT", root):
                self.assertEqual(run.contradictory_manifest_entries(), [("testarm", "case:a:unifont:testarm")])
            (snapshots / "exempt-testarm.txt").write_text("case:b:unifont:testarm  not compared (2026-09-18)\n")
            with patch.object(run, "ROOT", root):
                self.assertEqual(run.contradictory_manifest_entries(), [])


class RunVerdictTests(unittest.TestCase):
    """Every failing execution path reaches the named-set verdict."""

    def result(self, outcome, known=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "cases.json").write_text("[]")
            regression = run.Regression(
                identifier="case:verdict:testarm", catalog="cases", section="Section",
                title="verdict", kind="reply" if outcome.startswith("reply") else "snapshot",
                backend="testarm", expected_reply="^expected$", golden=root / "missing.png")
            rendered = []

            def render(binary, selected, target):
                rendered.append(target)
                target.write_bytes(bytes([len(rendered)]))

            output, errors = io.StringIO(), io.StringIO()
            with patch.object(run, "HERE", root), patch.object(run, "approved_goldens", return_value={}), \
                    patch.object(run, "known_failing_identifiers", return_value={regression.identifier} if known else set()), \
                    patch.object(run, "render_snapshot", side_effect=render), \
                    patch.object(run, "run_reply", return_value=b"expected" if outcome == "reply-pass" else b"wrong",
                                 side_effect=RuntimeError("reply failed") if outcome == "reply-error" else None), \
                    contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                status = run.run_regressions({"testarm": Path("unused")}, [regression], False,
                                             twice=outcome == "nondeterministic")
            return status, output.getvalue(), errors.getvalue()

    def test_reply_mismatch_and_exception_fail_the_verdict(self):
        for outcome in ("reply-mismatch", "reply-error"):
            with self.subTest(outcome=outcome):
                status, output, errors = self.result(outcome)
                self.assertEqual(status, 1, output + errors)
                self.assertIn("VERDICT  FAILED", output)
                self.assertIn("NEW case:verdict:testarm", output)

    def test_nondeterminism_and_missing_goldens_fail_the_verdict(self):
        for outcome in ("nondeterministic", "missing-golden"):
            with self.subTest(outcome=outcome):
                status, output, errors = self.result(outcome)
                self.assertEqual(status, 1, output + errors)
                self.assertIn("VERDICT  FAILED", output)
                self.assertIn("NEW case:verdict:testarm", output)

    def test_known_failures_stay_known_on_every_early_exit(self):
        for outcome in ("reply-mismatch", "reply-error", "nondeterministic", "missing-golden"):
            with self.subTest(outcome=outcome):
                status, output, errors = self.result(outcome, known=True)
                self.assertEqual(status, 0, output + errors)
                self.assertIn("KNOWN=1", output)
                self.assertNotIn("NOW PASSING", output + errors)

    def test_known_reply_recovery_still_fails_the_stale_waiver(self):
        status, output, errors = self.result("reply-pass", known=True)
        self.assertEqual(status, 1, output + errors)
        self.assertIn("NOW PASSING case:verdict:testarm", output)

    def test_unwaived_matching_reply_passes(self):
        status, output, errors = self.result("reply-pass")
        self.assertEqual(status, 0, output + errors)
        self.assertIn("VERDICT  PASSED", output)


class RunFailFastTests(unittest.TestCase):
    """The catalog stops on gating outcomes without rendering a real app."""

    def exercise(self, outcomes, *, known=(), fail_fast=True, backends=None,
                 budgets=None, approved=(), **options):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "cases.json").write_text("[]")
            regressions = []
            for index, outcome in enumerate(outcomes):
                backend = backends[index] if backends else "testarm"
                golden = root / f"case{index}.png"
                if outcome != "missing-golden":
                    golden.write_bytes(b"golden")
                regressions.append(run.Regression(
                    identifier=f"case:case{index}:{backend}", catalog="cases", section="Section",
                    title=f"case{index}", kind="reply" if outcome.startswith("reply") else "snapshot",
                    backend=backend, expected_reply="^expected$", golden=golden))
            indices = {regression.identifier: index for index, regression in enumerate(regressions)}
            calls = []

            def reply(binary, regression):
                calls.append(indices[regression.identifier])
                outcome = outcomes[calls[-1]]
                if outcome == "reply-error":
                    raise RuntimeError("reply failed")
                return b"expected" if outcome == "reply-pass" else b"wrong"

            def render(binary, regression, target):
                index = indices[regression.identifier]
                calls.append(index)
                outcome = outcomes[index]
                if outcome == "render-error":
                    raise RuntimeError("render failed")
                data = b"golden" if outcome == "identical" else b"actual"
                if outcome == "nondeterministic":
                    data += bytes([calls.count(index)])
                target.write_bytes(data)

            def compare(actual, golden, **kwargs):
                outcome = outcomes[int(golden.stem.removeprefix("case"))]
                return outcome in ("snapshot-pass", "identical"), "comparison", None, 5

            output, errors = io.StringIO(), io.StringIO()
            with patch.object(run, "HERE", root), patch.object(run, "GOLDEN_ROOT", root), \
                    patch.object(run, "approved_goldens", return_value={
                        regressions[index].golden.resolve(): {"on": "reviewed"} for index in approved}), \
                    patch.object(run, "known_failing_identifiers", return_value={
                        regressions[index].identifier for index in known}), \
                    patch.dict(run.KNOWN_BUDGETS, {
                        regressions[index].identifier: value for index, value in (budgets or {}).items()
                    }, clear=True), \
                    patch.object(run, "run_reply", side_effect=reply), \
                    patch.object(run, "render_snapshot", side_effect=render), \
                    patch.object(run, "compare_png", side_effect=compare), \
                    patch.object(run, "save_failure", side_effect=lambda regression, actual, heatmap: actual), \
                    patch.object(run, "write_known_budgets", return_value=1) as write_budgets, \
                    contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                status = run.run_regressions(
                    {regression.backend: Path("unused") for regression in regressions},
                    regressions, options.pop("update", False), quiet=True, fail_fast=fail_fast, **options)
            return {
                "status": status, "output": output.getvalue(), "errors": errors.getvalue(),
                "calls": calls, "regressions": regressions, "budget_calls": write_budgets.call_args_list,
                "goldens": [regression.golden.read_bytes() if regression.golden.exists() else None
                            for regression in regressions],
            }

    def test_each_unwaived_failure_stops_before_the_next_case(self):
        for outcome in ("reply-mismatch", "reply-error", "missing-golden", "render-error",
                        "nondeterministic", "snapshot-fail"):
            with self.subTest(outcome=outcome):
                result = self.exercise([outcome, "reply-pass"], twice=outcome == "nondeterministic")
                self.assertEqual(result["status"], 1, result["output"] + result["errors"])
                self.assertEqual(set(result["calls"]), {0})
                self.assertIn("UNEXECUTED=1", result["output"])
                self.assertIn("UNEXECUTED  case:case1:testarm", result["output"])
                self.assertIn("executed 1 of 2", result["output"])

    def test_known_failures_do_not_stop_the_catalog(self):
        for outcome in ("reply-mismatch", "reply-error", "missing-golden", "render-error",
                        "nondeterministic", "snapshot-fail"):
            with self.subTest(outcome=outcome):
                result = self.exercise([outcome, "reply-pass"], known={0},
                                       twice=outcome == "nondeterministic")
                self.assertEqual(result["status"], 0, result["output"] + result["errors"])
                self.assertEqual(set(result["calls"]), {0, 1})
                self.assertIn("UNEXECUTED=0", result["output"])

    def test_recovery_stops_but_unexecuted_known_cases_are_not_recovered(self):
        result = self.exercise(["reply-pass", "reply-pass"], known={0, 1})
        self.assertEqual(result["status"], 1)
        self.assertEqual(result["calls"], [0])
        self.assertIn("NOW PASSING case:case0:testarm", result["output"])
        self.assertNotIn("NOW PASSING case:case1:testarm", result["output"] + result["errors"])

    def test_failure_does_not_call_unexecuted_known_cases_recovered(self):
        result = self.exercise(["reply-mismatch", "reply-pass"], known={1})
        self.assertEqual(result["status"], 1)
        self.assertEqual(result["calls"], [0])
        self.assertNotIn("NOW PASSING", result["output"] + result["errors"])

    def test_local_aggregate_mode_still_exercises_the_whole_catalog(self):
        result = self.exercise(["reply-mismatch", "reply-pass"], fail_fast=False)
        self.assertEqual(result["status"], 1)
        self.assertEqual(result["calls"], [0, 1])
        self.assertIn("UNEXECUTED=0", result["output"])

    def test_all_passing_cases_and_a_last_case_failure_are_accounted(self):
        for final, status in (("reply-pass", 0), ("reply-mismatch", 1)):
            with self.subTest(final=final):
                result = self.exercise(["reply-pass", final])
                self.assertEqual(result["status"], status)
                self.assertEqual(result["calls"], [0, 1])
                self.assertIn("executed 2 of 2", result["output"])

    def test_mixed_backend_prefix_does_not_acquire_a_single_backend_waiver(self):
        result = self.exercise(["reply-mismatch", "reply-pass"], known={0},
                               backends=("firstarm", "secondarm"))
        self.assertEqual(result["status"], 1, result["output"] + result["errors"])
        self.assertEqual(result["calls"], [0])
        self.assertIn("KNOWN=0", result["output"])
        self.assertIn("scope: firstarm only", result["output"])

    def test_ceiling_overrun_and_required_missing_ceiling_stop(self):
        for options, reason in (({"budgets": {0: 4}}, "WORSE THAN KNOWN"),
                                ({"require_ceilings": True}, "name no ceiling")):
            with self.subTest(options=options):
                result = self.exercise(["snapshot-fail", "reply-pass"], known={0}, **options)
                self.assertEqual(result["status"], 1)
                self.assertEqual(result["calls"], [0])
                self.assertIn(reason, result["output"])

    def test_within_ceiling_continues_and_budget_updates_only_include_executed_cases(self):
        result = self.exercise(["snapshot-fail", "reply-mismatch", "snapshot-fail"],
                               known={0, 2}, budgets={0: 5}, update_budgets=True)
        self.assertEqual(result["status"], 1)
        self.assertEqual(result["calls"], [0, 1])
        self.assertEqual(len(result["budget_calls"]), 1)
        self.assertEqual(result["budget_calls"][0].args, ({"case:case0:testarm": 5},))

    def test_update_refusals_are_never_waived_and_leave_goldens_untouched(self):
        for options, reason in (({"approved": {0}, "remint_passing": True}, "approved golden"),
                                ({}, "launder a passing golden")):
            for known in (set(), {0}):
                with self.subTest(options=options, known=known):
                    result = self.exercise(["snapshot-pass", "snapshot-fail"], update=True,
                                           known=known, **options)
                    self.assertEqual(result["status"], 1, result["output"] + result["errors"])
                    self.assertEqual(result["calls"], [0, 0])
                    self.assertEqual(result["goldens"], [b"golden", b"golden"])
                    self.assertIn(reason, result["output"])

    def test_nondeterministic_update_preserves_goldens_and_normal_update_can_continue(self):
        result = self.exercise(["nondeterministic", "snapshot-fail"], update=True)
        self.assertEqual(result["status"], 1)
        self.assertEqual(result["calls"], [0, 0])
        self.assertEqual(result["goldens"], [b"golden", b"golden"])
        result = self.exercise(["identical", "snapshot-fail"], update=True, approved={0})
        self.assertEqual(result["status"], 0, result["output"] + result["errors"])
        self.assertEqual(result["calls"], [0, 0, 1, 1])
        self.assertEqual(result["goldens"], [b"golden", b"actual"])

    def test_cli_honors_ci_environment_and_explicit_flag(self):
        regression = run.Regression(
            identifier="case:cli:portable", catalog="cases", section="Section", title="cli",
            kind="reply", backend="portable", expected_reply="^expected$")
        for environment, arguments, expected in (({}, [], False),
                                                  ({"UBQ_TEST_FAIL_FAST": "0"}, [], False),
                                                  ({"UBQ_TEST_FAIL_FAST": "1"}, [], True),
                                                  ({}, ["--fail-fast"], True)):
            with self.subTest(environment=environment, arguments=arguments), \
                    patch.dict(run.os.environ, environment, clear=True), \
                    patch.object(run, "load_regressions", return_value=[regression]), \
                    patch.object(run, "baseline_problems", return_value=([], [])), \
                    patch.object(run, "find_binary", return_value=Path("unused")), \
                    patch.object(run, "run_regressions", return_value=0) as execute:
                self.assertEqual(run.main(["--catalog", "cases", "--backend", "portable"] + arguments), 0)
                self.assertEqual(execute.call_args.kwargs["fail_fast"], expected)


class KnownFailureCeilingTests(unittest.TestCase):
    """The ceiling on a waiver: parsed, enforced, minted, and in the verdict."""

    def waiver(self, body):
        """A manifest holding `body`, and the entries it parses to."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "known-failing-testarm.txt"
        path.write_text(body)
        run.KNOWN_BUDGETS.clear()
        run.KNOWN_REASONS.clear()
        return path, run.known_failing_entries(path)

    def test_a_ceiling_is_read_off_the_front_of_the_reason(self):
        path, entries = self.waiver(
            "case:a:unifont:testarm  [max 2324px] the dim tier is absent (2026-09-04)\n")
        self.assertEqual(entries, ["case:a:unifont:testarm"])
        self.assertEqual(run.KNOWN_BUDGETS["case:a:unifont:testarm"], 2324)
        # Stripped from the reason, so a result line reads as prose rather than
        # repeating a number the runner already prints beside it.
        self.assertEqual(run.KNOWN_REASONS["case:a:unifont:testarm"],
                         "the dim tier is absent (2026-09-04)")

    def test_an_entry_without_a_ceiling_carries_none(self):
        _, entries = self.waiver("case:b:unifont:testarm  no ceiling yet (2026-09-04)\n")
        self.assertEqual(entries, ["case:b:unifont:testarm"])
        self.assertNotIn("case:b:unifont:testarm", run.KNOWN_BUDGETS)

    def test_a_ceiling_is_not_a_reason(self):
        # The date rule already refuses a bare identifier. A bare ceiling would
        # otherwise satisfy the "two fields" split and say nothing about why.
        with self.assertRaises(ValueError):
            self.waiver("case:c:unifont:testarm  [max 10px]\n")

    def test_the_comparator_hands_back_the_differing_count(self):
        with tempfile.TemporaryDirectory() as temporary:
            golden = Path(temporary) / "golden.png"
            actual = Path(temporary) / "actual.png"
            run.write_rgb_png(golden, 4, 4, bytes(4 * 4 * 3))
            pixels = bytearray(4 * 4 * 3)
            pixels[0] = pixels[1] = pixels[2] = 255
            run.write_rgb_png(actual, 4, 4, bytes(pixels))
            _, _, _, differing = run.compare_png(actual, golden)
            self.assertEqual(differing, 1)
            # Identical files short-circuit on bytes and report no distance.
            _, _, _, same = run.compare_png(golden, golden)
            self.assertEqual(same, 0)

    def test_two_shapes_report_no_count_rather_than_zero(self):
        # Zero would read as an improvement to any ceiling, and there is no
        # pixel the two shapes share to count.
        with tempfile.TemporaryDirectory() as temporary:
            golden = Path(temporary) / "golden.png"
            actual = Path(temporary) / "actual.png"
            run.write_rgb_png(golden, 4, 4, bytes(4 * 4 * 3))
            run.write_rgb_png(actual, 5, 4, bytes(5 * 4 * 3))
            matches, _, _, differing = run.compare_png(actual, golden)
            self.assertFalse(matches)
            self.assertIsNone(differing)

    def regression(self, identifier):
        return run.Regression(
            identifier=identifier, catalog="cases", section="Section",
            title="case", kind="snapshot", backend="testarm", golden=None)

    def test_an_overrun_decides_the_verdict_the_set_calls_unchanged(self):
        # The hole this closes. A listed identifier is neither an unexpected
        # failure nor a repaired one, so the set comparison reports no change
        # however much worse the picture got.
        identifier = "case:a:unifont:testarm"
        regressions = [self.regression(identifier)]
        original = run.known_failing_identifiers
        try:
            run.known_failing_identifiers = lambda backend: {identifier}
            status, reason = run.judge_against_known_failures({identifier}, regressions)
            self.assertEqual(status, 0, "a listed failure at or under its ceiling is expected")
            status, reason = run.judge_against_known_failures(
                {identifier}, regressions, [(identifier, 20000, 2324)])
            self.assertEqual(status, 1)
            # On the verdict line itself, not in a stream a reader may have
            # filtered away.
            self.assertIn(identifier, reason)
            self.assertIn("20000px past 2324px", reason)
        finally:
            run.known_failing_identifiers = original

    def test_minting_rewrites_only_the_ceiling_and_only_what_it_measured(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        manifest = Path(directory.name) / "known-failing-testarm.txt"
        manifest.write_text(
            "# a comment\n"
            "case:a:unifont:testarm  [max 10px] the first reason (2026-09-04)\n"
            "case:b:unifont:testarm  the second reason (2026-09-05)\n"
            "case:c:unifont:testarm  an arm this run never touched (2026-09-06)\n")
        original_root = run.ROOT
        try:
            # write_known_budgets globs the manifests beside the snapshot tree,
            # so the test stands a tree of its own under it.
            snapshots = Path(directory.name) / "Tests" / "Snapshots"
            snapshots.mkdir(parents=True)
            (snapshots / manifest.name).write_text(manifest.read_text())
            run.ROOT = Path(directory.name)
            written = run.write_known_budgets({"case:a:unifont:testarm": 40,
                                               "case:b:unifont:testarm": 55})
            self.assertEqual(written, 2)
            lines = (snapshots / manifest.name).read_text().splitlines()
            self.assertEqual(lines[0], "# a comment")
            # An existing ceiling is replaced, not appended to.
            self.assertEqual(lines[1],
                             "case:a:unifont:testarm  [max 40px] the first reason (2026-09-04)")
            self.assertEqual(lines[2],
                             "case:b:unifont:testarm  [max 55px] the second reason (2026-09-05)")
            # An entry this run did not measure keeps its line exactly: an arm
            # cannot mint the ceilings of an arm it never ran.
            self.assertEqual(lines[3],
                             "case:c:unifont:testarm  an arm this run never touched (2026-09-06)")
        finally:
            run.ROOT = original_root


class MatrixTests(unittest.TestCase):
    def test_committed_feature_matrix_is_current(self):
        result = subprocess.run(
            [sys.executable, str(Path(gen_matrix.__file__)), "--check"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


class ApprovedGoldenTests(unittest.TestCase):
    """An approved golden is never written, and the manifest reads as intended."""

    def test_approval_action_truth_table(self):
        # Not approved: no opinion, update_action decides.
        self.assertIsNone(run.approval_action(False, False))
        self.assertIsNone(run.approval_action(False, True))
        # Approved and byte-identical: nothing to write, nothing to refuse.
        self.assertEqual(run.approval_action(True, True), "noop")
        # Approved and different: refused, whatever flag asked.
        self.assertEqual(run.approval_action(True, False), "refuse")

    def test_load_approved_skips_comments_and_resolves_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "approved.tsv"
            manifest.write_text(
                "# a comment\n"
                "\n"
                "Tests/Snapshots/golden/unifont/case.png\tabc123\t2026-09-04\tchecked\n"
                "Tests/Snapshots/golden/unifont/short.png\tdef456\n",
                encoding="utf-8")
            approved = run.load_approved(manifest)
        self.assertEqual(len(approved), 2)
        entry = approved[(run.ROOT / "Tests/Snapshots/golden/unifont/case.png").resolve()]
        self.assertEqual(entry, {"sha256": "abc123", "on": "2026-09-04", "note": "checked"})
        short = approved[(run.ROOT / "Tests/Snapshots/golden/unifont/short.png").resolve()]
        self.assertEqual(short["on"], "")

    def test_missing_manifest_approves_nothing(self):
        self.assertEqual(run.load_approved(Path("/nonexistent/approved.tsv")), {})


class KnownGapTests(unittest.TestCase):
    """A golden every holder is known unable to produce is not a missing baseline.

    The catalog check refuses a run whose goldens are incomplete, which is
    right for a golden an arm could mint and nobody did. A freetype golden for
    a case whose flags the Linux driver does not parse can never be minted,
    and a known-failing line already says so; counting it missing would make
    the manifest and the catalog check contradict each other forever.
    """

    def test_a_golden_held_only_by_known_arms_is_not_missing(self):
        with tempfile.TemporaryDirectory() as temporary:
            golden = Path(temporary) / "native" / "freetype" / "case.png"
            golden.parent.mkdir(parents=True)
            regression = run.Regression(
                identifier="case:case:freetype:linux", catalog="cases",
                section="Native agreement (freetype)", title="case",
                kind="snapshot", backend="linux", golden=golden)
            original = run.known_failing_identifiers
            try:
                run.known_failing_identifiers = lambda backend: set()
                missing, _ = run.baseline_problems([regression])
                self.assertEqual(missing, [golden.resolve()])
                run.known_failing_identifiers = lambda backend: {regression.identifier}
                missing, _ = run.baseline_problems([regression])
                self.assertEqual(missing, [])
                # One arm not known keeps the golden expected: the unifont tier
                # holds six arms, and five KNOWN lines leave the sixth to mint it.
                other = run.Regression(
                    identifier="case:case:unifont:portable", catalog="cases",
                    section="Unifont agreement", title="case", kind="snapshot",
                    backend="portable", golden=golden)
                run.known_failing_identifiers = lambda backend: (
                    {regression.identifier} if backend == "linux" else set())
                missing, _ = run.baseline_problems([regression, other])
                self.assertEqual(missing, [golden.resolve()])
            finally:
                run.known_failing_identifiers = original


class UpdateLaunderGateTests(unittest.TestCase):
    """--update must not rewrite a golden whose case already passes.

    A bulk `--update` renders every case and, before this gate, overwrote every
    golden unconditionally — so a golden that passed only within tolerance had
    its sub-tolerance drift silently folded into the commit. Five passing
    goldens were rebaselined that way during a doubled-row --update. The gate
    refuses (and fails) on a passing golden unless the render actually differs
    past tolerance; --remint-passing overrides the refusal and rewrites it.
    """

    def test_update_action_truth_table(self):
        # A missing golden is the legitimate first mint.
        self.assertEqual(run.update_action(False, False, False, False), "write")
        # A failing case is the legitimate re-mint (the 39 doubled-row goldens).
        self.assertEqual(run.update_action(True, False, False, False), "write")
        # A byte-identical golden needs no write — rewriting only bumps mtime.
        self.assertEqual(run.update_action(True, True, True, False), "noop")
        # A passing-but-drifted golden (bytes differ, still passes) is the
        # laundering the gate refuses.
        self.assertEqual(run.update_action(True, True, False, False), "launder")
        # --remint-passing overrides the refusal and rewrites the drifted golden.
        self.assertEqual(run.update_action(True, True, False, True), "write")

    def test_within_tolerance_drift_is_classified_as_launder(self):
        # Tie the decision to compare_png's real verdict: a render that passes
        # (within tolerance) but is not the bytes on disk must resolve to the
        # refusal, not a write — the identity that spares a write is byte
        # identity, which the comparator's "identical" verdict does not imply.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            golden = root / "golden.png"
            drifted = root / "drifted.png"
            identical = root / "identical.png"
            failing = root / "failing.png"
            run.write_rgb_png(golden, 2, 1, bytes((10, 20, 30, 40, 50, 60)))
            # delta 12 == CHANNEL_TOLERANCE: passes with zero differing pixels,
            # yet the bytes differ — the exact shape that reads "identical" but
            # would launder if written.
            run.write_rgb_png(drifted, 2, 1, bytes((22, 20, 30, 40, 50, 60)))
            run.write_rgb_png(identical, 2, 1, bytes((10, 20, 30, 40, 50, 60)))
            run.write_rgb_png(failing, 2, 1, bytes((90, 20, 30, 40, 50, 60)))

            def byte_identical(a, b):
                return a.read_bytes() == b.read_bytes()

            passing, _, _, _ = run.compare_png(drifted, golden)
            self.assertTrue(passing)
            self.assertFalse(byte_identical(drifted, golden))
            self.assertEqual(
                run.update_action(True, passing, byte_identical(drifted, golden), False),
                "launder")

            passing, _, _, _ = run.compare_png(identical, golden)
            self.assertTrue(passing)
            self.assertTrue(byte_identical(identical, golden))
            self.assertEqual(
                run.update_action(True, passing, byte_identical(identical, golden), False),
                "noop")

            passing, _, _, _ = run.compare_png(failing, golden)
            self.assertFalse(passing)
            self.assertEqual(
                run.update_action(True, passing, byte_identical(failing, golden), False),
                "write")


if __name__ == "__main__":
    unittest.main()
