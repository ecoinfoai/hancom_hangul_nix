# SPDX-License-Identifier: MIT
"""Enlarges HWP's own Windows cursors (.cur) by an integer factor.

HWP loads these bitmaps itself instead of asking the cursor theme, so they
ignore Xcursor sizing and stay 32 px in XWayland's 2x-scaled pixel space.
Scaling is nearest-neighbour on the palette indices and the AND mask, so the
bit depth, palette and transparency HWP sees are unchanged.

Usage: cur_scale.py FACTOR DIR  (rewrites every *.cur under DIR in place)
"""

import pathlib
import struct
import sys

_ENTRY = "<BBBBHHII"
_BITMAPINFO = "<IiiHHIIiiII"


def _scale_rows(data: bytes, width: int, height: int, bpp: int,
                factor: int) -> bytes:
    """Scales a bottom-up packed bitmap of `bpp`-bit pixels by `factor`."""
    stride = (width * bpp + 31) // 32 * 4
    out_width = width * factor
    out_stride = (out_width * bpp + 31) // 32 * 4
    mask = (1 << bpp) - 1
    out = bytearray()
    for y in range(height):
        row = data[y * stride:(y + 1) * stride]
        scaled = bytearray(out_stride)
        for x in range(out_width):
            bit = x // factor * bpp
            value = row[bit // 8] >> (8 - bpp - bit % 8) & mask
            out_bit = x * bpp
            scaled[out_bit // 8] |= value << (8 - bpp - out_bit % 8)
        out += bytes(scaled) * factor
    return bytes(out)


def scale_cur(data: bytes, factor: int) -> bytes:
    """Returns the single-image BMP cursor `data` enlarged by `factor`.

    Args:
        data: A .cur file holding exactly one 1, 4 or 8 bpp BMP image.
        factor: Integer scale; 1 returns `data` unchanged.

    Returns:
        The scaled .cur file, hotspot moved by the same factor.

    Raises:
        ValueError: For PNG or multi-image cursors, unsupported bit depths,
            or a result larger than the format's 256 px limit.
    """
    if factor == 1:
        return data
    _, kind, count = struct.unpack("<HHH", data[:6])
    if kind != 2 or count != 1:
        raise ValueError(f"expected a single-image cursor, got type {kind} "
                         f"with {count} images")
    _, _, _, _, hx, hy, _, off = struct.unpack(_ENTRY, data[6:22])
    if data[off:off + 4] == b"\x89PNG":
        raise ValueError("PNG cursor images are not supported")
    (size, width, height2, planes, bpp, compression, _, xppm, yppm, used,
     important) = struct.unpack(_BITMAPINFO, data[off:off + 40])
    if bpp not in (1, 4, 8) or compression != 0:
        raise ValueError(f"unsupported cursor bitmap: {bpp} bpp, "
                         f"compression {compression}")
    height = height2 // 2
    if max(width, height) * factor > 256:
        raise ValueError(f"{width}x{height} cursor scaled by {factor} exceeds "
                         f"256 px")
    palette_end = off + size + (used or 1 << bpp) * 4
    xor_len = (width * bpp + 31) // 32 * 4 * height
    and_len = (width + 31) // 32 * 4 * height
    xor = _scale_rows(data[palette_end:palette_end + xor_len], width, height,
                      bpp, factor)
    andm = _scale_rows(data[palette_end + xor_len:
                            palette_end + xor_len + and_len], width, height,
                       1, factor)
    w, h = width * factor, height * factor
    header = struct.pack(_BITMAPINFO, size, w, h * 2, planes, bpp, 0,
                         len(xor) + len(andm), xppm, yppm, used, important)
    image = (header + data[off + 40:palette_end] + xor + andm)
    entry = struct.pack(_ENTRY, w % 256, h % 256, data[8], 0, hx * factor,
                        hy * factor, len(image), 22)
    return data[:6] + entry + image


def main() -> None:
    factor, root = int(sys.argv[1]), pathlib.Path(sys.argv[2])
    if factor < 1:
        raise ValueError(f"factor must be a positive integer, got {factor}")
    for path in sorted(root.rglob("*.cur")):
        path.write_bytes(scale_cur(path.read_bytes(), factor))


if __name__ == "__main__":
    main()
