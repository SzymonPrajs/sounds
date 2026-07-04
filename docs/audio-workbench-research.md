# Audio Workbench Research Pass

This is a design research note for turning Sounds from a primarily live
spectrogram recorder into a small audio research workbench while preserving the
current look: dark pixel-rendered UI, thin banner, waveform strip, log-frequency
axis, color maps, and keyboard-friendly menus.

No implementation is proposed here as code. The goal is to identify the right
product shape, DSP vocabulary, technical tradeoffs, and validation path.

## Current Baseline

The current app is a live microphone visualizer:

- Core Audio HAL capture writes mono float samples into a ring buffer.
- A short waveform strip is drawn above a scrolling log-frequency spectrogram.
- The analysis engine emits dBFS columns for one selected live mode.
- The UI is SDL3 plus a custom pixel framebuffer and bitmap font.
- Recording saves a recent region of the ring buffer as mono 32-bit float WAV.
- Existing analysis modes are all time-frequency views:
  transient STFT, tonal wavelet SST/raw CWT, reassigned STFT, squeezed STFT,
  superlet, multitaper, S-transform, and sparse ridges.

The important design point is that the raw recording is more fundamental than
any displayed spectrogram. The current documentation already frames this well:
spectrograms are estimators, not the sound itself. The expanded app should keep
that idea visible in its data model and UI.

## Product Direction

Treat "spectrogram recording" as one workspace, not the app's whole identity.
The bigger app should have one active clip and several analysis or processing
views over that clip.

Recommended top-level workspaces:

1. Live Spectrogram
2. Recordings / Clip Source
3. Whole Spectrum
4. Band Lab
5. Compare / Measurements
6. Settings

This keeps the current visual mode system but stops overloading `1-8` as the
entire app. The current eight spectrogram algorithms become techniques inside
Live Spectrogram or Clip Spectrogram. Whole Spectrum and Band Lab get their own
technique menus.

## Core Data Model

The app needs an explicit clip model:

```text
SoundClip
  samples: float mono or interleaved float
  sample_rate
  source: live capture, saved recording, imported file, processed render
  start_time / duration
  name / path / dirty flag
```

Operations should be non-destructive:

```text
source clip -> analysis result
source clip + band operation -> rendered audition clip
rendered audition clip -> save/export
```

For research use, destructive replacement is risky. "Replace recording" should
mean "make this clip the active source" or "record/import over the active slot",
while the original is still available unless explicitly deleted.

### File Format

Keep WAV export for compatibility, but use float internally. If analysis quality
matters, add either:

- 32-bit float WAV for internal renders and re-analysis.
- CAF for long or metadata-heavy internal files on macOS.
- 16-bit PCM WAV only for quick sharing or broad playback.

Apple's Core Audio stack covers recording, editing, playback, compression,
decompression, file parsing, and signal processing on macOS, and Extended Audio
File Services can read and write linear PCM and compressed files through a
simplified interface.

## Whole-Recording Spectrum

The user request says "analyse the whole recording spectrum in one large
frequency analysis without the time binning." That is a global spectrum view.
It answers:

```text
Across this entire clip, how much energy exists at each frequency?
```

It deliberately does not answer:

```text
When did that frequency happen?
```

That limitation should be explicit in the design. A global spectrum is powerful
for stable hum, resonances, noise floor, room modes, microphone response, and
overall tonal content. It is poor for short events whose frequencies change over
time.

### Spectrum Estimators

Use several estimators, because "whole spectrum" can mean different things.

#### Full-Clip DFT / Periodogram

Compute one real FFT over the selected clip, optionally after applying a window.
Frequency spacing is:

```text
bin_hz = sample_rate / sample_count
```

This gives maximum apparent frequency resolution for the whole clip. It is the
most literal "one large Fourier analysis."

Best for:

- exact tones
- mains hum
- stable resonances
- comparing frequency peaks across recordings

Risks:

- assumes the entire clip is one observation
- spectral leakage if tones do not land exactly on FFT bins
- transient sounds spread across the spectrum without timing context
- long clips produce more bins than pixels, so rendering must aggregate

Apple Accelerate already provides DFT/FFT facilities and documentation for
windowing to reduce spectral leakage. The current app already uses vDSP DFTs,
so this is the most native path.

#### Windowed Full-Clip Spectrum

Apply Hann, Blackman, flat-top, or similar windows to the whole clip before the
FFT. This reduces leakage at the cost of widening peaks and changing amplitude
calibration.

Design implication:

- the UI should show the selected window
- peak amplitude measurements should be window-corrected
- for measurement, distinguish "peak amplitude" from "power density"

