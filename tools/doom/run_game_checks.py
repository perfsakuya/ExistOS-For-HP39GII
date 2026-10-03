"""Run reproducible native Game checks; never access or flash a calculator.

The host benchmark compares workloads only. It cannot measure ARM FPS, VM
page faults, LCD submission, or physical keyboard timing.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "System/applications/user/doom_lite"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=shutil.which("gcc"))
    parser.add_argument("--output", type=Path, default=ROOT / "build/game-validation")
    args = parser.parse_args()
    if not args.cc:
        parser.error("A native C compiler is required; pass --cc <gcc path>.")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []

    def run(name: str, command: list[str], timeout: int = 180) -> None:
        started = time.perf_counter()
        result = subprocess.run(command, cwd=ROOT, capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=timeout)
        duration = time.perf_counter() - started
        log = output / f"{name}.log"
        log.write_text(result.stdout + result.stderr, encoding="utf-8", newline="\n")
        records.append({"name": name, "command": command, "seconds": round(duration, 4),
                        "exit_code": result.returncode, "log": str(log)})
        (output / "checks.json").write_text(json.dumps({
            "host_only": True, "device_access": False, "checks": records,
            "all_passed": all(record["exit_code"] == 0 for record in records),
        }, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"{name}: {'PASS' if not result.returncode else 'FAIL'} ({duration:.3f}s)", flush=True)
        if result.returncode:
            print((result.stdout + result.stderr)[-6000:])
            raise SystemExit(result.returncode)

    python = sys.executable
    run("python-unit", [python, "-m", "unittest", "discover", "-s", "tools/doom", "-p", "test_*.py"])
    run("maps-reproduce", [python, "tools/doom/build_game_maps.py", "--check"])
    run("sprites-reproduce", [python, "tools/doom/build_game_sprites.py", "--check"])
    run("textures-reproduce", [python, "tools/doom/build_game_world.py", "--check"])
    run("routes-audit", [python, "tools/doom/audit_game_routes.py", "--output", str(output / "routes.json")])
    shared = [str(MODULE / "DoomLiteGame.c"), str(MODULE / "DoomMap.c")]
    tests = {
        "maps": (["tools/doom/test_game_map_data.c", str(MODULE / "DoomMap.c")], []),
        "sight": (["tools/doom/test_game_los.c", str(MODULE / "DoomMap.c")], []),
        "logic": (["tools/doom/check_lite_game.c", *shared], []),
        "walking-routes": (["tools/doom/check_lite_routes.c", *shared], []),
        "exit": (["tools/doom/check_lite_exit.c", *shared], []),
        "ui": (["tools/doom/check_lite_ui.c", *shared], []),
        "lite-rays": (["tools/doom/check_lite_rays.c", *shared], []),
        "renderer": (["tools/doom/check_game_renderer.c", *shared], [str(output)]),
        "world-render": (["tools/doom/check_game_world_render.c", *shared], []),
        "directions": (["tools/doom/check_game_directions.c", *shared], [str(output)]),
        "platform": (["tools/doom/check_game_platform.c", *shared], [str(output)]),
        "viewport": (["tools/doom/check_game_viewport.c", str(MODULE / "DoomLite.c"),
                      *shared, str(MODULE / "DoomLiteHud.c")], [str(output)]),
        "benchmark": (["tools/doom/check_game_bench.c", str(MODULE / "DoomLite.c"),
                       *shared, str(MODULE / "DoomLiteHud.c")], [str(output)]),
    }
    for name, (sources, arguments) in tests.items():
        binary = output / (name + (".exe" if sys.platform == "win32" else ""))
        flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(MODULE)]
        if name in ("benchmark", "viewport"):
            flags += ["-DDOOM_LITE_RAY_TEST", "-DLCD_PIX_W=256", "-DLCD_PIX_H=127"]
        run(name + "-compile", [args.cc, *flags, *sources, "-lm", "-o", str(binary)])
        run(name, [str(binary), *arguments])
    print(f"All offline checks passed. Report: {output / 'checks.json'}")


if __name__ == "__main__":
    main()
