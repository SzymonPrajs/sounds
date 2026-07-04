# Sounds

Small C11 macOS app for live microphone visualization.

It opens one SDL3 window with:

- a raw waveform at the top
- a scrolling log-frequency spectrogram below it, with a labeled axis from
  20 Hz to 24 kHz (highest frequencies at the top, ticks at 1-2-5 steps,
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
retained raw recording length, dB range, colors, window size, and the
spectrogram scroll rate (240 columns per second of audio; the image advances
on the audio clock, so a display frame may shift several columns at once).
The analysis constants are in `include/sounds/analysis.h`: frequency range,
voices per octave, and Morlet omega0.