#### Welch Spectrum

Welch's method splits the clip into short modified periodograms and averages
them. The output is still a single frequency curve, but the estimate is less
noisy. It is not "no time binning" internally, so label it clearly as an
averaged spectrum, not a single full DFT.

Best for:

- noise floor
- broad spectral profile
- less jumpy estimates from short or noisy recordings

Risks:

- poorer frequency resolution than full-clip DFT
- hides brief events

#### Multitaper Spectrum

The multitaper method applies several orthogonal tapers, traditionally DPSS /
Slepian tapers, and averages the resulting spectra. It is often a better
research estimator than a single window because it directly addresses variance
and leakage from finite observations.

Best for:

- robust PSD measurement
- resolving lines against noise
- comparing recordings scientifically

Risks:

- more complex controls: time-bandwidth product, taper count, adaptive weights
- less obvious to explain than FFT or Welch

#### Autoregressive / Burg / Maximum Entropy Spectrum

AR spectrum estimation fits a model rather than directly measuring FFT bins. It
can produce sharp peaks from short clips, but the peaks are model-dependent.

Best for:

- exploratory resonance finding in short, tonal clips
- estimating formants or modal peaks

Risks:

- can invent peaks
- model order selection becomes a major UI and validation problem

Recommended priority: full-clip FFT first, multitaper second, Welch third, AR
only as an advanced research mode.

### Spectrum Display

Keep the existing frequency-axis language. The best fit with the current UI is:

```text
left axis: log frequency, high at top
main area: horizontal magnitude curve or filled bars
band selection: two horizontal handles spanning the plot
```

This preserves the current "frequency is vertical" mental model. A standard
left-to-right spectrum could be offered later, but switching axes would make
band selection feel unrelated to the current spectrogram.

Display options to support:

- dBFS/bin: good for literal FFT bins, but changes with FFT length
- dBFS/Hz or PSD: good for comparing recordings of different lengths
- band-integrated dBFS: good for "how much energy is inside this band?"
- peak labels: strongest local maxima, with frequency and level
- octave or fractional-octave smoothing: useful for perceptual overview
- raw bins vs pixel aggregation: needed because the FFT can have far more bins
  than screen pixels

For long clips, never draw one bin per pixel by naive downsampling. Aggregate
bins per visual row or pixel using min/mean/max or percentile summaries so
narrow peaks remain visible.

## Band Lab

Band Lab is the workspace for selecting a lower and upper frequency and hearing
what lives inside that region.

The key UX concept:

```text
active clip + lower_hz + upper_hz + extraction method -> audition render
```

It should support at least three playback targets:

- Original
- Selected band
- Rejected band / inverse band

"Inverse Fourier transform it to hear it" is one valid technique, but it should
not be the only one. Different reconstruction methods imply different physics.

## Band Extraction Techniques

### 1. Full-Clip FFT Mask + Inverse FFT

Algorithm:

1. Transform the whole clip with one real FFT.
2. Zero or attenuate bins outside `[low_hz, high_hz]`.
3. Preserve original complex phase for bins inside the band.
4. Inverse FFT to time-domain samples.
5. Normalize or preserve gain according to mode.

This is closest to the user's wording.

Best for:

- stationary tones
- hum bands
- broad "what does this frequency range sound like?" experiments
- fastest conceptual prototype

Risks:

- assumes periodic extension of the entire clip
- hard bin cuts cause ringing in time
- not good when the same frequency should be selected only during part of the
  recording
- windowing complicates exact reconstruction

Design controls:

- band edge softness in Hz or octaves
- preserve original gain vs normalize preview
- include DC / exclude DC
- zero-pad for display only, not as fake resolution

UI label:

```text
METHOD FFT IFFT
```

### 2. FIR Bandpass Filter

Algorithm:

1. Design a finite impulse response bandpass filter.
2. Convolve it with the clip.
3. Optionally use FFT convolution for long filters.

Useful designs:

- windowed-sinc / Kaiser window
- Parks-McClellan equiripple
- least-squares FIR

Best for:

- honest offline audio extraction
- linear phase when waveform alignment matters
- complementary selected/rejected bands

Risks:

- narrow low-frequency bands require very long filters
- linear phase adds latency equal to about half the filter length
- sharp filters ring

Parks-McClellan is important because it designs linear-phase FIR filters with
controlled passband and stopband ripple. Kaiser/windowed-sinc is simpler and a
good first implementation.

UI label:

```text
METHOD FIR LINEAR
```

### 3. IIR Biquad Bandpass

Algorithm:

1. Design cascaded second-order sections.
2. Run samples through the filter state.

