#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Verifies complete, byte-preserving notice collection for packages and installs."""

import json
import os
from pathlib import Path
import re
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
        self.writeLibraryMetadata(core, json.loads(self.inventory.read_text()), "Core")
        files = notice_files(self.source, core)
        self.assertEqual(files["Source/Core/Themes/theme-credits.md"], core / "Source/Core/Themes/theme-credits.md")
        self.assertEqual(files["NOTICE"], self.source.resolve() / "NOTICE")
        copy_notices(self.source, self.destination, core)
        self.assertEqual((self.destination / "Source/ThirdParty/example/LICENSE").read_bytes(),
                         b"core notice for Source/ThirdParty/example/LICENSE\n")
        self.assertEqual(verify_notices(self.source, self.destination, core), [])
        with self.assertRaisesRegex(ValueError, "missing or external notice"):
            notice_files(self.source)

    def writeLibraryMetadata(self, root, inventory, owner):
        for name in ("LICENSE", "LICENSES/Apache-2.0.txt", "NOTICE", "TRADEMARKS.md",
                     "third-party-notices.md", "Docs/licensing.md"):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"{owner}'s own {name}\r\n".encode())
        (root / "Docs/third-party-inventory.json").write_text(json.dumps(inventory))
        catalogue = ("[example](Source/ThirdParty/example/LICENSE)\n"
                     "[inventory](Docs/third-party-inventory.json)\n") if owner == "Core" else (
                         "[Phosphor](Resources/phosphor/LICENSE)\n"
                         "[Core](Libraries/ubiquitty-core/third-party-notices.md)\n")
        (root / "third-party-notices.md").write_text(catalogue)

    def splitNoticeCheckouts(self):
        core = (Path(self.temporary.name) / "core").resolve()
        kit = (Path(self.temporary.name) / "kit").resolve()
        for name in ("Resources/fonts/Example [weight]-OFL.txt", "Source/ThirdParty/example/LICENSE",
                     "Source/Core/Themes/theme-credits.md"):
            destination = core / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            (self.source / name).rename(destination)
        self.writeLibraryMetadata(core, json.loads(self.inventory.read_text()), "Core")
        phosphor = kit / "Resources/phosphor/LICENSE"
        phosphor.parent.mkdir(parents=True, exist_ok=True)
        phosphor.write_bytes(b"Kit's Phosphor notice\r\n")
        self.writeLibraryMetadata(kit, [{"name": "Phosphor", "noticeRequired": True,
                                         "noticePath": "Resources/phosphor/LICENSE"}], "Kit")
        inventory = json.loads(self.inventory.read_text())
        inventory.append({"name": "Phosphor", "noticeRequired": True,
                          "noticePath": "Resources/phosphor/LICENSE"})
        self.inventory.write_text(json.dumps(inventory))
        self.names.add("Resources/phosphor/LICENSE")
        return core, kit

    def assertCatalogueLinks(self, destination, catalogues):
        for name in catalogues:
            catalogue = destination / name
            for link in re.findall(r'\]\(([^)]+)\)', catalogue.read_text()):
                target = (catalogue.parent / link).resolve()
                self.assertTrue(target.is_relative_to(destination.resolve()), (name, link))
                self.assertTrue(target.is_file(), (name, link))

    def testStandaloneKitCarriesItsCompleteCoreDependencyManifest(self):
        core, kit = self.splitNoticeCheckouts()
        files = notice_files(kit, core, kit)
        for name in ("NOTICE", "third-party-notices.md", "Docs/third-party-inventory.json",
                     "Source/ThirdParty/example/LICENSE", "Resources/fonts/Example [weight]-OFL.txt"):
            self.assertIn(f"Libraries/ubiquitty-core/{name}", files)
        self.assertEqual(files["NOTICE"], kit / "NOTICE")
        self.assertEqual(files["Libraries/ubiquitty-core/NOTICE"], core / "NOTICE")
        copy_notices(kit, self.destination, core, kit)
        self.assertEqual(verify_notices(kit, self.destination, core, kit), [])
        self.assertCatalogueLinks(self.destination, ["third-party-notices.md",
                                                   "Libraries/ubiquitty-core/third-party-notices.md"])

    def testThreeRootsKeepEveryOwnersNoticesAndNestedCatalogueLinks(self):
        core, kit = self.splitNoticeCheckouts()
        (self.source / "third-party-notices.md").write_text(
            "[Core](Libraries/ubiquitty-core/third-party-notices.md)\n"
            "[Kit](Libraries/ubiquitty-kit/third-party-notices.md)\n")
        files = notice_files(self.source, core, kit)
        owners = [("", self.source), ("Libraries/ubiquitty-core/", core),
                  ("Libraries/ubiquitty-kit/", kit),
                  ("Libraries/ubiquitty-kit/Libraries/ubiquitty-core/", core)]
        for prefix, owner in owners:
            for name in ("NOTICE", "third-party-notices.md", "Docs/third-party-inventory.json"):
                self.assertEqual(files[prefix + name], owner.resolve() / name)
        copy_notices(self.source, self.destination, core, kit)
        self.assertEqual(verify_notices(self.source, self.destination, core, kit), [])
        self.assertCatalogueLinks(self.destination, [prefix + "third-party-notices.md" for prefix, owner in owners])
        for name, original in files.items():
            self.assertEqual((self.destination / name).read_bytes(), original.read_bytes())

    def testMissingTransitiveCoreNoticeFailsBeforeAnyCopy(self):
        core, kit = self.splitNoticeCheckouts()
        (core / "Source/ThirdParty/example/LICENSE").unlink()
        with self.assertRaisesRegex(ValueError, "missing or external notice: Source/ThirdParty/example/LICENSE"):
            copy_notices(kit, self.destination, core, kit)
        self.assertFalse(self.destination.exists())

    def testDependencyInventoryCannotEscapeItsCheckout(self):
        core, kit = self.splitNoticeCheckouts()
        inventory = core / "Docs/third-party-inventory.json"
        inventory.unlink()
        outside = Path(self.temporary.name) / "outside-inventory.json"
        outside.write_text("not even JSON; must be refused before reading")
        inventory.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "missing or external notice: Docs/third-party-inventory.json"):
            copy_notices(kit, self.destination, core, kit)
        self.assertFalse(self.destination.exists())

    def testCoreAndKitCannotClaimTheSameExternalCheckout(self):
        core, kit = self.splitNoticeCheckouts()
        with self.assertRaisesRegex(ValueError, "must be distinct"):
            notice_files(self.source, core, core)

    def testDifferentOwnersCannotOverwriteTheSameManifestDestination(self):
        core, kit = self.splitNoticeCheckouts()
        shadow = self.source / "Libraries/ubiquitty-core/NOTICE"
        shadow.parent.mkdir(parents=True)
        shadow.write_bytes(b"another owner's notice\n")
        rows = json.loads(self.inventory.read_text())
        rows.append({"name": "shadow", "noticePath": "Libraries/ubiquitty-core/NOTICE"})
        self.inventory.write_text(json.dumps(rows))
        with self.assertRaisesRegex(ValueError, "conflicting notice destination: Libraries/ubiquitty-core/NOTICE"):
            copy_notices(self.source, self.destination, core, kit)
        self.assertFalse(self.destination.exists())

    def testCoreIsALeafEvenWhenAnUnrelatedKitWasNamed(self):
        core, kit = self.splitNoticeCheckouts()
        (core / "Scripts").mkdir()
        (core / "Scripts/repository.txt").write_text("core\n")
        files = notice_files(core, kit=kit)
        self.assertEqual(files["NOTICE"], core / "NOTICE")
        self.assertFalse(any(name.startswith("Libraries/") for name in files))

    def testMarkedDependenciesCannotBeMislabelledOrPointBackToTheApp(self):
        core, kit = self.splitNoticeCheckouts()
        for root, role in ((self.source, "app"), (core, "core"), (kit, "kit")):
            (root / "Scripts").mkdir()
            (root / "Scripts/repository.txt").write_text(role + "\n")
        for core_root, kit_root in ((self.source, kit), (core, self.source), (kit, core)):
            with self.subTest(core=core_root, kit=kit_root):
                with self.assertRaisesRegex(ValueError, "notice dependency must declare"):
                    copy_notices(self.source, self.destination, core_root, kit_root)
                self.assertFalse(self.destination.exists())

    def testEveryDestinationIsValidatedBeforeAnyCopy(self):
        self.destination.mkdir()
        outside = Path(self.temporary.name) / "outside"
        outside.mkdir()
        (self.destination / "Source").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "external notice destination"):
            copy_notices(self.source, self.destination)
        self.assertEqual(list(self.destination.iterdir()), [self.destination / "Source"])
        self.assertEqual(list(outside.iterdir()), [])

    def testInternalDestinationSymlinkCannotOverwriteAnotherNotice(self):
        self.destination.mkdir()
        (self.destination / "LICENSE").write_bytes(b"existing license\n")
        (self.destination / "NOTICE").symlink_to("LICENSE")
        with self.assertRaisesRegex(ValueError, "aliased notice destination: NOTICE"):
            copy_notices(self.source, self.destination)
        self.assertEqual((self.destination / "LICENSE").read_bytes(), b"existing license\n")
        self.assertEqual({path.name for path in self.destination.iterdir()}, {"LICENSE", "NOTICE"})

    def testDestinationDirectorySymlinkCannotAliasNamespaces(self):
        self.destination.mkdir()
        (self.destination / "alias").mkdir()
        (self.destination / "Source").symlink_to("alias", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "aliased notice destination"):
            copy_notices(self.source, self.destination)
        self.assertEqual({path.name for path in self.destination.iterdir()}, {"alias", "Source"})
        self.assertEqual(list((self.destination / "alias").iterdir()), [])

    def testDestinationHardlinkCannotAlterAnOriginalNotice(self):
        self.destination.mkdir()
        original = (self.source / "NOTICE").read_bytes()
        os.link(self.source / "NOTICE", self.destination / "LICENSE")
        with self.assertRaisesRegex(ValueError, "aliased notice destination: LICENSE"):
            copy_notices(self.source, self.destination)
        self.assertEqual((self.source / "NOTICE").read_bytes(), original)
        self.assertEqual(list(self.destination.iterdir()), [self.destination / "LICENSE"])

    def testVerifyRejectsAnExternalLinkEvenWhenTheBytesMatch(self):
        copy_notices(self.source, self.destination)
        (self.destination / "NOTICE").unlink()
        (self.destination / "NOTICE").symlink_to(self.source / "NOTICE")
        self.assertEqual(verify_notices(self.source, self.destination), ["external NOTICE"])

    def testAppCoreAndKitNoticesUseTheirOwningCheckouts(self):
        core, kit = self.splitNoticeCheckouts()
        files = notice_files(self.source, core, kit)
        self.assertEqual(files["Resources/phosphor/LICENSE"], kit / "Resources/phosphor/LICENSE")
        self.assertEqual(files["Source/ThirdParty/example/LICENSE"], core / "Source/ThirdParty/example/LICENSE")
        self.assertEqual(files["NOTICE"], self.source.resolve() / "NOTICE")
        copy_notices(self.source, self.destination, core, kit)
        self.assertEqual((self.destination / "Resources/phosphor/LICENSE").read_bytes(), b"Kit's Phosphor notice\r\n")
        self.assertEqual(verify_notices(self.source, self.destination, core, kit), [])
        result = subprocess.run([sys.executable, str(ROOT / "Scripts/license_notices.py"),
                                 "--source", str(self.source), "--core", str(core), "--kit", str(kit), "--files"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"Resources/phosphor/LICENSE\t{kit / 'Resources/phosphor/LICENSE'}", result.stdout)

    def testMissingKitNoticeCannotFallBackToAppOrCore(self):
        core, kit = self.splitNoticeCheckouts()
        (kit / "Resources/phosphor/LICENSE").unlink()
        for root in (self.source, core):
            path = root / "Resources/phosphor/LICENSE"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"wrong checkout's notice\n")
        with self.assertRaisesRegex(ValueError, "missing or external notice: Resources/phosphor/LICENSE"):
            copy_notices(self.source, self.destination, core, kit)
        self.assertFalse(self.destination.exists())

    def testKitNoticeSymlinkCannotEscapeItsCheckout(self):
        core, kit = self.splitNoticeCheckouts()
        notice = kit / "Resources/phosphor/LICENSE"
        notice.unlink()
        outside = Path(self.temporary.name) / "outside-license.txt"
        outside.write_bytes(b"outside\n")
        notice.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, "missing or external notice: Resources/phosphor/LICENSE"):
            notice_files(self.source, core, kit)

    @unittest.skipUnless(shutil.which("cmake"), "CMake is unavailable")
    def testCMakeInstallReadsNoticesAcrossThreeCheckouts(self):
        core, kit = self.splitNoticeCheckouts()
        (self.source / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.13)
