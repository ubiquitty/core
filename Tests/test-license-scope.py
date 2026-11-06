#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises complete file coverage and preservation of upstream license scopes."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Scripts"))
import license_scope


class LicenseScopeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.rules = []
        self.manifest()

    def manifest(self):
        boundary = [{"paths": ["./", ".github/", "Audits/", "Docs/", "Resources/", "Source/", "Tests/"],
                     "license": license_scope.APACHE_LICENSE, "reason": "Fixture boundary."}]
        (self.root / license_scope.MANIFEST).write_text(
            json.dumps({"version": 1, "boundary": boundary, "rules": self.rules}))

    def write(self, name, data):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        return name

    def source(self, name, identifier=license_scope.APACHE_LICENSE):
        return self.write(name, "// Copyright (C) 2026 Test contributors\n"
                          f"// SPDX-License-Identifier: {identifier}\n\n#pragma once\n")

    def audit(self, paths):
        self.manifest()
        return license_scope.file_license_audit(self.root, paths)

    def scope(self, paths, mode="metadata", identifier=license_scope.APACHE_LICENSE, **extra):
        self.rules.append({"paths": paths, "mode": mode, "license": identifier,
                           "reason": "Fixture ownership boundary.", "copyright": ["Test contributors"], **extra})

    def test_first_party_missing_identifier_fails_without_owner_heuristic(self):
        name = self.write("Source/Core/New.h", "#pragma once\n")
        errors, _, counts = self.audit([name])
        self.assertEqual(len(errors), 2)
        self.assertEqual(counts, {"header": 1})

    def test_wrong_license_is_rejected_without_project_author_name(self):
        name = self.source("Source/Core/New.h", "MIT")
        errors, _, _ = self.audit([name])
        self.assertEqual(len(errors), 1)
        self.assertIn(license_scope.APACHE_LICENSE, errors[0])

    def test_correct_license_also_requires_copyright(self):
        name = self.write("Source/Core/New.h", f"// SPDX-License-Identifier: {license_scope.APACHE_LICENSE}\n")
        self.assertIn("copyright", self.audit([name])[0][0])

    def test_composite_expression_and_supported_comment_syntax(self):
        for index, (prefix, suffix, extension) in enumerate((("// ", "", ".h"), ("/* ", " */", ".h"), ("<!-- ", " -->", ".md"), (";; ", "", ".el"))):
            name = f"Source/Derived{index}{extension}"
            self.scope([name], "header", "Apache-2.0 AND Unicode-3.0")
            self.write(name, f"{prefix}SPDX-FileCopyrightText: 2026 Test contributors{suffix}\n"
                       f"{prefix}SPDX-License-Identifier: Apache-2.0 AND Unicode-3.0{suffix}\n")
            self.assertEqual(self.audit([name])[0], [])

    def test_duplicate_license_identifiers_fail(self):
        name = self.source("Source/Core/New.h")
        path = self.root / name
        path.write_text(path.read_text().replace("#pragma once", "// SPDX-License-Identifier: MIT\n#pragma once"))
        self.assertEqual(len(self.audit([name])[0]), 1)

    def test_identifier_in_string_literal_does_not_satisfy_header(self):
        name = self.write("Source/Core/New.h", 'const char* text = "// SPDX-License-Identifier: MIT";\n')
        self.assertEqual(len(self.audit([name])[0]), 2)

    def test_multiline_generated_templates_are_not_header_declarations(self):
        cases = (("generator.py", "#", 'TABLE_HEADER = """\n// SPDX-License-Identifier: Apache-2.0\n"""\n'),
                 ("generator.cpp", "//", 'const char* header = R"(\n// SPDX-License-Identifier: Apache-2.0\n)";\n'))
        for name, marker, body in cases:
            with self.subTest(name=name):
                self.write(name, f"{marker} Copyright (C) 2026 Test contributors\n"
                           f"{marker} SPDX-License-Identifier: {license_scope.APACHE_LICENSE}\n\n" + body)
                self.assertEqual(self.audit([name])[0], [])

    def test_xml_prolog_and_doctype_can_precede_header(self):
        name = self.write("Info.plist.in", '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist SYSTEM "test.dtd">\n'
                          '<!-- Copyright (C) 2026 Test contributors -->\n'
                          f'<!-- SPDX-License-Identifier: {license_scope.APACHE_LICENSE} -->\n<plist/>\n')
        self.assertEqual(self.audit([name])[0], [])

    def test_configured_templates_carry_the_header_of_their_format(self):
        for name in ("Source/Platform/Apple/module.modulemap.in", "Source/Version.h.in"):
            with self.subTest(name=name):
                self.source(name)
                self.assertEqual(self.audit([name])[0], [])

    def test_glsl_version_can_precede_header(self):
        name = self.source("shader.frag")
        path = self.root / name
        path.write_text("#version 450\n\n" + path.read_text())
        self.assertEqual(self.audit([name])[0], [])

    def test_notices_after_sixteen_lines_are_checked(self):
        name = self.source("Source/Core/New.h")
        path = self.root / name
        path.write_text("// Preamble\n" * 20 + path.read_text())
        self.assertEqual(self.audit([name])[0], [])

    def test_new_permissive_client_cannot_omit_header(self):
        self.rules.append({"patterns": ["Source/Platform/Neovim/*.lua"], "mode": "header",
                           "license": "Apache-2.0", "reason": "Interoperability client."})
        name = self.write("Source/Platform/Neovim/new.lua", "return {}\n")
        self.assertEqual(len(self.audit([name])[0]), 2)

    def test_path_outside_the_boundary_fails(self):
        name = self.source("Elsewhere/New.h")
        self.assertIn("outside the license boundary", self.audit([name])[0][0])

    def test_metadata_must_agree_with_the_boundary(self):
        name = self.write("Resources/data.bin", b"\0data")
        self.scope([name], identifier="MIT")
        self.assertIn("boundary license", self.audit([name])[0][0])

    def test_unknown_format_requires_explicit_metadata(self):
        name = self.write("Resources/new.dat", b"\0data")
        self.assertIn("explicit metadata", self.audit([name])[0][0])
        self.scope([name])
        self.assertEqual(self.audit([name])[0], [])

    def test_raw_fixture_spdx_looking_bytes_are_payload(self):
        name = self.source("Tests/input.stream", "MIT")
        self.scope([name], payload=True)
        original = (self.root / name).read_bytes()
        self.assertEqual(self.audit([name])[0], [])
        self.assertEqual((self.root / name).read_bytes(), original)

    def test_metadata_cannot_override_a_conflicting_text_header(self):
        name = self.write("Resources/example.toml", "# SPDX-License-Identifier: MIT\n")
        self.scope([name])
        self.assertIn("conflicts", self.audit([name])[0][0])

    def test_new_vendor_and_generated_files_never_inherit_project_scope(self):
        for name in ("Source/ThirdParty/new/library.h", "Resources/fonts/new.otf",
                     "Source/Core/Unicode/New.tables.inc", "Docs/third-party-licenses/New.md"):
            self.source(name)
            self.assertIn("explicit metadata", self.audit([name])[0][0])

    def test_upstream_source_cannot_add_the_project_license(self):
        name = self.source("Source/ThirdParty/library/source.h", license_scope.APACHE_LICENSE + " AND MIT")
        self.scope([name], "upstream", "MIT")
        self.assertIn("upstream license identifier", self.audit([name])[0][0])

    def test_upstream_source_preserves_its_per_file_identifier(self):
        name = self.source("Source/ThirdParty/library/source.h", "MIT")
        self.scope([name], "upstream", "MIT AND BSD-3-Clause", header_licenses=["MIT"])
        self.assertEqual(self.audit([name])[0], [])
        self.source(name, "Apache-2.0")
        self.assertIn("upstream license identifier", self.audit([name])[0][0])

    def test_brackets_in_exact_font_paths_are_literal(self):
        name = self.write("Resources/fonts/Face[wght].otf", b"\0font")
        self.scope([name], "upstream", "OFL-1.1")
        self.assertEqual(self.audit([name])[0], [])
        other = self.write("Resources/fonts/Facew.otf", b"\0other")
        self.assertTrue(self.audit([other])[0])

    def test_overlapping_rules_fail(self):
        name = self.write("Resources/data.json", "{}\n")
        self.scope([name])
        self.scope([name], identifier="MIT")
        self.assertIn("overlapping", self.audit([name])[0][0])

    def test_missing_or_external_notice_evidence_fails(self):
        self.scope(["Resources/font.otf"], "upstream", "OFL-1.1", license_files=["LICENSE-MISSING"])
        self.assertIn("missing license evidence", self.audit([])[0][0])
        self.rules[0]["license_files"] = ["../LICENSE"]
        self.assertTrue(any("non-repository" in error for error in self.audit([])[0]))

    def test_invalid_manifest_fails_closed(self):
        for rules in ([{"paths": "not-an-array", "mode": "metadata"}], [None]):
            self.rules = rules
            self.assertTrue(self.audit([])[0])
        (self.root / license_scope.MANIFEST).unlink()
        self.assertTrue(license_scope.file_license_audit(self.root, [])[0])

    def test_unresolved_provenance_is_not_reported_as_cleared(self):
        name = self.write("Resources/font.otf", b"\0font")
        self.scope([name], "upstream", "LicenseRef-Review", review="The upstream font grant needs verification.")
        errors, warnings, counts = self.audit([name])
        self.assertEqual(errors, [])
        self.assertEqual(len(warnings), 1)
        self.assertEqual(counts, {"upstream": 1})

    def test_git_corpus_includes_audits_and_new_files_and_preserves_path_bytes(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.write(".gitignore", "ignored*\n")
        self.write("Audits/tracked.md", "report\n")
        self.write("ignored-tracked", "tracked\n")
        subprocess.run(["git", "-C", str(self.root), "add", "-f", "Audits/tracked.md", "ignored-tracked"], check=True)
        self.write("ignored-build", "build\n")
        self.write("new file\nwith newline.h", "source\n")
        paths = license_scope.repository_paths(self.root)
        self.assertIn("Audits/tracked.md", paths)
        self.assertIn("ignored-tracked", paths)
        self.assertIn("new file\nwith newline.h", paths)
        self.assertNotIn("ignored-build", paths)

    def test_source_export_includes_root_and_hidden_files(self):
        self.write("LICENSE", "license\n")
        self.write(".github/workflow.yml", "steps: []\n")
        self.assertIn("LICENSE", license_scope.repository_paths(self.root))
        self.assertIn(".github/workflow.yml", license_scope.repository_paths(self.root))

    def test_checker_does_not_create_unshipped_bytecode_in_source_export(self):
        repository = Path(__file__).resolve().parents[1]
        scripts = self.root / "Scripts"
        scripts.mkdir()
        for name in ("check-licenses.py", "license_scope.py", "gate_corpus.py", "ubq_roots.py"):
            (scripts / name).write_bytes((repository / "Scripts" / name).read_bytes())
        subprocess.run([sys.executable, str(scripts / "check-licenses.py"), "--help"],
                       cwd=self.root, capture_output=True, check=True)
        self.assertFalse((scripts / "__pycache__").exists())

    def test_broken_git_metadata_cannot_be_an_empty_pass(self):
        self.write(".git", "gitdir: /nonexistent-license-fixture\n")
        with self.assertRaises(subprocess.CalledProcessError):
            license_scope.repository_paths(self.root)

    def test_unifont_generator_preserves_wrapper_and_font_grants(self):
        repository = Path(__file__).resolve().parents[1]
        script = self.root / "Scripts/gen-unifont.py"
        script.parent.mkdir()
        script.write_bytes((repository / "Scripts/gen-unifont.py").read_bytes())
        output = self.root / "Source/Core/Rendering"
        output.mkdir(parents=True)
        source = self.write("unifont_all-17.0.04.hex", "0041:" + "00" * 16 + "\n")
        subprocess.run([sys.executable, str(script), str(self.root / source)], check=True, capture_output=True)
        self.assertEqual(license_scope.header_declarations(output / "UnifontData.h")[0], ["Apache-2.0"])
        self.assertEqual(license_scope.header_declarations(output / "UnifontData.cpp")[0], ["Apache-2.0 AND OFL-1.1"])
        # The header names the release and the exact input it was baked from.
        digest = hashlib.sha256((self.root / source).read_bytes()).hexdigest()
        for name in ("UnifontData.h", "UnifontData.cpp"):
            text = (output / name).read_text(encoding="utf-8")
            self.assertIn("unifont_all-17.0.04.hex of GNU Unifont 17.0.04", text)
            self.assertIn(digest, text)
        # A file whose name carries no release is refused rather than baked unnamed.
        unnamed = self.write("glyphs.hex", "0041:" + "00" * 16 + "\n")
        refused = subprocess.run([sys.executable, str(script), str(self.root / unnamed)], capture_output=True)
        self.assertNotEqual(refused.returncode, 0)

    def test_unicode_generators_preserve_wrapper_and_data_grants(self):
        repository = Path(__file__).resolve().parents[1]
        empty = self.root / self.write("empty.txt", "")
        grapheme = self.root / self.write("grapheme.txt", "0300 ; Extend\n0903 ; SpacingMark\n0600 ; Prepend\n1100 ; L\n1160 ; V\n11A8 ; T\n")
        core_properties = self.root / self.write("core-properties.txt", "0915 ; InCB; Consonant\n094D ; InCB; Linker\n")
        emoji = self.root / self.write("emoji.txt", "1F600 ; Extended_Pictographic\n")
        cases = (("build-normalization-tables.py", ["--unicodedata", str(empty), "--exclusions", str(empty)]),
                 ("build-unicode-tables.py", ["--eaw", str(empty), "--emoji", str(empty), "--gencat", str(empty), "--coreprops", str(empty)]),
                 ("build-grapheme-tables.py", ["--gcb", str(grapheme), "--coreprops", str(core_properties), "--emoji-data", str(emoji)]))
        for script, arguments in cases:
            with self.subTest(script=script):
                output = self.root / (script + ".inc")
                subprocess.run([sys.executable, str(repository / "Scripts" / script), *arguments,
                                "--unicode-version", "15.1.0", "--output", str(output)], check=True, capture_output=True)
                self.assertEqual(license_scope.header_declarations(output)[0], ["Apache-2.0 AND Unicode-3.0"])


if __name__ == "__main__":
    unittest.main()
