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


def state():
    raw = LOG.read_text(encoding="ascii", errors="replace") if LOG.exists() else ""
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
    last_exit = raw.rfind("DOOM_EXIT")
    last_zone = raw.rfind("DOOM_ZONE backing")
    last_verified = raw.rfind("DOOM_ZONE verified")
    last_status = raw.rfind("=============SYSTEM STATUS")
    latest_task_list = raw[last_status:] if last_status >= 0 else ""
    doom_task_visible = bool(re.search(r"(?m)^Doom\s+[XRBSD]\s+", latest_task_list))
    if last_start < 0:
        stage = "等待启动"
    elif last_exit > last_start:
        stage = "已退出"
    elif last_status > last_start and not doom_task_visible:
        stage = "应用界面"
    elif last_verified > last_start:
        stage = "引擎运行中"
    elif last_zone > last_start:
        stage = "交换区校验中"
    else:
        stage = "启动中"
    age = time.time() - LOG.stat().st_mtime if LOG.exists() else None
    return {
        "connected": age is not None and age < 25,
        "serial_name": SERIAL_NAME,
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
            data = LOG.read_bytes() if LOG.exists() else b""
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
    args = parser.parse_args()
    LOG = args.log.resolve()
    SERIAL_NAME = args.serial_name
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"SERIAL_DASHBOARD http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()
