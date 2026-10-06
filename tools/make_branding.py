#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 PokeMMO-Prospero contributors
"""Generates the title's branding from the project logo (assets/branding/logo-source.png).

  python3 tools/make_branding.py [path/to/DejaVuSans.ttf]

Writes (all committed, so the Docker build needs neither Pillow nor NumPy):
  assets/branding/icon0.png      512x512 home screen icon
  assets/branding/background.dds 3840x2160 BC7 home screen background (used as pic0.dds and pic1.dds)
  loader/src/overlay_assets.c    the loader's overlay font (DejaVu Sans, plus four controller button symbols) and logo,
                                 zlib-compressed

Needs Pillow and NumPy. DejaVu fonts: Bitstream Vera / DejaVu license (see CREDITS.md).
"""
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
BRANDING = ROOT / "assets/branding"
FONT = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
NAVY = (8, 18, 40)        # background: a little lighter than the logo's silhouette
NAVY_DEEP = (3, 8, 22)

logo = Image.open(BRANDING / "logo-source.png").convert("RGBA")
bbox = logo.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
logo = logo.crop(bbox)


def backdrop(width, height, centre):
    """Dark navy with a soft blue glow around `centre` (x, y in pixels)."""
    y, x = np.mgrid[0:height, 0:width].astype(np.float32)
    distance = np.hypot(x - centre[0], y - centre[1]) / max(width, height)
    glow = np.clip(1.0 - distance * 1.6, 0.0, 1.0) ** 2
    deep, light = np.array(NAVY_DEEP, np.float32), np.array((24, 52, 104), np.float32)
    pixels = deep + (light - deep) * glow[..., None]
    return Image.fromarray(pixels.round().astype(np.uint8), "RGB")


