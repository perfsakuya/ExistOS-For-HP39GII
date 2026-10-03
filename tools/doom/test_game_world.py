"""World asset fidelity, patch bounds, raw map alignment and LCD gray checks."""
import hashlib
import math
import struct
import unittest

from audit_expansion import RECORDS, directory
from build_game_maps import map_lumps
from build_game_world import (GRAY, OUTPUT, REPORT, SIZE, SKY, SOURCE,
                             SOURCE_SHA256, PatchPlacement, TextureDefinition,
                             compose_texture, decode_patch, enhance_gray,
                             generate, pack_columns, sample_material,
                             texture_definitions, unpack_columns, wad_name)


def patch(rows, left=0, top=0):
    """Build independent Doom-format posts from small test rows."""
    height, width = len(rows), len(rows[0])
    body = bytearray()
    offsets = []
    for x in range(width):
        offsets.append(8 + 4 * width + len(body))
        y = 0
        while y < height:
            if rows[y][x] is None:
                y += 1
                continue
            first = y
            values = bytearray()
            while y < height and rows[y][x] is not None:
                values.append(rows[y][x])
                y += 1
            body.extend(bytes((first, len(values), 0)) + values + b"\0")
        body.append(255)
    return struct.pack("<hhhh", width, height, left, top) + struct.pack(f"<{width}I", *offsets) + body


class CompositionTests(unittest.TestCase):
    def test_patch_order_clipping_transparency_and_ignored_sprite_offsets(self):
        lumps = {"BASE": patch([[10] * 3 for _ in range(3)]),
                 "LEFT": patch([[None, 20], [21, None]], left=123, top=-456),
                 "TOP": patch([[30, 31], [32, 33]], left=-222, top=999)}
        definition = TextureDefinition("TEST", 3, 3,
            (PatchPlacement(0, 0, "BASE"), PatchPlacement(-1, 1, "LEFT"),
             PatchPlacement(2, -1, "TOP")))
        self.assertEqual(compose_texture(definition, lumps),
                         [10, 10, 32, 20, 10, 10, 10, 10, 10])
        reversed_definition = TextureDefinition("TEST", 3, 3,
                                                tuple(reversed(definition.patches)))
        self.assertEqual(compose_texture(reversed_definition, lumps), [10] * 9)

    def test_post_holes_remain_transparent(self):
        self.assertEqual(decode_patch(patch([[1, None], [None, 2], [3, 4]])),
                         (2, 3, [1, None, None, 2, 3, 4]))

    def test_invalid_patch_boundaries_rejected(self):
        good = patch([[11]])
        invalid = [good[:7], good[:-1], good[:-2],
                   struct.pack("<hhhhI", 1, 1, 0, 0, 0) + b"\xff",
                   struct.pack("<hhhhI", 1, 1, 0, 0, 12) + bytes((1, 1, 0, 11, 0, 255)),
                   struct.pack("<hhhhI", 1, 1, 0, 0, 12) + bytes((0, 9, 0, 11, 0, 255))]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(ValueError):
                decode_patch(data)

    def test_tall_patch_relative_post_origins(self):
        # A later topdelta <= previous top is relative to that previous top.
        data = (struct.pack("<hhhhI", 1, 300, 0, 0, 12) +
                bytes((250, 1, 0, 11, 0, 10, 2, 0, 12, 13, 0, 255)))
        width, height, pixels = decode_patch(data)
        self.assertEqual((width, height), (1, 300))
        self.assertEqual(pixels[250], 11)
        self.assertEqual(pixels[260:262], [12, 13])

    def test_pnames_and_texture_directories_are_bounds_checked(self):
        valid_patch = patch([[11]])
        pnames = struct.pack("<I8s", 1, b"PATCH")
        definition = struct.pack("<8sIhhIh5h", b"TEST", 0, 1, 1, 0, 1, -2, 3, 0, 0, 0)
        texture1 = struct.pack("<II", 1, 8) + definition
        lumps = {"PNAMES": pnames, "TEXTURE1": texture1, "PATCH": valid_patch}
        self.assertEqual(texture_definitions(lumps)["TEST"],
                         TextureDefinition("TEST", 1, 1, (PatchPlacement(-2, 3, "PATCH"),)))
        for replacement in (b"", struct.pack("<I", 2), struct.pack("<II", 1, 999)):
            with self.subTest(replacement=replacement), self.assertRaises(ValueError):
                texture_definitions({**lumps, "TEXTURE1": replacement})
        with self.assertRaises(ValueError):
            texture_definitions({**lumps, "PNAMES": struct.pack("<I", 2)})
        with self.assertRaises(ValueError):
            texture_definitions({**lumps, "TEXTURE1": texture1[:-1]})


