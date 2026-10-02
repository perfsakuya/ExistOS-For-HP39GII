"""Audit a standard Doom-format IWAD for a future compact Game expansion.

Reads map counts, present features and sprite payload sizes. It does not
convert maps, claim playability, or change firmware. Runtime budgets are
explicit proposals, excluding the OS, renderer, stack and resource caches.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct

from build_lite_sprites import decode_patch, pack_pixels

RECORDS = {
    "THINGS": struct.Struct("<hhHHH"),
    "LINEDEFS": struct.Struct("<HHHHHHH"),
    "SIDEDEFS": struct.Struct("<hh8s8s8sH"),
    "VERTEXES": struct.Struct("<hh"),
    "SEGS": struct.Struct("<6H"),
    "SSECTORS": struct.Struct("<2H"),
    "NODES": struct.Struct("<12h2H"),
    "SECTORS": struct.Struct("<hh8s8shHH"),
}
MAP_LUMPS = tuple(RECORDS) + ("REJECT", "BLOCKMAP")
MONSTERS = {7, 9, 16, 58, 64, 65, 66, 67, 68, 69, 71, 84,
            3001, 3002, 3003, 3004, 3005, 3006}
NATIVE_MONSTERS = {9, 3001, 3002, 3004}
# Specials present in E1M1. This is a baseline inventory, not a support list:
# even E1M1 floor/platform specials do not have complete native semantics.
E1M1_SPECIALS = {1, 2, 11, 23, 26, 62, 88, 117}
SPRITE_FAMILIES = ("POSS", "SPOS", "TROO", "SARG")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def directory(data: bytes) -> list[tuple[str, bytes]]:
    require(len(data) >= 12, "short WAD header")
    magic, count, offset = struct.unpack_from("<4sII", data)
    require(magic == b"IWAD", "expected standard IWAD")
    require(offset <= len(data) and count <= (len(data) - offset) // 16,
            "invalid WAD directory")
    result = []
    for i in range(count):
        start, size, name = struct.unpack_from("<II8s", data, offset + i * 16)
        require(start <= len(data) and size <= len(data) - start,
                f"lump {i} outside WAD")
        result.append((name.rstrip(b"\0").decode("ascii"),
                       data[start:start + size]))
    return result


def medium_single_player(thing: tuple) -> bool:
    return bool(thing[4] & 2) and not bool(thing[4] & 16)


def map_audit(name: str, lumps: dict[str, bytes]) -> dict:
    records = {}
    for kind, record in RECORDS.items():
        require(len(lumps[kind]) % record.size == 0,
                f"{name}/{kind}: not standard Doom records")
        records[kind] = list(record.iter_unpack(lumps[kind]))
    vertices, sides = records["VERTEXES"], records["SIDEDEFS"]
    sectors, lines = records["SECTORS"], records["LINEDEFS"]
    require(bool(vertices) and bool(lines) and bool(sectors), f"{name}: empty map")
    require(all(s[5] < len(sectors) for s in sides), f"{name}: invalid sector")
    blocking_vertices = set()
    specials = Counter()
    for v1, v2, flags, special, tag, front, back in lines:
        require(v1 < len(vertices) and v2 < len(vertices), f"{name}: invalid vertex")
        require(front < len(sides) and (back == 65535 or back < len(sides)),
                f"{name}: invalid side")
        if flags & 1 or back == 65535:
            blocking_vertices.update((v1, v2))
        if special:
            specials[special] += 1
    require(bool(blocking_vertices), f"{name}: no blocking lines")
    xs, ys = zip(*(vertices[i] for i in blocking_vertices))
    width = max(xs) // 32 - (min(xs) // 32 - 1) + 2
    height = max(ys) // 32 - (min(ys) // 32 - 1) + 2
    things = records["THINGS"]
    active = [t for t in things if medium_single_player(t)]
    enemies = [t for t in active if t[3] in MONSTERS]
    types = Counter(t[3] for t in active)
    starts = [(t[0], t[1], t[2]) for t in things if t[3] == 1]
    sector_types = Counter(s[5] for s in sectors if s[5])
    # Conservative trial allocation, not an ABI measurement or admission test.
    # 32 B/enemy, 8 B/sector, pickup and visited-line bitsets, 64*16 B missiles.
    estimate = (32 * len(enemies) + 8 * len(sectors) +
                (len(things) + 7) // 8 + (len(lines) + 7) // 8 + 64 * 16)
    return {
        "map": name,
        "record_counts": {k: len(v) for k, v in records.items()},
        "raw_map_bytes": sum(len(lumps[k]) for k in MAP_LUMPS),
        "medium_sp_monsters": len(enemies),
        "medium_sp_native_monsters": sum(t[3] in NATIVE_MONSTERS for t in active),
        "new_monster_types": sorted(set(t[3] for t in enemies) - NATIVE_MONSTERS),
        "active_thing_types": dict(sorted(types.items())),
        "grid_32unit_dimensions": [width, height],
        "grid_u8_payload_bytes": width * height,
        "grid_1bit_payload_bytes_proposed": (width * height + 7) // 8,
        "current_grid_u8_dimensions_fit": width <= 255 and height <= 255,
        "current_sector_u8_sentinel_fits": len(sectors) < 255,
        "line_special_counts": dict(sorted(specials.items())),
        "specials_beyond_e1m1": sorted(set(specials) - E1M1_SPECIALS),
        "sector_special_counts": dict(sorted(sector_types.items())),
        "player1_starts": starts,
        "key_types": sorted(set(types) & {5, 6, 13, 38, 39, 40}),
        "floor_height_range": [min(s[0] for s in sectors), max(s[0] for s in sectors)],
        "mutable_state_bytes_proposed": estimate,
    }


def scaled_pixels(width: int, height: int, pixels: list) -> tuple[int, int, list]:
    scale = min(1.0, 32 / width, 48 / height)
    w, h = max(1, round(width * scale)), max(1, round(height * scale))
    return w, h, [pixels[min(height - 1, y * height // h) * width +
                          min(width - 1, x * width // w)]
                  for y in range(h) for x in range(w)]


def post_payload_bytes(width: int, height: int, pixels: list) -> int:
    """Proposed transparent column runs: u16 offsets, u8 top/len, 4-bit pixels.

    Includes column offsets and one terminator per column. Excludes the frame
    descriptor, as does the rectangle estimate; no RLE of opaque colors.
    """
    size = 2 * width
    for x in range(width):
        size += 1
        y = 0
        while y < height:
            if pixels[y * width + x] is None:
                y += 1
                continue
            start = y
            while y < height and pixels[y * width + x] is not None:
                y += 1
            size += 2 + (y - start + 1) // 2
    return size


def sprite_audit(lumps: dict[str, bytes]) -> list[dict]:
    palette = lumps["PLAYPAL"]
    result = []
    for prefix in SPRITE_FAMILIES:
        family = []
        for name, data in lumps.items():
            if not name.startswith(prefix) or len(name) not in (6, 8):
                continue
            pairs = [(name[i], name[i + 1]) for i in range(4, len(name), 2)]
            if not all(frame.isalpha() and rotation in "012345678" for frame, rotation in pairs):
                continue
            width, height, pixels = decode_patch(data)
            w, h, small = scaled_pixels(width, height, pixels)
            front = any(rotation in ("0", "1") for _, rotation in pairs)
            family.append({
                "name": name, "width": width, "height": height, "front_or_unrotated": front,
                "full_4bit_payload_bytes": len(pack_pixels(pixels, palette)),
                "capped_dimensions": [w, h],
                "capped_4bit_payload_bytes": len(pack_pixels(small, palette)),
                "capped_4bit_column_posts_bytes_proposed": post_payload_bytes(w, h, small),
            })
        require(bool(family), f"missing sprite family {prefix}")
        front_frames = [f for f in family if f["front_or_unrotated"]]
        metrics = ("full_4bit_payload_bytes", "capped_4bit_payload_bytes",
                   "capped_4bit_column_posts_bytes_proposed")
        result.append({
            "family": prefix,
            "all_rotation_patches": len(family),
            "front_or_unrotated_patches": len(front_frames),
            "all_rotation_totals": {k: sum(f[k] for f in family) for k in metrics},
            "front_or_unrotated_totals": {k: sum(f[k] for f in front_frames) for k in metrics},
            "frames": family,
        })
    return result


def audit(data: bytes) -> dict:
    entries = directory(data)
    maps = []
    for index, (name, _) in enumerate(entries):
        if not re.fullmatch(r"E[1-9]M[1-9]|MAP\d\d", name):
            continue
        selected = entries[index + 1:index + 1 + len(MAP_LUMPS)]
        require(tuple(n for n, _ in selected) == MAP_LUMPS, f"{name}: unexpected map lump order")
        maps.append(map_audit(name, dict(selected)))
    require(bool(maps), "no standard maps found")
    sprites = sprite_audit(dict(entries))
    return {
        "schema": 1,
        "source_sha256": hashlib.sha256(data).hexdigest(),
        "source_bytes": len(data),
        "scope": "Standard Doom IWAD; not a converted GBADoom/MCUDoom WAD",
        "limits": [
            "Presence/counts do not prove native playability, reachability or speed.",
            "Grid dimensions match the current blocking-line bounds formula; no grid conversion is performed.",
            "Mutable budget is a proposal: enemy*32 + sector*8 + ceil(things/8) + ceil(lines/8) + 64*16 bytes.",
            "Budget excludes OS, framebuffer, stack, navigation, pool overhead and resource caches.",
            "Sprite estimates exclude frame descriptors and metadata; assets have not been integrated.",
            "Front animation includes normal and extreme death frames if present; not every patch is required for the MVP.",
        ],
        "map_count": len(maps),
        "maps": maps,
        "sprite_families": sprites,
        "totals": {
            "raw_map_bytes": sum(m["raw_map_bytes"] for m in maps),
            "grid_u8_payload_bytes": sum(m["grid_u8_payload_bytes"] for m in maps),
            "grid_1bit_payload_bytes_proposed": sum(m["grid_1bit_payload_bytes_proposed"] for m in maps),
            "front_capped_4bit_sprite_bytes": sum(s["front_or_unrotated_totals"]["capped_4bit_payload_bytes"] for s in sprites),
            "all_rotation_capped_4bit_sprite_bytes": sum(s["all_rotation_totals"]["capped_4bit_payload_bytes"] for s in sprites),
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = audit(args.source.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{result['map_count']} maps audited; {result['source_bytes']} source bytes")
    for m in result["maps"][:9]:
        c = m["record_counts"]
        print(f"{m['map']}: {c['THINGS']} things / {m['medium_sp_monsters']} medium monsters / "
              f"{c['SECTORS']} sectors; grid {m['grid_32unit_dimensions']}; "
              f"trial mutable {m['mutable_state_bytes_proposed']} B")
    print("Sprite payloads: " + json.dumps(result["totals"]))


if __name__ == "__main__":
    main()
