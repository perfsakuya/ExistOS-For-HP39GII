# Ray: native sphere and studio demo

## First test version

`Ray` is a separate SkyOS application. Its fixed scene has three spheres, a
checkerboard floor, a gradient sky, one directional light, hard shadows and
one mirror reflection. It uses independent C implementations of analytic
sphere/plane intersections and polygon preview drawing; no scene files or
full image of floating-point colors are allocated at runtime.
The three sphere radii are 0.90, 1.08 and 0.78 scene units, enlarged by 20%
from the initial preview. Their centers keep each sphere tangent to the floor
and separated from the others; tracing, collision and preview share the scene.

| Key | Action |
|---|---|
| Up / Down | Move forward / backward |
| Left / Right | Turn |
| F1 | Reset the camera |
| F3 | Cycle C1 / C2 / C3 contrast |
| F6 / ON | Return to the application list |

The camera cannot pass through the spheres and is constrained to the small
scene area. While a movement key is held, only clipped polygons and scanlines
are drawn. The preview does not cast primary, reflection or shadow rays.
The observed key release starts a 1000 ms quiet period; camera reset or contrast
changes also restart the deadline. Moving again cancels the old generation.

The tracer refines an existing preview through nested pixel samples:

| Pass | Display block | Newly traced pixels | Cumulative pixels |
|---|---|---:|---:|
| 1 | 4 x 4 | 2,048 | 2,048 |
| 2 | 2 x 2 | 6,144 | 8,192 |
| 3 | 1 x 1 | 24,320 | 32,512 |

Pixels are sampled at their centers. Each pixel is traced only once across all
passes; the final image equals direct 256 x 127 pixel rendering. The final
row is clipped to the screen's 127-pixel height. Distant floor checks fade to
their average gray at grazing angles to reduce moire; this is a small display
filter, not complete antialiasing or ray differentials.

## Arithmetic, memory and display

The first implementation uses single-precision software floating point and
bounded iterative reflection. Scene, preview and session sources use `-O2`
without fast-math; the rest of System keeps its existing compiler options.
The light and sphere radius inverses are constants, specular exponent 16 uses
four squarings, and shadow traversal stops at the first blocker. Only mirror
material generates a secondary reflection, with no refraction branches.

## Studio V2 candidate

The next candidate uses the same three enlarged spheres, camera controls,
framebuffer and progressive sampling schedule. Materials are glass on the
left, chrome in the center, and patterned porcelain on the right. A procedural
studio environment supplies bright panels for chrome reflections. The floor
has a restrained reflection, and two fixed light samples approximate softer
shadows. This is not a fully sampled area light.

Glass uses Snell refraction at entry and exit, with a bounded analytic sphere
exit rather than a recursive scene traversal. View-dependent Fresnel blending
combines transmitted and reflected luminance. Secondary rays terminate at
direct surface shading; nested mirrors, internal multi-bounce glass, caustics
and stochastic global illumination are outside this candidate.

The moving preview approximates glass with face-level alpha blending, chrome
with an environment lookup, and porcelain with broad pattern bands. It still
does not cast rays or allocate a depth buffer. Ground contact shadows in the
preview are translucent polygons, distinct from traced shadows after stopping.

V2 adds refraction ray counts and inclusive refraction timing to the logs and
panel. Glass exits, total internal reflection events and floor reflection rays
are also recorded. `RAY_EVENT` now includes the device millisecond timestamp
so physical wait/start timing can be checked. The first tested V1 results below
remain a separate baseline; V2 hardware performance and visual quality are
pending a new device test.

The signed V2 candidate is 5,945,252 bytes (7,912 bytes above V1). Its ELF
contains 5,935,799 bytes of text, 4,948 bytes of data and 86,256 bytes of BSS.
All six host stages passed, including Snell/critical-angle/TIR tests, unchanged
pixel coverage, preview with zero ray queries, and 16 dashboard tests. The
largest individual ARM Ray function reports 472 stack bytes, excluding its
callees; the task stack remains 8 KiB and peak usage is not measured.

A same-host renderer comparison uses 50 frames at each of three identical
camera poses with counters and detailed clocks disabled. V2 costs 1.37 to
2.06 times V1 in this test. These ratios describe the host only and do not
predict ARM elapsed time. Evidence and previews are in
`build/ray-studio-validation/`, including `host-comparison.json`.

The app borrows the existing 32,512-byte UI framebuffer. Its renderers do not
allocate a second framebuffer, floating-point accumulation image, full-screen
depth buffer or decoded resource heap. The Ray task still requires its own
8 KiB stack and task control block; `renderer_heap_bytes=0` is not a claim
that starting the task needs zero heap.

