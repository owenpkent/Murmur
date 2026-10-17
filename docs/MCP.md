# MCP: letting Claude drive Murmur

Murmur embeds an MCP (Model Context Protocol) server, so Claude Code (or any other local MCP
client) can read and drive it directly: check what the voice analysis is hearing, set the
mode, scale and quantize, read the inferred tempo, and run or clear a drum calibration, all as
tool calls instead of mouse clicks. This does not change Murmur's own accessibility contract
(nothing in [CONTROLS.md](CONTROLS.md) requires it); it is an additional way in, for when you
would rather describe what you want than click it.

Murmur Host answers on the same server, so everything here applies to it too (`get_state` reports
`product: "Murmur Host"` and a `mix` control; `mix` is also a `set_params` id there).

Murmur makes no sound and plays nothing on request, so unlike Keys there are no play-a-note
tools. What Claude can do is configure Murmur and watch what it hears.

## Architecture

```
Claude Code  ->  murmur-mcp.exe (stdio shim)  ->  discovery file in the OK Studio
app-data mcp directory  ->  loopback TCP (127.0.0.1, OS-assigned port)  ->  the
in-plugin MCP server (okstudio::mcp::Server, embedded in MurmurProcessor)  ->  every
tool handler runs on the message thread, calling straight into MurmurProcessor / APVTS
the same way the editor does.
```

The transport (`okstudio::mcp::Server`, `okstudio/Mcp.h`) lives in `../okstudio-juce-kit`;
Murmur only registers tools. See `src/mcp/MurmurMcp.h` and `.cpp`. The bridge is owned by
`MurmurProcessor` (`processor.mcp()`), one per instance, created in its constructor and torn
down first in its destructor.

The live values the tools read (input level, last note, inferred tempo) are the same atomics
the editor's 30 Hz timer reads, so what Claude sees is what is on screen.

## Tools

| Tool | Purpose |
|------|---------|
| `get_state` | Snapshot: product and version, `mode`, every control's value as text (`controls`), `inputLevel` (0..1), `lastNote` (`{note, name}`, or null before the first), `inferredBpm` and its confidence, `hostPlaying`, `hostBpm`, `latencySamples`, and the `calibration` block. Call this first. |
| `get_transport` | Which clock the quantizer is on: `source` is `host` while the transport plays and `inferred` while it is stopped, with the `bpm` in use (null while an inferred tempo is not available yet), both tempos, the inference confidence and the current `grid`. |
| `list_params` | Every parameter with its id, name, current value, and choice list or min/max. |
| `set_params` | Set one or more parameters by id. Strings are parsed like typing into the control (`"Dorian"`), numbers are real-world values, booleans set toggles. All ids are validated first: one unknown id fails the call and nothing is applied. |
| `get_calibration` | Per drum class (`kick`, `snare`, `hatClosed`, `hatOpen`): output MIDI note, number of your own `examples`, and `calibrated` (false means the class is still on the built-in defaults). Also `calibrating` and `calibratingClass`. |
| `start_calibration` | Arm calibration for one class (`drumClass`: a name or index 0..3), exactly like its button: the next three onsets become examples. It only arms; the sound has to come from whoever is at the microphone. Poll `get_calibration` to watch it finish. |
| `clear_calibration` | Discard your examples for one class, like its Clear button. |
| `get_take` | The Take Editor's take: `hasTake`, `recording` with `secondsRecorded`, `analysing`, `playing` with `playheadSeconds`, `noteCount`, `error` (text or null), `anchor` (`{ppq, bpm}` or null for a free take) and `notes`: per note `index`, `start`, `end` (seconds), `sungMidi`, `detectedNote`, `targetNote`, `velocity` (0..1), `drift` (1 keeps the sung wobble, 0 flattens it) and the note names. Also `hasAudio` (false when the take's WAV is missing) and `renderingVoice` (the corrected voice is being re-rendered after an edit). |
| `edit_take` | `op` is `shift` (`index`, `steps`: semitones, or scale degrees while `scaleLock` is on), `set` (`index`, `note` 0..127), `reset` (`index`; drift back to 1), `drift` (`index`, `drift` 0..1; out of range or missing is an error, never a clamp), `delete` (`index`), `snap_to_scale` or `undo`. Each call is one undo step. Returns the updated note (shift, set, reset, drift), or the `noteCount` for delete, snap and undo. |
| `take_transport` | `action` is `record`, `stop_recording` (waits up to 30 s for the analysis, returns the `noteCount`), `play` (optional `fromSeconds`) or `stop`. Returns the transport state. |

A `drumClass` that is not one of the four names or 0..3 is rejected rather than clamped to
Kick. The take tools are the same: an unknown `op` or `action`, a missing, fractional or
out-of-range `index`, a missing `steps`, a `note` outside 0..127, a `fromSeconds` outside the
take, editing while recording or analysing, and an action that cannot happen now (play with no
notes, record while recording, stop when not playing, undo with nothing to undo) are errors and
change nothing.

### Parameters

`set_params` ids (see `list_params` for the live ranges): `mode` (Melody, Drums),
`sensitivity` (0..1), `quantize` (0..1), `swing` (0..1), `grid` (1/4, 1/8, 1/16, 1/8T,
1/16T, 1/32), `root` (note names), `scale` (the kit's scale list), `scaleLock` (bool).

## Setting it up in Claude Code

```
claude mcp add murmur -- <path-to>\murmur-mcp.exe
```

Build `murmur-mcp` alongside the plugin (`MURMUR_BUILD_MCP_SHIM`, on by default):

```powershell
cmake --build build --config Release --target murmur-mcp
```

A local build puts it at `build\Release\murmur-mcp.exe`. Or use a project-scoped `.mcp.json`
in your checkout (it is gitignored here because the path is machine-specific):

```json
{
  "mcpServers": {
    "murmur": {
      "type": "stdio",
      "command": "<path-to>\\Murmur\\build\\Release\\murmur-mcp.exe",
      "args": [],
      "env": {}
    }
  }
}
```

The shim is built with a default `--product=murmur` filter, so it connects to Murmur and not
to another plugin of the line that happens to be loaded in the same DAW.

## Multiple instances

If more than one Murmur is loaded (several tracks, several DAW windows), the shim connects to
whichever advertised itself most recently; pass `--port=N` to pin it to a specific instance.
Every instance in a Live set shares one process, so the discovery file's pid cannot tell them
apart: the port can. It is `MurmurMcp::port()` and is also in that instance's discovery file.

## Security

The server only binds `127.0.0.1` and never authenticates a connection: any local process
that can reach the loopback port can call these tools. That is an accepted trade-off for a
local creative tool, not a hardened remote API. Don't expose the port beyond loopback.

## Tests

`tests/McpTests.cpp` drives the tools through the server's JSON-RPC layer (`handleLine`), so
the schema, the registration and the envelope are under test alongside each tool body. It
covers the tool table, argument validation (an unknown parameter id applies nothing, a bad
drum class is rejected), and that `set_params` and the calibration tools really change the
processor.

## What to try

With a Murmur loaded and the shim connected:

1. `get_state`: see the mode, the key, and whether it is hearing anything (`inputLevel`).
2. `set_params { "values": { "scale": "Dorian", "root": "D" } }`: set the key.
3. `set_params { "values": { "mode": "Drums" } }`, then `start_calibration { "drumClass": "kick" }`
   and make a kick sound three times into the mic; `get_calibration` then shows `kick` calibrated.
4. With the host stopped, sing a steady beat and call `get_transport` to see the inferred
   tempo and its confidence.
