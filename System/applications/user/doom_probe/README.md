# Doom Probe

This historical diagnostic app is no longer linked into the current System
image or shown in the application list. Its source and host smoke check remain
for reproducing the original hardware gate.

Native C hardware gate for a future GBADoom port. This small perspective
renderer is **not the Doom engine** and does not use Pocket Vapor.

The historical **Doom Probe** build performed these checks on the HP 39gII:

1. Borrow the persistent System UI framebuffer, allocate a 160 × 120 scene
   buffer, and attempt a 160 KiB game heap (falling back to 128 KiB).
   Write and verify one byte per 1 KiB page while both buffers are live.
2. Enqueue 16 identical full-screen 256 × 127 updates. The loader's four-entry
   display queue should saturate; the measured total is queue throughput,
   **not a per-frame LCD-completion timestamp**. Five one-pixel sentinel
   requests then drain the FIFO before the shared buffer changes.
3. Render 64 changing perspective frames off-screen with integer math.
   Measure total CPU time separately from the display queue.
4. Show one scene. Left/right rotate it; F6 or ON returns to the app list.
   Each scene request is followed by the same FIFO barrier so its source
   buffer is never changed or freed while the loader still uses it.

System CDC logs at 9600 baud use the prefix `DOOMPROBE_`. The gates before
integrating GBADoom are: the scene buffer and game heap both allocate,
the touched memory verifies, full-screen updates reach the panel, and the app
returns cleanly with its allocations released. These checks do not predict
GBADoom's frame rate or peak heap use.

The portable renderer has a host smoke check:

```powershell
gcc -std=c11 -O2 -I System/applications/user/doom_probe System/applications/user/doom_probe/DoomProbeRender.c System/applications/user/doom_probe/host_smoke.c -o build/doom_probe_smoke.exe
./build/doom_probe_smoke.exe build/doom_probe.pgm
```

Reference: [GBADoom](https://github.com/doomhack/GBADoom) at
`89097b3ff31ac1e1b2cdce9854e49726cfa462bf`. Its source notices
specify GPL v2 or later; game data is separate. The local research checkout is
outside this firmware repository at
`F:\workspace\39gii-reverse\GBADoom-research`.
