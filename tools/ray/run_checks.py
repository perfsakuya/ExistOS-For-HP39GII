"""Build and check production Ray APIs on the host; write reproducible evidence.

The recorded durations belong to this host and never describe the HP 39gII ARM.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / "System/applications/user/ray_demo"
DEFAULT_GCC = Path("D:/w64devkit/bin/gcc.exe")
BASE_IMAGES = ["preview.pgm", "trace-c1.pgm", "trace-c2.pgm", "trace-c3.pgm"]
POSE_AA_IMAGES = ["preview-pitch.pgm", "preview-roll.pgm", "preview-tilt.pgm", "trace-tilt.pgm",
                  "trace-c2.pgm", "trace-aa.pgm", "trace-tilt.pgm", "trace-tilt-aa.pgm"]
HEIGHT_IMAGES = ["preview-low.pgm", "trace-low.pgm", "preview-high.pgm", "trace-high.pgm"]
FXAA_IMAGES = ["aa-diagonal-before.pgm", "aa-diagonal-after.pgm",
               "aa-diagonal-reference.pgm", "aa-diagonal-negative-reference.pgm",
               "aa-diagonal-negative-before.pgm", "aa-diagonal-negative-after.pgm",
               "aa-circle-before.pgm", "aa-circle-after.pgm",
               "aa-far-checker-before.pgm", "aa-far-checker-after.pgm",
               "aa-fine-lines-before.pgm", "aa-fine-lines-after.pgm"]
ALL_IMAGES = list(dict.fromkeys(BASE_IMAGES + POSE_AA_IMAGES + HEIGHT_IMAGES + FXAA_IMAGES))


def run_stage(stages, name, command, cwd, output):
    began = time.perf_counter()
    result = subprocess.run([str(arg) for arg in command], cwd=cwd,
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    duration = round((time.perf_counter() - began) * 1000, 3)
    log = output / f"{name}.log"
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    stages.append({"name": name, "command": [str(arg) for arg in command],
                   "cwd": str(cwd), "elapsed_ms": duration, "exit_code": result.returncode,
                   "log": str(log), "stdout": result.stdout, "stderr": result.stderr})
    print(f"{name}: exit={result.returncode} host_elapsed_ms={duration}", flush=True)
    if result.returncode:
        print(result.stdout + result.stderr, end="", flush=True)
    return result.returncode == 0


def contact_sheet(output):
    from PIL import Image, ImageDraw
    for name in ALL_IMAGES:
        with Image.open(output / name) as pgm:
            if pgm.size != (256, 127):
                raise ValueError(f"Unexpected Ray image dimensions: {name}: {pgm.size}")
            pgm.save(output / name.replace(".pgm", ".png"))

    def sheet_for(names, filename):
        rows = (len(names) + 1) // 2
        sheet = Image.new("RGB", (1040, rows * 301 + 20), "#ededed")
        draw = ImageDraw.Draw(sheet)
        for index, name in enumerate(names):
            with Image.open(output / name) as pgm:
                image = pgm.convert("RGB").resize((512, 254), Image.Resampling.NEAREST)
            x, y = (index % 2) * 520, (index // 2) * 301
            draw.text((x + 5, y + 6), name, fill="#111111")
            sheet.paste(image, (x + 4, y + 25))
        draw.text((5, rows * 301), "Host render correctness previews. Host duration is not ARM performance.", fill="#111111")
        sheet.save(output / filename)

    sheet_for(BASE_IMAGES, "contact-sheet.png")
    sheet_for(POSE_AA_IMAGES, "pose-aa-contact-sheet.png")
    sheet_for(HEIGHT_IMAGES, "height-contact-sheet.png")
    sheet_for(FXAA_IMAGES, "fxaa-quality-contact-sheet.png")


def dashboard_html(output):
    page = (ROOT / "tools/doom/serial_dashboard.html").read_text(encoding="utf-8")
    ids = re.findall(r'\bid="([^"]+)"', page)
    references = re.findall(r"byId\('([^']+)'\)", page)
    if len(ids) != len(set(ids)) or set(references) - set(ids):
        raise ValueError("Dashboard HTML has duplicate or missing element IDs")
    print("Dashboard HTML element IDs valid")
    script = output / "dashboard-script.js"
    script.write_text("\n".join(re.findall(r"<script>(.*?)</script>", page, re.S)), encoding="utf-8")
    node = shutil.which("node")
    if node:
        command = [node, "--check", str(script)]
        print("Syntax command: " + json.dumps(command), flush=True)
        subprocess.run(command, check=True)
    else:
        print("Node unavailable: JavaScript syntax check skipped; HTML IDs checked")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gcc", type=Path, default=DEFAULT_GCC)
    parser.add_argument("--out", type=Path, default=ROOT / "build/ray-validation")
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stages = []
    evidence = {"scope": "host_only", "host_python": sys.executable,
                "host_gcc": str(args.gcc.resolve()), "durations_are_ARM_performance": False,
                "hardware_exit_verified": False, "stages": stages, "artifacts": [], "passed": False}
    sources = [APP / name for name in ("RayCore.c", "RayCore.h", "RayPreview.c", "RaySession.c",
                                      "RaySession.h", "RayPost.c", "RayPost.h")]
    sources += [Path(__file__).resolve(), ROOT / "tools/ray/check_ray.c"]
    evidence["sources"] = [{"path": str(source), "sha256": hashlib.sha256(source.read_bytes()).hexdigest()}
                           for source in sources]
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wconversion", "-Wshadow", "-Wpedantic", "-Werror"]
    core_object = output / "RayCore.observed.o"
    executable = output / ("check_ray.exe" if os.name == "nt" else "check_ray")
    try:
        commands = [
            ("compile-core", [args.gcc, *flags, "-DRay_TracePixel=Ray_TestRealTracePixel",
                              "-c", APP / "RayCore.c", "-o", core_object]),
            ("compile-check", [args.gcc, *flags, ROOT / "tools/ray/check_ray.c",
                               APP / "RayPreview.c", APP / "RaySession.c", APP / "RayPost.c", core_object,
                               "-lm", "-o", executable]),
            ("ray-checks", [executable, output]),
            ("dashboard-checks", [sys.executable, "-m", "unittest", "test_serial_dashboard.py"]),
            ("dashboard-html-checks", [sys.executable, __file__, "--dashboard-html-only", output]),
        ]
        for name, command in commands:
            cwd = ROOT / "tools/doom" if name == "dashboard-checks" else ROOT
            if not run_stage(stages, name, command, cwd, output):
                return 1
        if not run_stage(stages, "images", [sys.executable, __file__, "--images-only", output], ROOT, output):
            return 1
        evidence["passed"] = True
        for name in ALL_IMAGES + [name.replace(".pgm", ".png") for name in ALL_IMAGES] + [
                "contact-sheet.png", "pose-aa-contact-sheet.png", "height-contact-sheet.png",
                "fxaa-quality-contact-sheet.png"]:
            artifact = output / name
            evidence["artifacts"].append({"path": str(artifact), "bytes": artifact.stat().st_size,
                                          "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest()})
        return 0
    except Exception as error:
        evidence["error"] = f"{type(error).__name__}: {error}"
        print(evidence["error"], file=sys.stderr)
        return 1
    finally:
        (output / "checks.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--images-only":
        contact_sheet(Path(sys.argv[2]))
    elif len(sys.argv) == 3 and sys.argv[1] == "--dashboard-html-only":
        dashboard_html(Path(sys.argv[2]))
    else:
        raise SystemExit(main())
