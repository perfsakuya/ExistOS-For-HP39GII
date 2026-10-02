"""Asset bounds/transparency/offset verification for the native actor bank."""
import struct
import unittest
from build_game_sprites import SOURCE, generate, resized_patch, GRAY, FAMILIES
from build_lite_sprites import read_lumps


class GameSpriteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = SOURCE.read_bytes()
        cls.header, cls.assets = generate(cls.data)

    def test_bank_budget_and_sequence_coverage(self):
        self.assertEqual(len(self.assets), 69)
        self.assertLess(sum(len(asset[-1]) for _, asset in self.assets), 36 * 1024)
        names = {name for name, _ in self.assets}
        for _, prefix, states in FAMILIES:
            for state in states:
                self.assertTrue({prefix + suffix for suffix in state.split()} <= names)

    def test_bounds_offsets_and_gray_detail(self):
        lumps = read_lumps(self.data)
        for name, (w, h, world_w, world_h, left, top, pixels) in self.assets:
            self.assertLessEqual(w, 32)
            self.assertLessEqual(h, 48)
            self.assertEqual((world_w, world_h, left, top), struct.unpack_from('<hhhh', lumps[name]))
            self.assertEqual(len(pixels), (w * h + 1) // 2)
            codes = {(pixels[i // 2] >> ((i & 1) * 4)) & 15 for i in range(w * h)}
            self.assertIn(0, codes, name)
            self.assertGreater(len(codes - {0}), 3, name)
        self.assertEqual(len(GRAY), 16)

    def test_death_anchor_preserves_low_height(self):
        assets = dict(self.assets)
        for prefix, last in (('POSS', 'L0'), ('SPOS', 'L0'), ('TROO', 'M0'), ('SARG', 'N0')):
            standing = assets[prefix + 'A1']
            dead = assets[prefix + last]
            self.assertLess(dead[3], standing[3])
            self.assertLess(dead[5], standing[5])

    def test_source_hash_required(self):
        with self.assertRaises(ValueError):
            generate(self.data[:-1])

if __name__ == '__main__':
    unittest.main()
