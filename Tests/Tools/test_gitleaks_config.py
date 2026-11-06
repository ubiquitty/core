#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Checks source-expression allowances against the real secret scanner."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BENIGN_EXPRESSIONS = (
    "std::uint64_t key = 0xcbf29ce484222325ull;\n"
    "__m512i const data_key_lo = _mm512_srli_epi64 (data_key, 32);\n"
    "__m512i const data_key_hi = _mm512_srli_epi64 (data_key, 32);\n"
)


class GitleaksConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scanner = shutil.which("gitleaks")
        if not cls.scanner:
            raise RuntimeError("gitleaks is required for this scanner regression test")
        cls.configuration = (ROOT / ".gitleaks.toml").read_text()

    def scan(self, files, configuration, *, redact=True):
        with tempfile.TemporaryDirectory(prefix="ubiquitty-gitleaks-test.") as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            for name, contents in files.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents)
            configuration_path = root / "config.toml"
            configuration_path.write_text(configuration)
            report = root / "report.json"
            command = [self.scanner, "dir", str(source), "--config", str(configuration_path),
                       "--no-banner", "--no-color", "--report-format=json", "--report-path", str(report),
                       "--exit-code=23", "--timeout=30"]
            if redact:
                command.append("--redact=100")
            result = subprocess.run(command, capture_output=True, text=True, timeout=40, cwd=root)
            self.assertIn(result.returncode, (0, 23), "scanner could not complete the fixture scan")
            self.assertTrue(report.is_file(), "scanner did not write its report")
            findings = json.loads(report.read_text())
            self.assertEqual(result.returncode, 23 if findings else 0)
            if redact:
                self.assertTrue(all(finding["Secret"] == "REDACTED" for finding in findings))
            return findings

    def test_default_rule_reproduces_source_expression_matches(self):
        # Only these public source expressions are inspected without redaction.
        findings = self.scan({"sample.cpp": BENIGN_EXPRESSIONS}, "[extend]\nuseDefault = true\n",
                             redact=False)
        self.assertEqual([(finding["RuleID"], finding["Secret"]) for finding in findings], [
            ("generic-api-key", "0xcbf29ce484222325ull"),
            ("generic-api-key", "_mm512_srli_epi64"),
            ("generic-api-key", "_mm512_srli_epi64"),
        ])

    def test_known_source_expressions_are_allowed(self):
        self.assertEqual(self.scan({"sample.cpp": BENIGN_EXPRESSIONS}, self.configuration), [])

    def test_credentials_and_near_matches_in_same_source_files_are_rejected(self):
        synthetic_value = hashlib.sha256(b"gitleaks regression fixture, never a credential").hexdigest()
        candidates = (
            synthetic_value,
            "0xcbf29ce484222325ull" + synthetic_value,
            "_mm512_srli_epi64" + synthetic_value,
        )
        contents = BENIGN_EXPRESSIONS + "".join(f'api_key = "{value}";\n' for value in candidates)
        for path in ("Source/Core/Rendering/TerminalFrameInputs.cpp", "Source/ThirdParty/xxhash/xxhash.h"):
            with self.subTest(path=path):
                findings = self.scan({path: contents}, self.configuration)
                self.assertEqual([(finding["RuleID"], finding["StartLine"]) for finding in findings], [
                    ("generic-api-key", 4), ("generic-api-key", 5), ("generic-api-key", 6),
                ])

    def test_allowances_do_not_suppress_other_rules(self):
        configuration = self.configuration + """
[[rules]]
id = "fixture-value-proof"
description = "Proves expression allowances are scoped to the generic rule"
regex = '(0xcbf29ce484222325ull|_mm512_srli_epi64)'
secretGroup = 1
"""
        findings = self.scan({"sample.cpp": BENIGN_EXPRESSIONS}, configuration)
        self.assertEqual([(finding["RuleID"], finding["StartLine"]) for finding in findings], [
            ("fixture-value-proof", 1), ("fixture-value-proof", 2), ("fixture-value-proof", 3),
        ])


if __name__ == "__main__":
    unittest.main()
