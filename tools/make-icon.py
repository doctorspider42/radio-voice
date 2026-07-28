#!/usr/bin/env python3
"""Renders res/RadioVoice.ico from the vector description below.

The icon is checked in, so this script only has to run when the artwork
changes. It is kept in the tree because a binary .ico is not reviewable and
not editable - this file is the actual source of the image.

Every size is drawn separately rather than downscaled from one master, because
strokes that read at 256 px turn into grey mush at 16 px. Small sizes drop the
transmission arcs and the grille and grow the microphone instead, which is the
only part still recognisable in a taskbar.

Requires Pillow.  Usage:  python tools/make-icon.py [output.ico]
"""

from __future__ import annotations

import struct
import sys
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

# Palette, mirroring src/gui/Theme.h so the icon and the window agree.
TILE_TOP     = (0x24, 0x29, 0x33)
TILE_BOTTOM  = (0x0E, 0x10, 0x14)
BORDER       = (0x3A, 0x41, 0x4E, 0xB0)
ACCENT       = (0x3D, 0xD1, 0xC4)
ACCENT_LIGHT = (0x7A, 0xF0, 0xE4)
ACCENT_DIM   = (0x27, 0x86, 0x7E)
GRILLE       = (0x10, 0x2E, 0x2C, 0xD0)

SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

# Sizes at or above this get the full drawing; below it, the reduced one.
FULL_DETAIL_FROM = 32


def _vertical_gradient(size: int, top: tuple, bottom: tuple) -> Image.Image:
    """A `size` x `size` RGB image fading from `top` down to `bottom`."""
    image = Image.new("RGB", (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        t = y / max(size - 1, 1)
        draw.line(
            [(0, y), (size, y)],
            fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)),
        )
    return image


def _paste_masked(target: Image.Image, colour, mask: Image.Image) -> None:
    layer = Image.new("RGBA", target.size, tuple(colour))
    target.paste(layer, (0, 0), mask)


def _paste_gradient(target: Image.Image, top, bottom, mask: Image.Image) -> None:
    layer = _vertical_gradient(target.size[0], top, bottom).convert("RGBA")
    target.paste(layer, (0, 0), mask)


