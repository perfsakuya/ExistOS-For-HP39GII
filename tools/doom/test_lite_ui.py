"""Validate UI source attribution, clipping, contrast, and flash budget."""
import unittest
from unittest.mock import patch
import build_lite_ui as ui
import build_lite_sprites as sprites


class UiTests(unittest.TestCase):
    def test_verified_assets_and_generated_header(self):
        header, assets = ui.generate(ui.SOURCE.read_bytes())
        self.assertEqual(header, ui.OUTPUT.read_text(encoding="ascii"))
        self.assertEqual(len(assets), 39)
        self.assertEqual(ui.PIXEL_BUDGET, 24 * 1024)
        self.assertEqual(sum(len(a[5]) for a in assets), 17062)
        self.assertLessEqual(sum(len(a[5]) for a in assets), ui.PIXEL_BUDGET)
        for name, w, h, x, y, packed in assets:
            self.assertEqual(len(packed), (w * h + 1) // 2)
            if name in ui.WEAPON_NAMES:
                self.assertGreaterEqual(x, 0)
                self.assertGreaterEqual(y, 0)
                self.assertLessEqual(x + w, 256)
                self.assertLessEqual(y + h, ui.VIEW_H)

    def test_shotgun_pump_and_flash_assets_preserve_offsets_and_detail(self):
        _, assets = ui.generate(ui.SOURCE.read_bytes())
        weapons = {name: (w, h, x, y, packed)
                   for name, w, h, x, y, packed in assets if name in ui.WEAPON_NAMES}
        self.assertEqual(len(weapons), 10)
        for name in ("SHTGA0", "SHTGB0", "SHTGC0", "SHTGD0", "SHTFA0", "SHTFB0"):
            w, h, x, y, packed = weapons[name]
            codes = [(packed[i // 2] >> ((i & 1) * 4)) & 15 for i in range(w * h)]
            self.assertIn(0, codes, name)
            self.assertGreater(len(set(codes) - {0}), 5, name)
            self.assertLessEqual(min(ui.GRAY[code] for code in codes if code), 36, name)
        self.assertEqual(weapons["SHTGA0"][:4], (54, 17, 98, 84))
        self.assertEqual(weapons["SHTGC0"][:4], (74, 67, 48, 34))
        self.assertNotEqual(weapons["SHTFA0"][-1], weapons["SHTFB0"][-1])
        for name in ("SHTFA0", "SHTFB0"):
            self.assertIn(15, [code for byte in weapons[name][-1]
                              for code in (byte & 15, byte >> 4)])

    def test_flash_palette_is_small_and_locally_brighter(self):
        self.assertEqual(len(ui.FLASH_GRAY), 16)
        self.assertEqual(ui.FLASH_GRAY[0], 0)
        for base, lit in zip(ui.GRAY[1:], ui.FLASH_GRAY[1:]):
            self.assertLessEqual(base, lit)
            if base < 255:
                self.assertLess(base, lit)
            self.assertLessEqual(lit - base, 24)
            self.assertLessEqual(lit, 255)

    def test_pixel_budget_boundary_is_enforced(self):
        with patch('build_lite_ui.PIXEL_BUDGET', 17061):
            with self.assertRaisesRegex(ValueError, "flash budget"):
                ui.generate(ui.SOURCE.read_bytes())

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
