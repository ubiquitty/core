#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# import-process-icons.py — refresh Resources/process-icons/ from the sources its
# catalog names.
#
#   Scripts/import-process-icons.py              fetch every icon, regenerate the Core table
#   Scripts/import-process-icons.py --offline    regenerate the Core table from the icons on disk
#
# A source is a theSVG icon (thesvg.org/icons/<slug>/<variant>.svg), an SVG in
# the program's own GitHub repository, cropped to the mark when it is a lockup
# with the program's name beside it, or the avatar of the GitHub account that
# publishes the program. Each download goes
# into a fresh temporary directory and is read as data, never executed.
#
# An SVG keeps only drawing elements and attributes: no style sheet, script,
# class, event attribute or reference outside the file. A template icon has its
# every fill made black, so a frontend tints it with the title's ink. An avatar
# is a mark on a ground: each pixel's place between the ground's colour and the
# mark's becomes its coverage, so the mark comes out alone on transparency, in
# black for a template icon and in its own colour otherwise, trimmed to a
# square. A non-PNG avatar is converted with sips, so a refresh from a GitHub
# avatar that is not a PNG runs on macOS.
#
# Source/Core/Title/ProcessIconCatalog.h is written from the catalog and the
# icons, so Core and every frontend read the same names and the same inks.

