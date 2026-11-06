# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Images & graphics: Sixel, the Kitty graphics protocol and iTerm2's OSC 1337
inline image, all drawing the same in-code RGBA test image.

The escape blobs come from the toolkit's image helpers (`test_image`,
`image_sixel`, `image_kitty`, `image_iterm2`) so every entry sends the same
pixels, and each blob doubles as the entry's snapshot sequence.
"""

import base64

from .. import tui as T
from ..tui import move
from . import Entry

TITLE = "Images and graphics"
ORDER = 70

# A small fixed test image keeps the escape blobs bounded; the gradient makes
# orientation and color fidelity obvious at a glance.
_IMG_W = 64
_IMG_H = 64


def _gradient():
    return T.test_image(_IMG_W, _IMG_H, "gradient")


def _image_page(tui, caption, label, blob):
    """One picture under one sequence label."""
    row = tui.page(TITLE, caption)
    tui.label(row, 4, label)
    tui.write(move(row + 2, 6), blob)
    return tui.footer()


# ---------------------------------------------------------------------------
# Single-protocol images
# ---------------------------------------------------------------------------

_SIXEL_SEQ = T.image_sixel(*_gradient())
_KITTY_SEQ = T.image_kitty(*T.test_image(_IMG_W, _IMG_H, "checkerboard"))
# The animation stream is a=T base + five a=f frames + a=a c=3 (loop). Playback
# is timer-driven, so the golden holds whichever frame the capture caught.
_KITTY_ANIM_SEQ = T.image_kitty_animation(
    _IMG_W, _IMG_H, T.test_image_frames(_IMG_W, _IMG_H, count=6),
    image_id=42, loop=True)
_ITERM2_SEQ = T.image_iterm2(*_gradient(), cells_wide=16, cells_high=8)


def _demo_sixel(tui):
    return _image_page(
        tui, "A gradient, green to red across and blue down, in sixteen flat blocks.",
        "DCS 0 ; 0 ; 0 q ... ST   64 x 64, four levels per channel, 2:1 aspect",
        _SIXEL_SEQ)


def _demo_kitty(tui):
    return _image_page(
        tui, "A black-and-white checkerboard sits at the cursor at its own pixel size.",
        "APC G a=T,f=32,q=2 ... ST   64 x 64 RGBA in 4096-byte chunks",
        _KITTY_SEQ)


def _demo_kitty_animation(tui):
    return _image_page(
        tui, "A cyan bar crosses the dark blue field from left to right and starts over.",
        "APC G a=T, then five a=f frames 120 ms apart, then a=a c=3 to loop",
        _KITTY_ANIM_SEQ)


def _demo_iterm2(tui):
    return _image_page(
        tui, "The square gradient fits inside a box 16 cells wide and 8 rows tall.",
        "OSC 1337 ; File=inline=1;width=16;height=8 : <base64 PNG> BEL",
        _ITERM2_SEQ)


# ---------------------------------------------------------------------------
# The same image in all three protocols
# ---------------------------------------------------------------------------

_COMPARE_SEQ = (
    move(7, 6) + _SIXEL_SEQ
    + move(7, 32) + T.image_kitty(*_gradient())
    + move(7, 58) + _ITERM2_SEQ
)


def _demo_compare(tui):
    row = tui.page(TITLE, "The same gradient three times, once per protocol.")
    for col, name in ((6, "Sixel"), (32, "Kitty graphics"), (58, "OSC 1337")):
        tui.label(row, col, name)
    tui.write(_COMPARE_SEQ)
    return tui.footer()


# ---------------------------------------------------------------------------
# OSC 1337 sizing: cells, pixels, percent, preserveAspectRatio
# ---------------------------------------------------------------------------

def _iterm2_fit(w, h, rgba, width=None, height=None, preserve=None):
    """An OSC 1337 File= image with raw width/height tokens and an aspect flag."""
    png = T._png_bytes(w, h, rgba)
    args = [f"name={base64.b64encode(b'fit.png').decode('ascii')}",
            f"size={len(png)}", "inline=1"]
    if width is not None:
        args.append(f"width={width}")
    if height is not None:
        args.append(f"height={height}")
    if preserve is not None:
        args.append(f"preserveAspectRatio={preserve}")
    return f"{T.OSC}1337;File={';'.join(args)}:{base64.b64encode(png).decode('ascii')}{T.BEL}"


_FIT_SEQ = (move(8, 6) + _iterm2_fit(*_gradient(), width=12, height=6, preserve=1)
            + move(8, 30) + _iterm2_fit(*_gradient(), width="160px", height="80px", preserve=0)
            + move(8, 56) + _iterm2_fit(*_gradient(), width="30%", height="auto", preserve=1))


def _demo_fit(tui):
    row = tui.page(TITLE, "One square image in three boxes: fitted, stretched, and sized by width.")
    tui.label(row, 6, "width=12;height=6")
    tui.label(row + 1, 6, "preserveAspectRatio=1")
    tui.label(row, 30, "width=160px;height=80px")
    tui.label(row + 1, 30, "preserveAspectRatio=0")
    tui.label(row, 56, "width=30%;height=auto")
    tui.write(_FIT_SEQ)
    return tui.footer()


# ---------------------------------------------------------------------------
# Kitty z-order: an image under the text
# ---------------------------------------------------------------------------

_UNDER_LINES = ("This text is printed after the image,",
                "over the same cells. The image was",
                "placed with z=-1, so it stays behind",
                "every glyph and the words read on top.")


def _under_text(top, left):
    w, h, rgba = _gradient()
    out = move(top, left) + T._kitty_chunked(
        f"a=T,f=32,s={w},v={h},c=40,r={len(_UNDER_LINES)},z=-1,C=1,q=2", rgba)
    for offset, line in enumerate(_UNDER_LINES):
        out += move(top + offset, left) + line
    return out


_UNDER_SEQ = _under_text(2, 4)


ENTRIES = [
    Entry(
        title="Sixel image (DCS q)",
        explanation=(
            "A 64x64 gradient at the cursor, green at the left edge, red at the "
            "right, blue rising down the rows, quantized to sixteen flat blocks. "
            "P1=0 asks for the 2:1 sixel aspect, so it is twice as tall as wide, "
            "and the cursor drops below it."),
        run=_demo_sixel,
        sequence=_SIXEL_SEQ,
        golden="images_sixel_gradient",
    ),
    Entry(
        title="Kitty graphics (APC G)",
        explanation=(
            "A 64x64 checkerboard of 8-pixel tiles at the cursor cell, at its own "
            "pixel size since no c= or r= is sent. q=2 suppresses the OK reply."),
        run=_demo_kitty,
        sequence=_KITTY_SEQ,
        golden="images_kitty_checkerboard",
    ),
    Entry(
        title="Kitty animation (a=f frames)",
        explanation=(
            "a=T places the base frame under image id 42, a=f appends five more "
            "with their delays on z=, and a=a c=3 loops them: a cyan bar crosses "
            "a dark blue field and starts over. A bar parked at the left means "
            "the animation never advanced."),
        run=_demo_kitty_animation,
        sequence=_KITTY_ANIM_SEQ,
    ),
    Entry(
        title="Inline image (OSC 1337 File=)",
        explanation=(
            "The square gradient, sent as a PNG, fits inside a box 16 cells wide "
            "and 8 rows tall with its aspect preserved."),
        run=_demo_iterm2,
        sequence=_ITERM2_SEQ,
        golden="images_iterm2_gradient",
    ),
    Entry(
        title="Same image in three protocols",
        explanation=(
            "The same gradient under each label: Sixel stepped to four levels per "
            "channel and doubled in height, Kitty at 64x64 pixels, OSC 1337 "
            "fitted to its 16x8-cell box."),
        run=_demo_compare,
        sequence=_COMPARE_SEQ,
        golden="images_compare_three",
    ),
    Entry(
        title="Image fit modes (OSC 1337 width, height)",
        explanation=(
            "A bare N counts cells, Npx device pixels, N% a share of the grid "
            "width, and auto derives one axis from the other. "
            "preserveAspectRatio=1 fits the square inside its 12x6-cell box; =0 "
            "stretches it to exactly 160x80 pixels."),
        run=_demo_fit,
        sequence=_FIT_SEQ,
        golden="images_fit_modes",
    ),
]
