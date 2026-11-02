"""Bake this mod's own PNG art into its native Xbox 360 UI atlas.

Requires Pillow. No game files are read. Pixels stay uncompressed to avoid
block-compression artifacts on the small map icons.
"""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / 'assets'

def tiled_offset(x, y, width):
    log_bpp = 2  # ARGB: 4 bytes per pixel
    aw = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    off = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    return (((off & ~511) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp

atlas = Image.new('RGBA', (128, 64))
for name, x, size in [('cd', 1, 38), ('graffiti', 41, 38)]:
    with Image.open(ASSETS / f'{name}.png') as image:
        assert image.size == (size, size)
        atlas.paste(image.convert('RGBA'), (x, 1))
atlas.save(ASSETS / 'ui-atlas.png')
payload = bytearray(32768)
used = set()
for y in range(64):
    for x in range(128):
        r,g,b,a = atlas.getpixel((x,y))
        dst = tiled_offset(x,y,128)*4
        assert dst not in used and 0 <= dst <= len(payload)-4
        used.add(dst)
        payload[dst:dst+4] = bytes((a,r,g,b))
assert len(used) == 128*64
(ASSETS / 'ui-atlas.bin').write_bytes(payload)
print('Baked 38x38 collectible badges into ui-atlas.bin')