Useful families:

- Butterworth: smooth passband, monotonic response
- Chebyshev: sharper transition with ripple
- Elliptic: sharpest transition with ripple in passband and stopband

Best for:

- real-time preview
- cheap playback
- musical "EQ-like" audition

Risks:

- nonlinear phase shifts transients
- high-order filters need stable second-order sections
- very narrow bands can ring strongly

UI label:

```text
METHOD IIR BIQUAD
```

### 4. Zero-Phase Offline IIR

Algorithm:

1. Run an IIR filter forward through the stored clip.
2. Reverse the result and filter backward.
3. Reverse again.

This cancels phase delay offline, but squares the magnitude response and needs
careful edge handling.

Best for:

- measurement-oriented offline filtering
- hearing a band without phase skew

Risks:

- not usable as true live playback
- edge transients can mislead the listener
- response is not the same as one pass

UI label:

```text
METHOD IIR ZERO PHASE
```

### 5. STFT Mask + Inverse STFT

Algorithm:

1. Compute STFT frames.
2. Apply a frequency mask to each frame.
3. Preserve complex phase.
4. Inverse FFT each frame.
5. Overlap-add with a reconstruction-compatible window and hop.

Best for:

- time-varying audio
- selecting bands while retaining temporal events
- later extension to time-frequency painting

Risks:

- window/hop must satisfy reconstruction conditions
- hard masks cause musical noise and ringing
- phase consistency matters

This is the most natural path if Band Lab later allows selecting a region in a
spectrogram, not only a whole-frequency band.

UI label:

```text
METHOD STFT MASK
```

### 6. Magnitude-Only Reconstruction / Griffin-Lim

If the user modifies only magnitude and discards phase, reconstructing audio is
not a simple inverse FFT. Griffin-Lim estimates a time-domain signal whose STFT
magnitude matches the target. It is iterative and approximate.

Best for:

- experimental "painted spectrogram" resynthesis
- education about phase

Risks:

- slower
- artifacts
- not needed if the app preserves complex phase from the original recording

UI label:

```text
METHOD GRIFFIN LIM
```

### 7. Invertible Constant-Q / Wavelet Reconstruction

The current app has a tonal wavelet mode, but a display-oriented CWT/SST view is
not automatically an audio reconstruction system. For band audition on a
log-frequency basis, prefer an explicitly invertible transform such as an
invertible constant-Q transform based on nonstationary Gabor frames.

Best for:

- musical/log-frequency selections
- octave-spaced analysis
- pitch/resonance research

Risks:

- much larger implementation surface
- exact reconstruction requires careful frame design
- SST is for sharpening and mode separation; it is not a casual filter

UI label:

```text
METHOD CQT INVERTIBLE
```

### 8. Gammatone / Auditory Filter Bank

Gammatone filters model auditory bands on the ERB scale. They are good when the
question is "what would this perceptual band sound like?" rather than "what
does this exact Fourier interval contain?"

Best for:

- psychoacoustic exploration
- perceptual bands
- comparing with human hearing resolution

Risks:

- not a literal Fourier selection
- adjacent filters overlap by design
- reconstruction can be approximate unless a synthesis bank is designed

UI label:

```text
METHOD ERB AUDITORY
```

### 9. Parametric Modal / Sinusoidal Extraction

Instead of filtering a continuous band, estimate resonant modes or sinusoidal
partials and synthesize those components.

Best for:

- ringing objects
- room modes
- harmonic/resonance lists

Risks:

- can miss diffuse energy
- can invent structure
- parameter fitting is more research-heavy than general filtering

UI label:

```text
METHOD MODAL
```

### 10. Sparse Pursuit

Matching pursuit or sparse coding chooses time-frequency atoms and reconstructs
from selected atoms. This is powerful but expensive.

Best for:

- offline research
- separating ridges from broadband noise
- inspecting resonant atoms

Risks:

- slow
- hard to explain
- reconstruction depends on dictionary choices

UI label:

```text
METHOD SPARSE
```

## Technique Priority

Recommended order for a useful, coherent design:

1. Full-clip FFT spectrum.
2. Full-clip FFT mask + inverse FFT audition.
3. FIR linear-phase bandpass audition.
4. IIR biquad preview.
5. STFT mask + inverse STFT.
6. Multitaper/Welch spectrum estimators.
7. Invertible CQT or wavelet-family reconstruction.
8. Modal and sparse methods.

The first three cover the user's request directly. The rest create a research
path without making the initial design too broad.

## Important DSP Design Rules

### Preserve Phase Whenever Possible

