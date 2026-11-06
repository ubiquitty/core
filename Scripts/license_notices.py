#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Collects distribution notices from the dependency inventory, preserving relative links."""

import argparse
import json
import re
from pathlib import Path
import shutil
import sys


# The notices ubiquitty-core carries for what it ships: its sources, fonts and
# process icons, and the third-party texts other than the covenant every
# checkout carries and those of what only the application ships. Every other
# notice belongs to the project, except the Kit's Phosphor resources below.
CORE_NOTICES = re.compile(
    r"^(Source/Core|Source/ThirdParty|Resources/fonts|Resources/process-icons|Docs/third-party-licenses)/")
KIT_NOTICES = re.compile(r"^Resources/phosphor/")
APPLICATION_NOTICES = re.compile(
    r"^Docs/third-party-licenses/(Contributor-Covenant-|AppImage-|appimagetool-|Sparkle-|swift-)")


def notice_names(source):
    """Returns the notice names the inventory requires, validated as relative paths."""
    source = Path(source).resolve()
    inventory = json.loads((source / "Docs/third-party-inventory.json").read_text(encoding="utf-8"))
    names = {
        "LICENSE", "LICENSES/Apache-2.0.txt", "NOTICE",
        "TRADEMARKS.md", "third-party-notices.md",
        "Docs/licensing.md", "Docs/third-party-inventory.json",
        "Source/Core/Themes/theme-credits.md",
    }
    for dependency in inventory:
        paths = dependency.get("noticePath", "")
        if dependency.get("noticeRequired") and not paths.strip():
            raise ValueError(f"{dependency['name']} has no noticePath")
        names.update(path.strip() for path in paths.split(";") if path.strip())
    for name in sorted(names):
        path = Path(name)
        if path.is_absolute() or ".." in path.parts or "\n" in name or ";" in name:
            raise ValueError(f"invalid notice path: {name}")
    return sorted(names)


def notice_files(source, core=None, kit=None):
    """Returns each notice name with the file it is read from.

    A project that compiles ubiquitty-core from its own checkout names it, and
    reads the notices Core carries from there; the Kit carries its Phosphor
    notices. Without those roots the project is a single checkout. A file
    outside the checkout it is read from is refused.
    """
    source = Path(source).resolve()
    core = Path(core).resolve() if core else None
    kit = Path(kit).resolve() if kit else None
    files = {}
    for name in notice_names(source):
        owned = CORE_NOTICES.match(name) and not APPLICATION_NOTICES.match(name)
        if kit and KIT_NOTICES.match(name):
            root = kit
        else:
            root = core if core and owned else source
        path = root / name
        if not path.resolve().is_relative_to(root) or not path.is_file():
            raise ValueError(f"missing or external notice: {name}")
        files[name] = path
    return files


def notice_paths(source, core=None, kit=None):
    """Returns the sorted notice names, each backed by an existing file."""
    return sorted(notice_files(source, core, kit))


def copy_notices(source, destination, core=None, kit=None):
    """Copies the complete validated set before a package is archived or installed."""
    destination = Path(destination).resolve()
    for name, path in notice_files(source, core, kit).items():
        target = destination / name
        if not target.resolve().is_relative_to(destination):
            raise ValueError(f"external notice destination: {name}")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)


def verify_notices(source, destination, core=None, kit=None):
    """Returns the notices a staged bundle or package lacks or carries altered."""
    destination = Path(destination).resolve()
    problems = []
    for name, path in notice_files(source, core, kit).items():
        target = destination / name
        if not target.is_file():
            problems.append(f"missing {name}")
        elif target.read_bytes() != path.read_bytes():
            problems.append(f"differs {name}")
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--core", type=Path, help="the ubiquitty-core checkout, when it is not --source")
    parser.add_argument("--kit", type=Path, help="the ubiquitty-kit checkout, when it is not --source")
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--list", action="store_true")
    action.add_argument("--files", action="store_true", help="each notice and the file it is read from, tab-separated")
    action.add_argument("--copy", type=Path, metavar="DESTINATION")
    action.add_argument("--verify", type=Path, metavar="DESTINATION")
    arguments = parser.parse_args()
    try:
        if arguments.list:
            print("\n".join(notice_paths(arguments.source, arguments.core, arguments.kit)))
        elif arguments.files:
            for name, path in notice_files(arguments.source, arguments.core, arguments.kit).items():
                print(f"{name}\t{path}")
        elif arguments.verify:
            problems = verify_notices(arguments.source, arguments.verify, arguments.core, arguments.kit)
            for problem in problems:
                print(f"license notices: {arguments.verify}: {problem}", file=sys.stderr)
            return 1 if problems else 0
        else:
            copy_notices(arguments.source, arguments.copy, arguments.core, arguments.kit)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"license notices: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
