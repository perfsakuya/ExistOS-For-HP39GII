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
| 8 / 2 | Pitch up / down |
| 4 / 6 | Roll left / right |
| 9 / 7 | Ascend / descend |
| F1 | Reset the camera |
| F2 | Toggle bounded grayscale FXAA (default OFF on launch) |
| F3 | Cycle C1 / C2 / C3 contrast |
| F6 / ON | Return to the application list |

The camera cannot pass through the spheres and is constrained to the small
scene area. Pitch is limited to +/-80 degrees; yaw and roll wrap through a
complete turn. Translation follows horizontal yaw even when the view is tilted.
F1 restores the original position and orientation while preserving the AA
setting. While a movement or orientation key is held, only clipped polygons and scanlines
are drawn. The preview does not cast primary, reflection or shadow rays.
The observed key release starts a 1000 ms quiet period; camera reset or contrast
changes and the AA toggle also restart the deadline. Moving again cancels the
old generation, including its post-processing stage.

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

## Studio V2

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
remain a separate baseline; V2 hardware observations are recorded separately.

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

## Studio V3: orientation and optional edge antialiasing

The `2/8` keys look down/up and `4/6` bank left/right. Preview and tracing use
the same analytic orthonormal camera basis; no incremental vector rotation
drift accumulates. Looking down does not move the camera into the ground.
The preview's sky gradient follows roll using three environment samples per
row, with no visibility rays. Pitch is limited to +/-80 degrees.

F2 switches AA on a press edge, with OFF as the launch default. A preview-only
`AA ON` / `AA OFF` badge confirms the setting. All traced samples overwrite
that badge before post-processing begins, so it is absent from the final image.
Changing AA regenerates the frame, since only one framebuffer is retained.

