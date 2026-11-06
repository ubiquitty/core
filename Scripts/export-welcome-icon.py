#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""export-welcome-icon.py — the Welcome screen's icon, from the Icon Composer document.

The Welcome screen draws the app icon as an inline image. Core owns the
screen's bytes, so the icon ships inside Core, where every arm that shows the
screen finds it without carrying a copy of its own. This script writes it, and
the results are committed:

    Resources/Icon/Welcome/Ubiquitty-Default-<N>.png    ictool's Default rendition,
                                                        N px, with its drop shadow:
                                                        the light-ground icon
    Resources/Icon/Welcome/Ubiquitty-Layers-<N>.png     the dark-ground icon's
                                                        layers above its tile fill
    Resources/Icon/Welcome/Ubiquitty-Coverage-<N>.png   its shape and its shadow
    Source/Core/App/WelcomeIconData.h                   every PNG's bytes

On a light ground the icon is the Default rendition, its black tile reading
against the light screen as the app icon does. On a dark ground the tile takes
the theme's tone instead — Core fills it with a lighter shade of the ground —
so the icon ships as layers rather than as a picture: the mark, the rim and
the glass highlights, which ictool draws over the tile, and how much of the
tile shows through each pixel. Those come from two renders of the Default
rendition with the document's fill replaced by black and by white: the black
one is everything above the fill, and the difference between the two is the
fill's share. Layers PNG: premultiplied red, green and blue above the fill,
and the fill's share in alpha. Coverage PNG: the icon's alpha in red and its
shadow's in green.

The screen gives the icon a box of 16 cells by 8 rows and draws it square, as
wide as the box at the default face: 16 cells of the 12 pt face's 7.2 pt
advance, 115.2 points. One render per backing scale, the box's side rounded —
115, 230 and 346 px for 1x, 2x and 3x — so the common case draws one texel per
device pixel (a quad within a pixel of its source is drawn at the source's
size), and any other face picks the nearest render at or above its box and is
filtered down.

Each render sits on a transparent canvas three cells wider on every side at
the default face — 158, 317 and 475 px, 22 cells — with a drop shadow as heavy
as a Dock icon's on a light desktop: the icon's alpha, blurred by 3.5% of its
side, moved down by 2.5%, black at 45%. The canvas is what the screen places,
so the icon keeps its size and place in the box and the shadow is never
clipped; the header records both sides so the screen centres the icon, not
the canvas.

    Scripts/export-welcome-icon.py          # render, then embed
    Scripts/export-welcome-icon.py --embed  # embed the committed PNGs only
    Scripts/export-welcome-icon.py --check  # the header matches the PNGs