If the app keeps the original complex phase, band reconstruction is much easier
and more faithful. A magnitude-only spectrogram is not enough to resynthesize
the original sound.

### Hard Frequency Edges Ring

A perfect rectangular cut in frequency corresponds to long sinc-like ringing in
time. The UI should offer soft band edges, not only hard upper/lower bounds.

### Low Frequencies Need Long Time Support

A narrow band around 30 Hz cannot be isolated with a short filter. The app
should show latency or effective window length for each method.

### A Global Spectrum Is Not a Timeline

If a 1 kHz tone happened for 50 ms and a 1 kHz tone happened for 10 seconds,
the global spectrum encodes energy, not the event structure. The UI should make
it easy to jump from Whole Spectrum to Clip Spectrogram when time matters.

### dB Labels Need Definitions

Use explicit measurement labels:

- dBFS peak
- dBFS RMS
- dBFS/bin
- dBFS/Hz
- band energy dBFS

Do not mix these silently.

### The Microphone Is Part Of The Measurement

The existing docs already note the input high-pass around 20-25 Hz and the ADC
brick wall near 23 kHz. Whole-spectrum and band tools should keep those bounds
visible so users do not over-interpret nonexistent sub-bass or ultrasonic data.

## UI Design

Keep the app visually compact and instrument-like. Avoid making it feel like a
generic file editor.

### Live Spectrogram

This can remain close to the current screen:

```text
banner
waveform
scrolling spectrogram
```

The banner should show workspace and technique:

```text
LIVE  METHOD TRANSIENT STFT  REC OFF  VIRIDIS  WAV 16
```

### Recordings / Clip Source

This workspace manages:

- active live capture
- frozen ring-buffer clip
- saved recordings
- imported audio files
- processed renders

It can still use the dark overlay menu style. The main screen can show a large
waveform overview with a simple list overlay.

Core commands:

- record new clip
- replace active clip
- import file
- save/export active clip
- duplicate render as source

### Whole Spectrum

Use the spectrogram area for a static spectrum:

```text
frequency axis left
magnitude curve across width
selected band shaded between two horizontal handles
peak labels in pixel text
```

Keep the waveform strip as a clip overview. It gives time context even though
the spectrum itself is global.

### Band Lab

Band Lab should combine:

- waveform overview
- spectrum plot
- lower/upper handles
- selected/rejected preview toggle
- method selector
- render/export status

The selected band can be shown as a translucent horizontal region in the
existing spectrogram colors. The rejected region can be dimmed.

Useful measurements in the banner or status line:

```text
BAND 120-380 HZ  WIDTH 1.66 OCT  METHOD FIR LINEAR  LATENCY 42.7 MS
```

### Compare / Measurements

This workspace compares original vs processed:

- spectrum overlay
- RMS/peak/band energy
- difference signal
- null test
- clipping warning

A null test is especially useful: selected band plus rejected band should
reconstruct the original for complementary linear methods within numerical
error.

## Architecture Implications

The app currently streams columns. Offline analysis wants cached results.

Add conceptual modules later:

```text
src/audio/playback.c        output callback / queue
src/audio/audio_file.c      import/export through AudioToolbox or current WAV
src/app/clip.c              active clip model
src/analysis/global_spectrum.c
src/analysis/band_filter.c
src/analysis/istft.c
src/app/workspace.c         Live, Spectrum, Band Lab, Recordings, Settings
```

The analysis engine can remain for live modes. Do not force whole-spectrum or
band-render operations into the live column API. They have different lifecycles:

```text
live analyzer: small incremental updates
offline analyzer: job over a fixed clip, cached result
band renderer: job over a fixed clip, produces playable samples
```

## Playback Model

The current app captures audio but does not play processed clips. Add playback
as a separate output path:

- output stream reads from an immutable float buffer
- UI can switch source: original, selected band, rejected band
- playback should never run heavy analysis in the real-time callback
- render offline first, then play

For C on macOS, Audio Queue Services or Audio Units fit the existing style.
AVAudioEngine is a higher-level Objective-C/Swift route, useful if the project
accepts that dependency later. Apple's docs also describe offline rendering
with AVAudioEngine, but this codebase is currently plain C.

## Validation Plan

Add synthetic tests before trusting the tools.

### Global Spectrum

- full-scale sine at exact FFT bin: peak at expected bin
- sine between bins: leakage shape changes with window
- two close tones: verify resolution from clip length
- white noise: PSD should be flat statistically
- impulse: spectrum should be broadband

### Band Extraction

