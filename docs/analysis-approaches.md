# Sounds Analysis Approaches

This document explains the eight live analysis modes in Sounds. It is written
for the current codebase, so it separates three things that are easy to mix up:

- the physical pressure waveform captured by the microphone
- the mathematical time-frequency estimator
- the visual display choices used to make that estimator readable in real time

The raw recording is the closest thing this app has to the real sound wave. A
spectrogram is not the sound wave; it is an estimate of how much of the signal
looks like each frequency near each time. Every estimate trades time precision,
frequency precision, variance, leakage, and readability.

The mode registry is in `src/app/app_mode.c`. The STFT-derived modes share
`src/analysis/spectrum.c` and `src/analysis/spectral_mode.c`. The wavelet mode
is implemented in `src/analysis/wavelet.c` and wrapped by `src/analysis/tonal.c`.

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

The current centered STFT modes intentionally delay display by the largest
half-window so each drawn column is centered on the physical audio time being
estimated. That removes the common artifact where low-frequency windows appear
late simply because the analyzer waited for enough samples.

## Display Pipeline

Most modes eventually produce one dBFS column per time step:

```text
audio ring buffer -> analyzer column -> log-frequency rows -> dBFS -> color map
```

The display uses logarithmic frequency rows from about 20 Hz to the Nyquist
limit. This matches how pitch and octave structure are usually perceived, but
it means FFT bins are being remapped onto nonuniform visual rows. Blockiness can
come from several places:

- FFT bin spacing: a window of length `N` has bin spacing `sample_rate / N`.
- Window shape: Hann and taper windows reduce sidelobes but widen the main
  lobe.
- Row density: display rows are finite; many high-frequency bins or few
  low-frequency bins must be interpolated onto them.
- Hop size: new columns are drawn at a fixed audio-clock step.
- Post-analysis visual smoothing: this makes the display less flickery, but it
  can blur time.
- Color compression: large dynamic range is squeezed into a colormap.

The app's job is not to pretend this tradeoff is absent. A good mode makes the
tradeoff explicit and useful for the thing being inspected.

## 1. Transient STFT

Mode name:

```text
1 TRANSIENT STFT
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

## 2. Tonal Wavelet SST

Mode name:

```text
2 TONAL WAVELET SST
2 TONAL WAVELET RAW
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

For a clap, this mode is less "literal" than mode 1. The wavelet filters have
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
  Analysis" (1998), useful for practical wavelet interpretation:
  <https://psl.noaa.gov/people/gilbert.p.compo/Torrence_compo1998.pdf>

## 3. Reassigned STFT

Mode name:

```text
3 REASSIGNED STFT
```

A normal spectrogram draws each energy value at the center of the analysis
window and the center of the FFT bin. That is convenient, but not always where
the energy is actually concentrated. Reassignment estimates a better time and
frequency coordinate for each spectrogram value and moves the value there.

Professional reassignment uses STFT phase derivatives. Conceptually:

```text
time correction      comes from phase derivative over frequency
frequency correction comes from phase derivative over time
```

The result is still based on the same measured signal, but the visual energy is
moved toward local centers of gravity in the time-frequency plane.

The current app implements a simplified real-time version: it computes the
centered multi-resolution STFT power, finds nearby local spectral peaks per
column, and deposits most of each row's power toward those peaks. It is
frequency reassignment inspired by the real method, not a full phase-gradient
time-frequency reassignment.

The physics intuition is that a windowed sinusoid leaks into neighboring bins
because the window is finite. Reassignment says: "this smeared patch came from a
more precise local oscillatory event, so draw it nearer that event." It improves
readability when the signal is made of separated components.

Artifacts to expect:

- Tonal ridges look sharper than mode 1.
- Broadband transients remain broad, but some frequency bands may snap toward
  local peaks.
- Dense noise can look artificially structured if the peak detector is too
  aggressive.
- Because the current app does not use phase-gradient time reassignment, it
  should not be treated as a physically exact reassigned spectrogram.

Relevant papers:

- Francois Auger and Patrick Flandrin, "Improving the Readability of
  Time-Frequency and Time-Scale Representations by the Reassignment Method"
  (1995): <https://ieeexplore.ieee.org/document/382394/>
- Public PDF copy of the same paper:
  <https://www.acousticslab.org/learnmoresra/files/augerflandrin1995ieee43.pdf>
- Francois Auger, Patrick Flandrin, Yu-Ting Lin, Stephen McLaughlin, Sylvain
  Meignen, Thomas Oberlin, and Hau-Tieng Wu, "Time-Frequency Reassignment and
  Synchrosqueezing: An Overview" (2013):
  <https://hal.science/hal-00983755/file/manuscriptR2.pdf>

## 4. Squeezed STFT

Mode name:

```text
4 SQUEEZED STFT
```

