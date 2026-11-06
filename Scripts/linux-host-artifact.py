#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Identify and verify the static Linux hosts bundled by the macOS builder."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MACHINES = {"x86_64": 62, "arm64": 183}
INPUTS = ("CMakeLists.txt", "VERSION", "cmake", "Source", "Scripts", "Resources", "Support")


def source_key(root, arch, image, quic, version, identity):
    """Hash build inputs without depending on checkout paths or file timestamps."""
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    tracked = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
                              "--exclude-standard", "--", *INPUTS], env=environment,
                             capture_output=True, check=False)
    if tracked.returncode == 0 and tracked.stdout:
        names = sorted(set(os.fsdecode(name) for name in tracked.stdout.split(b"\0") if name))
    else:
        names = sorted(str(path.relative_to(root)) for name in INPUTS
                       for path in ([root / name] if (root / name).is_file() else (root / name).rglob("*"))
                       if path.is_file() and "__pycache__" not in path.parts)
    digest = hashlib.sha256(json.dumps([arch, image, quic.upper(), version, identity]).encode())
    for name in names:
        path = root / name
        if not path.is_file():
            continue
        digest.update(os.fsencode(name) + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def verify_static_elf(data, arch):
    """Reject the wrong architecture, an interpreter, and shared dependencies."""
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected a little-endian ELF64 executable")
    kind, machine = struct.unpack_from("<HH", data, 16)
    if kind not in (2, 3) or machine != MACHINES[arch]:
        raise ValueError(f"expected an executable for Linux {arch}")
    offset = struct.unpack_from("<Q", data, 32)[0]
    entry_size, count = struct.unpack_from("<HH", data, 54)
    if entry_size < 56 or not count or offset + entry_size * count > len(data):
        raise ValueError("invalid ELF program headers")
    for index in range(count):
        entry = offset + entry_size * index
        segment = struct.unpack_from("<I", data, entry)[0]
        start = struct.unpack_from("<Q", data, entry + 8)[0]
        size = struct.unpack_from("<Q", data, entry + 32)[0]
        if start + size > len(data):
            raise ValueError("truncated ELF segment")
        if segment == 3:
            raise ValueError("host requires a dynamic loader")
        if segment == 2:
            if size % 16:
                raise ValueError("invalid ELF dynamic table")
            for at in range(start, start + size, 16):
                tag = struct.unpack_from("<q", data, at)[0]
                if tag == 0:
                    break
                if tag == 1:
                    raise ValueError("host requires a shared library")


def manifest_path(binary):
    return binary.with_name(binary.name + ".json")


def verify_artifact(binary, arch, key):
    data = binary.read_bytes()
    verify_static_elf(data, arch)
    expected = {"format": 1, "key": key, "sha256": hashlib.sha256(data).hexdigest()}
    if json.loads(manifest_path(binary).read_text()) != expected:
        raise ValueError("host does not match these build inputs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("key", "check", "record"))
    parser.add_argument("--arch", required=True, choices=MACHINES)
    parser.add_argument("--image", default="")
    parser.add_argument("--quic", default="ON")
    parser.add_argument("--version", default="")
    parser.add_argument("--identity", default="")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--key")
    args = parser.parse_args()
    try:
        if args.action == "key":
            print(source_key(ROOT, args.arch, args.image, args.quic, args.version, args.identity))
        else:
            if args.binary is None or args.key is None:
                parser.error("check and record need --binary and --key")
            if args.action == "check":
                verify_artifact(args.binary, args.arch, args.key)
            else:
                data = args.binary.read_bytes()
                verify_static_elf(data, args.arch)
                manifest = {"format": 1, "key": args.key, "sha256": hashlib.sha256(data).hexdigest()}
                destination = manifest_path(args.binary)
                temporary = destination.with_suffix(".json.tmp")
                temporary.write_text(json.dumps(manifest, sort_keys=True) + "\n")
                temporary.replace(destination)
    except (OSError, ValueError, struct.error) as error:
        print(f"linux-host-artifact: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