- sine inside band passes
- sine outside band rejects
- two tones: selected band isolates the right tone
- impulse through filter: reveals ringing and latency
- selected + rejected nulls against original for complementary methods
- STFT mask round-trip with all-pass mask reconstructs original
- low-frequency narrow band reports long effective latency

### UI / Measurement

- band handles map exactly to frequency labels
- dBFS labels match existing calibration
- exports do not clip unless explicitly normalized
- processed clip sample rate and duration match source

## Recommended Design Sequence

### Phase 1: Clip-Centered App Shape

Design the workspace model and active clip lifecycle. This is the foundation for
recording replacement, import, spectrum, and band audition.

### Phase 2: Whole Spectrum

Add full-clip FFT spectrum first. Keep display static, log-frequency, and
visually aligned with the existing spectrogram axis.

### Phase 3: Band Lab MVP

Add lower/upper frequency handles and FFT-mask inverse-FFT audition. Include
soft edges immediately to avoid teaching users that hard rectangular cuts are
neutral.

### Phase 4: Better Filters

Add FIR linear-phase and IIR biquad methods. FIR should become the default
"honest offline band" method. IIR should be labeled as a fast preview or EQ-like
method.

### Phase 5: Time-Frequency Resynthesis

Add STFT mask + inverse STFT for time-varying selections. This unlocks future
spectrogram painting and region selection.

### Phase 6: Research Techniques

Add multitaper/Welch global estimators, invertible CQT, auditory filters, modal
analysis, and sparse methods as explicit advanced modes.

## Design Summary

The right mental model is:

```text
Record or import a clip.
Choose an analysis view.
Select a frequency band.
Choose a reconstruction method.
Audition original / selected / rejected.
Measure and export without destroying the source.
```

The most important design distinction is between displays and reconstruction
systems. The current spectrogram modes are excellent displays. A band-audition
feature needs reconstruction-aware methods with phase, filter response, latency,
and edge artifacts exposed enough that the user can trust what they hear.

## Sources

- Apple Accelerate, Discrete Fourier Transforms:
  https://developer.apple.com/documentation/accelerate/discrete-fourier-transforms
- Apple Accelerate, Reducing spectral leakage with windowing:
  https://developer.apple.com/documentation/accelerate/reducing-spectral-leakage-with-windowing
- Apple Core Audio Overview:
  https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/Introduction/Introduction.html
- Apple AudioToolbox, Extended Audio File Services:
  https://developer.apple.com/documentation/audiotoolbox/extended-audio-file-services
- P. Welch, "The Use of Fast Fourier Transform for the Estimation of Power
  Spectra" (1967):
  https://research.ibm.com/publications/the-use-of-fast-fourier-transform-for-the-estimation-of-power-spectra-a-method-based-on-time-averaging-over-short-modified-periodograms
- D. J. Thomson, "Spectrum Estimation and Harmonic Analysis" (1982):
  https://www.math.ucdavis.edu/~saito/data/ONR15/thomson_spect-est-harm-anal.pdf
- T. Parks and J. McClellan, "Chebyshev Approximation for Nonrecursive Digital
  Filters with Linear Phase" (1972):
  https://repository.rice.edu/bitstreams/a924e584-8512-4852-9801-c602985dc0da/download
- D. Griffin and J. Lim, "Signal Estimation from Modified Short-Time Fourier
  Transform" (1984):
  https://ieeexplore.ieee.org/document/1164317/
- M. Portnoff, "Implementation of the Digital Phase Vocoder Using the Fast
  Fourier Transform" (1976):
  https://labrosa.ee.columbia.edu/~dpwe/papers/Portnoff76-pvoc.pdf
- I. Daubechies, J. Lu, and H.-T. Wu, "Synchrosqueezed Wavelet Transforms"
  (2011):
  https://arxiv.org/abs/0912.2437
- T. Oberlin, S. Meignen, and V. Perrier, "The Fourier-based
  Synchrosqueezing Transform" (2015):
  https://hal.science/hal-00916088/document
- J. C. Brown, "Calculation of a Constant Q Spectral Transform" (1991):
  https://www.ee.columbia.edu/~dpwe/papers/Brown91-cqt.pdf
- G. A. Velasco, N. Holighaus, M. Doerfler, and T. Grill, "Constructing an
  Invertible Constant-Q Transform with Nonstationary Gabor Frames" (2011):
  https://recherche.ircam.fr/pub/dafx11/Papers/47_e.pdf
- M. Slaney, "An Efficient Implementation of the Patterson-Holdsworth Auditory
  Filter Bank" (Apple Computer Technical Report 35, 1993):
  https://engineering.purdue.edu/~malcolm/apple/tr35/PattersonsEar.pdf
