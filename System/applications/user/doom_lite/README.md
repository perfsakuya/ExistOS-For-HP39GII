# E1M1 Lite

## Current Game: E1M1 and E1M2 candidate

The native `Game` now uses the standard Freedoom 0.13.0 E1M1/E1M2 maps,
immutable BSP/sparse line tables, and bounded actor/sector-mover pools. It
supports three key colors, moving doors/platforms/floors, pickups, armor,
secrets, damage floors, simplified monster AI and animation, level transition
and death retry. Only the pistol is playable; this is not a demo-compatible
replacement for the complete Doom engine.

F3 selects three scene contrast levels. F2 advances from a completed E1M1
to E1M2, retries after death, or replays a completed E1M2. The status bar
shows real armor/ammo inventory and B/R/Y key ownership. Other controls
retain the previous Game mappings. `Lite` stays as its independent baseline.

Run `python tools/doom/run_game_checks.py --cc <native-gcc>` from the repo
root for map/resource reproduction, logic, public-API walking routes, exact
LOS/ray reference comparisons, framebuffer guards and host benchmarks.
See [the candidate report](../../../../docs/doom-game-e1m1-e1m2.md)
for scope, profiling fields, current measurements and hardware test gaps.
The historical hardware FPS below refers to earlier single-map images;
it does not establish the FPS of this two-map candidate.

## Previous E1M1 Game MVP and hardware results

The `Game` application adds a deliberately simplified, E1M1-specific game
simulation to the Lite renderer. `build_lite_game_data.py` extracts medium
single-player THINGS, special lines, and door sectors from the converted
Freedoom E1M1 WAD. `build_lite_portals.py` adds exact WAD segments around
doors so opening one changes both collision and ray casting. These generated
tables can be checked against the WAD hash with the `--check` generators.

The MVP has 35 Hz movement and collisions, medium-skill enemy placements,
health and ammo, a basic pistol and enemy damage, pickups including the blue
key, door interaction, a map, and the original E1M1 exit switch. Four enemy
types and the blue key use one static Freedoom patch each, packed into 4,840
read-only bytes with transparency and 15 grayscale levels. These patches are
read directly from flash; no decoded sprite heap is allocated. AI, sound,
animated sectors, original wall textures, world-sprite animation, and
demo-compatible Doom rules are outside this MVP. The
legacy full Doom apps remain in source but are excluded from the default
firmware build with `SKYOS_BUILD_LEGACY_DOOM=OFF`; they have a known exit
freeze risk on this device. `Lite` remains available as a renderer baseline.

Controls: arrows move and turn, F1 shoots, F2 uses a door or switch, F5
toggles the map, and F6 or ON returns to the application list. The map
uses 10-pixel-tall status text. In the map, F4 switches between the default
2x view and full-level overview, with larger key and exit markers. Turning
advances 1.5 of 256 angle steps per 35 Hz tick (about 73.8 degrees/s), 25%
slower than the previous build, without changing walking speed. The display
uses the UI framebuffer and its proven queue barrier without a Doom swap
zone. Every 32 frames, and for a final partial batch on exit, `DOOMG_PERF`
records elapsed time, 35 Hz logic ticks, logic/render/LCD totals and maxima
in both microseconds and milliseconds, frame intervals, dropped ticks,
position, health, ammo, kills, key, and map state. Scene/map, sprite, and HUD
totals and maxima are also recorded in microseconds to measure each visual
change independently. `DOOMG_BOOT`,
`DOOMG_EVENT`, and `DOOMG_EXIT` mark startup, actions, and each exit stage.
`tools/doom/serial_dashboard.py` parses these records.
While Game runs, the System PrintTask emits `DOOMG_MEM` every 10 seconds
without collecting the full task table. Its byte fields are `a` (malloc
allocated/current capacity), `z` (ZRAM pool allocated/capacity), and `s`/`w`
(SRAM/Swap heap arena extent). The arena extents can stay high after frees and
must not be read as live allocation.
The Game-only F5 map marks closed doors as dark rectangles, open doors as
light rectangles with dark ends, the uncollected blue key as a diamond, and
the original exit switch as an X. A short line shows the player's heading.
The separate Lite map retains its original appearance.
In the 3D view, closed doors use a light panel with a dark frame and centre
seam so they are distinguishable from masonry. F2 still opens them instantly;
animated door height is not implemented.

