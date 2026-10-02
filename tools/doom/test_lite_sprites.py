"""Verify transparent pixels, malformed patch bounds and real asset budget."""
import struct
import unittest

import build_lite_sprites as sprites


class SpriteTests(unittest.TestCase):
    def test_black_is_opaque_and_odd_padding_is_transparent(self):
        palette = bytearray(768)
        palette[3:6] = b"\xff\xff\xff"
        packed = sprites.pack_pixels([None, 0, 1], palette)
        self.assertEqual(packed, b"\x10\x0f")
        self.assertEqual(sprites.GRAY[1], 0)
        self.assertEqual(sprites.GRAY[15], 255)

    def test_column_posts_preserve_holes(self):
        patch = struct.pack("<hhhhII", 2, 2, 0, 0, 16, 23)
        patch += bytes((0, 2, 0, 0, 1, 0, 255, 255))
        self.assertEqual(sprites.decode_patch(patch), (2, 2, [0, None, 1, None]))

    def test_truncated_post_and_bad_column_are_rejected(self):
        header = struct.pack("<hhhhI", 1, 2, 0, 0, 12)
        with self.assertRaises(ValueError):
            sprites.decode_patch(header + bytes((0, 2, 0, 1)))
        with self.assertRaises(ValueError):
            sprites.decode_patch(struct.pack("<hhhhI", 1, 2, 0, 0, 8) + b"\xff")

    def test_verified_freedoom_assets_fit_flash_budget(self):
        header, assets = sprites.generate(sprites.SOURCE.read_bytes())
        self.assertEqual(len(assets), 5)
        self.assertEqual(sum(len(a[3]) for a in assets), 4840)
        self.assertEqual(header, sprites.OUTPUT.read_text(encoding="ascii"))
        for _, width, height, packed in assets:
            self.assertEqual(len(packed), (width * height + 1) // 2)
            self.assertTrue(any(packed))


if __name__ == "__main__":
    unittest.main()
