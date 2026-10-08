#!/usr/bin/env python3
"""Draw the PS5 home screen art for Ring Racers.

    python3 tools/ps5/presentation.py OUTPUT_DIR

writes icon0.png (512x512), pic0.dds and pic1.dds (3840x2160, BC7, DX10, one
mip: the only form the shell takes, and what the native-app boilerplate's
tools/validate-assets.sh checks for) into OUTPUT_DIR.

Everything is drawn from art already in this repository - the pixel logo in
docs/logo.png, the icon in srb2.png and the banner in srb2banner.png - so the
result is reproducible and adds no new assets. The logo is scaled by whole
numbers with nearest-neighbour sampling, so it stays the crisp pixel art it
is in the game.

pic0 is the background behind the selected tile on the home screen, where the
shell writes the title's name on the left, so the logo sits right of centre.
pic1 is shown while the title launches; the logo is centred.

Needs Pillow and etcpak (tools/ps5/deps.sh installs both into its venv).
"""

import struct
import sys
from pathlib import Path

import etcpak
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

REPO = Path(__file__).resolve().parents[2]
W, H = 3840, 2160


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def backdrop():
    """A purple sky over the checkered purple track of the banner."""
    img = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(img)
    horizon = int(H * 0.62)
    top, mid, glow = (14, 10, 46), (58, 40, 150), (120, 90, 220)
    for y in range(horizon):
        t = y / horizon
        c = lerp(top, mid, t) if t < 0.8 else lerp(mid, glow, (t - 0.8) / 0.2)
        d.line([(0, y), (W, y)], fill=c)

    tile_a, tile_b = (70, 40, 170), (96, 60, 205)
    average = lerp(tile_a, tile_b, 0.5)
    rows, cols, cell = 14, 24, W / 12
    depth = [horizon + (H - horizon) * ((r / rows) ** 2.2) for r in range(rows + 1)]

    # Far away the squares are smaller than a pixel and blend to their
    # average; draw that first, so the floor reaches the edges everywhere.
    for y in range(horizon, H):
        s = (y - horizon) / (H - horizon)
        d.line([(0, y), (W, y)], fill=tuple(int(v * (0.55 + 0.45 * s)) for v in average))

    for r in range(rows):
        y0, y1 = depth[r], depth[r + 1]
        s0 = (y0 - horizon) / (H - horizon)
        s1 = (y1 - horizon) / (H - horizon)
        shade = 0.55 + 0.45 * s1
        for c in range(-cols, cols):
            colour = tile_a if (r + c) % 2 else tile_b
            colour = tuple(int(v * shade) for v in colour)
            d.polygon([
                (W / 2 + c * cell * s0, y0), (W / 2 + (c + 1) * cell * s0, y0),
                (W / 2 + (c + 1) * cell * s1, y1), (W / 2 + c * cell * s1, y1),
            ], fill=colour)

    # Haze where the track meets the sky.
    for y in range(horizon - 60, horizon + 120):
        t = 1 - abs(y - horizon) / (120 if y >= horizon else 60)
        t = max(0.0, min(1.0, t)) * 0.55
        row = img.crop((0, y, W, y + 1))
        img.paste(Image.blend(row, Image.new("RGB", (W, 1), glow), t), (0, y))
    return img


def with_logo(img, scale, cx, cy):
    logo = Image.open(REPO / "docs/logo.png").convert("RGBA")
    big = logo.resize((logo.width * scale, logo.height * scale), Image.NEAREST)
    out = img.convert("RGBA")
    out.alpha_composite(big, (int(cx - big.width / 2), int(cy - big.height / 2)))
    return out.convert("RGB")


def icon():
    """Robotnik, the game's own icon, over the banner's colours."""
    banner = Image.open(REPO / "srb2banner.png").convert("RGB")
    s = 512 / banner.height
    back = banner.resize((round(banner.width * s), 512), Image.BILINEAR)
    x = (back.width - 512) // 2
    back = back.crop((x, 0, x + 512, 512)).filter(ImageFilter.GaussianBlur(10))
    back = ImageEnhance.Brightness(back).enhance(0.6).convert("RGBA")
    face = Image.open(REPO / "srb2.png").convert("RGBA").resize((512, 512), Image.NEAREST)
    back.alpha_composite(face)
    return back.convert("RGB")


def write_bc7_dds(img, path):
    """A DX10 DDS holding one BC7_UNORM image, header as the boilerplate's."""
    rgba = img.convert("RGBA")
    blocks = etcpak.compress_bc7(rgba.tobytes(), rgba.width, rgba.height, None)
    assert len(blocks) == rgba.width // 4 * (rgba.height // 4) * 16
    header = struct.pack(
        "<4s7I44x8I5I",
        b"DDS ", 124,
        0x000A1007,                           # caps, height, width, pitch, pixelformat, mipmapcount, linearsize
        rgba.height, rgba.width,
        rgba.width // 4 * (rgba.height // 4) * 16, 0, 1,
        32, 0x4, 0x30315844, 0, 0, 0, 0, 0,   # pixel format: FourCC "DX10"
        0x1000, 0, 0, 0, 0,                   # caps: texture
    )
    dx10 = struct.pack("<5I", 98, 3, 0, 1, 0)  # BC7_UNORM, 2D, array of 1
    path.write_bytes(header + dx10 + blocks)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: presentation.py OUTPUT_DIR")
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)

    base = backdrop()
    write_bc7_dds(with_logo(base, 12, W * 0.66, H * 0.40), out / "pic0.dds")
    write_bc7_dds(with_logo(base, 14, W * 0.50, H * 0.42), out / "pic1.dds")
    icon().save(out / "icon0.png", optimize=True)


if __name__ == "__main__":
    main()
