"""Compose and quantize the verified Freedoom E1M1/E1M2 world materials.

PNAMES and TEXTURE1/2 are interpreted before resizing, including ordered
overlap, transparent patch posts and clipped patch origins. Original material
dimensions, sidedef offsets and linedef orientation remain available to the
renderer. Assets are read-only, 32x32, column-major, low nibble first; zero is
transparent and 1..15 use game_world_gray. F_SKY1 is the sector sentinel 255.
No WAD, decoded texture heap or preprocessing code is linked into firmware.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib

from audit_expansion import RECORDS, directory, require
from build_game_maps import map_lumps

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT.parent / "Freedoom-research/extracted/freedoom-0.13.0/freedoom1.wad"
OUTPUT = ROOT / "System/applications/user/doom_lite/GameWorldTextures.h"
REPORT = ROOT / "docs/doom-game-world.json"
SOURCE_SHA256 = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d"
MAP_NAMES = ("E1M1", "E1M2")
SIZE = 32
PACKED_BYTES = SIZE * SIZE // 2
GRAY = (0, 18, 34, 50, 67, 83, 99, 115, 131, 148, 164, 180, 196, 212, 228, 244)
SKY = 255
COMPOSITION_REFERENCE = "https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/r_data.c"


def wad_name(value: bytes) -> str:
    return value.rstrip(b"\0").decode("ascii").upper()


@dataclass(frozen=True)
class PatchPlacement:
    x: int
    y: int
    name: str


@dataclass(frozen=True)
class TextureDefinition:
    name: str
    width: int
    height: int
    patches: tuple[PatchPlacement, ...]


@dataclass
class Material:
    kind: str
    name: str
    width: int
    height: int
    source: list[int | None]
    sampled: list[int | None]
    pixels: bytes
    texture_id: int = 0
    pixel_id: int = 0


@dataclass
class WorldMap:
    name: str
    sides: list[tuple[int, int, int, int, int]]
    lines: list[tuple[int, int, int, int]]
    sectors: list[tuple[int, int]]


def texture_definitions(lumps: dict[str, bytes]) -> dict[str, TextureDefinition]:
    pnames = lumps["PNAMES"]
    require(len(pnames) >= 4, "short PNAMES header")
    count, = struct.unpack_from("<I", pnames)
    require(count <= (len(pnames) - 4) // 8, "short PNAMES list")
    names = [wad_name(pnames[4 + i * 8:12 + i * 8]) for i in range(count)]
    textures = {}
    for lump_name in ("TEXTURE1", "TEXTURE2"):
        if lump_name not in lumps:
            continue
        data = lumps[lump_name]
        require(len(data) >= 4, f"short {lump_name} header")
        count, = struct.unpack_from("<I", data)
        require(count <= (len(data) - 4) // 4, f"short {lump_name} directory")
        for i in range(count):
            offset, = struct.unpack_from("<I", data, 4 + i * 4)
            require(4 + count * 4 <= offset <= len(data) - 22,
                    f"invalid {lump_name} texture offset")
            raw_name, _, width, height, _, patch_count = struct.unpack_from("<8sIhhIh", data, offset)
            name = wad_name(raw_name)
            require(0 < width <= 4096 and 0 < height <= 4096,
                    f"{name}: invalid texture dimensions")
            require(0 < patch_count <= (len(data) - offset - 22) // 10,
                    f"{name}: invalid patch count")
            patches = []
            for j in range(patch_count):
                x, y, patch, _, _ = struct.unpack_from("<hhhhh", data, offset + 22 + j * 10)
                require(0 <= patch < len(names), f"{name}: invalid PNAMES index")
                require(names[patch] in lumps, f"{name}: missing patch {names[patch]}")
                patches.append(PatchPlacement(x, y, names[patch]))
            # Doom searches TEXTURE1 before TEXTURE2 when names repeat.
            textures.setdefault(name, TextureDefinition(name, width, height, tuple(patches)))
    return textures


def decode_patch(data: bytes) -> tuple[int, int, list[int | None]]:
    require(len(data) >= 8, "short world patch header")
    width, height, _, _ = struct.unpack_from("<hhhh", data)
    require(0 < width <= 4096 and 0 < height <= 4096, "invalid world patch dimensions")
    require(8 + width * 4 <= len(data), "short world patch column directory")
    pixels: list[int | None] = [None] * (width * height)
    for x in range(width):
        offset, = struct.unpack_from("<I", data, 8 + x * 4)
        require(8 + width * 4 <= offset < len(data), "invalid world patch column offset")
        previous_top = -1
        while True:
            require(offset < len(data), "unterminated world patch column")
            top = data[offset]
            if top == 255:
                break
            require(offset + 4 <= len(data), "short world patch post")
            length = data[offset + 1]
            if top <= previous_top:
                top += previous_top
            previous_top = top
            require(offset + length + 4 <= len(data), "short world patch post pixels")
            require(top + length <= height, "world patch post exceeds height")
            for y in range(length):
                pixels[(top + y) * width + x] = data[offset + 3 + y]
            offset += length + 4
    return width, height, pixels


def compose_texture(texture: TextureDefinition, lumps: dict[str, bytes],
                    cache: dict | None = None) -> list[int | None]:
    """Draw patch posts in definition order, clipping at the texture rectangle.

    Patch left/top sprite offsets are intentionally ignored: TEXTURE origins
    already place the patch in texture space, as in R_GenerateComposite.
    """
    if cache is None:
        cache = {}
    result: list[int | None] = [None] * (texture.width * texture.height)
    for placement in texture.patches:
        if placement.name not in cache:
            cache[placement.name] = decode_patch(lumps[placement.name])
        width, height, pixels = cache[placement.name]
        x0, x1 = max(0, placement.x), min(texture.width, placement.x + width)
        y0, y1 = max(0, placement.y), min(texture.height, placement.y + height)
        for y in range(y0, y1):
            for x in range(x0, x1):
                value = pixels[(y - placement.y) * width + x - placement.x]
                if value is not None:
                    result[y * texture.width + x] = value
    return result


def flat_lumps(entries: list[tuple[str, bytes]]) -> dict[str, bytes]:
    result, active = {}, False
    for name, data in entries:
        if name in ("F_START", "FF_START"):
            active = True
        elif name in ("F_END", "FF_END"):
            active = False
        elif active and len(data) == 4096:
            result[name] = data
    return result


def palette_luma(palette: bytes) -> list[int]:
    require(len(palette) >= 768, "short PLAYPAL")
    return [(77 * palette[3 * i] + 150 * palette[3 * i + 1] +
             29 * palette[3 * i + 2] + 128) // 256 for i in range(256)]


def sample_material(pixels: list[int | None], width: int, height: int,
                    luma: list[int]) -> list[int | None]:
    """Exact area resample keeps subpixel trim edges from aliasing away.

    At least half of each output pixel's area must be opaque. Transparent
    source posts do not darken the color of the remaining opaque coverage.
    """
    require(len(pixels) == width * height, "material pixel length differs")
    sampled = []
    area = width * height
    for y in range(SIZE):
        for x in range(SIZE):
            weight, total = 0, 0
            for sy in range(y * height // SIZE, ((y + 1) * height + SIZE - 1) // SIZE):
                yw = min((sy + 1) * SIZE, (y + 1) * height) - max(sy * SIZE, y * height)
                for sx in range(x * width // SIZE, ((x + 1) * width + SIZE - 1) // SIZE):
                    value = pixels[sy * width + sx]
                    if value is not None:
                        xw = min((sx + 1) * SIZE, (x + 1) * width) - max(sx * SIZE, x * width)
                        coverage = yw * xw
                        weight += coverage
                        total += luma[value] * coverage
            sampled.append((total + weight // 2) // weight if weight * 2 >= area else None)
    return sampled


def enhance_gray(sampled: list[int | None]) -> list[int]:
    """Brighten dark stone/metal, then add mild local detail before 4-bit output.

    One fixed tone curve is used for the whole bank, preserving relative
    material brightness. A wrapped four-neighbor unsharp term suits repeating
    tiles; transparent neighbors are excluded from sharpening.
    """
    require(len(sampled) == SIZE * SIZE, "expected 32x32 sampled texture")
    tone = [max(18, min(244, round(128 + (18 + 226 * (i / 255) ** 0.75 - 128) * 1.17)))
            for i in range(256)]
    result = []
    for y in range(SIZE):
        for x in range(SIZE):
            value = sampled[y * SIZE + x]
            if value is None:
                result.append(0)
                continue
            neighbors = [sampled[((y + dy) % SIZE) * SIZE + (x + dx) % SIZE]
                         for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1))]
            neighbors = [tone[n] for n in neighbors if n is not None]
            level = tone[value]
            if neighbors:
                level += round((level - sum(neighbors) / len(neighbors)) * 0.45)
            level = max(18, min(244, level))
            result.append(min(range(1, 16), key=lambda code: abs(GRAY[code] - level)))
    return result


def pack_columns(codes: list[int]) -> bytes:
    require(len(codes) == SIZE * SIZE and all(0 <= c < 16 for c in codes),
            "invalid world texture nibble")
    output = bytearray(PACKED_BYTES)
    for x in range(SIZE):
        for y in range(SIZE):
            i = x * SIZE + y
            output[i // 2] |= codes[y * SIZE + x] << ((i & 1) * 4)
    return bytes(output)


def unpack_columns(packed: bytes) -> list[int]:
    require(len(packed) == PACKED_BYTES, "invalid packed world texture length")
    return [(packed[(x * SIZE + y) // 2] >> (((x * SIZE + y) & 1) * 4)) & 15
            for y in range(SIZE) for x in range(SIZE)]


def unit_tangent(dx: int, dy: int) -> tuple[int, int]:
    require(dx != 0 or dy != 0, "zero-length linedef")
    length = math.hypot(dx, dy)
    return round(dx * 16384 / length), round(dy * 16384 / length)


def emit_header(materials: list[Material], unique: list[Material], banks: dict[str, list[Material]],
                maps: list[WorldMap]) -> str:
    lines = ["/* Generated by tools/doom/build_game_world.py. Do not edit.",
             f" * Freedoom 0.13.0 IWAD SHA-256: {SOURCE_SHA256}",
             " * Attribution: doom_port/data/COPYING.txt and CREDITS.txt.",
             " * 32x32 column-major 4-bit pixels; low nibble first, zero transparent.",
             " * Material dimensions and WAD record order remain unchanged.",
             " */", "#pragma once", "#include <stdint.h>", "",
             f"#define GAME_WORLD_TEXTURE_COUNT {len(banks['wall'])}u",
             f"#define GAME_WORLD_FLAT_COUNT {len(banks['flat'])}u",
             "#define GAME_WORLD_TEXTURE_SIZE 32u",
             "#define GAME_WORLD_TEXTURE_BYTES 512u",
             f"#define GAME_WORLD_PIXEL_BYTES {len(unique) * PACKED_BYTES}u",
             "#define GAME_WORLD_SKY 255u", "#define GAME_WORLD_NO_SIDE UINT16_MAX", "",
             "typedef struct {", "    uint16_t world_width, world_height;",
             "    const uint8_t *pixels;", "} GameWorldTexture;", "",
             "typedef struct {", "    int16_t x_offset, y_offset;",
             "    uint8_t upper, middle, lower;", "} GameWorldSide;", "",
             "typedef struct {", "    uint16_t front_side, back_side;",
             "    int16_t tangent_x, tangent_y; /* Q14, original v1 -> v2. */",
             "} GameWorldLine;", "",
             "/* Sector IDs index game_world_flats; wall IDs index game_world_textures. */",
             "typedef struct { uint8_t floor, ceiling; } GameWorldSector;", "",
             "typedef struct {", "    uint16_t side_count, line_count, sector_count;",
             "    const GameWorldSide *sides;", "    const GameWorldLine *lines;",
             "    const GameWorldSector *sectors;", "} GameWorldMap;", "",
             "static const uint8_t game_world_gray[16] = {",
             "    " + ", ".join(str(v) for v in GRAY) + ",", "};", ""]
    for material in unique:
        names = [f"{m.kind}:{m.name}" for m in materials if m.pixel_id == material.pixel_id]
        lines.append("/* " + ", ".join(names) + " */")
        lines.append(f"static const uint8_t game_world_pixels_{material.pixel_id:03d}[512] = {{")
        for offset in range(0, PACKED_BYTES, 16):
            lines.append("    " + ", ".join(f"0x{v:02x}" for v in material.pixels[offset:offset + 16]) + ",")
        lines.extend(("};", ""))
    for kind, symbol, macro in (("wall", "textures", "TEXTURE"), ("flat", "flats", "FLAT")):
        lines.extend((f"static const GameWorldTexture game_world_{symbol}[GAME_WORLD_{macro}_COUNT + 1u] = {{",
                      "    {0u, 0u, 0}, /* 0: no texture */"))
        for m in banks[kind]:
            lines.append(f"    {{{m.width}u, {m.height}u, game_world_pixels_{m.pixel_id:03d}}},")
        lines.extend(("};", ""))
    for world in maps:
        prefix = f"game_world_{world.name.lower()}"
        for kind, ctype, rows in (("sides", "GameWorldSide", world.sides),
                                  ("lines", "GameWorldLine", world.lines),
                                  ("sectors", "GameWorldSector", world.sectors)):
            lines.append(f"static const {ctype} {prefix}_{kind}[{len(rows)}] = {{")
            lines.extend("    {" + ", ".join(str(v) for v in row) + "}," for row in rows)
            lines.extend(("};", ""))
    lines.append(f"static const GameWorldMap game_world_maps[{len(maps)}] = {{")
    for world in maps:
        prefix = f"game_world_{world.name.lower()}"
        lines.append(f"    {{{len(world.sides)}u, {len(world.lines)}u, {len(world.sectors)}u, "
                     f"{prefix}_sides, {prefix}_lines, {prefix}_sectors}},")
    lines.extend(("};", ""))
    return "\n".join(lines)


def generate(data: bytes) -> tuple[str, dict, list[Material], list[WorldMap]]:
    require(hashlib.sha256(data).hexdigest() == SOURCE_SHA256,
            "source differs from verified Freedoom 0.13.0 IWAD")
    entries = directory(data)
    lumps = dict(entries)
    definitions = texture_definitions(lumps)
    flats = flat_lumps(entries)
    raw_maps = {name: map_lumps(entries, name) for name in MAP_NAMES}
    wall_names, flat_names = set(), set()
    for raw in raw_maps.values():
        for side in RECORDS["SIDEDEFS"].iter_unpack(raw["SIDEDEFS"]):
            wall_names.update(wad_name(side[i]) for i in (2, 3, 4))
        for sector in RECORDS["SECTORS"].iter_unpack(raw["SECTORS"]):
            flat_names.update(wad_name(sector[i]) for i in (2, 3))
    wall_names.discard("-")
    flat_names.discard("F_SKY1")
    luma = palette_luma(lumps["PLAYPAL"])
    materials, unique, ids, cache = [], [], {}, {}
    banks = {"wall": [], "flat": []}
    bank_ids = {"wall": {}, "flat": {}}
    for kind, names in (("wall", wall_names), ("flat", flat_names)):
        for name in sorted(names):
            if kind == "wall":
                require(name in definitions, f"missing TEXTURE definition {name}")
                texture = definitions[name]
                width, height = texture.width, texture.height
                pixels = compose_texture(texture, lumps, cache)
            else:
                require(name in flats, f"missing flat {name} in F_START/F_END namespace")
                width = height = 64
                pixels = list(flats[name])
            sampled = sample_material(pixels, width, height, luma)
            packed = pack_columns(enhance_gray(sampled))
            material = Material(kind, name, width, height, pixels, sampled, packed)
            # Merge only identical output pixels AND original world dimensions.
            key = width, height, packed
            if key not in ids:
                ids[key] = len(unique) + 1
                material.pixel_id = ids[key]
                unique.append(material)
            material.pixel_id = ids[key]
            if key not in bank_ids[kind]:
                bank_ids[kind][key] = len(banks[kind]) + 1
                material.texture_id = bank_ids[kind][key]
                banks[kind].append(material)
            material.texture_id = bank_ids[kind][key]
            materials.append(material)
    require(all(len(bank) <= 254 for bank in banks.values()),
            "world material bank exceeds uint8 capacity with sky sentinel")
    lookup = {(m.kind, m.name): m.texture_id for m in materials}
    lookup[("wall", "-")] = 0
    lookup[("flat", "F_SKY1")] = SKY
    worlds = []
    for name, raw in raw_maps.items():
        sides = [(s[0], s[1], lookup[("wall", wad_name(s[2]))],
                  lookup[("wall", wad_name(s[4]))], lookup[("wall", wad_name(s[3]))])
                 for s in RECORDS["SIDEDEFS"].iter_unpack(raw["SIDEDEFS"])]
        vertices = list(RECORDS["VERTEXES"].iter_unpack(raw["VERTEXES"]))
        lines = []
        for v1, v2, _, _, _, front, back in RECORDS["LINEDEFS"].iter_unpack(raw["LINEDEFS"]):
            require(v1 < len(vertices) and v2 < len(vertices), "invalid world linedef vertex")
            require(front < len(sides) and (back == 65535 or back < len(sides)), "invalid world linedef side")
            x1, y1 = vertices[v1]
            x2, y2 = vertices[v2]
            lines.append((front, back, *unit_tangent(x2 - x1, y2 - y1)))
        sectors = [(lookup[("flat", wad_name(s[2]))], lookup[("flat", wad_name(s[3]))])
                   for s in RECORDS["SECTORS"].iter_unpack(raw["SECTORS"])]
        worlds.append(WorldMap(name, sides, lines, sectors))
    header = emit_header(materials, unique, banks, worlds)
    # Exact ARM EABI sizes: Texture=8, Side=8, Line=8, Sector=2, Map=20.
    sizes = {"pixels": len(unique) * PACKED_BYTES, "gray_lut": len(GRAY),
             "texture_descriptors": (sum(len(bank) for bank in banks.values()) + 2) * 8,
             "sidedefs": sum(len(w.sides) for w in worlds) * 8,
             "linedefs": sum(len(w.lines) for w in worlds) * 8,
             "sectors": sum(len(w.sectors) for w in worlds) * 2,
             "map_descriptors": len(worlds) * 20}
    report = {
        "source": {"release": "Freedoom 0.13.0", "wad": "freedoom1.wad",
                   "sha256": SOURCE_SHA256, "bytes": len(data)},
        "composition_reference": COMPOSITION_REFERENCE,
        "composition": "PNAMES, TEXTURE1 then TEXTURE2; ordered opaque patch posts; clipped origins; sprite offsets ignored",
        "packing": {"size": [SIZE, SIZE], "bits_per_pixel": 4, "column_major": True,
                    "low_nibble_first": True, "transparent_code": 0, "texture_id_none": 0,
                    "sector_sky_id": SKY, "gray_lut": list(GRAY)},
        "gray_processing": {"luminance": "rounded Rec.601 integer 77R+150G+29B / 256",
                            "resampling": "area average, opaque if coverage >= half",
                            "gamma": 0.75, "tone_range": [18, 244],
                            "global_contrast": 1.17, "wrapped_local_detail_gain": 0.45},
        "raw_material_names": len(materials), "wall_names": len(wall_names),
        "flat_names": len(flat_names), "unique_materials": len(unique),
        "wall_bank_count": len(banks["wall"]), "flat_bank_count": len(banks["flat"]),
        "exact_aliases_removed": len(materials) - len(unique),
        "read_only_arm_bytes": sizes, "read_only_arm_total_bytes": sum(sizes.values()),
        "read_only_size_note": "32-bit ARM EABI object payload sum; excludes compiler/linker alignment padding",
        "mutable_asset_bytes": 0,
        "header_sha256": hashlib.sha256(header.encode("ascii")).hexdigest(),
        "maps": [{"name": w.name, "sidedefs": len(w.sides), "linedefs": len(w.lines),
                  "sectors": len(w.sectors), "sky_planes": sum(p == SKY for s in w.sectors for p in s)} for w in worlds],
        "materials": []}
    for m in materials:
        codes = unpack_columns(m.pixels)
        visible = [GRAY[c] for c in codes if c]
        original = [v for v in m.sampled if v is not None]
        report["materials"].append({"kind": m.kind, "name": m.name, "id": m.texture_id,
            "pixel_id": m.pixel_id,
            "world_dimensions": [m.width, m.height], "packed_bytes": len(m.pixels),
            "packed_sha256": hashlib.sha256(m.pixels).hexdigest(),
            "patches": [{"name": p.name, "x": p.x, "y": p.y} for p in definitions[m.name].patches] if m.kind == "wall" else [],
            "source_transparent_pixels": sum(p is None for p in m.source),
            "output_transparent_pixels": codes.count(0), "output_gray_levels": len(set(codes) - {0}),
            "mean_source_luma": round(sum(original) / len(original), 3) if original else None,
            "mean_output_luma": round(sum(visible) / len(visible), 3) if visible else None})
    return header, report, materials, worlds


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    require(len(rgb) == width * height * 3, "PNG pixel length differs")
    def chunk(kind: bytes, content: bytes) -> bytes:
        return struct.pack(">I", len(content)) + kind + content + struct.pack(">I", zlib.crc32(kind + content))
    scanlines = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))


def write_previews(path: Path, materials: list[Material], palette: bytes) -> None:
    """Small PNGs and labeled contact sheets, with a JSON placement index.

    Each row shows color source, source luma at 32x32, and enhanced packed
    result. Nearest magnification exposes precisely the runtime pixel grid.
    """
    path.mkdir(parents=True, exist_ok=True)
    cell_w, cell_h, columns = 300, 108, 4
    width, height = columns * cell_w, ((len(materials) + columns - 1) // columns) * cell_h
    sheet = bytearray([237] * (width * height * 3))
    index = []
    glyphs = {
        "0": "0E 11 13 15 19 11 0E", "1": "04 0C 04 04 04 04 0E",
        "2": "0E 11 01 02 04 08 1F", "3": "1E 01 01 0E 01 01 1E",
        "4": "02 06 0A 12 1F 02 02", "5": "1F 10 10 1E 01 01 1E",
        "6": "0E 10 10 1E 11 11 0E", "7": "1F 01 02 04 08 08 08",
        "8": "0E 11 11 0E 11 11 0E", "9": "0E 11 11 0F 01 01 0E",
        "A": "0E 11 11 1F 11 11 11", "B": "1E 11 11 1E 11 11 1E",
        "C": "0E 11 10 10 10 11 0E", "D": "1E 11 11 11 11 11 1E",
        "E": "1F 10 10 1E 10 10 1F", "F": "1F 10 10 1E 10 10 10",
        "G": "0E 11 10 17 11 11 0E", "H": "11 11 11 1F 11 11 11",
        "I": "0E 04 04 04 04 04 0E", "J": "07 02 02 02 12 12 0C",
        "K": "11 12 14 18 14 12 11", "L": "10 10 10 10 10 10 1F",
        "M": "11 1B 15 15 11 11 11", "N": "11 19 15 13 11 11 11",
        "O": "0E 11 11 11 11 11 0E", "P": "1E 11 11 1E 10 10 10",
        "Q": "0E 11 11 11 15 12 0D", "R": "1E 11 11 1E 14 12 11",
        "S": "0F 10 10 0E 01 01 1E", "T": "1F 04 04 04 04 04 04",
        "U": "11 11 11 11 11 11 0E", "V": "11 11 11 11 11 0A 04",
        "W": "11 11 11 15 15 1B 11", "X": "11 11 0A 04 0A 11 11",
        "Y": "11 11 0A 04 04 04 04", "Z": "1F 01 02 04 08 10 1F",
        "-": "00 00 00 1F 00 00 00", "_": "00 00 00 00 00 00 1F",
        ":": "00 04 04 00 04 04 00", "/": "01 01 02 04 08 10 10",
        " ": "00 00 00 00 00 00 00"}
    for number, m in enumerate(materials):
        codes = unpack_columns(m.pixels)
        output_rgb = bytearray()
        for y in range(SIZE):
            for x in range(SIZE):
                code = codes[y * SIZE + x]
                v = GRAY[code] if code else (237 if (x // 4 + y // 4) & 1 else 205)
                output_rgb.extend((v, v, v))
        write_png(path / f"{number + 1:03d}-{m.kind}-{m.name}.png", SIZE, SIZE, output_rgb)
        for panel in range(3):
            for y in range(96):
                for x in range(96):
                    sx, sy = x // 3, y // 3
                    if panel == 0:
                        source = m.source[min(m.height - 1, sy * m.height // SIZE) * m.width + min(m.width - 1, sx * m.width // SIZE)]
                        color = palette[source * 3:source * 3 + 3] if source is not None else bytes((205, 205, 205))
                    else:
                        v = m.sampled[sy * SIZE + sx] if panel == 1 else (GRAY[codes[sy * SIZE + sx]] if codes[sy * SIZE + sx] else None)
                        if v is None:
                            v = 237 if (sx // 4 + sy // 4) & 1 else 205
                        color = bytes((v, v, v))
                    ox = (number % columns) * cell_w + panel * 100 + x
                    oy = (number // columns) * cell_h + y
                    offset = (oy * width + ox) * 3
                    sheet[offset:offset + 3] = color
        label = f"{number + 1:03d} {m.kind.upper()} {m.name} ID {m.texture_id:03d} {m.width}X{m.height}"
        for letter, character in enumerate(label):
            for y, row in enumerate(glyphs.get(character, glyphs[" "]).split()):
                for x in range(5):
                    if int(row, 16) & (1 << (4 - x)):
                        ox = (number % columns) * cell_w + letter * 6 + x
                        oy = (number // columns) * cell_h + 99 + y
                        offset = (oy * width + ox) * 3
                        sheet[offset:offset + 3] = b"\x20\x20\x20"
        index.append({"contact_cell": number + 1, "row": number // columns + 1,
                      "column": number % columns + 1, "kind": m.kind, "name": m.name,
                      "id": m.texture_id, "panels": ["source color", "area sampled luma", "enhanced 4-bit"]})
    write_png(path / "contact-sheet.png", width, height, sheet)
    page_height = 12 * cell_h
    for first_y in range(0, height, page_height):
        page = first_y // page_height + 1
        current_height = min(page_height, height - first_y)
        write_png(path / f"contact-sheet-{page:02d}.png", width, current_height,
                  sheet[first_y * width * 3:(first_y + current_height) * width * 3])
    (path / "contact-sheet-index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="ascii", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--report", type=Path, default=REPORT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    data = args.source.read_bytes()
    header, report, materials, _ = generate(data)
    report_text = json.dumps(report, indent=2) + "\n"
    if args.check:
        require(args.output.read_text(encoding="ascii") == header, "generated world header differs")
        require(args.report.read_text(encoding="ascii") == report_text, "generated world audit differs")
    else:
        args.output.write_text(header, encoding="ascii", newline="\n")
        args.report.write_text(report_text, encoding="ascii", newline="\n")
    if args.preview_dir:
        write_previews(args.preview_dir, materials, dict(directory(data))["PLAYPAL"])
    print(f"Game world: {len(materials)} material names, {report['unique_materials']} exact unique assets; "
          f"{report['read_only_arm_total_bytes']} read-only ARM bytes; "
          f"header/audit {'verified' if args.check else 'generated'}")


if __name__ == "__main__":
    main()
