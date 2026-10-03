"""Local read-only serial log dashboard for HP 39gII debugging."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import re
import time

ROOT = Path(__file__).resolve().parent
PAGE = ROOT / "serial_dashboard.html"
LOG = ROOT / "serial.log"
FOLLOW_DIR = None
SERIAL_NAME = "串口"

MEM_RE = re.compile(r"Allocate MEM:(\d+)/(\d+) KB")
ZRAM_RE = re.compile(r"ZRAM:(\d+)/(\d+) KB")
SRAM_PRE_RE = re.compile(r"SRAM Heap Pre-allocated: (\d+) KB")
SWAP_PRE_RE = re.compile(r"Swap Heap Pre-allocated: (\d+) KB")
GAME_MEM_RE = re.compile(
    r"(?m)^DOOMG_MEM a=(\d+)/(\d+) z=(\d+)/(\d+) s=(\d+) w=(\d+)\r?$"
)
RAY_MEM_RE = re.compile(
    r"(?m)^RAY_MEM a=(\d+)/(\d+) z=(\d+)/(\d+) s=(\d+) w=(\d+)\r?$"
)
ERROR_RE = re.compile(r"DOOM_ERROR ([^\r\n]+)")
FRAME_RE = re.compile(r"DOOM_FRAME count=(\d+)")
PERF_RE = re.compile(
    r"DOOM_PERF frames=(\d+) wall_ms=(\d+) convert_ms=(\d+) "
    r"display_ms=(\d+) other_ms=(\d+)"
)
PHASE_LINE_RE = re.compile(r"(?m)^DOOM_PHASE\b[^\r\n]*")
RENDER_RE = re.compile(
    r"DOOM_RENDER frames=(\d+) setup_ms=(\d+) bsp_ms=(\d+) "
    r"planes_ms=(\d+) masked_ms=(\d+)"
)
LITE_RE = re.compile(
    r"DOOMLITE_PERF frames=(\d+) elapsed_ms=(\d+) render_ms=(\d+) "
    r"queue_ms=(\d+) max_interval_ms=(\d+) map=(\d+)"
)
FIELD_RE = re.compile(r"([a-z_]+)=(-?\d+)")
MODE_RE = re.compile(r"(?m)^DOOM_MODE\b[^\r\n]*")
FAST_RE = re.compile(r"(?m)^DOOM_FAST\b[^\r\n]*")
TIC_RE = re.compile(r"(?m)^DOOM_TIC\b[^\r\n]*")
GAME_PERF_RE = re.compile(r"(?m)^DOOMG_PERF\b[^\r\n]*")
GAME_DETAIL_RE = re.compile(r"(?m)^DOOMG_DETAIL\b[^\r\n]*")
GAME_BOOT_RE = re.compile(r"(?m)^DOOMG_BOOT\b[^\r\n]*")
GAME_EXIT_RE = re.compile(r"(?m)^DOOMG_EXIT\b[^\r\n]*")
DIAG_HEARTBEAT_RE = re.compile(
    r"DOOM_DIAG_HEARTBEAT seconds=(\d+) key=([0-9a-fA-F]+) "
    r"allocated=(\d+) critical=(\d+)"
)
DIAG_STAGE_RE = re.compile(r"DOOM_DIAG_STAGE ([^\r\n]+)")
RAY_LINE_RE = re.compile(r"(?m)^RAY_(BOOT|EVENT|PERF|EXIT)\b[^\r\n]*")
RAY_FIELD_RE = re.compile(r"([a-z_]+)=([^\s]+)")


def ray_state(raw):
    """Keep one Ray launch and generation; retain ASCII protocol values."""
    packets = list(RAY_LINE_RE.finditer(raw))
    if not packets:
        return None, -1
    boots = [packet for packet in packets if packet[1] == "BOOT"]
    starts = [packet for packet in boots if "phase=task_start" in packet[0] or
              "phase=task_allocation_error" in packet[0]]
    start = starts[-1].start() if starts else boots[0].start() if boots else packets[0].start()
    context, boot, perf, event, exits = {}, {}, None, None, []
    completed_elapsed_ms = None
    for packet in packets:
        if packet.start() < start:
            continue
        values = {key: int(value) if re.fullmatch(r"-?\d+", value) else value
                  for key, value in RAY_FIELD_RE.findall(packet[0])}
        kind = packet[1]
        if kind == "BOOT":
            boot.update(values)
        elif kind == "EXIT":
            exits.append(values.get("phase"))
        else:
            if ("generation" in values and values.get("generation") != context.get("generation")):
                perf = None
                context = {}
                completed_elapsed_ms = None
            if kind == "EVENT":
                event = values.get("event")
                if event in ("start", "cancel", "contrast", "reset"):
                    perf = None
                    completed_elapsed_ms = None
            else:
                perf = values
                # Later DONE heartbeats include time spent viewing the result.
                # Keep the first complete PERF duration, while raw stays current.
                if (completed_elapsed_ms is None and values.get("phase") in (3, "done") and
                        values.get("samples") == 32512 and "elapsed_ms" in values):
                    completed_elapsed_ms = values["elapsed_ms"]
            context.update(values)
    phase = context.get("phase", boot.get("phase", "boot"))
    phase = {0: "preview", 1: "wait", 2: "trace", 3: "done"}.get(phase, phase)
    if boot.get("phase") in ("task_allocation_error", "framebuffer_error"):
        status, stage = "error", "Ray 启动失败"
    elif "ui_resume_done" in exits or "task_delete" in exits:
        status, stage = "exited", "Ray 已退出"
    elif exits:
        status, stage = "exiting", "Ray 退出处理中"
    else:
        status = phase
        stage = {"preview": "Ray 预览中", "wait": "Ray 等待静止",
                 "trace": "Ray 追踪中", "done": "Ray 追踪完成"}.get(phase, "Ray 启动中")
    samples = context.get("samples", 0)
    total = boot.get("pixels", boot.get("width", 256) * boot.get("height", 127))
    return {
        "status": status, "stage": stage, "phase": phase, "event": event,
        "generation": context.get("generation"), "pass": context.get("pass", 0),
        "samples": samples, "total_samples": total,
        "progress_percent": round(min(samples / total, 1) * 100, 2) if total else 0,
        "contrast": context.get("contrast", boot.get("contrast")),
        "elapsed_ms": completed_elapsed_ms if completed_elapsed_ms is not None else
                      perf.get("elapsed_ms") if perf else None,
        **{key: perf.get(key) if perf else None for key in (
            "batch_samples", "trace_us", "preview_us", "lcd_us", "batch_max_us",
            "primary", "reflection", "refraction", "shadow", "sphere_tests", "plane_tests",
            "glass_exits", "tir_events", "floor_reflection",
            "camera_us", "intersect_us", "shadow_us", "shade_us", "reflection_us", "refraction_us", "stack_words")},
        "exit_phases": exits, "raw": perf, "boot": boot,
    }, packets[-1].start()


def fields(line):
    return {key: int(value) for key, value in FIELD_RE.findall(line)}


def latest_fields(pattern, raw):
    matches = pattern.findall(raw)
    return fields(matches[-1]) if matches else None


def mode_for_start(raw, start):
    """The firmware prints DOOM_MODE before DOOM_START.

    Also accept a mode line after DOOM_START for future firmware revisions.
    Do not inherit a previous run's mode when a new start has no mode line.
    """
    run_mode = latest_fields(MODE_RE, raw[start:])
    if run_mode is not None:
        return run_mode
    previous_start = raw.rfind("DOOM_START", 0, start)
    previous_exit = raw.rfind("DOOM_EXIT", 0, start)
    boundary = max(previous_start, previous_exit)
    return latest_fields(MODE_RE, raw[boundary + 1:start]) or {}


def current_log():
    if FOLLOW_DIR is not None:
        candidates = [path for path in FOLLOW_DIR.glob("freedoom-*-hardware.log")
                      if path.is_file()]
        if candidates:
            return max(candidates, key=lambda path: path.stat().st_mtime_ns)
    return LOG


def state():
    log = current_log()
    raw = log.read_text(encoding="ascii", errors="replace") if log.exists() else ""
    lines = raw.splitlines()
    mem = [(m.start(), int(m[1]), int(m[2])) for m in MEM_RE.finditer(raw)]
    zram = [(m.start(), int(m[1]), int(m[2])) for m in ZRAM_RE.finditer(raw)]
    sram_pre = SRAM_PRE_RE.findall(raw)
    swap_pre = SWAP_PRE_RE.findall(raw)
    game_mem = [(m.start(), *(int(value) for value in m.groups()))
                for m in GAME_MEM_RE.finditer(raw)]
    last_start = raw.rfind("DOOM_START")
    last_lite_perf = raw.rfind("DOOMLITE_PERF")
    last_lite_exit = raw.rfind("DOOMLITE_EXIT")
    last_game_boot = raw.rfind("DOOMG_BOOT phase=task_start")
    last_game_perf = raw.rfind("DOOMG_PERF")
    last_game_exit = raw.rfind("DOOMG_EXIT")
    last_game = max(last_game_boot, last_game_perf, last_game_exit)
    ray, last_ray = ray_state(raw)
    if last_ray > max(last_game, last_start, last_lite_perf, last_lite_exit, raw.rfind("DOOM_EXIT")):
        mode = "ray"
    elif last_game > max(last_start, last_lite_perf, last_lite_exit):
        mode = "game"
    elif last_start > max(last_lite_perf, last_lite_exit):
        mode = "full"
    elif last_lite_perf > last_lite_exit:
        mode = "lite_running"
    elif last_lite_exit >= 0 and last_lite_exit > last_start:
        mode = "lite_exited"
    else:
        mode = "idle"
    run = raw[last_start:] if mode == "full" else ""
    game_run = raw[last_game_boot:] if mode == "game" and last_game_boot >= 0 else ""
    game_perf = latest_fields(GAME_PERF_RE, game_run)
    game_detail = latest_fields(GAME_DETAIL_RE, game_run)
    game_boot = GAME_BOOT_RE.findall(game_run)
    game_context = {}
    for line in game_boot:
        game_context.update(fields(line))
    map_load = game_run.rfind("DOOMG_BOOT phase=map_load")
    perf_position = game_run.rfind("DOOMG_PERF")
    if map_load > perf_position:
        game_perf = game_detail = None
    elif game_run.rfind("DOOMG_DETAIL") < perf_position:
        game_detail = None  # A new PERF packet must not inherit the old DETAIL packet.
    if game_perf:
        game_context.update(game_perf)
    test_mode = bool(game_context.get("test", 0))
    preset = game_context.get("preset", 0)
    preset_names = ("SPRITES", "COMBAT", "ITEMS")
    preset_name = preset_names[preset] if 0 <= preset < len(preset_names) else "UNKNOWN"
    game_label = f"Test {preset_name}" if test_mode else f"E1M{game_context.get('level', 1)} Game"
    game_exits = GAME_EXIT_RE.findall(game_run)
    latest_system_mem = mem[-1][0] if mem else -1
    latest_game_mem = next((sample for sample in reversed(game_mem)
                            if sample[0] >= last_game_boot), None)
    latest_game_mem_position = latest_game_mem[0] if latest_game_mem else -1
    if (mode == "game" and last_game_exit >= last_game_boot and
            last_game_exit > max(latest_game_mem_position, latest_system_mem)):
        # The Game has exited, but the next System status sample has not arrived.
        memory_kb = zram_kb = heap_preallocated_kb = None
    elif mode == "game" and latest_game_mem and latest_game_mem[0] > latest_system_mem:
        _, allocated, capacity, zram_used, zram_capacity, sram_pre, swap_pre = latest_game_mem
        memory_kb = (round(allocated / 1024, 2), round(capacity / 1024, 2))
        zram_kb = (round(zram_used / 1024, 2), round(zram_capacity / 1024, 2))
        heap_preallocated_kb = (round(sram_pre / 1024, 2), round(swap_pre / 1024, 2))
    elif mode == "game" and latest_system_mem < last_game_boot:
        # The running Game intentionally suppresses the old System status dump.
        # Do not present a pre-launch value as a live sample.
        memory_kb = zram_kb = heap_preallocated_kb = None
    else:
        memory_kb = mem[-1][1:] if mem else None
        zram_kb = zram[-1][1:] if zram else None
        heap_preallocated_kb = (int(sram_pre[-1]), int(swap_pre[-1])) \
            if sram_pre and swap_pre else None
    memory_history = sorted(
        [(position, allocated) for position, allocated, _ in mem] +
        [(sample[0], sample[1] / 1024) for sample in game_mem]
    )
    if mode == "game" and memory_kb is None:
        memory_history = []
    if mode == "ray":
        ray_start = raw.rfind("RAY_BOOT phase=task_start")
        ray_memory = [(m.start(), *(int(value) for value in m.groups()))
                      for m in RAY_MEM_RE.finditer(raw) if m.start() >= ray_start]
        last_ray_memory = ray_memory[-1] if ray_memory else None
        ray_memory_position = last_ray_memory[0] if last_ray_memory else -1
        ray_exit = raw.rfind("RAY_EXIT")
        if ray_exit >= ray_start and ray_exit > max(ray_memory_position, latest_system_mem):
            memory_kb = zram_kb = heap_preallocated_kb = None
        elif last_ray_memory and ray_memory_position > latest_system_mem:
            _, allocated, capacity, used, zcapacity, sram, swap = last_ray_memory
            memory_kb = (round(allocated / 1024, 2), round(capacity / 1024, 2))
            zram_kb = (round(used / 1024, 2), round(zcapacity / 1024, 2))
            heap_preallocated_kb = (round(sram / 1024, 2), round(swap / 1024, 2))
        elif latest_system_mem < ray_start:
            memory_kb = zram_kb = heap_preallocated_kb = None
        memory_history = (sorted(
            [(position, allocated) for position, allocated, _ in mem] +
            [(sample[0], sample[1] / 1024) for sample in ray_memory])
                          if memory_kb is not None else [])
    fast_mode = mode == "full" and mode_for_start(raw, last_start).get("fast") == 1
    errors = ERROR_RE.findall(run if mode == "full" else raw)
    frames = FRAME_RE.findall(run)
    perf = PERF_RE.findall(run) if not fast_mode else []
    last_perf = [int(value) for value in perf[-1]] if perf else None
    last_phase = latest_fields(PHASE_LINE_RE, run)
    renders = RENDER_RE.findall(run) if not fast_mode else []
    last_render = [int(value) for value in renders[-1]] if renders else None
    lite_batches = LITE_RE.findall(raw) if mode.startswith("lite") else []
    last_lite = [int(value) for value in lite_batches[-1]] if lite_batches else None
    fast = latest_fields(FAST_RE, run) if fast_mode else None
    tic = latest_fields(TIC_RE, run) if fast_mode else None
    diagnostic_heartbeats = DIAG_HEARTBEAT_RE.findall(run)
    last_diagnostic = diagnostic_heartbeats[-1] if diagnostic_heartbeats else None
    diagnostic_stages = DIAG_STAGE_RE.findall(run)
    last_exit = raw.rfind("DOOM_EXIT")
    last_cleanup_done = raw.rfind("DOOM_CLEANUP ui_resumed")
    last_zone = raw.rfind("DOOM_ZONE backing")
    last_verified = raw.rfind("DOOM_ZONE verified")
    last_status = raw.rfind("=============SYSTEM STATUS")
    latest_task_list = raw[last_status:] if last_status >= 0 else ""
    doom_task_visible = bool(re.search(r"(?m)^Doom\s+[XRBSD]\s+", latest_task_list))
    if mode == "ray":
        stage = ray["stage"]
    elif mode == "game":
        prefix = game_label
        if any("phase=ui_resume_done" in line for line in game_exits):
            stage = prefix + " 已退出"
        elif game_exits:
            stage = prefix + " 退出处理中"
        elif game_perf:
            stage = prefix + " 运行中"
        else:
            stage = prefix + " 启动中"
    elif mode == "lite_running":
        stage = "E1M1 Lite 运行中"
    elif mode == "lite_exited":
        stage = "E1M1 Lite 已退出"
    elif mode == "idle":
        stage = "等待启动"
    elif last_exit > last_start:
        prefix = "E1M1 混合版" if fast_mode else "E1M1"
        stage = prefix + (" 已退出" if last_cleanup_done > last_exit else " 退出清理中")
    elif last_status > last_start and not doom_task_visible:
        stage = "应用界面"
    elif last_verified > last_start:
        stage = "E1M1 混合版运行中" if fast_mode else "引擎运行中"
    elif last_zone > last_start:
        stage = "交换区校验中"
    else:
        stage = "启动中"
    age = time.time() - log.stat().st_mtime if log.exists() else None
    return {
        "connected": age is not None and age < 25,
        "serial_name": SERIAL_NAME,
        "log_name": log.name,
        "age_seconds": round(age, 1) if age is not None else None,
        "memory_kb": memory_kb,
        "zram_kb": zram_kb,
        "heap_preallocated_kb": heap_preallocated_kb,
        "memory_history": [value for _, value in memory_history[-40:]],
        "starts": raw.count("DOOM_START") + raw.count("DOOMG_BOOT phase=task_start") + raw.count("RAY_BOOT phase=task_start"),
        "mode": "ray" if mode == "ray" else "game" if mode == "game" else "hybrid" if fast_mode else "legacy" if mode == "full"
                else "lite" if mode.startswith("lite") else "idle",
        "stage": stage,
        "ray": ray if mode == "ray" else None,
        "zone_verified": raw.count("DOOM_ZONE verified"),
        "errors": len(errors),
        "frames": len(frames),
        "latest_frame": (game_perf.get("frame_count", game_perf.get("frames", 0))
                         if mode == "game" and game_perf else int(frames[-1]) if frames else 0),
        "performance": {
            "fps": round(last_perf[0] * 1000 / last_perf[1], 2) if last_perf[1] else None,
            "total_ms": round(last_perf[1] / last_perf[0]),
            "convert_ms": round(last_perf[2] / last_perf[0]),
            "display_ms": round(last_perf[3] / last_perf[0]),
            "other_ms": round(last_perf[4] / last_perf[0]),
        } if last_perf and last_perf[0] else None,
        "phases": {
            "logic_ms": round(last_phase["logic_ms"] / last_phase["frames"], 1),
            "draw_ms": round(last_phase["draw_ms"] / last_phase["frames"], 1),
            "logic_max_ms": last_phase.get("logic_max_ms"),
            "draw_max_ms": last_phase.get("draw_max_ms"),
        } if last_phase and all(key in last_phase for key in
                                ("frames", "logic_ms", "draw_ms"))
             and last_phase["frames"] > 0 else None,
        "render": {
            "setup_ms": round(last_render[1] / last_render[0]),
            "bsp_ms": round(last_render[2] / last_render[0]),
            "planes_ms": round(last_render[3] / last_render[0]),
            "masked_ms": round(last_render[4] / last_render[0]),
        } if last_render and last_render[0] else None,
        "lite": {
            "fps": round(last_lite[0] * 1000 / last_lite[1], 2),
            "frame_ms": round(last_lite[1] / last_lite[0], 1),
            "render_ms": round(last_lite[2] / last_lite[0], 1),
            "queue_ms": round(last_lite[3] / last_lite[0], 1),
            "max_interval_ms": last_lite[4],
            "map": bool(last_lite[5]),
        } if last_lite and last_lite[0] and last_lite[1] else None,
        "fast": {
            "fps": round(fast["frames"] * 1000 / fast["elapsed_ms"], 2),
            "frame_ms": round(fast["elapsed_ms"] / fast["frames"], 1),
            "max_interval_ms": fast.get("max_interval_ms"),
            "render_ms": round(fast["render_ms"] / fast["frames"], 1),
            "max_render_ms": fast.get("max_render_ms"),
            "present_ms": round(fast["present_ms"] / fast["frames"], 1),
            "max_present_ms": fast.get("max_present_ms"),
            "health": fast.get("hp"),
            "kills": fast.get("kills"),
            "blue_key": bool(fast["blue"]) if "blue" in fast else None,
            "map": bool(fast["map"]) if "map" in fast else None,
        } if fast and all(key in fast for key in
                          ("frames", "elapsed_ms", "render_ms", "present_ms"))
             and fast["frames"] > 0 and fast["elapsed_ms"] > 0 else None,
        "tic": {
            "average_ms": round(tic["sum_ms"] / tic["tics"], 2),
            "max_ms": tic["max_ms"],
        } if tic and all(key in tic for key in ("tics", "sum_ms", "max_ms"))
             and tic["tics"] > 0 else None,
        "game": {
            "fps": round(game_perf["frames"] * 1000 / game_perf["elapsed_ms"], 2),
            "frame_ms": round(game_perf["elapsed_ms"] / game_perf["frames"], 2),
            "logic_ticks": game_perf["logic_ticks"],
            "logic_ms": round(game_perf.get("logic_total_us",
                                            game_perf["logic_total_ms"] * 1000) /
                              (game_perf["logic_ticks"] * 1000), 3)
                        if game_perf["logic_ticks"] else None,
            "logic_max_ms": round(game_perf.get("logic_max_us",
                                                 game_perf["logic_max_ms"] * 1000) / 1000, 3),
            "render_ms": round(game_perf.get("render_total_us",
                                             game_perf["render_total_ms"] * 1000) /
                               (game_perf["frames"] * 1000), 3),
            "render_max_ms": round(game_perf.get("render_max_us",
                                                  game_perf["render_max_ms"] * 1000) / 1000, 3),
            **{f"{phase}_ms": round(game_perf[f"{phase}_total_us"] /
                                    (game_perf["frames"] * 1000), 3)
               if f"{phase}_total_us" in game_perf else None
               for phase in ("scene", "sprite", "hud", "weapon")},
            **{f"{phase}_max_ms": round(game_perf[f"{phase}_max_us"] / 1000, 3)
               if f"{phase}_max_us" in game_perf else None
               for phase in ("scene", "sprite", "hud", "weapon")},
            "lcd_ms": round(game_perf.get("lcd_total_us",
                                          game_perf["lcd_total_ms"] * 1000) /
                            (game_perf["frames"] * 1000), 3),
            "lcd_max_ms": round(game_perf.get("lcd_max_us",
                                               game_perf["lcd_max_ms"] * 1000) / 1000, 3),
            "interval_ms": round(game_perf["interval_total_ms"] /
                                 game_perf.get("interval_samples", game_perf["frames"]), 2)
                           if game_perf.get("interval_samples", game_perf["frames"]) else None,
            "interval_max_ms": game_perf["interval_max_ms"],
            "dropped_ticks": game_perf["dropped_ticks"],
            "health": game_perf.get("hp"),
            "ammo": game_perf.get("ammo"),
            "kills": game_perf.get("kills"),
            "blue_key": bool(game_perf["blue"]) if "blue" in game_perf else None,
            "map": bool(game_perf["map"]) if "map" in game_perf else None,
            "zoom": game_perf.get("zoom"),
            "pistol_tics": game_perf.get("pistol_tics"),
            "weapon": game_perf.get("weapon", 0),
            "weapon_name": "SG" if game_perf.get("weapon", 0) == 1 else "PIS",
            "weapon_ammo": game_perf.get("weapon_ammo", game_perf.get("ammo")),
            "shells": game_perf.get("shells"),
            "shotgun_cooldown": game_perf.get("shotgun_cooldown"),
            "test": test_mode,
            "preset": preset if test_mode else None,
            "preset_name": preset_name if test_mode else None,
            "preview_ms": round(game_perf["preview_total_us"] / (game_perf["preview_calls"] * 1000), 3)
                if game_perf.get("preview_calls") else None,
            "preview_max_ms": round(game_perf["preview_max_us"] / 1000, 3)
                if "preview_max_us" in game_perf else None,
            "level": game_perf.get("level", 1),
            "armor": game_perf.get("armor"),
            "red_key": bool(game_perf["red"]) if "red" in game_perf else None,
            "yellow_key": bool(game_perf["yellow"]) if "yellow" in game_perf else None,
            "contrast": game_perf.get("contrast"),
            "actors": game_perf.get("actors"),
            "movers": game_perf.get("movers"),
            "x": game_perf.get("x"),
            "y": game_perf.get("y"),
        } if game_perf and all(key in game_perf for key in
                             ("frames", "elapsed_ms", "logic_ticks",
                              "logic_total_ms", "logic_max_ms",
                              "render_total_ms", "render_max_ms",
                              "lcd_total_ms", "lcd_max_ms",
                              "interval_total_ms", "interval_max_ms",
                              "dropped_ticks"))
             and game_perf["frames"] > 0 and game_perf["elapsed_ms"] > 0 else None,
        "game_boot": game_boot[-1] if game_boot else None,
        "game_label": game_label if mode == "game" else None,
        "game_resources": next(({
            "state_kb": round(fields(line)["state_bytes"] / 1024, 2),
            "render_kb": round(fields(line)["render_bytes"] / 1024, 2),
        } for line in reversed(game_boot) if "state_bytes=" in line and "render_bytes=" in line), None),
        "game_world_kb": next((round(fields(line)["world_bytes"] / 1024, 2)
                               for line in reversed(game_boot) if "world_bytes=" in line), None),
        "game_sprite_kb": next((round(fields(line)["sprite_bytes"] / 1024, 2)
                                for line in reversed(game_boot) if "sprite_bytes=" in line), None),
        "game_ui_kb": next((round(fields(line)["ui_bytes"] / 1024, 2)
                            for line in reversed(game_boot) if "ui_bytes=" in line), None),
        "game_test_kb": next((round(fields(line)["test_bytes"] / 1024, 2)
                              for line in reversed(game_boot) if "test_bytes=" in line), None),
        "game_detail": {
            "span_active_kb": round(game_detail["span_total_bytes"] /
                                    (game_detail["frames"] * 1024), 2)
                if game_detail.get("frames") and "span_total_bytes" in game_detail else None,
            "span_peak_kb": round(game_detail["span_peak_bytes"] / 1024, 2)
                if "span_peak_bytes" in game_detail else None,
            **{f"{phase}_ms": round(game_detail[f"{phase}_total_us"] /
                                     (game_detail["frames"] * 1000), 3)
               if game_detail.get("frames") and f"{phase}_total_us" in game_detail else None
               for phase in ("ray", "plane", "wall")},
            **{f"{phase}_max_ms": round(game_detail[f"{phase}_max_us"] / 1000, 3)
               if f"{phase}_max_us" in game_detail else None
               for phase in ("ray", "plane", "wall")},
            **{f"{phase}_ms": round(game_detail[f"{phase}_total_us"] /
                                     (game_detail["tics"] * 1000), 3)
               if game_detail.get("tics") and f"{phase}_total_us" in game_detail else None
               for phase in ("ai", "anim", "mover", "collision", "los")},
            **{f"{phase}_max_ms": round(game_detail[f"{phase}_max_us"] / 1000, 3)
               if f"{phase}_max_us" in game_detail else None
               for phase in ("ai", "anim", "mover", "collision", "los")},
            "raw": game_detail,
        } if game_detail and game_perf and
             game_detail.get("level", 1) == game_perf.get("level", 1) else None,
        "game_exit": game_exits[-1] if game_exits else None,
        "diagnostic": {
            "seconds": int(last_diagnostic[0]) if last_diagnostic else None,
            "key": last_diagnostic[1] if last_diagnostic else None,
            "allocated": int(last_diagnostic[2]) if last_diagnostic else None,
            "critical": int(last_diagnostic[3]) if last_diagnostic else None,
            "last_stage": diagnostic_stages[-1] if diagnostic_stages else None,
        } if last_diagnostic or diagnostic_stages else None,
        "exits": raw.count("DOOM_EXIT") + raw.count("DOOMG_EXIT phase=task_delete") + raw.count("RAY_EXIT phase=task_delete"),
        "panics": raw.lower().count("system panic"),
        "latest_error": errors[-1] if errors else None,
        "lines": lines[-120:],
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/state":
            data = json.dumps(state(), ensure_ascii=False).encode("utf-8")
            mime = "application/json; charset=utf-8"
        elif self.path == "/log":
            log = current_log()
            data = log.read_bytes() if log.exists() else b""
            mime = "text/plain; charset=us-ascii"
        elif self.path in ("/", "/index.html"):
            data = PAGE.read_bytes()
            mime = "text/html; charset=utf-8"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--serial-name", default="COM6")
    parser.add_argument("--follow-dir", type=Path,
                        help="follow the newest freedoom-*-hardware.log")
    args = parser.parse_args()
    LOG = args.log.resolve()
    FOLLOW_DIR = args.follow_dir.resolve() if args.follow_dir else None
    SERIAL_NAME = args.serial_name
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"SERIAL_DASHBOARD http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()