project(NoticeFixture LANGUAGES NONE)
set(UBIQUITTY_CORE_SOURCE_DIR "{core}")
set(UBIQUITTY_KIT_SOURCE_DIR "{kit}")
include("{ROOT}/cmake/LicenseNotices.cmake")
ubiquitty_install_license_notices("share/doc/ubiquitty")
ubiquitty_license_notice_file("Source/ThirdParty/example/LICENSE" direct)
ubiquitty_license_notice_file("Libraries/ubiquitty-core/Source/ThirdParty/example/LICENSE" dependency)
ubiquitty_license_notice_file("Libraries/ubiquitty-kit/Libraries/ubiquitty-core/Source/ThirdParty/example/LICENSE" nested)
file(WRITE "${{CMAKE_BINARY_DIR}}/notice-copies" "${{direct}}\n${{dependency}}\n${{nested}}")
''')
        build = Path(self.temporary.name) / "build"
        for command in (["cmake", "-S", str(self.source), "-B", str(build)],
                        ["cmake", "--install", str(build), "--prefix", str(self.destination)]):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        staged = self.destination / "share/doc/ubiquitty"
        self.assertEqual(verify_notices(self.source, staged, core, kit), [])
        copies = (build / "notice-copies").read_text().splitlines()
        self.assertEqual(len(set(copies)), 3)
        for path in copies:
            self.assertEqual(Path(path).read_bytes(), (core / "Source/ThirdParty/example/LICENSE").read_bytes())

    @unittest.skipUnless(shutil.which("cmake"), "CMake is unavailable")
    def testStandaloneKitCMakeInstallIncludesCoreAndTracksBothInventories(self):
        core, kit = self.splitNoticeCheckouts()
        for root, role in ((core, "core"), (kit, "kit")):
            (root / "Scripts").mkdir()
            (root / "Scripts/repository.txt").write_text(role + "\n")
        (kit / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.13)
project(NoticeFixture LANGUAGES NONE)
set(UBIQUITTY_CORE_SOURCE_DIR "{core}")
set(UBIQUITTY_KIT_SOURCE_DIR "{kit}")
include("{ROOT}/cmake/LicenseNotices.cmake")
ubiquitty_install_license_notices("share/doc/ubiquitty-kit")
get_property(dependencies DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)
file(WRITE "${{CMAKE_BINARY_DIR}}/notice-inputs" "${{dependencies}}")
''')
        build = Path(self.temporary.name) / "build"
        for command in (["cmake", "-S", str(kit), "-B", str(build)],
                        ["cmake", "--install", str(build), "--prefix", str(self.destination)]):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        staged = self.destination / "share/doc/ubiquitty-kit"
        self.assertEqual(verify_notices(kit, staged, core, kit), [])
        self.assertCatalogueLinks(staged, ["third-party-notices.md",
                                           "Libraries/ubiquitty-core/third-party-notices.md"])
        self.assertIn(str(core / "Docs/third-party-inventory.json"), (build / "notice-inputs").read_text())
        self.assertIn(str(kit / "Docs/third-party-inventory.json"), (build / "notice-inputs").read_text())
        self.assertIn(str(core / "Scripts/repository.txt"), (build / "notice-inputs").read_text())
        self.assertIn(str(kit / "Scripts/repository.txt"), (build / "notice-inputs").read_text())

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja") and shutil.which("make"),
                         "CMake, Ninja and Make are needed for generated-input checks")
    def testGeneratedNoticeInputsRefreshAndRegenerateInNormalBuilds(self):
        core, kit = self.splitNoticeCheckouts()
        for root, role in ((core, "core"), (kit, "kit")):
            (root / "Scripts").mkdir()
            (root / "Scripts/repository.txt").write_text(role + "\n")
        (kit / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.13)
