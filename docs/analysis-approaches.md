# Sounds Analysis Approaches

This document explains the three live analysis modes in Sounds. It separates
three things that are easy to mix up:

- the physical pressure waveform captured by the microphone
- the mathematical time-frequency estimator
- the visual display choices used to make that estimator readable in real time

The raw recording is the closest thing this app has to the real sound wave. A
spectrogram is not the sound wave; it is an estimate of how much of the signal
looks like each frequency near each time. Every estimate trades time precision,
frequency precision, variance, leakage, and readability.

The mode registry is in `src/analysis/engine.zig`. The current menu order is
`1 TONAL WAVELET`, `2 TRANSIENT STFT`, and `3 SPARSE RIDGES`. The STFT-derived
modes share `src/analysis/spectrum.zig` and `src/analysis/spectral_mode.zig`.
The wavelet mode is implemented in `src/analysis/wavelet.zig` and wrapped by
`src/analysis/tonal.zig`.

## Shared Physics

The microphone records a pressure signal as a sequence of samples:

```text
x[n] = pressure at time n / sample_rate
```

At 48 kHz, each sample is about 20.8 microseconds apart. The raw waveform is a
single function of time. A time-frequency display asks a harder question: "near
this time, how much of the waveform behaves like a 50 Hz, 500 Hz, 5 kHz, or
20 kHz oscillation?"

That question cannot be answered with arbitrary precision in both dimensions.
To recognize a frequency, the analyzer must observe enough cycles. A 20 kHz
cycle lasts 0.05 ms, so a few cycles fit into a tiny time window. A 20 Hz cycle
lasts 50 ms, so even one cycle is long. This is the Heisenberg-Gabor
time-frequency tradeoff: short windows localize time but smear frequency; long
windows localize frequency but smear time.

For a clap or two books hitting each other, the contact itself is close to a
broadband impulse. In ideal free space, broadband energy begins at the same
physical time across the spectrum. In a real room, the microphone receives:

- the direct impulse
- early reflections from nearby surfaces
- late reflections and room modes
- frequency-dependent absorption by air, surfaces, books, hands, and the
  microphone body

High frequencies usually decay faster because surfaces and air absorb them more
strongly. Low frequencies can ring longer because room modes have lower damping.
So a longer low-frequency tail after a clap can be physically real. A systematic
frequency-dependent start time is more suspicious: it can come from causal
smoothing, uncentered windows, display integration, or phase/group-delay effects
in the recording chain.

The centered STFT modes intentionally delay display by the largest half-window
so each drawn column is centered on the physical audio time being estimated.
That removes the common artifact where low-frequency windows appear late simply
because the analyzer waited for enough samples.

## Display Pipeline

Each live mode eventually produces one dBFS column per time step:

```text
audio ring buffer -> analyzer column -> log-frequency rows -> dBFS -> color map
```

The display uses logarithmic frequency rows from about 10 Hz to the Nyquist
limit. This matches how pitch and octave structure are usually perceived, but
it means FFT bins are being remapped onto nonuniform visual rows. Blockiness can
come from several places:

- FFT bin spacing: a window of length `N` has bin spacing `sample_rate / N`.
- Window shape: Hann windows reduce sidelobes but widen the main lobe.
- Row density: display rows are finite; many high-frequency bins or few
  low-frequency bins must be interpolated onto them.
- Hop size: new columns are drawn at a fixed audio-clock step.
- Color compression: large dynamic range is squeezed into a colormap.

Focused frequency-band views change the estimator's display mapping, not just
the label on the window. The built-in bands intentionally overlap: Low is
10-200 Hz, Mid is 100 Hz-2.4 kHz, and High is 2-24 kHz. When Whole, Low, Mid,
High, or Custom is selected, the STFT-derived modes rebuild log-frequency row
buckets for that span, integrate FFT bins that fall inside each row's frequency
edges, and raise the target cycle count modestly for narrower spans to trade a
little time precision for clearer frequency detail. The wavelet mode keeps its
fixed octave pyramid but marks only voices near the selected span active, so
focused views skip out-of-range octave voices and do not paint their old ridge
state into the current display.

## 1. Tonal Wavelet SST

Mode name:

```text
1 TONAL WAVELET SST
1 TONAL WAVELET RAW
```

