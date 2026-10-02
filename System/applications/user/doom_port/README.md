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
tries 160, 128, then 112 KiB for its zone. All allocations and fatal errors
are reported on the USB serial console. The exit path frees the zone and
conversion buffer and resumes the System UI.

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
- The native image and enlarged loader compile with GNU Arm Embedded
  Toolchain 10.3-2021.10. These are build checks, not gameplay confirmation.

The application needs a separate hardware launch test after flashing. If
`DOOM_ERROR` appears or the device panics, collect the serial log and screen
photo. The known-good SkyOS loader and System backups remain in the research
workspace for recovery.
