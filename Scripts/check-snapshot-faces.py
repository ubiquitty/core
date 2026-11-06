#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Refuse a snapshot profile naming a face nobody ships, or one nobody reads.

A case's [font] family is handed to the renderer by name. A name Core ships
resolves to a file in Resources/fonts and every arm rasterises the same face; a
name it does not ship falls through to the platform's font matcher, and then
what gets drawn is whatever that box happens to have. Two boxes substitute
differently — `Monaspace Neon NF` resolves to the variable Noto Sans from
/usr/share/fonts on a self-hosted Linux runner and to this tree's static NotoSans-Regular.ttf
in a developer's container — so the goldens record one box's substitution and
the other box reads a regression that is not there. That costs the typography
cases on the Linux arm, and their goldens on every tier record a face nobody
chose.

The second bound is the reader. A case carries its own settings in `cases.json`
and the harness materialises the TOML it renders with, so a file under
`Tests/Snapshots/configs` reaches a renderer only where a script names its path
or a case names it through `native`. A profile nothing names draws nothing: it
is validated here, carried by every checkout, and edited by whoever mistakes it
for the settings a case uses. The reader is what makes a profile a contract, so
a profile without one is refused.

One kind of unshipped family is admitted: a face the platform itself carries,
named by a profile whose every case is held on that platform's text stack alone
through `native_stacks`. SF Pro is on every macOS box and cannot be shipped, so
a case about its width axis is compared where it resolves and nowhere else; the
same profile under a case that also runs on another stack is refused, because
there the name falls through to that stack's matcher. The stack is not the
whole answer: the iOS simulator shares CoreText and does not resolve SF Pro by
name, so such a case is exempted there in the iOS arm's exemption list, with
what the arm drew instead.

The check is static: the families Core's bundled table names, plus the ones it
rasterises itself, against every family a snapshot profile names; and the
tracked tree against every profile the configs directory holds. It needs no
fonts installed and no rendering, so it runs in the style job — before a golden
is minted rather than after a tier disagrees.

    ./Scripts/check-snapshot-faces.py            the tree's profiles
    ./Scripts/check-snapshot-faces.py --self-test
