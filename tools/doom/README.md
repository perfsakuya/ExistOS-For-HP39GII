# Rebuilding the embedded E1M1 asset

## Community ports and expansion audit

See [the source comparison and expansion plan](../../docs/doom-community-expansion.md)
for the pinned nRF52840/MG24 sources, 39gII paging constraints, animation payloads
and map limits. `audit_expansion.py` reads a standard (unconverted) Freedoom IWAD
and reports all maps and four enemy sprite families without changing firmware:

```powershell
python tools/doom/audit_expansion.py ../Freedoom-research/extracted/freedoom-0.13.0/freedoom1.wad --output docs/doom-expansion-audit.json
python -m unittest discover -s tools/doom -p test_audit_expansion.py -v
```

The checked-in JSON is a reproducible resource audit, not a playability or
performance result. Proposed mutable-state budgets exclude OS and renderer costs.

## Rebuild the converted asset

1. Obtain Freedoom release 0.13.0 from
   <https://github.com/freedoom/freedoom/releases/tag/v0.13.0> and verify
   the official release checksum before extraction.
2. Run `python trim_e1m1.py freedoom1.wad freedoom-e1m1.wad`.
3. Run upstream GBADoom `GbaWadUtil.exe -in freedoom-e1m1.wad -cfile
   freedoom-e1m1-gba.c`.
4. Run `python c_array_to_bin.py freedoom-e1m1-gba.c
   freedoom-e1m1-gba.wad` and place the binary in
   `System/applications/user/doom_port/data/`.

The checked-in converted WAD is 7,025,444 bytes, SHA-256
`88403318CB328E42135BBE81607234F5CB781D6D95FB17A3399FD3084EC6943E`.
Retain Freedoom's `COPYING.txt` and `CREDITS.txt` with any redistributed
output. The intermediate C array is large and should not be checked in.

## Compact E1M1 Game tables

The default firmware builds `Lite` and the simplified `Game` application.
It does not embed the full converted WAD or link the GBADoom engine; use
`SKYOS_BUILD_LEGACY_DOOM=ON` only to reproduce the old E1M1/Flat/Hybrid
experiments. The compact map, gameplay and portal headers are generated from
the checked-in converted WAD. After changing it, regenerate with
`build_lite_grid.py`, `build_lite_game_data.py`, and `build_lite_portals.py`.
Before packaging a test image, run the last two scripts with `--check` and
run `test_lite_game_data.py`, `test_lite_portals.py`, `check_lite_game.c`,
`check_lite_exit.c`, and `check_lite_rays.c` on the host. See the Lite README
for game controls, scope, and the `DOOMG_PERF` log fields.

`build_lite_sprites.py` extracts four enemy front frames and the blue key
from that verified WAD into `E1M1Sprites.h`. Run it with `--check` and run
`test_lite_sprites.py` before packaging images that use these assets. They
retain the Freedoom `COPYING.txt` and `CREDITS.txt` attribution; only 4,840
packed grayscale bytes are linked instead of the full WAD.

`build_lite_ui.py` also reads that WAD to generate `E1M1Ui.h`: a pre-sized
status bar, digits, faces, key icon, three pistol frames, and muzzle flash.
The bar lettering is rebuilt from Freedoom's small menu font with stronger
LCD contrast. Run it with `--check`, `test_lite_ui.py`, and the portable
`check_lite_ui.c` composition/animation check. UI data occupies 8,622 flash
bytes without allocating a decoded framebuffer or image cache.

## Live serial debugging

The calculator exposes a 9600-baud USB serial port while System is running.
Start the capture and dashboard in separate PowerShell terminals from the
repository root:

```powershell
pwsh -File tools/doom/capture_serial.ps1 -PortName COM6 -LogPath .\doom-serial.log
python tools/doom/serial_dashboard.py --log .\doom-serial.log --serial-name COM6
```

Open <http://127.0.0.1:8765/> to watch System memory, ZRAM, game logic,
render and LCD timing, frame progress, errors, and the raw log. The dashboard
only reads the file and listens on localhost. Stop the serial capture before
running `edb`, because both need the device's USB interface.
