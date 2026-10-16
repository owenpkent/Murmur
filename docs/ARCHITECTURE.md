# Architecture

## Signal path

```
host audio in (mono/stereo)
   |-- passthrough: the buffer is only read, never modified, so audio out == audio in
   `-- mono mix ((L+R)/2, or the one channel)
         `-- MurmurEngine::process(mono, n, blockStartSample)
               |-- Melody: pitch tracker + onset gate -> note on/off, scale snap
               `-- Drums: spectral-flux onset -> kNN class (kick/snare/closed/open) -> hit
         quantize (strength, swing, grid; host ppq or inferred free tempo)
         `-- MurmurEngine::drainMidi -> MidiBuffer (sample offsets inside the block)
```

`PluginProcessor.cpp` is the shell. Each block it: clears the MIDI buffer (Murmur authors the
whole stream), reads the playhead (ppq, bpm, playing), pushes the APVTS values to the engine,
mixes to mono in chunks of at most `maxBlock`, calls `process`, drains MIDI after each chunk
and adds the chunk start to each event's offset, then updates reported latency if it changed.

## Threads

| Thread | Does | Touches |
|--------|------|---------|
| Audio | `processBlock`: mono mix, engine process, MIDI out | engine (exclusive), APVTS raw atomics, no allocation after `prepareToPlay` |
| Message/UI | editor, 30 Hz timer, calibration buttons | engine atomic getters; `beginCalibration` / `clearCalibration` / calibration ValueTree (not real-time) |

Engine parameter setters are plain floats/ints written from the audio thread each block, so
there is no cross-thread parameter state to lock.

## Latency

Pitch detection and onset confirmation need a short look-ahead window, and the quantizer may
hold an event to land it on the grid. The engine reports the fixed part through
`getLatencySamples()`; the processor forwards it with `setLatencySamples` in `prepareToPlay`
and whenever it changes. Hosts that compensate (Live does) line the MIDI up with the audio.

## State

`okstudio::state::save` writes the APVTS plus a `MURMUR_CALIBRATION` child wrapping the
engine's calibration tree, so a calibrated voice comes back with the session. Loading an
absent or invalid calibration leaves the engine defaults.

## Editor

Mode toggle, knobs, grid control, level bar, last note/hit and free-tempo readout. The 30 Hz
timer polls `getInputLevel`, `getLastNote`, `getInferredBpm`, `isCalibrating`. Scale controls
show only in Melody; the Calibrate row only in Drums.
