"""Generate the 50x25, 8-bit grayscale SkyOS OSLoader splash asset.

The header is consumed by OSLoader/start.c at both normal boot and after
leaving USB mass-storage mode.  The checked-in pixels make the build independent
of the local font; this generator documents how they were produced.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent
HEADER = ROOT.parent / "Include" / "logo.h"
WIDTH, HEIGHT = 50, 25
SCALE = 6


def render(font_path: Path) -> Image.Image:
    font = ImageFont.truetype(str(font_path), 23 * SCALE)
    box = font.getbbox("SkyOS")
    large = Image.new("L", (box[2] - box[0] + 4 * SCALE,
                            box[3] - box[1] + 4 * SCALE), 255)
    draw = ImageDraw.Draw(large)
    draw.text((2 * SCALE - box[0], 2 * SCALE - box[1]),
              "SkyOS", font=font, fill=0)
    ink = large.crop((2 * SCALE, 2 * SCALE,
                      2 * SCALE + box[2] - box[0],
                      2 * SCALE + box[3] - box[1]))
    word = ink.resize((47, 21), Image.Resampling.LANCZOS)
    logo = Image.new("L", (WIDTH, HEIGHT), 255)
    logo.paste(word, ((WIDTH - word.width) // 2,
                      (HEIGHT - word.height) // 2))
    return logo


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path,
                        default=Path("C:/Windows/Fonts/timesbd.ttf"))
    args = parser.parse_args()
    if not args.font.is_file():
        raise SystemExit(f"font not found: {args.font}")
    logo = render(args.font)
    values = logo.tobytes()
    assert len(values) == 1250
    lines = ["const unsigned char logo[1250] = {"]
    for start in range(0, len(values), 16):
        line = ", ".join(f"0x{v:02X}" for v in values[start:start + 16])
        lines.append(f"    {line},")
    lines.append("};")
    HEADER.write_bytes(("\n".join(lines) + "\n").encode("ascii"))

    logo.save(ROOT / "skyos-logo-50x25.png")
    screen = Image.new("L", (256, 127), 255)
    screen.paste(logo, (103, 32))
    ImageDraw.Draw(screen).rectangle((90, 84, 202, 92), fill=72)
    screen.resize((1024, 508), Image.Resampling.NEAREST).save(
        ROOT / "skyos-boot-preview.png")
    manifest = {
        "wordmark": "SkyOS",
        "font": args.font.name,
        "font_sha256": hashlib.sha256(args.font.read_bytes()).hexdigest(),
        "format": "50x25 8-bit row-major grayscale, 0=black, 255=white",
        "logo_sha256": hashlib.sha256(values).hexdigest(),
    }
    (ROOT / "skyos-logo.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
