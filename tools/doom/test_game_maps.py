"""Independent geometry, full corpus, and malformed-map checks."""
from fractions import Fraction
import struct
import unittest

import build_game_maps as maps


class GameMapsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = maps.directory(maps.SOURCE.read_bytes())
        cls.lumps = [maps.map_lumps(cls.entries, name) for name in ("E1M1", "E1M2")]
        cls.maps = [maps.extract(name, i, cls.lumps[i]) for i, name in enumerate(("E1M1", "E1M2"))]

    def test_counts_and_player_starts(self):
        first, second = self.maps
        self.assertEqual((len(first.things), len(first.lines), len(first.sectors)), (292, 1175, 182))
        self.assertEqual((len(second.things), len(second.lines), len(second.sectors)), (356, 2290, 380))
        self.assertEqual(first.start, (-416, 256, 0, 140))
        self.assertEqual(second.start, (608, 48, 270, 0))
        self.assertGreater(max(t[5] for t in second.things), 255)

    def test_complete_line_supercover(self):
        for data in self.maps:
            result = maps.audit(data)
            self.assertEqual(result["geometry_line_sample_checks"], 17 * len(data.lines))
            self.assertLess(len(data.cell_refs), 65536)
            self.assertLess(len(data.cells), 65536)
            self.assertEqual(data.row_first[-1], len(data.cells))

    def test_subsector_geometry_agrees_with_bsp(self):
        checks = 0
        for data, lumps in zip(self.maps, self.lumps):
            vertices = list(maps.RECORDS["VERTEXES"].iter_unpack(lumps["VERTEXES"]))
            segs = list(maps.RECORDS["SEGS"].iter_unpack(lumps["SEGS"]))
            subsectors = list(maps.RECORDS["SSECTORS"].iter_unpack(lumps["SSECTORS"]))
            for index, (count, first) in enumerate(subsectors):
                # WAD subsectors omit artificial BSP boundary edges, so the
                # vertex mean can land on a real wall. Nudge a seg midpoint
                # half a unit toward its right-hand (front) sector instead.
                found = False
                for seg in segs[first:first + count]:
                    a, b = vertices[seg[0]], vertices[seg[1]]
                    dx, dy = b[0] - a[0], b[1] - a[1]
                    if not (dx or dy):
                        continue
                    x = Fraction(a[0] + b[0], 2) + Fraction((dy > 0) - (dy < 0), 2)
                    y = Fraction(a[1] + b[1], 2) - Fraction((dx > 0) - (dx < 0), 2)
                    if maps.sector_at(data.nodes, data.subsectors, x, y) == data.subsectors[index]:
                        found = True
                        break
                self.assertTrue(found, f"{data.name} subsector {index} has no BSP-consistent interior sample")
                checks += 1
        self.assertEqual(checks, 1786)

    def test_tag_and_neighbor_indices(self):
        for data in self.maps:
            for tag, first, count in data.tags:
                refs = data.tag_refs[first:first + count]
                self.assertTrue(count)
                self.assertTrue(all(data.sectors[ref][4] == tag for ref in refs))
            for index, sector in enumerate(data.sectors):
                first, count = sector[10:12]
                refs = data.neighbor_refs[first:first + count]
                self.assertEqual(len(set(refs)), count)
                for neighbor in refs:
                    reverse = data.sectors[neighbor]
                    self.assertIn(index, data.neighbor_refs[reverse[10]:reverse[10] + reverse[11]])

    def test_edge_supercover_and_negative_coordinates(self):
        line = (-32, -32, -32, 32, 0, 0, 0, 0, 65535)
        rows, cells, refs = maps.build_cells([line], (-64, -64), (4, 5))
        lookup = {(cell[0], y): refs[cell[1]:cell[1] + cell[2]]
                  for y in range(5) for cell in cells[rows[y]:rows[y + 1]]}
        for x in (0, 1):
            for y in (0, 1, 2, 3):
                self.assertEqual(lookup[(x, y)], [0])

    def test_rejects_bad_vertex_sector_and_bsp(self):
        for field, offset, value in (("LINEDEFS", 0, 65535), ("SIDEDEFS", 28, 65535),
                                     ("NODES", 24, 0)):
            lumps = dict(self.lumps[0])
            changed = bytearray(lumps[field])
            struct.pack_into("<H", changed, offset, value)
            lumps[field] = bytes(changed)
            with self.assertRaises(ValueError, msg=field):
                maps.extract("E1M1", 0, lumps)

    def test_rejects_truncated_record(self):
        for field in maps.RECORDS:
            lumps = dict(self.lumps[0])
            lumps[field] = lumps[field][:-1]
            with self.assertRaises(ValueError, msg=field):
                maps.extract("E1M1", 0, lumps)


if __name__ == "__main__":
    unittest.main()
