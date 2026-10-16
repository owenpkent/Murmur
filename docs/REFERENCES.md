# References

The research each part of the engine is built on, what Murmur took from it, and what it
deliberately did not. [DESIGN.md](DESIGN.md) is the why for each choice; this file is the
sources behind those choices, so the next person does not re-derive a decision that was made
or re-invent one that was rejected. Each source is also named in the header comment of the
file that uses it. Read the source before changing the algorithm, not after.

## Pitch: McLeod Pitch Method (`src/dsp/PitchTracker.h`)

McLeod and Wyvill, *A smarter way to find pitch*, ICMC 2005.

- **Took:** the normalised square difference function (NSDF), which bounds every peak to
  [-1, 1] so the height of the first strong peak is a level-independent periodicity score
  (Murmur's `clarity`, the voiced/unvoiced signal), and the rule of picking the *first* peak
  within 93% of the tallest, which is what kills octave errors. Computed through one
  zero-padded FFT autocorrelation per hop so the cost is flat and allocation-free.
- **Alternatives kept as upgrade paths behind the same API:** YIN (de Cheveigne and Kawahara,
  *YIN, a fundamental frequency estimator for speech and music*, JASA 2002; aubio's
  YIN/YINFFT) and Cycfi Q's bitstream autocorrelation (lower latency, better on very low
  pitches). Rejected for v1 to keep the plugin dependency-free.

## Note tracking (`src/dsp/NoteSegmenter.h`)

- **Took:** a causal, hand-tuned note state machine (stable for 40 ms before a note starts,
  60-cent retrigger, 80 ms of silence to end, nothing shorter than 60 ms), with note-on
  back-dated to the start of the stable run.
- **Did not take:** the HMM / Viterbi note tracker of pYIN (Mauch and Dixon, *pYIN: a
  fundamental frequency estimator using probabilistic threshold distributions*, ICASSP 2014).
  More accurate offline, but it needs look-ahead a live plugin cannot afford.
- **Did not take:** Basic Pitch (Bittner et al., *A lightweight instrument-agnostic model for
  polyphonic note transcription and multipitch estimation*, ICASSP 2022). It is already in the
  kit as a possible non-real-time polish pass over a recorded take, not a live path.
- **Did not take:** CREPE (Kim et al., *CREPE: a convolutional representation for pitch
  estimation*, ICASSP 2018). Too heavy and too much latency for a DAW buffer, and a runtime
  to ship.

## Onsets: spectral flux with SuperFlux (`src/dsp/OnsetDetector.h`)

Bello et al., *A tutorial on onset detection in music signals*, IEEE Trans. Speech and Audio
Processing, 2005 (spectral flux as the detection function); Boeck and Widmer, *Maximum filter
vibrato suppression for onset detection*, DAFx 2013 (SuperFlux).

- **Took:** log-compressed magnitudes so quiet consonants count as much as loud vowels,
  comparing each bin to the maximum of its three neighbours in the previous frame so vibrato
  does not read as a stream of onsets, an adaptive median threshold scaled by Sensitivity, and
  a local-average guard so sustained noise (an open hat) does not retrigger itself.
- **Alternatives kept as upgrade paths behind the same `pop()` API:** aubio's onset module
  (complex-domain, specflux) or a small learned onset model.

## Drum classification: k-nearest neighbour (`src/dsp/DrumClassifier.h`)

Cover and Hart, *Nearest neighbor pattern classification*, IEEE Trans. Information Theory,
1967.

- **Took:** k = 3 with inverse-distance weighting over a small per-user spectral feature
  vector, with ring-overwrite fixed storage so adding an example never allocates. No training
  step, so adding or clearing an example is instant.
- **Did not take:** a fixed pretrained classifier, which fails on individual voices, or a
  small MLP / Gist MFCC model trained on a shared corpus, which needs a corpus Murmur does not
  have. Both could sit behind the same `classify()` signature later.

## Free-tempo inference (`src/dsp/Quantizer.h`, `GridInference`)

Dixon, *Automatic extraction of tempo and beat from expressive performances*, J. New Music
Research, 2001, and its BeatRoot system (Dixon, *Evaluation of the audio beat tracking system
BeatRoot*, JNMR 2007).

- **Took:** the inter-onset-interval clustering idea: histogram the intervals between recent
  onsets (not only neighbours, so skipped beats still vote), merge them into 25 ms clusters,
  score each by how many others sit at integer multiples of it, fold half/double ambiguity
  into 70..170 BPM, take phase from a circular mean, and smooth both so the grid follows the
  player.
- **Did not take:** a full beat tracker such as BTrack. It is kept as an upgrade path; none is
  worth the dependency on day one.

## Routing: Ableton Live "MIDI From"

Murmur's whole deployment story rests on Live's ability to route an audio-effect plug-in's MIDI
output to another track (Live 10.1 or later). The steps and the gotchas are in
[ABLETON_LIVE.md](ABLETON_LIVE.md); Live's own manual is the authority on the routing menu.

## The rule

A claim in this file is only here because the code does it. When an algorithm is swapped,
change its entry here in the same commit, and move the old one to "Did not take" with the
reason.
