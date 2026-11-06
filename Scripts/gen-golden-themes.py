#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""One-off helper: regenerate the kGolden[] block in
Tests/Core/Themes/test_builtin_themes_toml.cpp from Source/Core/Themes/builtin-themes.toml.

The golden array is a value-exact, order-sensitive snapshot used as a no-drift
guard. Generating it from the TOML keeps the two in sync; the independent check
is the manual verification of the TOML values against upstream theme sources.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOML = ROOT / "Source/Core/Themes/builtin-themes.toml"
TEST = ROOT / "Tests/Core/Themes/test_builtin_themes_toml.cpp"


def hex_to_rgb(h):
    h = h.strip().lstrip("#")
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def parse_themes(text):
    themes = []
    cur = None
    for line in text.splitlines():
        line = line.strip()
        if line == "[[themes]]":
            cur = {}
            themes.append(cur)
        elif cur is None or not line or line.startswith("#"):
            continue
        elif line.startswith("name"):
            cur["name"] = line.split("=", 1)[1].strip().strip('"')
        elif line.startswith("is_dark"):
            cur["is_dark"] = "true" in line.split("=", 1)[1]
        elif line.startswith("ansi"):
            cur["ansi"] = re.findall(r"#[0-9a-fA-F]{6}", line)
        elif line.startswith("fg"):
            cur["fg"] = re.findall(r"#[0-9a-fA-F]{6}", line)[0]
        elif line.startswith("bg"):
            cur["bg"] = re.findall(r"#[0-9a-fA-F]{6}", line)[0]
        elif line.startswith("cursor"):
            cur["cursor"] = re.findall(r"#[0-9a-fA-F]{6}", line)[0]
    return themes


def fmt_rgb(h):
    r, g, b = hex_to_rgb(h)
    return f"{{ {r}, {g}, {b} }}"


def emit(themes):
    out = ["static const Ubiquitty::Themes::Theme kGolden[] = {", ""]
    for t in themes:
        assert len(t["ansi"]) == 16, f"{t['name']} has {len(t['ansi'])} ansi colors"
        out.append(f"    // {t['name']}.")
        out.append(f'    {{ "{t["name"]}",')
        a = [fmt_rgb(c) for c in t["ansi"]]
        out.append("        { " + ", ".join(a[0:4]) + ",")
        out.append("            " + ", ".join(a[4:8]) + ",")
        out.append("            " + ", ".join(a[8:12]) + ",")
        out.append("            " + ", ".join(a[12:16]) + " },")
        dark = "true" if t["is_dark"] else "false"
        out.append(
            f'        {fmt_rgb(t["fg"])}, {fmt_rgb(t["bg"])}, {fmt_rgb(t["cursor"])}, {dark} }},'
        )
        out.append("")
    out.append("};")
    return "\n".join(out)


def main():
    themes = parse_themes(TOML.read_text())
    block = emit(themes)
    src = TEST.read_text()
    new = re.sub(
        r"static const (?:Ubiquitty::Themes::)?Theme kGolden\[\] = \{.*?\n\};",
        block,
        src,
        count=1,
        flags=re.S,
    )
    if new == src:
        print("ERROR: kGolden block not found/replaced", file=sys.stderr)
        sys.exit(1)
    TEST.write_text(new)
    print(f"Regenerated kGolden[] with {len(themes)} themes")


if __name__ == "__main__":
    main()
