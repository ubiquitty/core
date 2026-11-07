#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import run
from sources import SnapshotSources


class SplitCatalogTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        self.core, self.kit, self.app = (self.base / name for name in ("core", "kit", "app"))
        for root, role in ((self.core, "core"), (self.kit, "kit"), (self.app, "app")):
            (root / "Scripts").mkdir(parents=True)
            (root / "Scripts" / "repository.txt").write_text(role + "\n")
            (root / "CMakeLists.txt").write_text("project(Fixture)\n")
            (root / "Tests" / "Snapshots").mkdir(parents=True)
        self.write(self.core, "known-failing-portable.txt", "")
        self.write(self.core, "exempt-portable.txt", "")
        for name in ("known-failing-macos.txt", "known-failing-swift.txt", "exempt-macos.txt", "exempt-swift.txt", "exempt-ios.txt"):
            self.write(self.kit, name, "")
        for name in ("known-failing-linux.txt", "known-failing-web.txt", "exempt-linux.txt", "exempt-web.txt"):
            self.write(self.app, name, "")
        self.write(self.app, "approved.tsv", "Tests/Snapshots/golden/native/coretext/example.png\tabc\t2026-11-07\tchecked\n")
        self.write(self.core, "cases.json", json.dumps([{"name": "example", "columns": 80, "rows": 24,
                                                       "feed": "hello", "native": "fixture"}]))
        self.write(self.core, "configs/showcase.toml", "[font]\nsize = 10\n")
        self.write(self.core, "configs/native/fixture.toml", '[font]\nfamily = "Fixture Face"\n')
        self.write(self.core, "golden/unifont/example.png", "unifont")
        self.write(self.core, "golden/portable/showcase/probe.png", "showcase")
        self.write(self.kit, "golden/native/coretext/example.png", "coretext")
        self.write(self.app, "golden/native/freetype/example.png", "freetype")
        showcase = self.core / "Tools" / "showcase" / "showcase.py"
        showcase.parent.mkdir(parents=True)
        showcase.write_text("import json\nprint(json.dumps([" + repr({"section": "fixture", "module": "probe",
            "title": "probe", "sequence": "hello", "golden": "probe", "sequence_encoding": "utf-8",
            "snapshot": True, "expect": None}) + "]))\n")

    def write(self, root, relative, text):
        path = root / "Tests" / "Snapshots" / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def roots(self):
        return run._roots.source_roots(self.app, self.core, self.kit, environment={})

    @contextlib.contextmanager
    def binding(self):
        names = ("ROOT", "HERE", "SUBJECT_ROOT", "BUILD_ROOT", "GOLDEN_ROOT", "SHOWCASE", "NATIVE_PROFILE_ROOT",
                 "SOURCE_FILES", "_APPROVED", "_CONFIG_DIRECTORY", "FIXTURE_FONT_DIRECTORY", "_fixture_fontconfig")
        with patch.multiple(run, **{name: getattr(run, name) for name in names}):
            run.configure_sources(self.app, self.core, self.kit)
            yield

    def test_three_gitless_owners_produce_the_complete_catalog_and_native_paths(self):
        with self.binding():
            catalog = run.load_regressions()
            self.assertEqual(len(catalog), 11)
            self.assertEqual(run.baseline_problems(catalog), ([], []))
            by_identifier = {item.identifier: item for item in catalog}
            self.assertEqual(by_identifier["case:example:coretext:swift"].golden,
                             self.kit / "Tests/Snapshots/golden/native/coretext/example.png")
            self.assertEqual(by_identifier["case:example:freetype:linux"].golden,
                             self.app / "Tests/Snapshots/golden/native/freetype/example.png")
            native = by_identifier["case:example:coretext:macos"]
            self.assertIn('family = "Fixture Face"', Path(native.case["config"]).read_text())
            self.assertIn(native.golden.resolve(), run.load_approved())
            self.assertEqual(len(run.manifest_paths("exempt-*.txt")), 6)

    def test_missing_native_golden_is_reported_at_its_owner_without_dropping_cases(self):
        golden = self.kit / "Tests/Snapshots/golden/native/coretext/example.png"
        golden.unlink()
        with self.binding():
            catalog = run.load_regressions()
            self.assertEqual(len(catalog), 11)
            self.assertEqual(run.baseline_problems(catalog), ([golden], []))

    def test_missing_policy_and_duplicate_assets_refuse_instead_of_changing_scope(self):
        policy = self.app / "Tests/Snapshots/known-failing-linux.txt"
        policy.unlink()
        with self.assertRaisesRegex(ValueError, "snapshot data is missing"):
            SnapshotSources(self.roots(), run._roots.repositories)
        policy.write_text("")
        self.write(self.core, "golden/native/coretext/example.png", "coretext")
        files = SnapshotSources(self.roots(), run._roots.repositories)
        with self.assertRaisesRegex(ValueError, "ambiguous owners"):
            files.path("golden/native/coretext/example.png", required=True)

    def test_data_escape_and_competing_case_registry_refuse(self):
        files = SnapshotSources(self.roots(), run._roots.repositories)
        outside = self.base / "outside"
        outside.write_text("outside")
        link = self.app / "Tests/Snapshots/escape"
        link.symlink_to(outside)
        for relative in ("../outside", "escape"):
            with self.subTest(relative=relative), self.assertRaisesRegex(ValueError, "confined"):
                files.path(relative, required=True)
        self.write(self.app, "cases.json", "[]")
        with self.assertRaisesRegex(ValueError, "definitions must belong to Core"):
            SnapshotSources(self.roots(), run._roots.repositories)

    def test_core_alone_requires_no_repository_above_it(self):
        roots = run._roots.source_roots(self.core, environment={})
        self.assertIsNone(roots.kit)
        files = SnapshotSources(roots, run._roots.repositories)
        self.assertEqual(files.roots, (self.core,))
        self.assertEqual(files.manifests("known-failing-*.txt"),
                         [self.core / "Tests/Snapshots/known-failing-portable.txt"])
        arguments = ["--source-root", str(self.core), "--backend", "portable", "--check-catalog"]
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(run.main(arguments), 0)
        arguments[arguments.index("portable")] = "all"
        errors = io.StringIO()
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(errors):
            self.assertNotEqual(run.main(arguments), 0)
        self.assertIn("snapshot data is missing", errors.getvalue())

    def test_official_cli_context_is_gitless_and_does_not_pollute_later_calls(self):
        output = io.StringIO()
        previous = run.SOURCE_FILES
        arguments = ["--source-root", str(self.app), "--core-source-root", str(self.core),
                     "--kit-source-root", str(self.kit), "--backend", "all", "--list"]
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(output):
            self.assertEqual(run.main(arguments), 0)
        self.assertEqual(len(output.getvalue().splitlines()), 11)
        self.assertIs(run.SOURCE_FILES, previous)

    def test_products_are_found_under_the_consumers_build_root(self):
        build = self.base / "app-build"
        binary = build / "linux/ubiquitty-linux"
        binary.parent.mkdir(parents=True)
        binary.write_text("fixture")
        binary.chmod(0o700)
        with patch.dict(os.environ, {"UBQ_BUILD_ROOT": str(build)}, clear=True), self.binding(), \
                patch.object(run, "_refuse_if_stale") as stale, patch.object(run, "_refuse_if_foreign") as foreign:
            self.assertEqual(run.SUBJECT_ROOT, self.app)
            self.assertEqual(run.ROOT, self.core)
            self.assertEqual(run.find_binary(None, "linux"), str(binary))
            stale.assert_called_once_with(binary, "linux")
            foreign.assert_called_once_with(binary, "linux")
            stale.reset_mock()
            foreign.reset_mock()
            with patch.dict(os.environ, {"UBIQUITTY_MACOS_BIN": str(binary)}):
                self.assertEqual(run.find_binary(None, "swift"), str(binary))
            stale.assert_called_once_with(binary, "swift")
            foreign.assert_called_once_with(binary, "swift")


if __name__ == "__main__":
    unittest.main()
