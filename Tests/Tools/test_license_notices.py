#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Verifies complete, byte-preserving notice collection for packages and installs."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Scripts"))
from license_notices import copy_notices, notice_files, notice_paths, verify_notices


class LicenseNotices(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ubq notices ")
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name) / "source"
        self.destination = Path(self.temporary.name) / "destination"
        self.names = {
            "LICENSE", "LICENSES/Apache-2.0.txt", "NOTICE",
            "TRADEMARKS.md", "third-party-notices.md",
            "Docs/licensing.md", "Source/Core/Themes/theme-credits.md",
            "Resources/fonts/Example [weight]-OFL.txt", "Source/ThirdParty/example/LICENSE",
        }
        for name in self.names:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"upstream notice for {name}\r\n".encode())
        self.inventory = self.source / "Docs/third-party-inventory.json"
        self.inventory.write_text(json.dumps([
            {"name": "font", "noticeRequired": True,
             "noticePath": "Resources/fonts/Example [weight]-OFL.txt; Source/ThirdParty/example/LICENSE"},
            {"name": "shared", "noticeRequired": True, "noticePath": "Source/ThirdParty/example/LICENSE"},
        ]))
        self.names.add("Docs/third-party-inventory.json")

    def assertCompleteCopy(self, destination):
        files = {path.relative_to(destination).as_posix()
                 for path in destination.rglob("*") if path.is_file()}
        self.assertEqual(files, self.names)
        for name in self.names:
            self.assertEqual((destination / name).read_bytes(), (self.source / name).read_bytes())

    def testCopiesAllNoticesWithoutChangingNamesOrBytes(self):
        copy_notices(self.source, self.destination)
        self.assertCompleteCopy(self.destination)

    def testVerifyNamesEveryMissingOrChangedNotice(self):
        copy_notices(self.source, self.destination)
        self.assertEqual(verify_notices(self.source, self.destination), [])
        (self.destination / "NOTICE").unlink()
        (self.destination / "Source/ThirdParty/example/LICENSE").write_bytes(b"edited\n")
        self.assertEqual(verify_notices(self.source, self.destination),
                         ["missing NOTICE", "differs Source/ThirdParty/example/LICENSE"])

    def testMissingNoticeFailsBeforeCopying(self):
        (self.source / "Source/ThirdParty/example/LICENSE").unlink()
        with self.assertRaisesRegex(ValueError, "missing or external notice"):
            copy_notices(self.source, self.destination)
        self.assertFalse(self.destination.exists())

    def testReadsTheNoticesCoreCarriesFromCoreCheckout(self):
        core = (Path(self.temporary.name) / "core").resolve()
        for name in ("Resources/fonts/Example [weight]-OFL.txt", "Source/ThirdParty/example/LICENSE",
                     "Source/Core/Themes/theme-credits.md"):
            path = core / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"core notice for {name}\n".encode())
            (self.source / name).unlink()
        files = notice_files(self.source, core)
        self.assertEqual(files["Source/Core/Themes/theme-credits.md"], core / "Source/Core/Themes/theme-credits.md")
        self.assertEqual(files["NOTICE"], self.source.resolve() / "NOTICE")
        copy_notices(self.source, self.destination, core)
        self.assertEqual((self.destination / "Source/ThirdParty/example/LICENSE").read_bytes(),
                         b"core notice for Source/ThirdParty/example/LICENSE\n")
        self.assertEqual(verify_notices(self.source, self.destination, core), [])
        with self.assertRaisesRegex(ValueError, "missing or external notice"):
            notice_files(self.source)

    def testMissingRequiredPathFails(self):
        self.inventory.write_text('[{"name":"missing", "noticeRequired":true}]')
        with self.assertRaisesRegex(ValueError, "no noticePath"):
            notice_paths(self.source)

    def testExternalPathFails(self):
        self.inventory.write_text('[{"name":"external", "noticePath":"../secret"}]')
        with self.assertRaisesRegex(ValueError, "invalid notice path"):
            notice_paths(self.source)

    def testInventoryListsEveryActualNotice(self):
        paths = notice_paths(ROOT)
        self.assertIn("Resources/fonts/SymbolsNerdFont-LICENSE.txt", paths)
        self.assertIn("Source/Core/Rendering/UnifontLICENSE.txt", paths)
        self.assertIn("Source/Core/Unicode/UNICODE-LICENSE.txt", paths)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is unavailable")
    def testCMakeInstallCopiesTheSameCompleteSet(self):
        (self.source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.13)
project(NoticeFixture LANGUAGES NONE)
include("{ROOT}/cmake/LicenseNotices.cmake")
ubiquitty_install_license_notices("share/doc/ubiquitty")
''')
        build = Path(self.temporary.name) / "build"
        for command in (
            ["cmake", "-S", str(self.source), "-B", str(build)],
            ["cmake", "--install", str(build), "--prefix", str(self.destination)],
        ):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertCompleteCopy(self.destination / "share/doc/ubiquitty")


if __name__ == "__main__":
    unittest.main()
