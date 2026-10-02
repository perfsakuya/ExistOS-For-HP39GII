"""Check necessary sector-graph progression conditions for native E1M1/2.

This deliberately over-approximates reachability. It ignores corridor width,
wall occlusion for use, enemies and timing, and treats each WAD sector as one
connected region. It is a route regression aid, not proof of a playable run.
"""
from __future__ import annotations

import argparse
from collections import deque
import json
import math
from pathlib import Path

import build_game_maps as maps

MANUAL = {1, 26, 27, 28, 31, 32, 33, 34, 117, 118}
DOORS = MANUAL | {2, 63, 103, 109}
WALK = {2, 19, 38, 58, 88, 97, 109, 120}
USE = {1, 11, 23, 26, 27, 28, 31, 32, 33, 34, 62, 63, 71, 102, 103, 117, 118, 123}
KEY_FOR_SPECIAL = {26: "blue", 32: "blue", 27: "yellow", 34: "yellow", 28: "red", 33: "red"}
KEY_TYPES = {5: "blue", 40: "blue", 6: "yellow", 39: "yellow", 13: "red", 38: "red"}
PLAYER_HEIGHT = 56


def pickup_reach(data, thing, reachable, floors, max_above=PLAYER_HEIGHT, max_below=8):
    """Sample center and a 24-unit pickup disk; do not require platform entry.

    This retains the graph audit's optimistic omission of wall/corridor tests.
    Original Doom permits touching an item up to the player's full height above
    their feet, rather than only their 24-unit step height.
    """
    points = [(thing[0], thing[1])]
    for radius in (8, 16, 24):
        for angle in range(32):
            points.append((thing[0] + round(math.cos(angle * math.pi / 16) * radius),
                           thing[1] + round(math.sin(angle * math.pi / 16) * radius)))
    for x, y in points:
        if (thing[0] - x) ** 2 + (thing[1] - y) ** 2 > 24 ** 2:
            continue
        sector = maps.sector_at(data.nodes, data.subsectors, x, y)
        if sector not in reachable:
            continue
        if any(-max_below <= item_height - player_height <= max_above
               for item_height in floors[thing[5]] for player_height in floors[sector]):
            return {"x": x, "y": y, "player_sector": sector}
    return None


def targets(data: maps.MapData, line: tuple) -> list[int]:
    special, tag, back = line[5], line[6], line[8]
    if special in MANUAL:
        return [back] if back != maps.NO_SECTOR else []
    return [i for i, sector in enumerate(data.sectors) if tag and sector[4] == tag]


def target_height(sector: tuple, special: int) -> int | None:
    floor, ceiling, light, kind, tag, lowest, highest = sector[:7]
    if special in (19, 102):
        value = highest
    elif special in (23, 38, 62, 88, 120, 123):
        value = lowest
    elif special == 58:
        value = floor + 24
    elif special == 71:
        value = highest + (8 if highest != floor else 0)
    else:
        return None
    # Doom movers may close an unoccupied decorative sector below body
    # height. Occupancy can pause movement; it must not change the target.
    return value


def passage(line: tuple, source: int, floors: list[set[int]], ceilings: list[int]) -> bool:
    front, back = line[7:9]
    if line[4] & 1 or back == maps.NO_SECTOR:
        return False
    dest = back if source == front else front
    return any(min(ceilings[front], ceilings[back]) - max(fs, fd) >= PLAYER_HEIGHT
               and fd - fs <= 24 for fs in floors[source] for fd in floors[dest])


