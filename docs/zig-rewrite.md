# Zig rewrite plan and conventions

The app is being rewritten from C (frozen reference in `src_c/`, buildable
with `make -C src_c`) into Zig 0.16 in `src/`. This is a re-implementation,
not a translation: the C code documents behavior, algorithms, and tuned
constants, but the Zig code should read like it was written in Zig first.

## Hard rules

- `zig build` and `zig build test` must be green after every unit of work.
- Do not modify anything under `src_c/` — it is the read-only reference.
- SDL3 is imported only through `src/c.zig` (`@import("../c.zig").sdl`).
- Apple frameworks are used only through the hand-written bindings in
  `src/apple/coreaudio.zig` and `src/apple/vdsp.zig`. Their headers do not
  survive translate-c; extend the binding files (with layout/behavior tests)
  when a new symbol is needed instead of adding `@cImport`s.
- Every new module gets registered in `src/all_tests.zig`.

## Style

- Idiomatic Zig, not transliterated C: slices instead of pointer+length,
  error unions instead of status enums (`src_c`'s `SoundsError` maps to Zig
  `error` sets), `defer`/`errdefer` for cleanup, tagged unions and enums with
  methods where the C code used switch-on-int, comptime where it removes
  runtime bookkeeping.
- Allocations are explicit: take an `std.mem.Allocator` parameter; the
  audio IOProc path must stay allocation-free and lock-free exactly like the
  C version.
- Tests live next to the code as `test` blocks. Port the behavioral checks
  from `src_c/tests/*.c` into the corresponding module's tests.
- Fix bugs found in the C code during the port; note the fix in a short
  comment only when the divergence from `src_c` would otherwise look like a
  porting mistake.
- Small algorithmic reshaping is welcome when it makes the Zig clearer or
  faster; the observable behavior (analysis output, file formats, keys)
  stays compatible unless the plan says otherwise.

## Module map (Zig ← C reference)

| Zig | C reference |
| --- | --- |
| `src/support/colormap.zig` | `src_c/src/support/colormap.c` |
| `src/support/frequency_band.zig` | `src_c/src/support/frequency_band.c` |
| `src/audio/ring_buffer.zig` | `src_c/src/audio/ring_buffer.c` |
| `src/audio/capture.zig` | `src_c/src/audio/capture.c` |
| `src/audio/playback.zig` | `src_c/src/audio/playback.c` |
| `src/analysis/*.zig` | `src_c/src/analysis/*` |
| `src/app/*.zig` | `src_c/src/app/*` |
| `src/ui/` | fresh design (see below), not a port of `src_c/src/ui/` |

`src_c/src/support/error.c` and `include/sounds/error.h` have no Zig
counterpart file: they dissolve into per-module error sets.

## Phases

1. support: colormap, frequency_band, ring_buffer (+ ported tests)
2. audio: capture, playback over `src/apple/coreaudio.zig`
3. analysis: spectrum primitives, wavelet, transient, tonal, spectral_mode,
   offline_spectrum, band_render, engine (+ ported tests)
4. ui: fresh immediate-mode UI on SDL3 — clean and sparse, shortcut-first,
   redesigned from scratch; the C imui is only a feature checklist
5. app: settings, clip, recording, workspace, workbench, main loop

## UI redesign intent (phase 4)

The C UI works but is dense, text-heavy, and grew shortcuts ad hoc. The Zig
UI keeps the keyboard-first philosophy (all existing shortcuts remain the
primary interface) but is rebuilt from scratch: fewer words on screen,
consistent spacing and alignment, one visual language for banner, menu,
lists, and overlays, and discoverable key hints instead of walls of text.