def _draw(size: int, supersample: int) -> Image.Image:
    """Draws the icon at `size` * `supersample`, then downsamples once."""
    s = size * supersample
    detailed = size >= FULL_DETAIL_FROM

    def u(value: float) -> float:
        """Unit fraction of the canvas -> pixels."""
        return value * s

    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # --- rounded tile ------------------------------------------------------
    inset = u(0.025)
    radius = u(0.225)
    tile = Image.new("L", (s, s), 0)
    ImageDraw.Draw(tile).rounded_rectangle(
        (inset, inset, s - inset, s - inset), radius=radius, fill=255
    )
    _paste_gradient(image, TILE_TOP, TILE_BOTTOM, tile)

    if detailed:
        # A hairline lip along the top edge, so the tile reads as lit from above
        # rather than as a flat swatch.
        edge = Image.new("L", (s, s), 0)
        ImageDraw.Draw(edge).rounded_rectangle(
            (inset, inset, s - inset, s - inset),
            radius=radius,
            outline=255,
            width=max(1, round(u(0.008))),
        )
        _paste_masked(image, BORDER, edge)

    # Microphone geometry. The small variant grows to fill the tile, since the
    # arcs that would otherwise balance the composition are dropped.
    cx = u(0.5)
    cy = u(0.375) if detailed else u(0.40)
    half_w = u(0.115 if detailed else 0.135)
    half_h = u(0.185 if detailed else 0.215)

    # --- glow --------------------------------------------------------------
    glow = Image.new("L", (s, s), 0)
    glow_r = u(0.34)
    # Dimmer when small: at 16 px the glow is wide enough to reach the whole
    # tile, and what is left is a teal square with a teal microphone on it.
    ImageDraw.Draw(glow).ellipse(
        (cx - glow_r, cy - glow_r, cx + glow_r, cy + glow_r),
        fill=110 if detailed else 60,
    )
    glow = glow.filter(ImageFilter.GaussianBlur(glow_r * 0.55))
    _paste_masked(image, (*ACCENT, 255), glow)

    # --- transmission arcs -------------------------------------------------
    if detailed:
        arcs = ((0.285, 44, 0.030, 255), (0.375, 36, 0.026, 150))
        for radius_u, spread, width_u, alpha in arcs:
            layer = Image.new("L", (s, s), 0)
            draw = ImageDraw.Draw(layer)
            r = u(radius_u)
            box = (cx - r, cy - r, cx + r, cy + r)
            width = max(1, round(u(width_u)))
            draw.arc(box, -spread, spread, fill=255, width=width)
            draw.arc(box, 180 - spread, 180 + spread, fill=255, width=width)
            _paste_masked(image, (*ACCENT, alpha), layer)

    # --- yoke, stem and base ----------------------------------------------
    stand = Image.new("L", (s, s), 0)
    draw = ImageDraw.Draw(stand)

    yoke_r = u(0.205 if detailed else 0.235)
    yoke_cy = cy + u(0.045)
    stroke = max(1, round(u(0.045 if detailed else 0.055)))
    draw.arc(
        (cx - yoke_r, yoke_cy - yoke_r, cx + yoke_r, yoke_cy + yoke_r),
        18,
        162,
        fill=255,
        width=stroke,
    )

    stem_half = stroke / 2
    base_y = u(0.855)
    draw.rounded_rectangle(
        (cx - stem_half, yoke_cy + yoke_r - stroke, cx + stem_half, base_y),
        radius=stem_half,
        fill=255,
    )

    base_half_w = u(0.155 if detailed else 0.175)
    base_h = u(0.048 if detailed else 0.058)
    draw.rounded_rectangle(
        (cx - base_half_w, base_y - base_h, cx + base_half_w, base_y),
        radius=base_h / 2,
        fill=255,
    )
    # The stand sits behind the capsule in the large icon and can afford to be
    # darker than it; at 16 px that shade is one pixel wide and vanishes.
    _paste_masked(image, (*(ACCENT_DIM if detailed else ACCENT), 255), stand)

    # --- capsule -----------------------------------------------------------
    capsule = Image.new("L", (s, s), 0)
    ImageDraw.Draw(capsule).rounded_rectangle(
        (cx - half_w, cy - half_h, cx + half_w, cy + half_h),
        radius=half_w,
        fill=255,
    )
    _paste_gradient(image, ACCENT_LIGHT, ACCENT_DIM, capsule)

    if detailed:
        grille = Image.new("L", (s, s), 0)
        draw = ImageDraw.Draw(grille)
        line_h = max(1, round(u(0.020)))
        line_w = half_w * 0.52
        for offset in (-0.082, 0.0, 0.082):
            y = cy + u(offset)
            draw.rounded_rectangle(
                (cx - line_w, y - line_h / 2, cx + line_w, y + line_h / 2),
                radius=line_h / 2,
                fill=255,
            )
        _paste_masked(image, GRILLE, grille)

    return image.resize((size, size), Image.LANCZOS)


def _dib(image: Image.Image) -> bytes:
    """A 32-bit bottom-up DIB with an empty AND mask, as an .ico expects."""
    w, h = image.size
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, w * h * 4, 0, 0, 0, 0)

    pixels = image.load()
    rows = []
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for x in range(w):
            r, g, b, a = pixels[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))

    mask_stride = ((w + 31) // 32) * 4
    return header + b"".join(rows) + b"\x00" * (mask_stride * h)


def _png(image: Image.Image) -> bytes:
    buffer = BytesIO()
    image.save(buffer, format="PNG")
    return buffer.getvalue()


def write_ico(path: Path, images: list[Image.Image]) -> None:
    # Large sizes go in PNG-compressed - the convention since Vista, and the
    # only thing keeping a 256x256 entry from costing a quarter megabyte.
    blobs = [_png(im) if im.size[0] >= 128 else _dib(im) for im in images]

    offset = 6 + 16 * len(images)
    directory = struct.pack("<HHH", 0, 1, len(images))
    for image, blob in zip(images, blobs):
        w, h = image.size
        directory += struct.pack(
            "<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset
        )
        offset += len(blob)

    path.write_bytes(directory + b"".join(blobs))


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "res" / "RadioVoice.ico"

    # Enough supersampling that the smallest strokes still land on several
    # source pixels before the box filter sees them.
    images = [_draw(size, 16 if size <= 32 else 4) for size in SIZES]
    write_ico(output, images)

    print(f"{output} <- {', '.join(str(s) for s in SIZES)} ({output.stat().st_size} B)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
