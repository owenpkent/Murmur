# Murmur Host: design

**Status: implemented (2026-10-15).** VST3 and Standalone build; unit tests cover the processor
with no instrument loaded. Still owed: a real Ableton Live load test, and a test with a real
third-party instrument loaded (none is available to the unit tests).

## What it is

A second plugin target in this repo, built from the same sources as Murmur plus `src/host/`.
It is Murmur and it hosts one third-party instrument VST3 inside itself, so **voice in, drums
or synth out** works on ONE audio track. Plain Murmur only emits MIDI, which in Live needs a
second track taking "MIDI From Murmur"; Live just sees one audio effect that makes sound.

Modelled on Keys Host (`../Keys/docs/KEYS_HOST_DESIGN.md`). The difference: Keys Host is an
instrument on a MIDI track, Murmur Host stays an **AUDIO_EFFECT** because it needs the voice.

## Processor: `MurmurHostProcessor : MurmurProcessor` (`src/host/`)

- Members: an `AudioPluginFormatManager` (VST3 only), the hosted `AudioPluginInstance`, its
  path, a preallocated scratch buffer, and a one-second dry-voice ring. It is a
  `ChangeBroadcaster` so the editor follows load and eject.
- `processBlock`: copy the voice, call `MurmurProcessor::processBlock` (engine MIDI into the
  host buffer, audio untouched), run the instrument on a copy of that MIDI, then write
  `dry * (1 - mix) + instrument * mix`. The MIDI also stays in the host buffer so external
  routing ("MIDI From Murmur Host") keeps working. A hosted plugin's own MIDI output is not
  forwarded (same trade-off as Keys Host).
- **Mix** (`mix`, 0..1, default 1) exists only in the Host layout. 0 is the voice only, 1 is
  the instrument only. It is ramped across each block. The dry voice is delayed by the reported
  latency so it lines up with the instrument after the host's delay compensation.
- **Latency** is the engine's plus the instrument's. `MurmurProcessor::setExtraLatency` adds the
  instrument part to the engine part wherever the base class reports latency.
- **No instrument loaded** behaves exactly like plain Murmur: audio passes through.
- **Threading:** the audio thread only reads `instrument`; it never allocates or locks. Load and
  eject run on the message thread inside `suspendProcessing(true/false)`.
- **Instruments only, VST3 only.** A VST3 with no instrument in it is refused ("... is not an
  instrument"). Murmur and Murmur Host are never offered, so it cannot host itself.

## State

The usual `MURMUR` tree (APVTS plus `MURMUR_CALIBRATION`) plus a `hostedInstrument` child: file
path, name, and the hosted plugin's state as base64. On load the plugin is reloaded and its
state applied. If the file is missing the processor keeps going with no instrument, reports
"Instrument not found: <path>", and keeps the path and blob so saving the project again does not
lose them; it loads once the plugin is installed.

## Editor: `MurmurHostEditor`

The full `MurmurEditor` (its updater check is off, since Murmur Host is not the Murmur release
channel) with one strip under it, then the hosted instrument's own editor embedded below that:

- **Instrument** list (a combo box): the instruments the last **Scan** found.
- **Scan**: walks `C:\Program Files\Common Files\VST3` and `%USERPROFILE%\Ableton\vst3` on a
  background thread, asks each `.vst3` what it contains and keeps the instruments. Cached in
  `%APPDATA%\OK Studio\Murmur\HostInstruments.xml`. It loads plugins briefly in-process, which
  is why it only runs when pressed.
- **Hide GUI / Show GUI**, **Eject**, and the **Mix** knob.

Everything is mouse-only: single left-click, targets at least 34 px, no typing.

## Not mirrored from Keys Host

- The fader bank that maps hosted parameters to Keys faders (Murmur has no faders).
- The publisher-folder picker overlay, drag-and-drop of a `.vst3`, and the file-dialog fallback.
  A Scan-based list is simpler and meets the "list plus Scan" need; add them if odd install
  locations matter.
- The floating instrument window; the instrument GUI is embedded below Murmur instead.
- A separate MCP product slug: the Host answers on the same `murmur` server name so the
  `murmur-mcp` shim (which filters on it) finds it; `get_state` reports `product: "Murmur Host"`
  and a `mix` control.
- Installer: Keys's installer ships Keys only, so `murmur.iss` is unchanged.