The wavelet mode uses scaled and shifted analytic wavelets instead of fixed
sinusoids in a rectangular frequency grid. A continuous wavelet transform has
the form:

```text
W_x(a, b) = integral x(t) conj(psi((t - b) / a)) dt / sqrt(a)
```

The scale `a` controls frequency. Small scales see high frequencies with short
windows; large scales see low frequencies with long windows. This is naturally
constant-Q: each frequency is measured with a window proportional to its period.

Sounds uses an analytic Morlet-style wavelet bank. The input is passed through
an anti-aliased octave pyramid, then each octave is analyzed with log-spaced
voices. The raw mode shows wavelet magnitude. The SST mode estimates
instantaneous frequency from the complex phase evolution and squeezes coherent
energy toward sharper ridges.

The physics is good for stable oscillators: strings, whistles, motors, room
modes, hum, and ringing resonances. The phase of an analytic wavelet coefficient
rotates at the local instantaneous frequency, so a stable tone can be sharpened
without merely increasing contrast.

For a clap, this mode is less literal than mode 2. The wavelet filters have
memory, and the current implementation is streaming and causal. That makes it
excellent for tonal ridges, but not the cleanest view of the exact start time of
a broadband transient.

Artifacts to expect:

- Stable tones and resonances are much sharper than in the transient STFT.
- Short broadband clicks can smear or ring through the wavelet filters.
- Raw CWT is smoother; SST is sharper but can overemphasize coherent ridges.
- Two close tones can create beating and apparent ridge modulation.

Relevant papers:

- Alexander Grossmann and Jean Morlet, "Decomposition of Hardy Functions into
  Square Integrable Wavelets of Constant Shape" (1984):
  <https://epubs.siam.org/doi/10.1137/0515056>
- P. Goupillaud, A. Grossmann, and J. Morlet, "Cycle-octave and related
  transforms in seismic signal analysis" (1984):
  <https://www.sciencedirect.com/science/article/pii/0016714284900255>
- Ingrid Daubechies, Jianfeng Lu, and Hau-Tieng Wu, "Synchrosqueezed Wavelet
  Transforms: An Empirical Mode Decomposition-like Tool" (2011):
  <https://arxiv.org/abs/0912.2437>
- Christopher Torrence and Gilbert Compo, "A Practical Guide to Wavelet
  Analysis" (1998):
  <https://psl.noaa.gov/people/gilbert.p.compo/Torrence_compo1998.pdf>

## 2. Transient STFT

Mode name:

```text
2 TRANSIENT STFT
```

The short-time Fourier transform analyzes the signal through a moving window:

```text
X(t, f) = sum_n x[n] w[n - t] exp(-i 2 pi f n / sample_rate)
```

The squared magnitude `|X(t, f)|^2` is the spectrogram. The current app uses a
bank of centered Hann-window FFTs with multiple lengths. For each display row,
it chooses or blends window lengths so the frequency row gets a roughly
constant number of cycles. This makes it closer to a constant-Q view than a
single fixed-window STFT, while still using efficient FFTs.

The physics is direct: a finite window is a measurement aperture. If the window
is short, the analyzer knows when the energy happened but has poor frequency
resolution. If the window is long, it can distinguish nearby frequencies but
must average over a longer time.

For claps, this is usually the honest first view. A broadband impulse should
peak at the same displayed time across most frequencies. Low frequencies will
still look fatter because the window that can resolve them is physically longer.

Artifacts to expect:

- Low-frequency energy is wider in time.
- Broadband impulses produce vertical stripes, but not infinitely thin ones.
- Narrow tonal resonances can look broader than they sound.
- FFT leakage can create faint neighboring bands around strong components.

Relevant papers:

- Dennis Gabor, "Theory of Communication" (1946), the classic paper behind
  time-frequency atoms and the time-frequency uncertainty view:
  <https://digital-library.theiet.org/doi/abs/10.1049/ji-3-2.1946.0074>
- A readable scan of the same paper is available here:
  <https://jcsphysics.net/lit/gabor1946.pdf>

## 3. Sparse Ridges

Mode name:

```text
3 SPARSE RIDGES
```

