#!/usr/bin/env python3
"""Convert the desktop build's 24-bit BMP dump to a PNG, masked to the disc.

Stdlib only, like every other tool here - this machine has no PIL, and a
gallery that needs a pip install is a gallery that goes stale.

The mask is not decoration. The panel is a 360-pixel ROUND display: pixels
outside the disc do not physically exist, so a square screenshot shows corners
no one can ever see. Some views already mask their own corners and some do not,
which is invisible on hardware and looks like an inconsistency in a README.
Masking here makes every shot show exactly what the glass shows.

Usage: bmp_to_png.py IN.bmp OUT.png [--no-mask]
"""

import struct
import sys
import zlib


def read_bmp(path):
    """Returns (width, height, rows) with rows top-down as bytearrays of RGB."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path} is not a BMP")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path} is {bpp}bpp; this reader only does 24")
    stride = (width * 3 + 3) & ~3
    rows = []
    # BMP rows run bottom-up and pixels are BGR.
    for y in range(height - 1, -1, -1):
        start = pixel_offset + y * stride
        row = bytearray(width * 3)
        for x in range(width):
            b, g, r = data[start + x * 3: start + x * 3 + 3]
            row[x * 3: x * 3 + 3] = bytes((r, g, b))
        rows.append(row)
    return width, height, rows


def write_png(path, width, height, rows, mask=True):
    cx = cy = (width - 1) / 2.0
    radius = width / 2.0
    r2 = radius * radius

    raw = bytearray()
    for y, row in enumerate(rows):
        raw.append(0)  # filter type 0 (None) for every scanline
        dy = y - cy
        for x in range(width):
            raw += row[x * 3: x * 3 + 3]
            if mask:
                dx = x - cx
                raw.append(255 if dx * dx + dy * dy <= r2 else 0)
            else:
                raw.append(255)

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    # Colour type 6 = RGBA, bit depth 8.
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def main(argv):
    if len(argv) < 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    mask = "--no-mask" not in argv
    width, height, rows = read_bmp(argv[1])
    write_png(argv[2], width, height, rows, mask=mask)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