"""
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUNDLED = ROOT / "Source" / "Core" / "Rendering" / "BundledFonts.h"
SELECTION = ROOT / "Source" / "Core" / "Rendering" / "FontSelection.h"
CONFIGS = ROOT / "Tests" / "Snapshots" / "configs"
CASES = ROOT / "Tests" / "Snapshots" / "cases.json"
# What a reader of a profile is written in. A golden is PNG bytes and the
# configs directory is the subject rather than a reader of itself, so a
# profile's name in either says nothing about who opens it.
READER_SUFFIXES = {".py", ".sh", ".mjs", ".md", ".yml", ".yaml", ".json",
                   ".cpp", ".cc", ".h", ".mm", ".swift", ".cmake", ".txt",
                   ".tsv"}
NON_READER_DIRECTORIES = ("Tests/Snapshots/golden/", "Tests/Snapshots/configs/")
# Families a platform carries on every box of one text stack, and that stack.
# A fixture face is admitted on the same terms: Tests/Snapshots/run.py hands
# the faces under Tests/Snapshots/fixtures/fonts to the Linux arm alone, through
# fontconfig, so a profile naming one resolves only on the FreeType stack.
PLATFORM_FAMILIES = {"SF Pro": "coretext", "Ubiquitty Wdth Test": "freetype"}


def shippedFamilies(bundledText, selectionText):
    """Every family name a case may use: bundled files, plus Core's own raster.

    The bundled table pairs a family with the files under Resources/fonts. A
    family Core rasterises itself carries no file and is listed separately, in
    the selection table, so both are read rather than one.
    """
    families = set()
    for line in bundledText.splitlines():
        entry = re.match(r'\s*\{\s*"([^"]+)"\s*,\s*"[^"]+\.(?:ttf|otf)"', line)
        if entry:
            families.add(entry.group(1))
    for name in re.findall(r'FontFamilyDescriptor\s*\{\s*"([^"]+)"', selectionText):
        families.add(name)
    return families


def familiesNamedIn(profileText):
    """Every family one profile's text names, in the order it names them.

    Every key that names a face is read: `family` and `fallback`, bare or
    prefixed, such as `proportional_family` and `symbol_fallback`. A fallback
    naming an unshipped family falls through to the box's matcher exactly as a
    primary one does.
    """
    families = []
    for line in profileText.splitlines():
        if line.lstrip().startswith("#"):
            continue
        entry = re.match(r'\s*(?:\w+_)?(?:family|fallback)\s*=\s*"([^"]+)"', line)
        if entry:
            families.append(entry.group(1))
    return families


def namedFamilies(configRoot):
    """Every family a snapshot profile names, with the file that names it."""
    named = {}
    for path in sorted(configRoot.rglob("*.toml")):
        for family in familiesNamedIn(path.read_text(encoding="utf-8")):
            named.setdefault(family, []).append(str(path.relative_to(ROOT)))
    return named


def stacksHoldingProfiles(casesText):
    """The native stacks each profile is rendered on, by the cases naming it.

    A case with no `native_stacks` is held on every stack, which is spelled
    here as None so no reader has to know the list.
    """
    stacks = {}
    for case in json.loads(casesText):
        native = case.get("native")
        if not native:
            continue
        held = case.get("native_stacks")
        entry = stacks.setdefault(f"Tests/Snapshots/configs/native/{native}.toml", set())
        entry.add(None) if held is None else entry.update(held)
    return stacks


def report(shipped, named, profileStacks=None):
    """The families named but not shipped, and the lines that name them.

    - Parameters:
      - shipped: every family this project ships or rasterises itself.
      - named: each family a profile names, with the profiles naming it.
      - profileStacks: the stacks each profile is rendered on, from
        `stacksHoldingProfiles`; a platform family passes in a profile held
        on its own stack alone.
    """
    profileStacks = profileStacks or {}
    unshipped = {}
    for family, files in sorted(named.items()):
        if family in shipped:
            continue
        stack = PLATFORM_FAMILIES.get(family)
        refused = [file for file in files
                   if stack is None or profileStacks.get(file) != {stack}]
        if refused:
            unshipped[family] = refused
    return unshipped


def heldProfiles(configRoot):
    """Every profile the configs directory holds, spelled relative to it."""
    return {str(path.relative_to(configRoot))
            for path in configRoot.rglob("*.toml")}


def profilesCasesName(casesText):
    """The native profiles the case registry claims through `native`.

    A case names a profile by stem, and the runner appends the directory and
    the suffix; this spells the same path so both directions of the claim are
    one string.
    """
    return {f"native/{case['native']}.toml"
            for case in json.loads(casesText) if case.get("native")}


def profilesTreeNames(profiles, texts):
    """The profiles a file outside the configs directory names by basename.

    - Parameters:
      - profiles: the profiles the directory holds, relative to it.
      - texts: the text of every tracked file that could name one.
    """
    return {profile for profile in profiles
            if any(Path(profile).name in text for text in texts)}


def unreadProfiles(profiles, named):
    """The profiles nothing names, which no arm can be handed."""
    return sorted(profile for profile in profiles if profile not in named)


def readerTexts():
    """The text of every tracked file a profile's name can appear in.

    Tracked rather than walked: a build tree, a scratch render or an untracked
    note naming a profile is not a reader the next checkout has.
    """
    try:
        listing = subprocess.run(["git", "-C", str(ROOT), "ls-files"],
                                 capture_output=True, text=True)
    except OSError:
        return []
    texts = []
    for path in listing.stdout.split("\n"):
        if not path or path.startswith(NON_READER_DIRECTORIES):
            continue
        if Path(path).suffix not in READER_SUFFIXES:
            continue
        try:
            texts.append((ROOT / path).read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
    return texts


def selfTest():
    """Each bound fires: a shipped name passes, an unshipped one is caught."""
    shipped = shippedFamilies(
        '        { "Monaspace Neon", "MonaspaceNeon-Regular.otf" },\n'
        '        { "Noto Sans", "NotoSans-Regular.ttf" },\n',
        '    FontFamilyDescriptor { "GNU Unifont", FontRasterizerKind::Embedded },\n')
    if shipped != {"Monaspace Neon", "Noto Sans", "GNU Unifont"}:
        print(f"check-snapshot-faces --self-test: the tables parse to {shipped}")
        return 1
    caught = report(shipped, {"Monaspace Neon": ["a.toml"],
                              "Monaspace Neon NF": ["b.toml"]})
    if list(caught) != ["Monaspace Neon NF"]:
        print(f"check-snapshot-faces --self-test: the check answered {caught}")
        return 1
    if report(shipped, {"GNU Unifont": ["c.toml"]}):
        print("check-snapshot-faces --self-test: a family Core rasterises itself was refused")
        return 1
    read = familiesNamedIn('[font]\nfamily = "Primary"\nsize = 10\n'
                           '# family = "Commented Out"\n'
                           'proportional_family = "Proportional"\n'
                           'fallback = "Fallback"\nsymbol_fallback = "Symbols"\n'
                           'weight = "family"\n')
    if read != ["Primary", "Proportional", "Fallback", "Symbols"]:
        print(f"check-snapshot-faces --self-test: a profile's faces read as {read}")
        return 1
    profileNamingThePlatformFace = {"SF Pro": ["Tests/Snapshots/configs/native/platform_face.toml"]}
    heldOnItsStack = stacksHoldingProfiles(
        '[{"name": "a", "native": "platform_face", "native_stacks": ["coretext"]}]')
    if report(shipped, profileNamingThePlatformFace, heldOnItsStack):
        print("check-snapshot-faces --self-test: a platform face held on its own stack was refused")
        return 1
    heldEverywhere = stacksHoldingProfiles(
        '[{"name": "a", "native": "platform_face", "native_stacks": ["coretext"]},'
        ' {"name": "b", "native": "platform_face"}]')
    if not report(shipped, profileNamingThePlatformFace, heldEverywhere):
        print("check-snapshot-faces --self-test: a platform face on every stack passed")
        return 1
    if not report(shipped, profileNamingThePlatformFace):
        print("check-snapshot-faces --self-test: a platform face no case holds to a stack passed")
        return 1
    # Names no profile in the tree carries: a fixture naming a real one would
    # be a reader of it, and this file is inside the tree the check reads.
    held = {"opened.toml", "stale.toml", "native/opened_face.toml"}
    names = (profilesCasesName('[{"name": "a", "native": "opened_face"}]')
             | profilesTreeNames(held, ['"--config", str(HERE / "configs" / "opened.toml"),\n']))
    unread = unreadProfiles(held, names)
    if unread != ["stale.toml"]:
        print(f"check-snapshot-faces --self-test: the reader check answered {unread}")
        return 1
    print("check-snapshot-faces --self-test: ok (a shipped family passes, an "
          "unshipped one is caught, a fallback is read as a family is, "
          "Core's own raster counts as shipped, a "
          "platform face passes only where every case holds it to its stack, "
          "and a profile no script and no case names is caught)")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()
    for path in (BUNDLED, SELECTION):
        if not path.is_file():
            print(f"check-snapshot-faces: no {path.relative_to(ROOT)} to read "
                  f"the shipped families from", file=sys.stderr)
            return 2
    if not CONFIGS.is_dir():
        print("check-snapshot-faces: no Tests/Snapshots/configs to check",
              file=sys.stderr)
        return 2
    if not CASES.is_file():
        print("check-snapshot-faces: no Tests/Snapshots/cases.json to read the "
              "profiles the cases name", file=sys.stderr)
        return 2
    shipped = shippedFamilies(BUNDLED.read_text(encoding="utf-8"),
                              SELECTION.read_text(encoding="utf-8"))
    if not shipped:
        print("check-snapshot-faces: the shipped tables named no family, so "
              "this compared nothing", file=sys.stderr)
        return 2
    texts = readerTexts()
    if not texts:
        print("check-snapshot-faces: the tracked tree read as empty, so no "
              "profile could be shown a reader", file=sys.stderr)
        return 2
    named = namedFamilies(CONFIGS)
    unshipped = report(shipped, named, stacksHoldingProfiles(CASES.read_text(encoding="utf-8")))
    held = heldProfiles(CONFIGS)
    unread = unreadProfiles(
        held,
        profilesCasesName(CASES.read_text(encoding="utf-8"))
        | profilesTreeNames(held, texts))
    if unshipped:
        print("check-snapshot-faces: a snapshot profile names a family this "
              "project does not ship:")
        for family, files in unshipped.items():
            print(f'    "{family}" — named by {len(files)} profile(s)')
            for file in files[:6]:
                print(f"        {file}")
        print("  A family Core ships resolves to a file under Resources/fonts "
              "and every arm draws the same face. One it does not ship falls "
              "through to the platform's matcher, so the render is whatever "
              "that box has installed — and the golden records one box's "
              "substitution. Name a shipped family, ship the face, or — for "
              "a face the platform carries on every box — hold every case "
              "naming the profile to that stack with `native_stacks`.")
        print(f"  Shipped: {', '.join(sorted(shipped))}")
    if unread:
        print("check-snapshot-faces: a snapshot profile has no reader:")
        for profile in unread:
            print(f"    Tests/Snapshots/configs/{profile}")
        print("  A case's TOML is materialised from its settings in "
              "Tests/Snapshots/cases.json, so a file here is rendered only "
              "where a script names its path or a case names it through "
              "`native`. Name it from the script that wants it, or delete it.")
    if unshipped or unread:
        return 1
    print(f"check-snapshot-faces: ok ({len(named)} family name(s) across "
          f"{len(held)} profile(s), each family one this project ships and "
          f"each profile named by a reader)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
