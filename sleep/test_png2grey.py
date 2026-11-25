import contextlib
import io
import os
import struct
import tempfile
import unittest
import zlib

import png2grey

HERE = os.path.dirname(os.path.abspath(__file__))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def filter_row(ftype, row, prev, bpp):
    out = bytearray([ftype])
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[ftype]
        out.append((x - pred) & 0xFF)
    return bytes(out)


def chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def raw_png(width, height, ctype, raw, depth=8, interlace=0):
    ihdr = struct.pack(">IIBBBBB", width, height, depth, ctype, 0, 0, interlace)
    return (png2grey.SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))


def make_png(width, height, ctype, pixels, filters=(0,), depth=8, interlace=0):
    bpp = png2grey.CHANNELS.get(ctype, 1)
    stride = width * bpp
    raw = b""
    prev = bytes(stride)
    for y in range(height):
        row = pixels[y * stride:(y + 1) * stride]
        raw += filter_row(filters[y % len(filters)], row, prev, bpp)
        prev = row
    return raw_png(width, height, ctype, raw, depth, interlace)


class Png2GreyTest(unittest.TestCase):
    def assert_rejected(self, data, width, height, message):
        with self.assertRaises(png2grey.PngError) as ctx:
            png2grey.convert(data, width, height)
        self.assertIn(message, str(ctx.exception))

    def test_grey_rows_with_every_filter(self):
        pixels = bytes((x * 37 + y * 11) % 256 for y in range(5) for x in range(6))
        self.assertEqual(png2grey.convert(make_png(6, 5, 0, pixels, (0, 1, 2, 3, 4)), 6, 5), pixels)

    def test_multibyte_pixels_with_every_filter(self):
        for ctype in (2, 4, 6):
            pixels = bytes((i * 53 + 7) % 256 for i in range(4 * 5 * png2grey.CHANNELS[ctype]))
            plain = png2grey.convert(make_png(4, 5, ctype, pixels), 4, 5)
            filtered = png2grey.convert(make_png(4, 5, ctype, pixels, (0, 1, 2, 3, 4)), 4, 5)
            self.assertEqual(filtered, plain, "colour type %d" % ctype)

    def test_rgb_uses_rec601_luma(self):
        data = make_png(3, 1, 2, bytes([255, 0, 0, 0, 255, 0, 0, 0, 255]))
        self.assertEqual(png2grey.convert(data, 3, 1), bytes([76, 150, 29]))

    def test_rgb_grey_stays_exact(self):
        pixels = bytes(v for v in range(256) for _ in range(3))
        self.assertEqual(png2grey.convert(make_png(256, 1, 2, pixels), 256, 1), bytes(range(256)))

    def test_alpha_is_composited_on_white(self):
        grey_alpha = make_png(3, 1, 4, bytes([0, 255, 0, 0, 100, 128]))
        self.assertEqual(png2grey.convert(grey_alpha, 3, 1), bytes([0, 255, 177]))
        rgba = make_png(2, 1, 6, bytes([0, 0, 0, 0, 255, 0, 0, 255]))
        self.assertEqual(png2grey.convert(rgba, 2, 1), bytes([255, 76]))

    def test_rejects_other_sizes(self):
        self.assert_rejected(make_png(2, 2, 0, bytes(4)), 2, 3, "image is 2x2, want 2x3")

    def test_rejects_unsupported_formats(self):
        self.assert_rejected(make_png(2, 2, 0, bytes(8), depth=16), 2, 2, "depth 16")
        self.assert_rejected(make_png(2, 2, 3, bytes(4)), 2, 2, "colour type 3")
        self.assert_rejected(make_png(2, 2, 0, bytes(4), interlace=1), 2, 2, "interlaced")

    def test_rejects_damaged_files(self):
        good = make_png(2, 2, 0, bytes(4))
        self.assert_rejected(b"GIF89a" + good[6:], 2, 2, "not a PNG")
        bad_crc = bytearray(good)
        bad_crc[16] ^= 1
        self.assert_rejected(bytes(bad_crc), 2, 2, "bad CRC in IHDR")
        self.assert_rejected(good[:-12], 2, 2, "no IEND")
        self.assert_rejected(good[:-14], 2, 2, "truncated")
        self.assert_rejected(raw_png(2, 2, 0, bytes([5, 0, 0, 0, 0, 0])), 2, 2, "bad filter type 5")
        self.assert_rejected(raw_png(2, 2, 0, bytes([0, 0, 0])), 2, 2, "image data is 3 bytes, want 6")

    def test_cli_writes_raw_bytes_and_fails_cleanly(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, "in.png"), os.path.join(d, "out.gray")
            with open(src, "wb") as f:
                f.write(make_png(2, 2, 0, bytes([0, 64, 128, 255])))
            self.assertEqual(png2grey.main(["--size", "2x2", src, out]), 0)
            with open(out, "rb") as f:
                self.assertEqual(f.read(), bytes([0, 64, 128, 255]))
            os.remove(out)
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(png2grey.main(["--size", "2x3", src, out]), 1)
            self.assertIn("want 2x3", err.getvalue())
            self.assertFalse(os.path.exists(out))
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                png2grey.main(["--size", "2by2", src, out])

    def test_placeholder_converts_to_the_panel_size(self):
        with open(os.path.join(HERE, "placeholder.png"), "rb") as f:
            grey = png2grey.convert(f.read(), 1264, 1680)
        self.assertEqual(len(grey), 1264 * 1680)
        self.assertEqual(min(grey[:700 * 1264]), 255)
        self.assertEqual(min(grey[1000 * 1264:]), 255)
        self.assertLess(min(grey[800 * 1264:880 * 1264]), 64)


if __name__ == "__main__":
    unittest.main()
