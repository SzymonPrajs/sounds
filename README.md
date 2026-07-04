# Sounds

Small C11 macOS app for live microphone visualization.

It opens one SDL3 window with:

- a thin mode banner at the top
- a raw waveform at the top
- a scrolling log-frequency spectrogram below it, with a labeled axis from
  20 Hz to 24 kHz (highest frequencies at the top, ticks at 1-2-5 steps,
  faint gridlines at decades)

The app captures the default microphone through Core Audio HAL, analyzes it with
Accelerate/vDSP, and renders every pixel in plain C.

The default spectrogram mode is `1 TRANSIENT STFT`: a centered
multi-resolution STFT computed from the raw recording buffer. Every displayed
column is intentionally delayed by the largest half-window, so the analysis
window is centered on the physical audio time being drawn. This removes the
visual artifact where lower octaves appeared hundreds of milliseconds late
after a clap. Low frequencies still look wider because that is the real
time/frequency tradeoff: a 20 Hz estimate cannot be as time-sharp as a 20 kHz
estimate.

Press `2` for the streaming tonal wavelet mode. This is the analytic Morlet
constant-Q view: the input is decimated through an anti-aliased octave pyramid;
each octave is analyzed with 48 log-spaced Morlet voices on its own hop, and
each voice's power is smoothed over a window matched to the wavelet length.
That mode is useful for stable pitches and ridges, but it is causal and
time-integrated, so it is not the most honest view of sharp transients.

Press `3` for room-decay mode. It uses the same centered STFT timing as mode 1,
then applies a fast-attack/slow-release envelope per frequency row so echoes
and band decay are easier to read.

Output is calibrated dBFS: a full-scale sine reads about 0 dBFS.

Both range edges are measured, not guessed, from spectra of this app's own
raw recordings. The 20 Hz floor: the built-in microphone's input chain
collapses at 35-40 dB/decade below roughly 20-25 Hz, so lower octaves would
only ever display the noise floor, and dropping them buys double the voice
density across the audible band (about one voice per display row) with every
octave live within seconds of launch. At the top, analysis voices run to
about 23 kHz, where the ADC's anti-alias brick wall sits (the recorded
spectrum runs flat to 23.0 kHz, then drops about 30 dB within 500 Hz), but
the display extends to 24 kHz on purpose: the permanently dark strip above
the brick wall shows where the hardware ends, mirroring how the low edge
shows the input high-pass.

In the wavelet mode's synchrosqueezed display, coherent tones are reassigned to
their instantaneous frequency and drawn sharp, while noise and two-tone
interference are recognized by their envelope modulation and instantaneous
bandwidth and stay a smooth, honest continuum instead of scattering into
speckle. In mode 2, press `S` for the raw constant-Q magnitude view.


## Build

Install SDL3 if needed:

```sh
brew install sdl3
```

Build the app:

```sh
make
```

Run the microphone-free synthetic analyzer checks:

```sh
make test
```

## Run

```sh
./bin/sounds
```

Close the window, or press `Escape`/`Q`, to stop. Press `S` to toggle between
the synchrosqueezed and raw CWT versions of mode 2.

Mode keys:

```text
1  transient STFT
2  tonal wavelet
3  room decay
```

On exit, the app writes the most recent raw microphone samples to:

```text
recordings/sounds-YYYYMMDD-HHMMSS.f32
```

That file is mono `float32` PCM from the default input device. It is ignored by
Git and exists only as a debugging artifact.

## Notes

The app is split by responsibility:

- `src/app/` wires capture, analysis, UI, and recording together.
- `src/audio/` owns Core Audio capture and the sample ring buffer.
- `src/analysis/engine.c` drives the registered analysis algorithms through a
  shared input/output interface.
- `src/analysis/transient.c`, `src/analysis/tonal.c`, and
  `src/analysis/room_decay.c` are the three app-level algorithms. A new live
  analysis mode should follow that shape: consume `SoundAnalysisInput`, append
  dBFS columns to `SoundAnalysisOutput`, and register with the engine.
- `src/ui/` owns SDL, drawing, and text rendering.
- `src/support/` contains tiny shared support modules.

The wavelet constants live in `include/sounds/analysis.h`; centered STFT and
room-decay spectrum primitives live in `src/analysis/spectrum.c`.
