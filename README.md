# Sounds

Small C11 macOS app for live microphone visualization.

It opens one SDL3 window with:

- a raw waveform at the top
- a scrolling log-frequency spectrogram below it, with a labeled axis from
  20 Hz to 20 kHz (highest frequencies at the top, ticks at 1-2-5 steps,
  faint gridlines at decades)
- a Viridis palette strip along the bottom

The app captures the default microphone through Core Audio HAL, analyzes it with
Accelerate/vDSP, and renders every pixel in plain C.

The spectrogram analysis is a streaming multirate analytic Morlet wavelet
transform. The input is decimated through an anti-aliased octave pyramid; each
octave is analyzed with 48 log-spaced Morlet voices on its own hop, and each
voice's power is smoothed over a window matched to the wavelet length, so
every band is a time-integrated estimate rather than an instantaneous sample.
Output is calibrated dBFS: a full-scale sine reads about 0 dBFS.

The 20 Hz floor is deliberate: spectra measured from this app's own raw
recordings show the built-in microphone's input chain collapsing at
35-40 dB/decade below roughly 20-25 Hz, so sub-20 Hz octaves would only ever
display the noise floor. Dropping them buys double the voice density across
the audible band (about one voice per display row) and makes every octave
live within about three seconds of launch.

In the default synchrosqueezed display, coherent tones are reassigned to
their instantaneous frequency and drawn sharp, while noise and two-tone
interference are recognized by their envelope modulation and instantaneous
bandwidth and stay a smooth, honest continuum instead of scattering into
speckle. Press `S` for the raw constant-Q magnitude view.


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
the default synchrosqueezed display and raw CWT magnitude.

On exit, the app writes the most recent raw microphone samples to:

```text
recordings/sounds-YYYYMMDD-HHMMSS.f32
```

That file is mono `float32` PCM from the default input device. It is ignored by
Git and exists only as a debugging artifact.

## Notes

The display constants live at the top of `src/main.c`: waveform duration,
retained raw recording length, dB range, colors, and window size. The analysis
constants are in `include/sounds/analysis.h`: frequency range, voices per
octave, and Morlet omega0.
