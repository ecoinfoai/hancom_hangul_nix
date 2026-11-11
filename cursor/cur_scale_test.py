# SPDX-License-Identifier: MIT
"""Checks cur_scale.py on synthetic single-image Windows cursors."""

import struct
import unittest

from cur_scale import scale_cur


def make_cur(size: int, bpp: int, hotspot: tuple[int, int],
             pixel: tuple[int, int], index: int) -> bytes:
    """Builds a square cursor whose pixels are all index 0 and transparent,
    except one opaque pixel at `pixel` (top-down coordinates) set to `index`."""
    colors = 1 << bpp
    palette = b"".join(struct.pack("<BBBB", i, i, i, 0) for i in range(colors))
    xor_stride = (size * bpp + 31) // 32 * 4
    and_stride = (size + 31) // 32 * 4
    xor = bytearray(xor_stride * size)
    andm = bytearray(b"\xff" * and_stride * size)
    px, py = pixel
    row = size - 1 - py
    bit = px * bpp
    shift = 8 - bpp - bit % 8
    xor[row * xor_stride + bit // 8] |= index << shift
    andm[row * and_stride + px // 8] &= ~(0x80 >> px % 8) & 0xFF
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, bpp, 0,
                         len(xor) + len(andm), 0, 0, colors, 0)
    image = header + palette + bytes(xor) + bytes(andm)
    entry = struct.pack("<BBBBHHII", size, size, 0, 0, hotspot[0], hotspot[1],
                        len(image), 22)
    return struct.pack("<HHH", 0, 2, 1) + entry + image


def read_cur(data: bytes) -> dict:
    """Decodes a single-image cursor into its geometry and pixel arrays."""
    w, h, _, _, hx, hy, _, off = struct.unpack("<BBBBHHII", data[6:22])
    w, h = w or 256, h or 256
    _, bw, bh, _, bpp, _, _, _, _, used, _ = struct.unpack(
        "<IiiHHIIiiII", data[off:off + 40])
    colors = used or 1 << bpp
    pix = off + 40 + colors * 4
    xor_stride = (bw * bpp + 31) // 32 * 4
    and_stride = (bw + 31) // 32 * 4
    rows = bh // 2

    def index(x: int, y: int) -> int:
        bit = x * bpp
        byte = data[pix + (rows - 1 - y) * xor_stride + bit // 8]
        return byte >> (8 - bpp - bit % 8) & ((1 << bpp) - 1)

    def opaque(x: int, y: int) -> bool:
        base = pix + xor_stride * rows
        byte = data[base + (rows - 1 - y) * and_stride + x // 8]
        return not byte & (0x80 >> x % 8)

    return {"size": (w, h), "bmp": (bw, bh), "bpp": bpp, "hotspot": (hx, hy),
            "index": index, "opaque": opaque, "length": len(data),
            "end": pix + (xor_stride + and_stride) * rows}


class ScaleCurTest(unittest.TestCase):

    def check(self, bpp: int, index: int) -> None:
        src = make_cur(32, bpp, (4, 8), (5, 9), index)
        out = read_cur(scale_cur(src, 4))
        self.assertEqual(out["size"], (128, 128))
        self.assertEqual(out["bmp"], (128, 256))
        self.assertEqual(out["bpp"], bpp)
        self.assertEqual(out["hotspot"], (16, 32))
        self.assertEqual(out["end"], out["length"])
        for y in range(36, 40):
            for x in range(20, 24):
                self.assertTrue(out["opaque"](x, y))
                self.assertEqual(out["index"](x, y), index)
        for x, y in [(19, 36), (24, 39), (20, 35), (23, 40), (0, 0), (127, 127)]:
            self.assertFalse(out["opaque"](x, y))
            self.assertEqual(out["index"](x, y), 0)

    def test_1bpp(self) -> None:
        self.check(1, 1)

    def test_4bpp(self) -> None:
        self.check(4, 11)

    def test_8bpp(self) -> None:
        self.check(8, 200)

    def test_factor_one_is_identity(self) -> None:
        src = make_cur(32, 8, (4, 8), (5, 9), 7)
        self.assertEqual(scale_cur(src, 1), src)

    def test_result_over_256_pixels_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            scale_cur(make_cur(32, 8, (0, 0), (0, 0), 1), 9)

    def test_png_image_is_rejected(self) -> None:
        src = bytearray(make_cur(32, 8, (0, 0), (0, 0), 1))
        src[22:26] = b"\x89PNG"
        with self.assertRaises(ValueError):
            scale_cur(bytes(src), 2)

    def test_multi_image_cursor_is_rejected(self) -> None:
        src = bytearray(make_cur(32, 8, (0, 0), (0, 0), 1))
        src[4:6] = struct.pack("<H", 2)
        with self.assertRaises(ValueError):
            scale_cur(bytes(src), 2)


if __name__ == "__main__":
    unittest.main()