Rendering needs Xcode's Icon Composer (ictool) and Pillow; --embed and --check
need neither and run anywhere. Run it after changing the document, and commit
what it writes.
"""

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

repository = pathlib.Path(__file__).resolve().parent.parent
document = repository / "Resources/Icon/Ubiquitty.icon"
pngDirectory = repository / "Resources/Icon/Welcome"
header = repository / "Source/Core/App/WelcomeIconData.h"
ictool = pathlib.Path(
    "/Applications/Xcode.app/Contents/Applications/Icon Composer.app/Contents/Executables/ictool"
)

# (icon side, canvas side) per backing scale.
pixelSizes = ((115, 158), (230, 317), (346, 475))
shadowBlur = 0.035
shadowDrop = 0.025
shadowOpacity = 0.45


def pngPath(kind, size):
    return pngDirectory / f"Ubiquitty-{kind}-{size}.png"


def exportRendition(source, output, size):
    subprocess.run(
        [
            str(ictool), str(source), "--export-image", "--output-file", str(output),
            "--platform", "macOS", "--rendition", "Default",
            "--width", str(size), "--height", str(size), "--scale", "1",
        ],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    from PIL import Image

    # Without ictool's Display P3 profile: the platform decoder honours one
    # and the portable decoder does not, so a tagged PNG would draw in two
    # colours. The renders are neutral greys, the same numbers in sRGB.
    image = Image.open(output).convert("RGBA")
    image.info.pop("icc_profile", None)
    return image


def withFill(scratch, name, colour):
    """A copy of the document whose fill is one solid colour."""
    copy = pathlib.Path(scratch) / f"{name}.icon"
    shutil.copytree(document, copy)
    configuration = json.loads((copy / "icon.json").read_text())
    configuration["fill"] = {"solid": colour}
    (copy / "icon.json").write_text(json.dumps(configuration, indent=2))
    return copy


def shadowAlpha(alpha, canvasSize):
    """The drop shadow's alpha on the canvas, from the icon's."""
    from PIL import Image, ImageFilter

    size = alpha.width
    inset = (canvasSize - size) // 2
    placed = Image.new("L", (canvasSize, canvasSize), 0)
    placed.paste(alpha, (inset, inset + round(size * shadowDrop)))
    blurred = placed.filter(ImageFilter.GaussianBlur(size * shadowBlur))
    return blurred.point(lambda value: round(value * shadowOpacity))


def withShadow(icon, canvasSize):
    """The icon centred on a transparent canvas over its drop shadow, as
    straight alpha, the form every decoder hands the screen."""
    from PIL import Image

    inset = (canvasSize - icon.width) // 2
    shadow = Image.new("RGBA", (canvasSize, canvasSize), (0, 0, 0, 0))
    shadow.putalpha(shadowAlpha(icon.split()[3], canvasSize))
    placed = Image.new("RGBA", (canvasSize, canvasSize), (0, 0, 0, 0))
    placed.paste(icon, (inset, inset))
    return Image.alpha_composite(shadow, placed)


def layers(overBlack, overWhite, canvasSize):
    """The dark-ground icon's two layer images, from its renders over a black
    and a white fill."""
    from PIL import Image

    size = overBlack.width
    inset = (canvasSize - size) // 2
    above = Image.new("RGBA", (canvasSize, canvasSize), (0, 0, 0, 0))
    coverage = Image.new("RGBA", (canvasSize, canvasSize), (0, 0, 0, 255))
    black = overBlack.load()
    white = overWhite.load()
    abovePixels = above.load()
    coveragePixels = coverage.load()
    for y in range(size):
        for x in range(size):
            darkTexel = black[x, y]
            lightTexel = white[x, y]
            alpha = darkTexel[3] / 255
            # Premultiplied over black: what the icon adds above its fill.
            premultiplied = tuple(round(darkTexel[channel] * alpha) for channel in range(3))
            # The fill's share: how far white moved each channel, averaged.
            share = sum((lightTexel[channel] - darkTexel[channel]) * alpha for channel in range(3)) / 3
            abovePixels[x + inset, y + inset] = premultiplied + (max(0, min(255, round(share))),)
    shadow = shadowAlpha(overBlack.split()[3], canvasSize).load()
    for y in range(canvasSize):
        for x in range(canvasSize):
            inside = 0 <= x - inset < size and 0 <= y - inset < size
            shape = black[x - inset, y - inset][3] if inside else 0
            coveragePixels[x, y] = (shape, shadow[x, y], 0, 255)
    return above, coverage


def render():
    if not ictool.exists():
        print(f"export-welcome-icon: no ictool at {ictool}; this needs Xcode", file=sys.stderr)
        return False
    pngDirectory.mkdir(exist_ok=True)
    for stale in pngDirectory.glob("Ubiquitty-*.png"):
        stale.unlink()
    with tempfile.TemporaryDirectory() as scratch:
        black = withFill(scratch, "black", "srgb:0.00000,0.00000,0.00000,1.00000")
        white = withFill(scratch, "white", "srgb:1.00000,1.00000,1.00000,1.00000")
        for size, canvasSize in pixelSizes:
            directory = pathlib.Path(scratch)
            default = exportRendition(document, directory / f"default-{size}.png", size)
            withShadow(default, canvasSize).save(pngPath("Default", size), optimize=True)
            overBlack = exportRendition(black, directory / f"black-{size}.png", size)
            overWhite = exportRendition(white, directory / f"white-{size}.png", size)
            above, coverage = layers(overBlack, overWhite, canvasSize)
            above.save(pngPath("Layers", size), optimize=True)
            coverage.save(pngPath("Coverage", size), optimize=True)
    return True


bytesPerLine = 32


def byteLiteral(data, indent):
    # Every byte as a lowercase hex escape: the literal then holds no word a
    # source scan could take for an identifier, and no escape runs into the
    # character after it.
    lines = []
    for index in range(0, len(data), bytesPerLine):
        lines.append('"' + "".join(f"\\x{byte:02x}" for byte in data[index:index + bytesPerLine]) + '"')
    return ("\n" + " " * indent).join(lines)


def pngBytes(kind, size):
    data = pngPath(kind, size).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"export-welcome-icon: {pngPath(kind, size)} is not a PNG")
    return data


def headerText():
    arrays = []
    lightEntries = []
    darkEntries = []
    for size, canvasSize in pixelSizes:
        for kind, description in (
            ("Default", "the Default rendition with its shadow"),
            ("Layers", "the dark-ground icon above its fill"),
            ("Coverage", "the dark-ground icon's shape and shadow"),
        ):
            array = f"{kind[0].lower()}{kind[1:]}{size}"
            declaration = f"inline constexpr char {array}[] = "
            arrays.append(f"/// {description[0].upper()}{description[1:]}, {size} px on {canvasSize} px, as PNG bytes.\n"
                          f"{declaration}{byteLiteral(pngBytes(kind, size), len(declaration))};\n")
        lightEntries.append(f"    IconAsset {{ {canvasSize}, {size}, std::string_view(default{size}, sizeof(default{size}) - 1), {{}} }},")
        darkEntries.append(f"    IconAsset {{ {canvasSize}, {size}, std::string_view(layers{size}, sizeof(layers{size}) - 1), "
                           f"std::string_view(coverage{size}, sizeof(coverage{size}) - 1) }},")
    joinedArrays = "\n".join(arrays)
    count = len(pixelSizes)
    lightTable = "\n".join(lightEntries)
    darkTable = "\n".join(darkEntries)
    return f"""// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Welcome screen's icon, each at {", ".join(str(size) for size, _ in pixelSizes)} px square on canvases of
