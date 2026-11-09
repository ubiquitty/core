#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Checks project license scope and third-party inventory, notices, and pins.

Project checks verify the two license texts, the notices that explain the
split, and that every first-party file's header names the license its
directory has in license-scope.json's boundary table. Four additional checks
read Docs/third-party-inventory.json and the tree:

  coverage    every inventory row has a notice, and every notice a row
  discovery   every third-party input the tree carries has an inventory row —
              vendored sources, bundled fonts, fetched pins, generated tables
  surface     every font a package copies is on its row's distribution
              surface: the macOS bundle, UbiquittyKit.framework, the iOS
              bundle, the web stage and the AppImage, each read from the file
              that does the copying
  pin         everything redistributed is pinned to one upstream release: a
              version token, not a moving branch, or a stated reason a pin
              does not apply
  risk        nothing redistributed carries an unreviewed redistribution risk

  Scripts/check-licenses.py             # warnings do not block
  Scripts/check-licenses.py --release   # pin and risk warnings become failures
  Scripts/check-licenses.py --self-test # project-license regression fixtures

Exit 0 = clean, 1 = violations.

Every tracked and unignored new file is classified, including audit reports,
root configuration, binary assets, and raw fixtures. license-scope.json records
the Apache-2.0 scope and its upstream/metadata exceptions. Source exports scan their
entire shipped tree. Missing or conflicting declarations fail; unresolved
provenance also fails with --release.
"""

import glob
import hashlib
import json
import os
import re
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import ubq_roots
import license_scope

INVENTORY = "Docs/third-party-inventory.json"
NOTICES = "third-party-notices.md"
APACHE_TEXT = "LICENSES/Apache-2.0.txt"
# The unmodified text served at https://www.apache.org/licenses/LICENSE-2.0.txt.
APACHE_TEXT_SHA256 = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"


def project_license_errors(directory, paths, file_errors=None):
    """Returns an altered or missing license text, incomplete notices, and boundary conflicts."""
    directory = Path(directory)
    failures = []
    pinned = {APACHE_TEXT: APACHE_TEXT_SHA256}
    for name, digest in pinned.items():
        path = directory / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            failures.append(f"project-license: {name} must be the unmodified license text")
    required = {
        "LICENSE": (license_scope.APACHE_LICENSE, APACHE_TEXT, "Docs/licensing.md", "TRADEMARKS.md"),
        "NOTICE": ("Apache License, Version 2.0", APACHE_TEXT, "TRADEMARKS.md"),
        "TRADEMARKS.md": ("different name", "icon"),
    }
    for name, markers in required.items():
        path = directory / name
        normalized = " ".join(path.read_text(encoding="utf-8").split()) if path.is_file() else ""
        if any(marker not in normalized for marker in markers):
            failures.append(f"project-license: {name} is missing required license terms")

    if file_errors is None:
        file_errors, _, _ = license_scope.file_license_audit(directory, paths)
    failures.extend(file_errors)
    return failures


REDISTRIBUTED = {"source-repo", "libUbiquittyCore", "mac-app", "ubiquittykit-framework", "ios-app", "appimage", "web",
                 "ubiquitty-host"}
# A moving reference, matched as a whole word: "remains" and "header-only"
# hold "main" and "head" without naming a branch.
MOVING = re.compile(r"\b(?:continuous|latest|main|head|master|trunk)\b", re.I)
# What names one upstream release: a dotted version (v9.0.0, 2.104.0,
# NotoSans-v2.015), a dated release tag (re2's 2023-03-01), or a commit of
# seven or more hex digits with a letter and a digit among them, so neither a
# word spelled in hex nor a decimal count passes for one.
VERSION_TOKEN = re.compile(r"(?<![0-9.])\d+(?:\.\d+)+(?![0-9])"
                           r"|(?<![0-9])\d{4}-\d{2}-\d{2}(?![0-9])"
                           r"|\b(?=[0-9a-f]*[a-f])(?=[0-9a-f]*[0-9])[0-9a-f]{7,40}\b")
# A license's own version is not a release of the component: an SPDX
# identifier (OFL-1.1, CC-BY-4.0, Unicode-3.0) or a version the word "license"
# follows or leads ("Unicode 3.0 license", "License, Version 2.0"). These are
# removed before the pin is searched for a release.
LICENSE_VERSION = re.compile(
    r"\b(?:OFL|CC-BY(?:-SA|-NC|-ND)*|Apache|L?GPL|AGPL|MPL|EPL|EUPL|CDDL|Unicode|PSF|Artistic|BSL|LPPL|OSL|AFL)"
    r"-\d+(?:\.\d+)*(?:-only|-or-later)?(?![\w.])"
    r"|\b\d+(?:\.\d+)+\s+licen[cs]es?\b"
    r"|\blicen[cs]e,?\s+(?:version\s+|v)?\d+(?:\.\d+)*", re.I)


def is_redistributed(row):
    """Whether a row ships: a redistributed surface, unless the row says it does not."""
    surfaces = set(row.get("distributionSurface", []))
    # An explicit "redistributed": false overrides the surface set: a dep whose only
    # shipping surface is deferred from this cut (e.g. the Linux AppImage under the
    # macOS-first launch) is not gated as redistributed until it actually ships.
    return bool(surfaces & REDISTRIBUTED) and row.get("redistributed", True)


def pin_problems(row):
    """Returns why a redistributed row's pin does not name one fixed upstream release.

    A row whose input has no single upstream release states why in
    `pinNotApplicable`; the reason replaces the pin check, and an empty one
    is itself a problem.
    """
    if not is_redistributed(row):
        return []
    name = row.get("name", "?")
    if "pinNotApplicable" in row:
        reason = row["pinNotApplicable"]
        if not isinstance(reason, str) or not reason.strip():
            return [f"pin: '{name}' sets pinNotApplicable without a reason"]
        return []
    pin = str(row.get("pinnedRev", "")).strip()
    if MOVING.search(pin):
        return [f"pin: '{name}' is on a moving pin '{pin}' on a redistributed surface"]
    if not VERSION_TOKEN.search(LICENSE_VERSION.sub(" ", pin)):
        return [f"pin: '{name}' pin '{pin}' names no version (a dotted number other than a license's, "
                f"a dated release tag, or a commit of 7+ hex digits) on a redistributed surface"]
    return []


def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


def matches(a, b):
    a, b = norm(a), norm(b)
    return bool(a) and bool(b) and (a in b or b in a)


FONT_FILE = re.compile(r"[\w\-\[\]]+\.(?:ttf|otf|ttc|otc|woff2?)\b")
# Each package that carries fonts, and the file that copies them out of
# Resources/fonts. The macOS bundle copies the whole directory; the iOS bundle
# and the Linux install rule, which the AppImage is laid out from, copy a named
# CMake list; the web stage names each file.
#
# A package is built by the repositories named beside it, so a checkout that holds
# none of them has no copy to read; the fonts themselves are listed in the
# inventory of the repository that owns Resources/fonts either way.
FONT_COPIERS = (
    ("mac-app", "Source/Platform/macOS/CMakeLists.txt", "directory", ("app",)),
    ("ubiquittykit-framework", "Scripts/cmake/UbiquittyKit.cmake", "framework", ("kit",)),
    ("ios-app", "CMakeLists.txt", "list:UBIQUITTY_IOS_BUNDLED_FONT_FILES", ("app",)),
    ("web", "Scripts/build-web.sh", "script", ("app",)),
    ("appimage", "CMakeLists.txt", "list:UBIQUITTY_LINUX_BUNDLED_FONTS", ("app",)),
)


def copied_fonts(directory, path, kind):
    """Returns the font files a package copies, or None when the copy is not spelled as expected."""
    text = (directory / path).read_text(encoding="utf-8")
    if kind in ("directory", "framework"):
        if not re.search(r'file\(GLOB\s+UBIQUITTY_BUNDLED_FONT_FILES\b[^)]*Resources/fonts/\*"', text):
            return None
        if kind == "framework" and not re.search(
                r'ubiquitty_bundle_resource_directory\(\s*UbiquittyKit\s+"fonts"\s+\$\{UBIQUITTY_BUNDLED_FONT_FILES\}\s*\)', text):
            return None
        fonts = Path(ubq_roots.core_root(str(directory))) / "Resources/fonts"
        if not fonts.is_dir():
            return None
        return {font.name for font in fonts.glob("*") if FONT_FILE.fullmatch(font.name)}
    if kind.startswith("list:"):
        listed = re.search(r'set\(' + re.escape(kind[len("list:"):]) + r'\s(.*?)\)', text, re.S)
        return set(FONT_FILE.findall(listed.group(1))) or None if listed else None
    code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    return set(FONT_FILE.findall(code)) or None


def font_surface_errors(directory, rows):
    """Returns each font a package copies whose inventory row does not list that package's surface.

    A copy the parse cannot find is a failure rather than an empty set: a
    rename of the list or a new spelling of the copy would otherwise leave the
    surface unchecked and the gate green.
    """
    directory = Path(directory)
    failures = []
    held = ubq_roots.repositories(str(directory))
    core = Path(ubq_roots.core_root(str(directory)))
    kit = Path(ubq_roots.kit_root(str(directory)))
    kit_rows = None
    font_rows = rows
    if core != directory:
        try:
            font_rows = json.loads((core / INVENTORY).read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            return [f"surface: cannot read Core's font inventory at {core / INVENTORY}: {error}"]
    for surface, path, kind, builders in FONT_COPIERS:
        if not (directory / path).is_file() or not set(builders) & set(held):
            continue
        fonts = copied_fonts(directory, path, kind)
        if fonts is None:
            failures.append(f"surface: {path} copies its {surface} fonts in a form this check does not read; "
                            f"update copied_fonts() in Scripts/check-licenses.py")
            continue
        # The web stage copies Kit's Phosphor fonts beside Core's terminal
        # fonts. The explicit resource prefix selects their owning inventory;
        # another checkout's row cannot supply a missing distribution surface.
        code = "\n".join(line for line in (directory / path).read_text(encoding="utf-8").splitlines()
                         if not line.lstrip().startswith("#"))
        kit_fonts = set(re.findall(r'Resources/phosphor/(' + FONT_FILE.pattern + ')', code))
        for font in sorted(fonts):
            owning_rows = font_rows
            if font in kit_fonts:
                if not (kit / "Resources/phosphor" / font).is_file():
                    failures.append(f"surface: {path} copies a missing Kit font: Resources/phosphor/{font}")
                    continue
                if kit_rows is None:
                    if kit == directory:
                        kit_rows = rows
                    else:
                        try:
                            kit_rows = json.loads((kit / INVENTORY).read_text(encoding="utf-8"))
                        except (OSError, ValueError) as error:
                            failures.append(f"surface: cannot read Kit's font inventory at {kit / INVENTORY}: {error}")
                            continue
                owning_rows = kit_rows
            owners = [row for row in owning_rows if matches(font.rsplit(".", 1)[0], row.get("name", ""))]
            if not owners:
                failures.append(f"surface: {path} copies {font}, which has no inventory row")
            for row in owners:
                if surface not in row.get("distributionSurface", []):
                    failures.append(f"surface: {path} copies {font} into {surface}, "
                                    f"but '{row['name']}' does not list {surface}")
    return failures


def project_license_self_test():
    """Exercises dropped conditions, accidental grants, and permissive carveouts."""
    import importlib.util
    import tempfile
    import unittest
    from unittest import mock

    APACHE = license_scope.APACHE_LICENSE

    class ProjectLicenseTests(unittest.TestCase):
        def setUp(self):
            self.temporary = tempfile.TemporaryDirectory()
            self.addCleanup(self.temporary.cleanup)
            self.root = Path(self.temporary.name)
            repository = Path(__file__).resolve().parent.parent
            for name in ("LICENSE", APACHE_TEXT, "NOTICE", "TRADEMARKS.md"):
                (self.root / name).parent.mkdir(parents=True, exist_ok=True)
                (self.root / name).write_bytes((repository / name).read_bytes())
            boundary = [
                {"paths": ["Source/"], "license": APACHE, "reason": "Project fixture."},
                {"paths": ["Source/Platform/Emacs/", "Source/Platform/Linux/vendored.cpp"], "license": "MIT",
                 "reason": "Separately licensed fixture."},
            ]
            rules = [{"patterns": ["Source/Core/Unicode/Width.tables.inc"], "mode": "header",
                      "license": "Apache-2.0 AND Unicode-3.0", "reason": "Generated data fixture."}]
            (self.root / license_scope.MANIFEST).write_text(
                json.dumps({"version": 1, "boundary": boundary, "rules": rules}))

        def source(self, name, identifier):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            marker = {".el": ";;", ".lua": "--"}.get(path.suffix, "//")
            path.write_text(f"{marker} Copyright (C) 2026 Stanislav Yaglo\n"
                            f"{marker} SPDX-License-Identifier: {identifier}\n", encoding="utf-8")
            return name

        def test_the_project_license_and_the_separate_licenses(self):
            paths = [self.source("Source/Core/Terminal.h", APACHE),
                     self.source("Source/Platform/Host/Daemon.h", APACHE),
                     self.source("Source/Platform/Linux/main.cpp", APACHE),
                     self.source("Source/Platform/Linux/vendored.cpp", "MIT"),
                     self.source("Source/Core/Unicode/Width.tables.inc", "Apache-2.0 AND Unicode-3.0"),
                     self.source("Source/Platform/Emacs/client.el", "MIT")]
            self.assertEqual(project_license_errors(self.root, paths), [])

        def test_a_separately_licensed_file_cannot_claim_the_project_license(self):
            name = self.source("Source/Platform/Emacs/client.el", APACHE)
            self.assertTrue(project_license_errors(self.root, [name]))

        def test_a_project_file_cannot_claim_another_license(self):
            name = self.source("Source/Core/Terminal.h", "MIT")
            self.assertTrue(project_license_errors(self.root, [name]))

        def test_an_exact_file_overrides_its_directory(self):
            name = self.source("Source/Platform/Linux/vendored.cpp", APACHE)
            self.assertTrue(project_license_errors(self.root, [name]))

        def test_file_outside_the_boundary_fails(self):
            name = self.source("Elsewhere/new.h", APACHE)
            self.assertTrue(project_license_errors(self.root, [name]))

        def test_header_rule_must_agree_with_the_boundary(self):
            manifest = json.loads((self.root / license_scope.MANIFEST).read_text())
            manifest["rules"].append({"paths": ["Source/Platform/Emacs/Data.inc"], "mode": "header",
                                      "license": "Apache-2.0 AND Unicode-3.0", "reason": "Misplaced."})
            (self.root / license_scope.MANIFEST).write_text(json.dumps(manifest))
            name = self.source("Source/Platform/Emacs/Data.inc", "Apache-2.0 AND Unicode-3.0")
            self.assertTrue(project_license_errors(self.root, [name]))

        def test_a_path_claimed_by_both_licenses_fails(self):
            manifest = json.loads((self.root / license_scope.MANIFEST).read_text())
            manifest["boundary"].append({"paths": ["Source/"], "license": "MIT", "reason": "Conflict."})
            (self.root / license_scope.MANIFEST).write_text(json.dumps(manifest))
            self.assertTrue(project_license_errors(self.root, []))

        def test_missing_header_fails(self):
            name = "Source/Core/Terminal.h"
            path = self.root / name
            path.parent.mkdir(parents=True)
            path.write_text("#pragma once\n")
            self.assertTrue(project_license_errors(self.root, [name]))

        def test_an_altered_license_text_fails(self):
            for name in (APACHE_TEXT,):
                with self.subTest(name=name):
                    path = self.root / name
                    original = path.read_bytes()
                    path.write_bytes(original.replace(b"Version 2.0", b"Version 2.1"))
                    self.assertTrue(project_license_errors(self.root, []))
                    path.unlink()
                    self.assertTrue(project_license_errors(self.root, []))
                    path.write_bytes(original)
            self.assertEqual(project_license_errors(self.root, []), [])

        def test_notices_must_name_the_license_and_the_trademark_policy(self):
            for name in ("LICENSE", "NOTICE"):
                with self.subTest(name=name):
                    path = self.root / name
                    original = path.read_text()
                    path.write_text(original.replace("TRADEMARKS.md", "the policy"))
                    self.assertTrue(project_license_errors(self.root, []))
                    path.write_text(original)

    class PinTests(unittest.TestCase):
        def row(self, **fields):
            return {"name": "fixture", "distributionSurface": ["mac-app"], **fields}

        def test_a_release_version_pins(self):
            for pin in ("v9.0.0", "2.104.0", "NotoSans-v2.015 (statics)", "v0.4.0-alpha.10", "3.3.0 (e2dc220)",
                        "2023-03-01", "commit 19371302b", "17.0.04 (unifont_all-17.0.04.hex)",
                        "Unicode 17.0.0 (Width)", "2.1 (CC-BY-4.0 at b1b0a0f28b68717456ef4f93566356d14730cb82)"):
                with self.subTest(pin=pin):
                    self.assertEqual(pin_problems(self.row(pinnedRev=pin)), [])

        def test_a_pin_with_no_version_fails(self):
            for pin in ("embedded snapshot (Source/Core/Rendering/UnifontData)", "embedded", "",
                        "curated (upstream revision not recorded)", "defaced", "OFL"):
                with self.subTest(pin=pin):
                    self.assertTrue(pin_problems(self.row(pinnedRev=pin)))

        def test_a_license_version_or_a_count_is_not_a_release(self):
            for pin in ("bundled under OFL-1.1", "embedded (Unicode 3.0 license)", "snapshot of 4100000 bytes",
                        "under the Apache License, Version 2.0", "GPL-2.0-or-later copy", "CC-BY-SA-4.0 text"):
                with self.subTest(pin=pin):
                    self.assertTrue(pin_problems(self.row(pinnedRev=pin)))

        def test_a_moving_pin_fails_even_beside_a_version(self):
            for pin in ("continuous", "main", "latest 1.2.3", "origin/main at 1.2.3", "HEAD 1.2.3"):
                with self.subTest(pin=pin):
                    self.assertTrue(pin_problems(self.row(pinnedRev=pin)))

        def test_a_word_that_holds_a_moving_word_is_not_one(self):
            for pin in ("v2.304 (remains pinned)", "header-only 0.8.3", "maintained at 1.0.0"):
                with self.subTest(pin=pin):
                    self.assertEqual(pin_problems(self.row(pinnedRev=pin)), [])

        def test_a_stated_reason_replaces_the_pin_and_must_say_something(self):
            self.assertEqual(pin_problems(self.row(pinnedRev="embedded", pinNotApplicable="Many upstreams.")), [])
            for reason in ("", "  ", None):
                with self.subTest(reason=reason):
                    self.assertTrue(pin_problems(self.row(pinnedRev="embedded", pinNotApplicable=reason)))

        def test_a_row_that_does_not_ship_is_not_gated(self):
            self.assertEqual(pin_problems({"name": "doc", "distributionSurface": ["contributor-docs"],
                                           "pinnedRev": "embedded"}), [])
            self.assertEqual(pin_problems(self.row(pinnedRev="continuous", redistributed=False)), [])

    class FontSurfaceTests(unittest.TestCase):
        CMAKE_LISTS = ('set(UBIQUITTY_IOS_BUNDLED_FONT_FILES\n    "Face-Regular.ttf")\n'
                       'set(UBIQUITTY_LINUX_BUNDLED_FONTS\n    "Face-Regular.ttf")\n')
        MACOS_LISTS = ('file(GLOB UBIQUITTY_BUNDLED_FONT_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/Resources/fonts/*")\n'
                       'ubiquitty_bundle_resource_directory(UbiquittyKit "fonts" ${UBIQUITTY_BUNDLED_FONT_FILES})\n')
        SURFACES = ("mac-app", "ubiquittykit-framework", "ios-app", "web", "appimage")

        def setUp(self):
            self.temporary = tempfile.TemporaryDirectory()
            self.addCleanup(self.temporary.cleanup)
            self.root = Path(self.temporary.name)
            roots = mock.patch.dict(os.environ, {"UBQ_CORE_ROOT": "", "UBQ_KIT_ROOT": ""})
            roots.start()
            self.addCleanup(roots.stop)
            self.write("Resources/fonts/Face-Regular.ttf", "")
            self.write("Resources/fonts/Face-OFL.txt", "")
            self.write("Source/Platform/macOS/CMakeLists.txt", self.MACOS_LISTS)
            self.write("Scripts/cmake/UbiquittyKit.cmake", self.MACOS_LISTS)
            self.write("CMakeLists.txt", self.CMAKE_LISTS)
            self.write("Scripts/build-web.sh", '# Face-Other.ttf is named in a comment only\n'
                                               'cp "$ROOT/Resources/fonts/Face-Regular.ttf" "$STAGE/fonts/"\n')

        def write(self, name, text):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

        def rows(self, *surfaces):
            return [{"name": "Face", "distributionSurface": list(surfaces)}]

        def test_every_copy_on_its_surface_passes(self):
            self.assertEqual(font_surface_errors(self.root, self.rows(*self.SURFACES)), [])

        def test_a_copy_off_its_rows_surface_fails(self):
            for missing in self.SURFACES:
                with self.subTest(missing=missing):
                    surfaces = [s for s in self.SURFACES if s != missing]
                    failures = font_surface_errors(self.root, self.rows(*surfaces))
                    self.assertEqual(len(failures), 1)
                    self.assertIn(missing, failures[0])

        def test_a_copied_font_with_no_row_fails(self):
            self.assertTrue(font_surface_errors(self.root, [{"name": "Other", "distributionSurface": []}]))

        def test_a_copy_the_check_cannot_read_fails(self):
            self.write("CMakeLists.txt", 'set(UBIQUITTY_IOS_FONTS "Face-Regular.ttf")\n')
            self.assertTrue(font_surface_errors(self.root, self.rows(*self.SURFACES)))
            self.write("CMakeLists.txt", self.CMAKE_LISTS)
            self.write("Source/Platform/macOS/CMakeLists.txt", 'set(FONTS "${CMAKE_SOURCE_DIR}/Resources/fonts/Face-Regular.ttf")\n')
            self.assertTrue(font_surface_errors(self.root, self.rows(*self.SURFACES)))

        def test_a_framework_copy_of_another_list_fails(self):
            self.write("Scripts/cmake/UbiquittyKit.cmake", self.MACOS_LISTS.replace(
                "${UBIQUITTY_BUNDLED_FONT_FILES}", "${UBIQUITTY_KIT_FONT_FILES}"))
            failures = font_surface_errors(self.root, self.rows(*self.SURFACES))
            self.assertEqual(len(failures), 1)
            self.assertIn("ubiquittykit-framework", failures[0])

        def kit_with_core_fonts(self, *surfaces):
            self.write("Scripts/repository.txt", "kit\n")
            self.write("Libraries/ubiquitty-core/Scripts/repository.txt", "core\n")
            for font in (self.root / "Resources/fonts").iterdir():
                destination = self.root / "Libraries/ubiquitty-core/Resources/fonts" / font.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                font.rename(destination)
            self.write("Libraries/ubiquitty-core/" + INVENTORY, json.dumps(self.rows(*surfaces)))

        def test_framework_fonts_and_inventory_are_read_from_core(self):
            self.kit_with_core_fonts("ubiquittykit-framework")
            self.assertEqual(font_surface_errors(self.root, []), [])

        def test_a_framework_surface_missing_from_cores_row_fails(self):
            self.kit_with_core_fonts("mac-app")
            failures = font_surface_errors(self.root, self.rows(*self.SURFACES))
            self.assertEqual(len(failures), 1)
            self.assertIn("Face-Regular.ttf into ubiquittykit-framework", failures[0])

        def app_with_core_and_kit_fonts(self, *kit_surfaces):
            # The app carries the Kit, and Core as the Kit's own submodule.
            self.kit_with_core_fonts(*self.SURFACES)
            self.write("Scripts/repository.txt", "app\n")
            self.write("Libraries/ubiquitty-kit/Scripts/repository.txt", "kit\n")
            (self.root / "Libraries/ubiquitty-kit/Libraries").mkdir()
            (self.root / "Libraries/ubiquitty-core").rename(self.root / "Libraries/ubiquitty-kit/Libraries/ubiquitty-core")
            self.write("Libraries/ubiquitty-kit/Resources/phosphor/Phosphor.ttf", "")
            self.write("Libraries/ubiquitty-kit/" + INVENTORY,
                       json.dumps([{"name": "Phosphor Icons", "distributionSurface": list(kit_surfaces)}]))
            self.write("Scripts/build-web.sh", 'cp "$CORE_ROOT/Resources/fonts/Face-Regular.ttf" "$STAGE/fonts/"\n'
                                               'cp "$KIT_ROOT/Resources/phosphor/Phosphor.ttf" "$STAGE/phosphor/"\n')

        def test_web_fonts_read_core_and_kit_inventories(self):
            self.app_with_core_and_kit_fonts("web")
            self.assertEqual(font_surface_errors(self.root, []), [])

        def test_a_web_surface_missing_from_kits_row_fails(self):
            self.app_with_core_and_kit_fonts("ubiquittykit-framework")
            local_rows = [{"name": "Phosphor Icons", "distributionSurface": ["web"]}]
            failures = font_surface_errors(self.root, local_rows)
            self.assertEqual(len(failures), 1)
            self.assertIn("Phosphor.ttf into web", failures[0])

    test_path = Path(__file__).resolve().parent.parent / "Tests/test-license-scope.py"
    specification = importlib.util.spec_from_file_location("license_scope_tests", test_path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    suite = unittest.TestSuite((unittest.defaultTestLoader.loadTestsFromTestCase(ProjectLicenseTests),
                               unittest.defaultTestLoader.loadTestsFromTestCase(PinTests),
                               unittest.defaultTestLoader.loadTestsFromTestCase(FontSurfaceTests),
                               unittest.defaultTestLoader.loadTestsFromModule(module)))
    result = unittest.TextTestRunner().run(suite)
    return 0 if result.wasSuccessful() else 1


if "--self-test" in sys.argv[1:]:
    sys.exit(project_license_self_test())

if len(sys.argv) > 1 and sys.argv[1] in ("--help", "-h"):
    print(__doc__.strip())
    sys.exit(0)

os.chdir(gate_corpus.root())

if not os.path.exists(INVENTORY):
    print(f"check-licenses: FAIL — {INVENTORY} absent")
    sys.exit(1)

RELEASE = "--release" in sys.argv[1:]
paths = license_scope.repository_paths(Path.cwd())
file_errors, warnings, file_counts = license_scope.file_license_audit(Path.cwd(), paths)
errors = project_license_errors(Path.cwd(), paths, file_errors)
if RELEASE:
    errors.extend(warnings)
    warnings = []
print("file-license: audited " + str(len(paths)) + " files ("
      + ", ".join(f"{count} {mode}" for mode, count in sorted(file_counts.items())) + ")")


with open(INVENTORY, encoding="utf-8") as f:
    inv = json.load(f)

inv_names = [d["name"] for d in inv]
inv_norm = [norm(n) for n in inv_names]


def has_inventory(keyword):
    return any(matches(keyword, n) for n in inv_names)


# --- 1. coverage: inventory <-> notices ------------------------------------
with open(NOTICES, encoding="utf-8") as f:
    notices = f.read()
# component names are the bold leading cell of each table row: | **Name** ...
notice_comps = re.findall(r'^\|\s*\*\*(.+?)\*\*', notices, re.MULTILINE)

for name in inv_names:
    if not any(matches(name, c) for c in notice_comps):
        errors.append(f"coverage: inventory '{name}' has no matching notice in third-party-notices.md")
for comp in notice_comps:
    if not any(matches(comp, n) for n in inv_names):
        errors.append(f"coverage: notice component '{comp}' has no inventory row")

# --- 2. discovery: every third-party input has an inventory row -------------
for d in sorted(glob.glob("Source/ThirdParty/*/")):
    dep = os.path.basename(d.rstrip("/"))
    if not has_inventory(dep):
        errors.append(f"discovery: vendored Source/ThirdParty/{dep}/ has no inventory row")
    elif not glob.glob(d + "LICENSE*") and not glob.glob(d + "COPYING*") and not glob.glob(d + "*-OFL.txt"):
        warnings.append(f"discovery: Source/ThirdParty/{dep}/ has no vendored license file")

for font in sorted(Path("Resources/fonts").glob("*")):
    if font.suffix.lower() not in {".ttf", ".otf", ".ttc", ".otc", ".woff", ".woff2"}:
        continue
    stem = font.stem
    if not has_inventory(stem):
        errors.append(f"discovery: bundled font {font} has no inventory row")


def keyword_for(token):
    t = token.lower()
    return {"awslc": "aws-lc", "ngtcp2": "ngtcp2",
            "appimagetool": "appimagetool", "appimageupdate": "appimageupdate"}.get(t, t)


if os.path.exists("Scripts/fetch-quic-deps.sh"):
    txt = open("Scripts/fetch-quic-deps.sh", encoding="utf-8").read()
    for tok in re.findall(r'^\s*([A-Z0-9]+)_TAG=', txt, re.MULTILINE):
        if not has_inventory(keyword_for(tok)):
            errors.append(f"discovery: fetched dep {tok} (fetch-quic-deps.sh) has no inventory row")
if os.path.exists("Scripts/fetch-sparkle.sh") and not has_inventory("sparkle"):
    errors.append("discovery: Sparkle (fetch-sparkle.sh) has no inventory row")
# The iOS app links every package SwiftPM resolved for the SSH carrier, not
# only the ones Package.swift names, so each pin needs a row at the version
# that ships.
SWIFTPM_PINS = "Source/Platform/Apple/SSH/Package.resolved"
if os.path.exists("Source/Platform/Apple/SSH/Package.swift"):
    if not os.path.exists(SWIFTPM_PINS):
        errors.append(f"discovery: {SWIFTPM_PINS} is missing, so the iOS app's packages are unpinned and unchecked")
    else:
        with open(SWIFTPM_PINS, encoding="utf-8") as f:
            swiftpm_pins = json.load(f).get("pins", [])
        if not swiftpm_pins:
            errors.append(f"discovery: {SWIFTPM_PINS} pins nothing, so the scan checked no package")
        rows = {d["name"]: d for d in inv}
        for pin in swiftpm_pins:
            identity = pin.get("identity", "")
            version = pin.get("state", {}).get("version") or pin.get("state", {}).get("revision", "")
            row = rows.get(identity)
            if row is None:
                errors.append(f"discovery: SwiftPM package {identity} ({SWIFTPM_PINS}) has no inventory row")
            elif str(row.get("pinnedRev", "")) != version:
                errors.append(f"discovery: SwiftPM package {identity} is pinned at {version}, "
                              f"but its inventory row says {row.get('pinnedRev')}")

if os.path.exists("Scripts/package-linux-appimage.sh"):
    txt = open("Scripts/package-linux-appimage.sh", encoding="utf-8").read()
    for tok in set(re.findall(r'(APPIMAGE\w+?)_URL', txt)):
        if not has_inventory(keyword_for(tok)):
            errors.append(f"discovery: AppImage tool {tok} has no inventory row")

# --- 2b. surface: every font a package copies is on its row's surface -----
errors.extend(font_surface_errors(Path.cwd(), inv))

# generated / embedded inputs
if glob.glob("Source/Core/Unicode/*.tables.inc") and not has_inventory("unicode"):
    errors.append("discovery: Source/Core/Unicode/*.tables.inc present but no Unicode inventory row")
if os.path.exists("Source/Core/Themes/builtin-themes.toml") and not (has_inventory("colorscheme") or has_inventory("theme")):
    errors.append("discovery: Source/Core/Themes/builtin-themes.toml present but no themes inventory row")

# The Unicode tables and the built-in themes are permanent Core inputs, unlike
# the platform fetch scripts above, which are present only where their target
# ships. A discovery keyed on `*.tables.inc` or builtin-themes.toml that finds
# nothing is the file convention having drifted, not the input being gone — and
# the "if present" guards then skip the inventory-row check in silence. Assert
# the inputs are found where this looks, so a rename fails loud rather than
# dropping a redistributed input's licence check without a word.
# Core's inputs, so only a checkout that holds Core asserts them; a repository
# that carries Core as a library leaves them to Core's own check.
if "core" in ubq_roots.repositories(str(Path.cwd())) and not glob.glob("Source/Core/Unicode/*.tables.inc"):
    errors.append("discovery: no Source/Core/Unicode/*.tables.inc found — the generated "
                  "Unicode tables were renamed or moved, so their inventory row is no "
                  "longer checked. Update the path here.")
if "core" in ubq_roots.repositories(str(Path.cwd())) and not os.path.exists("Source/Core/Themes/builtin-themes.toml"):
    errors.append("discovery: Source/Core/Themes/builtin-themes.toml is absent — the "
                  "built-in themes were renamed or moved, so their inventory row is no "
                  "longer checked. Update the path here.")

# --- 3/4. release gate: pins + blocking risk -------------------------------
for d in inv:
    surfaces = set(d.get("distributionSurface", []))
    redistributed = is_redistributed(d)
    risk = d.get("redistributionRisk", "")
    (errors if RELEASE else warnings).extend(pin_problems(d))
    if risk and risk not in ("none", "low", "needs-review"):
        if redistributed:
            (errors if RELEASE else warnings).append(
                f"risk: '{d['name']}' carries redistributionRisk '{risk}' on a redistributed surface "
                f"({', '.join(sorted(surfaces))})")
    if d.get("noticeRequired") and d.get("noticeStatus") != "present-complete":
        warnings.append(f"notice-text: '{d['name']}' notice is '{d.get('noticeStatus')}' (full text pending)")

# --- report ----------------------------------------------------------------
for w in warnings:
    print(f"  WARN  {w}")
for e in errors:
    print(f"  FAIL  {e}")
print()
mode = "release" if RELEASE else "default"
if errors:
    print(f"check-licenses ({mode}): FAILED — {len(errors)} violation(s), {len(warnings)} warning(s)")
    sys.exit(1)
print(f"check-licenses ({mode}): ok ({len(inv)} deps, {len(warnings)} warning(s))")