def place(image, size, centre):
    scaled = logo.resize((size, size * logo.height // logo.width), Image.LANCZOS)
    shadow = Image.new("RGBA", image.size, (0, 0, 0, 0))
    mask = scaled.getchannel("A").point(lambda a: a * 0.55)
    shadow.paste((0, 0, 0, 255), (centre[0] - scaled.width // 2 + size // 60, centre[1] - scaled.height // 2 + size // 40), mask)
    shadow = shadow.filter(ImageFilter.GaussianBlur(size // 40))
    out = image.convert("RGBA")
    out.alpha_composite(shadow)
    out.alpha_composite(scaled, (centre[0] - scaled.width // 2, centre[1] - scaled.height // 2))
    return out.convert("RGB")


# ---- home screen icon ----------------------------------------------------------------------------------------------------------
icon = place(backdrop(512, 512, (256, 256)), 440, (256, 256))
icon.save(BRANDING / "icon0.png", optimize=True)

# ---- home screen background: BC7 (mode 6), 3840x2160, one mip level, DX10 header -------------------------------------------------
background = place(backdrop(3840, 2160, (2700, 1080)), 1500, (2700, 1080))
rgba = np.asarray(background.convert("RGBA"), dtype=np.int32)
H, W = rgba.shape[:2]
blocks = rgba.reshape(H // 4, 4, W // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
low, high = blocks.min(axis=1), blocks.max(axis=1)
# Endpoints: 7 bits per channel plus a shared p-bit of 0 (an 8-bit value rounded to even).
e0, e1 = (low >> 1), (high >> 1)
q0, q1 = (e0 << 1).astype(np.float32), (e1 << 1).astype(np.float32)
axis = q1 - q0
length = (axis * axis).sum(axis=1)
t = ((blocks - q0[:, None, :]) * axis[:, None, :]).sum(axis=2) / np.where(length > 0, length, 1)[:, None]
indices = np.clip(np.rint(t * 15), 0, 15).astype(np.int64)
swap = indices[:, 0] >= 8  # the first index is stored with 3 bits: its top bit must be 0
e0[swap], e1[swap] = e1[swap].copy(), e0[swap].copy()
indices[swap] = 15 - indices[swap]
bits = []  # (value array, width) in stream order, least significant first
bits.append((np.full(len(blocks), 1 << 6, np.int64), 7))  # mode 6
for channel in range(4):
    bits.append((e0[:, channel].astype(np.int64), 7))
    bits.append((e1[:, channel].astype(np.int64), 7))
bits.append((np.zeros(len(blocks), np.int64), 1))
bits.append((np.zeros(len(blocks), np.int64), 1))
for i in range(16):
    bits.append((indices[:, i], 3 if i == 0 else 4))
lo = np.zeros(len(blocks), np.uint64)
hi = np.zeros(len(blocks), np.uint64)
offset = 0
for value, width in bits:
    for b in range(width):
        bit = ((value >> b) & 1).astype(np.uint64)
        position = offset + b
        if position < 64:
            lo |= bit << np.uint64(position)
        else:
            hi |= bit << np.uint64(position - 64)
    offset += width
assert offset == 128
payload = np.stack([lo, hi], axis=1).astype("<u8").tobytes()
header = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, H, W, len(payload), 0, 1)
header += b"\0" * 44 + struct.pack("<2I", 32, 0x4) + b"DX10" + b"\0" * 20 + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
header += struct.pack("<5I", 98, 3, 0, 1, 0)
(BRANDING / "background.dds").write_bytes(header + payload)

# ---- overlay font ---------------------------------------------------------------------------------------------------------------
SIZE = 48
font = ImageFont.truetype(FONT, SIZE)
ascent, descent = font.getmetrics()
LINE = ascent + descent
ATLAS_W, ATLAS_H = 1024, 512
atlas = Image.new("L", (ATLAS_W, ATLAS_H), 0)
draw = ImageDraw.Draw(atlas)
glyphs = {}
x = y = 2
SYMBOLS = {1: "cross", 2: "circle", 3: "square", 4: "triangle"}  # the controller's face buttons, drawn as shapes


def reserve(width):
    global x, y
    if x + width + 2 > ATLAS_W:
        x, y = 2, y + LINE + 4
    assert y + LINE + 2 <= ATLAS_H, "atlas full"
    origin = (x, y)
    x += width + 4
    return origin


for code in list(range(32, 127)) + list(SYMBOLS):
    if code in SYMBOLS:
        width = advance = LINE * 3 // 4
        ox, oy = reserve(width)
        s, m = width, width // 8
        top = oy + (LINE - s) // 2
        box = (ox + m, top + m, ox + s - m, top + s - m)
        stroke = max(3, s // 9)
        kind = SYMBOLS[code]
        if kind == "cross":
            draw.line((box[0], box[1], box[2], box[3]), fill=255, width=stroke)
            draw.line((box[0], box[3], box[2], box[1]), fill=255, width=stroke)
        elif kind == "circle":
            draw.ellipse(box, outline=255, width=stroke)
        elif kind == "square":
            draw.rectangle(box, outline=255, width=stroke)
        else:
            draw.polygon(((box[0] + box[2]) // 2, box[1], box[2], box[3], box[0], box[3]), outline=255, width=stroke)
        glyphs[code] = (ox, oy, width, LINE, advance)
        continue
    ch = chr(code)
    advance = font.getlength(ch)
    left, _, right, _ = font.getbbox(ch)
    width = max(1, int(np.ceil(max(right, advance) - min(left, 0))) + 2)
    ox, oy = reserve(width)
    draw.text((ox - min(left, 0) + 1, oy), ch, font=font, fill=255)
    glyphs[code] = (ox, oy, width, LINE, advance, -min(left, 0) + 1)

font_alpha = zlib.compress(atlas.tobytes(), 9)
logo_size = 400
logo_rgba = logo.resize((logo_size, logo_size * logo.height // logo.width), Image.LANCZOS)
logo_bytes = zlib.compress(logo_rgba.tobytes(), 9)


def c_bytes(data):
    return "\n".join("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 20]) + "," for i in range(0, len(data), 20))


rows = []
for code in range(128):
    g = glyphs.get(code)
    if not g:
        rows.append("    {0, 0, 0, 0, 0.0f, 0.0f},")
        continue
    bearing = g[5] if len(g) > 5 else 0
    rows.append(f"    {{{g[0]}, {g[1]}, {g[2]}, {g[3]}, {g[4]:.3f}f, {bearing:.3f}f}},")

source = f"""// SPDX-License-Identifier: GPL-3.0-or-later
// Generated by tools/make_branding.py from assets/branding/logo-source.png and DejaVu Sans. Do not edit.
// DejaVu fonts: Bitstream Vera / DejaVu license (see CREDITS.md). Logo: PokeMMO-Prospero (see CREDITS.md).
#include "overlay_assets.h"

const unsigned overlay_font_width = {ATLAS_W}, overlay_font_height = {ATLAS_H}, overlay_font_line = {LINE}, overlay_font_ascent = {ascent};
const OverlayGlyph overlay_glyphs[128] = {{
{chr(10).join(rows)}
}};
const unsigned overlay_font_alpha_size = {len(font_alpha)};
const unsigned char overlay_font_alpha[] = {{
{c_bytes(font_alpha)}
}};
const unsigned overlay_logo_width = {logo_rgba.width}, overlay_logo_height = {logo_rgba.height};
const unsigned overlay_logo_rgba_size = {len(logo_bytes)};
const unsigned char overlay_logo_rgba[] = {{
{c_bytes(logo_bytes)}
}};
"""
(ROOT / "loader/src/overlay_assets.c").write_text(source)
print(f"icon0.png, background.dds ({len(payload)} bytes), overlay_assets.c (font {len(font_alpha)} + logo {len(logo_bytes)} bytes compressed)")
