"""Validate UI source attribution, clipping, contrast, and flash budget."""
import unittest
import build_lite_ui as ui
import build_lite_sprites as sprites


class UiTests(unittest.TestCase):
    def test_verified_assets_and_generated_header(self):
        header, assets = ui.generate(ui.SOURCE.read_bytes())
        self.assertEqual(header, ui.OUTPUT.read_text(encoding="ascii"))
        self.assertEqual(len(assets), 33)
        self.assertLessEqual(sum(len(a[5]) for a in assets), 16384)
        for name, w, h, x, y, packed in assets:
            self.assertEqual(len(packed), (w * h + 1) // 2)
            if name.startswith("PIS"):
                self.assertGreaterEqual(x, 0)
                self.assertGreaterEqual(y, 0)
                self.assertLessEqual(x + w, 256)
                self.assertLessEqual(y + h, ui.VIEW_H)

    def test_lcd_bar_and_number_contrast(self):
        _, assets = ui.generate(ui.SOURCE.read_bytes())
        by_name = {a[0]: a for a in assets}
        self.assertEqual(by_name["STBAR"][1:3], (256, 26))
        for name in ("STTNUM0", "STYSNUM0"):
            packed = by_name[name][5]
            codes = [n for b in packed for n in (b & 15, b >> 4) if n]
            self.assertLessEqual(min(ui.GRAY[n] for n in codes), 36)
            if name == "STTNUM0":
                self.assertGreaterEqual(max(ui.GRAY[n] for n in codes), 200)
            else:
                # The small counter has transparent holes, not opaque shadows.
                self.assertTrue(any(not n for b in packed for n in (b & 15, b >> 4)))
        pixels = by_name["STBAR"][5]
        codes = [n for b in pixels for n in (b & 15, b >> 4) if n]
        self.assertGreater(sum(ui.GRAY[n] for n in codes) / len(codes), 170)
        label_codes = [(pixels[i // 2] >> ((i & 1) * 4)) & 15
                       for i in range(19 * 256, 26 * 256)]
        self.assertEqual({ui.GRAY[n] for n in label_codes if n}, {18, 237})

    def test_wide_patch_is_explicit_and_checked(self):
        lumps = sprites.read_lumps(ui.SOURCE.read_bytes())
        with self.assertRaises(ValueError):
            sprites.decode_patch(lumps["STBAR"])
        self.assertEqual(sprites.decode_patch(lumps["STBAR"], max_width=320)[:2], (320, 32))
        broken = bytearray(lumps["STBAR"])
        broken[8:12] = b"\xff\xff\xff\x7f"
        with self.assertRaises(ValueError):
            sprites.decode_patch(broken, max_width=320)


if __name__ == "__main__":
    unittest.main()
