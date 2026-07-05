# Sounds

A fast, native macOS app for live microphone visualization, written in Zig.
Audio is captured straight from the Core Audio HAL, analyzed with
Accelerate/vDSP, and every pixel is rendered by hand into a single SDL3 window:

- a direct-render workspace/tab banner at the top
- a raw waveform below it
- a live scrolling log-frequency spectrogram, recordings file list, trim view,
  whole-recording spectrum, or band lab underneath, with a labeled axis from
  10 Hz to 24 kHz where frequency analysis is shown

The default spectrogram mode is `1 TONAL WAVELET SST`: an analytic Morlet
constant-Q view where the input is decimated through an anti-aliased octave
pyramid; each octave is analyzed with 48 log-spaced Morlet voices on its own
hop, and each voice's power is smoothed over a window matched to the wavelet
length. That mode is useful for stable pitches and ridges, but it is causal and
time-integrated, so it is not the most honest view of sharp transients.

Mode `2 TRANSIENT STFT` is a centered multi-resolution STFT computed from the
raw recording buffer. Every displayed column is intentionally delayed by the
largest half-window, so the analysis window is centered on the physical audio
time being drawn. This removes the visual artifact where lower octaves appeared
hundreds of milliseconds late after a clap. Low frequencies still look wider
because that is the real time/frequency tradeoff: a 10 Hz estimate cannot be as
time-sharp as a 20 kHz estimate.

Mode `3 SPARSE RIDGES` keeps local maxima prominent and leaves a faint
continuum behind them. Modes `4` through `8` are the remaining experimental
transient/tonal views built from the same centered STFT bank:

- `4 REASSIGNED STFT` pulls energy toward nearby spectral peaks to sharpen
  ridges without changing the centered timing.
- `5 SQUEEZED STFT` is a stronger frequency squeeze for line-like components.
- `6 SUPERLET` combines multiple window lengths by geometric mean, suppressing
  energy that is not stable across scales.
- `7 MULTITAPER` averages several orthogonal tapers to reduce blocky leakage
  and random speckle.
- `8 S TRANSFORM` uses shorter frequency-proportional windows for a more
  transient-heavy constant-Q view.

Output is calibrated dBFS: a full-scale sine reads about 0 dBFS.

Both range edges are measured, not guessed, from spectra of this app's own raw
recordings and the active Core Audio device. The built-in microphone's input
chain collapses below roughly 20-25 Hz, but an external Yeti USB input exposed
measurable 10-20 Hz energy in a raw capture, so the display keeps one extra low
octave. It still stops at 10 Hz to avoid spending rows and latency on DC and
handling rumble. At the top, the Yeti exposes only 44.1/48 kHz input modes, so
the display stays at the 48 kHz Nyquist limit of 24 kHz; analysis voices stop
slightly below that to preserve the anti-alias guard band.

In the wavelet mode's synchrosqueezed display, coherent tones are reassigned to
their instantaneous frequency and drawn sharp, while noise and two-tone
interference are recognized by their envelope modulation and instantaneous
bandwidth and stay a smooth, honest continuum instead of scattering into
speckle. In the menu's Analysis tab, the `TONAL VIEW` row switches between SST
and the raw constant-Q magnitude view.

The live display also has overlapping frequency-band views. The latest local
voice recording has its dominant speech energy around 111 Hz, so the mid band
starts below that instead of cutting across the voice fundamental:

```text
whole  10 Hz-24 kHz
low    10-200 Hz
mid    100 Hz-2.4 kHz
high   2-24 kHz
custom typed lower/upper range
bands  full range with overlapping low/mid/high regions marked
```

Focused and custom views are not just clipped labels. The STFT modes rebuild
their row buckets for the selected span, integrate FFT bins covered by each
display row, and use slightly longer target-cycle windows when a narrower
range gives enough row density for finer frequency detail. The wavelet mode
keeps the same octave pyramid but only evaluates voices near the selected
range, so tonal/custom views spend work on the visible band and avoid drawing
out-of-range ridges.

## Build

Install Zig 0.16 and SDL3 if needed:

```sh
brew install zig sdl3
```

Build the app:

```sh
zig build
```

Run the unit tests:

```sh
zig build test
```

Release build and release test run:

```sh
zig build -Doptimize=ReleaseFast
zig build test -Doptimize=ReleaseFast
```

