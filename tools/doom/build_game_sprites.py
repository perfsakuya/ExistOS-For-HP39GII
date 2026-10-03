"""Prepare eight-direction Freedoom actor animation and pickup patches for Game.

Source patch offsets remain in map units, so death frames stay on the floor
instead of growing back to standing height. Packed 4-bit pixels are sampled
directly from read-only storage: no decoded image or animation heap is needed.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct

from build_lite_sprites import decode_patch, pack_pixels, read_lumps, require

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT.parent / "Freedoom-research/extracted/freedoom-0.13.0/freedoom1.wad"
OUTPUT = ROOT / "System/applications/user/doom_lite/GameSprites.h"
SOURCE_SHA256 = "7323bcc168c5a45ff10749b339960e98314740a734c30d4b9f3337001f9e703d"
# walking, attack, pain, death; frame numbers are relative to these sequences.
FAMILIES = (
    (3004, "POSS", ("A1 B1 C1 D1", "E1 F1 E1", "G1", "H0 I0 J0 K0 L0")),
    (9, "SPOS", ("A1 B1 C1 D1", "E1 F1 E1", "G1", "H0 I0 J0 K0 L0")),
    (3001, "TROO", ("A1 B1 C1 D1", "E1 F1 G1", "H1", "I0 J0 K0 L0 M0")),
    (3002, "SARG", ("A1 B1 C1 D1", "E1 F1 G1", "H1", "I0 J0 K0 L0 M0 N0")),
)
KEYS = ((5, "BKEYA0"), (6, "YKEYA0"), (13, "RKEYA0"))
PICKUPS = ((2007, "CLIPA0"), (2048, "AMMOA0"),
           (2008, "SHELA0"), (2049, "SBOXA0"),
           (2011, "STIMA0"), (2012, "MEDIA0"),
           (2014, "BON1A0"), (2015, "BON2A0"), (2018, "ARM1A0"),
           (2013, "SOULA0"), (2019, "ARM2A0"), (8, "BPAKA0"),
           (2022, "PINVA0"), (2023, "PSTRA0"), (2025, "SUITA0"))
# Black edges and midtone detail remain distinct from the light masonry.
GRAY = (0, 12, 28, 44, 60, 78, 96, 114, 132, 150, 168, 185, 200, 214, 229, 244)
ROTATIONS = 8
MAX_FRAMES = 7
FLIP = 0x8000
INDEX_MASK = 0x7fff
PIXEL_BUDGET = 160 * 1024


def rotation_lookup(lumps: dict[str, bytes], prefix: str,
                    frame: str) -> tuple[tuple[str, bool], ...]:
    """Resolve Doom rotations 1..8; the second pair in a patch name is flipped.

    A rotation-zero patch supplies all directions. Mixing it with explicit
    rotations or accepting two patches for one rotation would make the bank
    depend on WAD directory order, so both are rejected.
    """
    rotations = {}
    for name in sorted(lumps):
        if not name.startswith(prefix):
            continue
        require(len(name) in (6, 8), f"invalid sprite lump name {name}")
        for offset in range(4, len(name), 2):
            letter, digit = name[offset:offset + 2]
            require("A" <= letter <= "Z" and "0" <= digit <= "8",
                    f"invalid sprite frame/rotation {name}")
            if letter != frame:
                continue
            rotation = int(digit)
            require(rotation != 0 or len(name) == 6,
                    f"rotation zero cannot share a patch name: {name}")
            directions = range(1, ROTATIONS + 1) if rotation == 0 else (rotation,)
            for direction in directions:
                require(direction not in rotations,
                        f"duplicate {prefix}{frame} rotation {direction}")
                rotations[direction] = (name, offset == 6)
    require(set(rotations) == set(range(1, ROTATIONS + 1)),
            f"incomplete {prefix}{frame} rotations")
    return tuple(rotations[direction] for direction in range(1, ROTATIONS + 1))


def build_mappings(lumps: dict[str, bytes]) -> tuple[list[str], list[tuple]]:
    """Keep the former front-facing/static indices, then append other views."""
    names = []
    for _, prefix, states in FAMILIES:
        for state in states:
            for suffix in state.split():
                name = prefix + suffix
                require(name in lumps, f"missing sprite patch {name}")
                if name not in names:
                    names.append(name)
    for _, name in KEYS + PICKUPS:
        require(name in lumps, f"missing pickup patch {name}")
        names.append(name)
    mappings = []
    for thing_type, prefix, states in FAMILIES:
        rows = []
        for state in states:
            frames = []
            for suffix in state.split():
                directions = []
                for name, flip in rotation_lookup(lumps, prefix, suffix[0]):
                    if name not in names:
                        names.append(name)
                    directions.append(names.index(name) | (FLIP if flip else 0))
                frames.append(directions)
            require(len(frames) <= MAX_FRAMES, "too many animation frames")
            rows.append(frames)
        mappings.append((thing_type, rows))
    require(len(names) <= INDEX_MASK + 1, "sprite bank exceeds reference capacity")
    validate_mappings(mappings, len(names))
    return names, mappings


def validate_mappings(mappings: list[tuple], asset_count: int) -> None:
    require(len(mappings) == len(FAMILIES), "missing actor family")
    require(len({thing_type for thing_type, _ in mappings}) == len(mappings),
            "duplicate actor family")
    for thing_type, states in mappings:
        require(len(states) == 4, f"invalid state count for {thing_type}")
        for frames in states:
            require(0 < len(frames) <= MAX_FRAMES, "invalid animation frame count")
            for directions in frames:
                require(len(directions) == ROTATIONS, "invalid rotation count")
                require(all(0 <= reference <= 0xffff and
                            (reference & INDEX_MASK) < asset_count
                            for reference in directions), "invalid sprite reference")
        for directions in states[3]:
            require(len(set(directions)) == 1 and not directions[0] & FLIP,
                    "death frames must be undirected")


def resized_patch(patch: bytes, palette: bytes) -> tuple[int, int, int, int, int, int, bytes]:
    width, height, pixels = decode_patch(patch)
    _, _, left, top = struct.unpack_from("<hhhh", patch)
    numerator, denominator = min((32, width), (48, height),
                                  key=lambda pair: pair[0] / pair[1])
    if numerator >= denominator:
        out_w, out_h = width, height
    else:
        out_w = max(1, width * numerator // denominator)
        out_h = max(1, height * numerator // denominator)
    sampled = [pixels[min(height - 1, (y * height + height // 2) // out_h) * width
                      + min(width - 1, (x * width + width // 2) // out_w)]
               for y in range(out_h) for x in range(out_w)]
    return out_w, out_h, width, height, left, top, pack_pixels(sampled, palette)


def generate(data: bytes) -> tuple[str, list[tuple[str, tuple]]]:
    digest = hashlib.sha256(data).hexdigest()
    require(digest == SOURCE_SHA256, "source differs from Freedoom 0.13.0")
    lumps = read_lumps(data)
    names, mappings = build_mappings(lumps)
    assets = [(name, resized_patch(lumps[name], lumps["PLAYPAL"])) for name in names]
    total = sum(len(asset[-1]) for _, asset in assets)
    require(total <= PIXEL_BUDGET, "actor animation exceeds 160 KiB pixel budget")
    lines = ["/* Generated by tools/doom/build_game_sprites.py. Do not edit.",
             f" * Freedoom 0.13.0 SHA-256: {digest}",
             " * Attribution: doom_port/data/COPYING.txt and CREDITS.txt.",
             " * Low nibble first, zero transparent. Patch offsets in map units.",
             " */", "#pragma once", "#include <stdint.h>", "",
             f"#define GAME_SPRITE_COUNT {len(assets)}u",
             f"#define GAME_SPRITE_BYTES {total}u",
             f"#define GAME_SPRITE_ROTATIONS {ROTATIONS}u",
             "#define GAME_SPRITE_FLIP 0x8000u",
             "#define GAME_SPRITE_INDEX_MASK 0x7fffu", "",
             "typedef struct {", "    uint8_t width, height;",
             "    uint8_t world_width, world_height;", "    int16_t left, top;",
             "    const uint8_t *pixels;", "} GameSpriteAsset;", "",
             "typedef struct {", "    uint16_t thing_type;",
             "    uint8_t count[4];", "    uint16_t frame[4][7][8];",
             "} GameSpriteSequence;", "",
             "static const uint8_t game_sprite_gray[16] = {",
             "    " + ", ".join(map(str, GRAY)) + ",", "};", ""]
    for name, asset in assets:
        packed = asset[-1]
        lines.append(f"static const uint8_t game_sprite_{name.lower()}[{len(packed)}] = {{")
        for offset in range(0, len(packed), 16):
            lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in packed[offset:offset + 16]) + ",")
        lines.extend(("};", ""))
    lines.append("static const GameSpriteAsset game_sprites[GAME_SPRITE_COUNT] = {")
    for name, (width, height, world_w, world_h, left, top, _) in assets:
        lines.append(f"    {{{width}u, {height}u, {world_w}u, {world_h}u, {left}, {top}, game_sprite_{name.lower()}}},")
    lines.extend(("};", "", "static const GameSpriteSequence game_sprite_sequences[4] = {"))
    for thing_type, states in mappings:
        rows = ["{" + ", ".join("{" + ", ".join(f"0x{ref:04x}u" for ref in directions) + "}"
                                  for directions in state) + "}" for state in states]
        lines.append(f"    {{{thing_type}u, {{{', '.join(str(len(state)) + 'u' for state in states)}}}, {{{', '.join(rows)}}}}},")
    lines.extend(("};", "", "static const uint16_t game_key_types[3] = {5u, 6u, 13u};",
                  "static const uint16_t game_key_sprites[3] = {" +
                  ", ".join(f"{names.index(name)}u" for _, name in KEYS) + "};", ""))
    lines.extend(("typedef struct { uint16_t thing_type, sprite; } GameStaticSprite;",
                  f"#define GAME_STATIC_SPRITE_COUNT {len(KEYS + PICKUPS)}u",
                  "static const GameStaticSprite game_static_sprites[GAME_STATIC_SPRITE_COUNT] = {"))
    for thing_type, name in sorted(KEYS + PICKUPS):
        lines.append(f"    {{{thing_type}u, {names.index(name)}u}},")
    lines.extend(("};", ""))
    return "\n".join(lines), assets


def write_previews(directory: Path, data: bytes, assets: list[tuple]) -> None:
    from PIL import Image, ImageDraw

    directory.mkdir(parents=True, exist_ok=True)
    images = {}
    for name, asset in assets:
        width, height, *_ = asset
        packed = asset[-1]
        pixels = bytes(GRAY[(packed[i // 2] >> ((i & 1) * 4)) & 15]
                       if (packed[i // 2] >> ((i & 1) * 4)) & 15 else 237
                       for i in range(width * height))
        (directory / f"{name}.pgm").write_bytes(
            f"P5\n{width} {height}\n255\n".encode("ascii") + pixels)
        images[name] = Image.frombytes("L", (width, height), pixels)
    names, mappings = build_mappings(read_lumps(data))
    cell_w, cell_h, margin = 80, 118, 110

    def sheet(path: Path, rows: list[tuple[str, list[int]]]) -> None:
        canvas = Image.new("L", (margin + ROTATIONS * cell_w, 24 + len(rows) * cell_h), 237)
        draw = ImageDraw.Draw(canvas)
        for rotation in range(ROTATIONS):
            draw.text((margin + rotation * cell_w + 20, 6), f"R{rotation + 1}", fill=0)
        for row, (label, references) in enumerate(rows):
            top = 24 + row * cell_h
            draw.text((4, top + 30), label, fill=0)
            for rotation, reference in enumerate(references):
                name = names[reference & INDEX_MASK]
                image = images[name]
                if reference & FLIP:
                    image = image.transpose(Image.Transpose.FLIP_LEFT_RIGHT)
                image = image.resize((image.width * 2, image.height * 2), Image.Resampling.NEAREST)
                left = margin + rotation * cell_w + (cell_w - image.width) // 2
                canvas.paste(image, (left, top + 4))
                draw.text((margin + rotation * cell_w + 2, top + 103),
                          name + (" *" if reference & FLIP else ""), fill=0)
        canvas.save(path)

    sheet(directory / "directions.png",
          [(prefix + " walking A", states[0][0])
           for (_, prefix, _), (_, states) in zip(FAMILIES, mappings)])
    state_labels = ("walk", "attack", "pain", "death")
    for (_, prefix, _), (_, states) in zip(FAMILIES, mappings):
        rows = [(f"{prefix} {state_labels[state]} {frame}", references)
                for state, frames in enumerate(states)
                for frame, references in enumerate(frames)]
        sheet(directory / f"animations-{prefix}.png", rows)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    data = args.source.read_bytes()
    header, assets = generate(data)
    if args.check:
        require(args.output.read_text(encoding="ascii") == header,
                "generated actor header differs")
    else:
        args.output.write_text(header, encoding="ascii", newline="\n")
    if args.preview_dir:
        write_previews(args.preview_dir, data, assets)
    print(f"Game sprites: {len(assets)} unique patches, "
          f"{sum(len(asset[-1]) for _, asset in assets)} packed bytes; "
          f"header {'verified' if args.check else 'generated'}")


if __name__ == "__main__":
    main()
