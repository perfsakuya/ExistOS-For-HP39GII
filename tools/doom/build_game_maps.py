"""Generate immutable, bounds-checked E1M1/E1M2 geometry from standard IWAD.

The runtime uses original BSP point queries and sparse 32-unit supercover
line cells. No cell, sector, tag, or line reference is reduced to eight bits.
Legacy E1M1 Lite headers and the converted GBA WAD are deliberately separate.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct

from audit_expansion import RECORDS, MAP_LUMPS, directory, require

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT.parent / "Freedoom-research/extracted/freedoom-0.13.0/freedoom1.wad"
OUTPUT = ROOT / "System/applications/user/doom_lite"
REPORT = ROOT / "docs/doom-game-maps.json"
EXPECTED_SHA256 = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d"
NO_SECTOR = 65535
CELL = 32
MAX_THINGS, MAX_SECTORS, MAX_LINES = 356, 380, 2290


@dataclass
class MapData:
    name: str
    index: int
    things: list[tuple]
    lines: list[tuple]
    sectors: list[tuple]
    nodes: list[tuple]
    subsectors: list[int]
    row_first: list[int]
    cells: list[tuple]
    cell_refs: list[int]
    special_refs: list[int]
    tags: list[tuple]
    tag_refs: list[int]
    neighbor_refs: list[int]
    origin: tuple[int, int]
    dimensions: tuple[int, int]
    start: tuple[int, int, int, int]


def map_lumps(entries: list[tuple[str, bytes]], name: str) -> dict[str, bytes]:
    markers = [i for i, item in enumerate(entries) if item[0] == name]
    require(len(markers) == 1, f"expected one {name} marker")
    first = markers[0] + 1
    records = entries[first:first + len(MAP_LUMPS)]
    require(tuple(item[0] for item in records) == MAP_LUMPS,
            f"{name}: map lump order differs from standard Doom")
    return dict(records)


def point_side(x: int, y: int, node: tuple) -> int:
    nx, ny, dx, dy, *_ = node
    if dx == 0:
        return int(dy > 0 if x <= nx else dy < 0)
    if dy == 0:
        return int(dx < 0 if y <= ny else dx > 0)
    return int((y - ny) * dx >= (x - nx) * dy)


def sector_at(nodes: list[tuple], subsectors: list[int], x: int, y: int) -> int:
    if not nodes:
        return subsectors[0]
    child = len(nodes) - 1
    for _ in range(len(nodes) + 1):
        if child & 0x8000:
            require((child & 0x7fff) < len(subsectors), "invalid BSP subsector")
            return subsectors[child & 0x7fff]
        require(child < len(nodes), "invalid BSP node")
        node = nodes[child]
        child = node[4 + point_side(x, y, node)]
    raise ValueError("cyclic BSP")


def orient(a: tuple, b: tuple, c: tuple) -> int:
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def segments_touch(a: tuple, b: tuple, c: tuple, d: tuple) -> bool:
    if max(a[0], b[0]) < min(c[0], d[0]) or max(c[0], d[0]) < min(a[0], b[0]):
        return False
    if max(a[1], b[1]) < min(c[1], d[1]) or max(c[1], d[1]) < min(a[1], b[1]):
        return False
    ab_c, ab_d = orient(a, b, c), orient(a, b, d)
    cd_a, cd_b = orient(c, d, a), orient(c, d, b)
    return (ab_c <= 0 <= ab_d or ab_d <= 0 <= ab_c) and \
           (cd_a <= 0 <= cd_b or cd_b <= 0 <= cd_a)


def touches_cell(line: tuple, gx: int, gy: int) -> bool:
    x1, y1, x2, y2, *_ = line
    x0, y0 = gx * CELL, gy * CELL
    if x0 <= x1 <= x0 + CELL and y0 <= y1 <= y0 + CELL:
        return True
    if x0 <= x2 <= x0 + CELL and y0 <= y2 <= y0 + CELL:
        return True
    corners = ((x0, y0), (x0 + CELL, y0),
               (x0 + CELL, y0 + CELL), (x0, y0 + CELL))
    return any(segments_touch((x1, y1), (x2, y2), corners[i], corners[(i + 1) % 4])
               for i in range(4))


def build_cells(lines: list[tuple], origin: tuple, dimensions: tuple):
    origin_x, origin_y = origin[0] // CELL, origin[1] // CELL
    width, height = dimensions
    by_cell = defaultdict(list)
    for index, line in enumerate(lines):
        x1, y1, x2, y2, *_ = line
        low_x, high_x = min(x1, x2), max(x1, x2)
        low_y, high_y = min(y1, y2), max(y1, y2)
        # Closed cells include a line exactly on the shared edge in both cells.
        x_first = low_x // CELL - int(low_x % CELL == 0)
        y_first = low_y // CELL - int(low_y % CELL == 0)
        for gy in range(y_first, high_y // CELL + 1):
            for gx in range(x_first, high_x // CELL + 1):
                if touches_cell(line, gx, gy):
                    rx, ry = gx - origin_x, gy - origin_y
                    require(0 <= rx < width and 0 <= ry < height,
                            "line supercover outside grid")
                    by_cell[(rx, ry)].append(index)
    rows, cells, refs = [0], [], []
    for y in range(height):
        for x in sorted(x for x, gy in by_cell if gy == y):
            values = by_cell[(x, y)]
            cells.append((x, len(refs), len(values)))
            refs.extend(values)
        rows.append(len(cells))
    require(len(cells) <= 65535 and len(refs) <= 65535,
            "sparse geometry references exceed uint16")
    return rows, cells, refs


def extract(name: str, index: int, lumps: dict[str, bytes]) -> MapData:
    records = {}
    for kind, record in RECORDS.items():
        require(len(lumps[kind]) % record.size == 0, f"{name}/{kind}: malformed records")
        records[kind] = list(record.iter_unpack(lumps[kind]))
    vertices, sides, raw_lines = (records[k] for k in ("VERTEXES", "SIDEDEFS", "LINEDEFS"))
    raw_sectors, raw_things = records["SECTORS"], records["THINGS"]
    require(0 < len(raw_sectors) <= MAX_SECTORS, "sector capacity exceeded")
    require(0 < len(raw_lines) <= MAX_LINES, "line capacity exceeded")
    require(0 < len(raw_things) <= MAX_THINGS, "thing capacity exceeded")
    require(all(side[5] < len(raw_sectors) for side in sides), "invalid sidedef sector")
    lines = []
    sector_points, neighbors = defaultdict(list), defaultdict(list)
    for v1, v2, flags, special, tag, front, back in raw_lines:
        require(v1 < len(vertices) and v2 < len(vertices), "invalid linedef vertex")
        require(front < len(sides) and (back == 65535 or back < len(sides)),
                "invalid linedef sidedef")
        x1, y1 = vertices[v1]
        x2, y2 = vertices[v2]
        front_sector = sides[front][5]
        back_sector = NO_SECTOR if back == 65535 else sides[back][5]
        lines.append((x1, y1, x2, y2, flags, special, tag, front_sector, back_sector))
        for sector in (front_sector, back_sector):
            if sector != NO_SECTOR:
                sector_points[sector].extend(((x1, y1), (x2, y2)))
        if back_sector != NO_SECTOR and back_sector != front_sector:
            for sector, adjacent in ((front_sector, back_sector), (back_sector, front_sector)):
                if adjacent not in neighbors[sector]:
                    neighbors[sector].append(adjacent)
    nodes = []
    require(len(records["NODES"]) < 0x8000 and len(records["SSECTORS"]) < 0x8000,
            "BSP index exceeds Doom child encoding")
    for node_index, row in enumerate(records["NODES"]):
        for child in row[-2:]:
            require((child & 0x7fff) < len(records["SSECTORS"]) if child & 0x8000
                    else child < node_index, "invalid or cyclic BSP child")
        require(row[2] or row[3], "zero-length BSP partition")
        nodes.append((*row[:4], *row[-2:]))
    subsectors = []
    seg_covered = [False] * len(records["SEGS"])
    for count, first in records["SSECTORS"]:
        require(count > 0 and first + count <= len(records["SEGS"]), "invalid subsector seg range")
        require(not any(seg_covered[first:first + count]), "overlapping subsector seg ranges")
        seg_covered[first:first + count] = [True] * count
        sectors = set()
        for seg in records["SEGS"][first:first + count]:
            v1, v2, angle, line_index, side, offset = seg
            require(v1 < len(vertices) and v2 < len(vertices) and line_index < len(lines),
                    "invalid seg geometry index")
            require(side in (0, 1), "invalid seg direction")
            sector = lines[line_index][7 + side]
            require(sector != NO_SECTOR, "subsector seg has no sector")
            sectors.add(sector)
        require(len(sectors) == 1, "subsector contains conflicting sectors")
        subsectors.append(sectors.pop())
    require(bool(subsectors), "empty BSP")
    require(all(seg_covered), "BSP leaves do not cover all seg records")
    things = []
    for x, y, angle, kind, options in raw_things:
        require(0 <= angle < 360, "invalid thing angle")
        things.append((x, y, angle, kind, options, sector_at(nodes, subsectors, x, y)))
    starts = [t for t in things if t[3] == 1]
    require(len(starts) == 1, "expected one single-player start")
    start = (starts[0][0], starts[0][1], starts[0][2], starts[0][5])
    xs, ys = zip(*(point for line in lines for point in ((line[0], line[1]), (line[2], line[3]))))
    origin = ((min(xs) // CELL - 1) * CELL, (min(ys) // CELL - 1) * CELL)
    dimensions = (max(xs) // CELL - origin[0] // CELL + 2,
                  max(ys) // CELL - origin[1] // CELL + 2)
    require(all(-32768 <= value <= 32767 for value in origin), "grid origin exceeds int16")
    require(all(0 < value <= 65535 for value in dimensions), "grid dimensions exceed uint16")
    rows, cells, refs = build_cells(lines, origin, dimensions)
    sector_data, neighbor_refs = [], []
    for index_sector, row in enumerate(raw_sectors):
        floor, ceiling, floor_tex, ceil_tex, light, special, tag = row
        adjacent = neighbors[index_sector]
        floor_neighbors = [raw_sectors[n][0] for n in adjacent]
        ceiling_neighbors = [raw_sectors[n][1] for n in adjacent]
        higher = [value for value in floor_neighbors if value > floor]
        first = len(neighbor_refs)
        neighbor_refs.extend(adjacent)
        points = sector_points[index_sector]
        require(bool(points), "sector has no referenced boundary")
        px, py = zip(*points)
        sector_data.append((floor, ceiling, light, special, tag,
                            min([floor, *floor_neighbors]),
                            max(floor_neighbors, default=floor), min(higher, default=floor),
                            min(ceiling_neighbors, default=ceiling),
                            max(ceiling_neighbors, default=ceiling), first, len(adjacent),
                            min(px), min(py), max(px), max(py)))
    groups = defaultdict(list)
    for i, sector in enumerate(raw_sectors):
        if sector[6]:
            groups[sector[6]].append(i)
    tags, tag_refs = [], []
    for tag, values in sorted(groups.items()):
        tags.append((tag, len(tag_refs), len(values)))
        tag_refs.extend(values)
    return MapData(name, index, things, lines, sector_data, nodes, subsectors,
                   rows, cells, refs, [i for i, line in enumerate(lines) if line[5]],
                   tags, tag_refs, neighbor_refs, origin, dimensions, start)


def array(kind: str, name: str, values: list, fields: bool = True) -> str:
    count = max(1, len(values))
    text = [f"static const {kind} {name}[{count}] = {{"]
    if fields:
        text.extend("    {" + ", ".join(str(v) for v in value) + "}," for value in values)
    else:
        text.extend("    " + ", ".join(str(v) for v in values[i:i + 16]) + ","
                    for i in range(0, len(values), 16))
    if not values:
        text.append("    {0}," if fields else "    0,")
    text.append("};\n")
    return "\n".join(text)


def generate_header(data: MapData, source_hash: str) -> str:
    prefix = data.name.lower()
    parts = ["/* Generated by tools/doom/build_game_maps.py; do not edit.",
             f" * Standard Freedoom IWAD SHA256: {source_hash}",
             " * All arrays are immutable; no runtime allocation is required. */",
             "#pragma once\n", '#include "DoomMap.h"\n']
    for field, kind, structured in (
        ("things", "DoomMapThing", True), ("lines", "DoomMapLine", True),
        ("sectors", "DoomMapSector", True), ("nodes", "DoomMapNode", True),
        ("subsectors", "uint16_t", False), ("row_first", "uint16_t", False),
        ("cells", "DoomMapCell", True), ("cell_refs", "uint16_t", False),
        ("special_refs", "uint16_t", False), ("tags", "DoomMapTag", True),
        ("tag_refs", "uint16_t", False), ("neighbor_refs", "uint16_t", False)):
        values = getattr(data, field)
        if field == "nodes":
            parts.append(f"static const DoomMapNode {prefix}_nodes[{len(values)}] = {{")
            parts.extend("    {" + ", ".join(str(v) for v in row[:4]) +
                         ", {" + str(row[4]) + ", " + str(row[5]) + "}}," for row in values)
            parts.append("};\n")
        else:
            parts.append(array(kind, f"{prefix}_{field}", values, structured))
    parts.extend((f"static const DoomMap {prefix}_map = {{",
                  f'    .name = "{data.name}", .index = {data.index},',
                  f"    .start_x = {data.start[0]}, .start_y = {data.start[1]},",
                  f"    .start_angle = {data.start[2]}, .start_sector = {data.start[3]},",
                  f"    .grid_x0 = {data.origin[0]}, .grid_y0 = {data.origin[1]},",
                  f"    .grid_width = {data.dimensions[0]}, .grid_height = {data.dimensions[1]},"))
    field_pairs = (("thing_count", "things"), ("line_count", "lines"),
                   ("sector_count", "sectors"), ("node_count", "nodes"),
                   ("subsector_count", "subsectors"), ("cell_count", "cells"),
                   ("cell_ref_count", "cell_refs"), ("special_line_count", "special_refs"),
                   ("tag_count", "tags"), ("neighbor_ref_count", "neighbor_refs"))
    parts.extend(f"    .{field} = {len(getattr(data, values))}," for field, values in field_pairs)
    parts.extend(f"    .{field} = {prefix}_{values}," for field, values in (
        ("things", "things"), ("lines", "lines"), ("sectors", "sectors"),
        ("nodes", "nodes"), ("subsector_sector", "subsectors"),
        ("row_first", "row_first"), ("cells", "cells"), ("cell_refs", "cell_refs"),
        ("special_line_refs", "special_refs"), ("tags", "tags"),
        ("tag_sector_refs", "tag_refs"), ("neighbor_refs", "neighbor_refs")))
    parts.append("};\n")
    return "\n".join(parts)


def audit(data: MapData) -> dict:
    cells_by_position = {}
    for gy in range(data.dimensions[1]):
        for cell in data.cells[data.row_first[gy]:data.row_first[gy + 1]]:
            x, first, count = cell
            cells_by_position[(x, gy)] = data.cell_refs[first:first + count]
    checks = 0
    for index, line in enumerate(data.lines):
        x1, y1, x2, y2, *_ = line
        for fraction in range(17):
            # Use rational coordinates. Integer floor is exact even when negative.
            x_num = x1 * 16 + (x2 - x1) * fraction
            y_num = y1 * 16 + (y2 - y1) * fraction
            gx = (x_num - data.origin[0] * 16) // (CELL * 16)
            gy = (y_num - data.origin[1] * 16) // (CELL * 16)
            require(index in cells_by_position[(gx, gy)], "supercover misses line sample")
            checks += 1
    payloads = {
        "things": len(data.things) * 12, "lines": len(data.lines) * 18,
        "sectors": len(data.sectors) * 32, "nodes": len(data.nodes) * 12,
        "subsector_sector": len(data.subsectors) * 2,
        "row_first": len(data.row_first) * 2, "cells": len(data.cells) * 6,
        "cell_refs": len(data.cell_refs) * 2, "special_line_refs": len(data.special_refs) * 2,
        "tags": len(data.tags) * 6, "tag_sector_refs": len(data.tag_refs) * 2,
        "neighbor_refs": len(data.neighbor_refs) * 2,
    }
    return {"name": data.name, "start": list(data.start),
            "counts": {kind: len(getattr(data, kind)) for kind in
                       ("things", "lines", "sectors", "nodes", "subsectors", "cells", "cell_refs")},
            "grid_origin": list(data.origin), "grid_dimensions": list(data.dimensions),
            "data_bytes_excluding_descriptor": sum(payloads.values()),
            "payload_bytes": payloads, "line_specials": dict(sorted(Counter(l[5] for l in data.lines if l[5]).items())),
            "cell_max_line_refs": max(cell[2] for cell in data.cells),
            "geometry_line_sample_checks": checks,
            "thing_sector_queries": len(data.things),
            "sector_max_reference": len(data.sectors) - 1,
            "validation": "all WAD indices, BSP children/subsectors, and line-cell samples checked"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wad", type=Path, default=SOURCE)
    parser.add_argument("--output-dir", type=Path, default=OUTPUT)
    parser.add_argument("--report", type=Path, default=REPORT)
    parser.add_argument("--check", action="store_true", help="fail if generated files differ; never write")
    args = parser.parse_args()
    source = args.wad.read_bytes()
    source_hash = hashlib.sha256(source).hexdigest()
    require(source_hash == EXPECTED_SHA256, "source is not the pinned Freedoom 0.13.0 IWAD")
    entries = directory(source)
    maps = [extract(name, index, map_lumps(entries, name)) for index, name in enumerate(("E1M1", "E1M2"))]
    report = {"schema": 1, "source_sha256": source_hash, "maps": [audit(data) for data in maps],
              "limits": ["This checks data and geometry, not Doom compatibility or hardware FPS.",
                         "BSP leaf membership outside enclosed map boundaries is not a walkability test."]}
    outputs = [(args.output_dir / f"{data.name}MapData.h", generate_header(data, source_hash)) for data in maps]
    outputs.append((args.report, json.dumps(report, indent=2, sort_keys=True) + "\n"))
    for path, content in outputs:
        if args.check:
            require(path.exists() and path.read_text(encoding="utf-8") == content, f"stale generated file: {path}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    print(json.dumps({"check": args.check, "maps": [audit(data) for data in maps]}, indent=2))


if __name__ == "__main__":
    main()