def route_audit(data: maps.MapData) -> dict:
    floors = [{s[0]} for s in data.sectors]
    ceilings = [s[1] for s in data.sectors]
    adjacency = [[] for _ in data.sectors]
    for i, line in enumerate(data.lines):
        if line[8] != maps.NO_SECTOR:
            adjacency[line[7]].append(i)
            adjacency[line[8]].append(i)
    keys, fired, teleport_edges = set(), set(), []
    key_pickups = {}
    stages = []
    reachable = {data.start[3]}
    for iteration in range(64):
        queue = deque(reachable)
        while queue:
            source = queue.popleft()
            for line_index in adjacency[source]:
                line = data.lines[line_index]
                dest = line[8] if source == line[7] else line[7]
                if dest not in reachable and passage(line, source, floors, ceilings):
                    reachable.add(dest)
                    queue.append(dest)
            for origin, dest in teleport_edges:
                if origin == source and dest not in reachable:
                    reachable.add(dest)
                    queue.append(dest)
        new_keys = set()
        for thing_index, thing in enumerate(data.things):
            if thing[4] & 2 and not thing[4] & 16 and thing[3] in KEY_TYPES:
                pickup = pickup_reach(data, thing, reachable, floors)
                if not pickup:
                    continue
                color = KEY_TYPES[thing[3]]
                if color not in keys:
                    keys.add(color)
                    new_keys.add(color)
                    key_pickups[thing_index] = pickup
        activated = []
        for line_index in data.special_refs:
            line = data.lines[line_index]
            special, tag, front, back = line[5], line[6], line[7], line[8]
            if line_index in fired or front not in reachable or special not in WALK | USE:
                continue
            if KEY_FOR_SPECIAL.get(special) not in keys and special in KEY_FOR_SPECIAL:
                continue
            if special in WALK and not passage(line, front, floors, ceilings):
                continue
            if special == 97:
                destinations = [t[5] for t in data.things if t[3] == 14 and data.sectors[t[5]][4] == tag]
                if destinations:
                    teleport_edges.extend((front, dest) for dest in destinations)
                else:
                    continue
            for sector_index in targets(data, line):
                sector = data.sectors[sector_index]
                if special in DOORS:
                    ceilings[sector_index] = max(ceilings[sector_index], sector[8] - 4)
                else:
                    value = target_height(sector, special)
                    if value is not None:
                        # Initial, moved and platform-return heights remain
                        # available in this optimistic necessary-condition model.
                        floors[sector_index].add(value)
            fired.add(line_index)
            activated.append(line_index)
        stages.append({"iteration": iteration, "reachable_sector_count": len(reachable),
                       "new_keys": sorted(new_keys), "newly_activated_lines": activated})
        if not activated and not new_keys:
            break
    key_records = [{"thing": i, "color": KEY_TYPES[t[3]], "x": t[0], "y": t[1], "sector": t[5],
                    "sector_reachable": t[5] in reachable,
                    "pickup_reachable_optimistic": i in key_pickups,
                    "pickup_sample": key_pickups.get(i)}
                   for i, t in enumerate(data.things) if t[3] in KEY_TYPES and t[4] & 2 and not t[4] & 16]
    exit_records = [{"line": i, "front_sector": data.lines[i][7],
                     "sector_reachable": data.lines[i][7] in reachable}
                    for i in data.special_refs if data.lines[i][5] == 11]
    special_records = []
    for i in data.special_refs:
        line = data.lines[i]
        special_records.append({"line": i, "special": line[5], "tag": line[6],
                                "front_sector": line[7], "back_sector": line[8],
                                "native_supported": line[5] in WALK | USE,
                                "cosmetic_only": line[5] == 48,
                                "front_sector_reachable": line[7] in reachable,
                                "targets": [{"sector": sector, "initial_floor": data.sectors[sector][0],
                                             "initial_ceiling": data.sectors[sector][1],
                                             "floor_target": target_height(data.sectors[sector], line[5])}
                                            for sector in targets(data, line)]})
    return {"map": data.name, "keys": key_records, "exits": exit_records,
            "reachable_sector_count_optimistic": len(reachable), "sector_count": len(data.sectors),
            "all_key_sectors_reachable": all(k["sector_reachable"] for k in key_records),
            "all_key_pickups_reachable_optimistic": all(k["pickup_reachable_optimistic"] for k in key_records),
            "all_exit_front_sectors_reachable": all(e["sector_reachable"] for e in exit_records),
            "unreachable_sectors": sorted(set(range(len(data.sectors))) - reachable),
            "stages": stages, "special_records": special_records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=maps.ROOT / "docs/doom-game-route-audit.json")
    args = parser.parse_args()
    entries = maps.directory(maps.SOURCE.read_bytes())
    data = [maps.extract(name, i, maps.map_lumps(entries, name)) for i, name in enumerate(("E1M1", "E1M2"))]
    results = [route_audit(item) for item in data]
    report = {"schema": 1, "source_sha256": maps.EXPECTED_SHA256,
              "limits": ["Optimistic necessary-condition sector graph only; not proof of playability.",
                         "Ignores corridor width, wall occlusion of use, enemies and trigger timing.",
                         "Each WAD sector is treated as connected; all initial/final mover heights remain available."],
              "maps": results}
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps([{key: value for key, value in result.items() if key not in
                       ("special_records", "unreachable_sectors")} for result in results], indent=2))


if __name__ == "__main__":
    main()
