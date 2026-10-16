# Design decisions

Why Murmur is built the way it is, and what was rejected.

## Chosen

- **In-house MPM pitch tracker (Melody).** McLeod Pitch Method is monophonic, robust on
  voice, cheap, and short enough to write and test ourselves. A hand-rolled tracker keeps
  latency under our control and adds no dependency or licence to the plugin.
- **Spectral-flux onsets.** Positive spectral flux with an adaptive threshold finds note
  starts and drum transients in a single pass, and one knob (Sensitivity) maps cleanly to the
  threshold.
- **Per-user kNN drum calibration.** Beatboxers sound different, so a fixed classifier fails.
  Three examples per class (kick, snare, closed hat, open hat) of a small spectral feature
  vector, classified by nearest neighbour, trains in seconds, needs no model file, and
  serializes as a ValueTree into the session.
- **Strength quantize plus IOI grid inference.** Quantize pulls each event toward the grid by
  a strength (0 keeps the human timing, 1 snaps), with swing on offbeats. When the host is
  stopped there is no tempo, so the engine estimates one from inter-onset intervals and
  uses it as the grid.
- **Audio effect, not instrument.** The plugin needs audio in. Live's "MIDI From" plug-in
  routing sends its MIDI to a Drum Rack track.

## Rejected or deferred

- **Basic Pitch.** Polyphonic and accurate, but offline-only (it needs the whole take) and
  heavy. It is already in the kit as a possible future "polish" pass over a recorded take,
  not a live path.
- **Cycfi Q, Gist, aubio, BTrack.** All viable. Kept as upgrade paths if the in-house pitch
  or beat code stops being enough; none is worth the dependency on day one.
- **CREPE / ONNX models.** Too heavy and too much latency for a real-time plugin that must
  run inside a DAW buffer; also a runtime to ship and keep safe.
- **A fixed pretrained drum classifier.** Fails on individual voices; calibration is both
  more accurate and more honest.
