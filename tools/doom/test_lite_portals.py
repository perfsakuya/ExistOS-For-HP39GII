"""Host checks for the sparse E1M1 door/solid segment table.

Run: python tools/doom/test_lite_portals.py
These checks read WAD/header data and do not write firmware files.
"""

from __future__ import annotations

from collections import defaultdict
import re
import unittest

import build_lite_portals as portal


def c_array(source: str, name: str, fields: int = 1) -> list[tuple[int, ...]]:
    match = re.search(rf"\b{name}\[[^]]+\]\s*=\s*\{{(.*?)\}};", source, re.S)
    if match is None:
        raise AssertionError(f"missing C array {name}")
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.S)
    numbers = [int(value) for value in re.findall(r"-?\d+", body)]
    if len(numbers) % fields:
        raise AssertionError(f"bad record width for {name}")
    return [tuple(numbers[i:i + fields])
            for i in range(0, len(numbers), fields)]


class PortalGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = portal.OUTPUT.read_text(encoding="ascii")
        cls.cell, cls.gx0, cls.gy0, cls.width, cls.height, cls.grid = (
            portal.grid_geometry())
        cls.lines, _, cls.sectors = portal.read_map(portal.SOURCE.read_bytes())
        cls.doors = portal.door_sectors(cls.lines, cls.sectors)
        cls.rectangles = portal.door_rectangles(cls.lines, cls.doors)
        cls.dynamic = {
            line.index: line.front if line.front in cls.doors else line.back
            for line in cls.lines if line.back != portal.NO_DOOR and
            (line.front in cls.doors) != (line.back in cls.doors)
        }
        cls.permanent = {line.index for line in cls.lines
                         if line.flags & 1 or line.back == portal.NO_DOOR}
        cls.row_first = [item[0] for item in c_array(
            cls.source, "e1m1_portal_row_first")]
        cls.cells = c_array(cls.source, "e1m1_portal_cells", 3)
        cls.segments = c_array(cls.source, "e1m1_portal_segments", 6)
        cls.refs = [item[0] for item in c_array(cls.source, "e1m1_portal_refs")]
        cls.door_index = [item[0] for item in c_array(
            cls.source, "e1m1_door_index_by_sector")]
        segment_body = cls.source.split(
            "e1m1_portal_segments[E1M1_PORTAL_SEGMENT_COUNT] = {", 1
        )[1].split("};", 1)[0]
        cls.source_lines = [int(value) for value in re.findall(
            r"/\* WAD line (\d+) \*/", segment_body)]

    def test_regeneration_is_byte_identical(self) -> None:
        regenerated, _ = portal.generate()
        self.assertEqual(self.source, regenerated)

    def test_table_shape_and_door_index(self) -> None:
        self.assertEqual(len(self.row_first), self.height + 1)
        self.assertEqual((self.row_first[0], self.row_first[-1]),
                         (0, len(self.cells)))
        self.assertEqual(len(self.cells), 163)
        self.assertEqual(len(self.segments), len(self.source_lines))
        self.assertEqual(len(self.door_index), len(self.sectors))
        self.assertEqual(len(self.dynamic), 29)
        for index, sector in enumerate(sorted(self.doors)):
            self.assertEqual(self.door_index[sector], index)
        for sector in set(range(len(self.sectors))) - self.doors:
            self.assertEqual(self.door_index[sector], portal.NO_DOOR)

    def test_every_special_cell_has_all_true_segments(self) -> None:
        sampled: dict[int, set[int]] = defaultdict(set)
        for index in self.dynamic:
            for key in portal.line_cells(self.lines[index], self.cell,
                                         self.gx0, self.gy0,
                                         self.width, self.height):
                sampled[key].add(index)
        self.assertEqual(len(sampled), 79)
        boundary_cells = set(sampled)
        expected_cells = set(boundary_cells)
        for x0, y0, x1, y1 in self.rectangles.values():
            for gy in range((y0 - portal.PLAYER_RADIUS) // self.cell - self.gy0,
                            (y1 + portal.PLAYER_RADIUS) // self.cell - self.gy0 + 1):
                for gx in range((x0 - portal.PLAYER_RADIUS) // self.cell - self.gx0,
                                (x1 + portal.PLAYER_RADIUS) // self.cell - self.gx0 + 1):
                    expected_cells.add(gy * self.width + gx)
        for x0, y0, x1, y1 in (portal.EXIT_RECESS, portal.EXIT_APPROACH):
            for gy in range((y0 - portal.PLAYER_RADIUS) // self.cell - self.gy0,
                            (y1 + portal.PLAYER_RADIUS) // self.cell - self.gy0 + 1):
                for gx in range((x0 - portal.PLAYER_RADIUS) // self.cell - self.gx0,
                                (x1 + portal.PLAYER_RADIUS) // self.cell - self.gx0 + 1):
                    expected_cells.add(gy * self.width + gx)
        observed = set()
        for gy in range(self.height):
            previous_x = -1
            for ci in range(self.row_first[gy], self.row_first[gy + 1]):
                first, gx, count = self.cells[ci]
                self.assertGreater(gx, previous_x)
                previous_x = gx
                key = gy * self.width + gx
                observed.add(key)
                self.assertLessEqual(first + count, len(self.refs))
                cx, cy = (gx + self.gx0) * self.cell, (gy + self.gy0) * self.cell
                expected = {index for index in self.permanent | self.dynamic.keys()
                            if portal.intersects_cell(self.lines[index],
                                                      cx, cy, self.cell)}
                expected |= sampled.get(key, set())
                actual_refs = self.refs[first:first + count]
                self.assertEqual(len(actual_refs), len(set(actual_refs)))
                actual = {self.source_lines[ref] for ref in actual_refs}
                self.assertEqual(actual, expected, f"cell ({gx},{gy})")
                for ref in actual_refs:
                    line_id = self.source_lines[ref]
                    record = self.segments[ref]
                    line = self.lines[line_id]
                    self.assertEqual(record[:4],
                                     (line.x1, line.y1, line.x2, line.y2))
                    self.assertEqual(record[4],
                                     self.dynamic.get(line_id, portal.NO_DOOR))
        self.assertEqual(observed, expected_cells)
        self.assertEqual(sum(self.grid[key] != 0 for key in boundary_cells), 38)
        self.assertEqual(sum(self.grid[key] != 0 for key in observed), 82)

    def test_exit_recess_uses_real_switch_walls(self) -> None:
        observed = set()
        for gy in range(74, 77):
            for gx in range(10, 13):
                matches = [ci for ci in range(self.row_first[gy],
                                              self.row_first[gy + 1])
                           if self.cells[ci][1] == gx]
                self.assertEqual(len(matches), 1)
                first, _, count = self.cells[matches[0]]
                observed.update(self.source_lines[ref]
                                for ref in self.refs[first:first + count])
        self.assertTrue(portal.EXIT_WALL_LINES <= observed)
        # The two-sided entry line 405 must not turn into a permanent wall.
        self.assertNotIn(405, observed)

    def test_sector_84_interior_uses_exact_permanent_wall(self) -> None:
        # The far side of door 84 is a genuine one-sided wall in a different
        # cell from its dynamic portal. Losing this cell makes the coarse
        # 32-unit occupancy block the entire door interior after it opens.
        gx, gy = 1296 // self.cell - self.gx0, -996 // self.cell - self.gy0
        self.assertEqual((gx, gy), (63, 3))
        ci = next(i for i in range(self.row_first[gy], self.row_first[gy + 1])
                  if self.cells[i][1] == gx)
        first, _, count = self.cells[ci]
        lines = {self.source_lines[ref]
                 for ref in self.refs[first:first + count]}
        self.assertIn(1094, lines)

    def test_door_midpoints_visible_from_both_sides(self) -> None:
        # A DDA that tests the starting cell and then each entered cell will
        # encounter every axis-aligned door line from either approach. Twelve
        # lines sit exactly on grid borders, so the starting-cell rule matters.
        aligned = 0
        for index in self.dynamic:
            line = self.lines[index]
            self.assertTrue(line.x1 == line.x2 or line.y1 == line.y2)
            mx, my = (line.x1 + line.x2) // 2, (line.y1 + line.y2) // 2
            gx, gy = mx // self.cell - self.gx0, my // self.cell - self.gy0
            matches = [ci for ci in range(self.row_first[gy],
                                          self.row_first[gy + 1])
                       if self.cells[ci][1] == gx]
            self.assertEqual(len(matches), 1, f"door line {index}")
            first, _, count = self.cells[matches[0]]
            self.assertIn(index, (self.source_lines[ref]
                                  for ref in self.refs[first:first + count]))
            if ((line.x1 == line.x2 and line.x1 % self.cell == 0) or
                    (line.y1 == line.y2 and line.y1 % self.cell == 0)):
                aligned += 1
        self.assertEqual(aligned, 12)


if __name__ == "__main__":
    unittest.main()
