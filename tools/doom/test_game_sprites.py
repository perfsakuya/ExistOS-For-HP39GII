"""Asset bounds/transparency/offset verification for the native actor bank."""
import struct
import unittest
from copy import deepcopy
from unittest.mock import patch
from build_game_sprites import (SOURCE, OUTPUT, generate, GRAY, FAMILIES, KEYS, PICKUPS, WEAPONS,
                                build_mappings, rotation_lookup, validate_mappings,
                                FLIP, INDEX_MASK, PIXEL_BUDGET)
from build_lite_sprites import read_lumps


class GameSpriteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = SOURCE.read_bytes()
        cls.header, cls.assets = generate(cls.data)
        cls.lumps = read_lumps(cls.data)
        cls.names, cls.mappings = build_mappings(cls.lumps)

    def test_bank_budget_and_sequence_coverage(self):
        self.assertEqual(len(self.assets), 216)
        self.assertEqual(self.header, OUTPUT.read_text(encoding="ascii"))
        self.assertLessEqual(sum(len(asset[-1]) for _, asset in self.assets), PIXEL_BUDGET)
        names = {name for name, _ in self.assets}
        self.assertEqual(len(names), len(self.assets))
        for (_, prefix, states), (_, rows) in zip(FAMILIES, self.mappings):
            self.assertEqual(tuple(map(len, rows)), tuple(len(state.split()) for state in states))
            for state, frames in zip(states, rows):
                for suffix, references in zip(state.split(), frames):
                    self.assertEqual(len(references), 8)
                    for rotation, reference in enumerate(references, 1):
                        name = self.names[reference & INDEX_MASK]
                        pair = suffix[0] + ("0" if suffix[1] == "0" else str(rotation))
                        offset = 6 if reference & FLIP else 4
                        self.assertEqual(name[offset:offset + 2], pair)
                    self.assertTrue({self.names[ref & INDEX_MASK] for ref in references} <= names)

    def test_former_indices_are_preserved(self):
        old_names = []
        for _, prefix, states in FAMILIES:
            for state in states:
                for suffix in state.split():
                    name = prefix + suffix
                    if name not in old_names:
                        old_names.append(name)
        old_names.extend(name for _, name in KEYS + PICKUPS)
        self.assertEqual(len(old_names), 69)
        self.assertEqual(self.names[:69], old_names)

    def test_weapon_pickups_append_after_all_former_directions(self):
        with patch('build_game_sprites.WEAPONS', ()):
            former_names, former_mappings = build_mappings(self.lumps)
        self.assertEqual(len(former_names), 210)
        self.assertEqual(self.names[:len(former_names)], former_names)
        self.assertEqual(self.mappings, former_mappings)
        self.assertEqual(self.names[len(former_names):], [name for _, name in WEAPONS])
        self.assertEqual(dict(WEAPONS)[2001], "SHOTA0")
        for thing_type, name in WEAPONS:
            self.assertIn(f"{{{thing_type}u, {self.names.index(name)}u}}", self.header)

    def test_paired_directions_share_pixels_with_flip(self):
        for family in (1, 2, 3):
            for frames in self.mappings[family][1][:3]:
                for refs in frames:
                    for first, second in ((1, 7), (2, 6), (3, 5)):
                        self.assertEqual(refs[first] ^ FLIP, refs[second])
                    self.assertFalse(refs[0] & FLIP)
                    self.assertFalse(refs[4] & FLIP)
        for frames in self.mappings[0][1][:3]:
            for refs in frames:
                self.assertEqual(len(set(refs)), 8)
                self.assertFalse(any(ref & FLIP for ref in refs))

    def test_death_rotations_repeat_one_unflipped_patch(self):
        for _, states in self.mappings:
            for refs in states[3]:
                self.assertEqual(len(set(refs)), 1)
                self.assertFalse(refs[0] & FLIP)

    def test_bounds_offsets_and_gray_detail(self):
        lumps = self.lumps
        for name, (w, h, world_w, world_h, left, top, pixels) in self.assets:
            self.assertLessEqual(w, 32)
            self.assertLessEqual(h, 48)
            self.assertEqual((world_w, world_h, left, top), struct.unpack_from('<hhhh', lumps[name]))
            self.assertEqual(len(pixels), (w * h + 1) // 2)
            codes = {(pixels[i // 2] >> ((i & 1) * 4)) & 15 for i in range(w * h)}
            self.assertIn(0, codes, name)
            self.assertGreater(len(codes - {0}), 3, name)
        self.assertEqual(len(GRAY), 16)

    def test_missing_rotation_is_rejected(self):
        lumps = dict(self.lumps)
        del lumps["POSSA8"]
        with self.assertRaisesRegex(ValueError, "incomplete POSSA rotations"):
            rotation_lookup(lumps, "POSS", "A")

    def test_duplicate_rotation_is_rejected(self):
        lumps = dict(self.lumps)
        lumps["SARGA8"] = lumps["SARGA2A8"]
        with self.assertRaisesRegex(ValueError, "duplicate SARGA rotation 8"):
            rotation_lookup(lumps, "SARG", "A")

    def test_rotation_zero_and_direction_mix_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate TESTA rotation 1"):
            rotation_lookup({"TESTA0": b"", "TESTA1": b""}, "TEST", "A")
        with self.assertRaisesRegex(ValueError, "cannot share"):
            rotation_lookup({"TESTA0A1": b""}, "TEST", "A")

    def test_malformed_rotation_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "invalid sprite frame/rotation"):
            rotation_lookup({"TESTA9": b""}, "TEST", "A")
        with self.assertRaisesRegex(ValueError, "invalid sprite lump name"):
            rotation_lookup({"TESTA": b""}, "TEST", "A")

    def test_out_of_bank_reference_is_rejected(self):
        mappings = deepcopy(self.mappings)
        mappings[0][1][0][0][0] = FLIP | len(self.assets)
        with self.assertRaisesRegex(ValueError, "invalid sprite reference"):
            validate_mappings(mappings, len(self.assets))

    def test_invalid_rotation_count_and_death_flip_are_rejected(self):
        mappings = deepcopy(self.mappings)
        mappings[0][1][0][0].pop()
        with self.assertRaisesRegex(ValueError, "invalid rotation count"):
            validate_mappings(mappings, len(self.assets))
        mappings = deepcopy(self.mappings)
        mappings[0][1][3][0] = [mappings[0][1][3][0][0] | FLIP] * 8
        with self.assertRaisesRegex(ValueError, "death frames must be undirected"):
            validate_mappings(mappings, len(self.assets))

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

    def test_pixel_budget_is_enforced(self):
        actual = sum(len(asset[-1]) for _, asset in self.assets)
        with patch('build_game_sprites.PIXEL_BUDGET', actual - 1):
            with self.assertRaisesRegex(ValueError, "pixel budget"):
                generate(self.data)

if __name__ == '__main__':
    unittest.main()