The filter uses integer local contrast and Sobel edge classification. Diagonal
edge candidates keep 75% of their center luminance and mix 25% of the four
neighbor mean. Low-contrast regions and straight axis-aligned steps/lines are
preserved. This is a bounded grayscale edge approximation, with no contour
search or subpixel reconstruction. It softens some jagged contours; distant
checkerboard undersampling can remain. Design references are
[NVIDIA FXAA](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf)
and [Intel CMAA2](https://www.intel.com/content/www/us/en/developer/articles/technical/conservative-morphological-anti-aliasing-20.html).

AA runs only after all 32,512 primary pixels have been traced. Three cached
original rows allow deterministic in-place processing without reading back
already filtered pixels. The cache is 768 bytes; its complete static state is
792 bytes on ARM, with no added heap, full-screen buffer, ray or task. Batches
process at most eight rows, checking a 6,000 us budget after each row and
yielding to input between batches. Movement, settings and F6 can interrupt
this stage. As with tracing, the budget can be exceeded by the last work unit.

`RAY_POSTAA=4` is a distinct logged stage. DONE is emitted after AA and the
final LCD submission, so its first completed elapsed time includes both.
`aa_us` measures initialization and active filter batches, excluding caller
yields/LCD/tracing. `aa_batch_max_us`, `aa_rows`, `aa_pixels` and `aa_changed`
are separate from ray timing and counts. The panel shows the AA setting,
processed rows, changed pixels and yaw/pitch/roll in degrees.

All six host stages passed, including 17 dashboard tests. Checks cover extreme
orientation and invalid input, horizontal movement, release/idle and AA toggle
gates, cancellation of partial AA, frame guards, flat/axial detail preservation,
and byte-exact equality across whole-frame and 1/3/7/31-row filter batches.
An independently integrated synthetic diagonal's squared error changed from
520,192 to 168,192. This one pattern is not a general image-quality score.
The default scene changed 2,313 pixels and a tilted scene 3,165, each scanning
32,512 pixels without adding rays. Previews are in
`build/ray-controls-aa-validation/pose-aa-contact-sheet.png`.

The signed image is 5,949,676 bytes and passes the System/VM ROM limits. Its
ELF reports 5,940,221 bytes of text, 4,948 bytes of data and 87,056 bytes of BSS.
Actual compiler-option stack reports show a largest individual Ray function
of 472 bytes and AA batch function of 96 bytes, excluding callees. The task
still has 8 KiB; its peak is unmeasured. V3 hardware observations are recorded
separately below.

## Studio V4: vertical movement and bounded grayscale FXAA

The `9/7` keys raise/lower the camera at the same rate as horizontal movement.
Height is constrained to 0.20--8.0 scene units. Collision uses the closest point
on the full three-dimensional movement segment against spheres inflated by the
camera radius of 0.18. This allows flying above a sphere without allowing a
large movement step to tunnel through it. Forward/backward translation remains
horizontal and independent of pitch and roll. F1 restores height 1.45.
Holding a blocked or limited movement key still prevents tracing; the observed
release restarts the full one-second quiet period.

V3's diagonal-neighbor blend is replaced with a CPU grayscale adaptation of
[NVIDIA FXAA](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf).
It is a bounded adaptation, not the original GPU shader or a conformant GPU
FXAA preset. It works on the final contrast-mapped 8-bit image, adding no rays.
Local contrast rejection skips flat regions; weighted second differences classify
edge orientation, and the strongest contrast pair defines the perpendicular
sample direction. Edge ends are searched up to four pixels in each direction.
Fixed-point resampling applies the resulting subpixel offset, with capped
subpixel blending and additional protection for thin lines and coherent axial
edges. Bounded search can miss longer edge structure; image-space filtering
cannot reconstruct details that were already lost to undersampling.

Eleven cached original rows (2,816 bytes) cover the search and bilinear sampling
footprint. The filter is deterministic across batch sizes and never reads back
filtered output as source. It retains the eight-row / 6,000 us soft batch budget
and input cancellation. Separate `aa_edges` and `aa_search_steps` counters
record classified candidates and actual endpoint sampling, alongside `aa_us`,
changed pixels and maximum batch time. V4 hardware cost is unmeasured.

Traditional MSAA is coverage sampling for rasterized primitives; four complete
ray samples per pixel here would be supersampling and require substantially
more tracing. Neither multisample ray tracing nor a GPU pipeline is introduced.
See section 3.2.1 of the [Khronos OpenGL specification](https://registry.khronos.org/OpenGL/specs/gl/glspec21.pdf).

The new logs include camera `y_q8` (height multiplied by 256), along with
position X/Z, yaw/pitch/roll, and `idle_base_ms` for checking the quiet deadline
without treating delayed serial-print timestamps as input timestamps. The
dashboard source displays height and the boot's AA method; its running backend
must load the new source to expose these fields. Earlier V3/V2 measurements
below remain measurements of their original images and algorithms.

Offline validation passed all six stages, including 17 dashboard tests. The
first dashboard attempt caught a field-name parser bug excluding numeric
characters; its failed evidence is retained, and only dashboard/HTML/image
stages were resumed after the parser correction. Elevation, three-dimensional
sweeps, finite input, held limits, release deadlines, clock wrap, twelve extreme
height/orientation previews, frame guards and unchanged sample coverage passed.
AA also preserves flat/linear ramps and coherent axial lines, keeps outer
borders exact, and produces the same bytes/counters across full-frame and
1/3/7/31-row batches with no new ray calls. Two independently integrated
synthetic diagonals changed squared error from 520,192 to 10,660 and from
736,596 to 141,456; these are local evidence, not general image-quality scores.
Previews are in `build/ray-fxaa-flight-validation/`.

The signed candidate is 5,952,228 bytes and passes System/VM ROM limits. Its
ELF has 5,942,770 bytes of text, 4,948 bytes of data and 89,120 bytes of BSS.
The static post state is 2,848 bytes on ARM. Actual production compiler flags
report 208 bytes for the AA batch function and 16 for its Q8 sampler, excluding
other callees; the largest individual Ray function remains 472 bytes. The
8 KiB task stack peak, vertical controls and V4 physical exit are pending
hardware measurement. V4 AA device observations are recorded below.

## Logs and dashboard

- `RAY_BOOT`: dimensions, session size, borrowed framebuffer, task stack,
  render allocation budget, clock reading and initial display drain.
- `RAY_EVENT`: generation, preview/wait/trace/done, cancellation, contrast,
  camera reset, orientation in milliradians, AA toggle/start/done, and sample count.
- `RAY_PERF`: cumulative trace/preview/LCD times, maximum batch duration,
  sample count, ray/test counts and camera/intersection/shadow/shading/reflection
  timings for the current generation. AA has separate processing time, scanned
  and changed pixel counts, processed rows, and maximum batch duration.
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

### V1 hardware smoke test: 2026-10-04

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

### Studio V2 hardware smoke test: 2026-10-04

Firmware `304f251` was flashed successfully with eight matching transfer
checksum samples and no independent NAND readback. The user reported normal
scene appearance and operation in response to the scene, movement, progressive
render and exit test. Evidence is saved under
`hp-39-gii/outputs/ray-studio-2026-10-04/hardware/` beside the repository.
`ray-studio-hardware-assessment.json` records the frozen raw log hash, capture
segments, exact packet lines and verification limits.

Three complete moved views were captured at C2 contrast. Each completed all
32,512 primary samples; these are the first completed PERF packets after their
corresponding done events, excluding later viewing heartbeats:

| Capture / generation | Complete frame | Trace batches | LCD and barriers | Refraction rays / glass exits | Floor reflection rays |
|---|---:|---:|---:|---:|---:|
| 1 / 13 | 10,129 ms | 5,632.202 ms | 1,440.450 ms | 7,822 / 3,911 | 8,813 |
| 2 / 75 | 10,385 ms | 5,515.370 ms | 1,773.319 ms | 6,616 / 3,308 | 9,286 |
| 2 / 79 | 10,052 ms | 5,530.633 ms | 1,419.593 ms | 6,922 / 3,461 | 9,223 |

The quiet wait is excluded from complete-frame duration. Paired ready/wait and
start events were 1,004, 1,005, 1,008, 1,000 and 1,005 ms apart. All recorded
moving and waiting PERF packets had zero tracing, ray and intersection counts.
The largest batch in the completed views was 17.389 ms; the 6 ms checked budget
is not a hard deadline. No total-internal-reflection events occurred in these
views; the host suite covers that path. Phase timings overlap and must not be
summed. These views do not establish a scene-wide bound or a controlled speed
comparison with V1.

Ray memory reports observed 69,320 allocated heap bytes out of 282,624 and a
73,056-byte SRAM arena with zero swap. These are whole-System observations,
not Ray-exclusive allocations or a measured task-stack peak.

The two bounded serial captures have a gap. The later session's BOOT was not
captured, and generation numbers are scoped to each capture. No RAY_EXIT or
post-exit task/heap telemetry was captured: V2 exit is user-reported, while V1
has separate serial cleanup proof above. No System Panic text appeared in the
captured portions. F1 reset, C1/C3, repeated launch/exit stability and task peak
stack remain unverified on hardware.

### Studio V3 hardware smoke test: 2026-10-04

Firmware `33f077c` was flashed with eight matching transfer checksum samples;
there was no independent NAND readback. The user reported all normal for
orientation, the AA badge/image, exit and application-list response. Evidence
is under `hp-39-gii/outputs/ray-controls-aa-2026-10-04/hardware/`;
`ray-controls-aa-hardware-assessment.json` records the completed capture hash.

One tilted C2 AA-enabled frame completed in 11,630 ms, including tracing,
post-processing, yields and LCD. Trace batches accumulated 6,203.538 ms and
LCD/barriers 1,564.802 ms. AA active work took 18.906 ms, scanning 32,512 pixels
and changing 3,149, with a largest AA batch of 5.395 ms. AA start to AA done
events spanned 213 ms; final done after LCD was 262 ms after AA start. These
wall intervals include scheduling and log/display overhead, unlike `aa_us`.

The recorded AA intermediate packet (88 rows) and completion (127 rows) had
identical primary/reflection/refraction/shadow/test counts and `trace_us`.
All thirteen preview/wait packets had zero ray and AA work. Three AA toggles
cancelled active traces; pitch and roll changed in both directions. AA OFF
traces were interrupted, so no completed matching OFF/ON speed comparison is
available. The complete frame's primary count remained 32,512.

Initial ready/start and a movement-release wait/start were 1,005 and 1,008 ms
apart. Setting-toggle wait timestamps follow synchronous diagnostic output;
their shorter printed intervals are not the controller's timing origin and
cannot independently verify the one-second setting delay.

No Panic or RAY_EXIT text was captured. Exit is user-reported normal; serial
task removal/heap return, repeated launches, F1 reset, C1/C3, hardware angle
limits/full roll and peak stack remain unverified in this test. The dashboard
source supports the new metrics, but automatic approval blocked restarting
its running backend with `blocked by policy`; new raw telemetry remains in
the separate V3 log. The existing dashboard service was preserved.

## References

The visual and algorithm research referenced
[tinyraytracer](https://github.com/ssloy/tinyraytracer),
[pbrt's Whitted integrator](https://github.com/mmp/pbrt-v3/blob/master/src/integrators/whitted.cpp)
and [smallpt](https://www.kevinbeason.com/smallpt/).
Ray implements its own small C scene and routines; it does not embed their
frameworks or stochastic path-tracing implementation.


## Studio V4 hardware capture: FXAA, 2026-10-04

Source `0ab335b59a627c0334f86b5bdab8bff02c012ea6` was written to System
using the hash-checked RAM recovery loader. The flasher exited 0 and all eight
reported transfer checksums matched; this is not an independent NAND readback.
The signed image SHA-256 is
`FCD438ACE5A6527AF9F5A67FAB50EAC364826AB227254ECB5E22BF2A583616B9`.
COM6 reconnected and a bounded five-minute capture closed with exit 0.

The capture contains three complete frames with unchanged camera and C2
contrast: two AA ON and one AA OFF. Their first DONE PERF durations were
12,906 / 13,809 / 13,389 ms. AA active work took 37,660 / 35,100 / 0 us;
maximum AA batches were 4,945 / 3,999 / 0 us. ON-stage wall times were 206
and 210 ms, including caller scheduling/yields and serial event timing.
Whole-frame elapsed differences include tracing/LCD/scheduling variation and
must not be attributed solely to AA.

Each frame has 32,512 primary samples, 18,306 reflection rays, 78,405 shadow
rays and 8,856 refraction rays. All recorded ray and intersection/test counts
are identical across the OFF/ON comparison. Both ON frames scanned 32,512
pixels, changed 5,301, classified 9,211 candidates and took 38,841 endpoint
samples. Five start events are at least 1,000 ms after their logged idle base;
seven WAIT PERF packets have zero ray counts. No Panic text was captured.

The capture starts after camera movement: height is already `y_q8=764`
(approximately 2.98) and does not change thereafter. It has no BOOT or EXIT
packet. Continuous device elevation, visual controls/AA, return to a responsive
list, exit heap return and peak task stack remain unverified here; host checks
are separate evidence. User confirmation was pending when this record was saved.

Evidence is under `hp-39-gii/outputs/ray-fxaa-flight-2026-10-04/hardware/`:
`deployment.json`, flash logs, `ray-fxaa-flight-hardware-assessment.json` and
`ray-fxaa-flight-hardware.log`. The closed raw log SHA-256 is
`81D9BCE03D59113117C373359CBB7A93BE2B62F89446F4C737266E31D4AA92A3`.
The immutable predeployment manifest retains `hardware_verified=false`; the
separate hardware assessment describes this partial verification.
