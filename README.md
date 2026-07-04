# Sounds

A fast, native macOS app for live microphone visualization, written in pure C.
Audio is captured straight from the Core Audio HAL, analyzed with
Accelerate/vDSP, and every pixel is rendered by hand into a single SDL3 window:

- a direct-render workspace/tab banner at the top
- a raw waveform below it
- a live scrolling log-frequency spectrogram, recordings file list, trim view,
  whole-recording spectrum, or band lab underneath, with a labeled axis from
  10 Hz to 24 kHz where frequency analysis is shown

The default spectrogram mode is `1 TRANSIENT STFT`: a centered
multi-resolution STFT computed from the raw recording buffer. Every displayed
column is intentionally delayed by the largest half-window, so the analysis
window is centered on the physical audio time being drawn. This removes the
visual artifact where lower octaves appeared hundreds of milliseconds late
after a clap. Low frequencies still look wider because that is the real
time/frequency tradeoff: a 10 Hz estimate cannot be as time-sharp as a 20 kHz
estimate.

Press `2` for the streaming tonal wavelet mode. This is the analytic Morlet
constant-Q view: the input is decimated through an anti-aliased octave pyramid;
each octave is analyzed with 48 log-spaced Morlet voices on its own hop, and
each voice's power is smoothed over a window matched to the wavelet length.
That mode is useful for stable pitches and ridges, but it is causal and
time-integrated, so it is not the most honest view of sharp transients.

Modes `3` through `8` are experimental transient/tonal views built from the
same centered STFT bank as mode 1:

- `3 REASSIGNED STFT` pulls energy toward nearby spectral peaks to sharpen
  ridges without changing the centered timing.
- `4 SQUEEZED STFT` is a stronger frequency squeeze for line-like components.
- `5 SUPERLET` combines multiple window lengths by geometric mean, suppressing
  energy that is not stable across scales.
- `6 MULTITAPER` averages several orthogonal tapers to reduce blocky leakage
  and random speckle.
- `7 S TRANSFORM` uses shorter frequency-proportional windows for a more
  transient-heavy constant-Q view.
- `8 SPARSE RIDGES` keeps local maxima prominent and leaves a faint continuum
  behind them.

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
speckle. In mode 2, press `S` for the raw constant-Q magnitude view.


## Build

Install Homebrew LLVM and SDL3 if needed:

```sh
brew install llvm sdl3
```

Build the app. The Makefile defaults to Homebrew Clang with C2y, the C defer
TS, local CPU tuning, and the existing Accelerate/CoreAudio/SDL links. LTO is
kept opt-in through `LTOFLAGS` because full LTO currently trips the Homebrew
LLVM linker on this macOS setup:

```sh
make
```

Run the microphone-free synthetic analyzer, spectrum, recording, playback API,
and band-render checks:

```sh
make test
```

## Run

```sh
./bin/sounds
```

Close the window, or press `Escape`/`Q`, to stop. Press `M` to open the
centered menu overlay. Press `Tab` to move through the workspace tabs. Press
`S` to toggle between the synchrosqueezed and raw CWT versions of mode 2.

Workspace keys:

```text
L  live spectrogram
V  recordings
T  trim
O  whole spectrum
B  band lab
```

Mode keys:

```text
1  transient STFT
2  tonal wavelet
3  reassigned STFT
4  squeezed STFT
5  superlet
6  multitaper
7  S-transform
8  sparse ridges
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
when there is enough room. Press `M` for the centered menu. Press `C` to cycle
color maps; in the menu, arrow keys also cycle the color map.

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
app is launched from. Current settings include analysis mode and color map.

## Notes

The app is split by responsibility:

- `src/app/` wires capture, analysis, UI, and recording together.
- `src/audio/` owns Core Audio capture and the sample ring buffer.
- `src/audio/playback.c` owns Core Audio HAL playback for mono float clips.
- `src/analysis/engine.c` drives the registered analysis algorithms through a
  shared input/output interface.
- `src/analysis/transient.c`, `src/analysis/tonal.c`, and
  `src/analysis/spectral_mode.c` are the app-level algorithms. A new live
  analysis mode should follow that shape: consume `SoundAnalysisInput`, append
  dBFS columns to `SoundAnalysisOutput`, and register with the engine.
- `src/analysis/offline_spectrum.c` computes one whole-clip frequency view.
- `src/analysis/band_render.c` renders selected/rejected band audition audio.
- `src/app/clip.c` owns the selected recording clip and basic trimming.
- `src/ui/` owns SDL, drawing, and text rendering.
- `src/support/` contains tiny shared support modules.

The wavelet constants live in `include/sounds/analysis.h`; centered STFT
spectrum primitives live in `src/analysis/spectrum.c`.

For the physics, papers, assumptions, and artifacts behind all eight analysis
modes, see [docs/analysis-approaches.md](docs/analysis-approaches.md).
