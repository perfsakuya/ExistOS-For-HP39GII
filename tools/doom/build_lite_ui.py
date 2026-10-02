"""Pre-size Freedoom status-bar and pistol patches for the 256x127 LCD.

All pixels are flash-resident nibbles: zero transparent, 1..15 grayscale.
The status bar is lightened for LCD readability. Weapon offsets come from
the patch headers and Doom's 32-unit ready-weapon position, cropped to the
320x168 view before the 256x101 conversion. No runtime scaling is needed.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct

from build_lite_sprites import SOURCE, SOURCE_SHA256, ROOT, GRAY, read_lumps, decode_patch, require

OUTPUT = ROOT / "System/applications/user/doom_lite/E1M1Ui.h"
VIEW_H = 101
ASSET_NAMES = ("STBAR",) + tuple(f"STTNUM{i}" for i in range(10)) + (
    "STTPRCNT",) + tuple(f"STYSNUM{i}" for i in range(10)) + tuple(
    f"STFST{i}0" for i in range(5)) + ("STFDEAD0", "STKEYS0",
    "PISGA0", "PISGB0", "PISGC0", "PISFA0")


def convert(name: str, patch: bytes, palette: bytes):
    w, h, pixels = decode_patch(patch, max_width=320)
    x = y = 0
    if name == "STBAR":
        dw, dh = 256, 26
    elif name.startswith("STT"):
        dw, dh = 10, 13
    elif name.startswith("STF"):
        dw, dh = 19, (h * 26 + 16) // 32
    elif name == "STKEYS0":
        dw, dh = 10, 8
    elif name.startswith("PIS"):
        left, top = struct.unpack_from("<hh", patch, 4)
        x = (-left * 256 + 160) // 320
        y = ((32 - top) * VIEW_H + 84) // 168
        dw, dh = (w * 256 + 160) // 320, (h * VIEW_H + 84) // 168
    else:
        dw, dh = w, h
    full_h = dh
    if name.startswith("PIS"):
        dh = min(dh, VIEW_H - y)
    require(dw > 0 and dh > 0, f"empty converted asset {name}")
    packed = bytearray((dw * dh + 1) // 2)
    for dy in range(dh):
        sy = min(h - 1, dy * h // full_h)
        for dx in range(dw):
            index = pixels[sy * w + min(w - 1, dx * w // dw)]
            if index is None:
                continue
            r, g, b = palette[index * 3:index * 3 + 3]
            gray = (77 * r + 150 * g + 29 * b) >> 8
            if name == "STBAR":
                # The original embossed labels lose contrast after scaling.
                # Preserve their glyphs, but isolate the bottom lettering band.
                gray = 237 if dy >= 19 else max(48, 244 - gray)
            elif name.startswith("STT") or name.startswith("STYS"):
                # Dark ink on light counters, with the original black shadow
                # and digit holes made light instead of solid black blobs.
                gray = 237 if gray < 25 else max(18, 237 - gray * 3)
            code = 1 + (gray * 14 + 127) // 255
            i = dy * dw + dx
            packed[i // 2] |= code << ((i & 1) * 4)
    return name, dw, dh, x, y, bytes(packed)


def status_labels(asset, lumps, palette):
    """Rebuild faint embossed labels with Freedoom's seven-row menu font.

    The result stays inside the packed background; no extra rendering or
    font storage is needed on the device. Remove the shadow border before
    sizing the letters so their holes survive on the small LCD.
    """
    name, w, h, x, y, raw = asset
    packed = bytearray(raw)
    for word, left, right in (("AMMO", 1, 35), ("HEALTH", 38, 84),
                              ("KILLS", 86, 113), ("ARMOR", 138, 189)):
        glyphs = [decode_patch(lumps[f"STCFN{ord(c):03d}"]) for c in word]
        cropped = []
        for gw, gh, pixels in glyphs:
            require(gh == 7, "unexpected label font height")
            mask = [False] * (gw * gh)
            for yy in range(7):
                for xx in range(gw):
                    index = pixels[yy * gw + xx]
                    if index is not None:
                        r, g, b = palette[index * 3:index * 3 + 3]
                        mask[yy * gw + xx] = ((77 * r + 150 * g + 29 * b) >> 8) >= 32
            occupied = [i for i, v in enumerate(mask) if v]
            require(bool(occupied), "empty label glyph")
            x0, x1 = min(i % gw for i in occupied), max(i % gw for i in occupied) + 1
            y0, y1 = min(i // gw for i in occupied), max(i // gw for i in occupied) + 1
            cropped.append((x1 - x0, y1 - y0,
                [mask[yy * gw + xx] for yy in range(y0, y1) for xx in range(x0, x1)]))
        sw = sum(g[0] for g in cropped) + len(cropped) - 1
        sh = max(g[1] for g in cropped)
        mask = [False] * (sw * sh)
        gx = 0
        for gw, gh, pixels in cropped:
            for yy in range(gh):
                for xx in range(gw): mask[(yy + sh - gh) * sw + gx + xx] = pixels[yy * gw + xx]
            gx += gw + 1
        dw = min(right - left - 1, sw)
        dx0 = (left + right - dw) // 2
        for yy in range(7):
            for xx in range(dw):
                code = 2 if mask[(yy * sh // 7) * sw + xx * sw // dw] else 14
                i = (19 + yy) * w + dx0 + xx
                shift = (i & 1) * 4
                packed[i // 2] = (packed[i // 2] & ~(15 << shift)) | (code << shift)
    return name, w, h, x, y, bytes(packed)


def generate(data: bytes):
    require(hashlib.sha256(data).hexdigest() == SOURCE_SHA256,
            "source differs from the verified Freedoom WAD")
    lumps = read_lumps(data)
    assets = [convert(name, lumps[name], lumps["PLAYPAL"]) for name in ASSET_NAMES]
    assets[0] = status_labels(assets[0], lumps, lumps["PLAYPAL"])
    total = sum(len(a[5]) for a in assets)
    require(total <= 16384, "UI assets exceed the 16 KiB flash budget")
    lines = ["/* Generated by tools/doom/build_lite_ui.py. Do not edit.",
             f" * Freedoom WAD SHA-256: {SOURCE_SHA256}",
             " * Attribution: doom_port/data/COPYING.txt and CREDITS.txt.",
             " * Low nibble first; 0 transparent, 1..15 black through white.",
             " */", "#pragma once", "#include <stdint.h>", "",
             f"#define E1M1_UI_BYTES {total}u",
             f"#define E1M1_UI_VIEW_H {VIEW_H}u",
             "typedef struct {", "    uint16_t width, height;",
             "    int16_t x, y;", "    const uint8_t *pixels;",
             "} E1M1UiPatch;", "",
             "static const uint8_t e1m1_ui_gray[16] = {",
             "    " + ", ".join(str(v) for v in GRAY) + ",", "};", ""]
    for name, _, _, _, _, packed in assets:
        lines.append(f"static const uint8_t ui_pixels_{name.lower()}[{len(packed)}] = {{")
        for i in range(0, len(packed), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in packed[i:i + 16]) + ",")
        lines.extend(("};", ""))
    for name, w, h, x, y, _ in assets:
        lines.extend((f"static const E1M1UiPatch ui_{name.lower()} = "
                      f"{{{w}u, {h}u, {x}, {y}, ui_pixels_{name.lower()}}};", ""))
    for label, names in (("digits", [f"STTNUM{i}" for i in range(10)]),
                         ("small_digits", [f"STYSNUM{i}" for i in range(10)]),
                         ("faces", [f"STFST{i}0" for i in range(5)] + ["STFDEAD0"])):
        lines.extend((f"static const E1M1UiPatch *const ui_{label}[{len(names)}] = {{",
                      "    " + ", ".join("&ui_" + n.lower() for n in names) + ",",
                      "};", ""))
    return "\n".join(lines), assets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    header, assets = generate(SOURCE.read_bytes())
    if args.check:
        require(OUTPUT.read_text(encoding="ascii") == header, "UI header differs; regenerate it")
    else:
        OUTPUT.write_text(header, encoding="ascii", newline="\n")
    if args.preview_dir:
        args.preview_dir.mkdir(parents=True, exist_ok=True)
        for name, w, h, _, _, packed in assets:
            pixels = bytes(GRAY[(packed[i // 2] >> ((i & 1) * 4)) & 15]
                           if (packed[i // 2] >> ((i & 1) * 4)) & 15 else 225
                           for i in range(w * h))
            (args.preview_dir / f"{name}.pgm").write_bytes(
                f"P5\n{w} {h}\n255\n".encode("ascii") + pixels)
    print(f"Freedoom UI: {len(assets)} patches, {sum(len(a[5]) for a in assets)} packed bytes")


if __name__ == "__main__":
    main()