Synchrosqueezing is closely related to reassignment, but it usually reassigns
only along the frequency axis. That matters because frequency-only squeezing can
preserve a time grid and, in the formal algorithms, can remain invertible under
assumptions about separated AM-FM modes.

For an STFT `V_x(t, eta)`, a Fourier synchrosqueezing transform estimates local
instantaneous frequency from the complex derivative:

```text
omega_x(t, eta) ~= Im(partial_t V_x(t, eta) / V_x(t, eta))
```

Energy at analysis frequency `eta` is moved to the estimated instantaneous
frequency `omega_x`.

The current app's squeezed STFT is a stronger version of the simplified
frequency reassignment in mode 3. It uses a wider search neighborhood, a lower
contrast threshold, and a narrower deposit kernel. It is intended to answer:
"if I force coherent ridges to be as sharp as this display can make them, what
does the sound look like?"

The physics is useful for signals that are locally close to:

```text
x(t) = A(t) cos(phi(t))
```

where amplitude `A(t)` and instantaneous frequency `phi'(t)` change slowly
relative to the carrier. That describes many musical and mechanical tones. It
does not describe a hard clap very well, because a clap is broadband and not a
small number of slowly varying modes.

Artifacts to expect:

- Stable tones become very narrow.
- Chirps can be clearer than in a raw STFT.
- Noisy or percussive energy can be over-concentrated into ridges.
- It can look "more real" for tones but less honest for broadband impacts.

Relevant papers:

- Thomas Oberlin, Sylvain Meignen, and Valerie Perrier, "The Fourier-based
  Synchrosqueezing Transform" (2015):
  <https://hal.science/hal-00916088/document>
- Daubechies, Lu, and Wu (2011) for the wavelet synchrosqueezing foundation:
  <https://arxiv.org/abs/0912.2437>
- Auger et al. (2013) for the relationship between reassignment and
  synchrosqueezing:
  <https://hal.science/hal-00983755/file/manuscriptR2.pdf>

## 5. Superlet

Mode name:

```text
5 SUPERLET
```

The superlet idea is to combine multiple wavelets with different cycle counts.
A short wavelet gives good time localization. Longer wavelets give better
frequency localization. A superlet set combines them geometrically so energy
that is consistent across scales survives, while scale-specific leakage is
suppressed.

In simplified form:

```text
P_super(t, f) = geometric_mean_k P_k(t, f)
```

where each `P_k` is power from a wavelet with a different number of cycles.

The current app uses a superlet-inspired STFT approximation rather than the
full wavelet superlet algorithm. It computes the existing centered STFT window
bank and takes a geometric mean across progressively longer windows. This keeps
the real-time implementation small and reuses the centered timing machinery,
but it is not yet the same as a professional adaptive Morlet superlet.

The physics intuition is cross-scale agreement. A real oscillatory burst should
appear in both short and longer apertures. Random leakage or one-window
accidents often do not. The geometric mean punishes energy that appears only in
one scale.

Artifacts to expect:

- Tonal bursts can look cleaner than in mode 1.
- Noise and leakage are reduced.
- Very brief broadband impulses may be de-emphasized because they are not
  equally strong in longer windows.
- If a feature is real but very short, superlet-style agreement can make it look
  weaker than it physically was.

Relevant papers:

- Vasile V. Moca, Harald Barzan, Adriana Nagy-Dabacan, and Raul C. Muresan,
  "Time-Frequency Super-Resolution with Superlets" (2021):
  <https://www.nature.com/articles/s41467-020-20539-9>
- PubMed record:
  <https://pubmed.ncbi.nlm.nih.gov/33436585/>

## 6. Multitaper

Mode name:

```text
6 MULTITAPER
```

A single-window spectrum is noisy and biased by the window's sidelobes. The
multitaper method applies several mutually orthogonal tapers to the same local
time segment, computes a spectrum for each, and averages them:

```text
S_hat(f) = (1 / K) sum_k |FFT{x[n] v_k[n]}|^2
```

Professional multitaper analysis typically uses discrete prolate spheroidal
sequences, also called Slepian tapers or DPSS tapers. These are designed to
concentrate energy optimally inside a chosen time-bandwidth product.

The current app uses a simpler real-time approximation: a small set of
orthogonal sine tapers for each STFT length. This is not full DPSS multitaper,
but it captures the most important display behavior: different tapers leak in
different ways, and averaging them reduces random speckle and single-window
artifacts.

The physics is statistical. The signal segment is finite, so the estimate has
variance. Multiple tapers are multiple controlled looks at the same finite
piece of sound. Averaging reduces variance, but it can also smooth or soften
features.

Artifacts to expect:

- Less speckle than mode 1.
- Reduced dependence on one particular Hann window.
- Slightly softer ridges and transients.
- It improves estimator stability; it does not beat the time-frequency
  uncertainty limit.

Relevant papers:

