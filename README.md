# Sounds

Small C11 macOS app for live microphone visualization.

It opens one SDL3 window with:

- a raw waveform at the top
- a scrolling log-frequency spectrogram below it
- a Viridis palette strip along the bottom

The app captures the default microphone through Core Audio HAL, analyzes it with
Accelerate/vDSP, and renders every pixel in plain C.

The spectrogram analysis is a streaming multirate analytic Morlet wavelet
transform. The input is decimated through an anti-aliased octave pyramid; each
octave is analyzed with 24 log-spaced Morlet voices, then displayed either as
raw CWT magnitude or as synchrosqueezed energy. The display spans about
0.5 Hz to 20 kHz when the input sample rate permits it.

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
