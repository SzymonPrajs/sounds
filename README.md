# Core Audio HAL Microphone Capture

Small C11 macOS project for direct microphone capture through Core Audio HAL and
frequency analysis through Accelerate/vDSP, with three front ends:

- `mic-hal-vdsp`: records the default input and writes analysis files
- `live-sound`: a truecolor terminal dashboard
- `soundscope`: a windowed live view rendered through SDL3

The recorder copies the first input channel into a float buffer, then writes:

- `mic_mono.f32`: raw mono `float32` samples
- `levels.csv`: block RMS and peak amplitude
- `spectrum.csv`: Hann-windowed FFT magnitude

## Build

`soundscope` needs SDL3 (`brew install sdl3`); everything else uses only system
frameworks.

```sh
make
```

## Test

```sh
make test
```

The tests use synthetic samples and do not require microphone permission.

## Run

```sh
./bin/mic-hal-vdsp 10 16384
```

Arguments are:

```text
mic-hal-vdsp [seconds] [fft-size] [output-directory]
```

For example, record five seconds with a 32768-sample FFT and write into the
current directory:

```sh
./bin/mic-hal-vdsp 5 32768 .
```

## Live View

```sh
./bin/live-sound
```

That opens a terminal dashboard with:

- waveform: the raw microphone amplitude samples over time
- RMS/peak: compact level readings in dBFS
- spectrogram: scrolling frequency energy, with color showing amplitude

Use `Ctrl-C` to stop. For a timed smoke run:

```sh
./bin/live-sound 2 4096
```

Arguments are:

```text
live-sound [duration-seconds] [fft-size]
```

Use duration `0` to run until interrupted. Smaller FFT sizes update with lower
latency; larger FFT sizes show finer frequency detail.

## Window View

```sh
./bin/soundscope
```

That opens a resizable, HiDPI-aware window with the raw waveform on top, a
scrolling log-frequency spectrogram below it, and a Viridis palette along the
bottom. The windowed view defaults to an 8192-sample FFT and maps the display
from 20 Hz to Nyquist, with anti-aliased bin sampling so adjacent rows do not
collapse into chunky repeated FFT bins. RMS and peak levels live in the window
title. Close the window or press `Escape`/`Q` to stop.

Arguments match `live-sound`:

```text
soundscope [duration-seconds] [fft-size]
```

Before recording, allow your terminal app under System Settings -> Privacy &
Security -> Microphone, select the intended input device under Sound settings,
and keep Mic Mode set to Standard.
