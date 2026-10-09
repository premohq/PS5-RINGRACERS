#!/usr/bin/env python3
"""Draw the README's banner (docs/banner.png).

    python3 tools/ps5/readme_banner.py OUTPUT GAME_DATA_DIR

The home screen's purple track (presentation.py) with the pixel logo from
docs/logo.png and, under it, "FOR PS5" in the game's title card font: the
GTOL outline glyphs under the GTFN ones, from GAME_DATA_DIR/data/gfx.pk3,
coloured with GAME_DATA_DIR/bios.pk3's palette. Everything is scaled by
whole numbers with nearest-neighbour sampling, so it stays pixel art.

Needs Pillow (tools/ps5/deps.sh installs it into its venv).
"""

import io
import struct
import sys
import zipfile
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True  # no __pycache__ beside the sources
sys.path.insert(0, str(Path(__file__).resolve().parent))
import presentation  # noqa: E402  the launch screen's backdrop
from firstboot_art import doom_patch  # noqa: E402

W, H = 1280, 400
LOGO_SCALE = 3
TEXT_SCALE = 2
TEXT = "FOR PS5"


def glyph_lump(pk3, name):
    """A title card glyph, which the archive keeps with or without .lmp."""
    for info in pk3.infolist():
        if "/1P/" in info.filename and info.filename.rsplit("/", 1)[-1].split(".")[0] == name:
            return pk3.read(info)
    raise SystemExit(f"gfx.pk3 has no {name}")


def glyph(data, palette):
    """A lump as RGBA, from a PNG or a Doom-format patch, and its offsets."""
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        img = Image.open(io.BytesIO(data))
        left = top = 0
        grab = data.find(b"grAb")
        if grab >= 0:
            left, top = struct.unpack_from(">ii", data, grab + 4)
        return img.convert("RGBA"), left, top
    _w, _h, left, _top = struct.unpack_from("<hhhh", data, 0)
    img, top = doom_patch(data, palette)
    return img, left, top


def title_text(text, gfx, palette):
    """text in the title card font, outline under fill, at 1x."""
    glyphs = []
    for ch in text:
        if ch == " ":
            glyphs.append(None)
            continue
        code = ord(ch)
        glyphs.append((glyph(glyph_lump(gfx, f"GTOL{code:03d}"), palette),
                       glyph(glyph_lump(gfx, f"GTFN{code:03d}"), palette)))

    space = max(g[1][0].width for g in glyphs if g) // 2
    pad = 32
    width = sum(g[1][0].width if g else space for g in glyphs) + 2 * pad
    height = max(g[0][0].height for g in glyphs if g) + 2 * pad
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))

    # Each lump is placed at the pen minus its own offsets, as the game
    # draws patches: the outline first, then the fill over it.
    x = pad
    for g in glyphs:
        if g is None:
            x += space
            continue
        for img, left, top in g:
            out.alpha_composite(img, (x - left, pad - top))
        x += g[1][0].width
    return out.crop(out.getbbox())


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: readme_banner.py OUTPUT GAME_DATA_DIR")
    out, data = Path(sys.argv[1]), Path(sys.argv[2])

    with zipfile.ZipFile(data / "bios.pk3") as bios:
        raw = next(bios.read(i) for i in bios.infolist()
                   if i.filename.rsplit("/", 1)[-1].split(".")[0] == "PLAYPAL")
    palette = [tuple(raw[i * 3:i * 3 + 3]) for i in range(256)]

    with zipfile.ZipFile(data / "data" / "gfx.pk3") as gfx:
        text = title_text(TEXT, gfx, palette)
    text = text.resize((text.width * TEXT_SCALE, text.height * TEXT_SCALE), Image.NEAREST)

    logo = Image.open(presentation.REPO / "docs/logo.png").convert("RGBA")
    logo = logo.resize((logo.width * LOGO_SCALE, logo.height * LOGO_SCALE), Image.NEAREST)

    # The track at 1280x720, cut to the band from the sky to the near floor.
    track = presentation.backdrop().resize((W, W * 9 // 16), Image.LANCZOS)
    top = (track.height - H) // 2
    banner = track.crop((0, top, W, top + H)).convert("RGBA")

    gap = 8
    y = (H - logo.height - gap - text.height) // 2
    banner.alpha_composite(logo, ((W - logo.width) // 2, y))
    banner.alpha_composite(text, ((W - text.width) // 2, y + logo.height + gap))

    banner.convert("RGB").save(out, optimize=True)


if __name__ == "__main__":
    main()
