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
ERROR_RE = re.compile(r"DOOM_ERROR ([^\r\n]+)")
FRAME_RE = re.compile(r"DOOM_FRAME count=(\d+)")
PERF_RE = re.compile(
    r"DOOM_PERF frames=(\d+) wall_ms=(\d+) convert_ms=(\d+) "
    r"display_ms=(\d+) other_ms=(\d+)"
)
PHASE_RE = re.compile(r"DOOM_PHASE frames=(\d+) logic_ms=(\d+) draw_ms=(\d+)")
RENDER_RE = re.compile(
    r"DOOM_RENDER frames=(\d+) setup_ms=(\d+) bsp_ms=(\d+) "
    r"planes_ms=(\d+) masked_ms=(\d+)"
)
LITE_RE = re.compile(
    r"DOOMLITE_PERF frames=(\d+) elapsed_ms=(\d+) render_ms=(\d+) "
    r"queue_ms=(\d+) max_interval_ms=(\d+) map=(\d+)"
)
DIAG_HEARTBEAT_RE = re.compile(
    r"DOOM_DIAG_HEARTBEAT seconds=(\d+) key=([0-9a-fA-F]+) "
    r"allocated=(\d+) critical=(\d+)"
)
DIAG_STAGE_RE = re.compile(r"DOOM_DIAG_STAGE ([^\r\n]+)")


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
    mem = [(int(a), int(b)) for a, b in MEM_RE.findall(raw)]
    zram = [(int(a), int(b)) for a, b in ZRAM_RE.findall(raw)]
    errors = ERROR_RE.findall(raw)
    last_start = raw.rfind("DOOM_START")
    frames = FRAME_RE.findall(raw[last_start:] if last_start >= 0 else "")
    perf = PERF_RE.findall(raw[last_start:] if last_start >= 0 else "")
    last_perf = [int(value) for value in perf[-1]] if perf else None
    phases = PHASE_RE.findall(raw[last_start:] if last_start >= 0 else "")
    last_phase = [int(value) for value in phases[-1]] if phases else None
    renders = RENDER_RE.findall(raw[last_start:] if last_start >= 0 else "")
    last_render = [int(value) for value in renders[-1]] if renders else None
    lite_batches = LITE_RE.findall(raw)
    last_lite = [int(value) for value in lite_batches[-1]] if lite_batches else None
    last_lite_perf = raw.rfind("DOOMLITE_PERF")
    last_lite_exit = raw.rfind("DOOMLITE_EXIT")
    diagnostic_heartbeats = DIAG_HEARTBEAT_RE.findall(raw)
    last_diagnostic = diagnostic_heartbeats[-1] if diagnostic_heartbeats else None
    diagnostic_stages = DIAG_STAGE_RE.findall(raw)
    last_exit = raw.rfind("DOOM_EXIT")
    last_cleanup_done = raw.rfind("DOOM_CLEANUP ui_resumed")
    last_zone = raw.rfind("DOOM_ZONE backing")
    last_verified = raw.rfind("DOOM_ZONE verified")
    last_status = raw.rfind("=============SYSTEM STATUS")
    latest_task_list = raw[last_status:] if last_status >= 0 else ""
    doom_task_visible = bool(re.search(r"(?m)^Doom\s+[XRBSD]\s+", latest_task_list))
    if last_lite_perf > last_start and last_lite_perf > last_lite_exit:
        stage = "E1M1 Lite 运行中"
    elif last_lite_exit > last_start:
        stage = "E1M1 Lite 已退出"
    elif last_start < 0:
        stage = "等待启动"
    elif last_exit > last_start:
        stage = "已退出" if last_cleanup_done > last_exit else "退出清理中"
    elif last_status > last_start and not doom_task_visible:
        stage = "应用界面"
    elif last_verified > last_start:
        stage = "引擎运行中"
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
        "memory_kb": mem[-1] if mem else None,
        "zram_kb": zram[-1] if zram else None,
        "memory_history": [a for a, _ in mem[-40:]],
        "starts": raw.count("DOOM_START"),
        "stage": stage,
        "zone_verified": raw.count("DOOM_ZONE verified"),
        "errors": len(errors),
        "frames": len(frames),
        "latest_frame": int(frames[-1]) if frames else 0,
        "performance": {
            "fps": round(last_perf[0] * 1000 / last_perf[1], 2) if last_perf[1] else None,
            "total_ms": round(last_perf[1] / last_perf[0]),
            "convert_ms": round(last_perf[2] / last_perf[0]),
            "display_ms": round(last_perf[3] / last_perf[0]),
            "other_ms": round(last_perf[4] / last_perf[0]),
        } if last_perf and last_perf[0] else None,
        "phases": {
            "logic_ms": round(last_phase[1] / last_phase[0]),
            "draw_ms": round(last_phase[2] / last_phase[0]),
        } if last_phase and last_phase[0] else None,
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
        "diagnostic": {
            "seconds": int(last_diagnostic[0]) if last_diagnostic else None,
            "key": last_diagnostic[1] if last_diagnostic else None,
            "allocated": int(last_diagnostic[2]) if last_diagnostic else None,
            "critical": int(last_diagnostic[3]) if last_diagnostic else None,
            "last_stage": diagnostic_stages[-1] if diagnostic_stages else None,
        } if last_diagnostic or diagnostic_stages else None,
        "exits": raw.count("DOOM_EXIT"),
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