## Run

```sh
zig build run
```

Close the window, or press `Q`, to stop. `Escape` cancels confirmations or
text editing; it does not quit. Press `M` to open the
centered menu overlay. In the menu, `Up`/`Down` move the cursor, `Left`/`Right`
or `Tab` switch between Analysis, Bands, and Colors, and `Enter` applies the
highlighted row. `SET` marks the currently active setting. Press `Tab`
outside the menu to move through the workspace tabs.

In the Bands tab, choose `custom range` to use the stored custom range. The
`custom low` and `custom high` rows accept typed numbers; press `Enter` on a
row, type the frequency in Hz, then press `Enter` again to apply it.

Workspace keys:

```text
L  live spectrogram
V  recordings
T  trim
O  whole spectrum
B  band lab
```

Analysis menu order:

```text
1  tonal wavelet
2  transient STFT
3  sparse ridges
4  reassigned STFT
5  squeezed STFT
6  superlet
7  multitaper
8  S-transform
```

Recording is off by default. Press `R` to start recording, then press `R` again
to save the captured window to:

```text
recordings/sounds-YYYYMMDD-HHMMSS-32f.wav
```

Recordings are always saved as mono 32-bit float WAV files, matching the
32-bit float samples Core Audio provides to the app. Each stopped recording is
added to the recordings list. In `RECS`, use `Up`/`Down` to highlight a file,
`Enter` to select it as the active clip, `N` to rename it, and `D` twice to
delete it. While recording, the top banner shows a centered `REC mm:ss` timer
when there is enough room. Press `M` for the centered menu.

Offline workbench controls:

```text
P / Space  play or stop the current audition
Up/Down    highlight recordings in Recs
Enter      select the highlighted recording in Recs
N          rename the highlighted recording in Recs
D          press twice to delete the highlighted recording in Recs
A          cycle original / selected band / rejected band
F          cycle band render method
H          switch the selected band edge for Up/Down adjustment
Up/Down    move selected band edge by semitone steps in Band
[ and ]    move lower band edge down/up
-/=        move upper band edge down/up
,          select the trim start line in Trim
.          select the trim end line in Trim
Arrows     move the selected trim line in Trim
/          apply the staged trim in Trim
Backspace  clear trim in Trim
```

Band render methods:

```text
FFT MASK IFFT
FIR LINEAR
IIR BIQUAD
IIR ZERO PHASE
STFT MASK ISTFT
GRIFFIN LIM
CQT APPROX
ERB AUDITORY
MODAL
SPARSE
```

The advanced CQT, auditory, modal, and sparse methods are intentionally labeled
as approximations where appropriate. They are available as research/audition
modes, while the FFT, FIR, IIR, zero-phase IIR, and STFT methods are the
primary band-isolation paths.

The app stores simple local settings in `sounds.settings` in the directory the
app is launched from. Current settings include analysis mode, frequency band,
custom frequency range, and color map.

## Notes

The app is split by responsibility:

- `src/main.zig` wires capture, analysis, UI, and recording together.
- `src/app/` owns settings, clips, recording WAV IO, and the offline workbench.
- `src/audio/` owns Core Audio capture, playback, and the sample ring buffer.
- `src/analysis/engine.zig` drives the registered analysis algorithms through a
  shared input/output interface.
- `src/analysis/tonal.zig` owns the live Morlet wavelet mode.
- `src/analysis/spectral_mode.zig` owns the live STFT-derived modes, including
  transient, reassigned, squeezed, superlet, multitaper, S-transform, and sparse
  ridges.
- `src/analysis/spectrum.zig` contains the shared centered STFT primitives.
- `src/analysis/offline_spectrum.zig` computes one whole-clip frequency view.
- `src/analysis/band_render.zig` renders selected/rejected band audition audio.
- `src/app/clip.zig` owns the selected recording clip and trim range.
- `src/ui/` owns SDL, drawing, and text rendering.
- `src/support/` contains tiny shared support modules.

The wavelet constants and centered STFT spectrum primitives now live under
`src/analysis/`.

For the physics, papers, assumptions, and artifacts behind all eight analysis
modes, see [docs/analysis-approaches.md](docs/analysis-approaches.md).
For code ownership and extension rules, see
[docs/architecture.md](docs/architecture.md).
