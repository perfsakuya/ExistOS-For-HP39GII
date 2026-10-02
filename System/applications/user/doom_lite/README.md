# E1M1 Lite

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