- David J. Thomson, "Spectrum Estimation and Harmonic Analysis" (1982):
  <https://doi.org/10.1109/PROC.1982.12433>
- Public PDF copy:
  <https://www.math.ucdavis.edu/~saito/data/ONR15/thomson_spect-est-harm-anal.pdf>

## 7. S Transform

Mode name:

```text
7 S TRANSFORM
```

The S-transform, introduced by Stockwell, Mansinha, and Lowe, combines ideas
from the STFT and continuous wavelet transform. It uses a Gaussian window whose
width changes with frequency, while maintaining a direct phase relationship to
the Fourier spectrum.

One continuous form is:

```text
S(t, f) = integral x(u) |f| exp(-(t - u)^2 f^2 / 2) exp(-i 2 pi f u) du
```

High frequencies get short windows. Low frequencies get long windows. That is
similar to wavelets, but the phase convention keeps a more direct connection to
Fourier components.

The current app uses an S-transform-inspired view: it asks the existing
multi-resolution STFT bank for fewer cycles per row than mode 1. In practice,
this makes the display more transient-heavy and less frequency-sharp. It is a
useful comparison mode when asking whether a blocky look came from too much
frequency localization.

The physics is again aperture length. If mode 1 looks too frequency-smoothed or
too slow for an impact, this mode deliberately shifts the tradeoff toward time.

Artifacts to expect:

- Sharper vertical timing for impacts.
- Wider frequency bands.
- More broadband-looking claps.
- Less reliable separation of nearby tonal components.

Relevant papers:

- R. G. Stockwell, L. Mansinha, and R. P. Lowe, "Localization of the Complex
  Spectrum: The S Transform" (1996):
  <https://doi.org/10.1109/78.492555>
- IEEE record:
  <https://ieeexplore.ieee.org/document/492555/>

## 8. Sparse Ridges

Mode name:

```text
8 SPARSE RIDGES
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
for live display. Instead, mode 8 is sparse-ridge inspired. It starts with the
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

1. Start with mode 1. Check whether the broadband impulse peak is vertically
   aligned.
2. Compare mode 7. If timing becomes much sharper but frequency detail smears,
   the original mode was limited by aperture length, not necessarily wrong.
3. Compare mode 6. If blockiness and speckle reduce, the issue was estimator
   variance or window leakage.
4. Compare modes 3 and 4. If ridges sharpen but the broadband impact becomes
   too line-like, the signal is not actually made only of coherent modes.
5. Compare mode 2. If post-clap ringing becomes clear as stable ridges, those
   are likely real resonances or room modes.
6. Use mode 8 only to read resonant structure. Do not use it alone to judge how
   broadband the clap was.

## Validation Signals

These are the signals that should be used to validate and tune the display.

### Impulse

Use one sample set to 1.0. Expected result:

- all centered modes peak at the impulse time
- high and low rows should not have systematic start-time offsets
- low rows may be wider in time

This is already partly tested in `tests/spectrum_test.c`.

### Windowed Sine

Use a sine burst with known frequency and known start/stop. Expected result:

- mode 1 shows the burst with window-dependent edges
- mode 2 shows a clean ridge after wavelet settling
- modes 3 and 4 sharpen the ridge
- mode 6 reduces speckle
- mode 8 keeps the ridge and suppresses background

### Chirp

Use a swept sine with known instantaneous frequency. Expected result:

- mode 2 and mode 4 should track the ridge well
- mode 1 should show the tradeoff between time and frequency resolution
- mode 7 should track fast changes but blur frequency more

### Two Close Tones

Use two sinusoids close in frequency. Expected result:

- longer-window modes separate them better
- transient-heavy modes merge them
- squeezed modes may over-separate if the tones beat strongly

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

## Upgrade Path Toward Professional Algorithms

The current app favors small, readable, real-time C. The most meaningful future
upgrades would be:

- Exact STFT reassignment: compute the extra STFTs needed for phase-gradient
  time and frequency reassignment, then move energy in both axes.
- Exact Fourier synchrosqueezing: compute instantaneous frequency from complex
  STFT derivatives and reassign frequency only.
- Real Morlet superlets: implement adaptive-order Morlet sets instead of the
  current STFT geometric-mean approximation.
- DPSS multitaper: generate Slepian tapers for a chosen time-bandwidth product
  and optionally use adaptive weighting.
- Exact S-transform: implement the frequency-scaled Gaussian transform rather
  than approximating it with fewer STFT cycles.
- Offline sparse pursuit: keep mode 8 lightweight for live display, but add an
  offline analysis path for true matching pursuit or convex sparse coding.

The important rule is that sharper is not automatically more physical. A mode
is better only when its assumptions match the sound. A hard clap is broadband
and impulsive at onset, then often resonant and room-colored afterward. No one
mode should be expected to show both parts perfectly.