// {", ".join(str(canvas) for _, canvas in pixelSizes)} px with its drop shadow, as PNG bytes: the Default rendition
// for a light ground, and for a dark ground the layers Core composites over a
// tile in the ground's own tone. Written by Scripts/export-welcome-icon.py from
// Resources/Icon/Welcome/; do not edit.

#include <array>
#include <string_view>

namespace Ubiquitty::App::Welcome::IconData {{

{joinedArrays}
/// One render of the icon.
struct IconAsset {{
    /// The canvas's side, in pixels: the icon centred on it, its shadow
    /// around it.
    int canvasPixelSize;
    /// The icon's own side on the canvas, in pixels.
    int pixelSize;
    /// The picture, or for a layered render the layers above the tile fill:
    /// premultiplied colour, and the fill's share in alpha.
    std::string_view png;
    /// For a layered render, the icon's alpha in red and its shadow's in
    /// green; empty for a picture.
    std::string_view coverage;
}};

/// The Default rendition with its shadow, drawn on a light ground, smallest first.
inline constexpr std::array<IconAsset, {count}> lightGroundIcons {{ {{
{lightTable}
}} }};

/// The layered render, composited over a tile in the ground's tone on a dark
/// ground, smallest first.
inline constexpr std::array<IconAsset, {count}> darkGroundIcons {{ {{
{darkTable}
}} }};

}} // namespace Ubiquitty::App::Welcome::IconData
"""


def main(arguments):
    if "--check" in arguments:
        if not header.exists() or header.read_text() != headerText():
            print(f"export-welcome-icon: {header.relative_to(repository)} does not match "
                  f"{pngDirectory.relative_to(repository)}; run Scripts/export-welcome-icon.py --embed",
                  file=sys.stderr)
            return 1
        print("export-welcome-icon: ok (the header is the committed PNGs)")
        return 0
    if "--embed" not in arguments and not render():
        return 1
    header.write_text(headerText())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
