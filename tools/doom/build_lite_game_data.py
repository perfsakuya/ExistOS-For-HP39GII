"""Generate compact, read-only E1M1 gameplay records from the converted WAD.

The input is GBADoom's converted Freedoom E1M1: LINEDEFS are 56-byte line_t
records with Q16.16 endpoints, while THINGS, SIDEDEFS, and SECTORS retain
their packed Doom record sizes. No game state or cell-to-door map is generated.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "System/applications/user/doom_port/data/freedoom-e1m1-gba.wad"
OUTPUT = ROOT / "System/applications/user/doom_lite/E1M1Gameplay.h"

THING_RECORD = struct.Struct("<hhhhh")
LINE_RECORD = struct.Struct("<iiiiIiiHHiiiiHhhh")
SIDE_RECORD = struct.Struct("<hhhhhh")
SECTOR_RECORD = struct.Struct("<hh8s8shhh")
VERTEX_RECORD = struct.Struct("<ii")

EXPECTED_COUNTS = {
    "THINGS": 292,
    "LINEDEFS": 1175,
    "SIDEDEFS": 1829,
    "VERTEXES": 1196,
    "SECTORS": 182,
}
EXPECTED_SPECIALS = {1: 20, 2: 6, 11: 1, 23: 1, 26: 4,
                     62: 4, 88: 5, 117: 1}
DOOR_MANUAL = 1
DOOR_BLUE = 2
DOOR_BLAZE = 3
DOOR_WALK = 4
NO_SIDE = 0xFFFF
NO_SECTOR = 0xFF


@dataclass(frozen=True)
class Thing:
    x: int
    y: int
    angle: int
    type: int
    options: int


@dataclass(frozen=True)
class Line:
    index: int
    x1: int
    y1: int
    x2: int
    y2: int
    front_side: int
    back_side: int
    flags: int
    special: int
    tag: int


@dataclass(frozen=True)
class Sector:
    floor: int
    ceiling: int
    special: int
    tag: int


@dataclass(frozen=True)
class SpecialLine:
    line: Line
    front_sector: int
    back_sector: int


@dataclass(frozen=True)
class Door:
    sector: int
    kind: int
    x0: int
    y0: int
    x1: int
    y1: int
    floor: int
    closed_ceiling: int
    open_ceiling: int


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def in_int16(value: int, what: str) -> int:
    require(-32768 <= value <= 32767, f"{what} exceeds int16")
    return value


def map_lumps(data: bytes) -> dict[str, bytes]:
    require(len(data) >= 12, "short WAD header")
    magic, count, directory = struct.unpack_from("<4sII", data)
    require(magic == b"IWAD", "expected converted IWAD")
    require(directory <= len(data) and
            count <= (len(data) - directory) // 16, "invalid WAD directory")
    entries = []
    for index in range(count):
        offset, size, raw_name = struct.unpack_from("<II8s", data,
                                                      directory + index * 16)
        require(offset <= len(data) and size <= len(data) - offset,
                f"invalid lump {index}")
        name = raw_name.rstrip(b"\0").decode("ascii")
        entries.append((name, data[offset:offset + size]))
    markers = [i for i, (name, _) in enumerate(entries) if name == "E1M1"]
    require(len(markers) == 1, "expected one E1M1 map marker")
    start = markers[0]
    expected = ("E1M1", "THINGS", "LINEDEFS", "SIDEDEFS", "VERTEXES",
                "SEGS", "SSECTORS", "NODES", "SECTORS", "REJECT",
                "BLOCKMAP")
    require(tuple(name for name, _ in entries[start:start + len(expected)])
            == expected, "unexpected E1M1 lump order")
    return dict(entries[start:start + len(expected)])


def unpack_records(data: bytes, record: struct.Struct, name: str,
                   expected: int) -> list[tuple]:
    require(len(data) == record.size * expected,
            f"{name}: expected {expected} records of {record.size} bytes")
    return list(record.iter_unpack(data))


def extract(data: bytes) -> tuple[list[Thing], list[SpecialLine], list[Door]]:
    lumps = map_lumps(data)
    records = {
        "THINGS": unpack_records(lumps["THINGS"], THING_RECORD, "THINGS", 292),
        "LINEDEFS": unpack_records(lumps["LINEDEFS"], LINE_RECORD,
                                    "LINEDEFS", 1175),
        "SIDEDEFS": unpack_records(lumps["SIDEDEFS"], SIDE_RECORD,
                                    "SIDEDEFS", 1829),
        "VERTEXES": unpack_records(lumps["VERTEXES"], VERTEX_RECORD,
                                    "VERTEXES", 1196),
        "SECTORS": unpack_records(lumps["SECTORS"], SECTOR_RECORD,
                                   "SECTORS", 182),
    }
    require(all(len(records[name]) == count
                for name, count in EXPECTED_COUNTS.items()),
            "unexpected E1M1 record count")
    things = [Thing(x, y, angle, type, options & 0xFFFF)
              for x, y, angle, type, options in records["THINGS"]]
    require([(t.x, t.y) for t in things if t.type == 1] == [(-416, 256)],
            "player start differs from expected E1M1")
    require([(t.x, t.y) for t in things if t.type == 5] == [(2192, 576)],
            "blue key differs from expected E1M1")
    require(all(0 <= t.angle < 360 and 0 <= t.type <= 65535 for t in things),
            "invalid THINGS angle or type")

    sides = records["SIDEDEFS"]
    sectors = [Sector(floor, ceiling, special, tag)
               for floor, ceiling, _, _, _, special, tag
               in records["SECTORS"]]
    require(len(sectors) < NO_SECTOR, "sector index no longer fits uint8")
    require(all(0 <= side[5] < len(sectors) for side in sides),
            "SIDEDEFS sector index out of bounds")

    lines = []
    for index, row in enumerate(records["LINEDEFS"]):
        (x1, y1, x2, y2, lineno, dx, dy, front, back,
         *bbox, flags, special, tag, slopetype) = row
        require(lineno == index and dx == x2 - x1 and dy == y2 - y1,
                f"LINEDEFS {index}: unexpected converted line geometry")
        require(len(bbox) == 4 and 0 <= front < len(sides) and
                (back == NO_SIDE or 0 <= back < len(sides)),
                f"LINEDEFS {index}: invalid sidedef")
        coords = tuple(in_int16(value >> 16, f"LINEDEFS {index} coordinate")
                       for value in (x1, y1, x2, y2))
        require(all((value & 0xFFFF) == 0 for value in (x1, y1, x2, y2)),
                f"LINEDEFS {index}: fractional endpoint")
        require(0 <= flags <= 65535 and 0 <= special <= 255 and
                0 <= tag <= 255, f"LINEDEFS {index}: field exceeds header type")
        lines.append(Line(index, *coords, front, back, flags, special, tag))

    special_lines = [
        SpecialLine(line, sides[line.front_side][5],
                    sides[line.back_side][5] if line.back_side != NO_SIDE
                    else NO_SECTOR)
        for line in lines if line.special
    ]
    require(dict(Counter(item.line.special for item in special_lines))
            == EXPECTED_SPECIALS, "E1M1 special LINEDEFS changed")
    exits = [item for item in special_lines if item.line.special == 11]
    require(len(exits) == 1 and exits[0].line.index == 407 and
            (exits[0].line.x1, exits[0].line.y1,
             exits[0].line.x2, exits[0].line.y2) ==
            (-400, 1280, -400, 1312) and
            exits[0].back_sector == NO_SECTOR,
            "exit switch differs from expected E1M1")

    door_kinds = {}
    for item in special_lines:
        kind = {1: DOOR_MANUAL, 26: DOOR_BLUE, 117: DOOR_BLAZE}.get(
            item.line.special)
        if kind is None:
            continue
        sector = item.back_sector
        require(sector != NO_SECTOR and
                door_kinds.get(sector, kind) == kind,
                f"door sector {sector}: missing or conflicting backside")
        door_kinds[sector] = kind
    for item in special_lines:
        if item.line.special != 2:
            continue
        targets = [index for index, sector in enumerate(sectors)
                   if sector.tag == item.line.tag]
        require(item.line.tag != 0 and targets,
                f"walk-open line {item.line.index}: no target sector")
        for sector in targets:
            require(door_kinds.get(sector, DOOR_WALK) == DOOR_WALK,
                    f"door sector {sector}: conflicting triggers")
            door_kinds[sector] = DOOR_WALK
    require(len(door_kinds) == 15 and
            {sector for sector, kind in door_kinds.items()
             if kind == DOOR_BLUE} == {51, 71} and
            {sector for sector, kind in door_kinds.items()
             if kind == DOOR_WALK} == {77, 145},
            "closed-door sectors differ from expected E1M1")

    doors = []
    for sector_index, kind in sorted(door_kinds.items()):
        boundary = []
        neighbors = []
        for line in lines:
            front_sector = sides[line.front_side][5]
            back_sector = (sides[line.back_side][5]
                           if line.back_side != NO_SIDE else NO_SECTOR)
            if sector_index not in (front_sector, back_sector):
                continue
            boundary.append(line)
            other = (back_sector if front_sector == sector_index
                     else front_sector)
            if other != NO_SECTOR and other != sector_index:
                neighbors.append(other)
        points = {(line.x1, line.y1) for line in boundary}
        points.update((line.x2, line.y2) for line in boundary)
        xs = sorted({x for x, _ in points})
        ys = sorted({y for _, y in points})
        if len(xs) == len(ys) == 2:
            expected_edges = {
                frozenset(((xs[0], ys[0]), (xs[1], ys[0]))),
                frozenset(((xs[0], ys[1]), (xs[1], ys[1]))),
                frozenset(((xs[0], ys[0]), (xs[0], ys[1]))),
                frozenset(((xs[1], ys[0]), (xs[1], ys[1]))),
            }
        else:
            expected_edges = set()
        observed_edges = {
            frozenset(((line.x1, line.y1), (line.x2, line.y2)))
            for line in boundary
        }
        require(len(boundary) == 4 and len(xs) == len(ys) == 2 and
                points == {(x, y) for x in xs for y in ys} and
                observed_edges == expected_edges,
                f"door sector {sector_index}: not an axis-aligned rectangle")
        closed = sectors[sector_index]
        require(closed.floor == closed.ceiling and neighbors,
                f"door sector {sector_index}: not closed or isolated")
        open_ceiling = min(sectors[other].ceiling for other in neighbors) - 4
        require(open_ceiling > closed.ceiling,
                f"door sector {sector_index}: no opening clearance")
        doors.append(Door(sector_index, kind, xs[0], ys[0], xs[1], ys[1],
                          closed.floor, closed.ceiling,
                          in_int16(open_ceiling, "door open ceiling")))
    return things, special_lines, doors


def render_header(data: bytes) -> str:
    things, special_lines, doors = extract(data)
    digest = hashlib.sha256(data).hexdigest()
    out = [
        "/* Generated by tools/doom/build_lite_game_data.py. Do not edit.",
        f" * Converted Freedoom E1M1 WAD SHA-256: {digest}",
        " * Coordinates and heights are in Doom map units.",
        " */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define E1M1_THING_COUNT {len(things)}u",
        f"#define E1M1_SPECIAL_LINE_COUNT {len(special_lines)}u",
        f"#define E1M1_DOOR_COUNT {len(doors)}u",
        "#define E1M1_NO_SIDE 65535u",
        "#define E1M1_NO_SECTOR 255u",
        "#define E1M1_DOOR_MANUAL 1u",
        "#define E1M1_DOOR_BLUE 2u",
        "#define E1M1_DOOR_BLAZE 3u",
        "#define E1M1_DOOR_WALK 4u",
        "",
        "typedef struct {",
        "    int16_t x, y;",
        "    int16_t angle;",
        "    uint16_t type, options;",
        "} E1M1Thing;",
        "",
        "typedef struct {",
        "    int16_t x1, y1, x2, y2;",
        "    uint16_t linedef, front_side, back_side, flags;",
        "    uint8_t special, tag, front_sector, back_sector;",
        "} E1M1SpecialLine;",
        "",
        "typedef struct {",
        "    int16_t x0, y0, x1, y1; /* [x0,x1] by [y0,y1] */",
        "    int16_t floor_height, closed_ceiling, open_ceiling;",
        "    uint8_t sector, kind;",
        "} E1M1Door;",
        "",
        "static const E1M1Thing e1m1_things[E1M1_THING_COUNT] = {",
    ]
    for index, thing in enumerate(things):
        out.append(f"    {{{thing.x}, {thing.y}, {thing.angle}, "
                   f"{thing.type}, {thing.options}}}, /* thing {index} */")
    out.extend(["};", "",
                "static const E1M1SpecialLine "
                "e1m1_special_lines[E1M1_SPECIAL_LINE_COUNT] = {"])
    for item in special_lines:
        line = item.line
        out.append(
            f"    {{{line.x1}, {line.y1}, {line.x2}, {line.y2}, "
            f"{line.index}, {line.front_side}, {line.back_side}, "
            f"{line.flags}, {line.special}, {line.tag}, "
            f"{item.front_sector}, {item.back_sector}}},"
            f" /* linedef {line.index} */"
        )
    out.extend(["};", "", "static const E1M1Door e1m1_doors[E1M1_DOOR_COUNT] = {"])
    for door in doors:
        out.append(
            f"    {{{door.x0}, {door.y0}, {door.x1}, {door.y1}, "
            f"{door.floor}, {door.closed_ceiling}, {door.open_ceiling}, "
            f"{door.sector}, {door.kind}}}, /* sector {door.sector} */"
        )
    out.extend(["};", ""])
    return "\n".join(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="verify the checked-in header without writing")
    args = parser.parse_args()
    rendered = render_header(SOURCE.read_bytes())
    if args.check:
        require(OUTPUT.exists() and OUTPUT.read_text(encoding="ascii")
                == rendered, f"{OUTPUT} needs regeneration")
        print(f"E1M1 gameplay header verified: {OUTPUT}")
    else:
        OUTPUT.write_text(rendered, encoding="ascii", newline="\n")
        print(f"E1M1 gameplay header generated: {OUTPUT}")


if __name__ == "__main__":
    main()
