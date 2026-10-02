# Freedoom E1M1 for SkyOS

This is a native, single-level Doom engine port for the HP 39gII. It uses the
GBADoom engine with a trimmed and converted Freedoom Phase 1 v0.13.0 IWAD.
The game data is free software; no id Software game WAD is included.

The engine's 120 × 120 16-bit backbuffer contains 240 × 120 palette bytes:
the 3D renderer duplicates horizontal pixels, while the automap and menus
use both bytes independently. The bridge shows all 240 × 120 pixels centered
on the 256 × 127 grayscale LCD; the unused margins are cleared at startup. Sound,
music, save games, and levels beyond E1M1 are currently disabled. The port
starts E1M1 directly. F1 fires, F2 uses, F3/F4 strafe, F5 opens the automap,
Enter opens the in-game menu, the direction pad moves/turns, and F6 or ON
returns to the application list.

`DoomPort.c` suspends the System UI and uses a private 28,800-byte engine
framebuffer with guards on both sides. This keeps game drawing away from UI
objects and detects out-of-bounds writes. It also allocates a 28,800-byte
grayscale conversion buffer. The engine
uses a 256 KiB zone at the end of the loader's NAND-backed 3 MiB VM RAM map.
The port enables VM swap before accessing the zone, then writes and reads every
1 KiB page to check that eviction through the 40-page RAM cache preserves data.
The zone lies above the System heap with a 64 KiB guard checked at launch.
Swap remains enabled after exit because dirty pages can still be in the cache.
All allocation failures and fatal errors are reported on the USB serial
console. F6/ON requests a return at a game-loop boundary. The current exit
path resumes the System UI, then releases the LCD buffer after 12 seconds,
the guarded game buffer after 24 seconds, and the Doom task after 36 seconds.
These delays are a tested workaround, not an identified root-cause fix.
The LCD palette is converted to a 256-entry grayscale lookup table only when
the game changes palettes. Serial output includes `DOOM_PERF` every 16 frames:
the wall time and time spent converting pixels and submitting LCD updates.
`DOOM_PHASE` separates game logic from drawing, which includes pixel conversion
and LCD submission. The remaining wall time includes engine work, VM swap,
and serial overhead.
`DOOM_RENDER` splits the 3D renderer into view setup, BSP/walls, planes, and
masked sprite passes. Each counter covers 16 rendered views.
The SkyOS automap uses a light background and dark line palette for the
monochrome LCD.

`Flat` is a profiling variant of the same E1M1 spawn. It keeps BSP
traversal, wall geometry, clipping, lighting, floors, and sprites but fills
walls with one lit palette color per texture ID. It skips wall texture cache
lookups, composition, and sampling. The serial log emits `DOOM_MODE
flat_walls=0/1` to identify each run. Compare `DOOM_RENDER` and `DOOM_PERF`
over at least 16 stationary frames in both apps; this variant is an upper
bound on wall texture savings, not an intended final visual design.

A controlled hardware A/B run over 16 stationary frames measured 798.5 ms
per view for normal BSP/walls versus 432.75 ms for flat walls. Complete
frame wall time improved from 1,280.6 to 929.1 ms per frame. Even removing
wall texture sampling cannot approach 10 fps in this engine configuration.
The separate `Lite` app tests a much smaller map renderer instead. Its
current 128-ray, 33 ms build measured 30.12 fps over 1,216 physical frames;
it omits the full engine's game logic and rendering features.

Both normal and flat modes have reproduced an F6 exit freeze. Two builds that
freed both buffers about one second after UI resume stopped producing serial
heartbeats, including one that kept the Doom task alive. A diagnostic build
that freed the LCD buffer at 12 seconds, the game buffer at 24 seconds, and
deleted the task at 36 seconds stayed responsive through all stages in one
hardware run. The current release image uses that ordering; repeated-launch
stability is still unverified. A later hardware run exited E1M1 while its
fire animation was active and froze on the game screen. Serial output reached
`DOOM_CLEANUP key_released=1 critical=0 resuming_ui` but not `ui_resumed`.
This remains an open exit defect; frame-rate work proceeds in the separate
Lite app. The System UI also had an eight-byte window
title allocation followed by a nine-byte copy, and submitted an out-of-range
LCD row during function-key redraw. These source defects were corrected in
the 40 ms Lite build, which boots and exits Lite on hardware; their
relationship to the full E1M1 F6 freeze is still unproven.

The WAD's `STBAR` is a compressed 320 × 32 patch (13,128 bytes). The old
status-bar code copied the lump directly into a 240 × 32 framebuffer tail
(7,680 bytes), overwriting 5,448 bytes beyond the frame. The SkyOS path now
decodes the patch, scales its columns to 240 pixels, and clips patch drawing
to the 120 screen rows. The frame guards remain active to catch other writes.

The embedded data file was produced from `freedoom1.wad` in the official
Freedoom 0.13.0 release using `tools/doom/trim_e1m1.py`, GBADoom's
`GbaWadUtil.exe`, and `tools/doom/c_array_to_bin.py`. See `data/COPYING.txt`
and `data/CREDITS.txt` for the data license and contributors. Engine provenance
is in `UPSTREAM.md`.

The System image is ~12.9 MB. This branch maps a 14 MB VM ROM region, so it
requires the matching OSLoader image. The System partition spans flash blocks
31 through 159 (16.125 MiB); the loader occupies block 22. Do not flash the
System image with the original 6 MB loader.

## Validation record

- The prior Doom Probe passed on physical hardware: directional changes and
  F6 return worked; a 128 KiB zone was allocated and touched successfully.
- The trimmed pre-conversion E1M1 IWAD ran through 7,117 game tics in
  Chocolate Doom's timedemo with rendering enabled on a desktop.
- The enlarged loader boots the System image on physical hardware.
- The first native image launched the Doom task but exited before its first
  frame: all three on-chip zone allocations failed. The serial log showed
  `EXT HEAP NOMEM` and `DOOM_ERROR Doom zone allocation failed`, with no Panic.
- The swap-backed zone image boots and runs E1M1 on physical hardware. The
  initial 120 × 120 build left the suspended application list visible around
  the game and ran below 2 fps. It emitted frame counters without a Panic.
- The palette lookup and screen-clear build compiles with GNU Arm Embedded
  Toolchain 10.3-2021.10 and passes the System image size check. Hardware
  performance measurements for this revision are pending.
- The wider display and automap were confirmed on hardware. A later F6 exit
  panicked because the status bar wrote beyond the framebuffer. The isolated
  framebuffer exposed the first changed guard byte (`109`), which exactly
  matches `STBAR[7680]` in the bundled WAD. Its launch Panic resolved to
  `free()`, after the 5,448-byte overwrite crossed the 4 KiB guard and damaged
  the adjacent allocation. The status-bar decoder and draw clipping compile.
  The replacement image ran 15 frames on the calculator and logged a complete
  F6 exit through `DOOM_CLEANUP ui_resumed`, with no guard error; the UI task
  resumed with available stack. The user confirmed the scene, automap, and
  return to the application list all appeared normal.

The application needs a separate hardware launch test after flashing. If
`DOOM_ERROR` appears or the device panics, collect the serial log and screen
photo. The known-good SkyOS loader and System backups remain in the research
workspace for recovery.
