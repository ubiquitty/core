#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

r"""Render a PNG from arbitrary text and config.

Usage:
    Scripts/render-png.py --text 'Hello\nWorld' --output out.png
    Scripts/render-png.py --feed feed.txt --output out.png
    Scripts/render-png.py --text '\e[31mRed\e[0m' --config macos_default.toml --output out.png
    Scripts/render-png.py --text 'Hello' --backend macos-swift --output out.png
    Scripts/render-png.py --text 'Hello' --backend software --output out.png

Backends:
    macos-swift    macOS binary, Swift encoder (Metal compositor)
    macos-objc     macOS binary, native ObjC++ encoder (Metal compositor)
    software       Standalone portable binary (software compositor)
    portable       Alias for software
    swift          Alias for macos-swift (Swift is the same on iOS and macOS)
    macos          Alias for macos-objc
    linux          Linux binary via OrbStack/Docker (software compositor)
    wscons         BSD console (unsupported by this script)
    ios            Alias for macos-swift

The script invokes the snapshot binary directly, bypassing the regression
harness. No staleness guards, no golden comparison — just render and write.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_ROOT = ROOT / "build"

# Fallback: if build directory doesn't exist here, check the main repo
if not BUILD_ROOT.exists():
    _alt = ROOT.parent / "ubiquitty" / "build"
    if _alt.exists():
        BUILD_ROOT = _alt

# Canonical backend names and their snapshot-backend token + binary path.
# The macOS binary accepts three --snapshot-backend values: portable, swift, macos.
# The Linux binary only accepts linux.
MACOS_BINARY = (BUILD_ROOT / "macos" / "Source" / "Platform" /
                "macOS" / "Ubiquitty.app" / "Contents" / "MacOS" /
                "Ubiquitty")
PORTABLE_BINARY = BUILD_ROOT / "core" / "ubiquitty-snapshot"
LINUX_SHIM = ROOT / "Scripts" / "linux-snapshot-shim.sh"

BACKENDS = {
    "macos-swift":  {"token": "swift",   "binary": MACOS_BINARY,
                     "compositor": "metal", "encoder": "swift",
                     "desc": "Metal compositor, Swift shell surface encode"},
    "macos-objc":   {"token": "macos",   "binary": MACOS_BINARY,
                     "compositor": "metal", "encoder": "swift",
                     "desc": "Metal compositor, default encode path"},
    "software":     {"token": "portable", "binary": PORTABLE_BINARY,
                     "compositor": "software", "encoder": None,
                     "desc": "Software compositor, standalone portable binary"},
    "linux":        {"token": "linux",   "binary": LINUX_SHIM,
                     "compositor": "software", "encoder": None,
                     "desc": "Software compositor via OrbStack/Docker"},
    "wscons":       {"token": None,      "binary": None,
                     "compositor": None, "encoder": None,
                     "desc": "BSD console (not yet implemented)"},
}

# Friendly aliases
ALIASES = {
    "portable": "software",
    "swift":    "macos-swift",
    "macos":    "macos-objc",
    "ios":      "macos-swift",
}

DEFAULT_CONFIG = ROOT / "Tests" / "Snapshots" / "configs" / "portable_solarized_light.toml"


def resolve_backend(name):
    """Resolve a backend name (with aliases) to a canonical backend dict."""
    canonical = ALIASES.get(name, name)
    if canonical not in BACKENDS:
        valid = sorted(set(list(BACKENDS.keys()) + list(ALIASES.keys())))
        print(f"Error: unknown backend '{name}'", file=sys.stderr)
        print(f"Available backends: {', '.join(valid)}", file=sys.stderr)
        sys.exit(1)
    return canonical, BACKENDS[canonical]


def find_binary(backend_name, info):
    """Locate the snapshot binary for the given backend."""
    # Environment variable override (keyed on canonical name)
    env_key = f"UBIQUITTY_{backend_name.upper().replace('-', '_')}_BIN"
    if env_key in os.environ:
        return Path(os.environ[env_key])

    # Also check the legacy keys used by run.py
    for legacy_key in ["UBIQUITTY_SNAPSHOT_BIN", "UBIQUITTY_MACOS_BIN",
                        "UBIQUITTY_LINUX_BIN", "UBIQUITTY_WEB_BIN"]:
        if legacy_key in os.environ:
            return Path(os.environ[legacy_key])

    candidate = info["binary"]
    if candidate is None:
        return None
    return candidate


def render(backend_name, info, config, text=None, feed_file=None,
           columns=80, rows=24, output=None, extra_args=None,
           scale=None):
    """Render a snapshot and return the output path."""
    binary = find_binary(backend_name, info)
    if binary is None or not binary.is_file():
        print(f"Error: {backend_name} binary not found at {binary}", file=sys.stderr)
        print("Build the project first, or set the environment variable:", file=sys.stderr)
        print(f"  UBIQUITTY_{backend_name.upper().replace('-', '_')}_BIN=/path/to/binary",
              file=sys.stderr)
        sys.exit(1)

    arguments = [
        str(binary),
        "--snapshot", str(output),
        "--snapshot-backend", info["token"],
        "--columns", str(columns),
        "--rows", str(rows),
        "--config", str(config),
    ]

    if scale is not None:
        arguments.extend(["--scale", str(scale)])

    if feed_file:
        arguments.extend(["--feed-file", str(feed_file)])
    elif text:
        arguments.extend(["--feed", text])
    else:
        print("Error: must provide --text or --feed", file=sys.stderr)
        sys.exit(1)

    if extra_args:
        arguments.extend(extra_args)

    result = subprocess.run(arguments, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        print(f"Error: snapshot binary exited with code {result.returncode}",
              file=sys.stderr)
        if result.stdout:
            print(f"stdout: {result.stdout}", file=sys.stderr)
        if result.stderr:
            print(f"stderr: {result.stderr}", file=sys.stderr)
        sys.exit(1)

    if result.stdout:
        print(result.stdout, end="")

    return output


def main():
    backend_choices = sorted(set(list(BACKENDS.keys()) + list(ALIASES.keys())))

    parser = argparse.ArgumentParser(
        description="Render a PNG from arbitrary text and config.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--text", "-t",
                        help="Text to render (supports ANSI escape sequences). "
                             "Use \\e for ESC, \\n for newline, \\r for carriage return.")
    parser.add_argument("--feed", "-f",
                        help="File containing the feed text (raw bytes, not interpreted).")
    parser.add_argument("--crlf", action="store_true",
                        help="Convert LF (\\n) to CRLF (\\r\\n) in feed files. "
                             "Terminal emulation requires \\r\\n for newlines; bare \\n "
                             "moves down but not to column 0, causing text overlap.")
    parser.add_argument("--output", "-o",
                        help="Output PNG path.")
    parser.add_argument("--config", "-c", default=str(DEFAULT_CONFIG),
                        help=f"TOML config file (default: {DEFAULT_CONFIG.name}).")
    parser.add_argument("--backend", "-b", default="software",
                        choices=backend_choices,
                        help="Renderer backend (default: software).")
    parser.add_argument("--columns", type=int, default=80,
                        help="Grid columns (default: 80).")
    parser.add_argument("--rows", type=int, default=24,
                        help="Grid rows (default: 24).")
    parser.add_argument("--scale", type=float, default=None,
                        help="Device scale factor (e.g. 2.0 for Retina).")
    parser.add_argument("--arg", action="append", default=[],
                        help="Extra argument passed to the binary (repeatable).")
    parser.add_argument("--list-backends", action="store_true",
                        help="List available backends and exit.")

    args = parser.parse_args()

    if args.list_backends:
        print("Available backends:")
        for name in sorted(BACKENDS.keys()):
            info = BACKENDS[name]
            token = info["token"] or "(n/a)"
            binary = info["binary"]
            status = "ok" if binary and binary.is_file() else "not built"
            print(f"  {name:15s}  [{status}]  {info.get('desc', '')}")
        print("\nAliases:")
        for alias, canonical in sorted(ALIASES.items()):
            print(f"  {alias:15s} -> {canonical}")
        return

    if not args.text and not args.feed:
        parser.error("must provide --text or --feed")

    if not args.output:
        parser.error("must provide --output")

    canonical, info = resolve_backend(args.backend)

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    text = None
    feed_file = None

    if args.feed:
        feed_file = Path(args.feed)
        if not feed_file.is_file():
            parser.error(f"feed file not found: {feed_file}")
        if args.crlf:
            # Read, convert LF to CRLF, write to temp file
            contents = feed_file.read_bytes()
            contents = contents.replace(b"\n", b"\r\n")
            tmp = tempfile.NamedTemporaryFile(suffix=".feed", delete=False)
            tmp.write(contents)
            tmp.close()
            feed_file = Path(tmp.name)
    elif args.text:
        # Pass the raw string to the binary — decodeEscapes handles \e \n \r \a etc.
        text = args.text

    config = Path(args.config)
    if not config.is_file():
        # Try relative to Tests/Snapshots/configs/
        candidate = ROOT / "Tests" / "Snapshots" / "configs" / config.name
        if candidate.is_file():
            config = candidate
        else:
            parser.error(f"config not found: {config}")

    render(
        backend_name=canonical,
        info=info,
        config=config,
        text=text,
        feed_file=feed_file,
        columns=args.columns,
        rows=args.rows,
        output=output,
        extra_args=args.arg,
        scale=args.scale,
    )

    print(f"Wrote {output} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
