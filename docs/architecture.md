# Architecture

Sounds is a native macOS audio visualization app written in Zig. It captures
mono float input from the Core Audio HAL, keeps recent samples in a ring buffer,
analyzes live or saved audio, and draws the UI into one SDL3 window.

## Module Map

- `src/main.zig` owns process startup, SDL window lifetime, capture/playback
  startup, settings application, event routing, and recording snapshots.
- `src/c.zig` is the single SDL3 import point. App code imports SDL as
  `@import("c.zig").sdl`.
- `src/apple/` contains hand-written Core Audio and vDSP bindings.
- `src/audio/` owns Core Audio input, Core Audio playback, and the sample ring
  buffer shared by capture and analysis.
- `src/analysis/engine.zig` owns the live analysis registry, active mode,
  frequency-band selection, output-column storage, and timeline resets.
- `src/analysis/tonal.zig` owns the live Morlet wavelet mode.
- `src/analysis/spectral_mode.zig` owns live STFT-derived modes: transient and
  sparse ridges.
- `src/analysis/spectrum.zig` contains the shared centered STFT primitives.
- `src/analysis/wavelet.zig` contains the analytic Morlet pyramid and
  synchrosqueezing implementation.
- `src/analysis/offline_spectrum.zig` computes whole-clip spectrum,
  spectrogram, and band-level envelope views for saved recordings.
- `src/analysis/band_render.zig` renders selected/rejected band audition audio.
- `src/app/` owns settings, clips, recording WAV IO, and the offline workbench.
- `src/ui/` owns layout, immediate UI state, drawing, text, and widgets.
- `src/support/` contains small shared modules such as colormaps, frequency
  bands, local-time formatting, and streaming column helpers.
- `src/all_tests.zig` imports the full tree so `zig build test` runs every test.

## Apple And SDL Interop

Apple framework bindings live under `src/apple/`. Extend those files by adding
only the declarations, layouts, constants, and tiny wrappers the app needs.
Every new layout or constant should have a test when its value matters to ABI
compatibility.

Do not use `@cImport` for Apple framework headers. The relevant macOS headers
do not translate reliably, and broad generated declarations make the binding
surface harder to review. SDL3 is the exception: it is imported only by
`src/c.zig`, and the rest of the app reaches it through that module.

## Hot Loops

Hot elementwise loops should use the local `@Vector` style already present in
the codebase:

- Choose `std.simd.suggestVectorLength(T) orelse fallback`.
- Process full vector chunks first.
- Finish with a scalar tail loop.
- Keep vDSP calls for FFTs, dot products, RMS, and other operations where the
  framework already provides the optimized primitive.

Prefer readable scalar code for cold paths, setup, metadata parsing, and UI
control flow.

## Allocation And IOProc Discipline

Allocators are passed into owners and stored on long-lived structs that allocate.
Avoid hidden global allocators in helpers that read files or allocate buffers.
Empty slices may be freed directly; no length guard is needed.

Core Audio IOProc callbacks must stay allocation-free and non-blocking. Capture
copies or deinterleaves into preallocated scratch storage, then calls the
configured callback. Playback consumes an owned sample copy and writes output
buffers in place. File IO, WAV encoding, directory scans, and workbench loading
stay outside IOProc callbacks.

Recording stop snapshots audio once from the ring buffer, then reuses that
snapshot for WAV saving and workbench insertion.

## Adding A Live Analysis Mode

1. Add a case to `analysis.Mode` in `src/analysis/engine.zig`.
2. Add a matching field to the `Algorithm` union.
3. Implement or reuse an analyzer with this method set:
   `init`, `deinit`, `minFrequency`, `maxFrequency`, `setFrequencyRange`,
   `reset`, and `update`.
4. Register the mode in `Engine.createAlgorithms`.
5. Add settings serialization for the new mode in `src/app/settings.zig`.
6. Add UI labels/menu entries where modes are displayed.
7. Add focused tests for scheduling, range handling, and representative output.

STFT-like modes should normally reuse `spectral_mode.Algorithm` with a new
`spectrum.Mode` value. Only add a new live algorithm type when the scheduling or
state model is genuinely different.
