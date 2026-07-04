# Sounds

Small C11 macOS app for live microphone visualization.

It opens one SDL3 window with:

- a raw waveform at the top
- a scrolling log-frequency spectrogram below it
- a Viridis palette strip along the bottom

The app captures the default microphone through Core Audio HAL, computes the FFT
with Accelerate/vDSP, and renders every pixel in plain C.

## Build

Install SDL3 if needed:

```sh
brew install sdl3
```

Build the app:

```sh
make
```

## Run

```sh
./bin/sounds
```

Close the window, or press `Escape`/`Q`, to stop.

On exit, the app writes the most recent raw microphone samples to:

```text
recordings/sounds-YYYYMMDD-HHMMSS.f32
```

That file is mono `float32` PCM from the default input device. It is ignored by
Git and exists only as a debugging artifact.

## Notes

The constants that shape the app live at the top of `src/main.c`: FFT
size, waveform duration, retained raw recording length, frequency range, dB
range, colors, and window size.
