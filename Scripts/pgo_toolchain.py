#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Run Linux profile work in the release image and verify its provenance."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess
import sys


def needs_image() -> bool:
    """Whether this invocation must enter the native Linux release image."""
    target = os.environ.get("UBQ_PGO_PLATFORM", platform.system())
    return target == "Linux" and not os.environ.get("UBQ_CI_LINUX_IMAGE_ID")


def enter_image(command: list[str]) -> None:
    """Replace this process with the command in the release compiler environment."""
    root = Path(__file__).resolve().parent.parent
    environment = os.environ.copy()
    environment["UBQ_CI_LINUX_IMAGE"] = "true"
    environment["UBQ_CI_LINUX_USER"] = f"{os.getuid()}:{os.getgid()}"
    if not environment.get("UBQ_BUILD_ROOT"):
        import build_root
        environment["UBQ_BUILD_ROOT"] = build_root.build_root(str(root))
    for field, setting in (("NAME", "user.name"), ("EMAIL", "user.email")):
        configured = subprocess.run(["git", "-C", str(root), "config", setting],
                                    capture_output=True, text=True).stdout.strip()
        if configured:
            environment.setdefault("GIT_AUTHOR_" + field, configured)
            environment.setdefault("GIT_COMMITTER_" + field, configured)
    launcher = root / "Scripts/ci-linux-image.sh"
    setup = 'export PATH="$UBQ_PGO_LLVM:$PATH"; export CC=clang CXX=clang++; exec "$@"'
    os.execvpe(str(launcher), [str(launcher), "bash", "-c", setup, "pgo", *command], environment)


def select_image_compiler() -> None:
    """A gate inside the image uses the release LLVM, regardless of its caller."""
    if os.environ.get("UBQ_CI_LINUX_IMAGE_ID"):
        llvm = os.environ["UBQ_PGO_LLVM"]
        os.environ["PATH"] = llvm + os.pathsep + os.environ.get("PATH", "")
        os.environ["CC"] = "clang"
        os.environ["CXX"] = "clang++"


def compiler_output(compiler: str, *arguments: str) -> str:
    """Ask the selected compiler, including any configured launcher."""
    return subprocess.run([*shlex.split(compiler), *arguments], check=True,
                          capture_output=True, text=True).stdout.strip()


def is_linux_compiler(compiler: str) -> bool:
    """Use the compiler target, not the operating system running the driver."""
    return "#define __linux__ " in compiler_output(compiler, "-dM", "-E", "-x", "c++", "/dev/null")


def identity(compiler: str) -> dict:
    """The immutable image and compiler that produce the profile's CFG hashes."""
    image = os.environ.get("UBQ_CI_LINUX_IMAGE_ID", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", image):
        raise ValueError("Linux PGO requires Scripts/ci-linux-image.sh; native builds use UBIQUITTY_PGO=OFF")
    return {"format": 1, "image": image,
            "recipe": os.environ.get("UBQ_CI_LINUX_RECIPE", ""),
            "compiler": compiler_output(compiler, "--version").splitlines()[0],
            "target": compiler_output(compiler, "-dumpmachine")}


def sidecar(profile: str) -> Path:
    """The toolchain sidecar belonging to this profile, including private outputs."""
    return Path(profile).with_suffix(".trained-toolchain")


def refusal(profile: str, compiler: str) -> str | None:
    """Explain a Linux consumer's mismatch; Apple profile policy is unchanged."""
    if not is_linux_compiler(compiler):
        return None
    try:
        current = identity(compiler)
        current["profileSha256"] = hashlib.sha256(Path(profile).read_bytes()).hexdigest()
        recorded = json.loads(sidecar(profile).read_text())
    except (OSError, ValueError) as error:
        return str(error)
    if recorded != current:
        return (f"{sidecar(profile)} does not match this release image/compiler; "
                "retrain with Scripts/pgo-train.sh in the selected release image")
    return None


def record(profile: str, compiler: str) -> None:
    """Record Linux provenance before measuring the newly trained counters."""
    if is_linux_compiler(compiler):
        recorded = identity(compiler)
        recorded["profileSha256"] = hashlib.sha256(Path(profile).read_bytes()).hexdigest()
        sidecar(profile).write_text(json.dumps(recorded, indent=2) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    run = commands.add_parser("run")
    run.add_argument("command", nargs=argparse.REMAINDER)
    for action in ("verify", "record"):
        subparser = commands.add_parser(action)
        subparser.add_argument("profile")
        subparser.add_argument("compiler")
    arguments = parser.parse_args()
    try:
        if arguments.action == "run":
            if not arguments.command:
                parser.error("run needs a command")
            enter_image(arguments.command)
        if arguments.action == "record":
            record(arguments.profile, arguments.compiler)
        else:
            reason = refusal(arguments.profile, arguments.compiler)
            if reason:
                print(reason, file=sys.stderr)
                return 1
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"pgo-toolchain: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
