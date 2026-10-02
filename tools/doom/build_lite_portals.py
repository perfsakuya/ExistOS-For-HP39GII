"""Generate sparse, exact LINEDEFS for door cells in Doom Lite's 32-unit grid.

The normal grid remains fast and unchanged. Cells around the 29 two-sided
boundaries of E1M1's 15 closed doors, their interiors, and the exit-switch
recess need exact segments: each may also contain a permanent wall, which a
single occupancy byte cannot represent. The generated table includes every
permanent blocking line intersecting a special cell, plus dynamic door edges.

Use --check to validate geometry and compare the committed header byte for
byte with a deterministic regeneration. This script never modifies the WAD or
E1M1Grid.h.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
from fractions import Fraction
import hashlib
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "System/applications/user/doom_port/data/freedoom-e1m1-gba.wad"
GRID = ROOT / "System/applications/user/doom_lite/E1M1Grid.h"
GAMEPLAY = ROOT / "System/applications/user/doom_lite/E1M1Gameplay.h"
OUTPUT = ROOT / "System/applications/user/doom_lite/E1M1Portals.h"
LINE = struct.Struct("<iiiiIiiHHiiiiHhhh")
SIDE = struct.Struct("<hhhhhh")
SECTOR = struct.Struct("<hh8s8shhh")
NO_SIDE = 0xFFFF
NO_DOOR = 0xFF
EXPECTED_DOOR_SECTORS = {10, 34, 48, 51, 54, 64, 71, 77, 78, 79,
                         80, 81, 84, 100, 145}
PLAYER_RADIUS = 8
EXIT_RECESS = (-400, 1280, -384, 1312)
EXIT_APPROACH = (-384, 1280, -352, 1312)
EXIT_WALL_LINES = {403, 404, 406, 407, 408}


@dataclass(frozen=True)
class Segment:
    index: int
    x1: int
    y1: int
    x2: int
    y2: int
    front: int
    back: int
    flags: int
    special: int
    tag: int


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def wad_lumps(data: bytes) -> dict[str, bytes]:
    require(len(data) >= 12, "short WAD header")
    magic, count, directory = struct.unpack_from("<4sII", data)
    require(magic == b"IWAD" and directory <= len(data) and
            count <= (len(data) - directory) // 16, "invalid converted IWAD")
    entries = []
    for i in range(count):
        offset, size, raw_name = struct.unpack_from("<II8s", data,
                                                     directory + 16 * i)
        require(offset <= len(data) and size <= len(data) - offset,
                f"invalid WAD lump {i}")
        entries.append((raw_name.rstrip(b"\0").decode("ascii"),
                        data[offset:offset + size]))
    markers = [i for i, (name, _) in enumerate(entries) if name == "E1M1"]
    require(len(markers) == 1, "expected exactly one E1M1 map")
    start = markers[0]
    names = ("E1M1", "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
             "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT", "BLOCKMAP")
    require(tuple(name for name, _ in entries[start:start + len(names)]) == names,
            "unexpected E1M1 lump order")
    return dict(entries[start:start + len(names)])


def records(data: bytes, record: struct.Struct, expected: int, name: str) -> list[tuple]:
    require(len(data) == record.size * expected,
            f"{name}: expected {expected} records of {record.size} bytes")
    return list(record.iter_unpack(data))


def grid_geometry() -> tuple[int, int, int, int, int, list[int]]:
    text = GRID.read_text(encoding="ascii")
    values = {}
    for key in ("CELL", "GX0", "GY0", "WIDTH", "HEIGHT"):
        match = re.search(rf"^#define E1M1_{key} (-?\d+)$", text, re.M)
        require(match is not None, f"missing E1M1_{key} in grid header")
        values[key] = int(match.group(1))
    body = text.split("static const uint8_t e1m1_grid[", 1)[1].split("{", 1)[1]
    body = body.split("};", 1)[0]
    grid = [int(value) for value in re.findall(r"\b[01]\b", body)]
    require(values["CELL"] == 32 and values["WIDTH"] == 126 and
            values["HEIGHT"] == 110 and len(grid) == 126 * 110 and
            all(value in (0, 1) for value in grid),
            "E1M1 grid differs from expected 32-unit binary geometry")
    return (values["CELL"], values["GX0"], values["GY0"],
            values["WIDTH"], values["HEIGHT"], grid)


def read_map(data: bytes) -> tuple[list[Segment], list[tuple[int, int]],
                                   list[tuple[int, int, int]]]:
    lumps = wad_lumps(data)
    side_rows = records(lumps["SIDEDEFS"], SIDE, 1829, "SIDEDEFS")
    sectors = [(row[0], row[1], row[6])
               for row in records(lumps["SECTORS"], SECTOR, 182, "SECTORS")]
    require(all(0 <= side[5] < len(sectors) for side in side_rows),
            "sidedef references invalid sector")
    lines = []
    for index, row in enumerate(records(lumps["LINEDEFS"], LINE,
                                        1175, "LINEDEFS")):
        (x1, y1, x2, y2, line_index, dx, dy, front_side, back_side,
         *bbox, flags, special, tag, slope) = row
        require(line_index == index and dx == x2 - x1 and dy == y2 - y1 and
                0 <= front_side < len(side_rows) and
                (back_side == NO_SIDE or 0 <= back_side < len(side_rows)),
                f"LINEDEFS {index}: invalid converted record")
        require(all(value & 0xFFFF == 0 for value in (x1, y1, x2, y2)),
                f"LINEDEFS {index}: fractional endpoint")
        coords = tuple(value >> 16 for value in (x1, y1, x2, y2))
        require(all(-32768 <= value <= 32767 for value in coords),
                f"LINEDEFS {index}: endpoint exceeds int16")
        front = side_rows[front_side][5]
        back = side_rows[back_side][5] if back_side != NO_SIDE else NO_DOOR
        lines.append(Segment(index, *coords, front, back, flags, special, tag))
    require(Counter(line.special for line in lines if line.special) ==
            {1: 20, 2: 6, 11: 1, 23: 1, 26: 4, 62: 4, 88: 5, 117: 1},
            "E1M1 specials differ from expected converted map")
    return lines, [(row[0], row[1]) for row in sectors], sectors


def door_sectors(lines: list[Segment], sectors: list[tuple[int, int, int]]) -> set[int]:
    doors = {line.back for line in lines if line.special in (1, 26, 117)}
    for line in lines:
        if line.special == 2:
            require(line.tag != 0, f"walk-open line {line.index} lacks tag")
            doors.update(i for i, sector in enumerate(sectors)
                         if sector[2] == line.tag)
    require(doors == EXPECTED_DOOR_SECTORS, "closed-door sectors changed")
    require(all(sectors[i][0] == sectors[i][1] for i in doors),
            "expected initially closed door sector")
    return doors


def door_rectangles(lines: list[Segment], doors: set[int]) -> dict[int, tuple[int, int, int, int]]:
    """Find each closed door sector's four real WAD boundary edges."""
    result = {}
    for sector in sorted(doors):
        boundary = [line for line in lines
                    if sector in (line.front, line.back)]
        points = {(line.x1, line.y1) for line in boundary}
        points.update((line.x2, line.y2) for line in boundary)
        xs = sorted({x for x, _ in points})
        ys = sorted({y for _, y in points})
        require(len(boundary) == 4 and len(xs) == len(ys) == 2 and
                points == {(x, y) for x in xs for y in ys},
                f"door sector {sector} is not a four-edge rectangle")
        result[sector] = xs[0], ys[0], xs[1], ys[1]
    return result


