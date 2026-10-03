# Ray host checks

From the repository root, run:

```powershell
& 'C:/Users/perfsakuya/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' tools/ray/run_checks.py
```

The default compiler is `D:/w64devkit/bin/gcc.exe`. Override it with `--gcc`.
Outputs go to `build/ray-validation`; override with `--out`. The runner creates
that directory without clearing it. It records every build/check command,
exit code, host duration and output in `checks.json` and individual logs.

The checks use production core, preview and session code. The core's public
pixel function is renamed only in the host object so an observer can count
every pixel requested by the unmodified session. The checks cover deterministic
samples, changed views, framebuffer guards, preview with no tracing or
intersection tests, movement limits and collision, the 999/1000 ms transition,
clock wrap, cancellation, batch bounds, contrast/reset controls, and all 32512
unique progressive samples. Each final image must exactly equal direct tracing
at the same contrast.

Artifacts include `preview.pgm`, `trace-c1.pgm`, `trace-c2.pgm`, `trace-c3.pgm`,
matching PNGs, and `contact-sheet.png`. Image conversion uses Pillow. Contrast
filenames C1/C2/C3 correspond to API values 0/1/2.

These are host correctness checks. Their durations are not ARM timings.
Physical key release, LCD synchronization, UI resume, and task deletion need
hardware verification. The dashboard independently accepts the ASCII
`RAY_BOOT`, `RAY_EVENT`, `RAY_PERF`, `RAY_MEM` and `RAY_EXIT` protocols. It retains
the original bytes at `/log`; generation changes clear the old trace metrics.
`stack_words=-1` is displayed as unknown. Timing categories contain nested
intervals and are shown individually, without summing them.