The Loader display queue borrows buffer pointers asynchronously. Before the
first preview write, after each submitted full frame, and before returning to
UI, the app drains the full-frame read using the existing four-entry-queue
barrier sequence. The borrowed buffer is never freed by Ray. Exit waits up to
one second for the exit key to be released, restores UI, and deletes the task.

Trace batches allow at most 512 new pixels and check a 6000 us time budget
after each eight pixels. The last group can exceed the budget; this is not a
hard real-time deadline. The task yields between batches. Progress frames are
submitted at roughly 100 ms intervals, at pass boundaries and on completion.
Moving preview refreshes at no more than one frame per 60 ms; actual frame
rate depends on calculation, display, paging and scheduling.

## Logs and dashboard

- `RAY_BOOT`: dimensions, session size, borrowed framebuffer, task stack,
  render allocation budget, clock reading and initial display drain.
- `RAY_EVENT`: generation, preview/wait/trace/done, cancellation, contrast,
  camera reset and current sample count.
- `RAY_PERF`: cumulative trace/preview/LCD times, maximum batch duration,
  sample count, ray/test counts and camera/intersection/shadow/shading/reflection
  timings for the current generation.
- `RAY_MEM`: whole-System heap and ZRAM observations, using the same definitions
  as Game memory reports.
- `RAY_EXIT`: key, key release, UI resume and task deletion.

Timing categories are inclusive: shading includes its shadow time; reflection
includes its secondary intersection/shading/shadow work. They must not be
summed to infer total execution time. `trace_us` records the batch wall time,
and `lcd_us` records the display submission/barrier separately. The current
FreeRTOS build does not expose the stack high-water API; `stack_words=-1`
means unknown. Static `.su` output is single-function evidence, not measured
task peak. The dashboard accepts Ray and older Game logs.
Once a generation completes, the dashboard preserves its first completed
PERF duration; later viewing heartbeats remain in the raw log without extending
the displayed render duration.

## Verification

```powershell
python tools/ray/run_checks.py --gcc D:/w64devkit/bin/gcc.exe
cmake --build build --target ExistOS.sys --parallel 6
```

The host checks cover deterministic output, frame guards, camera bounds and
collision, preview with zero tracing, key release / 999 ms / 1000 ms / timer
wrap, cancellation, bounded batches and exact progressive/direct equivalence
for all three contrast settings. Images and command/exit/time records are
saved under `build/ray-validation/`; host durations do not establish ARM
performance. Actual moving response, image readability, trace completion time,
restart and exit stability still require device testing.

The first signed ARM candidate is 5,937,340 bytes. Its ELF reports 5,927,880
bytes of text, 4,948 bytes of data and 86,240 bytes of BSS; it fits both the
14 MiB VM ROM window and the System partition. The six host verification
stages passed, including 13 dashboard tests. Static ARM stack output reports
464 bytes for the largest individual Ray function, excluding callees and
library routines. This is not a measurement of the task's peak stack usage.

### Hardware smoke test: 2026-10-04

Firmware `90d08b7` was flashed successfully, with eight matching transfer
checksum samples and no independent NAND readback. The panel backend `554932d`
passed 15 dashboard tests. Evidence is saved under
`hp-39-gii/outputs/ray-demo-2026-10-04/hardware/` beside the repository;
`ray-hardware-assessment.json` records the completed serial log hash and lines.

One Ray launch was recorded. The first two trace generations were cancelled
by movement; generation 29 completed all 32,512 primary samples. Its first
DONE PERF records:

| Observation | Time |
|---|---:|
| Trace start to complete frame, including yields and LCD | 6,412 ms |
| Trace batches, accumulated wall time | 3,471.666 ms |
| LCD submission and barriers, accumulated | 897.080 ms |
| Largest trace batch | 35.651 ms |

The quiet wait before tracing is excluded from the 6,412 ms. These timings
are for one moved view, not a scene-wide performance bound. The batch budget
is checked periodically and is not a strict 6 ms deadline. Reflection and
shadow ray counts were 5,216 and 21,769. Both recorded moving-preview PERF
packets had zero ray and intersection queries; wait packets also stayed at zero.

The user reported normal exit. All four exit stages were recorded, followed
by six System task lists with no RayDemo task. Allocated heap returned to
57 KiB, matching the pre-launch observation; the SRAM arena's expanded
71 KiB capacity remained reserved. No System Panic text was present.
F1 reset, C1/C3 contrast, repeated launches, precise physical quiet-wait timing,
visual quality and peak task stack remain unverified on hardware.

## References

The visual and algorithm research referenced
[tinyraytracer](https://github.com/ssloy/tinyraytracer),
[pbrt's Whitted integrator](https://github.com/mmp/pbrt-v3/blob/master/src/integrators/whitted.cpp)
and [smallpt](https://www.kevinbeason.com/smallpt/).
Ray implements its own small C scene and routines; it does not embed their
frameworks or stochastic path-tracing implementation.