class GrayTests(unittest.TestCase):
    def test_column_major_low_nibble_packing(self):
        codes = [(x + y) % 16 for y in range(32) for x in range(32)]
        packed = pack_columns(codes)
        self.assertEqual(len(packed), 512)
        self.assertEqual(packed[0], 0x10)
        self.assertEqual(packed[16], 0x21)
        self.assertEqual(unpack_columns(packed), codes)

    def test_black_is_opaque_and_holes_stay_zero(self):
        values = [0] * (SIZE * SIZE)
        values[10] = None
        codes = enhance_gray(values)
        self.assertEqual(codes[10], 0)
        self.assertTrue(all(c > 0 for i, c in enumerate(codes) if i != 10))
        self.assertEqual(GRAY[codes[100]], 18)

    def test_dark_material_is_brighter_and_detail_is_distinct(self):
        source = [40 if x < 16 else 96 for y in range(SIZE) for x in range(SIZE)]
        output = [GRAY[c] for c in enhance_gray(source)]
        self.assertGreater(sum(output) / len(output), sum(source) / len(source) + 15)
        self.assertGreaterEqual(output[8 * SIZE + 24] - output[8 * SIZE + 8], 56)
        # Local detail enhancement separates the bright/dark edge further.
        self.assertGreater(output[8 * SIZE + 16] - output[8 * SIZE + 15],
                           output[8 * SIZE + 24] - output[8 * SIZE + 8])

    def test_uniform_tone_curve_is_monotonic(self):
        levels = [enhance_gray([value] * (SIZE * SIZE))[0]
                  for value in (0, 16, 32, 48, 64, 96, 128, 160, 192, 224, 255)]
        self.assertEqual(levels, sorted(levels))
        self.assertGreater(len(set(levels)), 7)

    def test_area_resample_preserves_thin_trim_and_transparent_coverage(self):
        # A one-pixel trim among four source samples survives at quarter strength.
        source = [100 if x % 4 == 0 else 0 for y in range(128) for x in range(128)]
        sampled = sample_material(source, 128, 128, list(range(256)))
        self.assertEqual(set(sampled), {25})
        alpha = [100 if x % 4 < 2 else None for y in range(128) for x in range(128)]
        self.assertEqual(set(sample_material(alpha, 128, 128, list(range(256)))), {100})
        alpha = [100 if x % 4 < 1 else None for y in range(128) for x in range(128)]
        self.assertEqual(set(sample_material(alpha, 128, 128, list(range(256)))), {None})


class VerifiedWorldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = SOURCE.read_bytes()
        cls.header, cls.report, cls.materials, cls.worlds = generate(cls.data)
        cls.entries = directory(cls.data)
        cls.lookup = {(m.kind, m.name): m for m in cls.materials}

    def test_verified_source_and_generated_reproducibility(self):
        import json
        self.assertEqual(hashlib.sha256(self.data).hexdigest(), SOURCE_SHA256)
        self.assertEqual(OUTPUT.read_text(encoding="ascii"), self.header)
        self.assertEqual(json.loads(REPORT.read_text(encoding="ascii")), self.report)
        second = generate(self.data)
        self.assertEqual((second[0], second[1]), (self.header, self.report))
        with self.assertRaises(ValueError):
            generate(self.data[:-1])

    def test_bank_capacity_pixel_bytes_and_read_only_budget(self):
        self.assertEqual((self.report["wall_bank_count"], self.report["flat_bank_count"]), (198, 87))
        self.assertLessEqual(self.report["wall_bank_count"], 254)
        self.assertLessEqual(self.report["flat_bank_count"], 254)
        self.assertEqual(self.report["read_only_arm_bytes"]["pixels"], 285 * 512)
        self.assertEqual(self.report["read_only_arm_total_bytes"], 218116)
        self.assertEqual(self.report["mutable_asset_bytes"], 0)
        self.assertTrue(all(len(m.pixels) == 512 for m in self.materials))
        self.assertTrue(all(m.width == m.height == 64 for m in self.materials if m.kind == "flat"))
        # These maps include 72/96-high materials; metadata must retain them.
        self.assertTrue(any(m.height & (m.height - 1) for m in self.materials if m.kind == "wall"))

    def test_raw_sidedef_linedef_and_sector_alignment(self):
        for world in self.worlds:
            raw = map_lumps(self.entries, world.name)
            raw_sides = list(RECORDS["SIDEDEFS"].iter_unpack(raw["SIDEDEFS"]))
            vertices = list(RECORDS["VERTEXES"].iter_unpack(raw["VERTEXES"]))
            self.assertEqual(len(world.sides), len(raw_sides))
            for generated, source in zip(world.sides, raw_sides):
                self.assertEqual(generated[:2], source[:2])
                for asset_id, raw_name in zip(generated[2:], (source[2], source[4], source[3])):
                    name = wad_name(raw_name)
                    self.assertEqual(asset_id, 0 if name == "-" else self.lookup[("wall", name)].texture_id)
            raw_lines = list(RECORDS["LINEDEFS"].iter_unpack(raw["LINEDEFS"]))
            self.assertEqual(len(world.lines), len(raw_lines))
            for generated, source in zip(world.lines, raw_lines):
                self.assertEqual(generated[:2], source[5:7])
                x1, y1 = vertices[source[0]]
                x2, y2 = vertices[source[1]]
                tx, ty = generated[2:]
                self.assertAlmostEqual(math.hypot(tx, ty), 16384, delta=1)
                self.assertGreater(tx * (x2 - x1) + ty * (y2 - y1), 0)
                self.assertLess(abs(tx * (y2 - y1) - ty * (x2 - x1)), math.hypot(x2 - x1, y2 - y1))
            raw_sectors = list(RECORDS["SECTORS"].iter_unpack(raw["SECTORS"]))
            self.assertEqual(len(world.sectors), len(raw_sectors))
            for generated, source in zip(world.sectors, raw_sectors):
                for asset_id, raw_name in zip(generated, source[2:4]):
                    name = wad_name(raw_name)
                    self.assertEqual(asset_id, SKY if name == "F_SKY1" else self.lookup[("flat", name)].texture_id)

    def test_material_gray_detail_and_brightness(self):
        assets = self.report["materials"]
        # Intentional white PWHITE and almost black CEIL4_1 remain flat tones.
        self.assertEqual({(a["kind"], a["name"]) for a in assets if a["output_gray_levels"] < 2},
                         {("wall", "PWHITE"), ("flat", "CEIL4_1")})
        brighter = sum(a["mean_output_luma"] > a["mean_source_luma"] for a in assets)
        self.assertGreater(brighter, len(assets) * 0.90)
        self.assertGreater(sum(a["output_gray_levels"] >= 4 for a in assets), len(assets) * 0.90)


if __name__ == "__main__":
    unittest.main()