def verify_gameplay_door_order(doors: set[int], digest: str) -> None:
    """Keep door_open[15] indexes identical to E1M1Gameplay.h's door array."""
    require(GAMEPLAY.is_file(),
            "generate E1M1Gameplay.h before generating E1M1Portals.h")
    text = GAMEPLAY.read_text(encoding="ascii")
    require(f"Converted Freedoom E1M1 WAD SHA-256: {digest}" in text,
            "E1M1Gameplay.h was generated from a different WAD")
    require("e1m1_doors[E1M1_DOOR_COUNT] = {" in text,
            "E1M1Gameplay.h lacks the door array")
    body = text.split("e1m1_doors[E1M1_DOOR_COUNT] = {", 1)[1].split("};", 1)[0]
    order = [int(value) for value in re.findall(r"/\* sector (\d+) \*/", body)]
    require(order == sorted(doors),
            "portal door indexes differ from E1M1Gameplay.h door order")


def line_cells(line: Segment, cell: int, gx0: int, gy0: int,
               width: int, height: int) -> set[int]:
    # Match build_lite_grid.py's point rasterization exactly. Axis-aligned
    # door boundaries do not skip cells between four-unit samples.
    steps = max(abs(line.x2 - line.x1), abs(line.y2 - line.y1)) // 4 + 1
    result = set()
    for i in range(steps + 1):
        x = line.x1 + (line.x2 - line.x1) * i // steps
        y = line.y1 + (line.y2 - line.y1) * i // steps
        gx, gy = x // cell - gx0, y // cell - gy0
        if 0 <= gx < width and 0 <= gy < height:
            result.add(gy * width + gx)
    return result