import argparse
import json
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
ICONS = ROOT / "Resources" / "process-icons"
CATALOG = ICONS / "catalog.json"
HEADER = ROOT / "Source" / "Core" / "Title" / "ProcessIconCatalog.h"
MAX_BYTES = 2 * 1024 * 1024
MAX_NAMES = 4
SVG_NS = "http://www.w3.org/2000/svg"
ALLOWED_ELEMENTS = {
    "svg", "g", "path", "circle", "ellipse", "rect", "polygon", "polyline", "line",
    "defs", "linearGradient", "radialGradient", "stop", "clipPath", "title",
}
ALLOWED_ATTRIBUTES = {
    "d", "fill", "fill-rule", "fill-opacity", "clip-rule", "clip-path", "opacity", "transform",
    "viewBox", "width", "height", "id", "x", "y", "x1", "x2", "y1", "y2", "cx", "cy", "r", "rx", "ry",
    "fx", "fy", "points", "offset", "stop-color", "stop-opacity", "gradientUnits", "gradientTransform",
}
HEX_COLOUR = re.compile(r"#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})\b")
NUMBER = re.compile(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")
ARGUMENTS = {"m": 2, "l": 2, "h": 1, "v": 1, "c": 6, "s": 4, "q": 4, "t": 2, "a": 7, "z": 0}


def fetch(url, directory):
    """Downloads url into its own file in directory; returns its bytes."""
    request = urllib.request.Request(url, headers={"User-Agent": "ubiquitty-import-process-icons"})
    with urllib.request.urlopen(request, timeout=30) as response:
        host = urllib.parse.urlparse(response.geturl()).hostname or ""
        if host not in ("thesvg.org", "avatars.githubusercontent.com", "raw.githubusercontent.com"):
            raise SystemExit(f"{url}: answered from {host}, which this import does not read")
        data = response.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise SystemExit(f"{url}: larger than {MAX_BYTES} bytes")
    (directory / "download").write_bytes(data)
    return data


def colour(text):
    """The 0xRRGGBB a #rgb or #rrggbb names, or None."""
    match = HEX_COLOUR.fullmatch(text.strip()) if text else None
    if not match:
        return None
    digits = match.group(1)
    if len(digits) == 3:
        digits = "".join(c * 2 for c in digits)
    return int(digits, 16)


def spaced_path(d):
    """Path data with every argument apart.

    An arc's two flags may be written run together and into the next number
    ("013.046" is 0, 1, 3.046); CoreSVG reads that as one number and draws the
    arc wrong, so the flags are split here, where the grammar is known.
    """
    out, position, command = [], 0, None
    while position < len(d):
        character = d[position]
        if character in " ,\t\r\n":
            position += 1
            continue
        if character.isalpha():
            command = character
            if character.lower() not in ARGUMENTS:
                raise SystemExit(f"path command {character} is not SVG")
            out.append(character)
            position += 1
            if character.lower() == "z":
                continue
        if command is None:
            raise SystemExit("path data starts with a number")
        for index in range(ARGUMENTS[command.lower()]):
            while position < len(d) and d[position] in " ,\t\r\n":
                position += 1
            if command.lower() == "a" and index in (3, 4):
                if position >= len(d) or d[position] not in "01":
                    raise SystemExit("an arc flag that is not 0 or 1")
                out.append(d[position])
                position += 1
                continue
            match = NUMBER.match(d, position)
            if not match:
                raise SystemExit(f"path data stops short at {position}: {d[position:position + 12]!r}")
            out.append(match.group(0))
            position = match.end()
    return " ".join(out)


def scaled_view(root, scale):
    """Widens an SVG's view about its centre, so its drawing stands at `scale`
    of the square it is drawn in: an icon whose shape fills its square more
    than its neighbours' do is set back to their weight."""
    if not scale or scale == 1:
        return
    x, y, width, height = (float(v) for v in root.attrib["viewBox"].replace(",", " ").split())
    wider, taller = width / scale, height / scale
    root.attrib["viewBox"] = f"{x - (wider - width) / 2:g} {y - (taller - height) / 2:g} {wider:g} {taller:g}"
    for name, value in (("width", wider), ("height", taller)):
        if name in root.attrib:
            root.attrib[name] = f"{value:g}"


def clean_svg(data, is_template, crop=None, scale=None, gradient=None):
    """The drawing in an SVG, and the first colour it paints in.

    A crop of [x, y, width, height] keeps the mark of a lockup: the view is
    narrowed to it, and a top-level shape whose drawing starts outside it, a
    letter of the name beside the mark, is left out.
    """
    ET.register_namespace("", SVG_NS)
    root = ET.fromstring(data)
    first_colour = None

    def local(tag):
        return tag.rsplit("}", 1)[-1]

    def scrub(element):
        nonlocal first_colour
        for child in list(element):
            if not isinstance(child.tag, str) or local(child.tag) not in ALLOWED_ELEMENTS:
                element.remove(child)
                continue
            scrub(child)
        for name in list(element.attrib):
            value = element.attrib[name]
            if local(name) not in ALLOWED_ATTRIBUTES or ("url(" in value and not re.fullmatch(r"url\(#[\w.-]+\)", value)):
                del element.attrib[name]
                continue
            if local(name) == "d":
                element.attrib[name] = spaced_path(value)
            if local(name) in ("fill", "stop-color"):
                if first_colour is None:
                    first_colour = colour(value)
                if is_template and value not in ("none",) and not value.startswith("url("):
                    element.attrib[name] = "#000"

    if local(root.tag) != "svg" or "viewBox" not in root.attrib:
        raise SystemExit("an SVG source is not an <svg> with a viewBox")
    if crop:
        x, y, width, height = crop
        for child in list(root):
            start = re.match(r"\s*[Mm]\s*([+-]?[\d.]+)[\s,]*([+-]?[\d.]+)", child.attrib.get("d", ""))
            if local(child.tag) == "path" and start:
                px, py = float(start.group(1)), float(start.group(2))
                if not (x <= px <= x + width and y <= py <= y + height):
                    root.remove(child)
        root.attrib["viewBox"] = f"{x} {y} {width} {height}"
        root.attrib["width"], root.attrib["height"] = str(width), str(height)
    scrub(root)
    scaled_view(root, scale)
    if gradient:
        if is_template or len(gradient) < 2 or any(not re.fullmatch(r"[0-9A-Fa-f]{6}", stop) for stop in gradient):
            raise SystemExit("a drawn gradient needs at least two RGB colours")
        x, y, width, height = (float(value) for value in root.attrib["viewBox"].split())
        definitions = ET.SubElement(root, f"{{{SVG_NS}}}defs")
        paint = ET.SubElement(definitions, f"{{{SVG_NS}}}linearGradient", {
            "id": "process-ink", "gradientUnits": "userSpaceOnUse",
            "x1": f"{x + width / 2:g}", "x2": f"{x + width / 2:g}",
            "y1": f"{y:g}", "y2": f"{y + height:g}",
        })
        for index, stop in enumerate(gradient):
            ET.SubElement(paint, f"{{{SVG_NS}}}stop", {
                "offset": f"{index / (len(gradient) - 1):g}", "stop-color": f"#{stop}",
            })
        for element in root.iter():
            if "fill" in element.attrib and element.attrib["fill"] != "none":
                element.attrib["fill"] = "url(#process-ink)"
        root.attrib["fill"] = "url(#process-ink)"
        first_colour = int(gradient[len(gradient) // 2], 16)
    return ET.tostring(root, encoding="unicode") + "\n", first_colour


def read_png(data):
    """Width, height and rows of (r, g, b, a) from an 8-bit, non-interlaced PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    position, chunks, palette, transparency = 8, [], b"", b""
    width = height = depth = kind = interlace = 0
    while position < len(data):
        (length,) = struct.unpack(">I", data[position : position + 4])
        name, body = data[position + 4 : position + 8], data[position + 8 : position + 8 + length]
        position += 12 + length
        if name == b"IHDR":
            width, height, depth, kind, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif name == b"PLTE":
            palette = body
        elif name == b"tRNS":
            transparency = body
        elif name == b"IDAT":
            chunks.append(body)
        elif name == b"IEND":
            break
    if depth != 8 or interlace:
        raise SystemExit(f"a PNG of depth {depth}, interlace {interlace}: only 8-bit progressive PNGs are read")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[kind]
    raw, stride = zlib.decompress(b"".join(chunks)), width * channels
    rows, previous, offset = [], bytearray(stride), 0
    for _ in range(height):
        method, line = raw[offset], bytearray(raw[offset + 1 : offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            if method == 1:
                line[i] = (line[i] + left) & 0xFF
            elif method == 2:
                line[i] = (line[i] + up) & 0xFF
            elif method == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif method == 4:
                estimate = left + up - corner
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 0xFF
        rows.append(line)
        previous = line
    pixels = []
    for line in rows:
        row = []
        for x in range(width):
            p = line[x * channels : (x + 1) * channels]
            if kind == 0:
                row.append((p[0], p[0], p[0], 255))
            elif kind == 2:
                row.append((p[0], p[1], p[2], 255))
            elif kind == 3:
                index = p[0]
                alpha = transparency[index] if index < len(transparency) else 255
                row.append((palette[index * 3], palette[index * 3 + 1], palette[index * 3 + 2], alpha))
            elif kind == 4:
                row.append((p[0], p[0], p[0], p[1]))
            else:
                row.append((p[0], p[1], p[2], p[3]))
        pixels.append(row)
    return width, height, pixels


def write_png(path, pixels):
    """Writes rows of (r, g, b, a) as an RGBA PNG."""
    height, width = len(pixels), len(pixels[0])
    raw = b"".join(b"\x00" + bytes(channel for pixel in row for channel in pixel) for row in pixels)

    def chunk(name, body):
        return struct.pack(">I", len(body)) + name + body + struct.pack(">I", zlib.crc32(name + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def avatar_icon(data, directory, inset, is_template, clears_edges, mark_tone):
    """The mark on an avatar's ground, alone on transparency, and its colour."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        if not shutil.which("sips"):
            raise SystemExit("this avatar is not a PNG; converting it needs macOS's sips")
        source, converted = directory / "download", directory / "converted.png"
        subprocess.run(["sips", "-s", "format", "png", str(source), "--out", str(converted)], check=True, capture_output=True)
        data = converted.read_bytes()
    width, height, pixels = read_png(data)
    x0, y0 = round(width * inset), round(height * inset)
    crop = [row[x0 : width - x0] for row in pixels[y0 : height - y0]]
    side_w, side_h = len(crop[0]), len(crop)
    # A transparent pixel is ground whatever colour it carries.
    opaque = [p for row in crop for p in row if p[3] >= 128]
    border = [crop[0][x] for x in range(side_w)] + [crop[-1][x] for x in range(side_w)]
    border += [crop[y][0] for y in range(side_h)] + [crop[y][-1] for y in range(side_h)]
    border = [p for p in border if p[3] >= 128] or opaque
    if mark_tone == "light":
        # A light mark on a tile with a light page round it: the ground is the
        # tile, the darker half of what the edge crosses.
        border = sorted(border, key=lambda p: p[0] + p[1] + p[2])[: max(1, len(border) // 2)]
    ground = tuple(sorted(p[c] for p in border)[len(border) // 2] for c in range(3))

    def distance(p):
        return sum((p[c] - ground[c]) ** 2 for c in range(3))

    far = max(distance(p) for p in opaque)
    if far == 0:
        raise SystemExit("an avatar with nothing on its ground")
    marks = [p for p in opaque if distance(p) >= 0.8 * far]
    mark = tuple(round(sum(p[c] for p in marks) / len(marks)) for c in range(3))
    axis = tuple(mark[c] - ground[c] for c in range(3))
    length = sum(a * a for a in axis)

    def place(p):
        return sum((p[c] - ground[c]) * axis[c] for c in range(3)) / length

    # Nothing the ground itself reaches counts as ink: a tile with a gradient
    # across it would otherwise leave a glow where it is lightest.
    reach = sorted(place(p) for p in border)
    floor = max(0.04, reach[int(0.98 * (len(reach) - 1))] + 0.03)

    def coverage(p):
        if p[3] < 128:
            return 0.0
        return min(1.0, max(0.0, (place(p) - floor) / (0.96 - floor)))

    cover = [[coverage(p) for p in row] for row in crop]
    if clears_edges:
        # The ink that reaches the avatar's edge is what lies outside the mark's
        # own tile, the page the avatar was cut from.
        # The fill crosses only real ink, since a compressed ground is never
        # quite empty, then takes the two pixels of soft edge beside it.
        cleared = set()
        pending = [(x, y) for x in range(side_w) for y in (0, side_h - 1)] + [(x, y) for y in range(side_h) for x in (0, side_w - 1)]
        while pending:
            x, y = pending.pop()
            if 0 <= x < side_w and 0 <= y < side_h and (x, y) not in cleared and cover[y][x] > 0.25:
                cleared.add((x, y))
                pending += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
        for x, y in cleared:
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    if 0 <= x + dx < side_w and 0 <= y + dy < side_h:
                        cover[y + dy][x + dx] = 0.0
    inked = [(x, y) for y in range(side_h) for x in range(side_w) if cover[y][x] > 0.05]
    left, right = min(x for x, _ in inked), max(x for x, _ in inked)
    top, bottom = min(y for _, y in inked), max(y for _, y in inked)
    side = max(right - left + 1, bottom - top + 1)
    margin = round(side * 0.04)
    side += 2 * margin
    ox, oy = (side - (right - left + 1)) // 2, (side - (bottom - top + 1)) // 2
    ink = (0, 0, 0) if is_template else mark
    out = [[(0, 0, 0, 0)] * side for _ in range(side)]
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            a = round(cover[y][x] * 255)
            if a:
                out[oy + y - top][ox + x - left] = (*ink, a)
    return out, (mark[0] << 16) | (mark[1] << 8) | mark[2]


def cxx_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_header(entries):
    """Source/Core/Title/ProcessIconCatalog.h from the catalog and the inks found."""
    lines = [
        "// Copyright (C) 2026 Stanislav Yaglo",
        "// SPDX-License-Identifier: Apache-2.0",
        "",
        "// Generated by Scripts/import-process-icons.py from Resources/process-icons/catalog.json.",
        "// Edit the catalog and run the script; do not edit this file.",
        "",
        "#pragma once",
        "",
        '#include "Core/Title/ProcessIcon.h"',
        "",
        "#include <array>",
        "",
        "namespace Ubiquitty::Title {",
        "",
        "/// Every program a title can mark with its icon, in the order a name is matched.",
        f"inline constexpr std::array<ProcessIcon, {len(entries)}> processIconCatalog {{ {{",
    ]
    for entry in entries:
        names = ", ".join(cxx_string(n) for n in entry["names"])
        lines += [
            "    {",
            f"        .key = {cxx_string(entry['key'])},",
            f"        .title = {cxx_string(entry['title'])},",
            f"        .file = {cxx_string(entry['file'])},",
            f"        .ink = ProcessIconInk::{'Template' if entry['ink'] == 'template' else 'Drawn'},",
            f"        .inkRGB = 0x{entry['inkRGB']:06X},",
            f"        .hasOwnPlate = {'true' if entry.get('plate') == 'own' else 'false'},",
            f"        .names = {{ {names} }},",
            f"        .nameCount = {len(entry['names'])},",
            "    },",
        ]
    lines += ["} };", "", "} // namespace Ubiquitty::Title", ""]
    HEADER.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description="Refresh Resources/process-icons/ from the sources its catalog names.")
    parser.add_argument("--offline", action="store_true", help="regenerate the Core table from the icons on disk")
    arguments = parser.parse_args()
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    entries, keys, names = [], set(), set()
    for icon in catalog["icons"]:
        key, source = icon["key"], icon["source"]
        if key in keys or not re.fullmatch(r"[a-z0-9-]+", key):
            raise SystemExit(f"{key}: a key must be unique, lowercase letters, digits and hyphens")
        keys.add(key)
        if not 1 <= len(icon["names"]) <= MAX_NAMES:
            raise SystemExit(f"{key}: between 1 and {MAX_NAMES} names")
        for name in icon["names"]:
            if name.lower() != name or name in names:
                raise SystemExit(f"{key}: '{name}' must be lowercase and claimed by one icon")
            names.add(name)
        is_template = icon["ink"] == "template"
        suffix = ".png" if source["kind"] == "github-avatar" else ".svg"
        target = ICONS / (key + suffix)
        ink_rgb = 0
        if arguments.offline:
            if not target.exists():
                raise SystemExit(f"{key}: {target.name} is missing; run without --offline")
            ink_rgb = int(icon.get("inkRGB", "0"), 16) if isinstance(icon.get("inkRGB"), str) else 0
        else:
            with tempfile.TemporaryDirectory(prefix=f"process-icon-{key}-") as scratch:
                directory = pathlib.Path(scratch)
                if source["kind"] == "thesvg":
                    data = fetch(f"https://thesvg.org/icons/{source['slug']}/{source['variant']}.svg", directory)
                    text, first = clean_svg(data, is_template, scale=icon.get("scale"), gradient=icon.get("gradient"))
                    target.write_text(text, encoding="utf-8")
                    ink_rgb = 0 if is_template else (first or 0)
                elif source["kind"] == "svg":
                    if not source["url"].startswith("https://github.com/"):
                        raise SystemExit(f"{key}: an svg source is read from the program's GitHub repository")
                    data = fetch(source["url"], directory)
                    text, first = clean_svg(data, is_template, source.get("crop"), icon.get("scale"), icon.get("gradient"))
                    target.write_text(text, encoding="utf-8")
                    ink_rgb = 0 if is_template else (first or 0)
                elif source["kind"] == "github-avatar":
                    data = fetch(f"https://github.com/{source['owner']}.png?size=460", directory)
                    pixels, mark = avatar_icon(data, directory, source.get("inset", 0.0), is_template, source.get("clears_edges", False), source.get("mark"))
                    write_png(target, pixels)
                    ink_rgb = 0 if is_template else mark
                else:
                    raise SystemExit(f"{key}: unknown source kind {source['kind']}")
            print(f"{key}: {target.name}, ink {'template' if is_template else f'#{ink_rgb:06X}'}")
        entries.append({**icon, "file": target.name, "inkRGB": ink_rgb})
    if not arguments.offline:
        recorded = {e["key"]: f"{e['inkRGB']:06X}" for e in entries}
        for icon in catalog["icons"]:
            if icon["ink"] == "drawn":
                icon["inkRGB"] = recorded[icon["key"]]
            else:
                icon.pop("inkRGB", None)
        CATALOG.write_text(json.dumps(catalog, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    write_header(entries)
    print(f"wrote {HEADER.relative_to(ROOT)} ({len(entries)} icons)")


if __name__ == "__main__":
    sys.exit(main())