### Classic status bar and pistol

The next image adds a 26-row bottom bar with the Freedoom `STBAR`, tall
numbers, health-dependent face, and blue-key icon. The 3D viewport is
256x101, with its horizon and world sprites centred within that view. The
map keeps its readable top row and existing full-height overview instead
of squeezing its objectives underneath the bottom bar. The bar shows ammo,
health, kills, and the key; armor is zero because the MVP has no armor
inventory. Only bullet ammo is supported; other ammo counters are dashes.
The original small label strip is rebuilt with Freedoom's menu lettering,
using light backgrounds and dark lettering. Counter shadows and digit
holes are lightened separately so the numbers remain distinguishable.

`build_lite_ui.py` pre-sizes 33 UI/weapon patches into 8,622 packed flash
bytes. There is no decoded image heap or second framebuffer. The ARM
linker's static BSS remains 29,616 bytes. Pistol A/B/C and its muzzle flash
use the WAD patch offsets and the classic 32-unit weapon-ready position.
Successful shots trigger B6/C4/B5 recoil (15 game tics) and a seven-tic
flash. These are visual timers at 35 Hz; the MVP retains its immediate
hits and one shot per F1 press. Empty ammo causes no flash. The weapon is
hidden on the map, at death, and after completion. New sessions reset it.
Weapon bob, firing sounds, and other weapons are not implemented.

`DOOMG_PERF` additionally records `weapon_total_us`, `weapon_max_us`, and
`pistol_tics`; the dashboard displays weapon/flash mean and peak times.
Run `build_lite_ui.py --check`, `test_lite_ui.py`, and `check_lite_ui.c`
alongside the geometry/gameplay checks before packaging this image.
Host checks and the ARM build pass. The user confirmed that the bottom
labels/numbers are clear and the pistol animation looks normal. Three
physical runs recorded 559/52/912 frames over 23.139/2.115/37.017 seconds
(24.16/24.59/24.64 fps), zero dropped logic ticks, six actual shots with
`pistol_tics=15`, and three complete exits. In the first run, scene, weapon,
HUD, and LCD averaged 14.908/0.100/6.043/12.972 ms per frame. The longest
observed interval was 123 ms; this is an average frame-rate measurement,
not a per-frame 100 ms guarantee. The serial log is
`freedoom-game-classic-ui-hardware.log`. Game heap samples remained
69,320 bytes allocated out of 282,624, matching the previous Game build.

Host geometry and gameplay checks pass, and the ARM System image builds
within the ROM partition. On the calculator, the user confirmed the scene,
HUD, movement, F5 map and repeated F6 exits with responsive list keys.
The first image recorded 899/1381/1874 frames over 34.4/52.7/71.9 seconds
(about 26 fps per run), zero dropped logic ticks, and complete task exit
records. The door/map/input-fix image then recorded 1,939 frames in 76.043
seconds (25.5 fps), zero dropped ticks, one opened door, five kills, enemy
damage, a correctly blocked blue door, and a complete exit. Ammo remained
50 during initial movement before deliberate shots. The user confirmed
normal scenes and exit. The enlarged HUD/map, slower turning, and packed
sprite image then recorded 2,829 frames in 115.084 seconds (24.58 fps), zero
dropped logic ticks, seven kills, a blue-key pickup, and all five exit
stages. The user confirmed that the HUD and map are readable and that the
blue key was obtained. Across batches labelled as 3D, scene, sprites, HUD,
and LCD submission averaged 19.775, 0.075, 0.757, and 12.775 ms per frame.
The longest observed interval was 126 ms, so these results establish an
average above 10 fps, not a guarantee that every frame meets 100 ms. The
per-phase records are available in `freedoom-game-sprites-large-ui-hardware.log`.
Blue-door unlocking, level completion, and game-over/reset still need
hardware coverage. Mode-switch batches can contain both map and 3D frames.