project(NoticeFixture LANGUAGES NONE)
set(UBIQUITTY_CORE_SOURCE_DIR "{core}")
set(UBIQUITTY_KIT_SOURCE_DIR "{kit}")
include("{ROOT}/cmake/LicenseNotices.cmake")
ubiquitty_install_license_notices("share/doc/ubiquitty-kit")
ubiquitty_license_notice_file("Libraries/ubiquitty-core/Source/ThirdParty/example/LICENSE" input)
file(WRITE "${{CMAKE_BINARY_DIR}}/notice-input" "${{input}}")
add_custom_target(bundle-notice ALL
    COMMAND "${{CMAKE_COMMAND}}" -E copy_if_different "${{input}}" "${{CMAKE_BINARY_DIR}}/bundle-license"
    DEPENDS "${{input}}")
''')

        def run(*command):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        for index, generator in enumerate(("Ninja", "Unix Makefiles")):
            with self.subTest(generator=generator):
                build = Path(self.temporary.name) / f"build-{index}"
                run("cmake", "-S", str(kit), "-B", str(build), "-G", generator)
                run("cmake", "--build", str(build))
                staged = Path((build / "notice-input").read_text())
                original = core / "Source/ThirdParty/example/LICENSE"
                original.write_bytes(f"changed upstream bytes {index}\r\n".encode())
                run("cmake", "--build", str(build))
                self.assertEqual(staged.read_bytes(), original.read_bytes())
                self.assertEqual((build / "bundle-license").read_bytes(), original.read_bytes())
                staged.unlink()
                run("cmake", "--build", str(build))
                self.assertEqual(staged.read_bytes(), original.read_bytes())
                self.assertEqual((build / "bundle-license").read_bytes(), original.read_bytes())
                added = f"Source/ThirdParty/added-{index}/LICENSE"
                (core / added).parent.mkdir(parents=True)
                (core / added).write_bytes(b"another upstream license\r\n")
                inventory = core / "Docs/third-party-inventory.json"
                rows = json.loads(inventory.read_text())
                rows.append({"name": f"added-{index}", "noticeRequired": True, "noticePath": added})
                inventory.write_text(json.dumps(rows))
                run("cmake", "--build", str(build))
                installed = Path(self.temporary.name) / f"installed-{index}"
                run("cmake", "--install", str(build), "--prefix", str(installed))
                self.assertEqual(verify_notices(kit, installed / "share/doc/ubiquitty-kit", core, kit), [])
                (core / "Scripts/repository.txt").write_text("app\n")
                invalid = subprocess.run(["cmake", "--build", str(build)], capture_output=True, text=True)
                self.assertNotEqual(invalid.returncode, 0, invalid.stdout + invalid.stderr)
                self.assertIn("notice dependency must declare the core repository",
                              " ".join((invalid.stdout + invalid.stderr).split()))
                (core / "Scripts/repository.txt").write_text("core\n")

    def testMissingRequiredPathFails(self):
        self.inventory.write_text('[{"name":"missing", "noticeRequired":true}]')
        with self.assertRaisesRegex(ValueError, "no noticePath"):
            notice_paths(self.source)

    def testExternalPathFails(self):
        self.inventory.write_text('[{"name":"external", "noticePath":"../secret"}]')
        with self.assertRaisesRegex(ValueError, "invalid notice path"):
            notice_paths(self.source)

    def testNoticeNamesCannotInjectManifestSeparators(self):
        for separator in ("\t", "\r", "\n", "\\"):
            with self.subTest(separator=separator):
                self.inventory.write_text(json.dumps([{"name": "external", "noticePath": f"bad{separator}path"}]))
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