Sparse time-frequency methods start from the idea that many sounds can be
described by a small number of atoms: short sinusoidal packets, chirps,
resonances, or wavelets. Matching pursuit is the classic algorithm:

```text
residual = signal
repeat:
    choose atom with largest inner product against residual
    subtract that atom from residual
```

If the dictionary is made of time-frequency atoms, the chosen atoms form a
sparse time-frequency representation.

The current app does not run a full matching pursuit because that is expensive
for live display. Instead, mode 3 is sparse-ridge inspired. It starts with the
centered STFT power, keeps local maxima prominent, and leaves a faint continuum
for non-ridge energy.

The physics is interpretive rather than purely estimative. A resonant object
often produces narrow ridges. A broadband impact produces a continuum. Sparse
views help identify the ridge part of the sound, but they can hide diffuse
energy if used as the only view.

Artifacts to expect:

- Harmonics, hum, whistles, and resonances are easier to see.
- Broadband noise is intentionally suppressed.
- A clap can look less broadband than it really is.
- It is useful for reading structure, not for measuring total acoustic energy.

Relevant papers:

- Stephane Mallat and Zhifeng Zhang, "Matching Pursuits with Time-Frequency
  Dictionaries" (1993): <https://doi.org/10.1109/78.258082>
- Public PDF copy:
  <https://www.di.ens.fr/~mallat/papiers/MallatPursuit93.pdf>

## Removed Modes

The reassigned STFT, squeezed STFT, superlet, multitaper, and S-transform views
were experimental live modes built from the same centered STFT bank. They were
dropped from the app because the owner uses the tonal wavelet, transient STFT,
and sparse-ridge views in practice; keeping the live registry small makes the
UI and analyzer easier to reason about.

## What The Modes Mean For A Clap

For a loud clap or books hitting in a small room:

- The direct impact should be broadband and nearly simultaneous.
- High frequencies should often decay faster.
- Low-frequency room modes can ring for much longer.
- If high frequencies appear to start late, suspect analysis timing, smoothing,
  or display integration before assuming the room caused it.
- If low frequencies extend farther, that can be real room physics, analysis
  window length, or both.

A physically honest workflow is:

1. Start with mode 2. Check whether the broadband impulse peak is vertically
   aligned.
2. Compare mode 1. If post-clap ringing becomes clear as stable ridges, those
   are likely real resonances or room modes.
3. Use mode 3 to read resonant structure. Do not use it alone to judge how
   broadband the clap was.

## Validation Signals

These are the signals that should be used to validate and tune the display.

### Impulse

Use one sample set to 1.0. Expected result:

- centered STFT modes peak at the impulse time
- high and low rows should not have systematic start-time offsets
- low rows may be wider in time

This is already partly tested in `src/analysis/spectrum.zig`.

### Windowed Sine

Use a sine burst with known frequency and known start/stop. Expected result:

- mode 2 shows the burst with window-dependent edges
- mode 1 shows a clean ridge after wavelet settling
- mode 3 keeps the ridge and suppresses background

### Chirp

Use a swept sine with known instantaneous frequency. Expected result:

- mode 1 should track a clean tonal ridge after settling
- mode 2 should show the tradeoff between time and frequency resolution
- mode 3 should keep the strongest ridge and suppress low-level spread

### Two Close Tones

Use two sinusoids close in frequency. Expected result:

- longer effective windows separate them better
- transient-heavy readings merge them when the window is too short
- sparse ridges can make the strongest component easier to see while hiding
  weaker beating energy

### Decaying Resonator Bank

Model a clap as an impulse feeding resonators:

```text
y(t) = impulse(t) + sum_k A_k exp(-t / tau_k) sin(2 pi f_k t)
```

Expected result:

- broadband vertical onset
- narrow post-onset ridges at resonant frequencies
- longer low-frequency tails if low resonators have larger `tau_k`

This is a good synthetic model for "small room plus struck objects".

### Measured Room Impulse Response

The best validation is to play or create a known broadband pulse, record it,
and separately estimate the room impulse response. Then compare the app modes
against offline Python or MATLAB references using the same raw recording.

The important rule is that sharper is not automatically more physical. A mode
is better only when its assumptions match the sound. A hard clap is broadband
and impulsive at onset, then often resonant and room-colored afterward. No one
mode should be expected to show both parts perfectly.