def add_rectangle_halo(by_cell: dict[int, set[int]],
                       rectangle: tuple[int, int, int, int], radius: int,
                       cell: int, gx0: int, gy0: int,
                       width: int, height: int) -> None:
    x0, y0, x1, y1 = rectangle
    for gy in range((y0 - radius) // cell - gy0,
                    (y1 + radius) // cell - gy0 + 1):
        for gx in range((x0 - radius) // cell - gx0,
                        (x1 + radius) // cell - gx0 + 1):
            require(0 <= gx < width and 0 <= gy < height,
                    "special rectangle halo escapes E1M1 grid")
            by_cell[gy * width + gx]


def intersects_cell(line: Segment, cx: int, cy: int, cell: int) -> bool:
    """Exact inclusive Liang-Barsky clip, with no floating-point rounding."""
    low, high = Fraction(0), Fraction(1)
    dx, dy = line.x2 - line.x1, line.y2 - line.y1
    for p, q in ((-dx, line.x1 - cx), (dx, cx + cell - line.x1),
                 (-dy, line.y1 - cy), (dy, cy + cell - line.y1)):
        if p == 0:
            if q < 0:
                return False
        else:
            t = Fraction(q, p)
            if p < 0:
                low = max(low, t)
            else:
                high = min(high, t)
            if low > high:
                return False
    return True


def generate() -> tuple[str, dict[str, int]]:
    cell, gx0, gy0, width, height, grid = grid_geometry()
    data = SOURCE.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    lines, _, sectors = read_map(data)
    doors = door_sectors(lines, sectors)
    rectangles = door_rectangles(lines, doors)
    verify_gameplay_door_order(doors, digest)
    permanent = {line.index for line in lines
                 if line.flags & 1 or line.back == NO_DOOR}
    dynamic = {line.index: line.front if line.front in doors else line.back
               for line in lines if line.back != NO_DOOR and
               (line.front in doors) != (line.back in doors)}
    require(len(dynamic) == 29, "expected 29 two-sided door boundaries")
    require(not (set(dynamic) & permanent),
            "dynamic door line also carries permanent-blocking flag")
    require((lines[407].x1, lines[407].y1, lines[407].x2,
             lines[407].y2, lines[407].special) ==
            (-400, 1280, -400, 1312, 11) and EXIT_WALL_LINES <= permanent,
            "exit switch recess differs from expected E1M1")

    by_cell: dict[int, set[int]] = defaultdict(set)
    for index in sorted(dynamic):
        for key in line_cells(lines[index], cell, gx0, gy0, width, height):
            by_cell[key].add(index)
    boundary_cells = set(by_cell)
    require(len(boundary_cells) == 79 and
            len(set().union(*by_cell.values())) == 29,
            "door coverage changed from 79 cells / 29 lines")

    # Door interiors are not necessarily covered by their two-sided portal
    # edges. Sector 84, for example, has a permanent one-sided far wall in an
    # interior grid cell. Include the whole rectangle plus an 8-unit halo so
    # both the renderer and the player's collision-radius cell scan use exact
    # WAD segments instead of an ambiguous coarse occupancy bit.
    for rectangle in rectangles.values():
        add_rectangle_halo(by_cell, rectangle, PLAYER_RADIUS,
                           cell, gx0, gy0, width, height)

    # The exit switch lies in a 16-unit-deep recess. Adjacent permanent wall
    # edges rasterize its opening as a whole 32-unit solid cell. Treat the
    # recess and the player's collision-radius halo exactly, like door cells.
    add_rectangle_halo(by_cell, EXIT_RECESS, PLAYER_RADIUS,
                       cell, gx0, gy0, width, height)
    add_rectangle_halo(by_cell, EXIT_APPROACH, PLAYER_RADIUS,
                       cell, gx0, gy0, width, height)

    # Add every *true* permanent segment intersecting each special cell. A
    # coarse grid pixel may contain unrelated solid and door lines, and exact
    # ray intersections must choose the nearest live line, not cell priority.
    for key in sorted(by_cell):
        gx, gy = key % width, key // width
        cx, cy = (gx + gx0) * cell, (gy + gy0) * cell
        for index in permanent:
            if intersects_cell(lines[index], cx, cy, cell):
                by_cell[key].add(index)
        for index in dynamic:
            if intersects_cell(lines[index], cx, cy, cell):
                by_cell[key].add(index)

    # Independent cross-check against the existing grid generator's sampling:
    # every coarse static line in a portal cell must have a true segment here.
    static_raster: dict[int, set[int]] = defaultdict(set)
    for index in permanent:
        for key in line_cells(lines[index], cell, gx0, gy0, width, height):
            if key in by_cell:
                static_raster[key].add(index)
    for key, indices in by_cell.items():
        require(bool(static_raster[key]) == bool(grid[key]),
                f"special cell {key}: grid does not match static WAD lines")
        require(static_raster[key] <= indices,
                f"special cell {key}: rasterized static segment omitted")
        cx, cy = (key % width + gx0) * cell, (key // width + gy0) * cell
        require(all(intersects_cell(lines[index], cx, cy, cell)
                    for index in indices),
                f"special cell {key}: non-intersecting segment referenced")

    special = sorted(by_cell)
    overlap = sum(grid[key] != 0 for key in special)
    require(sum(grid[key] != 0 for key in boundary_cells) == 38,
            "expected 38 portal-boundary / coarse-wall overlaps")
    # Verify every dynamic line's sampled cells contain that line. The exact
    # clip may add adjacent dynamic lines on common grid edges, intentionally.
    for index in dynamic:
        require(all(index in by_cell[key] for key in line_cells(
            lines[index], cell, gx0, gy0, width, height)),
                f"door line {index} missing from special-cell references")
    require(EXIT_WALL_LINES <= set().union(*by_cell.values()),
            "exit switch recess boundary missing from special cells")

    used = sorted(set().union(*by_cell.values()))
    require(len(used) <= 255, "segment IDs no longer fit uint8")
    local = {index: i for i, index in enumerate(used)}
    refs = []
    cells = []
    row_first = []
    for gy in range(height + 1):
        row_first.append(len(cells))
        if gy == height:
            break
        for key in special:
            if key // width != gy:
                continue
            first = len(refs)
            refs.extend(local[index] for index in sorted(by_cell[key]))
            require(len(refs) - first <= 255, "cell reference count exceeds uint8")
            cells.append((key % width, first, len(refs) - first))
    require(len(cells) == len(special) and len(refs) <= 65535 and
            max(row_first) <= 255, "portal table exceeds compact index types")

    door_index = [NO_DOOR] * len(sectors)
    for index, sector in enumerate(sorted(doors)):
        door_index[sector] = index
    require(sorted(value for value in door_index if value != NO_DOOR) ==
            list(range(len(doors))), "door index map is not one-to-one")

    out = [
        "/* Generated by tools/doom/build_lite_portals.py from Freedoom E1M1.",
        f" * Converted Freedoom E1M1 WAD SHA-256: {digest}",
        " * Regenerate with python tools/doom/build_lite_portals.py.",
        " * Door sector heights are runtime state; this table is immutable.",
        " * Each special cell replaces coarse e1m1_grid occupancy with exact",
        " * line intersections over [cell_entry, cell_exit]. */",
        "#pragma once",
        "#include <stdint.h>",
        f"#define E1M1_PORTAL_WIDTH {width}u",
        f"#define E1M1_PORTAL_HEIGHT {height}u",
        f"#define E1M1_PORTAL_GX0 ({gx0})",
        f"#define E1M1_PORTAL_GY0 ({gy0})",
        f"#define E1M1_PORTAL_CELL_SIZE {cell}u",
        f"#define E1M1_PORTAL_CELL_COUNT {len(cells)}u",
        f"#define E1M1_PORTAL_BOUNDARY_CELL_COUNT {len(boundary_cells)}u",
        f"#define E1M1_PORTAL_SEGMENT_COUNT {len(used)}u",
        f"#define E1M1_PORTAL_REFERENCE_COUNT {len(refs)}u",
        f"#define E1M1_PORTAL_DOOR_COUNT {len(doors)}u",
        f"#define E1M1_PORTAL_SECTOR_COUNT {len(sectors)}u",
        "#define E1M1_PORTAL_NO_DOOR 255u",
        "",
        "typedef struct {",
        "    int16_t x1, y1, x2, y2; /* map units, not Q8 */",
        "    uint8_t door_sector;     /* 255 = permanent blocking line */",
        "    uint8_t reserved;",
        "} E1M1PortalSegment;",
        "typedef struct {",
        "    uint16_t first_ref;",
        "    uint8_t x;",
        "    uint8_t ref_count;",
        "} E1M1PortalCell;",
        "",
        "/* Rows [row_first[y], row_first[y+1]) are sorted by cell x. */",
        f"static const uint8_t e1m1_portal_row_first[{height + 1}] = {{",
    ]
    for i in range(0, len(row_first), 16):
        out.append("    " + ", ".join(str(v) for v in row_first[i:i + 16]) + ",")
    out.extend(["};", "", "/* Sector ID -> e1m1_doors[] / door_open[] index; 255 = none. */",
                "static const uint8_t",
                "e1m1_door_index_by_sector[E1M1_PORTAL_SECTOR_COUNT] = {"])
    for i in range(0, len(door_index), 16):
        out.append("    " + ", ".join(str(v) for v in door_index[i:i + 16]) + ",")
    out.extend(["};", "", "static const E1M1PortalCell",
                "e1m1_portal_cells[E1M1_PORTAL_CELL_COUNT] = {"])
    for i, (x, first, count) in enumerate(cells):
        key = special[i]
        out.append(f"    {{{first}, {x}, {count}}}, /* grid ({x},{key // width}) */")
    out.extend(["};", "", "static const E1M1PortalSegment",
                "e1m1_portal_segments[E1M1_PORTAL_SEGMENT_COUNT] = {"])
    for index in used:
        line = lines[index]
        door = dynamic.get(index, NO_DOOR)
        out.append(f"    {{{line.x1}, {line.y1}, {line.x2}, {line.y2}, "
                   f"{door}, 0}}, /* WAD line {index} */")
    out.extend(["};", "", "/* Segment IDs for each special cell. */",
                "static const uint8_t",
                "e1m1_portal_refs[E1M1_PORTAL_REFERENCE_COUNT] = {"])
    for i in range(0, len(refs), 24):
        out.append("    " + ", ".join(str(v) for v in refs[i:i + 24]) + ",")
    out.extend(["};", ""])
    size = len(row_first) + len(door_index) + len(cells) * 4 + len(used) * 10 + len(refs)
    return "\n".join(out), {
        "doors": len(doors), "door_lines": len(dynamic),
        "boundary_cells": len(boundary_cells), "cells": len(cells),
        "overlap": overlap, "segments": len(used), "refs": len(refs),
        "packed_bytes": size, "max_segments_per_cell": max(c[2] for c in cells),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="validate and compare output without changing files")
    args = parser.parse_args()
    output, stats = generate()
    if args.check:
        require(OUTPUT.is_file(), f"missing generated header: {OUTPUT}")
        require(OUTPUT.read_text(encoding="ascii") == output,
                "E1M1Portals.h differs; regenerate with build_lite_portals.py")
    else:
        OUTPUT.write_text(output, encoding="ascii", newline="\n")
    print("portal geometry verified: " + ", ".join(
        f"{key}={value}" for key, value in stats.items()))


if __name__ == "__main__":
    main()
