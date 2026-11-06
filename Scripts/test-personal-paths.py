#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Exercises notice attribution allowances through the public privacy gate."""

import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "personal_paths", ROOT / "Scripts/check-personal-paths.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class PersonalPathTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="privacy-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = {key: value for key, value in os.environ.items()
                            if not key.startswith("GIT_")}
        # No bytecode: the scratch repository has no .gitignore, so a
        # __pycache__ the gate's own imports write is an untracked file it then
        # scans, and the .pyc spells the scratch path. Under a TMPDIR inside a
        # home directory that path is a personal one, and the planted-clean
        # tree fails for where it was made rather than for what it holds.
        self.environment.update(UBIQUITTY_LINT_USER="runner", UBIQUITTY_LINT_HOST="ci",
                                PYTHONDONTWRITEBYTECODE="1")
        (self.root / "Scripts").mkdir()
        for name in ("check-personal-paths.py", "gate_corpus.py", "gate_report.py", "ubq_roots.py"):
            shutil.copy2(ROOT / "Scripts" / name, self.root / "Scripts" / name)
        self.git("init", "-q")

    def git(self, *arguments):
        return subprocess.run(["git", *arguments], cwd=self.root,
                              env=self.environment, capture_output=True,
                              text=True, check=True)

    def write(self, path, content):
        destination = self.root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content, encoding="utf-8")

    def run_gate(self, *arguments):
        result = subprocess.run(
            [sys.executable, "Scripts/check-personal-paths.py", *arguments],
            cwd=self.root, env=self.environment, capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def addresses(self, path):
        return [f"{mailbox}@{domain}"
                for mailbox, domain in GATE.PUBLISHED_ATTRIBUTIONS[path]]

    def test_published_notice_bytes_pass_explicit_untracked_and_staged_scans(self):
        self.assertEqual(sum(map(len, GATE.PUBLISHED_ATTRIBUTIONS.values())), 9)
        # A checkout holds the notices of its own repository; the rest of the
        # table names notices another repository publishes.
        present = [path for path in GATE.PUBLISHED_ATTRIBUTIONS if (ROOT / path).is_file()]
        self.assertTrue(present, "no published notice is in this checkout")
        for path in present:
            content = (ROOT / path).read_text(encoding="utf-8")
            for address in self.addresses(path):
                self.assertIn(address, content)
            self.write(path, content)
        for arguments in (tuple(present), ()):
            with self.subTest(arguments=arguments):
                status, output = self.run_gate(*arguments)
                self.assertEqual(status, 0, output)
                self.assertIn("file(s) scanned", output)
        self.git("add", ".")
        status, output = self.run_gate("--staged")
        self.assertEqual(status, 0, output)
        self.assertEqual(list(self.root.rglob("__pycache__")), [],
                         "the gate wrote bytecode into the tree it scans")

    def test_published_addresses_are_rejected_outside_their_own_notice(self):
        for path in GATE.PUBLISHED_ATTRIBUTIONS:
            addresses = self.addresses(path)
            other_notice = next(name for name in GATE.PUBLISHED_ATTRIBUTIONS if name != path)
            for destination in ("Docs/third-party-licenses/Other-LICENSE.txt",
                                "Source/Core/Example.cpp", other_notice):
                with self.subTest(path=path, destination=destination):
                    self.write(destination, "\n".join(addresses))
                    status, output = self.run_gate(destination)
                    self.assertEqual(status, 1, output)
                    for address in addresses:
                        self.assertIn(f"-> {address}", output)

    def test_published_host_passes_only_in_its_own_file(self):
        self.environment["UBIQUITTY_LINT_HOST"] = "hub"
        for path, hosts in GATE.PUBLISHED_HOSTS.items():
            for host in hosts:
                with self.subTest(path=path, host=host):
                    self.write(path, f"the title bar names the host, {host}, with its tag\n")
                    status, output = self.run_gate(path)
                    self.assertEqual(status, 0, output)
                    self.write("Docs/Other.md", f"ssh {host}\n")
                    status, output = self.run_gate("Docs/Other.md")
                    self.assertEqual(status, 1, output)
                    self.assertIn(f"-> {host}", output)

    def test_published_host_passes_in_the_files_that_spell_the_table(self):
        self.environment["UBIQUITTY_LINT_HOST"] = "hub"
        # The gate's own copy is the one that runs, and it already spells the
        # table; the test file is written with the same line.
        for path in GATE.PUBLISHED_HOSTS_SPELLED_IN:
            for hosts in GATE.PUBLISHED_HOSTS.values():
                for host in hosts:
                    with self.subTest(path=path, host=host):
                        if path != "Scripts/check-personal-paths.py":
                            self.write(path, f'    "README.md": ("{host}",),\n')
                        status, output = self.run_gate(path)
                        self.assertEqual(status, 0, output)
        self.write("Scripts/other-check.py", 'HOST = "hub"\n')
        status, output = self.run_gate("Scripts/other-check.py")
        self.assertEqual(status, 1, output)
        self.assertIn("-> hub", output)

    def test_allowed_address_does_not_hide_another_match_on_its_line(self):
        private_address = "private.person@" + "unpublished.invalid-domain.net"
        for path in GATE.PUBLISHED_ATTRIBUTIONS:
            with self.subTest(path=path):
                address = self.addresses(path)[0]
                self.write(path, f"Author: {address}; other contact: {private_address}\n")
                status, output = self.run_gate(path)
                self.assertEqual(status, 1, output)
                self.assertIn(f"-> {private_address}", output)
                self.assertNotIn(f"-> {address}", output)

    def test_address_allowance_requires_the_complete_match(self):
        for path in GATE.PUBLISHED_ATTRIBUTIONS:
            address = self.addresses(path)[0]
            for modified in ("other." + address, address + ".private"):
                with self.subTest(path=path, address=modified):
                    self.write(path, modified)
                    status, output = self.run_gate(path)
                    self.assertEqual(status, 1, output)
                    self.assertIn(f"-> {modified}", output)

    def test_notice_paths_are_exact_and_explicit_absolute_paths_work(self):
        for path in GATE.PUBLISHED_ATTRIBUTIONS:
            with self.subTest(path=path):
                address = self.addresses(path)[0]
                self.write(path, address)
                status, output = self.run_gate(str(self.root / path))
                self.assertEqual(status, 0, output)
                for other_path in ("copy/" + path, path + ".backup"):
                    self.write(other_path, address)
                    status, output = self.run_gate(other_path)
                    self.assertEqual(status, 1, output)
                    self.assertIn(f"-> {address}", output)

    def test_notice_allowance_does_not_hide_home_paths_or_derived_hosts(self):
        home = "/Users/" + "private-person"
        host = "private-test-" + "machine"
        for path in GATE.PUBLISHED_ATTRIBUTIONS:
            with self.subTest(path=path):
                self.write(path, f"{self.addresses(path)[0]} {home} {host}\n")
                status, output = self.run_gate("--host", host, path)
                self.assertEqual(status, 1, output)
                self.assertIn(f"-> {home}", output)
                self.assertIn(f"-> {host}", output)

    def test_neutral_source_fixtures_remain_allowed(self):
        path = "Tests/Core/Example.cpp"
        self.write(path, "/Users/user /home/user user@host.example laptop\n")
        status, output = self.run_gate(path)
        self.assertEqual(status, 0, output)


if __name__ == "__main__":
    unittest.main()