This is a small, native E1M1 walk-through built for a 10+ fps target on the
HP 39gII. The wall layout is generated from the freely licensed Freedoom
E1M1 `LINEDEFS` in the converted WAD by
`tools/doom/build_lite_grid.py`. One-sided and blocking lines become a
126 × 110 grid with 32-Doom-unit cells. The grid occupies 13,860 bytes in
the System image and is validated against the player start and reachable
area when generated.

The current renderer casts 128 fixed-point rays, paints two LCD columns per ray,
and draws directly into the UI's 256 × 127 grayscale buffer. It allocates no
game zone or separate framebuffer. The initial hardware build averaged
12.5 fps at an 80 ms target, with longer frames when the System task printed
its detailed 10-second status dump. The 50 ms build pauses that dump while
Lite runs. On physical hardware, its first 352 measured frames took 17,566 ms
(20.04 fps); the longest measured frame interval was 54 ms. `DOOMLITE_PERF`
reports elapsed time, render time, display submission time, and the longest
interval for each 32-frame group. This measures Lite's stationary view, not
the full Freedoom port or all possible camera positions. The next build
replaced eight-unit ray marching with grid-boundary traversal and used a
40 ms cadence. In its first physical run, 15 complete batches covered 480
frames in 19,197 ms: 25.00 fps, with a 62 ms longest interval. Average
rendering took 0.94 ms and LCD submission 12.07 ms per frame. The user
confirmed its moving scene, map, and F6 exit appeared normal. The production
ray caster was also checked on a host against a one-unit reference march:
3,072 rays from 12 open E1M1 cells had no late wall intersections. The next
candidate doubles angular detail to 128 rays, interpolates the sine table at
four times its original resolution, writes framebuffer pixels in screen-row
order, and targets a 33 ms frame period. On hardware it completed 736 frames
in 24,374 ms (30.20 fps), with 8.18 ms spent rendering and 12.86 ms submitting
the LCD frame on average; the longest observed interval was 48 ms. The user's
photo revealed severe vertical stripes. An offline preview reproduced them,
and per-ray traces identified a signed/unsigned multiplication in sine
interpolation: a small negative difference became a direction component near
one billion. The corrected build renders continuous walls in the offline
preview and on the calculator. The user confirmed movement, map, F6 exit,
and application-list keys work. In its first physical run, 38 complete
batches covered 1,216 frames in 40,378 ms: 30.12 fps, with an observed 47 ms
longest frame interval. Average render and LCD submission took 8.49 and
12.79 ms per frame. The host check covers 1,024 fine angles against `sin()`,
12,288 rays and 96 rendered views from 12 open cells, including framebuffer
guards. The ARM image fits the System partition.

Left/right turn, up/down walk, F5 toggles the map, and F6 or ON exits.
The approximation omits sector heights, textures, sprites, doors, enemies,
weapons, sound, and Doom game logic. It is intentionally separate from the
full `Freedoom E1M1` app so neither mode is mistaken for the other.

## Performance budget for further gameplay work

The measured stationary Lite batches spent about 1–2 ms per frame in the
renderer and about 12 ms submitting the LCD frame. A 10 fps floor gives a
100 ms frame budget, but new features should target less than 80 ms in their
worst measured views to leave room for input, UI, and flash activity. The
full GBADoom path measured about 993 ms per frame in one 16-frame E1M1 run:
about 443 ms in game logic and 612 ms in BSP/wall drawing (the counters cover
overlapping phases). Therefore optimizing LCD submission or removing texture
sampling alone cannot reach 10 fps. Extend Lite in small measured steps,
starting with doors and low-cost sprites, while keeping the full engine as a
compatibility and visual reference. These estimates come from stationary
hardware views, so moving through the map still needs measurement.
