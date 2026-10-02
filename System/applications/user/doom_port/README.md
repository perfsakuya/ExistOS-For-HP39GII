# Freedoom E1M1 for SkyOS

This is a native, single-level Doom engine port for the HP 39gII. It uses the
GBADoom engine with a trimmed and converted Freedoom Phase 1 v0.13.0 IWAD.
The game data is free software; no id Software game WAD is included.

The 120 × 120 renderer is centered on the 256 × 127 grayscale LCD. Sound,
music, save games, and levels beyond E1M1 are currently disabled. The port
starts E1M1 directly. F1 fires, F2 uses, F3/F4 strafe, F5 opens the automap,
Enter opens the in-game menu, the direction pad moves/turns, and F6 or ON
returns to the application list.

`DoomPort.c` borrows the System UI framebuffer for the engine's 16-bit pixel
indices and allocates a 14,400-byte grayscale conversion buffer. The engine
uses a 256 KiB zone at the end of the loader's NAND-backed 3 MiB VM RAM map.
The port enables VM swap before accessing the zone, then writes and reads every
1 KiB page to check that eviction through the 40-page RAM cache preserves data.
The zone lies above the System heap with a 64 KiB guard checked at launch.
Swap remains enabled after exit because dirty pages can still be in the cache.
All allocation failures and fatal errors are reported on the USB serial
console. The exit path frees the conversion buffer and resumes the System UI.

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
- The swap-backed zone image compiles with GNU Arm Embedded Toolchain
  10.3-2021.10 and passed the System image size check. Physical gameplay
  validation is pending.

The application needs a separate hardware launch test after flashing. If
`DOOM_ERROR` appears or the device panics, collect the serial log and screen
photo. The known-good SkyOS loader and System backups remain in the research
workspace for recovery.
