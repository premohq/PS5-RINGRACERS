#!/usr/bin/env python3
"""Pack the art for the PS5 first boot screen (src/ps5/firstboot_screen.c).

    python3 tools/ps5/firstboot_art.py OUTPUT GAME_DATA_DIR

writes OUTPUT (the title's firstboot.dat): the screen's background, the
launch screen's purple track and logo at 640x360, and the game's console
font, the STCFN glyphs that its own loading screen writes in, read from
GAME_DATA_DIR/data/gfx.pk3 and coloured with GAME_DATA_DIR/bios.pk3's
palette. The game data is needed here because the title that downloads it
on first boot cannot carry it; the font is a few kilobytes of it.

The format, little-endian:

    "RRFB" u16 version (1)
    u16 width, u16 height, then width*height RGBA pixels, top row first
    u16 first character, u16 count, then for each character:
        u8 width, u8 height, i8 top offset, u8 0, then width*height RGBA

Needs Pillow (tools/ps5/deps.sh installs it into its venv).
"""

import struct
import sys
import zipfile
from pathlib import Path

from PIL import Image

sys.dont_write_bytecode = True  # no __pycache__ beside the sources
sys.path.insert(0, str(Path(__file__).resolve().parent))
import presentation  # noqa: E402  the launch screen's backdrop and logo

SCREEN_W, SCREEN_H = 640, 360
FIRST_CHAR, LAST_CHAR = 33, 126  # space is a gap, as in the game


def read_lump(pk3, basename):
    """A lump by name, wherever the archive keeps it."""
    for info in pk3.infolist():
        if info.filename.rsplit("/", 1)[-1] == basename:
            return pk3.read(info)
    return None


def doom_patch(data, palette):
    """A Doom-format patch as RGBA, and its top offset."""
    width, height, _left, top = struct.unpack_from("<hhhh", data, 0)
    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    px = img.load()
    for x in range(width):
        (ofs,) = struct.unpack_from("<I", data, 8 + 4 * x)
        row = -1
        while data[ofs] != 0xFF:
            delta, length = data[ofs], data[ofs + 1]
            row = delta if delta > row else row + delta  # tall patches
            for i in range(length):
                px[x, row + i] = palette[data[ofs + 3 + i]] + (255,)
            ofs += length + 4
    return img, top


def background():
    """The launch screen (pic1), smaller, with the logo at twice its size."""
    track = presentation.backdrop().resize((SCREEN_W, SCREEN_H), Image.LANCZOS).convert("RGBA")
    logo = Image.open(presentation.REPO / "docs/logo.png").convert("RGBA")
    logo = logo.resize((logo.width * 2, logo.height * 2), Image.NEAREST)
    track.alpha_composite(logo, ((SCREEN_W - logo.width) // 2, 32))
    return track


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: firstboot_art.py OUTPUT GAME_DATA_DIR")
    out, data = Path(sys.argv[1]), Path(sys.argv[2])

    with zipfile.ZipFile(data / "bios.pk3") as bios:
        raw = read_lump(bios, "PLAYPAL")
    if raw is None:
        raise SystemExit(f"{data / 'bios.pk3'} has no PLAYPAL")
    palette = [tuple(raw[i * 3:i * 3 + 3]) for i in range(256)]

    blob = bytearray(b"RRFB" + struct.pack("<H", 1))

    bg = background()
    blob += struct.pack("<HH", bg.width, bg.height) + bg.tobytes()

    blob += struct.pack("<HH", FIRST_CHAR, LAST_CHAR - FIRST_CHAR + 1)
    missing = []
    with zipfile.ZipFile(data / "data" / "gfx.pk3") as gfx:
        for code in range(FIRST_CHAR, LAST_CHAR + 1):
            lump = read_lump(gfx, f"STCFN{code:03d}")
            if lump is None:
                missing.append(chr(code))
                blob += struct.pack("<BBbB", 0, 0, 0, 0)
                continue
            glyph, top = doom_patch(lump, palette)
            blob += struct.pack("<BBbB", glyph.width, glyph.height, top, 0) + glyph.tobytes()
    if missing:
        print(f"firstboot_art.py: no glyph for {''.join(missing)!r}", file=sys.stderr)

    out.write_bytes(bytes(blob))


if __name__ == "__main__":
    main()
