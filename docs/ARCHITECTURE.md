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

Drum classification needs 20 ms of audio after the onset, so the engine reports a fixed 20 ms
(`getLatencySamples()`, `DrumFeatureExtractor::windowSeconds`) and releases every event, drum or
melody, at event time + latency + quantize shift. Melody detection itself takes longer (pitch
window, 40 ms stability test); a note-on is back-dated to the true start but can only be released
once decided, so it comes out at offset 0 of the block that confirmed it when that is already
past. The processor forwards the latency with `setLatencySamples` in `prepareToPlay` and
whenever it changes; Murmur Host adds the hosted instrument's. Hosts that compensate (Live
does) line the MIDI up with the audio.

## State

`okstudio::state::save` writes the APVTS plus a `MURMUR_CALIBRATION` child wrapping the
engine's calibration tree, so a calibrated voice comes back with the session. Loading an
absent or invalid calibration leaves the engine defaults.

## MCP bridge

`MurmurProcessor` owns one `MurmurMcp` (`src/mcp/`), created in the constructor and reset
first in the destructor. It registers seven tools on the kit's `okstudio::mcp::Server`, which
binds a loopback port and writes a discovery file the `murmur-mcp` stdio shim finds. Tool
bodies run on the message thread, so they call into the processor and APVTS exactly as the
editor does; the live values they read are the same atomics the editor timer reads. The
processor additionally records the host tempo (`getHostBpm()`) each block for `get_transport`.
There is no timer and no scheduling: Murmur plays nothing on request. Tools and setup:
[MCP.md](MCP.md).

## Editor

Mode toggle, knobs, grid control, level bar, last note/hit and free-tempo readout. The 30 Hz
timer polls `getInputLevel`, `getLastNote`, `getInferredBpm`, `isCalibrating`. Scale controls
show only in Melody; the Calibrate row only in Drums.

## Murmur Host

`MurmurHostProcessor` (`src/host/`) subclasses `MurmurProcessor` and is built as a second plugin
target (`MurmurHost`, `MURMUR_BUILD_HOST`, default ON) from the same sources plus `src/host/`.
Still an `AUDIO_EFFECT`. Each block it copies the voice, runs `MurmurProcessor::processBlock`
(engine MIDI into the host buffer, audio untouched), hands a copy of that MIDI to the hosted
instrument's `processBlock`, and writes `dry * (1 - mix) + instrument * mix` to the output. The
dry voice goes through a ring buffer delayed by the reported latency so it lines up with the
instrument. Reported latency is the engine's plus the instrument's (`setExtraLatency`).
Instruments are VST3 only and must report `isInstrument`. State adds a `hostedInstrument`
child (path, name, base64 plugin state); a path that no longer exists is kept so a re-save does
not lose it. `MurmurHostEditor` embeds the full `MurmurEditor`, a strip (Instrument list,
Scan, Hide/Show GUI, Eject, Mix) and the instrument's own editor below. Design:
[MURMUR_HOST_DESIGN.md](MURMUR_HOST_DESIGN.md).
